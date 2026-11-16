/*
 * termica - the terminal half of the app. See term.h.
 */

#include "term.h"

#include <stdlib.h>
#include <string.h>

/* The program asked for a new window title (OSC 0 or OSC 2). The title
 * itself is read back from the terminal; the callback only says it
 * changed. */
static void on_title_changed(GhosttyTerminal terminal, void *userdata)
{
    Term *term = (Term *)userdata;
    GhosttyString title = { 0 };
    size_t length;

    if (!term)
        return;
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE,
                             &title) != GHOSTTY_SUCCESS)
        return;

    length = title.len;
    if (length >= sizeof term->title)
        length = sizeof term->title - 1;
    if (length > 0)
        memcpy(term->title, title.ptr, length);
    term->title[length] = '\0';
    term->title_changed = true;
}

/* The library needs to answer device status reports, mode queries and
 * XTWINOPS; those bytes belong in the program's input, so hand them to the
 * writer the app installed. */
static void on_write_pty(GhosttyTerminal terminal, void *userdata,
                         const uint8_t *data, size_t length)
{
    Term *term = (Term *)userdata;

    (void)terminal;
    if (term && term->writer && length > 0)
        term->writer(term->writer_userdata, data, length);
}

void term_set_writer(Term *term, TermWriteFn writer, void *userdata)
{
    if (!term)
        return;
    term->writer = writer;
    term->writer_userdata = userdata;
}

bool term_init(Term *term, uint16_t columns, uint16_t rows)
{
    bool ok = false;

    memset(term, 0, sizeof *term);
    term->columns = columns;
    term->rows = rows;

    if (ghostty_terminal_new(NULL, &term->handle, columns, rows)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_new(NULL, &term->render_state) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_row_iterator_new(NULL, &term->row_iterator)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_row_cells_new(NULL, &term->cell_iterator)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_key_encoder_new(NULL, &term->key_encoder) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_key_event_new(NULL, &term->key_event) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_mouse_encoder_new(NULL, &term->mouse_encoder) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_mouse_event_new(NULL, &term->mouse_event) != GHOSTTY_SUCCESS)
        goto done;
    term->mouse_size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);

    /* The only effect we act on so far is the window title; replies go
     * through the writer, which the app points at the pty. */
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_USERDATA, term);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         (const void *)on_title_changed);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         (const void *)on_write_pty);
    ok = true;

done:
    if (!ok)
        term_free(term);
    return ok;
}

void term_free(Term *term)
{
    /* The render state borrows the terminal, and the iterators borrow the
     * render state, so free in that order. */
    ghostty_mouse_event_free(term->mouse_event);
    ghostty_mouse_encoder_free(term->mouse_encoder);
    ghostty_key_event_free(term->key_event);
    ghostty_key_encoder_free(term->key_encoder);
    ghostty_render_state_row_cells_free(term->cell_iterator);
    ghostty_render_state_row_iterator_free(term->row_iterator);
    ghostty_render_state_free(term->render_state);
    ghostty_terminal_free(term->handle);
    memset(term, 0, sizeof *term);
}

void term_write(Term *term, const void *data, size_t length)
{
    if (!term->handle || length == 0)
        return;
    ghostty_terminal_vt_write(term->handle, (const uint8_t *)data, length);
}

void term_resize(Term *term, uint16_t columns, uint16_t rows,
                 uint32_t cell_width, uint32_t cell_height)
{
    if (!term->handle)
        return;
    term->columns = columns;
    term->rows = rows;
    term->cell_width = cell_width;
    term->cell_height = cell_height;
    ghostty_terminal_resize(term->handle, columns, rows, cell_width,
                            cell_height);
}

