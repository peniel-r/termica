/*
 * termica - fonts, the glyph cache and drawing text. See glyph.h.
 */

#include "glyph.h"

#include <SDL3_ttf/SDL_textengine.h>

#include "embed.h"
#include "font_lookup.h"

#include <stdio.h>
#include <stdint.h>
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

/* SDL_ttf lays a text object out even without a drawing engine, which is
 * exactly what shaping needs: the copy operations carry glyph indices and
 * positions, and glyph.c draws them into its own atlas. */
static void shaping_destroy(Glyphs *glyphs)
{
    if (glyphs->text_cluster)
        TTF_DestroyText(glyphs->text_cluster);
    if (glyphs->text_scratch)
        TTF_DestroyText(glyphs->text_scratch);
    glyphs->text_cluster = NULL;
    glyphs->text_scratch = NULL;
}

/* Shaping is an enhancement, so a failure here only disables ligatures; the
 * per-cell path keeps working. */
static bool shaping_create(Glyphs *glyphs)
{
    if (!glyphs->regular)
        return false;
    glyphs->text_scratch = TTF_CreateText(NULL, glyphs->regular, "", 0);
    glyphs->text_cluster = TTF_CreateText(NULL, glyphs->regular, "", 0);
    if (!glyphs->text_scratch || !glyphs->text_cluster) {
        shaping_destroy(glyphs);
        return false;
    }
    TTF_SetTextDirection(glyphs->text_scratch, TTF_DIRECTION_LTR);
    TTF_SetTextDirection(glyphs->text_cluster, TTF_DIRECTION_LTR);
    return true;
}

/* A one-pixel extruded gutter preserves texture-edge sampling when scaled
 * and prevents linear filtering from sampling a neighbouring glyph. Eviction
 * reuses a whole page; stale entries are rasterised again. */
static bool atlas_store(Glyphs *glyphs, SDL_Surface *surface, Glyph *glyph)
{
    SDL_Surface *padded;
    SDL_Rect destination, upload;
    GlyphAtlasPage *page = NULL;
    int width, height, x = 0, y = 0, row_height = 0;
    int i, chosen = -1;

    if (surface->w <= 0 || surface->h <= 0 || glyphs->atlas_size <= 2 ||
        surface->w > glyphs->atlas_size - 2 ||
        surface->h > glyphs->atlas_size - 2)
        return false;
    width = surface->w + 2;
    height = surface->h + 2;
    padded = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!padded)
        return false;
    /* The blit writes the interior; the gutter is filled from its edges. */
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    destination.x = 1;
    destination.y = 1;
    destination.w = surface->w;
    destination.h = surface->h;
    if (!SDL_BlitSurface(surface, NULL, padded, &destination)) {
        SDL_DestroySurface(padded);
        return false;
    }
    for (i = 1; i <= surface->h; i++) {
        Uint8 *row = (Uint8 *)padded->pixels + (size_t)i * padded->pitch;
        memcpy(row, row + 4, 4);
        memcpy(row + (size_t)(width - 1) * 4,
               row + (size_t)(width - 2) * 4, 4);
    }
    memcpy(padded->pixels, (Uint8 *)padded->pixels + padded->pitch,
           (size_t)width * 4);
    memcpy((Uint8 *)padded->pixels + (size_t)(height - 1) * padded->pitch,
           (Uint8 *)padded->pixels + (size_t)(height - 2) * padded->pitch,
           (size_t)width * 4);

    for (i = 0; i < GLYPH_ATLAS_PAGES; i++) {
        page = &glyphs->atlas[i];
        if (!page->texture) {
            chosen = i;
            x = y = row_height = 0;
            break;
        }
        x = page->x;
        y = page->y;
        row_height = page->row_height;
        if (x + width > glyphs->atlas_size) {
            x = 0;
            y += row_height;
            row_height = 0;
        }
        if (y + height <= glyphs->atlas_size) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) {
        chosen = 0;
        for (i = 1; i < GLYPH_ATLAS_PAGES; i++)
            if (glyphs->atlas[i].last_used < glyphs->atlas[chosen].last_used)
                chosen = i;
        page = &glyphs->atlas[chosen];
        ++page->generation;
        page->x = page->y = page->row_height = 0;
        x = y = row_height = 0;
    }
    page = &glyphs->atlas[chosen];
    if (!page->texture) {
        page->texture = SDL_CreateTexture(glyphs->renderer,
                                          SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          glyphs->atlas_size,
                                          glyphs->atlas_size);
        if (!page->texture) {
            SDL_DestroySurface(padded);
            return false;
        }
        page->generation = 1;
        SDL_SetTextureBlendMode(page->texture, SDL_BLENDMODE_BLEND);
    }
    upload.x = x;
    upload.y = y;
    upload.w = width;
    upload.h = height;
    if (!SDL_UpdateTexture(page->texture, &upload, padded->pixels,
                           padded->pitch)) {
        SDL_DestroySurface(padded);
        return false;
    }
    SDL_DestroySurface(padded);
    page->x = x + width;
    page->y = y;
    page->row_height = row_height > height ? row_height : height;
    page->last_used = ++glyphs->atlas_clock;
    glyph->source.x = (float)(x + 1);
    glyph->source.y = (float)(y + 1);
    glyph->source.w = (float)surface->w;
    glyph->source.h = (float)surface->h;
    glyph->page = (uint8_t)(chosen + 1);
    glyph->generation = page->generation;
    glyph->tried = true;
    return true;
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

