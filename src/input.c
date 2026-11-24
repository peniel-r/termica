/*
 * termica - keyboard, text/IME, pointer, focus and clipboard-policy routing.
 * See input.h.
 */
#include "input.h"

#include "chrome.h"

#include <stdio.h>

/* Rows moved by one wheel notch. */
#define SCROLL_ROWS_PER_NOTCH 3

/* Rows moved by one autoscroll tick, and how fast those ticks come. */
#define AUTOSCROLL_ROWS 1
#define AUTOSCROLL_INTERVAL_MS 40

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
TermClipboardResult input_handle_clipboard_write(void *userdata,
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

/* Encode one key event and write it to the program. The bytes are exactly
 * what the terminal modes ask for; the program echoes them back. */
static void write_key(App *app, const SDL_KeyboardEvent *event)
{
    Session *s = app_active_session(app);
    uint8_t encoded[64];
    size_t byte_count;
    GhosttyKey key = ghostty_key_for(event->scancode);
    SDL_Keycode unshifted;

    if (!s || key == GHOSTTY_KEY_UNIDENTIFIED)
        return;

    /* The unshifted codepoint (layout dependent, e.g. 'a' for the A key)
     * is what makes Ctrl+letter encode as 0x01-0x1a. */
    unshifted = SDL_GetKeyFromScancode(event->scancode, 0, false);
    if (unshifted & SDLK_SCANCODE_MASK)
        unshifted = 0;

    byte_count = term_encode_key(&s->term,
                                 event->repeat ? GHOSTTY_KEY_ACTION_REPEAT
                                               : GHOSTTY_KEY_ACTION_PRESS,
                                 key, ghostty_modifiers_for(event->mod),
                                 (uint32_t)unshifted, encoded, sizeof encoded);
    if (byte_count == 0)
        return;
    session_jump_to_bottom(s);
    session_send(s, encoded, byte_count);
    /* Some shell integrations report B and D but omit C. Record the last
     * unmodified Enter at the prompt as an approximate start; a real C
     * supersedes it. Do not treat Enter inside a running program as a start. */
    if (!event->repeat && (key == GHOSTTY_KEY_ENTER ||
                           key == GHOSTTY_KEY_NUMPAD_ENTER) &&
        !(event->mod & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT |
                        SDL_KMOD_GUI)) &&
        s->term.prompt_input_active && !s->term.command_running) {
        s->term.command_started_at = SDL_GetTicks();
        s->term.command_start_pending = true;
    }
}

void input_handle_key_down(App *app, const SDL_KeyboardEvent *event)
{
    Session *s = app_active_session(app);
    KeyAction action = keys_lookup(&app->settings.keys, event->scancode,
                                   event->mod);

    if (!s)
        return;
    switch (action) {
    case KEY_ACTION_QUIT:
        if (!event->repeat)
            app->running = false;
        return;
    case KEY_ACTION_NEW_TAB:
        if (!event->repeat)
            app_new_tab(app);
        return;
    case KEY_ACTION_CLOSE_TAB:
        if (!event->repeat)
            app_close_tab(app, app->active_session);
        return;
    case KEY_ACTION_NEXT_TAB:
        app_next_tab(app, 1);
        return;
    case KEY_ACTION_PREV_TAB:
        app_next_tab(app, -1);
        return;
    case KEY_ACTION_COPY:
        if (!event->repeat)
            session_copy_selection(s);
        return;
    case KEY_ACTION_PASTE:
        if (!event->repeat && session_paste(s, app->window))
            app->needs_redraw = true;
        return;
    case KEY_ACTION_SCROLL_UP:
        term_scroll_rows(&s->term, -1);
        app->needs_redraw = true;
        return;
    case KEY_ACTION_SCROLL_DOWN:
        term_scroll_rows(&s->term, 1);
        app->needs_redraw = true;
        return;
    case KEY_ACTION_SCROLL_PAGE_UP:
        term_scroll_rows(&s->term, -app->rows);
        app->needs_redraw = true;
        return;
    case KEY_ACTION_SCROLL_PAGE_DOWN:
        term_scroll_rows(&s->term, app->rows);
        app->needs_redraw = true;
        return;
    case KEY_ACTION_SCROLL_TOP:
        if (!event->repeat) {
            term_scroll_top(&s->term);
            app->needs_redraw = true;
        }
        return;
    case KEY_ACTION_SCROLL_BOTTOM:
        if (!event->repeat) {
            term_scroll_bottom(&s->term);
            app->needs_redraw = true;
        }
        return;
    case KEY_ACTION_OPACITY_DOWN:
        app->panel_opacity -= 0.05f;
        if (app->panel_opacity < 0.0f)
            app->panel_opacity = 0.0f;
        app->settings.panel_opacity = app->panel_opacity;
        app->needs_redraw = true;
        return;
    case KEY_ACTION_OPACITY_UP:
        app->panel_opacity += 0.05f;
        if (app->panel_opacity > 1.0f)
            app->panel_opacity = 1.0f;
        app->settings.panel_opacity = app->panel_opacity;
        app->needs_redraw = true;
        return;
    case KEY_ACTION_CYCLE_BACKDROP:
        if (!event->repeat) {
            WindowBackdrop next =
                (WindowBackdrop)((app->backdrop + 1) % WINDOW_BACKDROP_COUNT);
            /* The OS may not know the material; the window says so and the
             * app keeps the mode that is really in effect. */
            app->backdrop = window_sync_chrome(app->window, next,
                                               app->decoration,
                                               chrome_corner_radius(app));
            app->settings.backdrop = (int)app->backdrop;
            app->needs_redraw = true;
        }
        return;
    case KEY_ACTION_FONT_SIZE_UP:
        app_set_font_size(app, app->current_point_size + 1.0f);
        return;
    case KEY_ACTION_FONT_SIZE_DOWN:
        app_set_font_size(app, app->current_point_size - 1.0f);
        return;
    case KEY_ACTION_FONT_SIZE_RESET:
        if (!event->repeat)
            app_set_font_size(app, app->base_point_size);
        return;
    case KEY_ACTION_NONE:
    default:
        break;
    }
    write_key(app, event);
}

