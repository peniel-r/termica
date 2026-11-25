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
#include "pty.h"
#include "chrome.h"
#include "embed.h"
#include "font_lookup.h"
#include "git.h"
#include "desktop_signal.h"
#include "diagnostics.h"
#include "hyperlink.h"
#include "glyph.h"
#include "image.h"
#include "input.h"
#include "keys.h"
#include "settings.h"
#include "statusbar.h"
#include "style.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int config_test(void);
int search_selftest(void);
int glyph_atlas_selftest(void);

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

#ifdef _WIN32
/* Run one command through a real ConPTY and collect what came back. The
 * shared helper is what lets the checks below stay small: spawn, drain,
 * report. Returns false when the child could not be started, with the
 * failure text in error. */
static bool pty_run(const wchar_t *command, char *output, size_t capacity,
                    uint32_t *exit_code, char *error, size_t error_size)
{
    Pty    *pty;
    size_t  used = 0;
    uint32_t code = 0;
    bool    exited = false;
    int     spins;

    output[0] = '\0';
    if (exit_code)
        *exit_code = 0;

    pty = pty_spawn(command, 80, 24, NULL, NULL, error, error_size);
    if (!pty)
        return false;

    for (spins = 0; spins < 200; spins++) {
        uint8_t chunk[2048];
        size_t  got = pty_read(pty, chunk, sizeof chunk);

        if (got > 0) {
            size_t room = capacity - 1 - used;
            size_t take = got < room ? got : room;

            if (take > 0) {
                memcpy(output + used, chunk, take);
                used += take;
                output[used] = '\0';
            }
        }
        if (pty_exit_code(pty, &code))
            exited = true;
        /* conhost keeps the pseudoconsole session (and its output pipe)
         * alive until ClosePseudoConsole, so EOF is not the exit signal:
         * stop once the child is gone and the pipe has been quiet for a
         * moment. */
        if (exited && !pty_wait(pty, 100))
            break;
        if (!exited)
            pty_wait(pty, 100);
    }

    if (exit_code)
        *exit_code = code;
    pty_close(pty);
    return exited;
}

/* The only tests that touch a real child process: they prove the pipes, the
 * reader thread, the exit code and the close path work. */
static void test_pty(void)
{
    char     error[256] = "";
    char     output[4096];
    uint32_t code = 0;
    bool     ran;

    ran = pty_run(L"cmd.exe /c \"echo termica-pty-ok\"", output,
                  sizeof output, &code, error, sizeof error);
    if (!ran && error[0]) {
        check(false, "pty spawn");
        printf("      %s\n", error);
        return;
    }
    check(strstr(output, "termica-pty-ok") != NULL,
          "pty output reaches the terminal");
    check(code == 0, "pty child exits with code 0");
    if (strstr(output, "termica-pty-ok") == NULL)
        printf("      output was: %s\n", output);

    /* The codepage question milestone 2 left open. A non-ASCII character
     * that arrives in the child's command line has to come back as the same
     * UTF-8 bytes, whatever the console's own codepage is; if it were
     * translated through an OEM page the sequence below would not match. */
    (void)pty_run(L"cmd.exe /c \"echo caf\u00E9\"", output, sizeof output,
                  &code, error, sizeof error);
    check(strstr(output, "caf\xC3\xA9") != NULL,
          "a non-ASCII argument round-trips as UTF-8");
    if (strstr(output, "caf\xC3\xA9") == NULL)
        printf("      output was: %s\n", output);
}
#endif /* _WIN32 */

/* The text of one viewport row, as the render state reports it, with the
 * trailing blanks removed. */
static void row_text(GhosttyRenderState render_state, int row, char *out,
                     size_t capacity)
{
    GhosttyRenderStateRowIterator rows = NULL;
    GhosttyRenderStateRowCells cells = NULL;
    size_t used = 0;

    out[0] = '\0';
    if (ghostty_render_state_row_iterator_new(NULL, &rows) != GHOSTTY_SUCCESS)
        return;
    if (ghostty_render_state_row_cells_new(NULL, &cells) != GHOSTTY_SUCCESS) {
        ghostty_render_state_row_iterator_free(rows);
        return;
    }
    ghostty_render_state_get(render_state,
                             GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows);

    while (ghostty_render_state_row_iterator_next(rows)) {
        int32_t row_position = 0;

        ghostty_render_state_row_get(rows,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                                     &row_position);
        if (row_position != row)
            continue;
        ghostty_render_state_row_get(rows,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);
        while (ghostty_render_state_row_cells_next(cells)) {
            uint8_t bytes[64];
            GhosttyBuffer grapheme = { .ptr = bytes, .cap = sizeof bytes };

            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                &grapheme);
            if (grapheme.len > 0 && used + grapheme.len + 1 < capacity) {
                memcpy(out + used, grapheme.ptr, grapheme.len);
                used += grapheme.len;
                out[used] = '\0';
            }
        }
        break;
    }
    while (used > 0 && out[used - 1] == ' ')
        out[--used] = '\0';

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
}

/* The viewport shows the bottom of the screen; scrolling moves it into the
 * history without the program writing anything. */
static void test_scrollback(void)
{
    Term term;
    GhosttyRenderState render_state = NULL;
    GhosttyTerminalScrollbar bar;
    char line[16];
    char text[128];
    int index;

    if (!term_init(&term, 80, 24) ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "scrollback terminal created");
        return;
    }

    /* Exactly 30 rows, so 6 of them are history for a 24-row grid. */
    for (index = 1; index <= 30; index++) {
        snprintf(line, sizeof line, "L%02d", index);
        term_write(&term, line, strlen(line));
        if (index < 30)
            term_write(&term, "\r\n", 2);
    }

    check(term_viewport_at_bottom(&term), "the viewport follows the bottom");
    ghostty_render_state_update(render_state, term.handle);
    row_text(render_state, 0, text, sizeof text);
    check(strcmp(text, "L07") == 0, "the bottom viewport starts at L07");
    if (strcmp(text, "L07") != 0)
        printf("      row 0 was: %s\n", text);

    term_scroll_rows(&term, -3);
    check(!term_viewport_at_bottom(&term), "scrolling up leaves the bottom");
    ghostty_render_state_update(render_state, term.handle);
    row_text(render_state, 0, text, sizeof text);
    check(strcmp(text, "L04") == 0, "three rows up shows L04");
    if (strcmp(text, "L04") != 0)
        printf("      row 0 was: %s\n", text);

    if (term_scrollbar(&term, &bar)) {
        check(bar.total >= 30 && bar.len == 24 && bar.offset == 3,
              "the scrollbar reports total, length and offset");
        if (!(bar.total >= 30 && bar.len == 24 && bar.offset == 3))
            printf("      total=%llu len=%llu offset=%llu\n",
                   (unsigned long long)bar.total,
                   (unsigned long long)bar.len,
                   (unsigned long long)bar.offset);
    } else {
        check(false, "the scrollbar reports total, length and offset");
    }

    term_scroll_top(&term);
    ghostty_render_state_update(render_state, term.handle);
    row_text(render_state, 0, text, sizeof text);
    check(strcmp(text, "L01") == 0, "scroll to top shows the first row");

    term_scroll_bottom(&term);
    check(term_viewport_at_bottom(&term), "scroll to bottom follows again");
    ghostty_render_state_update(render_state, term.handle);
    row_text(render_state, 0, text, sizeof text);
    check(strcmp(text, "L07") == 0, "the bottom viewport is back");

    ghostty_render_state_free(render_state);
    term_free(&term);
}

/* Selecting cells, words and lines, and reading the text back out. */
static void test_selection(void)
{
    Term term;
    char *text;

    if (!term_init(&term, 80, 24)) {
        check(false, "selection terminal created");
        return;
    }
    term_write(&term, "hello world second\r\n", 20);

    check(!term_selection_has(&term), "nothing is selected at first");

    if (term_selection_cells(&term, 0, 0, 4, 0, false)) {
        check(term_selection_has(&term), "a cell range is installed");
        text = term_selection_text(&term, NULL);
        check(text && strcmp(text, "hello") == 0, "cells 0..4 copy as hello");
        if (text && strcmp(text, "hello") != 0)
            printf("      selection text: %s\n", text);
        free(text);
    } else {
        check(false, "a cell range is installed");
    }

    if (term_selection_word(&term, 6, 0)) {
        text = term_selection_text(&term, NULL);
        check(text && strcmp(text, "world") == 0,
              "a double click copies the whole word");
        free(text);
    } else {
        check(false, "a word can be selected");
    }

    if (term_selection_line(&term, 13, 0)) {
        text = term_selection_text(&term, NULL);
        check(text && strcmp(text, "hello world second") == 0,
              "a triple click copies the whole line");
        free(text);
    } else {
        check(false, "a line can be selected");
    }

    /* A rectangular selection takes only the columns inside the block:
     * row 0 "hello world second" columns 1..3 is "ell", row 1 "abcdef"
     * columns 1..3 is "bcd". */
    term_write(&term, "abcdef\r\n", 8);
    if (term_selection_cells(&term, 1, 0, 3, 1, true)) {
        text = term_selection_text(&term, NULL);
        check(text && strcmp(text, "ell\nbcd") == 0,
              "a block selection takes the columns only");
        if (text && strcmp(text, "ell\nbcd") != 0)
            printf("      block text: %s\n", text);
        free(text);
    } else {
        check(false, "a block can be selected");
    }

    term_selection_clear(&term);
    check(!term_selection_has(&term), "clearing removes the selection");
    check(term_selection_text(&term, NULL) == NULL,
          "an empty selection has no text");

    term_free(&term);
}

/* The mouse encoder follows the program's tracking modes, and focus
 * events come from mode 1004. */
