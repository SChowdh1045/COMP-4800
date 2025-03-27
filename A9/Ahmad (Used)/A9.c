// Compile: make -f Makefile
// Run: ./A9.exe pencil.wav
// To convert to .wav: ffmpeg -i pencil.mp3 output.wav

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <math.h>

#ifdef _WIN32
// This must come BEFORE the Windows headers
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#define AUDIO_SYSTEM "WinMM"
// Windows compatibility for usleep
#define usleep(x) Sleep((x)/1000)
#else
#include <alsa/asoundlib.h>
#define AUDIO_SYSTEM "ALSA"
#endif

// FFmpeg version compatibility
#define USING_NEW_FFMPEG_API (LIBAVCODEC_VERSION_MAJOR > 58)

// Constants
#define BUFFER_SIZE 16384  // Size of the circular buffer
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define LINE_WIDTH 2.0

// Drawing tool enum
typedef enum {
    TOOL_PENCIL,
    TOOL_ERASER
} DrawingTool;

// Circular buffer for audio data
typedef struct {
    uint8_t data[BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t size;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} CircularBuffer;

// Drawing point structure
typedef struct {
    double x, y;
} Point;

// Line structure
typedef struct {
    Point start;
    Point end;
    GdkRGBA color;
    double width;
    int is_eraser;
} Line;

// Global variables
static CircularBuffer audio_buffer;
static pthread_t audio_thread;
static volatile int keep_running = 1;
static volatile int is_drawing = 0;
static GtkWidget *drawing_area;
static cairo_surface_t *surface = NULL;
static GSList *lines = NULL;
static DrawingTool current_tool = TOOL_PENCIL;
static Point last_point = {0, 0};
static GdkRGBA current_color = {0.0, 0.0, 0.0, 1.0}; // Default: black
static double eraser_size = 20.0;
static double pencil_size = 2.0;

#ifdef _WIN32
// Windows audio variables
static HWAVEOUT hWaveOut = NULL;
static WAVEHDR waveHeader;
static DWORD waveBufferSize = 16384;
static LPBYTE waveBuffer = NULL;
#else
static snd_pcm_t *pcm_handle;
#endif

static volatile int is_audio_playing = 0;
static const char *audio_file;

// Forward declarations
static void init_circular_buffer(CircularBuffer *buffer);
static int write_to_buffer(CircularBuffer *buffer, const uint8_t *data, size_t size);
static int read_from_buffer(CircularBuffer *buffer, uint8_t *data, size_t size);
static void *audio_thread_func(void *arg);
static void setup_audio_system(void);
static void cleanup_audio_system(void);
static void play_audio(void);
static void stop_audio(void);
static void clear_surface(void);
static void create_surface(GtkWidget *widget);

#ifdef _WIN32
static void *winmm_playback_thread(void *arg);
#else
static void *alsa_playback_thread(void *arg);
#endif

// Initialize the circular buffer
static void init_circular_buffer(CircularBuffer *buffer) {
    buffer->read_pos = 0;
    buffer->write_pos = 0;
    buffer->size = 0;
    
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
}

// Write data to the circular buffer
static int write_to_buffer(CircularBuffer *buffer, const uint8_t *data, size_t size) {
    size_t bytes_written = 0;
    
    pthread_mutex_lock(&buffer->mutex);
    
    while (bytes_written < size && keep_running) {
        size_t available = BUFFER_SIZE - buffer->size;
        
        if (available == 0) {
            // Buffer is full, wait for space
            pthread_cond_wait(&buffer->not_full, &buffer->mutex);
            continue;
        }
        
        size_t to_write = (size - bytes_written) < available ? (size - bytes_written) : available;
        size_t first_chunk = BUFFER_SIZE - buffer->write_pos;
        
        if (to_write <= first_chunk) {
            memcpy(buffer->data + buffer->write_pos, data + bytes_written, to_write);
            buffer->write_pos = (buffer->write_pos + to_write) % BUFFER_SIZE;
        } else {
            memcpy(buffer->data + buffer->write_pos, data + bytes_written, first_chunk);
            memcpy(buffer->data, data + bytes_written + first_chunk, to_write - first_chunk);
            buffer->write_pos = to_write - first_chunk;
        }
        
        buffer->size += to_write;
        bytes_written += to_write;
        
        // Signal that the buffer is not empty
        pthread_cond_signal(&buffer->not_empty);
    }
    
    pthread_mutex_unlock(&buffer->mutex);
    return bytes_written;
}

// Read data from the circular buffer
static int read_from_buffer(CircularBuffer *buffer, uint8_t *data, size_t size) {
    size_t bytes_read = 0;
    
    pthread_mutex_lock(&buffer->mutex);
    
    while (bytes_read < size && keep_running) {
        if (buffer->size == 0) {
            // Buffer is empty, wait for data or fill with silence if needed
            if (!keep_running) {
                // Fill remaining with silence if shutting down
                memset(data + bytes_read, 0, size - bytes_read);
                bytes_read = size;
                break;
            }
            
            // Wait for data
            pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
            continue;
        }
        
        size_t to_read = (size - bytes_read) < buffer->size ? (size - bytes_read) : buffer->size;
        size_t first_chunk = BUFFER_SIZE - buffer->read_pos;
        
        if (to_read <= first_chunk) {
            memcpy(data + bytes_read, buffer->data + buffer->read_pos, to_read);
            buffer->read_pos = (buffer->read_pos + to_read) % BUFFER_SIZE;
        } else {
            memcpy(data + bytes_read, buffer->data + buffer->read_pos, first_chunk);
            memcpy(data + bytes_read + first_chunk, buffer->data, to_read - first_chunk);
            buffer->read_pos = to_read - first_chunk;
        }
        
        buffer->size -= to_read;
        bytes_read += to_read;
        
        // Signal that the buffer is not full
        pthread_cond_signal(&buffer->not_full);
    }
    
    pthread_mutex_unlock(&buffer->mutex);
    return bytes_read;
}

// Audio thread function - reads audio file and fills buffer
static void *audio_thread_func(void *arg) {
    AVFormatContext *format_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    SwrContext *swr_ctx = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    
    int audio_stream_idx = -1;
    int ret;
    
    // Initialize FFmpeg
    format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        fprintf(stderr, "Failed to allocate format context\n");
        goto cleanup;
    }
    
    // Open the file
    if (avformat_open_input(&format_ctx, audio_file, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open audio file %s\n", audio_file);
        goto cleanup;
    }
    
    // Find stream information
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto cleanup;
    }
    
    // Find the audio stream
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
            break;
        }
    }
    
    if (audio_stream_idx == -1) {
        fprintf(stderr, "Could not find audio stream\n");
        goto cleanup;
    }
    
    // Get the codec
    const AVCodec *codec = avcodec_find_decoder(format_ctx->streams[audio_stream_idx]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        goto cleanup;
    }
    
    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        goto cleanup;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(codec_ctx, format_ctx->streams[audio_stream_idx]->codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        goto cleanup;
    }
    
    // Open the codec
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        goto cleanup;
    }
    
    // Create resampler - Updated to handle newer FFmpeg versions
