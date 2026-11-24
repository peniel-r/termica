/*
 * termica - colour schemes (default dark, bundled light, and [colors] config).
 *
 * Strict C99.
 */
#ifndef TERMICA_COLORS_H
#define TERMICA_COLORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ColorRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ColorRgb;

typedef struct ColorScheme {
    char     scheme_name[32];    /* "", "dark", or "light" */
    ColorRgb foreground;
    ColorRgb background;
    ColorRgb cursor;
    bool     has_selection_fg;
    ColorRgb selection_fg;
    bool     has_selection_bg;
    ColorRgb selection_bg;
    ColorRgb palette[16];
    bool     custom;             /* true if user configured [colors] */
} ColorScheme;

/* Load a bundled preset ("dark" or "light"). Unknown names return false and
 * leave `scheme` untouched. */
bool colors_preset(ColorScheme *scheme, const char *name);

/* Parse "#RRGGBB" or "RRGGBB" into `out`. */
bool colors_parse_hex(const char *text, ColorRgb *out);

/* Format `c` as "#rrggbb". */
void colors_format_hex(ColorRgb c, char *out, size_t capacity);

#endif /* TERMICA_COLORS_H */