static void test_mouse_and_focus(void)
{
    Term term;
    uint8_t buffer[128];
    size_t bytes;

    if (!term_init(&term, 80, 24)) {
        check(false, "mouse terminal created");
        return;
    }

    check(!term_mouse_tracking(&term), "no mouse tracking by default");
    check(!term_focus_reporting(&term), "no focus reporting by default");

    /* Normal tracking, SGR coordinates, focus events. */
    write_string(term.handle, "\x1b[?1000h\x1b[?1006h\x1b[?1004h");
    check(term_mouse_tracking(&term), "mode 1000 turns tracking on");
    check(term_focus_reporting(&term), "mode 1004 turns focus reporting on");

    /* One cell is 8x16 pixels and the grid fills the window, so the
     * pointer at (8.5, 16.5) sits in column 2, row 2 (1-based). */
    term_resize(&term, 80, 24, 8, 16);
    term_set_geometry(&term, 640, 384, 0, 0, 0, 0);
    term_set_mouse_button(&term, true);

    bytes = term_encode_mouse(&term, GHOSTTY_MOUSE_ACTION_PRESS,
                              GHOSTTY_MOUSE_BUTTON_LEFT, 0, 8.5f, 16.5f,
                              buffer, sizeof buffer);
    check(bytes > 0 && memcmp(buffer, "\x1b[<0;2;2M", 9) == 0,
          "a press is SGR-encoded with the right cell");
    if (!(bytes > 0 && memcmp(buffer, "\x1b[<0;2;2M", 9) == 0)) {
        if (bytes > 0 && bytes < sizeof buffer) {
            buffer[bytes] = '\0';
            printf("      press bytes: %s\n", (char *)buffer);
        } else {
            printf("      press produced %lu bytes\n",
                   (unsigned long)bytes);
        }
    }

    bytes = term_encode_mouse(&term, GHOSTTY_MOUSE_ACTION_RELEASE,
                              GHOSTTY_MOUSE_BUTTON_LEFT, 0, 8.5f, 16.5f,
                              buffer, sizeof buffer);
    check(bytes > 0 && memcmp(buffer, "\x1b[<0;2;2m", 9) == 0,
          "a release ends with the lowercase code");

    term_set_mouse_button(&term, false);
    bytes = term_encode_mouse(&term, GHOSTTY_MOUSE_ACTION_MOTION,
                              GHOSTTY_MOUSE_BUTTON_UNKNOWN, 0, 24.0f, 40.0f,
                              buffer, sizeof buffer);
    check(bytes == 0, "a hover motion is not reported under mode 1000");

    term_write(&term, "\x1b[?1003h", 8);
    bytes = term_encode_mouse(&term, GHOSTTY_MOUSE_ACTION_MOTION,
                              GHOSTTY_MOUSE_BUTTON_UNKNOWN, 0, 24.0f, 40.0f,
                              buffer, sizeof buffer);
    check(bytes > 0, "any-event tracking reports motion");

    bytes = term_encode_focus(&term, true, buffer, sizeof buffer);
    check(bytes == 3 && memcmp(buffer, "\x1b[I", 3) == 0,
          "focus gained is CSI I");
    bytes = term_encode_focus(&term, false, buffer, sizeof buffer);
    check(bytes == 3 && memcmp(buffer, "\x1b[O", 3) == 0,
          "focus lost is CSI O");

    term_free(&term);
}

/* The underline kind has to survive the style layer: a spell-check wave and
 * a hard error are different things and must not draw the same. */
static void test_underline_styles(void)
{
    Term term;
    GhosttyRenderState render_state = NULL;
    GhosttyRenderStateColors colors =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    struct { const char *sequence; int expect; const char *what; } cases[] = {
        { "\x1b[4m",   GHOSTTY_SGR_UNDERLINE_SINGLE, "plain SGR 4 is a single line" },
        { "\x1b[4:3m", GHOSTTY_SGR_UNDERLINE_CURLY,  "SGR 4:3 is curly" },
        { "\x1b[4:4m", GHOSTTY_SGR_UNDERLINE_DOTTED, "SGR 4:4 is dotted" },
        { "\x1b[4:5m", GHOSTTY_SGR_UNDERLINE_DASHED, "SGR 4:5 is dashed" },
        { "\x1b[4:2m", GHOSTTY_SGR_UNDERLINE_DOUBLE, "SGR 4:2 is double" },
    };
    size_t index;

    if (!term_init(&term, 80, 24) ||
        ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS) {
        check(false, "underline terminal created");
        return;
    }

    for (index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        CellStyle style;
        CellInfo cell;
        bool passed;

        memset(&style, 0, sizeof style);
        /* Reset, home, then one character at (0,0): every case has to be
         * read back from the same place. */
        term_write(&term, "\x1b[0m\x1b[H", strlen("\x1b[0m\x1b[H"));
        term_write(&term, cases[index].sequence, strlen(cases[index].sequence));
        term_write(&term, "x", 1);
        ghostty_render_state_update(render_state, term.handle);
        ghostty_render_state_get(render_state,
                                 GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);

        passed = cell_info(render_state, 0, 0, &cell);
        if (passed) {
            memset(&style, 0, sizeof style);
            style_resolve(&style, &cell.style, cell.has_foreground,
                          cell.foreground, cell.has_background,
                          cell.background, &colors,
                          style_color(colors.foreground, 1.0f),
                          style_color(colors.background, 1.0f), 1.0f, false);
            passed = style.underline &&
                     style.underline_style == cases[index].expect;
        }
        check(passed, cases[index].what);
        if (!passed)
            printf("      got underline=%d style=%d, wanted %d\n",
                   (int)style.underline, style.underline_style,
                   cases[index].expect);
    }

    /* No underline at all must not leave a stale style behind. */
    term_write(&term, "\x1b[0m\x1b[H", strlen("\x1b[0m\x1b[H"));
    term_write(&term, "y", 1);
    ghostty_render_state_update(render_state, term.handle);
    ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_COLORS,
                             &colors);
    {
        CellStyle style;
        CellInfo cell;

        memset(&style, 0, sizeof style);
        if (cell_info(render_state, 0, 0, &cell)) {
            style_resolve(&style, &cell.style, cell.has_foreground,
                          cell.foreground, cell.has_background,
                          cell.background, &colors,
                          style_color(colors.foreground, 1.0f),
                          style_color(colors.background, 1.0f), 1.0f, false);
            check(!style.underline, "reset clears the underline");
        } else {
            check(false, "reset clears the underline");
        }
    }

    ghostty_render_state_free(render_state);
    term_free(&term);
}

/* The settings file: the TOML sections, their types and bounds, key folding,
 * and a format round trip. Nothing here needs a window or the disk. */