/* A color face is not necessarily named "emoji", and a single face can
 * contain both monochrome and color glyphs. Query the actual glyph image on
 * cache misses, including each member of a combining/emoji cluster. */
static bool text_has_color_glyph(TTF_Font *font, const char *text, size_t length)
{
    const char *cursor = text;

    while (length) {
        Uint32 codepoint = SDL_StepUTF8(&cursor, &length);
        TTF_ImageType type = TTF_IMAGE_INVALID;
        SDL_Surface *image;

        if (!codepoint)
            break;
        image = TTF_GetGlyphImage(font, codepoint, &type);
        if (image)
            SDL_DestroySurface(image);
        if (type == TTF_IMAGE_COLOR)
            return true;
    }
    return false;
}

static Glyph *glyph_lookup(Glyphs *glyphs, int slot, Uint32 codepoint)
{
    uint32_t hash, mask, probe;
    GlyphExtraEntry *entry;

    if (codepoint < GLYPH_MAX_CODEPOINT)
        return &glyphs->cache[(size_t)slot * GLYPH_MAX_CODEPOINT + codepoint];

    hash = (codepoint * 2654435761u) ^ ((uint32_t)slot * 40503u);
    mask = GLYPH_EXTRA_BUCKETS - 1;
    for (probe = 0; probe < GLYPH_EXTRA_BUCKETS; probe++) {
        entry = &glyphs->extra_cache[(hash + probe) & mask];
        if (entry->codepoint == codepoint && entry->slot == (uint8_t)slot)
            return &entry->glyph;
        if (entry->codepoint == 0) {
            entry->codepoint = codepoint;
            entry->slot = (uint8_t)slot;
            return &entry->glyph;
        }
    }
    /* A full supplementary cache must not make new codepoints disappear. */
    entry = &glyphs->extra_cache[hash & mask];
    memset(entry, 0, sizeof *entry);
    entry->codepoint = codepoint;
    entry->slot = (uint8_t)slot;
    return &entry->glyph;
}

static Glyph *cluster_lookup(Glyphs *glyphs, int slot, const char *text,
                             size_t length, Glyph *temporary)
{
    uint32_t hash = 2166136261u ^ (uint32_t)slot;
    size_t i;
    GlyphClusterEntry *entry;

    if (length > GLYPH_CLUSTER_BYTES)
        return temporary;
    for (i = 0; i < length; i++)
        hash = (hash ^ (uint8_t)text[i]) * 16777619u;
    entry = &glyphs->clusters[hash % GLYPH_CLUSTER_BUCKETS];
    if (entry->length != length || entry->slot != (uint8_t)slot ||
        memcmp(entry->text, text, length) != 0) {
        memset(entry, 0, sizeof *entry);
        memcpy(entry->text, text, length);
        entry->length = length;
        entry->slot = (uint8_t)slot;
    }
    return &entry->glyph;
}

