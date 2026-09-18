@echo off
setlocal

call "%~dp0build_msvc.bat"
if errorlevel 1 exit /b 1

if "%ASHARE_SIGN_CERT_THUMBPRINT%"=="" (
    echo Release build stopped: ASHARE_SIGN_CERT_THUMBPRINT is not set.
    echo Smart App Control blocks unsigned custom executables on this machine.
    exit /b 2
)

powershell.exe -NoProfile -File "%~dp0sign_release.ps1" ^
    -FilePath "%~dp0build\ashare_client.exe" ^
    -CertificateThumbprint "%ASHARE_SIGN_CERT_THUMBPRINT%"

if errorlevel 1 (
    echo Release signing failed.
    exit /b 1
)

echo Release ready: build\ashare_client.exe
