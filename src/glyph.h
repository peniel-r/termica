/*
 * termica - fonts, the glyph cache and drawing text.
 *
 * Rasterised text is packed into a bounded set of shared atlas textures,
 * tinted at draw time unless the selected face supplies colour glyphs.
 * BMP and supplementary codepoints and common grapheme clusters are cached.
 *
 * The default font is Maple Mono NF, compiled into the executable. A path
 * passed on the command line still wins; if it cannot be opened, the
 * built-in family is used instead, and its regular face also fills in
 * codepoints the chosen font lacks.
 *
 * SDL_ttf does the rasterising. For a font from disk, bold, italic or
 * bold-italic faces are looked up next to the regular one (consola.ttf ->
 * consolab.ttf, "... Nerd Font.ttf" -> "... Nerd Font Bold.ttf"); when
 * there is no bold face the glyph is simply drawn twice, one pixel apart.
 *
 * Adjacent printable-ASCII cells with the same style are shaped as one run
 * with SDL_ttf's HarfBuzz-backed text object, so programming ligatures like
 * "->" stay whole. Everything else keeps the per-cell path.
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

/* The direct-indexed cache covers the Basic Multilingual Plane; codepoints
 * above U+FFFF (emoji, Material Design Nerd icons, CJK Ext B) live in a
 * secondary hash table so they are cached too. */
#define GLYPH_MAX_CODEPOINT      0x10000
#define GLYPH_EXTRA_BUCKETS      2048
#define GLYPH_MAX_USER_FALLBACKS 8
#define GLYPH_MAX_SYS_FALLBACKS  6
#if defined(__linux__) || defined(__APPLE__)
#define GLYPH_MAX_NATIVE_FALLBACKS 12
#endif

#define GLYPH_ATLAS_SIZE         1024
#define GLYPH_ATLAS_PAGES        8
#define GLYPH_CLUSTER_BUCKETS    256
#define GLYPH_CLUSTER_BYTES      128

typedef struct Glyph {
    SDL_FRect source;
    uint64_t generation;
    uint8_t page;           /* one-based; 0 means no atlas entry */
    bool tried;
    bool is_color;          /* colour emoji: draw untinted (white mod) */
} Glyph;

typedef struct GlyphExtraEntry {
    uint32_t codepoint;     /* 0 = empty bucket */
    uint8_t  slot;
    Glyph    glyph;
} GlyphExtraEntry;

typedef struct GlyphAtlasPage {
    SDL_Texture *texture;
    int x, y, row_height;
    uint64_t generation;
    uint64_t last_used;
} GlyphAtlasPage;

typedef struct GlyphClusterEntry {
    char text[GLYPH_CLUSTER_BYTES];
    size_t length;
    uint8_t slot;
    Glyph glyph;
} GlyphClusterEntry;

/* Shaped runs end up with glyph indices, not codepoints, so ligature and
 * contextual-alternate glyphs need their own cache keyed by face + index. */
#define GLYPH_SHAPED_BUCKETS 2048

typedef struct ShapedGlyphEntry {
    TTF_Font *font;             /* NULL = empty bucket */
    uint32_t  glyph_index;
    Glyph     glyph;
} ShapedGlyphEntry;

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

    /* User-configured fallback chain from [font] fallback = [...]. */
    TTF_Font *user_fallbacks[GLYPH_MAX_USER_FALLBACKS];
    bool      user_fallback_color[GLYPH_MAX_USER_FALLBACKS];
    int       user_fallback_count;

    /* System CJK and colour-emoji fallback fonts discovered on the OS. */
    TTF_Font *sys_fallbacks[GLYPH_MAX_SYS_FALLBACKS];
    bool      sys_fallback_color[GLYPH_MAX_SYS_FALLBACKS];
    int       sys_fallback_count;

    /* Symbols Nerd Font Mono, compiled in, as the last resort for symbols. */
    TTF_Font *symbol_fallback;

