/*
 * termica - the user's settings. See settings.h.
 *
 * The file is TOML, parsed by tomlc17:
 *
 *   [font]
 *   face = ""
 *   size = 11.0
 *
 *   [window]
 *   opacity = 1.0
 *   backdrop = "mica"
 *   decoration = "native"
 *   title_bar = true
 *
 *   [terminal]
 *   copy_on_select = false
 *   shell = "auto"
 *   osc52 = "deny"
 *   notifications = "deny"
 *   scrollback_lines = 10000
 *
 * The legacy overlay parser ignores individual unknown or invalid values.
 * Live reload validates recognized values before committing the new state.
 */

#include "settings.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include "tomlc17.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_APP_DIR "termica"
#define SETTINGS_FILE_NAME "termica.toml"

/* Longest file we are willing to read; the real one is a dozen lines. */
#define SETTINGS_MAX_BYTES 65536


void settings_default(Settings *settings)
{
    memset(settings, 0, sizeof *settings);
    settings->point_size = 11.0f;
    settings->ligatures = true;
    settings->panel_opacity = 1.0f;
    settings->backdrop = 1;             /* WINDOW_BACKDROP_MICA        */
    settings->decoration = SETTINGS_DECORATION_NATIVE;
    settings->title_bar = true;
    settings->copy_on_select = false;
    settings->shell = SETTINGS_SHELL_AUTO;
    settings->osc52 = SETTINGS_OSC52_DENY;
    settings->notifications = SETTINGS_OSC52_DENY;
    settings->scrollback_lines = SETTINGS_SCROLLBACK_DEFAULT;
    settings->bell = SETTINGS_BELL_VISUAL;
    colors_preset(&settings->colors, "dark");
    keys_default(&settings->keys);
}

/* `-` and `_` are the same separator, and case does not matter:
 * CopyOnSelect, copy-on-select and copy_on_select all name one key. */
static void fold_key(const char *text, char *out, size_t capacity)
{
    size_t read = 0;
    size_t written = 0;

    if (capacity == 0)
        return;
    for (; text[read] && written + 1 < capacity; read++) {
        char c = text[read];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c == '-' || c == '_')
            continue;
        out[written++] = c;
    }
    out[written] = '\0';
}

static bool key_is(const char *key, const char *name)
{
    char folded_key[64];
    char folded_name[64];

    fold_key(key, folded_key, sizeof folded_key);
    fold_key(name, folded_name, sizeof folded_name);
    return strcmp(folded_key, folded_name) == 0;
}

static bool number_value(toml_datum_t value, double *out)
{
    if (value.type == TOML_FP64) {
        *out = value.u.fp64;
        return true;
    }
    if (value.type == TOML_INT64) {
        *out = (double)value.u.int64;
        return true;
    }
    return false;
}

static bool apply_font(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        /* `face` is the canonical name. `path` is still read so a file
         * written by an older build keeps working. */
        if ((key_is(key, "face") || key_is(key, "path")) &&
            value.type == TOML_STRING) {
            int length = value.u.str.len;

            if (length >= 0 && (size_t)length < sizeof settings->font &&
                !memchr(value.u.str.ptr, '\0', (size_t)length)) {
                memcpy(settings->font, value.u.str.ptr, (size_t)length);
                settings->font[length] = '\0';
                changed = true;
            }
        } else if (key_is(key, "size")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number > 0.0 && number <= 200.0) {
                settings->point_size = (float)number;
                changed = true;
            }
        } else if (key_is(key, "ligatures") && value.type == TOML_BOOLEAN) {
            settings->ligatures = value.u.boolean;
            changed = true;
        } else if (key_is(key, "fallback") && value.type == TOML_ARRAY) {
            char parsed[SETTINGS_MAX_FALLBACKS][256];
            int count = value.u.arr.size;
            bool ok = count >= 0 && count <= SETTINGS_MAX_FALLBACKS;
            int i;

            for (i = 0; ok && i < count; i++) {
                toml_datum_t elem = value.u.arr.elem[i];
                int length;

                if (elem.type != TOML_STRING) {
                    ok = false;
                    break;
                }
                length = elem.u.str.len;
                if (length <= 0 || (size_t)length >= sizeof parsed[0] ||
                    memchr(elem.u.str.ptr, '\0', (size_t)length)) {
                    ok = false;
                    break;
                }
                memcpy(parsed[i], elem.u.str.ptr, (size_t)length);
                parsed[i][length] = '\0';
            }
            if (ok) {
                for (i = 0; i < count; i++)
                    memcpy(settings->font_fallback[i], parsed[i],
                           strlen(parsed[i]) + 1);
                settings->font_fallback_count = count;
                changed = true;
            }
        }
    }
    return changed;
}

