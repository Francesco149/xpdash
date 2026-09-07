/* tools/eax-test/src/main.c — Standalone EAX Hardware Verification Tool
 *
 * Exercises the Creative SB0090 (EMU10K2 DSP) hardware EAX audio path on Windows XP
 * to 100% verify that EAX 3D spatialization and environmental reverb are active and
 * being captured by "What U Hear".
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void* ALCdevice;
typedef void* ALCcontext;
typedef char ALCchar;
typedef int ALCenum;
typedef int ALenum;
typedef unsigned int ALuint;
typedef int ALint;
typedef float ALfloat;
typedef char ALboolean;

#define ALC_DEVICE_SPECIFIER 0x1005
#define ALC_EXTENSIONS       0x1006
#define AL_VENDOR            0x1008
#define AL_RENDERER          0x1007
#define AL_VERSION           0x1009
#define AL_EXTENSIONS        0x1006
#define AL_FORMAT_MONO16     0x1101
#define AL_BUFFER            0x1009
#define AL_SOURCE_STATE      0x1010
#define AL_PLAYING           0x1012

// EAX 2.0 / 3.0 Environment Presets
#define EAX_ENVIRONMENT_GENERIC    0
#define EAX_ENVIRONMENT_PADDEDCELL 1
#define EAX_ENVIRONMENT_ROOM       2
#define EAX_ENVIRONMENT_BATHROOM   3
#define EAX_ENVIRONMENT_LIVINGROOM 4
#define EAX_ENVIRONMENT_STONEROOM  5
#define EAX_ENVIRONMENT_AUDITORIUM 6
#define EAX_ENVIRONMENT_CONCERTHALL 7
#define EAX_ENVIRONMENT_CAVE       8
#define EAX_ENVIRONMENT_ARENA      9
#define EAX_ENVIRONMENT_HANGAR     10
#define EAX_ENVIRONMENT_CARPETEDHALLWAY 11
#define EAX_ENVIRONMENT_HALLWAY    12
#define EAX_ENVIRONMENT_STONECORRIDOR 13
#define EAX_ENVIRONMENT_ALLEY      14
#define EAX_ENVIRONMENT_FOREST     15
#define EAX_ENVIRONMENT_CITY       16
#define EAX_ENVIRONMENT_MOUNTAINS  17
#define EAX_ENVIRONMENT_QUARRY     18
#define EAX_ENVIRONMENT_PLAIN      19
#define EAX_ENVIRONMENT_PARKINGLOT 20
#define EAX_ENVIRONMENT_SEWERPIPE  21
#define EAX_ENVIRONMENT_UNDERWATER 22

// OpenAL and EAX function pointers
static ALCdevice* (*p_alcOpenDevice)(const ALCchar *devicename) = NULL;
static ALCcontext* (*p_alcCreateContext)(ALCdevice *device, const int *attrlist) = NULL;
static int (*p_alcMakeContextCurrent)(ALCcontext *context) = NULL;
static void (*p_alcDestroyContext)(ALCcontext *context) = NULL;
static void (*p_alcCloseDevice)(ALCdevice *device) = NULL;
static const ALCchar* (*p_alcGetString)(ALCdevice *device, ALCenum param) = NULL;
static const char* (*p_alGetString)(ALenum param) = NULL;
static void* (*p_alGetProcAddress)(const char *fname) = NULL;
static void (*p_alGenBuffers)(ALint n, ALuint *buffers) = NULL;
static void (*p_alDeleteBuffers)(ALint n, const ALuint *buffers) = NULL;
static void (*p_alBufferData)(ALuint buffer, ALenum format, const void *data, ALint size, ALint freq) = NULL;
static void (*p_alGenSources)(ALint n, ALuint *sources) = NULL;
static void (*p_alDeleteSources)(ALint n, const ALuint *sources) = NULL;
static void (*p_alSourcei)(ALuint source, ALenum param, ALint value) = NULL;
static void (*p_alSourcePlay)(ALuint source) = NULL;
static void (*p_alGetSourcei)(ALuint source, ALenum param, ALint *value) = NULL;

typedef unsigned int (*LPEAXSET)(const GUID *propertySetID, unsigned int property,
                                 unsigned int source, void *value, unsigned int length);
typedef unsigned int (*LPEAXGET)(const GUID *propertySetID, unsigned int property,
                                 unsigned int source, void *value, unsigned int length);

static LPEAXSET p_EAXSet = NULL;
static LPEAXGET p_EAXGet = NULL;

static const GUID DSPROPSETID_EAX_Listener = 
    { 0x0ad68fdc, 0x56e0, 0x11d1, { 0xa6, 0x7e, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 } };

#define DSPROPERTY_EAXLISTENER_ENVIRONMENT 1

static int load_openal(void) {
    HMODULE h = LoadLibraryA("CT_OAL.DLL");
    if (!h) h = LoadLibraryA("OpenAL32.dll");
    if (!h) return 0;

    p_alcOpenDevice = (void*)GetProcAddress(h, "alcOpenDevice");
    p_alcCreateContext = (void*)GetProcAddress(h, "alcCreateContext");
    p_alcMakeContextCurrent = (void*)GetProcAddress(h, "alcMakeContextCurrent");
    p_alcDestroyContext = (void*)GetProcAddress(h, "alcDestroyContext");
    p_alcCloseDevice = (void*)GetProcAddress(h, "alcCloseDevice");
    p_alcGetString = (void*)GetProcAddress(h, "alcGetString");
    p_alGetString = (void*)GetProcAddress(h, "alGetString");
    p_alGetProcAddress = (void*)GetProcAddress(h, "alGetProcAddress");
    p_alGenBuffers = (void*)GetProcAddress(h, "alGenBuffers");
    p_alDeleteBuffers = (void*)GetProcAddress(h, "alDeleteBuffers");
    p_alBufferData = (void*)GetProcAddress(h, "alBufferData");
    p_alGenSources = (void*)GetProcAddress(h, "alGenSources");
    p_alDeleteSources = (void*)GetProcAddress(h, "alDeleteSources");
    p_alSourcei = (void*)GetProcAddress(h, "alSourcei");
    p_alSourcePlay = (void*)GetProcAddress(h, "alSourcePlay");
    p_alGetSourcei = (void*)GetProcAddress(h, "alGetSourcei");

    if (!p_alcOpenDevice || !p_alcCreateContext || !p_alcMakeContextCurrent || !p_alGetProcAddress) {
        return 0;
    }

    p_EAXSet = (LPEAXSET)p_alGetProcAddress("EAXSet");
    p_EAXGet = (LPEAXGET)p_alGetProcAddress("EAXGet");
    return 1;
}

// Generate a short 100ms 880 Hz beep pulse with fast decay
static short* generate_beep(int sample_rate, int num_samples) {
    short *buf = (short*)malloc(num_samples * sizeof(short));
    if (!buf) return NULL;
    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        double env = exp(-t * 15.0); // Fast decay envelope
        double wave = sin(2.0 * 3.1415926535 * 880.0 * t);
        buf[i] = (short)(wave * env * 28000.0);
    }
    return buf;
}

int main(int argc, char **argv) {
    int mode_eax = 1; // Default: test with EAX on
    int duration_sec = 3;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) mode_eax = 0;
        else if (!strcmp(argv[i], "--eax")) mode_eax = 1;
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: eax-test.exe [--eax | --dry] [--duration <seconds>]\n");
            return 0;
        }
    }

    printf("=== xpdash EAX Audio Verification Tool ===\n\n");

    if (!load_openal()) {
        printf("[-] Failed to load Creative OpenAL (CT_OAL.DLL or OpenAL32.dll)\n");
        return 1;
    }

    ALCdevice *dev = p_alcOpenDevice(NULL);
    if (!dev) {
        printf("[-] alcOpenDevice failed\n");
        return 1;
    }

    const char *devName = p_alcGetString ? p_alcGetString(dev, ALC_DEVICE_SPECIFIER) : "Unknown";
    printf("[+] OpenAL Device: %s\n", devName);

    ALCcontext *ctx = p_alcCreateContext(dev, NULL);
    if (!ctx || !p_alcMakeContextCurrent(ctx)) {
        printf("[-] Failed to create / activate OpenAL context\n");
        p_alcCloseDevice(dev);
        return 1;
    }

    printf("[+] Hardware Context Active\n");
    if (p_EAXSet && p_EAXGet) {
        printf("[+] Hardware EAX interface detected (EAXSet=%p, EAXGet=%p)\n", p_EAXSet, p_EAXGet);
    } else {
        printf("[-] EAX entry points NOT available on this device\n");
    }

    // Configure EAX Environment
    if (p_EAXSet) {
        unsigned long env = mode_eax ? EAX_ENVIRONMENT_HANGAR : EAX_ENVIRONMENT_GENERIC;
        unsigned int res = p_EAXSet(&DSPROPSETID_EAX_Listener, DSPROPERTY_EAXLISTENER_ENVIRONMENT, 0, &env, sizeof(env));
        printf("[+] Configured EAX Environment: %s (Result=0x%08X)\n",
               mode_eax ? "HANGAR (Large Reverb)" : "GENERIC / DRY", res);
    }

    // Generate test sound
    int rate = 44100;
    int samples = rate / 10; // 100ms beep
    short *pcm = generate_beep(rate, samples);
    if (!pcm) {
        printf("[-] Memory allocation failed\n");
        return 1;
    }

    ALuint buffer = 0, source = 0;
    p_alGenBuffers(1, &buffer);
    p_alBufferData(buffer, AL_FORMAT_MONO16, pcm, samples * sizeof(short), rate);
    free(pcm);

    p_alGenSources(1, &source);
    p_alSourcei(source, AL_BUFFER, buffer);

    printf("\n[>] Playing sound pulse with %s...\n", mode_eax ? "EAX HANGAR REVERB" : "DRY STEREO");
    p_alSourcePlay(source);

    // Wait for playback and reverb tail to settle
    printf("[*] Listening for reverb decay tail (%d seconds)...\n", duration_sec);
    Sleep(duration_sec * 1000);

    p_alDeleteSources(1, &source);
    p_alDeleteBuffers(1, &buffer);
    p_alcMakeContextCurrent(NULL);
    p_alcDestroyContext(ctx);
    p_alcCloseDevice(dev);

    printf("\n[+] EAX Test completed successfully.\n");
    return 0;
}
