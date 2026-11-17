# termica: milestone 6

A tiny SDL3 terminal that answers four questions:

1. **Does the stack produce the borderless, translucent, Nerd-Font look from your WezTerm
   screenshot?** (milestone 0 - yes)
2. **Can libghostty-vt drive a real terminal grid behind that look?** (milestone 1 - yes)
3. **Can it run a real shell?** (milestone 2 - yes)
4. **Can a program use the pointer, and can the reader reach what scrolled away?**
   (milestone 3 - yes: mouse reporting, scrollback, selection and copy, focus events)

Milestone 4 was a fidelity pass on what milestone 2 and 3 shipped: the five underline kinds
draw differently, a selection follows its own text through output and resize, the scrollbar
is a control rather than an indicator, every `Alt+*` reaches the shell, and the executable is
GUI-subsystem so nothing but the terminal window appears.

Milestone 5 made it a place to live: a settings file that remembers font, size, opacity,
backdrop, shell and clipboard policy; OSC 52 clipboard writes with an ask/allow/deny policy;
copy-on-select; a pointer shape the program can set; uncommitted IME text drawn at the
cursor; and a drag that keeps pulling history in when you hold it past an edge. The first
two are tested in `--selftest`; the rest need a real keyboard, mouse or input method and are
listed as manual checks.

