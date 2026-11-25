/* Software-renderer checks of atlas output, eviction and recreation. */
#include "glyph.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed)
        ++failures;
}

static void clear_frame(SDL_Renderer *renderer)
{
    SDL_SetRenderDrawColor(renderer, 19, 31, 47, 255);
    SDL_RenderClear(renderer);
}

static SDL_Surface *draw_snapshot(Glyphs *glyphs, SDL_Surface *frame,
                                  const char *text, size_t length,
                                  bool bold, bool italic, SDL_Color color)
{
    clear_frame(glyphs->renderer);
    glyphs_draw_text(glyphs, text, length, 8.0f, 8.0f, bold, italic, color);
    SDL_RenderPresent(glyphs->renderer);
    return SDL_DuplicateSurface(frame);
}

static SDL_Surface *run_snapshot(Glyphs *glyphs, SDL_Surface *frame,
                                 const char *text, size_t length,
                                 bool bold, bool italic, SDL_Color color)
{
    clear_frame(glyphs->renderer);
    if (!glyphs_draw_run(glyphs, text, length, 8.0f, 8.0f, bold, italic,
                         color))
        return NULL;
    SDL_RenderPresent(glyphs->renderer);
    return SDL_DuplicateSurface(frame);
}

static SDL_Surface *cells_snapshot(Glyphs *glyphs, SDL_Surface *frame,
                                   const char *text, size_t length,
                                   bool bold, bool italic, SDL_Color color)
{
    size_t i;

    clear_frame(glyphs->renderer);
    for (i = 0; i < length; i++)
        glyphs_draw_text(glyphs, text + i, 1,
                         8.0f + (float)i * (float)glyphs->cell_width, 8.0f,
                         bold, italic, color);
    SDL_RenderPresent(glyphs->renderer);
    return SDL_DuplicateSurface(frame);
}

static bool same_pixels(const SDL_Surface *a, const SDL_Surface *b)
{
    int y;

    if (!a || !b || a->w != b->w || a->h != b->h || a->format != b->format)
        return false;
    for (y = 0; y < a->h; y++) {
        const Uint8 *arow = (const Uint8 *)a->pixels + (size_t)y * a->pitch;
        const Uint8 *brow = (const Uint8 *)b->pixels + (size_t)y * b->pitch;
        if (memcmp(arow, brow, (size_t)a->w * 4) != 0)
            return false;
    }
    return true;
}

static bool has_ink(const SDL_Surface *surface)
{
    int x, y;
    if (!surface)
        return false;
    for (y = 0; y < surface->h; y++) {
        const Uint8 *row = (const Uint8 *)surface->pixels + (size_t)y * surface->pitch;
        for (x = 0; x < surface->w; x++) {
            const Uint8 *pixel = row + (size_t)x * 4;
            if (pixel[0] != 19 || pixel[1] != 31 || pixel[2] != 47)
                return true;
        }
    }
    return false;
}

/* Leftmost painted pixel, or the surface width when there is no ink. */
static int ink_min_x(const SDL_Surface *surface)
{
    int x, y;

    if (!surface)
        return 0;
    for (y = 0; y < surface->h; y++) {
        const Uint8 *row = (const Uint8 *)surface->pixels +
                           (size_t)y * surface->pitch;
        for (x = 0; x < surface->w; x++) {
            const Uint8 *pixel = row + (size_t)x * 4;
            if (pixel[0] != 19 || pixel[1] != 31 || pixel[2] != 47)
                return x;
        }
    }
    return surface->w;
}

/* Does the shaped run start in the same cell as the per-cell version? */
static bool ink_starts_near(const SDL_Surface *run, const SDL_Surface *cells,
                            int tolerance)
{
    int delta;

    if (!run || !cells)
        return false;
    delta = ink_min_x(run) - ink_min_x(cells);
    if (delta < 0)
        delta = -delta;
    return delta <= tolerance;
}

/* Count strongly different colour channels, ignoring the one-level
 * antialiasing difference between two rasterisation paths. */
static int strong_difference(const SDL_Surface *a, const SDL_Surface *b)
{
    int x, y, count = 0;

    if (!a || !b || a->w != b->w || a->h != b->h)
        return 0;
    for (y = 0; y < a->h; y++) {
        const Uint8 *arow = (const Uint8 *)a->pixels + (size_t)y * a->pitch;
        const Uint8 *brow = (const Uint8 *)b->pixels + (size_t)y * b->pitch;
        for (x = 0; x < a->w; x++) {
            int k;
            for (k = 0; k < 3; k++) {
                int delta = (int)arow[x * 4 + k] - (int)brow[x * 4 + k];
                if (delta < 0)
                    delta = -delta;
                if (delta > 16)
                    count++;
            }
        }
    }
    return count;
}

