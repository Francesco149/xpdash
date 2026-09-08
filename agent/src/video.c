#include <winsock2.h>
#include "video.h"
#include "lz4.h"
#include "log.h"
#include "net.h"
#include "d3d9hook.h"
#include <turbojpeg.h>
#include <stdio.h>
#include <stdlib.h>
#include <mmsystem.h>
#include <ddraw.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

static int g_capture_layered = 1; // Default to 1 (CAPTUREBLT) for capturing Rainmeter & transparent windows
static int g_target_fps = 60;
static int g_desktop_fps = 60;
static int g_fallback_fps = 20;
static int g_jpeg_quality = 85;
static char g_adapter_name[128] = "Unknown";
static int g_is_accelerated = 1;

static int get_effective_desktop_fps(void) {
    if (!g_is_accelerated) {
        return g_fallback_fps; // 20 FPS degraded fallback on unaccelerated/VGA drivers
    }
    return (g_desktop_fps > 0) ? g_desktop_fps : g_target_fps;
}
static void check_display_driver_acceleration(void) {
    DISPLAY_DEVICEA dd;
    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);

    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        strncpy(g_adapter_name, dd.DeviceString, sizeof(g_adapter_name) - 1);
        g_adapter_name[sizeof(g_adapter_name) - 1] = '\0';
    }

    // Check for fallback / unaccelerated display drivers on Windows XP
    if (strstr(g_adapter_name, "VGA") ||
        strstr(g_adapter_name, "VgaSave") ||
        strstr(g_adapter_name, "Standard") ||
        strstr(g_adapter_name, "Microsoft Basic") ||
        strstr(g_adapter_name, "mv video hook")) {
        g_is_accelerated = 0;
    }

    agent_log("display: active adapter '%s' (hardware_accel=%s)",
              g_adapter_name, g_is_accelerated ? "YES" : "NO (FALLBACK/DEGRADED)");

    if (!g_is_accelerated) {
        agent_log("================================================================");
        agent_log("WARNING: UNACCELERATED OR FALLBACK DISPLAY DRIVER DETECTED!");
        agent_log("Display Adapter: '%s'", g_adapter_name);
        agent_log("Running in software fallback mode (capping desktop at %d FPS to protect CPU).", g_fallback_fps);
        agent_log("Install official GPU drivers (ATI Catalyst / NVIDIA ForceWare) to");
        agent_log("enable hardware Direct3D/DirectDraw acceleration and full 60 FPS streaming.");
        agent_log("================================================================");
    }
}

