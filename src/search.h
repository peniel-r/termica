/* Scrollback search. All functions touching Term run on the UI thread. */
#ifndef TERMICA_SEARCH_H
#define TERMICA_SEARCH_H

#include "term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEARCH_QUERY_CAPACITY 512

typedef struct SearchMatch {
    uint64_t first_row, last_row; /* internal rows, offset by row_bias */
    uint16_t first_column, last_column;
} SearchMatch;
typedef struct SearchWork SearchWork;

typedef struct Search {
    bool open, case_sensitive, regex, dirty, mouse_down;
    char query[SEARCH_QUERY_CAPACITY];
    size_t length, cursor;
    SearchMatch *matches;
    size_t first, count, capacity, selected;
    uint64_t row_bias;
    SearchWork *work;
    bool running;
    char error[128];
} Search;

void search_free(Search *search);
void search_invalidate(Search *search);
/* Preserve completed history and restart only the mutable active tail. */
void search_output_changed(Search *search, Term *term);
bool search_pending(const Search *search);
void search_refresh(Search *search, Term *term);
void search_navigate(Search *search, Term *term, int direction);
bool search_insert(Search *search, const char *text);
void search_backspace(Search *search);
void search_delete(Search *search);
void search_move_cursor(Search *search, int direction);
/* 0: no match, 1: match, 2: selected match. */
int search_cell(const Search *search, uint64_t row, unsigned column);
int search_selftest(void);

#endif
