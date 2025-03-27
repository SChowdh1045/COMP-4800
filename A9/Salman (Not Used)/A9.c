/*
    A9: Animation with audio - Drawing Canvas
    
    Features:
    - GTK4 drawing canvas
    - Audio playback from WAV file during drawing
    - Thread synchronization with circular buffer
    - No busy-waiting
    
    Compile: gcc -o A9 A9.c `pkg-config --cflags --libs gtk4` -lole32 -loleaut32 -luuid -lwinmm -lm
    Run: ./A9 audio.wav
*/

#include <gtk/gtk.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <windows.h>

// Add WinMM for simpler audio playback as fallback
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// These must come before COM headers
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
#include <initguid.h>

// MinGW WASAPI COM support
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

// Constants
#define BUFFER_SIZE 10  // Size of circular buffer for messages
#define REFTIMES_PER_SEC 10000000  // 1 second in 100-nanosecond units
#define BUFFER_DURATION 500  // 500 ms buffer

// Audio API types
typedef enum {
    AUDIO_API_NONE,
    AUDIO_API_WASAPI,
    AUDIO_API_WINMM
} AudioApiType;

// Message types for thread communication
typedef enum {
    MSG_START_AUDIO,
    MSG_STOP_AUDIO,
    MSG_EXIT
} MessageType;

// Message structure
typedef struct {
    MessageType type;
} Message;

// Circular buffer structure
typedef struct {
    Message *buffer;
    int size;
    int read_pos;
    int write_pos;
    int count;
} CircularBuffer;

// App state structure
typedef struct {
    // GTK widgets
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *drawing_area;
    
    // Drawing state
    cairo_surface_t *surface;
    gboolean is_drawing;
    gdouble last_x;
    gdouble last_y;
    
    // Audio state
    HANDLE audio_thread;
    volatile BOOL audio_playing;
    char *audio_filename;
    volatile BOOL audio_initialized;
    volatile BOOL thread_should_exit;
    AudioApiType audio_api;
    
    // WASAPI audio objects
    IAudioClient *audio_client;
    IAudioRenderClient *render_client;
    WAVEFORMATEX *wave_format;
    
    // WinMM audio objects
    HWAVEOUT hwo;
    WAVEHDR wh;
    
    // Audio data
    void *audio_data;
    DWORD audio_data_size;
    DWORD audio_sample_count;
    
    // Thread communication
    CircularBuffer *message_buffer;
    HANDLE mutex;
    HANDLE buffer_not_empty;  // Condition variable
    HANDLE buffer_not_full;   // Condition variable
} AppState;

// Function prototypes
CircularBuffer* create_circular_buffer(int size);
void destroy_circular_buffer(CircularBuffer *buffer);
BOOL write_message(CircularBuffer *buffer, Message msg, HANDLE mutex, HANDLE buffer_not_full, HANDLE buffer_not_empty);
BOOL read_message(CircularBuffer *buffer, Message *msg, HANDLE mutex, HANDLE buffer_not_empty, HANDLE buffer_not_full);
static void send_message(AppState *app, MessageType type);

static DWORD WINAPI audio_thread_func(LPVOID lpParam);
static HRESULT load_wav_file(AppState *app);
static HRESULT initialize_wasapi(AppState *app);
static HRESULT initialize_winmm(AppState *app);
static void start_audio(AppState *app);
static void stop_audio(AppState *app);

static void activate(GtkApplication *app, gpointer user_data);
static void cleanup_app(AppState *app);
static void clear_surface(cairo_surface_t *surface);
static void resize_cb(GtkWidget *widget, int width, int height, gpointer data);
static void draw_function(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data);
static void pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, AppState *app);
static void released_cb(GtkGestureClick *gesture, int n_press, double x, double y, AppState *app);
static void motion_cb(GtkEventControllerMotion *controller, double x, double y, AppState *app);
static void draw_brush_stroke(AppState *app, double x, double y);

// Circular buffer implementation
CircularBuffer* create_circular_buffer(int size) {
    CircularBuffer *buffer = malloc(sizeof(CircularBuffer));
    if (!buffer) return NULL;
    
    buffer->buffer = malloc(sizeof(Message) * size);
    if (!buffer->buffer) {
        free(buffer);
        return NULL;
    }
    
    buffer->size = size;
    buffer->read_pos = 0;
    buffer->write_pos = 0;
    buffer->count = 0;
    
    return buffer;
}

