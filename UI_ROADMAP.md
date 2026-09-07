# xpdash UI & Client Architecture Roadmap

This document specifies the complete architectural design, engineering roadmap, and multi-session implementation plan for the **xpdash user-facing ecosystem**: the polished cross-platform client GUI, input confinement and modifier routing subsystem, retro visual shaders, and zero-touch remote/manual deployment packaging.

---

## 1. Executive Vision & Core User Experience

### 1.1 The Remote Retro Gaming Challenge
Playing 3D Windows XP titles over LAN poses unique human-interface challenges that standard remote desktop tools (RDP, VNC, AnyDesk) fail to address:
1. **Unconfined Mouse Trapping**: 3D games (FPS, action, flight simulators) use relative mouse deltas to rotate the camera. In standard remote desktop, the host cursor hits the window edge or second monitor and stops, halting player movement.
2. **Host vs. Guest Key Hijacking**: Pressing `Alt+Tab`, `Super / Windows`, or `Alt+F4` switches host applications rather than interacting with the retro guest.
3. **Aspect Ratio & Display Distortion**: Running classic 4:3 resolutions (640×480, 800×600, 1024×768) on modern 16:9 or 21:9 monitors often produces blurry scaling or unnatural widescreen stretching.
4. **Friction in Deployment**: Non-technical users need a zero-friction path: discover rigs automatically, push updates remotely, or double-click a single standalone installer on Windows XP.

### 1.2 Design Principles
- **Instant Immersion**: Open client, see auto-discovered machines, click Connect, and play with hardware DirectSound3D/EAX audio and 60 fps low-latency video.
- **Explicit Host/Guest Boundary**: Visual indicators (amber border glow, toast HUD) clearly state whether the keyboard and mouse are captured by the retro guest or released to the host.
- **Native Efficiency**: Built with Rust (`egui` + `wgpu`) for lightweight CPU overhead (< 2%), sub-millisecond frame submission, and zero Electron/Chromium baggage.
- **Authentic Retro Aesthetics**: Optional GPU-accelerated CRT scanlines, aperture grille mask, and pixel-perfect integer scaling preserving period aesthetics.

---

## 2. System Architecture & UI State Flow

```mermaid
flowchart TD
    subgraph HostClient [xpdash Client Application]
        Dashboard[Machine Discovery Dashboard]
        Viewport[wgpu Hardware Render Viewport]
        HUD[In-Game Slide-Down HUD Overlay]
        InputMgr[Input & Pointer Confinement Engine]
        SettingsModal[Configuration & Shader Manager]
        DeployWizard[Remote Agent Deployer Wizard]
    end

    subgraph HostOS [Host Operating System - Linux / Win10/11]
        RawInput[winit / Raw Input API]
        AudioOut[cpal Low-Latency Audio Stream]
        KeyHook[Low-Level Keyboard Hook / Wayland Inhibitor]
    end

    subgraph Network [LAN Transport]
        UDP_Media[UDP 7021 - 60fps Video & 48kHz Audio]
        TCP_Ctrl[TCP 7020 - Framing, Ping/Pong, Input Events]
        UDP_Beacon[UDP 7022 - Discovery Beacons]
    end

    subgraph XPAgent [Windows XP Guest]
        AgentNet[net.c TCP/UDP Engine]
        InputInj[input.c SendInput Scancode Injector]
        AudioCap[audio.c What U Hear waveIn]
        VideoCap[video.c DIBSection & fast LZ4]
    end

    RawInput --> InputMgr
    KeyHook --> InputMgr
    InputMgr -- "OP_INPUT_EVENT (dx, dy, scancode)" --> TCP_Ctrl
    TCP_Ctrl --> InputInj
    Dashboard -- "Discover" --> UDP_Beacon
    Dashboard -- "Connect" --> TCP_Ctrl
    UDP_Media --> Viewport
    UDP_Media --> AudioOut
    HUD --> Viewport
    SettingsModal --> Viewport
    DeployWizard -- "SMBv1 / iexec Push" --> XPAgent
    AudioCap --> UDP_Media
    VideoCap --> UDP_Media
```

