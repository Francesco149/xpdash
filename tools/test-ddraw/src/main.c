/*
 * tools/test-ddraw/src/main.c — DirectDraw 7 Graphics & Display Configuration Test
 *
 * Tests Windows XP DirectDraw 7 hardware surface capture, fullscreen exclusive page
 * flipping, windowed clipper blits, 8-bit paletted surfaces, 16-bit (565/555) high
 * color, 32-bit true color, and surface restoration after focus loss.
 *
 * Key controls:
 *   F / Enter : Toggle Fullscreen Exclusive / Windowed
 *   1         : 640x480
 *   2         : 800x600
 *   3         : 1024x768
 *   P         : Toggle palette cycling (in 8-bit mode)
 *   Esc / Q   : Exit
 *
 * Command-line options:
 *   --fullscreen      Start in fullscreen exclusive mode (default)
 *   --windowed        Start in windowed mode
 *   --res WxH         Set resolution (default 800x600)
 *   --bpp N           Set color depth (8, 16, 32; default 32)
 *   --auto SECONDS    Run automatically for N seconds and exit cleanly
 */

#define _WIN32_WINNT 0x0501
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <mmsystem.h>

#define WINDOW_CLASS_NAME "XpDash_TestDDraw"
#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600

static HWND g_hwnd = NULL;
static int g_fullscreen = 1;
static int g_target_w = DEFAULT_WIDTH;
static int g_target_h = DEFAULT_HEIGHT;
static int g_target_bpp = 32;
static int g_auto_seconds = 0;
static DWORD g_start_time = 0;

/* DirectDraw Interfaces */
static LPDIRECTDRAW7        g_pdd = NULL;
static LPDIRECTDRAWSURFACE7 g_pdds_primary = NULL;
static LPDIRECTDRAWSURFACE7 g_pdds_back = NULL;
static LPDIRECTDRAWCLIPPER  g_clipper = NULL;
static LPDIRECTDRAWPALETTE  g_pdd_pal = NULL;

static int g_pixel_format = 32; /* 8, 15 (555), 16 (565), or 32 */
static int g_palette_cycling = 1;
static UINT g_palette_shift = 0;
static PALETTEENTRY g_pal_entries[256];

/* Plasma & Animation state */
static float g_plasma_t = 0.0f;
static float g_box_x = 80.0f;
static float g_box_y = 80.0f;
static float g_box_vx = 240.0f;
static float g_box_vy = 180.0f;
static int g_bounce_count = 0;

/* Telemetry */
static DWORD g_last_fps_time = 0;
static int g_frames = 0;
static float g_fps = 0.0f;
static DWORD g_total_frames = 0;

static void cleanup_ddraw_surfaces(void) {
    if (g_pdd_pal) {
        g_pdd_pal->lpVtbl->Release(g_pdd_pal);
        g_pdd_pal = NULL;
    }
    if (g_clipper) {
        g_clipper->lpVtbl->Release(g_clipper);
        g_clipper = NULL;
    }
    if (g_fullscreen) {
        if (g_pdds_primary) {
            g_pdds_primary->lpVtbl->Release(g_pdds_primary);
            g_pdds_primary = NULL;
            g_pdds_back = NULL; // Backbuffer is owned by flip chain
        }
    } else {
        if (g_pdds_back) {
            g_pdds_back->lpVtbl->Release(g_pdds_back);
            g_pdds_back = NULL;
        }
        if (g_pdds_primary) {
            g_pdds_primary->lpVtbl->Release(g_pdds_primary);
            g_pdds_primary = NULL;
        }
    }
}

static void cleanup_ddraw(void) {
    cleanup_ddraw_surfaces();
    if (g_pdd) {
        g_pdd->lpVtbl->RestoreDisplayMode(g_pdd);
        g_pdd->lpVtbl->SetCooperativeLevel(g_pdd, g_hwnd, DDSCL_NORMAL);
        g_pdd->lpVtbl->Release(g_pdd);
        g_pdd = NULL;
    }
}