/* Layout text (letters, digits, punctuation, IME output). SDL delivers it
 * separately from key events; feed it straight through. */
void input_handle_text_input(App *app, const SDL_TextInputEvent *event)
{
    Session *s = app_active_session(app);

    if (!s)
        return;
    session_send_text(s, event->text);
    /* A commit ends the composition. */
    session_clear_composing(s);
}

/* Uncommitted IME text. Empty text means the composition was cancelled, so
 * the session forgets it; anything else is kept for the frame and drawn at
 * the cursor until the commit event above clears it. */
void input_handle_text_editing(App *app, const SDL_TextEditingEvent *event)
{
    Session *s = app_active_session(app);

    if (!s)
        return;
    session_set_composing(s, event->text);
    app->needs_redraw = true;
}

/* ------------------------------------------------------------------------- */
/* Pointer, mouse shape, autoscroll and focus                                 */
/* ------------------------------------------------------------------------- */

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
static bool pointer_belongs_to_shell(Session *s, SDL_Keymod modifiers)
{
    if (modifiers & SDL_KMOD_SHIFT)
        return false;
    return term_mouse_tracking(&s->term);
}

static void report_mouse(Session *s, GhosttyMouseAction action,
                         GhosttyMouseButton button, SDL_Keymod modifiers,
                         float x, float y)
{
    uint8_t encoded[128];
    size_t bytes;

    bytes = term_encode_mouse(&s->term, action, button,
                              ghostty_modifiers_for(modifiers), x, y,
                              encoded, sizeof encoded);
    session_send(s, encoded, bytes);
}

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
    Session *s = app_active_session(app);
    GhosttyMouseShape shape;
    SDL_Cursor *cursor;

    if (!s || !term_mouse_shape(&s->term, &shape))
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
void input_reset_mouse_shape(App *app)
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
    event.user.code = APP_EVENT_AUTOSCROLL;
    SDL_PushEvent(&event);
    return interval;
}

