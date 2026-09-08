/*
 * d3d9hook_dll.c — D3D9 Present hook for zero-flicker game capture.
 *
 * This DLL is injected into a D3D9 game process. It hooks Present() to
 * capture the completed back buffer at the exact right moment — after the
 * game finishes rendering but before the buffer is swapped. This eliminates
 * mid-render artifacts (flickering geometry, invisible HUD elements) that
 * plague GDI BitBlt capture.
 *
 * Communication with the xpdash-agent process is via named shared memory
 * and named events — no pipes, no sockets, just kernel objects.
 *
 * Zero external dependencies: d3d9.dll (already loaded), kernel32.dll, user32.dll.
 * Compatible with Windows XP SP3 (PE subsystem 5.1).
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>

static void hook_log(const char *fmt, ...) {
    FILE *f = fopen("C:\\xpdash\\hook.log", "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}
/* ─── Shared Memory Layout ────────────────────────────────────────────── */

#define XPDASH_HOOK_SHM_NAME   "XpDashHookShm"
#define XPDASH_HOOK_EVENT_NAME "XpDashHookFrameReady"
#define XPDASH_HOOK_ACK_NAME   "XpDashHookFrameAck"

/* Max frame size: 1920×1200×4 = 9,216,000 bytes. Round up to 10 MB.
   We only need 800×600×4 = 1,920,000 but leave room for resolution changes. */
#define SHM_MAX_FRAME_SIZE (10 * 1024 * 1024)

/* Header at the start of shared memory, followed by raw pixel data. */
typedef struct {
    volatile uint32_t magic;          /* 0x48443344 = 'D3DH' */
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t stride;         /* bytes per row */
    volatile uint32_t frame_index;
    volatile uint32_t data_size;      /* total pixel data bytes */
    volatile uint32_t format;         /* D3DFORMAT value */
    volatile uint32_t producer_seq;   /* incremented by hook on each frame */
    volatile uint32_t consumer_seq;   /* incremented by agent after reading */
    volatile uint32_t hook_active;    /* 1 = hook is installed and running */
    uint8_t           reserved[24];   /* pad to 64 bytes */
    /* Pixel data follows at offset 64 */
} HookShmHeader;

#define SHM_HEADER_SIZE   64
#define SHM_TOTAL_SIZE    (SHM_HEADER_SIZE + SHM_MAX_FRAME_SIZE)

/* ─── D3D9 vtable indices ─────────────────────────────────────────────── */

#define VTBL_IDX_RELEASE       2
#define VTBL_IDX_RESET        16
#define VTBL_IDX_PRESENT      17
#define VTBL_IDX_GETBACKBUF   18
#define VTBL_IDX_ENDSCENE     42
/* ─── Function pointer types ──────────────────────────────────────────── */

typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *dev,
    const RECT *src, const RECT *dst, HWND hWnd, const RGNDATA *dirty);
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9 *dev);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *dev,
    D3DPRESENT_PARAMETERS *pp);

/* ─── Globals ─────────────────────────────────────────────────────────── */

static Present_t  g_orig_present  = NULL;
static EndScene_t g_orig_endscene = NULL;
static Reset_t    g_orig_reset    = NULL;
static void     **g_hooked_vtable = NULL;
static HANDLE  g_shm_handle = NULL;
static void   *g_shm_ptr    = NULL;
static HANDLE  g_frame_event = NULL;  /* signaled when a frame is ready */
static HANDLE  g_ack_event   = NULL;  /* signaled when agent consumed frame */

static IDirect3DSurface9 *g_sysmem_surf = NULL;
static uint32_t g_surf_width  = 0;
static uint32_t g_surf_height = 0;
static uint32_t g_frame_idx   = 0;

static CRITICAL_SECTION g_cs;
static int g_initialized = 0;

/* ─── Shared Memory Setup ─────────────────────────────────────────────── */

