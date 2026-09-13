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
#include <ddraw.h>
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
    volatile uint32_t hook_active;        /* 1 = hook is installed and running */
    volatile uint32_t present_rva;        /* RVA of Present in d3d9.dll (e.g. 0x40EA0) */
    volatile uint32_t reset_rva;          /* RVA of Reset in d3d9.dll (e.g. 0x436B0) */
    volatile uint32_t create_device_rva;  /* RVA of CreateDevice in d3d9.dll (e.g. 0x81670) */
    volatile uint32_t ddraw_flip_rva;     /* RVA of Flip in ddraw.dll (e.g. 0x399C) */
    uint8_t           reserved[8];        /* pad to 64 bytes */
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
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *dev,
    D3DPRESENT_PARAMETERS *pp);
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *d3d, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice9 **ppReturnedDeviceInterface);
typedef HRESULT (WINAPI *DDrawFlip_t)(void *this_surf, void *surf_target_override, DWORD flags);

/* ─── Globals ─────────────────────────────────────────────────────────── */

static DDrawFlip_t    g_orig_ddraw_flip    = NULL;
static uint8_t        g_orig_ddraw_flip_bytes[5];
static uint8_t        g_ddraw_flip_trampoline[16];
static void          *g_hooked_ddraw_flip_addr = NULL;
/* ─── Globals ─────────────────────────────────────────────────────────── */

static Present_t      g_orig_present       = NULL;
static Reset_t        g_orig_reset         = NULL;
static CreateDevice_t g_orig_create_device = NULL;
static IDirect3DDevice9 *g_cur_dev         = NULL;
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

    /* Initialize header, preserving present_rva / reset_rva if already set by agent */
    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    uint32_t saved_present_rva = (hdr->magic == 0x48443344) ? hdr->present_rva : 0;
    uint32_t saved_reset_rva   = (hdr->magic == 0x48443344) ? hdr->reset_rva : 0;
    uint32_t saved_create_rva  = (hdr->magic == 0x48443344) ? hdr->create_device_rva : 0;
    memset(hdr, 0, SHM_HEADER_SIZE);
    hdr->magic = 0x48443344;  /* 'D3DH' */
    hdr->present_rva       = saved_present_rva;
    hdr->reset_rva         = saved_reset_rva;
    hdr->create_device_rva = saved_create_rva;
    hdr->hook_active = 0;
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
    g_cur_dev = NULL;
}

