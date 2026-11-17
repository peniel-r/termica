# termica shell integration (zsh)
#
# Source this from ~/.zshrc. It registers its hooks with add-zsh-hook, so it
# runs alongside a prompt framework (starship, oh-my-posh, pure) rather than
# replacing it:
#
#   eval "$(starship init zsh)"
#   . ~/.termica/shell/termica-zsh.sh
#
# It emits the OSC sequences the status bar needs: OSC 7 (working directory)
# and OSC 133 (prompt / command steps). Without it the bar still renders, but
# the command modules (status, duration, busy) stay hidden.

if [[ "$TERMICA_SHELL_INTEGRATION" != "1" ]]; then
    return 0 2>/dev/null || exit 0
fi

typeset -g __termica_ran=0

__termica_precmd() {
    local code=$?
    local dir=${PWD// /%20}

    if [[ "$__termica_ran" = 1 ]]; then
        printf '\033]133;D;%d\a' "$code"
        __termica_ran=0
    fi
    printf '\033]133;A\a'
    printf '\033]7;file://localhost%s\a' "$dir"
    printf '\033]133;B\a'
}

__termica_preexec() {
    # command submitted (output start, C)
    __termica_ran=1
    printf '\033]133;C\a'
}

autoload -Uz add-zsh-hook
add-zsh-hook precmd __termica_precmd
add-zsh-hook preexec __termica_preexec
