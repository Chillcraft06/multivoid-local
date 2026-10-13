# package-profile.ps1 -- assemble the one-click mod-manager profile zip.
#
# The profile is the unit install-profile.ps1 (dist/install-profile.ps1)
# downloads: a COMPLETE manager profile folder that a friend extracts into
# their mod manager's profiles/ directory, no hand-mirroring. Its shape was
# mapped from the real profile (see dist/mod-manager-profile.md).
#
# TWO INPUTS, NEITHER HAND-MADE:
#   -StageDir     the package.ps1 stage tree (invoke package.ps1 with -KeepStage),
#                i.e. our mod: manifest.json, icon.png, README.md, CHANGELOG.md,
#                LICENSE, THIRD-PARTY-NOTICES.md, mod/dlls/main.dll,
#                mod/enabled.txt, mod/updater.ps1, pak/*
#   -SkeletonDir dist/profile-skeleton -- the official unreal_shimloader
#                package contents (UE4SS DLLs, Lua helper mods, enable lists),
#                checked in once; our mod's declared dependency. NOT the mod.
#
# THE ZIP IS VERIFIED AFTER IT IS WRITTEN, not merely assembled (the package.ps1
# 7.4b rule applies here too: an empty or mis-rooted zip is a silently broken
# release). Test-ProfileZip fails CLOSED on a missing entry or a main.dll whose
# bytes do not match the payload that was staged.
#
# mods.yml is GENERATED, never hand-edited: the chilltivoid entry is stamped
# with the manifest's version_number, the unreal_shimloader entry with the
# skeleton's shimloader-version.txt, both with installedAtTime = now.
#
# SCOPE: this lane serves the rolling dev-latest prerelease (the in-game
# updater keeps reading the plain mod zip; the profile zip is the
# send-to-a-friend channel). No release signature is attached here; the
# versioned release lane assembles its own assets in publish.ps1.

param(
    [string]$StageDir    = '',   # required: package.ps1 stage tree (with -KeepStage)
    [string]$SkeletonDir = '',   # default: <repo>/dist/profile-skeleton
    [string]$OutDir      = '',   # required: where Multivoid-r2modman-profile.zip goes
    [string]$ProfileName = 'Multivoid'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $StageDir)    { throw '-StageDir is required (run package.ps1 with -KeepStage first)' }
if (-not $OutDir)     { throw '-OutDir is required' }
if (-not $SkeletonDir) { $SkeletonDir = Join-Path $repoRoot 'dist/profile-skeleton' }

# --- Validate the mod stage -------------------------------------------------
$manifestPath = Join-Path $StageDir 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "stage has no manifest.json: $StageDir" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$version = [string]$manifest.version_number
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "manifest version_number is not major.minor.patch: '$version'" }
$vMajor, $vMinor, $vPatch = $Matches[1], $Matches[2], $Matches[3]

$payloadDll = Join-Path $StageDir 'mod/dlls/main.dll'
if (-not (Test-Path -LiteralPath $payloadDll)) { throw "stage has no mod/dlls/main.dll" }
$payloadSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadDll).Hash.ToLowerInvariant()
foreach ($f in 'mod/enabled.txt', 'mod/updater.ps1', 'icon.png', 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
    if (-not (Test-Path -LiteralPath (Join-Path $StageDir $f))) { throw "stage is missing $f" }
}
$stagedPaks = @(Get-ChildItem (Join-Path $StageDir 'pak') -File -ErrorAction SilentlyContinue)

