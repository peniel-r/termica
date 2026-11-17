# termica plan

Status: milestones 2, 3, 4 and 5 are implemented, committed and pushed -
the code is in `05a1af1` ("milestones 4 and 5"), `bddc1d2` (milestone 3),
`2f4792e` (milestone 2) and `19c8523` (milestone 1), with the built-in font
commits interleaved; documentation commits sit on top, so `origin/main` is
ahead of the shas named here. Milestone 4 was reported as committed before it
had been, so its work and milestone 5's share one commit. Milestone 6 (TOML
settings) is implemented in the working tree; it is described at the end.
Each section keeps the design that was written before the code, with the
measured results folded into it, and closes with what actually verified -
including, where a feature could not be verified without a real keyboard,
mouse or input method, that plain statement. What is left is under
"Still not done".

---

# Milestone 2: a real shell behind the grid

## Goal

`termica.exe` starts a real interactive shell in a Windows pseudoconsole
(ConPTY) instead of echoing keystrokes into the terminal itself. Keys and
paste go to the program, its UTF-8 output drives the grid, resizing the
window resizes the console, and closing the window takes the process tree
down with it.

## Where milestone 1 stopped

The parser, screen state, render state and key encoder are libghostty-vt and
were already working. What was missing was the byte transport: no pipe, no
process, no pty. `main.c` faked the kernel's line-discipline echo for Enter
and Backspace because nothing else would.

## Design

```
SDL key/text events -> ghostty key/paste encoder -> WriteFile
    -> ConPTY input pipe -> conhost -> cmd.exe / pwsh.exe
    -> ConPTY output pipe -> reader thread -> ring buffer -> term_write
    -> render state -> glyphs -> window
term_write replies (DA, DSR, DECRQM, XTWINOPS) -> write_pty callback -> input pipe
```

- **`src/pty.{h,c}`.** Owns ConPTY: two anonymous pipes,
  `CreatePseudoConsole`, `CreateProcessW` with
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, a kill-on-close job object, a reader
  thread, a bounded ring buffer with backpressure, `ResizePseudoConsole`, and
  an ordered close. No SDL dependency: the reader signals through a callback,
  so the module stays testable without a window.
- **`src/term.{h,c}`.** `GHOSTTY_TERMINAL_OPT_WRITE_PTY` lets the library's
  replies reach the child. `term_paste()` sits on `ghostty_terminal_paste`,
  which frames bracketed paste and flags unsafe text instead of hand-rolling
  `\x1b[200~`.
- **`src/main.c`.** Spawn after the first grid fit (so the initial COORD is
  real), drain the ring on every wake, send key and IME text to the pty,
  resize the pty in `fit_grid`, report the exit status, close the pty before
  the terminal.

## Threading rules

- `ghostty_terminal_vt_write` stays on the main thread: only `main.c`'s thread
  calls `term_write`, `term_paste`, `term_resize`.
- The reader thread only copies `ReadFile` bytes into the ring buffer and
  calls the notify callback, which only does `SDL_PushEvent`.
- `pty_read` and `pty_write` are main-thread only. The `write_pty` callback
  writes too, and it always runs inside `term_write`, so on the main thread.
- Backpressure: when the ring is full the reader waits on a condition
  variable, which stalls conhost, which stalls the child. That is the correct
  terminal behaviour and it keeps memory bounded.

## Decisions

- **Shell order:** `pwsh.exe` (PowerShell 7), then `powershell.exe`, then
  `cmd.exe`, each found with `SearchPathW`; `-NoLogo` for the PowerShell pair.
  `cmd.exe` needs no arguments to be interactive.
- **Working directory:** inherited from `termica.exe`, so launching from a
  directory opens the shell there.
- **Child standard handles:** termica was a console program, so a child would
  otherwise inherit its stdin/stdout/stderr and write straight to the terminal
  that launched termica instead of into the pseudoconsole. The child is
  created with `STARTF_USESTDHANDLES` and three NULL handles, which makes
  conhost attach the pseudoconsole's console handles (measured: the child sees
  `FILE_TYPE_CHAR` and the pty's 80x24 buffer).
- **Environment:** the child gets termica's environment with `TERM` filled in
  when it is missing or `dumb`, and `COLORTERM` when missing or empty, because
  prompt tools and Unix ports degrade to "dumb terminal" behaviour otherwise.
