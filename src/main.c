/*
 * termica - milestone 0 for the SDL3 terminal experiment.
 *
 * Goal: prove the "look" is reachable before writing any terminal logic.
 *   1. borderless window with custom drag / resize (SDL_SetWindowHitTest)
 *   2. per-pixel transparency: translucent background, fully opaque text
 *   3. rounded corners (Windows 11 DWM)
 *   4. text drawn on a fixed cell grid, one glyph per cell, from a glyph
 *      cache (the same technique the real terminal renderer will use)
 *   5. Nerd Font icons (private-use-area codepoints)
 *
 * Usage:  termica [path\to\NerdFont.ttf] [point-size]
 * Keys:   Up/Down = background opacity, B = toggle acrylic (experimental),
 *         Esc = quit
 *
 * Strict C99. Windows-only bits are behind #ifdef _WIN32.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

/* ------------------------------------------------------------------------- */
/* Glyph cache: one white texture per codepoint, tinted at draw time.         */
/* BMP only (enough for ASCII, box drawing and Nerd Font PUA icons).          */
/* ------------------------------------------------------------------------- */

typedef struct Glyph {
    SDL_Texture *tex;
    float w, h;
    bool tried;
} Glyph;

static Glyph g_glyphs[0x10000];

typedef struct App {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    TTF_Font     *font;
    float         scale;       /* display scale (DPI)                         */
    int           cell_w;      /* pixels                                      */
    int           cell_h;      /* pixels                                      */
    float         bg_alpha;    /* 0..1                                        */
    bool          acrylic;
    bool          icons_ok;    /* does the font contain Nerd Font glyphs?     */
} App;

static Glyph *glyph_get(App *app, Uint32 cp)
{
    Glyph *g;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf;
    char buf[8];
    char *end;

    if (cp >= 0x10000)
        return NULL;
    g = &g_glyphs[cp];
    if (g->tried)
        return g->tex ? g : NULL;
    g->tried = true;

    if (!TTF_FontHasGlyph(app->font, cp))
        return NULL;

    end = SDL_UCS4ToUTF8(cp, buf);
    *end = '\0';

    /* TTF_RenderText_* returns a surface of exactly the font's line height with
     * the baseline at a fixed spot, so every glyph lines up when drawn at the
     * same y. That is what a monospace grid needs. */
    surf = TTF_RenderText_Blended(app->font, buf, 0, white);
    if (!surf)
        return NULL;
    g->tex = SDL_CreateTextureFromSurface(app->renderer, surf);
    g->w = (float)surf->w;
    g->h = (float)surf->h;
    SDL_DestroySurface(surf);
    if (g->tex)
        SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
    return g->tex ? g : NULL;
}

/* Draw a UTF-8 string, one codepoint per cell, starting at (col,row).
 * Returns the number of cells consumed. */
static int draw_cells(App *app, int col, int row, const char *s, SDL_Color c)
{
    const char *p = s;
    int n = 0;
    int pad = (int)(12.0f * app->scale);

    while (*p) {
        Uint32 cp = SDL_StepUTF8(&p, NULL);
        Glyph *g;
        if (cp == 0)
            break;
        g = glyph_get(app, cp);
        if (g && cp != ' ') {
            SDL_FRect dst;
            dst.x = (float)(pad + (col + n) * app->cell_w);
            dst.y = (float)(pad + row * app->cell_h);
            dst.w = g->w;
            dst.h = g->h;
            SDL_SetTextureColorMod(g->tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(g->tex, c.a);
            SDL_RenderTexture(app->renderer, g->tex, NULL, &dst);
        }
        n++;
    }
    return n;
}

static void fill_cell(App *app, int col, int row, SDL_Color c)
{
    int pad = (int)(12.0f * app->scale);
    SDL_FRect r;
    r.x = (float)(pad + col * app->cell_w);
    r.y = (float)(pad + row * app->cell_h);
    r.w = (float)app->cell_w;
    r.h = (float)app->cell_h;
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(app->renderer, &r);
}

/* ------------------------------------------------------------------------- */
/* Borderless window: dragging and resizing                                   */
/* ------------------------------------------------------------------------- */

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *win, const SDL_Point *p,
                                          void *data)
{
    const int border = 8;   /* resize margin, in window coordinates */
    const int drag_h = 30;  /* top strip that drags the window      */
    int w, h;
    bool left, right, top, bottom;

    (void)data;
    SDL_GetWindowSize(win, &w, &h);
    left   = p->x < border;
    right  = p->x >= w - border;
    top    = p->y < border;
    bottom = p->y >= h - border;

    if (top && left)     return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)    return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left)            return SDL_HITTEST_RESIZE_LEFT;
    if (right)           return SDL_HITTEST_RESIZE_RIGHT;
    if (top)             return SDL_HITTEST_RESIZE_TOP;
    if (bottom)          return SDL_HITTEST_RESIZE_BOTTOM;
    if (p->y < drag_h)   return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