static int init_ddraw(int w, int h, int bpp, int fullscreen) {
    cleanup_ddraw();

    HRESULT hr = DirectDrawCreateEx(NULL, (void **)&g_pdd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !g_pdd) {
        printf("[-] DirectDrawCreateEx failed: 0x%08lX\n", (unsigned long)hr);
        return 0;
    }

    if (fullscreen) {
        hr = g_pdd->lpVtbl->SetCooperativeLevel(g_pdd, g_hwnd,
            DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
        if (FAILED(hr)) {
            printf("[-] SetCooperativeLevel (exclusive) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }

        hr = g_pdd->lpVtbl->SetDisplayMode(g_pdd, w, h, bpp, 0, 0);
        if (FAILED(hr)) {
            printf("[-] SetDisplayMode(%dx%d@%d) failed: 0x%08lX\n", w, h, bpp, (unsigned long)hr);
            return 0;
        }

        /* Create complex flipping primary surface with 1 back buffer */
        DDSURFACEDESC2 ddsd;
        memset(&ddsd, 0, sizeof(ddsd));
        ddsd.dwSize = sizeof(ddsd);
        ddsd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        ddsd.dwBackBufferCount = 1;

        hr = g_pdd->lpVtbl->CreateSurface(g_pdd, &ddsd, &g_pdds_primary, NULL);
        if (FAILED(hr) || !g_pdds_primary) {
            printf("[-] CreateSurface (primary flip) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }

        DDSCAPS2 caps;
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        hr = g_pdds_primary->lpVtbl->GetAttachedSurface(g_pdds_primary, &caps, &g_pdds_back);
        if (FAILED(hr) || !g_pdds_back) {
            printf("[-] GetAttachedSurface (backbuffer) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }

        /* If 8-bit mode, create and attach palette */
        if (bpp == 8) {
            for (int i = 0; i < 256; i++) {
                float hue = (float)i / 256.0f * 6.2831853f;
                g_pal_entries[i].peRed   = (uint8_t)(127.5f * (1.0f + sinf(hue)));
                g_pal_entries[i].peGreen = (uint8_t)(127.5f * (1.0f + sinf(hue + 2.0943951f)));
                g_pal_entries[i].peBlue  = (uint8_t)(127.5f * (1.0f + sinf(hue + 4.1887902f)));
                g_pal_entries[i].peFlags = PC_RESERVED;
            }

            hr = g_pdd->lpVtbl->CreatePalette(g_pdd, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
                                              g_pal_entries, &g_pdd_pal, NULL);
            if (SUCCEEDED(hr) && g_pdd_pal) {
                g_pdds_primary->lpVtbl->SetPalette(g_pdds_primary, g_pdd_pal);
            }
        }
    } else {
        /* Windowed mode */
        hr = g_pdd->lpVtbl->SetCooperativeLevel(g_pdd, g_hwnd, DDSCL_NORMAL);
        if (FAILED(hr)) {
            printf("[-] SetCooperativeLevel (normal) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }

        DDSURFACEDESC2 ddsd;
        memset(&ddsd, 0, sizeof(ddsd));
        ddsd.dwSize = sizeof(ddsd);
        ddsd.dwFlags = DDSD_CAPS;
        ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

        hr = g_pdd->lpVtbl->CreateSurface(g_pdd, &ddsd, &g_pdds_primary, NULL);
        if (FAILED(hr) || !g_pdds_primary) {
            printf("[-] CreateSurface (primary windowed) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }

        /* Create Clipper to clip primary surface to window bounds */
        hr = g_pdd->lpVtbl->CreateClipper(g_pdd, 0, &g_clipper, NULL);
        if (SUCCEEDED(hr) && g_clipper) {
            g_clipper->lpVtbl->SetHWnd(g_clipper, 0, g_hwnd);
            g_pdds_primary->lpVtbl->SetClipper(g_pdds_primary, g_clipper);
        }

        /* Create Offscreen plain backbuffer */
        memset(&ddsd, 0, sizeof(ddsd));
        ddsd.dwSize = sizeof(ddsd);
        ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        ddsd.dwWidth = w;
        ddsd.dwHeight = h;

        hr = g_pdd->lpVtbl->CreateSurface(g_pdd, &ddsd, &g_pdds_back, NULL);
        if (FAILED(hr) || !g_pdds_back) {
            printf("[-] CreateSurface (offscreen backbuffer) failed: 0x%08lX\n", (unsigned long)hr);
            return 0;
        }
    }

    /* Inspect pixel format */
    DDPIXELFORMAT pf;
    memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(pf);
    hr = g_pdds_back->lpVtbl->GetPixelFormat(g_pdds_back, &pf);
    if (SUCCEEDED(hr)) {
        if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
            g_pixel_format = 8;
        } else if (pf.dwFlags & DDPF_RGB) {
            if (pf.dwRGBBitCount == 16) {
                if (pf.dwGBitMask == 0x07E0) {
                    g_pixel_format = 16; // RGB 565
                } else {
                    g_pixel_format = 15; // RGB 555
                }
            } else if (pf.dwRGBBitCount == 32) {
                g_pixel_format = 32;
            } else {
                g_pixel_format = pf.dwRGBBitCount;
            }
        }
    }

    printf("[test-ddraw] Initialized %s DirectDraw: %dx%d, format=%d-bit (pf_bpp=%lu)\n",
           fullscreen ? "EXCLUSIVE FULLSCREEN" : "WINDOWED",
           w, h, g_pixel_format, pf.dwRGBBitCount);
    return 1;
}

static void update_window_layout(void) {
    if (!g_hwnd) return;

    if (g_fullscreen) {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT rc = { 0, 0, g_target_w, g_target_h };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        int win_w = rc.right - rc.left;
        int win_h = rc.bottom - rc.top;
        int pos_x = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
        int pos_y = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;
        if (pos_x < 0) pos_x = 0;
        if (pos_y < 0) pos_y = 0;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, pos_x, pos_y, win_w, win_h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static void restore_surfaces_if_lost(void) {
    if (!g_pdds_primary) return;
    if (g_pdds_primary->lpVtbl->IsLost(g_pdds_primary) == DDERR_SURFACELOST) {
        g_pdds_primary->lpVtbl->Restore(g_pdds_primary);
    }
    if (g_pdds_back && g_pdds_back != g_pdds_primary) {
        if (g_pdds_back->lpVtbl->IsLost(g_pdds_back) == DDERR_SURFACELOST) {
            g_pdds_back->lpVtbl->Restore(g_pdds_back);
        }
    }
}

static inline uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t rgb_to_555(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

static inline uint32_t rgb_to_32(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)((r << 16) | (g << 8) | b);
}

static void render_frame(float dt) {
    if (!g_pdds_back) return;
    restore_surfaces_if_lost();

    g_plasma_t += dt * 1.5f;

    /* Update bouncing box */
    g_box_x += g_box_vx * dt;
    g_box_y += g_box_vy * dt;
    float box_w = 120.0f;
    float box_h = 70.0f;

    if (g_box_x < 0.0f) {
        g_box_x = 0.0f;
        g_box_vx = -g_box_vx;
        g_bounce_count++;
    } else if (g_box_x + box_w > (float)g_target_w) {
        g_box_x = (float)g_target_w - box_w;
        g_box_vx = -g_box_vx;
        g_bounce_count++;
    }

    if (g_box_y < 80.0f) {
        g_box_y = 80.0f;
        g_box_vy = -g_box_vy;
        g_bounce_count++;
    } else if (g_box_y + box_h > (float)g_target_h) {
        g_box_y = (float)g_target_h - box_h;
        g_box_vy = -g_box_vy;
        g_bounce_count++;
    }

    /* Lock backbuffer surface */
    DDSURFACEDESC2 ddsd;
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);

    HRESULT hr = g_pdds_back->lpVtbl->Lock(g_pdds_back, NULL, &ddsd, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) {
        return;
    }

    uint8_t *pixels = (uint8_t *)ddsd.lpSurface;
    int pitch = ddsd.lPitch;
    int width = g_target_w;
    int height = g_target_h;

    /* 1. Procedural plasma / color bars rendering */
    int bar_height = 60;
    int num_bars = 8;
    int bar_width = width / num_bars;

    uint8_t bar_colors[8][3] = {
        { 255, 255, 255 }, // White
        { 255, 255,   0 }, // Yellow
        {   0, 255, 255 }, // Cyan
        {   0, 255,   0 }, // Green
        { 255,   0, 255 }, // Magenta
        { 255,   0,   0 }, // Red
        {   0,   0, 255 }, // Blue
        {   0,   0,   0 }  // Black
    };

    if (g_pixel_format == 8) {
        /* 8-bit paletted direct pixel write */
        for (int y = 0; y < height; y++) {
            uint8_t *row = pixels + y * pitch;
            if (y < bar_height) {
                int b_idx = (y < bar_height) ? (0) : 0;
                (void)b_idx;
                for (int x = 0; x < width; x++) {
                    int b = x / bar_width;
                    if (b > 7) b = 7;
                    row[x] = (uint8_t)(b * 32);
                }
            } else {
                float fy = (float)y * 0.02f;
                for (int x = 0; x < width; x++) {
                    float fx = (float)x * 0.02f;
                    float v = sinf(fx + g_plasma_t) +
                              sinf(fy + g_plasma_t * 0.7f) +
                              sinf((fx + fy) * 0.5f + g_plasma_t * 1.3f);
                    uint8_t idx = (uint8_t)((v + 3.0f) * 40.0f);
                    row[x] = idx;
                }
            }
        }
    } else if (g_pixel_format == 16 || g_pixel_format == 15) {
        /* 16-bit RGB 565 or 555 */
        int is_565 = (g_pixel_format == 16);
        for (int y = 0; y < height; y++) {
            uint16_t *row = (uint16_t *)(pixels + y * pitch);
            if (y < bar_height) {
                for (int x = 0; x < width; x++) {
                    int b = x / bar_width;
                    if (b > 7) b = 7;
                    uint8_t cr = bar_colors[b][0];
                    uint8_t cg = bar_colors[b][1];
                    uint8_t cb = bar_colors[b][2];
                    row[x] = is_565 ? rgb_to_565(cr, cg, cb) : rgb_to_555(cr, cg, cb);
                }
            } else {
                float fy = (float)y * 0.015f;
                for (int x = 0; x < width; x++) {
                    float fx = (float)x * 0.015f;
                    float v1 = sinf(fx + g_plasma_t);
                    float v2 = sinf(fy + g_plasma_t * 0.8f);
                    float v3 = sinf((fx + fy + g_plasma_t) * 0.5f);
                    uint8_t cr = (uint8_t)(127.5f * (1.0f + v1));
                    uint8_t cg = (uint8_t)(127.5f * (1.0f + v2));
                    uint8_t cb = (uint8_t)(127.5f * (1.0f + v3));
                    row[x] = is_565 ? rgb_to_565(cr, cg, cb) : rgb_to_555(cr, cg, cb);
                }
            }
        }
    } else {
        /* 32-bit BGRX */
        for (int y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(pixels + y * pitch);
            if (y < bar_height) {
                for (int x = 0; x < width; x++) {
                    int b = x / bar_width;
                    if (b > 7) b = 7;
                    row[x] = rgb_to_32(bar_colors[b][0], bar_colors[b][1], bar_colors[b][2]);
                }
            } else {
                float fy = (float)y * 0.015f;
                for (int x = 0; x < width; x++) {
                    float fx = (float)x * 0.015f;
                    float v1 = sinf(fx + g_plasma_t);
                    float v2 = sinf(fy + g_plasma_t * 0.8f);
                    float v3 = sinf((fx + fy + g_plasma_t) * 0.5f);
                    uint8_t cr = (uint8_t)(127.5f * (1.0f + v1));
                    uint8_t cg = (uint8_t)(127.5f * (1.0f + v2));
                    uint8_t cb = (uint8_t)(127.5f * (1.0f + v3));
                    row[x] = rgb_to_32(cr, cg, cb);
                }
            }
        }
    }

    /* 2. Draw bouncing box */
    int bx0 = (int)g_box_x;
    int by0 = (int)g_box_y;
    int bx1 = bx0 + (int)box_w;
    int by1 = by0 + (int)box_h;
    if (bx1 > width) bx1 = width;
    if (by1 > height) by1 = height;

    for (int y = by0; y < by1; y++) {
        if (g_pixel_format == 8) {
            uint8_t *row = pixels + y * pitch;
            for (int x = bx0; x < bx1; x++) {
                if (x == bx0 || x == bx1 - 1 || y == by0 || y == by1 - 1)
                    row[x] = 255;
                else
                    row[x] = 120;
            }
        } else if (g_pixel_format == 16 || g_pixel_format == 15) {
            uint16_t *row = (uint16_t *)(pixels + y * pitch);
            uint16_t col = (g_pixel_format == 16) ? rgb_to_565(0, 220, 255) : rgb_to_555(0, 220, 255);
            uint16_t edge = (g_pixel_format == 16) ? rgb_to_565(255, 255, 255) : rgb_to_555(255, 255, 255);
            for (int x = bx0; x < bx1; x++) {
                row[x] = (x == bx0 || x == bx1 - 1 || y == by0 || y == by1 - 1) ? edge : col;
            }
        } else {
            uint32_t *row = (uint32_t *)(pixels + y * pitch);
            uint32_t col = rgb_to_32(0, 220, 255);
            uint32_t edge = rgb_to_32(255, 255, 255);
            for (int x = bx0; x < bx1; x++) {
                row[x] = (x == bx0 || x == bx1 - 1 || y == by0 || y == by1 - 1) ? edge : col;
            }
        }
    }

    g_pdds_back->lpVtbl->Unlock(g_pdds_back, NULL);

    /* 3. Render HUD via DirectDraw GDI surface context */
    HDC hdc = NULL;
    hr = g_pdds_back->lpVtbl->GetDC(g_pdds_back, &hdc);
    if (SUCCEEDED(hr) && hdc) {
        SetBkMode(hdc, TRANSPARENT);
        char buf[256];

        /* Header */
        SetTextColor(hdc, RGB(0, 255, 128));
        TextOutA(hdc, 20, 80, "=== xpdash DirectDraw 7 Test Application ===", 44);

        /* Telemetry */
        SetTextColor(hdc, RGB(255, 255, 255));
        snprintf(buf, sizeof(buf), "Mode: %s (%dx%d @ %d-bit)",
                 g_fullscreen ? "EXCLUSIVE FULLSCREEN (Flip Chain)" : "WINDOWED (Clipper Blt)",
                 width, height, g_pixel_format);
        TextOutA(hdc, 20, 105, buf, strlen(buf));

        SetTextColor(hdc, RGB(255, 215, 0));
        snprintf(buf, sizeof(buf), "FPS: %.1f | Frame: %lu | Bounces: %d",
                 g_fps, g_total_frames, g_bounce_count);
        TextOutA(hdc, 20, 125, buf, strlen(buf));

        if (g_auto_seconds > 0) {
            DWORD elapsed = (timeGetTime() - g_start_time) / 1000;
            int remaining = g_auto_seconds - (int)elapsed;
            if (remaining < 0) remaining = 0;
            snprintf(buf, sizeof(buf), "Auto-Exit in %d seconds...", remaining);
            SetTextColor(hdc, RGB(255, 100, 100));
            TextOutA(hdc, 20, 150, buf, strlen(buf));
        }

        /* Controls */
        SetTextColor(hdc, RGB(180, 200, 220));
        int legend_y = height - 70;
        if (legend_y < 180) legend_y = 180;
        TextOutA(hdc, 20, legend_y,      "[F] Toggle Fullscreen / Windowed   [1] 640x480   [2] 800x600", 61);
        TextOutA(hdc, 20, legend_y + 20, "[3] 1024x768                       [P] Palette   [Esc/Q] Exit", 61);

        g_pdds_back->lpVtbl->ReleaseDC(g_pdds_back, hdc);
    }

    /* 4. Animate palette in 8-bit mode */
    if (g_pdd_pal && g_palette_cycling) {
        g_palette_shift = (g_palette_shift + 1) % 256;
        PALETTEENTRY shifted_pal[256];
        for (int i = 0; i < 256; i++) {
            shifted_pal[i] = g_pal_entries[(i + g_palette_shift) % 256];
        }
        g_pdd_pal->lpVtbl->SetEntries(g_pdd_pal, 0, 0, 256, shifted_pal);
    }

    /* 5. Flip or Blt */
    if (g_fullscreen) {
        hr = g_pdds_primary->lpVtbl->Flip(g_pdds_primary, NULL, DDFLIP_WAIT);
    } else {
        RECT client_rc;
        GetClientRect(g_hwnd, &client_rc);
        POINT pt = { 0, 0 };
        ClientToScreen(g_hwnd, &pt);
        OffsetRect(&client_rc, pt.x, pt.y);

        RECT src_rc = { 0, 0, width, height };
        hr = g_pdds_primary->lpVtbl->Blt(g_pdds_primary, &client_rc, g_pdds_back, &src_rc, DDBLT_WAIT, NULL);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            return 0;

        case WM_KEYDOWN:
            switch (wParam) {
                case VK_ESCAPE:
                case 'Q':
                    PostQuitMessage(0);
                    return 0;

                case 'F':
                case VK_RETURN:
                    g_fullscreen = !g_fullscreen;
                    update_window_layout();
                    init_ddraw(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '1':
                    g_target_w = 640;
                    g_target_h = 480;
                    update_window_layout();
                    init_ddraw(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '2':
                    g_target_w = 800;
                    g_target_h = 600;
                    update_window_layout();
                    init_ddraw(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '3':
                    g_target_w = 1024;
                    g_target_h = 768;
                    update_window_layout();
                    init_ddraw(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case 'P':
                    g_palette_cycling = !g_palette_cycling;
                    return 0;
            }
            break;

        case WM_ACTIVATEAPP:
            if (wParam) {
                restore_surfaces_if_lost();
            }
            return 0;

        case WM_DESTROY:
            cleanup_ddraw();
            PostQuitMessage(0);
            return 0;

        case WM_SETCURSOR:
            if (g_fullscreen) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            g_fullscreen = 1;
        } else if (strcmp(argv[i], "--windowed") == 0 || strcmp(argv[i], "-w") == 0) {
            g_fullscreen = 0;
        } else if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &g_target_w, &g_target_h);
        } else if (strcmp(argv[i], "--bpp") == 0 && i + 1 < argc) {
            g_target_bpp = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--auto") == 0 && i + 1 < argc) {
            g_auto_seconds = atoi(argv[++i]);
        }
    }

    printf("====================================================\n");
    printf(" xpdash DirectDraw 7 Test Application\n");
    printf(" Settings: %dx%d @ %dbpp, Fullscreen=%d, Auto=%ds\n",
           g_target_w, g_target_h, g_target_bpp, g_fullscreen, g_auto_seconds);
    printf("====================================================\n");

    HINSTANCE hinst = GetModuleHandle(NULL);
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassEx(&wc);

    DWORD style = g_fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    g_hwnd = CreateWindowExA(
        0, WINDOW_CLASS_NAME, "xpdash DirectDraw 7 Test",
        style, CW_USEDEFAULT, CW_USEDEFAULT, g_target_w, g_target_h,
        NULL, NULL, hinst, NULL
    );

    if (!g_hwnd) {
        printf("[-] CreateWindowExA failed! err=%lu\n", GetLastError());
        return 1;
    }

    update_window_layout();

    if (!init_ddraw(g_target_w, g_target_h, g_target_bpp, g_fullscreen)) {
        printf("[-] init_ddraw failed!\n");
        cleanup_ddraw();
        return 1;
    }

    timeBeginPeriod(1);
    g_start_time = timeGetTime();
    g_last_fps_time = g_start_time;
    DWORD last_tick = g_start_time;

    MSG msg;
    int running = 1;

    while (running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        DWORD now = timeGetTime();
        float dt = (float)(now - last_tick) / 1000.0f;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_tick = now;

        if (g_auto_seconds > 0) {
            if ((now - g_start_time) >= (DWORD)(g_auto_seconds * 1000)) {
                printf("[test-ddraw] Auto-exit duration (%ds) elapsed. Exiting cleanly.\n", g_auto_seconds);
                running = 0;
                break;
            }
        }

        g_frames++;
        g_total_frames++;
        if (now - g_last_fps_time >= 500) {
            g_fps = (float)g_frames * 1000.0f / (float)(now - g_last_fps_time);
            g_frames = 0;
            g_last_fps_time = now;
        }

        render_frame(dt);
        Sleep(12); /* ~75-85 FPS target for retro CRT testing */
    }

    timeEndPeriod(1);
    cleanup_ddraw();
    printf("[test-ddraw] Test complete. Total frames rendered: %lu\n", g_total_frames);
    return 0;
}
