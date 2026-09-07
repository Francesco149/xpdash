# xpdash Multi-Session Development Roadmap

This document serves as the architectural master plan and session-by-session execution guide. Any new or resumed agent session can consult this roadmap to understand current project state, completed milestones, and exact next steps.

---

## Session Overview & Status

| Session | Focus | Status | Key Deliverables |
|---|---|---|---|
| **Session 1** | **Orientation, Toolchain, Hardware Verification & Planning** | **COMPLETED (Current)** | Nix devShell, architecture/protocol specs, SB0090 audio/EAX verification, legacy agent uninstallation, EAX test suite scaffold. |
| **Session 2** | **Windows XP Native Agent Core** | *Pending* | C agent (`waveIn` audio capture, DIBSection video, `SendInput`, UDP/TCP streaming, display change handler). |
| **Session 3** | **Host Server & Native Cross-Platform Client** | *Pending* | Rust workspace (`cpal` low-latency audio, `egui`/`wgpu` GUI, anti-desync demuxer, relative mouse capture). |
| **Session 4** | **Auto-Discovery, Security & Packaging** | *Pending* | UDP discovery beacons, Ed25519 fingerprinting, interactive XP trust UI, `deploy.sh` and public `install-agent.bat`. |
| **Session 5** | **End-to-End Integration, Soak Testing & Real EAX Games** | *Pending* | Real game EAX testing (UT2004 / Doom 3), 1-hour zero-desync soak test on `timemachine` & `q9650`. |

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

## Session 2: Windows XP Native Agent Core (Next Session)

### Goal
Build and verify the standalone C agent (`xpdash-agent.exe`) on Windows XP (`timemachine`).

### Subtasks
1. **Audio Capture Engine (`agent/src/audio.c`)**:
   - Implement WinMM mixer probe: automatically verify and select `"What U Hear"` on startup.
   - Implement `waveIn` double-buffering queue capturing 10ms PCM slices (48 kHz, 16-bit, stereo, 1920 bytes/slice).
   - Timestamp each audio packet with `GetTickCount()` millisecond presentation timestamps (PTS).
2. **Video Capture Engine (`agent/src/video.c`)**:
   - Implement `CreateDIBSection` screen capture loop targeting 60 FPS.
   - Implement dirty-rect / dirty-tile (64×64) change detection to minimize bandwidth on static desktop scenes.
   - Implement fast quantization/compression (TurboJPEG or fast LZ4).
3. **Dynamic Resolution Change Handling**:
   - Window procedure handling `WM_DISPLAYCHANGE`.
   - Reallocate capture buffers and transmit `VIDEO_RESIZE` control frame to client without dropping session.
4. **Input Injection (`agent/src/input.c`)**:
   - Receive keyboard scancodes and mouse motion from TCP control channel.
   - Inject via `SendInput()` using raw hardware scancodes (`KEYEVENTF_SCANCODE`) so DirectX 3D games receive valid inputs.
5. **Network Streaming Engine (`agent/src/net.c`)**:
   - TCP control server on port 7020.
   - UDP media streamer on port 7021 adhering to `PROTOCOL.md`.
6. **Verification on `timemachine`**:
   - Compile via `agent/build.sh`.
   - Deploy to `C:\xpdash\xpdash-agent.exe` on `timemachine` and verify clean execution.

---

## Session 3: Host Server & Native Cross-Platform Client

### Goal
Build the cross-platform Rust client and server (`xpdash-client` and `xpdash-server`) running on Linux and Windows.

### Subtasks
1. **Core Crates (`host/crates/`)**:
   - `xpdash-core`: Network packet serialization, framing, and PTS clock synchronization.
   - `xpdash-server`: UDP beacon discovery broadcaster, TCP control listener, UDP audio/video receiver.
2. **Low-Latency Audio Output (`cpal`)**:
   - Implement lock-free audio ring buffer.
   - Bounded jitter buffer: max 15ms depth.
   - **Anti-desync policy**: Drop late audio packets immediately; never let playback drift behind.
3. **Video Rendering Pipeline (`egui` / `wgpu`)**:
   - Stream textured quad to screen via GPU.
   - Dynamic texture resizing on `VIDEO_RESIZE` events.
   - Present video frames strictly aligned to the audio PTS clock.
4. **Input Capture & Forwarding**:
   - Relative mouse capture (mouse grab) for first-person 3D games.
   - Full keyboard scancode mapping (handling Windows and Linux key symbols).
5. **Verification**:
   - Connect Linux host to `timemachine` over LAN.
   - Verify simultaneous audio playback and video display with zero latency drift.

---

## Session 4: Auto-Discovery, Security & Packaging

### Goal
Implement zero-configuration LAN discovery, security fingerprinting, and public installation packaging.

### Subtasks
1. **Auto-Discovery**:
   - Server broadcasts UDP beacon on port 7022 (`XPDASH_BEACON`).
   - XP agent receives beacon and automatically connects to the server IP.
2. **Security & Trust Model**:
   - Ed25519 host keypair generation.
   - Pre-seeded trusted fingerprint mode for lab rigs (`agent.ini`).
   - Native XP GUI trust confirmation dialog for unknown servers in public mode.
3. **Automated Remote Deployment**:
   - `deploy/deploy-timemachine.sh`: One-command script to build, push via SMB, and launch agent on `timemachine`.
   - `deploy/deploy-q9650.sh`: Matching script for the second test rig (`q9650`).
4. **Public Standalone Installer**:
   - `deploy/install-agent.bat`: Clean batch script for general public users to install on their own XP rigs.

---

## Session 5: Real-Game EAX Testing & Performance Optimization

### Goal
Perform complete end-to-end validation with real retro games utilizing hardware EAX audio and optimize system performance.

### Subtasks
1. **Real Game Testing on `timemachine`**:
   - Launch games featuring EAX 2.0 / 3.0 (e.g. *Unreal Tournament 2004*, *Doom 3*, *Max Payne*).
   - Verify 3D positional audio and environmental reverb are cleanly streamed over LAN.
   - Verify CPU utilization on `timemachine` remains < 5% for `xpdash-agent.exe`.
2. **Soak Testing**:
   - Run a continuous 1-hour streaming session.
   - Confirm glass-to-glass latency remains < 30ms throughout the entire duration.
   - Confirm zero audio crackle, zero pops, and zero A/V desync.
3. **Web Client Gateway (Optional)**:
   - Verify WebSocket / WebCodecs / WebAudio browser client functionality.