void destroy_circular_buffer(CircularBuffer *buffer) {
    if (buffer) {
        free(buffer->buffer);
        free(buffer);
    }
}

// Write message to circular buffer
BOOL write_message(CircularBuffer *buffer, Message msg, HANDLE mutex, HANDLE buffer_not_full, HANDLE buffer_not_empty) {
    BOOL result = FALSE;
    
    // Acquire mutex
    WaitForSingleObject(mutex, INFINITE);
    
    // Wait until buffer is not full
    while (buffer->count == buffer->size) {
        // Release mutex and wait for notification
        ReleaseMutex(mutex);
        WaitForSingleObject(buffer_not_full, INFINITE);
        // Reacquire mutex
        WaitForSingleObject(mutex, INFINITE);
    }
    
    // Write to buffer
    buffer->buffer[buffer->write_pos] = msg;
    buffer->write_pos = (buffer->write_pos + 1) % buffer->size;
    buffer->count++;
    
    result = TRUE;
    
    // Signal that buffer is not empty
    SetEvent(buffer_not_empty);
    
    // Release mutex
    ReleaseMutex(mutex);
    
    return result;
}

// Read message from circular buffer
BOOL read_message(CircularBuffer *buffer, Message *msg, HANDLE mutex, HANDLE buffer_not_empty, HANDLE buffer_not_full) {
    BOOL result = FALSE;
    
    // Acquire mutex
    WaitForSingleObject(mutex, INFINITE);
    
    // Wait until buffer is not empty
    while (buffer->count == 0) {
        // Release mutex and wait for notification
        ReleaseMutex(mutex);
        WaitForSingleObject(buffer_not_empty, INFINITE);
        // Reacquire mutex
        WaitForSingleObject(mutex, INFINITE);
    }
    
    // Read from buffer
    *msg = buffer->buffer[buffer->read_pos];
    buffer->read_pos = (buffer->read_pos + 1) % buffer->size;
    buffer->count--;
    
    result = TRUE;
    
    // Signal that buffer is not full
    SetEvent(buffer_not_full);
    
    // Release mutex
    ReleaseMutex(mutex);
    
    return result;
}

// Send message to audio thread
static void send_message(AppState *app, MessageType type) {
    Message msg;
    msg.type = type;
    
    write_message(app->message_buffer, msg, app->mutex, app->buffer_not_full, app->buffer_not_empty);
}

// Create and initialize surface for drawing
static void clear_surface(cairo_surface_t *surface) {
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);  // White background
    cairo_paint(cr);
    cairo_destroy(cr);
}

// Resize callback - recreate surface when window resizes
static void resize_cb(GtkWidget *widget, int width, int height, gpointer data) {
    AppState *app = (AppState*)data;
    
    if (app->surface) {
        cairo_surface_destroy(app->surface);
    }
    
    // Create new surface with new dimensions
    app->surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        width,
        height
    );
    
    // Clear the new surface
    clear_surface(app->surface);
}

// Draw function - render surface to drawing area
static void draw_function(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    AppState *app = (AppState*)data;
    
    if (app->surface) {
        // Copy surface to drawing area
        cairo_set_source_surface(cr, app->surface, 0, 0);
        cairo_paint(cr);
    }
}

// Mouse press callback
static void pressed_cb(GtkGestureClick *gesture, int n_press, double x, double y, AppState *app) {
    app->is_drawing = TRUE;
    app->last_x = x;
    app->last_y = y;
    
    // Start audio when drawing begins
    if (app->audio_initialized) {
        send_message(app, MSG_START_AUDIO);
    }
    
    // Draw initial point
    draw_brush_stroke(app, x, y);
}

// Mouse release callback
static void released_cb(GtkGestureClick *gesture, int n_press, double x, double y, AppState *app) {
    app->is_drawing = FALSE;
    
    // Stop audio when drawing ends
    if (app->audio_initialized) {
        send_message(app, MSG_STOP_AUDIO);
    }
}

// Mouse motion callback
static void motion_cb(GtkEventControllerMotion *controller, double x, double y, AppState *app) {
    if (app->is_drawing) {
        draw_brush_stroke(app, x, y);
    }
}

// Draw a brush stroke from last position to current position
static void draw_brush_stroke(AppState *app, double x, double y) {
    cairo_t *cr = cairo_create(app->surface);
    
    // Set line properties
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgb(cr, 0, 0, 0);  // Black brush
    
    // Draw line from last position to current position
    cairo_move_to(cr, app->last_x, app->last_y);
    cairo_line_to(cr, x, y);
    cairo_stroke(cr);
    
    cairo_destroy(cr);
    
    // Update last position
    app->last_x = x;
    app->last_y = y;
    
    // Request redraw of the drawing area
    gtk_widget_queue_draw(app->drawing_area);
}

