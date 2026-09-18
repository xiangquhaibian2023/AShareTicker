[CmdletBinding()]
param(
    [string]$Version = (Get-Date -Format "yyyy.MM.dd"),
    [string]$ExecutablePath = "build\ashare_client.exe",
    [string]$CertificateThumbprint = $env:ASHARE_SIGN_CERT_THUMBPRINT,
    [string]$TimestampUrl = $env:ASHARE_TIMESTAMP_URL,
    [switch]$AllowUnsigned
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceExecutable = Join-Path $projectRoot $ExecutablePath
$guidePath = Join-Path $projectRoot "packaging\README_CN.txt"
if (-not (Test-Path -LiteralPath $sourceExecutable -PathType Leaf)) {
    throw "Release executable was not found: $sourceExecutable"
}
if (-not (Test-Path -LiteralPath $guidePath -PathType Leaf)) {
    throw "Release guide was not found: $guidePath"
}

$distRoot = Join-Path $projectRoot "dist"
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null

$signedRelease = -not [string]::IsNullOrWhiteSpace($CertificateThumbprint)
if (-not $signedRelease -and -not $AllowUnsigned) {
    throw "No signing certificate is configured. Set ASHARE_SIGN_CERT_THUMBPRINT, or explicitly use -AllowUnsigned for a test distribution."
}

$suffix = if ($signedRelease) { "" } else { "-unsigned" }
$packageName = "AShareTicker-$Version-win64-portable$suffix"
$packageDirectory = Join-Path $distRoot $packageName
$archivePath = Join-Path $distRoot "$packageName.zip"
if (Test-Path -LiteralPath $packageDirectory) {
    throw "Release directory already exists. Use a new version or archive it first: $packageDirectory"
}
if (Test-Path -LiteralPath $archivePath) {
    throw "Release archive already exists. Use a new version or archive it first: $archivePath"
}

New-Item -ItemType Directory -Path $packageDirectory | Out-Null
$releaseExecutable = Join-Path $packageDirectory "AShareTicker.exe"
Copy-Item -LiteralPath $sourceExecutable -Destination $releaseExecutable
Copy-Item -LiteralPath $guidePath -Destination (Join-Path $packageDirectory "README_CN.txt")

if ($signedRelease) {
    $signArguments = @{
        FilePath = $releaseExecutable
        CertificateThumbprint = $CertificateThumbprint
    }
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        $signArguments.TimestampUrl = $TimestampUrl
    }
    & (Join-Path $projectRoot "sign_release.ps1") @signArguments
}

$signature = Get-AuthenticodeSignature -LiteralPath $releaseExecutable
if ($signedRelease -and $signature.Status -ne "Valid") {
    throw "Release signature validation failed: $($signature.StatusMessage)"
}

$hash = Get-FileHash -LiteralPath $releaseExecutable -Algorithm SHA256
$signatureDescription = if ($signature.Status -eq "Valid") {
    "Valid ($($signature.SignerCertificate.Subject))"
} else {
    "Not signed (no trusted code-signing certificate is configured on this machine)"
}
$manifest = @"
AShareTicker release manifest
Version: $Version
Architecture: Windows x64
Build: Release / Windows GUI
Signature: $signatureDescription
Executable: AShareTicker.exe
SHA-256: $($hash.Hash)
Created: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz")
"@
Set-Content -LiteralPath (Join-Path $packageDirectory "MANIFEST.txt") -Value $manifest -Encoding UTF8
Set-Content -LiteralPath (Join-Path $packageDirectory "SHA256SUMS.txt") `
    -Value "$($hash.Hash)  AShareTicker.exe" -Encoding ASCII

Compress-Archive -LiteralPath $packageDirectory -DestinationPath $archivePath -CompressionLevel Optimal
$archiveHash = Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
Set-Content -LiteralPath "$archivePath.sha256" `
    -Value "$($archiveHash.Hash)  $([System.IO.Path]::GetFileName($archivePath))" -Encoding ASCII

Write-Host "Release directory: $packageDirectory"
Write-Host "Release archive: $archivePath"
Write-Host "SHA-256: $($hash.Hash)"
Write-Host "Archive SHA-256: $($archiveHash.Hash)"
Write-Host "Signature: $signatureDescription"
