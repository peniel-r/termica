/*
 * termica - frame rendering from libghostty's render state.
 * See render.h.
 */
#include "render.h"

#include "chrome.h"
#include "hyperlink.h"
#include "style.h"
#include "toast.h"

#include <limits.h>
#include <string.h>

/* Everything the cell drawing needs from the frame it belongs to. */
typedef struct FrameContext {
    App                      *app;
    GhosttyRenderStateColors  colors;
    SDL_Color                 default_foreground;
    SDL_Color                 default_background;
    SDL_Color                 cursor_color;
    GhosttyCursorStyle        cursor_style;
    const Hyperlink          *hovered_link;
    int                       row_offset; /* window y mapped to target y */
} FrameContext;

typedef struct CellPaint {
    int             column;
    int             row;
    GhosttyStyle    style;
    bool            has_foreground;
    GhosttyColorRgb foreground;
    bool            has_background;
    GhosttyColorRgb background;
    const uint8_t  *text;
    size_t          text_length;
    bool            at_cursor;
    bool            selected;
    int             search_match;
    bool            wide;
    bool            hyperlink;
} CellPaint;

static float cell_x(const App *app, int column)
{
    return (float)(app->grid_left + column * app->glyphs.cell_width);
}

static float cell_y(const App *app, int row)
{
    return (float)(app->grid_top + row * app->glyphs.cell_height);
}

/* A block cursor covers the glyph, so the glyph is drawn again in the
 * background color: a real terminal's white-on-black cursor. */
static void draw_cell_glyph(const FrameContext *frame, const CellPaint *cell,
                            const CellStyle *style, bool covered)
{
    App *app = frame->app;

    if (style->hidden || cell->text_length == 0)
        return;
    glyphs_draw_text(&app->glyphs, (const char *)cell->text, cell->text_length,
                     cell_x(app, cell->column),
                     cell_y(app, cell->row) - frame->row_offset,
                     style->bold, style->italic,
                     covered ? frame->default_background : style->foreground);
}