// Load and decode a WAV file
static HRESULT load_wav_file(AppState *app) {
    FILE *file;
    WAVEFORMATEX wfx = {0};
    char chunk_id[4];
    DWORD chunk_size;
    DWORD sample_count;
    WORD format_tag;
    
    // Open the file
    file = fopen(app->audio_filename, "rb");
    if (!file) {
        printf("Failed to open WAV file: %s\n", app->audio_filename);
        return E_FAIL;
    }
    
    // Read RIFF header
    fread(chunk_id, 1, 4, file);
    if (memcmp(chunk_id, "RIFF", 4) != 0) {
        printf("Invalid WAV file format (RIFF header missing)\n");
        fclose(file);
        return E_FAIL;
    }
    
    // Skip file size and WAVE header
    fseek(file, 4, SEEK_CUR);
    fread(chunk_id, 1, 4, file);
    if (memcmp(chunk_id, "WAVE", 4) != 0) {
        printf("Invalid WAV file format (WAVE header missing)\n");
        fclose(file);
        return E_FAIL;
    }
    
    // Find and read format chunk
    while (1) {
        if (fread(chunk_id, 1, 4, file) != 4) {
            printf("Unexpected end of file while searching for fmt chunk\n");
            fclose(file);
            return E_FAIL;
        }
        
        if (fread(&chunk_size, 4, 1, file) != 1) {
            printf("Failed to read chunk size\n");
            fclose(file);
            return E_FAIL;
        }
        
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            // Read format tag first to determine if it's standard PCM or something else
            if (fread(&format_tag, 2, 1, file) != 1) {
                printf("Failed to read format tag\n");
                fclose(file);
                return E_FAIL;
            }
            
            // Format tag 1 = PCM, anything else requires special handling
            printf("Format tag: %d (1=PCM, 3=IEEE float, other=special)\n", format_tag);
            
            // Now read the rest of the format data
            if (fread(&wfx.nChannels, 2, 1, file) != 1 ||
                fread(&wfx.nSamplesPerSec, 4, 1, file) != 1 ||
                fread(&wfx.nAvgBytesPerSec, 4, 1, file) != 1 ||
                fread(&wfx.nBlockAlign, 2, 1, file) != 1 ||
                fread(&wfx.wBitsPerSample, 2, 1, file) != 1) {
                printf("Failed to read format data\n");
                fclose(file);
                return E_FAIL;
            }
            
            // Set the format tag we already read
            wfx.wFormatTag = format_tag;
            
            printf("Format details:\n");
            printf("  Format tag: %d\n", wfx.wFormatTag);
            printf("  Channels: %d\n", wfx.nChannels);
            printf("  Sample rate: %d Hz\n", wfx.nSamplesPerSec);
            printf("  Avg bytes/sec: %d\n", wfx.nAvgBytesPerSec);
            printf("  Block align: %d\n", wfx.nBlockAlign);
            printf("  Bits per sample: %d\n", wfx.wBitsPerSample);
            
            // Skip any extra format bytes
            if (chunk_size > 16) {
                printf("  Extended format info present (%d extra bytes)\n", chunk_size - 16);
                fseek(file, chunk_size - 16, SEEK_CUR);
            }
            
            break;
        } else {
            // Skip unknown chunk
            printf("Skipping unknown chunk: %.4s (%d bytes)\n", chunk_id, chunk_size);
            fseek(file, chunk_size, SEEK_CUR);
        }
        
        // Check if we've reached the end of the file
        if (feof(file)) {
            printf("Format chunk not found in WAV file\n");
            fclose(file);
            return E_FAIL;
        }
    }
    
    // Find and read data chunk
    while (1) {
        if (fread(chunk_id, 1, 4, file) != 4) {
            printf("Unexpected end of file while searching for data chunk\n");
            fclose(file);
            return E_FAIL;
        }
        
        if (fread(&chunk_size, 4, 1, file) != 1) {
            printf("Failed to read chunk size\n");
            fclose(file);
            return E_FAIL;
        }
        
        if (memcmp(chunk_id, "data", 4) == 0) {
            printf("Found data chunk: %d bytes\n", chunk_size);
            
            // Allocate memory for the audio data
            app->audio_data = malloc(chunk_size);
            if (!app->audio_data) {
                printf("Failed to allocate memory for audio data\n");
                fclose(file);
                return E_OUTOFMEMORY;
            }
            
            app->audio_data_size = chunk_size;
            
            // Read the audio data
            size_t bytes_read = fread(app->audio_data, 1, chunk_size, file);
            if (bytes_read != chunk_size) {
                printf("Warning: Expected to read %d bytes, but read %d bytes\n", 
                       chunk_size, (int)bytes_read);
            }
            
            // Calculate number of samples
            sample_count = chunk_size / (wfx.wBitsPerSample / 8 * wfx.nChannels);
            app->audio_sample_count = sample_count;
            
            break;
        } else {
            // Skip unknown chunk
            printf("Skipping unknown chunk: %.4s (%d bytes)\n", chunk_id, chunk_size);
            fseek(file, chunk_size, SEEK_CUR);
        }
        
        // Check if we've reached the end of the file
        if (feof(file)) {
            printf("Data chunk not found in WAV file\n");
            fclose(file);
            return E_FAIL;
        }
    }
    
    // Close the file
    fclose(file);
    
    // Copy format information
    app->wave_format = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!app->wave_format) {
        printf("Failed to allocate memory for wave format\n");
        return E_OUTOFMEMORY;
    }
    
    memcpy(app->wave_format, &wfx, sizeof(WAVEFORMATEX));
    
    printf("WAV file loaded: %s\n", app->audio_filename);
    printf("Channels: %d\n", wfx.nChannels);
    printf("Sample rate: %d Hz\n", wfx.nSamplesPerSec);
    printf("Bits per sample: %d\n", wfx.wBitsPerSample);
    printf("Audio duration: %.2f seconds\n", 
           (float)sample_count / wfx.nSamplesPerSec);
    
    return S_OK;
}

