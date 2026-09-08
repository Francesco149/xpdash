# Windows XP Graphics & Display Test Suite

Comprehensive test application suite for verifying Windows XP graphics APIs, color depths, display resolutions, and streaming behavior under `xpdash`.

All tools are compiled as 32-bit PE binaries targeting subsystem 5.1 (Windows XP SP3) with **100% stock Windows XP DLL imports** (no external runtime dependencies or non-stock SDK DLLs like D3DX).

---

## Tool Matrix & API Coverage

| Tool | Binary | Graphics API | Target Scenarios / Coverage | Color Depths | Display Modes |
|---|---|---|---|---|---|
| **test-gdi** | `test-gdi.exe` | Win32 GDI | Double-buffered memory DC, dirty rects, SMPTE color bars, color ramps, animated DVD box, 8-bit palette cycling (`AnimatePalette`) | 8-bit, 16-bit, 32-bit | Windowed & Fullscreen |
| **test-ddraw** | `test-ddraw.exe` | DirectDraw 7 | Hardware primary flipping surfaces, `IDirectDrawClipper` windowed blits, procedural plasma, `IDirectDrawPalette` cycling, GDI interop via surface `GetDC` | 8-bit, 16-bit (565/555), 32-bit | Exclusive Fullscreen & Windowed |
| **test-d3d8** | `test-d3d8.exe` | Direct3D 8 | 3D Gouraud-shaded rotating lit cube, depth buffering (`D3DFMT_D16`), 2D screen-space HUD overlay (`D3DFVF_XYZRHW`), device reset on display changes | 16-bit (`R5G6B5`), 32-bit (`X8R8G8B8`) | Exclusive Fullscreen & Windowed |
| **test-d3d9** | `test-d3d9.exe` | Direct3D 9 | 3D rotating cube, hook injection testing (Path A), GDI BitBlt capture fallback (Path B), presentation intervals, device lost/reset | 16-bit (`R5G6B5`), 32-bit (`X8R8G8B8`) | Exclusive Fullscreen & Windowed |
| **test-opengl** | `test-opengl.exe` | Win32 WGL + OpenGL 1.1 | Double-buffered 3D scene (rotating shaded pyramid & cube), depth testing, 2D orthographic color bars HUD, WGL context creation | 16-bit, 24-bit, 32-bit | Windowed & Fullscreen |
| **test-modeswitch** | `test-modeswitch.exe` | Win32 Display | Automated matrix cycling across 11 resolution/color depth combinations, `WM_DISPLAYCHANGE` stress test, dynamic buffer reallocation validation | 8-bit, 16-bit, 32-bit | Fullscreen |
| **probe-video** | `probe-video.exe` | Win32 GDI Caps | Probes display caps (`BITSPIXEL`, `PLANES`, `RASTERCAPS`, `SIZEPALETTE`), system DAC palette dumps, BitBlt capture validation | 8-bit, 16-bit, 32-bit | Desktop DC |
| **eax-test** | `eax-test.exe` | OpenAL / DirectSound3D | Hardware EAX 1.0/2.0/3.0/4.0 EMU10K2 DSP reverb DSP verification on Sound Blaster cards | 16-bit 48kHz stereo PCM | Hardware DSP |

---

## Building

All tools can be built in one command using the Nix development shell:

```bash
# Build all tools and copy to tools/bin/
nix develop -c bash tools/build-all.sh
```

Or build individual tools:
```bash
nix develop -c bash tools/test-gdi/build.sh
nix develop -c bash tools/test-ddraw/build.sh
nix develop -c bash tools/test-d3d8/build.sh
nix develop -c bash tools/test-d3d9/build.sh
nix develop -c bash tools/test-opengl/build.sh
nix develop -c bash tools/test-modeswitch/build.sh
```

Every build script automatically validates PE imports against a strict whitelist of stock Windows XP SP3 DLLs:
`KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `ADVAPI32.dll`, `SHELL32.dll`, `WS2_32.dll`, `WINMM.dll`, `DDRAW.dll`, `D3D8.dll`, `D3D9.dll`, `OPENGL32.dll`, `GLU32.dll`, `msvcrt.dll`.

---

## Command-Line Arguments & Controls

### Common Command-Line Flags
- `--fullscreen` / `-f`: Start in fullscreen mode
- `--windowed` / `-w`: Start in windowed mode
- `--res WxH`: Set resolution (e.g. `--res 800x600`)
- `--bpp N`: Set color depth (`8`, `16`, `32`)
- `--auto SECONDS`: Run for N seconds and exit cleanly with status code 0 (ideal for automated remote testing)

### Common Interactive Key Bindings
- `F` / `Enter`: Toggle Fullscreen / Windowed
- `1`: Switch to 640x480
- `2`: Switch to 800x600
- `3`: Switch to 1024x768
- `4`: Switch to 1280x1024
- `0`: Restore original desktop resolution
- `P`: Toggle palette cycling (in 8-bit paletted modes)
- `Esc` / `Q`: Exit cleanly and restore desktop resolution

### `test-modeswitch.exe` Specific Flags
- `--matrix`: Run automated test sequence across all 11 resolution/color combinations
- `--sec N`: Duration per mode in seconds (default: 3s)
- `--list`: List all supported display modes of the current graphics adapter
- `Space`: Manually advance to the next mode in the matrix

---

## Verified Hardware Results (`timemachine` i7-4790K + GTX 750 Ti)

| Test Case | Mode / Configuration | Frames | FPS | RTT | Stream Bitrate | Video / Audio Drops |
|---|---|---|---|---|---|---|
| **Win32 GDI** | 800x600 @ 32bpp Windowed | 338 | 67.6 | 0.58 ms | 47.3 Mbps | 0 drops / 0 drops |
| **DirectDraw 7** | 800x600 @ 32bpp Fullscreen Flip | 121 | 24.2 | 0.46 ms | 10.7 Mbps | 0 drops / 0 drops |
| **DirectDraw 7** | 800x600 @ 8bpp Paletted Flip | 119 | 23.8 | 0.48 ms | 9.2 Mbps | 0 drops / 0 drops |
| **DirectDraw 7** | 800x600 @ 16bpp High Color Flip | 116 | 23.2 | 0.51 ms | 9.8 Mbps | 0 drops / 0 drops |
| **Direct3D 8** | 800x600 @ 32bpp Fullscreen | 169 | 33.8 | 1.94 ms | 10.4 Mbps | 0 drops / 0 drops |
| **Direct3D 9** | 800x600 @ 32bpp Fullscreen | 169 | 33.8 | 0.26 ms | 10.3 Mbps | 0 drops / 0 drops |
| **OpenGL 1.1** | 800x600 @ 32bpp Windowed WGL | 338 | 67.6 | 0.27 ms | 33.4 Mbps | 0 drops / 0 drops |
| **Mode Matrix** | 640x480..1280x1024 (8/16/32bpp) | 574 | Dynamic | 0.85 ms | 21.9–89.3 Mbps | 0 drops / 0 drops |
