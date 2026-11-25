<div align="center">

# termica

**A native terminal emulator for Windows, Linux and macOS, built on [libghostty-vt](https://lib.ghostty.org/vt/) and SDL3.**

![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-0078D4)
![Language](https://img.shields.io/badge/language-C99-555)
![Status](https://img.shields.io/badge/status-pre--alpha-orange)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

> [!WARNING]
> termica is **pre-alpha**. See [PLAN.md](PLAN.md) for the roadmap and the
> remaining hardware verification and packaging work.

## Why termica

- **Correct emulation, without writing it ourselves.** Parsing, screen state,
  scrollback, reflow, key and mouse encoding all come from libghostty-vt,
  the same VT core that Ghostty uses.
- **Native shell transport.** ConPTY on Windows; a POSIX pseudoterminal on
  Linux and macOS. Tabs own their processes and close them on exit.
- **Works out of the box.** Maple Mono NF and Symbols Nerd Font faces are
  built in, so prompt glyphs and icons render without installing fonts.
- **Small and readable.** Strict C99 with a headless self-test suite.

## Features

| Area | What works today |
|---|---|
| **Shell** | Windows `pwsh` → `powershell` → `cmd` auto-detection; on Unix, `$SHELL` or `/bin/sh`. Custom argv, resize and per-tab process cleanup |
| **Rendering** | 24-bit colour, bold, italic, dim, inverse, strikethrough and overline, five underline kinds (single, double, curly, dotted, dashed), all cursor styles plus blinking, wide characters, and configurable programming ligatures through HarfBuzz-shaped runs |
| **Images** | Kitty graphics parsing and drawing; POSIX PTYs forward APC sequences. Windows ConPTY consumes APC; Sixel remains unsupported |
| **Scrollback** | Mouse wheel, keyboard scrolling, a draggable scrollbar, configurable history, and search with case and regex toggles |
| **Selection** | Cell, word, line and block (Alt) selection. The selection follows its text through output and reflow, autoscrolls at the edges, and can copy on select |
| **Mouse** | Mouse reporting (modes 9/1000/1002/1003, SGR/UTF-8/urxvt/pixel formats), Shift to take the pointer back, OSC 22 pointer shape |
| **Clipboard** | Bracketed paste, unsafe-paste confirmation, and OSC 52 reads and writes with an `ask`/`allow`/`deny` policy (default `deny`) |
| **Input** | Alt keys reach the shell outside search, focus reporting (mode 1004), IME preedit drawn at the cursor |
| **Window** | `rounded`, `square`, `compact` and `native` frames, an optional drawn title bar and adjustable opacity; Windows also supports Mica, Acrylic and Liquid Glass backdrops |
| **Tabs** | Multiple independent sessions, a tab strip and keyboard and mouse controls |
| **Links** | OSC 8 and detected URLs, underlined on hover and opened with Ctrl+click for allowed schemes |
| **Desktop** | Policy-gated notifications on Windows, Linux and macOS; taskbar progress on Windows and macOS |
| **Status bar** | Starship-style `statusbar.toml`: directory, git branch and status, last command's exit code and duration, a busy spinner, a clock |
| **Diagnostics** | Version information, a local log and an unhandled-exception minidump on Windows |
| **Config** | Validated TOML, live reload, and targeted UI saves that preserve comments |

## Requirements

- **Windows:** Windows 10 1809 or later; Mica and Acrylic require Windows 11
  22H2. Build with CMake ≥ 3.21, Ninja, Git and llvm-mingw.
- **Linux:** a desktop with X11 or Wayland, a C/C++ toolchain, CMake ≥ 3.21,
  Ninja, Git, Fontconfig, GIO and libpng development headers. SDL's X11 or
  Wayland development dependencies are required for the corresponding backend.
- **macOS:** Xcode command-line tools, CMake ≥ 3.21, Ninja, Git and libpng.
- **Unix libghostty-vt:** install the pinned library, or put Zig 0.17 on PATH;
  CMake downloads and builds the pinned source release when not installed.

## Quick start

```powershell
# 1. Toolchain (Scoop)
scoop install cmake ninja git mingw-mstorsjo-llvm-ucrt

# 2. Build: the first configure downloads SDL3, SDL3_ttf, tomlc17, PCRE2 and
#    the prebuilt libghostty-vt, so allow a few minutes
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

The build produces `termica.exe` and the adjacent `ghostty-vt.dll`.

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

### Linux and macOS

Build with the native compiler and Zig 0.17 available on PATH:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
./build/native/termica --selftest     # Linux
./build/native/termica                # Linux
./build/native/termica.app/Contents/MacOS/termica --selftest  # macOS
open build/native/termica.app          # macOS
```

On Linux, install your distribution's development packages for Fontconfig,
GIO, libpng, X11 or Wayland, and OpenGL before configuring. On macOS, install
libpng and build the app bundle; notifications may require OS permission.
An installed libghostty-vt can instead be selected with
`-DCMAKE_PREFIX_PATH=/path/to/prefix`.

From Windows PowerShell, launch a Linux build through WSLg with:

```powershell
pwsh -File ./run.ps1 -Wsl
pwsh -File ./run.ps1 -Wsl -WslDistribution Ubuntu -SelfTest
```

`-Wsl` runs a Linux executable, not the Windows build. If the binary is
missing, open WSL in the checkout and build it there:

```powershell
wsl.exe --cd $PWD.Path
# In the WSL shell, after installing the Linux build prerequisites:
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
exit
```

The launcher uses the default distribution unless `-WslDistribution` is
given. Use `-Preset` to select another `build/` subdirectory, or `-WslBinary`
to run a Linux executable outside the checkout. `-Font` in WSL mode takes a
Linux font path or installed family name. On WSLg, the launcher disables SDL's
X11 RandR probe when `SDL_VIDEO_X11_XRANDR` is unset, avoiding broken RandR
outputs; an explicit environment setting is preserved.

### Command line

```text
termica.exe                          # use termica.toml
termica.exe 18                       # point-size override for this run
termica.exe C:\path\to\Font.ttf 16   # face and size override for this run
termica.exe --selftest               # headless test suite; exit code = failures
termica.exe --version                # build revision and loaded libghostty version
```

`run.ps1` accepts `-Font`, `-Size`, `-Preset`, `-SelfTest`, `-Wsl`,
`-WslDistribution` and `-WslBinary`.

## Key bindings

Most application bindings use **Ctrl+Shift**; search navigation and font
zoom also have the shortcuts below.

| Keys | Action |
|---|---|
| `Ctrl+Shift+C` | Copy the selection |
| `Ctrl+Shift+V` | Paste (bracketed when the program enables it; `Ctrl+V` reaches the shell) |
| `Ctrl+Shift+T` / `Ctrl+Shift+W` | Open a new tab / close the active tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous tab |
| `Ctrl+Shift+F` | Open scrollback search |
| `Enter` / `F3`, `Shift+Enter` / `Shift+F3` | Next / previous search match |
| `Alt+C` / `Alt+R` | Toggle case sensitivity / regular expressions in search |
| `Escape` | Close search |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | Font zoom in / out / reset |
| `Shift+PgUp` / `Shift+PgDn` | Scroll one page |
| `Ctrl+Shift+Up` / `Down` | Scroll one row |
| `Ctrl+Shift+Home` / `End` | Scroll to the top / back to the live view |
| `Ctrl+Shift+=` / `Ctrl+Shift+-` | Panel opacity up / down |
| `Ctrl+Shift+B` | Cycle the backdrop: panel → Mica → Mica Alt → Acrylic → Glass → Glass Dark |
| `Ctrl+Shift+Q` | Quit |

Bindings are configurable in `[keys]` inside `termica.toml` (for example,
`"Ctrl+V" = "paste"` to restore `Ctrl+V` paste, or `"Ctrl+Shift+Q" = "none"`
to disable a binding).

| Mouse | Action |
|---|---|
| Drag / double-click / triple-click | Select cells / a word / a line |
| `Alt` + drag | Block selection |
| `Shift` + drag or wheel | Take the pointer back from a program that is tracking the mouse |
| `Ctrl` + click | Open an OSC 8 or detected HTTP, HTTPS or mailto link |
| Wheel | Scroll the scrollback by 3 rows (or report the wheel to the program) |
| Scrollbar | Drag the thumb, or click the track to page |

## Configuration

Settings live in `%APPDATA%\termica\termica.toml`. On other platforms they are
in `$XDG_CONFIG_HOME/termica/` or `~/.config/termica/`. Both configuration
files reload while termica runs. Command-line font and size arguments take
precedence for that run.

```toml
[font]
face = ""              # "" = built-in Maple Mono NF, a .ttf path, or an installed family name
size = 11.0            # > 0, <= 200
ligatures = true       # shape adjacent ASCII cells with the font's ligatures
# fallback = ["Segoe UI Emoji", "Yu Gothic"]

[window]
opacity = 1.0          # 0..1
backdrop = "mica"      # panel | mica | mica_alt | acrylic | glass | glass_dark
decoration = "native"  # rounded | square | compact | native
title_bar = true       # drawn title bar (ignored with "native")

[terminal]
copy_on_select = false
shell = "auto"         # auto | pwsh | powershell | cmd
# command = ["wsl.exe", "-d", "Ubuntu"]
# cwd = 'C:\work'
osc52 = "deny"         # ask | allow | deny, for clipboard reads and writes
notifications = "deny" # ask | allow | deny, for OSC 9/777 notifications
scrollback_lines = 10000 # 0..1000000, retained in page-sized increments
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

- **Types are strict.** `size = "16"` is rejected, not converted. Invalid live
  edits keep the previous configuration and show an in-window message.
- **A syntax error rejects the whole file.** Unknown keys remain compatible.
- **Key names fold case and separators.** `CopyOnSelect`, `copy-on-select` and
  `copy_on_select` are the same key.
- **Windows paths** need escaped backslashes (`"C:\\Fonts\\x.ttf"`) or a
  literal string (`'C:\Fonts\x.ttf'`).
- **Font family names** are resolved through the Windows font registry,
  Fontconfig on Linux or CoreText on macOS, with matching faces and fallbacks.
- **UI edits** save only the changed key in place, preserving other settings
  and comments. Closing the window does not rewrite either configuration file.

See the generated [configuration and key binding reference](docs/configuration.md)
for every setting, module and action.

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

For integrations that report prompt and command-end markers but omit the
command-start marker, the duration is estimated from the last unmodified
Enter at the prompt. It includes shell and prompt overhead and may differ
from Starship's execution time. Without either start marker or Enter, the
duration is hidden rather than reused from a previous command.

## Known limitations

- Native splits and session restoration are not implemented.
- Image delivery is blocked on Windows: ConPTY consumes APC sequences, which
  is how the Kitty graphics protocol transports images. termica parses, stores
  and draws Kitty images (covered by `--selftest`), but a program running in a
  tab cannot get an image through the pty until the transport forwards APC.
  Sixel is not supported either.
- Windows-specific backdrop materials fall back to the drawn panel on Linux
  and macOS; native compositor blur is not implemented.
- Windows may suppress notifications according to its notification settings
  or Focus Assist.
- Linux taskbar progress has no standard freedesktop protocol. Notifications
  require a running session D-Bus notification service; URLs use the desktop
  opener. macOS notifications require OS authorization and the app bundle.
- The macOS backend has not yet been validated on native macOS hardware.
- Windows minidumps can contain private terminal text, process memory and paths.
  See [troubleshooting](docs/troubleshooting.md#crash-reports-and-privacy)
  before sharing logs or dumps. Set `TERMICA_DIAGNOSTICS=0` to disable them.

## Development

```powershell
build\llvm-mingw\termica.exe --selftest   # headless checks, no window or input
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
- For release signing prerequisites and the fail-closed signing command, see
  [signing](docs/signing.md). A trusted certificate does not guarantee
  SmartScreen reputation.
- If the window comes up opaque, force a renderer:
  `$env:SDL_RENDER_DRIVER = 'direct3d11'`.

| Document | Contents |
|---|---|
| [PLAN.md](PLAN.md) | Roadmap: v0.1 → v1.0 stages, scope and exit criteria |
| [DESIGN.md](DESIGN.md) | Architecture, data flow, threading, and the decision records |
| [docs/configuration.md](docs/configuration.md) | Generated configuration and key binding reference |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Troubleshooting and diagnostics privacy |
| [docs/signing.md](docs/signing.md) | Authenticode signing prerequisites and verification |
| [docs/perf.md](docs/perf.md) | Throughput measurements and benchmark limitations |
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
