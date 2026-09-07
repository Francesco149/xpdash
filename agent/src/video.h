#ifndef XPDASH_VIDEO_H
#define XPDASH_VIDEO_H

#include <windows.h>
#include <stdint.h>

typedef void (*video_frame_cb)(const uint8_t *comp_data, uint32_t comp_size,
                               uint32_t frame_index, uint16_t width, uint16_t height,
                               uint8_t codec, uint8_t flags, uint32_t pts_ms,
                               void *user_data);

/* Initialize video capture engine */
int video_init(video_frame_cb callback, void *user_data);

/* Capture and compress a single frame from the desktop if dirty */
int video_capture(void);

/* Force next frame to be a full keyframe */
void video_force_keyframe(void);

/* Handle resolution change event (e.g. from WM_DISPLAYCHANGE) */
int video_resize(int new_width, int new_height);

/* Start video capture background worker thread */
int video_start(void);

/* Stop video capture background worker thread */
void video_stop(void);

/* Cleanup video capture resources */
void video_shutdown(void);

#endif /* XPDASH_VIDEO_H */
