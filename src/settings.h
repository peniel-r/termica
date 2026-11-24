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
#include <stdint.h>

#include "colors.h"
#include "keys.h"

#define SETTINGS_MAX_FALLBACKS     8
#define SETTINGS_MAX_ARGS          16
#define SETTINGS_ARG_MAX           256
#define SETTINGS_MAX_ENV           32
#define SETTINGS_ENV_KEY_MAX       64
#define SETTINGS_ENV_VAL_MAX       512
#define SETTINGS_MAX_KEY_OVERRIDES 32

typedef struct SettingsEnv {
    char key[SETTINGS_ENV_KEY_MAX];
    char value[SETTINGS_ENV_VAL_MAX];
} SettingsEnv;

typedef struct SettingsKeyOverride {
    char chord[64];
    char action[32];
} SettingsKeyOverride;

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

/* What to do when the terminal receives BEL (0x07). */
typedef enum {
    SETTINGS_BELL_VISUAL = 0,
    SETTINGS_BELL_TASKBAR,
    SETTINGS_BELL_NONE
} SettingsBell;

/* How much frame the window draws around the terminal. */
typedef enum {
    SETTINGS_DECORATION_ROUNDED = 0,  /* the custom rounded panel  */
    SETTINGS_DECORATION_SQUARE,       /* square custom frame       */
    SETTINGS_DECORATION_COMPACT,      /* rounded, less top inset   */
    SETTINGS_DECORATION_NATIVE         /* the Windows title bar     */
} SettingsDecoration;

typedef struct Settings {
    char font[512];        /* empty = the built-in Maple Mono NF     */
    float point_size;      /* default 11                             */
    char font_fallback[SETTINGS_MAX_FALLBACKS][256];
    int  font_fallback_count;
    float panel_opacity;   /* default 1.0, 0..1                      */
    int backdrop;          /* WindowBackdrop value, default mica     */
    int decoration;        /* SettingsDecoration, default native     */
    bool title_bar;        /* draw a custom title bar, default true  */
    bool copy_on_select;   /* default false                          */
    SettingsShell shell;   /* default auto                           */
    SettingsOsc52 osc52;   /* default ask                            */
    SettingsBell bell;     /* default visual                         */

    /* Colour scheme (PLAN 1.6). */
    ColorScheme colors;

    /* Custom shell profile (PLAN 1.1). When command_argc > 0 it wins over
     * `shell`. `cwd` overrides the inherited working directory when non-empty.
     * `env` entries overlay the child's environment. */
    char command[SETTINGS_MAX_ARGS][SETTINGS_ARG_MAX];
    int  command_argc;
    char cwd[512];
    SettingsEnv env[SETTINGS_MAX_ENV];
    int  env_count;

    /* Key bindings (PLAN 1.2). `keys` holds defaults plus any `[keys]`
     * overrides; `key_overrides` preserves the user's overrides for save. */
    KeyMap keys;
    SettingsKeyOverride key_overrides[SETTINGS_MAX_KEY_OVERRIDES];
    int  key_override_count;
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

/* Where a named file in the same config directory lives (termica.toml,
 * statusbar.toml, ...). False when no config directory could be worked
 * out. */
bool config_path(const char *name, char *out, size_t capacity);

/* Create the directory part of a config file path, when needed. */
void config_create_directory(const char *path);

/* FNV-1a fingerprint used by the safe-save logic. */
uint64_t config_hash_bytes(const char *data, size_t length);

/* A missing file is not an error: the defaults stay in place and false is
 * returned so the app knows nothing was read. */
bool settings_load(Settings *settings);

/* Write the file, creating the directory when needed. */
bool settings_save(const Settings *settings);

#endif /* TERMICA_SETTINGS_H */