static int shm_init(void) {
    g_shm_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, SHM_TOTAL_SIZE, XPDASH_HOOK_SHM_NAME);
    if (!g_shm_handle) return 0;

    g_shm_ptr = MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS,
                              0, 0, SHM_TOTAL_SIZE);
    if (!g_shm_ptr) {
        CloseHandle(g_shm_handle);
        g_shm_handle = NULL;
        return 0;
    }

    /* Create events: manual-reset for frame ready, auto-reset for ack */
    g_frame_event = CreateEventA(NULL, TRUE, FALSE, XPDASH_HOOK_EVENT_NAME);
    g_ack_event   = CreateEventA(NULL, FALSE, FALSE, XPDASH_HOOK_ACK_NAME);
    if (!g_frame_event || !g_ack_event) return 0;

    /* Initialize header */
    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    memset(hdr, 0, SHM_HEADER_SIZE);
    hdr->magic = 0x48443344;  /* 'D3DH' */
    hdr->hook_active = 1;

    return 1;
}

static void shm_shutdown(void) {
    if (g_shm_ptr) {
        HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
        hdr->hook_active = 0;
        UnmapViewOfFile(g_shm_ptr);
        g_shm_ptr = NULL;
    }
    if (g_shm_handle)  { CloseHandle(g_shm_handle);  g_shm_handle = NULL; }
    if (g_frame_event) { CloseHandle(g_frame_event); g_frame_event = NULL; }
    if (g_ack_event)   { CloseHandle(g_ack_event);   g_ack_event = NULL; }
}

/* ─── System Memory Surface Management ────────────────────────────────── */
static IDirect3DSurface9 *g_resolve_surf = NULL;
static D3DFORMAT g_surf_format = D3DFMT_UNKNOWN;

static void release_sysmem_surf(void) {
    if (g_sysmem_surf) {
        g_sysmem_surf->lpVtbl->Release(g_sysmem_surf);
        g_sysmem_surf = NULL;
    }
    if (g_resolve_surf) {
        g_resolve_surf->lpVtbl->Release(g_resolve_surf);
        g_resolve_surf = NULL;
    }
    g_surf_width = 0;
    g_surf_height = 0;
    g_surf_format = D3DFMT_UNKNOWN;
}

static int ensure_sysmem_surf(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
    if (g_sysmem_surf && g_surf_width == w && g_surf_height == h && g_surf_format == fmt)
        return 1;

    release_sysmem_surf();

    HRESULT hr = dev->lpVtbl->CreateOffscreenPlainSurface(
        dev, w, h, fmt, D3DPOOL_SYSTEMMEM, &g_sysmem_surf, NULL);
    if (FAILED(hr) || !g_sysmem_surf) {
        hook_log("ensure_sysmem_surf: CreateOffscreenPlainSurface failed %ux%u fmt=%u hr=0x%08lX",
                 w, h, (unsigned int)fmt, (unsigned long)hr);
        return 0;
    }

    g_surf_width = w;
    g_surf_height = h;
    g_surf_format = fmt;
    return 1;
}

static int ensure_resolve_surf(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
    if (g_resolve_surf && g_surf_width == w && g_surf_height == h && g_surf_format == fmt)
        return 1;

    if (g_resolve_surf) {
        g_resolve_surf->lpVtbl->Release(g_resolve_surf);
        g_resolve_surf = NULL;
    }

    HRESULT hr = dev->lpVtbl->CreateRenderTarget(
        dev, w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolve_surf, NULL);
    if (FAILED(hr) || !g_resolve_surf) {
        hook_log("ensure_resolve_surf: CreateRenderTarget failed %ux%u fmt=%u hr=0x%08lX",
                 w, h, (unsigned int)fmt, (unsigned long)hr);
        return 0;
    }
    return 1;
}
/* ─── Frame Capture ───────────────────────────────────────────────────── */

