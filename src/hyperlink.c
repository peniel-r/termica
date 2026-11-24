/* Link lookup uses grid references only during the call. Rendering remains
 * on Ghostty's render-state API, and no selection state is touched. */
#include "hyperlink.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct LinkPosition {
    uint16_t x;
    uint32_t y;                 /* screen, not viewport, coordinates */
} LinkPosition;

typedef struct LinkGrid {
    Term *term;
    uint32_t top;
    uint32_t total;
} LinkGrid;

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool prefix_equal(const char *text, size_t length, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i]; i++) {
        if (i >= length || ascii_lower((unsigned char)text[i]) !=
                           (unsigned char)prefix[i])
            return false;
    }
    return true;
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = ascii_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool unicode_space(uint32_t cp)
{
    return cp <= 0x20 || (cp >= 0x7f && cp <= 0xa0) || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

/* Reject malformed UTF-8 rather than letting the OS repair it into a
 * different URI. Also reject C1 controls and malformed percent escapes. */
static bool uri_bytes_valid(const char *uri, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)uri[i++];
        uint32_t cp;
        unsigned int count, j;
        if (c < 0x80) {
            if (c <= 0x20 || c == 0x7f || c == '\\' || c == '"' ||
                c == '<' || c == '>' || c == '`')
                return false;
            if (c == '%') {
                int a, b;
                if (length - i < 2 || (a = hex_value((unsigned char)uri[i])) < 0 ||
                    (b = hex_value((unsigned char)uri[i + 1])) < 0)
                    return false;
                if ((a * 16 + b) < 0x20 || (a * 16 + b) == 0x7f)
                    return false;
                i += 2;
            }
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; count = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; count = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07; count = 3; }
        else return false;
        if (length - i < count) return false;
        for (j = 0; j < count; j++) {
            unsigned char next = (unsigned char)uri[i++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || unicode_space(cp))
            return false;
    }
    return true;
}

bool hyperlink_uri_allowed(const char *uri, size_t length)
{
    size_t authority = 0, end, host, i;
    if (!uri || length == 0 || length > HYPERLINK_URI_MAX ||
        !uri_bytes_valid(uri, length))
        return false;
    if (prefix_equal(uri, length, "http://")) authority = 7;
    else if (prefix_equal(uri, length, "https://")) authority = 8;
    else if (prefix_equal(uri, length, "mailto:"))
        return length > 7 && uri[7] != '?' && uri[7] != '#';
    else return false;

    end = authority;
    while (end < length && uri[end] != '/' && uri[end] != '?' &&
           uri[end] != '#') end++;
    host = authority;
    for (i = authority; i < end; i++)
        if (uri[i] == '@') host = i + 1;
    if (host == end) return false;
    if (uri[host] == '[') {
        i = host + 1;
        while (i < end && uri[i] != ']') i++;
        if (i == host + 1 || i == end) return false;
        i++;
    } else {
        i = host;
        while (i < end && uri[i] != ':') {
            if (uri[i] == '[' || uri[i] == ']') return false;
            i++;
        }
        if (i == host) return false;
    }
    if (i < end) {
        if (uri[i++] != ':' || i == end) return false;
        while (i < end) {
            if (uri[i] < '0' || uri[i] > '9') return false;
            i++;
        }
    }
    return true;
}

bool hyperlink_open(const char *uri)
{
    size_t length;
    if (!uri) return false;
    /* Do not inspect more than the maximum accepted input. */
    for (length = 0; length <= HYPERLINK_URI_MAX && uri[length]; length++) {}
    if (length > HYPERLINK_URI_MAX || !hyperlink_uri_allowed(uri, length))
        return false;
#ifdef _WIN32
    {
        wchar_t wide[HYPERLINK_URI_MAX + 1];
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        uri, (int)length, wide,
                                        HYPERLINK_URI_MAX);
        if (!count) return false;
        wide[count] = L'\0';
        return (INT_PTR)ShellExecuteW(NULL, L"open", wide, NULL, NULL,
                                     SW_SHOWNORMAL) > 32;
    }
#else
    return false;
#endif
}

bool hyperlink_contains(const Hyperlink *link, int x, int y)
{
    if (!link || x < 0 || y < 0 || y < link->start_y || y > link->end_y)
        return false;
    if (y == link->start_y && x < link->start_x) return false;
    if (y == link->end_y && x > link->end_x) return false;
    return true;
}

