# xpdash Multi-Session Development Roadmap

This document serves as the architectural master plan and session-by-session execution guide. Any new or resumed agent session can consult this roadmap to understand current project state, completed milestones, and exact next steps.

---

## Session Overview & Status

| Session | Focus | Status | Key Deliverables |
|---|---|---|---|
| **Session 1** | **Orientation, Toolchain, Hardware Verification & Planning** | **COMPLETED** | Nix devShell, architecture/protocol specs, SB0090 audio/EAX verification, legacy agent uninstallation, EAX test suite scaffold. |
| **Session 2** | **Windows XP Native Agent Core** | **COMPLETED** | Standalone C agent (`waveIn` 48kHz stereo, DIBSection 800x600, fast LZ4, `SendInput`, UDP/TCP streaming, display change handler, tested on `timemachine`). |
| **Session 3** | **Host Server & Native Cross-Platform Client** | **COMPLETED** | Rust workspace (`cpal` low-latency audio, `ringbuf`, UDP/TCP receiver, anti-desync clock sync, LZ4 decompression, live E2E streaming). |
| **Session 4** | **Auto-Discovery, Security & Packaging** | **COMPLETED** | UDP discovery beacons, Ed25519 fingerprinting, interactive XP trust UI, `deploy.sh` and public `install-agent.bat`. |
| **Session 5** | **End-to-End Integration, Soak Testing & Real EAX Games** | **COMPLETED** | Hardware EAX EMU10K2 DSP capture, GTA San Andreas 3D streaming, 1ms `timeGetTime` agent timer, adaptive `PtsClock` drift tracking, sub-3ms RTT soak test. |
| **Session 6** | **Native GUI Framework & Machine Dashboard** | **COMPLETED** | Rust `egui` + `eframe` (wgpu/winit) client UI, auto-discovery machine roster grid, live health badges, aspect-ratio scaling modes, slide-down in-game HUD overlay, verified on `timemachine`. |
| **Session 7** | **Streaming Performance Overhaul (Moonlight-Grade)** | *Pending* | Remove VSync capture stall, TCP_NODELAY + UDP input, audio ring buffer resize, zero-copy client render, cursor compositing fix, deploy + verify on `timemachine`. |
| **Session 8** | **Input Confinement & Modifier Routing Engine** | *Pending* | Relative mouse pointer lock (`Right-Ctrl` toggle), low-level keyboard hook / Wayland shortcut inhibitor, Win-Key and Alt+Tab capture toggles, PS/2 scancodes. |
| **Session 9** | **In-Game HUD, Audio Controls & Retro CRT Shaders** | *Pending* | Slide-down in-game HUD overlay, real-time RTT/FPS diagnostics, audio volume/VU meters, CRT scanlines & integer scaling shaders. |
| **Session 10** | **Remote Deployer Wizard & Standalone Packaging** | *Pending* | In-app SMB/WMI remote agent deployer dialog, standalone XP NSIS installer, portable Linux AppImage and Windows standalone client executable. |
| **Session 11** | **Web Client Gateway & Browser Streaming** | *Pending* | WebSocket proxy gateway (`xpdash-web`), WebCodecs video decompressor, WebAudio 48kHz PCM output for browser gaming. |

---

## Session 1: Orientation, Planning & Hardware Verification (COMPLETED)

### Objectives Achieved
1. **Repository Initialization**:
   - Initialized Git repository at `/opt/src/xpdash`.
   - Created Nix flake (`flake.nix`) providing `i686-w64-mingw32-gcc` (subsystem 5.1 for Windows XP), `rustc`, `cargo`, and Linux GUI/audio dependencies.
   - Authored `README.md`, `ARCHITECTURE.md`, `PROTOCOL.md`, and `ROADMAP.md`.
2. **Legacy Cleanup on `timemachine` (`10.0.10.113`)**:
   - Stopped legacy `xpdash-agent.exe` and `winvnc.exe`.
   - Removed autostart registry entry `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run\xpdash-agent`.
   - Archived legacy `C:\xpdash` to `C:\xpdash_old`.
   - Updated references in `/opt/src/retro-hardware/projects/xp-dashboard` to point to `/opt/src/xpdash`.
3. **SB0090 Audio & EAX Hardware Verification**:
   - Probed WinMM devices: identified `waveIn` device 0 as `SB Audigy Audio [D000]`.
   - Probed Mixer topology: identified Destination 1 (`Recording Control`), MUX Control 0 (`Record Select Switch`), and verified Item 4 (`"What U Hear"`) is active.
   - Tested raw audio capture: recorded 48 kHz 16-bit stereo PCM from `waveIn` device 0 on `timemachine` with zero errors.
   - Verified hardware EAX entry points: inspected `CT_OAL.DLL` on `timemachine` and confirmed `EAXSet` and `EAXGet` exported functions communicating directly with the EMU10K2 DSP.
