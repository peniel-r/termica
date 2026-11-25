<#
.SYNOPSIS
    Run termica on Windows or inside WSL.

.DESCRIPTION
    Launches the program in the current terminal, so its stdout (renderer
    name, cell size, bold/italic faces) shows up inline - there is no
    separate console window, because the exe inherits this console.

    The settings file decides the font face and size. -Font and -Size are
    one-run overrides: when they are omitted nothing is passed on the command
    line, so `[font] face` and `[font] size` in termica.toml win.

    With -SelfTest the terminal half is checked without opening a window
    (this is the documented libghostty-vt flow, see README).

    -Wsl launches a Linux build through WSLg. It defaults to
    build/native/termica and uses the default WSL distribution. Build the
    Linux binary first using the instructions in README.md.

.PARAMETER Font
    Optional font file or installed family name. Overrides `[font] face`
    for this run.

.PARAMETER Size
    Optional point size. Overrides `[font] size` for this run.

.PARAMETER Preset
    Which build/ subdirectory to run from. Defaults to llvm-mingw on Windows
    and native with -Wsl.

.PARAMETER SelfTest
    Run "termica --selftest" instead of the GUI.

.PARAMETER Wsl
    Run the Linux binary using WSL and WSLg instead of the Windows executable.

.PARAMETER WslDistribution
    Optional WSL distribution name. Defaults to the system's default distro.

.PARAMETER WslBinary
    Optional Linux path to a termica executable, for builds outside build/.
    Cannot be combined with -Preset.

.EXAMPLE
    pwsh -File ./run.ps1
    pwsh -File ./run.ps1 -Font C:\Windows\Fonts\consola.ttf -Size 18
    pwsh -File ./run.ps1 -SelfTest
    pwsh -File ./run.ps1 -Wsl
    pwsh -File ./run.ps1 -Wsl -WslDistribution Ubuntu -SelfTest
    pwsh -File ./run.ps1 -Wsl -WslBinary /home/user/termica/build/native/termica
#>
[CmdletBinding()]
param(
    [string] $Font,
    [string] $Size,
    [string] $Preset = 'llvm-mingw',
    [switch] $SelfTest,
    [switch] $Wsl,
    [string] $WslDistribution,
    [string] $WslBinary
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
Set-Location $project

if (-not $Wsl -and ($PSBoundParameters.ContainsKey('WslDistribution') -or
                    $PSBoundParameters.ContainsKey('WslBinary'))) {
    throw '-WslDistribution and -WslBinary require -Wsl.'
}

if ($Wsl) {
    if ($PSBoundParameters.ContainsKey('WslBinary') -and
        $PSBoundParameters.ContainsKey('Preset')) {
        throw '-WslBinary cannot be combined with -Preset.'
    }
    if ($PSBoundParameters.ContainsKey('WslDistribution') -and -not $WslDistribution) {
        throw '-WslDistribution must not be empty.'
    }
    if ($PSBoundParameters.ContainsKey('WslBinary') -and -not $WslBinary) {
        throw '-WslBinary must not be empty.'
    }
    $wslCommand = Get-Command wsl.exe -ErrorAction Stop
    if (-not $WslBinary) {
        if (-not $PSBoundParameters.ContainsKey('Preset')) { $Preset = 'native' }
        $WslBinary = "./build/$Preset/termica"
    }
    $wslBaseArgs = @('--cd', $project)
    if ($WslDistribution) { $wslBaseArgs += @('--distribution', $WslDistribution) }
    # ProcessStartInfo uses an explicit Windows command line so PowerShell
    # 5.1 preserves literal quotes as well as spaces in Linux arguments.
    function ConvertTo-WindowsArgument([string] $argument) {
        # wsl.exe recognizes switches only when they are not quoted.
        if ($argument -and $argument -notmatch '[\s"]') {
            return $argument
        }
        $result = [System.Text.StringBuilder]::new()
        [void] $result.Append('"')
        $slashes = 0
        foreach ($character in $argument.ToCharArray()) {
            if ($character -eq '\') {
                $slashes++
            } elseif ($character -eq '"') {
                [void] $result.Append(('\' * (2 * $slashes + 1)))
                [void] $result.Append('"')
                $slashes = 0
            } else {
                [void] $result.Append(('\' * $slashes))
                [void] $result.Append($character)
                $slashes = 0
            }
        }
        [void] $result.Append(('\' * (2 * $slashes)))
        [void] $result.Append('"')
        return $result.ToString()
    }
    function Invoke-WslProcess([string[]] $arguments) {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $wslCommand.Source
        $startInfo.Arguments = (($arguments | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' ')
        $startInfo.UseShellExecute = $false
        $startInfo.WorkingDirectory = $project
        $process = [System.Diagnostics.Process]::Start($startInfo)
        try {
            $process.WaitForExit()
            return $process.ExitCode
        } finally {
            $process.Dispose()
        }
    }

    $defaultBinary = -not $PSBoundParameters.ContainsKey('WslBinary')
    if ($defaultBinary -or $WslBinary.Contains('/')) {
        $check = Invoke-WslProcess ($wslBaseArgs + @('--exec', 'test', '-x', $WslBinary))
        if ($check -eq 1) {
            if (-not $defaultBinary) {
                throw "WSL executable '$WslBinary' is missing or not executable in the selected distribution."
            }
            if ($Preset -eq 'native') {
                $hint = 'In WSL, from this checkout, run: cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build build/native.'
            } else {
                $hint = 'Build the selected preset directory inside WSL as described in README.md.'
            }
            throw "WSL executable '$WslBinary' is missing or not executable. $hint Install the Linux prerequisites in README.md, or pass -WslBinary with an existing Linux build."
        }
        if ($check -ne 0) { exit $check }
    }

    # Some WSLg X servers expose an invalid RandR output, which makes SDL's
    # X11 initialization abort. Preserve an explicit user choice and limit
    # the workaround to WSLg; the fixed shell code never interpolates argv.
    $bootstrap = 'if [ -n "${WSL_INTEROP-}" ] && [ -n "${WAYLAND_DISPLAY-}" ] && [ -z "${SDL_VIDEO_X11_XRANDR+x}" ]; then SDL_VIDEO_X11_XRANDR=0; export SDL_VIDEO_X11_XRANDR; fi; exec "$@"'
    $wslArgs = $wslBaseArgs + @('--exec', '/bin/sh', '-c', $bootstrap, 'termica-wsl', $WslBinary)
    if ($SelfTest) {
        $wslArgs += '--selftest'
    } else {
        if ($Font) { $wslArgs += $Font }
        if ($Size) { $wslArgs += $Size }
    }
    exit (Invoke-WslProcess $wslArgs)
}

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