static void test_settings(void)
{
    Settings settings;
    Settings again;
    char text[4096];
    char long_font[700];
    const char *document =
        "# a comment\n"
        "[font]\n"
        "face = 'C:\\fonts\\Maple Mono.ttf'\n"
        "size = 13\n"
        "\n"
        "[window]\n"
        "opacity = 0.5\n"
        "backdrop = \"mica\"\n"
        "decoration = \"compact\"\n"
        "title_bar = true\n"
        "\n"
        "[terminal]\n"
        "copy_on_select = true\n"
        "shell = \"cmd\"\n"
        "osc52 = \"deny\"\n";

    settings_default(&settings);
    check(settings.point_size == 11.0f, "default point size is 11");
    check(settings.panel_opacity == 1.0f, "default opacity is 1.0");
    check(settings.backdrop == 1, "default backdrop is mica");
    check(settings.shell == SETTINGS_SHELL_AUTO, "default shell is auto");
    check(settings.osc52 == SETTINGS_OSC52_DENY, "default osc52 is deny");
    check(settings.font[0] == '\0', "default font is the built-in one");
    check(settings.decoration == SETTINGS_DECORATION_NATIVE,
          "default decoration is native");
    check(settings.title_bar, "default title bar is on");

    check(settings_parse_toml(&settings, document, strlen(document)),
          "a full TOML document parses");
    check(strcmp(settings.font, "C:\\fonts\\Maple Mono.ttf") == 0,
          "font face is stored");
    check(settings.point_size == 13.0f, "size is stored");
    check(settings.panel_opacity == 0.5f, "opacity is stored");
    check(settings.backdrop == 1, "backdrop is stored");
    check(settings.decoration == SETTINGS_DECORATION_COMPACT,
          "decoration is stored");
    check(settings.title_bar, "title bar is stored");
    check(settings.copy_on_select, "copy-on-select is stored");
    check(settings.shell == SETTINGS_SHELL_CMD, "shell is stored");
    check(settings.osc52 == SETTINGS_OSC52_DENY, "osc52 is stored");

    check(settings_parse_toml(&settings, "[font]\nsize = 13.5\n",
                              strlen("[font]\nsize = 13.5\n")),
          "a float size parses");
    check(settings.point_size == 13.5f, "float size is stored");

    check(!settings_parse_toml(&settings, "[font]\nsize = 0\n",
                               strlen("[font]\nsize = 0\n")),
          "a zero size is refused");
    check(settings.point_size == 13.5f, "a refused value left it alone");
    check(!settings_parse_toml(&settings, "[font]\nsize = 900\n",
                               strlen("[font]\nsize = 900\n")),
          "a huge size is refused");
    check(!settings_parse_toml(&settings, "[font]\nsize = nan\n",
                               strlen("[font]\nsize = nan\n")),
          "a non-finite size is refused");
    check(!settings_parse_toml(&settings, "[font]\nsize = \"big\"\n",
                               strlen("[font]\nsize = \"big\"\n")),
          "a non-numeric size is refused");

    check(!settings_parse_toml(&settings, "[window]\nopacity = 2\n",
                               strlen("[window]\nopacity = 2\n")),
          "opacity bounds");
    check(!settings_parse_toml(&settings, "[window]\nopacity = nan\n",
                               strlen("[window]\nopacity = nan\n")),
          "a non-finite opacity is refused");
    check(!settings_parse_toml(&settings, "[window]\nopacity = \"half\"\n",
                               strlen("[window]\nopacity = \"half\"\n")),
          "a non-numeric opacity is refused");
    check(settings.panel_opacity == 0.5f, "a refused opacity left it alone");

    check(settings_parse_toml(&settings,
                              "[terminal]\ncopy_on_select = false\n",
                              strlen("[terminal]\ncopy_on_select = false\n")),
          "copy-on-select parses");
    check(!settings.copy_on_select, "copy-on-select stored");
    check(!settings_parse_toml(&settings,
                               "[terminal]\ncopy_on_select = \"yes\"\n",
                               strlen("[terminal]\ncopy_on_select = \"yes\"\n")),
          "a non-boolean copy-on-select is refused");

    check(settings_parse_toml(&settings,
                              "[window]\nbackdrop = \"mica_alt\"\n",
                              strlen("[window]\nbackdrop = \"mica_alt\"\n")),
          "mica_alt parses");
    check(settings.backdrop == 2, "mica_alt stored");
    check(!settings_parse_toml(&settings,
                               "[window]\nbackdrop = \"bogus\"\n",
                               strlen("[window]\nbackdrop = \"bogus\"\n")),
          "an unknown backdrop is refused");
    check(settings.backdrop == 2, "a refused backdrop left it alone");

    check(settings_parse_toml(&settings, "[window]\ndecoration = \"square\"\n",
                              strlen("[window]\ndecoration = \"square\"\n")),
          "square decoration parses");
    check(settings.decoration == SETTINGS_DECORATION_SQUARE,
          "square decoration stored");
    check(settings_parse_toml(&settings, "[window]\ndecoration = \"native\"\n",
                              strlen("[window]\ndecoration = \"native\"\n")),
          "native decoration parses");
    check(settings.decoration == SETTINGS_DECORATION_NATIVE,
          "native decoration stored");
    check(!settings_parse_toml(&settings,
                               "[window]\ndecoration = \"fancy\"\n",
                               strlen("[window]\ndecoration = \"fancy\"\n")),
          "an unknown decoration is refused");
    check(settings.decoration == SETTINGS_DECORATION_NATIVE,
          "a refused decoration left it alone");
    check(!settings_parse_toml(&settings, "[window]\ndecoration = 2\n",
                               strlen("[window]\ndecoration = 2\n")),
          "a non-string decoration is refused");
    check(!settings_parse_toml(&settings, "[window]\ntitle_bar = \"yes\"\n",
                               strlen("[window]\ntitle_bar = \"yes\"\n")),
          "a non-boolean title bar is refused");
    check(settings_parse_toml(&settings, "[WINDOW]\nTitleBar = false\n",
                              strlen("[WINDOW]\nTitleBar = false\n")),
          "the title bar key folds case and separators");
    check(!settings.title_bar, "the folded title bar is stored");

    check(settings_parse_toml(&settings, "[terminal]\nshell = \"pwsh\"\n",
                              strlen("[terminal]\nshell = \"pwsh\"\n")),
          "shell parses");
    check(settings.shell == SETTINGS_SHELL_PWSH, "shell stored");
    check(!settings_parse_toml(&settings, "[terminal]\nshell = \"bash\"\n",
                               strlen("[terminal]\nshell = \"bash\"\n")),
          "an unknown shell is refused");
    check(settings_parse_toml(&settings, "[terminal]\nosc52 = \"allow\"\n",
                              strlen("[terminal]\nosc52 = \"allow\"\n")),
          "osc52 parses");
    check(settings.osc52 == SETTINGS_OSC52_ALLOW, "osc52 stored");
    check(!settings_parse_toml(&settings, "[terminal]\nosc52 = \"maybe\"\n",
                               strlen("[terminal]\nosc52 = \"maybe\"\n")),
          "an unknown osc52 policy is refused");

    check(settings_parse_toml(&settings,
                              "[FONT]\nFace = 'C:\\folded.ttf'\nSize = 14\n",
                              strlen("[FONT]\nFace = 'C:\\folded.ttf'\nSize = 14\n")),
          "section and key names fold case");
    check(strcmp(settings.font, "C:\\folded.ttf") == 0, "folded face stored");
    check(settings.point_size == 14.0f, "folded size stored");
    check(settings_parse_toml(&settings,
                              "[TERMINAL]\nCopyOnSelect = true\n",
                              strlen("[TERMINAL]\nCopyOnSelect = true\n")),
          "dash and underscore folding still works");
    check(settings.copy_on_select, "folded copy-on-select stored");

    check(!settings_parse_toml(&settings,
                               "nonsense = 1\n[other]\nfoo = \"bar\"\n",
                               strlen("nonsense = 1\n[other]\nfoo = \"bar\"\n")),
          "unknown keys and tables are ignored");
    check(!settings_parse_toml(&settings, "# only a comment\n",
                               strlen("# only a comment\n")),
          "a comment-only document changes nothing");

    settings_default(&settings);
    check(!settings_parse_toml(&settings, "[font\nsize = 1\n",
                               strlen("[font\nsize = 1\n")),
          "malformed TOML is refused");
    check(settings.point_size == 11.0f, "malformed TOML changes nothing");

    check(settings_parse_toml(&settings, "[font]\nface = \"\"\n",
                              strlen("[font]\nface = \"\"\n")),
          "an empty font face parses");
    check(settings.font[0] == '\0', "an empty font face is stored");

    memset(long_font, 'a', 600);
    long_font[600] = '\0';
    snprintf(text, sizeof text, "[font]\nface = \"%s\"\n", long_font);
    check(!settings_parse_toml(&settings, text, strlen(text)),
          "an oversized font face is refused");
    check(settings.font[0] == '\0', "the oversized face left the font alone");

    check(settings_parse_toml(&settings, "[font]\npath = \"old.ttf\"\n",
                              strlen("[font]\npath = \"old.ttf\"\n")),
          "the old path key still parses");
    check(strcmp(settings.font, "old.ttf") == 0,
          "the old path key is still stored");

    settings_default(&settings);
    settings.point_size = 13.5f;
    settings.panel_opacity = 0.85f;
    settings.backdrop = 3;
    settings.decoration = SETTINGS_DECORATION_SQUARE;
    settings.title_bar = true;
    settings.copy_on_select = true;
    settings.shell = SETTINGS_SHELL_PWSH;
    settings.osc52 = SETTINGS_OSC52_ALLOW;
    strcpy(settings.font, "C:\\fonts\\A \"B\".ttf");
    settings_format(&settings, text, sizeof text);
    check(strstr(text, "[font]") != NULL, "the formatted file has sections");
    check(strstr(text, "face = ") != NULL, "the formatted file uses face");
    check(strstr(text, "path = ") == NULL, "the formatted file has no path");
    check(strstr(text, "C:\\\\fonts") != NULL, "backslashes are escaped");

    settings_default(&again);
    check(settings_parse_toml(&again, text, strlen(text)),
          "the formatted TOML reparses");
    check(again.point_size == settings.point_size &&
          again.panel_opacity == settings.panel_opacity &&
          again.backdrop == settings.backdrop &&
          again.decoration == settings.decoration &&
          again.title_bar == settings.title_bar &&
          again.copy_on_select == settings.copy_on_select &&
          again.shell == settings.shell && again.osc52 == settings.osc52 &&
          strcmp(again.font, settings.font) == 0,
          "every setting survives the round trip");
}

/* What a program asked for the clipboard. The library decodes the payload
 * and hands the app the text, so this checks the copy, the clear request
 * and that the app's answer is what reaches the terminal. */
static char clipboard_text[512];
static size_t clipboard_calls;
static int clipboard_answer = TERM_CLIPBOARD_ALLOWED;

static TermClipboardResult record_clipboard(void *userdata, const char *text,
                                            size_t length, const char *name)
{
    (void)userdata;
    (void)name;
    clipboard_calls++;
    if (length >= sizeof clipboard_text)
        length = sizeof clipboard_text - 1;
    memcpy(clipboard_text, text, length);
    clipboard_text[length] = '\0';
    return (TermClipboardResult)clipboard_answer;
}

static void term_write_text(Term *term, const char *text)
{
    term_write(term, text, strlen(text));
}

static void test_clipboard_write(void)
{
    Term term;

    if (!term_init(&term, 80, 24)) {
        check(false, "clipboard terminal created");
        return;
    }
    term_set_clipboard_write(&term, record_clipboard, NULL);

    clipboard_calls = 0;
    clipboard_text[0] = '\0';
    clipboard_answer = TERM_CLIPBOARD_ALLOWED;
    term_write_text(&term, "\x1b]52;c;b3NjNTItd29ya3M=\x07");
    check(clipboard_calls == 1, "an OSC 52 write reaches the app");
    check(strcmp(clipboard_text, "osc52-works") == 0,
          "the payload arrives decoded");
    if (strcmp(clipboard_text, "osc52-works") != 0)
        printf("      got: [%s]\n", clipboard_text);

    /* Multiple lines survive the round trip. */
    clipboard_calls = 0;
    term_write_text(&term, "\x1b]52;c;dHdvDQpsaW5lcw==\x07");
    check(clipboard_calls == 1 &&
              strcmp(clipboard_text, "two\r\nlines") == 0,
          "a multi-line payload is kept whole");

    /* A clear request comes through as empty text, not as no call. */
    clipboard_calls = 0;
    clipboard_text[0] = 'x';
    term_write_text(&term, "\x1b]52;c;\x07");
    check(clipboard_calls == 1 && clipboard_text[0] == '\0',
          "an empty payload asks to clear");

    /* Denying must not crash: the library's reply path is what changes. */
    clipboard_answer = TERM_CLIPBOARD_DENIED;
    clipboard_calls = 0;
    term_write_text(&term, "\x1b]52;c;b3NjNTItd29ya3M=\x07");
    check(clipboard_calls == 1, "a denied write is still offered to the app");

    /* Without a handler the request is ignored rather than fatal. */
    term_set_clipboard_write(&term, NULL, NULL);
    term_write_text(&term, "\x1b]52;c;b3NjNTItd29ya3M=\x07");
    check(true, "no handler means no crash");

    term_free(&term);
}

static char clipboard_reply[1024];
static size_t clipboard_reply_length;
static bool clipboard_read_allowed;

static void record_terminal_reply(void *userdata, const void *data, size_t length)
{
    size_t available = sizeof clipboard_reply - 1 - clipboard_reply_length;
    (void)userdata;
    if (length > available)
        length = available;
    memcpy(clipboard_reply + clipboard_reply_length, data, length);
    clipboard_reply_length += length;
    clipboard_reply[clipboard_reply_length] = '\0';
}

static TermClipboardResult test_clipboard_read_handler(void *userdata,
                                                       const char *name,
                                                       bool granted, char **text)
{
    (void)userdata;
    (void)name;
    (void)granted;
    if (!clipboard_read_allowed)
        return TERM_CLIPBOARD_DENIED;
    *text = SDL_strdup("clipboard secret");
    return *text ? TERM_CLIPBOARD_ALLOWED : TERM_CLIPBOARD_UNSUPPORTED;
}

