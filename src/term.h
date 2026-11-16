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

typedef struct Term {
    GhosttyTerminal               handle;        /* the screen state      */
    GhosttyRenderState            render_state;  /* the frame we draw     */
    GhosttyRenderStateRowIterator row_iterator;  /* walks the rows        */
    GhosttyRenderStateRowCells    cell_iterator; /* walks a row's cells   */
    GhosttyKeyEncoder             key_encoder;   /* key press -> bytes    */
    GhosttyKeyEvent               key_event;     /* the event we fill in  */

    uint16_t columns;
    uint16_t rows;
    uint32_t cell_width;
    uint32_t cell_height;

    /* The program set a new window title (OSC 0 or OSC 2). The app picks
     * this up and applies it to the window. */
    bool title_changed;
    char title[128];
} Term;

/* Create the terminal and everything around it. False on failure. */
bool term_init(Term *term, uint16_t columns, uint16_t rows);

/* Free everything. Safe to call on a zeroed struct. */
void term_free(Term *term);

/* Feed bytes a program wrote. The parser handles every escape sequence:
 * SGR colors and attributes, cursor movement, erase, scroll, OSC, ... */
void term_write(Term *term, const void *data, size_t length);

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

/* Run the self test from the docs. Returns the number of failures. */
int term_selftest(void);

#endif /* TERMICA_TERM_H */