static bool apply_window(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if (key_is(key, "opacity")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number >= 0.0 && number <= 1.0) {
                settings->panel_opacity = (float)number;
                changed = true;
            }
        } else if (key_is(key, "backdrop") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "panel") == 0) {
                settings->backdrop = 0;
                changed = true;
            } else if (strcmp(value.u.s, "mica") == 0) {
                settings->backdrop = 1;
                changed = true;
            } else if (strcmp(value.u.s, "mica_alt") == 0 ||
                       strcmp(value.u.s, "mica-alt") == 0) {
                settings->backdrop = 2;
                changed = true;
            } else if (strcmp(value.u.s, "acrylic") == 0) {
                settings->backdrop = 3;
                changed = true;
            } else if (strcmp(value.u.s, "glass") == 0 ||
                       strcmp(value.u.s, "liquid_glass") == 0 ||
                       strcmp(value.u.s, "liquid-glass") == 0) {
                settings->backdrop = 4;
                changed = true;
            } else if (strcmp(value.u.s, "glass_dark") == 0 ||
                       strcmp(value.u.s, "glass-dark") == 0 ||
                       strcmp(value.u.s, "glass_alt") == 0 ||
                       strcmp(value.u.s, "glass-alt") == 0 ||
                       strcmp(value.u.s, "glass_obscure") == 0 ||
                       strcmp(value.u.s, "glass-obscure") == 0 ||
                       strcmp(value.u.s, "obscure_glass") == 0 ||
                       strcmp(value.u.s, "obscure-glass") == 0 ||
                       strcmp(value.u.s, "liquid_glass_dark") == 0 ||
                       strcmp(value.u.s, "liquid-glass-dark") == 0) {
                settings->backdrop = 5;
                changed = true;
            }
        } else if (key_is(key, "decoration") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "rounded") == 0) {
                settings->decoration = SETTINGS_DECORATION_ROUNDED;
                changed = true;
            } else if (strcmp(value.u.s, "square") == 0) {
                settings->decoration = SETTINGS_DECORATION_SQUARE;
                changed = true;
            } else if (strcmp(value.u.s, "compact") == 0) {
                settings->decoration = SETTINGS_DECORATION_COMPACT;
                changed = true;
            } else if (strcmp(value.u.s, "native") == 0) {
                settings->decoration = SETTINGS_DECORATION_NATIVE;
                changed = true;
            }
        } else if (key_is(key, "title_bar") && value.type == TOML_BOOLEAN) {
            settings->title_bar = value.u.boolean;
            changed = true;
        }
    }
    return changed;
}

static bool apply_terminal_command(Settings *settings, toml_datum_t array)
{
    char parsed[SETTINGS_MAX_ARGS][SETTINGS_ARG_MAX];
    int count;
    int index;

    if (array.type != TOML_ARRAY)
        return false;
    count = array.u.arr.size;
    if (count < 0 || count > SETTINGS_MAX_ARGS)
        return false;
    for (index = 0; index < count; index++) {
        toml_datum_t elem = array.u.arr.elem[index];
        int length;

        if (elem.type != TOML_STRING)
            return false;
        length = elem.u.str.len;
        if (length < 0 || (size_t)length >= SETTINGS_ARG_MAX ||
            memchr(elem.u.str.ptr, '\0', (size_t)length))
            return false;
        /* argv[0] must be non-empty. */
        if (index == 0 && length == 0)
            return false;
        memcpy(parsed[index], elem.u.str.ptr, (size_t)length);
        parsed[index][length] = '\0';
    }
    for (index = 0; index < count; index++)
        memcpy(settings->command[index], parsed[index], strlen(parsed[index]) + 1);
    settings->command_argc = count;
    return true;
}

static bool apply_terminal_env(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    if (table.type != TOML_TABLE)
        return false;
    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];
        size_t key_len;
        int val_len;
        int slot;

        if (value.type != TOML_STRING || !key || !key[0])
            continue;
        key_len = strlen(key);
        if (key_len >= SETTINGS_ENV_KEY_MAX || strchr(key, '='))
            continue;
        val_len = value.u.str.len;
        if (val_len < 0 || (size_t)val_len >= SETTINGS_ENV_VAL_MAX ||
            memchr(value.u.str.ptr, '\0', (size_t)val_len))
            continue;

        /* Update an existing key in place, or append a new one. */
        for (slot = 0; slot < settings->env_count; slot++) {
            if (strcmp(settings->env[slot].key, key) == 0)
                break;
        }
        if (slot == settings->env_count) {
            if (settings->env_count >= SETTINGS_MAX_ENV)
                continue;
            settings->env_count++;
        }
        memcpy(settings->env[slot].key, key, key_len + 1);
        memcpy(settings->env[slot].value, value.u.str.ptr, (size_t)val_len);
        settings->env[slot].value[val_len] = '\0';
        changed = true;
    }
    return changed;
}