static void draw_cursor(HDC hdc) {
    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&ci)) {
        static int s_logged_cursor = 0;
        if (!s_logged_cursor) {
            agent_log("draw_cursor: first call! flags=0x%lx, hCursor=%p, pos=(%ld,%ld)",
                      ci.flags, ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y);
            s_logged_cursor = 1;
        }
        /* Only composite cursor if application/OS currently shows it.
           Fullscreen games that hide the cursor (e.g. GTA SA gameplay) set
           CURSOR_SHOWING=0 — in that case we must NOT draw any cursor. */
        if (!(ci.flags & CURSOR_SHOWING)) {
            return;
        }
        HCURSOR hCur = ci.hCursor;
        if (!hCur) {
            hCur = LoadCursor(NULL, IDC_ARROW);
        }
        if (hCur) {
            ICONINFO ii;
            memset(&ii, 0, sizeof(ii));
            if (GetIconInfo(hCur, &ii)) {
                int x = ci.ptScreenPos.x - (int)ii.xHotspot;
                int y = ci.ptScreenPos.y - (int)ii.yHotspot;
                DrawIconEx(hdc, x, y, hCur, 0, 0, 0, NULL, DI_NORMAL);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
    } else {
        static int s_logged_ci_fail = 0;
        if (!s_logged_ci_fail) {
            agent_log("draw_cursor: GetCursorInfo failed! err=%lu", GetLastError());
            s_logged_ci_fail = 1;
        }
    }
}
#define TILE_SIZE 64
#define KEYFRAME_INTERVAL 120
#define JPEG_QUALITY 85

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
static int g_bpp = 32;
static int g_is_paletted = 0;

/* 8-bit paletted capture support */
static HDC g_hdc_mem8 = NULL;
static HBITMAP g_hbm8 = NULL;
static HBITMAP g_hbm8_old = NULL;
static uint8_t *g_pixels8 = NULL;

int video_get_bpp(void) {
    return g_bpp;
}

static RECT s_last_cursor_rect = { 0, 0, 0, 0 };
static int s_has_cursor_rect = 0;
static POINT s_last_cursor_pos = { -1, -1 };
static DWORD s_last_cursor_flags = 0;
/* Restore the 32x32 pixel area under the previous cursor from g_prev_pixels
   without calling BitBlt across the PCIe bus (takes 0.001ms instead of 120ms) */
static void restore_cursor_rect(void) {
    if (!s_has_cursor_rect || !g_pixels || !g_prev_pixels) return;
    int x0 = s_last_cursor_rect.left;
    int y0 = s_last_cursor_rect.top;
    int x1 = s_last_cursor_rect.right;
    int y1 = s_last_cursor_rect.bottom;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_width) x1 = g_width;
    if (y1 > g_height) y1 = g_height;
    int copy_w = x1 - x0;
    if (copy_w <= 0) return;

    for (int y = y0; y < y1; y++) {
        uint32_t *dst = (uint32_t *)g_pixels + (y * g_width + x0);
        const uint32_t *src = (const uint32_t *)g_prev_pixels + (y * g_width + x0);
        memcpy(dst, src, copy_w * 4);
    }
}

static video_frame_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

static tjhandle g_tj = NULL;     /* TurboJPEG compressor instance */

static HANDLE g_h_video_thread = NULL;
static volatile int g_video_running = 0;
static DWORD s_last_hook_frame_time = 0;
static int   s_in_hook_mode = 0;
static DWORD WINAPI video_worker_thread(LPVOID lpParam) {
    (void)lpParam;
    agent_log("video_worker_thread: capture thread started");
    DWORD frame_interval = (g_target_fps > 0) ? (1000 / g_target_fps) : 16;
    if (frame_interval == 0) frame_interval = 16;

    while (g_video_running) {
        if (net_is_streaming_active()) {
            DWORD t_start = timeGetTime();

            /* Frame capture: in hook mode, timing is driven by game's Present().
               On desktop, frame rate is paced via high-resolution multimedia timer. */
            video_capture();

            if (!s_in_hook_mode) {
                DWORD elapsed = timeGetTime() - t_start;
                int eff_fps = get_effective_desktop_fps();
                DWORD frame_interval = (eff_fps > 0) ? (1000 / eff_fps) : 16;
                if (elapsed < frame_interval) {
                    Sleep(frame_interval - elapsed);
                } else {
                    Sleep(0);
                }
            }
        } else {
            Sleep(50);
        }
    }
    agent_log("video_worker_thread: capture thread stopped");
    return 0;
}

