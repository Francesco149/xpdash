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
│   └── eax-test/         # Standalone EAX DirectSound3D/OpenAL hardware verification tool
│       ├── build.sh
│       ├── Makefile
│       └── src/main.c
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

### Building & Running the EAX Test App
```sh
nix develop -c bash tools/eax-test/build.sh
# -> outputs tools/eax-test/bin/eax-test.exe
```
