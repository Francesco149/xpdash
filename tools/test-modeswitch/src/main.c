/*
 * tools/test-modeswitch/src/main.c — Windows XP Display Mode & Configuration Matrix Test
 *
 * Programmatically tests display resolution, color depth (8-bit, 16-bit, 32-bit),
 * and refresh rate transitions on Windows XP to verify agent resilience, WM_DISPLAYCHANGE
 * message handling, video buffer reallocation, and streaming client adaptation.
 *
 * Usage:
 *   test-modeswitch.exe --matrix [--sec N]   Run automated display mode matrix
 *   test-modeswitch.exe --list               List all supported display modes
 *   test-modeswitch.exe                      Interactive mode switching
 *
 * Interactive Controls:
 *   1 : 640x480 @ 8 bpp       5 : 800x600 @ 32 bpp
 *   2 : 640x480 @ 16 bpp      6 : 1024x768 @ 32 bpp
 *   3 : 640x480 @ 32 bpp      7 : 1280x1024 @ 32 bpp
 *   4 : 800x600 @ 16 bpp      8 : 1920x1080 @ 32 bpp
 *   Space : Next matrix mode  0 : Restore desktop
 *   Esc / Q : Exit cleanly
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <mmsystem.h>

#define WINDOW_CLASS_NAME "XpDash_TestModeSwitch"

typedef struct {
    int width;
    int height;
    int bpp;
    const char *label;
} ModeConfig;

static const ModeConfig g_matrix[] = {
    {  640,  480,  8, "VGA 640x480 @ 8-bit Paletted (Retro DOS/Win95)" },
    {  640,  480, 16, "VGA 640x480 @ 16-bit High Color (DirectDraw 565)" },
    {  640,  480, 32, "VGA 640x480 @ 32-bit True Color" },
    {  800,  600,  8, "SVGA 800x600 @ 8-bit Paletted (StarCraft/Diablo II)" },
    {  800,  600, 16, "SVGA 800x600 @ 16-bit High Color (Unreal/Deus Ex)" },
    {  800,  600, 32, "SVGA 800x600 @ 32-bit True Color (Standard Lab Mode)" },
    { 1024,  768, 16, "XGA 1024x768 @ 16-bit High Color" },
    { 1024,  768, 32, "XGA 1024x768 @ 32-bit True Color (Classic XP 4:3)" },
    { 1280, 1024, 32, "SXGA 1280x1024 @ 32-bit True Color (5:4 LCD)" },
    { 1280,  720, 32, "HD 1280x720 @ 32-bit True Color (16:9 Widescreen)" },
    { 1920, 1080, 32, "FHD 1920x1080 @ 32-bit True Color (1080p Native)" },
};

static const int g_matrix_count = sizeof(g_matrix) / sizeof(g_matrix[0]);

static HWND g_hwnd = NULL;
static DEVMODE g_orig_devmode;
static int g_devmode_changed = 0;

static int g_matrix_mode = 0;
static int g_current_mode_idx = 0;
static int g_seconds_per_mode = 3;
static DWORD g_mode_start_time = 0;

/* Active Mode Metrics */
static int g_cur_w = 800;
static int g_cur_h = 600;
static int g_cur_bpp = 32;
static const char *g_cur_label = "";

/* Animation */
static float g_box_x = 50.0f;
static float g_box_y = 50.0f;
static float g_box_vx = 260.0f;
static float g_box_vy = 200.0f;

static DWORD g_last_fps_time = 0;
static int g_frames = 0;
static float g_fps = 0.0f;
static DWORD g_mode_frames = 0;
static DWORD g_total_frames = 0;

static void restore_display_mode(void) {
    if (g_devmode_changed) {
        ChangeDisplaySettings(NULL, 0);
        g_devmode_changed = 0;
        printf("[test-modeswitch] Restored desktop display mode (%lux%lu @ %lubpp)\n",
               g_orig_devmode.dmPelsWidth, g_orig_devmode.dmPelsHeight, g_orig_devmode.dmBitsPerPel);
    }
}

