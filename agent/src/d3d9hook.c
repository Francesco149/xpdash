/*
 * d3d9hook.c — Agent-side D3D9 hook manager.
 *
 * Handles DLL injection into game processes, shared memory reading,
 * and fallback detection. The video capture thread calls d3d9hook_read_frame()
 * to get frames from the hook, falling back to BitBlt when the hook is
 * not active.
 */
#define _WIN32_WINNT 0x0501
#include <winsock2.h>
#include "d3d9hook.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <ddraw.h>
#include <stdio.h>

/* ─── State ───────────────────────────────────────────────────────────── */

static CRITICAL_SECTION g_hook_cs;
static int g_hook_cs_inited = 0;

static HANDLE  g_shm_handle   = NULL;
static void   *g_shm_ptr      = NULL;
static HANDLE  g_frame_event  = NULL;
static HANDLE  g_ack_event    = NULL;
static uint32_t g_last_seq    = 0;
static HANDLE  g_injected_proc = NULL;
static DWORD   g_injected_pid  = 0;
static HMODULE g_remote_dll_base = NULL;
static DWORD   s_inject_time     = 0;

/* ─── Init / Shutdown ─────────────────────────────────────────────────── */

/* Resolve Present, Reset, and CreateDevice RVAs dynamically from d3d9.dll once at startup */
static void resolve_d3d9_rvas(uint32_t *p_present_rva, uint32_t *p_reset_rva, uint32_t *p_create_rva) {
    *p_present_rva = 0x40EA0; /* Standard XP SP3 default fallback */
    *p_reset_rva   = 0x436B0;
    *p_create_rva  = 0x81670;

    HMODULE hd3d9 = LoadLibraryA("d3d9.dll");
    if (!hd3d9) return;

    uintptr_t base = (uintptr_t)hd3d9;
    typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
    Direct3DCreate9_t pCreate9 = (Direct3DCreate9_t)(void *)GetProcAddress(hd3d9, "Direct3DCreate9");
    if (pCreate9) {
        IDirect3D9 *d3d = pCreate9(D3D_SDK_VERSION);
        if (d3d) {
            void **vt_d3d = *(void ***)d3d;
            uintptr_t create_addr = (uintptr_t)vt_d3d[16];
            if (create_addr > base) {
                *p_create_rva = (uint32_t)(create_addr - base);
            }

            HWND hwnd = CreateWindowExA(0, "STATIC", "xpdash_probe", WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandleA(NULL), NULL);
            D3DPRESENT_PARAMETERS pp;
            memset(&pp, 0, sizeof(pp));
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = hwnd;

            IDirect3DDevice9 *dev = NULL;
            HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
            if (SUCCEEDED(hr) && dev) {
                void **vt = *(void ***)dev;
                uintptr_t present_addr = (uintptr_t)vt[17];
                uintptr_t reset_addr   = (uintptr_t)vt[16];
                if (present_addr > base && reset_addr > base) {
                    *p_present_rva = (uint32_t)(present_addr - base);
                    *p_reset_rva   = (uint32_t)(reset_addr - base);
                }
                dev->lpVtbl->Release(dev);
            }
            if (hwnd) DestroyWindow(hwnd);
            d3d->lpVtbl->Release(d3d);
        }
    }
    agent_log("d3d9hook_init: resolved D3D9 RVAs: Present=0x%lX, Reset=0x%lX, CreateDevice=0x%lX",
              (unsigned long)*p_present_rva, (unsigned long)*p_reset_rva, (unsigned long)*p_create_rva);
}
static void resolve_ddraw_rva(uint32_t *p_flip_rva) {
    *p_flip_rva = 0x399C; /* Standard XP SP3 fallback */

    HMODULE hdd = LoadLibraryA("ddraw.dll");
    if (!hdd) return;

    typedef HRESULT (WINAPI *DirectDrawCreateEx_fn)(GUID *, VOID **, REFIID, IUnknown *);
    static const GUID IID_IDirectDraw7_val = { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x4b } };
    DirectDrawCreateEx_fn pCreateEx = (DirectDrawCreateEx_fn)(void *)GetProcAddress(hdd, "DirectDrawCreateEx");
    if (pCreateEx) {
        LPDIRECTDRAW7 pdd = NULL;
        if (SUCCEEDED(pCreateEx(NULL, (void**)&pdd, &IID_IDirectDraw7_val, NULL)) && pdd) {
            HWND hwnd = CreateWindowExA(0, "STATIC", "xpdash_ddprobe", WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandleA(NULL), NULL);
            pdd->lpVtbl->SetCooperativeLevel(pdd, hwnd, DDSCL_NORMAL);

            DDSURFACEDESC2 ddsd;
            memset(&ddsd, 0, sizeof(ddsd));
            ddsd.dwSize = sizeof(ddsd);
            ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            ddsd.dwWidth = 16;
            ddsd.dwHeight = 16;

            LPDIRECTDRAWSURFACE7 surf = NULL;
            if (SUCCEEDED(pdd->lpVtbl->CreateSurface(pdd, &ddsd, &surf, NULL)) && surf) {
                void **vt = *(void***)surf;
                uintptr_t flip_addr = (uintptr_t)vt[11];
                if (flip_addr > (uintptr_t)hdd) {
                    *p_flip_rva = (uint32_t)(flip_addr - (uintptr_t)hdd);
                }
                surf->lpVtbl->Release(surf);
            }
            pdd->lpVtbl->Release(pdd);
            if (hwnd) DestroyWindow(hwnd);
        }
    }
    agent_log("d3d9hook_init: resolved DirectDraw Flip RVA: 0x%lX", (unsigned long)*p_flip_rva);
}


