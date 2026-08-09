@echo off
REM GC-Chiptune : launcher for the devkitPPC + libogc2 installation.
REM
REM Exists for one reason only: the equivalent PowerShell command is too long to
REM paste without risking a line break. Here one short line is enough.
REM
REM   Start-Process "<repo>\install-devkitpro.cmd" -Verb RunAs
REM
REM Or: right-click this file -> "Run as administrator".

setlocal
set "SCRIPT=%~dp0tools\install-devkitpro.ps1"

if not exist "%SCRIPT%" (
    echo FAILED: script not found: "%SCRIPT%"
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
set "RC=%ERRORLEVEL%"

echo.
echo ---------------------------------------------------------------
if "%RC%"=="0" (
    echo Done. This window can be closed.
) else (
    echo Finished with code %RC% - see the messages above.
)
echo ---------------------------------------------------------------
pause
exit /b %RC%
