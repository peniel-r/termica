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

    /* The only effect we act on so far is the window title. */
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_USERDATA, term);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         (const void *)on_title_changed);
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
