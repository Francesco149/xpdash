# xpdash Agent & Architecture Guidelines

## 1. DirectX SDK & Graphics Hook Gotchas (Silent Performance Killers)

- **DirectX SDK Shell Extensions (`txview.dll`)**:
  Installing legacy DirectX SDKs (such as DirectX 9.0c Summer 2004) registers `txview.dll` in `explorer.exe`, loading `d3d9.dll` into the Windows Explorer shell.
  - **The Bug**: Blind heuristic process scanning for `d3d9.dll` will inject the hook into `explorer.exe`. Since Explorer never calls `Present()`, the hook frame read times out (e.g. 16ms/frame), silently capping streaming framerate at ~40 FPS instead of 60+ FPS, and prevents hooking the real game process.
  - **The Safeguards**:
    1. `find_d3d9_process()` in `d3d9hook.c` **must blacklist** system and shell processes (`explorer.exe`, `svchost.exe`, `services.exe`, `lsass.exe`, `csrss.exe`, `smss.exe`, `winlogon.exe`, etc.).
    2. Any non-hardcoded game process **must own an active visible top-level window** (`process_has_visible_window`) before injection.
    3. Inactivity detachment: if an injected process produces 0 frames for > 5 seconds, the hook subsystem must detach automatically.
    4. Non-blocking timeout: `d3d9hook_read_frame()` must never block with a positive timeout if `!s_in_hook_mode` or `producer_seq == 0`.

## 2. Window Messages & Broadcast Delivery on Windows XP

- **Message-Only Windows (`HWND_MESSAGE`)**:
  A window created with `HWND_MESSAGE` parent **never receives broadcast messages** (`HWND_BROADCAST`), including `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE`, and `WM_POWERBROADCAST`.
  - To receive `WM_DISPLAYCHANGE` without showing a window on the taskbar, create an unowned popup window:
    `CreateWindowA("class", "name", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL)`
    Do **not** call `ShowWindow()`. It remains invisible while still receiving all OS broadcast messages.

## 3. Dynamic Resolution Adaptation & Concurrency Protection

- **Buffer Overflow Protection**:
  When a fullscreen application changes resolution (e.g. 640×480 → 800×600), the D3D9 backbuffer size in shared memory increases from 1.22 MB to 1.92 MB.
  - `d3d9hook_read_frame()` must accept a `max_bytes` parameter and refuse to copy if `data_size > max_bytes`, returning the new dimensions so caller can reallocate before copying.
- **Thread Synchronization**:
  `video_resize()` (called on the main thread via `WM_DISPLAYCHANGE` or on the worker thread) and `video_capture()` (running on `video_worker_thread`) **must be synchronized via recursive `CRITICAL_SECTION g_video_cs`**. Reallocating DIBSections and freeing `g_prev_pixels` concurrently with frame capture causes access violations (`0xC0000005`).
- **Hook vs Desktop Oscillation**:
  Path A (D3D9 hook) must only resize and adopt hook dimensions when a new frame is confirmed ready in shared memory (`d3d9hook_has_new_frame()`). Otherwise, Path A and Path B will oscillate back and forth every frame between game resolution and desktop resolution.

## 4. UDP Input Fast-Path & Socket Rebinding

- **Port 7021 / `SO_REUSEADDR`**:
  `g_sock_udp` on the agent must set `SO_REUSEADDR` and retry `bind()` if `WSAEADDRINUSE (10048)` is encountered during quick restarts. If unbound, the socket will silently assign an ephemeral port and drop all incoming client inputs.
- **Absolute vs Relative Mouse Input**:
  `INPUT_TYPE_MOUSE_ABS` must always call `SetCursorPos(px, py)` regardless of `CURSOR_SHOWING`. 2D games like `osu!` hide the system cursor icon via `ShowCursor(FALSE)` and render custom sprites, but still rely on Windows absolute cursor coordinates.
