/*
 * termica - the self test from the docs.
 *
 * https://lib.ghostty.org/vt/first-terminal.html walks through the whole
 * flow: create a terminal, write bytes into it, read the screen back out
 * and draw it. This runs exactly that flow, plus the few extras the app
 * depends on (colors, cursor, wide characters, key encoding, resize), and
 * prints PASS or FAIL for each check.
 *
 * Run it with:  termica --selftest
 */

#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failure_count;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed)
        failure_count++;
}

/* Write a C string into the terminal. */
static void write_string(GhosttyTerminal terminal, const char *text)
{
    ghostty_terminal_vt_write(terminal, (const uint8_t *)text, strlen(text));
}

/* The whole screen as plain text, as the formatter produces it. The caller
 * frees the result with free(). */
static char *screen_text(GhosttyTerminal terminal)
{
    GhosttyFormatter formatter = NULL;
    GhosttyFormatterFormat format = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    bool trim = true;
    uint8_t *buffer = NULL;
    size_t length = 0;
    char *text;

    if (ghostty_formatter_terminal_new(NULL, &formatter, terminal)
            != GHOSTTY_SUCCESS)
        return NULL;
    if (ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_EMIT, &format)
            != GHOSTTY_SUCCESS ||
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim)
            != GHOSTTY_SUCCESS ||
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
    return text;
}

/* Is the given line of the formatted screen exactly the expected text? */
static bool line_is(const char *text, int line_index, const char *expected)
{
    const char *line = text;
    size_t length;
    int index;

    for (index = 0; index < line_index; index++) {
        const char *newline = strchr(line, '\n');
        if (!newline)
            return false;
        line = newline + 1;
    }
    length = strcspn(line, "\n");
    return strlen(expected) == length && memcmp(line, expected, length) == 0;
}

/* What is in one cell of the screen? */
typedef struct CellInfo {
    bool found;
    uint32_t codepoint;
    GhosttyCellWide wide;
    GhosttyStyle style;
    bool has_foreground;
    GhosttyColorRgb foreground;
    bool has_background;
    GhosttyColorRgb background;
} CellInfo;

static bool cell_info(GhosttyRenderState render_state, int column, int row,
                      CellInfo *out)
{
    GhosttyRenderStateRowIterator row_iterator = NULL;
    GhosttyRenderStateRowCells cell_iterator = NULL;
    bool found = false;
    int current_column = 0;

    memset(out, 0, sizeof *out);
    if (ghostty_render_state_row_iterator_new(NULL, &row_iterator)
            != GHOSTTY_SUCCESS)
        return false;
    if (ghostty_render_state_row_cells_new(NULL, &cell_iterator)
            != GHOSTTY_SUCCESS) {
        ghostty_render_state_row_iterator_free(row_iterator);
        return false;
    }
    if (ghostty_render_state_get(render_state,
                                 GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                 &row_iterator) != GHOSTTY_SUCCESS)
        goto done;

    while (!found && ghostty_render_state_row_iterator_next(row_iterator)) {
        int32_t row_position = 0;

        ghostty_render_state_row_get(row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                                     &row_position);
        if (row_position != row)
            continue;
        if (ghostty_render_state_row_get(row_iterator,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                         &cell_iterator) != GHOSTTY_SUCCESS)
            goto done;
        current_column = 0;
        while (ghostty_render_state_row_cells_next(cell_iterator)) {
            GhosttyCell raw_cell = 0;

            if (current_column != column) {
                current_column++;
                continue;
            }
            found = true;
            out->found = true;
            if (ghostty_render_state_row_cells_get(
                    cell_iterator, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &raw_cell) == GHOSTTY_SUCCESS) {
                ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_CODEPOINT,
                                 &out->codepoint);
                ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &out->wide);
            }
            ghostty_render_state_row_cells_get(
                cell_iterator, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                &out->style);
            out->has_foreground = ghostty_render_state_row_cells_get(
                cell_iterator, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                &out->foreground) == GHOSTTY_SUCCESS;
            out->has_background = ghostty_render_state_row_cells_get(
                cell_iterator, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                &out->background) == GHOSTTY_SUCCESS;
            current_column++;
        }
    }

done:
    ghostty_render_state_row_cells_free(cell_iterator);
    ghostty_render_state_row_iterator_free(row_iterator);
    return found;
}

/* The cursor position in cells, as the render state reports it. */
static bool cursor_position(GhosttyRenderState render_state, int *column,
                            int *row)
{
    GhosttyRenderStateCursor cursor =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);

    if (ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_CURSOR,
                                 &cursor) != GHOSTTY_SUCCESS)
        return false;
    if (!cursor.viewport_has_value)
        return false;
    *column = cursor.viewport_x;
    *row = cursor.viewport_y;
    return true;
}