size_t term_encode_key(Term *term, GhosttyKeyAction action, GhosttyKey key,
                       GhosttyMods modifiers, uint32_t unshifted_codepoint,
                       uint8_t *buffer, size_t capacity)
{
    size_t byte_count = 0;

    if (!term->key_encoder)
        return 0;

    /* The encoder follows the modes the program set (cursor keys, keypad,
     * kitty keyboard protocol, ...), so refresh it before every key. */
    ghostty_key_encoder_set_from_terminal(term->key_encoder, term->handle);

    ghostty_key_event_set_action(term->key_event, action);
    ghostty_key_event_set_key(term->key_event, key);
    ghostty_key_event_set_mods(term->key_event, modifiers);
    /* Plain text arrives as SDL_EVENT_TEXT_INPUT, not on the key event, so
     * there is no UTF-8 here. The unshifted codepoint (layout dependent,
     * e.g. 'a' for the A key) is what makes Ctrl+letter encode as
     * 0x01-0x1a. */
    ghostty_key_event_set_utf8(term->key_event, (GhosttyString){ 0 });
    ghostty_key_event_set_unshifted_codepoint(term->key_event,
                                              unshifted_codepoint);

    if (ghostty_key_encoder_encode(term->key_encoder, term->key_event,
                                   buffer, capacity, &byte_count)
            != GHOSTTY_SUCCESS)
        return 0;
    return byte_count;
}

/* The clipboard text handed to ghostty_terminal_paste is streamed through
 * this reader; the library asks for it once. */
typedef struct PasteText {
    const char *data;
    size_t      length;
} PasteText;

static bool paste_write(void *userdata, GhosttyString mime,
                        GhosttyWriter writer)
{
    const PasteText *text = (const PasteText *)userdata;

    (void)mime;
    if (text->length == 0)
        return true;
    return writer.write(writer.userdata, (const uint8_t *)text->data,
                        text->length);
}

bool term_paste(Term *term, const char *text, size_t length,
                bool allow_unsafe, bool *rejected)
{
    GhosttyPaste  paste = GHOSTTY_INIT_SIZED(GhosttyPaste);
    GhosttyString mime;
    PasteText     source;
    GhosttyResult result;
    bool          pasted = false;

    if (rejected)
        *rejected = false;
    if (!term->handle || !term->writer || !text || length == 0)
        return false;

    mime.ptr = (const uint8_t *)"text/plain";
    mime.len = 10;
    source.data = text;
    source.length = length;

    paste.location = GHOSTTY_CLIPBOARD_LOCATION_STANDARD;
    paste.source = GHOSTTY_PASTE_SOURCE_CLIPBOARD;
    paste.mimes = &mime;
    paste.mimes_len = 1;
    paste.reader.read = paste_write;
    paste.reader.userdata = &source;
    paste.allow_unsafe = allow_unsafe;

    result = ghostty_terminal_paste(term->handle, &paste, &pasted);
    if (result == GHOSTTY_REJECTED) {
        if (rejected)
            *rejected = true;
        return false;
    }
    return result == GHOSTTY_SUCCESS && pasted;
}

/* ------------------------------------------------------------------------- */
/* Mouse reporting                                                            */
/* ------------------------------------------------------------------------- */

/* The encoder turns surface-space pixels into the cell coordinates a
 * program expects, so it has to know where the grid is. Cell size is the
 * one term_resize already keeps. */
void term_set_geometry(Term *term, uint32_t screen_width,
                       uint32_t screen_height, uint32_t padding_left,
                       uint32_t padding_top, uint32_t padding_right,
                       uint32_t padding_bottom)
{
    if (!term->mouse_encoder)
        return;

    term->mouse_size.screen_width = screen_width;
    term->mouse_size.screen_height = screen_height;
    term->mouse_size.cell_width = term->cell_width > 0 ? term->cell_width : 1;
    term->mouse_size.cell_height =
        term->cell_height > 0 ? term->cell_height : 1;
    term->mouse_size.padding_left = padding_left;
    term->mouse_size.padding_top = padding_top;
    term->mouse_size.padding_right = padding_right;
    term->mouse_size.padding_bottom = padding_bottom;

    ghostty_mouse_encoder_set(term->mouse_encoder,
                              GHOSTTY_MOUSE_ENCODER_OPT_SIZE,
                              &term->mouse_size);
}

/* Whether a button is held changes what a motion event means (a drag is
 * reported, a hover only under any-event tracking), so the encoder has to
 * be told. Letting go also clears the last-cell deduplication. */
