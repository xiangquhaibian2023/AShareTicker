# Windows application-control build

This machine has Smart App Control enabled (`VerifiedAndReputablePolicyState=1`).
The generated client is custom software and must carry a trusted Authenticode
signature before Windows will allow it to run.

## Requirements

- Visual Studio Build Tools with the C++ workload
- Windows SDK (`signtool.exe`)
- A trusted code-signing certificate with its private key

The certificate must be trusted by the active Windows application-control
policy. A locally created self-signed certificate does not satisfy that
requirement.

## Signed release

Run from an x64 Native Tools Command Prompt:

```bat
cd /d D:\Code\AShareTicker
set ASHARE_SIGN_CERT_THUMBPRINT=YOUR_CERTIFICATE_THUMBPRINT
set ASHARE_TIMESTAMP_URL=YOUR_CA_TIMESTAMP_URL
build_release.bat
```

`build_release.bat` compiles the client, signs it with SHA-256, and verifies the
final Authenticode signature. It stops with a non-zero exit code when the
certificate, signing tool, or signature is invalid.
