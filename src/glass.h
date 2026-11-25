/*
 * termica - Liquid Glass backdrop and surface optics.
 *
 * Inspired by Apple's macOS Tahoe Liquid Glass material and the optical
 * pipeline in macos-tahoe-liquid-kde (squircle SDF, convex circular-cap rim
 * refraction, RGB chromatic dispersion, hexagonal Dual Kawase wallpaper blur,
 * BT.709 saturation boost, 3-stop cubic-smoothstep vertical glass veil,
 * inward exponential specular sheen, and opposing-light dual-contour glass
 * rims).
 *
 * Strict C99. Pure optical math is separated from SDL/Win32 so `--selftest`
 * exercises both the shader equations and software-rendered output.
 */

#ifndef TERMICA_GLASS_H
#define TERMICA_GLASS_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

/* Optical parameters tuned after macos-tahoe-liquid-kde's Tahoe Clear
 * preset: clear blurred backdrop, rim-gated convex refraction and RGB
 * dispersion, cool specular edge sheen, and vibrant saturation. */
typedef struct GlassOptics {
    float refraction_width;   /* logical px of the curved rim band          */
    float edge_lens_depth;    /* logical px of inward circular-cap pull     */
    float magnify_strength;   /* additional edge-gated inward lens pull     */
    float rgb_drift;          /* logical px of chromatic dispersion at rim  */
    float highlight_width;    /* logical px of inward exponential sheen     */
    float highlight_strength; /* peak inward specular intensity (0..1)      */
    float saturation;         /* BT.709 saturation multiplier               */
    float brightness;         /* linear brightness multiplier               */
    float gradient_center;    /* vertical stop position (0..1)              */
} GlassOptics;

typedef struct Glass {
    float        display_scale;
    int          mask_radius;
    float        keyline_width;
    float        rim_width;

    SDL_Texture *white_tex;       /* 2x2 solid white for geometry fills    */
    SDL_Texture *keyline_corner;  /* outer dark contour squircle arc       */
    SDL_Texture *rim_corner;      /* inner specular contour squircle arc   */
    SDL_Texture *pill_fill;       /* pre-rendered supersampled pill corner */
    SDL_Texture *pill_rim;        /* pre-rendered supersampled pill rim    */
    int          pill_radius;

    /* Cached low-res optical body + bevel refraction texture, plus a small
     * full-res 4-corner seam atlas so squircle corners stay 4x4-antialiased
     * without blurring at low body resolution. */
    SDL_Texture *body_tex;
    uint8_t     *body_rgba;
    int          body_w;
    int          body_h;

    SDL_Texture *corner_tex;
    uint8_t     *corner_rgba;
    int          corner_cap;      /* allocated single-corner size (px)     */

    /* Cache invalidation key for body_tex + corner_tex. */
    bool         cache_valid;
    bool         cached_obscure;
    int          cached_width;
    int          cached_height;
    int          cached_radius;
    SDL_Color    cached_bg;
    int          cached_opacity_q;
    int          cached_win_x;
    int          cached_win_y;
    uint32_t     cached_wp_gen;

    /* Pre-filtered desktop wallpaper (RGB24) for screen-space convex rim
     * refraction and chromatic dispersion, plus monitor bounds. */
    uint8_t     *wallpaper_rgb;
    int          wallpaper_w;
    int          wallpaper_h;
    uint32_t     wallpaper_gen;
    bool         wallpaper_loaded;
    int          win_screen_x;
    int          win_screen_y;
    int          mon_x;
    int          mon_y;
    int          mon_w;
    int          mon_h;

    GlassOptics  optics;
} Glass;

/* Populate default Tahoe Liquid Glass optical parameters. */
void glass_default_optics(GlassOptics *optics);

/* L4 superellipse ("squircle") signed distance in pixels from the surface
 * centre: negative inside, 0 on the boundary, positive outside. */
float glass_squircle_sdf(float px, float py, float half_w, float half_h,
                         float radius);

