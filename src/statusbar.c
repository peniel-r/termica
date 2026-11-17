/*
 * termica - the bottom status bar. See statusbar.h.
 *
 * statusbar.toml is read and written the same way as termica.toml: parsed
 * with tomlc17, unknown keys / wrong types / out-of-range values ignored
 * one at a time, a syntax error rejecting the whole document, and a
 * canonical rewrite on exit guarded by a file fingerprint so a hand edit
 * made while the app runs is not clobbered.
 */

#include "statusbar.h"

#include "settings.h"
#include "tomlc17.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATUSBAR_FILE_NAME "statusbar.toml"

/* Longest file we are willing to read. */
#define STATUSBAR_MAX_BYTES 65536

/* Room for the formatted file. */
#define STATUSBAR_FORMAT_BYTES 8192

/* The spinner is fixed: these are the only frames it shows, and this is the
 * resting icon when the user has not set `[busy] idle`. U+F1807 is a
 * Material Design icon in the Symbols set; it lives above U+FFFF so it is
 * drawn through the Symbols Nerd Font Mono fallback, not the BMP cache. */
static const char *const BUSY_FRAMES[] = {
    "\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"
};
#define BUSY_FRAME_COUNT ((int)(sizeof BUSY_FRAMES / sizeof BUSY_FRAMES[0]))
#define BUSY_IDLE_DEFAULT "󱠇"

static bool loaded_file_seen;
static uint64_t loaded_file_hash;

/* ------------------------------------------------------------------------- */
/* Small shared text helpers                                                  */
/* ------------------------------------------------------------------------- */

static void buf_append(char *out, size_t cap, size_t *used, const char *text,
                       size_t length)
{
    if (!out || cap == 0 || *used >= cap)
        return;
    if (length > cap - 1 - *used)
        length = cap - 1 - *used;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
}

/* `-` and `_` are the same separator, and case does not matter. */
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

/* Copy a NUL-terminated string datum into a fixed field, rejecting
 * embedded NUL bytes and overlong values. */
static bool set_string(char *dst, size_t capacity, const char *src, int length)
{
    if (!dst || capacity == 0 || !src || length < 0)
        return false;
    if ((size_t)length >= capacity || memchr(src, '\0', (size_t)length))
        return false;
    memcpy(dst, src, (size_t)length);
    dst[length] = '\0';
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *text, SDL_Color *out)
{
    int r;
    int g;
    int b;

    if (!text || text[0] != '#' || strlen(text) < 7)
        return false;
    r = (hex_digit(text[1]) << 4) | hex_digit(text[2]);
    g = (hex_digit(text[3]) << 4) | hex_digit(text[4]);
    b = (hex_digit(text[5]) << 4) | hex_digit(text[6]);
    if (hex_digit(text[1]) < 0 || hex_digit(text[2]) < 0 ||
        hex_digit(text[3]) < 0 || hex_digit(text[4]) < 0 ||
        hex_digit(text[5]) < 0 || hex_digit(text[6]) < 0)
        return false;
    out->r = (Uint8)r;
    out->g = (Uint8)g;
    out->b = (Uint8)b;
    out->a = 255;
    return true;
}

bool statusbar_style_parse(const char *text, StatusbarStyle *out)
{
    const char *p = text ? text : "";
    bool any = false;

    memset(out, 0, sizeof *out);
    while (*p) {
        char token[64];
        size_t used = 0;

        while (*p == ' ')
            p++;
        if (!*p)
            break;
        while (*p && *p != ' ' && used + 1 < sizeof token)
            token[used++] = *p++;
        token[used] = '\0';

        if (strcmp(token, "bold") == 0) {
            out->bold = true;
            any = true;
        } else if (strcmp(token, "italic") == 0) {
            out->italic = true;
            any = true;
        } else if (strcmp(token, "underline") == 0) {
            out->underline = true;
            any = true;
        } else if (strcmp(token, "dim") == 0) {
            out->dim = true;
            any = true;
        } else if (strncmp(token, "fg:", 3) == 0) {
            if (parse_color(token + 3, &out->fg)) {
                out->has_fg = true;
                any = true;
            }
        } else if (strncmp(token, "bg:", 3) == 0) {
            if (parse_color(token + 3, &out->bg)) {
                out->has_bg = true;
                any = true;
            }
        }
    }
    return any;
}

/* ------------------------------------------------------------------------- */
/* Defaults                                                                   */
/* ------------------------------------------------------------------------- */

