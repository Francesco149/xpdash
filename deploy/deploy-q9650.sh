#!/usr/bin/env bash
# deploy-q9650.sh — Build and push xpdash-agent.exe to q9650 rig (10.0.10.134)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

XP="${1:-10.0.10.134}"

echo "=== Building xpdash-agent.exe ==="
nix develop --command bash agent/build.sh

echo "=== Deploying to $XP (C:\xpdash\) ==="
# Stop running agent if any
nix run nixpkgs#netexec -- smb "$XP" -u Administrator -p '' -x 'taskkill /f /im xpdash-agent.exe 2>nul' >/dev/null || true

# Copy agent and agent.ini to C:\xpdash\
smbclient "//$XP/C$" -U 'Administrator%' -m NT1 --option='client min protocol=NT1' -c "
prompt OFF;
mkdir \\xpdash;
cd \\xpdash;
lcd agent/bin;
put xpdash-agent.exe;
lcd ../../deploy;
put agent.ini;
"

echo "=== Launching xpdash-agent.exe on console session ==="
nix run nixpkgs#netexec -- smb "$XP" -u Administrator -p '' --exec-method smbexec -x 'C:\probe\iexec.exe C:\xpdash\xpdash-agent.exe'

echo "=== Deployment to $XP complete ==="
