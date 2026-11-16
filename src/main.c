/*
 * termica - milestone 1: a real terminal grid.
 *
 * Milestone 0 proved the look (borderless, translucent, rounded, glyph
 * cache). This milestone puts a real terminal behind it:
 *
 *   1. libghostty-vt owns the screen state. vt_write() parses every
 *      escape sequence (SGR colors and attributes, cursor movement,
 *      erase, scroll, OSC, ...) for us. See
 *      https://lib.ghostty.org/vt/first-terminal.html
 *   2. the keyboard is encoded with the same library (arrow keys,
 *      modifiers, whatever the terminal modes ask for) and written back
 *      into the terminal. There is no pty yet, so the kernel's echo part
 *      of the line discipline is emulated for Enter and Backspace.
 *   3. every frame is drawn from a GhosttyRenderState: rows, cells,
 *      resolved colors, attributes, the cursor, on the fixed cell grid.
 *
 * The pieces live in their own files:
 *   term.c      the libghostty-vt wrapper (screen, key encoder)
 *   glyph.c     fonts, the glyph cache, drawing text
 *   style.c     terminal cell style -> colors and flags
 *   panel.c     the rounded translucent panel
 *   geometry.c  the squircle corner curve
 *   window.c    the SDL window and the Windows 11 chrome
 *
 * Usage:  termica [--selftest] [path\to\NerdFont.ttf] [point-size]
 * Keys:   type to write, arrows move the cursor, Enter newline,
 *         Backspace erase, Ctrl+V paste, Alt+Up/Down = panel opacity,
 *         Alt+M = cycle the window backdrop (panel, Mica, Mica Alt,
 *         Acrylic), Esc = quit
 *
 * Strict C99. Windows-only bits are behind #ifdef _WIN32.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "glyph.h"
#include "panel.h"
#include "style.h"
#include "term.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chevron prompt (U+276F); a plain '>' when the font does not carry it. */
#define PROMPT_CHEVRON "\xE2\x9D\xAF"

/* Over a DWM backdrop the panel is only a light tint: the material is the
 * background, the tint just keeps the text readable. */
#define BACKDROP_TINT 0.24f

/* The panel color doubles as the terminal's default background, so inverse
 * video and the cursor have something to work with. */
static const SDL_Color PANEL_BACKGROUND = { 14, 18, 22, 255 };
static const SDL_Color PANEL_BORDER = { 90, 100, 112, PANEL_BORDER_ALPHA };
static const SDL_Color STATUS_TEXT = { 130, 138, 148, 255 };

typedef struct App {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    Glyphs        glyphs;
    Panel         panel;
    Term          term;

    float          display_scale;
    float          panel_opacity;   /* 0..1                            */
    WindowBackdrop backdrop;

    int            grid_left;       /* pixel position of cell 0,0       */
    int            grid_top;
    int            columns;
    int            rows;

    bool           running;
    bool           needs_redraw;
    bool           cursor_lit;      /* blink phase                      */
    bool           cursor_blinks;   /* the terminal asked for a blink   */
    SDL_TimerID    blink_timer;
} App;

/* Everything the cell drawing needs from the frame it belongs to. */
typedef struct FrameContext {
    App                         *app;
    GhosttyRenderStateColors     colors;
    SDL_Color                    default_foreground;
    SDL_Color                    cursor_color;
    GhosttyCursorStyle           cursor_style;
} FrameContext;

typedef struct CellPaint {
    int            column;
    int            row;
    GhosttyStyle   style;
    bool           has_foreground;
    GhosttyColorRgb foreground;
    bool           has_background;
    GhosttyColorRgb background;
    const uint8_t *text;
    size_t         text_length;
    bool           at_cursor;
} CellPaint;

/* ------------------------------------------------------------------------- */
/* Small drawing helpers                                                      */
/* ------------------------------------------------------------------------- */

