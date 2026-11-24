/*
 * termica - window chrome: layout geometry, title bar, scrollbar and status bar.
 * See chrome.h.
 */
#include "chrome.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

const SDL_Color CHROME_PANEL_BG     = {  14,  18,  22, 255 };
const SDL_Color CHROME_PANEL_BORDER = {  90, 100, 112, PANEL_BORDER_ALPHA };
const SDL_Color CHROME_STATUS_TEXT  = { 130, 138, 148, 255 };
const SDL_Color CHROME_TITLE_TEXT   = { 205, 212, 220, 255 };

/* The three containers the status bar lays out into. */
enum {
    STATUSBAR_LEFT = 0,
    STATUSBAR_CENTER,
    STATUSBAR_RIGHT
};

void chrome_fill_rect(SDL_Renderer *renderer, float x, float y, float width,
                      float height, SDL_Color color)
{
    SDL_FRect rectangle;

    if (width <= 0.0f || height <= 0.0f)
        return;
    rectangle.x = x;
    rectangle.y = y;
    rectangle.w = width;
    rectangle.h = height;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rectangle);
}

/* The fill color of the panel: its own background in panel mode, a tint
 * over a DWM backdrop. */
SDL_Color chrome_panel_fill_color(const App *app)
{
    SDL_Color color;
    float opacity = app->panel_opacity;

    color.r = app->settings.colors.background.r;
    color.g = app->settings.colors.background.g;
    color.b = app->settings.colors.background.b;
    if (app->backdrop != WINDOW_BACKDROP_PANEL)
        opacity *= BACKDROP_TINT;
    color.a = (Uint8)(opacity * 255.0f + 0.5f);
    return color;
}

/* The frame presets: how much room the chrome takes and whether the panel
 * has corners. Native means Windows already drew a frame around us. */
bool chrome_custom_frame(const App *app)
{
    return app->decoration != WINDOW_DECORATION_NATIVE;
}

int chrome_corner_radius(const App *app)
{
    if (app->decoration == WINDOW_DECORATION_SQUARE ||
        app->decoration == WINDOW_DECORATION_NATIVE)
        return 0;
    return app->panel.radius;
}

int chrome_padding(const App *app)
{
    float padding = app->decoration == WINDOW_DECORATION_COMPACT ? 8.0f : 12.0f;

    return (int)(padding * app->display_scale);
}

int chrome_title_bar_height(const App *app)
{
    if (!app->title_bar || !chrome_custom_frame(app))
        return 0;
    return (int)(WINDOW_TITLE_BAR_HEIGHT * app->display_scale + 0.5f);
}

bool chrome_tab_strip_visible(const App *app)
{
    return app && app->session_count > 1;
}

int chrome_top(const App *app)
{
    /* A native frame has no drag strip, only padding - unless multiple tabs
     * are open, in which case the tab strip sits at the top of the client. */
    if (!chrome_custom_frame(app)) {
        if (chrome_tab_strip_visible(app))
            return (int)(WINDOW_TAB_STRIP_HEIGHT * app->display_scale + 0.5f) +
                   chrome_padding(app) / 2;
        return chrome_padding(app);
    }
    if (app->title_bar)
        return chrome_title_bar_height(app);
    if (chrome_tab_strip_visible(app))
        return (int)(WINDOW_TAB_STRIP_HEIGHT * app->display_scale + 0.5f);
    if (app->decoration == WINDOW_DECORATION_COMPACT)
        return (int)(WINDOW_COMPACT_DRAG_STRIP * app->display_scale);
    return (int)((WINDOW_DRAG_STRIP + 4.0f) * app->display_scale);
}

/* The bottom band the status bar occupies, from its own font metrics plus
 * a little breathing room above and below the text. */
int chrome_statusbar_height(const App *app)
{
    int cell = app->statusbar_glyphs.cell_height;

    if (cell < 1)
        cell = app->glyphs.cell_height;
    return cell + 2 * (int)(STATUSBAR_VPAD * app->display_scale);
}

/* ------------------------------------------------------------------------- */
/* Scrollbar                                                                  */
/* ------------------------------------------------------------------------- */