int video_start(void) {
    if (g_h_video_thread != NULL) return 1;
    g_video_running = 1;
    g_h_video_thread = CreateThread(NULL, 0, video_worker_thread, NULL, 0, NULL);
    if (!g_h_video_thread) {
        agent_log("CreateThread for video_worker_thread failed! err=%lu", GetLastError());
        g_video_running = 0;
        return 0;
    }
    SetThreadPriority(g_h_video_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    agent_log("video_start: worker thread launched");
    return 1;
}

void video_stop(void) {
    if (g_h_video_thread) {
        g_video_running = 0;
        WaitForSingleObject(g_h_video_thread, 2000);
        CloseHandle(g_h_video_thread);
        g_h_video_thread = NULL;
    }
}
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

    /* Re-acquire screen DC to reflect mode/color depth changes */
    if (g_hdc_screen) {
        ReleaseDC(NULL, g_hdc_screen);
        g_hdc_screen = NULL;
    }
    g_hdc_screen = GetDC(NULL);
    if (!g_hdc_screen) {
        agent_log("video_resize: GetDC(NULL) failed! err=%lu", GetLastError());
        return 0;
    }

    if (!g_hdc_mem) {
        g_hdc_mem = CreateCompatibleDC(g_hdc_screen);
    }

    /* Query display color depth and palette capability */
    int dev_bpp = GetDeviceCaps(g_hdc_screen, BITSPIXEL);
    int dev_planes = GetDeviceCaps(g_hdc_screen, PLANES);
    int raster_caps = GetDeviceCaps(g_hdc_screen, RASTERCAPS);
    g_bpp = dev_bpp * dev_planes;
    g_is_paletted = (g_bpp == 8) || (raster_caps & RC_PALETTE);
    agent_log("video_resize: display mode %dx%d@%d (paletted=%d, rc=0x%04X)",
              g_width, g_height, g_bpp, g_is_paletted, raster_caps);

    /* Clean up old 32-bit DIBSection */
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

    /* Size comp_buf for worst-case of both codecs */
    unsigned long jpeg_max = tjBufSize(g_width, g_height, TJSAMP_420);
    int lz4_max = LZ4_compressBound(raw_size);
    int max_comp_size = (int)jpeg_max > lz4_max ? (int)jpeg_max : lz4_max;
    if (max_comp_size > g_comp_buf_cap) {
        if (g_comp_buf) free(g_comp_buf);
        g_comp_buf = (uint8_t *)malloc(max_comp_size);
        if (!g_comp_buf) {
            agent_log("malloc comp_buf failed!");
            return 0;
        }
        g_comp_buf_cap = max_comp_size;
    }

    /* Allocate 32-bit DIBSection for compositing & TurboJPEG compression */
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
        agent_log("CreateDIBSection 32-bit failed! err=%lu", GetLastError());
        return 0;
    }
    g_hbm_old = (HBITMAP)SelectObject(g_hdc_mem, g_hbm);
    agent_log("CreateDIBSection 32-bit success: hbm=%p, pixels=%p", g_hbm, g_pixels);

    /* Allocate or clean up 8-bit DIBSection for paletted capture */
    if (g_hbm8) {
        if (g_hdc_mem8 && g_hbm8_old) {
            SelectObject(g_hdc_mem8, g_hbm8_old);
        }
        DeleteObject(g_hbm8);
        g_hbm8 = NULL;
        g_pixels8 = NULL;
    }

    if (g_is_paletted) {
        if (!g_hdc_mem8) {
            g_hdc_mem8 = CreateCompatibleDC(g_hdc_screen);
        }

        struct {
            BITMAPINFOHEADER bmiHeader;
            RGBQUAD bmiColors[256];
        } bmi8;
        memset(&bmi8, 0, sizeof(bmi8));
        bmi8.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi8.bmiHeader.biWidth = g_width;
        bmi8.bmiHeader.biHeight = -g_height; // Top-down DIB
        bmi8.bmiHeader.biPlanes = 1;
        bmi8.bmiHeader.biBitCount = 8;
        bmi8.bmiHeader.biCompression = BI_RGB;
        bmi8.bmiHeader.biClrUsed = 256;

        PALETTEENTRY sys_pal[256];
        memset(sys_pal, 0, sizeof(sys_pal));
        GetSystemPaletteEntries(g_hdc_screen, 0, 256, sys_pal);
        for (int i = 0; i < 256; i++) {
            bmi8.bmiColors[i].rgbRed = sys_pal[i].peRed;
            bmi8.bmiColors[i].rgbGreen = sys_pal[i].peGreen;
            bmi8.bmiColors[i].rgbBlue = sys_pal[i].peBlue;
            bmi8.bmiColors[i].rgbReserved = 0;
        }

        g_hbm8 = CreateDIBSection(g_hdc_screen, (BITMAPINFO *)&bmi8, DIB_RGB_COLORS, (void **)&g_pixels8, NULL, 0);
        if (!g_hbm8 || !g_pixels8) {
            agent_log("CreateDIBSection 8-bit failed! err=%lu", GetLastError());
            return 0;
        }
        g_hbm8_old = (HBITMAP)SelectObject(g_hdc_mem8, g_hbm8);
        agent_log("CreateDIBSection 8-bit success: hbm=%p, pixels=%p", g_hbm8, g_pixels8);
    } else {
        if (g_hdc_mem8) {
            DeleteDC(g_hdc_mem8);
            g_hdc_mem8 = NULL;
        }
    }

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
    /* Initialize TurboJPEG compressor for JPEG video encoding */
    g_tj = tjInitCompress();
    if (!g_tj) {
        agent_log("video_init: tjInitCompress failed!");
        return 0;
    }
    agent_log("video_init: TurboJPEG compressor initialized");

    if (!g_hdc_mem) {
        agent_log("CreateCompatibleDC failed! err=%lu", GetLastError());
        return 0;
    }

    g_capture_layered = GetPrivateProfileIntA("video", "capture_layered", 1, "C:\\xpdash\\agent.ini");
    g_target_fps = GetPrivateProfileIntA("video", "target_fps", 60, "C:\\xpdash\\agent.ini");
    if (g_target_fps <= 0 || g_target_fps > 120) g_target_fps = 60;

    g_desktop_fps = GetPrivateProfileIntA("video", "desktop_fps", g_target_fps, "C:\\xpdash\\agent.ini");
    if (g_desktop_fps <= 0 || g_desktop_fps > 120) g_desktop_fps = g_target_fps;
    g_jpeg_quality = GetPrivateProfileIntA("video", "jpeg_quality", 85, "C:\\xpdash\\agent.ini");
    if (g_jpeg_quality < 30 || g_jpeg_quality > 100) g_jpeg_quality = 85;

    check_display_driver_acceleration();
    int eff_fps = get_effective_desktop_fps();
    agent_log("video_init: capture_layered=%d, target_fps=%d, desktop_fps=%d (effective=%d), jpeg_quality=%d",
              g_capture_layered, g_target_fps, g_desktop_fps, eff_fps, g_jpeg_quality);
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    agent_log("video_init: GetSystemMetrics = %dx%d", w, h);
    return video_resize(w, h);
}

