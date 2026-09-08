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

/* ─── Init / Shutdown ─────────────────────────────────────────────────── */

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
    LeaveCriticalSection(&g_hook_cs);
    return 1;
}

/* Must be called while holding g_hook_cs */
static int open_shm_locked(void) {
    if (g_shm_ptr) return 1;  /* already open */

    g_shm_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
                                     XPDASH_HOOK_SHM_NAME);
    if (!g_shm_handle) return 0;

    g_shm_ptr = MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS,
                              0, 0, HOOK_SHM_TOTAL_SIZE);
    if (!g_shm_ptr) {
        CloseHandle(g_shm_handle);
        g_shm_handle = NULL;
        return 0;
    }

    g_frame_event = OpenEventA(EVENT_ALL_ACCESS, FALSE, XPDASH_HOOK_EVENT_NAME);
    g_ack_event   = OpenEventA(EVENT_ALL_ACCESS, FALSE, XPDASH_HOOK_ACK_NAME);

    if (!g_frame_event) {
        agent_log("d3d9hook: opened SHM but no frame event");
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    agent_log("d3d9hook: SHM opened, magic=0x%08lX, active=%lu",
              (unsigned long)hdr->magic, (unsigned long)hdr->hook_active);
    return 1;
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
        /* Direct match for known D3D9 game executables without needing module snapshot */
        if (lstrcmpiA(pe.szExeFile, "gta_sa.exe") == 0) {
            found_pid = pe.th32ProcessID;
            agent_log("d3d9hook: found known D3D9 game: %s (PID %lu)",
                      pe.szExeFile, (unsigned long)found_pid);
            break;
        }

        /* Check if this process has d3d9.dll loaded */
        HANDLE modsnap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
        if (modsnap == INVALID_HANDLE_VALUE) continue;

        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        if (Module32First(modsnap, &me)) {
            do {
                if (lstrcmpiA(me.szModule, "d3d9.dll") == 0) {
                    found_pid = pe.th32ProcessID;
                    agent_log("d3d9hook: found D3D9 process: %s (PID %lu)",
                              pe.szExeFile, (unsigned long)found_pid);
                    break;
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
            LeaveCriticalSection(&g_hook_cs);
            return 1;
        }
        agent_log("d3d9hook_inject: previously injected PID %lu exited, cleaning up",
                  (unsigned long)g_injected_pid);
        close_shm_locked();
        CloseHandle(g_injected_proc);
        g_injected_proc = NULL;
        g_injected_pid  = 0;
        g_remote_dll_base = NULL;
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
    /* Try to open shared memory — the hook DLL creates SHM in DllMain
       immediately, but the hook installation is deferred to a background
       thread (500ms delay for loader lock + device creation). Retry a
       few times. */
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

static void d3d9hook_try_rehook(void) {
    if (!g_injected_proc || !g_remote_dll_base) return;

    HMODULE hLocal = LoadLibraryA("C:\\xpdash\\xpdash-hook9.dll");
    if (!hLocal) hLocal = LoadLibraryA("C:\\xpdash\\xpdash-hook.dll");
    if (!hLocal) return;

    FARPROC pFn = GetProcAddress(hLocal, "install_d3d9_hooks");
    if (pFn) {
        uintptr_t offset = (uintptr_t)pFn - (uintptr_t)hLocal;
        LPTHREAD_START_ROUTINE pRemote = (LPTHREAD_START_ROUTINE)((uintptr_t)g_remote_dll_base + offset);
        HANDLE hThread = CreateRemoteThread(g_injected_proc, NULL, 0, pRemote, NULL, 0, NULL);
        if (hThread) {
            WaitForSingleObject(hThread, 1500);
            DWORD code = 0;
            GetExitCodeThread(hThread, &code);
            CloseHandle(hThread);
            agent_log("d3d9hook: remote install_d3d9_hooks returned %lu", (unsigned long)code);
        }
    }
    FreeLibrary(hLocal);
}

/* ─── Frame Reading ───────────────────────────────────────────────────── */

int d3d9hook_is_active(void) {
    if (!g_hook_cs_inited) return 0;
    EnterCriticalSection(&g_hook_cs);

    /* Always check if the injected process is still alive FIRST */
    if (g_injected_proc) {
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
    }

    /* Try to open SHM if not already open */
    if (!g_shm_ptr) {
        if (!open_shm_locked()) {
            LeaveCriticalSection(&g_hook_cs);
            return 0;
        }
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344) {
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }
    if (!hdr->hook_active) {
        static DWORD s_last_rehook_try = 0;
        DWORD now = timeGetTime();
        if (g_injected_proc && now - s_last_rehook_try >= 1500) {
            s_last_rehook_try = now;
            d3d9hook_try_rehook();
        }
        LeaveCriticalSection(&g_hook_cs);
        return 0;
    }

    LeaveCriticalSection(&g_hook_cs);
    return 1;
}

int d3d9hook_read_frame(uint8_t *pixels, uint32_t *width, uint32_t *height,
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

    /* Wait for a new frame if requested */
    if (hdr->producer_seq == g_last_seq && timeout_ms > 0 && g_frame_event) {
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

    uint8_t *src = (uint8_t *)g_shm_ptr + HOOK_SHM_HEADER_SIZE;
    memcpy(pixels, src, data_size);

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
