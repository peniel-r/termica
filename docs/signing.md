# Optional release signing

Release signing is a separate, optional manual step. Normal builds remain
unsigned. After producing the release build, use `scripts/sign-release.ps1`
to sign **both** `termica.exe` and its distributed `ghostty-vt.dll`. The
script does not create an installer, archive, or package metadata.

## Prerequisites

- Windows 10 or 11, Windows PowerShell 5.1 or PowerShell 7, and Windows SDK
  SignTool. The script discovers the x86 SignTool in the newest versioned
  Windows 10 SDK directory, or accepts an explicit `-SignToolPath`.
- A valid, CA-issued code-signing certificate in the `My` certificate store
  of the current user or local machine, with its private key available through
  an installed key provider. A hardware token can require an interactive PIN
  or approval through its provider. Provision the provider and certificate
  beforehand; the script never generates a certificate, exports a key, or
  accepts a password or PIN. The signing certificate must explicitly include
  the code-signing extended key usage and chain to a root trusted by the
  signing machine. Online revocation checking must succeed.
- The full SHA-1 thumbprint of the intended certificate, selected explicitly
  from Certificate Manager (`certmgr.msc` for CurrentUser, `certlm.msc` for
  LocalMachine). This is a public certificate identifier, not the file
  signing algorithm. Remove any invisible characters when copying it.
- A reachable HTTPS RFC 3161 timestamp service. Supply its public endpoint URL
  without user information, query parameters, or a fragment; do not put
  credentials or tokens in the URL or command line. The selected provider
  must support SHA-256 timestamps.

A signing certificate and timestamp establish a verifiable publisher and
signature; they do **not** guarantee SmartScreen reputation or that first
launch will be unblocked. A trust chain accepted on the signing machine also
does not guarantee trust on every recipient's machine.

## Sign a release

From the repository root, after building with the default release preset:

```powershell
$thumbprint = Read-Host 'Code-signing certificate thumbprint'
$timestampUrl = Read-Host 'HTTPS RFC 3161 timestamp URL'
.\scripts\sign-release.ps1 `
    -CertificateThumbprint $thumbprint `
    -TimestampUrl $timestampUrl `
    -OutputDirectory .\signed-release
```

The input defaults to `build\llvm-mingw`; use `-InputDirectory` to select a
release build elsewhere. `-OutputDirectory` must not already exist, and its
parent must exist. Use `-StoreLocation LocalMachine` when the selected
certificate is in that store. SignTool uses the certificate thumbprint and
store explicitly; there is no automatic certificate selection or self-signed
fallback. No secret is written or printed by the script. Provider prompts are
managed by the installed provider, not the script.

The script copies the two input binaries into a temporary sibling directory,
signs each with SHA-256 and an RFC 3161 SHA-256 timestamp, then verifies each
using the Authenticode policy and checks the signer and timestamp. It moves the
staging directory to the requested output name only after both pass. The input
build is not modified. A failure is an error, not an unsigned-release fallback;
if cleanup warns that a staging directory remains, remove it and do not
distribute its contents. SignTool and provider diagnostics are suppressed to
avoid exposing sensitive information. Check the certificate, provider access,
network, timestamp service, and trust/revocation configuration if it fails.

For an independent check, run the SDK's `signtool verify /pa /all /tw` on
each of `signed-release\termica.exe` and `signed-release\ghostty-vt.dll`, and
inspect their signatures in Explorer. A nonzero SignTool exit, missing
timestamp, unexpected signer, or untrusted signature means the release is not
ready. Repeat verification on the intended Windows 10 and Windows 11 recipient
systems after transferring the exact signed files; changing either file after
signing invalidates its signature.
