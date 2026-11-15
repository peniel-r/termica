<#
.SYNOPSIS
    Run termica.exe from PowerShell 7 / WezTerm.

.DESCRIPTION
    Launches the program in the current terminal, so its stdout (renderer
    name, cell size, Nerd Font check) shows up inline - there is no separate
    console window, because the exe inherits this console.

    The font argument is optional: if none is given, a Nerd Font Mono is
    looked up in this order:
      1. the per-user Windows font folder (*NerdFontMono-*, MapleMono-NF-*)
      2. the Scoop nerd-fonts package (Maple-Mono-NF)
      3. C:\Windows\Fonts\consola.ttf (no icons - the program warns about it)

.PARAMETER Font
    Path to a .ttf. Defaults to auto-detection.

.PARAMETER Size
    Point size. Default 16.

.PARAMETER Preset
    Which build/ subdirectory to run from. Default llvm-mingw.

.EXAMPLE
    pwsh -File ./run.ps1
    pwsh -File ./run.ps1 -Font C:\Windows\Fonts\consola.ttf -Size 18
#>
[CmdletBinding()]
param(
    [string] $Font,
    [string] $Size = '16',
    [string] $Preset = 'llvm-mingw'
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
Set-Location $project

$exe = Join-Path $project "build\$Preset\termica.exe"
if (-not (Test-Path $exe)) {
    throw "$exe not found - run 'pwsh -File ./build.ps1' first."
}

if (-not $Font) {
    $userFonts = "$env:LOCALAPPDATA\Microsoft\Windows\Fonts"
    $scoopFonts = "$env:USERPROFILE\scoop\apps\Maple-Mono-NF\current"

    $candidates = @(
        (Get-ChildItem $userFonts -Filter '*NerdFontMono-Regular.ttf' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName)
        (Get-ChildItem $userFonts -Filter 'MapleMono-NF-Regular.ttf' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName)
        (Get-ChildItem $scoopFonts -Filter 'MapleMono-NF-Regular.ttf' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName)
        (Get-ChildItem 'C:\Windows\Fonts' -Filter '*NerdFontMono-Regular.ttf' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName)
        'C:\Windows\Fonts\consola.ttf'
    ) | Where-Object { $_ -and (Test-Path $_) }

    $Font = $candidates | Select-Object -First 1
}

Write-Host "font: $Font" -ForegroundColor DarkGray
Write-Host "size: $Size" -ForegroundColor DarkGray
Write-Host ""

& $exe $Font $Size
