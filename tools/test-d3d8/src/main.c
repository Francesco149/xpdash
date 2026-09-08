/*
 * tools/test-d3d8/src/main.c — Direct3D 8 Graphics & Display Configuration Test
 *
 * Tests Windows XP Direct3D 8 hardware 3D rendering, backbuffer presentation,
 * fullscreen exclusive mode, depth buffering (Z-Buffer), 16-bit vs 32-bit color,
 * and device reset on display changes.
 *
 * 100% self-contained: implements 3D matrix math directly without any external
 * D3DX helper DLLs, ensuring zero non-stock dependencies.
 *
 * Key controls:
 *   F / Enter : Toggle Fullscreen Exclusive / Windowed
 *   1         : 640x480
 *   2         : 800x600
 *   3         : 1024x768
 *   4         : 1280x1024
 *   Esc / Q   : Exit
 *
 * Command-line options:
 *   --fullscreen      Start in fullscreen exclusive mode (default)
 *   --windowed        Start in windowed mode
 *   --res WxH         Set resolution (default 800x600)
 *   --bpp N           Set color depth (16, 32; default 32)
 *   --auto SECONDS    Run automatically for N seconds and exit cleanly
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <mmsystem.h>

#define WINDOW_CLASS_NAME "XpDash_TestD3D8"
#define DEFAULT_WIDTH  800
#define DEFAULT_HEIGHT 600

static HWND g_hwnd = NULL;
static int g_fullscreen = 1;
static int g_target_w = DEFAULT_WIDTH;
static int g_target_h = DEFAULT_HEIGHT;
static int g_target_bpp = 32;
static int g_auto_seconds = 0;
static DWORD g_start_time = 0;

/* Direct3D 8 Interfaces */
static LPDIRECT3D8       g_pD3D = NULL;
static LPDIRECT3DDEVICE8 g_pDevice = NULL;
static D3DPRESENT_PARAMETERS g_d3dpp;

/* 3D Scene State */
static float g_rot_x = 0.0f;
static float g_rot_y = 0.0f;
static float g_rot_z = 0.0f;

/* Telemetry */
static DWORD g_last_fps_time = 0;
static int g_frames = 0;
static float g_fps = 0.0f;
static DWORD g_total_frames = 0;

/* ─── 3D Math (Self-Contained 4x4 Matrix Engine) ──────────────────────── */

static void mat_identity(D3DMATRIX *m) {
    memset(m, 0, sizeof(D3DMATRIX));
    m->_11 = m->_22 = m->_33 = m->_44 = 1.0f;
}

static void mat_multiply(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b) {
    D3DMATRIX res;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            res.m[i][j] = a->m[i][0] * b->m[0][j] +
                          a->m[i][1] * b->m[1][j] +
                          a->m[i][2] * b->m[2][j] +
                          a->m[i][3] * b->m[3][j];
        }
    }
    *out = res;
}

static void mat_rotation_yaw_pitch_roll(D3DMATRIX *out, float yaw, float pitch, float roll) {
    D3DMATRIX mx, my, mz, mxy;
    mat_identity(&mx);
    mat_identity(&my);
    mat_identity(&mz);

    mx._22 = cosf(pitch);  mx._23 = sinf(pitch);
    mx._32 = -sinf(pitch); mx._33 = cosf(pitch);

    my._11 = cosf(yaw);    my._13 = -sinf(yaw);
    my._31 = sinf(yaw);    my._33 = cosf(yaw);

    mz._11 = cosf(roll);   mz._12 = sinf(roll);
    mz._21 = -sinf(roll);  mz._22 = cosf(roll);

    mat_multiply(&mxy, &mx, &my);
    mat_multiply(out, &mxy, &mz);
}

