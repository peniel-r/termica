/* Search logical lines incrementally on the UI thread. A tracked history
 * anchor keeps results aligned through scrollback eviction; the active tail
 * is rescanned after output, and a resize or screen switch restarts the scan. */
#include "search.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SearchPosition {
    uint64_t row;
    uint16_t column, width;
} SearchPosition;

typedef struct SearchLine {
    char *text;
    SearchPosition *positions;
    size_t length, capacity, meaningful;
} SearchLine;

/* A scan runs only on the UI thread, with a fixed cell/match budget per
 * frame. Historical matches survive output; only the active tail is rescanned. */
struct SearchWork {
    SearchLine line;
    pcre2_code *code;
    pcre2_match_data *data;
    pcre2_match_context *context;
    GhosttyTrackedGridRef anchor;
    GhosttyTerminalScreen screen;
    uint64_t row, total, history_end, safe_row, line_start, anchor_row;
    uint16_t column;
    bool row_started, row_wrapped, matching, selection_explicit, reveal_pending;
    int pending_direction;
    size_t match_offset;
    uint32_t match_flags;
};

static void work_free(SearchWork *w)
{
    if (!w)
        return;
    ghostty_tracked_grid_ref_free(w->anchor);
    pcre2_match_context_free(w->context);
    pcre2_match_data_free(w->data);
    pcre2_code_free(w->code);
    free(w->line.text);
    free(w->line.positions);
    free(w);
}

void search_free(Search *s)
{
    work_free(s->work);
    free(s->matches);
    memset(s, 0, sizeof *s);
}

void search_invalidate(Search *s)
{
    s->dirty = true;
}

bool search_pending(const Search *s)
{
    return s->open && (s->dirty || s->running);
}

static bool line_append(SearchLine *line, const char *bytes, size_t length,
                        SearchPosition position)
{
    size_t i;
    if (length > SIZE_MAX - line->length)
        return false;
    if (line->length + length > line->capacity) {
        size_t cap = line->capacity ? line->capacity : 256;
        char *text;
        SearchPosition *positions;
        while (cap < line->length + length) {
            if (cap > SIZE_MAX / 2 / sizeof *positions)
                return false;
            cap *= 2;
        }
        text = realloc(line->text, cap);
        if (!text)
            return false;
        line->text = text;
        positions = realloc(line->positions, cap * sizeof *positions);
        if (!positions)
            return false;
        line->positions = positions;
        line->capacity = cap;
    }
    memcpy(line->text + line->length, bytes, length);
    for (i = 0; i < length; i++)
        line->positions[line->length + i] = position;
    line->length += length;
    return true;
}

static bool append_cell(SearchLine *line, Term *term, uint32_t row,
                        uint16_t column, uint64_t bias)
{
    GhosttyPoint point = { .tag = GHOSTTY_POINT_TAG_SCREEN,
                           .value = { .coordinate = { column, row } } };
    GhosttyGridRef ref = {0};
    GhosttyCell cell;
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    uint32_t stack[8], *codepoints = stack;
    size_t count = 0, i;
    GhosttyResult result;
    SearchPosition pos;
    bool ok = true;

    if (ghostty_terminal_grid_ref(term->handle, point, &ref) != GHOSTTY_SUCCESS ||
        ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS)
        return false;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL ||
        wide == GHOSTTY_CELL_WIDE_SPACER_HEAD)
        return true;
    pos.row = bias + row;
    pos.column = column;
    pos.width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
    result = ghostty_grid_ref_graphemes(&ref, stack, 8, &count);
    if (result == GHOSTTY_OUT_OF_SPACE) {
        if (count > SIZE_MAX / sizeof *codepoints)
            return false;
        codepoints = malloc(count * sizeof *codepoints);
        if (!codepoints)
            return false;
        result = ghostty_grid_ref_graphemes(&ref, codepoints, count, &count);
    }
    if (result != GHOSTTY_SUCCESS) {
        ok = false;
    } else if (!count) {
        ok = line_append(line, " ", 1, pos);
    } else {
        for (i = 0; i < count && ok; i++) {
            char utf8[8];
            char *end = SDL_UCS4ToUTF8(codepoints[i], utf8);
            ok = line_append(line, utf8, (size_t)(end - utf8), pos);
        }
        line->meaningful = line->length;
    }
    if (codepoints != stack)
        free(codepoints);
    return ok;
}