static void fill_rectangle(App *app, float x, float y, float width,
                           float height, SDL_Color color)
{
    SDL_FRect rectangle;

    if (width <= 0.0f || height <= 0.0f)
        return;
    rectangle.x = x;
    rectangle.y = y;
    rectangle.w = width;
    rectangle.h = height;
    SDL_SetRenderDrawColor(app->renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(app->renderer, &rectangle);
}

static float cell_x(const App *app, int column)
{
    return (float)(app->grid_left + column * app->glyphs.cell_width);
}

static float cell_y(const App *app, int row)
{
    return (float)(app->grid_top + row * app->glyphs.cell_height);
}

/* The fill color of the panel: its own background in panel mode, a tint
 * over a DWM backdrop. */
static SDL_Color panel_fill_color(const App *app)
{
    SDL_Color color = PANEL_BACKGROUND;
    float opacity = app->panel_opacity;

    if (app->backdrop != WINDOW_BACKDROP_PANEL)
        opacity *= BACKDROP_TINT;
    color.a = (Uint8)(opacity * 255.0f + 0.5f);
    return color;
}

/* ------------------------------------------------------------------------- */
/* Drawing one cell                                                           */
/* ------------------------------------------------------------------------- */

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
                     covered ? PANEL_BACKGROUND : style->foreground);
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
        fill_rectangle(app, x, y, 2.0f, height, frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_UNDERLINE:
        fill_rectangle(app, x, y + height - 2.0f, width, 2.0f,
                       frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW:
        fill_rectangle(app, x, y, width, 1.0f, frame->cursor_color);
        fill_rectangle(app, x, y + height - 1.0f, width, 1.0f,
                       frame->cursor_color);
        fill_rectangle(app, x, y, 1.0f, height, frame->cursor_color);
        fill_rectangle(app, x + width - 1.0f, y, 1.0f, height,
                       frame->cursor_color);
        break;
    case GHOSTTY_CURSOR_STYLE_BLOCK:
    default:
        fill_rectangle(app, x, y, width, height, frame->cursor_color);
        draw_cell_glyph(frame, cell, style, true);
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
                  &frame->colors, frame->default_foreground, PANEL_BACKGROUND,
                  app->panel_opacity);

    if (style.has_background)
        fill_rectangle(app, x, y, width, height, style.background);

    draw_cell_glyph(frame, cell, &style, false);

    if (style.underline)
        fill_rectangle(app, x, y + height - 2.0f, width, 2.0f,
                       style.decoration);
    if (style.strikethrough)
        fill_rectangle(app, x, y + height / 2.0f, width, 1.0f,
                       style.decoration);
    if (style.overline)
        fill_rectangle(app, x, y, width, 1.0f, style.decoration);

    if (cell->at_cursor)
        draw_cell_cursor(frame, cell, &style);
}

/* ------------------------------------------------------------------------- */
/* Keyboard -> terminal bytes                                                 */
/* ------------------------------------------------------------------------- */

/* SDL scancode (USB HID, layout-independent) -> Ghostty physical key
 * (W3C UI Events code). Both are layout-independent physical key codes,
 * so this is a straight 1:1 table. */
typedef struct {
    SDL_Scancode sdl_scancode;
    GhosttyKey   ghostty_key;
} KeyMapping;

