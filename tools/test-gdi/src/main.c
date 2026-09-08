/*
 * tools/test-gdi/src/main.c — Win32 GDI Graphics & Display Configuration Test
 *
 * Tests Windows XP GDI rendering, double-buffering, color depths (8-bit paletted,
 * 16-bit high color, 32-bit true color), resolution switches, and palette animation.
 *
 * Key controls:
 *   F / Enter : Toggle Fullscreen / Windowed
 *   1         : Switch display to 640x480
 *   2         : Switch display to 800x600
 *   3         : Switch display to 1024x768
 *   4         : Switch display to 1280x1024
 *   0         : Restore original desktop mode
 *   P         : Toggle 8-bit palette cycling
 *   Esc / Q   : Exit
 *
 * Command-line options:
 *   --fullscreen      Start in fullscreen mode
 *   --windowed        Start in windowed mode (default)
 *   --res WxH         Start with specified resolution (e.g. 800x600)
 *   --bpp N           Request color depth (8, 16, 32)
 *   --auto SECONDS    Run automatically for N seconds and exit (for headless CI)
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#define WINDOW_CLASS_NAME "XpDash_TestGDI"
#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600

static HWND g_hwnd = NULL;
static int g_fullscreen = 0;
static int g_target_w = DEFAULT_WIDTH;
static int g_target_h = DEFAULT_HEIGHT;
static int g_target_bpp = 0;
static int g_auto_seconds = 0;
static DWORD g_start_time = 0;

static DEVMODE g_orig_devmode;
static int g_devmode_changed = 0;

/* Offscreen backbuffer for double-buffering */
static HDC g_mem_dc = NULL;
static HBITMAP g_mem_bm = NULL;
static HBITMAP g_old_bm = NULL;
static int g_backbuf_w = 0;
static int g_backbuf_h = 0;

/* Palette animation state (for 8-bit mode) */
static HPALETTE g_hpal = NULL;
static int g_palette_cycling = 1;
static UINT g_palette_shift = 0;
static PALETTEENTRY g_pal_entries[256];

/* Bouncing box animation */
static float g_box_x = 100.0f;
static float g_box_y = 100.0f;
static float g_box_vx = 220.0f;
static float g_box_vy = 160.0f;
static int g_bounce_count = 0;

/* Telemetry */
static DWORD g_last_fps_time = 0;
static int g_frames = 0;
static float g_fps = 0.0f;
static DWORD g_total_frames = 0;

static void cleanup_backbuffer(void) {
    if (g_mem_dc) {
        if (g_old_bm) {
            SelectObject(g_mem_dc, g_old_bm);
            g_old_bm = NULL;
        }
        if (g_mem_bm) {
            DeleteObject(g_mem_bm);
            g_mem_bm = NULL;
        }
        DeleteDC(g_mem_dc);
        g_mem_dc = NULL;
    }
    g_backbuf_w = 0;
    g_backbuf_h = 0;
}

static void ensure_backbuffer(HDC hdc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_mem_dc && g_mem_bm && g_backbuf_w == w && g_backbuf_h == h) {
        return;
    }
    cleanup_backbuffer();

    g_mem_dc = CreateCompatibleDC(hdc);
    g_mem_bm = CreateCompatibleBitmap(hdc, w, h);
    g_old_bm = (HBITMAP)SelectObject(g_mem_dc, g_mem_bm);
    g_backbuf_w = w;
    g_backbuf_h = h;
}

