param(
    [switch]$SkipBuild,
    [string]$ReleaseDirectory = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $projectRoot "build-win\Release"
$distDir = Join-Path $projectRoot "dist"
if ($ReleaseDirectory) { $releaseDir = [IO.Path]::GetFullPath($ReleaseDirectory) }
if ($OutputDirectory) { $distDir = [IO.Path]::GetFullPath($OutputDirectory) }
$stagingDir = Join-Path $distDir "ApexSenseBridge-Portable"
$zipPath = Join-Path $distDir "ApexSenseBridge-Portable.zip"

function Fail([string]$Message) {
    throw "ApexSenseBridge portable package: $Message"
}

function Copy-RequiredFile([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        Fail "required file is missing: $Source"
    }
    $destinationDirectory = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destinationDirectory)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

if (-not $SkipBuild) {
    Write-Host "Building native portable payload..."
    & (Join-Path $PSScriptRoot "build-windows.ps1")
    if ($LASTEXITCODE -ne 0) { Fail "native build failed" }

    Write-Host "Building portable Tray application..."
    & (Join-Path $PSScriptRoot "build-tray-app.ps1")
    if ($LASTEXITCODE -ne 0) { Fail "Tray app build failed" }
}

New-Item -ItemType Directory -Path $distDir -Force | Out-Null

$distFull = [System.IO.Path]::GetFullPath($distDir).TrimEnd('\')
$stagingFull = [System.IO.Path]::GetFullPath($stagingDir).TrimEnd('\')
if (-not $stagingFull.StartsWith($distFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    Fail "refusing to replace unexpected staging path: $stagingFull"
}
if (Test-Path -LiteralPath $stagingFull) {
    Remove-Item -LiteralPath $stagingFull -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingFull -Force | Out-Null

foreach ($name in @(
    "ApexSenseBridge.exe",
    "ApexSenseBridgeCapture.exe",
    "ApexSenseBridgeControl.exe",
    "ApexSenseBridgeIsolationProbe.exe",
    "ApexSenseBridgeTray.exe",
    "ApexSenseBridgeTray.exe.config",
    "libVIIPER.dll",
    "viiper.exe"
)) {
    Copy-RequiredFile (Join-Path $releaseDir $name) (Join-Path $stagingFull $name)
}

$abiVerifier = Join-Path $releaseDir 'ApexSenseBridgeCaptureAbiTests.exe'
if (-not (Test-Path -LiteralPath $abiVerifier)) { Fail 'Build the native ABI tests before packaging.' }
& $abiVerifier (Join-Path $stagingFull 'libVIIPER.dll')
if ($LASTEXITCODE -ne 0) { Fail 'Apex6 requires a matching raw-capable asb9-or-later DLL; ABI verification failed.' }

Copy-RequiredFile (Join-Path $projectRoot "data\supported_games.json") `
    (Join-Path $stagingFull "Data\supported_games.json")
Copy-RequiredFile (Join-Path $projectRoot "assets\app.ico") `
    (Join-Path $stagingFull "Resources\app.ico")

foreach ($name in @("VIIPER-LICENSE.txt", "VIIPER-SOURCE.txt", "LIBVIIPER-SOURCE.txt",
                    "VIIPER-v0.7.0-asb.patch")) {
    Copy-RequiredFile (Join-Path $releaseDir $name) (Join-Path $stagingFull "Licenses\$name")
}
$libraryRecord = Get-Content -LiteralPath (Join-Path $stagingFull 'Licenses\LIBVIIPER-SOURCE.txt') -Raw
if ($libraryRecord -notmatch 'Integrated library version: v0\.7\.0-asb([0-9]+)' -or [int]$Matches[1] -lt 9) { Fail 'Apex6 requires libVIIPER asb9 or later.' }
$actualLibraryHash = (Get-FileHash -LiteralPath (Join-Path $stagingFull 'libVIIPER.dll') -Algorithm SHA256).Hash
if ($libraryRecord -notmatch 'Artifact SHA-256: ([0-9a-fA-F]{64})' -or $Matches[1] -ne $actualLibraryHash) { Fail 'libVIIPER build record hash does not match the packaged DLL.' }
Copy-RequiredFile (Join-Path $projectRoot "LICENSE") `
    (Join-Path $stagingFull "Licenses\ApexSenseBridge-LICENSE.txt")
Copy-RequiredFile (Join-Path $projectRoot "THIRD_PARTY_NOTICES.md") `
    (Join-Path $stagingFull "Licenses\THIRD_PARTY_NOTICES.md")
Copy-RequiredFile (Join-Path $projectRoot "docs\APEX6_CAPTURE.md") `
    (Join-Path $stagingFull "APEX6_CAPTURE.md")
Copy-RequiredFile (Join-Path $projectRoot "docs\APEX6_INTEGRATED_BETA.md") `
    (Join-Path $stagingFull "APEX6_INTEGRATED_BETA.md")
Copy-RequiredFile (Join-Path $projectRoot "installer\driver-manifest.json") `
    (Join-Path $stagingFull "Licenses\driver-manifest.json")

foreach ($name in @(
    "USBip-0.9.8.0-x64.exe",
    "HidHide_1.5.230_x64.exe",
    "USBIP-WIN2-LICENSE.txt",
    "HIDHIDE-LICENSE.txt"
)) {
    Copy-RequiredFile (Join-Path $projectRoot "third_party\prerequisites\$name") `
        (Join-Path $stagingFull "Drivers\$name")
}

foreach ($name in @(
    "Install-Drivers.cmd",
    "Install-Drivers.ps1",
    "Start-ApexSenseBridge.cmd",
    "README-PORTABLE.txt"
)) {
    Copy-RequiredFile (Join-Path $projectRoot "portable\$name") (Join-Path $stagingFull $name)
}

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -LiteralPath $stagingFull -DestinationPath $zipPath -CompressionLevel Optimal

if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    Fail "ZIP creation succeeded without producing $zipPath"
}

Write-Host ""
Write-Host "Portable package ready:" -ForegroundColor Green
Write-Host "  $zipPath"
Write-Host "  SHA-256 $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
