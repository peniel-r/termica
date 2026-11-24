<div align="center">

# termica

**A small, native Windows terminal emulator built on [libghostty-vt](https://lib.ghostty.org/vt/), SDL3 and ConPTY.**

![Platform](https://img.shields.io/badge/platform-Windows%2010%201809%2B-0078D4)
![Language](https://img.shields.io/badge/language-C99-555)
![Status](https://img.shields.io/badge/status-pre--alpha%20(v0.0)-orange)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

> [!WARNING]
> termica is **pre-alpha**. The emulation core is solid, but some features that
> people use every day (tabs, custom shell commands, WSL, colour schemes, font
> zoom) are not implemented yet. See [PLAN.md](PLAN.md) for the roadmap to a
> daily-driver release.

## Why termica

- **Correct emulation, without writing it ourselves.** Parsing, screen state,
  scrollback, reflow, key and mouse encoding all come from libghostty-vt,
  the same VT core that Ghostty uses.
- **A real Windows citizen.** It runs your shell in a ConPTY, uses Mica,
  Mica Alt and Acrylic backdrops, has rounded squircle corners or the native
  frame, and ships as a GUI-subsystem executable with no console flash.
- **Works out of the box.** One standalone `.exe` with the Maple Mono NF and
  Symbols Nerd Font faces built in, so prompt glyphs and icons render without
  installing anything.
- **Small and readable.** About 10k lines of strict C99, and a headless
  self-test suite of 204 checks.

## Features

| Area | What works today |
|---|---|
| **Shell** | `pwsh` → `powershell` → `cmd` auto-detection, UTF-8 end to end, resize, the process tree is killed when the window closes, and the window closes when the shell exits |
| **Rendering** | 24-bit colour, bold, italic, dim, inverse, strikethrough and overline, five underline kinds (single, double, curly, dotted, dashed), all cursor styles plus blinking, wide characters |
| **Scrollback** | Mouse wheel, keyboard scrolling, and a draggable scrollbar that also pages when you click the track |
| **Selection** | Cell, word, line and block (Alt) selection. The selection follows its text through output and reflow, autoscrolls at the edges, and can copy on select |
| **Mouse** | Mouse reporting (modes 9/1000/1002/1003, SGR/UTF-8/urxvt/pixel formats), Shift to take the pointer back, OSC 22 pointer shape |
| **Clipboard** | Bracketed paste, a confirmation for unsafe pastes, and OSC 52 writes with an `ask`/`allow`/`deny` policy |
| **Input** | Every `Alt+*` goes to the shell, focus reporting (mode 1004), IME preedit drawn at the cursor |
| **Window** | `rounded`, `square`, `compact` and `native` frames, an optional drawn title bar, Mica/Mica Alt/Acrylic backdrops, adjustable opacity |
| **Status bar** | Starship-style `statusbar.toml`: directory, git branch and status, last command's exit code and duration, a busy spinner, a clock |
| **Config** | `termica.toml` with typed, validated values. Your hand edits are never overwritten |

## Requirements

- **Run:** Windows 10 1809 or later (ConPTY). The Mica and Acrylic backdrops
  need Windows 11 22H2 or later. On older systems termica falls back to its own
  drawn panel.
- **Build:** CMake ≥ 3.21, Ninja, git, and the
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) toolchain. You don't need
  Visual Studio, MSYS2 or Zig.

## Quick start

```powershell
# 1. Toolchain (Scoop)
scoop install cmake ninja git mingw-mstorsjo-llvm-ucrt

# 2. Build: the first configure downloads SDL3, SDL3_ttf, tomlc17 and the
#    prebuilt libghostty-vt, so allow a few minutes
pwsh -File ./build.ps1

# 3. Run
pwsh -File ./run.ps1
```

Or use CMake directly:

```powershell
cmake --preset llvm-mingw
cmake --build --preset llvm-mingw
build\llvm-mingw\termica.exe
```

> [!NOTE]
> Plain `scoop install llvm` is **not** enough: it has no MinGW sysroot.
> `mingw-mstorsjo-llvm-ucrt` provides clang, lld, the MinGW-w64 headers and the
> `x86_64-w64-mingw32-*` drivers that the presets expect. SDL3 compiles some
> C++, so the presets also set a C++ compiler.

The output is a single standalone executable that links only against Windows
system DLLs.

### Build presets

| Preset | Toolchain |
|---|---|
| `llvm-mingw` | llvm-mingw clang + lld, Release (default) |
| `llvm-mingw-debug` | the same toolchain, Debug |
| `gcc` | any MinGW `gcc` on `PATH` (Scoop `mingw`, WinLibs) |

```powershell
pwsh -File ./build.ps1 -Preset llvm-mingw-debug
pwsh -File ./run.ps1   -Preset llvm-mingw-debug
```

### Command line

```text
termica.exe                          # use termica.toml
termica.exe 18                       # point-size override for this run
termica.exe C:\path\to\Font.ttf 16   # face and size override for this run
termica.exe --selftest               # headless test suite; exit code = failures
```

`run.ps1` accepts `-Font`, `-Size`, `-Preset` and `-SelfTest`.

## Key bindings

App bindings live under **Ctrl+Shift** so that they never collide with shell
or readline bindings.

| Keys | Action |
|---|---|
| `Ctrl+Shift+C` | Copy the selection |
| `Ctrl+Shift+V` | Paste (bracketed when the program enables it; `Ctrl+V` reaches the shell) |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | Font zoom in / out / reset |
| `Shift+PgUp` / `Shift+PgDn` | Scroll one page |
| `Ctrl+Shift+Up` / `Down` | Scroll one row |
| `Ctrl+Shift+Home` / `End` | Scroll to the top / back to the live view |
| `Ctrl+Shift+=` / `Ctrl+Shift+-` | Panel opacity up / down |
| `Ctrl+Shift+B` | Cycle the backdrop: panel → Mica → Mica Alt → Acrylic |
| `Ctrl+Shift+Q` | Quit |

Bindings are configurable in `[keys]` inside `termica.toml` (for example,
`"Ctrl+V" = "paste"` to restore `Ctrl+V` paste, or `"Ctrl+Shift+Q" = "none"`
to disable a binding).

| Mouse | Action |
|---|---|
| Drag / double-click / triple-click | Select cells / a word / a line |
| `Alt` + drag | Block selection |
| `Shift` + drag or wheel | Take the pointer back from a program that is tracking the mouse |
| Wheel | Scroll the scrollback by 3 rows (or report the wheel to the program) |
| Scrollbar | Drag the thumb, or click the track to page |

## Configuration

Settings live in `%APPDATA%\termica\termica.toml`. On other platforms they are
in `$XDG_CONFIG_HOME/termica/` or `~/.config/termica/`. The file is read at
startup. Command-line arguments override it for that run.

```toml
[font]
face = ""              # "" = built-in Maple Mono NF, a .ttf path, or an installed family name
size = 11.0            # > 0, <= 200
# fallback = ["Segoe UI Emoji", "Yu Gothic"]

[window]
opacity = 1.0          # 0..1
backdrop = "mica"      # panel | mica | mica_alt | acrylic
decoration = "native"  # rounded | square | compact | native
title_bar = true       # drawn title bar (ignored with "native")

[terminal]
copy_on_select = false
shell = "auto"         # auto | pwsh | powershell | cmd
# command = ["wsl.exe", "-d", "Ubuntu"]
# cwd = 'C:\work'
osc52 = "ask"          # ask | allow | deny
bell = "visual"        # visual | taskbar | none

# [terminal.env]
# EDITOR = "nvim"

# [colors]
# scheme = "dark"      # dark | light (individual keys override the preset)
# foreground = "#d8dee9"
# background = "#0e1216"

# [keys]
# "Ctrl+V" = "paste"
```

- **Types are strict.** `size = "16"` is rejected, not converted. An invalid
  value keeps the default, and unknown keys are ignored.
- **A syntax error rejects the whole file.** The parser's message and line
  number go to stderr.
- **Key names fold case and separators.** `CopyOnSelect`, `copy-on-select` and
  `copy_on_select` are the same key.
- **Windows paths** need escaped backslashes (`"C:\\Fonts\\x.ttf"`) or a
  literal string (`'C:\Fonts\x.ttf'`).
- **Font family names** such as `"Cascadia Code"` are resolved through the
  Windows font registry, per-user fonts first. termica finds matching
  bold and italic faces automatically.
- **On exit** termica rewrites the file in canonical form, unless you edited it
  while the app was running. In that case your edit is kept untouched.

### Status bar (`statusbar.toml`)

The status bar is configured in the same folder, using the
[Starship](https://starship.rs) model: three containers made of `$module`
tokens, and one table per module.

```toml
format        = "$directory"
center_format = "$git_branch $git_status"
right_format  = "$cmd_duration $cmd_status $time  $busy"

[directory]
format = "[$symbol $value]($style)"
style  = "fg:#9ccfd8 bold"
truncation_length = 1
```

Modules: `directory`, `git_branch`, `git_status`, `cmd_status`,
`cmd_duration`, `time` and `busy`. The style grammar is
`fg:#RRGGBB bg:#RRGGBB bold italic underline dim`. The bar's font is separate
from the grid font and never smaller than 9 pt. The full default file is in
[DESIGN.md](DESIGN.md#status-bar).

### Shell integration

The directory and command modules need the shell to report its state through
OSC 7 (working directory) and OSC 133 (prompt and command boundaries). termica
sets `TERMICA_SHELL_INTEGRATION=1` for the child process. Source the matching
snippet from [`shell/`](shell/) **after** your prompt init. It wraps your
existing prompt (Starship, oh-my-posh, the default) instead of replacing it.

```powershell
# $PROFILE
Invoke-Expression (&starship init powershell)
. C:\path\to\termica\shell\termica-pwsh.ps1
```

Snippets are provided for PowerShell (`termica-pwsh.ps1`), bash
(`termica-bash.sh`) and zsh (`termica-zsh.sh`).

## Known limitations

These are tracked in [PLAN.md](PLAN.md):

- One window with one session. There are no tabs or splits yet.
- `shell` accepts only the four built-in names. Custom commands, WSL and ssh
  profiles are not supported yet.
- There's no CJK or colour-emoji fallback beyond the built-in fonts.
- Font size and DPI are fixed at startup. Moving the window to a monitor with
  a different DPI doesn't rescale the text.
- No colour scheme configuration, no scrollback search, no clickable URLs or
  OSC 8 hyperlinks, and the bell is ignored.
- A very large paste blocks the UI while it is written.

## Development

```powershell
build\llvm-mingw\termica.exe --selftest   # 204 headless checks, no window, no input
```

There is also an AddressSanitizer + UBSan build:

```powershell
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang `
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ `
  -DTERMICA_SANITIZE=ON
cmake --build build/asan
build\asan\termica.exe --selftest
```

- The project's code is strict C99 and must build cleanly under
  `-Wall -Wextra -pedantic`.
- [DESIGN.md](DESIGN.md) explains the architecture, the threading rules and
  why things are the way they are. Read it before changing `pty.c`, `term.c`
  or the input path.
- If the window comes up opaque, force a renderer:
  `$env:SDL_RENDER_DRIVER = 'direct3d11'`.

| Document | Contents |
|---|---|
| [PLAN.md](PLAN.md) | Roadmap: v0.1 → v1.0 stages, scope and exit criteria |
| [DESIGN.md](DESIGN.md) | Architecture, data flow, threading, and the decision records |
| [docs/archive/](docs/archive/) | The original per-milestone development diary |

## Acknowledgements

- [libghostty-vt](https://lib.ghostty.org/vt/) is the terminal emulation core
- [SDL3](https://libsdl.org) and [SDL3_ttf](https://github.com/libsdl-org/SDL_ttf) provide the window, input and text rendering
- [tomlc17](https://github.com/cktan/tomlc17) parses the TOML (MIT)
- [Maple Mono NF](https://github.com/subframe7536/maple-font) is the built-in font (SIL OFL 1.1)
- [Symbols Nerd Font Mono](https://www.nerdfonts.com) is the icon fallback (MIT)

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full notices.

## License

[MIT](LICENSE) © 2026 Peniel Ruiz
