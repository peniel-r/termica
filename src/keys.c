/*
 * termica - configurable key bindings. See keys.h.
 */
#include "keys.h"

#include <string.h>

typedef struct NamedScancode {
    const char  *name;
    SDL_Scancode scancode;
} NamedScancode;

static const NamedScancode KEY_NAMES[] = {
    { "a",            SDL_SCANCODE_A },
    { "b",            SDL_SCANCODE_B },
    { "c",            SDL_SCANCODE_C },
    { "d",            SDL_SCANCODE_D },
    { "e",            SDL_SCANCODE_E },
    { "f",            SDL_SCANCODE_F },
    { "g",            SDL_SCANCODE_G },
    { "h",            SDL_SCANCODE_H },
    { "i",            SDL_SCANCODE_I },
    { "j",            SDL_SCANCODE_J },
    { "k",            SDL_SCANCODE_K },
    { "l",            SDL_SCANCODE_L },
    { "m",            SDL_SCANCODE_M },
    { "n",            SDL_SCANCODE_N },
    { "o",            SDL_SCANCODE_O },
    { "p",            SDL_SCANCODE_P },
    { "q",            SDL_SCANCODE_Q },
    { "r",            SDL_SCANCODE_R },
    { "s",            SDL_SCANCODE_S },
    { "t",            SDL_SCANCODE_T },
    { "u",            SDL_SCANCODE_U },
    { "v",            SDL_SCANCODE_V },
    { "w",            SDL_SCANCODE_W },
    { "x",            SDL_SCANCODE_X },
    { "y",            SDL_SCANCODE_Y },
    { "z",            SDL_SCANCODE_Z },
    { "0",            SDL_SCANCODE_0 },
    { "1",            SDL_SCANCODE_1 },
    { "2",            SDL_SCANCODE_2 },
    { "3",            SDL_SCANCODE_3 },
    { "4",            SDL_SCANCODE_4 },
    { "5",            SDL_SCANCODE_5 },
    { "6",            SDL_SCANCODE_6 },
    { "7",            SDL_SCANCODE_7 },
    { "8",            SDL_SCANCODE_8 },
    { "9",            SDL_SCANCODE_9 },
    { "f1",           SDL_SCANCODE_F1 },
    { "f2",           SDL_SCANCODE_F2 },
    { "f3",           SDL_SCANCODE_F3 },
    { "f4",           SDL_SCANCODE_F4 },
    { "f5",           SDL_SCANCODE_F5 },
    { "f6",           SDL_SCANCODE_F6 },
    { "f7",           SDL_SCANCODE_F7 },
    { "f8",           SDL_SCANCODE_F8 },
    { "f9",           SDL_SCANCODE_F9 },
    { "f10",          SDL_SCANCODE_F10 },
    { "f11",          SDL_SCANCODE_F11 },
    { "f12",          SDL_SCANCODE_F12 },
    { "up",           SDL_SCANCODE_UP },
    { "down",         SDL_SCANCODE_DOWN },
    { "left",         SDL_SCANCODE_LEFT },
    { "right",        SDL_SCANCODE_RIGHT },
    { "home",         SDL_SCANCODE_HOME },
    { "end",          SDL_SCANCODE_END },
    { "pageup",       SDL_SCANCODE_PAGEUP },
    { "pgup",         SDL_SCANCODE_PAGEUP },
    { "pagedown",     SDL_SCANCODE_PAGEDOWN },
    { "pgdn",         SDL_SCANCODE_PAGEDOWN },
    { "insert",       SDL_SCANCODE_INSERT },
    { "delete",       SDL_SCANCODE_DELETE },
    { "del",          SDL_SCANCODE_DELETE },
    { "backspace",    SDL_SCANCODE_BACKSPACE },
    { "tab",          SDL_SCANCODE_TAB },
    { "enter",        SDL_SCANCODE_RETURN },
    { "return",       SDL_SCANCODE_RETURN },
    { "escape",       SDL_SCANCODE_ESCAPE },
    { "esc",          SDL_SCANCODE_ESCAPE },
    { "space",        SDL_SCANCODE_SPACE },
    { "minus",        SDL_SCANCODE_MINUS },
    { "-",            SDL_SCANCODE_MINUS },
    { "equals",       SDL_SCANCODE_EQUALS },
    { "equal",        SDL_SCANCODE_EQUALS },
    { "=",            SDL_SCANCODE_EQUALS },
    { "plus",         SDL_SCANCODE_EQUALS },
    { "+",            SDL_SCANCODE_EQUALS },
    { "kpplus",       SDL_SCANCODE_KP_PLUS },
    { "kpminus",      SDL_SCANCODE_KP_MINUS },
    { "kp0",          SDL_SCANCODE_KP_0 },
    { "kp1",          SDL_SCANCODE_KP_1 },
    { "kp2",          SDL_SCANCODE_KP_2 },
    { "kp3",          SDL_SCANCODE_KP_3 },
    { "kp4",          SDL_SCANCODE_KP_4 },
    { "kp5",          SDL_SCANCODE_KP_5 },
    { "kp6",          SDL_SCANCODE_KP_6 },
    { "kp7",          SDL_SCANCODE_KP_7 },
    { "kp8",          SDL_SCANCODE_KP_8 },
    { "kp9",          SDL_SCANCODE_KP_9 },
    { "leftbracket",  SDL_SCANCODE_LEFTBRACKET },
    { "[",            SDL_SCANCODE_LEFTBRACKET },
    { "rightbracket", SDL_SCANCODE_RIGHTBRACKET },
    { "]",            SDL_SCANCODE_RIGHTBRACKET },
    { "backslash",    SDL_SCANCODE_BACKSLASH },
    { "semicolon",    SDL_SCANCODE_SEMICOLON },
    { "apostrophe",   SDL_SCANCODE_APOSTROPHE },
    { "grave",        SDL_SCANCODE_GRAVE },
    { "comma",        SDL_SCANCODE_COMMA },
    { "period",       SDL_SCANCODE_PERIOD },
    { "slash",        SDL_SCANCODE_SLASH },
};