#if USING_NEW_FFMPEG_API
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, CHANNELS);
    
    // Create the context first, then configure it
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Failed to allocate swresample context\n");
        goto cleanup;
    }
    
    // Configure the resampler
    ret = swr_alloc_set_opts2(&swr_ctx,
                                &out_ch_layout,            // Output channel layout
                                AV_SAMPLE_FMT_S16,         // Output format
                                SAMPLE_RATE,               // Output sample rate
                                &codec_ctx->ch_layout,     // Input channel layout
                                codec_ctx->sample_fmt,     // Input format
                                codec_ctx->sample_rate,    // Input sample rate
                                0, NULL);
    
    if (ret < 0) {
        fprintf(stderr, "Failed to set resampler options\n");
        goto cleanup;
    }
#else
    // For older FFmpeg versions
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    swr_ctx = swr_alloc_set_opts(NULL,
                                out_channel_layout,         // Output channel layout
                                AV_SAMPLE_FMT_S16,          // Output format
                                SAMPLE_RATE,                // Output sample rate
                                codec_ctx->channel_layout,  // Input channel layout
                                codec_ctx->sample_fmt,      // Input format
                                codec_ctx->sample_rate,     // Input sample rate
                                0, NULL);
#endif
    
    if (!swr_ctx || swr_init(swr_ctx) < 0) {
        fprintf(stderr, "Failed to initialize resampler\n");
        goto cleanup;
    }
    
    // Allocate packet and frame
    packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        goto cleanup;
    }
    
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Failed to allocate frame\n");
        goto cleanup;
    }
    
    // Read packets and process frames
    while (keep_running) {
        // Only load and process audio when it's being played
        if (is_audio_playing) {
            // Reset file position to beginning when starting playback
            avformat_seek_file(format_ctx, audio_stream_idx, 0, 0, 0, 0);
            
            // Read audio data
            while (is_audio_playing && keep_running && av_read_frame(format_ctx, packet) >= 0) {
                if (packet->stream_index == audio_stream_idx) {
                    ret = avcodec_send_packet(codec_ctx, packet);
                    if (ret < 0) {
                        fprintf(stderr, "Error sending packet for decoding\n");
                        break;
                    }
                    
                    while (ret >= 0 && is_audio_playing) {
                        ret = avcodec_receive_frame(codec_ctx, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            fprintf(stderr, "Error during decoding\n");
                            goto cleanup;
                        }
                        
                        // Resample the audio
                        int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
                                                      SAMPLE_RATE, codec_ctx->sample_rate, AV_ROUND_UP);
                        
                        // Buffer for resampled data
                        uint8_t *resampled_data = malloc(out_samples * CHANNELS * sizeof(int16_t));
                        if (!resampled_data) {
                            fprintf(stderr, "Failed to allocate resampled data buffer\n");
                            goto cleanup;
                        }
                        
                        uint8_t *out_buffer[1] = { resampled_data };
                        int resampled = swr_convert(swr_ctx, out_buffer, out_samples,
                                                    (const uint8_t **)frame->data, frame->nb_samples);
                        
                        if (resampled < 0) {
                            fprintf(stderr, "Error resampling audio\n");
                            free(resampled_data);
                            goto cleanup;
                        }
                        
                        // Write resampled data to circular buffer
                        size_t data_size = resampled * CHANNELS * sizeof(int16_t);
                        write_to_buffer(&audio_buffer, resampled_data, data_size);
                        
                        free(resampled_data);
                    }
                }
                
                av_packet_unref(packet);
            }
            
            // If we reached end of file, stop playing
            if (!keep_running || !is_audio_playing) {
                continue;
            }
            
            // If we get here, we've reached the end of the file
            // Reset play state and seek back to beginning for next time
            is_audio_playing = 0;
            avformat_seek_file(format_ctx, audio_stream_idx, 0, 0, 0, 0);
        } else {
            // Sleep when not playing to avoid using CPU
            usleep(10000); // 10ms
        }
    }
    
