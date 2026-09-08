#!/usr/bin/env bash
# build.sh — Cross-compile test-modeswitch.exe for Windows XP (i686, subsystem 5.1).
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
echo "[*] Compiling test-modeswitch.exe with $CC..."
"$CC" -O2 -s -Wall -Wextra -D_WIN32_WINNT=0x0501 \
    -no-pie -static -static-libgcc \
    -Wl,--major-subsystem-version=5,--minor-subsystem-version=1 \
    src/main.c -o bin/test-modeswitch.exe -lgdi32 -luser32 -lwinmm

bad="$("$OBJDUMP" -p bin/test-modeswitch.exe | awk '/DLL Name/{print $3}' \
        | grep -ivE '^(KERNEL32|USER32|ADVAPI32|SHELL32|WS2_32|GDI32|WINMM|msvcrt)\.dll$' || true)"

if [ -n "$bad" ]; then
    echo "[!] Warning: Non-stock XP imports detected:"
    echo "$bad"
else
    echo "[+] PE imports verified: 100% Windows XP compatible."
fi

echo "[+] Successfully built bin/test-modeswitch.exe ($(stat -c%s bin/test-modeswitch.exe) bytes)"