typedef struct NamedAction {
    const char *name;
    KeyAction   action;
} NamedAction;

static const NamedAction ACTION_NAMES[] = {
    { "none",             KEY_ACTION_NONE },
    { "copy",             KEY_ACTION_COPY },
    { "paste",            KEY_ACTION_PASTE },
    { "scroll_up",        KEY_ACTION_SCROLL_UP },
    { "scroll_down",      KEY_ACTION_SCROLL_DOWN },
    { "scroll_page_up",   KEY_ACTION_SCROLL_PAGE_UP },
    { "scroll_page_down", KEY_ACTION_SCROLL_PAGE_DOWN },
    { "scroll_top",       KEY_ACTION_SCROLL_TOP },
    { "scroll_bottom",    KEY_ACTION_SCROLL_BOTTOM },
    { "opacity_up",       KEY_ACTION_OPACITY_UP },
    { "opacity_down",     KEY_ACTION_OPACITY_DOWN },
    { "cycle_backdrop",   KEY_ACTION_CYCLE_BACKDROP },
    { "font_size_up",     KEY_ACTION_FONT_SIZE_UP },
    { "font_size_down",   KEY_ACTION_FONT_SIZE_DOWN },
    { "font_size_reset",  KEY_ACTION_FONT_SIZE_RESET },
    { "new_tab",          KEY_ACTION_NEW_TAB },
    { "close_tab",        KEY_ACTION_CLOSE_TAB },
    { "next_tab",         KEY_ACTION_NEXT_TAB },
    { "prev_tab",         KEY_ACTION_PREV_TAB },
    { "quit",             KEY_ACTION_QUIT },
};

static void fold_token(const char *text, size_t length, char *out,
                       size_t capacity)
{
    size_t read = 0;
    size_t written = 0;

    if (capacity == 0)
        return;
    for (; read < length && written + 1 < capacity; read++) {
        char c = text[read];

        if (c == ' ' || c == '\t')
            continue;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        /* Keep a lone '-' intact so "Ctrl+-" parses as minus; strip '_' and
         * '-' inside multi-character names so "page_up" and "page-up" match. */
        if (length > 1 && (c == '_' || c == '-'))
            continue;
        out[written++] = c;
    }
    out[written] = '\0';
}

