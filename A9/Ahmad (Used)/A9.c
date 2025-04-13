// Compile: make -f Makefile
// Run: ./A9 pencil.wav
//
// To convert to .wav: ffmpeg -i input.mp3 output.wav
// For MP3 support: ffmpeg -i input.mp3 -f wav - | ./A9 -

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
#define BUFFER_SIZE 32768  // Size of the circular buffer (increased for better performance)
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

// Point structure
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
static WAVEHDR waveHeader[2];  // Double-buffering for smoother playback
static int current_buffer = 0;
static DWORD waveBufferSize = 8192;  // Larger buffer (compared to 4096) for smoother playback
static LPBYTE waveBuffer[2] = {NULL, NULL};
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
            if (!keep_running || !is_audio_playing) {
                // Fill remaining with silence if shutting down or not playing
                memset(data + bytes_read, 0, size - bytes_read);
                bytes_read = size;
                break;
            }
            
            // Wait for data
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_nsec += 5000000;  // 5ms timeout for more responsive playback
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }
            
            int wait_result = pthread_cond_timedwait(&buffer->not_empty, &buffer->mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                // If timeout, fill a bit of silence but don't exit loop
                size_t silence_size = size / 10;  // Fill 10% of requested size with silence
                if (silence_size > 0) {
                    memset(data + bytes_read, 0, silence_size);
                    bytes_read += silence_size;
                }
            }
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
    
    // Check for stdin input
    if (strcmp(audio_file, "-") == 0) {
        // Reading from stdin (pipe)
        format_ctx->pb = avio_alloc_context(
            av_malloc(4096), 4096, 0, stdin, 
            NULL, NULL, NULL);
        if (!format_ctx->pb) {
            fprintf(stderr, "Failed to allocate AVIO context\n");
            goto cleanup;
        }
        
        // Open input from stdin
        if (avformat_open_input(&format_ctx, "pipe:", NULL, NULL) != 0) {
            fprintf(stderr, "Could not open stdin for reading\n");
            goto cleanup;
        }
    } else {
        // Open the file
        fprintf(stderr, "Attempting to open audio file: '%s'\n", audio_file);

        if (avformat_open_input(&format_ctx, audio_file, NULL, NULL) != 0) {
            fprintf(stderr, "Could not open audio file %s\n", audio_file);
            goto cleanup;
        }
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
    
    // Display audio information
    printf("Audio Information:\n");
    printf("  Format: %s\n", avcodec_get_name(codec_ctx->codec_id));
    printf("  Sample Rate: %d Hz\n", codec_ctx->sample_rate);
    printf("  Channels: %d\n", codec_ctx->ch_layout.nb_channels);
    printf("  Sample Format: %s\n", av_get_sample_fmt_name(codec_ctx->sample_fmt));

    // Ensure sample rate matching
    printf("Resampling from %d Hz to %d Hz\n", codec_ctx->sample_rate, SAMPLE_RATE);
    if (codec_ctx->sample_rate != SAMPLE_RATE) {
        printf("Note: Resampling may affect audio quality/speed\n");
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
                        
                        // Resample the audio - improved quality settings
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
                        
                        // Calculate actual output size
                        size_t data_size = resampled * CHANNELS * sizeof(int16_t);
                        
                        // Write resampled data to circular buffer
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

    // Create buffers for double-buffering
    for (int i = 0; i < 2; i++) {
        waveBuffer[i] = (LPBYTE)malloc(waveBufferSize);
        if (!waveBuffer[i]) {
            fprintf(stderr, "Failed to allocate wave buffer %d\n", i);
            return;
        }
        
        // Initialize with silence
        memset(waveBuffer[i], 0, waveBufferSize);
        
        // Initialize header
        memset(&waveHeader[i], 0, sizeof(WAVEHDR));
        waveHeader[i].lpData = (LPSTR)waveBuffer[i];
        waveHeader[i].dwBufferLength = waveBufferSize;
        waveHeader[i].dwFlags = 0;
    }

    // Open the device
    MMRESULT result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        fprintf(stderr, "Failed to open WinMM audio device: %d\n", result);
        for (int i = 0; i < 2; i++) {
            free(waveBuffer[i]);
            waveBuffer[i] = NULL;
        }
        return;
    }

    // Create a thread for playback
    pthread_t playback_thread;
    if (pthread_create(&playback_thread, NULL, winmm_playback_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create WinMM playback thread\n");
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
        for (int i = 0; i < 2; i++) {
            free(waveBuffer[i]);
            waveBuffer[i] = NULL;
        }
        return;
    }
    pthread_detach(playback_thread);

    printf("%s audio system initialized with double-buffering\n", AUDIO_SYSTEM);
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
// WinMM playback thread with double-buffering
static void *winmm_playback_thread(void *arg) {
    MMRESULT result;
    
    while (keep_running) {
        if (is_audio_playing) {
            // Get the current buffer
            WAVEHDR *header = &waveHeader[current_buffer];
            
            // Check if we need to prepare a new buffer
            if (!(header->dwFlags & WHDR_PREPARED) || (header->dwFlags & WHDR_DONE)) {
                // Unprepare if it was previously prepared
                if (header->dwFlags & WHDR_PREPARED) {
                    result = waveOutUnprepareHeader(hWaveOut, header, sizeof(WAVEHDR));
                    if (result != MMSYSERR_NOERROR) {
                        fprintf(stderr, "Failed to unprepare wave header: %d\n", result);
                    }
                }
                
                // Read new data from circular buffer
                int bytes_read = read_from_buffer(&audio_buffer, 
                                            (uint8_t *)waveBuffer[current_buffer], 
                                            waveBufferSize);
                
                if (bytes_read > 0) {
                    // Update header with actual bytes read
                    header->dwBufferLength = bytes_read;
                    header->dwFlags = 0;
                    
                    // Prepare header
                    result = waveOutPrepareHeader(hWaveOut, header, sizeof(WAVEHDR));
                    if (result != MMSYSERR_NOERROR) {
                        fprintf(stderr, "Failed to prepare wave header: %d\n", result);
                        Sleep(10);
                        continue;
                    }
                    
                    // Write to device
                    result = waveOutWrite(hWaveOut, header, sizeof(WAVEHDR));
                    if (result != MMSYSERR_NOERROR) {
                        fprintf(stderr, "Failed to write to audio device: %d\n", result);
                        waveOutUnprepareHeader(hWaveOut, header, sizeof(WAVEHDR));
                        Sleep(10);
                        continue;
                    }
                    
                    // Switch to next buffer
                    current_buffer = (current_buffer + 1) % 2;
                } else {
                    // No data, sleep a bit
                    Sleep(10);
                }
            } else {
                // Current buffer is still playing, sleep a bit
                Sleep(1);
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
        
        // Clean up each buffer
        for (int i = 0; i < 2; i++) {
            if (waveHeader[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            }
            
            if (waveBuffer[i]) {
                free(waveBuffer[i]);
                waveBuffer[i] = NULL;
            }
        }
        
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
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

// GTK4 helper for creating CSS providers
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "button.color-button { min-width: 30px; min-height: 30px; padding: 0; }\n"
        "button.tool-button { padding: 5px; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// Create a surface of the appropriate size
static void create_surface(GtkWidget *widget) {
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    
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

// Configure event handler (GTK4 style using the draw function)
static void on_resize(GtkDrawingArea *area, int width, int height, gpointer data) {
    if (!surface) {
        create_surface(GTK_WIDGET(area));
    } else {
        // Resize surface
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
}

// Draw function for GTK4
static void draw_function(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    if (surface) {
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
    }
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
        // Set the color
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

// Mouse press handler for GTK4
static void on_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    // Start drawing
    is_drawing = 1;
    last_point.x = x;
    last_point.y = y;
    
    // Play the appropriate sound
    if (current_tool == TOOL_PENCIL) {
        play_audio();
    }
}

// Mouse release handler for GTK4
static void on_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data) {
    // Stop drawing
    is_drawing = 0;
    
    // Stop all sounds
    stop_audio();
}

// Mouse motion handler for GTK4
static void on_motion(GtkEventControllerMotion *motion, double x, double y, gpointer data) {
    if (is_drawing) {
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
            gtk_widget_queue_draw(drawing_area);
        }
    }
}

// Key press handler for GTK4
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, 
                              guint keycode, GdkModifierType state, gpointer data) {
    switch (keyval) {
        case GDK_KEY_p:
        case GDK_KEY_P:
            // Switch to pencil
            current_tool = TOOL_PENCIL;
            printf("Switched to pencil\n");
            break;
            
        case GDK_KEY_e: 
        case GDK_KEY_E:
            // Switch to eraser
            current_tool = TOOL_ERASER;
            printf("Switched to eraser\n");
            break;
            
        case GDK_KEY_c:
        case GDK_KEY_C:
            // Clear drawing
            clear_surface();
            gtk_widget_queue_draw(drawing_area);
            printf("Cleared drawing\n");
            break;
            
        case GDK_KEY_1:
            // Red color
            current_color.red = 1.0;
            current_color.green = 0.0;
            current_color.blue = 0.0;
            current_color.alpha = 1.0;
            printf("Switched to red color\n");
            break;
            
        case GDK_KEY_2:
            // Green color
            current_color.red = 0.0;
            current_color.green = 0.8;
            current_color.blue = 0.0;
            current_color.alpha = 1.0;
            printf("Switched to green color\n");
            break;
            
        case GDK_KEY_3:
            // Blue color
            current_color.red = 0.0;
            current_color.green = 0.0;
            current_color.blue = 1.0;
            current_color.alpha = 1.0;
            printf("Switched to blue color\n");
            break;
            
        case GDK_KEY_4:
            // Black color
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
            
        default:
            return FALSE; // Event not handled
    }
    
    return TRUE; // Event handled
}

// Color button click handler
static void on_color_button_clicked(GtkWidget *widget, gpointer data) {
    GdkRGBA *color = (GdkRGBA*)data;
    current_color = *color;
    
    // Print the selected color
    printf("Selected color: R=%.1f G=%.1f B=%.1f\n", 
           current_color.red, current_color.green, current_color.blue);
}

// Tool button click handler
static void on_tool_button_clicked(GtkWidget *widget, gpointer data) {
    DrawingTool tool = GPOINTER_TO_INT(data);
    current_tool = tool;
    
    if (tool == TOOL_PENCIL) {
        printf("Switched to pencil\n");
    } else {
        printf("Switched to eraser\n");
    }
}

// Clear button click handler
static void on_clear_button_clicked(GtkWidget *widget, gpointer data) {
    clear_surface();
    gtk_widget_queue_draw(drawing_area);
    printf("Cleared drawing\n");
}

// Exit handler for the GTK4 application
static void on_app_shutdown(GApplication *app, gpointer user_data) {
    // Stop the audio thread before exiting
    keep_running = 0;
    pthread_join(audio_thread, NULL);
    cleanup_audio_system();
}

// Create a color button with custom styling
static GtkWidget* create_color_button(GdkRGBA color) {
    GtkWidget *button = gtk_button_new();
    
    // Store the color as user data
    GdkRGBA *color_data = g_new(GdkRGBA, 1);
    *color_data = color;
    g_object_set_data_full(G_OBJECT(button), "color", color_data, g_free);
    
    // Set button style classes
    gtk_widget_add_css_class(button, "color-button");
    
    // Create a color swatch
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(box, 24, 24);
    
    // Set background color
    GtkCssProvider *provider = gtk_css_provider_new();
    
    char css[100];
    snprintf(css, sizeof(css),
             "box { background-color: rgba(%.3f, %.3f, %.3f, %.3f); }",
             color.red, color.green, color.blue, color.alpha);
    
    gtk_css_provider_load_from_string(provider, css);
    GdkDisplay *display = gtk_widget_get_display(box);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), 
                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    
    gtk_button_set_child(GTK_BUTTON(button), box);
    
    // Connect click handler
    g_signal_connect(button, "clicked", G_CALLBACK(on_color_button_clicked), color_data);
    
    return button;
}


// Add this function to handle the 'activate' signal
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window;
    GtkWidget *vbox, *toolbar;
    GtkWidget *pencil_button, *eraser_button;
    GtkWidget *clear_button;
    GtkWidget *color_label;
    GtkWidget *red_button, *green_button, *blue_button, *black_button;
    GtkGesture *click_gesture;
    GtkEventController *motion_controller, *key_controller;

    // Load CSS styles
    load_css();
    
    // Create the main window
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Sound-Based Drawing App");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    
    // Create a vertical box layout
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_window_set_child(GTK_WINDOW(window), vbox);
    
    // Create toolbar for controls
    toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_start(toolbar, 5);
    gtk_widget_set_margin_end(toolbar, 5);
    gtk_widget_set_margin_top(toolbar, 5);
    gtk_widget_set_margin_bottom(toolbar, 5);
    gtk_box_append(GTK_BOX(vbox), toolbar);
    
    // Create tool buttons
    pencil_button = gtk_button_new_with_label("Pencil");
    gtk_widget_add_css_class(pencil_button, "tool-button");
    g_signal_connect(pencil_button, "clicked", G_CALLBACK(on_tool_button_clicked), 
                    GINT_TO_POINTER(TOOL_PENCIL));
    gtk_box_append(GTK_BOX(toolbar), pencil_button);
    
    eraser_button = gtk_button_new_with_label("Eraser");
    gtk_widget_add_css_class(eraser_button, "tool-button");
    g_signal_connect(eraser_button, "clicked", G_CALLBACK(on_tool_button_clicked), 
                    GINT_TO_POINTER(TOOL_ERASER));
    gtk_box_append(GTK_BOX(toolbar), eraser_button);
    
    // Color label
    color_label = gtk_label_new("Colors:");
    gtk_box_append(GTK_BOX(toolbar), color_label);
    
    // Create color buttons
    GdkRGBA red_color = {1.0, 0.0, 0.0, 1.0};
    red_button = create_color_button(red_color);
    gtk_box_append(GTK_BOX(toolbar), red_button);
    
    GdkRGBA green_color = {0.0, 0.8, 0.0, 1.0};
    green_button = create_color_button(green_color);
    gtk_box_append(GTK_BOX(toolbar), green_button);
    
    GdkRGBA blue_color = {0.0, 0.0, 1.0, 1.0};
    blue_button = create_color_button(blue_color);
    gtk_box_append(GTK_BOX(toolbar), blue_button);
    
    GdkRGBA black_color = {0.0, 0.0, 0.0, 1.0};
    black_button = create_color_button(black_color);
    gtk_box_append(GTK_BOX(toolbar), black_button);
    
    // Create clear button (on the right side)
    clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_hexpand(clear_button, TRUE);
    gtk_widget_set_halign(clear_button, GTK_ALIGN_END);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), NULL);
    gtk_box_append(GTK_BOX(toolbar), clear_button);
    
    // Create drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_box_append(GTK_BOX(vbox), drawing_area);
    
    // Set up drawing area
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_function, NULL, NULL);
    
    // Connect resize callback
    g_signal_connect(drawing_area, "resize", G_CALLBACK(on_resize), NULL);
    
    // Set up gestures and event controllers
    click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(click_gesture));
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_pressed), NULL);
    g_signal_connect(click_gesture, "released", G_CALLBACK(on_released), NULL);
    
    motion_controller = gtk_event_controller_motion_new();
    gtk_widget_add_controller(drawing_area, motion_controller);
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_motion), NULL);
    
    // Add key controller to window
    key_controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(window, key_controller);
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    
    // Show the window
    gtk_window_present(GTK_WINDOW(window));
}


int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;
    
    // Check arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <audio_file>\n", argv[0]);
        return 1;
    }
    
    // Set the audio file path
    audio_file = argv[1];
    
    // Initialize circular buffer for audio data
    init_circular_buffer(&audio_buffer);
    
    // Create and start audio thread
    if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create audio thread\n");
        return 1;
    }
    
    // Setup audio system
    setup_audio_system();
    
    // Initialize GTK
    app = gtk_application_new("org.example.sounddraw", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    
    char* gtk_argv[1];
    gtk_argv[0] = argv[0];
    int gtk_argc = 1;
    status = g_application_run(G_APPLICATION(app), gtk_argc, gtk_argv);
    
    // Clean up
    g_object_unref(app);
    
    return status;
}