/*
 * termica - frame rendering from libghostty's render state.
 * See render.h.
 */
#include "render.h"

#include "chrome.h"
#include "style.h"

#include <string.h>

/* Everything the cell drawing needs from the frame it belongs to. */
typedef struct FrameContext {
    App                      *app;
    GhosttyRenderStateColors  colors;
    SDL_Color                 default_foreground;
    SDL_Color                 cursor_color;
    GhosttyCursorStyle        cursor_style;
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
                     cell_x(app, cell->column), cell_y(app, cell->row),
                     style->bold, style->italic,
                     covered ? CHROME_PANEL_BG : style->foreground);
}

static void draw_cell_cursor(const FrameContext *frame, const CellPaint *cell,
                             const CellStyle *style)
{
    App *app = frame->app;
    float x = cell_x(app, cell->column);
    float y = cell_y(app, cell->row);
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
    float y = cell_y(app, cell->row);
    float width = (float)app->glyphs.cell_width;
    float height = (float)app->glyphs.cell_height;

    style_resolve(&style, &cell->style,
                  cell->has_foreground, cell->foreground,
                  cell->has_background, cell->background,
                  &frame->colors, frame->default_foreground, CHROME_PANEL_BG,
                  app->panel_opacity, cell->selected);

    if (style.has_background)
        chrome_fill_rect(app->renderer, x, y, width, height, style.background);

    draw_cell_glyph(frame, cell, &style, false);

    if (style.underline)
        draw_underline(app, x, y + height - 2.0f, width, style.decoration,
                       style.underline_style);
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
    GhosttyRenderStateCursor cursor_state =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    SDL_Rect area;

    if (ghostty_render_state_get(app->session.term.render_state,
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
    const char *text = app->session.composing;
    int column = cursor_column;

    if (app->session.composing_length == 0)
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

/* The translucent window forces a full redraw every frame, so per-row dirty
 * flags do not buy anything here: every row of the viewport is visited. */
void render_frame(App *app)
{
    SDL_Renderer *renderer = app->renderer;
    Term *term = &app->session.term;
    GhosttyRenderStateColors state_colors =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyRenderStateCursor cursor_state =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    FrameContext frame;
    int pixel_width;
    int pixel_height;
    int cursor_column = 0;
    int cursor_row = 0;
    bool cursor_drawn = false;
    bool cursor_valid = false;

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);

    /* 1. Clear to fully transparent; everything below blends over this. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    /* 2. The panel: our rounded fill, or a tint over a DWM backdrop. */
    panel_draw(&app->panel, renderer, pixel_width, pixel_height,
               chrome_corner_radius(app), chrome_panel_fill_color(app),
               CHROME_PANEL_BORDER);
    chrome_draw_title_bar(app, pixel_width, pixel_height);

    /* 3. Capture the frame. The only step that reads the terminal. */
    ghostty_render_state_update(term->render_state, term->handle);

    /* 4. Defaults and cursor, before walking the rows. */
    ghostty_render_state_get(term->render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLORS, &state_colors);
    frame.app = app;
    frame.colors = state_colors;
    frame.default_foreground = style_color(state_colors.foreground, 1.0f);
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

    /* 5. Rows and cells. The data is valid until the next update. */
    ghostty_render_state_get(term->render_state,
                             GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                             &term->row_iterator);
    while (ghostty_render_state_row_iterator_next(term->row_iterator)) {
        int32_t row_position = 0;
        int column = 0;
        int selection_start = -1;
        int selection_end = -1;
        GhosttyRenderStateRowSelection row_selection =
            GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);

        ghostty_render_state_row_get(term->row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                                     &row_position);
        ghostty_render_state_row_get(term->row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &term->cell_iterator);

        /* The row-local selected range, so highlighting is a comparison
         * per cell instead of one library call per cell. GHOSTTY_NO_VALUE
         * when this row is not touched by the selection. */
        if (ghostty_render_state_row_get(
                term->row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                &row_selection) == GHOSTTY_SUCCESS) {
            selection_start = row_selection.start_x;
            selection_end = row_selection.end_x;
        }

        while (ghostty_render_state_row_cells_next(term->cell_iterator)) {
            CellPaint cell;
            GhosttyCell raw_cell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            uint8_t text_bytes[128];
            GhosttyBuffer grapheme = { .ptr = text_bytes,
                                       .cap = sizeof text_bytes };

            ghostty_render_state_row_cells_get(
                term->cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw_cell);
            ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &wide);

            /* The tail of a wide character has no text of its own. */
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                column++;
                continue;
            }

            memset(&cell, 0, sizeof cell);
            cell.column = column;
            cell.row = row_position;
            ghostty_render_state_row_cells_get(
                term->cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &cell.style);
            cell.has_foreground = ghostty_render_state_row_cells_get(
                term->cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                &cell.foreground) == GHOSTTY_SUCCESS;
            cell.has_background = ghostty_render_state_row_cells_get(
                term->cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                &cell.background) == GHOSTTY_SUCCESS;
            ghostty_render_state_row_cells_get(
                term->cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &grapheme);
            cell.text = grapheme.len <= grapheme.cap ? text_bytes : NULL;
            cell.text_length = cell.text ? grapheme.len : 0;
            cell.at_cursor = cursor_drawn && column == cursor_column &&
                             row_position == cursor_row;
            cell.selected = selection_start >= 0 && column >= selection_start &&
                            column <= selection_end;

            draw_cell(&frame, &cell);
            column++;
        }
    }

    /* 6. Uncommitted IME text goes on top of the cell the terminal cursor
     * is in: the screen buffer does not have it yet, so this is the only
     * place a user can see what they are mid-way through typing. It is
     * gated on the cursor being on screen, not on the blink phase - the
     * preedit must not strobe with the cursor. */
    if (cursor_valid) {
        draw_composition(app, cursor_column, cursor_row);
        update_text_input_area(app);
    }

    /* 7. The frame is drawn, so the next one only reports new changes. */
    ghostty_render_state_clean(term->render_state);

    /* 8. The scrollback indicator, over the right edge of the grid. */
    chrome_draw_scrollbar(app);

    /* 9. Status bar, in the band below the grid. */
    chrome_draw_statusbar(app, pixel_width, pixel_height,
                          frame.default_foreground);

    SDL_RenderPresent(renderer);
}
