#include "search.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check_search(bool ok, const char *description)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok)
        failures++;
}

static bool finish_search(Search *search, Term *term)
{
    int ticks;
    for (ticks = 0; ticks < 10000 && search_pending(search); ticks++)
        search_refresh(search, term);
    return !search_pending(search);
}

static void query(Search *search, Term *term, const char *text,
                  bool sensitive, bool regex)
{
    search->query[0] = '\0';
    search->length = search->cursor = 0;
    search->selected = SIZE_MAX;
    search->case_sensitive = sensitive;
    search->regex = regex;
    search_insert(search, text);
    search_invalidate(search);
    check_search(finish_search(search, term), "search scan completes");
}

int search_selftest(void)
{
    Term term;
    Search search = {0};
    GhosttyTerminalScrollbar bar = {0};
    const char *text = "Alpha 12\r\nbeta 34\r\nALPHA 56\r\nend";
    size_t initial;
    failures = 0;
    if (!term_init(&term, 20, 3)) {
        check_search(false, "search terminal initialises");
        return failures;
    }
    search.open = true;
    term_write(&term, text, strlen(text));
    query(&search, &term, "alpha", false, false);
    check_search(search.count == 2 && !search.error[0],
                 "search finds case-insensitive matches in scrollback");
    if (search.count == 2) {
        initial = search.selected;
        search_navigate(&search, &term, -1);
        check_search(search.selected != initial &&
                     term_scrollbar(&term, &bar) &&
                     search.matches[search.selected].first_row >= bar.offset &&
                     search.matches[search.selected].first_row < bar.offset + bar.len,
                     "search previous selects and reveals history");
        search_navigate(&search, &term, 1);
        check_search(search.selected == initial,
                     "search next wraps back to the newer match");
        check_search(search_cell(&search, search.matches[initial].first_row,
                                 search.matches[initial].first_column) == 2,
                     "search marks the selected cell");
    } else {
        check_search(false, "search previous selects and reveals history");
        check_search(false, "search next wraps back to the newer match");
        check_search(false, "search marks the selected cell");
    }
    query(&search, &term, "alpha", true, false);
    check_search(search.count == 0, "search case toggle is respected");
    query(&search, &term, "beta [0-9]+", false, true);
    check_search(search.count == 1 && !search.error[0],
                 "search evaluates regular expressions");
    query(&search, &term, "[", false, true);
    check_search(search.count == 0 && search.error[0],
                 "search reports invalid regular expressions");
    query(&search, &term, "^", false, true);
    check_search(search.count == 0 && !search.error[0],
                 "zero-length regex matches terminate without highlights");
    query(&search, &term, ".*?", false, true);
    check_search(search.count > 0 && !search.error[0],
                 "empty regex alternatives still find non-empty matches");
    query(&search, &term, "[0-9]", false, false);
    check_search(search.count == 0 && !search.error[0],
                 "literal search does not interpret regex syntax");
    term_write(&term, "\r\n\xc3\x89 \xe7\x95\x8c", 8);
    query(&search, &term, "\xc3\xa9", false, false);
    check_search(search.count == 1, "search uses Unicode case folding");
    query(&search, &term, "\xe7\x95\x8c", true, false);
    check_search(search.count == 1 &&
                 search.matches[0].last_column == search.matches[0].first_column + 1,
                 "search highlights both cells of a wide character");
    query(&search, &term, "end", false, false);
    term_write(&term, "\r\nend", 5);
    search_output_changed(&search, &term);
    check_search(finish_search(&search, &term), "search output rescan completes");
    check_search(search.count == 2, "search updates after terminal output");
    term_write(&term, "\x1b[?1049halt", strlen("\x1b[?1049halt"));
    query(&search, &term, "end", false, false);
    check_search(search.count == 0, "search follows the alternate screen");
    term_write(&term, "\x1b[?1049l", strlen("\x1b[?1049l"));
    search_output_changed(&search, &term);
    check_search(finish_search(&search, &term), "search screen switch completes");
    check_search(search.count == 2, "search restores primary scrollback results");
    term_resize(&term, 18, 3, 8, 16);
    search_invalidate(&search);
    check_search(finish_search(&search, &term), "search resize rescan completes");
    check_search(search.count == 2, "search refreshes after terminal reflow");
    search_free(&search);
    term_free(&term);

    memset(&search, 0, sizeof search);
    if (!term_init(&term, 5, 3)) {
        check_search(false, "wrapped search terminal initialises");
        return failures;
    }
    search.open = true;
    term_write(&term, "abcdefgh", 8);
    query(&search, &term, "defg", true, false);
    check_search(search.count == 1 &&
                 search.matches[0].first_row < search.matches[0].last_row,
                 "search joins soft-wrapped lines");
    search_free(&search);
    term_free(&term);

    memset(&search, 0, sizeof search);
    if (!term_init(&term, 20, 3)) {
        check_search(false, "incremental search terminal initialises");
        return failures;
    }
    check_search(term_set_scrollback_lines(&term, 1000),
                 "incremental search scrollback configured");
    {
        int i;
        size_t partial;
        for (i = 0; i < 200; i++)
            term_write(&term, "needle\r\n", 8);
        search.open = true;
        search_insert(&search, "needle");
        search_refresh(&search, &term);
        partial = search.count;
        check_search(search_pending(&search) && partial > 0 && partial < 200,
                     "search yields partial results within its work budget");
        term_write(&term, "needle\r\n", 8);
        search_output_changed(&search, &term);
        check_search(!search.dirty && search.count == partial,
                     "new output preserves scanned immutable history");
        check_search(finish_search(&search, &term) && search.count == 201,
                     "incremental search completes after concurrent output");
        search.cursor = search.length;
        search_backspace(&search);
        check_search(search.dirty && search_cell(&search, 0, 0) == 0,
                     "query changes immediately hide stale results");
        check_search(finish_search(&search, &term) && search.count == 201,
                     "incremental search restarts after query edits");
        check_search(term_set_scrollback_lines(&term, 20),
                     "bounded search scrollback configured");
        search_invalidate(&search);
        check_search(finish_search(&search, &term), "search after history trim completes");
        for (i = 0; i < 80; i++)
            term_write(&term, "needle\r\n", 8);
        search_output_changed(&search, &term);
        check_search(finish_search(&search, &term) && term_scrollbar(&term, &bar),
                     "search completes after scrollback eviction");
        {
            size_t j;
            bool valid = true;
            for (j = 0; j < search.count; j++) {
                const SearchMatch *m = &search.matches[search.first + j];
                if (m->first_row < search.row_bias ||
                    m->last_row - search.row_bias >= bar.total)
                    valid = false;
            }
            check_search(valid, "evicted results are not navigable or highlighted");
        }
    }
    search_free(&search);
    term_free(&term);

    memset(&search, 0, sizeof search);
    if (!term_init(&term, 20, 3)) {
        check_search(false, "long wrapped search terminal initialises");
        return failures;
    }
    check_search(term_set_scrollback_lines(&term, 1000),
                 "long wrapped search scrollback configured");
    {
        char long_line[4006];
        memset(long_line, 'a', 4000);
        memcpy(long_line + 4000, "needle", 6);
        term_write(&term, long_line, sizeof long_line);
        term_write(&term, "\r\nx\r\ny\r\nz\r\n", 11);
        search.open = true;
        search_insert(&search, "needle");
        search_refresh(&search, &term);
        check_search(search_pending(&search), "long wrapped search yields");
        term_write(&term, "tail\r\n", 6);
        search_output_changed(&search, &term);
        check_search(!search.dirty, "output retains a partial historical line");
        search_refresh(&search, &term);
        check_search(search.count == 1,
                     "wrapped search resumes rather than restarting on output");
        check_search(finish_search(&search, &term), "wrapped search completes");
    }
    search_free(&search);
    term_free(&term);

    memset(&search, 0, sizeof search);
    search_insert(&search, "a\xe7\x95\x8c" "b");
    search_move_cursor(&search, -1);
    search_backspace(&search);
    check_search(strcmp(search.query, "ab") == 0 && search.cursor == 1,
                 "search editing removes a whole UTF-8 character");
    search_delete(&search);
    check_search(strcmp(search.query, "a") == 0,
                 "search delete removes the next character");
    search_free(&search);
    return failures;
}