static void mat_perspective_fov_lh(D3DMATRIX *out, float fov_y, float aspect, float zn, float zf) {
    memset(out, 0, sizeof(D3DMATRIX));
    float y_scale = 1.0f / tanf(fov_y / 2.0f);
    float x_scale = y_scale / aspect;

    out->_11 = x_scale;
    out->_22 = y_scale;
    out->_33 = zf / (zf - zn);
    out->_34 = 1.0f;
    out->_43 = -zn * zf / (zf - zn);
}

static void mat_look_at_lh(D3DMATRIX *out, float eye_z) {
    mat_identity(out);
    out->_43 = eye_z; // Simple camera looking along +Z from -eye_z
}

/* ─── Vertex Definitions ──────────────────────────────────────────────── */

typedef struct {
    float x, y, z;
    DWORD color;
} CubeVertex;

#define D3DFVF_CUBE (D3DFVF_XYZ | D3DFVF_DIFFUSE)

typedef struct {
    float x, y, z, rhw;
    DWORD color;
} HudVertex;

#define D3DFVF_HUD (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* 36 vertices forming a 3D cube with different face colors */
static const CubeVertex g_cube_vertices[36] = {
    // Front face (Cyan) - Z = -1.0
    { -1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },
    { -1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },
    {  1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },
    { -1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },
    {  1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },
    {  1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 255) },

    // Back face (Red) - Z = 1.0
    {  1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },
    {  1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },
    { -1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },
    {  1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },
    { -1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },
    { -1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 0) },

    // Top face (Magenta) - Y = 1.0
    { -1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 255) },
    { -1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 255) },
    {  1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 255) },
    { -1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 255) },
    {  1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 0, 255) },
    {  1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(255, 0, 255) },

    // Bottom face (Blue) - Y = -1.0
    { -1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(0, 0, 255) },
    { -1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255) },
    {  1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255) },
    { -1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(0, 0, 255) },
    {  1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 0, 255) },
    {  1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(0, 0, 255) },

    // Left face (Green) - X = -1.0
    { -1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(0, 255, 0) },
    { -1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(0, 255, 0) },
    { -1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0) },
    { -1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(0, 255, 0) },
    { -1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0) },
    { -1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(0, 255, 0) },

    // Right face (Yellow) - X = 1.0
    {  1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 0) },
    {  1.0f,  1.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 0) },
    {  1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 255, 0) },
    {  1.0f, -1.0f, -1.0f, D3DCOLOR_XRGB(255, 255, 0) },
    {  1.0f,  1.0f,  1.0f, D3DCOLOR_XRGB(255, 255, 0) },
    {  1.0f, -1.0f,  1.0f, D3DCOLOR_XRGB(255, 255, 0) },
};

/* ─── D3D8 Lifecycle ─────────────────────────────────────────────────── */

static void cleanup_d3d8(void) {
    if (g_pDevice) {
        g_pDevice->lpVtbl->Release(g_pDevice);
        g_pDevice = NULL;
    }
    if (g_pD3D) {
        g_pD3D->lpVtbl->Release(g_pD3D);
        g_pD3D = NULL;
    }
}

static int init_d3d8(int w, int h, int bpp, int fullscreen) {
    cleanup_d3d8();

    g_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_pD3D) {
        printf("[-] Direct3DCreate8 failed!\n");
        return 0;
    }

    D3DDISPLAYMODE d3ddm;
    g_pD3D->lpVtbl->GetAdapterDisplayMode(g_pD3D, D3DADAPTER_DEFAULT, &d3ddm);

    memset(&g_d3dpp, 0, sizeof(g_d3dpp));
    g_d3dpp.Windowed = !fullscreen;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferWidth = w;
    g_d3dpp.BackBufferHeight = h;
    g_d3dpp.BackBufferFormat = fullscreen ? ((bpp == 16) ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8) : d3ddm.Format;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.hDeviceWindow = g_hwnd;
    g_d3dpp.FullScreen_RefreshRateInHz = 0; // 0 = adapter default
    g_d3dpp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    /* Try Hardware Vertex Processing first, fallback to Software */
    HRESULT hr = g_pD3D->lpVtbl->CreateDevice(
        g_pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pDevice
    );

    if (FAILED(hr)) {
        printf("[*] Hardware vertex processing unavailable, trying Software...\n");
        hr = g_pD3D->lpVtbl->CreateDevice(
            g_pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp, &g_pDevice
        );
    }

    if (FAILED(hr) || !g_pDevice) {
        printf("[-] CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        return 0;
    }

    /* Configure Initial Render States */
    g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_LIGHTING, FALSE);
    g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_CULLMODE, D3DCULL_CW);

    printf("[test-d3d8] Initialized D3D8: %dx%d @ %d-bit (%s)\n",
           w, h, (g_d3dpp.BackBufferFormat == D3DFMT_R5G6B5) ? 16 : 32,
           fullscreen ? "EXCLUSIVE FULLSCREEN" : "WINDOWED");
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