static bool add_match(Search *s, SearchPosition first, SearchPosition last)
{
    SearchMatch *m;
    if (s->first + s->count == s->capacity) {
        if (s->first >= s->capacity / 2 && s->first) {
            memmove(s->matches, s->matches + s->first,
                    s->count * sizeof *s->matches);
            s->first = 0;
        } else {
            size_t capacity = s->capacity ? s->capacity * 2 : 64;
            SearchMatch *matches;
            if (capacity < s->capacity || capacity > SIZE_MAX / sizeof *matches)
                return false;
            matches = realloc(s->matches, capacity * sizeof *matches);
            if (!matches)
                return false;
            s->matches = matches;
            s->capacity = capacity;
        }
    }
    m = &s->matches[s->first + s->count++];
    m->first_row = first.row;
    m->first_column = first.column;
    m->last_row = last.row;
    m->last_column = (uint16_t)(last.column + last.width - 1);
    return true;
}

/* A single regex step. Empty matches are retried anchored for a non-empty
 * alternative, then advanced by a UTF-8 character. */
static bool match_step(Search *s, SearchWork *w, bool *finished)
{
    SearchLine *line = &w->line;
    size_t length = line->meaningful;
    PCRE2_SIZE *ov;
    int rc = pcre2_match(w->code,
                         (PCRE2_SPTR)(line->text ? line->text : ""),
                         length, w->match_offset,
                         w->match_flags | PCRE2_NO_UTF_CHECK,
                         w->data, w->context);
    *finished = false;
    if (rc == PCRE2_ERROR_NOMATCH) {
        if (!w->match_flags || w->match_offset >= length) {
            *finished = true;
            return true;
        }
        w->match_flags = 0;
        w->match_offset++;
        while (w->match_offset < length &&
               ((unsigned char)line->text[w->match_offset] & 0xc0) == 0x80)
            w->match_offset++;
        return true;
    }
    if (rc < 0) {
        snprintf(s->error, sizeof s->error, "Regex match error %d", rc);
        return false;
    }
    ov = pcre2_get_ovector_pointer(w->data);
    if (ov[1] > ov[0]) {
        if (!add_match(s, line->positions[ov[0]], line->positions[ov[1] - 1]))
            return false;
        if (!w->selection_explicit)
            s->selected = s->count - 1;
        w->match_offset = ov[1];
        w->match_flags = 0;
    } else if (ov[1] >= length) {
        *finished = true;
    } else {
        w->match_offset = ov[1];
        w->match_flags = PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
    }
    return true;
}

static bool search_start(Search *s, Term *term)
{
    SearchWork *w;
    GhosttyTerminalScrollbar bar;
    int error_code;
    PCRE2_SIZE error_offset;
    uint32_t options = PCRE2_UTF;
    work_free(s->work);
    s->work = NULL;
    s->first = s->count = 0;
    s->selected = SIZE_MAX;
    s->row_bias = 0;
    s->running = false;
    s->dirty = false;
    s->error[0] = '\0';
    if (!s->open || !s->length)
        return true;
    w = calloc(1, sizeof *w);
    if (!w)
        return false;
    s->work = w;
    if (!s->case_sensitive)
        options |= PCRE2_CASELESS;
    options |= s->regex ? (PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C) : PCRE2_LITERAL;
    w->code = pcre2_compile((PCRE2_SPTR)s->query, s->length, options,
                            &error_code, &error_offset, NULL);
    if (!w->code) {
        PCRE2_UCHAR message[96];
        if (pcre2_get_error_message(error_code, message, sizeof message) < 0)
            snprintf((char *)message, sizeof message, "error %d", error_code);
        snprintf(s->error, sizeof s->error, "Regex at %zu: %s",
                 (size_t)error_offset, (char *)message);
        return false;
    }
    w->data = pcre2_match_data_create_from_pattern(w->code, NULL);
    w->context = pcre2_match_context_create(NULL);
    if (!w->data || !w->context || !term_scrollbar(term, &bar) ||
        ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                             &w->screen) != GHOSTTY_SUCCESS ||
        bar.total > (uint64_t)UINT32_MAX + 1)
        return false;
    pcre2_set_match_limit(w->context, 100000);
    pcre2_set_depth_limit(w->context, 1000);
    w->total = bar.total;
    w->history_end = bar.total > term->rows ? bar.total - term->rows : 0;
    w->reveal_pending = true;
    s->running = true;
    return true;
}

/* Track the newest scanned history row, including a partial wrapped line.
 * It gives earlier results their displacement after scrollback eviction. */
