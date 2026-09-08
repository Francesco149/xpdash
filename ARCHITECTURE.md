# xpdash Architecture Specification

## 1. System Overview

xpdash connects modern workstations (Linux / Windows) to vintage Windows XP gaming rigs over LAN. The architecture is split into two primary components:

1. **XP Agent (`xpdash-agent.exe`)**: A lightweight, standalone 32-bit Win32 service running in the interactive user session on Windows XP. Designed with zero modern dependencies (PE subsystem 5.1, stock XP SP3 DLLs only: `kernel32`, `user32`, `gdi32`, `winmm`, `ws2_32`, `advapi32`, `shell32`, `msvcrt`).
2. **Host Server & Client (`xpdash-server` & `xpdash-client`)**: A modern, cross-platform Rust application providing low-latency audio playback (`cpal`), hardware-accelerated rendering (`wgpu`), intuitive UI (`egui`), and an optional Web gateway.

```
┌────────────────────────────────────────────────────────┐
│                   Windows XP Machine                   │
│                                                        │
│   DirectX / OpenAL Game                                │
│       │                                                │
│   Creative SB Audigy (EMU10K2 DSP)                     │
│       │ [Hardware EAX 1/2/3 Reverb & 3D Spatialization]│
│       ▼                                                │
│   "What U Hear" Mixer MUX (LineID 0x00010001)          │
│       │                                                │
│   xpdash-agent.exe & xpdash-hook.dll                   │
│   ├── Audio: waveIn 48kHz 16-bit Stereo PCM (10ms)     │
│   ├── Video: DIBSection / DirectDraw VSync / D3D9 Hook │
│   ├── Encode: TurboJPEG SIMD (85) / Fast LZ4 Fallback  │
│   ├── Input: SendInput (DirectX Hardware Scancodes)    │
│   └── Network: UDP Media/Input (7021), TCP Ctrl (7020) │
└───────────────────────────┬────────────────────────────┘
                            │ LAN (Gigabit / 100M Ethernet)
                            │ Audio/Video UDP (Port 7021)
                            │ Control TCP (Port 7020)
                            ▼
┌────────────────────────────────────────────────────────┐
│                  Modern Host Machine                   │
│                                                        │
│   xpdash-client / xpdash-server (Rust)                 │
│   ├── LAN Discovery Beacon (UDP 7022)                  │
│   ├── Anti-Desync Demuxer (PTS Monotonic Clock)        │
│   ├── Audio Output: cpal bounded jitter ring (10ms)    │
│   ├── Video Output: egui / wgpu texture surface        │
│   ├── In-Game HUD: Slide-down F10 telemetry & OBS 1x   │
│   ├── Input Forwarder: Relative pointer lock + UDP     │
│   └── (Optional) Web Client Gateway (WS / WebCodecs)   │
└────────────────────────────────────────────────────────┘
```

---

## 2. Audio Pipeline & SB-0090 EAX Capture

### 2.1 The XP EAX Problem & Solution
On Windows XP, games utilizing DirectSound3D or OpenAL bypass software mixing and offload 3D positional voices and environmental reverb directly to the sound card DSP. For the Creative Sound Blaster Audigy Platinum (SB0090), the EMU10K2 chip performs this processing in dedicated hardware.

Traditional audio loopback APIs (such as WASAPI Loopback) do not exist on Windows XP. Instead, the Creative hardware driver (`ctaud2k.sys`) exposes an internal recording source labeled **`"What U Hear"`**.

### 2.2 WinMM Mixer Automation
On startup, `xpdash-agent` inspects the WinMM mixer topology:
- **Device**: `SB Audigy Audio [D000]` (or default recording mixer).
- **Destination**: Destination 1 (`Recording Control`, `MIXERLINE_COMPONENTTYPE_DST_WAVEIN`).
- **Control**: Control 0 (`Record Select Switch`, `MIXERCONTROL_CONTROLTYPE_MUX`).
- **Target Item**: Item 4 (`"What U Hear"`).

If `"What U Hear"` is not selected, the agent programmatically activates it via `mixerSetControlDetails()`.

