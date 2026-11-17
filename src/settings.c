/*
 * termica - the user's settings. See settings.h.
 *
 * The format is deliberately the boring one: `key = value` per line, `#`
 * starts a comment, unknown keys are ignored. That keeps the file editable
 * by hand, needs no parser library, and lets a half-written line cost at
 * most one setting.
 */

#include "settings.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <stdio.h>
#include <string.h>

#define SETTINGS_APP_DIR "termica"
#define SETTINGS_FILE_NAME "termica.conf"

/* Longest file we are willing to read; the real one is a dozen lines. */
#define SETTINGS_MAX_BYTES 8192

void settings_default(Settings *settings)
{
    memset(settings, 0, sizeof *settings);
    settings->point_size = 16.0f;
    settings->panel_opacity = 0.80f;
    settings->backdrop = 0;             /* WINDOW_BACKDROP_PANEL       */
    settings->copy_on_select = false;
    settings->shell = SETTINGS_SHELL_AUTO;
    settings->osc52 = SETTINGS_OSC52_ASK;
}

static void trim(char *text)
{
    size_t length;
    char *start = text;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1);
    length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' ||
                          text[length - 1] == '\t' ||
                          text[length - 1] == '\r' ||
                          text[length - 1] == '\n'))
        text[--length] = '\0';
}

