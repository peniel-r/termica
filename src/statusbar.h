/*
 * termica - the bottom status bar.
 *
 * The bar is configured by statusbar.toml, which lives next to
 * termica.toml and is parsed with tomlc17 the same way. The file uses the
 * Starship model: `format`, `center_format` and `right_format` are the left,
 * centre and right containers, each a string of `$module` tokens and literal
 * text, and one `[module]` table per module carries its own format, symbol,
 * style and options.
 *
 *   format = "$directory$git_branch$git_status $cmd_status$cmd_duration"
 *   center_format = "$busy"
 *   right_format = "$time"
 *
 *   [font]
 *   face = "GeistMono NF"
 *   size = 9
 *
 *   [directory]
 *   style = "fg:#9ccfd8 bold"
 *   truncation_length = 3
 *
 * A module's `format` is a mini template: `[text]($style)` groups carry a
 * style (the literal `$style` means "the module's own style"), and `$symbol`
 * / `$value` substitute the module's glyph and content. A single space that
 * directly follows a `$module` token is dropped when the module rendered
 * empty, which is what lets `$git_branch$git_status ` collapse outside a
 * repository. The busy slot always reserves its cell: while a command runs it
 * shows the next fixed braille frame, and at rest it shows `[busy] idle` if
 * that is set, otherwise the fixed resting icon.
 *
 * Strict C99.
 */

#ifndef TERMICA_STATUSBAR_H
#define TERMICA_STATUSBAR_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One styled run of a rendered line. */
typedef struct StatusbarStyle {
    SDL_Color fg;
    bool      has_fg;
    SDL_Color bg;
    bool      has_bg;
    bool      bold;
    bool      italic;
    bool      underline;
    bool      dim;
} StatusbarStyle;

typedef enum {
    STATUSBAR_DIRECTORY = 0,
    STATUSBAR_GIT_BRANCH,
    STATUSBAR_GIT_STATUS,
    STATUSBAR_CMD_STATUS,
    STATUSBAR_CMD_DURATION,
    STATUSBAR_TIME,
    STATUSBAR_BUSY,
    STATUSBAR_MODULE_COUNT
} StatusbarModuleId;

typedef struct StatusbarModule {
    char format[256];          /* the [text]($style) template          */
    char symbol[32];           /* UTF-8 glyph shown before the value   */
    char style[64];            /* style string; parsed_style is it     */
    StatusbarStyle parsed_style;

    /* [directory] */
    int  truncation_length;    /* trailing components, 0 = full        */
    char home_symbol[32];

    /* [git_status] */
    char ahead[16];
    char behind[16];
    char diverged[16];
    char dirty[16];
    char clean[16];
    char conflicted[16];

    /* [cmd_status] */
    char style_success[64];
    char style_failure[64];
    StatusbarStyle parsed_success;
    StatusbarStyle parsed_failure;
    char symbol_success[32];
    char symbol_failure[32];

    /* [cmd_duration] */
    int min_time_ms;

    /* [time] */
    char time_format[64];

    /* [busy] — only `idle` is configurable. The spinner frames and the
     * resting icon are fixed constants (see statusbar_busy_frames and
     * statusbar_busy_idle_default); `idle` optionally overrides the resting
     * icon when the user sets it. */
    char idle[32];             /* resting glyph override, "" = default */
} StatusbarModule;

typedef struct Statusbar {
    char font[512];            /* empty = built-in Maple Mono NF       */
    float point_size;          /* default 9, never below 9             */
    char format[512];          /* left container                       */
    char center_format[512];   /* center container                     */
    char right_format[512];    /* right container                      */
    SDL_Color bar_fg;
    SDL_Color bar_bg;
    float bar_opacity;
    StatusbarModule modules[STATUSBAR_MODULE_COUNT];
} Statusbar;

void statusbar_default(Statusbar *statusbar);

/* Parse a TOML document and overlay the recognised settings. Unknown keys,
 * wrong types and out-of-range values are ignored; a syntax error leaves
 * the settings untouched. Exposed for the self test. */