cleanup:
    if (swr_ctx) swr_free(&swr_ctx);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    
    return NULL;
}

// Setup audio output system
static void setup_audio_system(void) {
#ifdef _WIN32
    // WinMM setup
    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    // Create a buffer for audio data
    waveBuffer = (LPBYTE)malloc(waveBufferSize);
    if (!waveBuffer) {
        fprintf(stderr, "Failed to allocate wave buffer\n");
        return;
    }

    // Open the device
    MMRESULT result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        fprintf(stderr, "Failed to open WinMM audio device: %d\n", result);
        free(waveBuffer);
        waveBuffer = NULL;
        return;
    }

    // Setup the wave header
    memset(&waveHeader, 0, sizeof(WAVEHDR));
    waveHeader.lpData = (LPSTR)waveBuffer;
    waveHeader.dwBufferLength = waveBufferSize;
    waveHeader.dwFlags = 0;

    // Create a thread for playback
    pthread_t playback_thread;
    if (pthread_create(&playback_thread, NULL, winmm_playback_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create WinMM playback thread\n");
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
        free(waveBuffer);
        waveBuffer = NULL;
        return;
    }
    pthread_detach(playback_thread);

    printf("%s audio system initialized\n", AUDIO_SYSTEM);
#else
    // ALSA setup
    int err;
    
    // Open PCM device
    if ((err = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "Cannot open audio device: %s\n", snd_strerror(err));
        return;
    }
    
    // Set parameters
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_handle, hw_params);
    
    snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_handle, hw_params, CHANNELS);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &rate, 0);
    
    snd_pcm_uframes_t buffer_size = 1024;
    snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_size);
    
    if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot set parameters: %s\n", snd_strerror(err));
        return;
    }
    
    // Start playback thread
    pthread_t playback_thread;
    if (pthread_create(&playback_thread, NULL, alsa_playback_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create ALSA playback thread\n");
        return;
    }
    pthread_detach(playback_thread);
    
    printf("%s audio system initialized\n", AUDIO_SYSTEM);
#endif
}