void term_set_mouse_button(Term *term, bool pressed)
{
    if (!term->mouse_encoder)
        return;
    ghostty_mouse_encoder_set(term->mouse_encoder,
                              GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                              &pressed);
    if (!pressed)
        ghostty_mouse_encoder_reset(term->mouse_encoder);
}

size_t term_encode_mouse(Term *term, GhosttyMouseAction action,
                         GhosttyMouseButton button, GhosttyMods modifiers,
                         float x, float y, uint8_t *buffer, size_t capacity)
{
    GhosttyMousePosition position;
    size_t byte_count = 0;

    if (!term->mouse_encoder || !term->mouse_event)
        return 0;

    /* Tracking mode and output format belong to the program: set them from
     * the terminal before every event, the same way the key encoder is
     * refreshed. */
    ghostty_mouse_encoder_set_from_terminal(term->mouse_encoder,
                                            term->handle);

    if (ghostty_mouse_event_set_action(term->mouse_event, action)
            != GHOSTTY_SUCCESS)
        return 0;
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
        ghostty_mouse_event_clear_button(term->mouse_event);
    else if (ghostty_mouse_event_set_button(term->mouse_event, button)
                 != GHOSTTY_SUCCESS)
        return 0;
    ghostty_mouse_event_set_mods(term->mouse_event, modifiers);

    position.x = x;
    position.y = y;
    ghostty_mouse_event_set_position(term->mouse_event, position);

    if (ghostty_mouse_encoder_encode(term->mouse_encoder, term->mouse_event,
                                     buffer, capacity, &byte_count)
            != GHOSTTY_SUCCESS)
        return 0;
    return byte_count;
}

bool term_mouse_tracking(Term *term)
{
    bool tracking = false;

    if (!term->handle)
        return false;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                             &tracking) != GHOSTTY_SUCCESS)
        return false;
    return tracking;
}

/* ------------------------------------------------------------------------- */
/* Focus reporting                                                            */
/* ------------------------------------------------------------------------- */

bool term_focus_reporting(Term *term)
{
    GhosttyTerminalModeConfig config;

    if (!term->handle)
        return false;
    config.mode = GHOSTTY_MODE_FOCUS_EVENT;
    config.value = false;
    if (ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_MODE,
                             &config) != GHOSTTY_SUCCESS)
        return false;
    return config.value;
}

size_t term_encode_focus(Term *term, bool gained, uint8_t *buffer,
                         size_t capacity)
{
    size_t written = 0;

    (void)term;
    if (ghostty_focus_encode(gained ? GHOSTTY_FOCUS_GAINED
                                    : GHOSTTY_FOCUS_LOST,
                             buffer, capacity, &written) != GHOSTTY_SUCCESS)
        return 0;
    return written;
}

/* ------------------------------------------------------------------------- */
/* Scrollback                                                                 */
/* ------------------------------------------------------------------------- */

static void scroll_viewport(Term *term, GhosttyTerminalScrollViewport scroll)
{
    if (!term->handle)
        return;
    ghostty_terminal_scroll_viewport(term->handle, scroll);
}

void term_scroll_rows(Term *term, intptr_t delta)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = delta;
    scroll_viewport(term, scroll);
}

void term_scroll_top(Term *term)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_TOP;
    scroll_viewport(term, scroll);
}

void term_scroll_bottom(Term *term)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM;
    scroll_viewport(term, scroll);
}

bool term_viewport_at_bottom(Term *term)
{
    bool active = true;

    if (!term->handle)
        return true;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE,
                             &active) != GHOSTTY_SUCCESS)
        return true;
    return active;
}