void chrome_scrollbar_geometry(App *app, ScrollBar *bar)
{
    Session *s = app_active_session(app);
    GhosttyTerminalScrollbar data;
    int pixel_width;
    int pixel_height;

    memset(bar, 0, sizeof *bar);
    if (!s || !term_scrollbar(&s->term, &data) || data.total <= data.len)
        return;
    if (term_viewport_at_bottom(&s->term))
        return;

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    (void)pixel_height;
    bar->track_height = (float)app->rows * (float)app->glyphs.cell_height;
    if (bar->track_height <= 0.0f)
        return;

    bar->width = 4.0f * app->display_scale;
    if (bar->width < 2.0f)
        bar->width = 2.0f;
    /* SDL claims the last 8 pixels of the window for the resize border: a
     * click there moves the window instead of reaching the app, so the bar
     * is right-aligned just inside it. */
    bar->x = (float)pixel_width - 9.0f - bar->width;
    bar->track_top = (float)app->grid_top;
    bar->first_row = data.offset;
    bar->page_rows = data.len;
    bar->scrollable = data.total - data.len;

    bar->thumb_height = bar->track_height *
                        ((float)data.len / (float)data.total);
    if (bar->thumb_height < 16.0f)
        bar->thumb_height = 16.0f;
    bar->thumb_top = (bar->track_height - bar->thumb_height) *
                     ((float)data.offset / (float)bar->scrollable);
    bar->visible = true;
}

/* The first visible row for a pointer y while the thumb is being dragged.
 * The pointer controls the thumb's middle, so grabbing it anywhere keeps
 * the view where it was. */
uint64_t chrome_scrollbar_row_at(const ScrollBar *bar, float y)
{
    float span = bar->track_height - bar->thumb_height;
    float ratio;
    double row;

    if (span <= 0.0f)
        return 0;
    ratio = (y - bar->track_top - bar->thumb_height * 0.5f) / span;
    if (ratio < 0.0f)
        ratio = 0.0f;
    if (ratio > 1.0f)
        ratio = 1.0f;
    row = (double)ratio * (double)bar->scrollable;
    return (uint64_t)(row + 0.5);
}

bool chrome_scrollbar_hit(const ScrollBar *bar, float x, float y,
                          float window_width)
{
    float left = bar->x - 3.0f;
    float right = bar->x + bar->width + 3.0f;
    float limit = window_width - 9.0f;   /* the resize border starts here */

    if (!bar->visible)
        return false;
    if (right > limit)
        right = limit;
    if (x < left || x > right)
        return false;
    return y >= bar->track_top && y <= bar->track_top + bar->track_height;
}

/* A press on the bar: the thumb starts a drag, the track pages toward the
 * click. Returns true when the event belonged to the bar. */
bool chrome_scrollbar_press(App *app, const ScrollBar *bar, float y)
{
    Session *s = app_active_session(app);
    bool on_thumb = y >= bar->track_top + bar->thumb_top &&
                    y <= bar->track_top + bar->thumb_top + bar->thumb_height;
    uint64_t row;

    if (!s)
        return false;
    if (on_thumb) {
        s->bar_drag = true;
        return true;
    }
    if (y < bar->track_top + bar->thumb_top) {
        /* above the thumb: one page older */
        row = bar->first_row > bar->page_rows
                  ? bar->first_row - bar->page_rows
                  : 0;
    } else {
        row = bar->first_row + bar->page_rows;
        if (row > bar->scrollable)
            row = bar->scrollable;
    }
    term_scroll_row(&s->term, (uint32_t)row);
    app->needs_redraw = true;
    return true;
}

/* A thin bar on the right edge while the viewport is not at the bottom:
 * the thumb's size and position come straight from the terminal's own
 * scrollbar, so it says exactly which rows are on screen. Nothing is
 * drawn when the whole history fits. */
void chrome_draw_scrollbar(App *app)
{
    Session *s = app_active_session(app);
    ScrollBar bar;
    SDL_Color color = { 150, 158, 168, 190 };

    if (!s)
        return;
    chrome_scrollbar_geometry(app, &bar);
    if (!bar.visible)
        return;

    /* The thumb brightens while it is held, so a drag has feedback even
     * when the view itself is a moment behind. */
    if (s->bar_drag)
        color.a = 255;
    chrome_fill_rect(app->renderer, bar.x, bar.track_top + bar.thumb_top,
                     bar.width, bar.thumb_height, color);
}

/* ------------------------------------------------------------------------- */
/* Tab strip                                                                  */
/* ------------------------------------------------------------------------- */