static bool grid_ref(const LinkGrid *grid, LinkPosition pos, GhosttyGridRef *ref)
{
    GhosttyPoint point = { 0 };
    if (pos.x >= grid->term->columns || pos.y >= grid->total) return false;
    if (pos.y >= grid->top && (uint64_t)pos.y - grid->top < grid->term->rows) {
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.y = pos.y - grid->top;
    } else {
        point.tag = GHOSTTY_POINT_TAG_SCREEN;
        point.value.coordinate.y = pos.y;
    }
    point.value.coordinate.x = pos.x;
    return ghostty_terminal_grid_ref(grid->term->handle, point, ref) ==
           GHOSTTY_SUCCESS;
}

static bool row_wraps(const LinkGrid *grid, uint32_t row)
{
    GhosttyGridRef ref = { 0 };
    GhosttyRow raw;
    bool wrap = false;
    LinkPosition pos = { 0, row };
    return grid_ref(grid, pos, &ref) &&
           ghostty_grid_ref_row(&ref, &raw) == GHOSTTY_SUCCESS &&
           ghostty_row_get(raw, GHOSTTY_ROW_DATA_WRAP, &wrap) == GHOSTTY_SUCCESS &&
           wrap;
}

static bool previous(const LinkGrid *grid, LinkPosition *pos)
{
    if (pos->x) { pos->x--; return true; }
    if (!pos->y || !row_wraps(grid, pos->y - 1)) return false;
    pos->y--;
    pos->x = (uint16_t)(grid->term->columns - 1);
    return true;
}

static bool next(const LinkGrid *grid, LinkPosition *pos)
{
    if ((unsigned int)pos->x + 1 < grid->term->columns) {
        pos->x++;
        return true;
    }
    if ((uint64_t)pos->y + 1 >= grid->total || !row_wraps(grid, pos->y))
        return false;
    pos->x = 0;
    pos->y++;
    return true;
}

static bool cell_info(const LinkGrid *grid, LinkPosition pos, GhosttyGridRef *ref,
                      uint32_t *cp, GhosttyCellWide *wide)
{
    GhosttyCell cell;
    return grid_ref(grid, pos, ref) &&
           ghostty_grid_ref_cell(ref, &cell) == GHOSTTY_SUCCESS &&
           ghostty_cell_get(cell, GHOSTTY_CELL_DATA_CODEPOINT, cp) == GHOSTTY_SUCCESS &&
           ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, wide) == GHOSTTY_SUCCESS;
}

/* A wide glyph's spacer may not carry the link metadata itself. */
static GhosttyResult cell_uri(const LinkGrid *grid, LinkPosition pos,
                              uint8_t *buffer, size_t capacity, size_t *written)
{
    GhosttyGridRef ref = { 0 };
    GhosttyCellWide wide;
    uint32_t cp;
    GhosttyResult result;
    if (!cell_info(grid, pos, &ref, &cp, &wide)) return GHOSTTY_INVALID_VALUE;
    result = ghostty_grid_ref_hyperlink_uri(&ref, buffer, capacity, written);
    if (result != GHOSTTY_SUCCESS || *written)
        return result;
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
        if (!previous(grid, &pos)) return result;
    } else if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
        if (!next(grid, &pos)) return result;
    } else return result;
    if (!cell_info(grid, pos, &ref, &cp, &wide) ||
        wide != GHOSTTY_CELL_WIDE_WIDE)
        return result;
    return ghostty_grid_ref_hyperlink_uri(&ref, buffer, capacity, written);
}

static bool same_uri(const LinkGrid *grid, LinkPosition pos,
                     const char *uri, size_t len)
{
    uint8_t bytes[HYPERLINK_URI_MAX];
    size_t written = 0;
    if (cell_uri(grid, pos, bytes, len, &written) != GHOSTTY_SUCCESS)
        return false;
    return written == len && memcmp(bytes, uri, len) == 0;
}

static void set_range(const LinkGrid *grid, Hyperlink *out,
                      LinkPosition first, LinkPosition last)
{
    out->start_x = first.x;
    out->start_y = (int)((int64_t)first.y - grid->top);
    out->end_x = last.x;
    out->end_y = (int)((int64_t)last.y - grid->top);
}

