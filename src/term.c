/*
 * termica - the terminal half of the app. See term.h.
 */

#include "term.h"

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