#define TAB_MIN_WIDTH   80
#define TAB_MAX_WIDTH   180
#define TAB_NEW_WIDTH   28
#define TAB_CLOSE_WIDTH 20

typedef struct TabLayout {
    int start_x;
    int strip_height;
    int tab_width;
    int new_x;
    int right_edge;
} TabLayout;

static bool compute_tab_layout(const App *app, TabLayout *out)
{
    int win_w = 0;
    int win_h = 0;
    int right_reserved = 0;
    int available;
    int tab_w;

    memset(out, 0, sizeof *out);
    if (!chrome_tab_strip_visible(app))
        return false;

    SDL_GetWindowSize(app->window, &win_w, &win_h);
    (void)win_h;
    if (chrome_custom_frame(app) && app->title_bar) {
        out->strip_height = WINDOW_TITLE_BAR_HEIGHT;
        right_reserved = WINDOW_TITLE_BAR_BUTTONS *
                         WINDOW_TITLE_BAR_BUTTON_WIDTH + 16;
        out->start_x = 8;
    } else {
        out->strip_height = WINDOW_TAB_STRIP_HEIGHT;
        right_reserved = 8;
        out->start_x = chrome_custom_frame(app) ? 8 : 4;
    }

    available = win_w - out->start_x - right_reserved - TAB_NEW_WIDTH;
    if (available < TAB_MIN_WIDTH)
        available = TAB_MIN_WIDTH;
    tab_w = available / app->session_count;
    if (tab_w > TAB_MAX_WIDTH)
        tab_w = TAB_MAX_WIDTH;
    if (tab_w < TAB_MIN_WIDTH)
        tab_w = TAB_MIN_WIDTH;

    out->tab_width = tab_w;
    out->new_x = out->start_x + app->session_count * tab_w;
    out->right_edge = out->new_x + TAB_NEW_WIDTH;
    return true;
}

void chrome_sync_tab_hit_region(const App *app)
{
    TabLayout layout;

    if (chrome_custom_frame(app) && compute_tab_layout(app, &layout))
        window_set_tabs_width(layout.right_edge);
    else
        window_set_tabs_width(0);
}

void chrome_tab_label(const Session *s, int index, char *out, size_t capacity)
{
    const char *pwd;

    if (!out || capacity == 0)
        return;
    if (s && s->term.title[0]) {
        snprintf(out, capacity, "%s", s->term.title);
        return;
    }
    pwd = s ? term_pwd(&s->term) : NULL;
    if (pwd && pwd[0]) {
        size_t len = strlen(pwd);
        const char *slash;

        while (len > 1 && (pwd[len - 1] == '\\' || pwd[len - 1] == '/'))
            len--;
        slash = pwd + len;
        while (slash > pwd && slash[-1] != '\\' && slash[-1] != '/')
            slash--;
        if ((size_t)(pwd + len - slash) > 0) {
            size_t part = (size_t)(pwd + len - slash);

            if (part >= capacity)
                part = capacity - 1;
            memcpy(out, slash, part);
            out[part] = '\0';
            return;
        }
    }
    snprintf(out, capacity, "Tab %d", index + 1);
}

bool chrome_tab_hit(const App *app, float x, float y, int *out_tab,
                    bool *out_close, bool *out_new)
{
    TabLayout layout;
    int ix = (int)x;
    int iy = (int)y;

    if (out_tab) *out_tab = -1;
    if (out_close) *out_close = false;
    if (out_new) *out_new = false;
    if (!compute_tab_layout(app, &layout))
        return false;
    if (iy < 0 || iy >= layout.strip_height)
        return false;

    if (ix >= layout.new_x && ix < layout.new_x + TAB_NEW_WIDTH) {
        if (out_new)
            *out_new = true;
        return true;
    }
    if (ix >= layout.start_x && ix < layout.new_x) {
        int index = (ix - layout.start_x) / layout.tab_width;
        int local_x = (ix - layout.start_x) % layout.tab_width;

        if (index >= 0 && index < app->session_count) {
            if (out_tab)
                *out_tab = index;
            if (out_close && local_x >= layout.tab_width - TAB_CLOSE_WIDTH)
                *out_close = true;
            return true;
        }
    }
    return false;
}

