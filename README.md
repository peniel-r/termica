# termica: milestone 3

A tiny SDL3 terminal that answers four questions:

1. **Does the stack produce the borderless, translucent, Nerd-Font look from your WezTerm
   screenshot?** (milestone 0 - yes)
2. **Can libghostty-vt drive a real terminal grid behind that look?** (milestone 1 - yes)
3. **Can it run a real shell?** (milestone 2 - yes)
4. **Can a program use the pointer, and can the reader reach what scrolled away?**
   (milestone 3 - yes: mouse reporting, scrollback, selection and copy, focus events)

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
process tree.

| Key | Action |
|---|---|
| typing, arrows, Home/End/PgUp/PgDn, F1-F12 | encoded by libghostty-vt and written to the shell |
| Enter / Backspace / Esc | go to the shell; the shell echoes them, not the app |
| Ctrl+V or Ctrl+Shift+V | paste; bracketed paste when the program enabled it |
| wheel | scrollback by 3 rows per notch, unless the program asked for the mouse |
| Shift + wheel or drag | take the pointer back from the program, for scrolling and selecting |
| Shift+PgUp / Shift+PgDn | scroll a full page |
| Ctrl+Shift+Up / Down, Home, End | scroll one row, to the top, to the bottom |
| drag / double click / triple click | select cells / a word / a line; Alt+drag is a block |
| Ctrl+Shift+C | copy the selection to the clipboard |
| Alt+Up / Alt+Down | panel opacity (text must stay fully opaque) |
| Alt+M | cycle the backdrop: panel, Mica, Mica Alt, Acrylic |
| Ctrl+Shift+Q | quit |

Mouse, history and focus (milestone 3): a program that asks for the pointer
(modes 9, 1000, 1002, 1003, with 1005/1006/1015/1016 formats) gets encoded
pointer events, `fzf` included. Output that scrolls off the top stays
reachable in scrollback with a thumb on the right edge; selecting text
highlights it by swapping its colors and copies with `Ctrl+Shift+C`; and
programs that enabled focus reporting (mode 1004) learn when the window comes
and goes.

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

## 2. Fonts: Google Sans Code NF is built in

The executable carries **Google Sans Code Nerd Font Mono** - regular, bold, italic and
bold-italic, Nerd Fonts release 3.5.1, SIL OFL 1.1 - linked in as Windows resources. There
is nothing to install and no font path to pass: prompt symbols, box drawing and Nerd icons
render out of the box. The program prints `font: Google Sans Code NF (built in)`.

A path on the command line still wins:

```
build\llvm-mingw\termica.exe "C:\path\to\AnotherNerdFontMono-Regular.ttf" 16
```

If that file cannot be opened, the built-in family is used instead. With a font from disk
that lacks a codepoint (Consolas has no Nerd icons), the built-in regular face draws that
glyph, so icons keep working. There is no further fallback: CJK or emoji that neither face
has are still blank.

For a font from disk the program looks for matching bold, italic and bold-italic faces next
to the regular one (`consola.ttf` -> `consolab.ttf`, `... Nerd Font.ttf` -> `... Nerd Font
Bold.ttf`), and fakes bold by drawing the glyph twice when there is no bold face. The
built-in family has all four real faces.

The TTFs live in `assets/fonts/` with their `OFL.txt`; CMake embeds them through
`src/fonts.rc.in`. That is where roughly 10 MB of the executable's size goes.

## 3. Build and run

```
cmake --preset llvm-mingw
cmake --build --preset llvm-mingw
build\llvm-mingw\termica.exe              # built-in font, 16 pt
build\llvm-mingw\termica.exe 18           # point size only
build\llvm-mingw\termica.exe C:\path\to\Font.ttf 16
```

Or, from PowerShell 7 / WezTerm:

```
pwsh -File ./build.ps1
pwsh -File ./run.ps1
```

The first configure downloads and builds SDL3 and SDL3_ttf, and downloads the
libghostty-vt zip, so expect a few minutes. After that rebuilds take seconds.

Other presets: `llvm-mingw-debug`, and `gcc` for any gcc already on `PATH` (Scoop's `mingw`, WinLibs).

## 4. Testing: the self test and a sanitizer run

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
and focus encoders against a terminal that enabled the modes). The last check starts a real
ConPTY, runs `cmd.exe /c "echo termica-pty-ok"`, and verifies the text came back through the
pipes and the exit code is 0. Exit code is the number of failures.

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

## 5. What to check (this is the experiment)

1. **Transparency.** Is the wallpaper visible behind the panel, while the text stays crisp and opaque?
   Press Alt+Up/Down: the panel should fade, the text should not.