static const KeyMapping KEY_MAP[] = {
    { SDL_SCANCODE_A,              GHOSTTY_KEY_A },              /* ... Z */
    { SDL_SCANCODE_B,              GHOSTTY_KEY_B },
    { SDL_SCANCODE_C,              GHOSTTY_KEY_C },
    { SDL_SCANCODE_D,              GHOSTTY_KEY_D },
    { SDL_SCANCODE_E,              GHOSTTY_KEY_E },
    { SDL_SCANCODE_F,              GHOSTTY_KEY_F },
    { SDL_SCANCODE_G,              GHOSTTY_KEY_G },
    { SDL_SCANCODE_H,              GHOSTTY_KEY_H },
    { SDL_SCANCODE_I,              GHOSTTY_KEY_I },
    { SDL_SCANCODE_J,              GHOSTTY_KEY_J },
    { SDL_SCANCODE_K,              GHOSTTY_KEY_K },
    { SDL_SCANCODE_L,              GHOSTTY_KEY_L },
    { SDL_SCANCODE_M,              GHOSTTY_KEY_M },
    { SDL_SCANCODE_N,              GHOSTTY_KEY_N },
    { SDL_SCANCODE_O,              GHOSTTY_KEY_O },
    { SDL_SCANCODE_P,              GHOSTTY_KEY_P },
    { SDL_SCANCODE_Q,              GHOSTTY_KEY_Q },
    { SDL_SCANCODE_R,              GHOSTTY_KEY_R },
    { SDL_SCANCODE_S,              GHOSTTY_KEY_S },
    { SDL_SCANCODE_T,              GHOSTTY_KEY_T },
    { SDL_SCANCODE_U,              GHOSTTY_KEY_U },
    { SDL_SCANCODE_V,              GHOSTTY_KEY_V },
    { SDL_SCANCODE_W,              GHOSTTY_KEY_W },
    { SDL_SCANCODE_X,              GHOSTTY_KEY_X },
    { SDL_SCANCODE_Y,              GHOSTTY_KEY_Y },
    { SDL_SCANCODE_Z,              GHOSTTY_KEY_Z },
    { SDL_SCANCODE_1,              GHOSTTY_KEY_DIGIT_1 },
    { SDL_SCANCODE_2,              GHOSTTY_KEY_DIGIT_2 },
    { SDL_SCANCODE_3,              GHOSTTY_KEY_DIGIT_3 },
    { SDL_SCANCODE_4,              GHOSTTY_KEY_DIGIT_4 },
    { SDL_SCANCODE_5,              GHOSTTY_KEY_DIGIT_5 },
    { SDL_SCANCODE_6,              GHOSTTY_KEY_DIGIT_6 },
    { SDL_SCANCODE_7,              GHOSTTY_KEY_DIGIT_7 },
    { SDL_SCANCODE_8,              GHOSTTY_KEY_DIGIT_8 },
    { SDL_SCANCODE_9,              GHOSTTY_KEY_DIGIT_9 },
    { SDL_SCANCODE_0,              GHOSTTY_KEY_DIGIT_0 },
    { SDL_SCANCODE_RETURN,         GHOSTTY_KEY_ENTER },
    { SDL_SCANCODE_ESCAPE,         GHOSTTY_KEY_ESCAPE },
    { SDL_SCANCODE_BACKSPACE,      GHOSTTY_KEY_BACKSPACE },
    { SDL_SCANCODE_TAB,            GHOSTTY_KEY_TAB },
    { SDL_SCANCODE_SPACE,          GHOSTTY_KEY_SPACE },
    { SDL_SCANCODE_MINUS,          GHOSTTY_KEY_MINUS },
    { SDL_SCANCODE_EQUALS,         GHOSTTY_KEY_EQUAL },
    { SDL_SCANCODE_LEFTBRACKET,    GHOSTTY_KEY_BRACKET_LEFT },
    { SDL_SCANCODE_RIGHTBRACKET,   GHOSTTY_KEY_BRACKET_RIGHT },
    { SDL_SCANCODE_BACKSLASH,      GHOSTTY_KEY_BACKSLASH },
    { SDL_SCANCODE_NONUSBACKSLASH, GHOSTTY_KEY_INTL_BACKSLASH },
    { SDL_SCANCODE_SEMICOLON,      GHOSTTY_KEY_SEMICOLON },
    { SDL_SCANCODE_APOSTROPHE,     GHOSTTY_KEY_QUOTE },
    { SDL_SCANCODE_GRAVE,          GHOSTTY_KEY_BACKQUOTE },
    { SDL_SCANCODE_COMMA,          GHOSTTY_KEY_COMMA },
    { SDL_SCANCODE_PERIOD,         GHOSTTY_KEY_PERIOD },
    { SDL_SCANCODE_SLASH,          GHOSTTY_KEY_SLASH },
    { SDL_SCANCODE_UP,             GHOSTTY_KEY_ARROW_UP },
    { SDL_SCANCODE_DOWN,           GHOSTTY_KEY_ARROW_DOWN },
    { SDL_SCANCODE_LEFT,           GHOSTTY_KEY_ARROW_LEFT },
    { SDL_SCANCODE_RIGHT,          GHOSTTY_KEY_ARROW_RIGHT },
    { SDL_SCANCODE_DELETE,         GHOSTTY_KEY_DELETE },
    { SDL_SCANCODE_END,            GHOSTTY_KEY_END },
    { SDL_SCANCODE_HOME,           GHOSTTY_KEY_HOME },
    { SDL_SCANCODE_PAGEDOWN,       GHOSTTY_KEY_PAGE_DOWN },
    { SDL_SCANCODE_PAGEUP,         GHOSTTY_KEY_PAGE_UP },
    { SDL_SCANCODE_INSERT,         GHOSTTY_KEY_INSERT },
    { SDL_SCANCODE_F1,             GHOSTTY_KEY_F1 },
    { SDL_SCANCODE_F2,             GHOSTTY_KEY_F2 },
    { SDL_SCANCODE_F3,             GHOSTTY_KEY_F3 },
    { SDL_SCANCODE_F4,             GHOSTTY_KEY_F4 },
    { SDL_SCANCODE_F5,             GHOSTTY_KEY_F5 },
    { SDL_SCANCODE_F6,             GHOSTTY_KEY_F6 },
    { SDL_SCANCODE_F7,             GHOSTTY_KEY_F7 },
    { SDL_SCANCODE_F8,             GHOSTTY_KEY_F8 },
    { SDL_SCANCODE_F9,             GHOSTTY_KEY_F9 },
    { SDL_SCANCODE_F10,            GHOSTTY_KEY_F10 },
    { SDL_SCANCODE_F11,            GHOSTTY_KEY_F11 },
    { SDL_SCANCODE_F12,            GHOSTTY_KEY_F12 },
    { SDL_SCANCODE_KP_0,           GHOSTTY_KEY_NUMPAD_0 },
    { SDL_SCANCODE_KP_1,           GHOSTTY_KEY_NUMPAD_1 },
    { SDL_SCANCODE_KP_2,           GHOSTTY_KEY_NUMPAD_2 },
    { SDL_SCANCODE_KP_3,           GHOSTTY_KEY_NUMPAD_3 },
    { SDL_SCANCODE_KP_4,           GHOSTTY_KEY_NUMPAD_4 },
    { SDL_SCANCODE_KP_5,           GHOSTTY_KEY_NUMPAD_5 },
    { SDL_SCANCODE_KP_6,           GHOSTTY_KEY_NUMPAD_6 },
    { SDL_SCANCODE_KP_7,           GHOSTTY_KEY_NUMPAD_7 },
    { SDL_SCANCODE_KP_8,           GHOSTTY_KEY_NUMPAD_8 },
    { SDL_SCANCODE_KP_9,           GHOSTTY_KEY_NUMPAD_9 },
    { SDL_SCANCODE_KP_ENTER,       GHOSTTY_KEY_NUMPAD_ENTER },
    { SDL_SCANCODE_KP_PLUS,        GHOSTTY_KEY_NUMPAD_ADD },
    { SDL_SCANCODE_KP_MINUS,       GHOSTTY_KEY_NUMPAD_SUBTRACT },
    { SDL_SCANCODE_KP_MULTIPLY,    GHOSTTY_KEY_NUMPAD_MULTIPLY },
    { SDL_SCANCODE_KP_DIVIDE,      GHOSTTY_KEY_NUMPAD_DIVIDE },
    { SDL_SCANCODE_KP_PERIOD,      GHOSTTY_KEY_NUMPAD_DECIMAL },
};