- **Quit:** `Ctrl+Shift+Q` and the window close button. Esc is a terminal key
  now (vim, PSReadLine), not an app binding.
- **Exit:** keep the window open and print `[process exited with code N]`, so
  the last output stays readable.
- **Process tree:** the child starts suspended, joins a
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job, then resumes; closing the window
  cannot leave `conhost.exe` or grandchildren behind. Job assignment is best
  effort; closing the pseudoconsole is the primary path.
- **Unsafe paste:** `ghostty_terminal_paste` refuses text that can inject
  commands; the app then asks with a two-button message box and retries with
  `allow_unsafe = true` if the user confirms.

## Risks, and what checking them found

- **Pipe-handle ownership.** The parent's copies of the pseudoconsole-side
  ends are closed right after `CreatePseudoConsole`. ConPTY does duplicate
  them: the round trip works and still reports EOF on close.
- **ConPTY's own input parser.** Replies it does not understand can surface as
  literal text. Left as a known limit rather than a hack; the replies the
  library generates are standard.
- **`cmd.exe` codepage.** Was an open question until milestone 4 measured it:
  a non-ASCII argument handed to a ConPTY child comes back as UTF-8, so no
  `chcp 65001` is needed. See A5.
- **UTF-8 split across reads.** The ring is a byte stream and only `term_write`
  touches it, so a chunk boundary cannot corrupt a sequence.
- **The environment block was malformed** (found while writing milestone 3):
  appended entries need room for their own NUL *and* the block needs its final
  NUL. `CreateProcessW` failed with error 87 and no shell started at all. The
  pty round trip in `--selftest` is the guard.

## Verification (done)

- `cmake --build --preset llvm-mingw` clean under `-Wall -Wextra -pedantic`.
- `--selftest` gained a real ConPTY check: `cmd.exe /c "echo termica-pty-ok"`
  through the pipes with exit code 0.
- GUI: `pwsh` 7 and `cmd` fallback (verified with a restricted `PATH`), typed
  commands echo and run, `Ctrl+C` interrupts, resize reaches the console
  (`147x45` reported by PowerShell after resizing to 1200x700), and closing
  the window with a live shell leaves no `conhost.exe` or shell behind.

---

# The built-in font

Two entries, in order, because the second replaced the first.

## Google Sans Code NF Mono (milestone 2 follow-up)

The executable carries its own Nerd Font as four RCDATA resources
(`assets/fonts/`, `src/fonts.rc.in`, `src/embed.c`) so the grid works with
icons and box drawing with nothing installed. A path on the command line still
wins; a path that cannot be opened falls back to the built-in family, and its
regular face also fills in codepoints a disk font lacks.

## Maple Mono NF 7.9 (current default)

`MapleMono-NF.zip`, SHA-256
`59098b87c895d871635d37680e88000ae2b2b25b55428195b228ec589e35fb89`, SIL OFL
1.1 (`assets/fonts/LICENSE.txt`), chosen because it is the family the user's
own prompt already expects. Only the file names, the resource script and the
reported label changed.

Measured rather than assumed:

- **Icon width.** At 16 pt, `M` and every patched codepoint (U+E0B0/U+E0B4
  powerline, U+F0C9 git, U+E725 seti, U+276F chevron) have the same advance, so
  the grid stays aligned. The *ink* of powerline and Font Awesome glyphs is
  11-13 px, slightly over one cell: that is how prompt segments are meant to
  touch, and the cell drawn after it paints over the overlap.
- **Coverage.** Box drawing, blocks, arrows and the chevron are present. CJK
  (U+4E2D) and colour emoji (U+1F600) are **not**, in this build; the upstream
  `MapleMono-NF-CN.zip` is the variant that adds them. The README says so
  instead of letting the grid go blank silently.
- **Fallback earns its keep.** Consolas has neither U+276F nor any Nerd icon,
  so `glyphs.fallback` is what keeps icons working under a system font.
- Metrics: `cell: 9x22 px` at 16 pt (Google Sans Code NF Mono was 9x21);
  executable 15.9 MB (was 16.5 MB).

---

# Milestone 3: pointer, history and focus

## Goal

A shell program can ask for the pointer and get it; the user can read what
scrolled away, select it and copy it; and programs that watch focus (vim,
PSReadLine) learn when the window is focused. All of it already exists inside
libghostty-vt - this milestone is embedder wiring, not emulation.

