@echo off
REM install-agent.bat — Windows XP Standalone Public Installer for xpdash
echo ======================================================
echo           xpdash Windows XP Agent Installer
echo ======================================================
echo.

if not exist "%SystemDrive%\xpdash" (
    mkdir "%SystemDrive%\xpdash"
)

echo Copying xpdash-agent.exe to %SystemDrive%\xpdash...
copy /Y xpdash-agent.exe "%SystemDrive%\xpdash\xpdash-agent.exe" >nul
if errorlevel 1 (
    echo Error: Failed to copy xpdash-agent.exe.
    pause
    exit /b 1
)

echo Creating autostart registry entry...
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v "xpdash-agent" /t REG_SZ /d "%SystemDrive%\xpdash\xpdash-agent.exe" /f >nul

echo.
echo ======================================================
echo Installation Successful!
echo xpdash will automatically start with Windows.
echo Starting agent now...
echo ======================================================
start "" "%SystemDrive%\xpdash\xpdash-agent.exe"
