/* Headless optical math and software-renderer checks for Liquid Glass. */
#include "glass.h"
#include "geometry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed)
        ++failures;
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

static int surface_diff_pixels(const SDL_Surface *a, const SDL_Surface *b,
                               int threshold)
{
    int x, y, count = 0;

    if (!a || !b || a->w != b->w || a->h != b->h)
        return 0;
    for (y = 0; y < a->h; y++) {
        const Uint8 *arow = (const Uint8 *)a->pixels + (size_t)y * a->pitch;
        const Uint8 *brow = (const Uint8 *)b->pixels + (size_t)y * b->pitch;
        for (x = 0; x < a->w; x++) {
            int dr = abs((int)arow[x * 4 + 0] - (int)brow[x * 4 + 0]);
            int dg = abs((int)arow[x * 4 + 1] - (int)brow[x * 4 + 1]);
            int db = abs((int)arow[x * 4 + 2] - (int)brow[x * 4 + 2]);
            if (dr > threshold || dg > threshold || db > threshold)
                count++;
        }
    }
    return count;
}

static void test_optics_math(void)
{
    float nx = 0.0f, ny = 0.0f;
    float u = 0.0f, v = 0.0f;
    float r, g, b;
    uint8_t split_img[16 * 8 * 3];
    uint8_t flat_img[8 * 8 * 3];
    SDL_Color rim_tl, rim_mid, rim_br, rim_ptr;
    int x, y;

    /* 1. Squircle SDF and outward normal. */
    check(glass_squircle_sdf(0.0f, 0.0f, 100.0f, 60.0f, 16.0f) < -40.0f,
          "squircle SDF is negative at the surface centre");
    check(glass_squircle_sdf(99.5f, 59.5f, 100.0f, 60.0f, 16.0f) > 0.0f,
          "squircle SDF is positive outside a rounded corner");
    check(fabsf(glass_squircle_sdf(100.0f, 0.0f, 100.0f, 60.0f, 16.0f)) < 0.01f,
          "squircle SDF vanishes on the straight side boundary");

    glass_squircle_normal(0.0f, -59.0f, 100.0f, 60.0f, 16.0f, &nx, &ny);
    check(fabsf(nx) < 0.01f && fabsf(ny + 1.0f) < 0.01f,
          "squircle top-edge outward normal points straight up");

    glass_squircle_normal(96.0f, 56.0f, 100.0f, 60.0f, 16.0f, &nx, &ny);
    check(nx > 0.5f && ny > 0.5f &&
          fabsf(sqrtf(nx * nx + ny * ny) - 1.0f) < 0.01f,
          "squircle corner outward normal is unit-length and diagonal");

    /* 2. Cubic rim falloff (edgeQ). */
    check(glass_edge_factor(-64.0f, 48.0f) == 0.0f,
          "rim factor is zero in the undistorted interior");
    check(fabsf(glass_edge_factor(0.0f, 48.0f) - 1.0f) < 0.001f,
          "rim factor reaches unity at the outer silhouette");
    check(glass_edge_factor(-12.0f, 48.0f) > 0.0f &&
          glass_edge_factor(-12.0f, 48.0f) < 1.0f,
          "rim factor transitions smoothly across the refraction band");

    /* 3. Tahoe convex circular-cap lens UV refraction. */
    glass_refract_uv(0.5f, 0.5f, 1.0f, 0.0f, -80.0f, 48.0f, 28.0f, 0.032f,
                     0.5f, 0.5f, 400.0f, 300.0f, &u, &v);
    check(fabsf(u - 0.5f) < 0.0001f && fabsf(v - 0.5f) < 0.0001f,
          "convex lens leaves centre coordinates undistorted");

    glass_refract_uv(0.95f, 0.5f, 1.0f, 0.0f, -2.0f, 48.0f, 28.0f, 0.032f,
                     0.5f, 0.5f, 400.0f, 300.0f, &u, &v);
    check(u < 0.95f && fabsf(v - 0.5f) < 0.001f,
          "convex rim lens bends right-edge coordinates inward");

    /* 4. Chromatic dispersion (lgApplyRgbDrift) across a sharp vertical edge. */
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 16; x++) {
            uint8_t val = x >= 8 ? 255 : 0;
            size_t idx = ((size_t)y * 16 + (size_t)x) * 3;
            split_img[idx + 0] = val;
            split_img[idx + 1] = val;
            split_img[idx + 2] = val;
        }
    }
    glass_sample_rgb_drift(split_img, 16, 8, 0.5f, 0.5f, 1.0f, 0.0f,
                           0.04f, 0.0f, 1.0f, &r, &g, &b);
    check(r > g && g > b,
          "chromatic dispersion separates R, G and B across a contrast edge");
    glass_sample_rgb_drift(split_img, 16, 8, 0.5f, 0.5f, 1.0f, 0.0f,
                           0.04f, 0.0f, 0.0f, &r, &g, &b);
    check(fabsf(r - g) < 0.001f && fabsf(g - b) < 0.001f,
          "chromatic dispersion vanishes when edgeQ is zero");

    /* 5. BT.709 saturation and brightness boost. */
    r = 0.5f; g = 0.5f; b = 0.5f;
    glass_color_boost(&r, &g, &b, 1.33f, 1.0f);
    check(fabsf(r - 0.5f) < 0.001f && fabsf(g - 0.5f) < 0.001f &&
          fabsf(b - 0.5f) < 0.001f,
          "saturation boost keeps neutral greys neutral");
    r = 0.8f; g = 0.2f; b = 0.2f;
    glass_color_boost(&r, &g, &b, 1.33f, 1.08f);
    check(r > 0.85f && b < 0.2f,
          "saturation and brightness boost enrich chromatic colours");

    /* 6. Hexagonal Kawase blur preserves flat colours and diffuses impulses. */
    for (x = 0; x < 8 * 8; x++) {
        flat_img[x * 3 + 0] = 120;
        flat_img[x * 3 + 1] = 160;
        flat_img[x * 3 + 2] = 210;
    }
    glass_kawase_blur_rgb(flat_img, 8, 8, 2);
    check(abs((int)flat_img[(4 * 8 + 4) * 3 + 0] - 120) <= 1 &&
          abs((int)flat_img[(4 * 8 + 4) * 3 + 1] - 160) <= 1 &&
          abs((int)flat_img[(4 * 8 + 4) * 3 + 2] - 210) <= 1,
          "hexagonal Kawase blur preserves uniform colour fields");

    memset(flat_img, 0, sizeof flat_img);
    flat_img[(4 * 8 + 4) * 3 + 0] = 255;
    glass_kawase_blur_rgb(flat_img, 8, 8, 2);
    check(flat_img[(4 * 8 + 4) * 3 + 0] < 200 &&
          flat_img[(4 * 8 + 3) * 3 + 0] > 0 &&
          flat_img[(3 * 8 + 4) * 3 + 0] > 0,
          "hexagonal Kawase blur spreads a sharp highlight smoothly");

    /* 7. Opposing-light specular rim (GlassRim.qml) + pointer proximity. */
    rim_tl = glass_rim_color(0.0f, 0.0f, 300.0f, 200.0f, false, false, false, 0, 0);
    rim_mid = glass_rim_color(60.0f, 130.0f, 300.0f, 200.0f, false, false, false, 0, 0);
    rim_br = glass_rim_color(300.0f, 200.0f, 300.0f, 200.0f, false, false, false, 0, 0);
    rim_ptr = glass_rim_color(60.0f, 130.0f, 300.0f, 200.0f, false, false, true,
                              62.0f, 130.0f);
    check(rim_tl.a > rim_mid.a && rim_br.a > rim_mid.a,
          "opposing-light glass rim highlights top-left and bottom-right corners");
    check(rim_ptr.a > rim_mid.a,
          "fluid pointer proximity brightens nearby specular glass rim");
    check(glass_rim_color(0.0f, 0.0f, 300.0f, 200.0f, false, true, false, 0, 0).a <
          rim_tl.a,
          "obscure glass variant uses a more restrained hairline rim");
}