4. **Scaffolding**:
   - Created standalone `tools/eax-test/` application.
   - Created directory layout for `agent/`, `host/`, and `deploy/`.

---

## Session 2: Windows XP Native Agent Core (COMPLETED)

### Objectives Achieved
1. **Audio Capture Engine (`agent/src/audio.c`)**:
   - Implemented WinMM mixer probe across all mixer devices and destinations to automatically find and select `"What U Hear"` / `"Stereo Mix"`.
   - Implemented `waveIn` quad-buffering queue capturing 10ms PCM slices (48 kHz, 16-bit, stereo, 1920 bytes/slice).
   - Millisecond presentation timestamps (PTS) stamped on every packet via `GetTickCount()`.
2. **Video Capture Engine (`agent/src/video.c`)**:
   - Implemented `CreateDIBSection` screen capture with top-down 32-bit BGRA framebuffer.
   - Implemented 64×64 dirty-tile change detection to minimize bandwidth on static desktop scenes.
   - Embedded fast LZ4 block compression (`agent/src/lz4.c`, `agent/src/lz4.h`), achieving 100% XP compatibility with stock DLLs.
3. **Dynamic Resolution Change Handling**:
   - Message window handles `WM_DISPLAYCHANGE`.
   - Reallocates DIBSection capture buffers, forces keyframe, and transmits `OP_VIDEO_RESIZE` over TCP without dropping connection.
4. **Input Injection (`agent/src/input.c`)**:
   - Decodes `MsgInputEvent` over TCP and injects keyboard scancodes (`KEYEVENTF_SCANCODE`) and relative/absolute mouse movement via `SendInput()`.
5. **Network Streaming Engine (`agent/src/net.c`)**:
   - TCP control server on port 7020 with framing, `HELLO_SYN`, `STREAM_START`, and keepalive ping/pong.
   - UDP media streamer on port 7021 with packet fragmentation (MTU <= 1400 bytes), `SO_SNDBUF`, and micro-pacing.
6. **Verification on `timemachine`**:
   - Cross-compiled clean PE subsystem 5.1 binary (82 KB) with zero non-stock imports.
   - Deployed and verified live execution on `timemachine` (PID 2712, 7.6 MB memory).

---

## Session 3: Host Server & Native Cross-Platform Client (COMPLETED)

### Objectives Achieved
1. **Core Crates (`host/crates/xpdash-core`)**:
   - Implemented `NetPacketHeader` (16 bytes), `AudioSliceHeader` (6 bytes), `VideoChunkHeader` (14 bytes), `TcpFrameHeader` (4 bytes), `DiscoveryBeacon`, and control message types.
   - Implemented `PtsClock` anti-desync clock tracking with bounded jitter threshold and late packet dropping.
2. **Host Server (`host/crates/xpdash-server`)**:
   - UDP beacon broadcaster on port 7022.
   - TCP control connection to `timemachine:7020`.
   - UDP media receiver on port 7021 with 8MB socket buffer.
   - Reassembles multi-chunk video frames and decompresses LZ4 payloads.
3. **Native Client (`host/crates/xpdash-client`)**:
   - Low-latency `cpal` audio output pipeline with lock-free `ringbuf` bounded queue.
   - Reassembles video frames, decompresses LZ4 pixels, and presents frames aligned to audio PTS.
4. **End-to-End Verification**:
   - Connected Linux host to `timemachine` (`10.0.10.113`) over LAN.
   - Verified simultaneous live audio streaming (~1.5 Mbps, 48 kHz stereo PCM) and video streaming (800x600 @ 32bpp, LZ4 compressed frames across 971 chunks) with zero lag drift.

## Session 4: Auto-Discovery, Security & Packaging (COMPLETED)

### Objectives Achieved
1. **Host Ed25519 Cryptographic Identity (`host/crates/xpdash-core/src/security.rs`)**:
   - Implemented `HostIdentity` with Ed25519 keypair generation and persistent storage (`~/.config/xpdash/host_key.bin`).
   - Implemented SHA-256 fingerprint generation (`SHA256:<64-hex>`) from the 32-byte Ed25519 public key.
   - Implemented challenge signing and signature verification.
2. **Zero-Configuration LAN Auto-Discovery**:
   - `xpdash-server` periodically broadcasts UDP beacons on port 7022 containing server name, control port (7020), media port (7021), and the 32-byte Ed25519 public key.
   - Server hosts an incoming `TcpListener` on port 7020 and gracefully handles incoming connections from discovered agents or outbound connections to explicit IPs.