bool keys_parse_action(const char *text, KeyAction *action)
{
    char folded[64];
    size_t index;

    if (!text || !action)
        return false;
    fold_token(text, strlen(text), folded, sizeof folded);
    for (index = 0; index < sizeof ACTION_NAMES / sizeof ACTION_NAMES[0];
         index++) {
        char candidate[64];

        fold_token(ACTION_NAMES[index].name, strlen(ACTION_NAMES[index].name),
                   candidate, sizeof candidate);
        if (strcmp(folded, candidate) == 0) {
            *action = ACTION_NAMES[index].action;
            return true;
        }
    }
    return false;
}

const char *keys_action_name(KeyAction action)
{
    size_t index;

    for (index = 0; index < sizeof ACTION_NAMES / sizeof ACTION_NAMES[0];
         index++) {
        if (ACTION_NAMES[index].action == action)
            return ACTION_NAMES[index].name;
    }
    return "none";
}

static bool parse_modifier(const char *folded, uint8_t *mods)
{
    if (strcmp(folded, "ctrl") == 0 || strcmp(folded, "control") == 0) {
        *mods |= KEY_MOD_CTRL;
        return true;
    }
    if (strcmp(folded, "shift") == 0) {
        *mods |= KEY_MOD_SHIFT;
        return true;
    }
    if (strcmp(folded, "alt") == 0) {
        *mods |= KEY_MOD_ALT;
        return true;
    }
    if (strcmp(folded, "super") == 0 || strcmp(folded, "gui") == 0 ||
        strcmp(folded, "win") == 0 || strcmp(folded, "meta") == 0) {
        *mods |= KEY_MOD_SUPER;
        return true;
    }
    return false;
}

static bool parse_scancode(const char *folded, SDL_Scancode *scancode)
{
    size_t index;

    for (index = 0; index < sizeof KEY_NAMES / sizeof KEY_NAMES[0]; index++) {
        if (strcmp(folded, KEY_NAMES[index].name) == 0) {
            *scancode = KEY_NAMES[index].scancode;
            return true;
        }
    }
    return false;
}

bool keys_parse_chord(const char *text, SDL_Scancode *scancode, uint8_t *mods)
{
    const char *p = text;
    uint8_t parsed_mods = 0;
    SDL_Scancode parsed_key = SDL_SCANCODE_UNKNOWN;
    bool have_key = false;

    if (!text || !text[0] || !scancode || !mods)
        return false;

    while (*p) {
        const char *start;
        const char *end;
        char folded[32];

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        /* A trailing '+' after a separator (e.g. "Ctrl++") names the plus
         * key itself rather than an empty token. */
        if (*p == '+' && p[1] == '\0') {
            start = p;
            end = p + 1;
            p = end;
        } else {
            start = p;
            end = strchr(p, '+');
            if (end) {
                p = end + 1;
            } else {
                end = start + strlen(start);
                p = end;
            }
        }

        fold_token(start, (size_t)(end - start), folded, sizeof folded);
        if (!folded[0])
            return false;

        if (*p != '\0') {
            /* Everything before the final token must be a modifier. */
            if (!parse_modifier(folded, &parsed_mods))
                return false;
        } else {
            if (!parse_scancode(folded, &parsed_key))
                return false;
            have_key = true;
        }
    }

    if (!have_key)
        return false;
    *scancode = parsed_key;
    *mods = parsed_mods;
    return true;
}

bool keys_set(KeyMap *map, SDL_Scancode scancode, uint8_t mods,
              KeyAction action)
{
    int index;

    if (!map || scancode == SDL_SCANCODE_UNKNOWN)
        return false;

    for (index = 0; index < map->count; index++) {
        if (map->entries[index].scancode == scancode &&
            map->entries[index].mods == mods) {
            map->entries[index].action = action;
            return true;
        }
    }
    if (map->count >= KEY_BINDINGS_MAX)
        return false;
    map->entries[map->count].scancode = scancode;
    map->entries[map->count].mods = mods;
    map->entries[map->count].action = action;
    map->count++;
    return true;
}