static void set_module_defaults(Statusbar *sb)
{
    StatusbarModule *m;
    int index;

    for (index = 0; index < STATUSBAR_MODULE_COUNT; index++) {
        m = &sb->modules[index];
        snprintf(m->format, sizeof m->format, "%s", "[$symbol$value]($style)");
        m->symbol[0] = '\0';
        m->style[0] = '\0';
        memset(&m->parsed_style, 0, sizeof m->parsed_style);
    }

    m = &sb->modules[STATUSBAR_DIRECTORY];
    snprintf(m->style, sizeof m->style, "%s", "fg:#9ccfd8 bold");
    m->truncation_length = 3;
    snprintf(m->home_symbol, sizeof m->home_symbol, "%s", "~");

    m = &sb->modules[STATUSBAR_GIT_BRANCH];
    snprintf(m->style, sizeof m->style, "%s", "fg:#c4a7e7 bold");
    snprintf(m->symbol, sizeof m->symbol, "%s", "\uE0A0");

    m = &sb->modules[STATUSBAR_GIT_STATUS];
    snprintf(m->style, sizeof m->style, "%s", "fg:#ebcb8b");
    snprintf(m->ahead, sizeof m->ahead, "%s", "\u21E1");
    snprintf(m->behind, sizeof m->behind, "%s", "\u21E3");
    snprintf(m->diverged, sizeof m->diverged, "%s", "\u21D5");
    snprintf(m->dirty, sizeof m->dirty, "%s", "\u2717");
    snprintf(m->clean, sizeof m->clean, "%s", "\u2713");
    snprintf(m->conflicted, sizeof m->conflicted, "%s", "=");

    m = &sb->modules[STATUSBAR_CMD_STATUS];
    snprintf(m->style_success, sizeof m->style_success, "%s",
             "fg:#a3be8c");
    snprintf(m->style_failure, sizeof m->style_failure, "%s",
             "fg:#bf616a");
    snprintf(m->symbol_success, sizeof m->symbol_success, "%s", "\u2713");
    snprintf(m->symbol_failure, sizeof m->symbol_failure, "%s", "\u2717");

    m = &sb->modules[STATUSBAR_CMD_DURATION];
    snprintf(m->style, sizeof m->style, "%s", "fg:#ebcb8b");
    m->min_time_ms = 2000;

    m = &sb->modules[STATUSBAR_TIME];
    snprintf(m->style, sizeof m->style, "%s", "fg:#d8dee9");
    snprintf(m->time_format, sizeof m->time_format, "%s", "%H:%M:%S");

    m = &sb->modules[STATUSBAR_BUSY];
    snprintf(m->style, sizeof m->style, "%s", "fg:#81a1c1");
    snprintf(m->format, sizeof m->format, "%s", "[$symbol]($style)");
    /* The frames and the resting icon are constants; an empty `idle` means
     * "use the fixed resting icon". Only `[busy] idle` overrides it. */
    m->idle[0] = '\0';
}

void statusbar_default(Statusbar *sb)
{
    memset(sb, 0, sizeof *sb);
    sb->point_size = 9.0f;
    snprintf(sb->format, sizeof sb->format, "%s",
             "$directory$git_branch$git_status $cmd_status$cmd_duration"
             " $busy$time");
    sb->right_format[0] = '\0';
    sb->center_format[0] = '\0';
    sb->bar_fg = (SDL_Color){ 216, 222, 233, 255 };
    sb->bar_bg = (SDL_Color){ 14, 18, 22, 255 };
    sb->bar_opacity = 0.9f;
    set_module_defaults(sb);

    /* Resolve the style strings written above. */
    for (int index = 0; index < STATUSBAR_MODULE_COUNT; index++) {
        StatusbarModule *m = &sb->modules[index];

        statusbar_style_parse(m->style, &m->parsed_style);
    }
    statusbar_style_parse(sb->modules[STATUSBAR_CMD_STATUS].style_success,
                          &sb->modules[STATUSBAR_CMD_STATUS].parsed_success);
    statusbar_style_parse(sb->modules[STATUSBAR_CMD_STATUS].style_failure,
                          &sb->modules[STATUSBAR_CMD_STATUS].parsed_failure);
}

/* ------------------------------------------------------------------------- */
/* Parsing                                                                    */
/* ------------------------------------------------------------------------- */

static bool apply_font(Statusbar *sb, toml_datum_t table)
{
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if ((key_is(key, "face") || key_is(key, "path")) &&
            value.type == TOML_STRING) {
            if (set_string(sb->font, sizeof sb->font, value.u.str.ptr,
                           value.u.str.len))
                changed = true;
        } else if (key_is(key, "size")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number >= 9.0 && number <= 200.0) {
                sb->point_size = (float)number;
                changed = true;
            }
        }
    }
    return changed;
}