// Initialize WASAPI audio 
static HRESULT initialize_wasapi(AppState *app) {
    HRESULT hr;
    IMMDeviceEnumerator *device_enumerator = NULL;
    IMMDevice *device = NULL;
    REFERENCE_TIME buffer_duration;
    WAVEFORMATEX *device_format = NULL;
    
    // Initialize COM
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("Failed to initialize COM: %lx\n", hr);
        return hr;
    }
    
    // Create the device enumerator
    hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &IID_IMMDeviceEnumerator, (void**)&device_enumerator);
    if (FAILED(hr)) {
        printf("Failed to create device enumerator: %lx\n", hr);
        goto cleanup;
    }
    
    // Get the default audio endpoint
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(device_enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) {
        printf("Failed to get default audio endpoint: %lx\n", hr);
        goto cleanup;
    }
    
    // Create the audio client
    hr = IMMDevice_Activate(device,
        &IID_IAudioClient, CLSCTX_ALL,
        NULL, (void**)&app->audio_client);
    if (FAILED(hr)) {
        printf("Failed to activate audio client: %lx\n", hr);
        goto cleanup;
    }
    
    // Get the device mix format as a fallback
    hr = IAudioClient_GetMixFormat(app->audio_client, &device_format);
    if (FAILED(hr)) {
        printf("Failed to get device mix format: %lx\n", hr);
        goto cleanup;
    }
    
    printf("Device format:\n");
    printf("  Format tag: %d\n", device_format->wFormatTag);
    printf("  Channels: %d\n", device_format->nChannels);
    printf("  Sample rate: %d Hz\n", device_format->nSamplesPerSec);
    printf("  Bits per sample: %d\n", device_format->wBitsPerSample);
    
    // Calculate buffer duration
    buffer_duration = (REFERENCE_TIME)(REFTIMES_PER_SEC * (UINT64)BUFFER_DURATION / 1000);
    
    // Try initializing with the WAV format first
    printf("Trying to initialize audio client with WAV format...\n");
    hr = IAudioClient_Initialize(app->audio_client,
        AUDCLNT_SHAREMODE_SHARED, 0, // no event callbacks
        buffer_duration, 0, app->wave_format, NULL);
    
    if (FAILED(hr)) {
        printf("Failed to initialize with WAV format: %lx\n", hr);
        
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // Try initializing with the device's mix format instead
            printf("Trying device's mix format instead...\n");
            
            // Replace our wave format with the device format
            if (app->wave_format) {
                CoTaskMemFree(app->wave_format);
            }
            app->wave_format = device_format;
            device_format = NULL; // Prevent double-free
            
            // Try initialization again
            hr = IAudioClient_Initialize(app->audio_client,
                AUDCLNT_SHAREMODE_SHARED, 0,
                buffer_duration, 0, app->wave_format, NULL);
                
            if (FAILED(hr)) {
                printf("Failed to initialize with device format too: %lx\n", hr);
                printf("Cannot initialize audio at all\n");
                goto cleanup;
            } else {
                printf("Successfully initialized with device format!\n");
                printf("Note: Audio playback may not match the WAV file exactly\n");
            }
        } else {
            printf("Unexpected error initializing audio client\n");
            goto cleanup;
        }
    } else {
        printf("Successfully initialized with WAV format\n");
    }
    
    // Get the render client
    hr = IAudioClient_GetService(app->audio_client,
        &IID_IAudioRenderClient, (void**)&app->render_client);
    if (FAILED(hr)) {
        printf("Failed to get render client: %lx\n", hr);
        goto cleanup;
    }
    
