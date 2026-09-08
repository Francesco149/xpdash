#!/usr/bin/env bash
# scripts/package-release.sh — Build and package standalone release artifacts for Linux and Windows XP
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "========================================================="
echo " Packaging xpdash Standalone Release Bundles"
echo "========================================================="

DIST_DIR="dist"
mkdir -p "$DIST_DIR"

# 1. Build Linux Standalone Binaries
echo ""
echo "[1/4] Building Linux release binaries (xpdash-client, xpdash-server)..."
cargo build --manifest-path host/Cargo.toml --release --bin xpdash-client --bin xpdash-server

# 2. Build Windows 10/11 Client (xpdash-client.exe)
echo ""
echo "[2/6] Building Windows 10/11 client binary (xpdash-client.exe)..."
WIN_CLIENT_BIN=""
if [[ "$OSTYPE" == "msys"* || "$OSTYPE" == "cygwin"* || "$OSTYPE" == "win32"* ]]; then
    cargo build --manifest-path host/Cargo.toml --release --bin xpdash-client
    WIN_CLIENT_BIN="host/target/release/xpdash-client.exe"
else
    # Cross-compile for Windows x86_64 via MinGW
    if cargo build --manifest-path host/Cargo.toml --release --bin xpdash-client --target x86_64-pc-windows-gnu; then
        WIN_CLIENT_BIN="host/target/x86_64-pc-windows-gnu/release/xpdash-client.exe"
    else
        echo "[!] Warning: Cross-compilation to x86_64-pc-windows-gnu failed; skipping Windows client."
    fi
fi

# 3. Build Windows XP Agent and All Test Tools
echo ""
echo "[3/6] Building Windows XP agent and tools suite with MinGW subsystem 5.1..."
if command -v nix >/dev/null 2>&1 && [ -f flake.nix ]; then
    nix develop -c bash agent/build.sh
    nix develop -c bash tools/build-all.sh
else
    bash agent/build.sh
    bash tools/build-all.sh
fi
echo ""
echo "[4/6] Creating Linux standalone tarball..."
LINUX_PKG_DIR="$DIST_DIR/xpdash-linux-x86_64"
rm -rf "$LINUX_PKG_DIR"
mkdir -p "$LINUX_PKG_DIR"

cp host/target/release/xpdash-client "$LINUX_PKG_DIR/"
cp host/target/release/xpdash-server "$LINUX_PKG_DIR/"
strip "$LINUX_PKG_DIR/xpdash-client"
strip "$LINUX_PKG_DIR/xpdash-server"

cp LICENSE "$LINUX_PKG_DIR/"
cat << 'EOF' > "$LINUX_PKG_DIR/run-client.sh"
#!/usr/bin/env bash
# Launch xpdash client in native GUI or OBS mode
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/xpdash-client" "$@"
EOF
chmod +x "$LINUX_PKG_DIR/run-client.sh"

cat << 'EOF' > "$LINUX_PKG_DIR/run-obs.sh"
#!/usr/bin/env bash
# Launch xpdash client in OBS 1x fixed source size mode
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/xpdash-client" --obs "$@"
EOF
chmod +x "$LINUX_PKG_DIR/run-obs.sh"

tar -czf "$DIST_DIR/xpdash-linux-x86_64.tar.gz" -C "$DIST_DIR" "xpdash-linux-x86_64"
echo "[+] Created: $DIST_DIR/xpdash-linux-x86_64.tar.gz ($(stat -c%s "$DIST_DIR/xpdash-linux-x86_64.tar.gz") bytes)"

# 5. Package Windows 10/11 Client Bundle
if [ -n "$WIN_CLIENT_BIN" ] && [ -f "$WIN_CLIENT_BIN" ]; then
    echo ""
    echo "[5/6] Creating Windows 10/11 client zip bundle..."
    WIN_PKG_DIR="$DIST_DIR/xpdash-client-windows-x86_64"
    rm -rf "$WIN_PKG_DIR"
    mkdir -p "$WIN_PKG_DIR"

    cp "$WIN_CLIENT_BIN" "$WIN_PKG_DIR/"
    cp LICENSE "$WIN_PKG_DIR/"

    cat << 'EOF' > "$WIN_PKG_DIR/run-client.bat"
@echo off
start "" "%~dp0xpdash-client.exe" %*
EOF

    cat << 'EOF' > "$WIN_PKG_DIR/run-obs.bat"
@echo off
start "" "%~dp0xpdash-client.exe" --obs %*
EOF

    (cd "$DIST_DIR" && zip -rq "xpdash-client-windows-x86_64.zip" "xpdash-client-windows-x86_64")
    echo "[+] Created: $DIST_DIR/xpdash-client-windows-x86_64.zip ($(stat -c%s "$DIST_DIR/xpdash-client-windows-x86_64.zip") bytes)"
fi

# 6. Package Windows XP Agent & Tools Bundle
echo ""
echo "[6/6] Creating Windows XP agent & tools zip bundle..."
WINXP_PKG_DIR="$DIST_DIR/xpdash-agent-winxp"
rm -rf "$WINXP_PKG_DIR"
mkdir -p "$WINXP_PKG_DIR/tools"

cp agent/bin/xpdash-agent.exe "$WINXP_PKG_DIR/"
cp agent/bin/xpdash-hook.dll "$WINXP_PKG_DIR/"
cp agent/bin/xpdash-hook9.dll "$WINXP_PKG_DIR/"
cp deploy/agent.ini "$WINXP_PKG_DIR/"
cp deploy/install-agent.bat "$WINXP_PKG_DIR/"
cp deploy/uninstall-agent.bat "$WINXP_PKG_DIR/"
cp tools/bin/*.exe "$WINXP_PKG_DIR/tools/"
cp LICENSE "$WINXP_PKG_DIR/"

(cd "$DIST_DIR" && zip -rq "xpdash-agent-winxp.zip" "xpdash-agent-winxp")
echo "[+] Created: $DIST_DIR/xpdash-agent-winxp.zip ($(stat -c%s "$DIST_DIR/xpdash-agent-winxp.zip") bytes)"

echo ""
echo "========================================================="
echo " Release Artifacts Packaged in $DIST_DIR/:"
ls -lh "$DIST_DIR"/*.tar.gz "$DIST_DIR"/*.zip
echo "========================================================="
