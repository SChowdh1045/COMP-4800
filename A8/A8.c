/*
    Compile: gcc -o A8 A8.c -lole32 -loleaut32 -luuid -lm
    Run: ./A8
*/


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <conio.h> // For _kbhit() and _getch()
#include <time.h>
#include <windows.h>

// These must come before COM headers
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
#include <initguid.h>

// MinGW WASAPI COM support
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

// Constants for audio generation
#define CHANNELS 2
#define BITS_PER_SAMPLE 16
#define REFTIMES_PER_SEC 10000000  // 1 second in 100-nanosecond units
#define BUFFER_DURATION 500  // 500 ms buffer
#define M_PI 3.14159265358979323846
#define COBJMACROS


// Waveform types
typedef enum {
    SINE_WAVE,
    NOISE
} WaveformType;

// Global variables for audio playback
IAudioClient* pAudioClient = NULL;
IAudioRenderClient* pRenderClient = NULL;
HANDLE hEvent = NULL;
HANDLE hThread = NULL;
BOOL isPlaying = FALSE;
BOOL isFloatFormat = FALSE;
WaveformType currentWaveform = SINE_WAVE;
float frequency = 440.0f;  // A4 note
float amplitude = 0.8f;  // 80% volume
int deviceSampleRate = 44100;  // Will store actual device rate
int bitsPerSample = 16;


// Generates audio sample
float generateSample(int sampleIndex, WaveformType waveform, float freq, float amp) {
    float t = (float)sampleIndex / deviceSampleRate;  // Uses actual device rate
    float value = 0.0f;
    
    switch (waveform) {
        case SINE_WAVE:
            // Simple sine wave: amp * sin(2π * freq * t)
            value = amp * sinf(2.0f * M_PI * freq * t);
            break;
            
        case NOISE:
            // White noise: amp * random between -1 and 1
            value = amp * (((float)rand() / RAND_MAX) * 2.0f - 1.0f);
            break;
    }
    
    return value;
}


// Converts float audio sample (-1.0 to 1.0) to 16-bit PCM
SHORT floatToInt16(float sample) {
    // Ensure sample is in range [-1.0, 1.0]
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    
    // Scale to 16-bit range and convert to SHORT
    return (SHORT)(sample * 32767.0f);
}

// Thread function to fill audio buffer and play audio
DWORD WINAPI AudioThread(LPVOID lpParam) {
    HRESULT hr;
    UINT32 bufferFrameCount;
    UINT32 numFramesPadding;
    UINT32 numFramesAvailable;
    BYTE* pData;
    DWORD flags = 0;
    UINT64 sampleIndex = 0;
    UINT32 i, channel;
    
    // Get the buffer size
    hr = IAudioClient_GetBufferSize(pAudioClient, &bufferFrameCount);
    if (FAILED(hr)) {
        printf("Failed to get buffer size: %lx\n", hr);
        return 1;
    }
    
    // Start the audio client
    hr = IAudioClient_Start(pAudioClient);
    if (FAILED(hr)) {
        printf("Failed to start audio client: %lx\n", hr);
        return 1;
    }
    
    printf("Playing %s at %.1f Hz (press any key to stop)\n", 
        (currentWaveform == SINE_WAVE ? "sine wave" : "noise"),
        frequency);
    
    // Main audio loop
    while (isPlaying) {
        // Wait for buffer to be ready
        WaitForSingleObject(hEvent, INFINITE);
        
        // See how much buffer space is available
        hr = IAudioClient_GetCurrentPadding(pAudioClient, &numFramesPadding);
        if (FAILED(hr)) {
            printf("Failed to get current padding: %lx\n", hr);
            break;
        }
        
        numFramesAvailable = bufferFrameCount - numFramesPadding;
        
        // Grab the available space in the buffer
        hr = IAudioRenderClient_GetBuffer(pRenderClient, numFramesAvailable, &pData);
        if (FAILED(hr)) {
            printf("Failed to get buffer: %lx\n", hr);
            break;
        }

        // Add this debug info
        if (sampleIndex == 0) {
            printf("Debug: Writing %d frames with format: isFloat=%d, bits=%d\n", 
                numFramesAvailable, isFloatFormat, bitsPerSample);
        }
        
        // Fill the buffer with audio data
        for (i = 0; i < numFramesAvailable; i++) {
            // Generate sample value
            float sample = generateSample(sampleIndex++, currentWaveform, frequency, amplitude);
            
            // Write sample to both channels (stereo)
            for (channel = 0; channel < CHANNELS; channel++) {
                // Calculate byte position in buffer
                UINT32 position = (i * CHANNELS + channel) * (bitsPerSample / 8);
                
                if (isFloatFormat) {
                    // For float format, use the sample value directly
                    memcpy(pData + position, &sample, sizeof(float));
                } else if (bitsPerSample == 32) {
                    // 32-bit PCM
                    INT32 sampleInt = (INT32)(sample * 2147483647.0f);
                    memcpy(pData + position, &sampleInt, sizeof(INT32));
                } else if (bitsPerSample == 16) {
                    // 16-bit PCM
                    INT16 sampleInt = (INT16)(sample * 32767.0f);
                    memcpy(pData + position, &sampleInt, sizeof(INT16));
                } else if (bitsPerSample == 8) {
                    // 8-bit PCM (unsigned)
                    BYTE sampleByte = (BYTE)((sample + 1.0f) * 127.5f);
                    pData[position] = sampleByte;
                }
            }
        }
        
        // Release the buffer
        hr = IAudioRenderClient_ReleaseBuffer(pRenderClient, numFramesAvailable, flags);
        if (FAILED(hr)) {
            printf("Failed to release buffer: %lx\n", hr);
            break;
        }
    }
    
    // Stop and reset the audio client
    IAudioClient_Stop(pAudioClient);
    IAudioClient_Reset(pAudioClient);
    
    return 0;
}

