/*
 * termica - fonts, the glyph cache and drawing text.
 *
 * One white texture per codepoint and style (regular, bold, italic,
 * bold-italic), tinted at draw time. The cache covers the Basic
 * Multilingual Plane, which is enough for ASCII, box drawing and Nerd Font
 * icons.
 *
 * SDL_ttf does the rasterising. A bold, italic or bold-italic face is
 * looked up next to the regular one (consola.ttf -> consolab.ttf, "... Nerd
 * Font.ttf" -> "... Nerd Font Bold.ttf"); when there is no bold face the
 * glyph is simply drawn twice, one pixel apart.
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

    /* [GLYPH_STYLE_COUNT][GLYPH_MAX_CODEPOINT], heap allocated because it
     * is several megabytes. */
    Glyph *cache;

    /* One cell of the grid, from the font metrics. */
    int cell_width;
    int cell_height;
} Glyphs;

/* Load the font and its optional faces. `point_size` is in points and is
 * scaled by `display_scale`. False on failure (with SDL's error set). */
bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale);

/* Free every texture and font. Safe on a zeroed struct. */
void glyphs_close(Glyphs *glyphs);

/* Does the regular face carry this codepoint? */
bool glyphs_has(const Glyphs *glyphs, Uint32 codepoint);

/* Draw one cell's text at (x, y). `text` is UTF-8 and may be a whole
 * grapheme cluster; `length` is in bytes. */
void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color);

#endif /* TERMICA_GLYPH_H */