static bool is_token_cell(const LinkGrid *grid, LinkPosition pos)
{
    GhosttyGridRef ref = { 0 };
    GhosttyCellWide wide;
    uint32_t cp;
    if (!cell_info(grid, pos, &ref, &cp, &wide)) return false;
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL ||
        wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) return true;
    return !unicode_space(cp) && cp != '"' && cp != '\'' &&
           cp != '<' && cp != '>' && cp != '`' && cp != '\\';
}

static size_t encode_utf8(uint32_t cp, char *out)
{
    if (cp <= 0x7f) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7ff) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f)); return 2;
    }
    if (cp <= 0xffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f)); return 3;
    }
    if (cp <= 0x10ffff && cp > 0xffff) {
        out[0] = (char)(0xf0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (char)(0x80 | (cp & 0x3f)); return 4;
    }
    return 0;
}

static bool token_start_boundary(char c)
{
    return c == '(' || c == '[' || c == '{';
}

static size_t trim_url(const char *s, size_t begin, size_t end)
{
    size_t i;
    int parens = 0, brackets = 0, braces = 0;
    for (i = begin; i < end; i++) {
        switch (s[i]) {
        case '(': parens++; break;
        case '[': brackets++; break;
        case '{': braces++; break;
        case ')': if (!parens) { end = i; goto trailing; } parens--; break;
        case ']': if (!brackets) { end = i; goto trailing; } brackets--; break;
        case '}': if (!braces) { end = i; goto trailing; } braces--; break;
        default: break;
        }
    }
trailing:
    while (end > begin) {
        char c = s[end - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
            c == '?') end--;
        else break;
    }
    return end;
}

static bool detected_link(const LinkGrid *grid, LinkPosition hover, Hyperlink *out)
{
    char text[HYPERLINK_URI_MAX + 1];
    /* Each byte maps to its cell's offset from the beginning of the token. */
    uint16_t byte_cell[HYPERLINK_URI_MAX];
    LinkPosition first = hover, pos, before, last;
    size_t count = 0, cells = 0, begin, end, i;
    size_t hover_cell = 0;
    bool stopped = false;

    if (!is_token_cell(grid, hover)) return false;
    while (cells < HYPERLINK_URI_MAX) {
        before = first;
        if (!previous(grid, &before) || !is_token_cell(grid, before)) break;
        first = before;
        cells++;
    }
    if (cells == HYPERLINK_URI_MAX) return false;
    pos = first;
    cells = 0;
    for (;;) {
        GhosttyGridRef ref = { 0 };
        GhosttyCellWide wide;
        uint32_t cp;
        uint32_t stack_graphemes[32], *graphemes = stack_graphemes;
        size_t grapheme_count = 0, j;
        GhosttyResult grapheme_result;
        if (!is_token_cell(grid, pos) || !cell_info(grid, pos, &ref, &cp, &wide)) {
            stopped = true;
            break;
        }
        if (pos.x == hover.x && pos.y == hover.y) hover_cell = cells;
        if (wide != GHOSTTY_CELL_WIDE_SPACER_TAIL &&
            wide != GHOSTTY_CELL_WIDE_SPACER_HEAD) {
            grapheme_result = ghostty_grid_ref_graphemes(&ref, graphemes,
                sizeof stack_graphemes / sizeof stack_graphemes[0],
                &grapheme_count);
            if (grapheme_result == GHOSTTY_OUT_OF_SPACE &&
                grapheme_count <= HYPERLINK_URI_MAX - count) {
                graphemes = (uint32_t *)malloc(grapheme_count * sizeof *graphemes);
                if (!graphemes) return false;
                grapheme_result = ghostty_grid_ref_graphemes(&ref, graphemes,
                    grapheme_count, &grapheme_count);
            }
            if (grapheme_result != GHOSTTY_SUCCESS || !grapheme_count) {
                if (graphemes != stack_graphemes) free(graphemes);
                return false;
            }
            for (j = 0; j < grapheme_count; j++) {
                char encoded[4];
                size_t n = encode_utf8(graphemes[j], encoded), k;
                if (!n || count + n > HYPERLINK_URI_MAX) {
                    if (graphemes != stack_graphemes) free(graphemes);
                    return false;
                }
                for (k = 0; k < n; k++) {
                    text[count] = encoded[k];
                    byte_cell[count++] = (uint16_t)cells;
                }
            }
            if (graphemes != stack_graphemes) free(graphemes);
        }
        cells++;
        if (cells > HYPERLINK_URI_MAX) return false;
        if (!next(grid, &pos)) { stopped = true; break; }
    }
    if (!stopped) return false;
    text[count] = '\0';
    for (begin = 0; begin < count; begin++) {
        if (begin && !token_start_boundary(text[begin - 1])) continue;
        if (!prefix_equal(text + begin, count - begin, "http://") &&
            !prefix_equal(text + begin, count - begin, "https://") &&
            !prefix_equal(text + begin, count - begin, "mailto:")) continue;
        end = trim_url(text, begin, count);
        if (end <= begin || !hyperlink_uri_allowed(text + begin, end - begin))
            continue;
        /* A wide character's trailing spacer is part of the clickable cell. */
        i = byte_cell[end - 1];
        if (i + 1 < cells) {
            LinkPosition tail = first;
            size_t n;
            GhosttyGridRef ref = { 0 };
            GhosttyCellWide wide;
            uint32_t cp;
            for (n = 0; n <= i; n++) if (!next(grid, &tail)) break;
            if (n > i && cell_info(grid, tail, &ref, &cp, &wide) &&
                wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) i++;
        }
        if (hover_cell < byte_cell[begin] || hover_cell > i) continue;
        pos = first;
        for (cells = 0; cells < byte_cell[begin]; cells++)
            if (!next(grid, &pos)) return false;
        last = pos;
        for (cells = byte_cell[begin]; cells < i; cells++)
            if (!next(grid, &last)) return false;
        memcpy(out->uri, text + begin, end - begin);
        out->uri[end - begin] = '\0';
        set_range(grid, out, pos, last);
        return true;
    }
    return false;
}