# --- Validate the skeleton ---------------------------------------------------
foreach ($f in 'dwmapi.dll', 'ue4ss.dll', 'UE4SS-settings.ini',
            'shimloader/mod/mods.json', 'shimloader/mod/mods.txt',
            'shimloader-version.txt', '_state/installation_state.yml') {
    if (-not (Test-Path -LiteralPath (Join-Path $SkeletonDir $f))) { throw "skeleton is missing $f" }
}
$shimVersion = ((Get-Content -LiteralPath (Join-Path $SkeletonDir 'shimloader-version.txt') -Raw).Trim())
if ($shimVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "shimloader-version.txt is not major.minor.patch: '$shimVersion'" }
$sMajor, $sMinor, $sPatch = $Matches[1], $Matches[2], $Matches[3]
# The skeleton must not smuggle a mod of its own: Chilltivoid comes from the stage.
foreach ($f in 'shimloader/mod/Chilltivoid', 'shimloader/pak', 'mods.yml') {
    if (Test-Path -LiteralPath (Join-Path $SkeletonDir $f)) { throw "skeleton must not contain $f (generated per release)" }
}

# --- Assemble the profile tree ----------------------------------------------
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repoRoot $OutDir }
$profRoot = Join-Path $outAbs 'profile-stage'
if (Test-Path $profRoot) { Remove-Item $profRoot -Recurse -Force }
$prof = Join-Path $profRoot $ProfileName
New-Item -ItemType Directory -Path $prof -Force | Out-Null

# Skeleton first (everything except its repo bookkeeping files).
$exclude = @('README.md', 'shimloader-version.txt')
foreach ($item in Get-ChildItem -LiteralPath $SkeletonDir -Force) {
    if ($item.Name -in $exclude) { continue }
    Copy-Item -LiteralPath $item.FullName -Destination $prof -Recurse -Force
}
# The cfg dir ships empty (git cannot track it); the copy above brings the .gitkeep,
# which is repo bookkeeping, not profile content.
$gitkeep = Join-Path $prof 'shimloader/cfg/.gitkeep'
if (Test-Path -LiteralPath $gitkeep) { Remove-Item -LiteralPath $gitkeep -Force }

# Our mod: stage tree relocated under shimloader/mod/Chilltivoid/.
$modDir = Join-Path $prof 'shimloader/mod/Chilltivoid'
New-Item -ItemType Directory -Path (Join-Path $modDir 'dlls') -Force | Out-Null
foreach ($f in 'manifest.json', 'icon.png', 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
    Copy-Item -LiteralPath (Join-Path $StageDir $f) -Destination $modDir -Force
}
if (Test-Path -LiteralPath (Join-Path $StageDir 'CHANGELOG.md')) {
    Copy-Item -LiteralPath (Join-Path $StageDir 'CHANGELOG.md') -Destination $modDir -Force
}
Copy-Item -LiteralPath $payloadDll -Destination (Join-Path $modDir 'dlls/main.dll') -Force
Copy-Item -LiteralPath (Join-Path $StageDir 'mod/enabled.txt') -Destination (Join-Path $modDir 'enabled.txt') -Force
Copy-Item -LiteralPath (Join-Path $StageDir 'mod/updater.ps1') -Destination (Join-Path $modDir 'updater.ps1') -Force

# Skins: stage pak/* under shimloader/pak/Chilltivoid/.
if ($stagedPaks.Count -gt 0) {
    $pakDir = Join-Path $prof 'shimloader/pak/Chilltivoid'
    New-Item -ItemType Directory -Path $pakDir -Force | Out-Null
    foreach ($p in $stagedPaks) { Copy-Item -LiteralPath $p.FullName -Destination $pakDir -Force }
    Write-Host "pak: staged $($stagedPaks.Count) file(s)"
}

# mods.yml: the manager's package registry, generated per release.
$installedAt = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$modsYml = @"
- manifestVersion: 1
  name: Thunderstore-chilltivoid
  authorName: Chillcraft
  websiteUrl: https://thunderstore.io/c/voices-of-the-void/p/Pelmentor/Multivoid/
  displayName: chilltivoid
  description: chill's custom implementation of multivoid
  gameVersion: "0"
  networkMode: both
  packageType: other
  installMode: managed
  installedAtTime: $installedAt
  loaders: []
  dependencies: []
  incompatibilities: []
  optionalDependencies: []
  versionNumber:
    major: $vMajor
    minor: $vMinor
    patch: $vPatch
  enabled: true
  onlineSource: false
  trustedPackage: false
