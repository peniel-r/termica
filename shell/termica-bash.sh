# termica shell integration (bash 4.4+)
#
# Source this from ~/.bashrc AFTER your prompt is set up (for example after
# starship or oh-my-posh init), so it wraps the existing PROMPT_COMMAND
# instead of replacing it:
#
#   eval "$(starship init bash)"
#   . ~/.termica/shell/termica-bash.sh
#
# It emits the OSC sequences the status bar needs: OSC 7 (working directory)
# and OSC 133 (prompt / command steps). Without it the bar still renders, but
# the command modules (status, duration, busy) stay hidden.

[ "$TERMICA_SHELL_INTEGRATION" = "1" ] || return 0 2>/dev/null || exit 0

__termica_ran=0
__termica_old_prompt_command="$PROMPT_COMMAND"

# PS0 runs just before each command: mark that one ran and report output
# start (C) ahead of its output.
PS0='__termica_ran=1; printf "\033]133;C\007"'

__termica_prompt() {
    local code=$?
    local dir=${PWD// /%20}

    if [ "$__termica_ran" = 1 ]; then
        printf '\033]133;D;%d\007' "$code"
        __termica_ran=0
    fi
    printf '\033]133;A\007'
    printf '\033]7;file://localhost%s\007' "$dir"
    printf '\033]133;B\007'

    # Let the previously installed PROMPT_COMMAND (e.g. starship) run too.
    if [ -n "$__termica_old_prompt_command" ]; then
        eval "$__termica_old_prompt_command"
    fi
}

PROMPT_COMMAND=__termica_prompt