static void test_terminal_effects(void)
{
    Term term;
    TermNotification notification;
    size_t limit = 0;

    if (!term_init(&term, 80, 24)) {
        check(false, "initialize terminal for effects");
        return;
    }
    term_set_writer(&term, record_terminal_reply, NULL);
    term_set_clipboard_read(&term, test_clipboard_read_handler, NULL);
    clipboard_read_allowed = true;
    clipboard_reply_length = 0;
    clipboard_reply[0] = '\0';
    term_write_text(&term, "\x1b]52;c;?\x07");
    check(strstr(clipboard_reply, "Y2xpcGJvYXJkIHNlY3JldA==") != NULL,
          "allowed OSC 52 read returns the clipboard text");
    clipboard_read_allowed = false;
    clipboard_reply_length = 0;
    clipboard_reply[0] = '\0';
    term_write_text(&term, "\x1b]52;c;?\x07");
    check(strstr(clipboard_reply, "Y2xpcGJvYXJkIHNlY3JldA==") == NULL,
          "denied OSC 52 read never returns the clipboard text");

    check(term_set_scrollback_lines(&term, 1000) &&
              ghostty_terminal_get(term.handle, GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_LINES,
                                   &limit) == GHOSTTY_SUCCESS && limit == 1000,
          "configured scrollback line limit reaches the terminal");
    term_write_text(&term, "\x1b]777;notify;Title;Body\x07");
    check(term_take_notification(&term, &notification) &&
              strcmp(notification.title, "Title") == 0 &&
              strcmp(notification.body, "Body") == 0,
          "OSC 777 notification is queued with its title and body");
    term_write_text(&term, "\x1b]9;4;1;42\x07");
    check(term.progress_changed &&
              term.progress.state == GHOSTTY_TERMINAL_PROGRESS_STATE_SET &&
              term.progress.progress == 42,
          "OSC 9;4 progress reaches the terminal callback");
    term_free(&term);
}

/* Kitty graphics: the library parses the protocol, stores the pixels and
 * reports where to draw them. This checks the path the app depends on,
 * including the PNG decoder that term_init installs. */
static void test_kitty_graphics(void)
{
    Term term;
    GhosttyKittyGraphics graphics = NULL;
    GhosttyKittyGraphicsImage image;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGB;
    GhosttyString pixels = { 0 };
    GhosttyImageProtocol protocol = GHOSTTY_IMAGE_PROTOCOL_KITTY;
    uint32_t width = 0, height = 0, image_id = 0;
    uint64_t generation = 0, next_ms = 0;
    bool placed = false;

    if (!term_init(&term, 20, 5)) {
        check(false, "initialize terminal for kitty graphics");
        return;
    }
    term_resize(&term, 20, 5, 8, 16);

    /* A 2x2 RGBA image, transmitted in one chunk and displayed at the
     * cursor. */
    term_write_text(&term,
        "\033_Ga=T,f=32,s=2,v=2,i=7;/wAA/wD/AP8AAP///////w==\033\\");
    check(ghostty_terminal_get(term.handle, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
                               &graphics) == GHOSTTY_SUCCESS && graphics,
          "kitty graphics storage is available");
    image = graphics ? ghostty_kitty_graphics_image(graphics, 7) : NULL;
    check(image != NULL, "raw RGBA image is stored");
    if (image &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH, &width) == GHOSTTY_SUCCESS &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT, &height) == GHOSTTY_SUCCESS &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_FORMAT, &format) == GHOSTTY_SUCCESS &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_PIXELS, &pixels) == GHOSTTY_SUCCESS &&
        width == 2 && height == 2 && format == GHOSTTY_KITTY_IMAGE_FORMAT_RGBA &&
        pixels.ptr && pixels.len >= 16)
        check(true, "stored image is 2x2 RGBA with readable pixels");
    else
        check(false, "stored image is 2x2 RGBA with readable pixels");

    /* The same path with a PNG payload, which only decodes because term_init
     * installed SDL's decoder with ghostty_sys_set. */
    term_write_text(&term,
        "\033_Ga=T,f=100,i=8;"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
        "iZk9HQAAAABJRU5ErkJggg==\033\\");
    image = graphics ? ghostty_kitty_graphics_image(graphics, 8) : NULL;
    check(image != NULL, "PNG payload decodes through the installed decoder");
    if (image &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH, &width) == GHOSTTY_SUCCESS &&
        ghostty_kitty_graphics_image_get(image,
            GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT, &height) == GHOSTTY_SUCCESS &&
        width == 1 && height == 1)
        check(true, "decoded PNG is 1x1 pixels");
    else
        check(false, "decoded PNG is 1x1 pixels");

    /* The render state is where the placement geometry comes from. */
    if (ghostty_render_state_update(term.render_state, term.handle) ==
            GHOSTTY_SUCCESS &&
        ghostty_render_state_get(term.render_state,
            GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR,
            &term.image_iterator) == GHOSTTY_SUCCESS) {
        while (ghostty_render_state_image_iterator_next(term.image_iterator)) {
            if (ghostty_render_state_image_get(term.image_iterator,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, &protocol) !=
                    GHOSTTY_SUCCESS ||
                ghostty_render_state_image_get(term.image_iterator,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID, &image_id) !=
                    GHOSTTY_SUCCESS ||
                ghostty_render_state_image_get(term.image_iterator,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION,
                    &generation) != GHOSTTY_SUCCESS)
                continue;
            if (protocol == GHOSTTY_IMAGE_PROTOCOL_KITTY && image_id == 7 &&
                generation > 0) {
                placed = true;
                break;
            }
        }
    }
    check(placed, "render state reports the Kitty placement and generation");

    check(!term_animation_tick(&term, 0, &next_ms),
          "a static image asks for no animation frame");

    /* Give the image a second frame and gaps, then start the loop. The tick
     * has a deadline to report from then on. */
    term_write_text(&term,
        "\033_Ga=f,i=7,f=32,s=2,v=2,z=50;/wAA/wD/AP8AAP///////w==\033\\");
    term_write_text(&term, "\033_Ga=a,i=7,r=1,z=50\033\\");
    term_write_text(&term, "\033_Ga=a,i=7,s=3\033\\");
    check(term_animation_tick(&term, 1000, &next_ms) && next_ms >= 1000,
          "a playing animation asks for a deadline");

    /* Deleting the placements and the now-unused image frees the storage
     * entry (uppercase delete removes the data too). */
    term_write_text(&term, "\033_Ga=d,d=I,i=7\033\\");
    check(!graphics || !ghostty_kitty_graphics_image(graphics, 7),
          "deleting an image removes it from storage");

    term_free(&term);
}

/* Read one pixel from a surface, as R, G, B bytes. */
static void surface_pixel(const SDL_Surface *surface, int x, int y,
                          Uint8 out[4])
{
    const Uint8 *pixels = (const Uint8 *)surface->pixels;
    size_t offset = (size_t)y * (size_t)surface->pitch + (size_t)x * 4;

    memcpy(out, pixels + offset, 4);
}

/* Snapshot the renderer into RGBA32 so the test can inspect pixels. */
static SDL_Surface *read_surface(SDL_Renderer *renderer)
{
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *rgba;

    if (!shot)
        return NULL;
    rgba = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(shot);
    return rgba;
}

/* Check whether the pixel at (x, y) is the test image's opaque red or the
 * black background. */
static void check_pixel(const SDL_Surface *surface, int x, int y, bool red,
                        const char *description)
{
    Uint8 pixel[4];

    surface_pixel(surface, x, y, pixel);
    check(red ? pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0
              : pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0,
          description);
}

/* Clear to black and draw one image layer. */
static void draw_layer(ImageCache **cache, SDL_Renderer *renderer, Term *term,
                       GhosttyImageLayer layer)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    images_draw(cache, renderer, term, layer, 0, 0, 160, 80);
}

/* Draw real textures through a software renderer. This covers the parts the
 * placement test cannot see: RGBA upload, cell geometry, clipping to the
 * grid and the three image layers. */
