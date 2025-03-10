/**
    * A7.c - Video Player with GTK4 and FFmpeg
    * 
    * This program plays a video file using FFmpeg for decoding and GTK4 for display.
    * It utilizes pthreads for decoding and a GTK timer for displaying frames.
    * Frame rate can be specified as a command line argument.
    *
    * Compilation:
    * gcc A7.c -o A7 `pkg-config --cflags --libs gtk4` -lavformat -lavcodec -lavutil -lswscale -pthread
    * 
    * Usage: ./A7 <video_file> <frame_rate>
    * Example: ./A7 1.mp4 60
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <gtk/gtk.h>

// FFmpeg libraries
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// Size of the circular buffer (number of frames)
#define BUFFER_SIZE 30

// Structure to hold frame data
typedef struct {
    uint8_t *data;
    int width;
    int height;
    int linesize;
    int size;
} FrameData;

// Circular buffer for frames
typedef struct {
    FrameData frames[BUFFER_SIZE];
    int read_pos;
    int write_pos;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} CircularBuffer;

// Global variables
CircularBuffer buffer;
GtkWidget *picture;
GtkApplication *app;
int frame_rate;
int running = 1;

// FFmpeg context variables
AVFormatContext *format_context = NULL;
AVCodecContext *codec_context = NULL;
int video_stream_index = -1;
struct SwsContext *sws_context = NULL;


// Initializes the circular buffer
void init_buffer() {
    buffer.read_pos = 0;
    buffer.write_pos = 0;
    buffer.count = 0;
    
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.not_empty, NULL);
    pthread_cond_init(&buffer.not_full, NULL);
    
    // Initializes all frames
    for (int i = 0; i < BUFFER_SIZE; i++) {
        buffer.frames[i].data = NULL;
        buffer.frames[i].size = 0;
    }
}


// To clean up the circular buffer resources
void destroy_buffer() {
    pthread_mutex_destroy(&buffer.mutex);
    pthread_cond_destroy(&buffer.not_empty);
    pthread_cond_destroy(&buffer.not_full);
    
    // Frees any allocated frame data
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (buffer.frames[i].data != NULL) {
            free(buffer.frames[i].data);
            buffer.frames[i].data = NULL;
        }
    }
}


// Adds a frame to the circular buffer
void buffer_push(uint8_t *data, int width, int height, int linesize) {
    pthread_mutex_lock(&buffer.mutex);
    
    // Wait if buffer is full
    while (buffer.count >= BUFFER_SIZE && running) {
        pthread_cond_wait(&buffer.not_full, &buffer.mutex);
    }
    
    // Checks if we're still running after waiting
    if (!running) {
        pthread_mutex_unlock(&buffer.mutex);
        return;
    }
    
    // Calculates buffer size
    int frame_size = height * linesize;
    
    // Allocates memory for frame if needed
    if (buffer.frames[buffer.write_pos].data == NULL || buffer.frames[buffer.write_pos].size < frame_size) {
        if (buffer.frames[buffer.write_pos].data != NULL) {
            free(buffer.frames[buffer.write_pos].data);
        }
        buffer.frames[buffer.write_pos].data = (uint8_t*)malloc(frame_size);
        buffer.frames[buffer.write_pos].size = frame_size;
    }
    
    // Copy frame data
    memcpy(buffer.frames[buffer.write_pos].data, data, frame_size);
    buffer.frames[buffer.write_pos].width = width;
    buffer.frames[buffer.write_pos].height = height;
    buffer.frames[buffer.write_pos].linesize = linesize;
    
    // Updates write position
    buffer.write_pos = (buffer.write_pos + 1) % BUFFER_SIZE;
    buffer.count++;
    
    // Signals that buffer is not empty
    pthread_cond_signal(&buffer.not_empty);
    pthread_mutex_unlock(&buffer.mutex);
}


/**
* Gets a frame from the circular buffer
* Returns 1 if successful, 0 if buffer is empty
*/
int buffer_pop(FrameData *frame) {
    pthread_mutex_lock(&buffer.mutex);
    
    // Returns 0 if buffer is empty and we're not running
    if (buffer.count <= 0) {
        if (!running) {
            pthread_mutex_unlock(&buffer.mutex);
            return 0;
        }
        // Waits for buffer to have data
        pthread_cond_wait(&buffer.not_empty, &buffer.mutex);
        if (buffer.count <= 0) {
            pthread_mutex_unlock(&buffer.mutex);
            return 0;
        }
    }
    
    // Copy frame data
    frame->width = buffer.frames[buffer.read_pos].width;
    frame->height = buffer.frames[buffer.read_pos].height;
    frame->linesize = buffer.frames[buffer.read_pos].linesize;
    frame->size = buffer.frames[buffer.read_pos].size;
    
    // Allocates memory for frame data if needed
    if (frame->data == NULL || frame->size < buffer.frames[buffer.read_pos].size) {
        if (frame->data != NULL) {
            free(frame->data);
        }
        frame->data = (uint8_t*)malloc(buffer.frames[buffer.read_pos].size);
    }
    
    // Copy frame data
    memcpy(frame->data, buffer.frames[buffer.read_pos].data, buffer.frames[buffer.read_pos].size);
    
    // Updates read position
    buffer.read_pos = (buffer.read_pos + 1) % BUFFER_SIZE;
    buffer.count--;
    
    // Signals that buffer is not full
    pthread_cond_signal(&buffer.not_full);
    pthread_mutex_unlock(&buffer.mutex);
    
    return 1;
}


