# xpdash

Ultra-low-latency streaming, audio capture, and remote control for real Windows XP hardware over LAN.

Designed specifically for period-correct retro gaming rigs (e.g. `timemachine` i7-4790K + GTX 750 Ti + Sound Blaster Audigy SB0090; `q9650` Core 2 Quad Q9650 + Radeon HD 5770).

> **Project Status & Disclaimer**: *xpdash was literally vibe-coded in 1 day!* While it has been extensively tested and tuned on our physical reference lab rigs (`timemachine` and `q9650`) with sub-millisecond control latencies and zero audio/video drops, it has not yet been battle-tested across the wild ecosystem of retro PC builds, oddball sound cards, or exotic display adapters. Expect edge cases, and please open an issue or PR with your hardware details if you encounter unexpected behavior!

---

## Key Features

1. **Hardware EAX Audio Capture**: Native capture of hardware-accelerated DirectSound3D and EAX environmental reverb from Creative Sound Blaster cards (specifically the EMU10K1/EMU10K2 DSP on Audigy and Live! series). Automatically routes and captures the post-DSP `"What U Hear"` internal multiplexer at crystal-clear 48 kHz 16-bit stereo PCM with zero CPU overhead.
2. **Universal Windows XP Graphics API Support**:
   - **Direct3D 9**: Zero-flicker backbuffer capture via hooked `IDirect3DDevice9::Present()` with seamless desktop GDI `BitBlt` fallback.
   - **Direct3D 8**: Full support for early XP 3D classics (e.g. *GTA III*, *Vice City*, *Max Payne*, *Morrowind*).
   - **DirectDraw 7**: Fullscreen exclusive page-flipping (`DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN`) and windowed clipper presentation for classic 2D retro titles (e.g. *StarCraft*, *Diablo II*, *Age of Empires II*, *Fallout*).
   - **OpenGL**: Hardware-accelerated Win32 WGL double-buffered rendering (e.g. *Quake II/III*, *Doom 3*, *Half-Life*).
   - **Win32 GDI**: Double-buffered desktop rendering, dirty rects, and palette-animated legacy software.
3. **Dynamic Resolution & Color Depth Adaptation**: Seamless in-flight adaptation across 8-bit paletted (with 256-entry DAC palette LUT expansion), 16-bit High Color (RGB 565/555), and 32-bit True Color across resolutions from $640\times 480$ to $1280\times 1024$ without dropping the stream or requiring an agent restart.
4. **Virtual Capture Card for OBS Studio**: Dedicated 1x fixed source window mode with zero letterboxing, automatic resolution resizing, and stable window metadata for rock-solid OBS Window Capture and Application Audio Capture.
5. **Zero Audio/Video Desync & Bounded Latency**: Millisecond presentation timestamps (PTS) stamped on every packet via multimedia timers (`timeGetTime()`), coupled with adaptive baseline slewing and a strict **drop-late-frame** policy over buffer accumulation: network hiccups never cause permanent latency drift.
6. **Cross-Platform Native Client**: High-performance Rust client with `egui` / `wgpu` GUI, auto-discovery machine roster grid, slide-down HUD (`F10`), aspect-ratio modes, and scancode translation.

---

## Quick Start Guide

### Step 1: Install & Start the Agent on Windows XP

