<# .SYNOPSIS
#   One-click installer for the Multivoid r2modman profile.
#   Downloads the profile zip from the private GitHub release and extracts it
#   into r2modman's profiles folder. No explorer tango required.
#
# .USAGE
#   Right-click > Run with PowerShell, or:  powershell -ExecutionPolicy Bypass -File install-profile.ps1
#
#   No token or setup needed. Just run it.

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------- config ---
$Repo        = "Chillcraft06/multivoid-local"
$ReleaseTag  = "dev-latest"
$AssetName   = "Multivoid-r2modman-profile.zip"
# Name of the profile folder once extracted (must match the folder inside the zip).
$ProfileName = "Multivoid"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path

# ------------------------------------------------- close r2modman check ---
$r2 = Get-Process -Name "r2modman" -ErrorAction SilentlyContinue
if ($r2) {
    Write-Host ""
    Write-Host "  r2modman is running. Close it before installing the profile," -ForegroundColor Yellow
    Write-Host "  otherwise files may be locked and the install can fail." -ForegroundColor Yellow
    Write-Host ""
    $ans = Read-Host "  Close r2modman now? (Y/n)"
    if ($ans -notmatch '^[Nn]') {
        Stop-Process -Name "r2modman" -Force
        Start-Sleep -Seconds 2
    }
}

# ------------------------------------------------------- locate profiles ---
# r2modman keeps per-game data under %APPDATA%\r2modmanPlus-local\<Game>\profiles
$base = Join-Path $env:APPDATA "r2modmanPlus-local"
if (-not (Test-Path $base)) {
    Write-Host ""
    Write-Host "  Could not find r2modman data folder at:" -ForegroundColor Red
    Write-Host "    $base" -ForegroundColor Red
    Write-Host "  Is r2modman installed? Install it first, run it once, then re-run this script." -ForegroundColor Red
    Read-Host "  Press Enter to exit"
    exit 1
}

$profileDirs = Get-ChildItem -Path $base -Directory -Recurse -Depth 2 |
    Where-Object { $_.Name -eq "profiles" } |
    Select-Object -ExpandProperty FullName

if (-not $profileDirs) {
    Write-Host ""
    Write-Host "  Found r2modman but no profiles folder. Run r2modman once," -ForegroundColor Red
    Write-Host "  create/select your game, then re-run this script." -ForegroundColor Red
    Read-Host "  Press Enter to exit"
    exit 1
}

if ($profileDirs.Count -eq 1) {
    $ProfilesDir = $profileDirs[0]
} else {
    Write-Host ""
    Write-Host "  Multiple r2modman games found. Which one is Voices of the Void?" -ForegroundColor Cyan
    for ($i = 0; $i -lt $profileDirs.Count; $i++) {
        Write-Host ("    [{0}] {1}" -f ($i + 1), $profileDirs[$i])
    }
    $pick = Read-Host "  Enter number"
    $ProfilesDir = $profileDirs[[int]$pick - 1]
}

Write-Host ""
Write-Host "  Profiles folder: $ProfilesDir" -ForegroundColor Green

# -------------------------------------------------------------- download ---
# The repo is public, so the release asset downloads with no auth.
$downloadUrl = "https://github.com/$Repo/releases/download/$ReleaseTag/$AssetName"
$zipPath = Join-Path $env:TEMP $AssetName
Write-Host ""
Write-Host "  Downloading $AssetName..." -ForegroundColor Cyan
try {
    Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -UseBasicParsing
} catch {
    Write-Host "  Download failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Jordan needs to upload the profile zip to the $ReleaseTag release first." -ForegroundColor Red
    Read-Host "  Press Enter to exit"
    exit 1
}

# --------------------------------------------------------------- extract ---
$dest = Join-Path $ProfilesDir $ProfileName
if (Test-Path $dest) {
    Write-Host "  Removing old '$ProfileName' profile..." -ForegroundColor Yellow
    Remove-Item $dest -Recurse -Force
}
Write-Host "  Extracting profile..." -ForegroundColor Cyan
Expand-Archive -Path $zipPath -DestinationPath $ProfilesDir -Force
Remove-Item $zipPath -Force

Write-Host ""
Write-Host "  Done! Open r2modman and select the '$ProfileName' profile." -ForegroundColor Green
Write-Host ""
Read-Host "  Press Enter to exit"