---

## 3. UI Component Specifications

### 3.1 Machine Discovery Dashboard
The launch screen when opening `xpdash`:
- **Auto-Discovery Grid**:
  - Displays LAN rigs found via UDP 7022 beacons within 500ms.
  - Cards show: Hostname (`TIMEMACHINE-XP`), IP (`10.0.10.113`), OS badge (`Windows XP SP3`), Resolution (`800x600`), and Audio Hardware (`Creative SB0090 Audigy EMU10K2 - EAX 3.0`).
  - Real-time RTT latency badge: Green (`< 5ms`), Yellow (`5-20ms`), Red (`> 20ms`).
  - Security fingerprint status: Green shield for verified/trusted server, amber warning for new fingerprints.
  - Thumbnail snapshot preview updated every 5 seconds.
- **Manual Connect Bar**:
  - Quick-connect input for explicit IP:port entry (for cross-subnet or VPN use).
- **Global Actions**:
  - "Deploy Agent to XP Box" button launching the Remote Deployment Wizard.
  - Client Settings & Shader Preferences.

### 3.2 In-Game Viewport & Low-Latency Renderer
- **Hardware wgpu Surface**:
  - Zero-copy texture streaming: direct upload of decompressed LZ4 BGRA frames into GPU textures (`wgpu::TextureFormat::Bgra8UnormSrgb`).
  - Vertex/Fragment pipeline executing retro CRT shaders with negligible GPU load (< 1% on GTX 1060+).
- **Aspect Ratio & Scaling Options**:
  - `Fit 4:3 (Pillared)`: Preserves period aspect ratio with clean black side pillars on modern monitors.
  - `Integer Scale (1x/2x/3x)`: Pixel-perfect nearest-neighbor scaling for authentic CRT crispness.
  - `Bilinear Stretch`: Smooth edge interpolation for non-integer resolutions.
  - `CRT Scanline Shader`: Configurable scanline intensity (0–100%), aperture grille shadow mask, and slight barrel curvature simulating a Sony Trinitron CRT.

### 3.3 Slide-Down In-Game HUD
Activated by hovering the top 10 pixels of the window or pressing `F10`:
- **Live Stream Diagnostics Bar**:
  - Real-time glass-to-glass latency estimate ($RTT / 2 + Buffer$).
  - Current Video FPS (e.g. `60.0 FPS`), compression ratio, and instantaneous bitrate (`Mbps`).
  - Audio jitter buffer health (e.g. `10ms buffer, 0 underruns, ±1.2ms jitter`).
- **Quick Action Bar**:
  - `Pointer Lock Toggle`: Visual switch (`Confined [Right-Ctrl]`).
  - `Audio Volume Slider` (0% to 150% boost) & Mute button.
  - `Send Ctrl+Alt+Del`: Dedicated one-click trigger sending the security attention sequence to XP.
  - `Display Mode`: Quick toggle between Fullscreen (`F11`), Borderless, and Windowed.
  - `Disconnect`: Clean TCP session termination.

---

## 4. Input Confinement & Modifier Key Routing Subsystem

### 4.1 Pointer Confinement (Game vs. Desktop Mode)

| Mode | Trigger | Mouse Behavior | Coordinate System | Intended Use |
|---|---|---|---|---|
| **Desktop / Unconfined** | Default; toggled via `Right-Ctrl` | Host cursor visible; moves freely across host desktop and window borders. | Absolute screen coordinates mapped to XP desktop ($0..65535$). | Menus, file management, launcher tools, strategy games. |
| **Game / Confined (Locked)** | Click inside window or press `Right-Ctrl` | Host cursor grabbed and hidden (`CursorGrabMode::Locked`). Window borders glow amber. | Raw relative hardware motion deltas ($\Delta X, \Delta Y$). | 3D games (FPS, flight sims, racing) requiring continuous camera rotation. |