static bool bool_value(const char *value, bool *out)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        strcmp(value, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
        strcmp(value, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void fold_key(const char *text, char *out, size_t capacity)
{
    size_t read = 0;
    size_t written = 0;

    for (; text[read] && written + 1 < capacity; read++) {
        char c = text[read];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        /* `-` and `_` are the same separator, and a name written without
         * any separator at all means the same thing: Copy-On-Select,
         * copy_on_select and copyonselect all name one key. */
        if (c == '-' || c == '_')
            continue;
        out[written++] = c;
    }
    out[written] = '\0';
}

static bool settings_value(const char *key, const char *name)
{
    char folded_key[40];
    char folded_name[40];

    fold_key(key, folded_key, sizeof folded_key);
    fold_key(name, folded_name, sizeof folded_name);
    return strcmp(folded_key, folded_name) == 0;
}

bool settings_parse_line(Settings *settings, const char *raw_line)
{
    char line[600];
    char *equals;
    char *key;
    char *value;
    float number;

    if (!settings || !raw_line)
        return false;
    if (raw_line[0] == '#' || raw_line[0] == '\0')
        return false;
    if (strlen(raw_line) >= sizeof line)
        return false;
    strcpy(line, raw_line);

    equals = strchr(line, '=');
    if (!equals)
        return false;
    *equals = '\0';
    key = line;
    value = equals + 1;
    trim(key);
    trim(value);
    if (key[0] == '\0')
        return false;

    if (settings_value(key, "font")) {
        if (strlen(value) >= sizeof settings->font)
            return false;
        strcpy(settings->font, value);
        return true;
    }
    if (settings_value(key, "size")) {
        number = (float)SDL_atof(value);
        if (number <= 0.0f || number > 200.0f)
            return false;
        settings->point_size = number;
        return true;
    }
    if (settings_value(key, "opacity")) {
        number = (float)SDL_atof(value);
        if (number < 0.0f || number > 1.0f)
            return false;
        settings->panel_opacity = number;
        return true;
    }
    if (settings_value(key, "backdrop")) {
        if (strcmp(value, "panel") == 0)
            settings->backdrop = 0;
        else if (strcmp(value, "mica") == 0)
            settings->backdrop = 1;
        else if (strcmp(value, "mica_alt") == 0 || strcmp(value, "mica-alt") == 0)
            settings->backdrop = 2;
        else if (strcmp(value, "acrylic") == 0)
            settings->backdrop = 3;
        else
            return false;
        return true;
    }
    if (settings_value(key, "copy_on_select"))
        return bool_value(value, &settings->copy_on_select);
    if (settings_value(key, "shell")) {
        if (strcmp(value, "auto") == 0)
            settings->shell = SETTINGS_SHELL_AUTO;
        else if (strcmp(value, "pwsh") == 0)
            settings->shell = SETTINGS_SHELL_PWSH;
        else if (strcmp(value, "powershell") == 0)
            settings->shell = SETTINGS_SHELL_WINDOWS_POWERSHELL;
        else if (strcmp(value, "cmd") == 0)
            settings->shell = SETTINGS_SHELL_CMD;
        else
            return false;
        return true;
    }
    if (settings_value(key, "osc52")) {
        if (strcmp(value, "ask") == 0)
            settings->osc52 = SETTINGS_OSC52_ASK;
        else if (strcmp(value, "allow") == 0)
            settings->osc52 = SETTINGS_OSC52_ALLOW;
        else if (strcmp(value, "deny") == 0)
            settings->osc52 = SETTINGS_OSC52_DENY;
        else
            return false;
        return true;
    }
    return false;
}

void settings_format(const Settings *settings, char *out, size_t capacity)
{
    const char *shell = "auto";
    const char *osc52 = "ask";
    const char *backdrop = "panel";

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

    snprintf(out, capacity,
             "# termica settings\n"
             "font = %s\n"
             "size = %g\n"
             "opacity = %g\n"
             "backdrop = %s\n"
             "copy_on_select = %s\n"
             "shell = %s\n"
             "osc52 = %s\n",
             settings->font, (double)settings->point_size,
             (double)settings->panel_opacity, backdrop,
             settings->copy_on_select ? "true" : "false", shell, osc52);
}

bool settings_path(char *out, size_t capacity)
{
    const char *base = NULL;
    char directory[512];

#ifdef _WIN32
    base = SDL_getenv("APPDATA");
    if (!base || !base[0])
        return false;
    snprintf(directory, sizeof directory, "%s\\%s", base, SETTINGS_APP_DIR);
#else
    base = SDL_getenv("XDG_CONFIG_HOME");
    if (base && base[0])
        snprintf(directory, sizeof directory, "%s/%s", base, SETTINGS_APP_DIR);
    else {
        const char *home = SDL_getenv("HOME");

        if (!home || !home[0])
            return false;
        snprintf(directory, sizeof directory, "%s/.config/%s", home,
                 SETTINGS_APP_DIR);
    }
#endif

    if (strlen(directory) + strlen(SETTINGS_FILE_NAME) + 2 > capacity)
        return false;
    snprintf(out, capacity, "%s/%s", directory, SETTINGS_FILE_NAME);
    return true;
}

bool settings_load(Settings *settings)
{
    char path[512];
    char directory[512];
    char *contents;
    size_t length = 0;
    char *line;
    char *next;
    bool read_any = false;

    settings_default(settings);
    if (!settings_path(path, sizeof path))
        return false;

    /* The directory has to exist before saving, and creating it here costs
     * nothing when the file is already there. */
    strncpy(directory, path, sizeof directory - 1);
    directory[sizeof directory - 1] = '\0';
    next = strrchr(directory, '/');
    if (!next)
        next = strrchr(directory, '\\');
    if (next) {
        *next = '\0';
        if (!SDL_CreateDirectory(directory))
            return false;
    }

    contents = (char *)SDL_LoadFile(path, &length);
    if (!contents)
        return false;
    if (length > SETTINGS_MAX_BYTES)
        length = SETTINGS_MAX_BYTES;
    contents[length] = '\0';

    line = contents;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next)
            *next = '\0';
        if (settings_parse_line(settings, line))
            read_any = true;
        line = next ? next + 1 : NULL;
    }

    SDL_free(contents);
    return read_any;
}

bool settings_save(const Settings *settings)
{
    char path[512];
    char text[2048];
    SDL_IOStream *stream;

    if (!settings_path(path, sizeof path))
        return false;
    settings_format(settings, text, sizeof text);
    stream = SDL_IOFromFile(path, "w");
    if (!stream)
        return false;
    if (SDL_WriteIO(stream, text, strlen(text)) != strlen(text)) {
        SDL_CloseIO(stream);
        return false;
    }
    return SDL_CloseIO(stream);
}
