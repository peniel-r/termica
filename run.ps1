<#
.SYNOPSIS
    Run termica.exe from PowerShell 7 / WezTerm.

.DESCRIPTION
    Launches the program in the current terminal, so its stdout (renderer
    name, cell size, bold/italic faces) shows up inline - there is no
    separate console window, because the exe inherits this console.

    The settings file decides the font face and size. -Font and -Size are
    one-run overrides: when they are omitted nothing is passed on the command
    line, so `[font] face` and `[font] size` in termica.toml win.

    With -SelfTest the terminal half is checked without opening a window
    (this is the documented libghostty-vt flow, see README).

.PARAMETER Font
    Optional font file or installed family name. Overrides `[font] face`
    for this run.

.PARAMETER Size
    Optional point size. Overrides `[font] size` for this run.

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
    [string] $Size,
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

# termica.exe is a GUI-subsystem executable, so PowerShell returns from it
# immediately. Start it without a new window (it keeps this console for the
# startup diagnostics) and wait for it here, so this script still behaves
# like before: it returns when the window closes, with the app's exit code.
function Start-Termica([string[]] $app_args) {
    if ($app_args -and $app_args.Count -gt 0) {
        $process = Start-Process -FilePath $exe -ArgumentList $app_args `
                                 -NoNewWindow -PassThru
    } else {
        $process = Start-Process -FilePath $exe -NoNewWindow -PassThru
    }
    $process.WaitForExit()
    return $process.ExitCode
}

if ($SelfTest) {
    exit (Start-Termica @('--selftest'))
}

if ($Font) {
    Write-Host "font: $Font (override)" -ForegroundColor DarkGray
} else {
    Write-Host "font: from termica.toml (or built in)" -ForegroundColor DarkGray
}
if ($Size) {
    Write-Host "size: $Size (override)" -ForegroundColor DarkGray
} else {
    Write-Host "size: from termica.toml" -ForegroundColor DarkGray
}
Write-Host ""

$appArgs = @()
if ($Font) { $appArgs += $Font }
if ($Size) { $appArgs += $Size }
exit (Start-Termica -app_args $appArgs)