static void render_hud_bars(int width) {
    /* Draw 8 SMPTE color bars across the top using 2D screen-space transformed vertices */
    float bar_w = (float)width / 8.0f;
    float bar_h = 50.0f;

    DWORD colors[8] = {
        D3DCOLOR_XRGB(255, 255, 255), // White
        D3DCOLOR_XRGB(255, 255,   0), // Yellow
        D3DCOLOR_XRGB(  0, 255, 255), // Cyan
        D3DCOLOR_XRGB(  0, 255,   0), // Green
        D3DCOLOR_XRGB(255,   0, 255), // Magenta
        D3DCOLOR_XRGB(255,   0,   0), // Red
        D3DCOLOR_XRGB(  0,   0, 255), // Blue
        D3DCOLOR_XRGB(  0,   0,   0)  // Black
    };

    HudVertex verts[8 * 6];
    int idx = 0;

    for (int i = 0; i < 8; i++) {
        float x0 = (float)i * bar_w;
        float x1 = (i == 7) ? (float)width : (float)(i + 1) * bar_w;
        DWORD c = colors[i];

        verts[idx++] = (HudVertex){ x0,    0.0f, 0.0f, 1.0f, c };
        verts[idx++] = (HudVertex){ x1,    0.0f, 0.0f, 1.0f, c };
        verts[idx++] = (HudVertex){ x1, bar_h, 0.0f, 1.0f, c };

        verts[idx++] = (HudVertex){ x0,    0.0f, 0.0f, 1.0f, c };
        verts[idx++] = (HudVertex){ x1, bar_h, 0.0f, 1.0f, c };
        verts[idx++] = (HudVertex){ x0, bar_h, 0.0f, 1.0f, c };
    }

    g_pDevice->lpVtbl->SetVertexShader(g_pDevice, D3DFVF_HUD);
    g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->lpVtbl->DrawPrimitiveUP(g_pDevice, D3DPT_TRIANGLELIST, 16, verts, sizeof(HudVertex));
    g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_ZENABLE, D3DZB_TRUE);
}

