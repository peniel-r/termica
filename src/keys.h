/*
 * termica - configurable key bindings (scancode + modifier chords -> actions).
 *
 * Bindings match physical scancodes and canonical modifier bits (Ctrl, Shift,
 * Alt, Super) so Shift+letter chords work on any keyboard layout and SDL's
 * per-side modifier masks are normalised in one place.
 *
 * Strict C99.
 */
#ifndef TERMICA_KEYS_H
#define TERMICA_KEYS_H

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Canonical modifier flags (side-independent). */
#define KEY_MOD_CTRL   0x01u
#define KEY_MOD_SHIFT  0x02u
#define KEY_MOD_ALT    0x04u
#define KEY_MOD_SUPER  0x08u

#define KEY_BINDINGS_MAX 64

typedef enum KeyAction {
    KEY_ACTION_NONE = 0,
    KEY_ACTION_COPY,
    KEY_ACTION_PASTE,
    KEY_ACTION_SCROLL_UP,
    KEY_ACTION_SCROLL_DOWN,
    KEY_ACTION_SCROLL_PAGE_UP,
    KEY_ACTION_SCROLL_PAGE_DOWN,
    KEY_ACTION_SCROLL_TOP,
    KEY_ACTION_SCROLL_BOTTOM,
    KEY_ACTION_OPACITY_UP,
    KEY_ACTION_OPACITY_DOWN,
    KEY_ACTION_CYCLE_BACKDROP,
    KEY_ACTION_FONT_SIZE_UP,
    KEY_ACTION_FONT_SIZE_DOWN,
    KEY_ACTION_FONT_SIZE_RESET,
    KEY_ACTION_QUIT
} KeyAction;

typedef struct KeyBinding {
    SDL_Scancode scancode;
    uint8_t      mods;
    KeyAction    action;
} KeyBinding;

typedef struct KeyMap {
    KeyBinding entries[KEY_BINDINGS_MAX];
    int        count;
} KeyMap;

/* Populate `map` with the built-in default bindings. */
void keys_default(KeyMap *map);

/* Parse a chord string such as "Ctrl+Shift+V" or "Ctrl+0" into a scancode
 * and canonical modifier bitmask. Returns false on unknown tokens. */
bool keys_parse_chord(const char *text, SDL_Scancode *scancode, uint8_t *mods);

/* Parse an action name such as "paste", "font_size_up" or "none" (case and
 * separator folded). Returns false on an unknown name. */
bool keys_parse_action(const char *text, KeyAction *action);

/* Canonical action name for formatting, or "none". */
const char *keys_action_name(KeyAction action);

/* Install or replace a binding for (scancode, mods). Setting KEY_ACTION_NONE
 * unbinds that chord. Returns false if the table is full. */
bool keys_set(KeyMap *map, SDL_Scancode scancode, uint8_t mods,
              KeyAction action);

/* Look up the action bound to (scancode, sdl_mods), or KEY_ACTION_NONE. */
KeyAction keys_lookup(const KeyMap *map, SDL_Scancode scancode,
                      SDL_Keymod sdl_mods);

#endif /* TERMICA_KEYS_H */