/* ---- the checks ---------------------------------------------------------- */

/* The exact example from the docs. */
static void test_documented_screen(void)
{
    GhosttyTerminal terminal = NULL;
    char *screen;

    if (ghostty_terminal_new(NULL, &terminal, 80, 24) != GHOSTTY_SUCCESS) {
        check(false, "terminal created");
        return;
    }

    write_string(terminal, "Hello, World!\r\n");
    /* ANSI color codes: ESC[1;32m = bold green, ESC[0m = reset */
    write_string(terminal, "\x1b[1;32mGreen Text\x1b[0m\r\n");
    /* Cursor positioning: ESC[1;1H = move to row 1, column 1 */
    write_string(terminal, "\x1b[1;1HTop-left corner\r\n");
    /* Cursor movement: ESC[5B = move down 5 lines */
    write_string(terminal, "\x1b[5B");
    write_string(terminal, "Moved down!\r\n");
    /* Erase line: ESC[2K = clear entire line */
    write_string(terminal, "\x1b[2K");
    write_string(terminal, "New content\r\n");
    write_string(terminal, "Line A\r\nLine B\r\nLine C\r\n");

    screen = screen_text(terminal);
    if (!screen) {
        check(false, "screen formatted");
        ghostty_terminal_free(terminal);
        return;
    }

    /* The cursor moved to row 1 before writing, so "Top-left corner"
     * covers "Hello, World!". */
    check(line_is(screen, 0, "Top-left corner"), "row 0 is 'Top-left corner'");
    check(line_is(screen, 1, "Green Text"), "row 1 is 'Green Text'");
    check(line_is(screen, 6, "Moved down!"), "row 6 is 'Moved down!'");
    check(line_is(screen, 7, "New content"), "row 7 is 'New content'");
    check(line_is(screen, 8, "Line A"), "row 8 is 'Line A'");
    check(line_is(screen, 9, "Line B"), "row 9 is 'Line B'");
    check(line_is(screen, 10, "Line C"), "row 10 is 'Line C'");

    free(screen);
    ghostty_terminal_free(terminal);
}

/* SGR colors and attributes reach the cells. */
static void test_colors_and_attributes(void)
{
    GhosttyTerminal terminal = NULL;
    GhosttyRenderState render_state = NULL;
    GhosttyColorRgb palette[256];
    CellInfo cell;

    if (ghostty_terminal_new(NULL, &terminal, 40, 5) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "render state created");
        return;
    }

    write_string(terminal,
        "\x1b[1;32mG\x1b[0m\x1b[4mU\x1b[0m\x1b[38;2;255;128;0mR\x1b[0m\r\n");
    ghostty_render_state_update(render_state, terminal);
    ghostty_render_state_get(render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLOR_PALETTE, palette);

    if (cell_info(render_state, 0, 0, &cell)) {
        check((cell.style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0, "bold flag");
        /* SGR 32 is palette entry 2. The render state resolves it for us. */
        check(cell.has_foreground && cell.foreground.r == palette[2].r &&
              cell.foreground.g == palette[2].g &&
              cell.foreground.b == palette[2].b,
              "green foreground from the palette");
    } else {
        check(false, "cell 0,0 found");
    }

    if (cell_info(render_state, 1, 0, &cell)) {
        check(GHOSTTY_STYLE_UNDERLINE(cell.style) != GHOSTTY_SGR_UNDERLINE_NONE,
              "underline flag");
    } else {
        check(false, "cell 1,0 found");
    }

    if (cell_info(render_state, 2, 0, &cell)) {
        check(cell.has_foreground && cell.foreground.r == 255 &&
              cell.foreground.g == 128 && cell.foreground.b == 0,
              "truecolor foreground");
    } else {
        check(false, "cell 2,0 found");
    }

    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
}

/* The cursor moves, and the erase sequence clears a line. */
static void test_cursor_and_erase(void)
{
    GhosttyTerminal terminal = NULL;
    GhosttyRenderState render_state = NULL;
    CellInfo cell;
    int column = 0;
    int row = 0;

    if (ghostty_terminal_new(NULL, &terminal, 40, 5) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "terminal created");
        return;
    }

    write_string(terminal, "abc\r\ndef\r\n");
    write_string(terminal, "\x1b[3;5H");
    ghostty_render_state_update(render_state, terminal);
    check(cursor_position(render_state, &column, &row) && column == 4 &&
          row == 2, "cursor at row 3, col 5");

    /* Erase the second line. */
    write_string(terminal, "\x1b[2;1H");
    write_string(terminal, "\x1b[2K");
    ghostty_render_state_update(render_state, terminal);
    check(cell_info(render_state, 0, 1, &cell) && cell.codepoint == 0,
          "erase line clears the row");
    check(cell_info(render_state, 0, 0, &cell) && cell.codepoint == 'a',
          "erase leaves the other rows alone");

    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
}