int d3d9hook_init(void) {
    if (!g_hook_cs_inited) {
        InitializeCriticalSection(&g_hook_cs);
        g_hook_cs_inited = 1;
    }
    EnterCriticalSection(&g_hook_cs);
    g_shm_handle   = NULL;
    g_shm_ptr      = NULL;
    g_frame_event  = NULL;
    g_ack_event    = NULL;
    g_last_seq     = 0;
    g_injected_proc = NULL;
    g_injected_pid  = 0;
    g_remote_dll_base = NULL;

    uint32_t present_rva = 0, reset_rva = 0, create_rva = 0, ddraw_flip_rva = 0;
    resolve_d3d9_rvas(&present_rva, &reset_rva, &create_rva);
    resolve_ddraw_rva(&ddraw_flip_rva);

    /* Pre-create shared memory mapping and write dynamic RVAs so injected hook can read them */
    g_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      0, HOOK_SHM_TOTAL_SIZE, XPDASH_HOOK_SHM_NAME);
    if (g_shm_handle) {
        g_shm_ptr = MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, HOOK_SHM_TOTAL_SIZE);
        if (g_shm_ptr) {
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            hdr->magic = 0x48443344;
            hdr->present_rva       = present_rva;
            hdr->reset_rva         = reset_rva;
            hdr->create_device_rva = create_rva;
            hdr->ddraw_flip_rva    = ddraw_flip_rva;
            hdr->hook_active = 0;
        }
    }
    LeaveCriticalSection(&g_hook_cs);
    return 1;
}

/* Must be called while holding g_hook_cs */
static int open_shm_locked(void) {
    if (!g_shm_handle) {
        g_shm_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, XPDASH_HOOK_SHM_NAME);
        if (!g_shm_handle) return 0;
    }

    if (!g_shm_ptr) {
        g_shm_ptr = MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, HOOK_SHM_TOTAL_SIZE);
        if (!g_shm_ptr) {
            CloseHandle(g_shm_handle);
            g_shm_handle = NULL;
            return 0;
        }
    }

    if (!g_frame_event) {
        g_frame_event = OpenEventA(EVENT_ALL_ACCESS, FALSE, XPDASH_HOOK_EVENT_NAME);
    }
    if (!g_ack_event) {
        g_ack_event   = OpenEventA(EVENT_ALL_ACCESS, FALSE, XPDASH_HOOK_ACK_NAME);
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    return (hdr->magic == 0x48443344);
}

/* Must be called while holding g_hook_cs */
static void close_shm_locked(void) {
    if (g_shm_ptr)      { UnmapViewOfFile(g_shm_ptr); g_shm_ptr = NULL; }
    if (g_shm_handle)   { CloseHandle(g_shm_handle);  g_shm_handle = NULL; }
    if (g_frame_event)  { CloseHandle(g_frame_event);  g_frame_event = NULL; }
    if (g_ack_event)    { CloseHandle(g_ack_event);    g_ack_event = NULL; }
}