## Design

| Feature | Library API | Embedder work |
|---|---|---|
| mouse reporting | `ghostty_mouse_encoder_*`, `GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING` | feed surface-space pixel events, write the encoded bytes, keep the encoder geometry in step with the grid |
| scrollback UI | `ghostty_terminal_scroll_viewport`, `GHOSTTY_TERMINAL_DATA_SCROLLBAR`, `DATA_VIEWPORT_ACTIVE` | wheel, Shift+PgUp/PgDn, Ctrl+Shift rows and ends, a thumb on the right edge, jump to bottom on input |
| selection + copy | `ghostty_terminal_grid_ref`, `select_word`, `select_word_between`, `select_line`, `OPT_SELECTION`, `DATA_SELECTION`, `RENDER_STATE_ROW_DATA_SELECTION`, `FORMATTER_OPT_SELECTION` | click and drag gestures, Shift to override reporting, Alt for a block, highlight by swapping colors, `Ctrl+Shift+C` to the clipboard |
| focus events | `ghostty_focus_encode`, mode 1004 through `GHOSTTY_TERMINAL_DATA_MODE` | SDL focus events to bytes, bytes to the pty |

Everything above the "embedder work" line lives in `term.{h,c}`; `main.c`
keeps input routing and drawing.

## Decisions

- **Viewport cells everywhere.** Gestures and selections use
  `GHOSTTY_POINT_TAG_VIEWPORT`, so they keep pointing at the same text while
  scrolled into history.
- **The pointer does two jobs.** A program that enabled tracking gets the
  events; holding **Shift** takes them back for the app. When nobody is
  listening, the wheel scrolls the viewport.
- **Gestures by hand, semantics by the library.** SDL's click count picks cell
  / word / line; the ranges, word boundaries and line extents come from
  `ghostty_terminal_select_*`. Word drags use `select_word_between` so they do
  not collapse when the pointer passes between words.
- **A plain click clears the selection.** Copy is explicit (`Ctrl+Shift+C`);
  no copy-on-select.
- **Selection highlight = swapped colors**, so no new color is invented and it
  works over any background.
- **Scroll step**: 3 rows per notch, one page per PgUp/PgDn, one row per
  Ctrl+Shift+Up/Down. Anything that reaches the program first jumps to the
  bottom.
- **Reporting is silent outside the grid.** The drag strip and resize borders
  stay app chrome.

## Risks, and what checking them found

- **Grid refs are snapshots**, valid until the next mutating call. Every
  gesture resolves fresh points; nothing is cached across a `term_write`.
- **Wheel under mouse tracking** must reach the program: the app asks the
  encoder first and scrolls the viewport only when it produced nothing, which
  is also what makes scrolling work for a program that enabled only button
  events.
- **Continuous output while a selection is held** was assumed to need clearing
  on output. Measured: the library keeps the selection naming the same text on
  its own, so nothing is cleared. See milestone 4 A2.
- **Modifiers are per side in SDL3.** `KMOD_SHIFT` and `KMOD_CTRL` are unions
  (LSHIFT|RSHIFT, LCTRL|RCTRL), so `(mod & (SHIFT|CTRL)) == (SHIFT|CTRL)` can
  never be true - that made milestone 2's `Ctrl+Shift+Q` dead. Bindings test
  each union separately.
- **Bindings match scancodes, not keycodes**: the keycode for `Shift+C` is not
  `SDLK_C`, so Shift-held bindings must use `event->scancode`.
- **ConPTY reports a mode change, not the bytes a client wrote.** A script
  cannot simply `type` mouse-mode sequences; a program that uses the console
  input API (`fzf`) does enable reporting, and that is how the pointer handoff
  was verified.

## Verification (done)

- `--selftest`: scrolling by rows and to the ends, the scrollbar figures, cell /
  word / line / block selections and their copied text, SGR press and release
  encoding (`ESC[<0;2;2M` / `m`), motion not reported under mode 1000 but
  reported under 1003, `CSI I` / `CSI O`.
- GUI: wheel into history shows the thumb, drag/double/triple select and
  highlight by swapped colors, `Ctrl+Shift+C` fills the clipboard with the
  listing text, and with `fzf` holding the pointer a click moves fzf's
  selection while a plain drag selects nothing - Shift+drag does.

---

# Milestone 4: pointer fidelity and window chrome