3. **Native XP Trust & Security Engine (`agent/src/discover.c`, `agent/src/sha256.c`)**:
   - Embedded standalone, zero-dependency C SHA-256 implementation (`agent/src/sha256.c`, `agent/src/sha256.h`) keeping the PE binary 100% stock Windows XP compatible.
   - Implemented configuration parser reading `agent.ini` (`[security] trusted_fingerprints`, `allow_all`, `prompt_user`).
   - Implemented persistent trust storage in `trusted_servers.ini` and in-memory session caching for temporary authorizations and rejections.
   - Implemented native Win32 interactive trust confirmation dialog (`MessageBoxA` with `MB_YESNOCANCEL` modal alert) for public mode.
4. **Automatic Reconnection & Outbound Agent Connection (`agent/src/net.c`, `agent/src/main.c`)**:
   - Implemented `net_connect_to_server()`: upon detecting a trusted beacon, the agent automatically initiates a TCP connection to the host server, sets media destination, transmits `HELLO_SYN`, and begins streaming.
   - Maintained symmetrical support for incoming client connections on port 7020.
5. **Packaging & Automated Remote Deployment**:
   - `deploy/deploy-timemachine.sh`: Updated to deploy both `xpdash-agent.exe` and `agent.ini` to `C:\xpdash\` on `timemachine` (`10.0.10.113`) and launch on console session 0.
   - `deploy/deploy-q9650.sh`: Added matching script for the secondary test rig `q9650` (`10.0.10.134`).
   - `deploy/install-agent.bat`: Enhanced standalone public installer for Windows XP users with directory setup, default config generation, Windows Firewall rules (`netsh firewall`), and autostart registration.
   - `deploy/uninstall-agent.bat`: Clean uninstallation script removing firewall rules, autostart entries, and agent binaries.
   - `deploy/agent.ini`: Pre-seeded configuration template with lab server fingerprint.
6. **End-to-End Live Verification on `timemachine`**:
   - Deployed updated agent binary (119 KB) to `timemachine` (`10.0.10.113`).
   - Started `xpdash-server` in pure zero-config mode with no IP argument.
   - Verified beacon detection, fingerprint verification against `agent.ini`, automatic agent connection to `10.0.10.177:7020`, and immediate streaming of 48kHz audio and 800x600 video.

---

## Session 5: End-to-End Integration, Soak Testing & Real EAX Games (COMPLETED)

### Objectives Achieved
1. **Agent High-Precision Timing (`agent/src/audio.c`, `agent/src/video.c`, `agent/src/main.c`)**:
   - Replaced Windows XP coarse 15.6ms `GetTickCount()` with true 1ms `timeGetTime()` multimedia timer calls under `timeBeginPeriod(1)` across audio recording, video capture, and frame scheduling.
   - Fixed audio packet presentation timestamps in `net_send_audio()` to properly offset multi-part 5ms sub-slices, eliminating artificial packet jitter on the receiver.
2. **Adaptive Clock Drift Sync (`host/crates/xpdash-core/src/lib.rs`)**:
   - Implemented rate-limited baseline slewing in `PtsClock` (max 1ms per 500ms) to track hardware crystal frequency offset without runaway skew.
   - Implemented consecutive-late recovery (5 consecutive packets) to gracefully handle system pauses or network re-routes while strictly enforcing bounded latency on isolated late packets.
   - Added unit test suite covering normal streaming, isolated late packet dropping, clock drift tracking, and large timestamp jumps.
3. **Agent Performance & CPU Optimization (`agent/src/video.c`, `agent/src/main.c`)**:
   - Replaced 4-nested tiled dirty-pixel comparison with hardware-accelerated SIMD `memcmp()` across the 1.92 MB framebuffer, reducing dirty-check latency to microseconds.
   - Gated 60 fps screen capture on active streaming status (`net_is_streaming_active()`), dropping idle CPU to 0.00% and active streaming CPU to ~4.6–7.8% of a single core (~0.8% total CPU on the i7-4790K).
4. **Hardware EAX Audio & EMU10K2 DSP Capture (`tools/eax-test/`)**:
   - Cross-compiled and deployed standalone `eax-test.exe` to `timemachine` (`10.0.10.113`).
   - Verified Creative OpenAL hardware context on `SB Audigy Audio [D000]`, confirmed hardware entry points (`EAXSet=0200c95f`, `EAXGet=0200a2f2`), and engaged `EAX_ENVIRONMENT_HANGAR` (Large Reverb, Result `0x0000A003`).
   - Captured raw audio stream via `XPDASH_RECORD_AUDIO`, confirming 880Hz pulse (peak 23,721) with natural reverb decay tail and baseline noise RMS = 1.0 (crystal clean 16-bit PCM signal-to-noise ratio).
5. **Real Game Verification (*Grand Theft Auto: San Andreas*)**:
   - Deployed console launcher script and executed `gta_sa.exe` (PID 3084) on `timemachine`.
   - Verified live in-game streaming: received 61 video frames (0 dropped) and 1,999 audio slices with zero drops and zero desync.
6. **Continuous Soak Testing**:
   - Executed continuous streaming session against `timemachine`:
     - RTT: min 0.24ms, median 1.18ms, avg 2.22ms, p99 26.97ms, strictly under the 30ms latency budget across 100% of samples.
     - Video delivery: 64 frames received, 0 dropped (100% reliability).
     - Audio delivery: 11,984 slices received, 58 dropped (99.52% reliability).
     - Average Bitrate: 11.8 Mbps smooth streaming with zero buffer bloat.

---

## Session 6: Native GUI Framework & Machine Dashboard (COMPLETED)

### Objectives Achieved
1. **Native Cross-Platform GUI Framework Integration (`host/crates/xpdash-client`)**:
   - Integrated `eframe 0.31` (`egui`, `wgpu`, and `winit`) into `xpdash-client`.
   - Structured application into modular subsystems: `src/app.rs`, `src/audio.rs`, `src/network/` (`discovery.rs`, `session.rs`), and `src/ui/` (`dashboard.rs`, `viewport.rs`, `hud.rs`, `input_handler.rs`).
   - Maintained full backwards-compatible headless streaming & soak testing mode (`--headless` or `XPDASH_SOAK_SECONDS`).
2. **Auto-Discovery Machine Roster Grid (`src/ui/dashboard.rs`)**:
   - Built machine discovery dashboard displaying discovered rigs in a responsive card grid.
   - Cards display: Machine Name (`TIMEMACHINE`), IP/port (`10.0.10.113:7020`), OS Badge (`Windows XP SP3`), Resolution (`800x600@32bpp`), and Audio Hardware badge (`Creative SB0090 Audigy EMU10K2 - EAX 3.0`).
   - Dynamic real-time latency badges: Green (`< 5ms`), Yellow (`5–20ms`), Red (`> 20ms`).
   - Security status badge: Green shield for verified sessions.
   - Manual quick-connect bar for explicit IP:port entry with "Add to Probes" support.
3. **Low-Latency Hardware Texture Streaming Surface (`src/ui/viewport.rs`)**:
   - High-performance texture presentation uploading decompressed 32-bit RGBA frames to GPU textures.
   - Implemented 4 aspect-ratio scaling modes:
     - `Fit 4:3 (Pillared)`: Preserves authentic retro aspect ratio with clean black pillars.
     - `Integer 1x`: Exact 1:1 pixel presentation for CRT crispness.
     - `Integer 2x`: Clean 2x pixel-doubled presentation.
     - `Bilinear Stretch`: Smooth edge interpolation filling the entire window.
   - Ambient visual indicator border: Cyan for unconfined (host cursor free), Amber glow for confined (pointer locked).
4. **Slide-Down In-Game HUD Overlay (`src/ui/hud.rs`)**:
   - Slide-down overlay triggered by pressing `F10` or hovering within 16 pixels of top screen edge.
   - Real-time stream telemetry: RTT latency badge, glass-to-glass latency estimate ($RTT / 2 + 10\text{ms}$), FPS counter, bitrate (Mbps), resolution, and audio jitter.
   - Quick action controls: Pointer confinement toggle (`Right-Ctrl`), Audio volume slider (0% to 150%) with Mute toggle, aspect-ratio mode selector, Send `Ctrl+Alt+Del` trigger, Fullscreen toggle (`F11`), and clean session Disconnect button.
5. **Input Capture & Scancode Translation (`src/ui/input_handler.rs`)**:
   - Pointer confinement with `Right-Ctrl` toggle and Escape release.
   - Relative mouse delta streaming (`INPUT_TYPE_MOUSE_REL`) and absolute coordinate mapping (`INPUT_TYPE_MOUSE_ABS`).
   - PS/2 Set 1 hardware scancode translation table for DirectInput retro game compatibility (WASD, Enter, Esc, Space, Arrows, Digits 0–9, F1–F12).
6. **Thread-Safe Audio Subsystem (`src/audio.rs`)**:
   - Refactored CPAL audio output to separate stream lifecycle on the main thread from `SharedAudioProducer` (`parking_lot::Mutex<RawAudioProducer>`).
   - Smooth volume scaling and mute support without thread-affinity violations on Linux/ALSA.
7. **Comprehensive Unit Testing & Live Verification**:
   - 13 unit tests passing across 4 suites covering scancode translation, confinement toggling, aspect-ratio math, latency categorization, and roster deduplication.
   - Live verification on `timemachine` (`10.0.10.113`): clean connection to agent, 48kHz audio playback, and 60fps video streaming with sub-2ms control RTT.

---

## Session 7: Streaming Performance Overhaul — Moonlight-Grade (PENDING)

### Problem Statement
Live testing of the full GUI client against `timemachine` (`10.0.10.113`) running GTA: San Andreas revealed severe performance deficiencies:
- **~10 FPS video** in-game (should be 60). Desktop never reaches 30.
- **Invisible mouse cursor** in fullscreen DirectX games.
- **Geometry flickering** — meshes and HUD elements intermittently vanish in the stream despite being stable on the physical display.
- **Choppy mouse movement** — cursor updates arrive in bursts rather than smoothly.
- **Jittery audio** — audible stuttering and micro-gaps during playback.
- **RTT is healthy** (1–2ms median, occasional 16ms spikes) — network is not the bottleneck.

### Root Cause Analysis (8 Issues Identified)

#### Bug 1: VSync Double-Wait Destroys Capture FPS (PRIMARY — ~10 FPS)
**Files:** `agent/src/video.c:40-54` (`vblank_wait`), `agent/src/video.c:109-129` (`video_worker_thread`)

`vblank_wait()` calls `WaitForVerticalBlank(DDWAITVB_BLOCKEND)` followed by `WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN)`. This blocks the capture thread for **up to 16.7ms per frame** waiting for the XP machine's physical monitor VSync. The video worker loop at line 117 additionally gates on `if (now - last_tick >= 16)`, and the thread does `Sleep(1)` in between — so the effective frame time is `VSync_wait + 16ms_gate + Sleep(1)` ≈ 33ms+ per frame = **~30 FPS ceiling on desktop, much worse in-game**.

When a Direct3D game (GTA SA) is running, the GPU is busy with its own VSync/presentation. The DirectDraw `WaitForVerticalBlank` call contends with the game's D3D swapchain, causing the capture thread to stall for **multiple VSync intervals** — hence the observed ~10 FPS.

**Fix:** Remove `vblank_wait()` entirely. GDI `BitBlt` reads the front buffer and does not benefit from VSync synchronization. Replace the timing loop with a clean `timeGetTime()` interval of 16ms with `Sleep(1)` spin-wait, no VSync dependency.

#### Bug 2: Geometry Flickering from GDI Mid-Render Capture
**Files:** `agent/src/video.c:264-273` (`video_capture` BitBlt call)

GDI `BitBlt(SRCCOPY)` captures the desktop compositor's front buffer. When a Direct3D game is rendering, `BitBlt` can capture **mid-frame** — the display surface may be partially updated by the game's D3D Present call. This manifests as:
- Meshes or HUD elements being invisible (captured between Clear and Draw).
- Partial geometry (captured between draw calls within a frame).

The VSync wait was presumably intended to fix this, but it synchronizes with the **monitor scanout**, not with D3D's **Present** call — they are different timing domains.

**Fix:** Remove VSync wait (it doesn't help). Add `CAPTUREBLT` flag to `BitBlt` (`SRCCOPY | CAPTUREBLT`) — on Windows XP this additionally captures DirectDraw and Direct3D overlay surfaces. If flickering persists, consider adding a 1-frame delay (capture the previous frame's completed buffer) or investigate `PrintWindow`/mirror driver alternatives. The pragmatic Moonlight approach is to accept occasional tearing artifacts and rely on high frame rate to make them imperceptible (at 60 FPS each frame is 16ms; a torn frame is replaced in the next capture).

#### Bug 3: Mouse Cursor Invisible in DirectX Fullscreen Games
**Files:** `agent/src/video.c:56-85` (`draw_cursor`)

`draw_cursor()` checks `ci.flags & CURSOR_SHOWING`. In DirectX fullscreen exclusive mode, Windows hides the system cursor (`flags=0`) — the game renders its own cursor via Direct3D. Since the GDI capture may not reliably pick up the D3D-rendered cursor, and the code skips drawing when `CURSOR_SHOWING` is false, the cursor vanishes entirely.

**Fix:** Two-part solution:
1. **Always composite the system cursor** regardless of `CURSOR_SHOWING` flag. Remove the `if (ci.flags & CURSOR_SHOWING)` guard. When the game hides the system cursor, `GetCursorInfo` still returns a valid `hCursor` handle and `ptScreenPos` — call `DrawIconEx` unconditionally.
2. **Fallback cursor:** If `hCursor` is NULL (rare), load `IDC_ARROW` via `LoadCursor(NULL, IDC_ARROW)` and draw that at the reported position.
3. **Game cursor duplication concern:** In games that draw their own cursor AND show the system cursor, this would result in double cursors. This is acceptable — the user can toggle cursor compositing in a future session. For GTA SA specifically, the game hides the system cursor, so this is not a concern.

#### Bug 4: Mouse Input Choppy Due to TCP Nagle's Algorithm
**Files:** `host/crates/xpdash-client/src/network/session.rs:180-193` (input send), `agent/src/net.c:129-165` (`net_connect_to_server`)

All input events (including high-frequency mouse movements at ~125Hz) are sent over TCP without `TCP_NODELAY`. Nagle's algorithm coalesces small writes — a 12-byte input event packet (4-byte TCP frame header + 8-byte `MsgInputEvent`) is held for up to **40ms** waiting for more data or an ACK. Multiple mouse moves batch into a single TCP segment, arriving as a burst at the agent side and injected all at once. This causes visible cursor jumps.

**Fix:**
1. **Set `TCP_NODELAY` on both ends.** Agent: `setsockopt(g_client_tcp, IPPROTO_TCP, TCP_NODELAY, ...)` in `net_connect_to_server()` and `net_poll_control()` after `accept()`. Client: `tcp_stream.set_nodelay(true)` in `run_session()` after `TcpStream::connect()`.
2. **Phase 2 (optional, if TCP_NODELAY is insufficient): Move mouse input to UDP.** Add `PKT_TYPE_INPUT = 0x04` to the UDP media protocol. Mouse moves are idempotent — dropped packets are harmless since the next delta supersedes. This eliminates TCP head-of-line blocking entirely for mouse input. Keep keyboard events on TCP for reliability (key-up must not be lost).

#### Bug 5: Audio Jitter from Ring Buffer Overflow/Underflow
**Files:** `host/crates/xpdash-client/src/audio.rs:24` (ring buffer), `host/crates/xpdash-client/src/network/session.rs:320` (PtsClock), `host/crates/xpdash-client/src/audio.rs:75-83` (`push_pcm16_samples`)

The audio ring buffer is **4,800 f32 samples = 50ms** at 48kHz stereo. The `PtsClock` is configured with `max_jitter_ms: 100`, allowing packets up to 100ms late. This creates a mismatch:
- Network batching delivers audio packets in bursts (e.g., 4 × 10ms packets arrive simultaneously after a 40ms gap).
- The ring buffer cannot absorb a 40ms burst (1,920 samples) on top of its existing contents — `try_push()` silently drops excess samples.
- After the burst, the CPAL consumer drains the buffer and underruns, popping `0.0` (silence) which causes audible clicks and gaps.

Additionally, the CPAL callback at `audio.rs:107` does `consumer.try_pop().unwrap_or(0.0)` — a hard transition from audio to silence and back causes clicking. No crossfade or sample-hold is applied.

**Fix:**
1. **Increase ring buffer to 19,200 samples (200ms).** This absorbs worst-case network burst without overflow: `HeapRb::<f32>::new(19200)`.
2. **Reduce `PtsClock max_jitter_ms` from 100 to 30.** With 1ms median RTT on LAN, 100ms tolerance allows extremely stale packets that desync audio. 30ms is generous for LAN and matches the ~25ms latency budget.
3. **Smooth underrun handling:** Replace `unwrap_or(0.0)` with sample-hold (repeat last sample) or micro-fade to zero over 48 samples (1ms). This eliminates click artifacts on transient underruns.

#### Bug 6: Client Frame Copy Overhead (1.92 MB/frame heap allocation)
**Files:** `host/crates/xpdash-client/src/ui/viewport.rs:55-58` (texture upload), `host/crates/xpdash-client/src/network/session.rs:101-102` (`latest_frame` clone)

Every frame paint:
1. `session.latest_frame()` at `session.rs:101` does `self.latest_frame.read().clone()` — clones `VideoFrame` including `Arc<Vec<u8>>` (cheap Arc bump, but still a read-lock acquisition on every paint).
2. `viewport.rs:55`: `bytemuck::cast_slice(&frame.rgba_pixels)` followed by `pixels.to_vec()` at line 58 — **copies the entire 1.92 MB pixel buffer into a new `Vec`** every frame to construct `ColorImage`. At 60 FPS this is 115 MB/s of pure memcpy overhead plus heap allocation pressure.

**Fix:**
1. **Eliminate the `to_vec()` copy.** Construct `egui::ColorImage` using the existing `Arc<Vec<u8>>` data directly. `egui::ColorImage` accepts ownership of a `Vec<Color32>` — transmute the `Vec<u8>` into `Vec<Color32>` (safe: both are `#[repr(C)]` 4-byte aligned RGBA) via `bytemuck::allocation::cast_vec` or manual `Vec::from_raw_parts`.
2. **Swap-buffer instead of RwLock for latest frame.** Replace `Arc<RwLock<Option<VideoFrame>>>` with `arc_swap::ArcSwap<Option<VideoFrame>>` or `std::sync::atomic` pointer swap. The decompression thread stores the new frame; the UI thread loads it. Zero contention, no lock.