cleanup:
    if (device) IMMDevice_Release(device);
    if (device_enumerator) IMMDeviceEnumerator_Release(device_enumerator);
    if (device_format && device_format != app->wave_format) CoTaskMemFree(device_format);
    
    return hr;
}

// Initialize WinMM audio (simpler fallback)
static HRESULT initialize_winmm(AppState *app) {
    MMRESULT result;
    WAVEFORMATEX wfx = {0};
    
    // Set up the wave format - try to match the WAV file
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = app->wave_format->nChannels;
    wfx.nSamplesPerSec = app->wave_format->nSamplesPerSec;
    wfx.wBitsPerSample = app->wave_format->wBitsPerSample;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    
    // Open the wave output device
    result = waveOutOpen(&app->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        printf("Failed to open wave output device: %d\n", result);
        return E_FAIL;
    }
    
    // Set up the wave header (but don't prepare yet - we'll do that each time we play)
    app->wh.lpData = app->audio_data;
    app->wh.dwBufferLength = app->audio_data_size;
    app->wh.dwFlags = 0;
    app->wh.dwLoops = 0;
    
    printf("WinMM audio initialized successfully\n");
    return S_OK;
}

// Start audio playback
static void start_audio(AppState *app) {
    if (!app->audio_initialized || app->audio_playing) {
        return;
    }
    
    printf("Starting audio playback\n");
    app->audio_playing = TRUE;
    
    if (app->audio_api == AUDIO_API_WASAPI) {
        // Start WASAPI playback
        if (app->audio_client) {
            HRESULT hr = IAudioClient_Start(app->audio_client);
            if (FAILED(hr)) {
                printf("Failed to start WASAPI audio: %lx\n", hr);
                app->audio_playing = FALSE;
            }
        }
    } else if (app->audio_api == AUDIO_API_WINMM) {
        // For WinMM, we need to always prepare a fresh header
        MMRESULT result;
        
        // First, make sure any previous playing has stopped
        waveOutReset(app->hwo);
        
        // Check if header was previously prepared and unprepare if needed
        if (app->wh.dwFlags & WHDR_PREPARED) {
            result = waveOutUnprepareHeader(app->hwo, &app->wh, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                printf("Warning: Failed to unprepare header: %d\n", result);
            }
        }
        
        // Reset header flags and set looping
        app->wh.dwFlags = 0;
        app->wh.dwLoops = 100;  // Loop many times but not indefinitely
        
        // Prepare header fresh for this playback
        result = waveOutPrepareHeader(app->hwo, &app->wh, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            printf("Failed to prepare wave header: %d\n", result);
            app->audio_playing = FALSE;
            return;
        }
        
        // Set looping flags after preparation
        app->wh.dwFlags |= WHDR_BEGINLOOP | WHDR_ENDLOOP;
        
        // Start playback
        result = waveOutWrite(app->hwo, &app->wh, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            printf("Failed to write WinMM audio buffer: %d\n", result);
            app->audio_playing = FALSE;
            
            // Detailed error message
            switch (result) {
                case WAVERR_BADFORMAT:
                    printf("Error: Unsupported wave format\n");
                    break;
                case WAVERR_STILLPLAYING:
                    printf("Error: Buffer is still playing\n");
                    break;
                case WAVERR_UNPREPARED:
                    printf("Error: Header not prepared\n");
                    break;
                case MMSYSERR_INVALHANDLE:
                    printf("Error: Invalid device handle\n");
                    break;
                default:
                    printf("Error: Unknown WinMM error\n");
            }
        } else {
            printf("WinMM audio playback started successfully\n");
        }
    }
}