static bool apply_style(Statusbar *sb, toml_datum_t table)
{
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if (key_is(key, "fg") && value.type == TOML_STRING) {
            if (parse_color(value.u.str.ptr, &sb->bar_fg))
                changed = true;
        } else if (key_is(key, "bg") && value.type == TOML_STRING) {
            if (parse_color(value.u.str.ptr, &sb->bar_bg))
                changed = true;
        } else if (key_is(key, "opacity")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number >= 0.0 && number <= 1.0) {
                sb->bar_opacity = (float)number;
                changed = true;
            }
        }
    }
    return changed;
}

static int module_id(const char *name)
{
    if (key_is(name, "directory"))
        return STATUSBAR_DIRECTORY;
    if (key_is(name, "git_branch"))
        return STATUSBAR_GIT_BRANCH;
    if (key_is(name, "git_status"))
        return STATUSBAR_GIT_STATUS;
    if (key_is(name, "cmd_status"))
        return STATUSBAR_CMD_STATUS;
    if (key_is(name, "cmd_duration"))
        return STATUSBAR_CMD_DURATION;
    if (key_is(name, "time"))
        return STATUSBAR_TIME;
    if (key_is(name, "busy"))
        return STATUSBAR_BUSY;
    return -1;
}

static bool apply_module(Statusbar *sb, int id, toml_datum_t table)
{
    StatusbarModule *m = &sb->modules[id];
    bool changed = false;
    int index;

    for (index = 0; index < table.u.tab.size; index++) {
        const char *key = table.u.tab.key[index];
        toml_datum_t value = table.u.tab.value[index];

        if (key_is(key, "format") && value.type == TOML_STRING) {
            if (set_string(m->format, sizeof m->format, value.u.str.ptr,
                           value.u.str.len))
                changed = true;
        } else if (key_is(key, "symbol") && value.type == TOML_STRING &&
                   id != STATUSBAR_BUSY) {
            /* [busy] symbol is fixed; only idle is configurable there. */
            if (set_string(m->symbol, sizeof m->symbol, value.u.str.ptr,
                           value.u.str.len))
                changed = true;
        } else if (key_is(key, "style") && value.type == TOML_STRING) {
            if (set_string(m->style, sizeof m->style, value.u.str.ptr,
                           value.u.str.len)) {
                statusbar_style_parse(m->style, &m->parsed_style);
                changed = true;
            }
        } else if (id == STATUSBAR_DIRECTORY &&
                   key_is(key, "truncation_length")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number >= 0.0 && number <= 64.0) {
                m->truncation_length = (int)number;
                changed = true;
            }
        } else if (id == STATUSBAR_DIRECTORY &&
                   key_is(key, "home_symbol") && value.type == TOML_STRING) {
            if (set_string(m->home_symbol, sizeof m->home_symbol,
                           value.u.str.ptr, value.u.str.len))
                changed = true;
        } else if (id == STATUSBAR_GIT_STATUS && value.type == TOML_STRING) {
            const char *keys[] = { "ahead", "behind", "diverged", "dirty",
                                   "clean", "conflicted" };
            char *dst[] = { m->ahead, m->behind, m->diverged, m->dirty,
                            m->clean, m->conflicted };
            size_t cap[] = { sizeof m->ahead, sizeof m->behind,
                             sizeof m->diverged, sizeof m->dirty,
                             sizeof m->clean, sizeof m->conflicted };
            int k;

            for (k = 0; k < 6; k++) {
                if (key_is(key, keys[k])) {
                    if (set_string(dst[k], cap[k], value.u.str.ptr,
                                   value.u.str.len))
                        changed = true;
                    break;
                }
            }
        } else if (id == STATUSBAR_CMD_STATUS) {
            if (key_is(key, "style_success") && value.type == TOML_STRING) {
                if (set_string(m->style_success, sizeof m->style_success,
                               value.u.str.ptr, value.u.str.len)) {
                    statusbar_style_parse(m->style_success,
                                          &m->parsed_success);
                    changed = true;
                }
            } else if (key_is(key, "style_failure") &&
                       value.type == TOML_STRING) {
                if (set_string(m->style_failure, sizeof m->style_failure,
                               value.u.str.ptr, value.u.str.len)) {
                    statusbar_style_parse(m->style_failure,
                                          &m->parsed_failure);
                    changed = true;
                }
            } else if (key_is(key, "symbol_success") &&
                       value.type == TOML_STRING) {
                if (set_string(m->symbol_success, sizeof m->symbol_success,
                               value.u.str.ptr, value.u.str.len))
                    changed = true;
            } else if (key_is(key, "symbol_failure") &&
                       value.type == TOML_STRING) {
                if (set_string(m->symbol_failure, sizeof m->symbol_failure,
                               value.u.str.ptr, value.u.str.len))
                    changed = true;
            }
        } else if (id == STATUSBAR_CMD_DURATION &&
                   key_is(key, "min_time")) {
            double number;

            if (number_value(value, &number) && isfinite(number) &&
                number >= 0.0 && number <= 3600000.0) {
                m->min_time_ms = (int)number;
                changed = true;
            }
        } else if (id == STATUSBAR_TIME &&
                   key_is(key, "time_format") && value.type == TOML_STRING) {
            if (set_string(m->time_format, sizeof m->time_format,
                           value.u.str.ptr, value.u.str.len))
                changed = true;
        } else if (id == STATUSBAR_BUSY &&
                   key_is(key, "idle") && value.type == TOML_STRING) {
            if (set_string(m->idle, sizeof m->idle, value.u.str.ptr,
                           value.u.str.len))
                changed = true;
        }
    }
    return changed;
}