static GhosttyKey ghostty_key_for(SDL_Scancode scancode)
{
    size_t index;

    for (index = 0; index < sizeof KEY_MAP / sizeof KEY_MAP[0]; index++)
        if (KEY_MAP[index].sdl_scancode == scancode)
            return KEY_MAP[index].ghostty_key;
    return GHOSTTY_KEY_UNIDENTIFIED;
}

static GhosttyMods ghostty_modifiers_for(SDL_Keymod modifiers)
{
    GhosttyMods result = 0;

    if (modifiers & SDL_KMOD_SHIFT)
        result |= GHOSTTY_MODS_SHIFT |
                  ((modifiers & SDL_KMOD_RSHIFT) ? GHOSTTY_MODS_SHIFT_SIDE : 0);
    if (modifiers & SDL_KMOD_CTRL)
        result |= GHOSTTY_MODS_CTRL |
                  ((modifiers & SDL_KMOD_RCTRL) ? GHOSTTY_MODS_CTRL_SIDE : 0);
    if (modifiers & SDL_KMOD_ALT)
        result |= GHOSTTY_MODS_ALT |
                  ((modifiers & SDL_KMOD_RALT) ? GHOSTTY_MODS_ALT_SIDE : 0);
    if (modifiers & SDL_KMOD_GUI)
        result |= GHOSTTY_MODS_SUPER |
                  ((modifiers & SDL_KMOD_RGUI) ? GHOSTTY_MODS_SUPER_SIDE : 0);
    if (modifiers & SDL_KMOD_CAPS)
        result |= GHOSTTY_MODS_CAPS_LOCK;
    if (modifiers & SDL_KMOD_NUM)
        result |= GHOSTTY_MODS_NUM_LOCK;
    return result;
}