2. **Corners.** The window should have macOS-style corners: 10 pt, drawn as a squircle (continuous
   curvature) and antialiased, with the border following the curve.
3. **Backdrops.** Alt+M cycles panel -> Mica -> Mica Alt -> Acrylic. The status line names the
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

## 6. Window chrome: the corners and the backdrops

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

- **The system caption buttons come with the extended frame.** This window has its own drag strip
  and no buttons, so `WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX` are dropped while Mica is on
  and restored when it goes off.
- **The backdrop covers the whole window rectangle, corners included.** A drawn mask cannot cut
  into it, so in Mica modes the window is shaped with `SetWindowRgn`, using the same squircle
  sampled into a polygon. Two costs: regions are not antialiased, so those corners are hard
  edged, and clicks outside the shape go through to the desktop instead of hitting the window.

On Windows 10 and the first Windows 11 release the attribute is rejected (`DwmSetWindowAttribute`
fails), and the app falls back to the panel. It was verified on Windows 11 25H2 (build 26200):
the material follows the wallpaper - set the wallpaper to red and the window turns red - and
Mica Alt is visibly different from Mica.

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
  `select_line`, the copy goes through a formatter restricted to that selection, and the
  viewport moves with `ghostty_terminal_scroll_viewport` (`GHOSTTY_TERMINAL_DATA_SCROLLBAR`
  drives the thumb)

## Where the code lives

| file | what it does |
|---|---|
| `src/main.c` | the app: event loop, keyboard and paste, spawning and draining the shell, one frame |
| `src/term.{h,c}` | the libghostty-vt wrapper: screen, render state, key encoder, replies, paste |
| `src/pty.{h,c}` | ConPTY: pipes, the child process, the reader thread, resize, teardown |
| `src/embed.{h,c}` | the Google Sans Code NF bytes linked into the executable |
| `src/fonts.rc.in` | the Windows resource script that carries those bytes |
| `src/glyph.{h,c}` | fonts, the glyph cache, drawing a cell's text (bold/italic faces) |
| `src/style.{h,c}` | a terminal cell style -> colors and decorations for the renderer |
| `src/panel.{h,c}` | the rounded translucent panel and its antialiased corner masks |
| `src/geometry.{h,c}` | the squircle curve: pure math, shared by the panel and the region |
| `src/window.{h,c}` | the SDL window, the drag/resize hit test, the Windows 11 chrome |
| `src/selftest.c` | `--selftest`, including the ConPTY round trip |

`geometry.c` and `style.c` have no SDL state and no Windows calls, which is what makes them
easy to reason about on their own.

## Known limits of this milestone

- **ConPTY only.** The shell needs Windows 10 1809 or later. `pty.c` has a stub for other
  platforms, so the app still builds there, but no shell starts.
- Glyph cache covers the Basic Multilingual Plane. The fallback covers only the built-in
  face: a character neither the chosen font nor Google Sans Code NF has (most CJK, colour
  emoji) is blank.
- One font scale chosen at startup. Moving the window between monitors with different DPI
  will not rescale (the corner masks would need a rebuild too).
- The Mica/Acrylic backdrops need Windows 11 22H2 or later; below that the app stays on the
  panel. With a backdrop on, the corners are a window region and therefore hard edged, and the
  click-through outside the region means the very corners cannot start a resize.
- macOS also draws a shadow that follows the window shape; DWM draws its own, so the shadow is
  Windows' and is not part of this.
- No hyperlinks (OSC 8), no kitty graphics, no OSC 52 clipboard writes, no desktop
  notifications or progress reporting, and no IME composition window placed at the cursor.
  Mouse *shapes* (`GhosttyMouseShape`) are not applied either: the pointer stays an arrow.
- A very large paste blocks the UI thread while it is written to the console; the pipe is
  synchronous and there is no writer queue yet.
- **Verified on Windows 11 25H2 (build 26200)**: llvm-mingw clang 23.1.5 + lld + Ninja,
  `direct3d11` renderer, translucent panel, built-in Google Sans Code NF icons, a live
  `pwsh` 7 / `cmd` shell in ConPTY (a typed command and its output appear on the grid, the
  window title comes from the shell's OSC sequence), wheel and `Ctrl+Shift+*` scrolling with
  the thumb drawn, drag/word/line selection copied to the clipboard, `fzf` taking the pointer
  when it asks for it, and closing the window while the shell runs leaves no `conhost.exe` or
  shell behind. `--selftest` runs 33 checks (including the ConPTY round trip) with no
  warnings from the project's own sources under `-Wall -Wextra -pedantic`.
- SDL3 enables C++ on Windows for its own sources, which is why the presets also set a C++
  compiler. Your own code stays C99.