static bool matches_reference(Glyphs *glyphs, SDL_Surface *frame, TTF_Font *font,
                              const char *text, size_t length, SDL_Color color,
                              bool bold, bool italic, float scale)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *result, *rendered, *reference = NULL;
    SDL_Texture *texture;
    bool matches = false;

    clear_frame(glyphs->renderer);
    glyphs_draw_text_scaled(glyphs, text, length, 8.0f, 8.0f, bold, italic,
                            color, scale);
    SDL_RenderPresent(glyphs->renderer);
    result = SDL_DuplicateSurface(frame);
    rendered = TTF_RenderText_Blended(font, text, length, white);
    texture = rendered ? SDL_CreateTextureFromSurface(glyphs->renderer,
                                                       rendered) : NULL;

    if (texture) {
        SDL_FRect destination = {
            8.0f - (float)rendered->w * (scale - 1.0f) / 2.0f,
            scale == 1.0f ? 8.0f :
                8.0f + ((float)glyphs->cell_height - rendered->h * scale) / 2.0f,
            (float)rendered->w * scale, (float)rendered->h * scale
        };
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
        clear_frame(glyphs->renderer);
        SDL_RenderTexture(glyphs->renderer, texture, NULL, &destination);
        SDL_RenderPresent(glyphs->renderer);
        reference = SDL_DuplicateSurface(frame);
        matches = same_pixels(result, reference);
    }
    SDL_DestroySurface(reference);
    SDL_DestroyTexture(texture);
    SDL_DestroySurface(rendered);
    SDL_DestroySurface(result);
    return matches;
}

static bool sample_is(SDL_Surface *frame, int r, int g, int b, int tolerance)
{
    Uint8 actual_r, actual_g, actual_b, actual_a;
    if (!SDL_ReadSurfacePixel(frame, 8, 8, &actual_r, &actual_g,
                              &actual_b, &actual_a))
        return false;
    return (int)actual_r >= r - tolerance && (int)actual_r <= r + tolerance &&
           (int)actual_g >= g - tolerance && (int)actual_g <= g + tolerance &&
           (int)actual_b >= b - tolerance && (int)actual_b <= b + tolerance &&
           actual_a == 255;
}

