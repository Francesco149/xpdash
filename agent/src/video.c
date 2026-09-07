#include "video.h"
#include "lz4.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <mmsystem.h>

#define TILE_SIZE 64
#define KEYFRAME_INTERVAL 60

static HDC g_hdc_screen = NULL;
static HDC g_hdc_mem = NULL;
static HBITMAP g_hbm = NULL;
static HBITMAP g_hbm_old = NULL;
static uint8_t *g_pixels = NULL;
static uint8_t *g_prev_pixels = NULL;
static uint8_t *g_comp_buf = NULL;
static int g_comp_buf_cap = 0;

static int g_width = 0;
static int g_height = 0;
static uint32_t g_frame_counter = 0;
static int g_force_keyframe = 1;

static video_frame_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

void video_force_keyframe(void) {
    g_force_keyframe = 1;
}

int video_resize(int new_width, int new_height) {
    agent_log("video_resize called: %dx%d", new_width, new_height);
    if (new_width <= 0 || new_height <= 0) {
        agent_log("video_resize invalid dims!");
        return 0;
    }

    g_width = new_width;
    g_height = new_height;

    if (g_hbm) {
        if (g_hdc_mem && g_hbm_old) {
            SelectObject(g_hdc_mem, g_hbm_old);
        }
        DeleteObject(g_hbm);
        g_hbm = NULL;
        g_pixels = NULL;
    }

    if (g_prev_pixels) {
        free(g_prev_pixels);
        g_prev_pixels = NULL;
    }

    int raw_size = g_width * g_height * 4;
    g_prev_pixels = (uint8_t *)malloc(raw_size);
    if (!g_prev_pixels) {
        agent_log("malloc prev_pixels failed!");
        return 0;
    }
    memset(g_prev_pixels, 0, raw_size);

    int max_comp_size = LZ4_compressBound(raw_size);
    if (max_comp_size > g_comp_buf_cap) {
        if (g_comp_buf) free(g_comp_buf);
        g_comp_buf = (uint8_t *)malloc(max_comp_size);
        if (!g_comp_buf) {
            agent_log("malloc comp_buf failed!");
            return 0;
        }
        g_comp_buf_cap = max_comp_size;
    }

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_width;
    bmi.bmiHeader.biHeight = -g_height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hbm = CreateDIBSection(g_hdc_screen, &bmi, DIB_RGB_COLORS, (void **)&g_pixels, NULL, 0);
    if (!g_hbm || !g_pixels) {
        agent_log("CreateDIBSection failed! err=%lu", GetLastError());
        return 0;
    }
    agent_log("CreateDIBSection success: hbm=%p, pixels=%p", g_hbm, g_pixels);

    g_hbm_old = (HBITMAP)SelectObject(g_hdc_mem, g_hbm);
    g_force_keyframe = 1;
    return 1;
}

int video_init(video_frame_cb callback, void *user_data) {
    g_cb = callback;
    g_cb_userdata = user_data;

    g_hdc_screen = GetDC(NULL);
    agent_log("video_init: GetDC(NULL) = %p", g_hdc_screen);
    if (!g_hdc_screen) {
        agent_log("GetDC(NULL) failed! err=%lu", GetLastError());
        return 0;
    }

    g_hdc_mem = CreateCompatibleDC(g_hdc_screen);
    agent_log("video_init: CreateCompatibleDC = %p", g_hdc_mem);
    if (!g_hdc_mem) {
        agent_log("CreateCompatibleDC failed! err=%lu", GetLastError());
        return 0;
    }

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    agent_log("video_init: GetSystemMetrics = %dx%d", w, h);
    return video_resize(w, h);
}

/* Fast comparison: check if framebuffer has differences */
static int is_screen_dirty(void) {
    if (!g_pixels || !g_prev_pixels) return 1;
    int raw_size = g_width * g_height * 4;
    return (memcmp(g_pixels, g_prev_pixels, raw_size) != 0);
}

int video_capture(void) {
    if (!g_hdc_screen || !g_hdc_mem || !g_pixels || !g_comp_buf) {
        static int s_logged_null = 0;
        if (!s_logged_null) {
            agent_log("video_capture: null pointers! screen=%p, mem=%p, pix=%p, comp=%p",
                      g_hdc_screen, g_hdc_mem, g_pixels, g_comp_buf);
            s_logged_null = 1;
        }
        return 0;
    }

    if (!BitBlt(g_hdc_mem, 0, 0, g_width, g_height, g_hdc_screen, 0, 0, SRCCOPY)) {
        static int s_logged_blt_fail = 0;
        if (!s_logged_blt_fail) {
            agent_log("video_capture: BitBlt failed! err=%lu", GetLastError());
            s_logged_blt_fail = 1;
        }
        return 0;
    }

    uint32_t now = timeGetTime();
    g_frame_counter++;

    int is_keyframe = g_force_keyframe || ((g_frame_counter % KEYFRAME_INTERVAL) == 0);
    int dirty = is_keyframe ? 1 : is_screen_dirty();

    if (!dirty) {
        return 1; // Nothing changed
    }

    int raw_size = g_width * g_height * 4;
    int comp_size = LZ4_compress_fast((const char *)g_pixels, (char *)g_comp_buf,
                                      raw_size, g_comp_buf_cap, 1);

    static int s_logged_first_frame = 0;
    if (!s_logged_first_frame) {
        agent_log("video_capture: first frame encoded! raw=%d, comp=%d, cb=%p", raw_size, comp_size, g_cb);
        s_logged_first_frame = 1;
    }

    if (comp_size > 0 && g_cb) {
        uint8_t flags = is_keyframe ? 0x01 : 0x00;
        uint8_t codec = 2; // Fast LZ4

        g_cb(g_comp_buf, (uint32_t)comp_size, g_frame_counter,
             (uint16_t)g_width, (uint16_t)g_height, codec, flags, now, g_cb_userdata);

        memcpy(g_prev_pixels, g_pixels, raw_size);
        g_force_keyframe = 0;
    }

    return 1;
}

void video_shutdown(void) {
    if (g_hdc_mem) {
        if (g_hbm && g_hbm_old) {
            SelectObject(g_hdc_mem, g_hbm_old);
        }
        DeleteDC(g_hdc_mem);
        g_hdc_mem = NULL;
    }
    if (g_hbm) {
        DeleteObject(g_hbm);
        g_hbm = NULL;
        g_pixels = NULL;
    }
    if (g_prev_pixels) {
        free(g_prev_pixels);
        g_prev_pixels = NULL;
    }
    if (g_comp_buf) {
        free(g_comp_buf);
        g_comp_buf = NULL;
        g_comp_buf_cap = 0;
    }
    if (g_hdc_screen) {
        ReleaseDC(NULL, g_hdc_screen);
        g_hdc_screen = NULL;
    }
}