// Initializes FFmpeg and open video file
int init_ffmpeg(const char *filename) {
    // Initializes FFmpeg
    format_context = avformat_alloc_context();
    if (!format_context) {
        fprintf(stderr, "Could not allocate AVFormatContext\n");
        return 0;
    }
    
    // Opens input file
    if (avformat_open_input(&format_context, filename, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open input file: %s\n", filename);
        return 0;
    }
    
    // Gets stream information
    if (avformat_find_stream_info(format_context, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return 0;
    }
    
    // Finds video stream
    for (unsigned int i = 0; i < format_context->nb_streams; i++) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    
    if (video_stream_index == -1) {
        fprintf(stderr, "Could not find video stream\n");
        return 0;
    }
    
    // Gets codec
    const AVCodec *codec = avcodec_find_decoder(format_context->streams[video_stream_index]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        return 0;
    }
    
    // Allocates codec context
    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context) {
        fprintf(stderr, "Could not allocate codec context\n");
        return 0;
    }
    
    // Copies codec parameters
    if (avcodec_parameters_to_context(codec_context, format_context->streams[video_stream_index]->codecpar) < 0) {
        fprintf(stderr, "Could not copy codec parameters to context\n");
        return 0;
    }
    
    // Opens codec
    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 0;
    }
    
    return 1;
}


// To clean up FFmpeg resources
void cleanup_ffmpeg() {
    if (codec_context) {
        avcodec_free_context(&codec_context);
    }
    
    if (format_context) {
        avformat_close_input(&format_context);
        avformat_free_context(format_context);
    }
    
    if (sws_context) {
        sws_freeContext(sws_context);
    }
}


// Decodes thread function
void *decode_thread(void *arg) {
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    
    if (!frame || !rgb_frame || !packet) {
        fprintf(stderr, "Could not allocate frames or packet\n");
        return NULL;
    }
    
    // Allocates buffer for RGB frame
    int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codec_context->width, codec_context->height, 1);
    uint8_t *rgb_buffer = (uint8_t*)av_malloc(rgb_size);
    
    if (!rgb_buffer) {
        fprintf(stderr, "Could not allocate RGB buffer\n");
        return NULL;
    }
    
    // Sets up RGB frame
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, 
                        rgb_buffer, AV_PIX_FMT_RGB24, 
                        codec_context->width, codec_context->height, 1);
    
    // Initializes software scaler
    sws_context = sws_getContext(codec_context->width, codec_context->height, codec_context->pix_fmt,
                                codec_context->width, codec_context->height, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, NULL, NULL, NULL);
    
    if (!sws_context) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        return NULL;
    }
    
    // Reads frames
    while (running && av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            // Send packet to decoder
            int ret = avcodec_send_packet(codec_context, packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet for decoding\n");
                break;
            }
            
            // Get decoded frame
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_context, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    running = 0;
                    break;
                }
                
                // Convert to RGB
                sws_scale(sws_context, (const uint8_t * const*)frame->data, frame->linesize,
                        0, codec_context->height, rgb_frame->data, rgb_frame->linesize);
                
                // Add to buffer
                buffer_push(rgb_frame->data[0], codec_context->width, codec_context->height, rgb_frame->linesize[0]);
            }
        }
        
        av_packet_unref(packet);
    }
    
    // To flush decoder
    avcodec_send_packet(codec_context, NULL);
    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error during flushing decoder\n");
            break;
        }
        
        // Convert to RGB
        sws_scale(sws_context, (const uint8_t * const*)frame->data, frame->linesize,
                0, codec_context->height, rgb_frame->data, rgb_frame->linesize);
        
        // Add to buffer
        buffer_push(rgb_frame->data[0], codec_context->width, codec_context->height, rgb_frame->linesize[0]);
    }
    
    // Cleanup
    running = 0;  // Signal display thread to stop
    pthread_cond_signal(&buffer.not_empty);  // Wakes up display thread if waiting
    
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    av_free(rgb_buffer);
    
    return NULL;
}


