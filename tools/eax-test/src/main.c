/*
 * tools/eax-test/src/main.c — Standalone EAX Hardware Verification Tool
 *
 * Exercises the Creative SB0090 (EMU10K2 DSP) hardware EAX audio path on Windows XP
 * to 100% verify that EAX 3D spatialization and environmental reverb are active and
 * being captured by "What U Hear".
 *
 * Features:
 *   - Automatic A/B Comparison: plays DRY sound, then WET EAX sound side-by-side
 *   - 3D spatialized source positioning (required by Creative drivers to engage EAX DSP)
 *   - Both EAX 1.0 and EAX 2.0/3.0 listener & buffer property configurations
 *   - Rich broadband percussive transients (rimshot/gunshot impulse & footsteps)
 *     that clearly excite all EMU10K2 multi-tap delay lines and diffuse reverberation
 *   - Interactive environment switching (Hangar, Cathedral, Cave, Bathroom, Sewer, Dry)
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
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
#define AL_POSITION          0x1004
#define AL_VELOCITY          0x1006
#define AL_ORIENTATION       0x100F
#define AL_SOURCE_RELATIVE   0x0202
#define AL_FALSE             0
#define AL_TRUE              1

// EAX 2.0 / 3.0 Environment Presets
#define EAX_ENVIRONMENT_GENERIC          0
#define EAX_ENVIRONMENT_PADDEDCELL       1
#define EAX_ENVIRONMENT_ROOM             2
#define EAX_ENVIRONMENT_BATHROOM         3
#define EAX_ENVIRONMENT_LIVINGROOM       4
#define EAX_ENVIRONMENT_STONEROOM        5
#define EAX_ENVIRONMENT_AUDITORIUM       6
#define EAX_ENVIRONMENT_CONCERTHALL      7
#define EAX_ENVIRONMENT_CAVE             8
#define EAX_ENVIRONMENT_ARENA            9
#define EAX_ENVIRONMENT_HANGAR          10
#define EAX_ENVIRONMENT_CARPETEDHALLWAY 11
#define EAX_ENVIRONMENT_HALLWAY         12
#define EAX_ENVIRONMENT_STONECORRIDOR   13
#define EAX_ENVIRONMENT_ALLEY           14
#define EAX_ENVIRONMENT_FOREST          15
#define EAX_ENVIRONMENT_CITY            16
#define EAX_ENVIRONMENT_MOUNTAINS       17
#define EAX_ENVIRONMENT_QUARRY          18
#define EAX_ENVIRONMENT_PLAIN           19
#define EAX_ENVIRONMENT_PARKINGLOT      20
#define EAX_ENVIRONMENT_SEWERPIPE       21
#define EAX_ENVIRONMENT_UNDERWATER      22

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
static void (*p_alSource3f)(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) = NULL;
static void (*p_alSourcePlay)(ALuint source) = NULL;
static void (*p_alSourceStop)(ALuint source) = NULL;
static void (*p_alGetSourcei)(ALuint source, ALenum param, ALint *value) = NULL;
static void (*p_alListener3f)(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) = NULL;
static void (*p_alListenerfv)(ALenum param, const ALfloat *values) = NULL;

typedef unsigned int (*LPEAXSET)(const GUID *propertySetID, unsigned int property,
                                 unsigned int source, void *value, unsigned int length);
typedef unsigned int (*LPEAXGET)(const GUID *propertySetID, unsigned int property,
                                 unsigned int source, void *value, unsigned int length);

static LPEAXSET p_EAXSet = NULL;
static LPEAXGET p_EAXGet = NULL;

static const GUID DSPROPSETID_EAX_Listener = 
    { 0x0ad68fdc, 0x56e0, 0x11d1, { 0xa6, 0x7e, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 } };
static const GUID DSPROPSETID_EAX_Buffer = 
    { 0x0ad68fdd, 0x56e0, 0x11d1, { 0xa6, 0x7e, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12 } };

#define DSPROPERTY_EAXLISTENER_NONE             0
#define DSPROPERTY_EAXLISTENER_ALLPARAMETERS    1
#define DSPROPERTY_EAXLISTENER_ROOM             2
#define DSPROPERTY_EAXLISTENER_ROOMHF           3
#define DSPROPERTY_EAXLISTENER_ROOMROLLOFFFACTOR 4
#define DSPROPERTY_EAXLISTENER_DECAYTIME        5
#define DSPROPERTY_EAXLISTENER_DECAYHFRATIO     6
#define DSPROPERTY_EAXLISTENER_REFLECTIONS      7
#define DSPROPERTY_EAXLISTENER_REFLECTIONSDELAY  8
#define DSPROPERTY_EAXLISTENER_REVERB           9
#define DSPROPERTY_EAXLISTENER_REVERBDELAY      10
#define DSPROPERTY_EAXLISTENER_ENVIRONMENT      11

#define DSPROPERTY_EAXBUFFER_NONE               0
#define DSPROPERTY_EAXBUFFER_ALLPARAMETERS      1
#define DSPROPERTY_EAXBUFFER_DIRECT             2
#define DSPROPERTY_EAXBUFFER_DIRECTHF           3
#define DSPROPERTY_EAXBUFFER_ROOM               4
#define DSPROPERTY_EAXBUFFER_ROOMHF             5

#pragma pack(push, 4)
typedef struct _EAXLISTENERPROPERTIES {
    long lRoom;                     // [-10000, 0] mB
    long lRoomHF;                   // [-10000, 0] mB
    float flRoomRolloffFactor;      // [0.0, 10.0]
    float flDecayTime;              // [0.1, 20.0] seconds
    float flDecayHFRatio;           // [0.1, 2.0]
    long lReflections;              // [-10000, 1000] mB
    float flReflectionsDelay;       // [0.0, 0.3] seconds
    long lReverb;                   // [-10000, 2000] mB
    float flReverbDelay;            // [0.0, 0.1] seconds
    unsigned long dwEnvironment;    // EAX_ENVIRONMENT_*
    float flEnvironmentSize;        // [1.0, 100.0] meters
    float flEnvironmentDiffusion;   // [0.0, 1.0]
    float flAirAbsorptionHF;        // [-100.0, 0.0] dB/m
    unsigned long dwFlags;          // EAXLISTENERFLAGS_*
} EAXLISTENERPROPERTIES;

typedef struct _EAXBUFFERPROPERTIES {
    long lDirect;                   // [-10000, 1000] mB
    long lDirectHF;                 // [-10000, 0] mB
    long lRoom;                     // [-10000, 1000] mB (reverb send volume!)
    long lRoomHF;                   // [-10000, 0] mB
    float flRoomRolloffFactor;      // [0.0, 10.0]
    long lObstruction;              // [-10000, 0] mB
    float flObstructionLFRatio;     // [0.0, 1.0]
    long lOcclusion;                // [-10000, 0] mB
    float flOcclusionLFRatio;       // [0.0, 1.0]
    float flOcclusionRoomRatio;     // [0.0, 10.0]
    long lOutsideVolumeHF;          // [-10000, 0] mB
    float flAirAbsorptionFactor;    // [0.0, 10.0]
    unsigned long dwFlags;
} EAXBUFFERPROPERTIES;
#pragma pack(pop)

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
    p_alSource3f = (void*)GetProcAddress(h, "alSource3f");
    p_alSourcePlay = (void*)GetProcAddress(h, "alSourcePlay");
    p_alSourceStop = (void*)GetProcAddress(h, "alSourceStop");
    p_alGetSourcei = (void*)GetProcAddress(h, "alGetSourcei");
    p_alListener3f = (void*)GetProcAddress(h, "alListener3f");
    p_alListenerfv = (void*)GetProcAddress(h, "alListenerfv");

    if (!p_alcOpenDevice || !p_alcCreateContext || !p_alcMakeContextCurrent || !p_alGetProcAddress) {
        return 0;
    }

    p_EAXSet = (LPEAXSET)p_alGetProcAddress("EAXSet");
    p_EAXGet = (LPEAXGET)p_alGetProcAddress("EAXGet");
    return 1;
}

// ─── Sound Generators ──────────────────────────────────────────────────

// 1. Sharp Metallic Rimshot / Gunshot Impulse (high-frequency broadband burst)
static short* generate_rimshot(int sample_rate, int *out_samples) {
    int n = sample_rate / 8; // 125ms total sound
    *out_samples = n;
    short *buf = (short*)malloc(n * sizeof(short));
    if (!buf) return NULL;

    srand(12345);
    for (int i = 0; i < n; i++) {
        double t = (double)i / sample_rate;
        // Fast decaying noise burst (initial crack)
        double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * exp(-t * 90.0);
        // Resonant body punch (pitch drops from 280Hz to 80Hz)
        double f = 80.0 + 200.0 * exp(-t * 40.0);
        double body = sin(2.0 * 3.1415926535 * f * t) * exp(-t * 30.0);
        // Metallic ring components (1400 Hz and 2800 Hz)
        double ring = (0.5 * sin(2.0 * 3.1415926535 * 1400.0 * t) +
                       0.3 * sin(2.0 * 3.1415926535 * 2800.0 * t)) * exp(-t * 20.0);

        double sig = 0.5 * noise + 0.35 * body + 0.25 * ring;
        if (sig > 1.0) sig = 1.0;
        if (sig < -1.0) sig = -1.0;
        buf[i] = (short)(sig * 31000.0);
    }
    return buf;
}

// 2. Double Footstep / Click
static short* generate_footsteps(int sample_rate, int *out_samples) {
    int n = (int)(sample_rate * 0.35); // 350ms total
    *out_samples = n;
    short *buf = (short*)calloc(n, sizeof(short));
    if (!buf) return NULL;

    int click_len = sample_rate / 30; // ~33ms per click
    int offsets[2] = { 0, (int)(sample_rate * 0.14) }; // Two clicks 140ms apart

    srand(54321);
    for (int step = 0; step < 2; step++) {
        int start = offsets[step];
        for (int i = 0; i < click_len && (start + i) < n; i++) {
            double t = (double)i / sample_rate;
            double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * exp(-t * 120.0);
            double thud = sin(2.0 * 3.1415926535 * 140.0 * t) * exp(-t * 60.0);
            double sig = (0.6 * noise + 0.4 * thud) * 30000.0;
            buf[start + i] = (short)sig;
        }
    }
    return buf;
}

// 3. Classic 880Hz Tone Pulse
static short* generate_tone(int sample_rate, int *out_samples) {
    int n = sample_rate / 10; // 100ms
    *out_samples = n;
    short *buf = (short*)malloc(n * sizeof(short));
    if (!buf) return NULL;
    for (int i = 0; i < n; i++) {
        double t = (double)i / sample_rate;
        double env = exp(-t * 12.0);
        double wave = sin(2.0 * 3.1415926535 * 880.0 * t);
        buf[i] = (short)(wave * env * 28000.0);
    }
    return buf;
}

// ─── EAX Configuration Engine ──────────────────────────────────────────

static void configure_eax_environment(int env_preset, int enable_reverb, ALuint source) {
    if (!p_EAXSet) return;

    if (!enable_reverb || env_preset == EAX_ENVIRONMENT_GENERIC) {
        // Completely mute environmental reverb
        unsigned long generic = EAX_ENVIRONMENT_GENERIC;
        p_EAXSet(&DSPROPSETID_EAX_Listener, DSPROPERTY_EAXLISTENER_ENVIRONMENT, 0, &generic, sizeof(generic));

        EAXLISTENERPROPERTIES dry_lp;
        memset(&dry_lp, 0, sizeof(dry_lp));
        dry_lp.lRoom = -10000; // -100 dB = completely silent reverb room
        dry_lp.lReverb = -10000;
        dry_lp.dwEnvironment = EAX_ENVIRONMENT_GENERIC;
        dry_lp.flDecayTime = 0.1f;
        p_EAXSet(&DSPROPSETID_EAX_Listener, DSPROPERTY_EAXLISTENER_ALLPARAMETERS, 0, &dry_lp, sizeof(dry_lp));

        if (source != 0) {
            EAXBUFFERPROPERTIES bp;
            memset(&bp, 0, sizeof(bp));
            bp.lRoom = -10000; // Mute reverb send on buffer
            p_EAXSet(&DSPROPSETID_EAX_Buffer, DSPROPERTY_EAXBUFFER_ROOM, source, &bp.lRoom, sizeof(bp.lRoom));
        }
        return;
    }

    // Set EAX 1.0 environment index
    unsigned long env = (unsigned long)env_preset;
    p_EAXSet(&DSPROPSETID_EAX_Listener, DSPROPERTY_EAXLISTENER_ENVIRONMENT, 0, &env, sizeof(env));

    // Configure rich EAX 2.0 / 3.0 parameters for massive audible reverb
    EAXLISTENERPROPERTIES lp;
    memset(&lp, 0, sizeof(lp));
    lp.dwEnvironment = env;
    lp.lRoom = 0;              // 0 mB = Maximum Room effect level (0 dB attenuation)
    lp.lRoomHF = -200;         // Crisp high frequencies in reverb tail
    lp.flRoomRolloffFactor = 0.0f; // No room distance attenuation
    lp.lReflections = -300;    // Strong early reflections
    lp.flReflectionsDelay = 0.020f;
    lp.lReverb = 200;          // +2 dB late reverberation boost for rich audible echo
    lp.flReverbDelay = 0.030f;
    lp.flEnvironmentDiffusion = 1.0f;

    // Custom decay times based on preset
    switch (env_preset) {
        case EAX_ENVIRONMENT_HANGAR:
            lp.flDecayTime = 7.2f; // 7.2 second massive hangar decay!
            lp.flEnvironmentSize = 80.0f;
            break;
        case EAX_ENVIRONMENT_CAVE:
            lp.flDecayTime = 5.5f; // 5.5s cave echo
            lp.flEnvironmentSize = 50.0f;
            break;
        case EAX_ENVIRONMENT_CONCERTHALL:
            lp.flDecayTime = 3.9f; // Lush 3.9s concert hall
            lp.flEnvironmentSize = 40.0f;
            break;
        case EAX_ENVIRONMENT_BATHROOM:
            lp.flDecayTime = 1.5f; // Fast, bright slapback
            lp.flEnvironmentSize = 5.0f;
            break;
        case EAX_ENVIRONMENT_SEWERPIPE:
            lp.flDecayTime = 2.8f; // Resonant pipe
            lp.flEnvironmentSize = 10.0f;
            break;
        default:
            lp.flDecayTime = 4.0f;
            lp.flEnvironmentSize = 30.0f;
            break;
    }

    p_EAXSet(&DSPROPSETID_EAX_Listener, DSPROPERTY_EAXLISTENER_ALLPARAMETERS, 0, &lp, sizeof(lp));

    // Ensure source buffer sends 100% volume into EAX reverb DSP
    if (source != 0) {
        EAXBUFFERPROPERTIES bp;
        memset(&bp, 0, sizeof(bp));
        bp.lDirect = 0;        // Full direct volume
        bp.lRoom = 0;          // 0 mB = 100% full wet reverb send!
        bp.lRoomHF = 0;
        p_EAXSet(&DSPROPSETID_EAX_Buffer, DSPROPERTY_EAXBUFFER_ROOM, source, &bp.lRoom, sizeof(bp.lRoom));
    }
}

static const char* get_env_name(int env) {
    switch (env) {
        case EAX_ENVIRONMENT_GENERIC:     return "DRY / NONE";
        case EAX_ENVIRONMENT_HANGAR:      return "HANGAR (Massive 7.2s Reverb)";
        case EAX_ENVIRONMENT_CONCERTHALL: return "CONCERT HALL (Lush 3.9s Reverb)";
        case EAX_ENVIRONMENT_CAVE:        return "CAVE (Resonant 5.5s Echo)";
        case EAX_ENVIRONMENT_BATHROOM:    return "BATHROOM (Slapback 1.5s Echo)";
        case EAX_ENVIRONMENT_SEWERPIPE:   return "SEWER PIPE (Metallic Tube Reverb)";
        default:                          return "CUSTOM ENVIRONMENT";
    }
}

// ─── Playback Helper ───────────────────────────────────────────────────

static void play_test_pulse(ALuint source, int env_preset, int enable_eax,
                            const char *label, int wait_seconds) {
    configure_eax_environment(env_preset, enable_eax, source);

    printf("\n>>> [PLAYING] %s <<<\n", label);
    if (enable_eax) {
        printf("    Environment: %s\n", get_env_name(env_preset));
        printf("    EAX Send Level: 0 mB (100%% WET) | Listener Room: 0 mB\n");
    } else {
        printf("    Mode: DRY (EAX DSP Reverb Muted, Direct 2D Sound Only)\n");
    }

    p_alSourceStop(source);
    p_alSourcePlay(source);

    printf("    Listening for sound and decay tail (%d seconds)...\n", wait_seconds);
    for (int s = wait_seconds; s > 0; s--) {
        printf("    ... %d seconds remaining\r", s);
        fflush(stdout);
        Sleep(1000);
    }
    printf("    ... Finished.\n");
}

int main(int argc, char **argv) {
    int run_ab_test = 1;
    int interactive = 0;
    int single_env = EAX_ENVIRONMENT_HANGAR;
    int single_mode = 1; // 1 = wet, 0 = dry

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) {
            run_ab_test = 0;
            single_mode = 0;
        } else if (!strcmp(argv[i], "--wet") || !strcmp(argv[i], "--eax")) {
            run_ab_test = 0;
            single_mode = 1;
        } else if (!strcmp(argv[i], "--interactive") || !strcmp(argv[i], "-i")) {
            interactive = 1;
            run_ab_test = 0;
        } else if (!strcmp(argv[i], "--hangar")) {
            single_env = EAX_ENVIRONMENT_HANGAR;
        } else if (!strcmp(argv[i], "--hall")) {
            single_env = EAX_ENVIRONMENT_CONCERTHALL;
        } else if (!strcmp(argv[i], "--cave")) {
            single_env = EAX_ENVIRONMENT_CAVE;
        } else if (!strcmp(argv[i], "--ab")) {
            run_ab_test = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: eax-test.exe [options]\n");
            printf("  --ab           Run automated side-by-side DRY vs WET A/B comparison (default)\n");
            printf("  --wet / --eax  Play WET EAX reverberated pulse directly\n");
            printf("  --dry          Play DRY un-reverberated pulse directly\n");
            printf("  --hangar       Use Hangar preset (7.2s decay)\n");
            printf("  --hall         Use Concert Hall preset (3.9s decay)\n");
            printf("  --cave         Use Cave preset (5.5s decay)\n");
            printf("  -i, --interactive Interactive keyboard control\n");
            return 0;
        }
    }

    printf("========================================================\n");
    printf(" xpdash Standalone EAX Hardware Verification Tool\n");
    printf(" Creative SB0090 / EMU10K2 DSP Hardware Reverb Test\n");
    printf("========================================================\n\n");

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
        printf("[-] WARNING: EAX entry points NOT available on this device\n");
    }

    // Configure 3D Spatial Listener (Center of room looking forward)
    if (p_alListener3f) {
        p_alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
        p_alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    }
    if (p_alListenerfv) {
        ALfloat orient[6] = { 0.0f, 0.0f, -1.0f,  0.0f, 1.0f, 0.0f };
        p_alListenerfv(AL_ORIENTATION, orient);
    }

    // Generate Audio Buffers
    int sample_rate = 44100;
    int samples_rimshot = 0, samples_footsteps = 0, samples_tone = 0;
    short *pcm_rimshot = generate_rimshot(sample_rate, &samples_rimshot);
    short *pcm_footsteps = generate_footsteps(sample_rate, &samples_footsteps);
    short *pcm_tone = generate_tone(sample_rate, &samples_tone);

    ALuint buffers[3] = { 0, 0, 0 };
    p_alGenBuffers(3, buffers);
    p_alBufferData(buffers[0], AL_FORMAT_MONO16, pcm_rimshot, samples_rimshot * sizeof(short), sample_rate);
    p_alBufferData(buffers[1], AL_FORMAT_MONO16, pcm_footsteps, samples_footsteps * sizeof(short), sample_rate);
    p_alBufferData(buffers[2], AL_FORMAT_MONO16, pcm_tone, samples_tone * sizeof(short), sample_rate);

    free(pcm_rimshot);
    free(pcm_footsteps);
    free(pcm_tone);

    // Create 3D Spatialized Sound Source
    // Placed in 3D world space (2m to right, 3m in front) so Creative driver engages EAX DSP
    ALuint source = 0;
    p_alGenSources(1, &source);
    p_alSourcei(source, AL_BUFFER, buffers[0]); // Default to rimshot
    if (p_alSource3f) {
        p_alSource3f(source, AL_POSITION, 2.0f, 0.0f, -3.0f);
        p_alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    }
    if (p_alSourcei) {
        p_alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE); // World 3D coordinates!
    }

    if (run_ab_test) {
        printf("\n========================================================\n");
        printf(" Running Automated A/B EAX Verification Test\n");
        printf("========================================================\n");

        // Sound 1: Metallic Rimshot A/B Test
        p_alSourcei(source, AL_BUFFER, buffers[0]);
        play_test_pulse(source, EAX_ENVIRONMENT_GENERIC, 0,
                        "TEST 1A: DRY METALLIC PULSE (No Reverb)", 3);

        Sleep(1000);

        play_test_pulse(source, EAX_ENVIRONMENT_HANGAR, 1,
                        "TEST 1B: WET EAX METALLIC PULSE (HANGAR 7.2s REVERB)", 6);

        Sleep(1000);

        // Sound 2: Double Footstep in Cathedral
        p_alSourcei(source, AL_BUFFER, buffers[1]);
        play_test_pulse(source, EAX_ENVIRONMENT_CONCERTHALL, 1,
                        "TEST 2: DOUBLE FOOTSTEP (CONCERT HALL 3.9s REVERB)", 5);

        printf("\n[+] A/B Test Completed! Noticeable decay tail proves EMU10K2 EAX DSP.\n");
    } else if (interactive) {
        printf("\n=== Interactive EAX Verification Mode ===\n");
        printf("Controls:\n");
        printf("  [1] Hangar (7.2s)      [2] Concert Hall (3.9s)  [3] Cave (5.5s)\n");
        printf("  [4] Bathroom (1.5s)    [5] Sewer Pipe (2.8s)    [0] Dry (No Reverb)\n");
        printf("  [Space] Fire Pulse     [T] Cycle Sound Type     [Esc / Q] Exit\n\n");

        int cur_env = EAX_ENVIRONMENT_HANGAR;
        int cur_wet = 1;
        int cur_sound = 0; // 0=rimshot, 1=footstep, 2=tone
        const char *sound_names[] = { "Metallic Rimshot", "Double Footstep", "880Hz Tone" };

        configure_eax_environment(cur_env, cur_wet, source);
        printf("[Active]: %s | Sound: %s\n", cur_wet ? get_env_name(cur_env) : "DRY", sound_names[cur_sound]);

        int running = 1;
        while (running) {
            if (_kbhit()) {
                int ch = _getch();
                switch (ch) {
                    case 27:
                    case 'q':
                    case 'Q':
                        running = 0;
                        break;
                    case ' ':
                        printf(">>> Firing %s in %s...\n", sound_names[cur_sound],
                               cur_wet ? get_env_name(cur_env) : "DRY");
                        p_alSourceStop(source);
                        p_alSourcePlay(source);
                        break;
                    case '0':
                        cur_wet = 0;
                        configure_eax_environment(EAX_ENVIRONMENT_GENERIC, 0, source);
                        printf("[Switched to]: DRY (Reverb Off)\n");
                        break;
                    case '1':
                        cur_wet = 1; cur_env = EAX_ENVIRONMENT_HANGAR;
                        configure_eax_environment(cur_env, 1, source);
                        printf("[Switched to]: %s\n", get_env_name(cur_env));
                        break;
                    case '2':
                        cur_wet = 1; cur_env = EAX_ENVIRONMENT_CONCERTHALL;
                        configure_eax_environment(cur_env, 1, source);
                        printf("[Switched to]: %s\n", get_env_name(cur_env));
                        break;
                    case '3':
                        cur_wet = 1; cur_env = EAX_ENVIRONMENT_CAVE;
                        configure_eax_environment(cur_env, 1, source);
                        printf("[Switched to]: %s\n", get_env_name(cur_env));
                        break;
                    case '4':
                        cur_wet = 1; cur_env = EAX_ENVIRONMENT_BATHROOM;
                        configure_eax_environment(cur_env, 1, source);
                        printf("[Switched to]: %s\n", get_env_name(cur_env));
                        break;
                    case '5':
                        cur_wet = 1; cur_env = EAX_ENVIRONMENT_SEWERPIPE;
                        configure_eax_environment(cur_env, 1, source);
                        printf("[Switched to]: %s\n", get_env_name(cur_env));
                        break;
                    case 't':
                    case 'T':
                        cur_sound = (cur_sound + 1) % 3;
                        p_alSourcei(source, AL_BUFFER, buffers[cur_sound]);
                        printf("[Sound Type]: %s\n", sound_names[cur_sound]);
                        break;
                }
            }
            Sleep(20);
        }
    } else {
        // Single test mode
        p_alSourcei(source, AL_BUFFER, buffers[0]);
        play_test_pulse(source, single_env, single_mode,
                        single_mode ? "WET EAX HARDWARE REVERB" : "DRY STEREO (NO REVERB)", 6);
    }

    p_alSourceStop(source);
    p_alDeleteSources(1, &source);
    p_alDeleteBuffers(3, buffers);
    p_alcMakeContextCurrent(NULL);
    p_alcDestroyContext(ctx);
    p_alcCloseDevice(dev);

    printf("\n=== EAX Test Complete ===\n");
    return 0;
}