1. Download **`xpdash-agent-winxp.zip`** from the [Latest Nightly Release](https://github.com/headpats/xpdash/releases/tag/nightly) (or build it via `bash scripts/package-release.sh`).
2. Extract the archive to `C:\xpdash\` on your Windows XP machine.
3. Open an Administrator Command Prompt in `C:\xpdash\` (or double-click) and run:
   ```cmd
   install-agent.bat
   ```
   This script:
   - Configures Windows Firewall rules for TCP port 7020 (Control) and UDP ports 7021 (Media) and 7022 (Discovery).
   - Generates a default `agent.ini` configuration.
   - Copies `xpdash-agent.exe` and hook DLLs (`xpdash-hook.dll`, `xpdash-hook9.dll`).
   - Registers `xpdash-agent.exe` to run automatically at startup (`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run`).
   - Launches `xpdash-agent.exe` immediately on console session 0.
4. The agent is now running silently in the background, listening on TCP port 7020.
   *(To uninstall at any time, run `uninstall-agent.bat`).*

---

### Step 2: Run the Client on Modern Linux or Windows 10/11

#### On Linux (Wayland / X11):
1. Download **`xpdash-linux-x86_64.tar.gz`** from the [Latest Nightly Release](https://github.com/headpats/xpdash/releases/tag/nightly) and extract it:
   ```bash
   tar -xzf xpdash-linux-x86_64.tar.gz
   cd xpdash-linux-x86_64
   ```
2. Launch the client:
   ```bash
   # Open the GUI machine discovery dashboard:
   ./run-client.sh

   # Or connect directly to your XP machine by IP:
   ./run-client.sh 10.0.10.113

   # Or launch directly in OBS Virtual Capture Card mode:
   ./run-obs.sh 10.0.10.113
   ```

#### On Windows 10/11:
1. Download **`xpdash-client-windows-x86_64.zip`** from the [Latest Nightly Release](https://github.com/headpats/xpdash/releases/tag/nightly) and extract it.
2. Double-click **`run-client.bat`** (or `xpdash-client.exe`) to open the GUI dashboard.
   - Discovered Windows XP rigs on your local network will appear as interactive cards showing machine name, live RTT latency, resolution, and audio hardware.
   - Click **`▶ Connect to Console`** on the machine card (or type the IP into the Quick Connect bar).
   - To launch directly in 1x OBS Source mode, double-click **`run-obs.bat`**.

---

### Step 3: In-Stream Controls & Hotkeys

- **`F10`** (or hover cursor within 16px of top edge): Open/close the slide-down **In-Game HUD** overlay with live telemetry (FPS counter, RTT latency, bitrate, audio jitter), audio volume slider, mute toggle, mouse sensitivity slider, and aspect-ratio scaling modes.
- **`F12`**: Toggle mouse pointer lock (Confined / Unconfined).
- **`F9`**: Toggle OBS 1x Fixed Source Mode on/off.
- **`F11`**: Toggle Fullscreen.
- **`Right-Ctrl`**: Release captured mouse cursor to host.
- In HUD: Click **`Send Ctrl+Alt+Del`** to unlock Windows XP or open Task Manager.

---
## OBS Studio Guide: Virtual Capture Card Mode

Using `xpdash` as a virtual capture card is significantly more convenient and reliable than physical HDMI/VGA capture cards:
- **No Signal Loss on Mode Switches**: Physical capture cards frequently freeze, blink black, or lose sync for 3–10 seconds when retro games switch resolution between menus ($640\times 480$) and gameplay ($800\times 600$ or $1024\times 768$). `xpdash` adapts dynamically on the fly within a single frame.
- **No Desync from Plugin Timing**: Rather than fighting OBS clock synchronization via an internal plugin (a notorious source of desync in tools like OBS Teleport), `xpdash` runs as an independent, ultra-low-latency display window that OBS captures natively via GPU shared surface.

### Step-by-Step OBS Setup

1. **Launch xpdash in OBS Source Mode**:
   ```bash
   # Linux
   ./run-obs.sh 10.0.10.113
   # or directly:
   xpdash-client --obs 10.0.10.113

   # Windows 10/11
   xpdash-client.exe --obs 10.0.10.113
   ```
   *(You can also toggle OBS mode at any time by pressing **`F9`** or toggling `🎥 OBS 1x: ON` in the **`F10`** slide-down HUD).*

2. **Add Video Source in OBS**:
   - In OBS Studio, add a **Window Capture** source (or **Game Capture** on Windows / **PipeWire Window Capture** on Linux).
   - Select window: `xpdash — [YOUR_RIG] (OBS Source)`.
   - Set **Window Match Priority**: *Match title, otherwise find window of same executable*.
   - Because `xpdash` locks its title to a stable prefix and removes all confinement borders and letterbox pillars in OBS mode, OBS will reliably lock onto the window across restarts and display pure, unadulterated 1:1 pixels.
   - When the game changes resolution, `xpdash` automatically resizes its client area to match the new resolution (e.g. $640\times 480 \to 800\times 600$), and OBS automatically scales or anchors it according to your OBS scene transform settings.

3. **Add Audio Source in OBS**:
   - Add an **Application Audio Capture** source in OBS targeting `xpdash-client` (or use your default Desktop Audio).
   - Audio is streamed at uncompressed 48 kHz 16-bit stereo PCM straight from the retro sound card's hardware DSP.

---

## Audio Device Selection & Configuration

Windows XP machines often have multiple sound devices installed (e.g. a dedicated Creative Sound Blaster PCI card alongside onboard Realtek AC'97 or HDMI audio).

### Selection Priority

When the agent starts, it enumerates all recording devices (`waveInGetNumDevs()`) and applies the following precedence:

1. **Priority 1 (Manual Index Override)**: If `device_index` is configured in `agent.ini`, the agent selects that device index directly.
2. **Priority 2 (Manual Name Override)**: If `device_name` is set in `agent.ini`, the agent selects the first device whose name matches the substring (case-insensitive, e.g. `"Realtek"`, `"Audigy"`).
3. **Priority 3 (Automatic Creative EAX Hardware Heuristic)**: If no manual override is provided, the agent inspects device names for Creative hardware identifiers (`"Audigy"`, `"Sound Blaster"`, `"Live!"`, `"Creative"`, `"EMU10K"`). If found, it automatically selects this device to ensure hardware DirectSound3D/EAX audio is captured.
4. **Priority 4 (System Default Fallback)**: Falls back to `WAVE_MAPPER` (the default Windows XP recording device).

### Mixer Loopback Selection

Once the recording device is selected, the agent accesses its associated mixer (`MIXER_OBJECTF_WAVEIN`) and selects the loopback recording control:
- If `mixer_line` is specified in `agent.ini` (e.g. `mixer_line="What U Hear"`), that line is selected.
- Otherwise, it searches the recording destination for standard loopback names:
  1. `"What U Hear"` (Creative Audigy / Sound Blaster Live!)
  2. `"Stereo Mix"` (Realtek HD / AC'97)
  3. `"Wave Out Mix"`
  4. `"Wave"`
  5. `"Sum"` / `"Mono Mix"`

### Configuration Example (`C:\xpdash\agent.ini`)

```ini
[security]
trusted_fingerprints=SHA256:9b37df561b5a7e48df39b4756ac8ea1b0d64b609d1d095df9e0406b0dd14504f
prompt_user=1
allow_all=0

[audio]
# Optional: force specific device by index (0, 1, ...) or name substring ("Audigy", "Realtek")
# device_index=0
# device_name=Audigy

# Optional: force specific recording line
# mixer_line=What U Hear
```

---

## Known Limitations & Workarounds

### 1. Hardware Video Overlays (e.g. GTA San Andreas Intro Logos)

**Observed Symptom**: When launching certain retro games (e.g. *Grand Theft Auto: San Andreas*), the initial publisher splash videos (`movies\Logo.mpg` and `movies\GTAtitles.mpg`) appear as a solid black screen on the client. As soon as the intro video ends (or is skipped) and the game reaches the 3D main menu, the stream appears immediately at full 60 FPS.

**Technical Root Cause**:
- On Windows XP, pre-rendered MPEG-1/2 videos played via DirectShow or the RenderWare `rwmpeg` plugin render **before** the game initializes its Direct3D 9 device.
- DirectShow decodes the video directly into a dedicated **Hardware YUV Overlay Surface** on the GPU. On the Windows desktop framebuffer (VRAM), the player paints only the GPU's hardware overlay color key (e.g. `RGB(16, 0, 16)` on NVIDIA graphics cards).
- The physical monitor displays the video because the GPU's display controller / DAC replaces the `RGB(16, 0, 16)` color key with pixels from the overlay surface during hardware scanout to the physical monitor.
- Desktop screen capture tools (`BitBlt`, `PrintScreen`, DIBSection) capture the desktop framebuffer in VRAM, which contains only the `RGB(16, 0, 16)` color key, appearing as black on the client.
- *Note*: **All in-game story cutscenes in GTA San Andreas are 100% in-engine 3D rendered via Direct3D 9** and are streamed at full 60 FPS with zero flicker. The limitation applies strictly to standalone pre-rendered MPEG splash files. In contrast, games like *Lords of the Realm II* (which decode Bink/Smacker video in CPU software directly into standard GDI surfaces without an overlay plane) stream video without issue.

**Workarounds & How to Disable Hardware Overlays on Windows XP**:
If you wish to view pre-rendered MPEG videos through the stream, you can force Windows XP to render them onto standard desktop surfaces using one of the following methods:

1. **Option A (Skip Intro Logos — Recommended)**:
   - Press **`Space`**, **`Enter`**, or **`Esc`** at launch to skip the publisher intro logos and jump straight to the 3D main menu.
   - Alternatively, rename or delete `movies\Logo.mpg` and `movies\GTAtitles.mpg` in your game directory (`C:\Program Files\Rockstar Games\GTA San Andreas\movies`) to permanently boot straight to the main menu instantly.
2. **Option B (Disable Overlays in Windows Media Player / DirectShow)**:
   - On the Windows XP host machine, open **Windows Media Player**.
   - Navigate to **`Tools`** $\to$ **`Options`** $\to$ **`Performance`** tab.
   - Click the **`Advanced...`** button under *Video acceleration*.
   - Under *Video acceleration settings*, **uncheck "Use overlays"**.
   - Under *Digital Video*, **check "Use high quality mode"** (forces VMR-7 / VMR-9 software blitting).
   - Click **OK** $\to$ **Apply**. DirectShow will now composite video frames directly onto standard window surfaces, allowing `xpdash` to capture them.
3. **Option C (DirectX Display Troubleshooter / Registry)**:
   - In Windows XP, open **`Display Properties`** $\to$ **`Settings`** $\to$ **`Advanced`** $\to$ **`Troubleshoot`**.
   - Reduce the **Hardware acceleration** slider by one notch (disables cursor and advanced overlay drawing accelerations, forcing drivers to blit video frames to the primary surface).
   - Alternatively, add `DisableOverlay=1` as a `DWORD` under `HKLM\SOFTWARE\Microsoft\DirectDraw`.

### 2. Multi-Monitor Setups

**Multi-monitor display configurations on Windows XP are currently not supported.** The agent captures the primary monitor display context (`GetDC(NULL)`). If multiple monitors are connected, only the primary monitor is captured.

*Do you have a genuine retro multi-monitor gaming or productivity use case on real Windows XP hardware?* We would love to hear about it! Please feel free to open a GitHub Issue or submit a Pull Request describing your setup and requirements.
---

## Verified Hardware Matrix

| Machine | CPU | GPU | Audio Hardware | EAX Capability | Network IP | Role |
|---|---|---|---|---|---|---|
| **timemachine** | Intel Core i7-4790K | NVIDIA GTX 750 Ti | Creative Sound Blaster Audigy Platinum (SB0090) | EAX 1.0, 2.0, 3.0 (4.0 via driver) on EMU10K2 DSP | `10.0.10.113` | Primary EAX testing & LCD box |
| **q9650** | Intel Core 2 Quad Q9650 | AMD Radeon HD 5770 | Realtek HD / PCI Audio | Software DirectSound | `10.0.10.134` | Secondary box & CRT testing |

---

## Repository Layout

```
xpdash/
├── LICENSE               # MIT License
├── README.md             # Overview and documentation (this file)
├── ARCHITECTURE.md       # Architectural specifications & data paths
├── PROTOCOL.md           # Wire protocol specification
├── ROADMAP.md            # Multi-session development roadmap
├── scripts/
│   └── package-release.sh # Release packager for Linux and Windows XP bundles
├── agent/                # Windows XP Native Agent (C / Win32, i686 subsystem 5.1)
│   ├── Makefile
│   ├── build.sh
│   └── src/
│       ├── main.c        # Entry point, event loop, exception filter
│       ├── audio.c/.h    # waveIn capture & mixer loopback auto-select
│       ├── video.c/.h    # Low-latency DIBSection capture & mode switching
│       ├── d3d9hook.c/.h # D3D9 hook injection & shared memory reader
│       ├── d3d9hook_dll.c# Hook DLL injected into D3D9 game processes
│       ├── input.c/.h    # SendInput injection (scancodes for DirectX games)
│       └── net.c/.h      # UDP/TCP network streaming engine
├── tools/                # Windows XP Test & Verification Suite
│   ├── build-all.sh      # Master build script for all test tools
│   ├── Makefile          # Top-level tools Makefile
│   ├── README.md         # Detailed test suite documentation & verified matrix
│   ├── test-gdi/         # Win32 GDI double-buffered 8/16/32bpp & palette test
│   ├── test-ddraw/       # DirectDraw 7 exclusive fullscreen flip & windowed test
│   ├── test-d3d8/        # Direct3D 8 rotating 3D lit cube & depth buffer test
│   ├── test-d3d9/        # Direct3D 9 hook & BitBlt fallback test
│   ├── test-opengl/      # Win32 WGL + OpenGL 1.1 double-buffered 3D scene test
│   ├── test-modeswitch/  # Automated 11-mode display resolution & bpp matrix test
│   ├── probe-video/      # Display caps & system DAC palette dump probe
│   └── eax-test/         # Standalone EAX DirectSound3D/OpenAL hardware verification tool
├── host/                 # Host Server & Client (Rust workspace)
│   ├── Cargo.toml
│   └── crates/
│       ├── xpdash-core/    # Shared protocol framing, crypto, timestamps
│       ├── xpdash-server/  # Discovery broadcaster, agent connection manager
│       └── xpdash-client/  # egui/wgpu GUI, cpal low-latency audio output, OBS mode
└── deploy/               # Remote push and manual install scripts
    ├── deploy-timemachine.sh
    ├── deploy-q9650.sh
    ├── install-agent.bat
    ├── uninstall-agent.bat
    └── agent.ini
```

---

## Building & Packaging Release Bundles

### Prerequisites

All dependencies and cross-compilation toolchains are packaged via Nix:
```bash
nix develop
```

This provides:
- `i686-w64-mingw32-gcc` (stamped subsystem 5.1 for Windows XP SP3)
- `rustc` & `cargo`
- Linux X11/Wayland/ALSA development headers
- `smbclient` & `netexec` for remote hardware deployment

### One-Command Release Packaging

To build and package all standalone release artifacts:
```bash
bash scripts/package-release.sh
```

This generates standalone bundles in `dist/`:
- **`dist/xpdash-linux-x86_64.tar.gz`**: Portable Linux client bundle containing `xpdash-client`, `xpdash-server`, `./run-client.sh`, and `./run-obs.sh`.
- **`dist/xpdash-agent-winxp.zip`**: Complete Windows XP deployment bundle containing `xpdash-agent.exe`, hook DLLs, installer batch scripts, `agent.ini`, and all graphics/audio test tools.

### Building Individual Components

- **Windows XP Agent**: `nix develop -c bash agent/build.sh`
- **Host Client / Server**: `cargo build --manifest-path host/Cargo.toml --release`
- **Graphics & Audio Test Suite**: `nix develop -c bash tools/build-all.sh`

---

## License

Licensed under the [MIT License](LICENSE).