static uint8_t mods_from_sdl(SDL_Keymod sdl_mods)
{
    uint8_t mods = 0;

    if (sdl_mods & SDL_KMOD_CTRL)
        mods |= KEY_MOD_CTRL;
    if (sdl_mods & SDL_KMOD_SHIFT)
        mods |= KEY_MOD_SHIFT;
    if (sdl_mods & SDL_KMOD_ALT)
        mods |= KEY_MOD_ALT;
    if (sdl_mods & SDL_KMOD_GUI)
        mods |= KEY_MOD_SUPER;
    return mods;
}

KeyAction keys_lookup(const KeyMap *map, SDL_Scancode scancode,
                      SDL_Keymod sdl_mods)
{
    uint8_t mods = mods_from_sdl(sdl_mods);
    int index;

    if (!map)
        return KEY_ACTION_NONE;
    for (index = 0; index < map->count; index++) {
        if (map->entries[index].scancode == scancode &&
            map->entries[index].mods == mods)
            return map->entries[index].action;
    }
    return KEY_ACTION_NONE;
}

void keys_default(KeyMap *map)
{
    const uint8_t cs = KEY_MOD_CTRL | KEY_MOD_SHIFT;

    memset(map, 0, sizeof *map);
    keys_set(map, SDL_SCANCODE_Q,        cs,            KEY_ACTION_QUIT);
    keys_set(map, SDL_SCANCODE_C,        cs,            KEY_ACTION_COPY);
    /* Default paste is Ctrl+Shift+V only so Ctrl+V reaches vim and readline. */
    keys_set(map, SDL_SCANCODE_V,        cs,            KEY_ACTION_PASTE);
    keys_set(map, SDL_SCANCODE_UP,       cs,            KEY_ACTION_SCROLL_UP);
    keys_set(map, SDL_SCANCODE_DOWN,     cs,            KEY_ACTION_SCROLL_DOWN);
    keys_set(map, SDL_SCANCODE_HOME,     cs,            KEY_ACTION_SCROLL_TOP);
    keys_set(map, SDL_SCANCODE_END,      cs,            KEY_ACTION_SCROLL_BOTTOM);
    keys_set(map, SDL_SCANCODE_PAGEUP,   KEY_MOD_SHIFT, KEY_ACTION_SCROLL_PAGE_UP);
    keys_set(map, SDL_SCANCODE_PAGEDOWN, KEY_MOD_SHIFT, KEY_ACTION_SCROLL_PAGE_DOWN);
    keys_set(map, SDL_SCANCODE_MINUS,    cs,            KEY_ACTION_OPACITY_DOWN);
    keys_set(map, SDL_SCANCODE_KP_MINUS, cs,            KEY_ACTION_OPACITY_DOWN);
    keys_set(map, SDL_SCANCODE_EQUALS,   cs,            KEY_ACTION_OPACITY_UP);
    keys_set(map, SDL_SCANCODE_KP_PLUS,  cs,            KEY_ACTION_OPACITY_UP);
    keys_set(map, SDL_SCANCODE_B,        cs,            KEY_ACTION_CYCLE_BACKDROP);
    keys_set(map, SDL_SCANCODE_EQUALS,   KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_UP);
    keys_set(map, SDL_SCANCODE_KP_PLUS,  KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_UP);
    keys_set(map, SDL_SCANCODE_MINUS,    KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_DOWN);
    keys_set(map, SDL_SCANCODE_KP_MINUS, KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_DOWN);
    keys_set(map, SDL_SCANCODE_0,        KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_RESET);
    keys_set(map, SDL_SCANCODE_KP_0,     KEY_MOD_CTRL,  KEY_ACTION_FONT_SIZE_RESET);
    keys_set(map, SDL_SCANCODE_T,        cs,            KEY_ACTION_NEW_TAB);
    keys_set(map, SDL_SCANCODE_W,        cs,            KEY_ACTION_CLOSE_TAB);
    keys_set(map, SDL_SCANCODE_TAB,      KEY_MOD_CTRL,  KEY_ACTION_NEXT_TAB);
    keys_set(map, SDL_SCANCODE_TAB,      cs,            KEY_ACTION_PREV_TAB);
}
