@echo off
REM install-agent.bat — Windows XP Standalone Public Installer for xpdash
title xpdash Windows XP Agent Installer
echo ======================================================
echo           xpdash Windows XP Agent Installer
echo ======================================================
echo.

set TARGET_DIR=%SystemDrive%\xpdash

if not exist "%TARGET_DIR%" (
    echo [*] Creating directory %TARGET_DIR%...
    mkdir "%TARGET_DIR%"
)

if not exist "xpdash-agent.exe" (
    echo [!] Error: xpdash-agent.exe not found in current directory!
    echo     Please extract all files before running install-agent.bat.
    echo.
    pause
    exit /b 1
)

echo [*] Copying xpdash-agent.exe to %TARGET_DIR%...
copy /Y xpdash-agent.exe "%TARGET_DIR%\xpdash-agent.exe" >nul
if errorlevel 1 (
    echo [!] Error: Failed to copy xpdash-agent.exe.
    echo     Make sure you have Administrator privileges.
    echo.
    pause
    exit /b 1
)

if exist "agent.ini" (
    echo [*] Copying pre-configured agent.ini to %TARGET_DIR%...
    copy /Y agent.ini "%TARGET_DIR%\agent.ini" >nul
) else (
    if not exist "%TARGET_DIR%\agent.ini" (
        echo [*] Generating default agent.ini...
        (
            echo # xpdash Agent Configuration
            echo [security]
            echo # Comma-separated list of trusted server fingerprints
            echo # Or "*" to trust all LAN servers
            echo trusted_fingerprints=
            echo # Prompt user on interactive console when new server is discovered
            echo prompt_user=1
            echo # Disable security checks completely ^(1 = trust all, 0 = prompt/whitelist^)
            echo allow_all=0
        ) > "%TARGET_DIR%\agent.ini"
    )
)

echo [*] Configuring Windows Firewall for xpdash...
netsh firewall add allowedprogram "%TARGET_DIR%\xpdash-agent.exe" "xpdash Agent" ENABLE >nul 2>&1
netsh firewall add portopening TCP 7020 "xpdash Control" ENABLE >nul 2>&1
netsh firewall add portopening UDP 7021 "xpdash Media" ENABLE >nul 2>&1
netsh firewall add portopening UDP 7022 "xpdash Discovery" ENABLE >nul 2>&1

echo [*] Registering autostart with Windows...
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v "xpdash-agent" /t REG_SZ /d "%TARGET_DIR%\xpdash-agent.exe" /f >nul

echo.
echo ======================================================
echo           Installation Successful!
echo ======================================================
echo  - Location: %TARGET_DIR%\xpdash-agent.exe
echo  - Autostart: Enabled (HKLM Run)
echo  - Firewall: Configured (Ports 7020 TCP, 7021/7022 UDP)
echo.
echo Starting xpdash Agent now...
start "" "%TARGET_DIR%\xpdash-agent.exe"
echo Done.
echo ======================================================
pause
