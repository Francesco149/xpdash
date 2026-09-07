/* agent/src/main.c — xpdash Windows XP Native Agent */
#define _WIN32_WINNT 0x0501
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <mmsystem.h>

#include "audio.h"
#include "video.h"
#include "input.h"
#include "net.h"
#include "discover.h"
#include "log.h"
#include "d3d9hook.h"

static volatile int g_running = 1;
static HWND g_hwnd = NULL;

static void on_audio_frame(const uint8_t *pcm_data, uint32_t size, uint32_t pts_ms, void *user_data) {
    (void)user_data;
    net_send_audio(pcm_data, size, pts_ms);
}

static void on_video_frame(const uint8_t *comp_data, uint32_t comp_size,
                           uint32_t frame_index, uint16_t width, uint16_t height,
                           uint8_t codec, uint8_t flags, uint32_t pts_ms,
                           void *user_data) {
    (void)user_data;
    net_send_video_frame(comp_data, comp_size, frame_index, width, height, codec, flags, pts_ms);
}

static void on_stream_state(int is_streaming, void *user_data) {
    (void)user_data;
    agent_log("on_stream_state: streaming=%d", is_streaming);
    if (is_streaming) {
        video_force_keyframe();
        /* Attempt D3D9 hook injection when streaming starts.
           If a D3D9 game is running, this hooks Present() for
           flicker-free capture. If no game is running, this is a no-op. */
        if (!d3d9hook_is_active()) {
            d3d9hook_inject("C:\\xpdash\\xpdash-hook.dll", 0);
        }
    }
}

static void on_server_discovered(const DiscoveredServer *server, void *user_data) {
    (void)user_data;
    if (!server) return;

    if (!net_is_connected()) {
        if (discover_is_trusted(server)) {
            agent_log("Discovered trusted server: %s (%s), connecting to %d/%d",
                      server->server_name, server->ip, server->control_port, server->media_port);
            if (net_connect_to_server(server->ip, server->control_port, server->media_port)) {
                video_force_keyframe();
            }
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DISPLAYCHANGE: {
            int new_w = LOWORD(lParam);
            int new_h = HIWORD(lParam);
            agent_log("WM_DISPLAYCHANGE: %dx%d", new_w, new_h);
            video_resize(new_w, new_h);
            net_send_video_resize((uint16_t)new_w, (uint16_t)new_h, 32);
            video_force_keyframe();
            return 0;
        }
        case WM_DESTROY:
            agent_log("WM_DESTROY received");
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

    agent_log("=== xpdash-agent starting ===");
    timeBeginPeriod(1);
    g_hwnd = create_message_window(hInstance);
    agent_log("create_message_window: hwnd=%p", g_hwnd);

    if (!net_init()) {
        agent_log("net_init failed!");
        return 1;
    }

    net_listen_control(NET_TCP_CONTROL_PORT);
    discover_init(on_server_discovered, NULL);
    d3d9hook_init();
    audio_init(on_audio_frame, NULL);
    audio_start();
    video_init(on_video_frame, NULL);
    video_start();
    agent_log("All subsystems initialized, entering main loop");

    MSG msg;
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
        net_poll_control(on_stream_state, NULL);
        net_poll_udp_input();

        /* Periodically attempt hook injection if streaming is active but hook is not */
        if (net_is_streaming_active() && !d3d9hook_is_active()) {
            static DWORD s_last_hook_poll = 0;
            DWORD now = timeGetTime();
            if (now - s_last_hook_poll >= 1500) {
                s_last_hook_poll = now;
                d3d9hook_inject("C:\\xpdash\\xpdash-hook.dll", 0);
            }
        }

        Sleep(1);
    }

    agent_log("Exiting main loop, shutting down subsystems");
    d3d9hook_shutdown();
    video_shutdown();
    audio_shutdown();
    discover_shutdown();
    net_shutdown();

    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    timeEndPeriod(1);

    agent_log("=== xpdash-agent stopped ===");
    return 0;
}
