#ifndef XPDASH_AUDIO_H
#define XPDASH_AUDIO_H

#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_CHANNELS       2
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_FRAME_MS       10
#define AUDIO_FRAME_SAMPLES  (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000) // 480
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8)) // 1920

typedef void (*audio_frame_cb)(const uint8_t *pcm_data, uint32_t size, uint32_t pts_ms, void *user_data);

/* Initialize waveIn capture and configure "What U Hear" mixer line */
int audio_init(audio_frame_cb callback, void *user_data);

/* Start capturing audio stream */
int audio_start(void);

/* Stop capturing audio stream */
void audio_stop(void);

/* Cleanup audio resources */
void audio_shutdown(void);

/* Query whether "What U Hear" was found and selected */
int audio_is_what_u_hear_active(void);

#endif /* XPDASH_AUDIO_H */