#ifdef _WIN32
// WinMM playback thread
static void *winmm_playback_thread(void *arg) {
    MMRESULT result;
    
    while (keep_running) {
        if (is_audio_playing) {
            // Read data from circular buffer
            int bytes_read = read_from_buffer(&audio_buffer, 
                                            (uint8_t *)waveBuffer, 
                                            waveBufferSize);
            
            if (bytes_read > 0) {
                // Prepare the header
                waveHeader.dwBufferLength = bytes_read;
                result = waveOutPrepareHeader(hWaveOut, &waveHeader, sizeof(WAVEHDR));
                if (result != MMSYSERR_NOERROR) {
                    fprintf(stderr, "Failed to prepare wave header: %d\n", result);
                    Sleep(10);
                    continue;
                }
                
                // Write to device
                result = waveOutWrite(hWaveOut, &waveHeader, sizeof(WAVEHDR));
                if (result != MMSYSERR_NOERROR) {
                    fprintf(stderr, "Failed to write to audio device: %d\n", result);
                    waveOutUnprepareHeader(hWaveOut, &waveHeader, sizeof(WAVEHDR));
                    Sleep(10);
                    continue;
                }
                
                // Wait for playback to complete
                while ((waveHeader.dwFlags & WHDR_DONE) == 0 && keep_running && is_audio_playing) {
                    Sleep(10); // 10ms
                }
                
                // Unprepare the header
                waveOutUnprepareHeader(hWaveOut, &waveHeader, sizeof(WAVEHDR));
            } else {
                // No data, sleep a bit
                Sleep(10);
            }
        } else {
            // Not playing, sleep a bit
            Sleep(10);
        }
    }
    
    return NULL;
}
#endif

#ifndef _WIN32
// ALSA playback thread
static void *alsa_playback_thread(void *arg) {
    int err;
    const int period_size = 1024;
    int16_t buffer[period_size * CHANNELS];
    
    while (keep_running) {
        if (is_audio_playing) {
            // Read data from circular buffer
            int bytes_read = read_from_buffer(&audio_buffer, (uint8_t *)buffer, period_size * CHANNELS * sizeof(int16_t));
            int frames_read = bytes_read / (CHANNELS * sizeof(int16_t));
            
            if (frames_read > 0) {
                int frames_written = 0;
                
                while (frames_written < frames_read) {
                    int frames = snd_pcm_writei(pcm_handle, buffer + frames_written * CHANNELS, frames_read - frames_written);
                    
                    if (frames < 0) {
                        // Handle underrun
                        frames = snd_pcm_recover(pcm_handle, frames, 0);
                        if (frames < 0) {
                            fprintf(stderr, "ALSA write error: %s\n", snd_strerror(frames));
                            break;
                        }
                    } else {
                        frames_written += frames;
                    }
                }
            }
        } else {
            // No data or not playing, sleep a bit
            usleep(10000);  // 10ms
        }
    }
    
    return NULL;
}
#endif

// Clean up audio system
static void cleanup_audio_system(void) {
#ifdef _WIN32
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
    }
    
    if (waveBuffer) {
        free(waveBuffer);
        waveBuffer = NULL;
    }
#else
    // ALSA cleanup
    if (pcm_handle) {
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
    }
#endif
}

// Play audio
static void play_audio(void) {
    is_audio_playing = 1;
}

// Stop audio
static void stop_audio(void) {
    is_audio_playing = 0;
}

// Create a surface of the appropriate size
static void create_surface(GtkWidget *widget) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    
    // Create a new surface if needed
    if (surface) {
        cairo_surface_destroy(surface);
    }
    
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    
    // Clear the surface
    clear_surface();
}

// Clear the drawing surface
static void clear_surface(void) {
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);  // White
    cairo_paint(cr);
    cairo_destroy(cr);
    
    // Free the line list
    g_slist_free_full(lines, free);
    lines = NULL;
}

