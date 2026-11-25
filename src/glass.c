/*
 * termica - Liquid Glass backdrop and surface optics. See glass.h.
 */

#include "glass.h"

#include "geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#endif

/* Supersampling factor per axis for antialiased squircle contour masks. */
#define GLASS_SAMPLES 4

/* Resolution cap for the band-limited optical body texture. GPU bilinear
 * filtering upsamples smooth gradients, sheen and bevel refraction cleanly
 * while full-resolution corner & contour masks preserve crisp edges. */
#define GLASS_BODY_MAX_W 160
#define GLASS_BODY_MAX_H 104

/* Target dimensions for the pre-filtered desktop wallpaper buffer. */
#define GLASS_WP_TARGET_W 320
#define GLASS_WP_TARGET_H 180

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float smoothstepf(float edge0, float edge1, float x)
{
    float denom = edge1 - edge0;
    float t;

    if (denom <= 0.000001f)
        return x >= edge1 ? 1.0f : 0.0f;
    t = clampf((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void glass_default_optics(GlassOptics *optics)
{
    if (!optics)
        return;
    optics->refraction_width   = 38.0f;
    optics->edge_lens_depth    = 24.0f;
    optics->magnify_strength   = 0.028f;
    optics->rgb_drift          = 8.0f;
    optics->highlight_width    = 8.0f;
    optics->highlight_strength = 0.065f;
    optics->saturation         = 1.28f;
    optics->brightness         = 0.76f;
    optics->gradient_center    = 0.56f;
}

float glass_squircle_sdf(float px, float py, float half_w, float half_h,
                         float radius)
{
    float max_r = half_w < half_h ? half_w : half_h;
    float r = clampf(radius, 0.0f, max_r > 0.0f ? max_r : 0.0f);
    float qx = fabsf(px) - (half_w - r);
    float qy = fabsf(py) - (half_h - r);
    float cx = qx > 0.0f ? qx : 0.0f;
    float cy = qy > 0.0f ? qy : 0.0f;
    float inside_d = qx > qy ? qx : qy;
    float corner_len;

    if (inside_d > 0.0f)
        inside_d = 0.0f;
    if (r <= 0.0f) {
        corner_len = sqrtf(cx * cx + cy * cy);
    } else {
        float c2x = cx * cx;
        float c2y = cy * cy;
        corner_len = sqrtf(sqrtf(c2x * c2x + c2y * c2y));
    }
    return inside_d + corner_len - r;
}

void glass_squircle_normal(float px, float py, float half_w, float half_h,
                           float radius, float *out_nx, float *out_ny)
{
    float max_r = half_w < half_h ? half_w : half_h;
    float r = clampf(radius, 0.0f, max_r > 0.0f ? max_r : 0.0f);
    float qx = fabsf(px) - (half_w - r);
    float qy = fabsf(py) - (half_h - r);
    float cx = qx > 0.0f ? qx : 0.0f;
    float cy = qy > 0.0f ? qy : 0.0f;
    float sx = px >= 0.0f ? 1.0f : -1.0f;
    float sy = py >= 0.0f ? 1.0f : -1.0f;
    float gx, gy, len;

    if (cx > 0.001f || cy > 0.001f) {
        if (r > 0.0f) {
            gx = cx * cx * cx;
            gy = cy * cy * cy;
        } else {
            gx = cx;
            gy = cy;
        }
        len = sqrtf(gx * gx + gy * gy);
        if (len > 1e-12f) {
            *out_nx = (gx / len) * sx;
            *out_ny = (gy / len) * sy;
            return;
        }
    }
    if (qx > qy) {
        *out_nx = sx;
        *out_ny = 0.0f;
    } else {
        *out_nx = 0.0f;
        *out_ny = sy;
    }
}

float glass_edge_factor(float sdf_distance, float refraction_width)
{
    float band = refraction_width > 1.0f ? refraction_width : 1.0f;
    float edge_t = smoothstepf(-band, 0.0f, sdf_distance);

    return edge_t * edge_t * edge_t;
}

void glass_refract_uv(float u, float v, float nx, float ny,
                      float sdf_distance, float refraction_width,
                      float edge_lens_depth, float magnify_strength,
                      float center_u, float center_v,
                      float bg_w, float bg_h,
                      float *out_u, float *out_v)
{
    float band = refraction_width > 1.0f ? refraction_width : 1.0f;
    float inside = sdf_distance < 0.0f ? -sdf_distance : 0.0f;
    float edge_q = glass_edge_factor(sdf_distance, band);
    float safe_w = bg_w > 1.0f ? bg_w : 1.0f;
    float safe_h = bg_h > 1.0f ? bg_h : 1.0f;
    float ru = u;
    float rv = v;

    if (edge_lens_depth > 0.0f && inside < band) {
        /* Tahoe circular-cap bevel profile: steep near the silhouette,
         * flattening smoothly to zero at `refraction_width`. */
        float x = clampf(inside / band, 0.0f, 1.0f);
        float omx = 1.0f - x;
        float rad = 1.0f - omx * omx;
        float cap = 1.0f - sqrtf(rad > 0.0f ? rad : 0.0f);

        ru -= nx * (edge_lens_depth * cap) / safe_w;
        rv -= ny * (edge_lens_depth * cap) / safe_h;
    }
    if (magnify_strength > 0.0f && edge_q > 0.0f) {
        float mag = magnify_strength * edge_q * 4.5f;
        ru = center_u + (ru - center_u) * (1.0f - mag);
        rv = center_v + (rv - center_v) * (1.0f - mag);
    }

    *out_u = clampf(ru, 0.0f, 1.0f);
    *out_v = clampf(rv, 0.0f, 1.0f);
}

static void sample_bilinear_rgb(const uint8_t *rgb, int w, int h,
                                float u, float v,
                                float *r, float *g, float *b)
{
    float fx = clampf(u, 0.0f, 1.0f) * (float)(w - 1);
    float fy = clampf(v, 0.0f, 1.0f) * (float)(h - 1);
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    const uint8_t *p00 = rgb + ((size_t)y0 * (size_t)w + (size_t)x0) * 3;
    const uint8_t *p10 = rgb + ((size_t)y0 * (size_t)w + (size_t)x1) * 3;
    const uint8_t *p01 = rgb + ((size_t)y1 * (size_t)w + (size_t)x0) * 3;
    const uint8_t *p11 = rgb + ((size_t)y1 * (size_t)w + (size_t)x1) * 3;
    float w00 = (1.0f - tx) * (1.0f - ty) * (1.0f / 255.0f);
    float w10 = tx * (1.0f - ty) * (1.0f / 255.0f);
    float w01 = (1.0f - tx) * ty * (1.0f / 255.0f);
    float w11 = tx * ty * (1.0f / 255.0f);

    *r = (float)p00[0] * w00 + (float)p10[0] * w10 +
         (float)p01[0] * w01 + (float)p11[0] * w11;
    *g = (float)p00[1] * w00 + (float)p10[1] * w10 +
         (float)p01[1] * w01 + (float)p11[1] * w11;
    *b = (float)p00[2] * w00 + (float)p10[2] * w10 +
         (float)p01[2] * w01 + (float)p11[2] * w11;
}

void glass_sample_rgb_drift(const uint8_t *rgb, int w, int h,
                            float u, float v, float nx, float ny,
                            float drift_u, float drift_v, float edge_q,
                            float *out_r, float *out_g, float *out_b)
{
    float base_r, base_g, base_b;
    float rr, dummy1, dummy2;
    float gg, bb;
    float du, dv, q;

    if (!rgb || w <= 0 || h <= 0) {
        *out_r = *out_g = *out_b = 0.0f;
        return;
    }
    sample_bilinear_rgb(rgb, w, h, u, v, &base_r, &base_g, &base_b);
    q = clampf(edge_q, 0.0f, 1.0f);
    if (q <= 0.0005f) {
        *out_r = base_r;
        *out_g = base_g;
        *out_b = base_b;
        return;
    }

    du = nx * drift_u;
    dv = ny * drift_v;
    sample_bilinear_rgb(rgb, w, h, u + du, v + dv, &rr, &dummy1, &dummy2);
    sample_bilinear_rgb(rgb, w, h, u + du * 0.30f, v + dv * 0.30f,
                        &dummy1, &gg, &dummy2);
    sample_bilinear_rgb(rgb, w, h, u - du * 0.25f, v - dv * 0.25f,
                        &dummy1, &dummy2, &bb);

    *out_r = base_r + (rr - base_r) * q;
    *out_g = base_g + (gg - base_g) * q;
    *out_b = base_b + (bb - base_b) * q;
}

void glass_color_boost(float *r, float *g, float *b,
                       float saturation, float brightness)
{
    /* BT.709 luma weights matching KWin's colorTransformMatrix. */
    float luma = *r * 0.2126f + *g * 0.7152f + *b * 0.0722f;
    float sr = luma + (*r - luma) * saturation;
    float sg = luma + (*g - luma) * saturation;
    float sb = luma + (*b - luma) * saturation;

    *r = clampf(sr * brightness, 0.0f, 1.0f);
    *g = clampf(sg * brightness, 0.0f, 1.0f);
    *b = clampf(sb * brightness, 0.0f, 1.0f);
}

void glass_kawase_blur_rgb(uint8_t *rgb, int w, int h, int passes)
{
    /* 7-tap hexagonal disc kernel (centre weight 4 + 6 taps at 60 deg steps,
     * total weight 10) from macos-tahoe-liquid-kde's downsample.frag. */
    static const float offsets[6][2] = {
        {  1.0f,  0.0f   },
        {  0.5f,  0.866f },
        { -0.5f,  0.866f },
        { -1.0f,  0.0f   },
        { -0.5f, -0.866f },
        {  0.5f, -0.866f }
    };
    uint8_t *scratch;
    size_t bytes;
    int pass, x, y, tap;

    if (!rgb || w <= 1 || h <= 1 || passes <= 0)
        return;
    bytes = (size_t)w * (size_t)h * 3;
    scratch = (uint8_t *)malloc(bytes);
    if (!scratch)
        return;

    for (pass = 0; pass < passes; pass++) {
        float radius = 1.0f + (float)pass * 1.15f;

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const uint8_t *center = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
                float sr = (float)center[0] * 4.0f;
                float sg = (float)center[1] * 4.0f;
                float sb = (float)center[2] * 4.0f;
                uint8_t *dst = scratch + ((size_t)y * (size_t)w + (size_t)x) * 3;

                for (tap = 0; tap < 6; tap++) {
                    int sx = (int)((float)x + offsets[tap][0] * radius + 0.5f);
                    int sy = (int)((float)y + offsets[tap][1] * radius + 0.5f);
                    const uint8_t *p;

                    if (sx < 0) sx = 0;
                    if (sx >= w) sx = w - 1;
                    if (sy < 0) sy = 0;
                    if (sy >= h) sy = h - 1;
                    p = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
                    sr += (float)p[0];
                    sg += (float)p[1];
                    sb += (float)p[2];
                }
                dst[0] = (uint8_t)(sr * 0.1f + 0.5f);
                dst[1] = (uint8_t)(sg * 0.1f + 0.5f);
                dst[2] = (uint8_t)(sb * 0.1f + 0.5f);
            }
        }
        memcpy(rgb, scratch, bytes);
    }
    free(scratch);
}

SDL_Color glass_rim_color(float x, float y, float width, float height,
                          bool light_mode, bool obscure,
                          bool pointer_valid,
                          float pointer_x, float pointer_y)
{
    /* Project (x, y) onto the diagonal (0, 0) -> (0.3 * width, height)
     * matching GlassRim.qml's opposing-light linear gradient stops, scaled down
     * in `obscure` mode to match clipboard.py's subtle hairline contour. */
    float safe_w = width > 1.0f ? width : 1.0f;
    float safe_h = height > 1.0f ? height : 1.0f;
    float gx = 0.3f * safe_w;
    float gy = safe_h;
    float denom = gx * gx + gy * gy;
    float t = denom > 0.0f ? clampf((x * gx + y * gy) / denom, 0.0f, 1.0f) : 0.0f;
    float top_a = obscure ? (light_mode ? 0.62f : 0.32f)
                          : (light_mode ? 0.82f : 0.62f);
    float mid_a = obscure ? (light_mode ? 0.18f : 0.14f)
                          : (light_mode ? 0.14f : 0.11f);
    float low_a = obscure ? (light_mode ? 0.12f : 0.10f)
                          : (light_mode ? 0.08f : 0.07f);
    float bot_a = obscure ? (light_mode ? 0.42f : 0.24f)
                          : (light_mode ? 0.56f : 0.42f);
    float alpha;
    float r = obscure ? 228.0f : 242.0f;
    float g = obscure ? 236.0f : 248.0f;
    float b = 255.0f;
    SDL_Color out;

    if (t < 0.45f) {
        float u = smoothstepf(0.0f, 0.45f, t);
        alpha = top_a + (mid_a - top_a) * u;
    } else if (t < 0.72f) {
        float u = smoothstepf(0.45f, 0.72f, t);
        alpha = mid_a + (low_a - mid_a) * u;
    } else {
        float u = smoothstepf(0.72f, 1.0f, t);
        alpha = low_a + (bot_a - low_a) * u;
    }

    /* Overhead light bias: the top horizontal rim catches extra sky light. */
    if (y < safe_h * 0.08f) {
        float sky = 1.0f - y / (safe_h * 0.08f);
        float sky_boost = obscure ? 0.06f : 0.14f;
        alpha = clampf(alpha + sky_boost * sky, 0.0f, obscure ? 0.55f : 0.88f);
    }

    /* Fluid pointer-proximity specular highlight along the glass silhouette. */
    if (pointer_valid) {
        float dx = x - pointer_x;
        float dy = y - pointer_y;
        float radius = 140.0f;
        float dist2 = dx * dx + dy * dy;
        float glow = expf(-dist2 / (2.0f * radius * radius));
        float ptr_boost = obscure ? 0.20f : 0.30f;

        alpha = clampf(alpha + ptr_boost * glow, 0.0f, obscure ? 0.68f : 0.94f);
        r = r + (255.0f - r) * glow;
        g = g + (252.0f - g) * glow;
    }

    out.r = (Uint8)(r + 0.5f);
    out.g = (Uint8)(g + 0.5f);
    out.b = (Uint8)(b + 0.5f);
    out.a = (Uint8)(clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Supersampled squircle masks                                               */
/* ------------------------------------------------------------------------- */

static Uint8 sample_squircle_band(float ox, float oy, float radius,
                                  float outer_inset, float inner_inset)
{
    int hit = 0;
    int sx, sy;
    float r_out = radius - outer_inset;
    float r_in  = radius - inner_inset;

    if (r_out <= 0.0f)
        return 0;
    for (sy = 0; sy < GLASS_SAMPLES; sy++) {
        for (sx = 0; sx < GLASS_SAMPLES; sx++) {
            float u = radius - (ox + ((float)sx + 0.5f) / GLASS_SAMPLES);
            float v = radius - (oy + ((float)sy + 0.5f) / GLASS_SAMPLES);

            if (geometry_inside_squircle(u, v, r_out) &&
                (r_in <= 0.0f || !geometry_inside_squircle(u, v, r_in)))
                hit++;
        }
    }
    return (Uint8)(hit * 255 / (GLASS_SAMPLES * GLASS_SAMPLES));
}

static SDL_Texture *build_ring_corner(SDL_Renderer *renderer, int size,
                                      float outer_inset, float inner_inset)
{
    SDL_Surface *surface;
    SDL_Texture *texture;
    Uint8 *pixels;
    int x, y;

    if (size <= 0)
        return NULL;
    surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        return NULL;
    pixels = (Uint8 *)surface->pixels;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            Uint8 *p = pixels + (size_t)y * surface->pitch + (size_t)x * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = sample_squircle_band((float)x, (float)y, (float)size,
                                        outer_inset, inner_inset);
        }
    }
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    return texture;
}

static SDL_Texture *build_white_tex(SDL_Renderer *renderer)
{
    SDL_Surface *surface = SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture *texture;

    if (!surface)
        return NULL;
    memset(surface->pixels, 255, (size_t)surface->pitch * 2);
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}

/* ------------------------------------------------------------------------- */
/* Desktop wallpaper loader (Windows WIC, graceful fallback elsewhere)       */
/* ------------------------------------------------------------------------- */

bool glass_set_wallpaper_rgb(Glass *glass, const uint8_t *rgb, int w, int h)
{
    uint8_t *copy;
    size_t bytes;

    if (!glass || !rgb || w <= 0 || h <= 0 ||
        (size_t)w > SIZE_MAX / 3 / (size_t)h)
        return false;
    bytes = (size_t)w * (size_t)h * 3;
    copy = (uint8_t *)malloc(bytes);
    if (!copy)
        return false;
    memcpy(copy, rgb, bytes);
    glass_kawase_blur_rgb(copy, w, h, 3);

    free(glass->wallpaper_rgb);
    glass->wallpaper_rgb = copy;
    glass->wallpaper_w = w;
    glass->wallpaper_h = h;
    glass->wallpaper_loaded = true;
    glass->wallpaper_gen++;
    glass->cache_valid = false;
    return true;
}

#ifdef _WIN32
static const GUID glass_clsid_wic_factory = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}
};
static const GUID glass_iid_wic_factory = {
    0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}
};
static const GUID glass_wic_rgba32 = {
    0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}
};

