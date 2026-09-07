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

static HANDLE  g_shm_handle   = NULL;
static void   *g_shm_ptr      = NULL;
static HANDLE  g_frame_event  = NULL;
static HANDLE  g_ack_event    = NULL;
static uint32_t g_last_seq    = 0;
static HANDLE  g_injected_proc = NULL;
static DWORD   g_injected_pid  = 0;

/* ─── Init / Shutdown ─────────────────────────────────────────────────── */

int d3d9hook_init(void) {
    g_shm_handle   = NULL;
    g_shm_ptr      = NULL;
    g_frame_event  = NULL;
    g_ack_event    = NULL;
    g_last_seq     = 0;
    g_injected_proc = NULL;
    g_injected_pid  = 0;
    agent_log("d3d9hook_init: ready");
    return 1;
}

static int open_shm(void) {
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

static void close_shm(void) {
    if (g_shm_ptr)      { UnmapViewOfFile(g_shm_ptr); g_shm_ptr = NULL; }
    if (g_shm_handle)   { CloseHandle(g_shm_handle);  g_shm_handle = NULL; }
    if (g_frame_event)  { CloseHandle(g_frame_event);  g_frame_event = NULL; }
    if (g_ack_event)    { CloseHandle(g_ack_event);    g_ack_event = NULL; }
}

void d3d9hook_shutdown(void) {
    close_shm();
    if (g_injected_proc) {
        CloseHandle(g_injected_proc);
        g_injected_proc = NULL;
    }
    g_injected_pid = 0;
    agent_log("d3d9hook_shutdown: cleaned up");
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
    if (g_injected_proc) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_injected_proc, &code) && code == STILL_ACTIVE) {
            return 1;
        }
        agent_log("d3d9hook_inject: previously injected PID %lu exited, cleaning up",
                  (unsigned long)g_injected_pid);
        close_shm();
        CloseHandle(g_injected_proc);
        g_injected_proc = NULL;
        g_injected_pid = 0;
    }

    /* Auto-detect if no PID given */
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

    /* Try to open shared memory — the hook DLL creates SHM in DllMain
       immediately, but the hook installation is deferred to a background
       thread (500ms delay for loader lock + device creation). Retry a
       few times. */
    int retries;
    for (retries = 0; retries < 10; retries++) {
        Sleep(200);
        if (open_shm()) {
            agent_log("d3d9hook_inject: SHM ready after %dms", (retries + 1) * 200);
            break;
        }
    }
    if (!g_shm_ptr) {
        agent_log("d3d9hook_inject: hook loaded but SHM not available after 2s, will retry on demand");
    }

    return 1;
}

/* ─── Frame Reading ───────────────────────────────────────────────────── */

int d3d9hook_is_active(void) {
    /* Always check if the injected process is still alive FIRST */
    if (g_injected_proc) {
        DWORD code = 0;
        if (GetExitCodeProcess(g_injected_proc, &code) && code != STILL_ACTIVE) {
            agent_log("d3d9hook: injected process PID %lu exited (code=%lu)",
                      (unsigned long)g_injected_pid, (unsigned long)code);
            close_shm();
            CloseHandle(g_injected_proc);
            g_injected_proc = NULL;
            g_injected_pid = 0;
            return 0;
        }
    }

    /* Try to open SHM if not already open */
    if (!g_shm_ptr) {
        if (!open_shm()) return 0;
    }

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344) return 0;
    if (!hdr->hook_active) return 0;

    return 1;
}

int d3d9hook_read_frame(uint8_t *pixels, uint32_t *width, uint32_t *height,
                        uint32_t *frame_index, DWORD timeout_ms)
{
    if (!g_shm_ptr && !open_shm()) return 0;

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;

    /* Wait for a new frame if requested */
    if (hdr->producer_seq == g_last_seq && timeout_ms > 0 && g_frame_event) {
        ResetEvent(g_frame_event);
        /* Double-check after reset to avoid race */
        if (hdr->producer_seq == g_last_seq) {
            WaitForSingleObject(g_frame_event, timeout_ms);
        }
    }

    /* Check for new frame */
    uint32_t seq = hdr->producer_seq;
    if (seq == g_last_seq) return 0;

    /* Read frame data */
    _ReadWriteBarrier();

    uint32_t w = hdr->width;
    uint32_t h = hdr->height;
    uint32_t data_size = hdr->data_size;

    if (w == 0 || h == 0 || data_size == 0) return 0;
    if (data_size > HOOK_SHM_MAX_FRAME) return 0;

    uint8_t *src = (uint8_t *)g_shm_ptr + HOOK_SHM_HEADER_SIZE;
    memcpy(pixels, src, data_size);

    *width = w;
    *height = h;
    *frame_index = hdr->frame_index;

    /* Mark as consumed */
    g_last_seq = seq;
    hdr->consumer_seq = seq;
    if (g_ack_event) SetEvent(g_ack_event);

    return 1;
}

int d3d9hook_get_dimensions(uint32_t *width, uint32_t *height) {
    if (!g_shm_ptr && !open_shm()) return 0;

    HookShmHeader *hdr = (HookShmHeader *)g_shm_ptr;
    if (hdr->magic != 0x48443344) return 0;

    *width  = hdr->width;
    *height = hdr->height;
    return (*width > 0 && *height > 0);
}