void chrome_update_tab_hover(App *app, float x, float y)
{
    int tab = -1;
    bool is_new = false;

    chrome_tab_hit(app, x, y, &tab, NULL, &is_new);
    if (tab != app->tab_hover || is_new != app->tab_new_hover) {
        app->tab_hover = tab;
        app->tab_new_hover = is_new;
        app->needs_redraw = true;
    }
}

static void draw_tab_strip_items(App *app, const TabLayout *layout)
{
    SDL_Renderer *renderer = app->renderer;
    float s = app->display_scale;
    int strip_h = (int)(layout->strip_height * s + 0.5f);
    int tab_w = (int)(layout->tab_width * s + 0.5f);
    int start_x = (int)(layout->start_x * s + 0.5f);
    int new_w = (int)(TAB_NEW_WIDTH * s + 0.5f);
    int close_w = (int)(TAB_CLOSE_WIDTH * s + 0.5f);
    int pad_x = (int)(8.0f * s + 0.5f);
    int i;

    for (i = 0; i < app->session_count; i++) {
        int tx = start_x + i * tab_w;
        bool active = (i == app->active_session);
        bool hovered = (i == app->tab_hover);
        SDL_Color bg = { 255, 255, 255, active ? 32 : (hovered ? 18 : 6) };
        SDL_Color accent = { 136, 192, 208, 220 };
        SDL_Color fg = active ? CHROME_TITLE_TEXT : CHROME_STATUS_TEXT;
        char label[128];
        const char *p;
        SDL_Rect clip;
        int text_y = (strip_h - app->glyphs.cell_height) / 2;
        int col = 0;

        chrome_fill_rect(renderer, (float)tx + 1.0f, 2.0f, (float)tab_w - 2.0f,
                         (float)strip_h - 3.0f, bg);
        if (active)
            chrome_fill_rect(renderer, (float)tx + 1.0f, (float)strip_h - 3.0f,
                             (float)tab_w - 2.0f, 2.0f, accent);

        chrome_tab_label(app->sessions[i], i, label, sizeof label);
        if (text_y < 0)
            text_y = 0;
        clip.x = tx + pad_x;
        clip.y = 0;
        clip.w = tab_w - pad_x - close_w;
        clip.h = strip_h;
        if (clip.w > 0) {
            SDL_SetRenderClipRect(renderer, &clip);
            p = label;
            while (*p) {
                char utf8[8];
                char *end;
                Uint32 cp = SDL_StepUTF8(&p, NULL);

                if (!cp)
                    break;
                end = SDL_UCS4ToUTF8(cp, utf8);
                *end = '\0';
                glyphs_draw_text(&app->glyphs, utf8, (size_t)(end - utf8),
                                 (float)(clip.x + col * app->glyphs.cell_width),
                                 (float)text_y, false, false, fg);
                col++;
            }
            SDL_SetRenderClipRect(renderer, NULL);
        }

        /* Small 'x' close icon on the right side of each tab. */
        {
            float cx = (float)(tx + tab_w - close_w / 2);
            float cy = (float)strip_h * 0.5f;
            float r = 3.0f * s;

            SDL_SetRenderDrawColor(renderer, fg.r, fg.g, fg.b,
                                   active || hovered ? 200 : 110);
            SDL_RenderLine(renderer, cx - r, cy - r, cx + r, cy + r);
            SDL_RenderLine(renderer, cx - r, cy + r, cx + r, cy - r);
        }
    }

    /* '+' button to open a new tab. */
    {
        int nx = start_x + app->session_count * tab_w;
        float cx = (float)nx + (float)new_w * 0.5f;
        float cy = (float)strip_h * 0.5f;
        float r = 4.0f * s;

        if (app->tab_new_hover) {
            SDL_Color hover = { 255, 255, 255, 24 };
            chrome_fill_rect(renderer, (float)nx + 1.0f, 2.0f,
                             (float)new_w - 2.0f, (float)strip_h - 3.0f, hover);
        }
        SDL_SetRenderDrawColor(renderer, CHROME_TITLE_TEXT.r,
                               CHROME_TITLE_TEXT.g, CHROME_TITLE_TEXT.b, 200);
        SDL_RenderLine(renderer, cx - r, cy, cx + r, cy);
        SDL_RenderLine(renderer, cx, cy - r, cx, cy + r);
    }
}

