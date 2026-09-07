/* agent/src/main.c — xpdash Windows XP Native Agent */
#define _WIN32_WINNT 0x0501
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>

#include "audio.h"
#include "video.h"
#include "input.h"
#include "net.h"
#include "discover.h"

static volatile int g_running = 1;
static HWND g_hwnd = NULL;

static void on_audio_frame(const uint8_t *pcm_data, uint32_t size, uint32_t pts_ms, void *user_data) {
    (void)user_data;
    net_send_audio(pcm_data, size, pts_ms);
}

static void on_video_frame(const VideoFrame *frame, void *user_data) {
    (void)user_data;
    if (frame && frame->pixels) {
        // Send frame chunk
        net_send_video_chunk(frame->pixels, (frame->size_bytes > 1200) ? 1200 : frame->size_bytes, frame->pts_ms);
    }
}

static void on_server_discovered(const DiscoveredServer *server, void *user_data) {
    (void)user_data;
    if (server && discover_is_trusted(server->fingerprint)) {
        net_set_media_destination(server->ip, server->media_port);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DISPLAYCHANGE: {
            int new_w = LOWORD(lParam);
            int new_h = HIWORD(lParam);
            video_resize(new_w, new_h);
            return 0;
        }
        case WM_DESTROY:
            g_running = 0;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static HWND create_message_window(HINSTANCE hInstance) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "xpdash_agent_wnd";
    RegisterClassA(&wc);

    return CreateWindowA("xpdash_agent_wnd", "xpdash_agent", 0, 0, 0, 0, 0,
                         HWND_MESSAGE, NULL, hInstance, NULL);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    g_hwnd = create_message_window(hInstance);

    if (!net_init()) {
        return 1;
    }

    net_listen_control(NET_TCP_CONTROL_PORT);
    discover_init(on_server_discovered, NULL);
    audio_init(on_audio_frame, NULL);
    audio_start();
    video_init(on_video_frame, NULL);

    MSG msg;
    DWORD last_video_tick = GetTickCount();

    while (g_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        discover_poll();
        net_poll_control(NULL, NULL);

        DWORD now = GetTickCount();
        if (now - last_video_tick >= 16) { // ~60 fps
            video_capture();
            last_video_tick = now;
        } else {
            Sleep(1);
        }
    }

    video_shutdown();
    audio_shutdown();
    discover_shutdown();
    net_shutdown();

    return 0;
}
