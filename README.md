# termica: milestone 0

A tiny SDL3 program that answers one question before you write any terminal code:
**can this stack produce the borderless, translucent, Nerd-Font look from your WezTerm screenshot?**

What it draws: a borderless window with a translucent dark panel, opaque text on a fixed
cell grid, Nerd Font icons, a block cursor and a right-aligned clock, laid out like the screenshot.
Text goes through a one-texture-per-codepoint glyph cache, which is the same technique the real
renderer will use. Drag the top strip to move the window, drag the edges to resize.

| Key | Action |
|---|---|
| Up / Down | background opacity (text must stay fully opaque) |
| B | toggle Windows acrylic backdrop (experimental) |
| Esc | quit |

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

**Zig is not needed yet.** It only comes in when you build libghostty-vt in a later milestone.

## 2. Get a Nerd Font

The icons are private-use-area glyphs, so you need a font that contains them. Use a **"Nerd Font Mono"**
variant (single-width icons), for example JetBrainsMono Nerd Font Mono or Hack Nerd Font Mono, from
nerdfonts.com. Note the path of the `.ttf` file. (Fonts installed for your user usually live in
`%LOCALAPPDATA%\Microsoft\Windows\Fonts`.)

Without one, the program falls back to Consolas and shows an orange warning that icons are missing.

## 3. Build and run

```
cmake --preset llvm-mingw
cmake --build --preset llvm-mingw
build\llvm-mingw\termica.exe "C:\path\to\JetBrainsMonoNerdFontMono-Regular.ttf" 16
```

Or, from PowerShell 7 / WezTerm:

```
pwsh -File ./build.ps1
pwsh -File ./run.ps1
```

The first configure downloads and builds SDL3 and SDL3_ttf, so expect a few minutes. After that
rebuilds take seconds.

Other presets: `llvm-mingw-debug`, and `gcc` for any gcc already on `PATH` (Scoop's `mingw`, WinLibs).

## 4. What to check (this is the experiment)

1. **Transparency.** Is the wallpaper visible behind the panel, while the text stays crisp and opaque?
   Press Up/Down: the panel should fade, the text should not.
2. **Corners and edges.** Rounded corners on Windows 11? Can you drag and resize?
3. **Icons.** Do the folder, home and clock glyphs appear? If not, check the font.
4. **Text quality.** Compare the text with WezTerm at the same size. Look at it on your actual display scale.
5. **Console output.** The program prints the renderer name (you want `direct3d11` or `direct3d12`),
   the cell size and whether the font has Nerd glyphs.

## If transparency does not work

Try forcing a renderer before launching: `set SDL_RENDER_DRIVER=direct3d11` (or `direct3d12`, `vulkan`)
and run again. If every renderer shows an opaque (usually black) background, that tells us SDL's
transparent window is not enough on your machine. The fallback is to keep SDL for input and the event
loop, take the `HWND`, and drive a DirectComposition swap chain with premultiplied alpha ourselves.

## Known limits of this demo

- Glyph cache covers the Basic Multilingual Plane only. Fine for ASCII, box drawing and Nerd icons.
- One font scale chosen at startup. Moving the window between monitors with different DPI will not rescale.
- No bold or italic faces, no wide-character handling, no color emoji. Those come with the real renderer.
- Not run on Windows by me. I checked that it configures, compiles and links under strict C99 with
  `-Wall -Wextra -pedantic` using a MinGW-w64 gcc cross compiler on Linux.
- **Built and run on Windows** (llvm-mingw clang 23.1.5 + lld, Ninja, from PowerShell 7 / WezTerm):
  `direct3d11` renderer, translucent panel, Nerd Font icons working, no warnings from `src/main.c`.
  See [README-powershell.md](README-powershell.md).
- SDL3 enables C++ on Windows for its own sources, which is why the presets also set a C++ compiler.
  Your own code stays C99.