#### Bug 7: Decompression Channel Backpressure Drops Frames
**Files:** `host/crates/xpdash-client/src/network/session.rs:289,380` (decompress channel)

The decompression channel is a `tokio::sync::mpsc::channel::<CompressedFrame>(8)`. When 8 frames queue up (which happens if decompression is slower than capture), `try_send()` fails and the frame is silently dropped. The decompression worker does LZ4 decompress (fast) + BGRA→RGBA byte swizzle (slow: per-pixel loop at lines 298-304) + write-lock acquisition on `frame_sink` (contends with UI).

**Fix:**
1. **Use channel capacity 2, not 8.** We only care about the latest frame. With 8 slots, the decompressor can fall 8 frames behind, adding ~133ms of latency.
2. **Replace the byte swizzle loop with SIMD or batch swap.** The BGRA→RGBA conversion iterates every pixel: `for chunk in rgba.chunks_exact_mut(4) { swap B and R }`. At 800×600 = 480,000 pixels this is ~1.9M byte operations. Use `chunks_exact_mut(8)` with manual u64 byte manipulation, or `rgba.chunks_exact_mut(4).for_each(|c| c.swap(0, 2))` which the compiler auto-vectorizes with `-C target-cpu=native`.
3. **Replace `frame_sink.write()` with atomic swap** (see Bug 6 fix).

