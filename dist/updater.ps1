# updater.ps1 -- Multivoid self-updater (the private "just for us" fork).
#
# Launched from the mod's F1 menu (Mod > Update). It:
#   1. Checks the rolling dev-latest GitHub release for a newer commit SHA.
#   2. Downloads the zip (with progress) if one is available.
#   3. Asks before closing the game, closes it gracefully, installs, relaunches.
#
# The repo is public, so the GitHub API works with no token at all. If you ever
# hit API rate limits, you can drop a fine-grained PAT with Contents set to
# READ-ONLY (nothing else) on Chillcraft06/multivoid-local in updater_token.txt
# next to this script. The installer preserves that file across updates.

param(
    [int]$GamePid = 0,
    [string]$GameExe = "",
    [string]$ModDir = ""
)

$ErrorActionPreference = "Stop"

$repo = "Chillcraft06/multivoid-local"
$releaseTag = "dev-latest"

function Pause-Exit([string]$msg) {
    Write-Host ""
    Write-Host $msg
    Read-Host "Press Enter to close"
    exit 0
}

if (-not $ModDir -or -not (Test-Path -LiteralPath $ModDir)) {
    Pause-Exit "Mod folder not found: $ModDir"
}
if (-not $GameExe -or -not (Test-Path -LiteralPath $GameExe)) {
    Pause-Exit "Game exe not found: $GameExe"
}

$tokenFile = Join-Path $ModDir "updater_token.txt"
$token = $null
if (Test-Path -LiteralPath $tokenFile) { $token = (Get-Content -Raw $tokenFile).Trim() }

$headers = @{
    "Accept" = "application/vnd.github+json"
}
if ($token) { $headers["Authorization"] = "Bearer $token" }

# --- version check -------------------------------------------------------
Write-Host "Checking for updates..."
try {
    $release = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/$repo/releases/tags/$releaseTag" `
        -Headers $headers -UserAgent "multivoid-updater"
} catch {
    if (-not $token) {
        Pause-Exit ("Could not reach the GitHub release API, and updater_token.txt is missing.`n" +
            "Check your internet connection and try again. If it keeps failing,`n" +
            "a fine-grained GitHub PAT with Contents: READ-ONLY on $repo`n" +
            "in updater_token.txt next to this script raises the API rate limit.")
    }
    Pause-Exit ("Update check failed: " + $_.Exception.Message)
}

$remoteSha = $null
if ($release.body -match 'sha:\s*([0-9a-f]{40})') { $remoteSha = $matches[1] }
if (-not $remoteSha) {
    Pause-Exit "The release notes carry no commit SHA -- nothing to compare against."
}

$localSha = $null
$versionFile = Join-Path $ModDir "installed_version.txt"
if (Test-Path -LiteralPath $versionFile) {
    $c = (Get-Content -Raw $versionFile).Trim()
    if ($c -match '([0-9a-f]{40})') { $localSha = $matches[1] }
}

Write-Host ("Installed: " + $(if ($localSha) { $localSha.Substring(0, 12) } else { "unknown" }))
Write-Host ("Latest:    " + $remoteSha.Substring(0, 12))
if ($localSha -and ($localSha -eq $remoteSha)) {
    Pause-Exit "Already up to date."
}

# --- download ------------------------------------------------------------
$asset = @($release.assets | Where-Object { $_.name -like "*.zip" })[0]
if (-not $asset) { Pause-Exit "The latest release has no zip asset." }

$zipPath = Join-Path $env:TEMP "multivoid-update.zip"
Write-Host ""
Write-Host ("Downloading " + $asset.name + " ...")
$dlHeaders = $headers.Clone()
$dlHeaders["Accept"] = "application/octet-stream"
# asset.url is the API download URL (needs the token on a private repo).
Invoke-WebRequest -Uri $asset.url -Headers $dlHeaders -OutFile $zipPath `
    -UserAgent "multivoid-updater"
Write-Host "Downloaded."

# --- confirm, then close the game -----------------------------------------
Write-Host ""
$answer = Read-Host "Update ready. Close the game to install it? (Y/N)"
if ($answer -notmatch '^[Yy]') {
    Remove-Item -Force $zipPath
    Pause-Exit "Cancelled -- game left running."
}

Write-Host "Closing the game..."
$proc = $null
if ($GamePid -ne 0) { $proc = Get-Process -Id $GamePid -ErrorAction SilentlyContinue }
if ($proc) {
    $proc.CloseMainWindow() | Out-Null
    if (-not $proc.WaitForExit(30000)) {
        Write-Host "The game did not close in 30 seconds -- forcing it."
        $proc | Stop-Process -Force
        Start-Sleep -Seconds 2
    }
    Write-Host "Game closed."
} else {
    Write-Host "Game process not found (already closed?). Continuing."
}

# --- install ---------------------------------------------------------------
Write-Host "Installing..."
$stageDir = Join-Path $env:TEMP "mv-update-stage"
if (Test-Path -LiteralPath $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory $stageDir | Out-Null
Expand-Archive -Path $zipPath -DestinationPath $stageDir -Force
$modContent = Join-Path $stageDir "mod"
if (-not (Test-Path -LiteralPath $modContent)) {
    Pause-Exit "The zip has no mod/ folder -- aborting. Your game was NOT touched."
}

# The token survives the swap; everything else is replaced.
$tokenBackup = $null
if (Test-Path -LiteralPath $tokenFile) { $tokenBackup = Get-Content -Raw $tokenFile }

$backupDir = "$ModDir.bak"
if (Test-Path -LiteralPath $backupDir) { Remove-Item -Recurse -Force $backupDir }
Rename-Item -Path $ModDir -NewName (Split-Path -Leaf $backupDir)
New-Item -ItemType Directory $ModDir | Out-Null
Copy-Item -Path (Join-Path $modContent "*") -Destination $ModDir -Recurse -Force
if ($tokenBackup) {
    Set-Content -Path $tokenFile -Value $tokenBackup.Trim() -NoNewline
}
Set-Content -Path $versionFile -Value "commit: $remoteSha" -NoNewline
Remove-Item -Force $zipPath
Remove-Item -Recurse -Force $stageDir
Write-Host "Installed."

# --- relaunch ---------------------------------------------------------------
Write-Host "Relaunching the game..."
Start-Process -FilePath $GameExe -WorkingDirectory (Split-Path $GameExe)
Write-Host "Done."
Start-Sleep -Seconds 2