bool statusbar_parse_toml(Statusbar *statusbar, const char *text,
                          size_t length);

/* Render the settings as TOML, for saving. */
void statusbar_format(const Statusbar *statusbar, char *out, size_t capacity);

/* Where the file lives. False when no config directory could be worked
 * out, in which case nothing is read or written. */
bool statusbar_path(char *out, size_t capacity);

/* A missing file is not an error: the defaults stay in place and false is
 * returned so the app knows nothing was read. */
bool statusbar_load(Statusbar *statusbar);

/* Write the file, creating the directory when needed. */
bool statusbar_save(const Statusbar *statusbar);

/* Parse a style string: `fg:#RRGGBB`, `bg:#RRGGBB`, `bold`, `italic`,
 * `underline`, `dim`. Returns false when nothing was recognised. */
bool statusbar_style_parse(const char *text, StatusbarStyle *out);

/* ------------------------------------------------------------------------- */
/* Pure helpers the app and self test share                                   */
/* ------------------------------------------------------------------------- */

/* A path shortened for the directory module: `home_symbol` replaces the
 * home directory, then only the last `truncation_length` components are
 * kept (0 keeps the whole path). */
void statusbar_shorten_path(const Statusbar *statusbar, const char *path,
                            char *out, size_t capacity);

/* A command duration as text: "750ms", "12s", "3m 5s". */
void statusbar_format_duration(uint64_t ms, char *out, size_t capacity);

/* Format the local time with strftime into `out`. False when `out` would
 * overflow; the result is still usable (truncated). */
bool statusbar_format_time(const char *format, char *out, size_t capacity);

/* The git status glyphs for the given counts, empty when the repository is
 * clean and nothing is ahead or behind. */
void statusbar_git_status_text(const Statusbar *statusbar, int ahead,
                               int behind, int dirty, int conflicted,
                               bool clean, char *out, size_t capacity);

/* Fill `frames` with the fixed spinner frames. Returns the frame count.
 * The animation is not configurable; only the resting icon is (see
 * statusbar_busy_idle_default and the `[busy] idle` setting). */
int statusbar_busy_frames(char frames[][8], int max_frames);

/* The fixed resting icon shown when idle and `[busy] idle` is not set. */
const char *statusbar_busy_idle_default(void);

/* The scale the fixed resting icon is drawn at. That glyph's ink box is
 * narrower than the Font Awesome icons used elsewhere in the bar (the
 * folder symbol, for one), so it is enlarged to match their optical size.
 * A `[busy] idle` override is drawn at its natural size. */
float statusbar_busy_idle_scale(void);

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

/* What a module evaluates to for this frame. `style` overrides the module's
 * configured style when non-NULL (used by cmd_status success/failure). */
typedef struct StatusbarModuleValue {
    const char *value;         /* substituted for $value, NULL = ""   */
    const char *symbol;        /* substituted for $symbol, NULL = module's */
    const StatusbarStyle *style;
    float scale;               /* glyph scale, 0 = natural size       */
} StatusbarModuleValue;

typedef struct StatusbarState {
    StatusbarModuleValue directory;
    StatusbarModuleValue git_branch;
    StatusbarModuleValue git_status;
    StatusbarModuleValue cmd_status;
    StatusbarModuleValue cmd_duration;
    StatusbarModuleValue time;
    StatusbarModuleValue busy;
} StatusbarState;

/* One styled run inside a rendered line. `start` and `length` index `out`
 * (bytes), which `statusbar_render` fills. */
typedef struct StatusbarRun {
    int start;
    int length;
    StatusbarStyle style;
    float scale;               /* glyph scale, 0 = natural size       */
} StatusbarRun;

/* Render `format` (a `$module`/literal string) into `out` and record the
 * styled runs. Returns the number of runs; 0 means the line is empty. */
int statusbar_render(const Statusbar *statusbar, const StatusbarState *state,
                     const char *format, char *out, size_t out_capacity,
                     StatusbarRun *runs, int max_runs);

#endif /* TERMICA_STATUSBAR_H */
