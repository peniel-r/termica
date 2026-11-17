/*
 * termica - milestone 3: the pointer, the history and focus.
 *
 * Milestone 0 proved the look (borderless, translucent, rounded, glyph
 * cache). Milestone 1 put a real terminal behind it, milestone 2 a real
 * shell behind that. This milestone hands the shell what it asks for and
 * keeps the rest usable:
 *
 *   1. libghostty-vt owns the screen state. vt_write() parses every
 *      escape sequence (SGR colors and attributes, cursor movement,
 *      erase, scroll, OSC, ...) for us. See
 *      https://lib.ghostty.org/vt/first-terminal.html
 *   2. pty.c starts a shell in a ConPTY. Keys and pastes go into the
 *      pseudoconsole, its UTF-8 output comes back through a reader thread
 *      and is fed to the terminal. The program echoes, not us.
 *   3. pointer events go to the program when it enabled mouse tracking
 *      (encoded by the library from surface pixels), and to the app
 *      otherwise: dragging selects, Shift reclaims the pointer, and the
 *      wheel or the Ctrl+Shift+* keys move the viewport through scrollback
 *      with a thumb on the right edge. Focus follows mode 1004.
 *   4. every frame is drawn from a GhosttyRenderState: rows, cells,
 *      resolved colors, attributes, the cursor, selection and the bar.
 *
 * The pieces live in their own files:
 *   term.c      the libghostty-vt wrapper (screen, encoders, replies,
 *               scrolling, selection)
 *   pty.c       ConPTY: the child process, the pipes, the reader thread
 *   embed.c     the font compiled into the executable
 *   glyph.c     fonts, the glyph cache, drawing text
 *   style.c     terminal cell style -> colors and flags
 *   panel.c     the rounded translucent panel
 *   geometry.c  the squircle corner curve
 *   window.c    the SDL window and the Windows 11 chrome
 *
 * Usage:  termica [--selftest] [path\to\font.ttf] [point-size]
 * Keys:   typing, arrows, PgUp/PgDn, Enter, Backspace, Esc and every Alt+*
 *         go to the shell; Ctrl+V pastes, Ctrl+Shift+C copies the selection,
 *         Ctrl+Shift+Q quits, wheel / Shift+wheel / Ctrl+Shift+Up-Down-Home-End
 *         and the scrollbar move through the history, drag / double / triple
 *         select text (Alt+drag is a block),
 *         Ctrl+Shift+= / - = panel opacity,
 *         Ctrl+Shift+B = cycle the window backdrop (panel, Mica, Mica Alt,
 *         Acrylic)
 *
 * Strict C99. Windows-only bits are behind #ifdef _WIN32.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "console.h"
#include "embed.h"
#include "glyph.h"
#include "panel.h"
#include "pty.h"
#include "settings.h"
#include "style.h"
#include "term.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Over a DWM backdrop the panel is only a light tint: the material is the
 * background, the tint just keeps the text readable. */
#define BACKDROP_TINT 0.24f

/* The panel color doubles as the terminal's default background, so inverse
 * video and the cursor have something to work with. */
static const SDL_Color PANEL_BACKGROUND = { 14, 18, 22, 255 };
static const SDL_Color PANEL_BORDER = { 90, 100, 112, PANEL_BORDER_ALPHA };
static const SDL_Color STATUS_TEXT = { 130, 138, 148, 255 };

/* What the built-in family is called, for the startup line. */
#define BUILT_IN_FONT "Maple Mono NF (built in)"

/* Room for uncommitted IME text. SDL3 hands the string over for the
 * duration of the event, so the app keeps its own copy; 128 bytes is
 * several dozen composed characters. */
#define COMPOSING_MAX 128

/* What a left-drag means: single cells, whole words, or whole lines. */
enum {
    SELECT_CELLS = 0,
    SELECT_WORDS = 1,
    SELECT_LINES = 2
};

/* A left-button gesture. Coordinates are viewport cells, so a selection
 * stays where it is while the user scrolls through history. */
typedef struct SelectionDrag {
    bool dragging;
    bool moved;
    bool rectangle;
    int  mode;
    int  anchor_x;
    int  anchor_y;
    int  focus_x;
    int  focus_y;
} SelectionDrag;

typedef struct App {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    Glyphs        glyphs;
    Panel         panel;
    Term          term;
    Pty          *pty;              /* the shell behind the grid        */

    float          display_scale;
    float          panel_opacity;   /* 0..1                            */
    WindowBackdrop backdrop;

    int            grid_left;       /* pixel position of cell 0,0       */
    int            grid_top;
    int            columns;
    int            rows;

    SelectionDrag  select;
    bool           mouse_held;      /* a button is down over the grid   */
    bool           bar_drag;        /* the scrollbar thumb is being dragged */
    Settings       settings;

    /* The pointer shape the program last asked for, and the cursor made
     * from it. NULL cursor means the system default. */
    GhosttyMouseShape shape;
    SDL_Cursor      *cursor;

    /* In-progress IME text, drawn at the cursor until it is committed.
     * SDL hands the string over for the duration of the event only, so it
     * is copied here. */
    char             composing[COMPOSING_MAX];
    int              composing_length;

    int              autoscroll;    /* -1 up, 0 off, +1 down while dragging */
    SDL_TimerID      autoscroll_timer;

    bool           running;
    bool           needs_redraw;
    bool           shell_exited;
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
    bool           selected;
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
        fill_rectangle(app, x, base, width, 1.0f, color);
        fill_rectangle(app, x, base - 3.0f, width, 1.0f, color);
        break;
    case GHOSTTY_SGR_UNDERLINE_CURLY:
        pixels = (int)(width + 0.5f);
        for (pixel = 0; pixel < pixels; pixel++) {
            float wave = SDL_sinf((float)pixel / (float)pixels * 6.2831855f);
            float y = base + (wave < 0.0f ? -1.0f : 0.0f);
            fill_rectangle(app, x + (float)pixel, y, 1.0f, 1.0f, color);
        }
        break;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        /* one pixel of ink, one pixel of gap, continuing across the row */
        phase = (float)((int)SDL_fmod(x, 2.0));
        for (offset = phase > 0.0f ? 2.0f - phase : 0.0f;
             offset + 1.0f <= width; offset += 2.0f)
            fill_rectangle(app, x + offset, base, 1.0f, 1.0f, color);
        break;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        /* three pixels of ink, two of gap, continuing across the row */
        phase = (float)((int)SDL_fmod(x, 5.0));
        for (offset = phase > 0.0f ? 5.0f - phase : 0.0f; offset < width;
             offset += 5.0f) {
            float run = width - offset;
            if (run > 3.0f)
                run = 3.0f;
            fill_rectangle(app, x + offset, base, run, 1.0f, color);
        }
        break;
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
    default:
        fill_rectangle(app, x, base, width, 2.0f, color);
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
                  app->panel_opacity, cell->selected);

    if (style.has_background)
        fill_rectangle(app, x, y, width, height, style.background);

    draw_cell_glyph(frame, cell, &style, false);

    if (style.underline)
        draw_underline(app, x, y + height - 2.0f, width, style.decoration,
                       style.underline_style);
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