/* Write a block of text (a paste, or a burst from the IME) into the
 * terminal. Newlines become CRLF, because in a terminal a bare LF only
 * moves the cursor down. */
static void write_text_block(App *app, const char *text)
{
    size_t length;
    size_t index;
    size_t written = 0;
    char *converted;

    if (!text)
        return;
    length = strlen(text);
    if (length == 0)
        return;
    converted = (char *)malloc(length * 2 + 1);
    if (!converted)
        return;
    for (index = 0; index < length; index++) {
        if (text[index] == '\r')
            continue;
        if (text[index] == '\n') {
            converted[written++] = '\r';
            converted[written++] = '\n';
            continue;
        }
        converted[written++] = text[index];
    }
    term_write(&app->term, converted, written);
    free(converted);
    app->needs_redraw = true;
}

/* Encode one key event and write it into the terminal. No pty is attached,
 * so this also plays the kernel line discipline's echo role: plain Enter
 * echoes CRLF and plain Backspace erases the character in front of the
 * cursor. Escape sequences the encoder produces (arrows, ctrl combos, ...)
 * pass through untouched. */
static void write_key(App *app, const SDL_KeyboardEvent *event)
{
    static const uint8_t ENTER_ECHO[] = { '\r', '\n' };
    static const uint8_t BACKSPACE_ECHO[] = { '\b', ' ', '\b' };
    uint8_t encoded[64];
    size_t byte_count;
    GhosttyKey key = ghostty_key_for(event->scancode);
    SDL_Keycode unshifted;
    bool unmodified;

    if (key == GHOSTTY_KEY_UNIDENTIFIED)
        return;

    /* Ctrl+V (or Ctrl+Shift+V, the usual terminal binding) pastes from the
     * clipboard. There is no program behind the terminal yet, so the paste
     * goes into the terminal itself. */
    if (key == GHOSTTY_KEY_V && (event->mod & SDL_KMOD_CTRL) &&
        !(event->mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        char *clipboard = SDL_GetClipboardText();
        if (clipboard) {
            write_text_block(app, clipboard);
            SDL_free(clipboard);
        }
        return;
    }

    /* The unshifted codepoint (layout dependent, e.g. 'a' for the A key)
     * is what makes Ctrl+letter encode as 0x01-0x1a. */
    unshifted = SDL_GetKeyFromScancode(event->scancode, 0, false);
    if (unshifted & SDLK_SCANCODE_MASK)
        unshifted = 0;

    byte_count = term_encode_key(&app->term,
                                 event->repeat ? GHOSTTY_KEY_ACTION_REPEAT
                                               : GHOSTTY_KEY_ACTION_PRESS,
                                 key, ghostty_modifiers_for(event->mod),
                                 (uint32_t)unshifted, encoded, sizeof encoded);
    if (byte_count == 0)
        return;

    unmodified = !(event->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI));
    if (byte_count == 1 && encoded[0] == '\r' && unmodified)
        term_write(&app->term, ENTER_ECHO, sizeof ENTER_ECHO);
    else if (byte_count == 1 && (encoded[0] == 0x7f || encoded[0] == 0x08) &&
             unmodified)
        term_write(&app->term, BACKSPACE_ECHO, sizeof BACKSPACE_ECHO);
    else
        term_write(&app->term, encoded, byte_count);
    app->needs_redraw = true;
}

/* Layout text (letters, digits, punctuation, IME output). SDL delivers it
 * separately from key events; feed it straight through. */
static void handle_text_input(App *app, const SDL_TextInputEvent *event)
{
    write_text_block(app, event->text);
}

/* ------------------------------------------------------------------------- */
/* Terminal lifecycle and geometry                                            */
/* ------------------------------------------------------------------------- */

