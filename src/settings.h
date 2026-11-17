/*
 * termica - the user's settings.
 *
 * One TOML file, parsed with tomlc17:
 * %APPDATA%\termica\termica.toml on Windows,
 * $XDG_CONFIG_HOME/termica/termica.toml or ~/.config/termica/termica.toml
 * elsewhere. Whatever is not there keeps its default, so a missing or
 * half-written file still starts the app.
 *
 * Strict C99.
 */

#ifndef TERMICA_SETTINGS_H
#define TERMICA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

/* Which program to start. */
typedef enum {
    SETTINGS_SHELL_AUTO = 0,   /* pwsh, else powershell, else cmd      */
    SETTINGS_SHELL_PWSH,
    SETTINGS_SHELL_WINDOWS_POWERSHELL,
    SETTINGS_SHELL_CMD
} SettingsShell;

/* What to do with an OSC 52 clipboard write request. */
typedef enum {
    SETTINGS_OSC52_ASK = 0,
    SETTINGS_OSC52_ALLOW,
    SETTINGS_OSC52_DENY
} SettingsOsc52;

typedef struct Settings {
    char font[512];        /* empty = the built-in Maple Mono NF     */
    float point_size;      /* default 16                             */
    float panel_opacity;   /* default 0.80, 0..1                     */
    int backdrop;          /* WindowBackdrop value, default panel    */
    bool copy_on_select;   /* default false                          */
    SettingsShell shell;   /* default auto                           */
    SettingsOsc52 osc52;   /* default ask                            */
} Settings;

void settings_default(Settings *settings);

/* Parse a TOML document and overlay the recognised settings. Unknown keys,
 * wrong types and out-of-range values are ignored; a syntax error leaves
 * the settings untouched. Exposed for the self test. */
bool settings_parse_toml(Settings *settings, const char *text, size_t length);

/* Render the settings as TOML, for saving. */
void settings_format(const Settings *settings, char *out, size_t capacity);

/* Where the file lives. False when no config directory could be worked
 * out, in which case nothing is read or written. */
bool settings_path(char *out, size_t capacity);

/* A missing file is not an error: the defaults stay in place and false is
 * returned so the app knows nothing was read. */
bool settings_load(Settings *settings);

/* Write the file, creating the directory when needed. */
bool settings_save(const Settings *settings);

#endif /* TERMICA_SETTINGS_H */
