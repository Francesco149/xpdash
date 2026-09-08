# xpdash

Ultra-low-latency streaming and remote control for real Windows XP hardware over LAN.

Designed specifically for period-correct XP gaming rigs (e.g., `timemachine` i7-4790K + GTX 750 Ti + Sound Blaster Audigy SB0090; `q9650` Core 2 Quad Q9650 + Radeon HD 5770).

---

## The Vision & Critical Improvements Over PoC

The initial proof-of-concept in `retro-hardware/projects/xp-dashboard` demonstrated basic GDI/UltraVNC screen streaming and monitor power control, but had fundamental limitations. **xpdash** is a clean-sheet architecture that solves them:

1. **Hardware EAX Audio Capture**: Native capture of hardware-accelerated DirectSound3D and EAX audio from Creative Sound Blaster cards (specifically the EMU10K2 DSP on the SB0090). It automatically routes and captures the post-DSP `"What U Hear"` internal multiplexer at 48kHz 16-bit stereo PCM with zero CPU overhead.
2. **Cross-Platform Client (Linux & Windows) + Web**: Client runs natively on Linux (Wayland & X11) and modern Windows 10/11, with an optional Web client gateway (WebSocket + WebAudio + WebCodecs) for browser-based access.
3. **Resilience & Dynamic Reconnection**: Gracefully handles in-game desktop resolution changes (`WM_DISPLAYCHANGE`), network dropouts, and remote machine reboots without requiring agent or server restarts.
4. **LAN Auto-Discovery**: Built-in UDP beacon discovery so agents and servers automatically find each other on the local network.
5. **Zero Audio/Video Desync & Bounded Latency**: Millisecond presentation timestamps (PTS) on every media packet. Strictly enforces a **drop-late-frame** policy over buffer accumulation: network hiccups never cause permanent latency drift.
6. **No Display Blanking**: Removed legacy DPMS-off loops that caused display flickering and input desync.
7. **Security & Deployment**:
   - **Remote / Headless Deploy**: Pre-seeded server fingerprint in `agent.ini` for immediate zero-touch pairing.
   - **Public / Manual Install**: XP agent displays an interactive confirmation dialog prompting the user to trust newly discovered servers before connecting.
8. **EAX Verification Test Suite**: Standalone test tools (`tools/eax-test/`) that exercise the hardware DSP reverb paths to 100% prove EAX audio capture.

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
├── .gitignore
├── flake.nix             # Nix devShell (i686 mingw cross, rust, linux audio/gui deps)
├── README.md             # Overview and quickstart (this file)
├── ARCHITECTURE.md       # Full architectural design, data paths, and latency budget
├── PROTOCOL.md           # Wire protocol: Discovery, Handshake, Media streaming, Input
├── ROADMAP.md            # Multi-session development roadmap & execution status
├── deploy/REMOTE_EXEC.md # Windows XP SMBv1 & remote execution (smbexec/iexec) gotchas
├── agent/                # Windows XP Native Agent (C / Win32, i686 subsystem 5.1)
│   ├── Makefile          # mingw32 build file
│   ├── build.sh          # Automated build script with PE import validation
│   └── src/
│       ├── main.c        # Agent entry point, lifecycle, WinMain
│       ├── audio.c/.h    # waveIn capture & WinMM "What U Hear" auto-select
│       ├── video.c/.h    # Low-latency DIBSection / mirror driver capture
│       ├── input.c/.h    # SendInput injection (scancodes for DirectX games)
│       ├── net.c/.h      # UDP/TCP network streaming engine
│       └── discover.c/.h # UDP beacon listener and discovery
├── tools/
│   ├── build-all.sh      # Master build script compiling all XP test tools
│   ├── Makefile          # Top-level tools Makefile
│   ├── README.md         # Comprehensive test suite documentation & verified matrix
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
│       └── xpdash-client/  # egui/wgpu GUI, cpal low-latency audio output
└── deploy/               # Remote push and manual install scripts
    ├── deploy-timemachine.sh
    └── install-agent.bat
```

---

## Development & Building

### Prerequisites

All tools are packaged via Nix:
```sh
nix develop
```

This provides:
- `i686-w64-mingw32-gcc` (stamped subsystem 5.1 for Windows XP SP3)
- `rustc` & `cargo`
- Linux X11/Wayland/ALSA development headers
- `smbclient` & `netexec` for deploying to `timemachine`

### Building the XP Agent
```sh
nix develop -c bash agent/build.sh
# -> outputs agent/bin/xpdash-agent.exe
```

### Building the Host Client / Server
```sh
nix develop -c cargo build --workspace
```

### Building the Windows XP Graphics & Audio Test Suite
```sh
nix develop -c bash tools/build-all.sh
# -> outputs all test executables into tools/bin/
```

---

## License

Licensed under the [MIT License](LICENSE).