static void setup_palette(HDC hdc) {
    int raster_caps = GetDeviceCaps(hdc, RASTERCAPS);
    int bpp = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);

    if (bpp == 8 || (raster_caps & RC_PALETTE)) {
        struct {
            WORD palVersion;
            WORD palNumEntries;
            PALETTEENTRY palPalEntry[256];
        } logpal;

        logpal.palVersion = 0x300;
        logpal.palNumEntries = 256;

        /* Standard 16 system colors in 0..15 and 240..255 */
        GetSystemPaletteEntries(hdc, 0, 256, logpal.palPalEntry);

        /* Fill entries 16..239 with an animated rainbow color ramp */
        for (int i = 16; i < 240; i++) {
            float hue = (float)(i - 16) / 224.0f * 6.2831853f;
            uint8_t r = (uint8_t)(127.5f * (1.0f + sinf(hue)));
            uint8_t g = (uint8_t)(127.5f * (1.0f + sinf(hue + 2.0943951f)));
            uint8_t b = (uint8_t)(127.5f * (1.0f + sinf(hue + 4.1887902f)));
            logpal.palPalEntry[i].peRed = r;
            logpal.palPalEntry[i].peGreen = g;
            logpal.palPalEntry[i].peBlue = b;
            logpal.palPalEntry[i].peFlags = PC_RESERVED; /* Required for AnimatePalette */
        }

        memcpy(g_pal_entries, logpal.palPalEntry, sizeof(g_pal_entries));

        if (g_hpal) {
            DeleteObject(g_hpal);
            g_hpal = NULL;
        }
        g_hpal = CreatePalette((LOGPALETTE *)&logpal);
    }
}

static void update_palette_animation(HDC hdc) {
    if (!g_hpal || !g_palette_cycling) return;

    g_palette_shift = (g_palette_shift + 2) % 224;

    PALETTEENTRY anim_entries[224];
    for (int i = 0; i < 224; i++) {
        int src_idx = 16 + ((i + g_palette_shift) % 224);
        anim_entries[i] = g_pal_entries[src_idx];
    }

    AnimatePalette(g_hpal, 16, 224, anim_entries);
    RealizePalette(hdc);
}

static int set_display_mode(int w, int h, int bpp) {
    DEVMODE dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

    if (bpp > 0) {
        dm.dmBitsPerPel = bpp;
        dm.dmFields |= DM_BITSPERPEL;
    }

    LONG res = ChangeDisplaySettings(&dm, CDS_FULLSCREEN);
    if (res == DISP_CHANGE_SUCCESSFUL) {
        g_devmode_changed = 1;
        printf("[test-gdi] Changed display mode to %dx%d @ %dbpp\n", w, h, bpp);
        return 1;
    } else {
        printf("[test-gdi] ChangeDisplaySettings failed: %ld\n", res);
        return 0;
    }
}

