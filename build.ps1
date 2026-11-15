<#
.SYNOPSIS
    Build termica from PowerShell 7 (WezTerm) with Scoop-installed tools.

.DESCRIPTION
    Toolchain: llvm-mingw (clang + lld + MinGW-w64 headers, UCRT) installed
    through Scoop or winget, driven by CMake + Ninja. No MSYS2, no mintty,
    no POSIX layer, no compatibility shim - everything is a native Win32
    executable.

    The compiler is found by name (x86_64-w64-mingw32-clang), which resolves
    through PATH to the llvm-mingw package. If PATH is stale - e.g. the
    package was installed after this terminal was opened - the persisted
    machine/user PATH is merged in first.

.EXAMPLE
    pwsh -File ./build.ps1
    pwsh -File ./build.ps1 -Preset llvm-mingw-debug
#>
[CmdletBinding()]
param(
    [string] $Preset = 'llvm-mingw'
)

$ErrorActionPreference = 'Stop'

# PowerShell 7.4+ turns native-command stderr into error records. Third-party
# build noise (HarfBuzz warnings, for example) would then abort the script.
$PSNativeCommandUseErrorActionPreference = $false

$project = $PSScriptRoot
Set-Location $project

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# Scoop and winget append to the *user* PATH. A terminal that was started
# before the install therefore cannot see the new tools, so merge the
# persisted machine and user PATH into this session (existing entries keep
# their priority).
foreach ($scope in 'Machine', 'User') {
    $persisted = [Environment]::GetEnvironmentVariable('Path', $scope)
    if (-not $persisted) { continue }
    $known = $env:PATH -split ';' | Where-Object { $_ }
    foreach ($entry in ($persisted -split ';' | Where-Object { $_ })) {
        if ($known -notcontains $entry) {
            $env:PATH = "$env:PATH;$entry"
            $known += $entry
        }
    }
}

Write-Step "Tooling"
$missing = @()
foreach ($tool in 'cmake', 'ninja', 'git') {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Host ("  {0,-6} {1}" -f $tool, (& $tool --version 2>&1 | Select-Object -First 1))
    } else {
        $missing += $tool
    }
}
if ($missing.Count) {
    throw ("Missing from PATH: {0}.  Install with:  scoop install {1}" -f ($missing -join ', '), ($missing -join ' '))
}

$compiler = Get-Command x86_64-w64-mingw32-clang -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw ("llvm-mingw not found on PATH.  Install with:  scoop install mingw-mstorsjo-llvm-ucrt" +
           "  (or: winget install LLVM.LLVM / grab llvm-mingw from github.com/mstorsjo/llvm-mingw)")
}
Write-Host ("  clang  {0}" -f (& $compiler.Source --version | Select-Object -First 1))
Write-Host ("  clang  {0}" -f ('target: ' + (& $compiler.Source -dumpmachine)))

Write-Step "Configure ($Preset)"
& cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { throw "cmake --preset $Preset failed ($LASTEXITCODE)" }

Write-Step "Build ($Preset)"
& cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { throw "cmake --build --preset $Preset failed ($LASTEXITCODE)" }

$exe = Join-Path $project "build\$Preset\termica.exe"
Write-Step "Done"
if (Test-Path $exe) {
    $size = [math]::Round((Get-Item $exe).Length / 1MB, 2)
    Write-Host "  $exe  ($size MB)" -ForegroundColor Green
    Write-Host "  run it with:  pwsh -File ./run.ps1 -Preset $Preset"
} else {
    Write-Warning "termica.exe not found in build\$Preset"
}
