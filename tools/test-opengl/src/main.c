/*
 * tools/test-opengl/src/main.c — Win32 OpenGL Graphics & Display Configuration Test
 *
 * Tests Windows XP OpenGL hardware/software acceleration, double-buffered WGL contexts,
 * fullscreen mode transitions, depth buffering, and color reproduction across different
 * display modes.
 *
 * Key controls:
 *   F / Enter : Toggle Fullscreen / Windowed
 *   1         : 640x480
 *   2         : 800x600
 *   3         : 1024x768
 *   4         : 1280x1024
 *   5         : 1280x720
 *   6         : 1920x1080
 *   Esc / Q   : Exit
 *
 * Command-line options:
 *   --fullscreen      Start in fullscreen mode
 *   --windowed        Start in windowed mode (default)
 *   --res WxH         Set resolution (default 800x600)
 *   --bpp N           Set color depth (16, 32; default 32)
 *   --auto SECONDS    Run automatically for N seconds and exit cleanly
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <mmsystem.h>

#define WINDOW_CLASS_NAME "XpDash_TestOpenGL"
#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600

static HWND   g_hwnd = NULL;
static HDC    g_hdc = NULL;
static HGLRC  g_hglrc = NULL;

static int g_fullscreen = 0;
static int g_target_w = DEFAULT_WIDTH;
static int g_target_h = DEFAULT_HEIGHT;
static int g_target_bpp = 32;
static int g_auto_seconds = 0;
static DWORD g_start_time = 0;

static DEVMODE g_orig_devmode;
static int g_devmode_changed = 0;

/* 3D Scene Animation */
static float g_rot_x = 0.0f;
static float g_rot_y = 0.0f;
static float g_rot_z = 0.0f;

/* Telemetry */
static DWORD g_last_fps_time = 0;
static int g_frames = 0;
static float g_fps = 0.0f;
static DWORD g_total_frames = 0;

/* OpenGL strings */
static const char *g_gl_vendor = "";
static const char *g_gl_renderer = "";
static const char *g_gl_version = "";

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
        printf("[test-opengl] Changed display mode to %dx%d @ %dbpp\n", w, h, bpp);
        return 1;
    } else {
        printf("[test-opengl] ChangeDisplaySettings failed: %ld\n", res);
        return 0;
    }
}

static void restore_display_mode(void) {
    if (g_devmode_changed) {
        ChangeDisplaySettings(NULL, 0);
        g_devmode_changed = 0;
        printf("[test-opengl] Restored original display mode\n");
    }
}

static void cleanup_gl(void) {
    if (g_hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hglrc);
        g_hglrc = NULL;
    }
    if (g_hdc && g_hwnd) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
    }
}

static int init_gl(void) {
    cleanup_gl();

    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) {
        printf("[-] GetDC failed!\n");
        return 0;
    }

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = g_target_bpp;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixel_format = ChoosePixelFormat(g_hdc, &pfd);
    if (!pixel_format) {
        printf("[-] ChoosePixelFormat failed!\n");
        return 0;
    }

    if (!SetPixelFormat(g_hdc, pixel_format, &pfd)) {
        printf("[-] SetPixelFormat failed!\n");
        return 0;
    }

    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc) {
        printf("[-] wglCreateContext failed!\n");
        return 0;
    }

    if (!wglMakeCurrent(g_hdc, g_hglrc)) {
        printf("[-] wglMakeCurrent failed!\n");
        return 0;
    }

    /* Query OpenGL Driver Info */
    g_gl_vendor = (const char *)glGetString(GL_VENDOR);
    g_gl_renderer = (const char *)glGetString(GL_RENDERER);
    g_gl_version = (const char *)glGetString(GL_VERSION);

    printf("[test-opengl] Context Created: %s | %s | %s\n",
           g_gl_vendor ? g_gl_vendor : "Unknown",
           g_gl_renderer ? g_gl_renderer : "Unknown",
           g_gl_version ? g_gl_version : "Unknown");

    /* Initial OpenGL States */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);
    glDisable(GL_CULL_FACE);

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

