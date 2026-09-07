@echo off
REM uninstall-agent.bat — Windows XP Standalone Public Uninstaller for xpdash
title xpdash Windows XP Agent Uninstaller
echo ======================================================
echo           xpdash Windows XP Agent Uninstaller
echo ======================================================
echo.

set TARGET_DIR=%SystemDrive%\xpdash

echo [*] Stopping running xpdash-agent.exe processes...
taskkill /f /im xpdash-agent.exe >nul 2>&1

echo [*] Removing autostart registry entry...
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v "xpdash-agent" /f >nul 2>&1

echo [*] Removing Windows Firewall rules...
netsh firewall delete allowedprogram "%TARGET_DIR%\xpdash-agent.exe" >nul 2>&1
netsh firewall delete portopening TCP 7020 >nul 2>&1
netsh firewall delete portopening UDP 7021 >nul 2>&1
netsh firewall delete portopening UDP 7022 >nul 2>&1

echo [*] Removing files from %TARGET_DIR%...
del /q "%TARGET_DIR%\xpdash-agent.exe" >nul 2>&1
del /q "%TARGET_DIR%\agent.log" >nul 2>&1

echo.
echo Note: agent.ini and trusted_servers.ini were preserved.
echo To completely remove all configurations, delete %TARGET_DIR% manually.
echo.
echo ======================================================
echo           Uninstallation Complete!
echo ======================================================
pause
