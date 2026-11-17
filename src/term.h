/*
 * termica - the terminal half of the app.
 *
 * A thin layer between libghostty-vt and the renderer. It owns:
 *
 *   - the terminal itself (the screen state, the parser, the modes)
 *   - the render state a frame is read out of
 *   - the key encoder that turns key presses into the bytes a program
 *     expects
 *
 * The model is the one from the docs:
 * https://lib.ghostty.org/vt/first-terminal.html
 *
 * Strict C99.
 */

#ifndef TERMICA_TERM_H
#define TERMICA_TERM_H

#include <ghostty/vt.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Term Term;

/* Called when the terminal needs bytes written back to the program. */
typedef void (*TermWriteFn)(void *userdata, const void *data, size_t length);

/* What the app did with a program's clipboard write request (OSC 52,
 * iTerm2 OSC 1337 and the kitty protocol all arrive here normalized). */
typedef enum {
    TERM_CLIPBOARD_ALLOWED,
    TERM_CLIPBOARD_DENIED,
    TERM_CLIPBOARD_UNSUPPORTED
} TermClipboardResult;

/* `text` is the first textual representation, NUL-terminated for
 * convenience; `length` excludes that NUL. `name` is the program's own
 * label and may be empty. The app decides - ask, allow or deny - and
 * answers so the library can reply to the program. */
typedef TermClipboardResult (*TermClipboardWriteFn)(void *userdata,
                                                    const char *text,
                                                    size_t length,
                                                    const char *name);

struct Term {
    GhosttyTerminal               handle;        /* the screen state      */
    GhosttyRenderState            render_state;  /* the frame we draw     */
    GhosttyRenderStateRowIterator row_iterator;  /* walks the rows        */
    GhosttyRenderStateRowCells    cell_iterator; /* walks a row's cells   */
    GhosttyKeyEncoder             key_encoder;   /* key press -> bytes    */
    GhosttyKeyEvent               key_event;     /* the event we fill in  */
    GhosttyMouseEncoder           mouse_encoder; /* pointer -> bytes      */
    GhosttyMouseEvent             mouse_event;   /* the event we fill in  */
    GhosttyMouseEncoderSize       mouse_size;    /* grid geometry, pixels */

    /* Where the terminal's replies go: device status reports, mode
     * queries, XTWINOPS. Called synchronously, on the thread that wrote
     * into the terminal. */
    TermWriteFn writer;
    void       *writer_userdata;

    /* The cell where the left button went down, tracked while the drag is
     * in progress. NULL when there is no drag. */
    GhosttyTrackedGridRef drag_anchor;

    /* A program asked to put text on the clipboard. The app answers; the
     * library's reply is sent from inside the callback, so this must not
     * outlive it. */
    TermClipboardWriteFn clipboard_write;
    void                *clipboard_userdata;

    uint16_t columns;
    uint16_t rows;
    uint32_t cell_width;
    uint32_t cell_height;

    /* The program set a new window title (OSC 0 or OSC 2). The app picks
     * this up and applies it to the window. */
    bool title_changed;
    char title[128];

    /* The shell reported a working directory (OSC 7, OSC 9 or OSC 1337).
     * Stored decoded: a file:// URI becomes a bare path with percent
     * escapes resolved, so the status bar and git runner can use it
     * directly. Empty until the shell reports one. */
    bool pwd_changed;
    char pwd[4096];

    /* Shell integration state from OSC 133 semantic prompts: whether a
     * command is running now, and what the last finished command reported.
     * `command_started_at` and `last_command_ms` are SDL_GetTicks values. */
    bool     command_running;
    bool     last_command_known;
    int32_t  last_exit_code;
    uint64_t last_command_ms;
    uint64_t command_started_at;
};

/* Install the sink for term_write replies (usually pty_write). */
void term_set_writer(Term *term, TermWriteFn writer, void *userdata);

/* Install the handler for program clipboard writes. Without one the
 * library's requests are ignored (and the kitty protocol answers ENOSYS). */
void term_set_clipboard_write(Term *term, TermClipboardWriteFn write,
                              void *userdata);

/* Create the terminal and everything around it. False on failure. */
bool term_init(Term *term, uint16_t columns, uint16_t rows);

/* Free everything. Safe to call on a zeroed struct. */
void term_free(Term *term);

/* Feed bytes a program wrote. The parser handles every escape sequence:
 * SGR colors and attributes, cursor movement, erase, scroll, OSC, ... */
void term_write(Term *term, const void *data, size_t length);

/* The current working directory the shell reported, decoded to a bare
 * path. NULL or empty until the shell emits one (OSC 7/9/1337). */
const char *term_pwd(const Term *term);

/* Resize the grid. cell_width and cell_height are in pixels. */
void term_resize(Term *term, uint16_t columns, uint16_t rows,
                 uint32_t cell_width, uint32_t cell_height);

/* Encode one key press the way the terminal's current modes want it.
 * Returns the number of bytes written into buffer; 0 means the key
 * produced no bytes at all (a plain letter, for example, which arrives as
 * text instead). */