static bool apply_terminal(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if (key_is(key, "copy_on_select") && value.type == TOML_BOOLEAN) {
            settings->copy_on_select = value.u.boolean;
            changed = true;
        } else if (key_is(key, "shell") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "auto") == 0) {
                settings->shell = SETTINGS_SHELL_AUTO;
                changed = true;
            } else if (strcmp(value.u.s, "pwsh") == 0) {
                settings->shell = SETTINGS_SHELL_PWSH;
                changed = true;
            } else if (strcmp(value.u.s, "powershell") == 0) {
                settings->shell = SETTINGS_SHELL_WINDOWS_POWERSHELL;
                changed = true;
            } else if (strcmp(value.u.s, "cmd") == 0) {
                settings->shell = SETTINGS_SHELL_CMD;
                changed = true;
            }
        } else if (key_is(key, "osc52") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "ask") == 0) {
                settings->osc52 = SETTINGS_OSC52_ASK;
                changed = true;
            } else if (strcmp(value.u.s, "allow") == 0) {
                settings->osc52 = SETTINGS_OSC52_ALLOW;
                changed = true;
            } else if (strcmp(value.u.s, "deny") == 0) {
                settings->osc52 = SETTINGS_OSC52_DENY;
                changed = true;
            }
        } else if (key_is(key, "notifications") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "ask") == 0) {
                settings->notifications = SETTINGS_OSC52_ASK;
                changed = true;
            } else if (strcmp(value.u.s, "allow") == 0) {
                settings->notifications = SETTINGS_OSC52_ALLOW;
                changed = true;
            } else if (strcmp(value.u.s, "deny") == 0) {
                settings->notifications = SETTINGS_OSC52_DENY;
                changed = true;
            }
        } else if (key_is(key, "bell") && value.type == TOML_STRING) {
            if (strcmp(value.u.s, "visual") == 0) {
                settings->bell = SETTINGS_BELL_VISUAL;
                changed = true;
            } else if (strcmp(value.u.s, "taskbar") == 0) {
                settings->bell = SETTINGS_BELL_TASKBAR;
                changed = true;
            } else if (strcmp(value.u.s, "none") == 0) {
                settings->bell = SETTINGS_BELL_NONE;
                changed = true;
            }
        } else if (key_is(key, "command")) {
            if (apply_terminal_command(settings, value))
                changed = true;
        } else if (key_is(key, "cwd") && value.type == TOML_STRING) {
            int length = value.u.str.len;

            if (length >= 0 && (size_t)length < sizeof settings->cwd &&
                !memchr(value.u.str.ptr, '\0', (size_t)length)) {
                memcpy(settings->cwd, value.u.str.ptr, (size_t)length);
                settings->cwd[length] = '\0';
                changed = true;
            }
        } else if (key_is(key, "env")) {
            if (apply_terminal_env(settings, value))
                changed = true;
        } else if (key_is(key, "scrollback_lines") &&
                   value.type == TOML_INT64 && value.u.int64 >= 0 &&
                   value.u.int64 <= SETTINGS_SCROLLBACK_MAX) {
            settings->scrollback_lines = (int)value.u.int64;
            changed = true;
        }
    }
    return changed;
}

static bool apply_keys(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    if (table.type != TOML_TABLE)
        return false;
    for (index = 0; index < table.u.tab.size; index++) {
        const char *chord = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];
        SDL_Scancode scancode;
        uint8_t mods;
        KeyAction action;
        const char *canonical;
        int slot;

        if (value.type != TOML_STRING || !chord)
            continue;
        if (strlen(chord) >= sizeof settings->key_overrides[0].chord)
            continue;
        if (!keys_parse_chord(chord, &scancode, &mods))
            continue;
        if (!keys_parse_action(value.u.s, &action))
            continue;
        if (!keys_set(&settings->keys, scancode, mods, action))
            continue;

        canonical = keys_action_name(action);
        for (slot = 0; slot < settings->key_override_count; slot++) {
            SDL_Scancode existing_sc;
            uint8_t existing_mods;

            if (keys_parse_chord(settings->key_overrides[slot].chord,
                                 &existing_sc, &existing_mods) &&
                existing_sc == scancode && existing_mods == mods)
                break;
        }
        if (slot == settings->key_override_count) {
            if (settings->key_override_count < SETTINGS_MAX_KEY_OVERRIDES)
                settings->key_override_count++;
            else
                slot = -1;
        }
        if (slot >= 0) {
            snprintf(settings->key_overrides[slot].chord,
                     sizeof settings->key_overrides[slot].chord, "%s", chord);
            snprintf(settings->key_overrides[slot].action,
                     sizeof settings->key_overrides[slot].action, "%s",
                     canonical);
        }
        changed = true;
    }
    return changed;
}

static const char *const PALETTE_KEY_NAMES[16] = {
    "black", "red", "green", "yellow",
    "blue", "magenta", "cyan", "white",
    "bright_black", "bright_red", "bright_green", "bright_yellow",
    "bright_blue", "bright_magenta", "bright_cyan", "bright_white"
};