- **Confinement Release Hotkey**:
  - Default: **Right Control** (established standard in QEMU, VirtualBox, VMware).
  - Alternate user-configurable presets: `Scroll Lock`, `Ctrl + Alt + Z`, or `F12`.
  - Visual indicator: Subtle 2-pixel ambient border around the viewport (cyan = released, amber = captured).

### 4.2 Granular Modifier Interception Matrix

The settings panel provides independent toggles for system-intercepted keys:

```
[ Input Routing Preferences ]
[X] Capture Windows / Super Key     (Opens Windows XP Start Menu instead of Host menu)
[X] Capture Alt + Tab               (Cycles taskbar windows inside Windows XP)
[X] Capture Alt + F4                (Closes active XP application instead of xpdash)
[X] Capture PrintScreen             (Sends keypress to XP for in-game screenshot tools)
[ ] Enable Host Passthrough Hotkey  (Double-tap Shift to temporarily release input)
```

- **Implementation Mechanism**:
  - **Windows Host**: Installs low-level keyboard hook via `SetWindowsHookEx(WH_KEYBOARD_LL)`. When client has focus and input is captured, hooks consume `VK_LWIN`, `VK_RWIN`, `VK_TAB` (with Alt down), and `VK_F4` (with Alt down), converting them into raw PS/2 scancodes sent over TCP while returning `1` to Windows to suppress host actions.
  - **Linux Host (X11)**: Uses `XGrabKeyboard` / `XGrabPointer` while window is in confined mode.
  - **Linux Host (Wayland)**: Leverages `zwp_keyboard_shortcuts_inhibit_v1` (Wayland shortcut inhibitor protocol supported by GNOME and KDE) and `zwp_pointer_constraints_v1` for pointer locking.

### 4.3 Authentic Hardware Scancode Mapping (DirectInput Compatibility)
Modern Windows/Linux keyboards send USB HID usages; legacy DirectX games (DirectInput 8/9) read hardware PS/2 scan codes. The input engine includes a hardware scancode translation table:

```rust
// PS/2 Set 1 Scancodes for legacy DirectX games
pub fn keycode_to_ps2_scancode(key: KeyCode) -> (u16, bool) {
    match key {
        KeyCode::KeyW => (0x11, false),
        KeyCode::KeyA => (0x1E, false),
        KeyCode::KeyS => (0x1F, false),
        KeyCode::KeyD => (0x20, false),
        KeyCode::Space => (0x39, false),
        KeyCode::Enter => (0x1C, false),
        KeyCode::Escape => (0x01, false),
        KeyCode::Tab => (0x0F, false),
        KeyCode::LeftShift => (0x2A, false),
        KeyCode::LeftControl => (0x1D, false),
        KeyCode::LeftAlt => (0x38, false),
        KeyCode::ArrowUp => (0x48, true),     // Extended prefix 0xE0
        KeyCode::ArrowDown => (0x50, true),   // Extended prefix 0xE0
        KeyCode::ArrowLeft => (0x4B, true),   // Extended prefix 0xE0
        KeyCode::ArrowRight => (0x4D, true),  // Extended prefix 0xE0
        // ... Complete 104-key map
    }
}
```

On Windows XP, `input.c` injects these via:
```c
inp.ki.wScan = scancode;
inp.ki.dwFlags = KEYEVENTF_SCANCODE | (is_extended ? KEYEVENTF_EXTENDEDKEY : 0);
SendInput(1, &inp, sizeof(INPUT));
```

---

## 5. Standalone Packaging & Distribution System

### 5.1 Standalone Client Bundles (Zero Dependencies)
- **Linux**:
  - `xpdash-client` standalone executable with embedded default font and wgpu WGSL shaders.
  - Portable AppImage: `xpdash-x86_64.AppImage` (glibc 2.31+, runs across Debian, Ubuntu, Arch, Fedora, openSUSE).
- **Windows (10 / 11)**:
  - `xpdash-client.exe`: Single self-contained PE binary with statically linked C runtime and DX12/Vulkan backends.
  - Optional `xpdash-setup.exe` installer associating `xpdash://` URL protocol.