static bool search_track_history(Search *s, Term *term)
{
    SearchWork *w = s->work;
    GhosttyPoint point = {0};
    uint64_t row;
    if (w->row < w->history_end && w->row_started)
        row = w->row;
    else {
        row = w->row < w->history_end ? w->row : w->history_end;
        if (row <= s->row_bias)
            return true;
        row--;
    }
    if (w->anchor && w->anchor_row == row)
        return true;
    point.tag = GHOSTTY_POINT_TAG_SCREEN;
    point.value.coordinate.x = 0;
    point.value.coordinate.y = (uint32_t)(row - s->row_bias);
    if (w->anchor) {
        if (ghostty_tracked_grid_ref_set(w->anchor, term->handle, point) !=
            GHOSTTY_SUCCESS)
            return false;
    } else if (ghostty_terminal_grid_ref_track(term->handle, point, &w->anchor) !=
               GHOSTTY_SUCCESS) {
        return false;
    }
    w->anchor_row = row;
    return true;
}

void search_output_changed(Search *s, Term *term)
{
    SearchWork *w = s->work;
    GhosttyPointCoordinate anchor;
    GhosttyTerminalScrollbar bar;
    GhosttyTerminalScreen screen;
    uint64_t bias;
    size_t low, high, removed;
    bool stable;
    if (!s->open || s->dirty || !w)
        return;
    if (!w->anchor || ghostty_tracked_grid_ref_point(w->anchor,
            GHOSTTY_POINT_TAG_SCREEN, &anchor) != GHOSTTY_SUCCESS ||
        ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                             &screen) != GHOSTTY_SUCCESS || screen != w->screen ||
        !term_scrollbar(term, &bar) || w->anchor_row < anchor.y) {
        search_invalidate(s);
        return;
    }
    bias = w->anchor_row - anchor.y;
    if (bias < s->row_bias || bar.total > (uint64_t)UINT32_MAX + 1 ||
        bias > UINT64_MAX - bar.total) {
        search_invalidate(s);
        return;
    }
    stable = s->running && w->line_start >= bias &&
             (w->row < w->history_end ||
              (w->row == w->history_end && !w->row_started));
    if (w->safe_row < bias) {
        if (!stable) {
            search_invalidate(s);
            return;
        }
        w->safe_row = w->line_start;
    }
    s->row_bias = bias;
    /* Drop evicted results without moving the remaining match array. */
    low = 0;
    high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (s->matches[s->first + mid].first_row < bias)
            low = mid + 1;
        else
            high = mid;
    }
    removed = low;
    s->first += removed;
    s->count -= removed;
    if (s->selected < removed)
        s->selected = SIZE_MAX;
    else if (s->selected != SIZE_MAX)
        s->selected -= removed;
    /* Only the active tail can change. If the in-progress logical line is
     * still entirely in history, keep even its partial regex progress. */
    if (!stable) {
        low = 0;
        high = s->count;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (s->matches[s->first + mid].first_row < w->safe_row)
                low = mid + 1;
            else
                high = mid;
        }
        s->count = low;
        w->row = w->line_start = w->safe_row;
        w->column = 0;
        w->row_started = w->matching = false;
        w->line.length = w->line.meaningful = 0;
        w->match_offset = w->match_flags = 0;
    }
    if (s->selected >= s->count) {
        s->selected = s->count ? s->count - 1 : SIZE_MAX;
        w->selection_explicit = false;
    }
    w->total = bias + bar.total;
    w->history_end = bias + (bar.total > term->rows ? bar.total - term->rows : 0);
    s->running = true;
}

static void reveal_selected(Search *s, Term *term)
{
    GhosttyTerminalScrollbar bar;
    SearchMatch *match;
    uint64_t first, last;
    if (s->selected >= s->count)
        return;
    match = &s->matches[s->first + s->selected];
    if (match->first_row < s->row_bias)
        return;
    first = match->first_row - s->row_bias;
    last = match->last_row - s->row_bias;
    if (term_scrollbar(term, &bar) &&
        (first < bar.offset || last >= bar.offset + bar.len))
        term_scroll_row(term, (uint32_t)first);
}