### 2.3 Low-Latency Capture Loop
- Audio is captured using standard Windows Multimedia `waveIn` with a dual- or quad-buffer ping-pong queue.
- Format: **48,000 Hz, 16-bit signed integer, 2 channels (Stereo)**.
- Frame size: **10 milliseconds** (480 samples per channel = 960 samples = 1,920 bytes per frame).
- Bitrate: 1,536 kbps (~1.5 Mbps), negligible bandwidth on local LAN.
- Zero encoding latency: Raw PCM samples are timestamped and packetized immediately.

---

## 3. Video Pipeline & Dynamic Resolution Adaptation

### 3.1 Framebuffer Capture
1. **GDI DIBSection & DirectDraw VSync Capture (Desktop, 2D & Fallback)**:
   - `CreateCompatibleDC(NULL)` + `CreateDIBSection()` with a shared memory buffer.
   - DirectDraw hardware VSync synchronization (`WaitForVerticalBlank` with `DDWAITVB_BLOCKBEGIN` via `IDirectDraw` in `ddraw.dll`) synchronizes desktop capture to the display refresh cycle, preventing torn frames.
   - `BitBlt()` with `SRCCOPY | CAPTUREBLT` (0x40CC0020) captures the desktop surface at up to 60 FPS, including `WS_EX_LAYERED` transparent windows (Rainmeter widgets, alpha-blended overlays).
   - **8-Bit Paletted & Dynamic Color Depth Adaptation**: When running 8-bit paletted retro games (e.g. *StarCraft*, *Diablo II*, *Fallout*), the agent detects paletted mode via `GetDeviceCaps(hdc, BITSPIXEL)` and `RASTERCAPS & RC_PALETTE`. It creates an 8-bit DIBSection, captures raw palette indices, extracts the active 256-entry DAC palette table via `GetSystemPaletteEntries`, and expands indices to 32-bit BGRX before TurboJPEG compression.
   - **Hardware Cursor Compositing**: Because GDI `BitBlt` does not include the hardware cursor, the agent queries cursor state via `GetCursorInfo()`. If `CURSOR_SHOWING` is active, it composites the cursor icon with proper hotspot coordinates directly onto the frame. Fullscreen games that hide the mouse cursor (e.g. GTA San Andreas gameplay) clear `CURSOR_SHOWING`, automatically suppressing cursor overlay.
   - *Hardware Video Overlay limitation*: Pre-rendered MPEG videos decoded via legacy DirectShow hardware overlays render directly to an offscreen YUV overlay plane on the GPU while painting the desktop window with the GPU's overlay color key (`RGB(16, 0, 16)`). Desktop captures see only this color key unless hardware overlays are disabled (e.g. in Windows Media Player options or DirectX troubleshooter), prompting DirectShow to use VMR-7/9 software blitting. In-game 3D cutscenes are rendered via Direct3D 9 and captured via the D3D9 hook with zero issues.
2. **Direct3D 9 Backbuffer Hook (3D Gaming)**:
   - Injected hook DLL (`xpdash-hook.dll`) intercepts `IDirect3DDevice9::Present()` (vtable index 17), `EndScene()` (index 42), and `Reset()` (index 16).
   - Reads backbuffer directly via `GetRenderTargetData` to offscreen system memory, writing frames to a named shared memory section (`CreateFileMapping`) and signaling an event (`SetEvent`).
   - The agent's video thread reads from shared memory when the hook is active, completely eliminating mid-render tearing and flicker.

### 3.2 Dynamic Resolution Change (`WM_DISPLAYCHANGE`)
Older remote desktop tools crash or render garbled pixels when retro games switch resolutions (e.g. 1024×768 desktop switching to 640×480 in-game).
- `xpdash-agent` registers a top-level message window.
- Upon receiving `WM_DISPLAYCHANGE`:
  1. Pause capture loop.
  2. Query new dimensions (`LOWORD(lParam)`, `HIWORD(lParam)`).
  3. Reallocate DIBSection buffer to match new resolution.
  4. Emit a `MSG_VIDEO_RESIZE { width, height, bpp }` control packet to the client.
  5. Client resizes its render target texture dynamically without dropping connection.
  6. Resume streaming seamlessly.

---

## 4. Clock Synchronization & Anti-Desync Policy

### 4.1 The Latency Drift Dilemma
Many streaming tools (such as OBS Teleport) use unbounded buffers. When network latency spikes or a packet drops, audio and video slowly fall behind, accumulating hundreds of milliseconds of lag over hours of use.