static void test_image_draw(void)
{
    Term term;
    SDL_Surface *surface;
    SDL_Surface *shot;
    SDL_Renderer *renderer;
    ImageCache *cache = NULL;

    if (!term_init(&term, 20, 5)) {
        check(false, "initialize terminal for image drawing");
        return;
    }
    term_resize(&term, 20, 5, 8, 16);

    /* Three one-pixel images, one per image layer. Kitty's z-index picks the
     * layer: 0 is above the text, a small negative value is below the text
     * and below INT32_MIN / 2 is below the cell background. */
    term_write_text(&term, "\033[3;4H");
    term_write_text(&term, "\033_Ga=T,f=32,s=1,v=1,i=9,z=0;/wAA/w==\033\\");
    term_write_text(&term, "\033[1;11H");
    term_write_text(&term, "\033_Ga=T,f=32,s=1,v=1,i=10,z=-1;/wAA/w==\033\\");
    term_write_text(&term, "\033[5;16H");
    term_write_text(&term,
                    "\033_Ga=T,f=32,s=1,v=1,i=11,z=-2000000000;/wAA/w==\033\\");

    if (ghostty_render_state_update(term.render_state, term.handle) !=
            GHOSTTY_SUCCESS)
        check(false, "render state updates for image drawing");
    surface = SDL_CreateSurface(160, 80, SDL_PIXELFORMAT_RGBA32);
    renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if (!renderer) {
        check(false, "software renderer for image drawing");
        SDL_DestroySurface(surface);
        term_free(&term);
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    draw_layer(&cache, renderer, &term, GHOSTTY_IMAGE_LAYER_ABOVE_TEXT);
    shot = read_surface(renderer);
    if (shot) {
        check_pixel(shot, 3 * 8, 2 * 16, true,
                    "above-text image draws at its cell");
        check_pixel(shot, 10 * 8, 0, false,
                    "a below-text image is not drawn in the above-text layer");
        check_pixel(shot, 15 * 8, 4 * 16, false,
                    "a below-background image is not drawn above the text");
        check_pixel(shot, 0, 0, false,
                    "image drawing leaves other pixels alone");
        SDL_DestroySurface(shot);
    } else {
        check(false, "read the software renderer for image drawing");
    }

    draw_layer(&cache, renderer, &term, GHOSTTY_IMAGE_LAYER_BELOW_TEXT);
    shot = read_surface(renderer);
    if (shot) {
        check_pixel(shot, 10 * 8, 0, true,
                    "z = -1 image draws in the below-text layer");
        check_pixel(shot, 3 * 8, 2 * 16, false,
                    "an above-text image is not drawn below the text");
        check_pixel(shot, 15 * 8, 4 * 16, false,
                    "a below-background image is not drawn below the text");
        SDL_DestroySurface(shot);
    } else {
        check(false, "read the software renderer for the below-text layer");
    }

    draw_layer(&cache, renderer, &term, GHOSTTY_IMAGE_LAYER_BELOW_BACKGROUND);
    shot = read_surface(renderer);
    if (shot) {
        check_pixel(shot, 15 * 8, 4 * 16, true,
                    "z below INT32_MIN / 2 draws below cell backgrounds");
        check_pixel(shot, 3 * 8, 2 * 16, false,
                    "an above-text image is not drawn below the background");
        check_pixel(shot, 10 * 8, 0, false,
                    "a below-text image is not drawn below the background");
        SDL_DestroySurface(shot);
    } else {
        check(false, "read the software renderer for the background layer");
    }

    images_free(&cache);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    term_free(&term);
}

#ifdef _WIN32
/* The same path, but with the bytes coming from a real ConPTY child rather
 * than from a string in this test: a program asking for the clipboard over
 * the pipes is the case that has to work. powershell writes the escape and
 * the base64 payload as raw console output, so nothing here types keys or
 * needs a window. */
static void test_clipboard_write_conpty(void)
{
    char error[256] = "";
    char output[8192];
    Term term;
    bool ran;

    ran = pty_run(L"powershell.exe -NoProfile -Command "
                  L"\"[Console]::Write([char]27+']52;c;ZnJvbS10aGUtcHJvZ3JhbQ=="
                  L"'+[char]7)\"",
                  output, sizeof output, NULL, error, sizeof error);
    if (!ran) {
        check(false, "the clipboard child started");
        printf("      %s\n", error);
        return;
    }

    if (!term_init(&term, 80, 24)) {
        check(false, "conpty clipboard terminal created");
        return;
    }
    term_set_clipboard_write(&term, record_clipboard, NULL);
    clipboard_calls = 0;
    clipboard_text[0] = '\0';
    clipboard_answer = TERM_CLIPBOARD_ALLOWED;

    term_write(&term, output, strlen(output));
    check(clipboard_calls >= 1, "a program's OSC 52 through ConPTY reaches the app");
    check(strcmp(clipboard_text, "from-the-program") == 0,
          "the ConPTY payload arrives decoded");
    if (strcmp(clipboard_text, "from-the-program") != 0)
        printf("      got: [%s] after %d call(s)\n", clipboard_text,
               (int)clipboard_calls);

    term_free(&term);
}

/* The image path through a real ConPTY child: what matters is whether the
 * pseudoconsole forwards the APC payload to us at all. ConPTY consumes APC
 * sequences on every current Windows version, so this check only asserts the
 * text that surrounds the image. It is a canary: if a future ConPTY starts
 * forwarding APC, this starts observing the image and the assertion needs to
 * flip. */
static void test_kitty_graphics_conpty(void)
{
    char output[8192] = "";
    char error[256] = "";
    bool ran;

    ran = pty_run(L"powershell.exe -NoProfile -Command "
                  L"\"[Console]::Write('before|'); "
                  L"[Console]::Write([string][char]27 + "
                  L"'_Ga=T,f=32,s=1,v=1,i=77;/wAA/w==' + "
                  L"[string][char]27 + '\\'); "
                  L"[Console]::Write('|after')\"",
                  output, sizeof output, NULL, error, sizeof error);
    if (!ran) {
        check(false, "the Kitty graphics child started");
        printf("      %s\n", error);
        return;
    }

    check(strstr(output, "before|") && strstr(output, "|after"),
          "ConPTY forwards the text around an APC image payload");
    check(!strstr(output, "_G") && !strstr(output, "/wAA/w=="),
          "ConPTY consumes APC image payloads (known Windows limit)");
}
#endif /* _WIN32 */

/* The faces compiled into the executable. On Windows they are RCDATA
 * resources; the glyph system relies on the built-in Maple family and the
 * Symbols Nerd Font Mono fallback being present. */
static void test_embedded_fonts(void)
{
#ifdef _WIN32
    const void *data = NULL;
    size_t size = 0;

    check(embedded_font(EMBEDDED_FONT_REGULAR, &data, &size) &&
              data && size > 1024,
          "the built-in Maple Mono NF regular face is embedded");
    check(embedded_font(EMBEDDED_FONT_SYMBOLS_MONO, &data, &size) &&
              data && size > 1024,
          "Symbols Nerd Font Mono is embedded");
    check(embedded_font_available(), "embedded_font_available is true");
#else
    check(true, "embedded fonts are Windows-only");
#endif
}

/* A font setting may be a family name. The resolver leaves a path that
 * already names a file alone and finds an installed family through the
 * Windows font registry. */
static void test_font_lookup(void)
{
    char path[512];
    char again[512];

    check(!font_lookup_path("termica-no-such-font-family", path, sizeof path),
          "an unknown font name is not resolved");
#ifdef _WIN32
    check(font_lookup_path("Consolas", path, sizeof path) && path[0] != '\0',
          "an installed family name resolves to a file");
    check(font_lookup_path(path, again, sizeof again) &&
          strcmp(again, path) == 0,
          "a resolved path is returned unchanged");
#endif
}

/* The status bar config file: parse, format round trip, the 9 pt floor,
 * the format string and the style grammar. */
static void test_statusbar_settings(void)
{
    Statusbar sb;
    Statusbar again;
    char text[8192];
    const char *document =
        "format = '$directory$git_branch $time'\n"
        "[font]\n"
        "face = 'GeistMono NF'\n"
        "size = 8\n"
        "\n"
        "[directory]\n"
        "truncation_length = 2\n"
        "style = 'fg:#aabbcc bold'\n"
        "\n"
        "[cmd_duration]\n"
        "min_time = 1000\n";

    statusbar_default(&sb);
    check(sb.point_size == 11.0f, "default statusbar size is 11");
    check(sb.modules[STATUSBAR_DIRECTORY].truncation_length == 1,
          "default directory truncation is 1");
    check(sb.modules[STATUSBAR_CMD_STATUS].parsed_success.has_fg,
          "cmd success style resolves");

    statusbar_parse_toml(&sb, document, strlen(document));
    check(sb.point_size == 11.0f, "a statusbar size below 9 is refused");
    check(sb.modules[STATUSBAR_DIRECTORY].truncation_length == 2,
          "directory truncation parses");
    check(sb.modules[STATUSBAR_DIRECTORY].parsed_style.has_fg &&
              sb.modules[STATUSBAR_DIRECTORY].parsed_style.bold,
          "module style resolves to color + bold");
    check(sb.modules[STATUSBAR_CMD_DURATION].min_time_ms == 1000,
          "cmd_duration min_time parses");
    check(strcmp(sb.format, "$directory$git_branch $time") == 0,
          "top-level format parses");

    statusbar_format(&sb, text, sizeof text);
    check(strstr(text, "size = 11") != NULL, "formatted file keeps the size");
    check(strstr(text, "truncation_length = 2") != NULL,
          "formatted file keeps truncation");

    statusbar_default(&again);
    check(statusbar_parse_toml(&again, text, strlen(text)),
          "the formatted file reparses");
    check(again.point_size == 11.0f &&
              again.modules[STATUSBAR_DIRECTORY].truncation_length == 2,
          "the statusbar format round trip holds");

    /* The three containers and the reserved busy slot parse and round trip. */
    {
        const char *containers =
            "center_format = '$busy'\n"
            "right_format = '$time'\n"
            "[busy]\n"
            "idle = '.'\n";
        Statusbar c;
        char container_text[8192];

        statusbar_default(&c);
        check(statusbar_parse_toml(&c, containers, strlen(containers)),
              "center_format and busy idle parse");
        check(strcmp(c.center_format, "$busy") == 0, "center_format stored");
        check(strcmp(c.right_format, "$time") == 0, "right_format stored");
        check(strcmp(c.modules[STATUSBAR_BUSY].idle, ".") == 0,
              "busy idle stored");
        statusbar_format(&c, container_text, sizeof container_text);
        check(strstr(container_text, "center_format = \"$busy\"") != NULL,
              "formatted file has center_format");
        check(strstr(container_text, "idle = \".\"") != NULL,
              "formatted file has the busy idle");
        statusbar_default(&again);
        check(statusbar_parse_toml(&again, container_text,
                                   strlen(container_text)) &&
              strcmp(again.center_format, "$busy") == 0,
              "center_format survives the round trip");
    }
}

/* The render engine: module values, style runs and the empty-module space
 * elision rule. */
static void test_statusbar_render(void)
{
    Statusbar sb;
    StatusbarState state;
    char out[1024];
    StatusbarRun runs[32];
    int count;

    statusbar_default(&sb);

    memset(&state, 0, sizeof state);
    state.directory.value = "~/proj";
    state.time.value = "12:00:00";
    count = statusbar_render(&sb, &state, "$directory $time", out,
                             sizeof out, runs, 32);
    check(count >= 2, "a populated line produces runs");
    check(strstr(out, "~/proj") != NULL, "directory value renders");
    check(strstr(out, "12:00:00") != NULL, "time value renders");

    memset(&state, 0, sizeof state);
    state.time.value = "12:00:00";
    state.time.symbol = "";
    count = statusbar_render(&sb, &state, "$directory $time", out,
                             sizeof out, runs, 32);
    check(strcmp(out, "12:00:00") == 0,
          "an empty module drops its following space");

    /* Outside a repository the git modules have no data at all. They must
     * collapse entirely, including git_branch's configured symbol. */
    memset(&state, 0, sizeof state);
    count = statusbar_render(&sb, &state, "$git_branch$git_status ", out,
                             sizeof out, runs, 32);
    check(count == 0 && out[0] == '\0',
          "git modules hide completely when there is no repository");

    /* The busy slot keeps its cell when it carries a placeholder, so the
     * bar does not reflow when a command starts; only an empty placeholder
     * collapses. */
    memset(&state, 0, sizeof state);
    state.busy.symbol = " ";
    state.time.value = "12:00:00";
    count = statusbar_render(&sb, &state, "$busy  $time", out,
                             sizeof out, runs, 32);
    check(out[0] == ' ', "the idle busy placeholder reserves a cell");
    check(strstr(out, "12:00:00") != NULL, "time renders after the slot");

    memset(&state, 0, sizeof state);
    state.busy.symbol = "";
    state.time.value = "12:00:00";
    state.time.symbol = "";
    count = statusbar_render(&sb, &state, "$busy $time", out,
                             sizeof out, runs, 32);
    check(strcmp(out, "12:00:00") == 0,
          "an empty busy placeholder collapses with its space");

    /* The fixed resting icon is enlarged to match the bar's Font Awesome
     * icons; the scale reaches the run that carries it. */
    memset(&state, 0, sizeof state);
    state.busy.symbol = ".";
    state.busy.scale = 1.25f;
    count = statusbar_render(&sb, &state, "$busy", out, sizeof out, runs, 32);
    check(count == 1 && runs[0].scale == 1.25f,
          "the resting icon's scale reaches its run");
}

/* The pure statusbar helpers. */
static void test_statusbar_helpers(void)
{
    Statusbar sb;
    char out[256];
    char frames[16][8];
    int count;

    statusbar_default(&sb);
    /* The shipped default is 1 component; the truncation cases below are
     * written for three, so set that explicitly. */
    sb.modules[STATUSBAR_DIRECTORY].truncation_length = 3;

    statusbar_shorten_path(&sb, "C:/Users/me/proj/src", out, sizeof out);
    check(strcmp(out, "me/proj/src") == 0, "path truncation keeps 3 parts");
    statusbar_shorten_path(&sb, "a/b", out, sizeof out);
    check(strcmp(out, "a/b") == 0, "fewer parts than the limit stay whole");

    /* The child's PEB reports the working directory with a trailing
     * separator; the display must not show it. */
    sb.modules[STATUSBAR_DIRECTORY].truncation_length = 0;
    statusbar_shorten_path(&sb, "C:\\work\\deep\\path\\", out, sizeof out);
    check(strcmp(out, "C:\\work\\deep\\path") == 0,
          "a trailing separator is dropped");
    sb.modules[STATUSBAR_DIRECTORY].truncation_length = 3;

    /* Home substitution: the leading home components become the configured
     * home symbol and the rest re-joins with a separator. */
    {
        char expected[64];

        SDL_setenv_unsafe("USERPROFILE", "C:\\Users\\tester", 1);
        statusbar_shorten_path(&sb, "C:\\Users\\tester\\Music\\", out,
                               sizeof out);
        snprintf(expected, sizeof expected, "%s\\Music",
                 sb.modules[STATUSBAR_DIRECTORY].home_symbol);
        check(strcmp(out, expected) == 0,
              "the home symbol re-joins with a separator");
    }

    statusbar_format_duration(750, out, sizeof out);
    check(strcmp(out, "750ms") == 0, "millisecond duration");
    statusbar_format_duration(12500, out, sizeof out);
    check(strcmp(out, "12s") == 0, "second duration");
    statusbar_format_duration(185000, out, sizeof out);
    check(strcmp(out, "3m 5s") == 0, "minute duration");

    statusbar_git_status_text(&sb, 2, 0, 1, 0, false, out, sizeof out);
    check(strstr(out, "2") != NULL &&
              strstr(out, sb.modules[STATUSBAR_GIT_STATUS].dirty) != NULL,
          "git status shows ahead and dirty");
    statusbar_git_status_text(&sb, 0, 0, 0, 0, true, out, sizeof out);
    check(strcmp(out, sb.modules[STATUSBAR_GIT_STATUS].clean) == 0,
          "a clean repo shows the clean glyph");

    count = statusbar_busy_frames(frames, 16);
    check(count == 10, "ten fixed spinner frames");
    check(strcmp(frames[0], "\u280B") == 0, "the first spinner frame");
    check(strcmp(statusbar_busy_idle_default(), "\U000F1807") == 0,
          "the resting icon default is fixed");
    check(statusbar_busy_idle_scale() > 1.0f,
          "the resting icon is enlarged to match the folder icon");
}

/* The git porcelain-v2 parser. */
static void test_git_parser(void)
{
    GitStatus status;
    const char *clean =
        "# branch.oid abc\n"
        "# branch.head main\n"
        "# branch.upstream origin/main\n"
        "# branch.ab +0 -0\n";
    const char *dirty =
        "# branch.oid abc\n"
        "# branch.head feature\r\n"
        "# branch.ab +2 -1\n"
        "1 M. N... 100644 100644 100644 file.c\n"
        "u UU N... 100644 100644 100644 conflict.txt\n"
        "? untracked.txt\n";

    check(git_parse_status(clean, strlen(clean), &status),
          "clean repo output parses");
    check(status.in_repo && strcmp(status.branch, "main") == 0,
          "the branch name is parsed");
    check(status.clean, "a clean repo is clean");

    check(git_parse_status(dirty, strlen(dirty), &status),
          "dirty repo output parses");
    check(strcmp(status.branch, "feature") == 0, "CRLF branch name is trimmed");
    check(status.ahead == 2 && status.behind == 1, "ahead/behind are parsed");
    check(status.dirty == 2, "tracked and untracked changes are counted");
    check(status.conflicted == 1, "unmerged entries are counted");
    check(!status.clean, "a dirty repo is not clean");

    check(!git_parse_status("not a repo\n", strlen("not a repo\n"), &status),
          "non-repo output is refused");
}

/* OSC 7 and OSC 133 feed the terminal's pwd and command state. */
static void test_term_pwd_and_prompts(void)
{
    Term term;

    check(term_init(&term, 40, 10), "term_init for pwd and prompts");
    check(term_pwd(&term) == NULL, "no pwd before the shell reports one");

    write_string(term.handle, "\x1b]7;file:///C:/Users/a%20b\x07");
    check(term.pwd_changed, "OSC 7 sets the pwd_changed flag");
#ifdef _WIN32
    /* On Windows the decoded URI path is normalized to backslashes. */
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:\\Users\\a b") == 0,
          "OSC 7 decodes the file URI");
#else
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:/Users/a b") == 0,
          "OSC 7 decodes the file URI");