Five fidelity items plus the console-subsystem switch, each measured before
being written down.

## A1 - underline styles

`style.c` collapsed every underline to a single 2 px bar, so curly (the
spell/parse error in PSReadLine and vim), dotted, dashed and double all looked
like a hard error. The library reports `NONE/SINGLE/DOUBLE/CURLY/DOTTED/DASHED`;
the style layer now passes the kind through (`CellStyle.underline_style`, with
`underline` kept as the "is there any" test) and the renderer draws each:

| style | drawing |
|---|---|
| single | one 2 px bar on the baseline row |
| double | two 1 px bars, 3 px apart |
| curly | a one-pixel sine, one period per cell |
| dotted | 1 px dots every 2 px, phased across the row |
| dashed | 3 px on, 2 px off, phased across the row |

## A2 - the selection follows its text

Measured against the library before changing anything:

```
selected now: [alpha]        ->  after 12 new rows: [alpha]
after resize to 20 cols: [alpha 3 br]
```

`GHOSTTY_TERMINAL_OPT_SELECTION` stores terminal-owned tracked state: the
selection keeps naming the same text through scrolling **and** through reflow,
and the row-local ranges move with it. Two consequences:

- `update_geometry()` no longer clears the selection; milestone 3 destroyed
  there a selection the library would have kept.
- A drag still needs its own anchor. While the button is held, the focus end
  is the cell under the pointer (viewport coordinates, which is right), but the
  anchor is the cell where the press happened, and rebuilding it from viewport
  coordinates means output arriving mid-drag slides the anchor onto different
  text. `term_selection_drag_start` keeps a `GhosttyTrackedGridRef` for it,
  `term_selection_drag_cells` / `_words` / `_line` snapshot it against the live
  focus cell, and `term_selection_drag_end` releases it.

## A3 - the app stops stealing Alt

`Alt+Up/Down/M` were intercepted before the encoder, but readline's Meta
bindings are exactly those keys. App chrome moved under `Ctrl+Shift`, which no
shell binds, so every `Alt+*` reaches the program:

| was | now | why the new one |
|---|---|---|
| `Alt+Up` / `Alt+Down` (opacity) | `Ctrl+Shift+=` / `Ctrl+Shift+-` | `Ctrl+Alt+*` is AltGr on many layouts and would eat the character that key produces |
| `Alt+M` (backdrop) | `Ctrl+Shift+B` | the same AltGr reason |
| `Ctrl+Shift+Up/Down/Home/End` | unchanged | scroll rows / top / bottom |

## A4 - the scrollbar is a control

The thumb was drawn but not hit-tested and there was no way to jump. Now a
press on the thumb starts a drag, a press on the track pages toward the click,
and dragging maps the pointer through
`GHOSTTY_TERMINAL_SCROLL_VIEWPORT_ROW` - the same row space the scrollbar
reports as `offset`, so it round-trips. The strip sits just inside SDL's 8 px
resize border: at the window's last pixels clicks are consumed as resizing and
never reach the app. It is outside the cell grid, so it never competes with
mouse reporting either.

## A5 - the codepage risk, actually checked

Milestone 2 listed "test `chcp 65001`" as an open risk and never ran it. The
pty self test now hands a non-ASCII argument to a real ConPTY child and
requires UTF-8 back. It passes, so no `chcp` hack is needed - documented in
the README rather than worked around.

## No console window on double-click

`WIN32_EXECUTABLE` is set, so starting `termica.exe` from Explorer no longer
flashes a console. `src/console.c` reclaims the launching console for the
diagnostics when the process was given no standard handles, and leaves anything
the launcher already wired up - a console, a pipe, a redirected file - alone, so
`--selftest` output is still capturable. `run.ps1` waits for the process
explicitly, because a GUI-subsystem launch returns immediately.

## Verification (done)

- `--selftest`: 59 checks, no failures - that was the suite's size at this
  milestone; it is 92 now, see milestone 5. New: SGR 4 / 4:2 / 4:3 / 4:4 / 4:5
  resolve to single / double / curly / dotted / dashed, reset leaves no
  underline behind, and the non-ASCII ConPTY round trip.
- Screenshot of `type build\under.txt` in `cmd`: five visibly different
  decorations under `single curly dotted dashed double`.