static void render_frame(float dt) {
    if (!g_pDevice) return;

    /* Handle device lost state */
    HRESULT hr = g_pDevice->lpVtbl->TestCooperativeLevel(g_pDevice);
    if (hr == D3DERR_DEVICELOST) {
        Sleep(20);
        return;
    } else if (hr == D3DERR_DEVICENOTRESET) {
        printf("[test-d3d8] Device needs reset, resetting...\n");
        hr = g_pDevice->lpVtbl->Reset(g_pDevice, &g_d3dpp);
        if (FAILED(hr)) return;
        g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_ZENABLE, D3DZB_TRUE);
        g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_LIGHTING, FALSE);
        g_pDevice->lpVtbl->SetRenderState(g_pDevice, D3DRS_CULLMODE, D3DCULL_CW);
    }

    /* Update rotations */
    g_rot_x += dt * 0.8f;
    g_rot_y += dt * 1.2f;
    g_rot_z += dt * 0.5f;

    /* Setup 3D Matrices */
    D3DMATRIX mat_world, mat_view, mat_proj;
    mat_rotation_yaw_pitch_roll(&mat_world, g_rot_y, g_rot_x, g_rot_z);
    mat_look_at_lh(&mat_view, 3.8f);

    float aspect = (float)g_target_w / (float)g_target_h;
    mat_perspective_fov_lh(&mat_proj, 3.14159265f / 4.0f, aspect, 0.5f, 100.0f);

    g_pDevice->lpVtbl->SetTransform(g_pDevice, D3DTS_WORLD, &mat_world);
    g_pDevice->lpVtbl->SetTransform(g_pDevice, D3DTS_VIEW, &mat_view);
    g_pDevice->lpVtbl->SetTransform(g_pDevice, D3DTS_PROJECTION, &mat_proj);

    /* Clear screen and depth buffer */
    g_pDevice->lpVtbl->Clear(
        g_pDevice, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(20, 24, 32), 1.0f, 0
    );

    /* Render 3D Scene */
    if (SUCCEEDED(g_pDevice->lpVtbl->BeginScene(g_pDevice))) {
        g_pDevice->lpVtbl->SetVertexShader(g_pDevice, D3DFVF_CUBE);
        g_pDevice->lpVtbl->DrawPrimitiveUP(
            g_pDevice, D3DPT_TRIANGLELIST, 12,
            g_cube_vertices, sizeof(CubeVertex)
        );

        /* Render 2D Screen-space HUD color bars */
        render_hud_bars(g_target_w);

        g_pDevice->lpVtbl->EndScene(g_pDevice);
    }

    /* Present */
    g_pDevice->lpVtbl->Present(g_pDevice, NULL, NULL, NULL, NULL);
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
                    init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '1':
                    g_target_w = 640;
                    g_target_h = 480;
                    update_window_layout();
                    init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '2':
                    g_target_w = 800;
                    g_target_h = 600;
                    update_window_layout();
                    init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '3':
                    g_target_w = 1024;
                    g_target_h = 768;
                    update_window_layout();
                    init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;

                case '4':
                    g_target_w = 1280;
                    g_target_h = 1024;
                    update_window_layout();
                    init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen);
                    return 0;
            }
            break;

        case WM_DESTROY:
            cleanup_d3d8();
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
    printf(" xpdash Direct3D 8 Test Application\n");
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
        0, WINDOW_CLASS_NAME, "xpdash Direct3D 8 Test",
        style, CW_USEDEFAULT, CW_USEDEFAULT, g_target_w, g_target_h,
        NULL, NULL, hinst, NULL
    );

    if (!g_hwnd) {
        printf("[-] CreateWindowExA failed! err=%lu\n", GetLastError());
        return 1;
    }

    update_window_layout();

    if (!init_d3d8(g_target_w, g_target_h, g_target_bpp, g_fullscreen)) {
        printf("[-] init_d3d8 failed!\n");
        cleanup_d3d8();
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
                printf("[test-d3d8] Auto-exit duration (%ds) elapsed. Exiting cleanly.\n", g_auto_seconds);
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
                     "xpdash D3D8 Test - %s (%dx%d @ %dbpp) - %.1f FPS - Frame %lu",
                     g_fullscreen ? "FULLSCREEN" : "WINDOWED",
                     g_target_w, g_target_h,
                     (g_d3dpp.BackBufferFormat == D3DFMT_R5G6B5) ? 16 : 32,
                     g_fps, g_total_frames);
            SetWindowTextA(g_hwnd, title);
        }

        render_frame(dt);
        Sleep(10); /* ~80-100 FPS target */
    }

    timeEndPeriod(1);
    cleanup_d3d8();
    printf("[test-d3d8] Test complete. Total frames rendered: %lu\n", g_total_frames);
    return 0;
}
