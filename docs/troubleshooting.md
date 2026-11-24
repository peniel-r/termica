# Troubleshooting

termica requires Windows 10 version 1809 or later; Mica and Acrylic require
Windows 11 22H2 or later. See the [configuration and key reference](configuration.md)
for supported settings, defaults and key names.

## Startup and shells

- If the executable cannot find `ghostty-vt.dll`, keep the DLL from the same
  build next to `termica.exe`.
- If a shell does not start, read the message in the terminal. Check that the
  configured executable is installed and on `PATH`, and that `[terminal] cwd`
  exists. `command` is an array of executable and individual arguments, not a
  shell command line; it takes precedence over `shell`. An empty command uses
  the selected shell. For example:

  ```toml
  [terminal]
  command = ["wsl.exe", "-d", "Ubuntu"]
  cwd = 'C:\work'
  ```

- To isolate configuration problems, close termica, make a backup of
  `%APPDATA%\termica\termica.toml` and `statusbar.toml`, then temporarily
  rename them and restart. Missing files use defaults. Restore the backups
  after diagnosis; do not delete settings you need.

## Configuration and appearance

- Check the file is in `%APPDATA%\termica\` and is valid TOML. Use a literal
  string such as `'C:\Fonts\font.ttf'` or escape each backslash in a double
  quoted string. Wrong types and out-of-range values are not accepted. During
  live reload, a syntax error or invalid recognized value retains the previous
  configuration and shows a message; unknown keys are ignored. Initial loading
  ignores invalid individual values instead.
- A command-line font or size override takes precedence for that run. If an
  installed font family or fallback does not appear, check its spelling and
  that it is installed for the current user or system. The empty font face
  selects the embedded font.
- Backdrops fall back to the drawn panel on unsupported Windows versions. If
  the window unexpectedly appears opaque, try launching from PowerShell with
  `$env:SDL_RENDER_DRIVER = 'direct3d11'`. Remove the variable afterwards with
  `Remove-Item Env:SDL_RENDER_DRIVER`.
- The status bar's directory and command information needs shell integration.
  Source the matching script in [`shell/`](../shell/) after prompt
  initialization; see [shell integration](../README.md#shell-integration).
  Git modules are empty outside a repository. The command duration may be an
  approximation when the shell omits the command-start marker.

## Input, clipboard, and notifications

- `Ctrl+V` is sent to the program by default; paste with `Ctrl+Shift+V` or
  configure a binding. Key bindings use physical scancodes and exact modifiers.
  Search has its own navigation shortcuts, listed in the reference.
- Applications can capture the mouse; hold Shift while selecting or scrolling
  to take the pointer back. OSC 52 clipboard access and OSC notifications are
  denied by default; configure their policies only if you trust the program.
  Windows notification settings or Focus Assist can also suppress alerts.

## Crash reports and privacy

Diagnostics are enabled by default. On Windows, logs and unhandled-exception
minidumps are written under `%LOCALAPPDATA%\termica\logs` when possible. To
opt out before launching from PowerShell, set `$env:TERMICA_DIAGNOSTICS = '0'`;
remove the variable to restore the default. A missing dump does not mean a
crash did not happen: the process may not have been able to write one.

To report a crash, record the Windows version, exact steps and the output of
`termica.exe --version` from the affected build. Include any relevant log
entries and indicate whether a dump exists. **Minidumps can contain private
terminal and process memory, secrets, and paths.** Inspect logs and config for
sensitive information too; redact before sharing. Do not upload a dump to a
public issue: arrange a private transfer with a maintainer if needed. Use the
[bug report template](../.github/ISSUE_TEMPLATE/bug_report.yml).