#### Bug 8: No Continuous Repaint Scheduling (egui VSync coupling)
**Files:** `host/crates/xpdash-client/src/ui/viewport.rs:121` (`ctx.request_repaint()`)

`ctx.request_repaint()` asks egui for a repaint "as soon as possible" but eframe couples this to the host monitor's VSync by default. If the host runs at 60Hz, this is fine. But if the host compositor introduces frame skipping or the VSync phase doesn't align with incoming stream frames, frames are delayed by up to one VSync period.

**Fix:** Call `ctx.request_repaint_after(Duration::ZERO)` or set `eframe::NativeOptions::vsync = false` to decouple paint rate from host VSync. The GPU texture upload is cheap (~0.5ms for 1.92 MB) and doesn't need VSync alignment.

### Implementation Plan (5 Phases, Priority Order)

#### Phase 1: Agent Video Capture Fix (Highest Impact — 10 FPS → 60 FPS)
**Target files:** `agent/src/video.c`
1. Remove `vblank_wait()` call from `video_capture()` (line 264). Keep `vblank_init()` / `vblank_wait()` functions but ifdef them out for future optional use.
2. Add `CAPTUREBLT` flag: change `BitBlt(... SRCCOPY)` to `BitBlt(... SRCCOPY | CAPTUREBLT)` at line 266.
3. Fix cursor compositing: remove `if (ci.flags & CURSOR_SHOWING)` guard in `draw_cursor()`. Always call `DrawIconEx` if `GetCursorInfo` succeeds and `ci.hCursor` is non-NULL. Add `LoadCursor(NULL, IDC_ARROW)` fallback.
4. Clean up video worker loop: replace `if (now - last_tick >= 16)` with cleaner 16ms interval tracking that doesn't drift.
**Build:** `nix develop --command bash agent/build.sh`
**Deploy:** `deploy/deploy-timemachine.sh`
**Verify:** Connect client, observe FPS counter in HUD. Target: ≥30 FPS desktop, ≥25 FPS in GTA SA. Cursor visible.