// Stop audio playback
static void stop_audio(AppState *app) {
    if (!app->audio_initialized || !app->audio_playing) {
        return;
    }
    
    printf("Stopping audio playback\n");
    app->audio_playing = FALSE;
    
    if (app->audio_api == AUDIO_API_WASAPI) {
        // Stop WASAPI playback
        if (app->audio_client) {
            HRESULT hr = IAudioClient_Stop(app->audio_client);
            if (FAILED(hr)) {
                printf("Failed to stop WASAPI audio: %lx\n", hr);
            }
        }
    } else if (app->audio_api == AUDIO_API_WINMM) {
        // Stop WinMM playback
        MMRESULT result = waveOutReset(app->hwo);
        if (result != MMSYSERR_NOERROR) {
            printf("Failed to reset WinMM audio: %d\n", result);
        }
        
        // Unprepare header when done playing
        if (app->wh.dwFlags & WHDR_PREPARED) {
            result = waveOutUnprepareHeader(app->hwo, &app->wh, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                printf("Failed to unprepare header: %d\n", result);
            }
        }
    }
}

// Audio thread function
static DWORD WINAPI audio_thread_func(LPVOID lpParam) {
    AppState *app = (AppState*)lpParam;
    BOOL running = TRUE;
    HRESULT hr;
    Message msg;
    
    // Load WAV file
    hr = load_wav_file(app);
    if (FAILED(hr)) {
        printf("Failed to load WAV file: %lx\n", hr);
        app->audio_initialized = FALSE;
        return 1;
    }
    
    // First try WinMM
    printf("Trying WinMM audio initialization...\n");
    hr = initialize_winmm(app);
    if (SUCCEEDED(hr)) {
        app->audio_api = AUDIO_API_WINMM;
        app->audio_initialized = TRUE;
        printf("WinMM audio initialized successfully!\n");
    } else {
        // If WinMM fails, try WASAPI
        printf("WinMM initialization failed, trying WASAPI as fallback...\n");
        hr = initialize_wasapi(app);
        if (SUCCEEDED(hr)) {
            app->audio_api = AUDIO_API_WASAPI;
            app->audio_initialized = TRUE;
            printf("WASAPI audio initialized successfully!\n");
        } else {
            printf("All audio initialization methods failed\n");
            app->audio_api = AUDIO_API_NONE;
            app->audio_initialized = FALSE;
            return 1;
        }
    }
    
    // If we're using WASAPI, we need to handle filling the buffer
    UINT32 buffer_frames = 0;
    UINT32 padding_frames = 0;
    BYTE *buffer_data = NULL;
    UINT32 bytes_per_frame = 0;
    UINT32 frames_to_write = 0;
    UINT32 current_position = 0;
    
    if (app->audio_api == AUDIO_API_WASAPI) {
        // Get buffer size
        hr = IAudioClient_GetBufferSize(app->audio_client, &buffer_frames);
        if (FAILED(hr)) {
            printf("Failed to get buffer size: %lx\n", hr);
            return 1;
        }
        
        // Calculate bytes per frame
        bytes_per_frame = app->wave_format->nBlockAlign;
        
        printf("WASAPI audio ready with buffer size: %u frames\n", buffer_frames);
    }
    
    // Audio processing loop
    while (running) {
        // Check for exit flag
        if (app->thread_should_exit) {
            running = FALSE;
            break;
        }
        
        // Check for messages
        if (read_message(app->message_buffer, &msg, app->mutex, app->buffer_not_empty, app->buffer_not_full)) {
            switch (msg.type) {
                case MSG_START_AUDIO:
                    start_audio(app);
                    break;
                    
                case MSG_STOP_AUDIO:
                    stop_audio(app);
                    break;
                    
                case MSG_EXIT:
                    running = FALSE;
                    break;
            }
        }
        
        // If using WASAPI and audio is playing, feed data to the audio buffer
        if (app->audio_api == AUDIO_API_WASAPI && app->audio_playing) {
            // Get current padding (filled frames)
            hr = IAudioClient_GetCurrentPadding(app->audio_client, &padding_frames);
            if (FAILED(hr)) {
                printf("Failed to get current padding: %lx\n", hr);
                break;
            }
            
            // Calculate frames to write
            frames_to_write = buffer_frames - padding_frames;
            if (frames_to_write == 0) {
                // Buffer is full, wait a bit
                Sleep(10);
                continue;
            }
            
            // Get buffer to write to
            hr = IAudioRenderClient_GetBuffer(app->render_client, frames_to_write, &buffer_data);
            if (FAILED(hr)) {
                printf("Failed to get buffer: %lx\n", hr);
                break;
            }
            
            // Calculate how many frames we can actually write from our data
            UINT32 frames_available = (app->audio_data_size - current_position) / bytes_per_frame;
            UINT32 frames_to_copy = frames_to_write;
            
            if (frames_available < frames_to_copy) {
                // We've reached the end of the audio data, loop back to beginning
                frames_to_copy = frames_available;
                
                // Copy what we have
                if (frames_to_copy > 0) {
                    memcpy(buffer_data, 
                           (BYTE*)app->audio_data + current_position, 
                           frames_to_copy * bytes_per_frame);
                }
                
                // Fill the rest of the buffer with beginning of audio file
                if (frames_to_copy < frames_to_write) {
                    memcpy(buffer_data + frames_to_copy * bytes_per_frame,
                           app->audio_data,
                           (frames_to_write - frames_to_copy) * bytes_per_frame);
                }
                
                // Reset position for next time
                current_position = (frames_to_write - frames_to_copy) * bytes_per_frame;
            } else {
                // Copy requested frames
                memcpy(buffer_data, 
                       (BYTE*)app->audio_data + current_position, 
                       frames_to_write * bytes_per_frame);
                
                // Update position
                current_position += frames_to_write * bytes_per_frame;
            }
            
            // Release the buffer
            hr = IAudioRenderClient_ReleaseBuffer(app->render_client, frames_to_write, 0);
            if (FAILED(hr)) {
                printf("Failed to release buffer: %lx\n", hr);
                break;
            }
        } else {
            // Not playing or not using WASAPI, just wait a bit
            Sleep(10);
        }
    }
    
    // Clean up audio before exiting
    if (app->audio_api == AUDIO_API_WASAPI) {
        if (app->audio_client) {
            IAudioClient_Stop(app->audio_client);
        }
    } else if (app->audio_api == AUDIO_API_WINMM) {
        if (app->audio_playing) {
            waveOutReset(app->hwo);
        }
        
        // Unprepare header if it was prepared
        if (app->wh.dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(app->hwo, &app->wh, sizeof(WAVEHDR));
        }
        
        waveOutClose(app->hwo);
    }
    
    return 0;
}