void search_refresh(Search *s, Term *term)
{
    SearchWork *w;
    unsigned cells = 0, matches = 0;
    bool ok = true;
    if (s->dirty && !search_start(s, term))
        ok = false;
    w = s->work;
    if (!ok || !s->running || !w)
        goto done;
    while (cells < 2048 && matches < 64 && s->running) {
        if (w->matching) {
            bool finished;
            matches++;
            if (!match_step(s, w, &finished)) {
                ok = false;
                break;
            }
            if (finished) {
                w->matching = false;
                w->line.length = w->line.meaningful = 0;
                w->match_offset = w->match_flags = 0;
                if (w->row <= w->history_end)
                    w->safe_row = w->row;
                w->line_start = w->row;
                if (w->row == w->total)
                    s->running = false;
            }
            continue;
        }
        if (w->row >= w->total) {
            s->running = false;
            break;
        }
        if (!w->row_started) {
            GhosttyPoint point = { .tag = GHOSTTY_POINT_TAG_SCREEN,
                .value = { .coordinate = { 0, (uint32_t)(w->row - s->row_bias) } } };
            GhosttyGridRef ref = {0};
            GhosttyRow row;
            if (ghostty_terminal_grid_ref(term->handle, point, &ref) != GHOSTTY_SUCCESS ||
                ghostty_grid_ref_row(&ref, &row) != GHOSTTY_SUCCESS) {
                ok = false;
                break;
            }
            w->row_wrapped = false;
            ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &w->row_wrapped);
            w->row_started = true;
        }
        if (w->column < term->columns) {
            if (!append_cell(&w->line, term, (uint32_t)(w->row - s->row_bias),
                             w->column, s->row_bias)) {
                ok = false;
                break;
            }
            w->column++;
            cells++;
        }
        if (w->column == term->columns) {
            w->column = 0;
            w->row_started = false;
            w->row++;
            if (!w->row_wrapped || w->row == w->total)
                w->matching = true;
        }
    }
    if (ok && !search_track_history(s, term))
        ok = false;
    if (ok && !s->running && w->pending_direction && s->count) {
        if (w->pending_direction > 0)
            s->selected = (s->selected + 1) % s->count;
        else
            s->selected = (s->selected + s->count - 1) % s->count;
        w->pending_direction = 0;
        w->reveal_pending = true;
    }
    if (ok && !s->running && w->reveal_pending) {
        reveal_selected(s, term);
        w->reveal_pending = false;
    }
done:
    if (!ok) {
        s->first = s->count = 0;
        s->selected = SIZE_MAX;
        s->running = false;
        if (!s->error[0])
            snprintf(s->error, sizeof s->error, "Search unavailable");
        work_free(s->work);
        s->work = NULL;
    }
}

void search_navigate(Search *s, Term *term, int direction)
{
    search_refresh(s, term);
    if (!s->count) {
        if (direction && s->running && s->work)
            s->work->pending_direction = direction;
        return;
    }
    if (s->selected >= s->count)
        s->selected = s->count - 1;
    else if (direction > 0)
        s->selected = (s->selected + 1) % s->count;
    else if (direction < 0)
        s->selected = (s->selected + s->count - 1) % s->count;
    if (direction && s->work) {
        s->work->selection_explicit = true;
        s->work->reveal_pending = false;
    }
    if (direction || !s->running)
        reveal_selected(s, term);
}

bool search_insert(Search *s, const char *text)
{
    size_t n;
    if (!text)
        return false;
    n = strlen(text);
    if (!n || n >= sizeof s->query - s->length)
        return false;
    memmove(s->query + s->cursor + n, s->query + s->cursor,
            s->length - s->cursor + 1);
    memcpy(s->query + s->cursor, text, n);
    s->cursor += n;
    s->length += n;
    search_invalidate(s);
    return true;
}

void search_move_cursor(Search *s, int direction)
{
    if (direction < 0 && s->cursor) {
        s->cursor--;
        while (s->cursor && ((unsigned char)s->query[s->cursor] & 0xc0) == 0x80)
            s->cursor--;
    } else if (direction > 0 && s->cursor < s->length) {
        s->cursor++;
        while (s->cursor < s->length &&
               ((unsigned char)s->query[s->cursor] & 0xc0) == 0x80)
            s->cursor++;
    }
}

void search_backspace(Search *s)
{
    size_t end = s->cursor;
    search_move_cursor(s, -1);
    if (s->cursor != end) {
        memmove(s->query + s->cursor, s->query + end, s->length - end + 1);
        s->length -= end - s->cursor;
        search_invalidate(s);
    }
}

void search_delete(Search *s)
{
    size_t start = s->cursor;
    search_move_cursor(s, 1);
    if (s->cursor != start) {
        memmove(s->query + start, s->query + s->cursor,
                s->length - s->cursor + 1);
        s->length -= s->cursor - start;
        s->cursor = start;
        search_invalidate(s);
    }
}

int search_cell(const Search *s, uint64_t row, unsigned column)
{
    size_t i, low = 0, high = s->count;
    int found = 0;
    if (!s->open || s->dirty)
        return 0;
    if (row > UINT64_MAX - s->row_bias)
        return 0;
    row += s->row_bias;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (s->matches[s->first + mid].last_row < row)
            low = mid + 1;
        else
            high = mid;
    }
    for (i = low; i < s->count; i++) {
        const SearchMatch *m = &s->matches[s->first + i];
        if (row < m->first_row)
            break;
        if (row > m->last_row || (row == m->first_row && column < m->first_column) ||
            (row == m->last_row && column > m->last_column))
            continue;
        found = i == s->selected ? 2 : 1;
        if (found == 2)
            break;
    }
    return found;
}
