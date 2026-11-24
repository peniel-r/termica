/*
 * termica - per-session terminal, ConPTY, selection and IME state.
 *
 * One Session owns one libghostty-vt terminal, its child ConPTY, its
 * pointer/selection gesture state, and its uncommitted IME text. Splitting
 * this out of App keeps main.c small and prepares for multi-session tabs.
 */
#ifndef TERMICA_SESSION_H
#define TERMICA_SESSION_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pty.h"
#include "settings.h"
#include "term.h"
#include "search.h"

/* Room for uncommitted IME text. SDL3 hands the string over for the
 * duration of the event, so the session keeps its own copy; 128 bytes is
 * several dozen composed characters. */
#define SESSION_COMPOSING_MAX 128

/* What a left-drag means: single cells, whole words, or whole lines. */
enum {
    SELECT_CELLS = 0,
    SELECT_WORDS = 1,
    SELECT_LINES = 2
};

/* A left-button gesture. Coordinates are viewport cells, so a selection
 * stays where it is while the user scrolls through history. */
typedef struct SelectionDrag {
    bool dragging;
    bool moved;
    bool rectangle;
    int  mode;
    int  anchor_x;
    int  anchor_y;
    int  focus_x;
    int  focus_y;
} SelectionDrag;

typedef struct Session {
    Term          term;
    Pty          *pty;              /* the shell behind the grid            */

    SelectionDrag select;
    Search        search;          /* independent find bar and results     */
    bool          mouse_held;       /* a button is down over the grid       */
    bool          bar_drag;         /* the scrollbar thumb is being dragged */
    int           autoscroll;       /* -1 up, 0 off, +1 down while dragging */

    /* In-progress IME text, drawn at the cursor until it is committed.
     * SDL hands the string over for the duration of the event only, so it
     * is copied here. */
    char          composing[SESSION_COMPOSING_MAX];
    int           composing_length;

    bool          shell_exited;
} Session;

/* Initialise the terminal and wire its writer and clipboard callbacks. */
bool session_init(Session *s, uint16_t columns, uint16_t rows,
                  TermClipboardWriteFn clipboard_fn, void *clipboard_userdata);

/* Tear down the pty (if any) and the terminal. Safe on a zeroed session. */
void session_free(Session *s);

/* Spawn the configured shell (or custom command/cwd/env) at the given grid
 * dimensions. Spawn errors are printed into the session's own grid so the
 * user sees why nothing started. */
void session_spawn_shell(Session *s, const Settings *settings, uint16_t columns,
                         uint16_t rows, PtyNotifyFn on_output,
                         void *output_userdata);

/* Send raw bytes to the child shell (no-op if the pty is not running). */
void session_send(Session *s, const void *data, size_t length);

/* Write an app-level message into the terminal grid. */
void session_show_message(Session *s, const char *text);

/* Resize both the terminal grid and the underlying pseudoconsole. */
void session_resize(Session *s, uint16_t columns, uint16_t rows,
                    uint32_t cell_width, uint32_t cell_height);

/* Update the terminal's mouse-encoder pixel geometry and reset in-flight
 * pointer gestures whose cell coordinates are no longer valid. */
void session_update_geometry(Session *s, uint32_t pixel_width,
                             uint32_t pixel_height, uint32_t grid_left,
                             uint32_t grid_top, uint32_t grid_right,
                             uint32_t grid_bottom);

/* Drain pending ConPTY output into the terminal on the main thread. Returns
 * true if any bytes were parsed. Sets *exited_now to true the first time the
 * direct child reports an exit code. */
bool session_pump(Session *s, bool *exited_now);

/* Snap the viewport back to the active area if scrolled into history. */
void session_jump_to_bottom(Session *s);

/* Copy the active selection to the system clipboard. */
void session_copy_selection(Session *s);

/* Rebuild the selection from s->select using libghostty's word/line/cell
 * semantics. */
void session_selection_update(Session *s);

/* Send layout/IME text to the shell, stripping stray CRs that SDL reports
 * alongside the Return key event. */
void session_send_text(Session *s, const char *text);

/* Record or clear uncommitted IME composition text. */
void session_set_composing(Session *s, const char *text);
void session_clear_composing(Session *s);

/* Paste from the system clipboard through libghostty's bracketed/unsafe
 * paste path, prompting with a modal dialog over `window` if unsafe. */
bool session_paste(Session *s, SDL_Window *window);

#endif /* TERMICA_SESSION_H */