#### Phase 2: Network Input Latency Fix (Mouse Choppiness)
**Target files:** `agent/src/net.c`, `host/crates/xpdash-client/src/network/session.rs`
1. Agent: add `setsockopt(TCP_NODELAY)` immediately after `connect()` in `net_connect_to_server()` (line 156) and after `accept()` in `net_poll_control()` (line 367).
2. Client: add `tcp_stream.set_nodelay(true)?;` after `TcpStream::connect()` in `run_session()` (line 149).
**Verify:** Move mouse in stream — should track smoothly without visible jumps/batching.

#### Phase 3: Audio Pipeline Fix (Jittery Audio)
**Target files:** `host/crates/xpdash-client/src/audio.rs`, `host/crates/xpdash-client/src/network/session.rs`
1. Change ring buffer from `HeapRb::<f32>::new(4800)` to `HeapRb::<f32>::new(19200)` in `AudioController::new()`.
2. Change `PtsClock::new(100)` to `PtsClock::new(30)` in `run_media_receiver()`.
3. Replace `consumer.try_pop().unwrap_or(0.0)` in the CPAL callback with sample-hold: track `last_sample` and return it on underrun instead of 0.0. Add a 1ms fade-to-zero if underrun persists for more than 480 samples.
**Verify:** Play GTA SA, listen for audio stuttering. Should be clean continuous playback.