/* Fast comparison: check if visible 24-bit RGB framebuffer has differences,
   masking out the unused 4th byte (X/Alpha) which contains uninitialized VRAM padding in GDI */
static int is_screen_dirty(void) {
    if (!g_pixels || !g_prev_pixels) return 1;
    const uint32_t *p1 = (const uint32_t *)g_pixels;
    const uint32_t *p2 = (const uint32_t *)g_prev_pixels;
    int total_pixels = g_width * g_height;

    int rgb_diff = 0;
    for (int i = 0; i < total_pixels; i++) {
        uint32_t diff = p1[i] ^ p2[i];
        if ((diff & 0x00FFFFFF) != 0) {
            rgb_diff++;
            if (rgb_diff > 16) {
                return 1;
            }
        }
    }
    return 0;
}

int video_capture(void) {
    DWORD t0 = timeGetTime();
    uint32_t now = t0;

    /* ── Path A: D3D9 hook capture (zero-flicker, perfect timing) ──── */
    if (d3d9hook_is_active()) {
        uint32_t hook_w = 0, hook_h = 0, hook_idx = 0;

        /* Ensure our pixel buffer can hold the hook's frame */
        d3d9hook_get_dimensions(&hook_w, &hook_h);
        if (hook_w > 0 && hook_h > 0 &&
            ((int)hook_w != g_width || (int)hook_h != g_height)) {
            video_resize((int)hook_w, (int)hook_h);
        }

        if (g_pixels && d3d9hook_read_frame(g_pixels, &hook_w, &hook_h,
                                             &hook_idx, 16)) {
            s_last_hook_frame_time = now;
            if (!s_in_hook_mode) {
                s_in_hook_mode = 1;
                agent_log("video_capture: switched to D3D9 hook stream (%ux%u)", hook_w, hook_h);
                video_force_keyframe();
            }
            g_frame_counter = hook_idx;

            /* Hook frames are always "dirty" — the hook only fires on Present */
            DWORD t1 = timeGetTime();

            /* Draw cursor onto hooked frame */
            draw_cursor(g_hdc_mem);

            DWORD t2 = timeGetTime();

            unsigned long jpeg_size = 0;
            unsigned char *jpeg_buf = g_comp_buf;
            int comp_size = 0;
            uint8_t codec;

            if (g_tj && tjCompress2(g_tj, g_pixels, (int)hook_w, 0, (int)hook_h,
                                     TJPF_BGRX, &jpeg_buf, &jpeg_size,
                                     TJSAMP_420, g_jpeg_quality,
                                     TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == 0) {
                comp_size = (int)jpeg_size;
                codec = 1;  /* VIDEO_CODEC_JPEG */
            } else {
                /* JPEG failed — fall back to LZ4 */
                int raw_size = (int)hook_w * (int)hook_h * 4;
                comp_size = LZ4_compress_fast((const char *)g_pixels,
                    (char *)g_comp_buf, raw_size, g_comp_buf_cap, 10);
                codec = 2;  /* VIDEO_CODEC_LZ4 */
            }

            DWORD t3 = timeGetTime();

            if (comp_size > 0 && g_cb) {
                uint8_t flags = 0x01;  /* hook frames are always keyframes */

                g_cb(g_comp_buf, (uint32_t)comp_size, g_frame_counter,
                     (uint16_t)hook_w, (uint16_t)hook_h, codec, flags,
                     now, g_cb_userdata);

                DWORD t4 = timeGetTime();
                static uint32_t s_hook_timing = 0;
                s_hook_timing++;
                if (s_hook_timing >= 120) {
                    agent_log("frame_timing[hook]: read=%lums cursor=%lums "
                              "comp=%lums send=%lums total=%lums comp_sz=%d",
                              (unsigned long)(t1 - t0),
                              (unsigned long)(t2 - t1),
                              (unsigned long)(t3 - t2),
                              (unsigned long)(t4 - t3),
                              (unsigned long)(t4 - t0), comp_size);
                    s_hook_timing = 0;
                }
            }
            return 1;
        }

        /* If hook was active recently (< 120ms), wait for next Present() */
        if (s_in_hook_mode && (now - s_last_hook_frame_time < 120)) {
            return 1;
        }

        /* Game is minimized, tabbed out, or paused — fall through to Path B (BitBlt)
           so the desktop stream continues seamlessly! */
        if (s_in_hook_mode) {
            s_in_hook_mode = 0;
            agent_log("video_capture: hook idle (%lums), falling back to desktop BitBlt",
                      (unsigned long)(now - s_last_hook_frame_time));
            video_force_keyframe();
        }
    } else {
        if (s_in_hook_mode) {
            s_in_hook_mode = 0;
            video_force_keyframe();
        }
    }

    /* ── Path B: GDI BitBlt capture (fallback for desktop / non-D3D) ─ */
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int cur_bpp = GetDeviceCaps(g_hdc_screen, BITSPIXEL) * GetDeviceCaps(g_hdc_screen, PLANES);
    /* Only check and adapt to desktop screen resolution if D3D9 hook is not active */
    if (!d3d9hook_is_active() && screen_w > 0 && screen_h > 0 &&
        (g_width != screen_w || g_height != screen_h || g_bpp != cur_bpp)) {
        agent_log("video_capture: screen display changed to %dx%d@%d", screen_w, screen_h, cur_bpp);
        video_resize(screen_w, screen_h);
        net_send_video_resize((uint16_t)screen_w, (uint16_t)screen_h, (uint8_t)cur_bpp);
        video_force_keyframe();
    }
    if (!g_hdc_screen || !g_hdc_mem || !g_pixels || !g_comp_buf) {
        static int s_logged_null = 0;
        if (!s_logged_null) {
            agent_log("video_capture: null pointers! screen=%p, mem=%p, pix=%p, comp=%p",
                      g_hdc_screen, g_hdc_mem, g_pixels, g_comp_buf);
            s_logged_null = 1;
        }
        return 0;
    }

    if (g_is_paletted && g_hdc_mem8 && g_pixels8) {
        if (!BitBlt(g_hdc_mem8, 0, 0, g_width, g_height, g_hdc_screen, 0, 0, SRCCOPY)) {
            static int s_logged_blt8_fail = 0;
            if (!s_logged_blt8_fail) {
                agent_log("video_capture: 8-bit BitBlt failed! err=%lu", GetLastError());
                s_logged_blt8_fail = 1;
            }
            return 0;
        }

        /* Retrieve active hardware/system palette (updates dynamically on palette animation & transitions) */
        PALETTEENTRY sys_pal[256];
        memset(sys_pal, 0, sizeof(sys_pal));
        GetSystemPaletteEntries(g_hdc_screen, 0, 256, sys_pal);

        /* Build 256-entry 32-bit BGRX LUT */
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) {
            lut[i] = ((uint32_t)sys_pal[i].peRed << 16) |
                     ((uint32_t)sys_pal[i].peGreen << 8) |
                     ((uint32_t)sys_pal[i].peBlue);
        }

        /* Expand 8-bit indices into 32-bit BGRX pixel buffer */
        uint32_t *dst32 = (uint32_t *)g_pixels;
        const uint8_t *src8 = g_pixels8;
        int total_pixels = g_width * g_height;
        for (int i = 0; i < total_pixels; i++) {
            dst32[i] = lut[src8[i]];
        }
    } else {
        DWORD rop = SRCCOPY;
        if (g_capture_layered) rop |= CAPTUREBLT;
        if (!BitBlt(g_hdc_mem, 0, 0, g_width, g_height, g_hdc_screen, 0, 0, rop)) {
            // Fallback to plain SRCCOPY if CAPTUREBLT failed on an exotic driver
            if ((rop & CAPTUREBLT) && BitBlt(g_hdc_mem, 0, 0, g_width, g_height, g_hdc_screen, 0, 0, SRCCOPY)) {
                static int s_logged_rop_fb = 0;
                if (!s_logged_rop_fb) {
                    agent_log("video_capture: CAPTUREBLT failed, falling back to plain SRCCOPY");
                    s_logged_rop_fb = 1;
                }
            } else {
                static int s_logged_blt_fail = 0;
                if (!s_logged_blt_fail) {
                    agent_log("video_capture: BitBlt failed! err=%lu", GetLastError());
                    s_logged_blt_fail = 1;
                }
                return 0;
            }
        }
    }

    DWORD t1 = timeGetTime();
    g_frame_counter++;

    /* Query cursor movement */
    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(CURSORINFO);
    int cursor_moved = 0;
    if (GetCursorInfo(&ci)) {
        if (ci.ptScreenPos.x != s_last_cursor_pos.x ||
            ci.ptScreenPos.y != s_last_cursor_pos.y ||
            ci.flags != s_last_cursor_flags) {
            cursor_moved = 1;
            s_last_cursor_pos = ci.ptScreenPos;
            s_last_cursor_flags = ci.flags;
        }
    }

    int is_keyframe = g_force_keyframe || ((g_frame_counter % KEYFRAME_INTERVAL) == 0);
    int screen_dirty = is_keyframe ? 1 : is_screen_dirty();

    /* If neither the screen changed nor the cursor moved, skip completely! */
    if (!screen_dirty && !cursor_moved) {
        return 1;
    }

    int raw_size = g_width * g_height * 4;

    if (screen_dirty) {
        /* Desktop contents changed: save clean desktop to g_prev_pixels */
        memcpy(g_prev_pixels, g_pixels, raw_size);
        g_force_keyframe = 0;
        s_has_cursor_rect = 0;
    } else if (cursor_moved) {
        /* Desktop clean, only cursor moved: restore old cursor rectangle from g_prev_pixels
           in 0.001ms without touching VRAM across PCIe bus! */
        restore_cursor_rect();
    }

    /* Draw cursor at current position */
    draw_cursor(g_hdc_mem);

    /* Save cursor bounding rectangle for fast restoration on next frame */
    s_last_cursor_rect.left = ci.ptScreenPos.x - 4;
    s_last_cursor_rect.top = ci.ptScreenPos.y - 4;
    s_last_cursor_rect.right = ci.ptScreenPos.x + 36;
    s_last_cursor_rect.bottom = ci.ptScreenPos.y + 36;
    s_has_cursor_rect = 1;

    DWORD t2 = timeGetTime();

    unsigned long jpeg_size = 0;
    unsigned char *jpeg_buf = g_comp_buf;
    int comp_size = 0;
    uint8_t codec;

    if (g_tj && tjCompress2(g_tj, g_pixels, g_width, 0, g_height,
                             TJPF_BGRX, &jpeg_buf, &jpeg_size,
                             TJSAMP_420, g_jpeg_quality,
                             TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == 0) {
        comp_size = (int)jpeg_size;
        codec = 1;  /* VIDEO_CODEC_JPEG */
    } else {
        /* JPEG failed — fall back to LZ4 */
        comp_size = LZ4_compress_fast((const char *)g_pixels, (char *)g_comp_buf,
                                      raw_size, g_comp_buf_cap, 10);
        codec = 2;  /* VIDEO_CODEC_LZ4 */
    }

    DWORD t3 = timeGetTime();

    static int s_logged_first_frame = 0;
    if (!s_logged_first_frame) {
        agent_log("video_capture: first frame encoded! raw=%d, comp=%d, codec=%d, cb=%p",
                  raw_size, comp_size, codec, g_cb);
        s_logged_first_frame = 1;
    }

    if (comp_size > 0 && g_cb) {
        uint8_t flags = is_keyframe ? 0x01 : 0x00;

        g_cb(g_comp_buf, (uint32_t)comp_size, g_frame_counter,
             (uint16_t)g_width, (uint16_t)g_height, codec, flags, now, g_cb_userdata);

        DWORD t4 = timeGetTime();

        // Note: g_prev_pixels is already updated with clean desktop above!
        /* Log frame timing every 120 frames (~2s at 60fps) */
        static uint32_t s_timing_accum = 0;
        s_timing_accum++;
        if (s_timing_accum >= 10) {
            agent_log("frame_timing[blt]: blt=%lums cursor=%lums comp=%lums send=%lums total=%lums comp_sz=%d",
                      (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                      (unsigned long)(t3 - t2), (unsigned long)(t4 - t3),
                      (unsigned long)(t4 - t0), comp_size);
            s_timing_accum = 0;
        }
    }

    return 1;
}

void video_shutdown(void) {
    video_stop();
    if (g_hdc_mem8) {
        if (g_hbm8 && g_hbm8_old) {
            SelectObject(g_hdc_mem8, g_hbm8_old);
        }
        DeleteDC(g_hdc_mem8);
        g_hdc_mem8 = NULL;
    }
    if (g_hbm8) {
        DeleteObject(g_hbm8);
        g_hbm8 = NULL;
        g_pixels8 = NULL;
    }
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
    if (g_tj) {
        tjDestroy(g_tj);
        g_tj = NULL;
    }
    if (g_hdc_screen) {
        ReleaseDC(NULL, g_hdc_screen);
        g_hdc_screen = NULL;
    }
}