- **A3, the basis for "every `Alt+*` reaches the shell"**: no key binding in
  `main.c` tests `SDL_KMOD_ALT` positively - the only Alt mention in the key
  path is the paste guard, which fires *without* Alt (`key == V && CTRL &&
  !(ALT | GUI)`), and Alt+drag block selection is a mouse gesture - while
  `ghostty_modifiers_for` copies Alt into `GHOSTTY_MODS_ALT` (with the
  right-side flag) for the encoder. So the claim is a code fact plus the
  encoder test, not a keystroke test: `Alt+b` moving a word in readline stays
  a manual check (README item 17).
- Scrollbar: 10 wheel notches showed the thumb; a click near the top of the
  track changed the view; dragging the thumb back down produced a sampled band
  **identical to the live view** with the bar hidden. With the strip still
  under the resize border the hit test reported `hit=0`; inside it, `hit=1`.
- Selection across a resize to 1200x760: copied text identical before and
  after, and containing the rows that were selected.
- `Alt+M` left the sampled panel band byte-identical while `Ctrl+Shift+=`
  changed it; `Ctrl+Shift+B` still switches the backdrop.
- PE subsystem is 2 (Windows GUI); launched from a shell the `renderer:` and
  `font:` lines still print, and `run.ps1` returns the app's exit code.

## Harness notes, because they cost real time

Synthetic input comes in two flavours, and only one of them is evidence.
Milestone 5 found this the hard way; it is stated once here and applies to
every GUI claim in this file.

- **Window-targeted** - `PostMessage` / `SendMessage` to termica's `HWND`, and
  screenshots of its own window. The message cannot land anywhere else, and a
  screenshot of the grid showing the effect proves the bytes reached that
  window. Trustworthy. (SDL's hit test swallows anything within 8 px of the
  edge, so client coordinates have to account for it.)
- **Desktop-wide** - `SendKeys`, `keybd_event`, `mouse_event`, `SetCursorPos`.
  These go to whatever the desktop has focused, which may not be termica, and
  they move or type into the user's own session while doing so. They stopped
  being used in milestone 5 for that second reason alone. A result from these
  is only evidence when it was read out of termica's window (a screenshot),
  never when the only proof was an external side effect - the clipboard, or a
  dialog that did not appear.

So the test is whether the effect was seen **inside termica's own window**. A
change in its pixels can only have been caused by input that reached it:
the five distinguishable underline kinds, the thumb appearing after wheel
notches, a track click and a thumb drag changing the view, the sampled band
identical to the live view, `fzf` moving its selection on a click, the panel
band changing under `Ctrl+Shift+=` - all stand. `--selftest` and the headless
probes cover the rest: the underline kinds resolving from SGR, selection text
surviving reflow (measured before any GUI code changed), the scrollbar
figures, the OSC 52 decode from both a literal string and a real child.

What does not stand: a result whose only proof was something outside the
window - the clipboard contents, the absence of a dialog - or any negative
result ("`Alt+M` changed nothing"), because input that never arrived looks
exactly like input that arrived and was refused. Those are re-listed as manual
checks: `Ctrl+Shift+C` filling the clipboard, `copy_on_select`, the
`osc52 = allow` write, and the `Alt` keys genuinely reaching the shell.

The technical notes that survive:

- `WM_MOUSEWHEEL` goes to the window **under the pointer**, so synthetic wheel
  messages need a real `SetCursorPos` into the window first - and that moves
  the user's cursor, which is exactly the disturbance to avoid.
- `SendMessage(WM_LBUTTONDOWN)` takes client coordinates, and SDL's hit test
  swallows anything within 8 px of the window edge.
- In PowerShell's `SendKeys`, `%` means Alt and `{=}` / `{EQUALS}` are not
  valid keys - which is why `Ctrl+Shift+=` (VK_OEM_PLUS, scan 0x0D) was typed
  with `keybd_event`: a window-targeted way to press a held modifier
  combination does not exist in Win32.

---

# Milestone 5: everyday usability

## Goal

Take away the rough edges that make termica feel like a demo: remember what
the user chose, let a program put text on the clipboard, make a selection
longer than the screen possible without letting go of the button, keep the
pointer shape honest, and show uncommitted IME text instead of nothing.

## Design

