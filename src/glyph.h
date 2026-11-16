/*
 * termica - fonts, the glyph cache and drawing text.
 *
 * One white texture per codepoint and style (regular, bold, italic,
 * bold-italic), tinted at draw time. The cache covers the Basic
 * Multilingual Plane, which is enough for ASCII, box drawing and Nerd Font
 * icons.
 *
 * The default font is Google Sans Code NF Mono, compiled into the
 * executable. A path passed on the command line still wins; if it cannot be
 * opened, the built-in family is used instead, and its regular face also
 * fills in codepoints the chosen font lacks.
 *
 * SDL_ttf does the rasterising. For a font from disk, bold, italic or
 * bold-italic faces are looked up next to the regular one (consola.ttf ->
 * consolab.ttf, "... Nerd Font.ttf" -> "... Nerd Font Bold.ttf"); when
 * there is no bold face the glyph is simply drawn twice, one pixel apart.
 *
 * Strict C99.
 */

#ifndef TERMICA_GLYPH_H
#define TERMICA_GLYPH_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <stdbool.h>
#include <stddef.h>

/* Cache slots: one per combination of bold and italic. */
#define GLYPH_STYLE_REGULAR      0
#define GLYPH_STYLE_BOLD         1
#define GLYPH_STYLE_ITALIC       2
#define GLYPH_STYLE_BOLD_ITALIC  3
#define GLYPH_STYLE_COUNT        4

/* The cache covers the Basic Multilingual Plane. */
#define GLYPH_MAX_CODEPOINT      0x10000

typedef struct Glyph {
    SDL_Texture *texture;
    float width;
    float height;
    bool tried;
} Glyph;

typedef struct Glyphs {
    SDL_Renderer *renderer;
    TTF_Font *regular;
    TTF_Font *bold;
    TTF_Font *italic;
    TTF_Font *bold_italic;

    /* The built-in regular face, used for codepoints the chosen font does
     * not carry (Nerd icons in Consolas, for example). NULL when the
     * built-in family is the main font. */
    TTF_Font *fallback;

    /* True when the main family is the font compiled into the executable
     * (no path, or the requested path could not be opened). */
    bool built_in;

    /* [GLYPH_STYLE_COUNT][GLYPH_MAX_CODEPOINT], heap allocated because it
     * is several megabytes. */
    Glyph *cache;

    /* One cell of the grid, from the font metrics. */
    int cell_width;
    int cell_height;
} Glyphs;

/* Load the font and its optional faces. `font_path` may be NULL to use the
 * built-in family; a path that cannot be opened falls back to it too.
 * `point_size` is in points and is scaled by `display_scale`. False on
 * failure (with SDL's error set). */
bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale);

/* Free every texture and font. Safe on a zeroed struct. */
void glyphs_close(Glyphs *glyphs);

/* Draw one cell's text at (x, y). `text` is UTF-8 and may be a whole
 * grapheme cluster; `length` is in bytes. */
void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color);

#endif /* TERMICA_GLYPH_H */