/* ------------------------------------------------------------------------- */
/* Custom title bar                                                           */
/* ------------------------------------------------------------------------- */

/* Title bar buttons live in window coordinates, which is what SDL puts in
 * mouse events; the drawing uses pixels. */
bool chrome_title_button_at(const App *app, float x, float y, int *index)
{
    int width;
    int height;
    float left;

    if (!app->title_bar || !chrome_custom_frame(app))
        return false;
    SDL_GetWindowSize(app->window, &width, &height);
    (void)height;
    if (y < 0.0f || y >= (float)WINDOW_TITLE_BAR_HEIGHT)
        return false;
    left = (float)(width - WINDOW_TITLE_BAR_BUTTONS *
                               WINDOW_TITLE_BAR_BUTTON_WIDTH);
    if (x < left || x >= (float)width)
        return false;
    *index = (int)((x - left) / (float)WINDOW_TITLE_BAR_BUTTON_WIDTH);
    return *index >= 0 && *index < WINDOW_TITLE_BAR_BUTTONS;
}

void chrome_title_button_action(App *app, int index)
{
    switch (index) {
    case 0:
        SDL_MinimizeWindow(app->window);
        break;
    case 1:
        if ((SDL_GetWindowFlags(app->window) & SDL_WINDOW_MAXIMIZED) != 0)
            SDL_RestoreWindow(app->window);
        else
            SDL_MaximizeWindow(app->window);
        break;
    default:
        app->running = false;
        break;
    }
    app->needs_redraw = true;
}

void chrome_update_title_hover(App *app, float x, float y)
{
    int hover = -1;
    int index;

    if (chrome_title_button_at(app, x, y, &index))
        hover = index;
    if (hover != app->title_hover) {
        app->title_hover = hover;
        app->needs_redraw = true;
    }
}

static void draw_title_symbol(App *app, int index, const SDL_FRect *area,
                              bool maximized)
{
    float centre_x = area->x + area->w * 0.5f;
    float centre_y = area->y + area->h * 0.5f;
    float size = 5.0f * app->display_scale;
    float offset = 2.0f * app->display_scale;
    SDL_Renderer *renderer = app->renderer;

    SDL_SetRenderDrawColor(renderer, CHROME_TITLE_TEXT.r, CHROME_TITLE_TEXT.g,
                           CHROME_TITLE_TEXT.b, CHROME_TITLE_TEXT.a);
    if (index == 0) {
        SDL_RenderLine(renderer, centre_x - size, centre_y, centre_x + size,
                       centre_y);
    } else if (index == 1) {
        SDL_FRect box = { centre_x - size, centre_y - size, size * 2.0f,
                          size * 2.0f };

        if (maximized) {
            SDL_FRect back = { box.x + offset, box.y - offset, box.w, box.h };
            SDL_FRect front = { box.x, box.y + offset, box.w, box.h };

            SDL_RenderRect(renderer, &back);
            SDL_RenderRect(renderer, &front);
        } else {
            SDL_RenderRect(renderer, &box);
        }
    } else {
        SDL_RenderLine(renderer, centre_x - size, centre_y - size,
                       centre_x + size, centre_y + size);
        SDL_RenderLine(renderer, centre_x - size, centre_y + size,
                       centre_x + size, centre_y - size);
    }
}