static void test_software_render(void)
{
    SDL_Surface *frame = NULL, *snap_plain = NULL, *snap_wp = NULL, *snap_again = NULL;
    SDL_Renderer *renderer = NULL;
    Glass glass = {0};
    SDL_Color dark_bg = { 14, 18, 22, 255 };
    SDL_Color light_bg = { 242, 245, 250, 255 };
    uint8_t vibrant_wp[32 * 24 * 3];
    Uint8 r0 = 0, g0 = 0, b0 = 0, a0 = 255;
    Uint8 rc = 0, gc = 0, bc = 0, ac = 0;
    Uint8 rr = 0, gr = 0, br = 0, ar = 0;
    bool initialized = SDL_Init(0);
    bool ok = false;
    int radius, x, y;

    if (initialized)
        frame = SDL_CreateSurface(160, 120, SDL_PIXELFORMAT_RGBA32);
    if (frame)
        renderer = SDL_CreateSoftwareRenderer(frame);
    if (renderer)
        ok = glass_init(&glass, renderer, 1.0f);
    check(ok, "Liquid Glass software renderer initialization");
    if (!ok)
        goto cleanup;

    radius = geometry_corner_radius(1.0f);

    /* Clear to fully transparent and draw rounded Liquid Glass. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    glass_draw(&glass, renderer, 160, 120, radius, dark_bg, 1.0f, false,
               false, 0, 0);
    SDL_RenderPresent(renderer);
    snap_plain = SDL_DuplicateSurface(frame);

    SDL_ReadSurfacePixel(frame, 0, 0, &r0, &g0, &b0, &a0);
    SDL_ReadSurfacePixel(frame, 80, 60, &rc, &gc, &bc, &ac);
    SDL_ReadSurfacePixel(frame, 80, 1, &rr, &gr, &br, &ar);
    check(a0 == 0, "pixels outside the squircle corner remain transparent");
    check(ac > 20 && ac < 200,
          "Liquid Glass interior stays translucent for DWM blur behind it");
    check(ar > ac && ((int)rr + (int)gr + (int)br) > ((int)rc + (int)gc + (int)bc),
          "top specular glass rim is brighter than the translucent interior");

    /* Cache hit produces identical pixels. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    glass_draw(&glass, renderer, 160, 120, radius, dark_bg, 1.0f, false,
               false, 0, 0);
    SDL_RenderPresent(renderer);
    snap_again = SDL_DuplicateSurface(frame);
    check(same_pixels(snap_plain, snap_again),
          "cached Liquid Glass draw reproduces identical output");
    SDL_DestroySurface(snap_again);
    snap_again = NULL;

    /* Inject a vibrant synthetic wallpaper and verify bevel refraction tints
     * the glass rim while leaving exterior corners transparent. */
    for (y = 0; y < 24; y++) {
        for (x = 0; x < 32; x++) {
            size_t idx = ((size_t)y * 32 + (size_t)x) * 3;
            vibrant_wp[idx + 0] = (uint8_t)(x < 16 ? 245 : 30);
            vibrant_wp[idx + 1] = (uint8_t)(y < 12 ? 90 : 210);
            vibrant_wp[idx + 2] = (uint8_t)(x < 16 ? 120 : 250);
        }
    }
    check(glass_set_wallpaper_rgb(&glass, vibrant_wp, 32, 24),
          "custom RGB backdrop uploads and Kawase-filters cleanly");

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    glass_draw(&glass, renderer, 160, 120, radius, dark_bg, 1.0f, false,
               false, 0, 0);
    SDL_RenderPresent(renderer);
    snap_wp = SDL_DuplicateSurface(frame);
    SDL_ReadSurfacePixel(frame, 0, 0, &r0, &g0, &b0, &a0);
    check(a0 == 0 && surface_diff_pixels(snap_plain, snap_wp, 4) > 50,
          "wallpaper-coupled convex bevel refraction tints the glass rim");

    /* Obscure (`glass_dark`) variant over a bright colorful wallpaper must
     * apply deep smoked attenuation while keeping corners transparent. */
    {
        Uint8 ro = 0, go = 0, bo = 0, ao = 0;
        Uint8 rc_clear = 0, gc_clear = 0, bc_clear = 0, ac_clear = 0;

        SDL_ReadSurfacePixel(snap_wp, 80, 60, &rc_clear, &gc_clear, &bc_clear,
                             &ac_clear);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        glass_draw(&glass, renderer, 160, 120, radius, dark_bg, 1.0f, true,
                   false, 0, 0);
        SDL_RenderPresent(renderer);
        SDL_ReadSurfacePixel(frame, 0, 0, &r0, &g0, &b0, &a0);
        SDL_ReadSurfacePixel(frame, 80, 60, &ro, &go, &bo, &ao);
        check(a0 == 0 && ao > ac_clear + 70 && ao < 245 &&
              ((int)ro + (int)go + (int)bo) < 32,
              "obscure Liquid Glass smokes bright wallpaper down to deep obsidian");
    }

    /* Light colour scheme adapts to a bright frosted glass veil. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    glass_draw(&glass, renderer, 160, 120, radius, light_bg, 1.0f, false,
               false, 0, 0);
    SDL_RenderPresent(renderer);
    SDL_ReadSurfacePixel(frame, 80, 60, &r0, &g0, &b0, &a0);
    check(r0 > rc && g0 > gc && b0 > bc,
          "light colour scheme switches Liquid Glass to a luminous light veil");

    /* Shared capsule primitive renders a rounded translucent pill. */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    glass_draw_capsule(&glass, renderer, 20.0f, 30.0f, 120.0f, 36.0f, 10.0f,
                       (SDL_Color){ 255, 255, 255, 64 }, false, 1.0f);
    SDL_RenderPresent(renderer);
    SDL_ReadSurfacePixel(frame, 20, 30, &r0, &g0, &b0, &a0);
    SDL_ReadSurfacePixel(frame, 80, 48, &rc, &gc, &bc, &ac);
    check(a0 == 0 && ac > 40,
          "shared glass capsule clips pill corners and fills interior");

    /* Re-init after device reset / DPI scale change succeeds cleanly. */
    glass_free(&glass);
    check(glass_init(&glass, renderer, 1.5f),
          "Liquid Glass re-initializes cleanly at 1.5x display scale");

cleanup:
    SDL_DestroySurface(snap_plain);
    SDL_DestroySurface(snap_wp);
    SDL_DestroySurface(snap_again);
    glass_free(&glass);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (frame)
        SDL_DestroySurface(frame);
    if (initialized)
        SDL_Quit();
}

int glass_selftest(void)
{
    failures = 0;
    test_optics_math();
    test_software_render();
    return failures;
}