/* Unit outward normal of the squircle SDF at (px, py) relative to centre. */
void glass_squircle_normal(float px, float py, float half_w, float half_h,
                           float radius, float *out_nx, float *out_ny);

/* Cubic rim falloff (edgeQ in glass_core.glsl): 0 across the flat interior,
 * rising smoothly to 1 across `refraction_width` up to the outer rim. */
float glass_edge_factor(float sdf_distance, float refraction_width);

/* Tahoe convex circular-cap + edge-gated lens UV mapping. Leaves the
 * interior undistorted while bending background coordinates inward near the
 * curved glass rim. Coordinates are normalized in [0, 1]. */
void glass_refract_uv(float u, float v, float nx, float ny,
                      float sdf_distance, float refraction_width,
                      float edge_lens_depth, float magnify_strength,
                      float center_u, float center_v,
                      float bg_w, float bg_h,
                      float *out_u, float *out_v);

/* Chromatic dispersion along `outward_normal` (lgApplyRgbDrift): samples R, G
 * and B with opposing radial offsets and blends by `edge_q`. Output channels
 * are normalized in [0, 1]. */
void glass_sample_rgb_drift(const uint8_t *rgb, int w, int h,
                            float u, float v, float nx, float ny,
                            float drift_u, float drift_v, float edge_q,
                            float *out_r, float *out_g, float *out_b);

/* BT.709 saturation and brightness transform on normalized [0, 1] RGB. */
void glass_color_boost(float *r, float *g, float *b,
                       float saturation, float brightness);

/* Hexagonal 7-tap disc Kawase blur pass(es) in-place on an RGB24 buffer. */
void glass_kawase_blur_rgb(uint8_t *rgb, int w, int h, int passes);

/* Opposing-light specular rim colour at (x, y) on a `width` x `height`
 * surface (following GlassRim.qml's top-left -> bottom-right diagonal stops
 * and clipboard.py's subtle contour in `obscure` mode), with an optional
 * fluid pointer-proximity specular boost. */
SDL_Color glass_rim_color(float x, float y, float width, float height,
                          bool light_mode, bool obscure,
                          bool pointer_valid,
                          float pointer_x, float pointer_y);

/* Build GPU masks and state for the given display scale. */
bool glass_init(Glass *glass, SDL_Renderer *renderer, float display_scale);

/* Release all textures and buffers. Safe on a zeroed struct. */
void glass_free(Glass *glass);

/* Force optical body cache rebuild on the next frame. */
void glass_invalidate(Glass *glass);

/* Supply an explicit RGB24 backdrop image (used by headless tests and as a
 * wallpaper override); runs hexagonal Kawase pre-filtering automatically. */
bool glass_set_wallpaper_rgb(Glass *glass, const uint8_t *rgb, int w, int h);

/* Refresh window and monitor screen coordinates for live rim refraction.
 * Returns true when the quantized screen position changed. */
bool glass_sync_window_rect(Glass *glass, SDL_Window *window);

/* Render the full-window Liquid Glass backdrop, convex bevel refraction,
 * vertical glass veil, inward specular sheen, and dual-contour opposing-light
 * rim. When `obscure` is true, uses a deep smoked glass body with adaptive
 * wallpaper-hotspot suppression and a restrained hairline contour. */
void glass_draw(Glass *glass, SDL_Renderer *renderer,
                int width, int height, int corner_radius,
                SDL_Color base_bg, float opacity, bool obscure,
                bool pointer_valid, float pointer_x, float pointer_y);

/* Shared Liquid Glass rounded capsule primitive for chrome elements (tabs,
 * detached search bar, hover pills, toasts). */
void glass_draw_capsule(Glass *glass, SDL_Renderer *renderer,
                        float x, float y, float width, float height,
                        float radius, SDL_Color fill, bool light_mode,
                        float rim_alpha_scale);

/* Headless unit and software-renderer verification for `--selftest`. */
int glass_selftest(void);

#endif /* TERMICA_GLASS_H */