#endif

    write_string(term.handle, "\x1b]9;9;\"C:\\work\\termica\"\x07");
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:\\work\\termica") == 0,
          "quoted OSC 9;9 Windows directory has no quotes");
    write_string(term.handle, "\x1b]9;9;C:\\work\\termica\x07");
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:\\work\\termica") == 0,
          "unquoted OSC 9;9 Windows directory is preserved");
    write_string(term.handle, "\x1b]9;9;\"\\\\server\\share\\dir\"\x07");
    check(term_pwd(&term) && strcmp(term_pwd(&term), "\\\\server\\share\\dir") == 0,
          "quoted OSC 9;9 UNC directory has no quotes");
    write_string(term.handle, "\x1b]9;9;C:\\work\\termica\"\x07");
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:\\work\\termica\"") == 0,
          "an unmatched quote is preserved");
    write_string(term.handle, "\x1b]7;file:///C:/work/%22name%22\x07");
#ifdef _WIN32
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:\\work\\\"name\"") == 0,
          "OSC 7 URI-encoded quotes are preserved");
#else
    check(term_pwd(&term) && strcmp(term_pwd(&term), "C:/work/\"name\"") == 0,
          "OSC 7 URI-encoded quotes are preserved");
#endif

    write_string(term.handle, "\x1b]133;D;0\x07");
    check(term.last_command_known && !term.last_duration_known,
          "command end without start does not invent a duration");

    write_string(term.handle, "\x1b]133;C\x07");
    check(term.command_running, "OSC 133 C marks a running command");

    write_string(term.handle, "\x1b]133;D;7\x07");
    check(!term.command_running, "OSC 133 D ends the command");
    check(term.last_command_known, "the exit code is known");
    check(term.last_duration_known, "OSC 133 C and D measure a duration");
    check(term.last_exit_code == 7, "the exit code is stored");

    write_string(term.handle, "\x1b" "c");
    check(!term.command_running, "a hard reset clears the running flag");
    check(term_pwd(&term) == NULL, "a hard reset clears the pwd");

    term_free(&term);
}

/* An integration may report prompt and command end without output start. */
static void test_prompt_duration_without_output_start(void)
{
    App *app = (App *)calloc(1, sizeof *app);
    Session session;
    SDL_KeyboardEvent enter = { 0 };
    uint64_t provisional_start;

    check(app != NULL, "allocate app for prompt duration");
    if (!app)
        return;
    if (!session_init(&session, 40, 10, NULL, NULL)) {
        check(false, "initialize session for prompt duration");
        free(app);
        return;
    }
    settings_default(&app->settings);
    app->sessions[0] = &session;
    app->session_count = 1;
    enter.scancode = SDL_SCANCODE_RETURN;

    write_string(session.term.handle, "\x1b]133;A\x07\x1b]133;B\x07");
    enter.mod = SDL_KMOD_SHIFT;
    input_handle_key_down(app, &enter);
    check(!session.term.command_start_pending,
          "modified Enter does not start a command timer");
    enter.mod = 0;
    input_handle_key_down(app, &enter);
    check(session.term.command_start_pending,
          "Enter at a marked prompt records a possible start");
    SDL_Delay(40);
    write_string(session.term.handle, "\x1b]133;D;0\x07");
    check(session.term.last_duration_known && session.term.last_command_ms >= 30,
          "command end without C uses elapsed time since Enter");

    write_string(session.term.handle, "\x1b]133;A\x07\x1b]133;B\x07");
    input_handle_key_down(app, &enter);
    provisional_start = session.term.command_started_at;
    SDL_Delay(40);
    write_string(session.term.handle, "\x1b]133;C\x07");
    check(session.term.command_running && !session.term.command_start_pending &&
              session.term.command_started_at >= provisional_start + 30,
          "OSC 133 C supersedes the provisional start");
    write_string(session.term.handle, "\x1b]133;D;0\x07");
    check(session.term.last_duration_known,
          "the real OSC 133 start produces a duration");

    write_string(session.term.handle, "\x1b]133;A\x07\x1b]133;B\x07");
    input_handle_key_down(app, &enter);
    write_string(session.term.handle, "\x1b]133;A\x07\x1b]133;D;0\x07");
    check(!session.term.last_duration_known,
          "a new prompt discards an unused provisional start");
    session_free(&session);
    free(app);
}