- manifestVersion: 1
  name: Thunderstore-unreal_shimloader
  authorName: Thunderstore
  websiteUrl: https://thunderstore.io/c/voices-of-the-void/p/Thunderstore/unreal_shimloader/
  displayName: unreal_shimloader
  description: Thunderstore Mod Manager and r2modmanPlus support for RE-UE4SS.
  gameVersion: "0"
  networkMode: both
  packageType: other
  installMode: managed
  installedAtTime: $installedAt
  loaders: []
  dependencies: []
  incompatibilities: []
  optionalDependencies: []
  versionNumber:
    major: $sMajor
    minor: $sMinor
    patch: $sPatch
  enabled: true
  onlineSource: true
  trustedPackage: false
"@
Set-Content -LiteralPath (Join-Path $prof 'mods.yml') -Value $modsYml -Encoding utf8 -NoNewline
Write-Host "mods.yml: chilltivoid $version (local), unreal_shimloader $shimVersion (online)"

# --- Zip ----------------------------------------------------------------------
$zipName = 'Multivoid-r2modman-profile.zip'
$zipPath = Join-Path $outAbs $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $profRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal

# --- Verify the WRITTEN zip, not the staged tree ------------------------------
$violations = @()
$zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $entries = @{}
    foreach ($e in $zip.Entries) { $entries[$e.FullName] = $e }
    $need = @(
        "$ProfileName/dwmapi.dll", "$ProfileName/ue4ss.dll", "$ProfileName/UE4SS-settings.ini",
        "$ProfileName/mods.yml", "$ProfileName/_state/installation_state.yml",
        "$ProfileName/shimloader/mod/mods.json", "$ProfileName/shimloader/mod/mods.txt",
        "$ProfileName/shimloader/mod/Chilltivoid/manifest.json",
        "$ProfileName/shimloader/mod/Chilltivoid/dlls/main.dll",
        "$ProfileName/shimloader/mod/Chilltivoid/enabled.txt",
        "$ProfileName/shimloader/mod/Chilltivoid/updater.ps1"
    )
    foreach ($n in $need) {
        if (-not $entries.ContainsKey($n)) { $violations += "missing entry: $n" }
    }
    # No stray nesting: exactly one top-level folder, the profile name.
    $tops = @($entries.Keys | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique)
    if ($tops.Count -ne 1 -or $tops[0] -ne $ProfileName) {
        $violations += "zip root is not exactly one '$ProfileName' folder (saw: $($tops -join ', '))"
    }
    # The payload bytes in the zip must be the staged payload, not a truncated twin.
    $dllKey = "$ProfileName/shimloader/mod/Chilltivoid/dlls/main.dll"
    if ($entries.ContainsKey($dllKey)) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $s = $entries[$dllKey].Open()
            try {
                $hash = $sha.ComputeHash($s)
            } finally { $s.Dispose() }
            $zipDllSha = (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
            if ($zipDllSha -ne $payloadSha) { $violations += 'main.dll in zip does not match the staged payload' }
        } finally { $sha.Dispose() }
    }
    # Every staged pak must have ridden along.
    foreach ($p in $stagedPaks) {
        $k = "$ProfileName/shimloader/pak/Chilltivoid/$($p.Name)"
        if (-not $entries.ContainsKey($k)) { $violations += "missing staged pak: $k" }
    }
} finally { $zip.Dispose() }
if ($violations.Count -gt 0) {
    throw ("profile zip FAILED its own tree check:`n  " + ($violations -join "`n  "))
}

Remove-Item $profRoot -Recurse -Force
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
Write-Host ''
Write-Host "PROFILE OK  $zipName" -ForegroundColor Green
Write-Host "  path   : $zipPath"
Write-Host "  sha256 : $sha"
Write-Host "  mod    : chilltivoid $version (payload sha256 $payloadSha)"
Write-Host "  loader : unreal_shimloader $shimVersion (skeleton)"
