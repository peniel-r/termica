/*
 * termica - fonts, the glyph cache and drawing text. See glyph.h.
 */

#include "glyph.h"

#include "embed.h"
#include "font_lookup.h"

#include <stdio.h>
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

/* Standard OS fallback families for colour emoji, symbols and CJK. */
static const char *const SYSTEM_FALLBACK_FAMILIES[] = {
    "Segoe UI Emoji",
    "Segoe UI Symbol",
    "Microsoft YaHei",
    "Yu Gothic",
    "Malgun Gothic",
    "SimSun",
    NULL
};

static bool path_is_color_emoji(const char *path)
{
    if (!path)
        return false;
    return SDL_strcasestr(path, "emoji") != NULL ||
           SDL_strcasestr(path, "seguiemj") != NULL;
}

int glyphs_resolve_fallback_paths(const char *const *user_fallbacks,
                                  int user_count, char out[][256],
                                  int max_out)
{
    int written = 0;
    int i;

    if (!out || max_out <= 0)
        return 0;

    for (i = 0; i < user_count && written < max_out; i++) {
        char resolved[512];

        if (!user_fallbacks || !user_fallbacks[i] || !user_fallbacks[i][0])
            continue;
        if (font_lookup_path(user_fallbacks[i], resolved, sizeof resolved) &&
            strlen(resolved) < 256) {
            snprintf(out[written++], 256, "%s", resolved);
        }
    }

    for (i = 0; SYSTEM_FALLBACK_FAMILIES[i] && written < max_out; i++) {
        char resolved[512];
        int j;
        bool duplicate = false;

        if (!font_lookup_path(SYSTEM_FALLBACK_FAMILIES[i], resolved,
                              sizeof resolved) ||
            strlen(resolved) >= 256)
            continue;
        for (j = 0; j < written; j++) {
            if (SDL_strcasecmp(out[j], resolved) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            snprintf(out[written++], 256, "%s", resolved);
    }
    return written;
}

/* The face that can draw this codepoint: primary -> built-in Maple Mono ->
 * user fallbacks -> OS CJK/emoji fallbacks -> Symbols Nerd Font Mono. */
static TTF_Font *font_with_glyph(const Glyphs *glyphs, int slot,
                                 Uint32 codepoint, bool *out_color)
{
    TTF_Font *font = font_for_slot(glyphs, slot);
    int i;

    if (out_color)
        *out_color = false;
    if (font && TTF_FontHasGlyph(font, codepoint))
        return font;
    if (glyphs->fallback && TTF_FontHasGlyph(glyphs->fallback, codepoint))
        return glyphs->fallback;
    for (i = 0; i < glyphs->user_fallback_count; i++) {
        if (glyphs->user_fallbacks[i] &&
            TTF_FontHasGlyph(glyphs->user_fallbacks[i], codepoint)) {
            if (out_color)
                *out_color = glyphs->user_fallback_color[i];
            return glyphs->user_fallbacks[i];
        }
    }
    for (i = 0; i < glyphs->sys_fallback_count; i++) {
        if (glyphs->sys_fallbacks[i] &&
            TTF_FontHasGlyph(glyphs->sys_fallbacks[i], codepoint)) {
            if (out_color)
                *out_color = glyphs->sys_fallback_color[i];
            return glyphs->sys_fallbacks[i];
        }
    }
    if (glyphs->symbol_fallback &&
        TTF_FontHasGlyph(glyphs->symbol_fallback, codepoint))
        return glyphs->symbol_fallback;
    return NULL;
}

static Glyph *glyph_lookup_extra(Glyphs *glyphs, int slot, Uint32 codepoint)
{
    uint32_t hash = (codepoint * 2654435761u) ^ ((uint32_t)slot * 40503u);
    uint32_t mask = GLYPH_EXTRA_BUCKETS - 1;
    uint32_t probe;

    if (!glyphs->extra_cache)
        return NULL;

    for (probe = 0; probe < GLYPH_EXTRA_BUCKETS; probe++) {
        GlyphExtraEntry *entry = &glyphs->extra_cache[(hash + probe) & mask];

        if (entry->codepoint == codepoint && entry->slot == (uint8_t)slot)
            return entry->glyph.texture ? &entry->glyph : NULL;
        if (entry->codepoint == 0) {
            bool is_color = false;
            TTF_Font *font = font_with_glyph(glyphs, slot, codepoint,
                                             &is_color);
            char utf8[8];
            char *end;

            entry->codepoint = codepoint;
            entry->slot = (uint8_t)slot;
            entry->glyph.tried = true;
            entry->glyph.is_color = is_color;
            if (!font)
                return NULL;
            end = SDL_UCS4ToUTF8(codepoint, utf8);
            *end = '\0';
            entry->glyph.texture = bake_texture(glyphs->renderer, font, utf8, 0,
                                                &entry->glyph.width,
                                                &entry->glyph.height);
            return entry->glyph.texture ? &entry->glyph : NULL;
        }
    }
    return NULL;
}

/* The cached texture for one codepoint (BMP or out-of-BMP), or NULL when no
 * face has it. */
static Glyph *glyph_lookup(Glyphs *glyphs, int slot, Uint32 codepoint)
{
    TTF_Font *font;
    Glyph *glyph;
    bool is_color = false;
    char utf8[8];
    char *end;

    if (codepoint >= GLYPH_MAX_CODEPOINT)
        return glyph_lookup_extra(glyphs, slot, codepoint);
    glyph = &glyphs->cache[(size_t)slot * GLYPH_MAX_CODEPOINT + codepoint];
    if (glyph->tried)
        return glyph->texture ? glyph : NULL;
    glyph->tried = true;

    font = font_with_glyph(glyphs, slot, codepoint, &is_color);
    if (!font)
        return NULL;
    glyph->is_color = is_color;

    end = SDL_UCS4ToUTF8(codepoint, utf8);
    *end = '\0';
    glyph->texture = bake_texture(glyphs->renderer, font, utf8, 0,
                                  &glyph->width, &glyph->height);
    return glyph->texture ? glyph : NULL;
}

static void draw_texture(Glyphs *glyphs, SDL_Texture *texture, float x, float y,
                         float width, float height, SDL_Color color,
                         float scale, bool is_color)
{
    SDL_FRect destination;
    float fit = 1.0f;
    float drawn_width;
    float drawn_height;

    /* If a fallback font's glyph is taller than the grid cell (common with
     * CJK or emoji faces), scale it down to fit the row height cleanly. */
    if (glyphs->cell_height > 0 && height > (float)glyphs->cell_height * 1.15f)
        fit = (float)glyphs->cell_height / height;

    drawn_width = width * fit * scale;
    drawn_height = height * fit * scale;

    /* Scaling happens about the texture's centre: the glyph stays on the
     * baseline and inside its cell's optical middle while the advance is
     * unchanged. */
    destination.x = x - (drawn_width - width * fit) / 2.0f;
    destination.y = y + ((float)glyphs->cell_height - drawn_height) / 2.0f;
    if (scale == 1.0f && fit == 1.0f) {
        destination.x = x;
        destination.y = y;
    }
    destination.w = drawn_width;
    destination.h = drawn_height;
    if (is_color) {
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, color.a);
    } else {
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
    }
    SDL_RenderTexture(glyphs->renderer, texture, NULL, &destination);
}

static TTF_Font *try_variant_suffixes(const char *base_path, float point_size,
                                     const char *const *suffixes)
{
    size_t suffix;

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

/* Look up a bold or italic face next to the regular one: strip the
 * extension and try the known spellings. Nerd Fonts name the regular face
 * "...-Regular", so retry once without that suffix to find "...-Bold". */
static TTF_Font *open_variant(const char *font_path, float point_size,
                              const char *const *suffixes)
{
    char base_path[512];
    const char *extension;
    size_t base_length;
    TTF_Font *font;

    extension = strrchr(font_path, '.');
    if (!extension || (size_t)(extension - font_path) >= sizeof base_path)
        return NULL;
    memcpy(base_path, font_path, (size_t)(extension - font_path));
    base_path[extension - font_path] = '\0';

    font = try_variant_suffixes(base_path, point_size, suffixes);
    if (font)
        return font;

    base_length = strlen(base_path);
    if (base_length >= 8 &&
        (SDL_strcasecmp(base_path + base_length - 8, "-Regular") == 0 ||
         SDL_strcasecmp(base_path + base_length - 8, " Regular") == 0)) {
        base_path[base_length - 8] = '\0';
        return try_variant_suffixes(base_path, point_size, suffixes);
    }
    return NULL;
}

/* Open one face from the executable's resources. Null when this build has
 * no embedded font or the resource is missing. */
static TTF_Font *open_embedded(EmbeddedFontFace face, float point_size)
{
    const void *data = NULL;
    size_t size = 0;
    SDL_IOStream *stream;

    if (!embedded_font(face, &data, &size) || !data || size == 0)
        return NULL;
    stream = SDL_IOFromConstMem(data, size);
    if (!stream)
        return NULL;
    /* TTF_OpenFontIO closes the stream itself when loading fails. */
    return TTF_OpenFontIO(stream, true, point_size);
}

/* The built-in family. False when the executable does not carry it. */
static bool open_embedded_family(Glyphs *glyphs, float point_size)
{
    glyphs->regular = open_embedded(EMBEDDED_FONT_REGULAR, point_size);
    if (!glyphs->regular)
        return false;
    glyphs->bold = open_embedded(EMBEDDED_FONT_BOLD, point_size);
    glyphs->italic = open_embedded(EMBEDDED_FONT_ITALIC, point_size);
    glyphs->bold_italic = open_embedded(EMBEDDED_FONT_BOLD_ITALIC, point_size);
    return true;
}

bool glyphs_open_ex(Glyphs *glyphs, SDL_Renderer *renderer,
                    const char *font_path,
                    const char *const *user_fallbacks, int user_fallback_count,
                    float point_size, float display_scale)
{
    float scaled_size = point_size * display_scale;
    int advance = 0;
    int height = 0;
    int i;

    memset(glyphs, 0, sizeof *glyphs);
    glyphs->renderer = renderer;

    glyphs->cache = (Glyph *)calloc((size_t)GLYPH_STYLE_COUNT *
                                    GLYPH_MAX_CODEPOINT, sizeof(Glyph));
    glyphs->extra_cache = (GlyphExtraEntry *)calloc(GLYPH_EXTRA_BUCKETS,
                                                    sizeof(GlyphExtraEntry));
    if (!glyphs->cache || !glyphs->extra_cache) {
        glyphs_close(glyphs);
        return false;
    }

    if (font_path) {
        glyphs->regular = TTF_OpenFont(font_path, scaled_size);
        if (glyphs->regular) {
            glyphs->bold = open_variant(font_path, scaled_size, BOLD_SUFFIXES);
            glyphs->italic = open_variant(font_path, scaled_size,
                                          ITALIC_SUFFIXES);
            glyphs->bold_italic = open_variant(font_path, scaled_size,
                                               BOLD_ITALIC_SUFFIXES);
        }
    }

    if (!glyphs->regular) {
        /* No path, or the requested font could not be opened: the built-in
         * family is both the default and the fallback. */
        if (!open_embedded_family(glyphs, scaled_size)) {
            glyphs_close(glyphs);
            return false;
        }
        glyphs->built_in = true;
    } else {
        /* A font from disk may lack symbols in the output; its regular
         * built-in counterpart fills those codepoints in. */
        glyphs->fallback = open_embedded(EMBEDDED_FONT_REGULAR, scaled_size);
    }

    /* User-configured fallback chain from [font] fallback = [...]. */
    for (i = 0; i < user_fallback_count &&
                glyphs->user_fallback_count < GLYPH_MAX_USER_FALLBACKS; i++) {
        char resolved[512];
        const char *path;
        TTF_Font *face;

        if (!user_fallbacks || !user_fallbacks[i] || !user_fallbacks[i][0])
            continue;
        path = font_lookup_path(user_fallbacks[i], resolved, sizeof resolved)
                   ? resolved
                   : user_fallbacks[i];
        face = TTF_OpenFont(path, scaled_size);
        if (face) {
            int slot = glyphs->user_fallback_count++;
            glyphs->user_fallbacks[slot] = face;
            glyphs->user_fallback_color[slot] = path_is_color_emoji(path);
        }
    }

    /* System CJK and colour-emoji fallback fonts. */
    for (i = 0; SYSTEM_FALLBACK_FAMILIES[i] &&
                glyphs->sys_fallback_count < GLYPH_MAX_SYS_FALLBACKS; i++) {
        char resolved[512];
        TTF_Font *face;

        if (!font_lookup_path(SYSTEM_FALLBACK_FAMILIES[i], resolved,
                              sizeof resolved))
            continue;
        face = TTF_OpenFont(resolved, scaled_size);
        if (face) {
            int slot = glyphs->sys_fallback_count++;
            glyphs->sys_fallbacks[slot] = face;
            glyphs->sys_fallback_color[slot] = path_is_color_emoji(resolved);
        }
    }

    /* Symbols Nerd Font Mono, compiled in, as the last-resort face for
     * symbols. Opened whether the main font came from disk or is built-in. */
    glyphs->symbol_fallback =
        open_embedded(EMBEDDED_FONT_SYMBOLS_MONO, scaled_size);

    TTF_SetFontHinting(glyphs->regular, TTF_HINTING_LIGHT);

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

bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale)
{
    return glyphs_open_ex(glyphs, renderer, font_path, NULL, 0, point_size,
                          display_scale);
}

void glyphs_close(Glyphs *glyphs)
{
    int slot;
    int i;
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
    if (glyphs->extra_cache) {
        for (i = 0; i < GLYPH_EXTRA_BUCKETS; i++) {
            if (glyphs->extra_cache[i].glyph.texture)
                SDL_DestroyTexture(glyphs->extra_cache[i].glyph.texture);
        }
        free(glyphs->extra_cache);
    }
    for (i = 0; i < glyphs->user_fallback_count; i++) {
        if (glyphs->user_fallbacks[i])
            TTF_CloseFont(glyphs->user_fallbacks[i]);
    }
    for (i = 0; i < glyphs->sys_fallback_count; i++) {
        if (glyphs->sys_fallbacks[i])
            TTF_CloseFont(glyphs->sys_fallbacks[i]);
    }
    if (glyphs->fallback)
        TTF_CloseFont(glyphs->fallback);
    if (glyphs->symbol_fallback)
        TTF_CloseFont(glyphs->symbol_fallback);
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

void glyphs_draw_text_scaled(Glyphs *glyphs, const char *text, size_t length,
                             float x, float y, bool bold, bool italic,
                             SDL_Color color, float scale)
{
    const char *cursor;
    Uint32 first_codepoint;
    int slot;
    Glyph *glyph;

    if (scale <= 0.0f)
        scale = 1.0f;
    if (length == 0)
        return;
    cursor = text;
    first_codepoint = SDL_StepUTF8(&cursor, NULL);
    if (first_codepoint == 0)
        return;

    slot = style_slot(glyphs, bold, italic);

    if ((size_t)(cursor - text) == length) {
        /* Single codepoint (BMP or out-of-BMP): served from the cache. */
        glyph = glyph_lookup(glyphs, slot, first_codepoint);
        if (!glyph)
            return;
        draw_texture(glyphs, glyph->texture, x, y, glyph->width, glyph->height,
                     color, scale, glyph->is_color);
        if (bold && !glyphs->bold && !glyph->is_color) {
            /* No bold face: fake it with a second copy one pixel over. */
            draw_texture(glyphs, glyph->texture, x + 1.0f, y, glyph->width,
                         glyph->height, color, scale, false);
        }
        return;
    }

    /* A multi-codepoint grapheme cluster (base character plus combining
     * marks, an emoji with a ZWJ, ...): render it, uncached, from whichever
     * face has the base codepoint. */
    {
        bool is_color = false;
        TTF_Font *font = font_with_glyph(glyphs, slot, first_codepoint,
                                         &is_color);
        SDL_Texture *texture;
        float width = 0.0f;
        float height = 0.0f;

        if (!font)
            return;
        texture = bake_texture(glyphs->renderer, font, text, length,
                               &width, &height);
        if (!texture)
            return;
        draw_texture(glyphs, texture, x, y, width, height, color, scale,
                     is_color);
        SDL_DestroyTexture(texture);
    }
}

void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color)
{
    glyphs_draw_text_scaled(glyphs, text, length, x, y, bold, italic, color,
                            1.0f);
}
