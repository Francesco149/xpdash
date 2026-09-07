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
