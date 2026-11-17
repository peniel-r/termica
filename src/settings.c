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
 *   osc52 = "ask"
 *
 * Unknown keys, wrong types and out-of-range values are ignored one at a
 * time. A syntax error rejects the whole document, which is what TOML
 * parsers do; the app then keeps its defaults and says why on stderr. The
 * file is rewritten in a canonical form on exit, so comments are not kept.
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

/* Room for the formatted file, including an escaped font face. */
#define SETTINGS_FORMAT_BYTES 4096

/* Fingerprint of the file as it was read. The save on exit is skipped when
 * the file changed while the app ran, so a hand edit is not overwritten by
 * the values the app still has in memory. */
static bool loaded_file_seen;
static uint64_t loaded_file_hash;

static uint64_t hash_bytes(const char *data, size_t length)
{
    uint64_t hash = 14695981039346656037ULL;
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= (unsigned char)data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void settings_default(Settings *settings)
{
    memset(settings, 0, sizeof *settings);
    settings->point_size = 11.0f;
    settings->panel_opacity = 1.0f;
    settings->backdrop = 1;             /* WINDOW_BACKDROP_MICA        */
    settings->decoration = SETTINGS_DECORATION_NATIVE;
    settings->title_bar = true;
    settings->copy_on_select = false;
    settings->shell = SETTINGS_SHELL_AUTO;
    settings->osc52 = SETTINGS_OSC52_ASK;
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
    switch (settings->backdrop) {
    case 1: backdrop = "mica"; break;
    case 2: backdrop = "mica_alt"; break;
    case 3: backdrop = "acrylic"; break;
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

    snprintf(out, capacity,
             "# termica settings\n"
             "\n"
             "[font]\n"
             "face = \"%s\"\n"
             "size = %s\n"
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
             "osc52 = \"%s\"\n",
             font, size, opacity, backdrop, decoration,
             settings->title_bar ? "true" : "false",
             settings->copy_on_select ? "true" : "false", shell, osc52);
}

static bool settings_path_named(const char *name, char *out, size_t capacity)
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
    return settings_path_named(SETTINGS_FILE_NAME, out, capacity);
}

static void settings_create_directory(const char *path)
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
    loaded_file_seen = false;
    loaded_file_hash = 0;
    settings_default(settings);
    if (!settings_path(path, sizeof path))
        return false;

    settings_create_directory(path);
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

    loaded_file_seen = true;
    loaded_file_hash = hash_bytes(contents, length);
    parsed = parse_toml(settings, contents, length, error, sizeof error);
    SDL_free(contents);
    if (error[0])
        fprintf(stderr, "settings: %s: %s\n", path, error);
    return parsed;
}

bool settings_save(const Settings *settings)
{
    char path[512];
    char temporary[520];
    char text[SETTINGS_FORMAT_BYTES];
    SDL_PathInfo info;
    SDL_IOStream *stream;
    size_t length;
    bool exists;

    if (!settings || !settings_path(path, sizeof path))
        return false;

    /* Do not overwrite a file that appeared, disappeared or changed after
     * it was read: the user may have edited it while the app was running. */
    exists = SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
    if (exists != loaded_file_seen)
        return false;
    if (exists) {
        size_t current_length = 0;
        char *current = (char *)SDL_LoadFile(path, &current_length);
        bool same;

        if (!current || current_length > SETTINGS_MAX_BYTES) {
            SDL_free(current);
            return false;
        }
        same = hash_bytes(current, current_length) == loaded_file_hash;
        SDL_free(current);
        if (!same)
            return false;
    }

    settings_create_directory(path);
    settings_format(settings, text, sizeof text);
    length = strlen(text);

    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >=
        (int)sizeof temporary)
        return false;

    stream = SDL_IOFromFile(temporary, "w");
    if (!stream)
        return false;
    if (SDL_WriteIO(stream, text, length) != length) {
        SDL_CloseIO(stream);
        SDL_RemovePath(temporary);
        return false;
    }
    if (!SDL_CloseIO(stream)) {
        SDL_RemovePath(temporary);
        return false;
    }
    if (!SDL_RenamePath(temporary, path)) {
        SDL_RemovePath(temporary);
        return false;
    }
    loaded_file_seen = true;
    loaded_file_hash = hash_bytes(text, length);
    return true;
}