static void restore_display_mode(void) {
    if (g_devmode_changed) {
        ChangeDisplaySettings(NULL, 0);
        g_devmode_changed = 0;
        printf("[test-gdi] Restored original display mode (%lux%lu @ %lubpp)\n",
               g_orig_devmode.dmPelsWidth, g_orig_devmode.dmPelsHeight, g_orig_devmode.dmBitsPerPel);
    }
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

static void render_scene(HDC hdc, int width, int height, float dt) {
    /* Update bouncing box */
    g_box_x += g_box_vx * dt;
    g_box_y += g_box_vy * dt;

    float box_w = 90.0f;
    float box_h = 60.0f;

    if (g_box_x < 0.0f) {
        g_box_x = 0.0f;
        g_box_vx = -g_box_vx;
        g_bounce_count++;
    } else if (g_box_x + box_w > (float)width) {
        g_box_x = (float)width - box_w;
        g_box_vx = -g_box_vx;
        g_bounce_count++;
    }

    if (g_box_y < 0.0f) {
        g_box_y = 0.0f;
        g_box_vy = -g_box_vy;
        g_bounce_count++;
    } else if (g_box_y + box_h > (float)height) {
        g_box_y = (float)height - box_h;
        g_box_vy = -g_box_vy;
        g_bounce_count++;
    }

    /* 1. Dark charcoal background */
    HBRUSH bg_brush = CreateSolidBrush(RGB(24, 28, 36));
    RECT client_rc = { 0, 0, width, height };
    FillRect(hdc, &client_rc, bg_brush);
    DeleteObject(bg_brush);

    /* 2. SMPTE-style Color Bars across top (height = 60px) */
    COLORREF bars[] = {
        RGB(255, 255, 255), // White
        RGB(255, 255, 0),   // Yellow
        RGB(0, 255, 255),   // Cyan
        RGB(0, 255, 0),     // Green
        RGB(255, 0, 255),   // Magenta
        RGB(255, 0, 0),     // Red
        RGB(0, 0, 255),     // Blue
        RGB(0, 0, 0)        // Black
    };
    int bar_count = sizeof(bars) / sizeof(bars[0]);
    int bar_w = width / bar_count;
    for (int i = 0; i < bar_count; i++) {
        HBRUSH b = CreateSolidBrush(bars[i]);
        RECT brc = { i * bar_w, 0, (i == bar_count - 1) ? width : (i + 1) * bar_w, 60 };
        FillRect(hdc, &brc, b);
        DeleteObject(b);
    }

    /* 3. Smooth Color Ramps (R, G, B, Grayscale) for testing color depth */
    int ramp_y = 70;
    int ramp_h = 16;
    int ramp_gap = 4;
    int ramp_steps = 64;
    int step_w = width / ramp_steps;
    if (step_w < 1) step_w = 1;

    for (int s = 0; s < ramp_steps; s++) {
        int v = (s * 255) / (ramp_steps - 1);
        int x0 = s * step_w;
        int x1 = (s == ramp_steps - 1) ? width : (s + 1) * step_w;

        /* Red ramp */
        HBRUSH br_r = CreateSolidBrush(RGB(v, 0, 0));
        RECT r_rc = { x0, ramp_y, x1, ramp_y + ramp_h };
        FillRect(hdc, &r_rc, br_r);
        DeleteObject(br_r);

        /* Green ramp */
        HBRUSH br_g = CreateSolidBrush(RGB(0, v, 0));
        RECT g_rc = { x0, ramp_y + ramp_h + ramp_gap, x1, ramp_y + 2 * ramp_h + ramp_gap };
        FillRect(hdc, &g_rc, br_g);
        DeleteObject(br_g);

        /* Blue ramp */
        HBRUSH br_b = CreateSolidBrush(RGB(0, 0, v));
        RECT b_rc = { x0, ramp_y + 2 * (ramp_h + ramp_gap), x1, ramp_y + 3 * ramp_h + 2 * ramp_gap };
        FillRect(hdc, &b_rc, br_b);
        DeleteObject(br_b);

        /* Gray ramp */
        HBRUSH br_gray = CreateSolidBrush(RGB(v, v, v));
        RECT gray_rc = { x0, ramp_y + 3 * (ramp_h + ramp_gap), x1, ramp_y + 4 * ramp_h + 3 * ramp_gap };
        FillRect(hdc, &gray_rc, br_gray);
        DeleteObject(br_gray);
    }

    /* 4. Concentric circles and grid in the center */
    int center_x = width / 2;
    int center_y = height / 2 + 50;
    HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(50, 60, 75));
    HPEN old_pen = (HPEN)SelectObject(hdc, grid_pen);
    HBRUSH null_brush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH old_brush = (HBRUSH)SelectObject(hdc, null_brush);

    /* Grid lines */
    MoveToEx(hdc, 0, center_y, NULL);
    LineTo(hdc, width, center_y);
    MoveToEx(hdc, center_x, 150, NULL);
    LineTo(hdc, center_x, height);

    /* Circles */
    for (int r = 30; r <= 150; r += 30) {
        Ellipse(hdc, center_x - r, center_y - r, center_x + r, center_y + r);
    }

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(grid_pen);

    /* 5. Animated Bouncing DVD-style Box */
    HBRUSH box_brush = CreateSolidBrush(RGB(0, 200, 255));
    HPEN box_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(hdc, box_brush);
    SelectObject(hdc, box_pen);
    RoundRect(hdc, (int)g_box_x, (int)g_box_y,
              (int)(g_box_x + box_w), (int)(g_box_y + box_h), 12, 12);
    DeleteObject(box_brush);
    DeleteObject(box_pen);

    /* Text inside bouncing box */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    RECT box_text_rc = { (int)g_box_x, (int)g_box_y, (int)(g_box_x + box_w), (int)(g_box_y + box_h) };
    DrawTextA(hdc, "xpdash\nDVD", -1, &box_text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* 6. On-screen Telemetry & Diagnostics */
    int cur_bpp = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
    int raster_caps = GetDeviceCaps(hdc, RASTERCAPS);
    int is_paletted = (cur_bpp == 8) || (raster_caps & RC_PALETTE);

    char buf[256];
    SetBkMode(hdc, TRANSPARENT);

    /* Header Banner */
    SetTextColor(hdc, RGB(0, 255, 128));
    TextOutA(hdc, 20, 160, "=== xpdash GDI Test Application ===", 35);

    /* Mode details */
    SetTextColor(hdc, RGB(220, 230, 245));
    snprintf(buf, sizeof(buf), "API: Win32 GDI (Double-Buffered Memory DC)");
    TextOutA(hdc, 20, 185, buf, strlen(buf));

    snprintf(buf, sizeof(buf), "Display: %dx%d @ %d bpp (Paletted: %s)",
             width, height, cur_bpp, is_paletted ? "YES" : "NO");
    TextOutA(hdc, 20, 205, buf, strlen(buf));

    snprintf(buf, sizeof(buf), "Mode: %s | Palette Cycling: %s",
             g_fullscreen ? "FULLSCREEN (Exclusive/Popup)" : "WINDOWED",
             (is_paletted && g_palette_cycling) ? "ACTIVE" : "OFF");
    TextOutA(hdc, 20, 225, buf, strlen(buf));

    /* Performance Stats */
    SetTextColor(hdc, RGB(255, 215, 0));
    snprintf(buf, sizeof(buf), "FPS: %.1f | Frame: %lu | Bounces: %d",
             g_fps, g_total_frames, g_bounce_count);
    TextOutA(hdc, 20, 250, buf, strlen(buf));

    if (g_auto_seconds > 0) {
        DWORD elapsed = (timeGetTime() - g_start_time) / 1000;
        int remaining = g_auto_seconds - (int)elapsed;
        if (remaining < 0) remaining = 0;
        snprintf(buf, sizeof(buf), "Auto-Exit in %d seconds...", remaining);
        SetTextColor(hdc, RGB(255, 100, 100));
        TextOutA(hdc, 20, 275, buf, strlen(buf));
    }

    /* Controls Legend */
    SetTextColor(hdc, RGB(160, 175, 195));
    int legend_y = height - 90;
    if (legend_y < 310) legend_y = 310;

    TextOutA(hdc, 20, legend_y,      "[F / Enter] Toggle Fullscreen      [1] 640x480   [2] 800x600", 61);
    TextOutA(hdc, 20, legend_y + 20, "[3] 1024x768   [4] 1280x1024       [0] Restore Desktop", 54);
    TextOutA(hdc, 20, legend_y + 40, "[P] Toggle Palette Cycling         [Esc / Q] Exit", 49);
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
                    return 0;

                case '1':
                    set_display_mode(640, 480, g_target_bpp);
                    g_target_w = 640;
                    g_target_h = 480;
                    update_window_layout();
                    return 0;

                case '2':
                    set_display_mode(800, 600, g_target_bpp);
                    g_target_w = 800;
                    g_target_h = 600;
                    update_window_layout();
                    return 0;

                case '3':
                    set_display_mode(1024, 768, g_target_bpp);
                    g_target_w = 1024;
                    g_target_h = 768;
                    update_window_layout();
                    return 0;

                case '4':
                    set_display_mode(1280, 1024, g_target_bpp);
                    g_target_w = 1280;
                    g_target_h = 1024;
                    update_window_layout();
                    return 0;

                case '0':
                    restore_display_mode();
                    g_target_w = DEFAULT_WIDTH;
                    g_target_h = DEFAULT_HEIGHT;
                    update_window_layout();
                    return 0;

                case 'P':
                    g_palette_cycling = !g_palette_cycling;
                    return 0;
            }
            break;

        case WM_DISPLAYCHANGE: {
            int new_bpp = (int)wParam;
            int new_w = LOWORD(lParam);
            int new_h = HIWORD(lParam);
            printf("[test-gdi] WM_DISPLAYCHANGE received: %dx%d @ %dbpp\n", new_w, new_h, new_bpp);
            HDC hdc = GetDC(hwnd);
            setup_palette(hdc);
            ReleaseDC(hwnd, hdc);
            cleanup_backbuffer();
            update_window_layout();
            return 0;
        }

        case WM_DESTROY:
            cleanup_backbuffer();
            restore_display_mode();
            if (g_hpal) {
                DeleteObject(g_hpal);
                g_hpal = NULL;
            }
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1; // Prevent flicker in double buffering
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char **argv) {
    /* Save original desktop mode */
    memset(&g_orig_devmode, 0, sizeof(g_orig_devmode));
    g_orig_devmode.dmSize = sizeof(g_orig_devmode);
    EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &g_orig_devmode);

    /* Parse command-line args */
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
    printf(" xpdash GDI Graphics & Display Test App\n");
    printf(" Initial settings: %dx%d, Fullscreen=%d, BPP=%d, Auto=%ds\n",
           g_target_w, g_target_h, g_fullscreen, g_target_bpp, g_auto_seconds);
    printf("====================================================\n");

    /* Apply target display mode if specified */
    if (g_target_bpp > 0 || (g_fullscreen && (g_target_w != (int)g_orig_devmode.dmPelsWidth || g_target_h != (int)g_orig_devmode.dmPelsHeight))) {
        set_display_mode(g_target_w, g_target_h, g_target_bpp);
    }

    /* Register Window Class */
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

    /* Create Window */
    DWORD style = g_fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    g_hwnd = CreateWindowExA(
        0, WINDOW_CLASS_NAME, "xpdash GDI Graphics & Display Test",
        style, CW_USEDEFAULT, CW_USEDEFAULT, g_target_w, g_target_h,
        NULL, NULL, hinst, NULL
    );

    if (!g_hwnd) {
        printf("[-] CreateWindowExA failed! err=%lu\n", GetLastError());
        restore_display_mode();
        return 1;
    }

    update_window_layout();

    HDC hdc_win = GetDC(g_hwnd);
    setup_palette(hdc_win);
    ReleaseDC(g_hwnd, hdc_win);

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

        /* Check auto-exit condition */
        if (g_auto_seconds > 0) {
            if ((now - g_start_time) >= (DWORD)(g_auto_seconds * 1000)) {
                printf("[test-gdi] Auto-exit duration (%ds) elapsed. Exiting cleanly.\n", g_auto_seconds);
                running = 0;
                break;
            }
        }

        /* Update FPS */
        g_frames++;
        g_total_frames++;
        if (now - g_last_fps_time >= 500) {
            g_fps = (float)g_frames * 1000.0f / (float)(now - g_last_fps_time);
            g_frames = 0;
            g_last_fps_time = now;
        }

        /* Render Frame */
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        int client_w = rc.right - rc.left;
        int client_h = rc.bottom - rc.top;

        if (client_w > 0 && client_h > 0) {
            HDC hdc = GetDC(g_hwnd);
            ensure_backbuffer(hdc, client_w, client_h);

            if (g_hpal) {
                SelectPalette(g_mem_dc, g_hpal, FALSE);
                RealizePalette(g_mem_dc);
                update_palette_animation(g_mem_dc);
            }

            render_scene(g_mem_dc, client_w, client_h, dt);

            BitBlt(hdc, 0, 0, client_w, client_h, g_mem_dc, 0, 0, SRCCOPY);
            ReleaseDC(g_hwnd, hdc);
        }

        Sleep(15); /* Target ~60 FPS */
    }

    timeEndPeriod(1);
    restore_display_mode();
    printf("[test-gdi] Test complete. Total frames rendered: %lu\n", g_total_frames);
    return 0;
}
