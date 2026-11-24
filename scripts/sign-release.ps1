<#
.SYNOPSIS
    Sign and verify both release binaries into a new output directory.

.DESCRIPTION
    Uses a preinstalled code-signing certificate and its private-key provider.
    The source build is not modified. The destination appears only after both
    copied binaries have been signed and verified. See docs/signing.md.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $CertificateThumbprint,

    [Parameter(Mandatory = $true)]
    [string] $TimestampUrl,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [string] $InputDirectory = (Join-Path $PSScriptRoot '..\build\llvm-mingw'),

    [ValidateSet('CurrentUser', 'LocalMachine')]
    [string] $StoreLocation = 'CurrentUser',

    [string] $SignToolPath
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
# Import the module belonging to this PowerShell installation rather than an
# incompatible version earlier on PSModulePath.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1')


# A thumbprint is a public certificate identifier, not a private key or PIN.
$thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
if ($thumbprint -notmatch '^[0-9A-F]{40}$') {
    throw 'CertificateThumbprint must be the full 40-digit SHA-1 certificate thumbprint.'
}

# Do not put credentials, tokens, or other secrets in the timestamp URL.
$timestamp = $null
if (-not [Uri]::TryCreate($TimestampUrl, [UriKind]::Absolute, [ref] $timestamp) -or
    $timestamp.Scheme -ne 'https' -or $timestamp.UserInfo -or
    $timestamp.Query -or $timestamp.Fragment) {
    throw 'TimestampUrl must be an HTTPS RFC 3161 endpoint without credentials, query, or fragment.'
}

if (-not $SignToolPath) {
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $versions = @(Get-ChildItem -LiteralPath $sdkBin -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version] $_.Name } -Descending)
    foreach ($version in $versions) {
        # The x86 SDK tool also runs on x64 and ARM64 Windows.
        $candidate = Join-Path $version.FullName 'x86\signtool.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $SignToolPath = $candidate
            break
        }
    }
}
if (-not $SignToolPath -or -not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) {
    throw 'Windows SDK SignTool was not found. Install the SDK or provide SignToolPath.'
}
$SignToolPath = (Get-Item -LiteralPath $SignToolPath).FullName

# Require the exact certificate, a usable private key, an explicit code-signing
# EKU, current validity, and a chain trusted by this machine. Never create or
# import a certificate, export a key, or fall back to another identity.
$store = [System.Security.Cryptography.X509Certificates.X509Store]::new('My', $StoreLocation)
try {
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly -bor
                [System.Security.Cryptography.X509Certificates.OpenFlags]::OpenExistingOnly)
    $matches = @($store.Certificates | Where-Object { $_.Thumbprint -eq $thumbprint })
    if ($matches.Count -ne 1) {
        throw 'Exactly one certificate with the requested thumbprint must exist in the selected My store.'
    }
    $certificate = $matches[0]
    if (-not $certificate.HasPrivateKey) {
        throw 'The selected certificate does not have an accessible private key provider.'
    }
    $now = [DateTime]::UtcNow
    if ($now -lt $certificate.NotBefore.ToUniversalTime() -or
        $now -gt $certificate.NotAfter.ToUniversalTime()) {
        throw 'The selected code-signing certificate is not currently valid.'
    }
    $codeSigningEku = @($certificate.Extensions | Where-Object {
        $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] -and
        @($_.EnhancedKeyUsages | Where-Object { $_.Value -eq '1.3.6.1.5.5.7.3.3' }).Count -gt 0
    })
    if ($codeSigningEku.Count -eq 0) {
        throw 'The selected certificate must explicitly allow code signing.'
    }

    $chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
    try {
        $chain.ChainPolicy.ApplicationPolicy.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
        $chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::Online
        $chain.ChainPolicy.RevocationFlag = [System.Security.Cryptography.X509Certificates.X509RevocationFlag]::ExcludeRoot
        $chain.ChainPolicy.VerificationFlags = [System.Security.Cryptography.X509Certificates.X509VerificationFlags]::NoFlag
        if (-not $chain.Build($certificate)) {
            throw 'The selected code-signing certificate chain is not trusted or revocation status is unavailable.'
        }
    } finally {
        $chain.Dispose()
    }
} finally {
    $store.Close()
}

function Invoke-SignTool([string[]] $ToolArguments, [string] $FailureMessage) {
    # Native output can contain provider diagnostics. Keep it out of logs;
    # the exit code still causes a closed failure, including warnings.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = $null
        & $SignToolPath @ToolArguments > $null 2>&1
        $result = $LASTEXITCODE
    } catch {
        throw $FailureMessage
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($result -ne 0) { throw $FailureMessage }
}

try {
    $inputPath = (Get-Item -LiteralPath $InputDirectory).FullName
    if (-not (Test-Path -LiteralPath $inputPath -PathType Container)) {
        throw 'InputDirectory must be an existing directory.'
    }
    $outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
    $parentPath = [IO.Path]::GetDirectoryName($outputPath)
    if (-not $parentPath -or -not (Test-Path -LiteralPath $parentPath -PathType Container)) {
        throw 'The parent of OutputDirectory must already exist.'
    }
    if (Test-Path -LiteralPath $outputPath) {
        throw 'OutputDirectory must not already exist.'
    }
    foreach ($name in @('termica.exe', 'ghostty-vt.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $inputPath $name) -PathType Leaf)) {
            throw 'InputDirectory must contain both termica.exe and ghostty-vt.dll.'
        }
    }
} catch {
    throw 'Invalid release paths: require both input binaries and a new output directory under an existing parent.'
}

# Stage beside the destination so the final directory move stays on one volume.
$stage = Join-Path $parentPath ('.termica-signing-' + [Guid]::NewGuid().ToString('N'))
$staged = $false
try {
    [void] [IO.Directory]::CreateDirectory($stage)
    $staged = $true
    foreach ($name in @('termica.exe', 'ghostty-vt.dll')) {
        $file = Join-Path $stage $name
        [IO.File]::Copy((Join-Path $inputPath $name), $file)
        $signArgs = @('sign', '/fd', 'SHA256', '/tr', $timestamp.AbsoluteUri,
                      '/td', 'SHA256', '/s', 'My', '/sha1', $thumbprint)
        if ($StoreLocation -eq 'LocalMachine') { $signArgs += '/sm' }
        $signArgs += $file
        Invoke-SignTool $signArgs 'Signing or RFC 3161 timestamping failed.'

        # /tw makes a missing timestamp a warning; any nonzero exit fails.
        Invoke-SignTool @('verify', '/pa', '/all', '/tw', $file) 'Authenticode verification failed.'
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ne $thumbprint -or
            -not $signature.TimeStamperCertificate) {
            throw 'Signature, selected signer, or timestamp verification failed.'
        }
    }
    [IO.Directory]::Move($stage, $outputPath)
    $staged = $false
    Write-Host 'Both release binaries were signed, timestamped, and verified.'
} finally {
    if ($staged) {
        try { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Stop }
        catch { Write-Warning 'Could not remove the incomplete signing staging directory; do not distribute it.' }
    }
}