/* Fit the grid to the window: below the drag strip, above the status line,
 * inside the side padding. Resizes the terminal when the grid changed. */
static void fit_grid(App *app)
{
    int pixel_width;
    int pixel_height;
    int padding;
    int columns;
    int rows;

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    padding = (int)(12.0f * app->display_scale);
    app->grid_left = padding;
    app->grid_top = (int)(34.0f * app->display_scale);

    columns = (pixel_width - 2 * padding) / app->glyphs.cell_width;
    rows = (pixel_height - app->grid_top - padding - app->glyphs.cell_height) /
           app->glyphs.cell_height;
    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;

    if (columns != app->columns || rows != app->rows) {
        app->columns = columns;
        app->rows = rows;
        term_resize(&app->term, (uint16_t)columns, (uint16_t)rows,
                    (uint32_t)app->glyphs.cell_width,
                    (uint32_t)app->glyphs.cell_height);
    }
    /* The window region follows the window size. */
    window_sync_chrome(app->window, app->backdrop, app->panel.radius);
    app->needs_redraw = true;
}

static void write_demo(App *app)
{
    const char *prompt = glyphs_has(&app->glyphs, 0x276F) ? PROMPT_CHEVRON
                                                          : ">";
    const char *lines[] = {
        "\x1b[1;36mtermica\x1b[0m \x1b[90m- milestone 1: a real terminal grid\x1b[0m\r\n",
        "\x1b[90mlibghostty-vt 1.0.1 does the parsing, we only draw it\x1b[0m\r\n",
        "\x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m "
        "\x1b[7minverse\x1b[0m \x1b[9mstrike\x1b[0m \x1b[2mdim\x1b[0m\r\n",
        "\x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m "
        "\x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m\r\n",
        "\x1b[38;2;255;128;0mtruecolor orange\x1b[0m  "
        "\x1b[48;2;40;44;52m 256-color background \x1b[0m\r\n",
        "\x1b[90mtype below: arrows move the cursor, enter breaks the line, "
        "backspace erases, ctrl+v pastes\x1b[0m\r\n",
        "\x1b[90mwindow: Alt+Up/Down opacity, Alt+M backdrop, Esc quit\x1b[0m\r\n",
        "\r\n",
        "\x1b[1;32m",
        prompt,
        "\x1b[0m ",
    };
    size_t index;

    for (index = 0; index < sizeof lines / sizeof lines[0]; index++)
        term_write(&app->term, lines[index], strlen(lines[index]));
    app->needs_redraw = true;
}

/* ------------------------------------------------------------------------- */
/* Frame rendering from the render state                                      */
/* ------------------------------------------------------------------------- */

static void draw_status_line(App *app, int row, int cursor_column,
                             int cursor_row)
{
    char status[160];
    const char *text = status;
    int column = 0;

    SDL_snprintf(status, sizeof status,
                 "%dx%d cells | cursor %d,%d | opacity %d%% | %s",
                 app->columns, app->rows, cursor_column + 1, cursor_row + 1,
                 (int)(app->panel_opacity * 100.0f + 0.5f),
                 window_backdrop_name(app->backdrop));

    while (*text) {
        char utf8[8];
        char *end;
        Uint32 codepoint = SDL_StepUTF8(&text, NULL);

        if (!codepoint)
            break;
        end = SDL_UCS4ToUTF8(codepoint, utf8);
        *end = '\0';
        glyphs_draw_text(&app->glyphs, utf8, (size_t)(end - utf8),
                         cell_x(app, column), cell_y(app, row), false, false,
                         STATUS_TEXT);
        column++;
    }
}

/* The translucent window forces a full redraw every frame, so per-row dirty
 * flags do not buy anything here: every row of the viewport is visited. */
