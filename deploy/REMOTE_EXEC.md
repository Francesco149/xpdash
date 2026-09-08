# Windows XP Remote Execution & SMB Gotchas Guide

This document records the exact mechanics, gotchas, and working command patterns for interacting with Windows XP target machines (e.g. `timemachine` at `10.0.10.113`, `q9650` at `10.0.10.114`) over LAN.

---

## 1. SMBv1 Protocol (`smbclient`)

Windows XP SP3 only supports **SMBv1 (NT1)**. Modern Linux `smbclient` defaults to SMB2/3 and will reject connections or fail negotiation unless NT1 is explicitly forced.

### Working Pattern
```bash
smbclient "//10.0.10.113/C$" -U 'Administrator%' -m NT1 --option='client min protocol=NT1' -c "
prompt OFF;
cd \\probe;
lcd /local/path;
put filename.ext;
get remote_file.ext /local/path/dest.ext;
"
```

### Key Gotchas
- **`-m NT1`**: Forces NT1 dialect.
- **`--option='client min protocol=NT1'`**: Prevents client-side protocol version assertion errors.
- **`-U 'Administrator%'`**: Specifies empty password with `%` suffix.
- Double-backslashes `\\` are required when referencing remote Windows paths inside `-c "..."`.

---

## 2. Remote Command Execution (`netexec`)

`netexec` (`nix run nixpkgs#netexec -- smb <IP> ...`) provides two primary execution methods on Windows XP:

### 2.1 `wmiexec` (Default) vs `smbexec` (SYSTEM)
- **`wmiexec`**: Executes commands via WMI under the authenticated user (`Administrator`).
  - **Limitation**: On Windows XP, the Administrator user in non-interactive network sessions does **not** have `SE_TCB_NAME` privilege.
  - Calling `iexec.exe` under `wmiexec` **fails** with:
    ```
    iexec: WTSQueryUserToken(sid=0) failed 1314 (need SYSTEM + a logged-in console user)
    ```
  - **Best for**: Non-interactive command inspection (`type`, `dir`, `tasklist`, `reg query`).
    ```bash
    nix run nixpkgs#netexec -- smb 10.0.10.113 -u Administrator -p '' -x 'tasklist'
    ```

- **`smbexec` (`--exec-method smbexec`)**:
  - Installs and starts a temporary Windows service via the Service Control Manager (SCM).
  - Commands execute as **`NT AUTHORITY\SYSTEM`**.
  - Has `SE_TCB_NAME` privilege.
  - **Required** when invoking `iexec.exe` to launch interactive GUI processes on console session 0.

### 2.2 Command Quoting & Redirection in `smbexec`
- `smbexec` wraps commands inside a temporary service batch command: `cmd.exe /Q /c <command>`.
- Passing complex command lines with nested quotes, `&&`, or redirection operators (`>`, `2>&1`) directly in `-x` frequently triggers syntax errors:
  ```
  The filename, directory name, or volume label syntax is incorrect.
  ```
- **Golden Rule**: **Always stage a `.bat` file on the XP host via `smbclient`, then run the batch file via `smbexec`**:
  ```bash
  # Step 1: Upload batch script
  cat << 'EOF' > /tmp/run_task.bat
  @echo off
  cd /d "C:\target\directory"
  C:\probe\iexec.exe C:\target\app.exe > C:\probe\out\result.txt 2>&1
  EOF

  smbclient "//10.0.10.113/C$" -U 'Administrator%' -m NT1 --option='client min protocol=NT1' -c "
  cd \\probe;
  lcd /tmp;
  put run_task.bat;
  "

  # Step 2: Execute via smbexec
  nix run nixpkgs#netexec -- smb 10.0.10.113 -u Administrator -p '' --exec-method smbexec -x 'cmd.exe /c C:\probe\run_task.bat'

  # Step 3: Inspect output via wmiexec or smbclient
  nix run nixpkgs#netexec -- smb 10.0.10.113 -u Administrator -p '' -x 'type C:\probe\out\result.txt'
  ```

---

## 3. Interactive Console Execution (`iexec.exe`)

Windows XP service sessions (session 0 isolation or network logons) cannot directly show windows or interact with the physical display without targeting the active console session.

- **Tool**: `C:\probe\iexec.exe` (installed on `timemachine`).
- **Function**: Uses `WTSQueryUserToken(0)` to duplicate the active logged-in console user's token and spawns the target process inside the interactive desktop (`WinSta0\Default`).
- **Prerequisites**:
  1. Must be invoked with **SYSTEM privileges** (i.e. via `smbexec`, never `wmiexec`).
  2. An active user must be logged in on console session 0 (`query user` shows `>console Administrator 0 Active`).
- **Working Directory**:
  - Legacy games (like *Lords of the Realm II*, *Fallout*, etc.) depend on finding assets relative to their current working directory.
  - `iexec.exe` does not change the working directory itself. Always wrap in a `.bat` that calls `cd /d "C:\Games\..."` before launching the game binary.