void d3d9hook_shutdown(void) {
    if (g_hook_cs_inited) EnterCriticalSection(&g_hook_cs);
    close_shm_locked();
    if (g_injected_proc) {
        CloseHandle(g_injected_proc);
        g_injected_proc = NULL;
        g_injected_pid  = 0;
        g_remote_dll_base = NULL;
    }
    if (g_hook_cs_inited) {
        LeaveCriticalSection(&g_hook_cs);
        DeleteCriticalSection(&g_hook_cs);
        g_hook_cs_inited = 0;
    }
}
/* ─── Process Detection ───────────────────────────────────────────────── */

static int is_blacklisted_process(const char *name) {
    static const char *blacklist[] = {
        "explorer.exe",
        "svchost.exe",
        "services.exe",
        "lsass.exe",
        "csrss.exe",
        "smss.exe",
        "winlogon.exe",
        "spoolsv.exe",
        "alg.exe",
        "wscntfy.exe",
        "ctfmon.exe",
        "rundll32.exe",
        "cmd.exe",
        "tasklist.exe",
        "taskmgr.exe",
        "wmiprvse.exe",
        "xpdash-agent.exe",
        "Launch.exe",
        "RTHDCPL.EXE",
        "CTAudSvc.exe",
        "sqlservr.exe",
        "sqlwriter.exe",
        "uphclean.exe",
        "nvsvc32.exe",
        "gcalsrv.exe",
        "conime.exe",
        "logonui.exe",
        NULL
    };
    for (int i = 0; blacklist[i]; i++) {
        if (lstrcmpiA(name, blacklist[i]) == 0) return 1;
    }
    return 0;
}

typedef struct {
    DWORD pid;
    HWND  hwnd;
} FindProcessWnd;

