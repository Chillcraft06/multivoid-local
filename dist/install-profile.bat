@echo off
rem One-click launcher for install-profile.ps1.
rem
rem Runs the installer with -ExecutionPolicy Bypass. A script downloaded from
rem the internet carries a "Mark of the Web", and the default PowerShell
rem execution policy (RemoteSigned) refuses to run it -- the window just
rem flashes red and vanishes. This wrapper sidesteps that, exactly like the
rem in-game updater does when it launches its own updater.ps1.
rem
rem Just double-click this file. (You may get a SmartScreen "unknown publisher"
rem prompt the first time -- that's normal for a downloaded file.)

set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install-profile.ps1"
echo.
pause