// Cleanup application resources
static void cleanup_app(AppState *app) {
    printf("Cleaning up resources...\n");
    
    // Send exit message to audio thread
    if (app->audio_thread) {
        app->thread_should_exit = TRUE;
        send_message(app, MSG_EXIT);
        
        // Wait for thread to exit with timeout
        DWORD wait_result = WaitForSingleObject(app->audio_thread, 3000);  // 3 second timeout
        if (wait_result == WAIT_TIMEOUT) {
            printf("Warning: Audio thread did not exit in time. Forcing termination...\n");
            TerminateThread(app->audio_thread, 1);
        }
        CloseHandle(app->audio_thread);
    }
    
    // Clean up circular buffer
    destroy_circular_buffer(app->message_buffer);
    
    // Clean up synchronization objects
    if (app->mutex) CloseHandle(app->mutex);
    if (app->buffer_not_empty) CloseHandle(app->buffer_not_empty);
    if (app->buffer_not_full) CloseHandle(app->buffer_not_full);
    
    // Clean up audio resources based on which API was used
    if (app->audio_api == AUDIO_API_WASAPI) {
        if (app->render_client) IAudioRenderClient_Release(app->render_client);
        if (app->audio_client) IAudioClient_Release(app->audio_client);
        if (app->wave_format) CoTaskMemFree(app->wave_format);
    } else if (app->audio_api == AUDIO_API_WINMM) {
        // WinMM cleanup should have been handled in the audio thread
    }
    
    // Free audio data
    if (app->audio_data) free(app->audio_data);
    
    // Clean up drawing surface
    if (app->surface) cairo_surface_destroy(app->surface);
    
    // Uninitialize COM
    CoUninitialize();
    
    // Free app structure
    free(app);
    
    printf("Cleanup complete\n");
}