size_t term_encode_key(Term *term, GhosttyKeyAction action, GhosttyKey key,
                       GhosttyMods modifiers, uint32_t unshifted_codepoint,
                       uint8_t *buffer, size_t capacity);

/* Paste clipboard text the way the program asked for it: bracketed by
 * \x1b[200~ / \x1b[201~ when mode 2004 is set, or as a kitty paste event
 * when the clipboard protocol is on. Then the text goes to the writer.
 *
 * Unsafe text (line breaks outside bracketed paste, an embedded paste-end
 * sequence) is refused unless allow_unsafe is set; *rejected says so.
 * Returns false when nothing was written. */
bool term_paste(Term *term, const char *text, size_t length,
                bool allow_unsafe, bool *rejected);

/* Run the self test from the docs. Returns the number of failures. */
int term_selftest(void);

/* ------------------------------------------------------------------------- */
/* Mouse reporting (milestone 3)                                              */
/* ------------------------------------------------------------------------- */

/* Where the grid sits inside the window, in pixels. The cell size comes
 * from term_resize. The mouse encoder turns surface-space pointer
 * positions into the cell coordinates programs expect. */
void term_set_geometry(Term *term, uint32_t screen_width,
                       uint32_t screen_height, uint32_t padding_left,
                       uint32_t padding_top, uint32_t padding_right,
                       uint32_t padding_bottom);

/* Whether a button is held down, so the encoder can tell a hover from a
 * drag. */
void term_set_mouse_button(Term *term, bool pressed);

/* Encode one pointer event for the terminal's current tracking mode.
 * Returns the number of bytes written; 0 means the program is not
 * listening for this kind of event. */
size_t term_encode_mouse(Term *term, GhosttyMouseAction action,
                         GhosttyMouseButton button, GhosttyMods modifiers,
                         float x, float y, uint8_t *buffer, size_t capacity);

/* Whether any mouse tracking mode is on. When it is, pointer events belong
 * to the program unless the user holds Shift. */
bool term_mouse_tracking(Term *term);

/* The pointer shape the program asked for (OSC 22). Initially
 * GHOSTTY_MOUSE_SHAPE_TEXT, which over a cell grid is what an I-beam is
 * for. False when the terminal has nothing to say, in which case the app
 * keeps its own cursor. */
bool term_mouse_shape(Term *term, GhosttyMouseShape *out_shape);

/* ------------------------------------------------------------------------- */
/* Focus reporting (milestone 3)                                              */
/* ------------------------------------------------------------------------- */

/* Whether the program asked for focus events (mode 1004). */
bool term_focus_reporting(Term *term);

/* The bytes for a focus gained/lost report. */
size_t term_encode_focus(Term *term, bool gained, uint8_t *buffer,
                         size_t capacity);

/* ------------------------------------------------------------------------- */
/* Scrollback (milestone 3)                                                   */
/* ------------------------------------------------------------------------- */

/* Scroll by a number of rows (negative is up), or to an end. */
void term_scroll_rows(Term *term, intptr_t delta);
void term_scroll_top(Term *term);
void term_scroll_bottom(Term *term);

/* Scroll so that `row` becomes the first visible row. The row space is the
 * same one GhosttyTerminalScrollbar reports as `offset`, which is what makes
 * a scrollbar drag land where the thumb was. */
void term_scroll_row(Term *term, uint32_t row);

/* Whether the viewport is following the active area (at the bottom). */
bool term_viewport_at_bottom(Term *term);

/* The rows needed to draw a scrollbar: total area, viewport offset and
 * viewport length. False when the terminal cannot say. */
bool term_scrollbar(Term *term, GhosttyTerminalScrollbar *out);

/* ------------------------------------------------------------------------- */
/* Selection (milestone 3)                                                    */
/* ------------------------------------------------------------------------- */

/* All coordinates are viewport cells: x 0..columns-1, y 0..rows-1, with y
 * counted from the top of what is on screen, scrolled or not. */

void term_selection_clear(Term *term);
bool term_selection_cells(Term *term, int start_x, int start_y, int end_x,
                          int end_y, bool rectangle);
bool term_selection_word(Term *term, int x, int y);
bool term_selection_word_between(Term *term, int x1, int y1, int x2, int y2);
bool term_selection_line(Term *term, int x, int y);
/* An in-progress drag. The anchor is a tracked reference, so output that
 * scrolls or reflows the screen while the button is held keeps the
 * selection anchored to the text it started on instead of sliding onto
 * whatever moved into those viewport cells. Call drag_start on press, one of
 * the three update functions while the pointer moves, and drag_end on
 * release. */
bool term_selection_drag_start(Term *term, int x, int y);
bool term_selection_drag_cells(Term *term, int focus_x, int focus_y,
                               bool rectangle);
bool term_selection_drag_words(Term *term, int focus_x, int focus_y);
bool term_selection_drag_line(Term *term);
void term_selection_drag_end(Term *term);

bool term_selection_has(Term *term);

/* The selected text as UTF-8, trailing spaces of each line trimmed. The
 * caller frees the result. NULL when nothing is selected or it is empty. */
char *term_selection_text(Term *term, size_t *out_length);

#endif /* TERMICA_TERM_H */