// Displays function called by GTK timer
gboolean display_frame(gpointer data) {
    static FrameData frame = {NULL, 0, 0, 0, 0};
    
    if (!running) {
        // Cleanup frame data
        if (frame.data != NULL) {
            free(frame.data);
            frame.data = NULL;
        }
        return FALSE;  // Stops timer
    }
    
    // Get frame from buffer
    if (buffer_pop(&frame)) {
        // Creates a GdkPixbuf from the frame data
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
            frame.data,
            GDK_COLORSPACE_RGB,
            FALSE,  // No alpha channel
            8,      // 8 bits per sample
            frame.width,
            frame.height,
            frame.linesize,
            NULL,   // No destroy function
            NULL    // No destroy function data
        );
        
        // Creates a texture from the pixbuf
        GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf);
        
        gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
        
        // Release resources
        g_object_unref(texture);
        g_object_unref(pixbuf);
    }
    
    return TRUE;  // Continue timer
}


// Called when the application is activated
static void app_activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Video Player");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    
    // Creates picture widget
    picture = gtk_picture_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 30);   // Left margin
    gtk_widget_set_margin_end(box, 30);     // Right margin
    gtk_widget_set_margin_top(box, 30);
    gtk_widget_set_margin_bottom(box, 30);

    gtk_box_append(GTK_BOX(box), picture);  // Adds the picture to the box
    gtk_window_set_child(GTK_WINDOW(window), box);  // Adds the box to the window
    
    // Set up timer for frame display
    // 1000ms / frame_rate = milliseconds per frame
    g_timeout_add(1000 / frame_rate, display_frame, NULL);
    
    // Shows window
    gtk_window_present(GTK_WINDOW(window));
}


// Called when the application window is closed
static void on_app_shutdown(GtkApplication *app, gpointer user_data) {
    running = 0;  // Signal threads to stop
    
    // Wakes up threads if they are waiting
    pthread_cond_signal(&buffer.not_empty);
    pthread_cond_signal(&buffer.not_full);
}



int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <video_file> <frame_rate>\n", argv[0]);
        return 1;
    }
    
    frame_rate = atoi(argv[2]);
    if (frame_rate <= 0) {
        fprintf(stderr, "Invalid frame rate: %s\n", argv[2]);
        return 1;
    }
    
    // Initializes buffer
    init_buffer();
    
    // Initializes FFmpeg
    if (!init_ffmpeg(argv[1])) {
        destroy_buffer();
        return 1;
    }
    
    // Creates decode thread
    pthread_t decode_tid;
    if (pthread_create(&decode_tid, NULL, decode_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create decode thread\n");
        cleanup_ffmpeg();
        destroy_buffer();
        return 1;
    }
    
    // Creates GTK application
    app = gtk_application_new("org.example.videoapp", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
    
    // To run the application
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    
    // Waits for decode thread to finish
    pthread_join(decode_tid, NULL);
    
    // Cleaning up resources
    g_object_unref(app);
    cleanup_ffmpeg();
    destroy_buffer();
    
    return status;
}