static BOOL CALLBACK enum_proc_wnd(HWND hwnd, LPARAM lParam) {
    FindProcessWnd *fpw = (FindProcessWnd *)lParam;
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid == fpw->pid && IsWindowVisible(hwnd)) {
        LONG style = GetWindowLongA(hwnd, GWL_STYLE);
        if (!(style & WS_CHILD)) {
            fpw->hwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static int process_has_visible_window(DWORD pid) {
    FindProcessWnd fpw;
    fpw.pid = pid;
    fpw.hwnd = NULL;
    EnumWindows(enum_proc_wnd, (LPARAM)&fpw);
    return (fpw.hwnd != NULL);
}

/* Find the PID of a process that has d3d9.dll loaded.
   This is a heuristic — walk the module list of every process. */
static DWORD find_d3d9_process(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return 0;
    }

    DWORD my_pid = GetCurrentProcessId();
    DWORD found_pid = 0;

    do {
        if (pe.th32ProcessID == my_pid) continue;
        if (pe.th32ProcessID <= 4) continue;  /* system processes */
        if (is_blacklisted_process(pe.szExeFile)) continue;

        /* Verify process is still alive before considering it */
        HANDLE hCheck = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
        if (!hCheck) continue;
        DWORD pcode = 0;
        if (!GetExitCodeProcess(hCheck, &pcode) || pcode != STILL_ACTIVE) {
            CloseHandle(hCheck);
            continue;
        }
        CloseHandle(hCheck);

        /* Direct match for known D3D9 game executables without needing module snapshot */
        /* Direct match for known game executables */
        if (lstrcmpiA(pe.szExeFile, "gta_sa.exe") == 0 ||
            lstrcmpiA(pe.szExeFile, "osu!.exe") == 0 ||
            lstrcmpiA(pe.szExeFile, "SpriteAnimate.exe") == 0) {
            found_pid = pe.th32ProcessID;
            agent_log("d3d9hook: found known game: %s (PID %lu)",
                      pe.szExeFile, (unsigned long)found_pid);
            break;
        }

        /* Check if this process has d3d9.dll loaded and has an active visible window */
        HANDLE modsnap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
        if (modsnap == INVALID_HANDLE_VALUE) continue;

        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        if (Module32First(modsnap, &me)) {
            do {
                if (lstrcmpiA(me.szModule, "d3d9.dll") == 0 ||
                    lstrcmpiA(me.szModule, "ddraw.dll") == 0) {
                    if (process_has_visible_window(pe.th32ProcessID)) {
                        found_pid = pe.th32ProcessID;
                        agent_log("d3d9hook: found graphics process (%s) with visible window: %s (PID %lu)",
                                  me.szModule, pe.szExeFile, (unsigned long)found_pid);
                        break;
                    }
                }
            } while (Module32Next(modsnap, &me) && !found_pid);
        }
        CloseHandle(modsnap);
    } while (Process32Next(snap, &pe) && !found_pid);

    CloseHandle(snap);
    return found_pid;
}

/* ─── DLL Injection ───────────────────────────────────────────────────── */

int d3d9hook_inject(const char *dll_path, DWORD target_pid) {
    if (!g_hook_cs_inited) d3d9hook_init();
    EnterCriticalSection(&g_hook_cs);

    if (g_injected_proc) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_injected_proc, &code) && code == STILL_ACTIVE) {
            /* If the injected process has produced 0 frames for > 5 seconds,
               detach so we can hook active D3D9 games instead of getting wedged. */
            DWORD now = timeGetTime();
            HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
            if (hdr && hdr->producer_seq == 0 && (now - s_inject_time > 5000)) {
                agent_log("d3d9hook_inject: PID %lu produced 0 frames after 5s, detaching",
                          (unsigned long)g_injected_pid);
                close_shm_locked();
                CloseHandle(g_injected_proc);
                g_injected_proc = NULL;
                g_injected_pid  = 0;
                g_remote_dll_base = NULL;
            } else {
                LeaveCriticalSection(&g_hook_cs);
                return 1;
            }
        } else {
            agent_log("d3d9hook_inject: previously injected PID %lu exited, cleaning up",
                      (unsigned long)g_injected_pid);
            close_shm_locked();
            CloseHandle(g_injected_proc);
            g_injected_proc = NULL;
            g_injected_pid  = 0;
            g_remote_dll_base = NULL;
        }
    }
    LeaveCriticalSection(&g_hook_cs);
    if (target_pid == 0) {
        target_pid = find_d3d9_process();
        if (target_pid == 0) {
            /* No D3D9 process found — not an error, just no game running */
            return 0;
        }
    }

    agent_log("d3d9hook_inject: injecting %s into PID %lu",
              dll_path, (unsigned long)target_pid);

    /* Open the target process */
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, target_pid);
    if (!hProc) {
        agent_log("d3d9hook_inject: OpenProcess failed, err=%lu", GetLastError());
        return 0;
    }

    /* Allocate memory in the target process for the DLL path */
    size_t path_len = strlen(dll_path) + 1;
    void *remote_path = VirtualAllocEx(hProc, NULL, path_len,
                                        MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!remote_path) {
        agent_log("d3d9hook_inject: VirtualAllocEx failed, err=%lu", GetLastError());
        CloseHandle(hProc);
        return 0;
    }

    /* Write the DLL path to target process memory */
    if (!WriteProcessMemory(hProc, remote_path, dll_path, path_len, NULL)) {
        agent_log("d3d9hook_inject: WriteProcessMemory failed, err=%lu", GetLastError());
        VirtualFreeEx(hProc, remote_path, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 0;
    }

    /* Get the address of LoadLibraryA in kernel32.dll.
       On Windows XP, kernel32.dll is loaded at the same base address in
       every process (ASLR is not present), so this is safe. */
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLib = GetProcAddress(hKernel32, "LoadLibraryA");

    /* Create a remote thread that calls LoadLibraryA(dll_path) */
    HANDLE hThread = CreateRemoteThread(
        hProc, NULL, 0,
        (LPTHREAD_START_ROUTINE)pLoadLib, remote_path,
        0, NULL);
    if (!hThread) {
        agent_log("d3d9hook_inject: CreateRemoteThread failed, err=%lu", GetLastError());
        VirtualFreeEx(hProc, remote_path, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 0;
    }

    /* Wait for LoadLibraryA to complete */
    WaitForSingleObject(hThread, 5000);

    DWORD exit_code = 0;
    GetExitCodeThread(hThread, &exit_code);
    CloseHandle(hThread);

    /* Cleanup remote memory (LoadLibraryA has already read the string) */
    VirtualFreeEx(hProc, remote_path, 0, MEM_RELEASE);

    if (exit_code == 0) {
        agent_log("d3d9hook_inject: LoadLibraryA returned NULL — hook DLL failed to load");
        CloseHandle(hProc);
        return 0;
    }

    agent_log("d3d9hook_inject: hook DLL loaded at 0x%08lX in PID %lu",
              (unsigned long)exit_code, (unsigned long)target_pid);

    g_injected_proc = hProc;
    g_injected_pid = target_pid;
    g_remote_dll_base = (HMODULE)exit_code;
    s_inject_time = timeGetTime();

    /* Invoke install_d3d9_hooks in the target process to ensure hooks and SHM are active,
       even if the DLL was already mapped into this process from a previous run! */
    HMODULE hLocal = LoadLibraryA(dll_path);
    if (hLocal) {
        FARPROC pLocalFunc = GetProcAddress(hLocal, "install_d3d9_hooks");
        if (pLocalFunc) {
            uintptr_t func_rva = (uintptr_t)pLocalFunc - (uintptr_t)hLocal;
            FARPROC pRemoteFunc = (FARPROC)(exit_code + func_rva);
            HANDLE hHookThread = CreateRemoteThread(hProc, NULL, 0,
                (LPTHREAD_START_ROUTINE)pRemoteFunc, NULL, 0, NULL);
            if (hHookThread) {
                WaitForSingleObject(hHookThread, 3000);
                DWORD hook_exit = 0;
                GetExitCodeThread(hHookThread, &hook_exit);
                CloseHandle(hHookThread);
                agent_log("d3d9hook_inject: install_d3d9_hooks returned %lu", hook_exit);
            }
        }
        FreeLibrary(hLocal);
    }

    /* Try to open shared memory. Retry a few times. */
    EnterCriticalSection(&g_hook_cs);
    g_injected_proc = hProc;
    g_injected_pid = target_pid;
    g_remote_dll_base = (HMODULE)exit_code;
    int retries;
    for (retries = 0; retries < 10; retries++) {
        if (open_shm_locked()) {
            agent_log("d3d9hook_inject: SHM ready after %dms", (retries + 1) * 200);
            break;
        }
        LeaveCriticalSection(&g_hook_cs);
        Sleep(200);
        EnterCriticalSection(&g_hook_cs);
    }
    if (!g_shm_ptr) {
        agent_log("d3d9hook_inject: hook loaded but SHM not available after 2s, will retry on demand");
    }
    LeaveCriticalSection(&g_hook_cs);

    return 1;
}

/* ─── Frame Reading ───────────────────────────────────────────────────── */

int d3d9hook_is_active(void) {
    if (!g_hook_cs_inited) return 0;
    EnterCriticalSection(&g_hook_cs);

    if (!g_injected_proc) {
        if (g_shm_ptr) {
            close_shm_locked();
        }
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Always check if the injected process is still alive FIRST */
    DWORD code = 0;
    if (GetExitCodeProcess(g_injected_proc, &code) && code != STILL_ACTIVE) {
        agent_log("d3d9hook: injected process PID %lu exited (code=%lu)",
                  (unsigned long)g_injected_pid, (unsigned long)code);
        close_shm_locked();
        CloseHandle(g_injected_proc);
        g_injected_proc = NULL;
        g_injected_pid = 0;
        g_remote_dll_base = NULL;
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Try to open SHM if not already open */
    if (!g_shm_ptr) {
        if (!open_shm_locked()) {
            LeaveCriticalSection(&g_hook_cs);
            return 0;
        }
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344 || !hdr->hook_active) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    LeaveCriticalSection(&g_hook_cs);
    return 1;
}
int d3d9hook_has_new_frame(uint32_t *width, uint32_t *height) {
    if (!g_hook_cs_inited) return 0;
    EnterCriticalSection(&g_hook_cs);

    if (!g_shm_ptr && !open_shm_locked()) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344 || !hdr->hook_active) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    if (hdr->producer_seq == g_last_seq || hdr->producer_seq == 0) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    if (width)  *width  = hdr->width;
    if (height) *height = hdr->height;

    LeaveCriticalSection(&g_hook_cs);
    return 1;
}

int d3d9hook_read_frame(uint8_t *pixels, uint32_t max_bytes, uint32_t *width, uint32_t *height,
                        uint32_t *frame_index, DWORD timeout_ms)
{
    if (!g_hook_cs_inited) return 0;

    HANDLE hWaitEvent = NULL;
    EnterCriticalSection(&g_hook_cs);

    if (!g_shm_ptr && !open_shm_locked()) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344 || !hdr->hook_active) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Wait for a new frame if requested and hook has previously produced frames */
    if (hdr->producer_seq == g_last_seq && timeout_ms > 0 && hdr->producer_seq > 0 && g_frame_event) {
        hWaitEvent = g_frame_event;
        ResetEvent(hWaitEvent);
    }
    LeaveCriticalSection(&g_hook_cs);

    /* Wait OUTSIDE the critical section so close_shm can run or other threads aren't blocked */
    if (hWaitEvent) {
        WaitForSingleObject(hWaitEvent, timeout_ms);
    }

    /* Re-acquire critical section to inspect and copy frame safely */
    EnterCriticalSection(&g_hook_cs);

    /* Verify shared memory is still mapped and valid */
    if (!g_shm_ptr) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Re-verify process is still alive */
    if (g_injected_proc) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_injected_proc, &code) && code != STILL_ACTIVE) {
            agent_log("d3d9hook: injected process PID %lu exited during frame wait (code=%lu)",
                      (unsigned long)g_injected_pid, (unsigned long)code);
            close_shm_locked();
            CloseHandle(g_injected_proc);
            g_injected_proc = NULL;
            g_injected_pid = 0;
            g_remote_dll_base = NULL;
            LeaveCriticalSection(&g_hook_cs);
            return 0;
        }
    }

    hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344 || !hdr->hook_active) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Check for new frame */
    uint32_t seq = hdr->producer_seq;
    if (seq == g_last_seq) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    /* Read frame data */
    _ReadWriteBarrier();

    uint32_t w = hdr->width;
    uint32_t h = hdr->height;
    uint32_t data_size = hdr->data_size;

    if (w == 0 || h == 0 || data_size == 0 || data_size > HOOK_SHM_MAX_FRAME) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }
    /* Buffer overflow guard: verify destination buffer is large enough for frame */
    uint32_t final_size = (hdr->format == 16) ? (w * h * 4) : data_size;
    if (final_size > max_bytes) {
        agent_log("d3d9hook_read_frame: frame final_size %u exceeds max_bytes %u (dims %ux%u)",
                  final_size, max_bytes, w, h);
        if (width)  *width  = w;
        if (height) *height = h;
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    uint8_t *src = (uint8_t *)g_shm_ptr + HOOK_SHM_HEADER_SIZE;
    if (hdr->format == 16) {
        /* Unpack 16-bit RGB 565 from host RAM shared memory into 32-bit BGRX in ~0.15ms */
        const uint16_t *src16 = (const uint16_t *)src;
        uint32_t *dst32 = (uint32_t *)pixels;
        uint32_t total = w * h;
        for (uint32_t i = 0; i < total; i++) {
            uint16_t p = src16[i];
            uint32_t r = ((p & 0xF800) >> 8);
            uint32_t g = ((p & 0x07E0) >> 3);
            uint32_t b = ((p & 0x001F) << 3);
            r |= (r >> 5);
            g |= (g >> 6);
            b |= (b >> 5);
            dst32[i] = (r << 16) | (g << 8) | b;
        }
    } else {
        memcpy(pixels, src, data_size);
    }
    *width = w;
    *height = h;
    *frame_index = hdr->frame_index;

    /* Mark as consumed */
    g_last_seq = seq;
    hdr->consumer_seq = seq;
    if (g_ack_event) SetEvent(g_ack_event);

    LeaveCriticalSection(&g_hook_cs);
    return 1;
}

int d3d9hook_get_dimensions(uint32_t *width, uint32_t *height) {
    if (!g_hook_cs_inited) return 0;
    EnterCriticalSection(&g_hook_cs);

    if (!g_shm_ptr && !open_shm_locked()) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    *width  = hdr->width;
    *height = hdr->height;
    int valid = (*width > 0 && *height > 0);
    LeaveCriticalSection(&g_hook_cs);
    return valid;
}