static bool apply_table(Statusbar *sb, const char *name, toml_datum_t table)
{
    int id;

    if (table.type != TOML_TABLE)
        return false;
    if (key_is(name, "font"))
        return apply_font(sb, table);
    if (key_is(name, "style"))
        return apply_style(sb, table);
    id = module_id(name);
    if (id >= 0)
        return apply_module(sb, id, table);
    return false;
}

static bool parse_toml(Statusbar *sb, const char *text, size_t length,
                       char *error, size_t error_capacity)
{
    toml_result_t document;
    bool changed = false;
    int index;

    if (error && error_capacity)
        error[0] = '\0';
    if (!sb || !text || length > (size_t)INT_MAX)
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
            const char *key = document.toptab.u.tab.key[index];
            toml_datum_t value = document.toptab.u.tab.value[index];

            if (key_is(key, "format") && value.type == TOML_STRING) {
                if (set_string(sb->format, sizeof sb->format,
                               value.u.str.ptr, value.u.str.len))
                    changed = true;
            } else if (key_is(key, "center_format") &&
                       value.type == TOML_STRING) {
                if (set_string(sb->center_format, sizeof sb->center_format,
                               value.u.str.ptr, value.u.str.len))
                    changed = true;
            } else if (key_is(key, "right_format") &&
                       value.type == TOML_STRING) {
                if (set_string(sb->right_format, sizeof sb->right_format,
                               value.u.str.ptr, value.u.str.len))
                    changed = true;
            } else if (apply_table(sb, key, value)) {
                changed = true;
            }
        }
    }
    toml_free(document);
    return changed;
}