void chrome_draw_title_bar(App *app, int pixel_width, int pixel_height)
{
    SDL_Renderer *renderer = app->renderer;
    int height = chrome_title_bar_height(app);
    int button_width = (int)(WINDOW_TITLE_BAR_BUTTON_WIDTH *
                             app->display_scale + 0.5f);
    int buttons_left = pixel_width - WINDOW_TITLE_BAR_BUTTONS * button_width;
    SDL_Color tint = { 255, 255, 255, 16 };
    SDL_Color separator = { 90, 100, 112, 90 };
    const char *title = APP_TITLE;
    bool maximized = (SDL_GetWindowFlags(app->window) &
                      SDL_WINDOW_MAXIMIZED) != 0;
    SDL_Rect clip;
    int text_x;
    int text_y;
    int column = 0;
    int index;
    TabLayout tab_layout;
    bool has_tabs = compute_tab_layout(app, &tab_layout);

    if (height <= 0) {
        /* No custom title bar (e.g. native frame or title_bar = false): draw
         * a standalone top tab strip only when more than one tab is open. */
        if (has_tabs) {
            int strip_h = (int)(tab_layout.strip_height * app->display_scale +
                                0.5f);
            chrome_fill_rect(renderer, 0.0f, 0.0f, (float)pixel_width,
                             (float)strip_h, tint);
            chrome_fill_rect(renderer, 0.0f, (float)strip_h - 1.0f,
                             (float)pixel_width, 1.0f, separator);
            draw_tab_strip_items(app, &tab_layout);
        }
        return;
    }

    /* The tint follows the panel's rounded top corners instead of painting
     * a square band over them. */
    {
        SDL_Color no_border = { 0, 0, 0, 0 };
        SDL_Rect band = { 0, 0, pixel_width, height };

        SDL_SetRenderClipRect(renderer, &band);
        panel_draw(&app->panel, renderer, pixel_width, pixel_height,
                   chrome_corner_radius(app), tint, no_border);
        SDL_SetRenderClipRect(renderer, NULL);
    }
    chrome_fill_rect(renderer, 0.0f, (float)height - 1.0f, (float)pixel_width,
                     1.0f, separator);

    for (index = 0; index < WINDOW_TITLE_BAR_BUTTONS; index++) {
        SDL_FRect area;
        SDL_Color hover;

        area.x = (float)(buttons_left + index * button_width);
        area.y = 0.0f;
        area.w = (float)button_width;
        area.h = (float)height;
        if (app->title_hover == index) {
            int radius = chrome_corner_radius(app);

            if (index == WINDOW_TITLE_BAR_BUTTONS - 1) {
                hover.r = 196;
                hover.g = 48;
                hover.b = 48;
                hover.a = 220;
            } else {
                hover.r = 255;
                hover.g = 255;
                hover.b = 255;
                hover.a = 26;
            }
            if (index == WINDOW_TITLE_BAR_BUTTONS - 1 && radius > 0) {
                /* Paint the close button through the panel's own shape, so
                 * its hover follows the top-right squircle instead of
                 * leaving a square notch beside the curve. */
                SDL_Color no_border = { 0, 0, 0, 0 };
                SDL_Rect button_clip;

                button_clip.x = (int)area.x;
                button_clip.y = 0;
                button_clip.w = (int)area.w;
                button_clip.h = height;
                SDL_SetRenderClipRect(renderer, &button_clip);
                panel_draw(&app->panel, renderer, pixel_width, pixel_height,
                           radius, hover, no_border);
                SDL_SetRenderClipRect(renderer, NULL);
            } else {
                chrome_fill_rect(renderer, area.x, area.y, area.w, area.h,
                                 hover);
            }
        }
        draw_title_symbol(app, index, &area, maximized);
    }

    if (has_tabs) {
        draw_tab_strip_items(app, &tab_layout);
        return;
    }

    /* The title is clipped to the space left of the buttons. */
    text_x = chrome_padding(app);
    text_y = (height - app->glyphs.cell_height) / 2;
    if (text_y < 0)
        text_y = 0;
    clip.x = text_x;
    clip.y = 0;
    clip.w = buttons_left - text_x - chrome_padding(app);
    clip.h = height;
    if (clip.w <= 0)
        return;

    SDL_SetRenderClipRect(renderer, &clip);
    while (*title) {
        char utf8[8];
        char *end;
        Uint32 codepoint = SDL_StepUTF8(&title, NULL);

        if (!codepoint)
            break;
        end = SDL_UCS4ToUTF8(codepoint, utf8);
        *end = '\0';
        glyphs_draw_text(&app->glyphs, utf8, (size_t)(end - utf8),
                         (float)(text_x + column * app->glyphs.cell_width),
                         (float)text_y, false, false, CHROME_TITLE_TEXT);
        column++;
    }
    SDL_SetRenderClipRect(renderer, NULL);
}

/* ------------------------------------------------------------------------- */
/* Status bar                                                                 */
/* ------------------------------------------------------------------------- */

/* How many UTF-8 codepoints a run holds. The status bar font is monospace,
 * so this is also how many cells wide it is. */
static int utf8_codepoint_count(const char *text, size_t length)
{
    const char *p = text;
    const char *end = text + length;
    int count = 0;

    while (p < end) {
        if (!SDL_StepUTF8(&p, NULL))
            break;
        count++;
    }
    return count;
}