### 5.2 Windows XP Native Agent Packaging
- **Zero-Dependency Native PE Subsystem 5.1 Binary**:
  - `xpdash-agent.exe` (119 KB), imports only stock Windows XP DLLs (`KERNEL32`, `USER32`, `GDI32`, `ADVAPI32`, `WS2_32`, `WINMM`, `msvcrt`).
- **Public Installer (`xpdash-agent-setup.exe`)**:
  - Installs to `C:\xpdash\`.
  - Configures Windows Firewall exception via `netsh firewall add allowedprogram "C:\xpdash\xpdash-agent.exe" "xpdash Agent" ENABLE`.
  - Registers system autostart under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run`.
  - Verifies Sound Blaster mixer line ("What U Hear" active) and tests audio loopback.
  - Installs clean uninstaller in `Add/Remove Programs`.

### 5.3 One-Click Remote Deployer (Integrated into Client)
For headless rigs or networked XP machines:
1. User opens **Deploy Agent** in client UI.
2. Enters Target IP (default scanned), Username (`Administrator`), and Password (default blank).
3. Client executes automated remote staging over SMB/WMI:
   - Stops any running `xpdash-agent.exe`.
   - Copies `xpdash-agent.exe` and `agent.ini` to `\\<IP>\C$\xpdash\`.
   - Adds Windows Firewall rule via remote command.
   - Launches `C:\probe\iexec.exe C:\xpdash\xpdash-agent.exe` on console session 0.
   - Verifies discovery beacon reception within 3 seconds and highlights rig as ready in the dashboard.

---

## 6. Multi-Session Implementation Roadmap (Sessions 6 to 10)

| Session | Focus Area | Key Deliverables | Validation Target |
|---|---|---|---|
| **Session 6** | **Native GUI Framework & Dashboard** | Integration of `egui` + `wgpu` into `xpdash-client`, Auto-discovery machine grid, live latency/health badges, zero-copy texture presentation. | Launch client, auto-discover `timemachine`, see 60 fps live preview in GUI window. |
| **Session 7** | **Input Confinement & Modifier Routing** | Mouse pointer lock (`Right-Ctrl` toggle), low-level keyboard hook / Wayland shortcut inhibitor, Win-Key and Alt+Tab capture toggles, PS/2 scancode injection. | Play GTA San Andreas with 360° mouse rotation, Alt+Tab within XP, Win key opening XP Start Menu. |
| **Session 8** | **In-Game HUD, Audio Mixer & Retro Shaders** | Slide-down top HUD drawer, real-time RTT/FPS/jitter telemetry overlay, volume slider with VU meter, CRT scanline & integer scaling shaders. | Toggle CRT scanlines at 800x600, view live diagnostics HUD, adjust audio volume on the fly. |
| **Session 9** | **Remote Deployer Wizard & Standalone Packaging** | In-app SMB/WMI deployment dialog, standalone XP NSIS installer, portable Linux AppImage and Windows single `.exe` bundle. | One-click deploy to fresh XP machine, install via setup package, verify firewall and service persistence. |
| **Session 10** | **Web Client Gateway (Browser Access)** | WebSocket proxy gateway (`host/crates/xpdash-web`), WebCodecs video decompressor, WebAudio 48kHz PCM output. | Stream and play Windows XP retro games inside Chrome/Firefox over LAN with zero local installs. |

---

## 7. Immediate Next Steps: Session 6 Preparation
1. Add `egui`, `egui-wgpu`, and `winit` dependencies to `host/crates/xpdash-client/Cargo.toml`.
2. Scaffold `host/crates/xpdash-client/src/ui/` containing:
   - `dashboard.rs`: Machine grid and status cards.
   - `viewport.rs`: wgpu texture presentation surface.
   - `input_handler.rs`: Mouse capture and key routing.
   - `hud.rs`: Slide-down overlay and metrics display.
3. Wire discovery beacon listener into UI state to dynamically populate discovered rigs.