static void draw_texture(Glyphs *glyphs, SDL_Texture *texture,
                         const SDL_FRect *source, float x, float y,
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
    SDL_RenderTexture(glyphs->renderer, texture, source, &destination);
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
    glyphs->atlas_size = GLYPH_ATLAS_SIZE;
    {
        Sint64 max_size = SDL_GetNumberProperty(
            SDL_GetRendererProperties(renderer),
            SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, GLYPH_ATLAS_SIZE);
        if (max_size > 0 && max_size < glyphs->atlas_size)
            glyphs->atlas_size = (int)max_size;
    }

    glyphs->cache = (Glyph *)calloc((size_t)GLYPH_STYLE_COUNT *
                                    GLYPH_MAX_CODEPOINT, sizeof(Glyph));
    glyphs->extra_cache = (GlyphExtraEntry *)calloc(GLYPH_EXTRA_BUCKETS,
                                                    sizeof(GlyphExtraEntry));
    glyphs->clusters = (GlyphClusterEntry *)calloc(GLYPH_CLUSTER_BUCKETS,
                                                   sizeof(GlyphClusterEntry));
    glyphs->shaped_cache = (ShapedGlyphEntry *)calloc(GLYPH_SHAPED_BUCKETS,
                                                      sizeof(ShapedGlyphEntry));
    if (!glyphs->cache || !glyphs->extra_cache || !glyphs->clusters ||
        !glyphs->shaped_cache) {
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
    (void)shaping_create(glyphs);
    return true;
}

bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale)
{
    return glyphs_open_ex(glyphs, renderer, font_path, NULL, 0, point_size,
                          display_scale);
}

static void atlas_destroy(Glyphs *glyphs)
{
    int i;

    for (i = 0; i < GLYPH_ATLAS_PAGES; i++) {
        if (glyphs->atlas[i].texture)
            SDL_DestroyTexture(glyphs->atlas[i].texture);
    }
    memset(glyphs->atlas, 0, sizeof glyphs->atlas);
    glyphs->atlas_clock = 0;
}

void glyphs_reset(Glyphs *glyphs)
{
    shaping_destroy(glyphs);
    atlas_destroy(glyphs);
    if (glyphs->cache)
        memset(glyphs->cache, 0, (size_t)GLYPH_STYLE_COUNT *
               GLYPH_MAX_CODEPOINT * sizeof *glyphs->cache);
    if (glyphs->extra_cache)
        memset(glyphs->extra_cache, 0,
               GLYPH_EXTRA_BUCKETS * sizeof *glyphs->extra_cache);
    if (glyphs->clusters)
        memset(glyphs->clusters, 0,
               GLYPH_CLUSTER_BUCKETS * sizeof *glyphs->clusters);
    if (glyphs->shaped_cache)
        memset(glyphs->shaped_cache, 0,
               GLYPH_SHAPED_BUCKETS * sizeof *glyphs->shaped_cache);
    (void)shaping_create(glyphs);
}