### 4.2 The xpdash Solution: Bounded Jitter & Drop-Late Policy
1. **Monotonic PTS Timestamps**:
   - Every audio and video packet carries a 32-bit millisecond Presentation Timestamp (PTS) from `GetTickCount()`.
2. **Audio Jitter Buffer**:
   - The client maintains a strictly bounded audio jitter buffer of **10 to 20 milliseconds**.
   - If network delay causes a buffer under-run, the client inserts a micro-fade concealment rather than stretching time.
   - If audio arrives late (PTS older than current playback cursor), it is discarded immediately.
3. **Video Presentation**:
   - Video frames are presented against the audio clock.
   - If a video frame arrives late, it is immediately dropped without rendering.
   - **Rule**: Never buffer old frames. Freshness is always prioritized over historical frame preservation.

---

## 5. Network Protocol & Framing

Communication uses two ports:
1. **TCP Port 7020 (Control & Handshake)**:
   - Client authentication and fingerprint verification.
   - Resolution change notifications.
   - Input injection events (keystrokes and mouse packets).
2. **UDP Port 7021 (Media Stream & High-Frequency Input)**:
   - Packetized audio (10ms chunks) and video frames/chunks (Agent → Client).
   - High-frequency input event datagrams (Client → Agent) for 125–1000 Hz mouse deltas.
   - Lightweight 16-byte header:
     ```
     [u8 magic: 0x58 'X']
     [u8 type: 0x01 Video | 0x02 Audio | 0x03 Ping | 0x04 Input]
     [u16 flags (bit 0 = keyframe)]
     [u32 seq]
     [u32 pts_ms]
     [u16 payload_len]
     [u16 reserved]
     [raw payload bytes]
     ```
   - Ultra-low-latency 10-byte input datagram: `[Magic: 0x58][Type: 0x04][MsgInputEvent: 8 bytes]`.
   - Video codecs: `VIDEO_CODEC_JPEG` (0x01, TurboJPEG quality 85, default), `VIDEO_CODEC_LZ4` (0x02, fallback lossless).
3. **UDP Port 7022 (Discovery)**:
   - Host server broadcasts periodic beacon `XPDASH_BEACON`.
   - XP agent listens and connects upon beacon detection.

---

## 6. Security & Trust Architecture

### 6.1 Server Fingerprint
The host server generates an Ed25519 keypair on first run. The SHA256 hash of the public key serves as the **Server Fingerprint** (e.g. `SHA256:7f9a8b...`).

### 6.2 Pre-Seeded Trust (Headless / Lab Machines)
For managed rigs like `timemachine` and `q9650`:
- The configuration file `C:\xpdash\agent.ini` contains:
  ```ini
  [security]
  trusted_fingerprints=SHA256:7f9a8b...
  ```
- When the agent sees a beacon or connection from this fingerprint, it establishes the connection immediately with zero user intervention.

### 6.3 Public / Manual Installation Trust
For general public users running the agent on their own XP hardware:
- When an unknown server attempts connection:
- The agent displays a native Windows XP dialog or tray notification:
  ```
  [xpdash Security Alert]
  Incoming connection from: "Gaming Rig" (10.0.10.50)
  Fingerprint: SHA256:7f9a8b...
  Do you want to trust this machine?
  [ Trust Always ]   [ Trust Once ]   [ Reject ]
  ```
- If approved, the fingerprint is appended to `trusted_servers.ini`.

---

## 7. Latency Budget Breakdown (Target: < 25 ms)

| Stage | Mechanism | Budget |
|---|---|---|
| **Audio Capture** | WinMM waveIn 48kHz (10ms buffer) | 10.0 ms |
| **Video Capture** | GDI DIBSection BitBlt (60 fps) | ~4.0 ms |
| **Video Encode** | TurboJPEG / Fast LZ4 | ~3.0 ms |
| **Network LAN** | Direct UDP Ethernet transmit | < 0.5 ms |
| **Network Recv** | Socket read + PTS demux | < 0.5 ms |
| **Audio Playback**| cpal / WASAPI / ALSA buffer | ~5.0 ms |
| **Video Render**  | wgpu texture upload + VSync | ~2.0 ms |
| **Total Glass-to-Glass** | — | **~25.0 ms** |