/* ------------------------------------------------------------------------- */
/* Windows 11 window chrome via DWM                                           */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
/* Defined here so older MinGW headers still compile. */
enum {
    TT_DWMWA_USE_IMMERSIVE_DARK_MODE = 20,
    TT_DWMWA_WINDOW_CORNER_PREFERENCE = 33,
    TT_DWMWA_SYSTEMBACKDROP_TYPE = 38
};
enum { TT_DWMWCP_ROUND = 2 };
enum { TT_DWMSBT_NONE = 1, TT_DWMSBT_TRANSIENTWINDOW = 3 /* acrylic */ };

static HWND window_hwnd(SDL_Window *w)
{
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

static void win_apply_chrome(SDL_Window *w)
{
    HWND hwnd = window_hwnd(w);
    int corner = TT_DWMWCP_ROUND;
    BOOL dark = TRUE;
    if (!hwnd)
        return;
    DwmSetWindowAttribute(hwnd, TT_DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof corner);
    DwmSetWindowAttribute(hwnd, TT_DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof dark);
}

static void win_set_acrylic(SDL_Window *w, bool on)
{
    HWND hwnd = window_hwnd(w);
    int type = on ? TT_DWMSBT_TRANSIENTWINDOW : TT_DWMSBT_NONE;
    if (!hwnd)
        return;
    DwmSetWindowAttribute(hwnd, TT_DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof type);
}
#else
static void win_apply_chrome(SDL_Window *w) { (void)w; }
static void win_set_acrylic(SDL_Window *w, bool on) { (void)w; (void)on; }
#endif

/* ------------------------------------------------------------------------- */
/* Scene                                                                      */
/* ------------------------------------------------------------------------- */

/* Nerd Font codepoints, as UTF-8 byte strings. Written as separate literals so
 * a hex escape never swallows a following hex digit. */
#define ICON_FOLDER_OPEN "\xEF\x81\xBC"  /* U+F07C */
#define ICON_FOLDER      "\xEF\x81\xBB"  /* U+F07B */
#define ICON_HOME        "\xEF\x80\x95"  /* U+F015 */
#define ICON_CLOCK       "\xEF\x80\x97"  /* U+F017 */
#define ICON_FILE        "\xEF\x80\x96"  /* U+F016 */
#define PROMPT_CHEVRON   "\xE2\x9D\xAF"  /* U+276F */

static void draw_scene(App *app)
{
    SDL_Renderer *r = app->renderer;
    int pw, ph, cols, row = 0, i;
    SDL_FRect border;
    SDL_Color teal   = {  38, 190, 170, 255 };
    SDL_Color cyan   = {  80, 200, 240, 255 };
    SDL_Color white  = { 225, 228, 232, 255 };
    SDL_Color grey   = { 130, 138, 148, 255 };
    SDL_Color orange = { 255, 160,  60, 255 };
    static const char *dirs[] = { "Documents", "Downloads", "Links", "Music" };
    char status[160];

    SDL_GetWindowSizeInPixels(app->window, &pw, &ph);
    cols = (pw - (int)(24.0f * app->scale)) / app->cell_w;

    /* 1. Clear to fully transparent. Everything below blends over this. */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);

    /* 2. Translucent panel (this is the "window_background_opacity" part). */
    SDL_SetRenderDrawColor(r, 14, 18, 22, (Uint8)(app->bg_alpha * 255.0f));
    SDL_RenderFillRect(r, NULL);

    /* 3. Thin border. */
    border.x = 0.5f; border.y = 0.5f;
    border.w = (float)pw - 1.0f; border.h = (float)ph - 1.0f;
    SDL_SetRenderDrawColor(r, 90, 100, 112, 170);
    SDL_RenderRect(r, &border);

    /* 4. Prompt line, like the screenshot. */
    draw_cells(app, 0, row, ICON_FOLDER_OPEN, white);
    draw_cells(app, 2, row, ICON_HOME, white);
    draw_cells(app, 3, row, " ~", cyan);
    if (cols > 10) {
        int cc = cols - 7;
        draw_cells(app, cc, row, ICON_CLOCK, grey);
        draw_cells(app, cc + 1, row, " 13:28", grey);
    }
    row++;
    draw_cells(app, 0, row, PROMPT_CHEVRON, teal);
    draw_cells(app, 2, row, "lsd", white);
    row++;

    for (i = 0; i < 4; i++) {
        draw_cells(app, 0, row, ICON_FOLDER, teal);
        draw_cells(app, 2, row, dirs[i], cyan);
        row++;
    }
    draw_cells(app, 0, row, ICON_FILE, white);
    draw_cells(app, 2, row, "notes.md", white);
    row += 2;

    /* 5. Second prompt with a block cursor. */
    draw_cells(app, 0, row, ICON_FOLDER_OPEN, white);
    draw_cells(app, 2, row, ICON_HOME, white);
    draw_cells(app, 3, row, " ~", cyan);
    row++;
    draw_cells(app, 0, row, PROMPT_CHEVRON, teal);
    fill_cell(app, 2, row, teal);
    row += 2;

    /* 6. Status line. */
    if (!app->icons_ok) {
        draw_cells(app, 0, row++,
                   "Nerd Font glyphs not found in this font: icons are missing.",
                   orange);
        draw_cells(app, 0, row++,
                   "Run: termica C:\\path\\to\\SomeNerdFontMono.ttf", orange);
    }
    SDL_snprintf(status, sizeof status,
                 "opacity %.2f | renderer %s | Up/Down opacity, B acrylic, Esc quit",
                 (double)app->bg_alpha, SDL_GetRendererName(r));
    draw_cells(app, 0, row, status, grey);

    SDL_RenderPresent(r);
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

static const char *pick_font(int argc, char **argv)
{
    static const char *fallbacks[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        NULL
    };
    int i;
    if (argc > 1)
        return argv[1];
    for (i = 0; fallbacks[i]; i++) {
        SDL_IOStream *io = SDL_IOFromFile(fallbacks[i], "rb");
        if (io) {
            SDL_CloseIO(io);
            return fallbacks[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    App app;
    const char *font_path;
    float ptsize = 16.0f;
    bool running = true;
    int cw = 0, ch = 0;
    int i;

    memset(&app, 0, sizeof app);
    app.bg_alpha = 0.80f;

    font_path = pick_font(argc, argv);
    if (!font_path) {
        fprintf(stderr, "usage: %s path\\to\\font.ttf [point-size]\n", argv[0]);
        return 1;
    }
    if (argc > 2)
        ptsize = (float)SDL_atof(argv[2]);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        return 1;
    }

    app.window = SDL_CreateWindow("termica", 900, 480,
                                  SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT |
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowHitTest(app.window, hit_test, NULL);
    win_apply_chrome(app.window);

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    printf("renderer: %s\n", SDL_GetRendererName(app.renderer));

    app.scale = SDL_GetWindowDisplayScale(app.window);
    if (app.scale <= 0.0f)
        app.scale = 1.0f;

    app.font = TTF_OpenFont(font_path, ptsize * app.scale);
    if (!app.font) {
        fprintf(stderr, "TTF_OpenFont(%s): %s\n", font_path, SDL_GetError());
        return 1;
    }
    TTF_SetFontHinting(app.font, TTF_HINTING_LIGHT);

    /* Cell size comes from the font: advance of 'M' by line height. */
    if (!TTF_GetStringSize(app.font, "M", 0, &cw, &ch) || cw <= 0)
        cw = 10;
    app.cell_w = cw;
    app.cell_h = TTF_GetFontHeight(app.font);
    app.icons_ok = TTF_FontHasGlyph(app.font, 0xF07B) &&
                   TTF_FontHasGlyph(app.font, 0xF015);
    printf("font: %s  cell: %dx%d px  scale: %.2f  nerd icons: %s\n",
           font_path, app.cell_w, app.cell_h, (double)app.scale,
           app.icons_ok ? "yes" : "NO");

    draw_scene(&app);

    while (running) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            break;
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) {
                    running = false;
                } else if (e.key.key == SDLK_UP) {
                    app.bg_alpha += 0.05f;
                    if (app.bg_alpha > 1.0f) app.bg_alpha = 1.0f;
                } else if (e.key.key == SDLK_DOWN) {
                    app.bg_alpha -= 0.05f;
                    if (app.bg_alpha < 0.0f) app.bg_alpha = 0.0f;
                } else if (e.key.key == SDLK_B) {
                    app.acrylic = !app.acrylic;
                    win_set_acrylic(app.window, app.acrylic);
                }
                break;
            default:
                break;
            }
        } while (running && SDL_PollEvent(&e));
        if (running)
            draw_scene(&app);
    }

    for (i = 0; i < 0x10000; i++)
        if (g_glyphs[i].tex)
            SDL_DestroyTexture(g_glyphs[i].tex);
    TTF_CloseFont(app.font);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