// Initialize the audio client and get device properties
HRESULT InitializeAudioClient() {
    HRESULT hr;
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    WAVEFORMATEX* pwfx = NULL;
    IAudioEndpointVolume* pEndpointVolume = NULL;
    IPropertyStore* pProps = NULL;
    LPWSTR deviceId = NULL;
    float currentVolume = 0.0f;
    float leftVolume = 0.0f;
    float rightVolume = 0.0f;
    UINT channelCount;
    PROPVARIANT varName;
    char buffer[256];
    size_t convertedChars = 0;
    REFERENCE_TIME hnsBufferDuration;
    
    // Initialize COM
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("Failed to initialize COM: %lx\n", hr);
        return hr;
    }
    
    // Create the device enumerator
    hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) {
        printf("Failed to create device enumerator: %lx\n", hr);
        goto cleanup;
    }
    
    // Get the default audio endpoint
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        printf("Failed to get default audio endpoint: %lx\n", hr);
        goto cleanup;
    }
    
    // ==== Display device properties ====
    
    // 1. Get device ID
    hr = IMMDevice_GetId(pDevice, &deviceId);
    if (SUCCEEDED(hr)) {
        // Convert LPWSTR to char* for printing
        wcstombs_s(&convertedChars, buffer, sizeof(buffer), deviceId, _TRUNCATE);
        printf("Device ID: %s\n", buffer);
        CoTaskMemFree(deviceId);
    } else {
        printf("Failed to get device ID: %lx\n", hr);
    }
    
    // 2. Get device volume (left, right, or both)
    hr = IMMDevice_Activate(pDevice,
        &IID_IAudioEndpointVolume, CLSCTX_ALL,
        NULL, (void**)&pEndpointVolume);
    if (SUCCEEDED(hr)) {
        hr = IAudioEndpointVolume_GetMasterVolumeLevelScalar(pEndpointVolume, &currentVolume);
        if (SUCCEEDED(hr)) {
            printf("Default Volume: %.1f%%\n", currentVolume * 100.0f);
        } else {
            printf("Failed to get master volume: %lx\n", hr);
        }
        
        // Check if the device has channel volume controls
        hr = IAudioEndpointVolume_GetChannelCount(pEndpointVolume, &channelCount);
        if (SUCCEEDED(hr) && channelCount >= 2) {
            hr = IAudioEndpointVolume_GetChannelVolumeLevelScalar(pEndpointVolume, 0, &leftVolume);
            hr = IAudioEndpointVolume_GetChannelVolumeLevelScalar(pEndpointVolume, 1, &rightVolume);
            
            printf("Left Channel Volume: %.1f%%\n", leftVolume * 100.0f);
            printf("Right Channel Volume: %.1f%%\n", rightVolume * 100.0f);
        }
        
        IAudioEndpointVolume_Release(pEndpointVolume);
    }
    
    // Create the audio client
    hr = IMMDevice_Activate(pDevice,
        &IID_IAudioClient, CLSCTX_ALL,
        NULL, (void**)&pAudioClient);
    if (FAILED(hr)) {
        printf("Failed to activate audio client: %lx\n", hr);
        goto cleanup;
    }
    
    // 3. Get mix format (sample rate)
    hr = IAudioClient_GetMixFormat(pAudioClient, &pwfx);
    if (FAILED(hr)) {
        printf("Failed to get mix format: %lx\n", hr);
        goto cleanup;
    }
    
    printf("Sample Rate: %u Hz\n", pwfx->nSamplesPerSec);
    printf("Channel Count: %u\n", pwfx->nChannels);
    printf("Bits Per Sample: %u\n", pwfx->wBitsPerSample);
    
    // Get extra properties using IPropertyStore
    hr = IMMDevice_OpenPropertyStore(pDevice, STGM_READ, &pProps);
    if (SUCCEEDED(hr)) {
        // Get friendly name
        PropVariantInit(&varName);
        hr = IPropertyStore_GetValue(pProps, (PROPERTYKEY*)&PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            wcstombs_s(&convertedChars, buffer, sizeof(buffer), varName.pwszVal, _TRUNCATE);
            printf("Device Name: %s\n", buffer);
        }
        PropVariantClear(&varName);
        IPropertyStore_Release(pProps);
    }
    
    // Create an event for buffer notifications
    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hEvent == NULL) {
        printf("Failed to create event\n");
        hr = E_FAIL;
        goto cleanup;
    }
    
    // Examine the format
    // Handle WAVE_FORMAT_EXTENSIBLE format correctly
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // WAVE_FORMAT_EXTENSIBLE requires looking at the subformat
        WAVEFORMATEXTENSIBLE* pwfxExt = (WAVEFORMATEXTENSIBLE*)pwfx;
        
        // Print GUID for debugging
        printf("Subformat GUID: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
            pwfxExt->SubFormat.Data1, pwfxExt->SubFormat.Data2, pwfxExt->SubFormat.Data3,
            pwfxExt->SubFormat.Data4[0], pwfxExt->SubFormat.Data4[1],
            pwfxExt->SubFormat.Data4[2], pwfxExt->SubFormat.Data4[3],
            pwfxExt->SubFormat.Data4[4], pwfxExt->SubFormat.Data4[5],
            pwfxExt->SubFormat.Data4[6], pwfxExt->SubFormat.Data4[7]);
        
        // Check if it's PCM or float
        // KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00aa00389b71}
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
        if (pwfxExt->SubFormat.Data1 == 1) {
            isFloatFormat = FALSE;
            printf("Using PCM format (via WAVE_FORMAT_EXTENSIBLE)\n");
        } else if (pwfxExt->SubFormat.Data1 == 3) {
            isFloatFormat = TRUE;
            printf("Using float format (via WAVE_FORMAT_EXTENSIBLE)\n");
        } else {
            printf("WARNING: Unknown subformat, assuming PCM\n");
            isFloatFormat = FALSE;
        }
    } else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || pwfx->wFormatTag == 3) {
        isFloatFormat = TRUE;
        printf("Using float format\n");
    } else {
        isFloatFormat = FALSE;
        printf("Using integer format\n");
    }
    bitsPerSample = pwfx->wBitsPerSample;

    deviceSampleRate = pwfx->nSamplesPerSec;
    
    // Initialize the audio client - fix for integer overflow
    hnsBufferDuration = (REFERENCE_TIME)REFTIMES_PER_SEC * BUFFER_DURATION / 1000;
    hr = IAudioClient_Initialize(pAudioClient,
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration, 0, pwfx, NULL);
    if (FAILED(hr)) {
        printf("Failed to initialize audio client: %lx\n", hr);
        goto cleanup;
    }
    
    // Set the event handle
    hr = IAudioClient_SetEventHandle(pAudioClient, hEvent);
    if (FAILED(hr)) {
        printf("Failed to set event handle: %lx\n", hr);
        goto cleanup;
    }
    
    // Get the render client
    hr = IAudioClient_GetService(pAudioClient,
        &IID_IAudioRenderClient, (void**)&pRenderClient);
    if (FAILED(hr)) {
        printf("Failed to get render client: %lx\n", hr);
        goto cleanup;
    }
    
