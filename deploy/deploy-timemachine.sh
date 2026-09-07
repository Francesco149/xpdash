#!/usr/bin/env bash
# deploy-timemachine.sh — Build and push xpdash-agent.exe to timemachine (10.0.10.113)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

XP="${1:-10.0.10.113}"

echo "=== Building xpdash-agent.exe and xpdash-hook.dll ==="
nix develop --command bash agent/build.sh

echo "=== Deploying to $XP (C:\xpdash\) ==="
# Stop running agent if any
nix run nixpkgs#netexec -- smb "$XP" -u Administrator -p '' -x 'taskkill /f /im xpdash-agent.exe 2>nul' >/dev/null || true

# Copy agent to C:\xpdash\
smbclient "//$XP/C$" -U 'Administrator%' -m NT1 --option='client min protocol=NT1' -c "
prompt OFF;
mkdir \\xpdash;
cd \\xpdash;
lcd agent/bin;
put xpdash-agent.exe;
put xpdash-hook.dll;
lcd ../../deploy;
put agent.ini;
"

echo "=== Launching xpdash-agent.exe on console session ==="
nix run nixpkgs#netexec -- smb "$XP" -u Administrator -p '' --exec-method smbexec -x 'C:\probe\iexec.exe C:\xpdash\xpdash-agent.exe'

echo "=== Deployment to $XP complete ==="
