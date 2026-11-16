/*
 * termica - fonts, the glyph cache and drawing text. See glyph.h.
 */

#include "glyph.h"

#include <stdlib.h>
#include <string.h>

/* Where the bold and italic faces usually live, given the regular path
 * without its extension: Windows uses a single letter (consolab.ttf), Nerd
 * Fonts spell it out. */
static const char *const BOLD_SUFFIXES[] = {
    "b.ttf", " Bold.ttf", "-Bold.ttf", "bold.ttf", NULL
};
static const char *const ITALIC_SUFFIXES[] = {
    "i.ttf", " Italic.ttf", "-Italic.ttf", "italic.ttf", NULL
};
static const char *const BOLD_ITALIC_SUFFIXES[] = {
    "bi.ttf", " Bold Italic.ttf", " BoldItalic.ttf", "z.ttf", NULL
};

/* Which cache slot and face to use for a set of attributes. Falls back to
 * the regular face when a face is missing. */
static int style_slot(const Glyphs *glyphs, bool bold, bool italic)
{
    if (bold && italic) {
        if (glyphs->bold_italic) return GLYPH_STYLE_BOLD_ITALIC;
        if (glyphs->bold)        return GLYPH_STYLE_BOLD;
        if (glyphs->italic)      return GLYPH_STYLE_ITALIC;
        return GLYPH_STYLE_REGULAR;
    }
    if (bold)
        return glyphs->bold ? GLYPH_STYLE_BOLD : GLYPH_STYLE_REGULAR;
    if (italic)
        return glyphs->italic ? GLYPH_STYLE_ITALIC : GLYPH_STYLE_REGULAR;
    return GLYPH_STYLE_REGULAR;
}

static TTF_Font *font_for_slot(const Glyphs *glyphs, int slot)
{
    switch (slot) {
    case GLYPH_STYLE_BOLD:        return glyphs->bold;
    case GLYPH_STYLE_ITALIC:      return glyphs->italic;
    case GLYPH_STYLE_BOLD_ITALIC: return glyphs->bold_italic;
    default:                      return glyphs->regular;
    }
}

/* Bake a texture from one rendered line of text. White is used so the draw
 * call can tint it. */
static SDL_Texture *bake_texture(SDL_Renderer *renderer, TTF_Font *font,
                                 const char *text, size_t length,
                                 float *out_width, float *out_height)
{
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surface;
    SDL_Texture *texture;

    /* TTF_RenderText_* returns a surface of exactly the font's line height
     * with the baseline at a fixed spot, so every glyph lines up when drawn
     * at the same y. That is what a monospace grid needs. */
    surface = TTF_RenderText_Blended(font, text, length, white);
    if (!surface)
        return NULL;
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        *out_width = (float)surface->w;
        *out_height = (float)surface->h;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    SDL_DestroySurface(surface);
    return texture;
}

/* The cached texture for one codepoint, or NULL when the font lacks it. */
static Glyph *glyph_lookup(Glyphs *glyphs, int slot, Uint32 codepoint)
{
    TTF_Font *font = font_for_slot(glyphs, slot);
    Glyph *glyph;
    char utf8[8];
    char *end;

    if (codepoint >= GLYPH_MAX_CODEPOINT || !font)
        return NULL;
    glyph = &glyphs->cache[(size_t)slot * GLYPH_MAX_CODEPOINT + codepoint];
    if (glyph->tried)
        return glyph->texture ? glyph : NULL;
    glyph->tried = true;

    if (!TTF_FontHasGlyph(font, codepoint))
        return NULL;

    end = SDL_UCS4ToUTF8(codepoint, utf8);
    *end = '\0';
    glyph->texture = bake_texture(glyphs->renderer, font, utf8, 0,
                                  &glyph->width, &glyph->height);
    return glyph->texture ? glyph : NULL;
}

static void draw_texture(Glyphs *glyphs, SDL_Texture *texture, float x, float y,
                         float width, float height, SDL_Color color)
{
    SDL_FRect destination;

    destination.x = x;
    destination.y = y;
    destination.w = width;
    destination.h = height;
    SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(texture, color.a);
    SDL_RenderTexture(glyphs->renderer, texture, NULL, &destination);
}

/* Look up a bold or italic face next to the regular one: strip the
 * extension and try the known spellings. */