// Configure event handler - create or resize surface 
static gboolean on_configure_event(GtkWidget *widget, GdkEventConfigure *event, gpointer data) {
    if (!surface) {
        create_surface(widget);
    } else {
        // Resize surface
        int width = gtk_widget_get_allocated_width(widget);
        int height = gtk_widget_get_allocated_height(widget);
        
        cairo_surface_t *new_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cr = cairo_create(new_surface);
        
        // Copy old surface to new one
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        
        // Replace old surface
        cairo_surface_destroy(surface);
        surface = new_surface;
    }
    
    return TRUE;
}

// Draw event handler
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    // Draw the surface
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    
    return FALSE;
}

// Add a line to the drawing
static void add_line(double x1, double y1, double x2, double y2, GdkRGBA color, double width, int is_eraser) {
    Line *line = malloc(sizeof(Line));
    line->start.x = x1;
    line->start.y = y1;
    line->end.x = x2;
    line->end.y = y2;
    line->color = color;
    line->width = width;
    line->is_eraser = is_eraser;
    
    lines = g_slist_append(lines, line);
    
    // Draw the line
    cairo_t *cr = cairo_create(surface);
    
    if (is_eraser) {
        // For eraser, draw using a white stroke
        cairo_set_source_rgb(cr, 1, 1, 1);
    } else {
        // Fixed GdkRGBA structure members
        cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
    }
    
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
    
    cairo_destroy(cr);
}

// Button press event handler
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    if (event->button == 1) { // Left button
        // Start drawing
        is_drawing = 1;
        last_point.x = event->x;
        last_point.y = event->y;
        
        // Play the appropriate sound
        if (current_tool == TOOL_PENCIL) {
            play_audio();
        }
    }
    return TRUE;
}

// Button release event handler
static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    if (event->button == 1) { // Left button
        // Stop drawing
        is_drawing = 0;
        
        // Stop all sounds
        stop_audio();
    }
    return TRUE;
}

// Motion event handler
static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer data) {
    if (is_drawing) {
        double x = event->x;
        double y = event->y;
        
        // Calculate distance moved
        double dx = x - last_point.x;
        double dy = y - last_point.y;
        double distance = sqrt(dx*dx + dy*dy);
        
        // Only draw if we've moved a bit (to avoid excessive small segments)
        if (distance > 2.0) {
            if (current_tool == TOOL_PENCIL) {
                add_line(last_point.x, last_point.y, x, y, current_color, pencil_size, 0);
                // Make sure pencil sound is playing
                play_audio();
            } else {
                add_line(last_point.x, last_point.y, x, y, current_color, eraser_size, 1);
            }
            
            last_point.x = x;
            last_point.y = y;
            
            // Request redraw
            gtk_widget_queue_draw(widget);
        }
    }
    
    return TRUE;
}

// Key press event handler
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    switch (event->keyval) {
        case GDK_KEY_p:
        case GDK_KEY_P:
            // Switch to pencil
            current_tool = TOOL_PENCIL;
            printf("Switched to pencil\n");
            break;
            
        case GDK_KEY_e: 
            // Switch to eraser
            current_tool = TOOL_ERASER;
            printf("Switched to eraser\n");
            break;
            
        case GDK_KEY_c:
        case GDK_KEY_C:
            // Clear drawing
            clear_surface();
            gtk_widget_queue_draw(widget);
            printf("Cleared drawing\n");
            break;
            
        case GDK_KEY_1:
            // Red color - fixed GdkRGBA structure
            current_color.red = 1.0;
            current_color.green = 0.0;
            current_color.blue = 0.0;
            current_color.alpha = 1.0;
            printf("Switched to red color\n");
            break;
            
        case GDK_KEY_2:
            // Green color - fixed GdkRGBA structure
            current_color.red = 0.0;
            current_color.green = 0.8;
            current_color.blue = 0.0;
            current_color.alpha = 1.0;
            printf("Switched to green color\n");
            break;
            
        case GDK_KEY_3:
            // Blue color - fixed GdkRGBA structure
            current_color.red = 0.0;
            current_color.green = 0.0;
            current_color.blue = 1.0;
            current_color.alpha = 1.0;
            printf("Switched to blue color\n");
            break;
            
        case GDK_KEY_4:
            // Black color - fixed GdkRGBA structure
            current_color.red = 0.0;
            current_color.green = 0.0;
            current_color.blue = 0.0;
            current_color.alpha = 1.0;
            printf("Switched to black color\n");
            break;
            
        case GDK_KEY_plus:
        case GDK_KEY_equal:
            // Increase brush size
            if (current_tool == TOOL_PENCIL) {
                pencil_size += 1.0;
                if (pencil_size > 20.0) pencil_size = 20.0;
                printf("Pencil size: %.1f\n", pencil_size);
            } else {
                eraser_size += 5.0;
                if (eraser_size > 50.0) eraser_size = 50.0;
                printf("Eraser size: %.1f\n", eraser_size);
            }
            break;
            
        case GDK_KEY_minus:
            // Decrease brush size
            if (current_tool == TOOL_PENCIL) {
                pencil_size -= 1.0;
                if (pencil_size < 1.0) pencil_size = 1.0;
                printf("Pencil size: %.1f\n", pencil_size);
            } else {
                eraser_size -= 5.0;
                if (eraser_size < 5.0) eraser_size = 5.0;
                printf("Eraser size: %.1f\n", eraser_size);
            }
            break;
    }
    
    return TRUE;
}