/* Send bytes to the program: key encoding, pastes, and the terminal's own
 * replies (device status reports, mode queries) all end up here. A no-op
 * before the shell exists or after it is gone. */
static void send_to_shell(void *userdata, const void *data, size_t length)
{
    App *app = (App *)userdata;

    if (app->pty && length > 0)
        pty_write(app->pty, data, length);
}

/* Anything the user types is meant for the live view, so a scrolled-back
 * viewport snaps to the bottom first. The library ignores it when the
 * viewport already follows the active area. */
static void jump_to_bottom(App *app)
{
    if (!term_viewport_at_bottom(&app->term))
        term_scroll_bottom(&app->term);
}

/* Copy the selection to the clipboard. */
static void copy_selection(App *app)
{
    char *text = term_selection_text(&app->term, NULL);

    if (!text)
        return;
    if (!SDL_SetClipboardText(text))
        fprintf(stderr, "clipboard: %s\n", SDL_GetError());
    free(text);
}

/* Send text to the program (layout text, an IME commit). SDL reports Enter
 * as "\r" in SDL_EVENT_TEXT_INPUT as well as on the key event, and the key
 * encoder already sent it, so the carriage returns are dropped here instead
 * of typing twice. */
static void send_text(App *app, const char *text)
{
    const char *start;
    const char *cursor;

    if (!text)
        return;
    jump_to_bottom(app);
    start = text;
    cursor = text;
    while (*cursor) {
        if (*cursor == '\r') {
            if (cursor > start)
                send_to_shell(app, start, (size_t)(cursor - start));
            start = cursor + 1;
        }
        cursor++;
    }
    if (cursor > start)
        send_to_shell(app, start, (size_t)(cursor - start));
}

/* Ask before a program replaces the clipboard. The message names the
 * program when the protocol carried one, and Enter copies while Escape
 * denies. */
static bool confirm_clipboard_write(App *app, const char *name)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Deny" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Copy" }
    };
    char detail[512];
    const SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_WARNING,
        app->window,
        "Allow clipboard write?",
        detail,
        SDL_arraysize(buttons),
        buttons,
        NULL
    };
    int pressed = 1;

    if (name && name[0])
        snprintf(detail, sizeof detail,
                 "\"%s\" wants to replace the clipboard contents.", name);
    else
        snprintf(detail, sizeof detail,
                 "The program wants to replace the clipboard contents.");

    if (!SDL_ShowMessageBox(&box, &pressed))
        return false;
    return pressed == 0;
}

/* A program asked for the clipboard (OSC 52, iTerm2, kitty). The settings
 * decide the policy: ask, allow or deny. Nothing is written back into the
 * terminal from here - the VT stream is paused while this runs, so a
 * message box is the feedback. */
static TermClipboardResult handle_clipboard_write(void *userdata,
                                                  const char *text,
                                                  size_t length,
                                                  const char *name)
{
    App *app = (App *)userdata;

    (void)length;
    switch (app->settings.osc52) {
    case SETTINGS_OSC52_ALLOW:
        break;
    case SETTINGS_OSC52_DENY:
        return TERM_CLIPBOARD_DENIED;
    default:
        if (!confirm_clipboard_write(app, name))
            return TERM_CLIPBOARD_DENIED;
        break;
    }
    if (!SDL_SetClipboardText(text))
        return TERM_CLIPBOARD_DENIED;
    return TERM_CLIPBOARD_ALLOWED;
}

/* Ask before pasting text that could run commands: line breaks outside
 * bracketed paste, or an embedded end-of-paste marker. */
static bool confirm_unsafe_paste(App *app)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Paste" }
    };
    const SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_WARNING,
        app->window,
        "Paste anyway?",
        "The clipboard contains line breaks or an end-of-paste marker that a "
        "shell can run as commands.",
        SDL_arraysize(buttons),
        buttons,
        NULL
    };
    int pressed = 1;

    if (!SDL_ShowMessageBox(&box, &pressed))
        return false;
    return pressed == 0;
}

/* Paste the clipboard the way the program asked for it. The library frames
 * bracketed paste and rejects unsafe text; the rejection is confirmed with
 * the user and then retried. */