| Item | Library / SDL surface | Where it lands |
|---|---|---|
| settings file | none - plain `key = value` | `src/settings.{h,c}`: read at startup, written on exit, command line wins |
| copy-on-select | existing `term_selection_text` | end of the release branch in `handle_mouse_button`, gated by the setting |
| OSC 52 clipboard write | `GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE`, `GhosttyClipboardWrite`, `GhosttyClipboardWriteReply` | `on_clipboard_write` in `term.c` (decode, choose a representation, answer), `handle_clipboard_write` in `main.c` (policy) |
| pointer shape | `GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE` (OSC 22), `SDL_CreateSystemCursor` | `apply_mouse_shape` on motion, `reset_mouse_shape` on window mouse-leave |
| IME composition | `SDL_EVENT_TEXT_EDITING`, `SDL_SetTextInputArea` | `App.composing`, drawn over the cursor cell in `draw_frame`, input area set to the same cell |
| selection autoscroll | `ghostty_terminal_scroll_viewport` deltas plus the tracked anchor | 40 ms timer while a drag is held past the first or last row |

## Decisions

- **No parser dependency.** `key = value` lines, `#` comments, unknown keys
  ignored, values validated. Keys fold case and separators, so
  `CopyOnSelect`, `copy-on-select` and `copy_on_select` are one key - which
  is the whole reason the folder of names is folded on both sides rather
  than translated.
- **Location:** `%APPDATA%\termica\termica.conf`; elsewhere
  `$XDG_CONFIG_HOME/termica` or `~/.config/termica`. The directory is created
  on load, so a save cannot fail for that reason.
- **`shell = auto|pwsh|powershell|cmd`** picks a program by name instead of
  taking a path: a named shell that is not installed shows an error rather
  than silently starting a different one.
- **OSC 52 defaults to `ask`.** A remote host can put anything on the
  clipboard, so `allow` is something the user opts into; `deny` answers the
  program with the protocol's permission failure.
- **Nothing mutates the terminal inside the clipboard callback** - the VT
  stream is paused there, so no status message is printed into the grid; the
  message box is the feedback.
- **The starting pointer shape is `TEXT`**, which over a cell grid means an
  I-beam by default. That is the right default cursor for a terminal, so no
  special case is needed, but `SDL_SetCursor` is process-wide and so the
  shape is handed back on window mouse-leave.
- **Autoscroll runs only while the button is held past an edge**, with the
  focus end pinned to that edge row; the tracked anchor from milestone 4
  keeps the selection on its own text while the viewport moves.
- **Preedit text is drawn over the cursor cell**, not written into the grid,
  because the screen buffer does not contain it yet.

## Verification

- `--selftest` gained the settings parser (defaults, bounds that must be
  refused, unknown keys, comments, case and separator folding, a full format
  round trip) and the OSC 52 path in two forms: a literal byte string through
  the parser, and a real `powershell.exe` child under ConPTY writing
  `OSC 52;c;<base64>` that must arrive decoded at the app's callback.
  92 checks, none failing - the suite was that size at this milestone; it is
  124 now, see milestone 6.
- Settings were checked without simulating input: with `shell = cmd` the
  process tree shows `cmd.exe` as termica's child, and after closing the
  window the file holds exactly the values the session used.
- **Not machine-verified:** copy-on-select, selection autoscroll, the OSC 22
  pointer shape, the `ask` dialog and IME preedit drawing. Each needs a real
  keyboard, pointer or input method. An attempt to drive them with
  `SendKeys`/`SetCursorPos` was stopped rather than trusted: synthetic input
  goes to whatever window the desktop has focused, and keys started landing
  in the session's own terminal instead of in termica - useless as evidence
  and rude to run. The README lists them as manual checks.
- The throwaway PowerShell harnesses used for the windowed checks were deleted
  with the other build-time scratch files, so nothing here is re-runnable
  except `--selftest`. Rebuilding one should use `PostMessage` / `SendMessage`
  and screenshots of the window only; the reason is above.

## Still not done

- OSC 8 hyperlinks and kitty graphics: both need anchor geometry and an image
  store in the renderer, not callback wiring.
- Desktop notifications, progress reporting and the bell.
- Per-glyph fallback to a system CJK/emoji font; per-monitor DPI rescaling.
- A paste writer queue, so a multi-megabyte paste cannot block the UI thread.
- CI: nothing about a workflow file can be verified from here, so it is
  deliberately not added rather than added blind.

---

# Milestone 6: TOML settings

## Goal