static void capture_backbuffer(IDirect3DDevice9 *dev) {
    if (!g_shm_ptr || !g_frame_event) {
        static int s_logged_no_shm = 0;
        if (!s_logged_no_shm) {
            hook_log("capture_backbuffer: no SHM (%p) or frame_event (%p)", g_shm_ptr, g_frame_event);
            s_logged_no_shm = 1;
        }
        return;
    }

    IDirect3DSurface9 *back_buf = NULL;
    HRESULT hr = dev->lpVtbl->GetBackBuffer(dev, 0, 0,
                                             D3DBACKBUFFER_TYPE_MONO, &back_buf);
    if (FAILED(hr) || !back_buf) {
        static int s_logged_bb_fail = 0;
        if (!s_logged_bb_fail) {
            hook_log("capture_backbuffer: GetBackBuffer failed hr=0x%08lX", (unsigned long)hr);
            s_logged_bb_fail = 1;
        }
        return;
    }

    /* Get back buffer dimensions */
    D3DSURFACE_DESC desc;
    hr = back_buf->lpVtbl->GetDesc(back_buf, &desc);
    if (FAILED(hr)) {
        back_buf->lpVtbl->Release(back_buf);
        return;
    }

    static int s_logged_desc = 0;
    if (!s_logged_desc) {
        hook_log("capture_backbuffer: backbuffer %ux%u, format=%u, multisample=%u",
                 desc.Width, desc.Height, (unsigned int)desc.Format, (unsigned int)desc.MultiSampleType);
        s_logged_desc = 1;
    }

    /* Ensure our system memory surface matches format and dimensions */
    if (!ensure_sysmem_surf(dev, desc.Width, desc.Height, desc.Format)) {
        back_buf->lpVtbl->Release(back_buf);
        return;
    }

    IDirect3DSurface9 *src_surf = back_buf;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        /* Resolve multisampled backbuffer via StretchRect to non-MSAA target */
        if (!ensure_resolve_surf(dev, desc.Width, desc.Height, desc.Format)) {
            back_buf->lpVtbl->Release(back_buf);
            return;
        }
        HRESULT hr_sr = dev->lpVtbl->StretchRect(dev, back_buf, NULL, g_resolve_surf, NULL, D3DTEXF_NONE);
        if (FAILED(hr_sr)) {
            static int s_logged_sr_fail = 0;
            if (!s_logged_sr_fail) {
                hook_log("capture_backbuffer: StretchRect resolve failed hr=0x%08lX", (unsigned long)hr_sr);
                s_logged_sr_fail = 1;
            }
            back_buf->lpVtbl->Release(back_buf);
            return;
        }
        src_surf = g_resolve_surf;
    }

    /* GPU → system memory copy */
    hr = dev->lpVtbl->GetRenderTargetData(dev, src_surf, g_sysmem_surf);
    back_buf->lpVtbl->Release(back_buf);
    if (FAILED(hr)) {
        static int s_logged_rt_fail = 0;
        if (!s_logged_rt_fail) {
            hook_log("capture_backbuffer: GetRenderTargetData failed hr=0x%08lX (src=%p, dst=%p)",
                     (unsigned long)hr, src_surf, g_sysmem_surf);
            s_logged_rt_fail = 1;
        }
        return;
    }

    /* Lock and copy to shared memory */
    D3DLOCKED_RECT lr;
    hr = g_sysmem_surf->lpVtbl->LockRect(g_sysmem_surf, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        static int s_logged_lock_fail = 0;
        if (!s_logged_lock_fail) {
            hook_log("capture_backbuffer: LockRect failed hr=0x%08lX", (unsigned long)hr);
            s_logged_lock_fail = 1;
        }
        return;
    }

    uint32_t w = desc.Width;
    uint32_t h = desc.Height;
    uint32_t stride = (uint32_t)lr.Pitch;
    uint32_t data_size = stride * h;

    /* Sanity check: don't overflow shared memory */
    if (data_size > SHM_MAX_FRAME_SIZE) {
        g_sysmem_surf->lpVtbl->UnlockRect(g_sysmem_surf);
        return;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    uint8_t *pixel_dst = (uint8_t *)g_shm_ptr + SHM_HEADER_SIZE;

    /* Copy pixel data row by row (pitch may differ from width*4) */
    uint8_t *src = (uint8_t *)lr.pBits;
    uint32_t row_bytes = w * 4;
    uint32_t row;
    for (row = 0; row < h; row++) {
        memcpy(pixel_dst + row * row_bytes, src + row * stride, row_bytes);
    }

    g_frame_idx++;

    /* Update header atomically — write fields, then bump sequence */
    hdr->width       = w;
    hdr->height      = h;
    hdr->stride      = row_bytes;
    hdr->data_size   = row_bytes * h;
    hdr->frame_index = g_frame_idx;
    hdr->format      = (uint32_t)desc.Format;

    /* Memory fence: ensure pixel data is visible before sequence bump */
    _ReadWriteBarrier();
    hdr->producer_seq = g_frame_idx;

    /* Signal the agent that a frame is ready */
    SetEvent(g_frame_event);

    g_sysmem_surf->lpVtbl->UnlockRect(g_sysmem_surf);

    static uint32_t s_log_count = 0;
    if (s_log_count++ == 0) {
        hook_log("capture_backbuffer: first frame captured! %ux%u stride=%u, frame_idx=%lu",
                 w, h, row_bytes, g_frame_idx);
    }
}