cleanup:
    CoTaskMemFree(pwfx);
    if (pDevice) IMMDevice_Release(pDevice);
    if (pEnumerator) IMMDeviceEnumerator_Release(pEnumerator);
    
    return hr;
}

// Clean up audio resources
void CleanupAudio() {
    if (isPlaying) {
        isPlaying = FALSE;
        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
            hThread = NULL;
        }
    }
    
    if (hEvent) {
        CloseHandle(hEvent);
        hEvent = NULL;
    }
    
    if (pRenderClient) {
        IAudioRenderClient_Release(pRenderClient);
        pRenderClient = NULL;
    }
    
    if (pAudioClient) {
        IAudioClient_Release(pAudioClient);
        pAudioClient = NULL;
    }
    
    CoUninitialize();
}

// Simple menu system for the console application
void showMenu() {
    printf("\n==== Audio Demo Menu ====\n");
    printf("1: Play Sine Wave\n");
    printf("2: Play White Noise\n");
    printf("3: Change Frequency (currently %.1f Hz)\n", frequency);
    printf("4: Change Amplitude (currently %.2f)\n", amplitude);
    printf("0: Quit\n");
    printf("Enter your choice: ");
}

int main() {
    int choice;
    BOOL running = TRUE;
    float newFreq, newAmp;
    DWORD threadId;
    UINT32 numFramesAvailable;
    
    // Initialize random number generator
    srand((unsigned int)time(NULL));
    
    // Initialize audio client and get device properties
    HRESULT hr = InitializeAudioClient();
    if (FAILED(hr)) {
        printf("Failed to initialize audio. Error code: %lx\n", hr);
        return 1;
    }
    
    printf("\nAudio initialization successful!\n");
    
    // Menu-driven console application
    while (running) {
        showMenu();
        
        scanf_s("%d", &choice);
        
        // Stop any playing audio before changing settings
        if (isPlaying) {
            isPlaying = FALSE;
            if (hThread) {
                WaitForSingleObject(hThread, INFINITE);
                CloseHandle(hThread);
                hThread = NULL;
            }
        }
        
        switch (choice) {
            case 0: // Quit
                running = FALSE;
                break;
                
            case 1: // Sine Wave
                currentWaveform = SINE_WAVE;
                isPlaying = TRUE;
                hThread = CreateThread(NULL, 0, AudioThread, NULL, 0, &threadId);
                break;
                
            case 2: // White Noise
                currentWaveform = NOISE;
                isPlaying = TRUE;
                hThread = CreateThread(NULL, 0, AudioThread, NULL, 0, &threadId);
                break;
                
            case 3: // Change Frequency
                printf("Enter new frequency (20-20000 Hz): ");
                scanf_s("%f", &newFreq);
                if (newFreq < 20) newFreq = 20;
                if (newFreq > 20000) newFreq = 20000;
                frequency = newFreq;
                break;
                
            case 4: // Change Amplitude
                printf("Enter new amplitude (0.0-1.0): ");
                scanf_s("%f", &newAmp);
                if (newAmp < 0) newAmp = 0;
                if (newAmp > 1) newAmp = 1;
                amplitude = newAmp;
                break;
                
            default:
                printf("Invalid choice. Please try again.\n");
        }
        
        // If audio is playing, wait for a key press to stop it
        if (isPlaying) {
            printf("Press any key to stop...\n");
            _getch(); // Wait for a key press
            
            // Stop the audio
            isPlaying = FALSE;
            if (hThread) {
                WaitForSingleObject(hThread, INFINITE);
                CloseHandle(hThread);
                hThread = NULL;
            }
        }
    }
    
    // Clean up
    CleanupAudio();
    
    return 0;
}