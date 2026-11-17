# termica shell integration (PowerShell 5.1+ / 7+)
#
# Dot-source this from your PowerShell profile ($PROFILE) AFTER your prompt
# is set up (for example after `Invoke-Expression (&starship init powershell)`),
# so it can wrap the existing prompt instead of replacing it:
#
#   Invoke-Expression (&starship init powershell)
#   . (Join-Path $PSScriptRoot "termica-pwsh.ps1")
#
# It emits the OSC sequences the status bar needs: OSC 7 (working directory)
# and OSC 133 (prompt / command steps). The directory comes from $PWD, which
# PowerShell keeps current on every `cd`; the process working directory does
# NOT track $PWD, so this integration is what makes the bar follow your
# prompt. Without it the bar still renders, but the directory stays put and
# the command modules (status, duration, busy) stay hidden.

if ($env:TERMICA_SHELL_INTEGRATION -ne '1') { return }

$esc = [char]27
$bell = [char]7
$Global:__termica_ran = $false

# Capture whatever prompt is installed right now (starship, oh-my-posh, the
# default) so we can render it from inside our wrapper.
$__termica_prev = Get-Item -ErrorAction SilentlyContinue function:prompt
if ($__termica_prev) {
    $Global:__termica_prev_prompt = [scriptblock]::Create($__termica_prev.Definition)
} else {
    $Global:__termica_prev_prompt = $null
}

function global:prompt {
    $out = ''

    # The previous command finished: report its exit code (D;code).
    if ($Global:__termica_ran) {
        $code = 0
        if ($null -ne $global:LASTEXITCODE) { $code = $global:LASTEXITCODE }
        elseif (-not $?) { $code = 1 }
        $out += "$esc]133;D;$code$bell"
        $Global:__termica_ran = $false
    }

    # Prompt start (A), the working directory (7), then the user's prompt,
    # then input start (B).
    $out += "$esc]133;A$bell"
    $uri = 'file://' + ($PWD.Path -replace '\\', '/')
    $out += "$esc]7;$uri$bell"

    if ($Global:__termica_prev_prompt) {
        $out += (& $Global:__termica_prev_prompt)
    } else {
        $out += ('PS ' + $PWD.Path + '> ')
    }

    $out += "$esc]133;B$bell"
    $out
}

# Command submitted (output start, C): emitted before the line is accepted,
# so it lands ahead of the command's own output.
if (Get-Command Set-PSReadLineKeyHandler -ErrorAction SilentlyContinue) {
    Set-PSReadLineKeyHandler -Key Enter -ScriptBlock {
        [Console]::Write("$esc]133;C$bell")
        $Global:__termica_ran = $true
        [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
    }
}
