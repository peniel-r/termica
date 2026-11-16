# Milestone 2: a real shell behind the grid

## Goal

`termica.exe` starts a real interactive shell in a Windows pseudoconsole
(ConPTY) instead of echoing keystrokes into the terminal itself. Keys and
paste go to the program, its UTF-8 output drives the grid, resizing the
window resizes the console, and closing the window takes the process tree
down with it.

## Where milestone 1 stopped

The parser, screen state, render state and key encoder are libghostty-vt and
are already working. What is missing is the byte transport: no pipe, no
process, no pty. `main.c` fakes the kernel's line-discipline echo for Enter and
Backspace because nothing else would.

## Design

```
SDL key/text events -> ghostty key/paste encoder -> WriteFile
    -> ConPTY input pipe -> conhost -> cmd.exe / pwsh.exe
    -> ConPTY output pipe -> reader thread -> ring buffer -> term_write
    -> render state -> glyphs -> window
term_write replies (DA, DSR, DECRQM, XTWINOPS) -> write_pty callback -> input pipe
```

- **`src/pty.{h,c}` (new).** Owns ConPTY: two anonymous pipes,
  `CreatePseudoConsole`, `CreateProcessW` with
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, a kill-on-close job object, a reader
  thread, a bounded ring buffer with backpressure, `ResizePseudoConsole`, and
  an ordered close. No SDL dependency: the reader signals through a callback,
  so the module stays testable without a window.
- **`src/term.{h,c}`.** Install `GHOSTTY_TERMINAL_OPT_WRITE_PTY` so the
  library's replies reach the child. Add `term_paste()` on top of
  `ghostty_terminal_paste`, which frames bracketed paste and flags unsafe
  text instead of hand-rolling `\x1b[200~`.
- **`src/main.c`.** Spawn after the first grid fit (so the initial COORD is
  real), drain the ring on every wake with a per-frame cap, send key and IME
  text to the pty, resize the pty in `fit_grid`, print the exit status, and
  close the pty before the terminal.

## Threading rules

- `ghostty_terminal_vt_write` stays on the main thread. Only the main thread
  calls `term_write`, `term_paste`, `term_resize`.
- The reader thread only copies `ReadFile` bytes into the ring buffer and
  calls the notify callback. That callback only does `SDL_PushEvent`.
- `pty_read` is main-thread only. `pty_write` is main-thread only, except
  the write from the `write_pty` callback, which also runs on the main thread
  inside `term_write`.
- Backpressure: when the ring is full the reader blocks on a condition
  variable, which stalls conhost, which stalls the child. This is correct
  terminal behaviour and keeps memory bounded.

## Work items

1. `PLAN.md` (this file).
2. `src/pty.{h,c}`: spawn, resize, read, write, wait, exit code, close,
   shell discovery (`pwsh.exe` -> `powershell.exe` -> `%COMSPEC%`).
3. `CMakeLists.txt`: add `pty.c`, `pty.h`.
4. `src/term.{h,c}`: `write_pty` callback, `term_paste`.
5. `src/main.c`: spawn/drain/input/resize/shutdown, drop the echo emulation
   and the CRLF paste rewrite, move quit off Esc.
6. `src/selftest.c`: spawn `cmd.exe /c echo ...` through the pty, assert the
   text came back and the exit code is 0.
7. `README.md`: shell milestone, key table, known limits.
8. Build, run `--selftest`, interactive smoke test.
9. Local commit. **No push; wait for explicit authorization.**

## Decisions

- **Shell order:** `pwsh.exe` (PowerShell 7), then `powershell.exe`, then
  `cmd.exe`, each found with `SearchPathW`; `-NoLogo` for the PowerShell
  pair. `cmd.exe` needs no arguments to be interactive.
- **Working directory:** inherited from `termica.exe`. Launching from a
  directory and getting that directory in the shell is the least
  surprising behaviour; a profile default can come later.
- **Child standard handles:** termica is a console program, so a child would
  otherwise inherit termica's own stdin/stdout/stderr and write straight to
  the terminal that launched termica instead of into the pseudoconsole.
  The child is created with `STARTF_USESTDHANDLES` and three NULL handles,
  which makes conhost attach the pseudoconsole's console handles (verified:
  the child then sees `FILE_TYPE_CHAR` and the 80x24 buffer of the pty).
  GUI terminals never hit this because they have no console handles.
- **Environment:** the child inherits termica's environment with
  `TERM=xterm-256color` and `COLORTERM=truecolor` added when absent. Windows
  itself has no `TERM`, and prompt tools (starship) and Unix ports (git,
  vim) check it; without it they degrade to "dumb terminal" behaviour.
- **Quit:** `Ctrl+Shift+Q` and the window close button. Esc is a terminal
  key now (vim, PSReadLine), not an app binding.
- **Exit:** keep the window open and print
  `[process exited with code N]`, so the last output stays readable.
- **Process tree:** the child starts suspended, joins a
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job, then resumes. Closing the window
  cannot leave `conhost.exe` or grandchildren behind. Assigning to the job is
  best effort; closing the pseudoconsole is the primary path.
- **Unsafe paste:** `ghostty_terminal_paste` refuses text that can inject
  commands (line breaks outside bracketed paste, a paste-end sequence). The
  app then asks with a two-button message box and retries with
  `allow_unsafe = true` if the user confirms.

## Risks and how they are checked

- **ConPTY handle ownership.** The parent's copies of the two
  pseudoconsole-side pipe handles are closed after `CreatePseudoConsole`.
  If the API does not duplicate them, the pty self test fails immediately
  (no output, broken input); the fix is to keep them until close.
- **ConPTY input is a parser of its own.** Replies it does not understand can
  surface as literal text. Checked by hand with a cursor-position probe,
  PSReadLine's Tab menu and vim. If they leak, only forward replies the
  console accepts.
- **Large paste blocks the UI thread** because `WriteFile` is synchronous on
  the input pipe. Accepted for this milestone; a writer queue is the fix if a
  multi-megabyte paste stutters.
- **`cmd.exe` codepage.** `chcp 65001` then non-ASCII text is part of the
  smoke test; if it mojibakes, the launch command needs `chcp`.
- **UTF-8 split across reads.** The ring is treated as a byte stream and only
  `term_write` touches it, so a chunk boundary cannot corrupt a sequence. No
  app-level decoding is added.

## Verification

- `cmake --build --preset llvm-mingw` clean under `-Wall -Wextra -pedantic`.
- `build/llvm-mingw/termica.exe --selftest`: existing checks plus the pty
  spawn/echo/exit-code check.
- GUI smoke: `cmd.exe` and `pwsh.exe`, `dir`, `cls`, Ctrl+C during
  `ping -t`, PSReadLine Tab completion, `vim` (alt screen, arrows, Esc),
  resize while `more` runs, close the window and confirm no orphaned
  `conhost.exe` / shell in Task Manager.

## Non-goals (next milestone)

Mouse reporting, scrollback scrolling UI, selection and copy, focus events,
OSC 52 clipboard writes, IME composition window placement, font fallback,
per-monitor DPI rescaling, hyperlinks.