static void draw_cell_cursor(const FrameContext *frame, const CellPaint *cell,
                             const CellStyle *style)
{
    App *app = frame->app;
    float x = cell_x(app, cell->column);
    float y = cell_y(app, cell->row) - frame->row_offset;
    float width = (float)app->glyphs.cell_width;
    float height = (float)app->glyphs.cell_height;

    switch (frame->cursor_style) {
    case GHOSTTY_CURSOR_STYLE_BAR:
        chrome_fill_rect(app->renderer, x, y, 2.0f, height,
                         frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_UNDERLINE:
        chrome_fill_rect(app->renderer, x, y + height - 2.0f, width, 2.0f,
                         frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW:
        chrome_fill_rect(app->renderer, x, y, width, 1.0f,
                         frame->cursor_color);
        chrome_fill_rect(app->renderer, x, y + height - 1.0f, width, 1.0f,
                         frame->cursor_color);
        chrome_fill_rect(app->renderer, x, y, 1.0f, height,
                         frame->cursor_color);
        chrome_fill_rect(app->renderer, x + width - 1.0f, y, 1.0f, height,
                         frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_BLOCK:
    default:
        chrome_fill_rect(app->renderer, x, y, width, height,
                         frame->cursor_color);
        draw_cell_glyph(frame, cell, style, true);
        break;
    }
}

/* The decoration under a cell. Dotted and dashed patterns are phased from
 * the cell's own x, so a run of underlined cells reads as one continuous
 * line instead of one pattern per cell, and the curly kind gets exactly one
 * period per cell - which is what a spell-check wave looks like. */
static void draw_underline(App *app, float x, float base, float width,
                           SDL_Color color, int style)
{
    float offset;
    float phase;
    int pixel;
    int pixels;

    switch (style) {
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        chrome_fill_rect(app->renderer, x, base, width, 1.0f, color);
        chrome_fill_rect(app->renderer, x, base - 3.0f, width, 1.0f, color);
        break;
    case GHOSTTY_SGR_UNDERLINE_CURLY:
        pixels = (int)(width + 0.5f);
        for (pixel = 0; pixel < pixels; pixel++) {
            float wave = SDL_sinf((float)pixel / (float)pixels * 6.2831855f);
            float y = base + (wave < 0.0f ? -1.0f : 0.0f);
            chrome_fill_rect(app->renderer, x + (float)pixel, y, 1.0f, 1.0f,
                             color);
        }
        break;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        /* one pixel of ink, one pixel of gap, continuing across the row */
        phase = (float)((int)SDL_fmod(x, 2.0));
        for (offset = phase > 0.0f ? 2.0f - phase : 0.0f;
             offset + 1.0f <= width; offset += 2.0f)
            chrome_fill_rect(app->renderer, x + offset, base, 1.0f, 1.0f,
                             color);
        break;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        /* three pixels of ink, two of gap, continuing across the row */
        phase = (float)((int)SDL_fmod(x, 5.0));
        for (offset = phase > 0.0f ? 5.0f - phase : 0.0f; offset < width;
             offset += 5.0f) {
            float run = width - offset;
            if (run > 3.0f)
                run = 3.0f;
            chrome_fill_rect(app->renderer, x + offset, base, run, 1.0f, color);
        }
        break;
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
    default:
        chrome_fill_rect(app->renderer, x, base, width, 2.0f, color);
        break;
    }
}

static void draw_cell(const FrameContext *frame, const CellPaint *cell)
{
    App *app = frame->app;
    CellStyle style;
    float x = cell_x(app, cell->column);
    float y = cell_y(app, cell->row) - frame->row_offset;
    float width = (float)app->glyphs.cell_width * (cell->wide ? 2.0f : 1.0f);
    float height = (float)app->glyphs.cell_height;

    const ColorScheme *scheme = &app->settings.colors;
    bool custom_sel = cell->selected &&
                      (scheme->has_selection_bg || scheme->has_selection_fg);

    style_resolve(&style, &cell->style,
                  cell->has_foreground, cell->foreground,
                  cell->has_background, cell->background,
                  &frame->colors, frame->default_foreground,
                  frame->default_background, app->panel_opacity,
                  cell->selected && !custom_sel);
    if (custom_sel) {
        if (scheme->has_selection_bg) {
            style.background.r = scheme->selection_bg.r;
            style.background.g = scheme->selection_bg.g;
            style.background.b = scheme->selection_bg.b;
            style.background.a = 255;
            style.has_background = true;
        }
        if (scheme->has_selection_fg) {
            style.foreground.r = scheme->selection_fg.r;
            style.foreground.g = scheme->selection_fg.g;
            style.foreground.b = scheme->selection_fg.b;
            style.foreground.a = 255;
        }
    }
    if (cell->search_match && !cell->selected) {
        style.background = cell->search_match == 2
            ? (SDL_Color){ 255, 164, 64, 255 }
            : (SDL_Color){ 235, 213, 94, 255 };
        style.foreground = (SDL_Color){ 24, 24, 24, 255 };
        style.has_background = true;
    }

    if (style.has_background)
        chrome_fill_rect(app->renderer, x, y, width, height, style.background);

    draw_cell_glyph(frame, cell, &style, false);

    if (style.underline)
        draw_underline(app, x, y + height - 2.0f, width, style.decoration,
                       style.underline_style);
    else if (cell->hyperlink)
        chrome_fill_rect(app->renderer, x, y + height - 2.0f, width, 1.0f,
                         style.foreground);
    if (style.strikethrough)
        chrome_fill_rect(app->renderer, x, y + height / 2.0f, width, 1.0f,
                         style.decoration);
    if (style.overline)
        chrome_fill_rect(app->renderer, x, y, width, 1.0f, style.decoration);

    if (cell->at_cursor)
        draw_cell_cursor(frame, cell, &style);
}

/* Where the IME should put its candidate window: the cell the terminal
 * cursor is in. The render state already knows it, so this is computed
 * while drawing rather than guessed from history. */
static void update_text_input_area(App *app)
{
    Session *s = app_active_session(app);
    GhosttyRenderStateCursor cursor_state =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    SDL_Rect area;

    if (!s || ghostty_render_state_get(s->term.render_state,
                                       GHOSTTY_RENDER_STATE_DATA_CURSOR,
                                       &cursor_state) != GHOSTTY_SUCCESS)
        return;
    if (!cursor_state.visible || !cursor_state.viewport_has_value)
        return;

    /* Window coordinates, not pixels: divide the layout back out. */
    area.x = (int)((float)(app->grid_left +
                           cursor_state.viewport_x * app->glyphs.cell_width) /
                   app->display_scale);
    area.y = (int)((float)(app->grid_top +
                           cursor_state.viewport_y * app->glyphs.cell_height) /
                   app->display_scale);
    area.w = (int)((float)app->glyphs.cell_width / app->display_scale);
    area.h = (int)((float)app->glyphs.cell_height / app->display_scale);
    SDL_SetTextInputArea(app->window, &area, 0);
}

/* The text the IME is still composing. It is not in the screen buffer, so
 * without this the user types and sees nothing until the commit. */
static void draw_composition(App *app, int cursor_column, int cursor_row)
{
    const Session *s = app_active_session_const(app);
    const char *text = s ? s->composing : NULL;
    int column = cursor_column;

    if (!s || s->composing_length == 0)
        return;

    while (*text && column < app->columns) {
        char utf8[8];
        char *end;
        Uint32 codepoint = SDL_StepUTF8(&text, NULL);

        if (codepoint == 0)
            break;
        end = SDL_UCS4ToUTF8(codepoint, utf8);
        *end = '\0';
        glyphs_draw_text(&app->glyphs, utf8, (size_t)(end - utf8),
                         cell_x(app, column), cell_y(app, cursor_row), false,
                         false, CHROME_STATUS_TEXT);
        column++;
    }
}

/* A row has its own transparent layer: ink may extend into the next row,
 * and compositing layers in order preserves that overlap. The pixels are
 * premultiplied by SDL's normal blend into a transparent target. */
typedef struct CachedRow {
    SDL_Texture *texture;
    GhosttyRenderStateRowId id;
    bool valid;
    int selection_start, selection_end;
    int hover_start, hover_end;
    int cursor_column;
    GhosttyCursorStyle cursor_style;
} CachedRow;

struct RenderCache {
    CachedRow *row;
    uint8_t *search_cells;
    int width, height, rows, columns, grid_left, grid_top, cell_width, cell_height;
    Session *session;
    GhosttyRenderStateColors colors;
    ColorScheme scheme;
    float opacity;
    bool valid, unavailable, search_open;
};

static void cache_release_rows(struct RenderCache *cache)
{
    int i;
    if (cache->row) {
        for (i = 0; i < cache->rows; i++) {
            if (cache->row[i].texture)
                SDL_DestroyTexture(cache->row[i].texture);
        }
    }
    SDL_free(cache->row);
    SDL_free(cache->search_cells);
    cache->row = NULL;
    cache->search_cells = NULL;
    cache->valid = false;
}

void render_invalidate(App *app)
{
    if (app && app->render_cache) {
        app->render_cache->valid = false;
        app->render_cache->unavailable = false;
    }
}

void render_free(App *app)
{
    if (!app || !app->render_cache)
        return;
    cache_release_rows(app->render_cache);
    SDL_free(app->render_cache);
    app->render_cache = NULL;
}

static struct RenderCache *cache_prepare(App *app, int width, int height)
{
    struct RenderCache *cache = app->render_cache;
    bool changed;

    if (!cache) {
        cache = SDL_calloc(1, sizeof *cache);
        if (!cache)
            return NULL;
        app->render_cache = cache;
    }
    changed = cache->width != width || cache->height != height ||
              cache->rows != app->rows || cache->columns != app->columns ||
              cache->grid_left != app->grid_left || cache->grid_top != app->grid_top ||
              cache->cell_width != app->glyphs.cell_width ||
              cache->cell_height != app->glyphs.cell_height;
    if (changed) {
        cache_release_rows(cache);
        cache->unavailable = false;
        cache->width = width;
        cache->height = height;
        cache->rows = app->rows;
        cache->columns = app->columns;
        cache->grid_left = app->grid_left;
        cache->grid_top = app->grid_top;
        cache->cell_width = app->glyphs.cell_width;
        cache->cell_height = app->glyphs.cell_height;
    }
    if (cache->unavailable || width <= 0 || height <= 0 || app->rows <= 0 ||
        app->columns <= 0 || app->glyphs.cell_height <= 0 ||
        app->glyphs.cell_height > INT_MAX / 2 ||
        (size_t)app->rows > SIZE_MAX / (size_t)app->columns)
        return NULL;
    if (!cache->row) {
        cache->row = SDL_calloc((size_t)app->rows, sizeof *cache->row);
        cache->search_cells = SDL_calloc((size_t)app->rows * (size_t)app->columns, 1);
        if (!cache->row || !cache->search_cells) {
            cache_release_rows(cache);
            cache->unavailable = true;
            return NULL;
        }
    }
    return cache;
}

/* Paint one row with the same cell order as the direct renderer. */
static void paint_row(FrameContext *frame, Session *session, int row_position,
                      int selection_start, int selection_end,
                      bool cursor_drawn, int cursor_column, int cursor_row,
                      uint64_t search_offset)
{
    Term *term = &session->term;
    int column = 0;
    while (ghostty_render_state_row_cells_next(term->cell_iterator)) {
        CellPaint cell;
        GhosttyCell raw_cell = 0;
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        uint8_t text_bytes[128];
        GhosttyBuffer grapheme = { .ptr = text_bytes, .cap = sizeof text_bytes };

        ghostty_render_state_row_cells_get(term->cell_iterator,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw_cell);
        ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &wide);
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            column++;
            continue;
        }
        memset(&cell, 0, sizeof cell);
        cell.column = column;
        cell.row = row_position;
        cell.wide = wide == GHOSTTY_CELL_WIDE_WIDE;
        ghostty_render_state_row_cells_get(term->cell_iterator,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &cell.style);
        cell.has_foreground = ghostty_render_state_row_cells_get(term->cell_iterator,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &cell.foreground) == GHOSTTY_SUCCESS;
        cell.has_background = ghostty_render_state_row_cells_get(term->cell_iterator,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &cell.background) == GHOSTTY_SUCCESS;
        ghostty_render_state_row_cells_get(term->cell_iterator,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &grapheme);
        cell.text = grapheme.len <= grapheme.cap ? text_bytes : NULL;
        cell.text_length = cell.text ? grapheme.len : 0;
        cell.at_cursor = cursor_drawn && column == cursor_column && row_position == cursor_row;
        cell.selected = selection_start >= 0 && column >= selection_start && column <= selection_end;
        if (row_position >= 0)
            cell.search_match = search_cell(&session->search,
                search_offset + (uint64_t)row_position, (unsigned)column);
        cell.hyperlink = frame->hovered_link &&
            hyperlink_contains(frame->hovered_link, column, row_position);
        draw_cell(frame, &cell);
        column++;
    }
}

/* A failed target or premultiplied blend falls back to the direct renderer. */
static bool paint_grid(FrameContext *frame, Session *session,
                       struct RenderCache *cache, bool cursor_drawn,
                       int cursor_column, int cursor_row, uint64_t search_offset)
{
    App *app = frame->app;
    Term *term = &session->term;
    GhosttyRenderStateDirty damage = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    bool full = !cache || !cache->valid;
    int i;

    if (cache && (ghostty_render_state_get(term->render_state,
            GHOSTTY_RENDER_STATE_DATA_DIRTY, &damage) != GHOSTTY_SUCCESS ||
        damage == GHOSTTY_RENDER_STATE_DIRTY_FULL || cache->session != session ||
        memcmp(&cache->colors, &frame->colors, sizeof cache->colors) != 0 ||
        memcmp(&cache->scheme, &app->settings.colors, sizeof cache->scheme) != 0 ||
        cache->opacity != app->panel_opacity))
        full = true;
    if (ghostty_render_state_get(term->render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &term->row_iterator) != GHOSTTY_SUCCESS)
        return false;
    while (ghostty_render_state_row_iterator_next(term->row_iterator)) {
        int32_t y = 0;
        int selection_start = -1, selection_end = -1;
        int hover_start = -1, hover_end = -1;
        int cursor = -1;
        bool dirty = true;
        GhosttyRenderStateRowId id = {0};
        GhosttyRenderStateRowSelection selection = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
        CachedRow *row = NULL;

        if (ghostty_render_state_row_get(term->row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y, &y) != GHOSTTY_SUCCESS ||
            y < 0 || y >= app->rows)
            continue;
        if (ghostty_render_state_row_get(term->row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection) == GHOSTTY_SUCCESS) {
            selection_start = selection.start_x;
            selection_end = selection.end_x;
        }
        if (frame->hovered_link && y >= frame->hovered_link->start_y &&
            y <= frame->hovered_link->end_y) {
            hover_start = y == frame->hovered_link->start_y ? frame->hovered_link->start_x : 0;
            hover_end = y == frame->hovered_link->end_y ? frame->hovered_link->end_x : app->columns - 1;
        }
        if (cursor_drawn && y == cursor_row)
            cursor = cursor_column;

        if (cache) {
            bool row_dirty = true;
            bool id_valid;
            uint8_t *search = cache->search_cells + (size_t)y * (size_t)app->columns;
            row = &cache->row[y];
            id_valid = ghostty_render_state_row_get(term->row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_ID, &id) == GHOSTTY_SUCCESS;
            if (ghostty_render_state_row_get(term->row_iterator,
                    GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty) != GHOSTTY_SUCCESS)
                row_dirty = true;
            dirty = full || !row->valid || row_dirty || !id_valid ||
                id.bits[0] != row->id.bits[0] || id.bits[1] != row->id.bits[1] ||
                selection_start != row->selection_start || selection_end != row->selection_end ||
                hover_start != row->hover_start || hover_end != row->hover_end ||
                cursor != row->cursor_column ||
                (cursor >= 0 && frame->cursor_style != row->cursor_style);
            if (session->search.open || cache->search_open) {
                for (i = 0; i < app->columns; i++) {
                    uint8_t match = (uint8_t)search_cell(&session->search,
                        search_offset + (uint64_t)y, (unsigned)i);
                    if (search[i] != match) {
                        search[i] = match;
                        dirty = true;
                    }
                }
            }
            if (!dirty)
                continue;
            if (!row->texture) {
                row->texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888,
                    SDL_TEXTUREACCESS_TARGET, cache->width, cache->cell_height * 2);
                if (!row->texture || !SDL_SetTextureBlendMode(row->texture,
                        SDL_BLENDMODE_BLEND_PREMULTIPLIED))
                    return false;
            }
            if (!SDL_SetRenderTarget(app->renderer, row->texture))
                return false;
            if (!SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 0) ||
                !SDL_RenderClear(app->renderer) ||
                !SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND))
                return false;
            frame->row_offset = app->grid_top + y * app->glyphs.cell_height;
        }
        if (ghostty_render_state_row_get(term->row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &term->cell_iterator) != GHOSTTY_SUCCESS)
            return false;
        paint_row(frame, session, y, selection_start, selection_end,
                  cursor_drawn, cursor_column, cursor_row, search_offset);
        if (row) {
            row->id = id;
            row->selection_start = selection_start;
            row->selection_end = selection_end;
            row->hover_start = hover_start;
            row->hover_end = hover_end;
            row->cursor_column = cursor;
            row->cursor_style = frame->cursor_style;
            row->valid = true;
        }
    }
    frame->row_offset = 0;
    if (!cache)
        return true;
    if (!SDL_SetRenderTarget(app->renderer, NULL))
        return false;
    for (i = 0; i < cache->rows; i++) {
        SDL_FRect destination = { 0.0f,
            (float)(app->grid_top + i * app->glyphs.cell_height),
            (float)cache->width, (float)(cache->cell_height * 2) };
        if (!cache->row[i].valid || !cache->row[i].texture ||
            !SDL_RenderTexture(app->renderer, cache->row[i].texture, NULL, &destination))
            return false;
    }
    cache->valid = true;
    if (full) {
        cache->session = session;
        cache->colors = frame->colors;
        cache->scheme = app->settings.colors;
        cache->opacity = app->panel_opacity;
    }
    cache->search_open = session->search.open;
    return true;
}