bool hyperlink_at(Term *term, int x, int y, Hyperlink *out)
{
    GhosttyGridRef ref = { 0 };
    GhosttyPoint point = { 0 };
    GhosttyPointCoordinate screen;
    GhosttyTerminalScrollbar bar;
    LinkGrid grid;
    LinkPosition hover, first, last, candidate;
    size_t length = 0;
    GhosttyResult result;

    if (!term || !term->handle || !out || term->columns == 0 ||
        x < 0 || y < 0 || x >= term->columns || y >= term->rows)
        return false;
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = (uint16_t)x;
    point.value.coordinate.y = (uint32_t)y;
    if (ghostty_terminal_grid_ref(term->handle, point, &ref) != GHOSTTY_SUCCESS ||
        ghostty_terminal_point_from_grid_ref(term->handle, &ref,
            GHOSTTY_POINT_TAG_SCREEN, &screen) != GHOSTTY_SUCCESS ||
        ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                             &bar) != GHOSTTY_SUCCESS ||
        bar.offset > UINT32_MAX || bar.total > UINT32_MAX)
        return false;
    grid.term = term;
    grid.top = (uint32_t)bar.offset;
    grid.total = (uint32_t)bar.total;
    hover.x = screen.x;
    hover.y = screen.y;
    result = cell_uri(&grid, hover, (uint8_t *)out->uri,
                      HYPERLINK_URI_MAX, &length);
    /* A disallowed OSC 8 URI must not fall through to its visible label. */
    if (result == GHOSTTY_OUT_OF_SPACE || length > HYPERLINK_URI_MAX)
        return false;
    if (result != GHOSTTY_SUCCESS) return false;
    if (!length) return detected_link(&grid, hover, out);
    if (!hyperlink_uri_allowed(out->uri, length)) return false;
    out->uri[length] = '\0';
    first = last = hover;
    /* Only the visible part is needed for painting. Limiting the walk to
     * the viewport avoids traversing an arbitrarily long OSC 8 label in
     * scrollback without truncating its visible clickable range. */
    for (;;) {
        candidate = first;
        if (!previous(&grid, &candidate) || candidate.y < grid.top ||
            !same_uri(&grid, candidate, out->uri, length)) break;
        first = candidate;
    }
    for (;;) {
        candidate = last;
        if (!next(&grid, &candidate) ||
            (uint64_t)candidate.y - grid.top >= term->rows ||
            !same_uri(&grid, candidate, out->uri, length)) break;
        last = candidate;
    }
    set_range(&grid, out, first, last);
    return true;
}