// Color button click handler
static void on_color_button(GtkWidget *widget, gpointer data) {
    const gchar *button_name = gtk_widget_get_name(widget);
    
    if (g_strcmp0(button_name, "red_button") == 0) {
        // Fixed GdkRGBA structure
        current_color.red = 1.0;
        current_color.green = 0.0;
        current_color.blue = 0.0;
        current_color.alpha = 1.0;
    } else if (g_strcmp0(button_name, "green_button") == 0) {
        // Fixed GdkRGBA structure
        current_color.red = 0.0;
        current_color.green = 0.8;
        current_color.blue = 0.0;
        current_color.alpha = 1.0;
    } else if (g_strcmp0(button_name, "blue_button") == 0) {
        // Fixed GdkRGBA structure
        current_color.red = 0.0;
        current_color.green = 0.0;
        current_color.blue = 1.0;
        current_color.alpha = 1.0;
    } else if (g_strcmp0(button_name, "black_button") == 0) {
        // Fixed GdkRGBA structure
        current_color.red = 0.0;
        current_color.green = 0.0;
        current_color.blue = 0.0;
        current_color.alpha = 1.0;
    }
}

// Tool button click handler
static void on_tool_button(GtkWidget *widget, gpointer data) {
    const gchar *button_name = gtk_widget_get_name(widget);
    
    if (g_strcmp0(button_name, "pencil_button") == 0) {
        current_tool = TOOL_PENCIL;
        printf("Switched to pencil\n");
    } else if (g_strcmp0(button_name, "eraser_button") == 0) {
        current_tool = TOOL_ERASER;
        printf("Switched to eraser\n");
    }
}

// Helper function to draw color swatches
static gboolean on_draw_color(GtkWidget *widget, cairo_t *cr, gpointer data);

static gboolean on_draw_color(GtkWidget *widget, cairo_t *cr, gpointer data) {
    uintptr_t color_val = (uintptr_t)data;
    double r = ((color_val >> 24) & 0xFF) / 255.0;
    double g = ((color_val >> 16) & 0xFF) / 255.0;
    double b = ((color_val >> 8) & 0xFF) / 255.0;
    double a = (color_val & 0xFF) / 255.0;
    
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    cairo_fill(cr);
    
    return FALSE;
}