bool statusbar_parse_toml(Statusbar *sb, const char *text, size_t length)
{
    return parse_toml(sb, text, length, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Formatting                                                                 */
/* ------------------------------------------------------------------------- */

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
        buf_append(out, capacity, &used, piece, strlen(piece));
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

static void append_kv_string(char *out, size_t cap, size_t *used,
                             const char *key, const char *value)
{
    char escaped[1024];
    char line[1100];

    escape_toml_string(value, escaped, sizeof escaped);
    snprintf(line, sizeof line, "%s = \"%s\"\n", key, escaped);
    buf_append(out, cap, used, line, strlen(line));
}

static void append_kv_int(char *out, size_t cap, size_t *used,
                          const char *key, int value)
{
    char line[64];

    snprintf(line, sizeof line, "%s = %d\n", key, value);
    buf_append(out, cap, used, line, strlen(line));
}

static void append_kv_color(char *out, size_t cap, size_t *used,
                            const char *key, SDL_Color color)
{
    char line[64];

    snprintf(line, sizeof line, "%s = \"#%02x%02x%02x\"\n", key, color.r,
             color.g, color.b);
    buf_append(out, cap, used, line, strlen(line));
}

static void append_module_table(char *out, size_t cap, size_t *used,
                                const char *name, const StatusbarModule *m,
                                bool with_symbol)
{
    char line[256];

    snprintf(line, sizeof line, "\n[%s]\n", name);
    buf_append(out, cap, used, line, strlen(line));
    append_kv_string(out, cap, used, "format", m->format);
    if (with_symbol)
        append_kv_string(out, cap, used, "symbol", m->symbol);
    append_kv_string(out, cap, used, "style", m->style);
}

void statusbar_format(const Statusbar *sb, char *out, size_t capacity)
{
    const StatusbarModule *m;
    char size[32];
    char opacity[32];
    size_t used = 0;

    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!sb)
        return;

    format_float((double)sb->point_size, size, sizeof size);
    format_float((double)sb->bar_opacity, opacity, sizeof opacity);

    buf_append(out, capacity, &used, "# termica status bar\n",
               sizeof "# termica status bar\n" - 1);

    {
        char escaped[1024];
        char line[1100];

        escape_toml_string(sb->format, escaped, sizeof escaped);
        snprintf(line, sizeof line, "\nformat = \"%s\"\n", escaped);
        buf_append(out, capacity, &used, line, strlen(line));
        if (sb->center_format[0]) {
            char center[1024];

            escape_toml_string(sb->center_format, center, sizeof center);
            snprintf(line, sizeof line, "center_format = \"%s\"\n", center);
            buf_append(out, capacity, &used, line, strlen(line));
        }
        if (sb->right_format[0]) {
            char right[1024];

            escape_toml_string(sb->right_format, right, sizeof right);
            snprintf(line, sizeof line, "right_format = \"%s\"\n", right);
            buf_append(out, capacity, &used, line, strlen(line));
        }
    }

    {
        char escaped[1024];
        char line[1100];

        escape_toml_string(sb->font, escaped, sizeof escaped);
        snprintf(line, sizeof line, "\n[font]\nface = \"%s\"\nsize = %s\n",
                 escaped, size);
        buf_append(out, capacity, &used, line, strlen(line));
    }

    buf_append(out, capacity, &used, "\n[style]\n", 9);
    append_kv_color(out, capacity, &used, "fg", sb->bar_fg);
    append_kv_color(out, capacity, &used, "bg", sb->bar_bg);
    append_kv_string(out, capacity, &used, "opacity", opacity);

    m = &sb->modules[STATUSBAR_DIRECTORY];
    append_module_table(out, capacity, &used, "directory", m, true);
    append_kv_int(out, capacity, &used, "truncation_length",
                  m->truncation_length);
    append_kv_string(out, capacity, &used, "home_symbol", m->home_symbol);

    append_module_table(out, capacity, &used, "git_branch",
                        &sb->modules[STATUSBAR_GIT_BRANCH], true);

    m = &sb->modules[STATUSBAR_GIT_STATUS];
    append_module_table(out, capacity, &used, "git_status", m, true);
    append_kv_string(out, capacity, &used, "ahead", m->ahead);
    append_kv_string(out, capacity, &used, "behind", m->behind);
    append_kv_string(out, capacity, &used, "diverged", m->diverged);
    append_kv_string(out, capacity, &used, "dirty", m->dirty);
    append_kv_string(out, capacity, &used, "clean", m->clean);
    append_kv_string(out, capacity, &used, "conflicted", m->conflicted);

    m = &sb->modules[STATUSBAR_CMD_STATUS];
    append_module_table(out, capacity, &used, "cmd_status", m, true);
    append_kv_string(out, capacity, &used, "style_success", m->style_success);
    append_kv_string(out, capacity, &used, "style_failure", m->style_failure);
    append_kv_string(out, capacity, &used, "symbol_success", m->symbol_success);
    append_kv_string(out, capacity, &used, "symbol_failure", m->symbol_failure);

    m = &sb->modules[STATUSBAR_CMD_DURATION];
    append_module_table(out, capacity, &used, "cmd_duration", m, true);
    append_kv_int(out, capacity, &used, "min_time", m->min_time_ms);

    m = &sb->modules[STATUSBAR_TIME];
    append_module_table(out, capacity, &used, "time", m, true);
    append_kv_string(out, capacity, &used, "time_format", m->time_format);

    /* The spinner frames and the resting icon are constants; the only thing
     * written here is an `idle` override, and only when the user set one. */
    m = &sb->modules[STATUSBAR_BUSY];
    append_module_table(out, capacity, &used, "busy", m, false);
    if (m->idle[0])
        append_kv_string(out, capacity, &used, "idle", m->idle);
}

/* ------------------------------------------------------------------------- */
/* Load / save                                                                */
/* ------------------------------------------------------------------------- */

bool statusbar_path(char *out, size_t capacity)
{
    return config_path(STATUSBAR_FILE_NAME, out, capacity);
}

bool statusbar_load(Statusbar *sb)
{
    char path[512];
    char error[200];
    SDL_PathInfo info;
    char *contents;
    size_t length = 0;
    bool parsed;

    if (!sb)
        return false;
    loaded_file_seen = false;
    loaded_file_hash = 0;
    statusbar_default(sb);
    if (!statusbar_path(path, sizeof path))
        return false;

    config_create_directory(path);
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return false;
    contents = (char *)SDL_LoadFile(path, &length);
    if (!contents)
        return false;
    if (length > STATUSBAR_MAX_BYTES) {
        SDL_free(contents);
        fprintf(stderr, "statusbar: %s: file is too large\n", path);
        return false;
    }

    loaded_file_seen = true;
    loaded_file_hash = config_hash_bytes(contents, length);
    parsed = parse_toml(sb, contents, length, error, sizeof error);
    SDL_free(contents);
    if (error[0])
        fprintf(stderr, "statusbar: %s: %s\n", path, error);
    return parsed;
}

bool statusbar_save(const Statusbar *sb)
{
    char path[512];
    char temporary[520];
    char text[STATUSBAR_FORMAT_BYTES];
    SDL_PathInfo info;
    SDL_IOStream *stream;
    size_t length;
    bool exists;

    if (!sb || !statusbar_path(path, sizeof path))
        return false;

    exists = SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
    if (exists != loaded_file_seen)
        return false;
    if (exists) {
        size_t current_length = 0;
        char *current = (char *)SDL_LoadFile(path, &current_length);
        bool same;

        if (!current || current_length > STATUSBAR_MAX_BYTES) {
            SDL_free(current);
            return false;
        }
        same = config_hash_bytes(current, current_length) == loaded_file_hash;
        SDL_free(current);
        if (!same)
            return false;
    }

    config_create_directory(path);
    statusbar_format(sb, text, sizeof text);
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
    loaded_file_hash = config_hash_bytes(text, length);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Pure helpers                                                               */
/* ------------------------------------------------------------------------- */

/* Fold a path byte for a Windows-style comparison: separators are
 * equivalent and case does not matter. OSC 7 arrives with forward slashes,
 * the child's PEB path and USERPROFILE with backslashes; both must match
 * the home directory the same way. */
static char canon_home_char(char c)
{
    if (c == '/')
        c = '\\';
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
    return c;
}

void statusbar_shorten_path(const Statusbar *sb, const char *path, char *out,
                            size_t capacity)
{
    const StatusbarModule *dir = sb ? &sb->modules[STATUSBAR_DIRECTORY] : NULL;
    const char *home_symbol = dir ? dir->home_symbol : "";
    const char *home = NULL;
    int trunc = dir ? dir->truncation_length : 0;
    const char *comp_start[64];
    const char *comp_end[64];
    int ncomp = 0;
    int first_out = 0;   /* components covered by home_symbol */
    size_t used = 0;
    const char *p;
    int i;

    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!path || !path[0])
        return;

    /* Split into components, dropping empty ones from leading or doubled
     * separators. The kept result is a slice of the original string, so the
     * path's own separators survive and the trailing separator the child's
     * PEB reports is simply past the last component. */
    for (p = path; *p && ncomp < 64; ) {
        const char *start;

        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        comp_start[ncomp] = start;
        comp_end[ncomp] = p;
        ncomp++;
    }
    if (ncomp == 0) {
        buf_append(out, capacity, &used, path, strlen(path));   /* a bare root */
        return;
    }

    /* Replace the home directory's leading components with home_symbol,
     * compared per component with separators and case folded, so an OSC 7
     * path with forward slashes and a PEB path with backslashes both match
     * USERPROFILE. */
    home = SDL_getenv("USERPROFILE");
    if (!home || !home[0])
        home = SDL_getenv("HOME");
    if (home && home[0] && home_symbol[0]) {
        const char *hc[64];
        int hl[64];
        int hn = 0;
        int matched = 1;

        for (p = home; *p && hn < 64; ) {
            const char *start;

            while (*p == '/' || *p == '\\')
                p++;
            if (!*p)
                break;
            start = p;
            while (*p && *p != '/' && *p != '\\')
                p++;
            hc[hn] = start;
            hl[hn] = (int)(p - start);
            hn++;
        }
        if (hn > 0 && hn <= ncomp) {
            for (i = 0; i < hn && matched; i++) {
                int j;

                if (comp_end[i] - comp_start[i] != hl[i]) {
                    matched = 0;
                    break;
                }
                for (j = 0; j < hl[i]; j++) {
                    if (canon_home_char(comp_start[i][j]) !=
                        canon_home_char(hc[i][j])) {
                        matched = 0;
                        break;
                    }
                }
            }
            if (matched) {
                first_out = hn;
                buf_append(out, capacity, &used, home_symbol,
                           strlen(home_symbol));
            }
        }
    }

    /* Keep the last `trunc` of the remaining components (all when 0). */
    {
        int keep_from = first_out;
        int remaining = ncomp - first_out;

        if (trunc > 0 && remaining > trunc)
            keep_from = ncomp - trunc;

        if (keep_from < ncomp) {
            const char *begin = comp_start[keep_from];
            size_t length;

            /* After a home substitution the slice starts at the separator
             * that followed the home components, so "~" joins as "~\dir". */
            if (first_out > 0 && begin > path &&
                (begin[-1] == '/' || begin[-1] == '\\'))
                begin--;
            length = (size_t)(comp_end[ncomp - 1] - begin);
            buf_append(out, capacity, &used, begin, length);
        }
    }
}

void statusbar_format_duration(uint64_t ms, char *out, size_t capacity)
{
    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (ms < 1000)
        snprintf(out, capacity, "%llums", (unsigned long long)ms);
    else if (ms < 60000)
        snprintf(out, capacity, "%llus", (unsigned long long)(ms / 1000));
    else
        snprintf(out, capacity, "%llum %llus",
                 (unsigned long long)(ms / 60000),
                 (unsigned long long)((ms % 60000) / 1000));
}

bool statusbar_format_time(const char *format, char *out, size_t capacity)
{
    time_t now;
    struct tm local;

    if (!out || capacity == 0)
        return false;
    out[0] = '\0';
    if (!format || !format[0])
        return false;

    now = time(NULL);
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0)
        return false;
#else
    if (!localtime_r(&now, &local))
        return false;
#endif
    return strftime(out, capacity, format, &local) > 0;
}