static bool apply_colors(Settings *settings, toml_datum_t table)
{
    bool changed = false;
    int index;

    if (table.type != TOML_TABLE)
        return false;

    /* Pass 1: apply `scheme` preset first so individual keys can override it. */
    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if (key_is(key, "scheme") && value.type == TOML_STRING) {
            if (colors_preset(&settings->colors, value.u.s)) {
                settings->colors.custom = true;
                changed = true;
            }
        }
    }

    /* Pass 2: individual colours and palette array. */
    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];
        ColorRgb rgb;

        if (key_is(key, "scheme"))
            continue;
        if (key_is(key, "palette") && value.type == TOML_ARRAY) {
            int count = value.u.arr.size;
            int i;

            if (count > 16)
                count = 16;
            for (i = 0; i < count; i++) {
                toml_datum_t elem = value.u.arr.elem[i];

                if (elem.type == TOML_STRING &&
                    colors_parse_hex(elem.u.s, &rgb)) {
                    settings->colors.palette[i] = rgb;
                    settings->colors.custom = true;
                    changed = true;
                }
            }
            continue;
        }
        if (value.type != TOML_STRING || !colors_parse_hex(value.u.s, &rgb))
            continue;
        if (key_is(key, "foreground")) {
            settings->colors.foreground = rgb;
            settings->colors.custom = true;
            changed = true;
        } else if (key_is(key, "background")) {
            settings->colors.background = rgb;
            settings->colors.custom = true;
            changed = true;
        } else if (key_is(key, "cursor")) {
            settings->colors.cursor = rgb;
            settings->colors.custom = true;
            changed = true;
        } else if (key_is(key, "selection_foreground") ||
                   key_is(key, "selection_fg")) {
            settings->colors.selection_fg = rgb;
            settings->colors.has_selection_fg = true;
            settings->colors.custom = true;
            changed = true;
        } else if (key_is(key, "selection_background") ||
                   key_is(key, "selection_bg")) {
            settings->colors.selection_bg = rgb;
            settings->colors.has_selection_bg = true;
            settings->colors.custom = true;
            changed = true;
        } else {
            int p;

            for (p = 0; p < 16; p++) {
                if (key_is(key, PALETTE_KEY_NAMES[p])) {
                    settings->colors.palette[p] = rgb;
                    settings->colors.custom = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}

static bool apply_table(Settings *settings, const char *name,
                        toml_datum_t table)
{
    if (table.type != TOML_TABLE)
        return false;
    if (key_is(name, "font"))
        return apply_font(settings, table);
    if (key_is(name, "window"))
        return apply_window(settings, table);
    if (key_is(name, "terminal"))
        return apply_terminal(settings, table);
    if (key_is(name, "colors"))
        return apply_colors(settings, table);
    if (key_is(name, "keys"))
        return apply_keys(settings, table);
    return false;
}

static bool parse_toml(Settings *settings, const char *text, size_t length,
                       char *error, size_t error_capacity)
{
    toml_result_t document;
    bool changed = false;
    int index;

    if (error && error_capacity)
        error[0] = '\0';
    if (!settings || !text || length > (size_t)INT_MAX)
        return false;

    document = toml_parse(text, (int)length);
    if (!document.ok) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s", document.errmsg);
        toml_free(document);
        return false;
    }
    if (document.toptab.type == TOML_TABLE) {
        for (index = 0; index < document.toptab.u.tab.size; index++) {
            if (apply_table(settings, document.toptab.u.tab.key[index],
                            document.toptab.u.tab.value[index]))
                changed = true;
        }
    }
    toml_free(document);
    return changed;
}

bool settings_parse_toml(Settings *settings, const char *text, size_t length)
{
    return parse_toml(settings, text, length, NULL, 0);
}

/* Validate known values individually with the same rules as the legacy
 * overlay parser. Unknown keys are intentionally ignored for compatibility. */
static bool settings_known_key(const char *table, const char *key)
{
    static const char *const font[] = { "face", "path", "size", "ligatures", "fallback" };
    static const char *const window[] = { "opacity", "backdrop", "decoration", "title_bar" };
    static const char *const terminal[] = { "copy_on_select", "shell", "osc52", "notifications", "bell", "command", "cwd", "env", "scrollback_lines" };
    static const char *const colors[] = { "scheme", "palette", "foreground", "background", "cursor", "selection_foreground", "selection_fg", "selection_background", "selection_bg" };
    const char *const *names = NULL;
    size_t count = 0, i;
    if (key_is(table, "keys")) return true;
    if (key_is(table, "font")) { names = font; count = sizeof font / sizeof font[0]; }
    else if (key_is(table, "window")) { names = window; count = sizeof window / sizeof window[0]; }
    else if (key_is(table, "terminal")) { names = terminal; count = sizeof terminal / sizeof terminal[0]; }
    else if (key_is(table, "colors")) {
        names = colors; count = sizeof colors / sizeof colors[0];
        for (i = 0; i < 16; i++)
            if (key_is(key, PALETTE_KEY_NAMES[i])) return true;
    }
    for (i = 0; i < count; i++)
        if (key_is(key, names[i])) return true;
    return false;
}

static bool settings_validate_table(const char *name, toml_datum_t table,
                                    char *error, size_t capacity)
{
    int i;
    bool known = key_is(name, "font") || key_is(name, "window") ||
                 key_is(name, "terminal") || key_is(name, "colors") ||
                 key_is(name, "keys");
    if (!known) return true;
    if (table.type != TOML_TABLE) {
        if (error && capacity) snprintf(error, capacity, "[%s] must be a table", name);
        return false;
    }
    for (i = 0; i < table.u.tab.size; i++) {
        const char *key = table.u.tab.key[i];
        toml_datum_t value = table.u.tab.value[i];
        toml_datum_t one = table;
        Settings scratch;
        bool valid;
        int j;
        if (!settings_known_key(name, key)) continue;
        settings_default(&scratch);
        one.u.tab.size = 1;
        one.u.tab.key = &table.u.tab.key[i];
        one.u.tab.value = &table.u.tab.value[i];
        if (key_is(name, "terminal") && key_is(key, "env")) {
            valid = value.type == TOML_TABLE && value.u.tab.size <= SETTINGS_MAX_ENV;
            for (j = 0; valid && j < value.u.tab.size; j++) {
                toml_datum_t entry = value;
                entry.u.tab.size = 1;
                entry.u.tab.key = &value.u.tab.key[j];
                entry.u.tab.value = &value.u.tab.value[j];
                valid = apply_terminal_env(&scratch, entry);
            }
        } else if (key_is(name, "colors") && key_is(key, "palette")) {
            valid = value.type == TOML_ARRAY && value.u.arr.size <= 16;
            for (j = 0; valid && j < value.u.arr.size; j++) {
                ColorRgb rgb;
                toml_datum_t element = value.u.arr.elem[j];
                valid = element.type == TOML_STRING &&
                        (int)strlen(element.u.s) == element.u.str.len &&
                        colors_parse_hex(element.u.s, &rgb);
            }
        } else {
            valid = apply_table(&scratch, name, one);
        }
        if (value.type == TOML_STRING &&
            (value.u.str.len < 0 || memchr(value.u.str.ptr, '\0', (size_t)value.u.str.len)))
            valid = false;
        if (!valid) {
            if (error && capacity) snprintf(error, capacity, "invalid %s.%s", name, key);
            return false;
        }
    }
    if (key_is(name, "keys") && table.u.tab.size > SETTINGS_MAX_KEY_OVERRIDES) {
        if (error && capacity) snprintf(error, capacity, "too many key bindings");
        return false;
    }
    return true;
}

static unsigned settings_diff(const Settings *old, const Settings *next)
{
    unsigned diff = 0;
    int i;
    if (strcmp(old->font, next->font) || old->point_size != next->point_size ||
        old->font_fallback_count != next->font_fallback_count)
        diff |= SETTINGS_DIFF_FONT;
    else for (i = 0; i < old->font_fallback_count; i++)
        if (strcmp(old->font_fallback[i], next->font_fallback[i]))
            diff |= SETTINGS_DIFF_FONT;
    if (old->ligatures != next->ligatures)
        diff |= SETTINGS_DIFF_LIGATURES;
    if (memcmp(&old->colors, &next->colors, sizeof old->colors))
        diff |= SETTINGS_DIFF_COLORS;
    if (memcmp(&old->keys, &next->keys, sizeof old->keys))
        diff |= SETTINGS_DIFF_KEYS;
    if (old->panel_opacity != next->panel_opacity || old->backdrop != next->backdrop ||
        old->decoration != next->decoration || old->title_bar != next->title_bar)
        diff |= SETTINGS_DIFF_WINDOW;
    if (old->copy_on_select != next->copy_on_select || old->shell != next->shell ||
        old->osc52 != next->osc52 || old->notifications != next->notifications ||
        old->bell != next->bell || old->scrollback_lines != next->scrollback_lines ||
        old->command_argc != next->command_argc || strcmp(old->cwd, next->cwd) ||
        old->env_count != next->env_count)
        diff |= SETTINGS_DIFF_TERMINAL;
    for (i = 0; i < old->command_argc && i < next->command_argc; i++)
        if (strcmp(old->command[i], next->command[i])) diff |= SETTINGS_DIFF_TERMINAL;
    for (i = 0; i < old->env_count && i < next->env_count; i++)
        if (strcmp(old->env[i].key, next->env[i].key) ||
            strcmp(old->env[i].value, next->env[i].value)) diff |= SETTINGS_DIFF_TERMINAL;
    return diff;
}

bool settings_reload_text(Settings *settings, const char *text, size_t length,
                          unsigned *diff, char *error, size_t error_capacity)
{
    toml_result_t document;
    Settings candidate;
    int i;
    if (diff) *diff = 0;
    if (error && error_capacity) error[0] = '\0';
    if (!settings || !text || length > SETTINGS_MAX_BYTES) {
        if (error && error_capacity) snprintf(error, error_capacity, "invalid or oversized settings");
        return false;
    }
    document = toml_parse(text, (int)length);
    if (!document.ok) {
        if (error && error_capacity) snprintf(error, error_capacity, "%s", document.errmsg);
        toml_free(document);
        return false;
    }
    settings_default(&candidate);
    for (i = 0; i < document.toptab.u.tab.size; i++) {
        const char *name = document.toptab.u.tab.key[i];
        toml_datum_t table = document.toptab.u.tab.value[i];
        if (!settings_validate_table(name, table, error, error_capacity)) {
            toml_free(document);
            return false;
        }
        apply_table(&candidate, name, table);
    }
    toml_free(document);
    if (diff) *diff = settings_diff(settings, &candidate);
    *settings = candidate;
    return true;
}

bool settings_reload(Settings *settings, unsigned *diff,
                     char *error, size_t error_capacity)
{
    char path[512];
    char *text;
    size_t length;
    bool ok;
    if (diff) *diff = 0;
    if (!settings_path(path, sizeof path) || !(text = SDL_LoadFile(path, &length))) {
        if (error && error_capacity) snprintf(error, error_capacity, "cannot read termica.toml");
        return false;
    }
    ok = settings_reload_text(settings, text, length, diff, error, error_capacity);
    SDL_free(text);
    return ok;
}

static void append_piece(char *out, size_t capacity, size_t *used,
                         const char *piece, size_t length)
{
    if (*used >= capacity)
        return;
    if (length > capacity - 1 - *used)
        length = capacity - 1 - *used;
    memcpy(out + *used, piece, length);
    *used += length;
    out[*used] = '\0';
}

static void escape_toml_string(const char *text, char *out, size_t capacity)
{
    size_t used = 0;

    if (capacity == 0)
        return;
    out[0] = '\0';
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        const char *piece = NULL;
        char escape[8];
        char one[2];

        switch (c) {
        case '\\': piece = "\\\\"; break;
        case '"':  piece = "\\\""; break;
        case '\b': piece = "\\b"; break;
        case '\t': piece = "\\t"; break;
        case '\n': piece = "\\n"; break;
        case '\f': piece = "\\f"; break;
        case '\r': piece = "\\r"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                snprintf(escape, sizeof escape, "\\u%04X", (unsigned)c);
                piece = escape;
            }
            break;
        }
        if (!piece) {
            one[0] = (char)c;
            one[1] = '\0';
            piece = one;
        }
        append_piece(out, capacity, &used, piece, strlen(piece));
    }
}