/* Draw one styled run with the status bar font, advancing past it. */
static float statusbar_draw_text(App *app, const char *text, size_t length,
                                 float x, float y, bool bold, bool italic,
                                 SDL_Color color, float scale)
{
    const char *p = text;
    const char *end = text + length;

    while (p < end) {
        char utf8[8];
        char *term;
        Uint32 codepoint = SDL_StepUTF8(&p, NULL);

        if (!codepoint)
            break;
        term = SDL_UCS4ToUTF8(codepoint, utf8);
        *term = '\0';
        glyphs_draw_text_scaled(&app->statusbar_glyphs, utf8,
                                (size_t)(term - utf8), x, y, bold, italic,
                                color, scale);
        x += (float)app->statusbar_glyphs.cell_width;
    }
    return x;
}

/* Draw every styled run of a rendered line, left to right from `x`. */
static void statusbar_draw_runs(App *app, const char *line,
                                const StatusbarRun *runs, int run_count,
                                float x, float y, SDL_Color default_fg)
{
    int index;

    for (index = 0; index < run_count; index++) {
        const StatusbarRun *run = &runs[index];
        SDL_Color color = run->style.has_fg ? run->style.fg : default_fg;
        bool bold = run->style.bold;
        bool italic = run->style.italic;
        float scale = run->scale > 0.0f ? run->scale : 1.0f;
        int width = utf8_codepoint_count(line + run->start,
                                         (size_t)run->length) *
                    app->statusbar_glyphs.cell_width;

        if (run->style.dim) {
            color.r = (Uint8)(color.r * 0.5f);
            color.g = (Uint8)(color.g * 0.5f);
            color.b = (Uint8)(color.b * 0.5f);
        }
        if (run->style.has_bg)
            chrome_fill_rect(app->renderer, x, y, (float)width,
                             (float)app->statusbar_glyphs.cell_height,
                             run->style.bg);

        x = statusbar_draw_text(app, line + run->start, (size_t)run->length,
                                x, y, bold, italic, color, scale);
    }
}

/* Turn the app's current state into the module values the renderer needs.
 * The strings point into the caller's buffers, which must outlive the
 * render calls that follow. */
static void statusbar_fill_state(App *app, StatusbarState *state,
                                 char *dir_buf, size_t dir_cap,
                                 char *git_buf, size_t git_cap,
                                 char *dur_buf, size_t dur_cap,
                                 char *time_buf, size_t time_cap)
{
    const Statusbar *sb = &app->statusbar;
    const Session *s = app_active_session_const(app);
    const Term *term = s ? &s->term : NULL;
    const char *pwd = term ? term_pwd(term) : NULL;

    memset(state, 0, sizeof *state);
    if (!term)
        return;

    if (pwd) {
        statusbar_shorten_path(sb, pwd, dir_buf, dir_cap);
        state->directory.value = dir_buf;
    }

    if (app->git_have && app->git_status.in_repo && pwd) {
        state->git_branch.value = app->git_status.branch;
        statusbar_git_status_text(sb, app->git_status.ahead,
                                  app->git_status.behind,
                                  app->git_status.dirty,
                                  app->git_status.conflicted,
                                  app->git_status.clean,
                                  git_buf, git_cap);
        state->git_status.value = git_buf;
    }

    if (!term->command_running && term->last_command_known) {
        const StatusbarModule *m = &sb->modules[STATUSBAR_CMD_STATUS];
        bool ok = term->last_exit_code == 0;

        state->cmd_status.symbol = ok ? m->symbol_success : m->symbol_failure;
        state->cmd_status.style = ok ? &m->parsed_success : &m->parsed_failure;
    }

    if (!term->command_running && term->last_command_known &&
        term->last_duration_known && term->last_command_ms >=
            (uint64_t)sb->modules[STATUSBAR_CMD_DURATION].min_time_ms) {
        statusbar_format_duration(term->last_command_ms, dur_buf, dur_cap);
        state->cmd_duration.value = dur_buf;
    }

    if (statusbar_format_time(sb->modules[STATUSBAR_TIME].time_format,
                              time_buf, time_cap))
        state->time.value = time_buf;

    /* busy: a fixed braille frame while a command runs; at rest the `[busy]
     * idle` override if set, otherwise the fixed resting icon. Either way the
     * slot keeps its width, so the bar does not reflow. */
    {
        const StatusbarModule *busy = &sb->modules[STATUSBAR_BUSY];

        if (term->command_running && app->busy_frame_count > 0)
            state->busy.symbol = app->busy_frames[app->busy_frame_index];
        else if (busy->idle[0])
            state->busy.symbol = busy->idle;
        else {
            state->busy.symbol = statusbar_busy_idle_default();
            state->busy.scale = statusbar_busy_idle_scale();
        }
    }
}