#if defined(__linux__) || defined(__APPLE__)
    /* On-demand native fallback faces, kept after the built-in symbols face. */
    TTF_Font *native_fallbacks[GLYPH_MAX_NATIVE_FALLBACKS];
    bool      native_fallback_color[GLYPH_MAX_NATIVE_FALLBACKS];
    int       native_fallback_count;
#endif

    /* True when the main family is the font compiled into the executable
     * (no path, or the requested path could not be opened). */
    bool built_in;

    /* [GLYPH_STYLE_COUNT][GLYPH_MAX_CODEPOINT], heap allocated because it
     * is several megabytes. */
    Glyph *cache;

    /* Open-addressed cache for codepoints >= GLYPH_MAX_CODEPOINT. */
    GlyphExtraEntry *extra_cache;

    GlyphAtlasPage atlas[GLYPH_ATLAS_PAGES];
    int atlas_size;
    uint64_t atlas_clock;
    GlyphClusterEntry *clusters;

    /* SDL_ttf's HarfBuzz shaping, driven through text objects with no
     * drawing engine: they only lay a run out so glyph.c can read its glyph
     * indices and positions. NULL when shaping could not be initialised. */
    TTF_Text *text_scratch;       /* the whole run */
    TTF_Text *text_cluster;       /* one multi-cell cluster */
    ShapedGlyphEntry *shaped_cache;

    /* One cell of the grid, from the font metrics. */
    int cell_width;
    int cell_height;
} Glyphs;

/* Resolve user fallback names + OS CJK/emoji fallbacks into file paths.
 * Exposed for the self test. Returns the number of resolved paths written. */
int glyphs_resolve_fallback_paths(const char *const *user_fallbacks,
                                  int user_count, char out[][256],
                                  int max_out);

/* Load the font, its optional faces, and the fallback chain. */
bool glyphs_open_ex(Glyphs *glyphs, SDL_Renderer *renderer,
                    const char *font_path,
                    const char *const *user_fallbacks, int user_fallback_count,
                    float point_size, float display_scale);

/* Load the font and its optional faces (with OS/built-in fallbacks only). */
bool glyphs_open(Glyphs *glyphs, SDL_Renderer *renderer, const char *font_path,
                 float point_size, float display_scale);

/* Free every texture and font. Safe on a zeroed struct. */
void glyphs_close(Glyphs *glyphs);

/* Invalidate textures after SDL_EVENT_RENDER_DEVICE_RESET; fonts stay open. */
void glyphs_reset(Glyphs *glyphs);

/* Draw one cell's text at (x, y). `text` is UTF-8 and may be a whole
 * grapheme cluster; `length` is in bytes. */
void glyphs_draw_text(Glyphs *glyphs, const char *text, size_t length,
                      float x, float y, bool bold, bool italic,
                      SDL_Color color);

/* Can this one cell join a shaped run? Only a single printable ASCII
 * character that the primary face carries qualifies: fallbacks, wide cells
 * and grapheme clusters keep the per-cell path. */
bool glyphs_can_join_run(const Glyphs *glyphs, const char *text, size_t length,
                         bool bold, bool italic);

/* Draw one shaped run of printable ASCII at (x, y). All cells share the same
 * style and colour, so HarfBuzz can substitute ligature glyphs across them.
 * False means the caller should draw the run one cell at a time. */
bool glyphs_draw_run(Glyphs *glyphs, const char *text, size_t length,
                     float x, float y, bool bold, bool italic,
                     SDL_Color color);

/* The same, with the glyph's ink scaled about its centre. A `scale` of 1
 * draws exactly like glyphs_draw_text; a larger value enlarges the glyph
 * within (and slightly past) its cell without changing the cell's advance.
 * A non-positive scale is treated as 1. */
void glyphs_draw_text_scaled(Glyphs *glyphs, const char *text, size_t length,
                             float x, float y, bool bold, bool italic,
                             SDL_Color color, float scale);

#endif /* TERMICA_GLYPH_H */