/* A wide character takes two cells: the head has the text, the tail is a
 * spacer that must not be drawn. */
static void test_wide_character(void)
{
    GhosttyTerminal terminal = NULL;
    GhosttyRenderState render_state = NULL;
    CellInfo head;
    CellInfo tail;

    if (ghostty_terminal_new(NULL, &terminal, 10, 3) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "terminal created");
        return;
    }

    /* U+4E2D, a CJK ideograph: two cells wide. */
    write_string(terminal, "\xe4\xb8\xad");
    ghostty_render_state_update(render_state, terminal);

    if (cell_info(render_state, 0, 0, &head) &&
        cell_info(render_state, 1, 0, &tail)) {
        check(head.codepoint == 0x4E2D, "wide character in the head cell");
        check(head.wide == GHOSTTY_CELL_WIDE_WIDE, "head cell is wide");
        check(tail.wide == GHOSTTY_CELL_WIDE_SPACER_TAIL,
              "tail cell is a spacer");
    } else {
        check(false, "wide character cells found");
    }

    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
}

/* The key encoder turns key presses into the bytes a program expects. */
static void test_key_encoding(void)
{
    Term term;
    uint8_t buffer[32];
    size_t byte_count;

    if (!term_init(&term, 80, 24)) {
        check(false, "terminal created");
        return;
    }

    byte_count = term_encode_key(&term, GHOSTTY_KEY_ACTION_PRESS,
                                 GHOSTTY_KEY_ARROW_UP, 0, 0, buffer,
                                 sizeof buffer);
    check(byte_count == 3 && buffer[0] == 0x1b && buffer[1] == '[' &&
          buffer[2] == 'A', "arrow up is CSI A");

    byte_count = term_encode_key(&term, GHOSTTY_KEY_ACTION_PRESS,
                                 GHOSTTY_KEY_A, GHOSTTY_MODS_CTRL, 'a',
                                 buffer, sizeof buffer);
    check(byte_count == 1 && buffer[0] == 0x01, "ctrl+a is 0x01");

    byte_count = term_encode_key(&term, GHOSTTY_KEY_ACTION_PRESS,
                                 GHOSTTY_KEY_ENTER, 0, 0, buffer,
                                 sizeof buffer);
    check(byte_count == 1 && buffer[0] == '\r', "enter is CR");

    byte_count = term_encode_key(&term, GHOSTTY_KEY_ACTION_PRESS,
                                 GHOSTTY_KEY_BACKSPACE, 0, 0, buffer,
                                 sizeof buffer);
    check(byte_count == 1 && buffer[0] == 0x7f, "backspace is DEL");

    byte_count = term_encode_key(&term, GHOSTTY_KEY_ACTION_PRESS,
                                 GHOSTTY_KEY_A, 0, 'a', buffer,
                                 sizeof buffer);
    check(byte_count == 0, "a plain letter produces no bytes");

    term_free(&term);
}

/* Resizing the terminal changes the grid. */
static void test_resize(void)
{
    Term term;
    GhosttyRenderState render_state = NULL;
    uint16_t columns = 0;
    uint16_t rows = 0;

    if (!term_init(&term, 80, 24) ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "terminal created");
        return;
    }

    term_resize(&term, 10, 5, 8, 16);
    ghostty_render_state_update(render_state, term.handle);

    ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_COLS,
                             &columns);
    ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_ROWS,
                             &rows);
    check(columns == 10 && rows == 5, "grid resized to 10x5");

    ghostty_render_state_free(render_state);
    term_free(&term);
}

/* The program sets a window title with OSC 2. */
static void test_title(void)
{
    Term term;

    if (!term_init(&term, 80, 24)) {
        check(false, "terminal created");
        return;
    }

    term_write(&term, "\x1b]2;termica - test\x07", 19);
    check(term.title_changed && strcmp(term.title, "termica - test") == 0,
          "window title from OSC 2");

    term_free(&term);
}

int term_selftest(void)
{
    printf("termica self test (libghostty-vt)\n\n");

    test_documented_screen();
    test_colors_and_attributes();
    test_cursor_and_erase();
    test_wide_character();
    test_key_encoding();
    test_resize();
    test_title();

    printf("\n%s (%d failure%s)\n", failure_count ? "FAILED" : "OK",
           failure_count, failure_count == 1 ? "" : "s");
    return failure_count;
}