void glyphs_close(Glyphs *glyphs)
{
    int i;

    shaping_destroy(glyphs);
    atlas_destroy(glyphs);
    free(glyphs->cache);
    free(glyphs->extra_cache);
    free(glyphs->clusters);
    free(glyphs->shaped_cache);
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
    const char *cursor = text;
    size_t remaining = length;
    Uint32 first_codepoint;
    int slot;
    bool single, is_color = false;
    Glyph temporary = {0};
    Glyph *glyph;
    GlyphAtlasPage *page;
    TTF_Font *font;
    SDL_Surface *surface;
    SDL_Texture *texture;
    SDL_Color white = {255, 255, 255, 255};

    if (!text || length == 0)
        return;
    if (scale <= 0.0f)
        scale = 1.0f;
    first_codepoint = SDL_StepUTF8(&cursor, &remaining);
    if (first_codepoint == 0)
        return;
    single = remaining == 0;
    slot = style_slot(glyphs, bold, italic);
    glyph = single ? glyph_lookup(glyphs, slot, first_codepoint) :
                     cluster_lookup(glyphs, slot, text, length, &temporary);

    if (glyph->tried && glyph->page == 0)
        return; /* No face has the base codepoint. */
    if (glyph->page) {
        page = &glyphs->atlas[glyph->page - 1];
        if (page->texture && page->generation == glyph->generation)
            goto draw_atlas;
    }
    glyph->page = 0;
    glyph->tried = false;
    font = font_with_glyph(glyphs, slot, first_codepoint, &is_color);
    if (!font) {
        glyph->tried = true;
        return;
    }
    surface = TTF_RenderText_Blended(font, text, length, white);
    if (!surface)
        return;
    if (!is_color)
        is_color = text_has_color_glyph(font, text, length);
    glyph->is_color = is_color;
    if (atlas_store(glyphs, surface, glyph)) {
        SDL_DestroySurface(surface);
        page = &glyphs->atlas[glyph->page - 1];
        goto draw_atlas;
    }

    /* A glyph can be larger than an atlas page, or an upload may fail.
     * Draw it transiently and retry the atlas next time rather than dropping
     * it or retaining an unbounded dedicated texture. */
    texture = SDL_CreateTextureFromSurface(glyphs->renderer, surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        draw_texture(glyphs, texture, NULL, x, y, (float)surface->w,
                     (float)surface->h, color, scale, is_color);
        if (single && bold && !glyphs->bold && !is_color)
            draw_texture(glyphs, texture, NULL, x + 1.0f, y,
                         (float)surface->w, (float)surface->h, color, scale,
                         false);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
    return;

draw_atlas:
    page->last_used = ++glyphs->atlas_clock;
    draw_texture(glyphs, page->texture, &glyph->source, x, y,
                 glyph->source.w, glyph->source.h, color, scale,
                 glyph->is_color);
    if (single && bold && !glyphs->bold && !glyph->is_color)
        draw_texture(glyphs, page->texture, &glyph->source, x + 1.0f, y,
                     glyph->source.w, glyph->source.h, color, scale, false);
}

void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color)
{
    glyphs_draw_text_scaled(glyphs, text, length, x, y, bold, italic, color,
                            1.0f);
}

/* Runs are already positioned in glyph pixels, so they bypass the per-cell
 * texture fit/centre rules. */
static void draw_shaped_texture(Glyphs *glyphs, SDL_Texture *texture,
                                const SDL_FRect *source, float x, float y,
                                float width, float height, SDL_Color color,
                                bool is_color)
{
    SDL_FRect destination;

    destination.x = x;
    destination.y = y;
    destination.w = width;
    destination.h = height;
    if (is_color) {
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, color.a);
    } else {
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
    }
    (void)SDL_RenderTexture(glyphs->renderer, texture, source, &destination);
}

/* Shaped runs produce glyph indices, not codepoints, so ligature and
 * contextual-alternate glyphs get their own face/index cache. */
static Glyph *shaped_lookup(Glyphs *glyphs, TTF_Font *font,
                            uint32_t glyph_index)
{
    uint32_t hash, mask, probe;
    ShapedGlyphEntry *entry;

    if (!glyphs->shaped_cache || !font || glyph_index == 0)
        return NULL;
    hash = (glyph_index * 2654435761u) ^
           (uint32_t)(((uintptr_t)font >> 4) & 0xFFFFFFFFu);
    mask = GLYPH_SHAPED_BUCKETS - 1;
    for (probe = 0; probe < GLYPH_SHAPED_BUCKETS; probe++) {
        entry = &glyphs->shaped_cache[(hash + probe) & mask];
        if (entry->font == font && entry->glyph_index == glyph_index)
            return &entry->glyph;
        if (!entry->font) {
            memset(&entry->glyph, 0, sizeof entry->glyph);
            entry->font = font;
            entry->glyph_index = glyph_index;
            return &entry->glyph;
        }
    }
    entry = &glyphs->shaped_cache[hash & mask];
    memset(entry, 0, sizeof *entry);
    entry->font = font;
    entry->glyph_index = glyph_index;
    return &entry->glyph;
}