void input_autoscroll_start(App *app, int direction)
{
    Session *s = app_active_session(app);

    if (!s)
        direction = 0;
    if (s && s->autoscroll == direction && app->autoscroll_timer)
        return;
    if (s)
        s->autoscroll = direction;
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

void input_autoscroll_tick(App *app)
{
    Session *s = app_active_session(app);

    if (!s || s->autoscroll == 0 || !s->select.dragging)
        return;
    term_scroll_rows(&s->term, s->autoscroll > 0 ? AUTOSCROLL_ROWS
                                                 : -AUTOSCROLL_ROWS);
    /* The focus end is the edge row that is now under the pointer. */
    s->select.focus_y = s->autoscroll > 0 ? app->rows - 1 : 0;
    s->select.moved = true;
    session_selection_update(s);
    app->needs_redraw = true;
}

void input_handle_mouse_button(App *app, const SDL_MouseButtonEvent *event)
{
    Session *s = app_active_session(app);
    bool pressed = event->down;
    SDL_Keymod modifiers = mouse_modifiers();
    ScrollBar bar;
    int pixel_width = 0;
    int pixel_height = 0;
    int cx = 0;
    int cy = 0;
    int title_button;
    int tab_index = -1;
    bool tab_close = false;
    bool tab_new = false;
    bool inside;

    if (!s)
        return;

    /* The title bar is above the grid and owns its own clicks. */
    if (event->button == SDL_BUTTON_LEFT &&
        chrome_title_button_at(app, event->x, event->y, &title_button)) {
        if (pressed)
            chrome_title_button_action(app, title_button);
        return;
    }

    /* Tab strip clicks (switch, close, or '+' new tab). */
    if ((event->button == SDL_BUTTON_LEFT ||
         event->button == SDL_BUTTON_MIDDLE) &&
        chrome_tab_hit(app, event->x, event->y, &tab_index, &tab_close,
                       &tab_new)) {
        if (pressed) {
            if (tab_new && event->button == SDL_BUTTON_LEFT)
                app_new_tab(app);
            else if (tab_index >= 0) {
                if (tab_close || event->button == SDL_BUTTON_MIDDLE)
                    app_close_tab(app, tab_index);
                else
                    app_switch_tab(app, tab_index);
            }
        }
        return;
    }

    /* The bar comes first: it is outside the grid, so a program that owns
     * the pointer still cannot swallow the only control that reaches the
     * top of the scrollback without the keyboard. */
    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    chrome_scrollbar_geometry(app, &bar);
    if (event->button == SDL_BUTTON_LEFT && !s->select.dragging &&
        chrome_scrollbar_hit(&bar, event->x, event->y, (float)pixel_width)) {
        if (pressed)
            chrome_scrollbar_press(app, &bar, event->y);
        else
            s->bar_drag = false;
        return;
    }
    if (s->bar_drag && !pressed) {
        s->bar_drag = false;
        return;
    }

    inside = cell_at(app, event->x, event->y, &cx, &cy);

    if (pointer_belongs_to_shell(s, modifiers)) {
        /* Coordinates outside the grid are still worth sending while a
         * button is down, so the program sees the release. */
        if (!inside && !s->mouse_held && pressed)
            return;
        s->mouse_held = pressed;
        term_set_mouse_button(&s->term, pressed);
        report_mouse(s, pressed ? GHOSTTY_MOUSE_ACTION_PRESS
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
        s->select.dragging = true;
        s->select.moved = false;
        s->select.rectangle = (modifiers & SDL_KMOD_ALT) != 0;
        s->select.anchor_x = cx;
        s->select.anchor_y = cy;
        s->select.focus_x = cx;
        s->select.focus_y = cy;
        /* Click count decides the gesture: one click selects cells, two a
         * word, three a line. */
        if (event->clicks >= 3)
            s->select.mode = SELECT_LINES;
        else if (event->clicks == 2)
            s->select.mode = SELECT_WORDS;
        else
            s->select.mode = SELECT_CELLS;

        /* The anchor is tracked from here on: output arriving while the
         * button is held moves it with the text it named. */
        term_selection_drag_start(&s->term, cx, cy);
        if (s->select.mode == SELECT_CELLS)
            term_selection_clear(&s->term);
        else
            session_selection_update(s);
        app->needs_redraw = true;
        return;
    }

    term_selection_drag_end(&s->term);
    input_autoscroll_start(app, 0);
    if (!s->select.dragging)
        return;
    s->select.dragging = false;
    /* A click that never moved selects nothing: it only clears. */
    if (!s->select.moved && s->select.mode == SELECT_CELLS) {
        term_selection_clear(&s->term);
        app->needs_redraw = true;
        return;
    }
    /* copy_on_select puts the gesture's text on the clipboard as soon as
     * the button comes up, the way most Windows terminals do it. */
    if (s->select.moved && app->settings.copy_on_select)
        session_copy_selection(s);
}

void input_handle_mouse_motion(App *app, const SDL_MouseMotionEvent *event)
{
    Session *s = app_active_session(app);
    /* SDL does not carry the modifier state on motion events. */
    SDL_Keymod modifiers = mouse_modifiers();
    int cx = 0;
    int cy = 0;
    bool inside;

    if (!s)
        return;
    inside = cell_at(app, event->x, event->y, &cx, &cy);

    chrome_update_title_hover(app, event->x, event->y);
    chrome_update_tab_hover(app, event->x, event->y);

    /* OSC 22 only says what the shape should be; the only moment it can
     * matter is when the pointer moves over the grid. */
    apply_mouse_shape(app);

    if (s->bar_drag) {
        ScrollBar bar;

        chrome_scrollbar_geometry(app, &bar);
        if (bar.visible)
            term_scroll_row(&s->term,
                            (uint32_t)chrome_scrollbar_row_at(&bar, event->y));
        app->needs_redraw = true;
        return;
    }

    if (pointer_belongs_to_shell(s, modifiers)) {
        if (inside || s->mouse_held)
            report_mouse(s, GHOSTTY_MOUSE_ACTION_MOTION,
                         GHOSTTY_MOUSE_BUTTON_UNKNOWN, modifiers,
                         event->x, event->y);
        return;
    }

    if (!s->select.dragging)
        return;

    /* A drag at the first or last row keeps pulling rows in from history,
     * so a selection longer than the screen needs no second gesture. */
    if (event->y < (float)app->grid_top)
        input_autoscroll_start(app, -1);
    else if (event->y >= (float)(app->grid_top +
                                 app->rows * app->glyphs.cell_height))
        input_autoscroll_start(app, 1);
    else
        input_autoscroll_start(app, 0);

    cell_clamped(app, event->x, event->y, &cx, &cy);
    if (cx == s->select.focus_x && cy == s->select.focus_y)
        return;
    s->select.moved = true;
    s->select.focus_x = cx;
    s->select.focus_y = cy;
    session_selection_update(s);
    app->needs_redraw = true;
}

void input_handle_mouse_wheel(App *app, const SDL_MouseWheelEvent *event)
{
    Session *s = app_active_session(app);
    SDL_Keymod modifiers = mouse_modifiers();
    float ticks = event->integer_y != 0 ? (float)event->integer_y
                                        : event->y;
    intptr_t delta;

    if (!s || ticks == 0.0f)
        return;
    if (event->direction == SDL_MOUSEWHEEL_FLIPPED)
        ticks = -ticks;
    /* Scrolling up (positive, away from the user) shows older lines, which
     * is a negative row delta. */
    delta = (ticks > 0.0f ? -1 : 1) * SCROLL_ROWS_PER_NOTCH;

    /* A program that tracks the mouse gets the wheel as button 4/5 presses
     * unless it enabled alternate scroll, in which case the encoder
     * produces nothing and the wheel scrolls the viewport. */
    if (pointer_belongs_to_shell(s, modifiers)) {
        GhosttyMouseButton button = ticks > 0.0f ? GHOSTTY_MOUSE_BUTTON_FOUR
                                                 : GHOSTTY_MOUSE_BUTTON_FIVE;
        uint8_t encoded[128];
        size_t bytes;

        bytes = term_encode_mouse(&s->term, GHOSTTY_MOUSE_ACTION_PRESS,
                                  button, ghostty_modifiers_for(modifiers),
                                  event->mouse_x, event->mouse_y,
                                  encoded, sizeof encoded);
        if (bytes > 0) {
            session_send(s, encoded, bytes);
            bytes = term_encode_mouse(&s->term, GHOSTTY_MOUSE_ACTION_RELEASE,
                                      button,
                                      ghostty_modifiers_for(modifiers),
                                      event->mouse_x, event->mouse_y,
                                      encoded, sizeof encoded);
            session_send(s, encoded, bytes);
            return;
        }
    }

    term_scroll_rows(&s->term, delta);
    app->needs_redraw = true;
}

/* Focus reporting (mode 1004): programs that draw a status line or watch
 * for a remote shell want to know when the window comes and goes. */
void input_handle_focus(App *app, bool gained)
{
    Session *s = app_active_session(app);
    uint8_t encoded[32];
    size_t bytes;

    if (!s || !term_focus_reporting(&s->term))
        return;
    bytes = term_encode_focus(&s->term, gained, encoded, sizeof encoded);
    session_send(s, encoded, bytes);
}
