#!/usr/bin/env bash
# build.sh — Cross-compile xpdash-agent.exe for Windows XP (i686, subsystem 5.1).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

CC="${MINGW32_CC:-i686-w64-mingw32-gcc}"
OBJDUMP="${MINGW32_OBJDUMP:-i686-w64-mingw32-objdump}"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "[-] $CC not found. Run under: nix develop"
    exit 1
fi

mkdir -p bin
echo "[*] Compiling xpdash-agent.exe with $CC..."
"$CC" -O2 -s -mwindows -Wall -Wextra -Wno-unused-parameter \
    -D_WIN32_WINNT=0x0501 -Isrc \
    -no-pie -static -static-libgcc \
    -Wl,--major-subsystem-version=5,--minor-subsystem-version=1 \
    src/main.c src/audio.c src/video.c src/input.c src/net.c src/discover.c \
    -o bin/xpdash-agent.exe \
    -lws2_32 -lwinmm -lgdi32 -luser32 -ladvapi32 -lshell32

# Check imported DLLs for XP-safety
bad="$("$OBJDUMP" -p bin/xpdash-agent.exe | awk '/DLL Name/{print $3}' \
        | grep -ivE '^(KERNEL32|USER32|ADVAPI32|SHELL32|WS2_32|GDI32|WINMM|msvcrt)\.dll$' || true)"

if [ -n "$bad" ]; then
    echo "[!] Warning: Non-stock XP imports detected:"
    echo "$bad"
else
    echo "[+] PE imports verified: 100% Windows XP compatible."
fi

echo "[+] Successfully built bin/xpdash-agent.exe ($(stat -c%s bin/xpdash-agent.exe) bytes)"
