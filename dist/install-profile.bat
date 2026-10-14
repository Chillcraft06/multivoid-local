@echo off
rem Multivoid one-click profile installer launcher.
rem
rem Downloads install-profile.ps1 from the repo's dist folder and runs it
rem with -ExecutionPolicy Bypass. A script downloaded from the internet
rem carries a "Mark of the Web", and the default PowerShell execution policy
rem (RemoteSigned) refuses to run it -- the window just flashes red and
rem vanishes. This launcher sidesteps that, exactly like the in-game updater
rem does when it launches its own updater.ps1.
rem
rem Just double-click this file. (You may get a SmartScreen "unknown publisher"
rem prompt the first time -- that's normal for a downloaded file.)

set "PS1_URL=https://raw.githubusercontent.com/Chillcraft06/multivoid-local/main/dist/install-profile.ps1"
set "PS1_FILE=%TEMP%\multivoid-install-profile.ps1"

echo Downloading installer script...
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Invoke-WebRequest -Uri '%PS1_URL%' -OutFile '%PS1_FILE%' -UseBasicParsing } catch { Write-Host ('Download failed: ' + $_.Exception.Message); exit 1 }"
if not exist "%PS1_FILE%" (
    echo.
    echo Could not download the installer script. Check your internet connection and try again.
    echo.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"
del "%PS1_FILE%" >nul 2>&1
echo.
pause