// Activate callback for GTK application
static void activate(GtkApplication *gtk_app, gpointer user_data) {
    AppState *app = (AppState*)user_data;
    GtkWidget *window;
    GtkWidget *drawing_area;
    GtkGesture *press_gesture;
    GtkEventController *motion_controller;
    
    // Create main window
    window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), "A9: Drawing Canvas with Audio");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    app->window = window;
    
    // Create drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    app->drawing_area = drawing_area;
    
    // Set drawing function
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), 
                                   draw_function, app, NULL);
    
    // Connect resize signal
    g_signal_connect(drawing_area, "resize", G_CALLBACK(resize_cb), app);
    
    // Set up mouse handling
    press_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press_gesture), GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(press_gesture));
    
    g_signal_connect(press_gesture, "pressed", G_CALLBACK(pressed_cb), app);
    g_signal_connect(press_gesture, "released", G_CALLBACK(released_cb), app);
    
    motion_controller = gtk_event_controller_motion_new();
    gtk_widget_add_controller(drawing_area, motion_controller);
    g_signal_connect(motion_controller, "motion", G_CALLBACK(motion_cb), app);
    
    // Add drawing area to window
    gtk_window_set_child(GTK_WINDOW(window), drawing_area);
    
    // Create initial surface
    app->surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        800, 600  // Initial size
    );
    clear_surface(app->surface);
    
    // Show window
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char *argv[]) {
    AppState *app;
    int status;
    DWORD thread_id;
    
    // Check for audio filename
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <audio_file.wav>\n", argv[0]);
        return 1;
    }
    
    // Create modified argc/argv for GTK (to avoid GLib-GIO errors)
    char* new_argv[1];
    new_argv[0] = argv[0];
    int new_argc = 1;
    
    // Allocate and initialize app state
    app = malloc(sizeof(AppState));
    if (!app) {
        fprintf(stderr, "Failed to allocate memory for app state\n");
        return 1;
    }
    
    // Initialize app state
    app->app = gtk_application_new("org.example.A9", G_APPLICATION_DEFAULT_FLAGS);
    app->window = NULL;
    app->drawing_area = NULL;
    app->surface = NULL;
    app->is_drawing = FALSE;
    app->last_x = 0;
    app->last_y = 0;
    app->audio_thread = NULL;
    app->audio_playing = FALSE;
    app->audio_filename = argv[1];
    app->audio_initialized = FALSE;
    app->thread_should_exit = FALSE;
    app->audio_api = AUDIO_API_NONE;
    app->audio_client = NULL;
    app->render_client = NULL;
    app->wave_format = NULL;
    app->audio_data = NULL;
    app->audio_data_size = 0;
    app->audio_sample_count = 0;
    
    // Create circular buffer for thread communication
    app->message_buffer = create_circular_buffer(BUFFER_SIZE);
    if (!app->message_buffer) {
        fprintf(stderr, "Failed to create circular buffer\n");
        free(app);
        return 1;
    }
    
    // Create synchronization objects
    app->mutex = CreateMutex(NULL, FALSE, NULL);
    app->buffer_not_empty = CreateEvent(NULL, FALSE, FALSE, NULL);
    app->buffer_not_full = CreateEvent(NULL, FALSE, TRUE, NULL);
    
    if (!app->mutex || !app->buffer_not_empty || !app->buffer_not_full) {
        fprintf(stderr, "Failed to create synchronization objects\n");
        destroy_circular_buffer(app->message_buffer);
        if (app->mutex) CloseHandle(app->mutex);
        if (app->buffer_not_empty) CloseHandle(app->buffer_not_empty);
        if (app->buffer_not_full) CloseHandle(app->buffer_not_full);
        free(app);
        return 1;
    }
    
    // Create and start audio thread
    app->audio_thread = CreateThread(NULL, 0, audio_thread_func, app, 0, &thread_id);
    if (!app->audio_thread) {
        fprintf(stderr, "Failed to create audio thread\n");
    } else {
        // Wait a short time for audio initialization
        Sleep(500);  // Give audio thread time to initialize
        
        if (app->audio_initialized) {
            printf("Audio initialized successfully with API: %s\n", 
                   app->audio_api == AUDIO_API_WASAPI ? "WASAPI" : 
                   app->audio_api == AUDIO_API_WINMM ? "WinMM" : "Unknown");
        } else {
            printf("Warning: Audio failed to initialize, drawing will work without sound\n");
        }
    }
    
    // Connect signals for the application
    g_signal_connect(app->app, "activate", G_CALLBACK(activate), app);
    
    // Run the application with modified argc/argv
    status = g_application_run(G_APPLICATION(app->app), new_argc, new_argv);
    
    // Clean up
    g_object_unref(app->app);
    cleanup_app(app);
    
    return status;
}