/* ─── Hooked Functions ────────────────────────────────────────────────── */

static HRESULT WINAPI hook_present(IDirect3DDevice9 *dev,
    const RECT *src, const RECT *dst, HWND hWnd, const RGNDATA *dirty)
{
    static uint32_t s_present_calls = 0;
    if (s_present_calls++ == 0) {
        hook_log("hook_present: first call! dev=%p, hWnd=%p", dev, hWnd);
    }
    /* Capture BEFORE the original Present — back buffer is complete */
    EnterCriticalSection(&g_cs);
    capture_backbuffer(dev);
    LeaveCriticalSection(&g_cs);

    return g_orig_present(dev, src, dst, hWnd, dirty);
}

static HRESULT WINAPI hook_reset(IDirect3DDevice9 *dev,
    D3DPRESENT_PARAMETERS *pp)
{
    /* Release D3D resources before Reset */
    EnterCriticalSection(&g_cs);
    release_sysmem_surf();
    LeaveCriticalSection(&g_cs);

    return g_orig_reset(dev, pp);
}

/* ─── Vtable Hooking ──────────────────────────────────────────────────── */

static int hook_vtable(void **vtable, int index, void *hook, void **original) {
    DWORD old_protect;
    if (!VirtualProtect(&vtable[index], sizeof(void *),
                        PAGE_EXECUTE_READWRITE, &old_protect))
        return 0;

    *original = vtable[index];
    vtable[index] = hook;

    VirtualProtect(&vtable[index], sizeof(void *), old_protect, &old_protect);
    return 1;
}