int main(int argc, char *argv[]) {
    // Check arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <audio_file>\n", argv[0]);
        return 1;
    }
    
    // Set the audio file path
    audio_file = argv[1];
    
    // Initialize circular buffer for audio data
    init_circular_buffer(&audio_buffer);
    
    // Initialize GTK
    gtk_init(&argc, &argv);
    
    // Create main window
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Sound-Based Drawing App");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    // Create a vbox for vertical layout
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    
    // Create toolbar for controls
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 5);
    
    // Create tool buttons
    GtkWidget *pencil_button = gtk_button_new_with_label("Pencil");
    gtk_widget_set_name(pencil_button, "pencil_button");
    g_signal_connect(pencil_button, "clicked", G_CALLBACK(on_tool_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), pencil_button, FALSE, FALSE, 0);
    
    GtkWidget *eraser_button = gtk_button_new_with_label("Eraser");
    gtk_widget_set_name(eraser_button, "eraser_button");
    g_signal_connect(eraser_button, "clicked", G_CALLBACK(on_tool_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), eraser_button, FALSE, FALSE, 0);
    
    // Create color buttons
    GtkWidget *color_label = gtk_label_new("Colors:");
    gtk_box_pack_start(GTK_BOX(toolbar), color_label, FALSE, FALSE, 5);
    
    GtkWidget *red_button = gtk_button_new();
    gtk_widget_set_name(red_button, "red_button");
    GtkWidget *red_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(red_button), red_box);
    GtkWidget *red_color = gtk_drawing_area_new();
    gtk_widget_set_size_request(red_color, 20, 20);
    g_signal_connect(red_color, "draw", G_CALLBACK(on_draw_color), (gpointer)(uintptr_t)0xFF0000FF);
    gtk_container_add(GTK_CONTAINER(red_box), red_color);
    g_signal_connect(red_button, "clicked", G_CALLBACK(on_color_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), red_button, FALSE, FALSE, 0);
    
    GtkWidget *green_button = gtk_button_new();
    gtk_widget_set_name(green_button, "green_button");
    GtkWidget *green_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(green_button), green_box);
    GtkWidget *green_color = gtk_drawing_area_new();
    gtk_widget_set_size_request(green_color, 20, 20);
    g_signal_connect(green_color, "draw", G_CALLBACK(on_draw_color), (gpointer)(uintptr_t)0x00CC00FF);
    gtk_container_add(GTK_CONTAINER(green_box), green_color);
    g_signal_connect(green_button, "clicked", G_CALLBACK(on_color_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), green_button, FALSE, FALSE, 0);
    
    GtkWidget *blue_button = gtk_button_new();
    gtk_widget_set_name(blue_button, "blue_button");
    GtkWidget *blue_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(blue_button), blue_box);
    GtkWidget *blue_color = gtk_drawing_area_new();
    gtk_widget_set_size_request(blue_color, 20, 20);
    g_signal_connect(blue_color, "draw", G_CALLBACK(on_draw_color), (gpointer)(uintptr_t)0x0000FFFF);
    gtk_container_add(GTK_CONTAINER(blue_box), blue_color);
    g_signal_connect(blue_button, "clicked", G_CALLBACK(on_color_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), blue_button, FALSE, FALSE, 0);
    
    GtkWidget *black_button = gtk_button_new();
    gtk_widget_set_name(black_button, "black_button");
    GtkWidget *black_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(black_button), black_box);
    GtkWidget *black_color = gtk_drawing_area_new();
    gtk_widget_set_size_request(black_color, 20, 20);
    g_signal_connect(black_color, "draw", G_CALLBACK(on_draw_color), (gpointer)(uintptr_t)0x000000FF);
    gtk_container_add(GTK_CONTAINER(black_box), black_color);
    g_signal_connect(black_button, "clicked", G_CALLBACK(on_color_button), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), black_button, FALSE, FALSE, 0);
    
    // Create clear button
    GtkWidget *clear_button = gtk_button_new_with_label("Clear");
    g_signal_connect_swapped(clear_button, "clicked", G_CALLBACK(clear_surface), NULL);
    g_signal_connect_swapped(clear_button, "clicked", G_CALLBACK(gtk_widget_queue_draw), drawing_area);
    gtk_box_pack_end(GTK_BOX(toolbar), clear_button, FALSE, FALSE, 0);
    
    // Create drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, 800, 600);
    gtk_box_pack_start(GTK_BOX(vbox), drawing_area, TRUE, TRUE, 0);
    
    // Setup signals for drawing area
    gtk_widget_add_events(drawing_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(drawing_area, "configure-event", G_CALLBACK(on_configure_event), NULL);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(drawing_area, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_signal_connect(drawing_area, "button-release-event", G_CALLBACK(on_button_release), NULL);
    g_signal_connect(drawing_area, "motion-notify-event", G_CALLBACK(on_motion_notify), NULL);
    
    // Setup key events for the main window
    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    
    // Setup audio system and start audio thread
    setup_audio_system();
    
    // Create audio thread
    if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create audio thread\n");
        return 1;
    }
    
    // Show all widgets
    gtk_widget_show_all(window);
    
    // Start GTK main loop
    gtk_main();
    
    // Cleanup
    keep_running = 0;
    pthread_join(audio_thread, NULL);
    cleanup_audio_system();
    
    return 0;
}