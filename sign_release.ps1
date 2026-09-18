[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,

    [string]$CertificateThumbprint = $env:ASHARE_SIGN_CERT_THUMBPRINT,

    [ValidateSet("CurrentUser", "LocalMachine")]
    [string]$CertificateStoreLocation = "CurrentUser",

    [string]$TimestampUrl = $env:ASHARE_TIMESTAMP_URL,

    [string]$SignToolPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedFile = (Resolve-Path -LiteralPath $FilePath).Path
if ([string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    throw "No code-signing certificate was specified. Set ASHARE_SIGN_CERT_THUMBPRINT or pass -CertificateThumbprint."
}
$thumbprint = ($CertificateThumbprint -replace "\s", "").ToUpperInvariant()

$certificatePath = "Cert:\$CertificateStoreLocation\My\$thumbprint"
if (-not (Test-Path -LiteralPath $certificatePath)) {
    throw "Certificate $thumbprint was not found in $CertificateStoreLocation\My."
}

$certificate = Get-Item -LiteralPath $certificatePath
if (-not $certificate.HasPrivateKey) {
    throw "Certificate $thumbprint has no accessible private key."
}

$codeSigningOid = "1.3.6.1.5.5.7.3.3"
if ($certificate.EnhancedKeyUsageList.ObjectId -notcontains $codeSigningOid) {
    throw "Certificate $thumbprint is not valid for code signing."
}

if ([string]::IsNullOrWhiteSpace($SignToolPath)) {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        $SignToolPath = Get-ChildItem -Path (Join-Path $kitsRoot "*\x64\signtool.exe") -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
}

if ([string]::IsNullOrWhiteSpace($SignToolPath) -or -not (Test-Path -LiteralPath $SignToolPath)) {
    throw "signtool.exe was not found. Install the Windows SDK or pass -SignToolPath."
}

$signArguments = @("sign", "/fd", "SHA256", "/sha1", $thumbprint, "/s", "My")
if ($CertificateStoreLocation -eq "LocalMachine") {
    $signArguments += "/sm"
}
if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
    $signArguments += @("/tr", $TimestampUrl, "/td", "SHA256")
}
$signArguments += $resolvedFile

& $SignToolPath @signArguments
if ($LASTEXITCODE -ne 0) {
    throw "signtool failed with exit code $LASTEXITCODE."
}

& $SignToolPath verify /pa /v $resolvedFile
if ($LASTEXITCODE -ne 0) {
    throw "Authenticode verification failed with exit code $LASTEXITCODE."
}

$signature = Get-AuthenticodeSignature -LiteralPath $resolvedFile
if ($signature.Status -ne "Valid") {
    throw "PowerShell verification returned status $($signature.Status): $($signature.StatusMessage)"
}

Write-Host "Signed and verified: $resolvedFile"
Write-Host "Signer: $($signature.SignerCertificate.Subject)"