static int install_hooks(void) {
    hook_log("install_hooks: starting hook installation in PID %lu", GetCurrentProcessId());

    /* Method 1: Direct check for RenderWare games (GTA San Andreas v1.0) */
    IDirect3DDevice9 **ppDev = (IDirect3DDevice9 **)0x00C97C28;
    if (!IsBadReadPtr(ppDev, sizeof(void *)) && *ppDev != NULL) {
        IDirect3DDevice9 *dev = *ppDev;
        if (!IsBadReadPtr(dev, sizeof(void *))) {
            void **vtable = *(void ***)dev;
            if (!IsBadReadPtr(vtable, sizeof(void *) * 43)) {
                hook_log("install_hooks: found active RenderWare D3D9 device at %p (vtable=%p)", dev, vtable);
                g_hooked_vtable = vtable;
                int ok = 1;
                ok &= hook_vtable(vtable, VTBL_IDX_PRESENT, (void *)hook_present,
                                  (void **)&g_orig_present);
                ok &= hook_vtable(vtable, VTBL_IDX_RESET, (void *)hook_reset,
                                  (void **)&g_orig_reset);
                if (ok) {
                    hook_log("install_hooks: RenderWare D3D9 device hooked successfully (orig_present=%p)",
                             g_orig_present);
                    return 1;
                } else {
                    hook_log("install_hooks: hook_vtable failed for RenderWare device");
                }
            }
        }
    }

    /* Method 2: Temporary D3D9 device creation (windowed apps & desktop fallback) */
    hook_log("install_hooks: attempting dummy device creation");
    HMODULE hd3d9 = LoadLibraryA("d3d9.dll");
    if (!hd3d9) {
        hook_log("install_hooks: LoadLibraryA(d3d9.dll) failed, err=%lu", GetLastError());
        return 0;
    }

    typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
    Direct3DCreate9_t pCreate9 = (Direct3DCreate9_t)GetProcAddress(hd3d9, "Direct3DCreate9");
    if (!pCreate9) {
        hook_log("install_hooks: GetProcAddress(Direct3DCreate9) failed");
        return 0;
    }

    IDirect3D9 *d3d9 = pCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
        hook_log("install_hooks: Direct3DCreate9 failed");
        return 0;
    }

    HWND hwnd = CreateWindowExA(0, "STATIC", "xpdash_hook_tmp",
                                WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!hwnd) {
        hwnd = GetDesktopWindow();
        hook_log("install_hooks: CreateWindowExA failed, using GetDesktopWindow=%p", hwnd);
    }

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;

    D3DDISPLAYMODE d3ddm;
    if (SUCCEEDED(d3d9->lpVtbl->GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &d3ddm))) {
        pp.BackBufferFormat = d3ddm.Format;
    } else {
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    }
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = d3d9->lpVtbl->CreateDevice(
        d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    hook_log("install_hooks: CreateDevice(HAL) hr=0x%08lX, dev=%p", (unsigned long)hr, dev);
    if (FAILED(hr) || !dev) {
        hr = d3d9->lpVtbl->CreateDevice(
            d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        hook_log("install_hooks: CreateDevice(NULLREF) hr=0x%08lX, dev=%p", (unsigned long)hr, dev);
        if (FAILED(hr) || !dev) {
            d3d9->lpVtbl->Release(d3d9);
            if (hwnd && hwnd != GetDesktopWindow()) DestroyWindow(hwnd);
            return 0;
        }
    }

    void **vtable = *(void ***)dev;
    hook_log("install_hooks: dummy device created, vtable=%p", vtable);

    g_hooked_vtable = vtable;
    int ok = 1;
    ok &= hook_vtable(vtable, VTBL_IDX_PRESENT, (void *)hook_present,
                      (void **)&g_orig_present);
    ok &= hook_vtable(vtable, VTBL_IDX_RESET, (void *)hook_reset,
                      (void **)&g_orig_reset);
    dev->lpVtbl->Release(dev);
    d3d9->lpVtbl->Release(d3d9);
    if (hwnd && hwnd != GetDesktopWindow()) DestroyWindow(hwnd);

    hook_log("install_hooks: dummy device hooking result=%d (orig_present=%p)", ok, g_orig_present);
    return ok;
}

static void remove_hooks(void) {
    /* Restore original function pointers so process continues cleanly */
    if (g_hooked_vtable && g_orig_present) {
        DWORD old_protect;
        if (VirtualProtect(&g_hooked_vtable[VTBL_IDX_PRESENT], sizeof(void *),
                            PAGE_EXECUTE_READWRITE, &old_protect)) {
            g_hooked_vtable[VTBL_IDX_PRESENT] = (void *)g_orig_present;
            VirtualProtect(&g_hooked_vtable[VTBL_IDX_PRESENT], sizeof(void *),
                            old_protect, &old_protect);
        }
    }
    if (g_hooked_vtable && g_orig_reset) {
        DWORD old_protect;
        if (VirtualProtect(&g_hooked_vtable[VTBL_IDX_RESET], sizeof(void *),
                            PAGE_EXECUTE_READWRITE, &old_protect)) {
            g_hooked_vtable[VTBL_IDX_RESET] = (void *)g_orig_reset;
            VirtualProtect(&g_hooked_vtable[VTBL_IDX_RESET], sizeof(void *),
                            old_protect, &old_protect);
        }
    }
    hook_log("remove_hooks: vtable restored cleanly");
    g_hooked_vtable = NULL;
    g_orig_present  = NULL;
    g_orig_endscene = NULL;
    g_orig_reset    = NULL;
}
/* ─── DLL Entry Point ─────────────────────────────────────────────────── */

static HANDLE g_init_thread = NULL;
static volatile int g_stop_init_thread = 0;
__declspec(dllexport) int install_d3d9_hooks(void) {
    if (g_initialized && g_hooked_vtable) return 1;
    if (install_hooks()) {
        HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
        if (hdr) hdr->hook_active = 1;
        g_initialized = 1;
        return 1;
    }
    return 0;
}

static DWORD WINAPI hook_init_thread(LPVOID param) {
    (void)param;

    hook_log("hook_init_thread: background init loop started");

    /* Retry install_hooks() periodically until the game initializes its D3D9 device.
       Games take between 1 to 15 seconds to create their device after launch. */
    for (int retry = 0; retry < 150 && !g_stop_init_thread; retry++) {
        Sleep(200);
        if (g_stop_init_thread) break;
        if (install_hooks()) {
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            if (hdr) hdr->hook_active = 1;
            g_initialized = 1;
            hook_log("hook_init_thread: successfully hooked after %dms", (retry + 1) * 200);
            return 0;
        }
    }

    hook_log("hook_init_thread: gave up after 30s, hook inactive");
    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr) hdr->hook_active = 0;
    return 1;
}
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hInstDLL);

        char proc_name[MAX_PATH];
        GetModuleFileNameA(NULL, proc_name, sizeof(proc_name));
        if (strstr(proc_name, "xpdash-agent.exe") || strstr(proc_name, "xpdash-agent")) {
            /* Never initialize hook or threads if loaded inside xpdash-agent */
            return TRUE;
        }

        InitializeCriticalSection(&g_cs);

        if (!shm_init()) return FALSE;

        /* Defer hook installation — DllMain cannot call LoadLibrary,
           create COM objects, or do anything that acquires the loader lock.
           We spawn a thread that will install hooks after DllMain returns. */
        g_stop_init_thread = 0;
        g_init_thread = CreateThread(NULL, 0, hook_init_thread, NULL, 0, NULL);
        if (!g_init_thread) {
            shm_shutdown();
            return FALSE;
        }
        break;
    }

    case DLL_PROCESS_DETACH: {
        char proc_name[MAX_PATH];
        GetModuleFileNameA(NULL, proc_name, sizeof(proc_name));
        if (strstr(proc_name, "xpdash-agent.exe") || strstr(proc_name, "xpdash-agent")) {
            return TRUE;
        }

        g_stop_init_thread = 1;
        if (g_init_thread) {
            /* Wait briefly for init thread to finish */
            WaitForSingleObject(g_init_thread, 500);
            CloseHandle(g_init_thread);
            g_init_thread = NULL;
        }
        if (g_initialized) {
            remove_hooks();
            release_sysmem_surf();
            g_initialized = 0;
        }
        shm_shutdown();
        DeleteCriticalSection(&g_cs);
        break;
    }
    }
    return TRUE;
}
