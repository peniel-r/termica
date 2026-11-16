# termica plan

Milestone 2 (a real shell) is done and committed; milestone 3 (pointer,
history, focus) is the section below.

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

## Non-goals (milestone 2)

Nothing here anymore: pointer, scrollback UI, selection and copy, focus
reporting and the embedded font are done in milestones 2 and 3.

---

# Milestone 3: pointer, history and focus

## Goal

A shell program can ask for the mouse and get it; the user can read what
scrolled away, select it with the pointer and copy it; and programs that
watch focus (vim, PSReadLine) learn when the window is focused. All four
features already exist inside libghostty-vt — this milestone is embedder
wiring, not emulation.

## Design

| Feature | Library API | Embedder work |
|---|---|---|
| mouse reporting | `ghostty_mouse_encoder_*`, `GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING` | feed surface-space pixel events, write the encoded bytes, keep the encoder geometry in step with the grid |
| scrollback UI | `ghostty_terminal_scroll_viewport`, `GHOSTTY_TERMINAL_DATA_SCROLLBAR`, `DATA_VIEWPORT_ACTIVE` | wheel, Shift+arrows/PgUp/PgDn/Home/End, a thumb on the right edge, jump to bottom on input |
| selection + copy | `ghostty_terminal_grid_ref`, `select_word`, `select_word_between`, `select_line`, `OPT_SELECTION`, `DATA_SELECTION`, `RENDER_STATE_ROW_DATA_SELECTION`, `FORMATTER_OPT_SELECTION` | click/drag gestures, Shift to override reporting, block select with Alt, highlight by swapping colors, Ctrl+Shift+C to the clipboard |
| focus events | `ghostty_focus_encode`, mode 1004 via `GHOSTTY_TERMINAL_DATA_MODE` | SDL focus events → bytes to the pty |

Ownership: everything above the "embedder work" line goes in `term.{h,c}`;
`main.c` keeps only input routing and drawing.

## Decisions

- **Coordinates are viewport cells.** A selection or a wheel gesture is
  expressed with `GHOSTTY_POINT_TAG_VIEWPORT`, so it keeps pointing at the
  same text while scrolled into history.
- **The pointer does two jobs.** When a program enables mouse tracking,
  pointer events go to it; holding **Shift** takes them back for
  selection/scrolling (the usual terminal convention). When no program is
  listening, the wheel scrolls the viewport.
- **Gestures by hand, the library does the semantics.** Click count comes
  from SDL (1 cell, 2 word, 3 line); the ranges, word boundaries and line
  extents come from `ghostty_terminal_select_*`. Alt+drag is a rectangular
  selection; dragging a word selection uses `select_word_between` so it
  does not collapse when the pointer is between words.
- **A plain click clears the selection;** a click that never moved and a
  release outside the grid also clear it. Copy is explicit
  (`Ctrl+Shift+C`), no copy-on-select.
- **Selection highlight = swapped colors** (like inverse video), so no new
  color is invented and it works over any background.
- **Scroll step**: 3 rows per wheel notch, one page per PgUp/PgDn, one row
  per Shift+Up/Down. Any key or paste that reaches the program first jumps
  to the bottom.
- **Reporting is silent outside the grid.** The drag strip and the resize
  borders stay app chrome; only coordinates inside the cell area produce
  mouse events.
- **Keys**: `Shift+PgUp/PgDn/Home/End` and `Shift+Up/Down` scroll,
  `Ctrl+Shift+C` copies. Everything else is unchanged; the shell still gets
  Esc and plain arrows.

## Work items

1. `PLAN.md` (this section).
2. `src/term.{h,c}`: geometry, mouse encoder, tracking/focus/mode queries,
   scroll + scrollbar + viewport state, selection setters, selection text.
3. `src/style.{h,c}`: a selected cell swaps its colors.
4. `src/main.c`: mouse and wheel routing, gesture state, scroll keys,
   `Ctrl+Shift+C`, focus events, selection highlight, scrollbar drawing,
   encoder geometry from `fit_grid`.