static void load_system_wallpaper(Glass *glass)
{
    WCHAR path[MAX_PATH];
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    uint8_t *rgba = NULL;
    uint8_t *rgb = NULL;
    HRESULT hr;
    bool com_inited = false;
    int w = GLASS_WP_TARGET_W;
    int h = GLASS_WP_TARGET_H;
    int i;

    if (glass->wallpaper_loaded)
        return;
    glass->wallpaper_loaded = true;

    path[0] = L'\0';
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) &&
        path[0] != L'\0') {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE)
            hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        com_inited = SUCCEEDED(hr);

        hr = CoCreateInstance(&glass_clsid_wic_factory, NULL,
                              CLSCTX_INPROC_SERVER, &glass_iid_wic_factory,
                              (void **)&factory);
        if (SUCCEEDED(hr) && factory) {
            hr = factory->lpVtbl->CreateDecoderFromFilename(
                factory, path, NULL, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder);
        }
        if (SUCCEEDED(hr) && decoder)
            hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
        if (SUCCEEDED(hr) && factory && frame)
            hr = factory->lpVtbl->CreateBitmapScaler(factory, &scaler);
        if (SUCCEEDED(hr) && scaler) {
            hr = scaler->lpVtbl->Initialize(
                scaler, (IWICBitmapSource *)frame, (UINT)w, (UINT)h,
                WICBitmapInterpolationModeFant);
        }
        if (SUCCEEDED(hr) && factory && scaler)
            hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
        if (SUCCEEDED(hr) && converter) {
            hr = converter->lpVtbl->Initialize(
                converter, (IWICBitmapSource *)scaler, &glass_wic_rgba32,
                WICBitmapDitherTypeNone, NULL, 0.0,
                WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr) && converter) {
            rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
            rgb  = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
            if (rgba && rgb &&
                SUCCEEDED(converter->lpVtbl->CopyPixels(
                    converter, NULL, (UINT)(w * 4),
                    (UINT)(w * h * 4), rgba))) {
                for (i = 0; i < w * h; i++) {
                    rgb[i * 3 + 0] = rgba[i * 4 + 0];
                    rgb[i * 3 + 1] = rgba[i * 4 + 1];
                    rgb[i * 3 + 2] = rgba[i * 4 + 2];
                }
                glass_kawase_blur_rgb(rgb, w, h, 4);
                free(glass->wallpaper_rgb);
                glass->wallpaper_rgb = rgb;
                glass->wallpaper_w = w;
                glass->wallpaper_h = h;
                glass->wallpaper_gen++;
                rgb = NULL;
            }
        }
        free(rgba);
        free(rgb);
        if (converter) converter->lpVtbl->Release(converter);
        if (scaler)    scaler->lpVtbl->Release(scaler);
        if (frame)     frame->lpVtbl->Release(frame);
        if (decoder)   decoder->lpVtbl->Release(decoder);
        if (factory)   factory->lpVtbl->Release(factory);
        if (com_inited)
            CoUninitialize();
        if (glass->wallpaper_rgb)
            return;
    }

    /* Fallback when no image wallpaper is set: solid desktop background. */
    {
        DWORD desk = GetSysColor(COLOR_DESKTOP);
        uint8_t solid[4 * 4 * 3];

        for (i = 0; i < 16; i++) {
            solid[i * 3 + 0] = GetRValue(desk);
            solid[i * 3 + 1] = GetGValue(desk);
            solid[i * 3 + 2] = GetBValue(desk);
        }
        glass_set_wallpaper_rgb(glass, solid, 4, 4);
    }
}
#else
static void load_system_wallpaper(Glass *glass)
{
    glass->wallpaper_loaded = true;
}
#endif

