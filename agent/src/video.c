#include "video.h"
#include <stdio.h>
#include <stdlib.h>

static HDC g_hdc_screen = NULL;
static HDC g_hdc_mem = NULL;
static HBITMAP g_hbm = NULL;
static HBITMAP g_hbm_old = NULL;
static uint8_t *g_pixels = NULL;
static int g_width = 0;
static int g_height = 0;
static video_frame_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

int video_init(video_frame_cb callback, void *user_data) {
    g_cb = callback;
    g_cb_userdata = user_data;

    g_hdc_screen = GetDC(NULL);
    if (!g_hdc_screen) return 0;

    g_hdc_mem = CreateCompatibleDC(g_hdc_screen);
    if (!g_hdc_mem) return 0;

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    return video_resize(w, h);
}

int video_resize(int new_width, int new_height) {
    if (new_width <= 0 || new_height <= 0) return 0;
    g_width = new_width;
    g_height = new_height;

    if (g_hbm) {
        SelectObject(g_hdc_mem, g_hbm_old);
        DeleteObject(g_hbm);
        g_hbm = NULL;
        g_pixels = NULL;
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
    if (!g_hbm || !g_pixels) return 0;

    g_hbm_old = (HBITMAP)SelectObject(g_hdc_mem, g_hbm);
    return 1;
}

int video_capture(void) {
    if (!g_hdc_screen || !g_hdc_mem || !g_pixels) return 0;

    if (!BitBlt(g_hdc_mem, 0, 0, g_width, g_height, g_hdc_screen, 0, 0, SRCCOPY)) {
        return 0;
    }

    if (g_cb) {
        VideoFrame frame;
        frame.width = (uint16_t)g_width;
        frame.height = (uint16_t)g_height;
        frame.bpp = 32;
        frame.pixels = g_pixels;
        frame.size_bytes = (uint32_t)(g_width * g_height * 4);
        frame.pts_ms = GetTickCount();
        g_cb(&frame, g_cb_userdata);
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
    if (g_hdc_screen) {
        ReleaseDC(NULL, g_hdc_screen);
        g_hdc_screen = NULL;
    }
}