static void format_float(double value, char *out, size_t capacity)
{
    size_t length;

    if (capacity == 0)
        return;
    snprintf(out, capacity, "%g", value);
    length = strlen(out);
    if (strchr(out, '.') || strchr(out, 'e') || strchr(out, 'E'))
        return;
    if (length + 2 >= capacity)
        return;
    out[length++] = '.';
    out[length++] = '0';
    out[length] = '\0';
}

void settings_format(const Settings *settings, char *out, size_t capacity)
{
    const char *shell = "auto";
    const char *osc52 = "ask";
    const char *notifications = "ask";
    const char *bell = "visual";
    const char *backdrop = "panel";
    const char *decoration = "rounded";
    char font[3072];
    char size[32];
    char opacity[32];

    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!settings)
        return;

    switch (settings->shell) {
    case SETTINGS_SHELL_PWSH: shell = "pwsh"; break;
    case SETTINGS_SHELL_WINDOWS_POWERSHELL: shell = "powershell"; break;
    case SETTINGS_SHELL_CMD: shell = "cmd"; break;
    default: break;
    }
    switch (settings->osc52) {
    case SETTINGS_OSC52_ALLOW: osc52 = "allow"; break;
    case SETTINGS_OSC52_DENY: osc52 = "deny"; break;
    default: break;
    }
    switch (settings->notifications) {
    case SETTINGS_OSC52_ALLOW: notifications = "allow"; break;
    case SETTINGS_OSC52_DENY: notifications = "deny"; break;
    default: break;
    }
    switch (settings->bell) {
    case SETTINGS_BELL_TASKBAR: bell = "taskbar"; break;
    case SETTINGS_BELL_NONE: bell = "none"; break;
    default: break;
    }
    switch (settings->backdrop) {
    case 1: backdrop = "mica"; break;
    case 2: backdrop = "mica_alt"; break;
    case 3: backdrop = "acrylic"; break;
    case 4: backdrop = "glass"; break;
    case 5: backdrop = "glass_dark"; break;
    default: break;
    }
    switch (settings->decoration) {
    case SETTINGS_DECORATION_SQUARE: decoration = "square"; break;
    case SETTINGS_DECORATION_COMPACT: decoration = "compact"; break;
    case SETTINGS_DECORATION_NATIVE: decoration = "native"; break;
    default: break;
    }

    escape_toml_string(settings->font, font, sizeof font);
    format_float((double)settings->point_size, size, sizeof size);
    format_float((double)settings->panel_opacity, opacity, sizeof opacity);

    size_t used = 0;
    int index;

    snprintf(out, capacity,
             "# termica settings\n"
             "\n"
             "[font]\n"
             "face = \"%s\"\n"
             "size = %s\n"
             "ligatures = %s\n",
             font, size, settings->ligatures ? "true" : "false");
    used = strlen(out);

    if (settings->font_fallback_count > 0) {
        append_piece(out, capacity, &used, "fallback = [", 12);
        for (index = 0; index < settings->font_fallback_count; index++) {
            char escaped[512];

            if (index > 0)
                append_piece(out, capacity, &used, ", ", 2);
            escape_toml_string(settings->font_fallback[index], escaped,
                               sizeof escaped);
            append_piece(out, capacity, &used, "\"", 1);
            append_piece(out, capacity, &used, escaped, strlen(escaped));
            append_piece(out, capacity, &used, "\"", 1);
        }
        append_piece(out, capacity, &used, "]\n", 2);
    }

    {
        char middle[1024];

        snprintf(middle, sizeof middle,
                 "\n"
                 "[window]\n"
                 "opacity = %s\n"
                 "backdrop = \"%s\"\n"
                 "decoration = \"%s\"\n"
                 "title_bar = %s\n"
                 "\n"
                 "[terminal]\n"
                 "copy_on_select = %s\n"
                 "shell = \"%s\"\n"
                 "osc52 = \"%s\"\n"
                 "notifications = \"%s\"\n"
                 "bell = \"%s\"\n"
                 "scrollback_lines = %d\n",
                 opacity, backdrop, decoration,
                 settings->title_bar ? "true" : "false",
                 settings->copy_on_select ? "true" : "false",
                 shell, osc52, notifications, bell, settings->scrollback_lines);
        append_piece(out, capacity, &used, middle, strlen(middle));
    }

    if (settings->command_argc > 0) {
        append_piece(out, capacity, &used, "command = [", 11);
        for (index = 0; index < settings->command_argc; index++) {
            char escaped[SETTINGS_ARG_MAX * 2];

            if (index > 0)
                append_piece(out, capacity, &used, ", ", 2);
            escape_toml_string(settings->command[index], escaped,
                               sizeof escaped);
            append_piece(out, capacity, &used, "\"", 1);
            append_piece(out, capacity, &used, escaped, strlen(escaped));
            append_piece(out, capacity, &used, "\"", 1);
        }
        append_piece(out, capacity, &used, "]\n", 2);
    }

    if (settings->cwd[0]) {
        char escaped[1024];

        escape_toml_string(settings->cwd, escaped, sizeof escaped);
        append_piece(out, capacity, &used, "cwd = \"", 7);
        append_piece(out, capacity, &used, escaped, strlen(escaped));
        append_piece(out, capacity, &used, "\"\n", 2);
    }

    if (settings->env_count > 0) {
        append_piece(out, capacity, &used, "\n[terminal.env]\n", 16);
        for (index = 0; index < settings->env_count; index++) {
            char escaped_key[SETTINGS_ENV_KEY_MAX * 2];
            char escaped_val[SETTINGS_ENV_VAL_MAX * 2];

            escape_toml_string(settings->env[index].key, escaped_key,
                               sizeof escaped_key);
            escape_toml_string(settings->env[index].value, escaped_val,
                               sizeof escaped_val);
            append_piece(out, capacity, &used, "\"", 1);
            append_piece(out, capacity, &used, escaped_key,
                         strlen(escaped_key));
            append_piece(out, capacity, &used, "\" = \"", 5);
            append_piece(out, capacity, &used, escaped_val,
                         strlen(escaped_val));
            append_piece(out, capacity, &used, "\"\n", 2);
        }
    }

    if (settings->colors.custom) {
        char hex[16];
        char line[128];

        append_piece(out, capacity, &used, "\n[colors]\n", 10);
        if (settings->colors.scheme_name[0]) {
            snprintf(line, sizeof line, "scheme = \"%s\"\n",
                     settings->colors.scheme_name);
            append_piece(out, capacity, &used, line, strlen(line));
        }
        colors_format_hex(settings->colors.foreground, hex, sizeof hex);
        snprintf(line, sizeof line, "foreground = \"%s\"\n", hex);
        append_piece(out, capacity, &used, line, strlen(line));
        colors_format_hex(settings->colors.background, hex, sizeof hex);
        snprintf(line, sizeof line, "background = \"%s\"\n", hex);
        append_piece(out, capacity, &used, line, strlen(line));
        colors_format_hex(settings->colors.cursor, hex, sizeof hex);
        snprintf(line, sizeof line, "cursor = \"%s\"\n", hex);
        append_piece(out, capacity, &used, line, strlen(line));
        if (settings->colors.has_selection_fg) {
            colors_format_hex(settings->colors.selection_fg, hex, sizeof hex);
            snprintf(line, sizeof line, "selection_foreground = \"%s\"\n", hex);
            append_piece(out, capacity, &used, line, strlen(line));
        }
        if (settings->colors.has_selection_bg) {
            colors_format_hex(settings->colors.selection_bg, hex, sizeof hex);
            snprintf(line, sizeof line, "selection_background = \"%s\"\n", hex);
            append_piece(out, capacity, &used, line, strlen(line));
        }
        append_piece(out, capacity, &used, "palette = [", 11);
        for (index = 0; index < 16; index++) {
            if (index > 0)
                append_piece(out, capacity, &used, ", ", 2);
            colors_format_hex(settings->colors.palette[index], hex, sizeof hex);
            append_piece(out, capacity, &used, "\"", 1);
            append_piece(out, capacity, &used, hex, strlen(hex));
            append_piece(out, capacity, &used, "\"", 1);
        }
        append_piece(out, capacity, &used, "]\n", 2);
    }

    if (settings->key_override_count > 0) {
        append_piece(out, capacity, &used, "\n[keys]\n", 8);
        for (index = 0; index < settings->key_override_count; index++) {
            char escaped_chord[128];
            char escaped_action[64];

            escape_toml_string(settings->key_overrides[index].chord,
                               escaped_chord, sizeof escaped_chord);
            escape_toml_string(settings->key_overrides[index].action,
                               escaped_action, sizeof escaped_action);
            append_piece(out, capacity, &used, "\"", 1);
            append_piece(out, capacity, &used, escaped_chord,
                         strlen(escaped_chord));
            append_piece(out, capacity, &used, "\" = \"", 5);
            append_piece(out, capacity, &used, escaped_action,
                         strlen(escaped_action));
            append_piece(out, capacity, &used, "\"\n", 2);
        }
    }
}