static int ensure_sysmem_surf(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {
    if (g_sysmem_surf && g_cur_dev == dev && g_surf_width == w && g_surf_height == h && g_surf_format == fmt)
        return 1;

    release_sysmem_surf();
    g_cur_dev = dev;

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
    if (g_resolve_surf && g_cur_dev == dev && g_surf_width == w && g_surf_height == h && g_surf_format == fmt)
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
        return;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;

    /* Flow control: If agent has not consumed previous frame yet,
       skip expensive GPU readback so game render thread does not stall */
    if (hdr->producer_seq != hdr->consumer_seq) {
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
    if (dev != g_cur_dev) {
        if (g_cur_dev != NULL) {
            hook_log("hook_present: device changed from %p to %p, refreshing capture surfaces", g_cur_dev, dev);
        }
        release_sysmem_surf();
        g_cur_dev = dev;
    }
    capture_backbuffer(dev);
    LeaveCriticalSection(&g_cs);

    return g_orig_present(dev, src, dst, hWnd, dirty);
}
static void capture_ddraw_surface(void *this_surf, void *target_override) {
    if (!g_shm_ptr || !g_frame_event) return;

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->producer_seq != hdr->consumer_seq) {
        return;
    }

    IDirectDrawSurface7 *target_surf = (IDirectDrawSurface7 *)target_override;
    int need_release = 0;

    if (!target_surf) {
        IDirectDrawSurface7 *front = (IDirectDrawSurface7 *)this_surf;
        DDSCAPS2 caps;
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        if (FAILED(front->lpVtbl->GetAttachedSurface(front, &caps, &target_surf)) || !target_surf) {
            return;
        }
        need_release = 1;
    }

    DDSURFACEDESC2 ddsd;
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(DDSURFACEDESC2);
    HRESULT hr_lock = target_surf->lpVtbl->Lock(target_surf, NULL, &ddsd, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
    if (FAILED(hr_lock)) {
        ddsd.dwSize = sizeof(DDSURFACEDESC);
        hr_lock = target_surf->lpVtbl->Lock(target_surf, NULL, &ddsd, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
    }

    if (SUCCEEDED(hr_lock)) {
        uint32_t w = ddsd.dwWidth;
        uint32_t h = ddsd.dwHeight;
        uint32_t pitch = ddsd.lPitch;
        uint32_t bpp = ddsd.ddpfPixelFormat.dwRGBBitCount;
        uint8_t *src = (uint8_t *)ddsd.lpSurface;

        if (w > 0 && h > 0 && src && (w * h * 4 <= SHM_MAX_FRAME_SIZE)) {
            uint8_t *dst = (uint8_t *)g_shm_ptr + SHM_HEADER_SIZE;

            if (bpp == 32) {
                uint32_t row_bytes = w * 4;
                for (uint32_t y = 0; y < h; y++) {
                    memcpy(dst + y * row_bytes, src + y * pitch, row_bytes);
                }
            } else if (bpp == 16) {
                for (uint32_t y = 0; y < h; y++) {
                    const uint16_t *src_row = (const uint16_t *)(src + y * pitch);
                    uint32_t *dst_row = (uint32_t *)(dst + y * (w * 4));
                    for (uint32_t x = 0; x < w; x++) {
                        uint16_t p = src_row[x];
                        uint32_t r = ((p & 0xF800) >> 8);
                        uint32_t g = ((p & 0x07E0) >> 3);
                        uint32_t b = ((p & 0x001F) << 3);
                        r |= (r >> 5);
                        g |= (g >> 6);
                        b |= (b >> 5);
                        dst_row[x] = (r << 16) | (g << 8) | b;
                    }
                }
            } else if (bpp == 8) {
                PALETTEENTRY pal_entries[256];
                memset(pal_entries, 0, sizeof(pal_entries));
                LPDIRECTDRAWPALETTE pPal = NULL;
                target_surf->lpVtbl->GetPalette(target_surf, &pPal);
                if (pPal) {
                    pPal->lpVtbl->GetEntries(pPal, 0, 0, 256, pal_entries);
                    pPal->lpVtbl->Release(pPal);
                }
                uint32_t lut[256];
                for (int i = 0; i < 256; i++) {
                    lut[i] = ((uint32_t)pal_entries[i].peRed << 16) |
                             ((uint32_t)pal_entries[i].peGreen << 8) |
                             ((uint32_t)pal_entries[i].peBlue);
                }
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *src_row = src + y * pitch;
                    uint32_t *dst_row = (uint32_t *)(dst + y * (w * 4));
                    for (uint32_t x = 0; x < w; x++) {
                        dst_row[x] = lut[src_row[x]];
                    }
                }
            }

            g_frame_idx++;
            hdr->width       = w;
            hdr->height      = h;
            hdr->stride      = w * 4;
            hdr->data_size   = w * h * 4;
            hdr->frame_index = g_frame_idx;
            hdr->format      = bpp;
            _ReadWriteBarrier();
            hdr->producer_seq = g_frame_idx;
            SetEvent(g_frame_event);

            static uint32_t s_ddraw_log = 0;
            if (s_ddraw_log++ == 0) {
                hook_log("capture_ddraw_surface: first frame captured! %ux%u@%ubpp, frame_idx=%lu",
                         w, h, bpp, g_frame_idx);
            }
        }
        target_surf->lpVtbl->Unlock(target_surf, NULL);
    }

    if (need_release && target_surf) {
        target_surf->lpVtbl->Release(target_surf);
    }
}

static HRESULT WINAPI hook_ddraw_flip(void *this_surf, void *surf_target_override, DWORD flags) {
    EnterCriticalSection(&g_cs);
    capture_ddraw_surface(this_surf, surf_target_override);
    LeaveCriticalSection(&g_cs);
    return g_orig_ddraw_flip(this_surf, surf_target_override, flags);
}


static HRESULT WINAPI hook_reset(IDirect3DDevice9 *dev,
    D3DPRESENT_PARAMETERS *pp)
{
    hook_log("hook_reset: game resetting device %p (%ux%u, interval=0x%lx)",
             dev, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? (unsigned long)pp->PresentationInterval : 0);


    /* Release D3D resources before Reset so D3D9 Reset() doesn't fail with D3DERR_DEVICELOST */
    EnterCriticalSection(&g_cs);
    release_sysmem_surf();
    LeaveCriticalSection(&g_cs);
    HRESULT hr = g_orig_reset(dev, pp);
    hook_log("hook_reset: Reset returned 0x%08lX", (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_create_device(IDirect3D9 *d3d, UINT Adapter, D3DDEVTYPE DeviceType,
                                         HWND hFocusWindow, DWORD BehaviorFlags,
                                         D3DPRESENT_PARAMETERS *pp,
                                         IDirect3DDevice9 **ppDev)
{
    hook_log("hook_create_device: game creating D3D9 device (type=%u, flags=0x%lx, res=%ux%u, interval=0x%lx)",
             (unsigned int)DeviceType, (unsigned long)BehaviorFlags,
             pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? (unsigned long)pp->PresentationInterval : 0);


    /* Release any surfaces from older devices so the old device is fully freed in COM */
    EnterCriticalSection(&g_cs);
    release_sysmem_surf();
    LeaveCriticalSection(&g_cs);

    HRESULT hr = D3D_OK;
    int max_retries = 6;
    for (int retry = 0; retry < max_retries; retry++) {
        hr = g_orig_create_device(d3d, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, ppDev);
        if (SUCCEEDED(hr) && ppDev && *ppDev) {
            if (retry > 0) {
                hook_log("hook_create_device: succeeded after retry %d (dev=%p)", retry, *ppDev);
            } else {
                hook_log("hook_create_device: succeeded (dev=%p)", *ppDev);
            }
            return hr;
        }

        /* Transient errors during screen resolution / display mode switch */
        if (hr == D3DERR_DEVICELOST || hr == D3DERR_NOTAVAILABLE || hr == D3DERR_INVALIDCALL) {
            hook_log("hook_create_device: transient failure 0x%08lX on attempt %d/%d, retrying after 50ms...",
                     (unsigned long)hr, retry + 1, max_retries);
            Sleep(50);
        } else {
            break;
        }
    }

    hook_log("hook_create_device: final result hr=0x%08lX", (unsigned long)hr);
    return hr;
}
/* ─── Microsoft Hotpatch Detours ───────────────────────────────────────── */

static uint8_t  g_present_trampoline[8];
static uint8_t  g_reset_trampoline[8];
static uint8_t  g_create_device_trampoline[8];
static uint8_t *g_hooked_present_addr = NULL;
static uint8_t *g_hooked_reset_addr = NULL;
static uint8_t *g_hooked_create_device_addr = NULL;
static uint8_t  g_orig_present_bytes[5];
static uint8_t  g_orig_reset_bytes[5];
static uint8_t  g_orig_create_device_bytes[5];
static int install_hotpatch(void *target, void *hook, uint8_t *trampoline, void **p_orig, uint8_t *saved_bytes) {
    uint8_t *p = (uint8_t *)target;
    if (IsBadReadPtr(p, 5)) {
        hook_log("install_hotpatch: target %p is unreadable memory", target);
        return 0;
    }
    /* Verify standard Microsoft hotpatchable prologue:
       8B FF    mov edi, edi
       55       push ebp
       8B EC    mov ebp, esp */
    if (p[0] != 0x8B || p[1] != 0xFF || p[2] != 0x55 || p[3] != 0x8B || p[4] != 0xEC) {
        hook_log("install_hotpatch: target %p does not match hotpatchable prologue (%02X %02X %02X %02X %02X)",
                 target, p[0], p[1], p[2], p[3], p[4]);
        return 0;
    }

    memcpy(saved_bytes, p, 5);

    DWORD oldProtect;
    if (!VirtualProtect(trampoline, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        hook_log("install_hotpatch: VirtualProtect on trampoline failed, err=%lu", GetLastError());
        return 0;
    }

    /* Trampoline executes:
       push ebp        (0x55)
       mov ebp, esp    (0x8B 0xEC)
       jmp p + 5       (0xE9 rel32) */
    trampoline[0] = 0x55;
    trampoline[1] = 0x8B;
    trampoline[2] = 0xEC;
    trampoline[3] = 0xE9;
    uint32_t rel_trampoline = (uint32_t)((p + 5) - (&trampoline[3] + 5));
    memcpy(&trampoline[4], &rel_trampoline, 4);
    *p_orig = (void *)trampoline;

    /* Overwrite target prologue: jmp hook (0xE9 rel32) */
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        hook_log("install_hotpatch: VirtualProtect on target failed, err=%lu", GetLastError());
        return 0;
    }
    p[0] = 0xE9;
    uint32_t rel_hook = (uint32_t)((uint8_t *)hook - (p + 5));
    memcpy(&p[1], &rel_hook, 4);
    VirtualProtect(p, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), p, 5);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 8);
    return 1;
}
static int install_5byte_detour(void *target, void *hook, uint8_t *trampoline, void **p_orig, uint8_t *saved_bytes) {
    uint8_t *p = (uint8_t *)target;
    if (IsBadReadPtr(p, 5)) {
        hook_log("install_5byte_detour: target %p is unreadable memory", target);
        return 0;
    }
    memcpy(saved_bytes, p, 5);

    DWORD oldProtect;
    if (!VirtualProtect(trampoline, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        hook_log("install_5byte_detour: VirtualProtect on trampoline failed, err=%lu", GetLastError());
        return 0;
    }

    memcpy(trampoline, p, 5);
    trampoline[5] = 0xE9; // JMP rel32
    uint32_t rel_trampoline = (uint32_t)((p + 5) - (&trampoline[5] + 5));
    memcpy(&trampoline[6], &rel_trampoline, 4);
    *p_orig = (void *)trampoline;

    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        hook_log("install_5byte_detour: VirtualProtect on target failed, err=%lu", GetLastError());
        return 0;
    }
    p[0] = 0xE9;
    uint32_t rel_hook = (uint32_t)((uint8_t *)hook - (p + 5));
    memcpy(&p[1], &rel_hook, 4);
    VirtualProtect(p, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), p, 5);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 10);
    return 1;
}

static int install_hooks(void) {
    hook_log("install_hooks: installing hotpatch detours in PID %lu", GetCurrentProcessId());

    int hook_installed = 0;

    HMODULE hd3d9 = GetModuleHandleA("d3d9.dll");
    if (hd3d9) {
        uint32_t present_rva       = 0x40EA0;
        uint32_t reset_rva         = 0x436B0;
        uint32_t create_device_rva = 0x81670;
        if (g_shm_ptr) {
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            if (hdr->present_rva != 0)       present_rva       = hdr->present_rva;
            if (hdr->reset_rva != 0)         reset_rva         = hdr->reset_rva;
            if (hdr->create_device_rva != 0) create_device_rva = hdr->create_device_rva;
        }

        uint8_t *pPresent       = (uint8_t *)((uintptr_t)hd3d9 + present_rva);
        uint8_t *pReset         = (uint8_t *)((uintptr_t)hd3d9 + reset_rva);
        uint8_t *pCreateDevice  = (uint8_t *)((uintptr_t)hd3d9 + create_device_rva);

        int ok_present = install_hotpatch(pPresent, (void *)hook_present, g_present_trampoline,
                                          (void **)&g_orig_present, g_orig_present_bytes);
        if (ok_present) {
            g_hooked_present_addr = pPresent;
            hook_log("install_hooks: D3D9 Present hotpatched successfully at %p", pPresent);
            hook_installed = 1;
        }

        int ok_reset = install_hotpatch(pReset, (void *)hook_reset, g_reset_trampoline,
                                        (void **)&g_orig_reset, g_orig_reset_bytes);
        if (ok_reset) {
            g_hooked_reset_addr = pReset;
            hook_log("install_hooks: D3D9 Reset hotpatched successfully at %p", pReset);
        }

        int ok_create = install_hotpatch(pCreateDevice, (void *)hook_create_device,
                                         g_create_device_trampoline,
                                         (void **)&g_orig_create_device,
                                         g_orig_create_device_bytes);
        if (ok_create) {
            g_hooked_create_device_addr = pCreateDevice;
            hook_log("install_hooks: D3D9 CreateDevice hotpatched successfully at %p", pCreateDevice);
        }
    }

    HMODULE hddraw = GetModuleHandleA("ddraw.dll");
    if (hddraw) {
        uint32_t flip_rva = 0x399C;
        if (g_shm_ptr) {
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            if (hdr->ddraw_flip_rva != 0) flip_rva = hdr->ddraw_flip_rva;
        }
        uint8_t *pFlip = (uint8_t *)((uintptr_t)hddraw + flip_rva);
        if (install_5byte_detour(pFlip, (void *)hook_ddraw_flip, g_ddraw_flip_trampoline,
                                (void **)&g_orig_ddraw_flip, g_orig_ddraw_flip_bytes)) {
            g_hooked_ddraw_flip_addr = pFlip;
            hook_log("install_hooks: DirectDraw Flip detoured successfully at %p (RVA 0x%lX)",
                     pFlip, (unsigned long)flip_rva);
            hook_installed = 1;
        }
    }

    return hook_installed;
}

static void remove_hooks(void) {
    DWORD oldProtect;
    if (g_hooked_present_addr) {
        if (VirtualProtect(g_hooked_present_addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_hooked_present_addr, g_orig_present_bytes, 5);
            VirtualProtect(g_hooked_present_addr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), g_hooked_present_addr, 5);
        }
        g_hooked_present_addr = NULL;
    }
    if (g_hooked_reset_addr) {
        if (VirtualProtect(g_hooked_reset_addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_hooked_reset_addr, g_orig_reset_bytes, 5);
            VirtualProtect(g_hooked_reset_addr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), g_hooked_reset_addr, 5);
        }
        g_hooked_reset_addr = NULL;
    }
    if (g_hooked_create_device_addr) {
        if (VirtualProtect(g_hooked_create_device_addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_hooked_create_device_addr, g_orig_create_device_bytes, 5);
            VirtualProtect(g_hooked_create_device_addr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), g_hooked_create_device_addr, 5);
        }
        g_hooked_create_device_addr = NULL;
    }
    if (g_hooked_ddraw_flip_addr) {
        if (VirtualProtect(g_hooked_ddraw_flip_addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_hooked_ddraw_flip_addr, g_orig_ddraw_flip_bytes, 5);
            VirtualProtect(g_hooked_ddraw_flip_addr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), g_hooked_ddraw_flip_addr, 5);
        }
        g_hooked_ddraw_flip_addr = NULL;
    }
    hook_log("remove_hooks: hotpatch detours restored cleanly");
}
/* ─── DLL Exports & Entry Point ───────────────────────────────────────── */

__declspec(dllexport) int install_d3d9_hooks(void) {
    if (g_initialized && g_hooked_present_addr) return 1;
    if (install_hooks()) {
        HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
        if (hdr) hdr->hook_active = 1;
        g_initialized = 1;
        return 1;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hInstDLL);

        char proc_name[MAX_PATH];
        GetModuleFileNameA(NULL, proc_name, sizeof(proc_name));
        if (strstr(proc_name, "xpdash-agent.exe") || strstr(proc_name, "xpdash-agent")) {
            /* Never initialize hook inside xpdash-agent */
            return TRUE;
        }

        InitializeCriticalSection(&g_cs);

        if (!shm_init()) return FALSE;

        if (install_hooks()) {
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            if (hdr) hdr->hook_active = 1;
            g_initialized = 1;
        }
        break;
    }

    case DLL_PROCESS_DETACH: {
        char proc_name[MAX_PATH];
        GetModuleFileNameA(NULL, proc_name, sizeof(proc_name));
        if (strstr(proc_name, "xpdash-agent.exe") || strstr(proc_name, "xpdash-agent")) {
            return TRUE;
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
