#ifndef XPDASH_VIDEO_H
#define XPDASH_VIDEO_H

#include <windows.h>
#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  *pixels;
    uint32_t size_bytes;
    uint32_t pts_ms;
} VideoFrame;

typedef void (*video_frame_cb)(const VideoFrame *frame, void *user_data);

/* Initialize video capture engine */
int video_init(video_frame_cb callback, void *user_data);

/* Capture a single frame from the desktop */
int video_capture(void);

/* Handle resolution change event (e.g. from WM_DISPLAYCHANGE) */
int video_resize(int new_width, int new_height);

/* Cleanup video capture resources */
void video_shutdown(void);

#endif /* XPDASH_VIDEO_H */