void render_frame(App *app)
{
    SDL_Renderer *renderer = app->renderer;
    Session *session = app_active_session(app);
    Term *term = session ? &session->term : NULL;
    GhosttyRenderStateColors state_colors =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyRenderStateCursor cursor_state =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    GhosttyTerminalScrollbar search_bar = {0};
    Hyperlink hovered_link;
    bool have_hovered_link = false;
    FrameContext frame;
    struct RenderCache *cache;
    int pixel_width;
    int pixel_height;
    int cursor_column = 0;
    int cursor_row = 0;
    bool cursor_drawn = false;
    bool cursor_valid = false;
    bool grid_painted;

    if (!term)
        return;

    if (session->search.open) {
        search_refresh(&session->search, term);
        term_scrollbar(term, &search_bar);
    }
    if (app->link_pointer_valid && app->glyphs.cell_width > 0 &&
        app->glyphs.cell_height > 0 &&
        app->link_pointer_x >= app->grid_left &&
        app->link_pointer_y >= app->grid_top &&
        chrome_search_hit(app, app->link_pointer_x, app->link_pointer_y) ==
            CHROME_SEARCH_NONE) {
        int x = (int)((app->link_pointer_x - app->grid_left) /
                      app->glyphs.cell_width);
        int y = (int)((app->link_pointer_y - app->grid_top) /
                      app->glyphs.cell_height);
        if (x < app->columns && y < app->rows)
            have_hovered_link = hyperlink_at(term, x, y, &hovered_link);
    }

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);

    /* 1. Clear to fully transparent; everything below blends over this. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    frame.hovered_link = have_hovered_link ? &hovered_link : NULL;
    frame.row_offset = 0;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    /* 2. The panel: our rounded fill, or a tint over a DWM backdrop. */
    panel_draw(&app->panel, renderer, pixel_width, pixel_height,
               chrome_corner_radius(app), chrome_panel_fill_color(app),
               CHROME_PANEL_BORDER);
    chrome_draw_title_bar(app, pixel_width, pixel_height);

    /* 3. Capture the frame after search and hover have read the grid. */
    if (ghostty_render_state_update(term->render_state, term->handle) != GHOSTTY_SUCCESS) {
        render_invalidate(app);
        return;
    }

    /* 4. Defaults and cursor, before walking the rows. */
    ghostty_render_state_get(term->render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLORS, &state_colors);
    frame.app = app;
    frame.colors = state_colors;
    frame.default_foreground = style_color(state_colors.foreground, 1.0f);
    frame.default_background.r = app->settings.colors.background.r;
    frame.default_background.g = app->settings.colors.background.g;
    frame.default_background.b = app->settings.colors.background.b;
    frame.default_background.a = 255;
    frame.cursor_color = state_colors.cursor_has_value
                         ? style_color(state_colors.cursor, 1.0f)
                         : frame.default_foreground;
    frame.cursor_style = GHOSTTY_CURSOR_STYLE_BLOCK;

    if (ghostty_render_state_get(term->render_state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR,
                                 &cursor_state) == GHOSTTY_SUCCESS) {
        frame.cursor_style = cursor_state.visual_style;
        app->cursor_blinks = cursor_state.blinking;
        if (cursor_state.visible && cursor_state.viewport_has_value) {
            cursor_column = cursor_state.viewport_x;
            cursor_row = cursor_state.viewport_y;
            cursor_drawn = true;
            cursor_valid = true;
        }
    }
    /* A blinking cursor is only drawn half the time. */
    if (app->cursor_blinks && !app->cursor_lit)
        cursor_drawn = false;

    /* Rebuild only damaged row layers, then composite them over the freshly
     * drawn panel. On failure, discard partial layers and repaint directly. */
    cache = cache_prepare(app, pixel_width, pixel_height);
    grid_painted = paint_grid(&frame, session, cache, cursor_drawn,
                              cursor_column, cursor_row, search_bar.offset);
    if (!grid_painted && cache) {
        if (!SDL_SetRenderTarget(renderer, NULL)) {
            render_invalidate(app);
            return;
        }
        cache_release_rows(cache);
        cache->unavailable = true;
        frame.row_offset = 0;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        panel_draw(&app->panel, renderer, pixel_width, pixel_height,
                   chrome_corner_radius(app), chrome_panel_fill_color(app),
                   CHROME_PANEL_BORDER);
        chrome_draw_title_bar(app, pixel_width, pixel_height);
        grid_painted = paint_grid(&frame, session, NULL, cursor_drawn,
                                  cursor_column, cursor_row, search_bar.offset);
    }

    /* 6. Uncommitted IME text goes on top of the cell the terminal cursor
     * is in: the screen buffer does not have it yet, so this is the only
     * place a user can see what they are mid-way through typing. It is
     * gated on the cursor being on screen, not on the blink phase - the
     * preedit must not strobe with the cursor. */
    if (cursor_valid && !session->search.open) {
        draw_composition(app, cursor_column, cursor_row);
        update_text_input_area(app);
    }

    /* 8. The scrollback indicator, over the right edge of the grid. */
    chrome_draw_scrollbar(app);

    /* 9. Status bar, in the band below the grid. */
    chrome_draw_statusbar(app, pixel_width, pixel_height,
                          frame.default_foreground);

    /* 10. Visual bell flash overlay, when active. */
    if (app->bell_flash_until_ms > 0 &&
        SDL_GetTicks() < app->bell_flash_until_ms) {
        SDL_Color no_border = { 0, 0, 0, 0 };
        bool light_bg = ((int)app->settings.colors.background.r +
                         (int)app->settings.colors.background.g +
                         (int)app->settings.colors.background.b) > 384;
        SDL_Color flash = light_bg ? (SDL_Color){ 0, 0, 0, 32 }
                                   : (SDL_Color){ 255, 255, 255, 40 };

        panel_draw(&app->panel, renderer, pixel_width, pixel_height,
                   chrome_corner_radius(app), flash, no_border);
    }
    chrome_draw_search(app);
    toast_render(app);

    /* Only mark damage consumed if the complete frame is presented. */
    if (SDL_RenderPresent(renderer) && grid_painted)
        ghostty_render_state_clean(term->render_state);
    else
        render_invalidate(app);
    if (search_pending(&session->search)) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_USER;
        event.user.code = APP_EVENT_SEARCH;
        SDL_PushEvent(&event);
    }
}
