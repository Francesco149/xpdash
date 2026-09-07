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

static HMODULE g_h_ddraw = NULL;
static LPDIRECTDRAW g_pdd = NULL;

static void vblank_init(void) {
    if (g_pdd) return;
    g_h_ddraw = LoadLibraryA("ddraw.dll");
    if (!g_h_ddraw) {
        agent_log("vblank_init: LoadLibraryA(ddraw.dll) failed, err=%lu", GetLastError());
        return;
    }

    typedef HRESULT (WINAPI *DirectDrawCreate_fn)(GUID *lpGUID, LPDIRECTDRAW *lplpDD, IUnknown *pUnkOuter);
    DirectDrawCreate_fn pfnDirectDrawCreate = (DirectDrawCreate_fn)GetProcAddress(g_h_ddraw, "DirectDrawCreate");
    if (!pfnDirectDrawCreate) {
        agent_log("vblank_init: GetProcAddress(DirectDrawCreate) failed, err=%lu", GetLastError());
        return;
    }

    LPDIRECTDRAW pdd = NULL;
    HRESULT hr = pfnDirectDrawCreate(NULL, &pdd, NULL);
    if (FAILED(hr) || !pdd) {
        agent_log("vblank_init: DirectDrawCreate failed hr=0x%08lX", (unsigned long)hr);
        return;
    }
    pdd->lpVtbl->SetCooperativeLevel(pdd, NULL, DDSCL_NORMAL);
    g_pdd = pdd;
    agent_log("vblank_init: DirectDraw hardware VSync synchronization initialized (pdd=%p)", g_pdd);
}

static void vblank_wait(void) {
    if (g_pdd) {
        /* Wait for the next vertical blanking interval to begin.
           During VBlank the front buffer is stable — Present() has
           completed and the game hasn't started drawing the next frame.
           Single BLOCKBEGIN is sufficient; the double-wait pattern
           (BLOCKEND then BLOCKBEGIN) costs an extra VSync period. */
        HRESULT hr = g_pdd->lpVtbl->WaitForVerticalBlank(g_pdd, DDWAITVB_BLOCKBEGIN, NULL);
        if (FAILED(hr)) {
            static int s_logged_vb_fail = 0;
            if (!s_logged_vb_fail) {
                agent_log("vblank_wait: WaitForVerticalBlank failed hr=0x%08lX", (unsigned long)hr);
                s_logged_vb_fail = 1;
            }
        }
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
        /* Always composite cursor — DirectX fullscreen games hide the system
           cursor (CURSOR_SHOWING=0) but GetCursorInfo still reports position.
           Without this, the cursor is invisible in games like GTA SA. */
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

static video_frame_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

static tjhandle g_tj = NULL;     /* TurboJPEG compressor instance */

static HANDLE g_h_video_thread = NULL;
static volatile int g_video_running = 0;

static DWORD WINAPI video_worker_thread(LPVOID lpParam) {
    (void)lpParam;
    agent_log("video_worker_thread: capture thread started (VSync-synced)");

    while (g_video_running) {
        if (net_is_streaming_active()) {
            /* VSync-synchronized capture: wait for the vertical blanking
               interval, then BitBlt immediately. During VBlank the front
               buffer is stable — the game has finished Present() and hasn't
               started the next Clear/Draw yet. This eliminates mid-render
               flicker for all content (desktop, windowed, fullscreen). */
            if (g_pdd) {
                vblank_wait();
            } else {
                /* No DirectDraw — fall back to fixed interval */
                Sleep(16);
            }
            video_capture();
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
    vblank_init();
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
                                     TJSAMP_420, JPEG_QUALITY,
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
        /* Hook active but no new frame this tick — skip, don't fall through
           to BitBlt which would produce a flickery frame. */
        return 1;
    }

    /* ── Path B: GDI BitBlt capture (fallback for desktop / non-D3D) ─ */
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

    DWORD t1 = timeGetTime();

    g_frame_counter++;

    int is_keyframe = g_force_keyframe || ((g_frame_counter % KEYFRAME_INTERVAL) == 0);
    int dirty = is_keyframe ? 1 : is_screen_dirty();

    if (!dirty) {
        return 1;
    }

    /* Draw cursor AFTER dirty check — compositing the cursor into the
       framebuffer before comparison would make every frame "dirty" even
       when nothing on screen changed, forcing full LZ4 compression. */
    draw_cursor(g_hdc_mem);

    DWORD t2 = timeGetTime();

    int raw_size = g_width * g_height * 4;
    unsigned long jpeg_size = 0;
    unsigned char *jpeg_buf = g_comp_buf;
    int comp_size = 0;
    uint8_t codec;

    if (g_tj && tjCompress2(g_tj, g_pixels, g_width, 0, g_height,
                             TJPF_BGRX, &jpeg_buf, &jpeg_size,
                             TJSAMP_420, JPEG_QUALITY,
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

        memcpy(g_prev_pixels, g_pixels, raw_size);
        g_force_keyframe = 0;

        /* Log frame timing every 120 frames (~2s at 60fps) */
        static uint32_t s_timing_accum = 0;
        s_timing_accum++;
        if (s_timing_accum >= 120) {
            agent_log("frame_timing[blt]: blt=%lums comp=%lums send=%lums total=%lums comp_sz=%d",
                      (unsigned long)(t1 - t0), (unsigned long)(t3 - t2),
                      (unsigned long)(t4 - t3), (unsigned long)(t4 - t0), comp_size);
            s_timing_accum = 0;
        }
    }

    return 1;
}

void video_shutdown(void) {
    video_stop();
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
    if (g_pdd) {
        g_pdd->lpVtbl->Release(g_pdd);
        g_pdd = NULL;
    }
    if (g_h_ddraw) {
        FreeLibrary(g_h_ddraw);
        g_h_ddraw = NULL;
    }
}