static void draw_hud_bars(int width) {
    /* 2D Orthographic overlay for SMPTE color bars */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)width, (double)g_target_h, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);

    float bar_w = (float)width / 8.0f;
    float bar_h = 50.0f;

    float colors[8][3] = {
        { 1.0f, 1.0f, 1.0f }, // White
        { 1.0f, 1.0f, 0.0f }, // Yellow
        { 0.0f, 1.0f, 1.0f }, // Cyan
        { 0.0f, 1.0f, 0.0f }, // Green
        { 1.0f, 0.0f, 1.0f }, // Magenta
        { 1.0f, 0.0f, 0.0f }, // Red
        { 0.0f, 0.0f, 1.0f }, // Blue
        { 0.0f, 0.0f, 0.0f }  // Black
    };

    glBegin(GL_QUADS);
    for (int i = 0; i < 8; i++) {
        float x0 = (float)i * bar_w;
        float x1 = (i == 7) ? (float)width : (float)(i + 1) * bar_w;

        glColor3fv(colors[i]);
        glVertex2f(x0, 0.0f);
        glVertex2f(x1, 0.0f);
        glVertex2f(x1, bar_h);
        glVertex2f(x0, bar_h);
    }
    glEnd();

    glEnable(GL_DEPTH_TEST);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