5. `src/selftest.c`: scrollback + selection + copy round trip, focus and
   mouse encoders against a terminal that enabled the modes.
6. Build, `--selftest`, GUI checks: `vim`/`less` mouse, wheel scrollback,
   drag/double/triple select, copy into pwsh, focus reporting.
7. README + `PLAN.md` status; commit locally, no push without authorization.

## Risks

- **Selection refs are snapshots**, valid only until the next mutating call
  on the terminal. Every gesture therefore rebuilds the selection from
  fresh viewport points; nothing is cached across a `term_write`.
- **Wheel under mouse tracking** must reach the program, not the viewport;
  if the encoder emits nothing for a wheel event, the app scrolls instead —
  that is also what makes scrolling work in a program that enabled only
  button events.
- **Continuous output while a selection is held** may or may not keep
  pointing at the same text; checked by hand, and the selection is cleared
  on new output if it visibly drifts. *(Result: it stays; nothing clears on
  output.)*
- **SDL reports mouse positions in window coordinates**; the layout is in
  pixels. Verified at a non-1.0 display scale before trusting it.

## Outcome (milestone 3, done)

Implemented as planned, with three findings worth keeping in the repo:

- **Modifiers are per side in SDL3.** `KMOD_SHIFT` and `KMOD_CTRL` are unions of
  two bits (LSHIFT|RSHIFT, LCTRL|RCTRL), so `(mod & (SHIFT|CTRL)) == (SHIFT|CTRL)`
  can never be true. That made the milestone-2 `Ctrl+Shift+Q` binding dead and it
  is now tested per union. Bindings also match **scancodes**, not keycodes: the
  keycode for `Shift+C` is not `SDLK_C`.
- **The child environment block was malformed**, which is what broke ConPTY
  spawning outright (`CreateProcessW` failed with error 87). An appended entry
  needs room for its own NUL *and* the block needs its final NUL; the earlier
  two-pass version accidentally did this, the rewrite dropped one NUL. Caught by
  `--selftest`'s pty round trip, which is now the guard for it.
- **ConPTY reports a mode change, not the sequences a client wrote.** Escape
  bytes written by a child to its own stdout go through conhost's console
  output, so a script cannot simply `type` mouse-mode sequences; a program that
  uses the console input API (`fzf`) does enable reporting, and that is how the
  pointer handoff was verified.

Verification (all green):

- `--selftest`: 33 checks, 0 failures, ~2 s (scrolling by rows and to the ends,
  scrollbar figures, cell/word/line/block selections and their copied text, SGR
  press/release encoding, motion not reported under mode 1000 but reported under
  1003, `CSI I` / `CSI O`, ConPTY round trip).
- GUI with the built-in font: wheel up into history shows the thumb on the right
  edge (152 bright pixels sampled in that column) and returns to the live view;
  drag/double/triple select and highlight by swapped colors; `Ctrl+Shift+C` puts
  the listing text on the clipboard; with fzf holding the pointer a click moves
  fzf's selection and a plain drag no longer selects, while Shift+drag does.

## Non-goals (milestone 4)

- OSC 8 hyperlinks, kitty graphics, OSC 52 clipboard writes, desktop
  notifications and progress reporting (`GHOSTTY_TERMINAL_OPT_*` all exist).
- `GhosttyMouseShape` mapped to the real cursor, and selection autoscroll while
  dragging near an edge (the selection gesture API exposes an autoscroll request;
  the app builds selections from grid refs directly, so it would need the
  gesture objects to get that for free).
- IME composition placement at the cursor (`SDL_SetTextInputArea`), and per-glyph
  fallback to a system CJK/emoji font.
- Per-monitor DPI rescaling after startup.
- Selection across reflow: ends are untracked snapshots today, so a resize
  clears the selection rather than following the text
  (`ghostty_tracked_grid_ref_*` is the way to change that).