static void append_field(char *out, size_t cap, size_t *used,
                         const char *glyph, int count)
{
    char text[64];

    if (!glyph || count <= 0)
        return;
    if (count == 1)
        snprintf(text, sizeof text, "%s", glyph);
    else
        snprintf(text, sizeof text, "%s%d", glyph, count);
    buf_append(out, cap, used, text, strlen(text));
}

void statusbar_git_status_text(const Statusbar *sb, int ahead, int behind,
                               int dirty, int conflicted, bool clean,
                               char *out, size_t capacity)
{
    const StatusbarModule *m = &sb->modules[STATUSBAR_GIT_STATUS];
    size_t used = 0;

    if (!out || capacity == 0)
        return;
    out[0] = '\0';

    if (ahead > 0 && behind > 0) {
        append_field(out, capacity, &used, m->diverged, 1);
    } else {
        append_field(out, capacity, &used, m->ahead, ahead);
        append_field(out, capacity, &used, m->behind, behind);
    }
    append_field(out, capacity, &used, m->conflicted, conflicted);
    append_field(out, capacity, &used, m->dirty, dirty);
    if (used == 0 && clean)
        append_field(out, capacity, &used, m->clean, 1);
}

int statusbar_busy_frames(char frames[][8], int max_frames)
{
    int count = 0;
    int index;

    if (!frames || max_frames <= 0)
        return 0;
    for (index = 0; index < BUSY_FRAME_COUNT && count < max_frames; index++) {
        snprintf(frames[count], 8, "%s", BUSY_FRAMES[index]);
        count++;
    }
    return count;
}