static void render_frame(float dt) {
    if (!g_hdc || !g_hglrc) return;

    g_rot_x += dt * 45.0f;
    g_rot_y += dt * 60.0f;
    g_rot_z += dt * 30.0f;

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int client_w = rc.right - rc.left;
    int client_h = rc.bottom - rc.top;
    if (client_w <= 0) client_w = 1;
    if (client_h <= 0) client_h = 1;

    glViewport(0, 0, client_w, client_h);
    glClearColor(0.08f, 0.10f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* 3D Perspective Projection */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)client_w / (double)client_h, 0.5, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* 1. Left Object: Multi-colored Rotating Pyramid */
    glPushMatrix();
    glTranslatef(-1.4f, -0.2f, -4.5f);
    glRotatef(g_rot_y, 0.0f, 1.0f, 0.0f);
    glRotatef(g_rot_x, 1.0f, 0.0f, 0.0f);

    glBegin(GL_TRIANGLES);
        // Front Face (Apex Red, Bottom Green/Blue)
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.0f,  0.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 1.0f, -1.0f,  1.0f);

        // Right Face
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.0f,  0.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 1.0f, -1.0f,  1.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);

        // Back Face
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.0f,  0.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f(-1.0f, -1.0f, -1.0f);

        // Left Face
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.0f,  0.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f(-1.0f, -1.0f, -1.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
    glEnd();
    glPopMatrix();

    /* 2. Right Object: Multi-colored Rotating Cube */
    glPushMatrix();
    glTranslatef(1.4f, -0.2f, -4.5f);
    glRotatef(g_rot_x, 1.0f, 0.0f, 0.0f);
    glRotatef(g_rot_z, 0.0f, 0.0f, 1.0f);
    glRotatef(g_rot_y, 0.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
        // Top Face (Cyan)
        glColor3f(0.0f, 1.0f, 1.0f);
        glVertex3f( 0.8f,  0.8f, -0.8f);
        glVertex3f(-0.8f,  0.8f, -0.8f);
        glVertex3f(-0.8f,  0.8f,  0.8f);
        glVertex3f( 0.8f,  0.8f,  0.8f);

        // Bottom Face (Orange)
        glColor3f(1.0f, 0.5f, 0.0f);
        glVertex3f( 0.8f, -0.8f,  0.8f);
        glVertex3f(-0.8f, -0.8f,  0.8f);
        glVertex3f(-0.8f, -0.8f, -0.8f);
        glVertex3f( 0.8f, -0.8f, -0.8f);

        // Front Face (Red)
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex3f( 0.8f,  0.8f,  0.8f);
        glVertex3f(-0.8f,  0.8f,  0.8f);
        glVertex3f(-0.8f, -0.8f,  0.8f);
        glVertex3f( 0.8f, -0.8f,  0.8f);

        // Back Face (Yellow)
        glColor3f(1.0f, 1.0f, 0.0f);
        glVertex3f( 0.8f, -0.8f, -0.8f);
        glVertex3f(-0.8f, -0.8f, -0.8f);
        glVertex3f(-0.8f,  0.8f, -0.8f);
        glVertex3f( 0.8f,  0.8f, -0.8f);

        // Left Face (Blue)
        glColor3f(0.0f, 0.0f, 1.0f);
        glVertex3f(-0.8f,  0.8f,  0.8f);
        glVertex3f(-0.8f,  0.8f, -0.8f);
        glVertex3f(-0.8f, -0.8f, -0.8f);
        glVertex3f(-0.8f, -0.8f,  0.8f);

        // Right Face (Magenta)
        glColor3f(1.0f, 0.0f, 1.0f);
        glVertex3f( 0.8f,  0.8f, -0.8f);
        glVertex3f( 0.8f,  0.8f,  0.8f);
        glVertex3f( 0.8f, -0.8f,  0.8f);
        glVertex3f( 0.8f, -0.8f, -0.8f);
    glEnd();
    glPopMatrix();

    /* 3. 2D HUD Color Bars across the top */
    draw_hud_bars(client_w);

    /* Swap Front and Back Buffers */
    SwapBuffers(g_hdc);
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
                    if (g_fullscreen) {
                        set_display_mode(g_target_w, g_target_h, g_target_bpp);
                    } else {
                        restore_display_mode();
                    }
                    update_window_layout();
                    return 0;

                case '1':
                    g_target_w = 640; g_target_h = 480;
                    if (g_fullscreen) set_display_mode(640, 480, g_target_bpp);
                    update_window_layout();
                    return 0;

                case '2':
                    g_target_w = 800; g_target_h = 600;
                    if (g_fullscreen) set_display_mode(800, 600, g_target_bpp);
                    update_window_layout();
                    return 0;

                case '3':
                    g_target_w = 1024; g_target_h = 768;
                    if (g_fullscreen) set_display_mode(1024, 768, g_target_bpp);
                    update_window_layout();
                    return 0;

                case '4':
                    g_target_w = 1280; g_target_h = 1024;
                    if (g_fullscreen) set_display_mode(1280, 1024, g_target_bpp);
                    update_window_layout();
                    return 0;

                case '5':
                    g_target_w = 1280; g_target_h = 720;
                    if (g_fullscreen) set_display_mode(1280, 720, g_target_bpp);
                    update_window_layout();
                    return 0;

                case '6':
                    g_target_w = 1920; g_target_h = 1080;
                    if (g_fullscreen) set_display_mode(1920, 1080, g_target_bpp);
                    update_window_layout();
                    return 0;
            }
            break;

        case WM_DESTROY:
            cleanup_gl();
            restore_display_mode();
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1; // Prevent flicker
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char **argv) {
    memset(&g_orig_devmode, 0, sizeof(g_orig_devmode));
    g_orig_devmode.dmSize = sizeof(g_orig_devmode);
    EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &g_orig_devmode);

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
    printf(" xpdash Win32 OpenGL Graphics & Display Test\n");
    printf(" Settings: %dx%d @ %dbpp, Fullscreen=%d, Auto=%ds\n",
           g_target_w, g_target_h, g_target_bpp, g_fullscreen, g_auto_seconds);
    printf("====================================================\n");

    if (g_fullscreen) {
        set_display_mode(g_target_w, g_target_h, g_target_bpp);
    }

    HINSTANCE hinst = GetModuleHandle(NULL);
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassEx(&wc);

    DWORD style = g_fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    g_hwnd = CreateWindowExA(
        0, WINDOW_CLASS_NAME, "xpdash OpenGL Graphics & Display Test",
        style, CW_USEDEFAULT, CW_USEDEFAULT, g_target_w, g_target_h,
        NULL, NULL, hinst, NULL
    );

    if (!g_hwnd) {
        printf("[-] CreateWindowExA failed! err=%lu\n", GetLastError());
        restore_display_mode();
        return 1;
    }

    update_window_layout();

    if (!init_gl()) {
        printf("[-] init_gl failed!\n");
        cleanup_gl();
        restore_display_mode();
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
                printf("[test-opengl] Auto-exit duration (%ds) elapsed. Exiting cleanly.\n", g_auto_seconds);
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

            char title[256];
            snprintf(title, sizeof(title),
                     "xpdash OpenGL Test - %s (%dx%d @ %dbpp) - %.1f FPS - Frame %lu",
                     g_fullscreen ? "FULLSCREEN" : "WINDOWED",
                     g_target_w, g_target_h, g_target_bpp,
                     g_fps, g_total_frames);
            SetWindowTextA(g_hwnd, title);
        }

        render_frame(dt);
        Sleep(12); /* ~75-85 FPS target */
    }

    timeEndPeriod(1);
    cleanup_gl();
    restore_display_mode();
    printf("[test-opengl] Test complete. Total frames rendered: %lu\n", g_total_frames);
    return 0;
}