/* Render and draw one container line. mode is left, centered, or
 * right-aligned within the bar. */
static void statusbar_draw_line(App *app, const StatusbarState *state,
                                const char *format, float y, int mode,
                                int pixel_width, SDL_Color default_fg)
{
    char line[STATUSBAR_LINE_BYTES];
    StatusbarRun runs[STATUSBAR_MAX_RUNS];
    int count = statusbar_render(&app->statusbar, state, format, line,
                                 sizeof line, runs, STATUSBAR_MAX_RUNS);
    int width = 0;
    int index;
    float x;

    if (count == 0)
        return;

    for (index = 0; index < count; index++) {
        width += utf8_codepoint_count(line + runs[index].start,
                                       (size_t)runs[index].length) *
                 app->statusbar_glyphs.cell_width;
    }

    switch (mode) {
    case STATUSBAR_CENTER:
        x = (float)pixel_width / 2.0f - (float)width / 2.0f;
        break;
    case STATUSBAR_RIGHT:
        x = (float)(pixel_width - chrome_padding(app)) - (float)width;
        break;
    default:
        x = (float)app->grid_left;
        break;
    }
    if (x < (float)chrome_padding(app))
        x = (float)chrome_padding(app);

    statusbar_draw_runs(app, line, runs, count, x, y, default_fg);
}

void chrome_draw_statusbar(App *app, int pixel_width, int pixel_height,
                           SDL_Color default_fg)
{
    int height = chrome_statusbar_height(app);
    int top = pixel_height - height;
    StatusbarState state;
    char dir_buf[512];
    char git_buf[256];
    char dur_buf[64];
    char time_buf[64];
    float y;

    /* The full-window panel draw already filled this band with the grid's
     * backdrop (chrome_panel_fill_color() and CHROME_PANEL_BORDER, including
     * the rounded bottom corners). Don't paint a separate opaque bar or
     * separator over it, so the status bar shares the exact same backdrop as
     * the grid. */

    y = (float)top +
        ((float)height - (float)app->statusbar_glyphs.cell_height) / 2.0f;
    if (y < (float)top)
        y = (float)top;

    statusbar_fill_state(app, &state, dir_buf, sizeof dir_buf, git_buf,
                         sizeof git_buf, dur_buf, sizeof dur_buf, time_buf,
                         sizeof time_buf);

    statusbar_draw_line(app, &state, app->statusbar.format, y,
                        STATUSBAR_LEFT, pixel_width, default_fg);
    if (app->statusbar.center_format[0])
        statusbar_draw_line(app, &state, app->statusbar.center_format, y,
                            STATUSBAR_CENTER, pixel_width, default_fg);
    if (app->statusbar.right_format[0])
        statusbar_draw_line(app, &state, app->statusbar.right_format, y,
                            STATUSBAR_RIGHT, pixel_width, default_fg);
}

/* Advance the spinner and refresh the clock when either actually changed. */
void chrome_statusbar_tick(App *app)
{
    const Session *s = app_active_session_const(app);
    bool redraw = false;
    time_t now;
    struct tm local;
    int second;

    if (s && s->term.command_running && app->busy_frame_count > 0) {
        app->busy_frame_index =
            (app->busy_frame_index + 1) % app->busy_frame_count;
        redraw = true;
    }

    now = time(NULL);
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) {
#else
    if (!localtime_r(&now, &local)) {
#endif
        second = -1;
    } else {
        second = local.tm_sec;
    }
    if (second != app->last_time_second) {
        app->last_time_second = second;
        redraw = true;
    }

    if (app->bell_flash_until_ms > 0 &&
        SDL_GetTicks() >= app->bell_flash_until_ms) {
        app->bell_flash_until_ms = 0;
        redraw = true;
    }

    if (redraw)
        app->needs_redraw = true;
}