bool config_path(const char *name, char *out, size_t capacity)
{
    const char *base = NULL;
    char directory[512];
    int written;

    if (!out || capacity == 0)
        return false;

#ifdef _WIN32
    base = SDL_getenv("APPDATA");
    if (!base || !base[0])
        return false;
    written = snprintf(directory, sizeof directory, "%s/%s", base,
                       SETTINGS_APP_DIR);
#else
    base = SDL_getenv("XDG_CONFIG_HOME");
    if (base && base[0]) {
        written = snprintf(directory, sizeof directory, "%s/%s", base,
                           SETTINGS_APP_DIR);
    } else {
        const char *home = SDL_getenv("HOME");

        if (!home || !home[0])
            return false;
        written = snprintf(directory, sizeof directory, "%s/.config/%s", home,
                           SETTINGS_APP_DIR);
    }
#endif
    if (written < 0 || (size_t)written >= sizeof directory)
        return false;

    if (strlen(directory) + strlen(name) + 2 > capacity)
        return false;
    snprintf(out, capacity, "%s/%s", directory, name);
    return true;
}

bool settings_path(char *out, size_t capacity)
{
    return config_path(SETTINGS_FILE_NAME, out, capacity);
}

void config_create_directory(const char *path)
{
    char directory[512];
    char *slash;
    size_t length = strlen(path);

    if (length >= sizeof directory)
        return;
    memcpy(directory, path, length + 1);
    slash = strrchr(directory, '/');
    if (!slash)
        slash = strrchr(directory, '\\');
    if (!slash)
        return;
    *slash = '\0';
    SDL_CreateDirectory(directory);
}

bool settings_load(Settings *settings)
{
    char path[512];
    char error[200];
    SDL_PathInfo info;
    char *contents;
    size_t length = 0;
    bool parsed;

    if (!settings)
        return false;
    settings_default(settings);
    if (!settings_path(path, sizeof path))
        return false;

    config_create_directory(path);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return false;
    contents = (char *)SDL_LoadFile(path, &length);
    if (!contents)
        return false;
    if (length > SETTINGS_MAX_BYTES) {
        SDL_free(contents);
        fprintf(stderr, "settings: %s: file is too large\n", path);
        return false;
    }

    parsed = parse_toml(settings, contents, length, error, sizeof error);
    SDL_free(contents);
    if (error[0])
        fprintf(stderr, "settings: %s: %s\n", path, error);
    return parsed;
}