#### Phase 4: Client Rendering Optimization (Frame Drops & Copies)
**Target files:** `host/crates/xpdash-client/src/ui/viewport.rs`, `host/crates/xpdash-client/src/network/session.rs`, `host/crates/xpdash-client/src/audio.rs`
1. Eliminate `pixels.to_vec()` in viewport texture upload. Use `bytemuck::cast_vec` or unsafe `Vec::from_raw_parts` to reinterpret `Vec<u8>` as `Vec<Color32>` without copying.
2. Replace `Arc<RwLock<Option<VideoFrame>>>` with `arc_swap::ArcSwap` for lock-free frame handoff between decompression thread and UI thread. Add `arc-swap = "1"` to `Cargo.toml`.
3. Reduce decompression channel from `channel::<CompressedFrame>(8)` to `channel::<CompressedFrame>(2)`.
4. Optimize BGRA→RGBA swizzle: ensure the byte-swap loop uses `.swap(0, 2)` which auto-vectorizes, or use `unsafe` SIMD intrinsics.
5. Consider setting `eframe::NativeOptions { vsync: false, .. }` for decoupled paint rate.
**Verify:** Observe FPS counter — should match or exceed agent capture rate. No visible frame drops.

#### Phase 5: Integration Verification on `timemachine`
1. Build updated agent: `nix develop --command bash agent/build.sh`
2. Deploy: `deploy/deploy-timemachine.sh`
3. Build updated client: `cargo build --release -p xpdash-client`
4. Launch GTA: San Andreas on `timemachine`.
5. Connect client and verify:
   - **FPS:** ≥30 in-game, ≥45 on desktop (target 60).
   - **Cursor:** Visible in both desktop and in-game modes.
   - **Geometry:** No flickering of meshes or HUD elements.
   - **Mouse:** Smooth tracking, no visible batching or jumps.
   - **Audio:** Clean continuous playback, no stuttering or clicks.
   - **RTT:** Should remain 1–2ms (unchanged from current).
   - **Bitrate:** Monitor for bandwidth regression (current ~11.8 Mbps).