static TTF_Font *open_variant(const char *font_path, float point_size,
                              const char *const *suffixes)
{
    char base_path[512];
    const char *extension;
    size_t suffix;

    extension = strrchr(font_path, '.');
    if (!extension || (size_t)(extension - font_path) >= sizeof base_path)
        return NULL;
    memcpy(base_path, font_path, (size_t)(extension - font_path));
    base_path[extension - font_path] = '\0';

    for (suffix = 0; suffixes[suffix]; suffix++) {
        char candidate_path[600];
        SDL_IOStream *probe;
        TTF_Font *font;

        SDL_snprintf(candidate_path, sizeof candidate_path, "%s%s", base_path,
                     suffixes[suffix]);
        probe = SDL_IOFromFile(candidate_path, "rb");
        if (!probe)
            continue;
        SDL_CloseIO(probe);
        font = TTF_OpenFont(candidate_path, point_size);
        if (font)
            return font;
    }
    return NULL;
}

bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale)
{
    float scaled_size = point_size * display_scale;
    int advance = 0;
    int height = 0;

    memset(glyphs, 0, sizeof *glyphs);
    glyphs->renderer = renderer;

    glyphs->cache = (Glyph *)calloc((size_t)GLYPH_STYLE_COUNT *
                                    GLYPH_MAX_CODEPOINT, sizeof(Glyph));
    if (!glyphs->cache)
        return false;

    glyphs->regular = TTF_OpenFont(font_path, scaled_size);
    if (!glyphs->regular) {
        glyphs_close(glyphs);
        return false;
    }
    TTF_SetFontHinting(glyphs->regular, TTF_HINTING_LIGHT);

    glyphs->bold = open_variant(font_path, scaled_size, BOLD_SUFFIXES);
    glyphs->italic = open_variant(font_path, scaled_size, ITALIC_SUFFIXES);
    glyphs->bold_italic = open_variant(font_path, scaled_size,
                                       BOLD_ITALIC_SUFFIXES);

    /* The cell is as wide as 'M' advances and as tall as the line. */
    if (!TTF_GetStringSize(glyphs->regular, "M", 0, &advance, &height) ||
        advance <= 0)
        advance = 10;
    glyphs->cell_width = advance;
    glyphs->cell_height = TTF_GetFontHeight(glyphs->regular);
    if (glyphs->cell_height <= 0)
        glyphs->cell_height = height > 0 ? height : 16;
    return true;
}

void glyphs_close(Glyphs *glyphs)
{
    int slot;
    Uint32 codepoint;

    if (glyphs->cache) {
        for (slot = 0; slot < GLYPH_STYLE_COUNT; slot++) {
            for (codepoint = 0; codepoint < GLYPH_MAX_CODEPOINT; codepoint++) {
                Glyph *glyph = &glyphs->cache[(size_t)slot *
                                              GLYPH_MAX_CODEPOINT + codepoint];
                if (glyph->texture)
                    SDL_DestroyTexture(glyph->texture);
            }
        }
        free(glyphs->cache);
    }
    if (glyphs->bold_italic)
        TTF_CloseFont(glyphs->bold_italic);
    if (glyphs->italic)
        TTF_CloseFont(glyphs->italic);
    if (glyphs->bold)
        TTF_CloseFont(glyphs->bold);
    if (glyphs->regular)
        TTF_CloseFont(glyphs->regular);
    memset(glyphs, 0, sizeof *glyphs);
}

void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color)
{
    const char *cursor;
    Uint32 first_codepoint;
    int slot;
    Glyph *glyph;

    if (length == 0)
        return;
    cursor = text;
    first_codepoint = SDL_StepUTF8(&cursor, NULL);
    if (first_codepoint == 0)
        return;

    slot = style_slot(glyphs, bold, italic);

    if ((size_t)(cursor - text) == length) {
        /* The usual case: one codepoint per cell, served from the cache. */
        glyph = glyph_lookup(glyphs, slot, first_codepoint);
        if (!glyph)
            return;
        draw_texture(glyphs, glyph->texture, x, y, glyph->width, glyph->height,
                     color);
        if (bold && !glyphs->bold) {
            /* No bold face: fake it with a second copy one pixel over. */
            draw_texture(glyphs, glyph->texture, x + 1.0f, y, glyph->width,
                         glyph->height, color);
        }
        return;
    }

    /* A grapheme cluster (a base character plus combining marks, an emoji
     * with a ZWJ, ...): render the whole cluster, uncached. */
    {
        TTF_Font *font = font_for_slot(glyphs, slot);
        SDL_Texture *texture;
        float width = 0.0f;
        float height = 0.0f;

        texture = bake_texture(glyphs->renderer, font, text, length,
                               &width, &height);
        if (!texture)
            return;
        draw_texture(glyphs, texture, x, y, width, height, color);
        SDL_DestroyTexture(texture);
    }
}