static int apply_mode(int w, int h, int bpp, const char *label) {
    DEVMODE dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

    /* Test if supported first */
    LONG test_res = ChangeDisplaySettings(&dm, CDS_TEST);
    if (test_res != DISP_CHANGE_SUCCESSFUL) {
        printf("[SKIP] Mode %dx%d @ %dbpp not supported by display driver (code %ld)\n", w, h, bpp, test_res);
        return 0;
    }

    LONG res = ChangeDisplaySettings(&dm, CDS_FULLSCREEN);
    if (res != DISP_CHANGE_SUCCESSFUL) {
        printf("[FAIL] Failed to apply mode %dx%d @ %dbpp (code %ld)\n", w, h, bpp, res);
        return 0;
    }

    g_devmode_changed = 1;
    g_cur_w = w;
    g_cur_h = h;
    g_cur_bpp = bpp;
    g_cur_label = label;
    g_mode_start_time = timeGetTime();
    g_mode_frames = 0;

    if (g_hwnd) {
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    printf("[APPLY] Mode %dx%d @ %dbpp applied successfully (%s)\n", w, h, bpp, label);
    return 1;
}

static void list_display_modes(void) {
    printf("=== Supported Display Modes for Primary Adapter ===\n");
    DEVMODE dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);

    int idx = 0;
    int prev_w = 0, prev_h = 0, prev_bpp = 0;
    int count = 0;

    while (EnumDisplaySettings(NULL, idx++, &dm)) {
        if ((int)dm.dmPelsWidth != prev_w || (int)dm.dmPelsHeight != prev_h || (int)dm.dmBitsPerPel != prev_bpp) {
            printf("  [%3d] %4lux%4lu @ %2lubpp (%3luHz)\n",
                   count + 1, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
            prev_w = (int)dm.dmPelsWidth;
            prev_h = (int)dm.dmPelsHeight;
            prev_bpp = (int)dm.dmBitsPerPel;
            count++;
        }
    }
    printf("Total unique resolution/bpp modes: %d\n", count);
}

static void render_frame(HDC hdc, int w, int h, float dt) {
    /* Update animation */
    g_box_x += g_box_vx * dt;
    g_box_y += g_box_vy * dt;
    float box_w = 120.0f;
    float box_h = 60.0f;

    if (g_box_x < 0.0f) { g_box_x = 0.0f; g_box_vx = -g_box_vx; }
    else if (g_box_x + box_w > (float)w) { g_box_x = (float)w - box_w; g_box_vx = -g_box_vx; }

    if (g_box_y < 70.0f) { g_box_y = 70.0f; g_box_vy = -g_box_vy; }
    else if (g_box_y + box_h > (float)h) { g_box_y = (float)h - box_h; g_box_vy = -g_box_vy; }

    /* 1. Background fill */
    HBRUSH bg_br = CreateSolidBrush(RGB(18, 22, 30));
    RECT rc = { 0, 0, w, h };
    FillRect(hdc, &rc, bg_br);
    DeleteObject(bg_br);

    /* 2. SMPTE Color Bars across top */
    COLORREF bars[8] = {
        RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
        RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0)
    };
    int bar_w = w / 8;
    for (int i = 0; i < 8; i++) {
        HBRUSH b = CreateSolidBrush(bars[i]);
        RECT brc = { i * bar_w, 0, (i == 7) ? w : (i + 1) * bar_w, 50 };
        FillRect(hdc, &brc, b);
        DeleteObject(b);
    }

    /* 3. Concentric circles */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 75, 95));
    HPEN old_pen = (HPEN)SelectObject(hdc, pen);
    HBRUSH null_br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH old_br = (HBRUSH)SelectObject(hdc, null_br);

    int cx = w / 2;
    int cy = h / 2 + 30;
    for (int r = 40; r <= 160; r += 40) {
        Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    }
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);

    /* 4. Animated Box */
    HBRUSH box_br = CreateSolidBrush(RGB(255, 140, 0));
    HPEN box_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(hdc, box_br);
    SelectObject(hdc, box_pen);
    RoundRect(hdc, (int)g_box_x, (int)g_box_y, (int)(g_box_x + box_w), (int)(g_box_y + box_h), 10, 10);
    DeleteObject(box_br);
    DeleteObject(box_pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    RECT box_rc = { (int)g_box_x, (int)g_box_y, (int)(g_box_x + box_w), (int)(g_box_y + box_h) };
    DrawTextA(hdc, "MODE TEST", -1, &box_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* 5. Telemetry & Diagnostics */
    char buf[256];

    SetTextColor(hdc, RGB(0, 255, 128));
    TextOutA(hdc, 20, 70, "=== xpdash Display Mode Matrix Test ===", 39);

    SetTextColor(hdc, RGB(255, 255, 255));
    if (g_matrix_mode) {
        snprintf(buf, sizeof(buf), "Test %d / %d: %dx%d @ %d bpp",
                 g_current_mode_idx + 1, g_matrix_count, g_cur_w, g_cur_h, g_cur_bpp);
    } else {
        snprintf(buf, sizeof(buf), "Current Mode: %dx%d @ %d bpp (Interactive)",
                 g_cur_w, g_cur_h, g_cur_bpp);
    }
    TextOutA(hdc, 20, 95, buf, strlen(buf));

    SetTextColor(hdc, RGB(180, 210, 240));
    TextOutA(hdc, 20, 115, g_cur_label, strlen(g_cur_label));

    SetTextColor(hdc, RGB(255, 215, 0));
    snprintf(buf, sizeof(buf), "FPS: %.1f | Frame: %lu | Mode Frames: %lu",
             g_fps, g_total_frames, g_mode_frames);
    TextOutA(hdc, 20, 140, buf, strlen(buf));

    if (g_matrix_mode) {
        DWORD elapsed = (timeGetTime() - g_mode_start_time) / 1000;
        int remaining = g_seconds_per_mode - (int)elapsed;
        if (remaining < 0) remaining = 0;
        SetTextColor(hdc, RGB(255, 120, 120));
        snprintf(buf, sizeof(buf), "Next mode in %d seconds...", remaining);
        TextOutA(hdc, 20, 165, buf, strlen(buf));
    }

    SetTextColor(hdc, RGB(150, 170, 190));
    int leg_y = h - 60;
    if (leg_y < 195) leg_y = 195;
    TextOutA(hdc, 20, leg_y,      "[1] 640x480@8   [2] 640x480@16   [3] 640x480@32   [4] 800x600@16", 65);
    TextOutA(hdc, 20, leg_y + 20, "[5] 800x600@32  [6] 1024x768@32  [0] Restore      [Esc/Q] Exit", 63);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_ESCAPE:
                case 'Q':
                    PostQuitMessage(0);
                    return 0;

                case '1':
                    apply_mode(640, 480, 8, "VGA 640x480 @ 8-bit");
                    return 0;

                case '2':
                    apply_mode(640, 480, 16, "VGA 640x480 @ 16-bit");
                    return 0;

                case '3':
                    apply_mode(640, 480, 32, "VGA 640x480 @ 32-bit");
                    return 0;

                case '4':
                    apply_mode(800, 600, 16, "SVGA 800x600 @ 16-bit");
                    return 0;

                case '5':
                    apply_mode(800, 600, 32, "SVGA 800x600 @ 32-bit");
                    return 0;

                case '6':
                    apply_mode(1024, 768, 32, "XGA 1024x768 @ 32-bit");
                    return 0;

                case '7':
                    apply_mode(1280, 1024, 32, "SXGA 1280x1024 @ 32-bit");
                    return 0;

                case '8':
                    apply_mode(1920, 1080, 32, "FHD 1920x1080 @ 32-bit");
                    return 0;

                case VK_SPACE:
                    /* Advance matrix mode */
                    g_current_mode_idx = (g_current_mode_idx + 1) % g_matrix_count;
                    apply_mode(g_matrix[g_current_mode_idx].width,
                               g_matrix[g_current_mode_idx].height,
                               g_matrix[g_current_mode_idx].bpp,
                               g_matrix[g_current_mode_idx].label);
                    return 0;

                case '0':
                    restore_display_mode();
                    g_cur_w = (int)g_orig_devmode.dmPelsWidth;
                    g_cur_h = (int)g_orig_devmode.dmPelsHeight;
                    g_cur_bpp = (int)g_orig_devmode.dmBitsPerPel;
                    g_cur_label = "Restored Desktop Mode";
                    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, g_cur_w, g_cur_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                    return 0;
            }
            break;

        case WM_DESTROY:
            restore_display_mode();
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char **argv) {
    memset(&g_orig_devmode, 0, sizeof(g_orig_devmode));
    g_orig_devmode.dmSize = sizeof(g_orig_devmode);
    EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &g_orig_devmode);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            list_display_modes();
            return 0;
        } else if (strcmp(argv[i], "--matrix") == 0 || strcmp(argv[i], "-m") == 0) {
            g_matrix_mode = 1;
        } else if (strcmp(argv[i], "--sec") == 0 && i + 1 < argc) {
            g_seconds_per_mode = atoi(argv[++i]);
            if (g_seconds_per_mode <= 0) g_seconds_per_mode = 1;
        }
    }

    printf("====================================================\n");
    printf(" xpdash Display Mode & Configuration Matrix Test\n");
    printf(" Initial Desktop: %lux%lu @ %lubpp (%luHz)\n",
           g_orig_devmode.dmPelsWidth, g_orig_devmode.dmPelsHeight,
           g_orig_devmode.dmBitsPerPel, g_orig_devmode.dmDisplayFrequency);
    printf(" Matrix Mode: %s (%d sec/mode)\n",
           g_matrix_mode ? "AUTOMATED" : "INTERACTIVE", g_seconds_per_mode);
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

    g_hwnd = CreateWindowExA(
        WS_EX_TOPMOST, WINDOW_CLASS_NAME, "xpdash Display Mode Matrix Test",
        WS_POPUP | WS_VISIBLE, 0, 0,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        NULL, NULL, hinst, NULL
    );

    if (!g_hwnd) {
        printf("[-] CreateWindowExA failed!\n");
        return 1;
    }

    /* Start with first mode */
    if (g_matrix_mode) {
        apply_mode(g_matrix[0].width, g_matrix[0].height, g_matrix[0].bpp, g_matrix[0].label);
    } else {
        g_cur_w = (int)g_orig_devmode.dmPelsWidth;
        g_cur_h = (int)g_orig_devmode.dmPelsHeight;
        g_cur_bpp = (int)g_orig_devmode.dmBitsPerPel;
        g_cur_label = "Desktop Native Mode";
    }

    timeBeginPeriod(1);
    DWORD last_tick = timeGetTime();
    g_last_fps_time = last_tick;

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

        /* Automated matrix step */
        if (g_matrix_mode) {
            if ((now - g_mode_start_time) >= (DWORD)(g_seconds_per_mode * 1000)) {
                printf("[PASS] Mode %d/%d (%s): rendered %lu frames\n",
                       g_current_mode_idx + 1, g_matrix_count, g_cur_label, g_mode_frames);

                g_current_mode_idx++;
                if (g_current_mode_idx >= g_matrix_count) {
                    printf("=== All %d Matrix Modes Tested Successfully! ===\n", g_matrix_count);
                    running = 0;
                    break;
                }

                while (g_current_mode_idx < g_matrix_count) {
                    if (apply_mode(g_matrix[g_current_mode_idx].width,
                                   g_matrix[g_current_mode_idx].height,
                                   g_matrix[g_current_mode_idx].bpp,
                                   g_matrix[g_current_mode_idx].label)) {
                        break;
                    }
                    g_current_mode_idx++;
                }
                if (g_current_mode_idx >= g_matrix_count) {
                    printf("=== Matrix Test Completed ===\n");
                    running = 0;
                    break;
                }
            }
        }

        g_frames++;
        g_total_frames++;
        g_mode_frames++;

        if (now - g_last_fps_time >= 500) {
            g_fps = (float)g_frames * 1000.0f / (float)(now - g_last_fps_time);
            g_frames = 0;
            g_last_fps_time = now;
        }

        HDC hdc = GetDC(g_hwnd);
        render_frame(hdc, g_cur_w, g_cur_h, dt);
        ReleaseDC(g_hwnd, hdc);

        Sleep(15);
    }

    timeEndPeriod(1);
    restore_display_mode();
    printf("[test-modeswitch] Test complete. Total frames rendered: %lu\n", g_total_frames);
    return 0;
}