Replace the hand-written `key = value` parser with a TOML file parsed by
[tomlc17](https://github.com/cktan/tomlc17), without changing the `Settings`
struct or the `settings_load` / `settings_save` API that `main.c` uses.

## Design

- The file is `%APPDATA%\termica\termica.toml`; elsewhere
  `$XDG_CONFIG_HOME/termica/termica.toml` or
  `~/.config/termica/termica.toml`. The old `termica.conf` is not read.
- Three sections keep the keys together: `[font]` (`path`, `size`),
  `[window]` (`opacity`, `backdrop`) and `[terminal]` (`copy_on_select`,
  `shell`, `osc52`). Section and key names still fold case and `-`/`_`.
- Values are validated by their TOML type: strings for paths and enums,
  booleans for `copy_on_select`, floats or integers for `size` and `opacity`.
  Non-finite numbers are refused before the range check, so `nan` and `inf`
  cannot pass. A wrong type or an out-of-range value leaves the previous value
  alone; unknown keys and tables are ignored.
- TOML syntax is all-or-nothing: a malformed document keeps the defaults and
  prints the parser's message and line number on stderr. The old line parser
  could apply the good lines around a bad one; the TOML contract cannot.
- tomlc17 is fetched by CMake, pinned to release `R261116`, and built as its
  own C17 static library target; termica's own code stays strict C99.
- Files are read with `SDL_LoadFile` and `toml_parse`, not
  `toml_parse_file_ex`, because the latter uses narrow `fopen` and would fail
  on a non-ASCII Windows profile path.
- Saves write a temporary file and `SDL_RenamePath` it over the real one, so
  a failed write cannot truncate the settings. The output is canonical TOML;
  comments and unknown keys are not preserved on exit.
- `font.path` accepts either a file or an installed family name. The family
  is resolved by `src/font_lookup.c` from
  `HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts` and then the
  `HKLM` equivalent: the `(TrueType)` / `(OpenType)` suffix is removed, the
  name must be the whole family or the family followed by a known style word,
  and `Regular` outranks the other styles. Registry values that are relative
  file names are tried under `%WINDIR%\Fonts` and
  `%LOCALAPPDATA%\Microsoft\Windows\Fonts`; `REG_EXPAND_SZ` values are
  expanded first. A name that is already a readable file is returned
  untouched. Adjacent bold/italic faces named `...-Regular.ttf` are found by
  retrying without the `-Regular` suffix.

## Verification

- Build clean under `-Wall -Wextra -pedantic`; `--selftest` runs 124 checks,
  none failing. The settings checks cover the three sections, typed values,
  bounds (`0`, `900`, `nan`, `inf`, wrong types), folding of section and key
  names, unknown keys, malformed documents, string escaping, an oversized
  font path, and a full format round trip. The font lookup checks that an
  unknown name is refused, that an installed family (`Consolas`) resolves,
  and that a resolved path is returned unchanged.
- A hand-edited `termica.toml` with `size = 13.5`, `opacity = 0.65`,
  `backdrop = "acrylic"`, `shell = "cmd"`, `osc52 = "deny"` and a Windows
  font path loaded and came back unchanged after the app exited. The process
  tree showed `cmd.exe` (and `conhost.exe`) as termica's children, so the
  section was read, not just written.
- The saved file parsed back into the same values and wrote the font path as
  an escaped basic string (`C:\\Windows\\Fonts\\consola.ttf`).
- A deliberately malformed file (`[font` with no bracket) printed
  `settings: ...: (line 1) missing right-bracket` on stderr and the app
  started on its defaults.
- The same load/save round trip worked with `APPDATA` pointed at a directory
  whose name contained non-ASCII characters (`termica-settings-测试`), which
  is the reason the file is read through SDL rather than `fopen`.
- A `font.path` of `"Inconsolata Nerd Font Propo"` (installed per user, not
  under `C:\Windows\Fonts`) opened
  `C:\Users\i_rac\AppData\Local\Microsoft\Windows\Fonts\InconsolataNerdFontPropo-Regular.ttf`
  at `size = 18`; the `font:` line named that file, and `bold: yes` showed
  the adjacent `-Bold.ttf` was found. Before the resolver this name reached
  `TTF_OpenFont` as if it were a path and silently fell back to the built-in
  font.
- Not verified: nothing in this milestone needs a keyboard, pointer or input
  method, so every claim above is from the build, the self test or a real
  process run.