static void draw_frame(App *app)
{
    SDL_Renderer *renderer = app->renderer;
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

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);

    /* 1. Clear to fully transparent; everything below blends over this. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    /* 2. The panel: our rounded fill, or a tint over a DWM backdrop. */
    panel_draw(&app->panel, renderer, pixel_width, pixel_height,
               panel_fill_color(app), PANEL_BORDER);

    /* 3. Capture the frame. The only step that reads the terminal. */
    ghostty_render_state_update(app->term.render_state, app->term.handle);

    /* 4. Defaults and cursor, before walking the rows. */
    ghostty_render_state_get(app->term.render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLORS, &state_colors);
    frame.app = app;
    frame.colors = state_colors;
    frame.default_foreground = style_color(state_colors.foreground, 1.0f);
    frame.cursor_color = state_colors.cursor_has_value
                         ? style_color(state_colors.cursor, 1.0f)
                         : frame.default_foreground;
    frame.cursor_style = GHOSTTY_CURSOR_STYLE_BLOCK;

    if (ghostty_render_state_get(app->term.render_state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR,
                                 &cursor_state) == GHOSTTY_SUCCESS) {
        frame.cursor_style = cursor_state.visual_style;
        app->cursor_blinks = cursor_state.blinking;
        if (cursor_state.visible && cursor_state.viewport_has_value) {
            cursor_column = cursor_state.viewport_x;
            cursor_row = cursor_state.viewport_y;
            cursor_drawn = true;
        }
    }
    /* A blinking cursor is only drawn half the time. */
    if (app->cursor_blinks && !app->cursor_lit)
        cursor_drawn = false;

    /* 5. Rows and cells. The data is valid until the next update. */
    ghostty_render_state_get(app->term.render_state,
                             GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                             &app->term.row_iterator);
    while (ghostty_render_state_row_iterator_next(app->term.row_iterator)) {
        int32_t row_position = 0;
        int column = 0;

        ghostty_render_state_row_get(app->term.row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                                     &row_position);
        ghostty_render_state_row_get(app->term.row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &app->term.cell_iterator);

        while (ghostty_render_state_row_cells_next(app->term.cell_iterator)) {
            CellPaint cell;
            GhosttyCell raw_cell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            uint8_t text_bytes[128];
            GhosttyBuffer grapheme = { .ptr = text_bytes,
                                       .cap = sizeof text_bytes };

            ghostty_render_state_row_cells_get(
                app->term.cell_iterator,
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
                app->term.cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &cell.style);
            cell.has_foreground = ghostty_render_state_row_cells_get(
                app->term.cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                &cell.foreground) == GHOSTTY_SUCCESS;
            cell.has_background = ghostty_render_state_row_cells_get(
                app->term.cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                &cell.background) == GHOSTTY_SUCCESS;
            ghostty_render_state_row_cells_get(
                app->term.cell_iterator,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &grapheme);
            cell.text = grapheme.len <= grapheme.cap ? text_bytes : NULL;
            cell.text_length = cell.text ? grapheme.len : 0;
            cell.at_cursor = cursor_drawn && column == cursor_column &&
                             row_position == cursor_row;

            draw_cell(&frame, &cell);
            column++;
        }
    }

    /* 6. The frame is drawn, so the next one only reports new changes. */
    ghostty_render_state_clean(app->term.render_state);

    /* 7. Status line, one row below the grid. */
    draw_status_line(app, app->rows, cursor_column, cursor_row);

    SDL_RenderPresent(renderer);
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
    int index;

    if (argc > 1)
        return argv[1];
    for (index = 0; fallbacks[index]; index++) {
        SDL_IOStream *probe = SDL_IOFromFile(fallbacks[index], "rb");
        if (probe) {
            SDL_CloseIO(probe);
            return fallbacks[index];
        }
    }
    return NULL;
}

/* The cursor blink. A timer wakes the event loop twice a second so the
 * cursor can be drawn and not drawn. */
static Uint32 SDLCALL on_blink_timer(void *userdata, SDL_TimerID timer_id,
                                     Uint32 interval)
{
    SDL_Event event;

    (void)userdata;
    (void)timer_id;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = 1;
    SDL_PushEvent(&event);
    return interval;
}

static void handle_key_down(App *app, const SDL_KeyboardEvent *event)
{
    if (event->key == SDLK_ESCAPE) {
        app->running = false;
        return;
    }
    if (event->mod & SDL_KMOD_ALT) {
        if (event->key == SDLK_UP) {
            app->panel_opacity += 0.05f;
            if (app->panel_opacity > 1.0f)
                app->panel_opacity = 1.0f;
            app->needs_redraw = true;
            return;
        }
        if (event->key == SDLK_DOWN) {
            app->panel_opacity -= 0.05f;
            if (app->panel_opacity < 0.0f)
                app->panel_opacity = 0.0f;
            app->needs_redraw = true;
            return;
        }
        if (event->key == SDLK_M && !event->repeat) {
            WindowBackdrop next =
                (WindowBackdrop)((app->backdrop + 1) % WINDOW_BACKDROP_COUNT);
            /* The OS may not know the material; the window says so and the
             * app keeps the mode that is really in effect. */
            app->backdrop = window_sync_chrome(app->window, next,
                                               app->panel.radius);
            app->needs_redraw = true;
            return;
        }
    }
    write_key(app, event);
}