Milestone 6 moved that settings file to TOML: `termica.toml` with `[font]`, `[window]` and
`[terminal]` sections, parsed by [tomlc17](https://github.com/cktan/tomlc17) and rewritten
in a canonical form on exit. Typed values are checked for type and range, and a syntax error
is reported with its line instead of silently keeping half a file. The old `termica.conf` is
no longer read. `font.face` now also accepts the installed family name, resolved through the
Windows font registry, so a font can be named the way its installer shows it.

What it draws: a borderless window with a translucent dark panel, opaque text on a fixed
cell grid, a block cursor and a status line. Text goes through a one-texture-per-codepoint
glyph cache, which is the same technique the real renderer will use. Drag the top strip to
move the window, drag the edges to resize.

What is behind it: a **GhosttyTerminal** from [libghostty-vt](https://lib.ghostty.org/vt/)
owns the screen. It parses everything a program writes - SGR colors and attributes, cursor
movement, erase, scroll, OSC - and the keyboard goes back through the same library's key
encoder. This is the flow from
[Your First Terminal](https://lib.ghostty.org/vt/first-terminal.html).

The program itself is a real process in a Windows pseudoconsole (ConPTY): `pwsh.exe` if it
is installed, else `powershell.exe`, else `cmd.exe`. Its output drives the grid, keystrokes
go into its console, the window size is the console size, and closing the window kills the
process tree - while `exit`, or the shell ending for any other reason, closes the window.

| Key | Action |
|---|---|
| typing, arrows, Home/End/PgUp/PgDn, F1-F12 | encoded by libghostty-vt and written to the shell |
| Enter / Backspace / Esc | go to the shell; the shell echoes them, not the app |
| **every `Alt+*`** | goes to the shell: readline's Meta bindings are theirs, not the app's |
| Ctrl+V or Ctrl+Shift+V | paste; bracketed paste when the program enabled it |
| wheel | scrollback by 3 rows per notch, unless the program asked for the mouse |
| Shift + wheel or drag | take the pointer back from the program, for scrolling and selecting |
| Shift+PgUp / Shift+PgDn | scroll a full page |
| Ctrl+Shift+Up / Down | scroll one row |
| Ctrl+Shift+Home / End | scroll to the top / back to the live view |
| scrollbar | drag the thumb, or click the track to page toward the click |
| drag / double click / triple click | select cells / a word / a line; Alt+drag is a block |
| Ctrl+Shift+C | copy the selection to the clipboard |
| Ctrl+Shift+= / Ctrl+Shift+- | panel opacity (text must stay fully opaque) |
| Ctrl+Shift+B | cycle the backdrop: panel, Mica, Mica Alt, Acrylic |
| Ctrl+Shift+Q | quit |
| `exit` (or the shell ending for any reason) | closes the window with it |

Mouse, history and focus: a program that asks for the pointer (modes 9,
1000, 1002, 1003, with the 1005/1006/1015/1016 formats) gets encoded
pointer events, `fzf` included. Output that scrolls off the top stays
reachable in scrollback - by the wheel, by `Shift+PgUp/PgDn`, by
`Ctrl+Shift+Up/Down/Home/End`, or by dragging the thumb on the right edge,
which is a control and not just an indicator. Selecting text highlights it
by swapping its colors and copies with `Ctrl+Shift+C`; a selection keeps
naming the same text while output scrolls or the window is resized, and a
drag in progress keeps its anchor on the text it started on. Programs that
enabled focus reporting (mode 1004) learn when the window comes and goes.
Underlines keep their SGR kind, so a curly spell-check wave and a single
error line do not look alike.

> **On Windows:** PowerShell 7 + WezTerm, everything from Scoop. See
> [README-powershell.md](README-powershell.md) - llvm-mingw (clang + lld +
> MinGW-w64 headers) driven by CMake + Ninja. No MSYS2, no mintty, no POSIX
> layer. `build.ps1` / `run.ps1` are the entry points.

## 1. Install the tools (no Visual Studio)

You need: **CMake**, **Ninja**, a **clang + MinGW-w64** toolchain, and **git**
(CMake uses it to download SDL3). All of them just need to be on `PATH`.

**On Windows, use Scoop:**

```powershell
scoop install cmake ninja git
scoop install mingw-mstorsjo-llvm-ucrt   # llvm-mingw: clang + lld + headers
```

`mingw-mstorsjo-llvm-ucrt` is llvm-mingw: upstream LLVM plus the MinGW-w64
CRT, headers and import libraries in one package, with the
`x86_64-w64-mingw32-clang` drivers the `llvm-mingw` preset expects. Plain
`scoop install llvm` is **not** enough - it has no mingw sysroot and cannot
target Windows on its own.

Check: `x86_64-w64-mingw32-clang --version`, `cmake --version`, `ninja --version`.

**Zig is still not needed.** CMake downloads the prebuilt libghostty-vt release
zip for Windows, so the library is linked without a Zig toolchain.

## 2. Fonts: Maple Mono NF is built in

The executable carries **Maple Mono NF** - regular, bold, italic and bold-italic, version
7.9 from [subframe7536/maple-font](https://github.com/subframe7536/maple-font), SIL OFL 1.1 -
linked in as Windows resources. There is nothing to install and no font path to pass: prompt
symbols, box drawing and Nerd icons render out of the box. The program prints
`font: Maple Mono NF (built in)`.

Its icons are **single-width**: at 16 pt every patched codepoint (powerline separators,
`nf-fa-git`, seti icons) has the same advance as `M`, so the grid stays aligned. The ink of
the powerline and Font Awesome glyphs is 11-13 px wide, slightly more than one cell, which is
deliberate - the segments have to touch. A cell that follows one is drawn after it, so the
overlap only shows where a coloured prompt segment ends.

A path on the command line still wins:

```
build\llvm-mingw\termica.exe "C:\path\to\AnotherFont-Regular.ttf" 16
```

If that file cannot be opened, the built-in family is used instead. With a font from disk
that lacks a codepoint (Consolas has no U+276F chevron and no Nerd icons at all), the
built-in regular face draws that glyph, so icons keep working. There is no further
fallback: Maple Mono NF carries no CJK and no colour emoji, and those stay blank unless the
chosen font has them. (The `MapleMono-NF-CN` build of the same family does carry CJK, if
you want that as the built-in instead.)

For a font from disk the program looks for matching bold, italic and bold-italic faces next
to the regular one (`consola.ttf` -> `consolab.ttf`, `... Nerd Font.ttf` -> `... Nerd Font
Bold.ttf`), and fakes bold by drawing the glyph twice when there is no bold face. The
built-in family has all four real faces.

The TTFs live in `assets/fonts/` with their `LICENSE.txt`; CMake embeds them through
`src/fonts.rc.in`. That is where roughly 9.5 MB of the executable's size goes.

## 3. Build and run

```
cmake --preset llvm-mingw
cmake --build --preset llvm-mingw
build\llvm-mingw\termica.exe              # uses termica.toml, 11 pt built-in by default
build\llvm-mingw\termica.exe 18           # point size override
build\llvm-mingw\termica.exe C:\path\to\Font.ttf 16   # face and size overrides
```

Or, from PowerShell 7 / WezTerm:

```
pwsh -File ./build.ps1
pwsh -File ./run.ps1
```

`run.ps1` passes `-Font` / `-Size` through only when you give them, so the
settings file decides the face and size by default.

The first configure downloads and builds SDL3 and SDL3_ttf, and downloads the
libghostty-vt zip, so expect a few minutes. After that rebuilds take seconds.

Other presets: `llvm-mingw-debug`, and `gcc` for any gcc already on `PATH` (Scoop's `mingw`, WinLibs).

## 4. Settings

`%APPDATA%\termica\termica.toml` (elsewhere
`$XDG_CONFIG_HOME/termica/termica.toml` or
`~/.config/termica/termica.toml`), a TOML file parsed with
[tomlc17](https://github.com/cktan/tomlc17). It is read at startup, so edits
apply on the next launch; anything on the command line wins over the file for
that run. On exit the app writes the values it was using, unless the file
changed while it was running - a hand edit is never overwritten by the copy
the app still has in memory.

```toml
[font]
face = ""                    # empty = the built-in Maple Mono NF
size = 11.0

[window]
opacity = 1.0                # 0..1
backdrop = "mica"            # panel, mica, mica_alt, acrylic
decoration = "native"        # rounded, square, compact, native
title_bar = true             # a drawn title bar (not with native)

[terminal]
copy_on_select = false
shell = "auto"               # auto, pwsh, powershell, cmd
osc52 = "ask"                # ask, allow, deny
```

| Key | TOML type | Values | Default |
|---|---|---|---|
| `font.face` | string | path to a `.ttf`, an installed family name, or `""` | `""` (the built-in Maple Mono NF) |
| `font.size` | float or integer | point size, greater than `0`, up to `200` | `11.0` |
| `window.opacity` | float or integer | `0`..`1` | `1.0` |
| `window.backdrop` | string | `panel`, `mica`, `mica_alt`, `acrylic` | `"mica"` |
| `window.decoration` | string | `rounded`, `square`, `compact`, `native` | `"native"` |
| `window.title_bar` | boolean | `true`/`false` | `true` |
| `terminal.copy_on_select` | boolean | `true`/`false` | `false` |
| `terminal.shell` | string | `auto`, `pwsh`, `powershell`, `cmd` | `"auto"` |
| `terminal.osc52` | string | `ask`, `allow`, `deny` | `"ask"` |

Section and key names fold case and separators, so `[Font]` with
`Face`/`Size`, or `CopyOnSelect`, name the same settings as the canonical
spelling. Values must have the type in the table: `size = "16"` and
`copy_on_select = "yes"` are refused, not converted. Bad values leave the
previous value alone; unknown keys and tables are ignored.

`font.face` names a font file or an installed family; `font.path` is still
read as an alias so a file written by an older build keeps working, but the
canonical output uses `face`.

The defaults are the canonical block above: the built-in font at 11 pt, full
opacity, the Mica backdrop and the native Windows frame. `title_bar = true` is
kept in the file even though `native` ignores it, so switching the decoration
back to one of the custom frames brings the bar with it.

`decoration` picks the frame. `rounded` is the custom borderless window with
the squircle corners (the original look); `square` keeps the borderless frame
but gives the panel square corners; `compact` keeps the rounded corners and
shrinks the side padding to 8 px with a 22 px top strip; `native` gives the
window back to Windows, so the system title bar and its minimize, maximize and
close buttons are used and the panel is square. `title_bar = true` adds a drawn
title bar to the three custom frames: the terminal title, plus minimize,
maximize/restore and close buttons. It is ignored with `decoration = "native"`,
which already has the system buttons. The title bar is 30 logical pixels high
and its three buttons are 46 wide.

TOML strings need their escapes. For a Windows path either escape the
backslashes:

```toml
[font]
face = "C:\\Windows\\Fonts\\consola.ttf"
```

or use a TOML literal string, where backslashes are exactly what they look
like:

```toml
[font]
face = 'C:\Windows\Fonts\consola.ttf'
```

A family name works too, and is easier to keep across updates:

```toml
[font]
face = "Inconsolata Nerd Font Propo"
size = 18.0
```

On Windows the name is looked up in the font registry (per-user fonts first,
then machine-wide), matched against the display names with the `(TrueType)` /
`(OpenType)` suffix removed, and the regular face wins over bold or italic.
Relative file names in the registry are resolved against `%WINDIR%\Fonts` and
`%LOCALAPPDATA%\Microsoft\Windows\Fonts`. If the name is not found, the
built-in font is used and the `font:` line on stdout says so.

A TOML syntax error rejects the whole document, so the app starts with its
defaults and prints the parser's message and line number on stderr when it
was started from a console. An untouched file is rewritten on exit in the
canonical form above, so hand-written comments and unknown keys are not
preserved. If the file was edited, created or deleted while the app ran, the
save is skipped and the file stays as the user left it. The old
`termica.conf` is not read any more. tomlc17 is MIT-licensed; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

`shell` names a program rather than a path: if the named shell is not
installed the terminal says so on screen instead of quietly starting another
one.

`osc52` decides what happens when a program asks for the clipboard (OSC 52,
iTerm2's OSC 1337 and the kitty protocol all arrive normalized and decoded).
`ask` pops the same style of two-button dialog as an unsafe paste and answers
the program with what the user chose; `allow` writes it, `deny` refuses. The
default is `ask`, because a host you ssh into can otherwise replace your
clipboard unnoticed.

## 4b. Status bar (`statusbar.toml`)

The bar along the bottom of the window is configured by a second file,
`statusbar.toml`, next to `termica.toml`. It uses the Starship model: a
top-level `format` string of `$module` tokens and literal text, and one
`[module]` table per module. The bar's font is separate from the grid font
and never smaller than 9 pt.

```toml
format = "$directory$git_branch$git_status $cmd_status$cmd_duration"
center_format = "$busy"
right_format = "$time"

[font]
face = "GeistMono NF"          # installed family; falls back to Maple Mono NF
size = 9                       # points; values below 9 are refused

[style]
fg = "#d8dee9"
bg = "#0e1216"
opacity = 0.9

[directory]
style = "fg:#9ccfd8 bold"
truncation_length = 3          # trailing path components; 0 = full path
home_symbol = "~"

[cmd_status]
style_success = "fg:#a3be8c"
style_failure = "fg:#bf616a"
symbol_success = "\u2713"
symbol_failure = "\u2717"

[cmd_duration]
min_time = 2000                # ms; hide commands faster than this

[time]
time_format = "%H:%M:%S"

[busy]
style = "fg:#81a1c1"
idle = ""                        # optional resting-icon override (U+F1807 if unset)
```

The bar has three containers, laid out left, centred and right-aligned:
`format`, `center_format` and `right_format`, each a string of `$module`
tokens and literal text.

Modules: `$directory` (the shell's working directory), `$git_branch` and
`$git_status` (branch, ahead/behind, dirty and conflicted counts, shown only
inside a repository), `$cmd_status` and `$cmd_duration` (the previous
command's exit status and wall time), `$busy` (a spinner while a command is
running) and `$time`. A single space directly after a `$module` is dropped
when that module renders empty, which is what makes the git section disappear
cleanly outside a repository. See `[busy]` below for keeping a slot's width
fixed.

A module's `format` is a mini template of `[text]($style)` groups, with
`$symbol` and `$value` substituted; `($style)` uses the module's own style.
The style grammar is `fg:#RRGGBB`, `bg:#RRGGBB`, `bold`, `italic`,
`underline`, `dim`. `center_format` and `right_format` are the optional
centre and right containers (the example puts `$busy` centred and `$time`
right-aligned, lualine-style).

The `[busy]` spinner is fixed: the ten braille frames (U+2800 block) and the
resting icon (U+F1807, a Material Design spinner) are built in and cannot be
changed, so there is no `symbols` or `symbol` to get wrong. The slot always
keeps its cell - a frame while a command runs, the resting icon at idle - so
the bar does not reflow. `[busy] idle` is the one knob: set it to a glyph to
override the resting icon at idle (leave it empty or remove the line for the
built-in U+F1807).

Symbol glyphs like these live above U+FFFF, which the glyph cache does not
cover; termica draws them uncached and, when the terminal font lacks one,
falls back to **Symbols Nerd Font Mono** (compiled into the executable,
resource 5) - the same per-glyph fallback WezTerm uses, so a Nerd icon pastes
onto the grid and renders even under a base font that does not carry it. As
with all settings, `statusbar.toml` is read at startup: restart termica after
editing.

The dynamic modules need the shell to cooperate: termica sets
`TERMICA_SHELL_INTEGRATION=1` in the child's environment, and the snippets in
`shell/` (PowerShell, bash, zsh) emit the standard OSC 7 (directory) and
OSC 133 (prompt / command steps) sequences when they see it. The directory is
taken from the shell's own location (`$PWD`), not the process working
directory, because PowerShell keeps those separate: `cd` changes `$PWD` and
what your prompt shows, but not the process's current directory, so OSC 7 is
the only thing that makes the bar's path track your prompt.

The snippets **wrap** whatever prompt you already have (starship, oh-my-posh,
pure, the default), so your prompt keeps rendering — capture its definition
and call it from inside the integration. For that to work, dot-source the
snippet **after** your prompt init in the profile, e.g.:

```powershell
Invoke-Expression (&starship init powershell)
. C:\path\to\termica\shell\termica-pwsh.ps1
```

Without the snippet the bar still renders; the directory stays at the folder
the shell started in and the command modules stay hidden, since no command
boundaries or directory changes are reported.

`statusbar.toml` is read at startup and rewritten in canonical form on exit
with the same hand-edit protection as `termica.toml`.

## 5. Testing: the self test and a sanitizer run

The terminal half has a self test that follows the documentation example, so the stack can
be checked without a display:

```
build\llvm-mingw\termica.exe --selftest
```

It creates a terminal, writes the bytes from [Your First
Terminal](https://lib.ghostty.org/vt/first-terminal.html), formats the screen back to plain
text with a `GhosttyFormatter`, and checks the result. Then it checks the parts the app
depends on: resolved colors, bold/underline, the cursor position, erase, wide characters
(head cell + spacer tail), key encoding (arrows, Ctrl+letter, Enter, Backspace), resize and
the OSC window title; the history and pointer half (scrolling by rows, to the top and to the
bottom, the scrollbar figures, cell / word / line / block selections, the copied text, mouse
and focus encoders against a terminal that enabled the modes); the underline kinds (SGR 4,
4:2, 4:3, 4:4, 4:5 must come out as single, double, curly, dotted and dashed); the TOML
settings parser (sections, typed values, bounds, key folding, string escaping and a full
format round trip); the font-name resolver (an installed family resolves through the registry,
an existing path is returned unchanged); and the OSC 52 clipboard path
(decoded payload, multi-line payload, clear request, denial, and no handler installed).
The last checks start a real ConPTY: `cmd.exe /c "echo termica-pty-ok"` and a non-ASCII echo
have to come back through the pipes as UTF-8 with exit code 0, and a `powershell.exe` child
that writes an OSC 52 request has to reach the app's clipboard callback decoded. The settings
checks also cover the `decoration` enum and the `title_bar` boolean, including refused values
and the format round trip. The status-bar checks cover `statusbar.toml` (the 9 pt floor, the
format string, the style grammar, the canonical format round trip), the renderer (module
values and empty-module space elision), the pure helpers (path shortening, duration text, git
glyphs, spinner frames), the `git status --porcelain=v2` parser, and the OSC 7 / OSC 133
callbacks that feed the bar's directory, exit-status, duration and busy modules. That is 188
checks; the exit code is the number of failures.

Everything in that list runs without a window and without touching the keyboard or mouse of
whoever is logged in, which is deliberate: the features that can only be exercised by a real
user (IME composition, pointer shape, selection autoscroll, the OSC 52 prompt) are listed
under "What to check" as manual checks rather than claimed as tested.

### How the windowed checks are counted as evidence

Some behaviour only shows up in a live window, so it has been checked by driving one. That
needs a rule about what counts:

- **In the self test** - headless, no window, no input: counts.
- **A change seen in termica's own pixels** (a screenshot, or a sampled band of the window):
  counts, because only input that actually reached the window can have caused it. This is
  how the five underline kinds, the scrollbar thumb and track, `fzf` taking the pointer and
  `Ctrl+Shift+=` changing the opacity were confirmed.
- **Messages posted to termica's `HWND`** (`PostMessage`/`SendMessage`): counts, they cannot
  land anywhere else.
- **Keystrokes or mouse events injected desktop-wide** (`SendKeys`, `keybd_event`,
  `mouse_event`, `SetCursorPos`) with the only proof *outside* the window - clipboard
  contents, a dialog that did not appear: does not count, because that input goes to
  whichever window the desktop has focused, and it disturbs the session doing it. The same
  goes for any negative result ("this key changed nothing"), which cannot tell a refused
  key from a misdelivered one. Those checks are re-listed as manual.

See PLAN's "Harness notes" for the same rule with the specific claims it applies to.

### Memory safety

A second configuration builds the app with AddressSanitizer and
UndefinedBehaviorSanitizer, which catch buffer overflows, use after free, double frees,
misaligned access and other undefined behaviour while the program runs:

```
cmake -S . -B build/asan -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang ^
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ ^
  -DFETCHCONTENT_SOURCE_DIR_SDL3=build/_deps/sdl3-src ^
  -DFETCHCONTENT_SOURCE_DIR_SDL3_TTF=build/_deps/sdl3_ttf-src ^
  -DTERMICA_SANITIZE=ON
cmake --build build/asan
build\asan\termica.exe --selftest
```

Reusing the already downloaded SDL sources keeps that configure quick. Then run the GUI
normally: any finding is printed to stderr with a stack trace and a non-zero exit code.

LeakSanitizer is not available for the `windows-gnu` target, so this does not report leaks.
Everything the app owns is freed on the way out (`term_free`, `panel_free`, `glyphs_close`,
then the renderer and the window), and every setup step cleans up after itself if it fails
half way, so an early `return` cannot leak either.

## 6. What to check (this is the experiment)

1. **Transparency.** Is the wallpaper visible behind the panel, while the text stays crisp and opaque?
   Press Ctrl+Shift+= / Ctrl+Shift+-: the panel should fade, the text should not.
2. **Corners.** The window should have macOS-style corners: 10 pt, drawn as a squircle (continuous
   curvature) and antialiased, with the border following the curve.
3. **Backdrops.** Ctrl+Shift+B cycles panel -> Mica -> Mica Alt -> Acrylic. The status line names the
   current one. Mica should pick up the tint of the wallpaper, and Mica Alt should look different.
4. **Edges.** Drag and resize. The grid re-fits, the terminal is resized with it, and a Mica
   window keeps its rounded region while resizing.
5. **The shell.** A prompt appears (PowerShell 7, Windows PowerShell or `cmd`, whichever is
   installed). `dir`, `cls`, `echo %CD%` and a quoted argument work. Ctrl+C interrupts
   `ping -t localhost`. Tab completion and the PSReadLine menu draw correctly. `vim` uses the
   alternate screen and Esc leaves insert mode instead of closing the terminal.
6. **Typing.** Characters appear at the prompt because the shell echoes them, not the app.
   Enter runs the line, Backspace edits it, Up/Down walk the history, arrows work inside a line.
   Ctrl+V pastes; pasting several lines asks for confirmation when the program has not enabled
   bracketed paste.
7. **Colors and attributes.** A program that emits SGR shows bold, underline, 24-bit color and
   the 16 ANSI colors (`git log`, `ls`, or a PowerShell escape-sequence sample).
8. **Resize.** Drag an edge while `more` or a full-screen program runs: the console is resized
   with the grid and the program reflows.
9. **Text quality.** Compare the text with WezTerm at the same size. Look at it on your actual display scale.
10. **Console output.** The program prints the renderer name (you want `direct3d11` or
    `direct3d12`), the cell size and whether it found bold/italic faces. Closing the window
    while the shell runs must leave no `conhost.exe` or shell behind in Task Manager.
11. **Scrollback.** Run something long (`dir C:\Windows\System32 -Name`), then scroll the
    wheel up: the thumb on the right edge shows where you are, `Ctrl+Shift+End` / `Home` go
    to the ends, and typing anything returns to the live view.
12. **Selection and copy.** Drag across text (double click for a word, triple for a line,
    Alt+drag for a block) and press `Ctrl+Shift+C`; paste it back with `Ctrl+V` and it should
    arrive unchanged.
13. **Mouse reporting.** `dir C:\Windows\System32 -Name | fzf` takes the pointer: a click
    moves the selection inside fzf instead of selecting terminal cells, and holding Shift
    while dragging takes the pointer back for selecting.
14. **Focus reporting.** A program that sets mode 1004 (vim with `reportFocus`, or
    `\x1b[?1004h` from a script) receives `CSI I` / `CSI O` when this window gains or loses
    focus.
15. **Underline kinds.** `printf '\x1b[4msingle\x1b[4:3mcurly\x1b[4:4mdotted\x1b[4:5mdashed\x1b[m\n'`
    (in PowerShell: `Write-Host` with `` `e[4:3m ``) must draw four visually different
    underlines, not one repeated bar.
16. **The scrollbar is a control.** With history on screen, grab the thumb and move it: the
    view follows. Click above or below it: the view pages that way. This works even while a
    program owns the pointer, because the strip is outside the cell grid.
17. **Alt belongs to the shell.** `Alt+b` / `Alt+f` move a word in PowerShell and bash,
    `Alt+M` does whatever readline says it does; nothing under Alt is the app's any more.
18. **No console window.** Double-click `termica.exe` in Explorer: only the terminal window
    appears. Started from a terminal, the `renderer:` / `font:` lines still land inline.
19. **Settings survive a restart.** Change opacity (`Ctrl+Shift+-`) or backdrop
    (`Ctrl+Shift+B`), close the window, start again: the values come back from
    `%APPDATA%\termica\termica.toml`, and the TOML file is plain enough to edit by hand.
    Verified: `shell = "cmd"` starts `cmd.exe` and nothing else (checked in the process
    tree), and the file after exit holds the values the session was using.
    A `font.face` that is an installed family name (for example
    `"Inconsolata Nerd Font Propo"`) resolves to that family's regular file; the `font:`
    line names the `.ttf` it opened and the cell size reflects `font.size`.
20. **A program can ask for the clipboard.** The OSC 52 path is covered by `--selftest`,
    including the real case: a `powershell.exe` child under ConPTY writes
    `\e]52;c;<base64>\a` and the app's handler receives it decoded. Try the `ask`
    policy by hand: it should name the request in a dialog and answer the program
    with what you chose.
21. **Window decoration.** Set `decoration` to each of `rounded`, `square`, `compact` and
    `native` and restart. The first three should keep the borderless window, with the panel
    corners, padding and top strip as described in section 7; `native` should show the normal
    Windows title bar and buttons. With `title_bar = true` the custom frames should draw the
    terminal title and the three buttons: minimize should go to the taskbar,
    maximize/restore should toggle, and close should exit cleanly. The title the shell sets
    should appear on a fresh start.
22. **Long selections (manual).** Press the left button, drag below the last row and
    hold: the view should keep scrolling and the selection keep growing from the
    tracked anchor. This needs a real pointer; it is not covered by the self test.
23. **Pointer shape (manual).** A program that sets it (OSC 22, e.g.
    `printf '\e]22;pointer\a'`) should change the cursor over the grid, and leaving the
    window should hand the cursor back. Not covered by the self test.
24. **IME composition (manual).** With an input method active, the uncommitted text
    should appear at the cursor cell and the candidate window open there. Not covered
    by the self test; no input method is installed on the machine this was built on.

## 7. Window chrome: the corners, the backdrops and the decorations

The window is borderless, per-pixel transparent (SDL does that with
`DwmEnableBlurBehindWindow` and an empty blur region) and drawn by the app. That rules out the
easy path for both effects, so both are done by hand.

**Corners.** `DWMWA_WINDOW_CORNER_PREFERENCE` (Windows 11's rounded corners) has no effect on
this window: DWM applies it to the window frame it draws, and a per-pixel transparent window is
composited differently, so the corners stay square. They are drawn by the app instead: the panel
is a rounded rectangle in three bands plus four pre-rendered corner masks, 4x4 supersampled, with
the border following the same curve. The radius is 10 pt, like macOS Big Sur and later, and the
curve is a **squircle** - `(u/r)^4 + (v/r)^4 <= 1` - rather than a circular arc, because that is
what macOS's "continuous" corners are. At the same radius a squircle looks rounder and softer
than the arc Windows 11 uses.

**Mica.** Mica is a Windows 11 22H2+ DWM material: the wallpaper, blurred and tinted by the
system theme. It works with this window, with three requirements:

1. `DWMWA_SYSTEMBACKDROP_TYPE` = `DWMSBT_MAINWINDOW` (2); `DWMSBT_TABBEDWINDOW` (4) is Mica Alt
   and `DWMSBT_TRANSIENTWINDOW` (3) is Acrylic. The window must look like a normal framed window
   to DWM, and SDL's borderless style keeps `WS_CAPTION`, so it does.
2. The frame has to cover the client area: `DwmExtendFrameIntoClientArea(hwnd, {-1,-1,-1,-1})`.
   Without it the material only exists behind the zero-height frame and nothing shows.
3. The app's pixels above the backdrop have to be transparent, so the panel fill drops to a
   light tint (up to about 24% of the panel color) when a backdrop is on. The opacity keys
   still work.

Two side effects are handled:

- **The system caption buttons come with the extended frame.** The custom frames have their own
  drag strip, so `WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX` are dropped while Mica is on and
  restored when it goes off. A `native` decoration keeps them: it is the Windows frame.
- **The backdrop covers the whole window rectangle, corners included.** A drawn mask cannot cut
  into it, so in Mica modes the window is shaped with `SetWindowRgn`, using the same squircle
  sampled into a polygon. Two costs: regions are not antialiased, so those corners are hard
  edged, and clicks outside the shape go through to the desktop instead of hitting the window.

On Windows 10 and the first Windows 11 release the attribute is rejected (`DwmSetWindowAttribute`
fails), and the app falls back to the panel. It was verified on Windows 11 25H2 (build 26200):
the material follows the wallpaper - set the wallpaper to red and the window turns red - and
Mica Alt is visibly different from Mica.

**Decorations.** `decoration` is a small table of frame presets. The first three keep the
borderless transparent window; only the values they feed into the panel and the top strip
change. `rounded` draws the squircle panel and reserves the usual 34 px at the top; `square`
passes a corner radius of 0, so the panel and the window region are rectangles; `compact`
drops the side padding from 12 to 8 px and the top strip from 34 to 22 px. `native` removes
`SDL_WINDOW_BORDERLESS` and the hit test, so Windows draws the frame and the caption buttons,
and the client panel is square. The custom title bar is a 30 px strip drawn inside the panel:
a subtle tint clipped to the panel's top corners, the terminal's title in the same font as the
grid, and three 46 px buttons on the right. The buttons use the ordinary hover colours (red
for close) and call `SDL_MinimizeWindow`, `SDL_MaximizeWindow` / `SDL_RestoreWindow`, and the
app's normal shutdown. The hit test reports the button strip as normal, so a click is a click
and not the start of a window drag.

## If transparency does not work

Try forcing a renderer before launching: `set SDL_RENDER_DRIVER=direct3d11` (or `direct3d12`, `vulkan`)
and run again. If every renderer shows an opaque (usually black) background, that tells us SDL's
transparent window is not enough on your machine. The fallback is to keep SDL for input and the event
loop, take the `HWND`, and drive a DirectComposition swap chain with premultiplied alpha ourselves.

## What the terminal emulates

All of this is libghostty-vt, not hand-written code in this repo:

- the VT parser: CSI, OSC, DCS, APC, SGR (`\x1b[...m`), cursor movement, insert/delete,
  erase, scroll regions, tabs, origin mode, wrap, alternate screen, bracketed paste, ...
- the screen: primary and alternate screens, scrollback, reflow on resize, wide characters,
  grapheme clusters, 256-color and 24-bit color
- keyboard encoding: arrow keys, function keys, keypad, modifiers, and the terminal modes
  that change them (DECCKM, kitty keyboard protocol, ...)
- effects: OSC window title (applied to the SDL window); bell and the rest are ignored for now

The app itself only does the parts the library leaves to the embedder:

- draws the render state: cell text, resolved colors, bold/italic/underline/strike/overline,
  dim, inverse, invisible, wide-character tails, all four cursor styles and blinking
- runs the shell: `pty.c` creates the pseudoconsole, starts the process, reads its output on
  a thread into a bounded ring buffer and writes keys, pastes and the terminal's own replies
  into the input pipe (`GHOSTTY_TERMINAL_OPT_WRITE_PTY`)
- passes the clipboard through `ghostty_terminal_paste`, so bracketed paste, the kitty paste
  protocol and the unsafe-paste check are the library's rules, not reimplemented ones
- resizes with `ResizePseudoConsole`, and kills the process tree with a kill-on-close job
  object when the window closes
- turns pointer events into bytes with `ghostty_mouse_encoder` (geometry in pixels, tracking
  mode and format taken from the terminal), and focus events with `ghostty_focus_encode`
- owns the gestures and the history UI: click counts and drags become `GhosttySelection`s
  through `ghostty_terminal_grid_ref` / `select_word` / `select_word_between` /
  `select_line`, a drag in progress keeps a tracked anchor so it cannot slide off its own
  text, the copy goes through a formatter restricted to that selection, and the viewport
  moves with `ghostty_terminal_scroll_viewport` (`GHOSTTY_TERMINAL_DATA_SCROLLBAR` drives
  both the thumb and its hit test)

## Where the code lives

| file | what it does |
|---|---|
| `src/main.c` | the app: event loop, keyboard and paste, spawning and draining the shell, one frame |
| `src/term.{h,c}` | the libghostty-vt wrapper: screen, render state, key encoder, replies, paste |
| `src/pty.{h,c}` | ConPTY: pipes, the child process, the reader thread, resize, teardown |
| `src/embed.{h,c}` | the Maple Mono NF bytes linked into the executable |
| `src/console.{h,c}` | reclaiming the launching console, since the exe is GUI-subsystem |
| `src/settings.{h,c}` | the `termica.toml` settings file (TOML via tomlc17): read at startup, written on exit when untouched |
| `src/statusbar.{h,c}` | `statusbar.toml`, the style/format grammar, module helpers and the line renderer |
| `src/git.{h,c}` | the background `git status --porcelain=v2` runner and its parser |
| `src/font_lookup.{h,c}` | a font path or installed family name -> the font file to open |
| `src/fonts.rc.in` | the Windows resource script that carries those bytes |
| `src/glyph.{h,c}` | fonts, the glyph cache, drawing a cell's text (bold/italic faces) |
| `src/style.{h,c}` | a terminal cell style -> colors and decorations for the renderer |
| `src/panel.{h,c}` | the translucent panel, round or square, and its antialiased corner masks |
| `src/geometry.{h,c}` | the squircle curve: pure math, shared by the panel and the region |
| `src/window.{h,c}` | the SDL window, the decoration presets, the drag/resize hit test, the Windows 11 chrome |
| `src/selftest.c` | `--selftest`, including the ConPTY round trip |

`geometry.c` and `style.c` have no SDL state and no Windows calls, which is what makes them
easy to reason about on their own.

## Known limits of this milestone

- **ConPTY only.** The shell needs Windows 10 1809 or later. `pty.c` has a stub for other
  platforms, so the app still builds there, but no shell starts.
- Glyph cache covers the Basic Multilingual Plane. The fallback covers only the built-in
  face: a character neither the chosen font nor Maple Mono NF has (most CJK, colour
  emoji) is blank.
- One font scale chosen at startup. Moving the window between monitors with different DPI
  will not rescale (the corner masks would need a rebuild too).
- The Mica/Acrylic backdrops need Windows 11 22H2 or later; below that the app stays on the
  panel. With a backdrop on, the corners are a window region and therefore hard edged, and the
  click-through outside the region means the very corners cannot start a resize.
- macOS also draws a shadow that follows the window shape; DWM draws its own, so the shadow is
  Windows' and is not part of this.
- No hyperlinks (OSC 8) and no kitty graphics: both need anchor geometry and an image
  store in the renderer, not just a callback.
- No desktop notifications, no progress reporting in the taskbar, and the bell is
  swallowed. (`GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION`, `..._PROGRESS_REPORT` and
  `..._BELL` are the hooks.)
- OSC 52 *reads* are not answered, so a remote `tmux`/vim can set the clipboard but
  cannot ask for it (`GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ`).
- IME composition is drawn at the cursor and the candidate window is placed there, but
  the preedit is limited to 128 bytes and replaces the cell it sits on rather than
  pushing the row aside.
- A very large paste blocks the UI thread while it is written to the console; the pipe is
  synchronous and there is no writer queue yet.
- **Verified on Windows 11 25H2 (build 26200)**: llvm-mingw clang 23.1.5 + lld + Ninja,
  `direct3d11` renderer, translucent panel, built-in Maple Mono NF icons, a live
  `pwsh` 7 / `cmd` shell in ConPTY (the window title comes from the shell's OSC sequence,
  and a command handed to the child comes back through the pipes as UTF-8 with exit code 0 -
  that round trip is a `--selftest` check, not a screenshot),
  wheel and `Ctrl+Shift+*` scrolling with a draggable thumb (the thumb appeared, a track
  click and a thumb drag changed the view, and a sampled band matched the live view),
  drag/word/line selection highlighting the right text, and the selection still naming the
  same text after scrolling and after reflow (measured against the library headlessly, before
  the app stopped clearing it), `fzf` taking the pointer when it asks for it, five
  distinguishable underline kinds, no console window when started by double-click, and closing
  the window while the shell runs leaves no `conhost.exe` or shell behind. Each decoration
  preset started and saved its own value, and screenshots of `rounded` with a title bar over the
  panel and over Mica, plus `native`, showed the expected frame and client area.
  `--selftest` runs 143 checks, all passing: the ConPTY round trips, the TOML settings parser
  and its format round trip, installed font family names resolving through the registry, and
  the OSC 52 path both from a literal byte string and from a real `powershell.exe` child under
  ConPTY. Settings are read at startup and written on exit when the file was not edited meanwhile -
  `shell = "cmd"` was confirmed by inspecting the process tree, and the file after exit holds
  the session's values. No warnings from the project's own sources under
  `-Wall -Wextra -pedantic`.
- Not machine-verified, because they need a real keyboard, mouse or input method:
  copy-on-select, selection autoscroll at the grid edge, the OSC 22 pointer shape, the
  OSC 52 `ask` dialog, IME preedit drawing, and the custom title bar buttons. The buttons
  did maximize, restore and close the window when clicked during development, but the
  pointer events were injected desktop-wide, so by the rule above that is not counted as
  evidence; they are listed as a manual check.
- Also not machine-verified, for a method reason rather than a feature reason: anything
  whose only proof was a clipboard read driven by desktop-wide synthetic keys -
  `Ctrl+Shift+C` reaching the app, the `shell = cmd` setting reaching the child, and the
  `osc52 = allow` write landing on the clipboard. Synthetic keystrokes go to whichever
  window the desktop has focused, so a changed clipboard does not show that termica
  received them; see PLAN's "Harness notes". The selection-to-text path underneath them
  *is* tested headlessly: `--selftest` formats the library's selection and checks the
  copied characters, cell / word / line / block.
- The executable is GUI-subsystem, so it has no console of its own: `src/console.c` reattaches
  to the launching console for diagnostics and leaves a redirected stdout alone.
- SDL3 enables C++ on Windows for its own sources, which is why the presets also set a C++
  compiler. Your own code stays C99.