bool glass_sync_window_rect(Glass *glass, SDL_Window *window)
{
    int wx = 0, wy = 0;
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    int qx, qy;

    if (!glass || !window)
        return false;

#ifdef _WIN32
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        RECT wr;
        HMONITOR mon;
        MONITORINFO mi;

        if (hwnd && GetWindowRect(hwnd, &wr)) {
            wx = wr.left;
            wy = wr.top;
            mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            memset(&mi, 0, sizeof mi);
            mi.cbSize = sizeof mi;
            if (mon && GetMonitorInfoW(mon, &mi)) {
                mx = mi.rcMonitor.left;
                my = mi.rcMonitor.top;
                mw = mi.rcMonitor.right - mi.rcMonitor.left;
                mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            }
        }
    }
#else
    SDL_GetWindowPosition(window, &wx, &wy);
#endif

    if (mw < 1) mw = 1920;
    if (mh < 1) mh = 1080;
    glass->mon_x = mx;
    glass->mon_y = my;
    glass->mon_w = mw;
    glass->mon_h = mh;

    /* Quantize screen coordinates to 8 px so tiny drag sub-steps do not
     * churn the optical bevel cache unnecessarily. */
    qx = (wx - mx) & ~7;
    qy = (wy - my) & ~7;
    if (qx != glass->win_screen_x || qy != glass->win_screen_y) {
        glass->win_screen_x = qx;
        glass->win_screen_y = qy;
        glass->cache_valid = false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

bool glass_init(Glass *glass, SDL_Renderer *renderer, float display_scale)
{
    if (!glass || !renderer)
        return false;
    memset(glass, 0, sizeof *glass);
    if (display_scale <= 0.0f)
        display_scale = 1.0f;

    glass->display_scale = display_scale;
    glass_default_optics(&glass->optics);
    glass->mask_radius = geometry_corner_radius(display_scale);
    glass->keyline_width = 0.95f * display_scale;
    if (glass->keyline_width < 0.85f)
        glass->keyline_width = 0.85f;
    glass->rim_width = 1.15f * display_scale;
    if (glass->rim_width < 1.0f)
        glass->rim_width = 1.0f;

    glass->pill_radius = (int)(12.0f * display_scale + 0.5f);
    if (glass->pill_radius < 6)
        glass->pill_radius = 6;

    glass->white_tex = build_white_tex(renderer);
    glass->keyline_corner = build_ring_corner(
        renderer, glass->mask_radius, 0.0f, glass->keyline_width);
    glass->rim_corner = build_ring_corner(
        renderer, glass->mask_radius, glass->keyline_width * 0.5f,
        glass->keyline_width * 0.5f + glass->rim_width);
    glass->pill_fill = build_ring_corner(
        renderer, glass->pill_radius, 0.0f, (float)glass->pill_radius + 1.0f);
    glass->pill_rim = build_ring_corner(
        renderer, glass->pill_radius, 0.0f, 1.15f * display_scale);

    glass->mon_w = 1920;
    glass->mon_h = 1080;

    if (!glass->white_tex || !glass->keyline_corner || !glass->rim_corner ||
        !glass->pill_fill || !glass->pill_rim) {
        glass_free(glass);
        return false;
    }
    return true;
}

void glass_free(Glass *glass)
{
    if (!glass)
        return;
    if (glass->white_tex)      SDL_DestroyTexture(glass->white_tex);
    if (glass->keyline_corner) SDL_DestroyTexture(glass->keyline_corner);
    if (glass->rim_corner)     SDL_DestroyTexture(glass->rim_corner);
    if (glass->pill_fill)      SDL_DestroyTexture(glass->pill_fill);
    if (glass->pill_rim)       SDL_DestroyTexture(glass->pill_rim);
    if (glass->body_tex)       SDL_DestroyTexture(glass->body_tex);
    if (glass->corner_tex)     SDL_DestroyTexture(glass->corner_tex);
    free(glass->body_rgba);
    free(glass->corner_rgba);
    free(glass->wallpaper_rgb);
    memset(glass, 0, sizeof *glass);
}

void glass_invalidate(Glass *glass)
{
    if (glass)
        glass->cache_valid = false;
}

/* ------------------------------------------------------------------------- */
/* Optical body + bevel synthesis                                            */
/* ------------------------------------------------------------------------- */

/* Evaluate the 3-stop cubic-smoothstep vertical glass veil matching
 * gradientOverlayAt() in macos-tahoe-liquid-kde/src/gradientcolors.h and the
 * deep smoked attenuation of Clipboard / Golden Gate in `obscure` mode. */
static void glass_vertical_veil(float height_frac, float center_pos,
                                bool light_mode, bool obscure,
                                float *veil_r, float *veil_g,
                                float *veil_b, float *veil_a)
{
    float cp = clampf(center_pos, 0.05f, 0.95f);
    bool upper = height_frac < cp;
    float top_a, mid_a, bot_a;
    float a0, a1, seg_start, seg_len, t;

    if (obscure) {
        top_a = light_mode ? 0.52f : 0.54f;
        mid_a = light_mode ? 0.34f : 0.30f;
        bot_a = light_mode ? 0.40f : 0.22f;
    } else {
        top_a = light_mode ? 0.36f : 0.26f;
        mid_a = light_mode ? 0.16f : 0.08f;
        bot_a = light_mode ? 0.24f : 0.14f;
    }
    a0 = upper ? top_a : mid_a;
    a1 = upper ? mid_a : bot_a;
    seg_start = upper ? 0.0f : cp;
    seg_len = upper ? cp : (1.0f - cp);
    t = smoothstepf(0.0f, 1.0f, (height_frac - seg_start) / seg_len);

    *veil_a = a0 + (a1 - a0) * t;
    if (light_mode) {
        *veil_r = 0.96f;
        *veil_g = 0.98f;
        *veil_b = 1.00f;
    } else if (obscure) {
        /* Deep obsidian-midnight smoke matching the macOS Tahoe Clipboard
         * dark glass card: near-black at the top, rich deep navy below. */
        *veil_r = 0.010f + 0.012f * height_frac;
        *veil_g = 0.016f + 0.018f * height_frac;
        *veil_b = 0.038f + 0.030f * height_frac;
    } else {
        *veil_r = 0.035f + 0.020f * height_frac;
        *veil_g = 0.050f + 0.020f * height_frac;
        *veil_b = 0.085f + 0.030f * (1.0f - height_frac);
    }
}

static void sample_body_bilinear(const uint8_t *rgba, int bw, int bh,
                                 float u, float v, Uint8 out[4])
{
    float fx = clampf(u, 0.0f, 1.0f) * (float)(bw - 1);
    float fy = clampf(v, 0.0f, 1.0f) * (float)(bh - 1);
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1 < bw ? x0 + 1 : x0;
    int y1 = y0 + 1 < bh ? y0 + 1 : y0;
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    const uint8_t *p00 = rgba + ((size_t)y0 * (size_t)bw + (size_t)x0) * 4;
    const uint8_t *p10 = rgba + ((size_t)y0 * (size_t)bw + (size_t)x1) * 4;
    const uint8_t *p01 = rgba + ((size_t)y1 * (size_t)bw + (size_t)x0) * 4;
    const uint8_t *p11 = rgba + ((size_t)y1 * (size_t)bw + (size_t)x1) * 4;
    float w00 = (1.0f - tx) * (1.0f - ty);
    float w10 = tx * (1.0f - ty);
    float w01 = (1.0f - tx) * ty;
    float w11 = tx * ty;
    int c;

    for (c = 0; c < 4; c++) {
        float val = (float)p00[c] * w00 + (float)p10[c] * w10 +
                    (float)p01[c] * w01 + (float)p11[c] * w11;
        out[c] = (Uint8)(clampf(val, 0.0f, 255.0f) + 0.5f);
    }
}

static bool rebuild_optical_cache(Glass *glass, SDL_Renderer *renderer,
                                  int width, int height, int radius,
                                  SDL_Color base_bg, float opacity,
                                  bool obscure)
{
    int bw = width / 4;
    int bh = height / 4;
    bool light_mode = ((int)base_bg.r + (int)base_bg.g + (int)base_bg.b) > 384;
    float scale = glass->display_scale > 0.0f ? glass->display_scale : 1.0f;
    float refr_band = (obscure ? 22.0f : glass->optics.refraction_width) * scale;
    float lens_depth = (obscure ? 18.0f : glass->optics.edge_lens_depth) * scale;
    float drift_px = (obscure ? 5.5f : glass->optics.rgb_drift) * scale;
    float sheen_band = (obscure ? 4.5f : glass->optics.highlight_width) * scale;
    float sheen_str = obscure
        ? (light_mode ? 0.06f : 0.028f)
        : (glass->optics.highlight_strength * (light_mode ? 1.25f : 1.0f));
    float half_w = (float)width * 0.5f;
    float half_h = (float)height * 0.5f;
    float mon_w = (float)(glass->mon_w > 0 ? glass->mon_w : width);
    float mon_h = (float)(glass->mon_h > 0 ? glass->mon_h : height);
    float center_u = ((float)glass->win_screen_x + half_w) / mon_w;
    float center_v = ((float)glass->win_screen_y + half_h) / mon_h;
    float base_r = (float)base_bg.r / 255.0f;
    float base_g = (float)base_bg.g / 255.0f;
    float base_b = (float)base_bg.b / 255.0f;
    float base_tint;
    int x, y;

    if (obscure && !light_mode) {
        /* Deepen base colour toward smoked obsidian-midnight (~78% total body
         * attenuation with the 3-stop veil, matching clipboard.py). */
        base_r = base_r * 0.55f + 0.008f;
        base_g = base_g * 0.55f + 0.014f;
        base_b = base_b * 0.65f + 0.032f;
        base_tint = 0.68f;
    } else if (obscure) {
        base_tint = 0.60f;
    } else {
        base_tint = light_mode ? 0.26f : 0.26f;
    }

    if (bw < 16) bw = width < 16 ? width : 16;
    if (bh < 16) bh = height < 16 ? height : 16;
    if (bw > GLASS_BODY_MAX_W) bw = GLASS_BODY_MAX_W;
    if (bh > GLASS_BODY_MAX_H) bh = GLASS_BODY_MAX_H;
    if (bw < 1 || bh < 1)
        return false;

    if (!glass->wallpaper_loaded)
        load_system_wallpaper(glass);

    if (!glass->body_tex || bw != glass->body_w || bh != glass->body_h) {
        uint8_t *buf = (uint8_t *)malloc((size_t)bw * (size_t)bh * 4);
        SDL_Texture *tex;

        if (!buf)
            return false;
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, bw, bh);
        if (!tex) {
            free(buf);
            return false;
        }
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        if (glass->body_tex)
            SDL_DestroyTexture(glass->body_tex);
        free(glass->body_rgba);
        glass->body_tex = tex;
        glass->body_rgba = buf;
        glass->body_w = bw;
        glass->body_h = bh;
    }

    for (y = 0; y < bh; y++) {
        float v_win = bw > 1 ? ((float)y + 0.5f) / (float)bh : 0.5f;
        float py = v_win * (float)height - half_h;
        float veil_r, veil_g, veil_b, veil_a;

        glass_vertical_veil(v_win, glass->optics.gradient_center, light_mode,
                            obscure, &veil_r, &veil_g, &veil_b, &veil_a);

        for (x = 0; x < bw; x++) {
            float u_win = ((float)x + 0.5f) / (float)bw;
            float px = u_win * (float)width - half_w;
            float d = glass_squircle_sdf(px, py, half_w, half_h, (float)radius);
            /* Clamp exterior texels in body_tex to the silhouette (`d = 0`) so
             * GPU bilinear filtering near the rounded corners never bleeds
             * interior darkness or transparent zeros across the seam. */
            float clamped_d = d > 0.0f ? 0.0f : d;
            float inside = -clamped_d;
            float nx, ny;
            float edge_q = glass_edge_factor(clamped_d, refr_band);
            float col_r, col_g, col_b, col_a;
            uint8_t *dst = glass->body_rgba + ((size_t)y * (size_t)bw + (size_t)x) * 4;

            glass_squircle_normal(px, py, half_w, half_h, (float)radius,
                                  &nx, &ny);

            /* 1. Translucent base tint + 3-stop vertical Liquid Glass veil. */
            {
                float a_base = base_tint * opacity;
                float a_veil = veil_a * opacity;
                float out_a = a_base + a_veil * (1.0f - a_base);

                if (out_a > 0.0001f) {
                    col_r = (base_r * a_base * (1.0f - a_veil) + veil_r * a_veil) / out_a;
                    col_g = (base_g * a_base * (1.0f - a_veil) + veil_g * a_veil) / out_a;
                    col_b = (base_b * a_base * (1.0f - a_veil) + veil_b * a_veil) / out_a;
                } else {
                    col_r = base_r;
                    col_g = base_g;
                    col_b = base_b;
                }
                col_a = out_a;
            }

            /* 2. Convex circular-cap rim refraction + RGB chromatic dispersion
             * from the pre-filtered desktop backdrop, plus adaptive hotspot
             * suppression in `obscure` mode so bright yellow/white wallpaper
             * patches are smoked down into rich, legible dark tones. */
            if (glass->wallpaper_rgb && glass->wallpaper_w > 0 &&
                glass->wallpaper_h > 0) {
                float bg_u = ((float)glass->win_screen_x + u_win * (float)width) / mon_w;
                float bg_v = ((float)glass->win_screen_y + v_win * (float)height) / mon_h;
                float lens_u, lens_v;
                float wp_r, wp_g, wp_b;
                float raw_r, raw_g, raw_b, raw_luma;
                float env_share;
                float wp_bright = obscure
                    ? (light_mode ? 0.92f : 0.28f)
                    : (light_mode ? 1.04f : glass->optics.brightness);

                sample_bilinear_rgb(glass->wallpaper_rgb, glass->wallpaper_w,
                                    glass->wallpaper_h, bg_u, bg_v,
                                    &raw_r, &raw_g, &raw_b);
                raw_luma = raw_r * 0.2126f + raw_g * 0.7152f + raw_b * 0.0722f;

                if (obscure && !light_mode) {
                    /* Adaptive hotspot damping: extra obsidian smoke directly
                     * over high-luminance wallpaper regions. */
                    float hotspot = smoothstepf(0.28f, 0.80f, raw_luma);
                    float damper = 0.44f * hotspot * opacity;
                    float next_a = col_a + damper * (1.0f - col_a);
                    if (next_a > 0.0001f) {
                        col_r = (col_r * col_a * (1.0f - damper) + 0.015f * damper) / next_a;
                        col_g = (col_g * col_a * (1.0f - damper) + 0.020f * damper) / next_a;
                        col_b = (col_b * col_a * (1.0f - damper) + 0.040f * damper) / next_a;
                    }
                    col_a = next_a;
                }

                glass_refract_uv(bg_u, bg_v, nx, ny, clamped_d, refr_band,
                                 lens_depth, glass->optics.magnify_strength,
                                 center_u, center_v, mon_w, mon_h,
                                 &lens_u, &lens_v);
                glass_sample_rgb_drift(
                    glass->wallpaper_rgb, glass->wallpaper_w, glass->wallpaper_h,
                    lens_u, lens_v, nx, ny,
                    drift_px / mon_w, drift_px / mon_h, edge_q,
                    &wp_r, &wp_g, &wp_b);
                glass_color_boost(&wp_r, &wp_g, &wp_b,
                                  glass->optics.saturation, wp_bright);

                env_share = obscure ? (0.08f * edge_q)
                                    : (0.02f + 0.24f * edge_q);
                env_share *= (0.35f + 0.65f * opacity);
                if (env_share > 0.0005f) {
                    float next_a = col_a + env_share * (1.0f - col_a);
                    if (next_a > 0.0001f) {
                        col_r = (col_r * col_a * (1.0f - env_share) + wp_r * env_share) / next_a;
                        col_g = (col_g * col_a * (1.0f - env_share) + wp_g * env_share) / next_a;
                        col_b = (col_b * col_a * (1.0f - env_share) + wp_b * env_share) / next_a;
                    }
                    col_a = next_a;
                }
            } else if (edge_q > 0.001f) {
                /* Subtle prismatic grazing-angle dispersion when no wallpaper
                 * buffer is present: warm outer prism -> cool cyan inner bevel. */
                float prism_scale = obscure ? 0.35f : 1.0f;
                float prism_r = (0.72f + 0.22f * nx) * prism_scale;
                float prism_g = (0.84f - 0.06f * ny) * prism_scale;
                float prism_b = (0.98f - 0.18f * nx) * prism_scale;
                float prism_a = (obscure ? 0.06f : 0.10f) * edge_q *
                                (0.4f + 0.6f * opacity);
                float next_a = col_a + prism_a * (1.0f - col_a);

                if (next_a > 0.0001f) {
                    col_r = (col_r * col_a * (1.0f - prism_a) + prism_r * prism_a) / next_a;
                    col_g = (col_g * col_a * (1.0f - prism_a) + prism_g * prism_a) / next_a;
                    col_b = (col_b * col_a * (1.0f - prism_a) + prism_b * prism_a) / next_a;
                }
                col_a = next_a;
            }

            /* 3. Inward exponential specular sheen (highlight.glsl):
             * lgRimAngularWeight(outNorm) * exp(-inside * 3 / band), tinted
             * cool specular ice-white (0.87, 0.93, 1.0). */
            {
                float band = sheen_band > 1.0f ? sheen_band : 1.0f;
                float angular = 0.65f + 0.55f * clampf(-ny * 0.5f + 0.5f, 0.0f, 1.0f);
                float rim = expf(-inside * (3.0f / band));
                float sheen_a = clampf(rim * sheen_str * angular, 0.0f, 0.85f) *
                                (0.45f + 0.55f * opacity);
                float next_a = col_a + sheen_a * (1.0f - col_a);

                if (next_a > 0.0001f) {
                    col_r = (col_r * col_a * (1.0f - sheen_a) + 0.87f * sheen_a) / next_a;
                    col_g = (col_g * col_a * (1.0f - sheen_a) + 0.93f * sheen_a) / next_a;
                    col_b = (col_b * col_a * (1.0f - sheen_a) + 1.00f * sheen_a) / next_a;
                }
                col_a = next_a;
            }

            dst[0] = (uint8_t)(clampf(col_r, 0.0f, 1.0f) * 255.0f + 0.5f);
            dst[1] = (uint8_t)(clampf(col_g, 0.0f, 1.0f) * 255.0f + 0.5f);
            dst[2] = (uint8_t)(clampf(col_b, 0.0f, 1.0f) * 255.0f + 0.5f);
            dst[3] = (uint8_t)(clampf(col_a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    if (!SDL_UpdateTexture(glass->body_tex, NULL, glass->body_rgba, bw * 4))
        return false;

    /* Build the 2r x 2r full-resolution 4-corner seam atlas so squircle
     * corners are 4x4-antialiased while matching body_tex seamlessly. */
    if (radius > 0) {
        int atlas_side = radius * 2;
        int q, cx, cy;

        if (!glass->corner_tex || radius != glass->corner_cap) {
            uint8_t *cbuf = (uint8_t *)malloc((size_t)atlas_side * (size_t)atlas_side * 4);
            SDL_Texture *ctex;

            if (!cbuf)
                return false;
            ctex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     atlas_side, atlas_side);
            if (!ctex) {
                free(cbuf);
                return false;
            }
            SDL_SetTextureBlendMode(ctex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(ctex, SDL_SCALEMODE_NEAREST);
            if (glass->corner_tex)
                SDL_DestroyTexture(glass->corner_tex);
            free(glass->corner_rgba);
            glass->corner_tex = ctex;
            glass->corner_rgba = cbuf;
            glass->corner_cap = radius;
        }

        for (q = 0; q < 4; q++) {
            int ox = (q & 1) ? radius : 0;
            int oy = (q & 2) ? radius : 0;
            int win_ox = (q & 1) ? (width - radius) : 0;
            int win_oy = (q & 2) ? (height - radius) : 0;

            for (cy = 0; cy < radius; cy++) {
                for (cx = 0; cx < radius; cx++) {
                    /* Map quadrant coordinates back to top-left corner frame
                     * for 4x4 supersampled squircle coverage. */
                    float mx = (q & 1) ? (float)(radius - 1 - cx) : (float)cx;
                    float my = (q & 2) ? (float)(radius - 1 - cy) : (float)cy;
                    Uint8 cov = sample_squircle_band(mx, my, (float)radius,
                                                     0.0f, (float)radius + 2.0f);
                    float u = ((float)(win_ox + cx) + 0.5f) / (float)width;
                    float v = ((float)(win_oy + cy) + 0.5f) / (float)height;
                    Uint8 samp[4];
                    uint8_t *dst = glass->corner_rgba +
                        ((size_t)(oy + cy) * (size_t)atlas_side + (size_t)(ox + cx)) * 4;

                    sample_body_bilinear(glass->body_rgba, bw, bh, u, v, samp);
                    dst[0] = samp[0];
                    dst[1] = samp[1];
                    dst[2] = samp[2];
                    dst[3] = (Uint8)(((unsigned)samp[3] * (unsigned)cov + 127u) / 255u);
                }
            }
        }
        if (!SDL_UpdateTexture(glass->corner_tex, NULL, glass->corner_rgba,
                               atlas_side * 4))
            return false;
    }

    glass->cache_valid = true;
    glass->cached_obscure = obscure;
    glass->cached_width = width;
    glass->cached_height = height;
    glass->cached_radius = radius;
    glass->cached_bg = base_bg;
    glass->cached_opacity_q = (int)(opacity * 1000.0f + 0.5f);
    glass->cached_win_x = glass->win_screen_x;
    glass->cached_win_y = glass->win_screen_y;
    glass->cached_wp_gen = glass->wallpaper_gen;
    return true;
}

/* ------------------------------------------------------------------------- */
/* GPU drawing helpers                                                       */
/* ------------------------------------------------------------------------- */

static SDL_FColor to_fcolor(SDL_Color c, float alpha_scale)
{
    SDL_FColor fc;
    fc.r = (float)c.r / 255.0f;
    fc.g = (float)c.g / 255.0f;
    fc.b = (float)c.b / 255.0f;
    fc.a = clampf(((float)c.a / 255.0f) * alpha_scale, 0.0f, 1.0f);
    return fc;
}

static void draw_quad_gradient(SDL_Renderer *renderer, SDL_Texture *white_tex,
                               float x0, float y0, float x1, float y1,
                               SDL_FColor c00, SDL_FColor c10,
                               SDL_FColor c01, SDL_FColor c11)
{
    SDL_Vertex verts[4];
    static const int indices[6] = { 0, 1, 2, 2, 1, 3 };

    if (x1 <= x0 || y1 <= y0 || !white_tex)
        return;
    verts[0].position.x = x0; verts[0].position.y = y0;
    verts[0].color = c00;     verts[0].tex_coord.x = 0.5f; verts[0].tex_coord.y = 0.5f;
    verts[1].position.x = x1; verts[1].position.y = y0;
    verts[1].color = c10;     verts[1].tex_coord.x = 0.5f; verts[1].tex_coord.y = 0.5f;
    verts[2].position.x = x0; verts[2].position.y = y1;
    verts[2].color = c01;     verts[2].tex_coord.x = 0.5f; verts[2].tex_coord.y = 0.5f;
    verts[3].position.x = x1; verts[3].position.y = y1;
    verts[3].color = c11;     verts[3].tex_coord.x = 0.5f; verts[3].tex_coord.y = 0.5f;

    SDL_RenderGeometry(renderer, white_tex, verts, 4, indices, 6);
}

/* Draw a straight rim segment subdivided into `segments` pieces so both the
 * diagonal opposing-light gradient and pointer-proximity specular glow vary
 * smoothly along long edges. */
static void draw_rim_run(SDL_Renderer *renderer, SDL_Texture *white_tex,
                         float x0, float y0, float x1, float y1,
                         bool horizontal, float surf_x, float surf_y,
                         float surf_w, float surf_h,
                         bool light_mode, bool obscure, bool ptr_valid,
                         float ptr_x, float ptr_y, float alpha_scale)
{
    const int segments = 12;
    int i;

    if (x1 <= x0 || y1 <= y0)
        return;
    for (i = 0; i < segments; i++) {
        float t0 = (float)i / (float)segments;
        float t1 = (float)(i + 1) / (float)segments;
        float sx0 = horizontal ? (x0 + (x1 - x0) * t0) : x0;
        float sx1 = horizontal ? (x0 + (x1 - x0) * t1) : x1;
        float sy0 = horizontal ? y0 : (y0 + (y1 - y0) * t0);
        float sy1 = horizontal ? y1 : (y0 + (y1 - y0) * t1);
        SDL_FColor c0 = to_fcolor(
            glass_rim_color(sx0 - surf_x, sy0 - surf_y, surf_w, surf_h,
                            light_mode, obscure, ptr_valid,
                            ptr_x - surf_x, ptr_y - surf_y),
            alpha_scale);
        SDL_FColor c1 = to_fcolor(
            glass_rim_color(sx1 - surf_x, sy1 - surf_y, surf_w, surf_h,
                            light_mode, obscure, ptr_valid,
                            ptr_x - surf_x, ptr_y - surf_y),
            alpha_scale);

        if (horizontal)
            draw_quad_gradient(renderer, white_tex, sx0, sy0, sx1, sy1,
                               c0, c1, c0, c1);
        else
            draw_quad_gradient(renderer, white_tex, sx0, sy0, sx1, sy1,
                               c0, c0, c1, c1);
    }
}

static void draw_corner_mask(SDL_Renderer *renderer, SDL_Texture *mask,
                             float x, float y, float size,
                             SDL_Color color, SDL_FlipMode flip)
{
    SDL_FRect dst = { x, y, size, size };

    if (!mask || size <= 0.0f)
        return;
    SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(mask, color.a);
    SDL_RenderTextureRotated(renderer, mask, NULL, &dst, 0.0, NULL, flip);
}

static void draw_body_subrect(SDL_Renderer *renderer, SDL_Texture *body_tex,
                              int bw, int bh, int win_w, int win_h,
                              float dx, float dy, float dw, float dh)
{
    SDL_FRect src, dst;

    if (dw <= 0.0f || dh <= 0.0f || win_w <= 0 || win_h <= 0)
        return;
    src.x = (dx / (float)win_w) * (float)bw;
    src.y = (dy / (float)win_h) * (float)bh;
    src.w = (dw / (float)win_w) * (float)bw;
    src.h = (dh / (float)win_h) * (float)bh;
    dst.x = dx;
    dst.y = dy;
    dst.w = dw;
    dst.h = dh;
    SDL_RenderTexture(renderer, body_tex, &src, &dst);
}

void glass_draw(Glass *glass, SDL_Renderer *renderer,
                int width, int height, int corner_radius,
                SDL_Color base_bg, float opacity, bool obscure,
                bool pointer_valid, float pointer_x, float pointer_y)
{
    int radius = corner_radius;
    int op_q;
    bool light_mode;
    float fw = (float)width;
    float fh = (float)height;
    float rim_scale;
    SDL_Color keyline_col;

    if (!glass || !renderer || width <= 0 || height <= 0)
        return;

    opacity = clampf(opacity, 0.0f, 1.0f);
    if (radius < 0)
        radius = 0;
    if (radius * 2 > width)
        radius = width / 2;
    if (radius * 2 > height)
        radius = height / 2;

    op_q = (int)(opacity * 1000.0f + 0.5f);
    if (!glass->cache_valid ||
        glass->cached_obscure != obscure ||
        glass->cached_width != width ||
        glass->cached_height != height ||
        glass->cached_radius != radius ||
        glass->cached_bg.r != base_bg.r ||
        glass->cached_bg.g != base_bg.g ||
        glass->cached_bg.b != base_bg.b ||
        glass->cached_opacity_q != op_q ||
        glass->cached_win_x != glass->win_screen_x ||
        glass->cached_win_y != glass->win_screen_y ||
        glass->cached_wp_gen != glass->wallpaper_gen) {
        if (!rebuild_optical_cache(glass, renderer, width, height, radius,
                                   base_bg, opacity, obscure))
            return;
    }

    light_mode = ((int)base_bg.r + (int)base_bg.g + (int)base_bg.b) > 384;
    /* Keep the glass rim visible even at low panel opacity, like a physical
     * lens silhouette. */
    rim_scale = 0.45f + 0.55f * opacity;
    keyline_col.r = 0;
    keyline_col.g = 0;
    keyline_col.b = 0;
    keyline_col.a = (Uint8)((obscure ? (light_mode ? 88.0f : 122.0f)
                                     : (light_mode ? 78.0f : 96.0f)) *
                            rim_scale + 0.5f);

    /* 1. Composite optical body + bevel refraction + vertical glass veil. */
    if (radius <= 0) {
        SDL_FRect full = { 0.0f, 0.0f, fw, fh };
        SDL_RenderTexture(renderer, glass->body_tex, NULL, &full);
    } else {
        float fr = (float)radius;
        SDL_FRect c_src, c_dst;

        draw_body_subrect(renderer, glass->body_tex, glass->body_w, glass->body_h,
                          width, height, fr, 0.0f, fw - 2.0f * fr, fr);
        draw_body_subrect(renderer, glass->body_tex, glass->body_w, glass->body_h,
                          width, height, 0.0f, fr, fw, fh - 2.0f * fr);
        draw_body_subrect(renderer, glass->body_tex, glass->body_w, glass->body_h,
                          width, height, fr, fh - fr, fw - 2.0f * fr, fr);

        c_src = (SDL_FRect){ 0.0f, 0.0f, fr, fr };
        c_dst = (SDL_FRect){ 0.0f, 0.0f, fr, fr };
        SDL_RenderTexture(renderer, glass->corner_tex, &c_src, &c_dst);

        c_src = (SDL_FRect){ fr, 0.0f, fr, fr };
        c_dst = (SDL_FRect){ fw - fr, 0.0f, fr, fr };
        SDL_RenderTexture(renderer, glass->corner_tex, &c_src, &c_dst);

        c_src = (SDL_FRect){ 0.0f, fr, fr, fr };
        c_dst = (SDL_FRect){ 0.0f, fh - fr, fr, fr };
        SDL_RenderTexture(renderer, glass->corner_tex, &c_src, &c_dst);

        c_src = (SDL_FRect){ fr, fr, fr, fr };
        c_dst = (SDL_FRect){ fw - fr, fh - fr, fr, fr };
        SDL_RenderTexture(renderer, glass->corner_tex, &c_src, &c_dst);
    }

    /* 2. Dual-contour silhouette (docs/glass-materials.md & GlassRim.qml):
     *    a) Outer neutral keyline so the glass silhouette stays sharp even
     *       above bright white windows.
     *    b) Inner opposing-light specular rim with fluid pointer glow. */
    {
        float fr = (float)radius;
        float kw = glass->keyline_width;
        float rw = glass->rim_width;
        float inset = kw * 0.5f;
        SDL_FColor kfc = to_fcolor(keyline_col, 1.0f);

        /* 2a. Outer keyline. */
        draw_quad_gradient(renderer, glass->white_tex, fr, 0.0f, fw - fr, kw,
                           kfc, kfc, kfc, kfc);
        draw_quad_gradient(renderer, glass->white_tex, fr, fh - kw, fw - fr, fh,
                           kfc, kfc, kfc, kfc);
        draw_quad_gradient(renderer, glass->white_tex, 0.0f, fr, kw, fh - fr,
                           kfc, kfc, kfc, kfc);
        draw_quad_gradient(renderer, glass->white_tex, fw - kw, fr, fw, fh - fr,
                           kfc, kfc, kfc, kfc);
        if (radius > 0) {
            draw_corner_mask(renderer, glass->keyline_corner, 0.0f, 0.0f, fr,
                             keyline_col, SDL_FLIP_NONE);
            draw_corner_mask(renderer, glass->keyline_corner, fw - fr, 0.0f, fr,
                             keyline_col, SDL_FLIP_HORIZONTAL);
            draw_corner_mask(renderer, glass->keyline_corner, 0.0f, fh - fr, fr,
                             keyline_col, SDL_FLIP_VERTICAL);
            draw_corner_mask(renderer, glass->keyline_corner, fw - fr, fh - fr, fr,
                             keyline_col, SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
        }

        /* 2b. Inner opposing-light specular contour + pointer catchlight. */
        draw_rim_run(renderer, glass->white_tex,
                     fr, inset, fw - fr, inset + rw, true,
                     0.0f, 0.0f, fw, fh, light_mode, obscure,
                     pointer_valid, pointer_x, pointer_y, rim_scale);
        draw_rim_run(renderer, glass->white_tex,
                     fr, fh - inset - rw, fw - fr, fh - inset, true,
                     0.0f, 0.0f, fw, fh, light_mode, obscure,
                     pointer_valid, pointer_x, pointer_y, rim_scale);
        draw_rim_run(renderer, glass->white_tex,
                     inset, fr, inset + rw, fh - fr, false,
                     0.0f, 0.0f, fw, fh, light_mode, obscure,
                     pointer_valid, pointer_x, pointer_y, rim_scale);
        draw_rim_run(renderer, glass->white_tex,
                     fw - inset - rw, fr, fw - inset, fh - fr, false,
                     0.0f, 0.0f, fw, fh, light_mode, obscure,
                     pointer_valid, pointer_x, pointer_y, rim_scale);

        if (radius > 0) {
            SDL_Color c_tl = glass_rim_color(
                fr * 0.3f, fr * 0.3f, fw, fh, light_mode, obscure,
                pointer_valid, pointer_x, pointer_y);
            SDL_Color c_tr = glass_rim_color(
                fw - fr * 0.3f, fr * 0.3f, fw, fh, light_mode, obscure,
                pointer_valid, pointer_x, pointer_y);
            SDL_Color c_bl = glass_rim_color(
                fr * 0.3f, fh - fr * 0.3f, fw, fh, light_mode, obscure,
                pointer_valid, pointer_x, pointer_y);
            SDL_Color c_br = glass_rim_color(
                fw - fr * 0.3f, fh - fr * 0.3f, fw, fh, light_mode, obscure,
                pointer_valid, pointer_x, pointer_y);

            c_tl.a = (Uint8)((float)c_tl.a * rim_scale + 0.5f);
            c_tr.a = (Uint8)((float)c_tr.a * rim_scale + 0.5f);
            c_bl.a = (Uint8)((float)c_bl.a * rim_scale + 0.5f);
            c_br.a = (Uint8)((float)c_br.a * rim_scale + 0.5f);

            draw_corner_mask(renderer, glass->rim_corner, 0.0f, 0.0f, fr,
                             c_tl, SDL_FLIP_NONE);
            draw_corner_mask(renderer, glass->rim_corner, fw - fr, 0.0f, fr,
                             c_tr, SDL_FLIP_HORIZONTAL);
            draw_corner_mask(renderer, glass->rim_corner, 0.0f, fh - fr, fr,
                             c_bl, SDL_FLIP_VERTICAL);
            draw_corner_mask(renderer, glass->rim_corner, fw - fr, fh - fr, fr,
                             c_br, SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Shared Liquid Glass capsule primitive                                     */
/* ------------------------------------------------------------------------- */

void glass_draw_capsule(Glass *glass, SDL_Renderer *renderer,
                        float x, float y, float width, float height,
                        float radius, SDL_Color fill, bool light_mode,
                        float rim_alpha_scale)
{
    float r = radius;
    float rw;
    SDL_FColor fc;

    if (!glass || !renderer || width <= 1.0f || height <= 1.0f)
        return;
    if (r < 0.0f)
        r = 0.0f;
    if (r * 2.0f > width)
        r = width * 0.5f;
    if (r * 2.0f > height)
        r = height * 0.5f;

    fc = to_fcolor(fill, 1.0f);
    rw = 1.0f * (glass->display_scale > 0.0f ? glass->display_scale : 1.0f);
    if (rw < 1.0f)
        rw = 1.0f;

    /* 1. Rounded squircle fill. */
    if (r < 1.0f) {
        draw_quad_gradient(renderer, glass->white_tex, x, y, x + width, y + height,
                           fc, fc, fc, fc);
    } else {
        draw_quad_gradient(renderer, glass->white_tex,
                           x + r, y, x + width - r, y + r, fc, fc, fc, fc);
        draw_quad_gradient(renderer, glass->white_tex,
                           x, y + r, x + width, y + height - r, fc, fc, fc, fc);
        draw_quad_gradient(renderer, glass->white_tex,
                           x + r, y + height - r, x + width - r, y + height,
                           fc, fc, fc, fc);
        draw_corner_mask(renderer, glass->pill_fill, x, y, r, fill, SDL_FLIP_NONE);
        draw_corner_mask(renderer, glass->pill_fill, x + width - r, y, r, fill,
                         SDL_FLIP_HORIZONTAL);
        draw_corner_mask(renderer, glass->pill_fill, x, y + height - r, r, fill,
                         SDL_FLIP_VERTICAL);
        draw_corner_mask(renderer, glass->pill_fill, x + width - r,
                         y + height - r, r, fill,
                         SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
    }

    /* 2. Opposing-light capsule rim. */
    if (rim_alpha_scale <= 0.001f)
        return;
    draw_rim_run(renderer, glass->white_tex,
                 x + r, y, x + width - r, y + rw, true,
                 x, y, width, height, light_mode, true, false, 0.0f, 0.0f,
                 rim_alpha_scale);
    draw_rim_run(renderer, glass->white_tex,
                 x + r, y + height - rw, x + width - r, y + height, true,
                 x, y, width, height, light_mode, true, false, 0.0f, 0.0f,
                 rim_alpha_scale);
    draw_rim_run(renderer, glass->white_tex,
                 x, y + r, x + rw, y + height - r, false,
                 x, y, width, height, light_mode, true, false, 0.0f, 0.0f,
                 rim_alpha_scale);
    draw_rim_run(renderer, glass->white_tex,
                 x + width - rw, y + r, x + width, y + height - r, false,
                 x, y, width, height, light_mode, true, false, 0.0f, 0.0f,
                 rim_alpha_scale);

    if (r >= 1.0f) {
        SDL_Color ctl = glass_rim_color(r * 0.3f, r * 0.3f, width, height,
                                        light_mode, true, false, 0.0f, 0.0f);
        SDL_Color ctr = glass_rim_color(width - r * 0.3f, r * 0.3f, width, height,
                                        light_mode, true, false, 0.0f, 0.0f);
        SDL_Color cbl = glass_rim_color(r * 0.3f, height - r * 0.3f, width, height,
                                        light_mode, true, false, 0.0f, 0.0f);
        SDL_Color cbr = glass_rim_color(width - r * 0.3f, height - r * 0.3f,
                                        width, height, light_mode, true, false,
                                        0.0f, 0.0f);

        ctl.a = (Uint8)(clampf((float)ctl.a * rim_alpha_scale, 0.0f, 255.0f) + 0.5f);
        ctr.a = (Uint8)(clampf((float)ctr.a * rim_alpha_scale, 0.0f, 255.0f) + 0.5f);
        cbl.a = (Uint8)(clampf((float)cbl.a * rim_alpha_scale, 0.0f, 255.0f) + 0.5f);
        cbr.a = (Uint8)(clampf((float)cbr.a * rim_alpha_scale, 0.0f, 255.0f) + 0.5f);

        draw_corner_mask(renderer, glass->pill_rim, x, y, r, ctl, SDL_FLIP_NONE);
        draw_corner_mask(renderer, glass->pill_rim, x + width - r, y, r, ctr,
                         SDL_FLIP_HORIZONTAL);
        draw_corner_mask(renderer, glass->pill_rim, x, y + height - r, r, cbl,
                         SDL_FLIP_VERTICAL);
        draw_corner_mask(renderer, glass->pill_rim, x + width - r,
                         y + height - r, r, cbr,
                         SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
    }
}