bool term_scrollbar(Term *term, GhosttyTerminalScrollbar *out)
{
    if (!term->handle || !out)
        return false;
    return ghostty_terminal_get(term->handle,
                                GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                out) == GHOSTTY_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Selection                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Grid references are snapshots: they are only valid until the next call
 * that changes the terminal. So every gesture resolves fresh viewport
 * points here and nothing is cached; a selection that has to survive new
 * output is rebuilt by the caller.
 */

static bool viewport_ref(Term *term, int x, int y, GhosttyGridRef *out_ref)
{
    GhosttyPoint point = { 0 };

    if (!term->handle || x < 0 || y < 0)
        return false;
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = (uint16_t)x;
    point.value.coordinate.y = (uint32_t)y;

    return ghostty_terminal_grid_ref(term->handle, point, out_ref)
           == GHOSTTY_SUCCESS;
}

static bool install_selection(Term *term, const GhosttySelection *selection)
{
    if (!term->handle)
        return false;
    return ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SELECTION,
                                selection) == GHOSTTY_SUCCESS;
}

void term_selection_clear(Term *term)
{
    if (!term->handle)
        return;
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SELECTION, NULL);
}

bool term_selection_cells(Term *term, int start_x, int start_y, int end_x,
                          int end_y, bool rectangle)
{
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, start_x, start_y, &selection.start) ||
        !viewport_ref(term, end_x, end_y, &selection.end))
        return false;
    selection.rectangle = rectangle;
    return install_selection(term, &selection);
}

bool term_selection_word(Term *term, int x, int y)
{
    GhosttyTerminalSelectWordOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x, y, &options.ref))
        return false;
    if (ghostty_terminal_select_word(term->handle, &options, &selection)
            != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

/* The word range under an anchor and the drag point. Asking for the
 * nearest word between the two instead of only the one under the pointer
 * is what keeps a word drag from collapsing when the pointer passes
 * between words. */
bool term_selection_word_between(Term *term, int x1, int y1, int x2, int y2)
{
    GhosttyTerminalSelectWordBetweenOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordBetweenOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x1, y1, &options.start) ||
        !viewport_ref(term, x2, y2, &options.end))
        return false;
    if (ghostty_terminal_select_word_between(term->handle, &options,
                                             &selection) != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

bool term_selection_line(Term *term, int x, int y)
{
    GhosttyTerminalSelectLineOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x, y, &options.ref))
        return false;
    if (ghostty_terminal_select_line(term->handle, &options, &selection)
            != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

bool term_selection_has(Term *term)
{
    GhosttySelection selection = { 0 };

    if (!term->handle)
        return false;
    return ghostty_terminal_get(term->handle,
                                GHOSTTY_TERMINAL_DATA_SELECTION,
                                &selection) == GHOSTTY_SUCCESS;
}

char *term_selection_text(Term *term, size_t *out_length)
{
    GhosttySelection selection = { 0 };
    GhosttyFormatter formatter = NULL;
    GhosttyFormatterFormat emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    bool trim = true;
    uint8_t *buffer = NULL;
    size_t length = 0;
    char *text;

    if (out_length)
        *out_length = 0;
    if (!term->handle)
        return NULL;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_SELECTION,
                             &selection) != GHOSTTY_SUCCESS)
        return NULL;

    /* The formatter reads the terminal's own selection, so a wrapped line
     * comes out as the rows the user saw, without the padding. */
    if (ghostty_formatter_terminal_new(NULL, &formatter, term->handle)
            != GHOSTTY_SUCCESS)
        return NULL;
    if (ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_EMIT, &emit)
            != GHOSTTY_SUCCESS ||
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim)
            != GHOSTTY_SUCCESS ||
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_SELECTION,
                              &selection) != GHOSTTY_SUCCESS ||
        ghostty_formatter_format_alloc(formatter, NULL, &buffer, &length)
            != GHOSTTY_SUCCESS) {
        ghostty_formatter_free(formatter);
        return NULL;
    }
    ghostty_formatter_free(formatter);

    text = (char *)malloc(length + 1);
    if (!text) {
        ghostty_free(NULL, buffer, length);
        return NULL;
    }
    if (length > 0)
        memcpy(text, buffer, length);
    text[length] = '\0';
    ghostty_free(NULL, buffer, length);

    if (out_length)
        *out_length = length;
    if (length == 0) {
        free(text);
        return NULL;
    }
    return text;
}