static void draw_shaped_glyph(Glyphs *glyphs, TTF_Font *font,
                              uint32_t glyph_index,
                              const SDL_Rect *source_rect, float x, float y,
                              SDL_Color color, bool bold)
{
    Glyph *glyph;
    GlyphAtlasPage *page;
    TTF_ImageType type = TTF_IMAGE_INVALID;
    SDL_Surface *surface;
    SDL_Texture *texture;
    SDL_FRect source;
    bool is_color;
    float width;
    float height;

    if (!source_rect || source_rect->w <= 0 || source_rect->h <= 0)
        return;
    glyph = shaped_lookup(glyphs, font, glyph_index);
    if (!glyph)
        return;
    if (glyph->tried && glyph->page == 0)
        return;
    width = (float)source_rect->w;
    height = (float)source_rect->h;

    if (glyph->page) {
        page = &glyphs->atlas[glyph->page - 1];
        if (page->texture && page->generation == glyph->generation) {
            source.x = glyph->source.x + (float)source_rect->x;
            source.y = glyph->source.y + (float)source_rect->y;
            source.w = width;
            source.h = height;
            page->last_used = ++glyphs->atlas_clock;
            draw_shaped_texture(glyphs, page->texture, &source, x, y, width,
                                height, color, glyph->is_color);
            if (bold && !glyphs->bold && !glyph->is_color)
                draw_shaped_texture(glyphs, page->texture, &source, x + 1.0f,
                                    y, width, height, color, false);
            return;
        }
    }

    glyph->page = 0;
    glyph->tried = false;
    surface = TTF_GetGlyphImageForIndex(font, glyph_index, &type);
    if (!surface) {
        glyph->tried = true;
        return;
    }
    is_color = type == TTF_IMAGE_COLOR;
    glyph->is_color = is_color;
    source.x = (float)source_rect->x;
    source.y = (float)source_rect->y;
    source.w = width;
    source.h = height;

    if (atlas_store(glyphs, surface, glyph)) {
        SDL_DestroySurface(surface);
        page = &glyphs->atlas[glyph->page - 1];
        page->last_used = ++glyphs->atlas_clock;
        source.x = glyph->source.x + (float)source_rect->x;
        source.y = glyph->source.y + (float)source_rect->y;
        draw_shaped_texture(glyphs, page->texture, &source, x, y, width,
                            height, color, is_color);
        if (bold && !glyphs->bold && !is_color)
            draw_shaped_texture(glyphs, page->texture, &source, x + 1.0f, y,
                                width, height, color, false);
        return;
    }

    /* Oversized or atlas-hostile glyphs still draw, just without caching. */
    texture = SDL_CreateTextureFromSurface(glyphs->renderer, surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        draw_shaped_texture(glyphs, texture, &source, x, y, width, height,
                            color, is_color);
        if (bold && !glyphs->bold && !is_color)
            draw_shaped_texture(glyphs, texture, &source, x + 1.0f, y, width,
                                height, color, false);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
}

bool glyphs_can_join_run(const Glyphs *glyphs, const char *text, size_t length,
                         bool bold, bool italic)
{
    TTF_Font *font;
    unsigned char codepoint;

    if (!glyphs || !glyphs->text_scratch || !glyphs->text_cluster || !text ||
        length != 1)
        return false;
    codepoint = (unsigned char)text[0];
    /* Printable ASCII only. Control characters, wide characters, emoji and
     * combining clusters keep the per-cell path and its fallback chain. */
    if (codepoint < 0x20 || codepoint > 0x7E)
        return false;
    font = font_for_slot(glyphs, style_slot(glyphs, bold, italic));
    return font && TTF_FontHasGlyph(font, (Uint32)codepoint);
}

bool glyphs_draw_run(Glyphs *glyphs, const char *text, size_t length,
                     float x, float y, bool bold, bool italic,
                     SDL_Color color)
{
    TTF_Font *font;
    TTF_TextData *data;
    int i;

    if (!glyphs || !glyphs->text_scratch || !glyphs->text_cluster || !text ||
        length == 0)
        return false;
    font = font_for_slot(glyphs, style_slot(glyphs, bold, italic));
    if (!font)
        return false;
    if (!TTF_SetTextFont(glyphs->text_scratch, font) ||
        !TTF_SetTextString(glyphs->text_scratch, text, length) ||
        !TTF_UpdateText(glyphs->text_scratch))
        return false;
    data = glyphs->text_scratch->internal;
    if (!data)
        return false;

    for (i = 0; i < data->num_ops; i++) {
        const TTF_CopyOperation *op;
        size_t offset, end;
        int next, same;

        if (data->ops[i].cmd != TTF_DRAW_COMMAND_COPY)
            continue;
        op = &data->ops[i].copy;
        offset = (size_t)op->text_offset;
        if (offset >= length)
            continue;

        /* A cluster runs until the next copy operation with a different
         * source offset. Multiple glyphs can share one offset. */
        next = i + 1;
        same = 1;
        while (next < data->num_ops) {
            if (data->ops[next].cmd == TTF_DRAW_COMMAND_COPY &&
                (size_t)data->ops[next].copy.text_offset != offset)
                break;
            if (data->ops[next].cmd == TTF_DRAW_COMMAND_COPY)
                same++;
            next++;
        }
        end = next < data->num_ops
                  ? (size_t)data->ops[next].copy.text_offset : length;
        if (end <= offset || end > length)
            end = length;

        if (end - offset == 1 && same == 1) {
            TTF_Font *glyph_font = op->glyph_font ? op->glyph_font : font;
            int minx, maxx, miny, maxy, advance;

            /* One cell: place the glyph from its own bearing, not from the
             * run's accumulated font advances, so cells never drift. */
            if (TTF_GetGlyphMetrics(glyph_font,
                    (Uint32)(unsigned char)text[offset], &minx, &maxx,
                    &miny, &maxy, &advance)) {
                draw_shaped_glyph(glyphs, glyph_font, op->glyph_index,
                                  &op->src,
                                  x + (float)offset * (float)glyphs->cell_width
                                    + (float)minx,
                                  y + (float)op->dst.y, color, bold);
            }
        } else {
            /* A ligature or other multi-cell cluster: shape just this
             * substring so its operations are relative to the cluster's
             * own origin. */
            float cluster_x = x + (float)offset * (float)glyphs->cell_width;

            if (TTF_SetTextFont(glyphs->text_cluster, font) &&
                TTF_SetTextString(glyphs->text_cluster, text + offset,
                                  end - offset) &&
                TTF_UpdateText(glyphs->text_cluster)) {
                TTF_TextData *cluster = glyphs->text_cluster->internal;
                int k;

                for (k = 0; cluster && k < cluster->num_ops; k++) {
                    const TTF_CopyOperation *cluster_op;

                    if (cluster->ops[k].cmd != TTF_DRAW_COMMAND_COPY)
                        continue;
                    cluster_op = &cluster->ops[k].copy;
                    draw_shaped_glyph(glyphs,
                                      cluster_op->glyph_font ? cluster_op->glyph_font
                                                             : font,
                                      cluster_op->glyph_index,
                                      &cluster_op->src,
                                      cluster_x + (float)cluster_op->dst.x,
                                      y + (float)cluster_op->dst.y, color,
                                      bold);
                }
            }
        }

        i = next - 1;
    }
    return true;
}
