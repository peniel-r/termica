<#
.SYNOPSIS
    Run termica.exe from PowerShell 7 / WezTerm.

.DESCRIPTION
    Launches the program in the current terminal, so its stdout (renderer
    name, cell size, bold/italic faces) shows up inline - there is no
    separate console window, because the exe inherits this console.

    No font is needed: the exe carries Maple Mono NF and uses it
    unless -Font names a different .ttf.

    With -SelfTest the terminal half is checked without opening a window
    (this is the documented libghostty-vt flow, see README).

.PARAMETER Font
    Optional path to a .ttf. The built-in Nerd Font is the default.

.PARAMETER Size
    Point size. Default 16.

.PARAMETER Preset
    Which build/ subdirectory to run from. Default llvm-mingw.

.PARAMETER SelfTest
    Run "termica --selftest" instead of the GUI.

.EXAMPLE
    pwsh -File ./run.ps1
    pwsh -File ./run.ps1 -Font C:\Windows\Fonts\consola.ttf -Size 18
    pwsh -File ./run.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string] $Font,
    [string] $Size = '16',
    [string] $Preset = 'llvm-mingw',
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
Set-Location $project

$exe = Join-Path $project "build\$Preset\termica.exe"
if (-not (Test-Path $exe)) {
    throw "$exe not found - run 'pwsh -File ./build.ps1' first."
}

if ($SelfTest) {
    & $exe --selftest
    exit $LASTEXITCODE
}

if ($Font) {
    Write-Host "font: $Font (override)" -ForegroundColor DarkGray
} else {
    Write-Host "font: Maple Mono NF (built in)" -ForegroundColor DarkGray
}
Write-Host "size: $Size" -ForegroundColor DarkGray
Write-Host ""

if ($Font) { & $exe $Font $Size } else { & $exe $Size }