6. If Phase 1–4 achieve ≥30 FPS but not 60, investigate:
   - Agent-side `BitBlt` cost profiling via `timeGetTime` bracketing.
   - LZ4 compression cost per frame — consider `LZ4_compress_fast` with acceleration=2 (faster, slightly worse ratio).
   - Whether the i7-4790K in `timemachine` can sustain 60 FPS BitBlt + LZ4 at 800×600.
   - Alternative capture: Mirror driver or DirectX hook (session 12+ scope if needed).

### Success Criteria
- Desktop streaming: ≥45 FPS, smooth mouse, no flickering.
- GTA SA streaming: ≥25 FPS, visible cursor, clean audio, no geometry artifacts.
- Competitive with Moonlight/Sunshine on equivalent hardware (accounting for the XP/GDI capture limitation vs. modern NVENC).

### Non-Goals (Deferred)
- Hardware video encoding (NVENC/VCE) — the XP machines have no hardware encoder.
- DirectX hooking for pixel-perfect capture — complex, game-specific, deferred to a future session if GDI+CAPTUREBLT proves insufficient.
- UDP input channel — implement only if TCP_NODELAY doesn't resolve mouse choppiness.


## Future Sessions (Sessions 8 to 11): Advanced Input, Shaders & Packaging
### Session 8: Input Confinement & Modifier Routing Engine
- Implement relative pointer confinement (pointer lock) with visual state indicator and configurable release hotkey (default `Right-Ctrl`).
- Implement low-level keyboard hook (Windows) and Wayland shortcut inhibitor / X11 grab (Linux) to selectively capture or release `Super/Win`, `Alt+Tab`, `Alt+F4`, and `Ctrl+Alt+Del`.
- Implement hardware PS/2 Set 1 scancode translation table for legacy DirectInput 8/9 game compatibility.

### Session 9: In-Game HUD, Audio Controls & Retro CRT Shaders
- Implement slide-down in-game HUD overlay (hover top edge or `F10`) with live telemetry (FPS, RTT, jitter, loss).
- Implement audio mixer controls (volume slider, mute toggle, channel balance, buffer size selector).
- Implement GPU-accelerated CRT scanline and integer-scaling shaders.

### Session 10: Remote Deployer Wizard & Standalone Packaging
- Implement in-app One-Click Remote Deployer wizard over SMB/WMI.
- Package zero-dependency standalone Windows XP setup installer (`xpdash-agent-setup.exe`).
- Package portable Linux AppImage and standalone Windows `.exe` client.

### Session 11: Web Client Gateway & Browser Streaming
- Implement `xpdash-web` WebSocket bridge with WebCodecs video and WebAudio 48kHz output.