/* 1.1: shell profiles (command argv, cwd, [terminal.env]) and Windows
 * command-line quoting / environment block construction. */
static void test_shell_profiles(void)
{
    Settings s;
    Settings roundtrip;
    char formatted[16384];
    const char *doc =
        "[terminal]\n"
        "command = [\"wsl.exe\", \"-d\", \"Ubuntu 24.04\"]\n"
        "cwd = 'C:\\Users\\dev'\n"
        "\n"
        "[terminal.env]\n"
        "EDITOR = \"nvim\"\n"
        "TERM = \"xterm-ghostty\"\n";
    const char *bad_cmd =
        "[terminal]\n"
        "command = [\"ok\", 42]\n";

    settings_default(&s);
    check(settings_parse_toml(&s, doc, strlen(doc)),
          "shell profile TOML parses");
    check(s.command_argc == 3, "command argv count is 3");
    check(strcmp(s.command[0], "wsl.exe") == 0 &&
              strcmp(s.command[1], "-d") == 0 &&
              strcmp(s.command[2], "Ubuntu 24.04") == 0,
          "command argv elements are stored");
    check(strcmp(s.cwd, "C:\\Users\\dev") == 0, "cwd is stored");
    check(s.env_count == 2, "two env entries are stored");
    check(strcmp(s.env[0].key, "EDITOR") == 0 &&
              strcmp(s.env[0].value, "nvim") == 0,
          "first env entry is stored");

    /* Non-string command elements are rejected without clobbering. */
    check(!settings_parse_toml(&s, bad_cmd, strlen(bad_cmd)),
          "non-string command element is refused");
    check(s.command_argc == 3, "refused command leaves previous argv intact");

    /* Format round trip preserves command, cwd and [terminal.env]. */
    settings_format(&s, formatted, sizeof formatted);
    settings_default(&roundtrip);
    check(settings_parse_toml(&roundtrip, formatted, strlen(formatted)),
          "formatted shell profile reparses");
    check(roundtrip.command_argc == 3 &&
              strcmp(roundtrip.command[2], "Ubuntu 24.04") == 0,
          "command survives format round trip");
    check(strcmp(roundtrip.cwd, "C:\\Users\\dev") == 0,
          "cwd survives format round trip");
    check(roundtrip.env_count == 2 &&
              strcmp(roundtrip.env[1].key, "TERM") == 0 &&
              strcmp(roundtrip.env[1].value, "xterm-ghostty") == 0,
          "env survives format round trip");

#ifdef _WIN32
    {
        const char *argv[] = {
            "C:\\Program Files\\app.exe",
            "-m",
            "hello \"world\"",
            "trail\\",
            ""
        };
        wchar_t *line = pty_build_command_line(argv, 5);

        check(line != NULL, "pty_build_command_line succeeds");
        check(line && wcscmp(line,
                             L"\"C:\\Program Files\\app.exe\" -m "
                             L"\"hello \\\"world\\\"\" trail\\ \"\"") == 0,
              "Windows command-line quoting matches MSVC rules");
        free(line);
    }
    {
        PtyEnvEntry extras[2] = {
            { "TERMICA_TEST_VAR", "hello_env" },
            { "TERM", "xterm-custom" }
        };
        wchar_t *block = pty_build_environment(extras, 2);
        const wchar_t *cursor;
        bool saw_custom = false;
        bool saw_term_custom = false;
        int term_count = 0;

        check(block != NULL, "pty_build_environment succeeds");
        if (block) {
            for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
                if (wcscmp(cursor, L"TERMICA_TEST_VAR=hello_env") == 0)
                    saw_custom = true;
                if (wcsncmp(cursor, L"TERM=", 5) == 0) {
                    term_count++;
                    if (wcscmp(cursor, L"TERM=xterm-custom") == 0)
                        saw_term_custom = true;
                }
            }
            free(block);
        }
        check(saw_custom, "env block contains custom variable");
        check(saw_term_custom && term_count == 1,
              "env block overrides TERM without duplicating it");
    }
#endif
}

/* 1.2: configurable key bindings (chord parsing, defaults, overrides). */
static void test_key_bindings(void)
{
    KeyMap map;
    SDL_Scancode sc;
    uint8_t mods;
    KeyAction act;
    Settings s;
    Settings roundtrip;
    char formatted[16384];
    const char *doc =
        "[keys]\n"
        "\"Ctrl+V\" = \"paste\"\n"
        "\"Ctrl+Shift+Q\" = \"none\"\n"
        "\"Ctrl+Alt+Z\" = \"font_size_reset\"\n";

    keys_default(&map);
    check(keys_lookup(&map, SDL_SCANCODE_V, SDL_KMOD_LCTRL) == KEY_ACTION_NONE,
          "Ctrl+V is unbound by default (reaches vim/readline)");
    check(keys_lookup(&map, SDL_SCANCODE_V,
                      SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) == KEY_ACTION_PASTE,
          "Ctrl+Shift+V pastes by default");
    check(keys_lookup(&map, SDL_SCANCODE_EQUALS,
                      SDL_KMOD_LCTRL) == KEY_ACTION_FONT_SIZE_UP,
          "Ctrl+= zooms font up by default");
    check(keys_lookup(&map, SDL_SCANCODE_MINUS,
                      SDL_KMOD_LCTRL) == KEY_ACTION_FONT_SIZE_DOWN,
          "Ctrl+- zooms font down by default");
    check(keys_lookup(&map, SDL_SCANCODE_0,
                      SDL_KMOD_RCTRL) == KEY_ACTION_FONT_SIZE_RESET,
          "Ctrl+0 resets font size (side-independent)");

    check(keys_parse_chord("Ctrl+Shift+V", &sc, &mods) &&
              sc == SDL_SCANCODE_V && mods == (KEY_MOD_CTRL | KEY_MOD_SHIFT),
          "chord parser handles Ctrl+Shift+V");
    check(keys_parse_chord("Ctrl++", &sc, &mods) &&
              sc == SDL_SCANCODE_EQUALS && mods == KEY_MOD_CTRL,
          "chord parser handles Ctrl++");
    check(keys_parse_chord("Ctrl+-", &sc, &mods) &&
              sc == SDL_SCANCODE_MINUS && mods == KEY_MOD_CTRL,
          "chord parser handles Ctrl+-");
    check(!keys_parse_chord("Ctrl+BogusKey", &sc, &mods),
          "chord parser refuses unknown key");
    check(keys_parse_action("font-size-up", &act) &&
              act == KEY_ACTION_FONT_SIZE_UP,
          "action parser folds separators and case");
    check(!keys_parse_action("explode", &act),
          "action parser refuses unknown action");

    settings_default(&s);
    check(settings_parse_toml(&s, doc, strlen(doc)), "[keys] TOML parses");
    check(keys_lookup(&s.keys, SDL_SCANCODE_V, SDL_KMOD_LCTRL) ==
              KEY_ACTION_PASTE,
          "user can bind Ctrl+V back to paste");
    check(keys_lookup(&s.keys, SDL_SCANCODE_Q,
                      SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) == KEY_ACTION_NONE,
          "binding a chord to none disables it");
    check(keys_lookup(&s.keys, SDL_SCANCODE_Z,
                      SDL_KMOD_LCTRL | SDL_KMOD_LALT) ==
              KEY_ACTION_FONT_SIZE_RESET,
          "custom chord binds to font_size_reset");

    settings_format(&s, formatted, sizeof formatted);
    settings_default(&roundtrip);
    check(settings_parse_toml(&roundtrip, formatted, strlen(formatted)),
          "formatted [keys] reparses");
    check(keys_lookup(&roundtrip.keys, SDL_SCANCODE_V, SDL_KMOD_LCTRL) ==
              KEY_ACTION_PASTE,
          "[keys] overrides survive format round trip");
}

/* 1.5: font fallback chain configuration and OS fallback resolution. */
static void test_font_fallback(void)
{
    Settings s;
    Settings roundtrip;
    char formatted[16384];
    const char *doc =
        "[font]\n"
        "fallback = [\"Consolas\", \"Segoe UI Emoji\"]\n";

    settings_default(&s);
    check(settings_parse_toml(&s, doc, strlen(doc)),
          "font.fallback TOML parses");
    check(s.font_fallback_count == 2 &&
              strcmp(s.font_fallback[0], "Consolas") == 0 &&
              strcmp(s.font_fallback[1], "Segoe UI Emoji") == 0,
          "font.fallback entries are stored");

    settings_format(&s, formatted, sizeof formatted);
    settings_default(&roundtrip);
    check(settings_parse_toml(&roundtrip, formatted, strlen(formatted)),
          "formatted font.fallback reparses");
    check(roundtrip.font_fallback_count == 2 &&
              strcmp(roundtrip.font_fallback[1], "Segoe UI Emoji") == 0,
          "font.fallback survives format round trip");

#ifdef _WIN32
    {
        const char *user[1] = { "Consolas" };
        char paths[16][256];
        int count = glyphs_resolve_fallback_paths(user, 1, paths, 16);

        check(count >= 2, "fallback resolver finds user and OS fallback fonts");
        check(count >= 1 && SDL_strcasestr(paths[0], "consola") != NULL,
              "user fallback precedes OS fallbacks in chain");
    }
#endif
}

/* 1.6: colour schemes ([colors] parsing, presets, and libghostty render
 * state propagation). */