/* Feed a known colored atlas texel through the same public draw path. */
static bool color_modulation(Glyphs *glyphs, SDL_Surface *frame)
{
    SDL_Color foreground = {255, 0, 0, 255};
    const Uint8 pixel[4] = {20, 200, 80, 255};
    SDL_Rect rect = {0, 0, 1, 1};
    Glyph *glyph = &glyphs->cache['!'];
    GlyphAtlasPage *page = &glyphs->atlas[0];
    bool preserved, blended;

    glyphs_reset(glyphs);
    page->texture = SDL_CreateTexture(glyphs->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (!page->texture || !SDL_UpdateTexture(page->texture, &rect, pixel, 4))
        return false;
    SDL_SetTextureBlendMode(page->texture, SDL_BLENDMODE_BLEND);
    page->generation = 1;
    glyph->page = 1;
    glyph->generation = 1;
    glyph->tried = true;
    glyph->is_color = true;
    glyph->source.x = glyph->source.y = 0.0f;
    glyph->source.w = glyph->source.h = 1.0f;
    clear_frame(glyphs->renderer);
    glyphs_draw_text(glyphs, "!", 1, 8.0f, 8.0f, false, false, foreground);
    SDL_RenderPresent(glyphs->renderer);
    preserved = sample_is(frame, 20, 200, 80, 1);
    foreground.a = 128;
    clear_frame(glyphs->renderer);
    glyphs_draw_text(glyphs, "!", 1, 8.0f, 8.0f, false, false, foreground);
    SDL_RenderPresent(glyphs->renderer);
    /* Half alpha over the clear colour (19, 31, 47). */
    blended = sample_is(frame, 20, 116, 64, 2);
    glyphs_reset(glyphs);
    return preserved && blended;
}

int glyph_atlas_selftest(void)
{    SDL_Surface *frame = NULL, *before = NULL, *after = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *target = NULL;
    Glyphs glyphs = {0};
    SDL_Color color = {90, 180, 240, 171};
    const char cluster[] = "a\xcc\x81";
    bool initialized = false, fonts_initialized = false, opened = false;
    bool evicted = false, target_preserved = false, supplementary = false;
    int i;

    failures = 0;
    initialized = SDL_Init(0);
    if (initialized)
        fonts_initialized = TTF_Init();
    if (fonts_initialized)
        frame = SDL_CreateSurface(128, 128, SDL_PIXELFORMAT_RGBA32);
    if (frame)
        renderer = SDL_CreateSoftwareRenderer(frame);
    if (renderer) {
#ifdef _WIN32
        opened = glyphs_open(&glyphs, renderer, NULL, 16.0f, 1.0f);
#else
        opened = glyphs_open(&glyphs, renderer,
                             "assets/fonts/MapleMono-NF-Regular.ttf", 16.0f,
                             1.0f);
        if (opened && !glyphs.symbol_fallback)
            glyphs.symbol_fallback = TTF_OpenFont(
                "assets/fonts/SymbolsNerdFontMono-Regular.ttf", 16.0f);
#endif
    }
    check(opened, "glyph atlas software renderer and bundled font setup");
    if (!opened)
        goto cleanup;

    check(matches_reference(&glyphs, frame, glyphs.regular, "A", 1, color,
                            false, false, 1.0f) &&
          matches_reference(&glyphs, frame, glyphs.regular, "A", 1, color,
                            false, false, 1.0f),
          "atlas first draw and cache hit preserve tinted alpha output");
    check(matches_reference(&glyphs, frame, glyphs.regular, "A", 1, color,
                            false, false, 1.25f),
          "scaled atlas sampling matches a standalone texture");
    check(matches_reference(&glyphs, frame, glyphs.regular, cluster,
                            sizeof cluster - 1, color, false, false, 1.0f) &&
          matches_reference(&glyphs, frame, glyphs.regular, cluster,
                            sizeof cluster - 1, color, false, false, 1.0f),
          "combining cluster output and cache hit match standalone text");
    if (glyphs.bold && glyphs.italic) {
        check(matches_reference(&glyphs, frame, glyphs.bold, "A", 1, color,
                                true, false, 1.0f) &&
              matches_reference(&glyphs, frame, glyphs.italic, "A", 1, color,
                                false, true, 1.0f),
              "bold and italic faces use separate atlas entries");
    } else {
        check(false, "bundled bold and italic faces available for atlas test");
    }

    /* U+F0388 is in Symbols Nerd Font but not Maple Mono NF. */
    if (glyphs.symbol_fallback &&
        !TTF_FontHasGlyph(glyphs.regular, 0xF0388) &&
        TTF_FontHasGlyph(glyphs.symbol_fallback, 0xF0388)) {
        char text[4];
        char *end = SDL_UCS4ToUTF8(0xF0388, text);
        before = draw_snapshot(&glyphs, frame, text, (size_t)(end - text),
                               false, false, color);
        after = draw_snapshot(&glyphs, frame, text, (size_t)(end - text),
                              false, false, color);
        for (i = 0; i < GLYPH_EXTRA_BUCKETS; i++)
            if (glyphs.extra_cache[i].codepoint == 0xF0388 &&
                glyphs.extra_cache[i].glyph.page)
                supplementary = true;
        supplementary = supplementary && has_ink(before) &&
                        same_pixels(before, after);
        SDL_DestroySurface(before);
        SDL_DestroySurface(after);
        before = after = NULL;
    }
    check(supplementary, "supplementary fallback glyph is cached and stable");

    glyphs_reset(&glyphs);
    target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                               SDL_TEXTUREACCESS_TARGET, 128, 128);
    if (target && SDL_SetRenderTarget(renderer, target)) {
        glyphs_draw_text(&glyphs, "A", 1, 0.0f, 0.0f, false, false, color);
        target_preserved = SDL_GetRenderTarget(renderer) == target &&
                           glyphs.cache['A'].page != 0;
        SDL_SetRenderTarget(renderer, NULL);
    }
    SDL_DestroyTexture(target);
    check(target_preserved, "atlas creation preserves the active render target");

    glyphs_reset(&glyphs);
    glyphs.atlas_size = 48; /* Make page churn deterministic with ASCII. */
    before = draw_snapshot(&glyphs, frame, "A", 1, false, false, color);
    for (i = 33; i <= 126; i++) {
        char text = (char)i;
        glyphs_draw_text(&glyphs, &text, 1, 0.0f, 0.0f, false, false, color);
    }
    for (i = 0; i < GLYPH_ATLAS_PAGES; i++)
        if (glyphs.atlas[i].generation > 1)
            evicted = true;
    after = draw_snapshot(&glyphs, frame, "A", 1, false, false, color);
    check(evicted && same_pixels(before, after),
          "bounded atlas page eviction rerasterizes stale entries");
    SDL_DestroySurface(after);
    after = NULL;
    glyphs_reset(&glyphs);
    after = draw_snapshot(&glyphs, frame, "A", 1, false, false, color);
    check(same_pixels(before, after),
          "device reset recreates atlas without changing text output");
    SDL_DestroySurface(before);
    SDL_DestroySurface(after);
    before = after = NULL;
    glyphs_reset(&glyphs);
    glyphs.atlas_size = 2; /* Deliberately too small for a glyph. */
    check(matches_reference(&glyphs, frame, glyphs.regular, "A", 1, color,
                            false, false, 1.0f) && !glyphs.cache['A'].page,
          "oversized glyph uses a temporary texture without disappearing");
    check(color_modulation(&glyphs, frame),
          "color glyphs ignore foreground tint but retain alpha blending");

    /* Runs of plain ASCII must line up exactly with the per-cell path.
     * Then a programming ligature has to change the glyphs, and a three-cell
     * ligature has to fit in its three cells. */
    {
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *run = NULL, *cells = NULL, *again = NULL;

        run = run_snapshot(&glyphs, frame, "ab", 2, false, false, white);
        cells = cells_snapshot(&glyphs, frame, "ab", 2, false, false, white);
        check(run && cells && same_pixels(run, cells),
              "shaped runs keep per-cell positions when no ligature applies");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        if (glyphs.bold && glyphs.italic) {
            run = run_snapshot(&glyphs, frame, "ab", 2, true, false, white);
            cells = cells_snapshot(&glyphs, frame, "ab", 2, true, false,
                                   white);
            check(run && cells && same_pixels(run, cells),
                  "bold shaped runs keep per-cell positions");
            SDL_DestroySurface(run);
            SDL_DestroySurface(cells);

            run = run_snapshot(&glyphs, frame, "ab", 2, false, true, white);
            cells = cells_snapshot(&glyphs, frame, "ab", 2, false, true,
                                   white);
            check(run && cells && same_pixels(run, cells),
                  "italic shaped runs keep per-cell positions");
            SDL_DestroySurface(run);
            SDL_DestroySurface(cells);
        }

        run = run_snapshot(&glyphs, frame, "->", 2, false, false, white);
        cells = cells_snapshot(&glyphs, frame, "->", 2, false, false, white);
        check(run && cells && strong_difference(run, cells) > 100,
              "a shaped run applies the font's -> ligature");
        check(ink_starts_near(run, cells, glyphs.cell_width / 2),
              "a two-cell ligature starts in its first cell");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        run = run_snapshot(&glyphs, frame, " ->", 3, false, false, white);
        cells = cells_snapshot(&glyphs, frame, " ->", 3, false, false, white);
        check(run && cells && ink_starts_near(run, cells,
                                              glyphs.cell_width / 2),
              "a ligature after spaces still starts in its own cell");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        run = run_snapshot(&glyphs, frame, "===", 3, false, false, white);
        cells = cells_snapshot(&glyphs, frame, "===", 3, false, false, white);
        check(run && cells && strong_difference(run, cells) > 100,
              "a three-cell ligature is shaped as one run");
        check(ink_starts_near(run, cells, glyphs.cell_width / 2),
              "a three-cell ligature starts in its first cell");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        run = run_snapshot(&glyphs, frame, "==>", 3, false, false, white);
        cells = cells_snapshot(&glyphs, frame, "==>", 3, false, false, white);
        check(run && cells && ink_starts_near(run, cells,
                                              glyphs.cell_width / 2),
              "a long arrow ligature starts in its first cell");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        run = run_snapshot(&glyphs, frame, "->", 2, true, false, white);
        cells = cells_snapshot(&glyphs, frame, "->", 2, true, false, white);
        check(run && cells && strong_difference(run, cells) > 100,
              "a bold shaped run keeps its ligature");
        SDL_DestroySurface(run);
        SDL_DestroySurface(cells);

        check(glyphs_can_join_run(&glyphs, "A", 1, false, false) &&
              glyphs_can_join_run(&glyphs, " ", 1, false, false),
              "plain ASCII cells can join a shaped run");
        check(!glyphs_can_join_run(&glyphs, cluster, sizeof cluster - 1,
                                   false, false),
              "combining clusters stay on the per-cell path");

        run = run_snapshot(&glyphs, frame, "->", 2, false, false, white);
        glyphs_reset(&glyphs);
        again = run_snapshot(&glyphs, frame, "->", 2, false, false, white);
        check(run && again && same_pixels(run, again),
              "device reset rerasterises ligature glyphs consistently");
        SDL_DestroySurface(run);
        SDL_DestroySurface(again);
    }

cleanup:
    SDL_DestroySurface(before);
    SDL_DestroySurface(after);
    glyphs_close(&glyphs);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(frame);
    if (fonts_initialized)
        TTF_Quit();
    if (initialized)
        SDL_Quit();
    return failures;
}