static void paste_clipboard(App *app)
{
    char *clipboard = SDL_GetClipboardText();
    size_t length;
    bool rejected = false;

    if (!clipboard)
        return;
    length = strlen(clipboard);
    jump_to_bottom(app);
    if (!term_paste(&app->term, clipboard, length, false, &rejected) &&
        rejected && confirm_unsafe_paste(app))
        term_paste(&app->term, clipboard, length, true, NULL);
    SDL_free(clipboard);
    app->needs_redraw = true;
}

/* Encode one key event and write it to the program. The bytes are exactly
 * what the terminal modes ask for; the program echoes them back. */
static void write_key(App *app, const SDL_KeyboardEvent *event)
{
    uint8_t encoded[64];
    size_t byte_count;
    GhosttyKey key = ghostty_key_for(event->scancode);
    SDL_Keycode unshifted;

    if (key == GHOSTTY_KEY_UNIDENTIFIED)
        return;

    /* Ctrl+V and Ctrl+Shift+V paste from the clipboard, like every other
     * Windows terminal. */
    if (key == GHOSTTY_KEY_V && (event->mod & SDL_KMOD_CTRL) &&
        !(event->mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        paste_clipboard(app);
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
    jump_to_bottom(app);
    send_to_shell(app, encoded, byte_count);
}

/* Layout text (letters, digits, punctuation, IME output). SDL delivers it
 * separately from key events; feed it straight through. */
static void handle_text_input(App *app, const SDL_TextInputEvent *event)
{
    send_text(app, event->text);
    /* A commit ends the composition. */
    app->composing_length = 0;
    app->composing[0] = '\0';
}

/* Uncommitted IME text. Empty text means the composition was cancelled, so
 * the app forgets it; anything else is kept for the frame and drawn at the
 * cursor until the commit event above clears it. */
static void handle_text_editing(App *app, const SDL_TextEditingEvent *event)
{
    size_t length;

    if (!event->text || event->text[0] == '\0') {
        app->composing_length = 0;
        app->composing[0] = '\0';
        app->needs_redraw = true;
        return;
    }

    length = strlen(event->text);
    if (length >= COMPOSING_MAX)
        length = COMPOSING_MAX - 1;
    memcpy(app->composing, event->text, length);
    app->composing[length] = '\0';
    app->composing_length = (int)length;
    app->needs_redraw = true;
}

/* ------------------------------------------------------------------------- */
/* Pointer and focus                                                          */
/* ------------------------------------------------------------------------- */

/* Rows moved by one wheel notch. */
#define SCROLL_ROWS_PER_NOTCH 3

/* The cell under a pointer position, or false when the position is not on
 * the grid. The drag strip and the resize borders are app chrome: a click
 * there is not a click on text. */
static bool cell_at(const App *app, float x, float y, int *out_x, int *out_y)
{
    int column;
    int row;

    if (x < (float)app->grid_left || y < (float)app->grid_top)
        return false;
    column = (int)((x - app->grid_left) / (float)app->glyphs.cell_width);
    row = (int)((y - app->grid_top) / (float)app->glyphs.cell_height);
    if (column < 0 || row < 0 || column >= app->columns || row >= app->rows)
        return false;
    *out_x = column;
    *out_y = row;
    return true;
}

/* The same, but clamped to the grid: a drag that leaves the window keeps
 * extending the selection to the last row instead of freezing. */
static void cell_clamped(const App *app, float x, float y, int *out_x,
                         int *out_y)
{
    int column = (int)((x - app->grid_left) / (float)app->glyphs.cell_width);
    int row = (int)((y - app->grid_top) / (float)app->glyphs.cell_height);

    if (column < 0)
        column = 0;
    if (row < 0)
        row = 0;
    if (column > app->columns - 1)
        column = app->columns - 1;
    if (row > app->rows - 1)
        row = app->rows - 1;
    *out_x = column;
    *out_y = row;
}

/* The scrollback bar. It lives in the right margin, outside the cell grid,
 * so it is app chrome even when a program has asked for the pointer. */
typedef struct ScrollBar {
    bool     visible;
    float    x;
    float    width;
    float    track_top;
    float    track_height;
    float    thumb_top;
    float    thumb_height;
    uint64_t first_row;    /* the row currently at the top of the viewport */
    uint64_t page_rows;    /* rows in the viewport                         */
    uint64_t scrollable;   /* total - page                                 */
} ScrollBar;

static void scrollbar_geometry(App *app, ScrollBar *bar)
{
    GhosttyTerminalScrollbar data;
    int pixel_width;
    int pixel_height;

    memset(bar, 0, sizeof *bar);
    if (!term_scrollbar(&app->term, &data) || data.total <= data.len)
        return;
    if (term_viewport_at_bottom(&app->term))
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
static uint64_t scrollbar_row_at(const ScrollBar *bar, float y)
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

static bool scrollbar_hit(const ScrollBar *bar, float x, float y,
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
static bool scrollbar_press(App *app, const ScrollBar *bar, float y)
{
    bool on_thumb = y >= bar->track_top + bar->thumb_top &&
                    y <= bar->track_top + bar->thumb_top + bar->thumb_height;
    uint64_t row;

    if (on_thumb) {
        app->bar_drag = true;
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
    term_scroll_row(&app->term, (uint32_t)row);
    app->needs_redraw = true;
    return true;
}

static GhosttyMouseButton mouse_button_for(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return GHOSTTY_MOUSE_BUTTON_LEFT;
    case SDL_BUTTON_MIDDLE: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case SDL_BUTTON_RIGHT:  return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case SDL_BUTTON_X1:     return GHOSTTY_MOUSE_BUTTON_FOUR;
    case SDL_BUTTON_X2:     return GHOSTTY_MOUSE_BUTTON_FIVE;
    default:                return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

/* SDL reports modifiers on key events only; a pointer event needs the
 * current state. */
static SDL_Keymod mouse_modifiers(void)
{
    return SDL_GetModState();
}

/* Whether the pointer belongs to the program. A program that asked for
 * mouse tracking gets it; holding Shift takes it back for the app, which
 * is how every terminal keeps selection and scrollback reachable. */
static bool pointer_belongs_to_shell(App *app, SDL_Keymod modifiers)
{
    if (modifiers & SDL_KMOD_SHIFT)
        return false;
    return term_mouse_tracking(&app->term);
}

static void report_mouse(App *app, GhosttyMouseAction action,
                         GhosttyMouseButton button, SDL_Keymod modifiers,
                         float x, float y)
{
    uint8_t encoded[128];
    size_t bytes;

    bytes = term_encode_mouse(&app->term, action, button,
                              ghostty_modifiers_for(modifiers), x, y,
                              encoded, sizeof encoded);
    send_to_shell(app, encoded, bytes);
}

/* Rebuild the selection from the current gesture. The anchor is the tracked
 * reference from the press, the focus is the cell under the pointer; the
 * library owns the semantics (word boundaries, line extents). */
static void selection_update(App *app)
{
    SelectionDrag *drag = &app->select;

    switch (drag->mode) {
    case SELECT_WORDS:
        /* The nearest word between the anchor and the pointer, so a drag
         * that passes between words does not collapse to nothing. */
        if (!term_selection_drag_words(&app->term, drag->focus_x,
                                       drag->focus_y))
            term_selection_word(&app->term, drag->focus_x, drag->focus_y);
        break;
    case SELECT_LINES:
        if (!term_selection_drag_line(&app->term))
            term_selection_line(&app->term, drag->anchor_x, drag->anchor_y);
        break;
    default:
        if (!term_selection_drag_cells(&app->term, drag->focus_x,
                                       drag->focus_y, drag->rectangle))
            term_selection_cells(&app->term, drag->anchor_x, drag->anchor_y,
                                 drag->focus_x, drag->focus_y,
                                 drag->rectangle);
        break;
    }
    app->needs_redraw = true;
}

/* ------------------------------------------------------------------------- */
/* Pointer shape, IME composition and dragging into history                 */
/* ------------------------------------------------------------------------- */

/* OSC 22 asks for a shape by name; SDL has a system cursor for each of the
 * ones a terminal ever gets asked for. Anything unknown is the default,
 * because a wrong arrow is better than a wrong resize double-arrow. */
static SDL_SystemCursor cursor_for_shape(GhosttyMouseShape shape)
{
    switch (shape) {
    case GHOSTTY_MOUSE_SHAPE_TEXT:            return SDL_SYSTEM_CURSOR_TEXT;
    case GHOSTTY_MOUSE_SHAPE_POINTER:         return SDL_SYSTEM_CURSOR_POINTER;
    case GHOSTTY_MOUSE_SHAPE_WAIT:            return SDL_SYSTEM_CURSOR_WAIT;
    case GHOSTTY_MOUSE_SHAPE_PROGRESS:        return SDL_SYSTEM_CURSOR_PROGRESS;
    case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:       return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:     return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    case GHOSTTY_MOUSE_SHAPE_MOVE:            return SDL_SYSTEM_CURSOR_MOVE;
    case GHOSTTY_MOUSE_SHAPE_N_RESIZE:        return SDL_SYSTEM_CURSOR_N_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_S_RESIZE:        return SDL_SYSTEM_CURSOR_S_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_E_RESIZE:        return SDL_SYSTEM_CURSOR_E_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_W_RESIZE:        return SDL_SYSTEM_CURSOR_W_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_NE_RESIZE:       return SDL_SYSTEM_CURSOR_NE_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_NW_RESIZE:       return SDL_SYSTEM_CURSOR_NW_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_SE_RESIZE:       return SDL_SYSTEM_CURSOR_SE_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_SW_RESIZE:       return SDL_SYSTEM_CURSOR_SW_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:       return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:       return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE:     return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE:     return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT:   return SDL_SYSTEM_CURSOR_TEXT;
    default:                                  return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

/* The program asked for a shape; make it the window's cursor. Called on
 * motion because that is the only moment a shape can have changed under
 * the pointer. */
static void apply_mouse_shape(App *app)
{
    GhosttyMouseShape shape;
    SDL_Cursor *cursor;

    if (!term_mouse_shape(&app->term, &shape))
        return;
    if (app->cursor && shape == app->shape)
        return;
    app->shape = shape;

    cursor = SDL_CreateSystemCursor(cursor_for_shape(shape));
    if (!cursor)
        return;
    if (!SDL_SetCursor(cursor)) {
        SDL_DestroyCursor(cursor);
        return;
    }
    if (app->cursor)
        SDL_DestroyCursor(app->cursor);
    app->cursor = cursor;
}

/* SDL's cursor is process-wide, so leaving the window has to hand the
 * pointer back to the system or the whole desktop keeps the terminal's
 * shape. */
static void reset_mouse_shape(App *app)
{
    SDL_Cursor *system_default = SDL_GetDefaultCursor();

    if (!app->cursor)
        return;
    if (system_default)
        SDL_SetCursor(system_default);
    SDL_DestroyCursor(app->cursor);
    app->cursor = NULL;
    app->shape = GHOSTTY_MOUSE_SHAPE_DEFAULT;
}

/* Where the IME should put its candidate window: the cell the terminal
 * cursor is in. The render state already knows it, so this is computed
 * while drawing rather than guessed from history. */
static void update_text_input_area(App *app)
{
    GhosttyRenderStateCursor cursor_state =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    SDL_Rect area;

    if (ghostty_render_state_get(app->term.render_state,
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
    const char *text = app->composing;
    int column = cursor_column;

    if (app->composing_length == 0)
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
                         false, STATUS_TEXT);
        column++;
    }
}

/* Rows moved by one autoscroll tick, and how fast those ticks come. */
#define AUTOSCROLL_ROWS 1
#define AUTOSCROLL_INTERVAL_MS 40

/* A drag that reaches the last row keeps pulling history in, so a
 * selection longer than the screen is possible without letting go. */
static Uint32 SDLCALL on_autoscroll_timer(void *userdata, SDL_TimerID timer_id,
                                          Uint32 interval)
{
    SDL_Event event;

    (void)userdata;
    (void)timer_id;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = 3;
    SDL_PushEvent(&event);
    return interval;
}

static void autoscroll_start(App *app, int direction)
{
    if (app->autoscroll == direction && app->autoscroll_timer)
        return;
    app->autoscroll = direction;
    if (direction == 0) {
        if (app->autoscroll_timer) {
            SDL_RemoveTimer(app->autoscroll_timer);
            app->autoscroll_timer = 0;
        }
        return;
    }
    if (!app->autoscroll_timer)
        app->autoscroll_timer = SDL_AddTimer(AUTOSCROLL_INTERVAL_MS,
                                             on_autoscroll_timer, app);
}

static void autoscroll_tick(App *app)
{
    if (app->autoscroll == 0 || !app->select.dragging)
        return;
    term_scroll_rows(&app->term, app->autoscroll > 0 ? AUTOSCROLL_ROWS
                                                     : -AUTOSCROLL_ROWS);
    /* The focus end is the edge row that is now under the pointer. */
    app->select.focus_y = app->autoscroll > 0 ? app->rows - 1 : 0;
    app->select.moved = true;
    selection_update(app);
}

static void handle_mouse_button(App *app, const SDL_MouseButtonEvent *event)
{
    bool pressed = event->down;
    SDL_Keymod modifiers = mouse_modifiers();
    ScrollBar bar;
    int pixel_width = 0;
    int pixel_height = 0;
    int cell_x = 0;
    int cell_y = 0;
    bool inside;

    /* The bar comes first: it is outside the grid, so a program that owns
     * the pointer still cannot swallow the only control that reaches the
     * top of the scrollback without the keyboard. */
    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    scrollbar_geometry(app, &bar);
    if (event->button == SDL_BUTTON_LEFT && !app->select.dragging &&
        scrollbar_hit(&bar, event->x, event->y, (float)pixel_width)) {
        if (pressed)
            scrollbar_press(app, &bar, event->y);
        else
            app->bar_drag = false;
        return;
    }
    if (app->bar_drag && !pressed) {
        app->bar_drag = false;
        return;
    }

    inside = cell_at(app, event->x, event->y, &cell_x, &cell_y);

    if (pointer_belongs_to_shell(app, modifiers)) {
        /* Coordinates outside the grid are still worth sending while a
         * button is down, so the program sees the release. */
        if (!inside && !app->mouse_held && pressed)
            return;
        app->mouse_held = pressed;
        term_set_mouse_button(&app->term, pressed);
        report_mouse(app, pressed ? GHOSTTY_MOUSE_ACTION_PRESS
                                  : GHOSTTY_MOUSE_ACTION_RELEASE,
                     mouse_button_for(event->button), modifiers,
                     event->x, event->y);
        return;
    }

    if (event->button != SDL_BUTTON_LEFT)
        return;

    if (pressed) {
        if (!inside)
            return;
        app->select.dragging = true;
        app->select.moved = false;
        app->select.rectangle = (modifiers & SDL_KMOD_ALT) != 0;
        app->select.anchor_x = cell_x;
        app->select.anchor_y = cell_y;
        app->select.focus_x = cell_x;
        app->select.focus_y = cell_y;
        /* Click count decides the gesture: one click selects cells, two a
         * word, three a line. */
        if (event->clicks >= 3)
            app->select.mode = SELECT_LINES;
        else if (event->clicks == 2)
            app->select.mode = SELECT_WORDS;
        else
            app->select.mode = SELECT_CELLS;

        /* The anchor is tracked from here on: output arriving while the
         * button is held moves it with the text it named. */
        term_selection_drag_start(&app->term, cell_x, cell_y);
        if (app->select.mode == SELECT_CELLS)
            term_selection_clear(&app->term);
        else
            selection_update(app);
        app->needs_redraw = true;
        return;
    }

    term_selection_drag_end(&app->term);
    autoscroll_start(app, 0);
    if (!app->select.dragging)
        return;
    app->select.dragging = false;
    /* A click that never moved selects nothing: it only clears. */
    if (!app->select.moved && app->select.mode == SELECT_CELLS) {
        term_selection_clear(&app->term);
        app->needs_redraw = true;
        return;
    }
    /* copy_on_select puts the gesture's text on the clipboard as soon as
     * the button comes up, the way most Windows terminals do it. */
    if (app->select.moved && app->settings.copy_on_select)
        copy_selection(app);
}

static void handle_mouse_motion(App *app, const SDL_MouseMotionEvent *event)
{
    /* SDL does not carry the modifier state on motion events. */
    SDL_Keymod modifiers = mouse_modifiers();
    int cell_x = 0;
    int cell_y = 0;
    bool inside = cell_at(app, event->x, event->y, &cell_x, &cell_y);

    /* OSC 22 only says what the shape should be; the only moment it can
     * matter is when the pointer moves over the grid. */
    apply_mouse_shape(app);

    if (app->bar_drag) {
        ScrollBar bar;

        scrollbar_geometry(app, &bar);
        if (bar.visible)
            term_scroll_row(&app->term,
                            (uint32_t)scrollbar_row_at(&bar, event->y));
        app->needs_redraw = true;
        return;
    }

    if (pointer_belongs_to_shell(app, modifiers)) {
        if (inside || app->mouse_held)
            report_mouse(app, GHOSTTY_MOUSE_ACTION_MOTION,
                         GHOSTTY_MOUSE_BUTTON_UNKNOWN, modifiers,
                         event->x, event->y);
        return;
    }

    if (!app->select.dragging)
        return;

    /* A drag at the first or last row keeps pulling rows in from history,
     * so a selection longer than the screen needs no second gesture. */
    if (event->y < (float)app->grid_top)
        autoscroll_start(app, -1);
    else if (event->y >= (float)(app->grid_top +
                                 app->rows * app->glyphs.cell_height))
        autoscroll_start(app, 1);
    else
        autoscroll_start(app, 0);

    cell_clamped(app, event->x, event->y, &cell_x, &cell_y);
    if (cell_x == app->select.focus_x && cell_y == app->select.focus_y)
        return;
    app->select.moved = true;
    app->select.focus_x = cell_x;
    app->select.focus_y = cell_y;
    selection_update(app);
}

static void handle_mouse_wheel(App *app, const SDL_MouseWheelEvent *event)
{
    SDL_Keymod modifiers = mouse_modifiers();
    float ticks = event->integer_y != 0 ? (float)event->integer_y
                                        : event->y;
    intptr_t delta;

    if (ticks == 0.0f)
        return;
    if (event->direction == SDL_MOUSEWHEEL_FLIPPED)
        ticks = -ticks;
    /* Scrolling up (positive, away from the user) shows older lines, which
     * is a negative row delta. */
    delta = (ticks > 0.0f ? -1 : 1) * SCROLL_ROWS_PER_NOTCH;

    /* A program that tracks the mouse gets the wheel as button 4/5 presses
     * unless it enabled alternate scroll, in which case the encoder
     * produces nothing and the wheel scrolls the viewport. */
    if (pointer_belongs_to_shell(app, modifiers)) {
        GhosttyMouseButton button = ticks > 0.0f ? GHOSTTY_MOUSE_BUTTON_FOUR
                                                 : GHOSTTY_MOUSE_BUTTON_FIVE;
        uint8_t encoded[128];
        size_t bytes;

        bytes = term_encode_mouse(&app->term, GHOSTTY_MOUSE_ACTION_PRESS,
                                  button, ghostty_modifiers_for(modifiers),
                                  event->mouse_x, event->mouse_y,
                                  encoded, sizeof encoded);
        if (bytes > 0) {
            send_to_shell(app, encoded, bytes);
            bytes = term_encode_mouse(&app->term, GHOSTTY_MOUSE_ACTION_RELEASE,
                                      button,
                                      ghostty_modifiers_for(modifiers),
                                      event->mouse_x, event->mouse_y,
                                      encoded, sizeof encoded);
            send_to_shell(app, encoded, bytes);
            return;
        }
    }

    term_scroll_rows(&app->term, delta);
    app->needs_redraw = true;
}

/* Focus reporting (mode 1004): programs that draw a status line or watch
 * for a remote shell want to know when the window comes and goes. */
static void handle_focus(App *app, bool gained)
{
    uint8_t encoded[32];
    size_t bytes;

    if (!term_focus_reporting(&app->term))
        return;
    bytes = term_encode_focus(&app->term, gained, encoded, sizeof encoded);
    send_to_shell(app, encoded, bytes);
}

/* ------------------------------------------------------------------------- */
/* Terminal lifecycle and geometry                                            */
/* ------------------------------------------------------------------------- */

/* Tell the mouse encoder where the grid is, in window pixels. A resize
 * moves every cell, so the selection is dropped: its ends would now name
 * different text. */
static void update_geometry(App *app)
{
    int pixel_width;
    int pixel_height;
    int grid_right;
    int grid_bottom;

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    grid_right = app->grid_left + app->columns * app->glyphs.cell_width;
    grid_bottom = app->grid_top + app->rows * app->glyphs.cell_height;

    term_set_geometry(&app->term, (uint32_t)pixel_width,
                      (uint32_t)pixel_height, (uint32_t)app->grid_left,
                      (uint32_t)app->grid_top,
                      (uint32_t)(pixel_width > grid_right
                                     ? pixel_width - grid_right
                                     : 0),
                      (uint32_t)(pixel_height > grid_bottom
                                     ? pixel_height - grid_bottom
                                     : 0));
    /* The selection is left alone: the terminal owns a tracked copy of it,
     * so it keeps naming the same text through a reflow. Only the pointer
     * state is reset, because the cell under the cursor changed. */
    app->select.dragging = false;
    app->bar_drag = false;
    app->mouse_held = false;
    term_selection_drag_end(&app->term);
    term_set_mouse_button(&app->term, false);
}

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
        /* The console follows the grid, so full-screen programs see the
         * new size and reflow. */
        if (app->pty)
            pty_resize(app->pty, (uint16_t)columns, (uint16_t)rows);
    }
    /* The window region follows the window size. */
    window_sync_chrome(app->window, app->backdrop, app->panel.radius);
    update_geometry(app);
    app->needs_redraw = true;
}

/* Write an app-level message into the terminal (spawn failures, the exit
 * status). Gives the user something to read on the grid. */
static void show_message(App *app, const char *text)
{
    term_write(&app->term, text, strlen(text));
    app->needs_redraw = true;
}

/* The reader thread only announces that bytes are waiting. SDL_PushEvent is
 * thread-safe; a full queue means the main thread is already awake, so
 * retry briefly and let it drain. */
static void on_pty_output(void *userdata)
{
    SDL_Event event;
    int attempt;

    (void)userdata;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = 2;
    for (attempt = 0; attempt < 100; attempt++) {
        if (SDL_PushEvent(&event))
            return;
        SDL_Delay(1);
    }
}

/* Move pending program output into the terminal. Main thread only: the
 * parser is not thread-safe. The pty's ring is bounded (256 KiB), so this
 * cannot run away even if the program floods output. */
static void pump_shell(App *app)
{
    uint8_t buffer[16384];
    bool parsed = false;

    if (!app->pty)
        return;

    for (;;) {
        size_t got = pty_read(app->pty, buffer, sizeof buffer);

        if (got == 0)
            break;
        term_write(&app->term, buffer, got);
        parsed = true;
    }

    /* The direct child is gone. Report it once, and leave the last output
     * on screen; the window can be closed after reading it. */
    if (!app->shell_exited) {
        uint32_t code = 0;

        if (pty_exit_code(app->pty, &code)) {
            char message[128];

            app->shell_exited = true;
            SDL_snprintf(message, sizeof message,
                         "\r\n\x1b[90m[process exited with code %u]"
                         "\x1b[0m\r\n",
                         (unsigned)code);
            term_write(&app->term, message, strlen(message));
            parsed = true;
        }
    }

    if (parsed)
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

/* A thin bar on the right edge while the viewport is not at the bottom:
 * the thumb's size and position come straight from the terminal's own
 * scrollbar, so it says exactly which rows are on screen. Nothing is
 * drawn when the whole history fits. */
static void draw_scrollbar(App *app)
{
    ScrollBar bar;
    SDL_Color color = { 150, 158, 168, 190 };

    scrollbar_geometry(app, &bar);
    if (!bar.visible)
        return;

    /* The thumb brightens while it is held, so a drag has feedback even
     * when the view itself is a moment behind. */
    if (app->bar_drag)
        color.a = 255;
    fill_rectangle(app, bar.x, bar.track_top + bar.thumb_top, bar.width,
                   bar.thumb_height, color);
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
    bool cursor_valid = false;

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
            cursor_valid = true;
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
        int selection_start = -1;
        int selection_end = -1;
        GhosttyRenderStateRowSelection row_selection =
            GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);

        ghostty_render_state_row_get(app->term.row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                                     &row_position);
        ghostty_render_state_row_get(app->term.row_iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &app->term.cell_iterator);

        /* The row-local selected range, so highlighting is a comparison
         * per cell instead of one library call per cell. GHOSTTY_NO_VALUE
         * when this row is not touched by the selection. */
        if (ghostty_render_state_row_get(
                app->term.row_iterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                &row_selection) == GHOSTTY_SUCCESS) {
            selection_start = row_selection.start_x;
            selection_end = row_selection.end_x;
        }

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
    ghostty_render_state_clean(app->term.render_state);

    /* 8. The scrollback indicator, over the right edge of the grid. */
    draw_scrollbar(app);

    /* 9. Status line, one row below the grid. */
    draw_status_line(app, app->rows, cursor_column, cursor_row);

    SDL_RenderPresent(renderer);
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

/* Is this argument a point size rather than a font path? */
static bool is_point_size(const char *text, float *out)
{
    char *end = NULL;
    double value = strtod(text, &end);

    if (!end || end == text || *end != '\0' || value <= 0.0 || value > 200.0)
        return false;
    if (out)
        *out = (float)value;
    return true;
}

/* The font is optional. Order: a path on the command line, then the one in
 * the settings file, then the built-in Maple Mono NF. A build without the
 * embedded font (anything but Windows) falls back to a monospace font from
 * the system. */
static const char *pick_font(const char *requested, const char *from_settings)
{
    static const char *fallbacks[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        NULL
    };
    int index;

    if (requested)
        return requested;
    if (from_settings && from_settings[0])
        return from_settings;
    if (embedded_font_available())
        return NULL;
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
    /* Scancodes, not keycodes: a keycode carries the shifted symbol, so
     * "SDLK_C with Shift held" is a different value from "SDLK_C". The
     * bindings below hold Shift and should work on any layout.
     *
     * SDL3 reports modifiers per side (LSHIFT, LCTRL, ...), so KMOD_SHIFT
     * and KMOD_CTRL are unions of two bits. Test each union on its own:
     * comparing the combined mask for equality would ask for all four
     * bits and never match. */
    bool ctrl_shift = (event->mod & SDL_KMOD_CTRL) != 0 &&
                      (event->mod & SDL_KMOD_SHIFT) != 0;

    /* Esc belongs to the program now (vim, PSReadLine). Quit with
     * Ctrl+Shift+Q or the window's close button. */
    if (event->scancode == SDL_SCANCODE_Q && !event->repeat && ctrl_shift) {
        app->running = false;
        return;
    }
    /* Copy needs the selection to stay put, so it is handled before the
     * key reaches the shell. Plain Ctrl+C still interrupts the program. */
    if (event->scancode == SDL_SCANCODE_C && !event->repeat && ctrl_shift) {
        copy_selection(app);
        return;
    }
    /* History and panel chrome. Both live under Ctrl+Shift because that is
     * what no shell binds; Shift+Home/End stay with the shell (readline
     * line start and end), and every Alt+* now reaches it as Meta.
     * Ctrl+Alt+* is deliberately unused: on many layouts that pair is
     * AltGr and produces a character of its own. */
    if (ctrl_shift && !event->repeat) {
        switch (event->scancode) {
        case SDL_SCANCODE_UP:
            term_scroll_rows(&app->term, -1);
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_DOWN:
            term_scroll_rows(&app->term, 1);
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_HOME:
            term_scroll_top(&app->term);
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_END:
            term_scroll_bottom(&app->term);
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_MINUS:
        case SDL_SCANCODE_KP_MINUS:
            app->panel_opacity -= 0.05f;
            if (app->panel_opacity < 0.0f)
                app->panel_opacity = 0.0f;
            app->settings.panel_opacity = app->panel_opacity;
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_EQUALS:
        case SDL_SCANCODE_KP_PLUS:
            app->panel_opacity += 0.05f;
            if (app->panel_opacity > 1.0f)
                app->panel_opacity = 1.0f;
            app->settings.panel_opacity = app->panel_opacity;
            app->needs_redraw = true;
            return;
        case SDL_SCANCODE_B: {
            WindowBackdrop next =
                (WindowBackdrop)((app->backdrop + 1) % WINDOW_BACKDROP_COUNT);
            /* The OS may not know the material; the window says so and the
             * app keeps the mode that is really in effect. */
            app->backdrop = window_sync_chrome(app->window, next,
                                               app->panel.radius);
            app->settings.backdrop = (int)app->backdrop;
            app->needs_redraw = true;
            return;
        }
        default:
            break;
        }
    }
    /* Scroll a page at a time with Shift alone. */
    if ((event->mod & SDL_KMOD_SHIFT) && !(event->mod & SDL_KMOD_CTRL) &&
        !event->repeat) {
        if (event->scancode == SDL_SCANCODE_PAGEUP) {
            term_scroll_rows(&app->term, -app->rows);
            app->needs_redraw = true;
            return;
        }
        if (event->scancode == SDL_SCANCODE_PAGEDOWN) {
            term_scroll_rows(&app->term, app->rows);
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
    const char *font_label;
    const char *requested_font = NULL;
    float point_size = 0.0f;   /* 0 means "whatever the settings say" */
    int exit_code = 1;
    int index;

    /* The executable is a GUI one, so a console only exists when something
     * launched us from one. Claim it before anything is printed. */
    console_reclaim();

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return term_selftest() == 0 ? 0 : 1;

    memset(&app, 0, sizeof app);
    app.running = true;
    app.cursor_lit = true;
    settings_load(&app.settings);

    /* Command-line arguments win over the file: the file is what the app
     * remembers, the arguments are what the user asked for this once. */
    for (index = 1; index < argc; index++) {
        float size;

        if (is_point_size(argv[index], &size))
            point_size = size;
        else if (!requested_font)
            requested_font = argv[index];
    }
    if (point_size <= 0.0f)
        point_size = app.settings.point_size;
    app.panel_opacity = app.settings.panel_opacity;
    app.backdrop = (WindowBackdrop)app.settings.backdrop;

    font_path = pick_font(requested_font, app.settings.font);
    if (!font_path && !embedded_font_available()) {
        fprintf(stderr, "usage: %s [--selftest] [path\\to\\font.ttf] "
                "[point-size]\n", argv[0]);
        return 1;
    }
    font_label = font_path ? font_path : BUILT_IN_FONT;

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
        fprintf(stderr, "font: %s: %s\n", font_label, SDL_GetError());
        goto cleanup;
    }
    if (app.glyphs.built_in)
        font_label = BUILT_IN_FONT;
    printf("font: %s  cell: %dx%d px  scale: %.2f  bold: %s  italic: %s\n",
           font_label, app.glyphs.cell_width, app.glyphs.cell_height,
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
    /* Replies from the parser belong in the program's input, like every
     * other keystroke. */
    term_set_writer(&app.term, send_to_shell, &app);
    term_set_clipboard_write(&app.term, handle_clipboard_write, &app);

    window_sync_chrome(app.window, app.backdrop, app.panel.radius);
    fit_grid(&app);

    /* The grid is final now, so the console starts at the right size. */
    {
        wchar_t *command = pty_shell_for((int)app.settings.shell);
        char error[256];

        if (!command) {
            if (app.settings.shell == SETTINGS_SHELL_AUTO)
                show_message(&app,
                             "\x1b[31mno shell found: pwsh.exe, "
                             "powershell.exe and cmd.exe are all missing"
                             "\x1b[0m\r\n");
            else
                show_message(&app,
                             "\x1b[31mthe shell named in the settings is not "
                             "installed\x1b[0m\r\n");
        } else {
            app.pty = pty_spawn(command, (uint16_t)app.columns,
                                (uint16_t)app.rows, on_pty_output, &app,
                                error, sizeof error);
            free(command);
            if (!app.pty) {
                char message[400];

                SDL_snprintf(message, sizeof message,
                             "\x1b[31mcould not start a shell: %s"
                             "\x1b[0m\r\n", error);
                show_message(&app, message);
            }
        }
    }

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
                } else if (event.user.code == 3) {
                    autoscroll_tick(&app);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                handle_key_down(&app, &event.key);
                break;
            case SDL_EVENT_TEXT_INPUT:
                handle_text_input(&app, &event.text);
                break;
            case SDL_EVENT_TEXT_EDITING:
                handle_text_editing(&app, &event.edit);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                handle_mouse_motion(&app, &event.motion);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                handle_mouse_button(&app, &event.button);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                handle_mouse_button(&app, &event.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                handle_mouse_wheel(&app, &event.wheel);
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                reset_mouse_shape(&app);
                autoscroll_start(&app, 0);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                handle_focus(&app, true);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                handle_focus(&app, false);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                fit_grid(&app);
                break;
            default:
                break;
            }
        } while (app.running && SDL_PollEvent(&event));

        /* Drain the program's output: the pty event, but also the blink
         * timer, which is what notices an exit that produced no output. */
        pump_shell(&app);

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
     * middle of setup leaks nothing either. What the user changed while the
     * window was open (opacity, backdrop) is remembered for next time. */
    if (app.window)
        settings_save(&app.settings);
    if (app.blink_timer)
        SDL_RemoveTimer(app.blink_timer);
    if (app.autoscroll_timer)
        SDL_RemoveTimer(app.autoscroll_timer);
    if (app.cursor)
        SDL_DestroyCursor(app.cursor);
    pty_close(app.pty);
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
