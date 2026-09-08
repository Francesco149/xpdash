#ifndef XPDASH_D3D9HOOK_H
#define XPDASH_D3D9HOOK_H

#include <windows.h>
#include <stdint.h>

/* Shared memory header — must match d3d9hook_dll.c exactly */
#define XPDASH_HOOK_SHM_NAME   "XpDashHookShm"
#define XPDASH_HOOK_EVENT_NAME "XpDashHookFrameReady"
#define XPDASH_HOOK_ACK_NAME   "XpDashHookFrameAck"

#define HOOK_SHM_HEADER_SIZE   64
#define HOOK_SHM_MAX_FRAME     (10 * 1024 * 1024)
#define HOOK_SHM_TOTAL_SIZE    (HOOK_SHM_HEADER_SIZE + HOOK_SHM_MAX_FRAME)

typedef struct {
    volatile uint32_t magic;          /* 0x48443344 = 'D3DH' */
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t stride;
    volatile uint32_t frame_index;
    volatile uint32_t data_size;
    volatile uint32_t format;
    volatile uint32_t producer_seq;
    volatile uint32_t consumer_seq;
    volatile uint32_t hook_active;
    volatile uint32_t present_rva;
    volatile uint32_t reset_rva;
    volatile uint32_t create_device_rva;
    uint8_t           reserved[12];
} HookShmHeader;

/* Initialize the hook subsystem (does not inject yet) */
int d3d9hook_init(void);

/* Inject xpdash-hook.dll into a target process.
   dll_path: full path to xpdash-hook.dll on the XP filesystem.
   target_pid: PID of the D3D9 game process (0 = auto-detect).
   Returns 1 on success. */
int d3d9hook_inject(const char *dll_path, DWORD target_pid);

/* Check if the hook is active and producing frames */
int d3d9hook_is_active(void);

/* Wait for and read the next frame from the hook.
   pixels: output buffer (must be at least width*height*4 bytes)
   width, height: output dimensions
   frame_index: output frame counter
   timeout_ms: how long to wait (0 = no wait, check only)
   Returns 1 if a new frame was read, 0 if no new frame available. */
int d3d9hook_read_frame(uint8_t *pixels, uint32_t *width, uint32_t *height,
                        uint32_t *frame_index, DWORD timeout_ms);

/* Get the current frame dimensions without reading pixels */
int d3d9hook_get_dimensions(uint32_t *width, uint32_t *height);

/* Cleanup: detach from shared memory, but does NOT unload the DLL
   from the game (that happens when the game exits). */
void d3d9hook_shutdown(void);

#endif /* XPDASH_D3D9HOOK_H */