const char *statusbar_busy_idle_default(void)
{
    return BUSY_IDLE_DEFAULT;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct RenderOut {
    char *buf;
    size_t cap;
    size_t used;
    StatusbarRun *runs;
    int max_runs;
    int run_count;
} RenderOut;

static StatusbarStyle bar_default_style(const Statusbar *sb)
{
    StatusbarStyle style;

    memset(&style, 0, sizeof style);
    style.fg = sb->bar_fg;
    style.has_fg = true;
    return style;
}

static StatusbarStyle module_style(const StatusbarModule *module,
                                   const StatusbarModuleValue *value)
{
    if (value && value->style)
        return *value->style;
    return module->parsed_style;
}

static void render_append(RenderOut *out, const char *text, size_t length,
                          StatusbarStyle style)
{
    if (!out || !out->buf || out->cap == 0 || length == 0)
        return;
    if (out->used + length >= out->cap)
        length = out->cap - 1 - out->used;
    if (length == 0)
        return;
    memcpy(out->buf + out->used, text, length);
    if (out->runs && out->run_count < out->max_runs) {
        out->runs[out->run_count].start = (int)out->used;
        out->runs[out->run_count].length = (int)length;
        out->runs[out->run_count].style = style;
        out->run_count++;
    }
    out->used += length;
    out->buf[out->used] = '\0';
}

static void substitute(const char *text, const char *symbol,
                       const char *content, char *out, size_t capacity)
{
    size_t used = 0;
    const char *p = text;

    if (capacity == 0)
        return;
    out[0] = '\0';
    while (*p) {
        if (strncmp(p, "$symbol", 7) == 0) {
            buf_append(out, capacity, &used, symbol, strlen(symbol));
            p += 7;
        } else if (strncmp(p, "$value", 6) == 0) {
            buf_append(out, capacity, &used, content, strlen(content));
            p += 6;
        } else {
            char one[2] = { *p, '\0' };

            buf_append(out, capacity, &used, one, 1);
            p++;
        }
    }
}

static bool render_module(const Statusbar *sb, const StatusbarModule *module,
                          const StatusbarModuleValue *value, RenderOut *out)
{
    const char *template_ = module->format[0] ? module->format
                                              : "[$symbol$value]($style)";
    const char *symbol = value && value->symbol ? value->symbol : module->symbol;
    const char *content = value && value->value ? value->value : "";
    StatusbarStyle mod_style = module_style(module, value);
    StatusbarStyle plain = bar_default_style(sb);
    bool emitted = false;
    const char *p = template_;

    while (*p) {
        if (*p == '[') {
            const char *close = strchr(p + 1, ']');

            if (!close) {
                render_append(out, p, strlen(p), plain);
                return true;
            }
            {
                char inner[512];
                char substituted[512];
                size_t inner_len = (size_t)(close - (p + 1));

                if (inner_len >= sizeof inner)
                    inner_len = sizeof inner - 1;
                memcpy(inner, p + 1, inner_len);
                inner[inner_len] = '\0';

                substitute(inner, symbol, content, substituted,
                           sizeof substituted);
                if (substituted[0]) {
                    StatusbarStyle style = plain;
                    const char *after = close + 1;

                    if (*after == '(') {
                        const char *sc = strchr(after + 1, ')');

                        if (sc) {
                            char spec[64];
                            size_t spec_len = (size_t)(sc - (after + 1));

                            if (spec_len >= sizeof spec)
                                spec_len = sizeof spec - 1;
                            memcpy(spec, after + 1, spec_len);
                            spec[spec_len] = '\0';
                            after = sc + 1;

                            if (strcmp(spec, "$style") == 0 ||
                                strcmp(spec, "style") == 0)
                                style = mod_style;
                            else
                                statusbar_style_parse(spec, &style);
                        }
                    }
                    render_append(out, substituted, strlen(substituted),
                                  style);
                    emitted = true;
                    p = after;
                    continue;
                }
                /* empty group: continue after the group */
                p = close + 1;
                if (*p == '(') {
                    const char *sc = strchr(p + 1, ')');

                    if (sc)
                        p = sc + 1;
                }
                continue;
            }
        } else {
            const char *start = p;

            while (*p && *p != '[')
                p++;
            if (p > start) {
                render_append(out, start, (size_t)(p - start), plain);
                emitted = true;
            }
        }
    }
    return emitted;
}

static const StatusbarModuleValue *state_value(const StatusbarState *state,
                                               int id)
{
    switch (id) {
    case STATUSBAR_DIRECTORY: return &state->directory;
    case STATUSBAR_GIT_BRANCH: return &state->git_branch;
    case STATUSBAR_GIT_STATUS: return &state->git_status;
    case STATUSBAR_CMD_STATUS: return &state->cmd_status;
    case STATUSBAR_CMD_DURATION: return &state->cmd_duration;
    case STATUSBAR_TIME: return &state->time;
    case STATUSBAR_BUSY: return &state->busy;
    default: return NULL;
    }
}

int statusbar_render(const Statusbar *sb, const StatusbarState *state,
                     const char *format, char *out, size_t out_capacity,
                     StatusbarRun *runs, int max_runs)
{
    RenderOut ro;
    const char *p = format ? format : "";

    memset(&ro, 0, sizeof ro);
    ro.buf = out;
    ro.cap = out_capacity;
    ro.runs = runs;
    ro.max_runs = max_runs;
    if (out && out_capacity)
        out[0] = '\0';

    while (*p) {
        if (*p == '$') {
            char name[32];
            size_t n = 0;
            const char *q = p + 1;

            while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                   (*q >= '0' && *q <= '9') || *q == '_') {
                if (n + 1 < sizeof name)
                    name[n++] = *q;
                q++;
            }
            name[n] = '\0';
            if (n == 0) {
                render_append(&ro, p, 1, bar_default_style(sb));
                p++;
                continue;
            }
            {
                int id = module_id(name);
                bool rendered = false;

                if (id >= 0)
                    rendered = render_module(sb, &sb->modules[id],
                                             state_value(state, id), &ro);
                if (!rendered && *q == ' ')
                    q++;
                p = q;
            }
        } else {
            const char *start = p;

            while (*p && *p != '$')
                p++;
            if (p > start)
                render_append(&ro, start, (size_t)(p - start),
                              bar_default_style(sb));
        }
    }
    return ro.run_count;
}