static void test_color_schemes(void)
{
    Settings s;
    Settings roundtrip;
    char formatted[16384];
    Term term;
    GhosttyRenderStateColors rc = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    const char *doc =
        "[colors]\n"
        "scheme = \"light\"\n"
        "foreground = \"#112233\"\n"
        "background = \"#ddeeff\"\n"
        "cursor = \"#ff0088\"\n"
        "selection_background = \"#334455\"\n"
        "red = \"#cc1122\"\n";

    settings_default(&s);
    check(settings_parse_toml(&s, doc, strlen(doc)), "[colors] TOML parses");
    check(s.colors.custom, "custom flag is set");
    check(strcmp(s.colors.scheme_name, "light") == 0, "light preset loaded");
    check(s.colors.foreground.r == 0x11 && s.colors.foreground.g == 0x22 &&
              s.colors.foreground.b == 0x33,
          "foreground override applied over preset");
    check(s.colors.palette[1].r == 0xcc && s.colors.palette[1].g == 0x11 &&
              s.colors.palette[1].b == 0x22,
          "named palette entry overrides red (index 1)");
    check(s.colors.has_selection_bg && s.colors.selection_bg.r == 0x33,
          "selection_background is stored");

    settings_format(&s, formatted, sizeof formatted);
    settings_default(&roundtrip);
    check(settings_parse_toml(&roundtrip, formatted, strlen(formatted)),
          "formatted [colors] reparses");
    check(roundtrip.colors.foreground.r == 0x11 &&
              roundtrip.colors.palette[1].r == 0xcc &&
              roundtrip.colors.has_selection_bg,
          "[colors] survives format round trip");

    check(term_init(&term, 40, 10), "term_init for colour scheme");
    term_set_colors(&term, &s.colors);
    ghostty_render_state_update(term.render_state, term.handle);
    ghostty_render_state_get(term.render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLORS, &rc);
    check(rc.foreground.r == 0x11 && rc.foreground.g == 0x22 &&
              rc.foreground.b == 0x33,
          "foreground colour reaches render state");
    check(rc.background.r == 0xdd && rc.background.g == 0xee &&
              rc.background.b == 0xff,
          "background colour reaches render state");
    check(rc.cursor_has_value && rc.cursor.r == 0xff && rc.cursor.g == 0x00 &&
              rc.cursor.b == 0x88,
          "cursor colour reaches render state");
    check(rc.palette[1].r == 0xcc && rc.palette[1].g == 0x11 &&
              rc.palette[1].b == 0x22,
          "ANSI palette index 1 reaches render state");
    term_free(&term);
}

/* 1.8: bell setting and BEL (0x07) callback. */
static void test_bell(void)
{
    Settings s;
    Settings roundtrip;
    char formatted[16384];
    Term term;
    const char *doc = "[terminal]\nbell = \"taskbar\"\n";

    settings_default(&s);
    check(s.bell == SETTINGS_BELL_VISUAL, "default bell is visual");
    check(settings_parse_toml(&s, doc, strlen(doc)), "bell setting parses");
    check(s.bell == SETTINGS_BELL_TASKBAR, "bell = taskbar is stored");

    settings_format(&s, formatted, sizeof formatted);
    settings_default(&roundtrip);
    check(settings_parse_toml(&roundtrip, formatted, strlen(formatted)),
          "formatted bell setting reparses");
    check(roundtrip.bell == SETTINGS_BELL_TASKBAR,
          "bell setting survives format round trip");

    check(term_init(&term, 40, 10), "term_init for bell");
    check(!term.bell_rang, "bell flag starts clear");
    write_string(term.handle, "\x07");
    check(term.bell_rang, "BEL (0x07) sets term.bell_rang");
    term_free(&term);
}

#ifdef _WIN32
/* 1.7: asynchronous pty writer queue preserves FIFO byte ordering across
 * rapid writes into a real ConPTY child. */
static void test_pty_writer_queue(void)
{
    char error[128];
    Pty *pty = pty_spawn(L"cmd.exe /Q /K", 80, 24, NULL, NULL,
                         error, sizeof error);
    char collected[8192];
    size_t used = 0;
    uint32_t code = 99;
    int ticks;
    const char *p1;
    const char *p2;

    check(pty != NULL, "pty_spawn for async writer queue test");
    if (!pty)
        return;

    check(pty_write(pty, "echo QUEUE_FIRST\r\n", 18),
          "async pty_write first chunk");
    check(pty_write(pty, "echo QUEUE_SECOND\r\n", 19),
          "async pty_write second chunk");
    check(pty_write(pty, "exit 0\r\n", 8),
          "async pty_write exit command");

    for (ticks = 0; ticks < 100; ticks++) {
        uint8_t buf[512];
        size_t got;

        pty_wait(pty, 100);
        while ((got = pty_read(pty, buf, sizeof buf)) > 0) {
            size_t copy = got;

            if (used + copy >= sizeof collected)
                copy = sizeof collected - 1 - used;
            if (copy > 0) {
                memcpy(collected + used, buf, copy);
                used += copy;
            }
        }
        if (pty_exit_code(pty, &code))
            break;
    }
    collected[used] = '\0';
    p1 = strstr(collected, "QUEUE_FIRST");
    p2 = strstr(collected, "QUEUE_SECOND");
    check(p1 != NULL && p2 != NULL && p1 < p2,
          "async writer queue delivers writes in strict FIFO order");
    check(code == 0, "async writer child exits with code 0");
    pty_close(pty);
}

/* A full ring must request another bounded drain; the last OSC title must
 * arrive without waiting for the status-bar timer to wake the event loop. */
static void test_session_pump_continuation(void)
{
    Session session;
    char error[256] = "";
    Uint64 deadline;
    bool more = false, exited = false, resumed = false;

    if (!session_init(&session, 80, 24, NULL, NULL)) {
        check(false, "initialize session for output continuation");
        return;
    }
    session.pty = pty_spawn(
        L"powershell.exe -NoProfile -Command \""
        L"[Console]::Write(('x' * 262144) + [char]27 + ']2;pump-suffix' + [char]7); "
        L"Start-Sleep -Seconds 2\"", 80, 24, NULL, NULL, error, sizeof error);
    if (!session.pty) {
        check(false, "start output continuation child");
        session_free(&session);
        return;
    }
    deadline = SDL_GetTicks() + 10000;
    while (pty_pending(session.pty) < 131072 && SDL_GetTicks() < deadline)
        SDL_Delay(10);
    (void)session_pump(&session, &exited, &more);
    resumed = more;
    while (SDL_GetTicks() < deadline &&
           strcmp(session.term.title, "pump-suffix") != 0 && !exited) {
        if (!more)
            pty_wait(session.pty, 50);
        (void)session_pump(&session, &exited, &more);
    }
    check(resumed && session.term.title_changed &&
              strcmp(session.term.title, "pump-suffix") == 0,
          "bounded PTY drains continue to the final output marker");
    if (!resumed || strcmp(session.term.title, "pump-suffix") != 0)
        printf("      continuation=%d title=[%s] exited=%d pending=%zu\n",
               resumed, session.term.title, exited, pty_pending(session.pty));
    session_free(&session);
}
#endif

/* 2.2: tab keybindings, tab labels and multi-session lifecycle. */
static void test_tabs(void)
{
    KeyMap map;
    Session s1;
    Session s2;
    char label[64];

    keys_default(&map);
    check(keys_lookup(&map, SDL_SCANCODE_T,
                      SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) == KEY_ACTION_NEW_TAB,
          "Ctrl+Shift+T opens a new tab by default");
    check(keys_lookup(&map, SDL_SCANCODE_W,
                      SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) == KEY_ACTION_CLOSE_TAB,
          "Ctrl+Shift+W closes the active tab by default");
    check(keys_lookup(&map, SDL_SCANCODE_TAB,
                      SDL_KMOD_LCTRL) == KEY_ACTION_NEXT_TAB,
          "Ctrl+Tab switches to the next tab by default");
    check(keys_lookup(&map, SDL_SCANCODE_TAB,
                      SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) == KEY_ACTION_PREV_TAB,
          "Ctrl+Shift+Tab switches to the previous tab by default");

    check(session_init(&s1, 40, 10, NULL, NULL), "first session initializes");
    check(session_init(&s2, 40, 10, NULL, NULL), "second session initializes");

    chrome_tab_label(&s1, 0, label, sizeof label);
    check(strcmp(label, "Tab 1") == 0,
          "tab label falls back to Tab N when empty");

    snprintf(s1.term.pwd, sizeof s1.term.pwd, "C:\\sandbox\\termica\\");
    chrome_tab_label(&s1, 0, label, sizeof label);
    check(strcmp(label, "termica") == 0,
          "tab label uses last directory component of pwd");

    snprintf(s2.term.title, sizeof s2.term.title, "vim init.lua");
    chrome_tab_label(&s2, 1, label, sizeof label);
    check(strcmp(label, "vim init.lua") == 0,
          "tab label prefers OSC window title over pwd");

    session_resize(&s1, 80, 24, 9, 22);
    session_resize(&s2, 80, 24, 9, 22);
    check(s1.term.columns == 80 && s2.term.rows == 24,
          "independent sessions resize cleanly");

    session_free(&s1);
    session_free(&s2);
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
    test_scrollback();
    test_selection();
    test_mouse_and_focus();
    test_underline_styles();
    test_settings();
    test_shell_profiles();
    test_key_bindings();
    test_font_fallback();
    test_color_schemes();
    test_bell();
    test_tabs();
    test_statusbar_settings();
    test_statusbar_render();
    test_statusbar_helpers();
    test_git_parser();
    test_term_pwd_and_prompts();
    test_prompt_duration_without_output_start();
    test_embedded_fonts();
    test_font_lookup();
    test_clipboard_write();
    test_terminal_effects();
    test_kitty_graphics();
    test_image_draw();
    failure_count += config_test();
    failure_count += desktop_signal_selftest();
    failure_count += hyperlink_selftest();
    failure_count += search_selftest();
    failure_count += diagnostics_selftest();
    failure_count += glyph_atlas_selftest();
#ifdef _WIN32
    test_pty();
    test_pty_writer_queue();
    test_session_pump_continuation();
    test_clipboard_write_conpty();
    test_kitty_graphics_conpty();
#endif

    printf("\n%s (%d failure%s)\n", failure_count ? "FAILED" : "OK",
           failure_count, failure_count == 1 ? "" : "s");
    return failure_count;
}
