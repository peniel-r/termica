# Building `termica` on Windows (PowerShell 7 + Scoop)

The Windows-native build path. No MSYS2, no mintty, no POSIX layer, no
compatibility shim - every tool is a native Win32 executable installed with
Scoop (or winget) and driven from your whatever terminal emulator you have.

Everything below was executed and verified on this machine.

---

## 0. The short version

```powershell
scoop install cmake ninja git mingw-mstorsjo-llvm-ucrt
pwsh -File ./build.ps1             # configure + build
pwsh -File ./run.ps1               # launch it
```

---

## 1. The toolchain

| Tool | Scoop package | winget equivalent | Provides |
|---|---|---|---|
| CMake | `cmake` | `Kitware.CMake` | the build system driver |
| Ninja | `ninja` | `Ninja-build.Ninja` | the build backend |
| git | `git` | `Git.Git` | `FetchContent` clones SDL3 with it |
| **llvm-mingw** | `mingw-mstorsjo-llvm-ucrt` | *(no official id; grab the release zip from github.com/mstorsjo/llvm-mingw)* | **clang 23 + lld + MinGW-w64 headers + llvm-windres + llvm-ar** |
| Zig | `zig` | `zig.zig` | only needed later, for `libghostty-vt` |

```powershell
scoop install cmake ninja git mingw-mstorsjo-llvm-ucrt
```

### Why llvm-mingw and not plain LLVM

`scoop install llvm` gives you clang, but **no MinGW-w64 headers and no
sysroot**, so it cannot target Windows on its own - it needs MSVC or a mingw
sysroot. `mingw-mstorsjo-llvm-ucrt` is llvm-mingw: upstream LLVM plus the
MinGW-w64 CRT, headers and import libraries, in one self-contained package.
It ships the prefixed drivers the CMake presets ask for:

```
x86_64-w64-mingw32-clang.exe
x86_64-w64-mingw32-clang++.exe
x86_64-w64-mingw32-windres.exe
```

Check it:

```powershell
> x86_64-w64-mingw32-clang --version
clang version 23.1.5 (https://github.com/llvm/llvm-project.git 392dfff97d29b59d77e5cac6d4b6ef2585fe7117)
Target: x86_64-w64-windows-gnu
Thread model: posix
InstalledDir: C:/Users/i_rac/scoop/apps/mingw-mstorsjo-llvm-ucrt/23.1.5-20261104/bin
```

---

## 2. Build

```powershell
pwsh -File ./build.ps1
```

```
==> Tooling
  cmake  cmake version 4.5.0
  ninja  1.13.2
  git    git version 2.56.0.windows.4
  clang  clang version 23.1.5 (https://github.com/llvm/llvm-project.git ...)
  clang  target: x86_64-w64-windows-gnu
==> Configure (llvm-mingw)
...
==> Build (llvm-mingw)
...
==> Done
  C:\sandbox\termica\build\llvm-mingw\termica.exe  (5.93 MB)
```

Or without the wrapper:

```powershell
cmake --preset llvm-mingw
cmake --build --preset llvm-mingw
```

The first configure downloads and builds SDL3, SDL3_ttf, FreeType and
HarfBuzz, so expect several minutes. Rebuilds after that take seconds.

`build.ps1` merges the persisted machine/user PATH into the session first, so
a Scoop package installed a moment ago is picked up without restarting the
terminal.

---

## 3. Run

```powershell
pwsh -File ./run.ps1
```

`run.ps1` passes no font by default: the exe carries Maple Mono NF and
uses it, so nothing has to be installed. Pass `-Font` to use a
different `.ttf`.

Because the program is a console-subsystem app launched from an existing
terminal, it **inherits your current terminal emulator tab** - no second console window pops up,
and the renderer/cell-size output lands inline in your prompt.

```
renderer: direct3d11
font: Maple Mono NF (built in)  cell: 9x22 px  scale: 1.00  bold: yes  italic: yes
```

Override the font or the size:

```powershell
pwsh -File ./run.ps1 -Size 18
pwsh -File ./run.ps1 -Font C:\Windows\Fonts\consola.ttf -Size 18
```

---

## 4. Presets

| Preset | Toolchain |
|---|---|
| `llvm-mingw` | llvm-mingw (clang 23 + lld), release - **the default** |
| `llvm-mingw-debug` | same, `Debug` |
| `gcc` | any `gcc` on `PATH` (e.g. `scoop install mingw`, WinLibs) |

```powershell
pwsh -File ./build.ps1 -Preset llvm-mingw-debug
pwsh -File ./run.ps1    -Preset llvm-mingw-debug
```

---

## 5. The binary is standalone

The build links only against Windows system DLLs:

```
DLL Name: KERNEL32.dll   DLL Name: USER32.dll   DLL Name: GDI32.dll
DLL Name: dwmapi.dll     DLL Name: ole32.dll    DLL Name: SETUPAPI.dll
DLL Name: api-ms-win-crt-*.dll  (resolved through the API-set schema)
```

So `termica.exe` runs anywhere on Windows, with no toolchain installed.

---

## 6. Gotchas

* **PowerShell 7.4+ turns native-command stderr into error records**, so
  third-party build warnings (HarfBuzz emits a lot of them) would abort a
  script that sets `$ErrorActionPreference = 'Stop'`. `build.ps1` sets
  `$PSNativeCommandUseErrorActionPreference = $false` for that reason.
* **`git` must be reachable** - `FetchContent` clones SDL3 and SDL3_ttf with
  it.
* **SDL3 compiles C++ on Windows**, so a C++ compiler is required even though
  the code is strict C99. The presets set `CMAKE_CXX_COMPILER`.
* **`SDL_RENDER_DRIVER`**: force a renderer with
  `$env:SDL_RENDER_DRIVER='direct3d11'` if the window comes up opaque.