int main(int argc, char **argv)
{
    App app;
    const char *font_path;
    float point_size = 16.0f;
    int exit_code = 1;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return term_selftest() == 0 ? 0 : 1;

    memset(&app, 0, sizeof app);
    app.running = true;
    app.panel_opacity = 0.80f;
    app.cursor_lit = true;

    font_path = pick_font(argc, argv);
    if (!font_path) {
        fprintf(stderr, "usage: %s [--selftest] path\\to\\font.ttf "
                "[point-size]\n", argv[0]);
        return 1;
    }
    if (argc > 2)
        point_size = (float)SDL_atof(argv[2]);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        goto cleanup;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        goto cleanup;
    }

    app.window = window_create("termica", 900, 480);
    if (!app.window) {
        fprintf(stderr, "window_create: %s\n", SDL_GetError());
        goto cleanup;
    }
    SDL_StartTextInput(app.window);  /* deliver SDL_EVENT_TEXT_INPUT */

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        goto cleanup;
    }
    printf("renderer: %s\n", SDL_GetRendererName(app.renderer));

    app.display_scale = SDL_GetWindowDisplayScale(app.window);
    if (app.display_scale <= 0.0f)
        app.display_scale = 1.0f;

    if (!glyphs_open(&app.glyphs, app.renderer, font_path, point_size,
                     app.display_scale)) {
        fprintf(stderr, "font: %s: %s\n", font_path, SDL_GetError());
        goto cleanup;
    }
    printf("font: %s  cell: %dx%d px  scale: %.2f  bold: %s  italic: %s\n",
           font_path, app.glyphs.cell_width, app.glyphs.cell_height,
           (double)app.display_scale, app.glyphs.bold ? "yes" : "no",
           app.glyphs.italic ? "yes" : "no");
    fflush(stdout);

    if (!panel_init(&app.panel, app.renderer, app.display_scale)) {
        fprintf(stderr, "panel_init: %s\n", SDL_GetError());
        goto cleanup;
    }

    /* Terminal: create at a default size, then fit to the window. */
    if (!term_init(&app.term, 80, 24)) {
        fprintf(stderr, "libghostty-vt setup failed\n");
        goto cleanup;
    }

    window_sync_chrome(app.window, app.backdrop, app.panel.radius);
    fit_grid(&app);
    write_demo(&app);

    app.blink_timer = SDL_AddTimer(500, on_blink_timer, &app);

    while (app.running) {
        SDL_Event event;

        if (!SDL_WaitEvent(&event))
            break;
        do {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                app.running = false;
                break;
            case SDL_EVENT_USER:
                if (event.user.code == 1) {
                    app.cursor_lit = !app.cursor_lit;
                    if (app.cursor_blinks)
                        app.needs_redraw = true;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                handle_key_down(&app, &event.key);
                break;
            case SDL_EVENT_TEXT_INPUT:
                handle_text_input(&app, &event.text);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                fit_grid(&app);
                break;
            default:
                break;
            }
        } while (app.running && SDL_PollEvent(&event));

        /* The program changed the window title with an OSC sequence. */
        if (app.term.title_changed) {
            app.term.title_changed = false;
            if (app.term.title[0])
                SDL_SetWindowTitle(app.window, app.term.title);
        }

        if (app.running && app.needs_redraw) {
            draw_frame(&app);
            app.needs_redraw = false;
        }
    }
    exit_code = 0;

cleanup:
    /* Every step is safe on a partly initialised app, so a failure in the
     * middle of setup leaks nothing either. */
    if (app.blink_timer)
        SDL_RemoveTimer(app.blink_timer);
    term_free(&app.term);
    panel_free(&app.panel);
    glyphs_close(&app.glyphs);
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();
    return exit_code;
}
