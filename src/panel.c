/*
 * termica - the rounded panel. See panel.h.
 */

#include "panel.h"

#include "geometry.h"

#include <string.h>

/* Supersampling per pixel and axis in the corner masks. 4x4 is enough to
 * hide the staircase at 1x and 2x. */
#define CORNER_SAMPLES 4

/* Portion of a pixel that the squircle covers. The mask is white with this
 * as its alpha, so drawing it tinted and alpha-modulated gives a smooth
 * edge. */
static Uint8 corner_coverage(float offset_x, float offset_y, float radius,
                             float border_width, bool outline)
{
    int hit = 0;
    int sample_x;
    int sample_y;

    for (sample_y = 0; sample_y < CORNER_SAMPLES; sample_y++) {
        for (sample_x = 0; sample_x < CORNER_SAMPLES; sample_x++) {
            float u = radius - (offset_x +
                                ((float)sample_x + 0.5f) / CORNER_SAMPLES);
            float v = radius - (offset_y +
                                ((float)sample_y + 0.5f) / CORNER_SAMPLES);

            if (outline) {
                if (geometry_inside_squircle_ring(u, v, radius, border_width))
                    hit++;
            } else if (geometry_inside_squircle(u, v, radius)) {
                hit++;
            }
        }
    }
    return (Uint8)(hit * 255 / (CORNER_SAMPLES * CORNER_SAMPLES));
}

/* One corner of the panel as a white texture with a shaped alpha channel.
 * Drawing it three more times with a flip covers the other three. */
static SDL_Texture *build_corner_mask(SDL_Renderer *renderer, int size,
                                      float border_width, bool outline)
{
    SDL_Surface *surface;
    SDL_Texture *texture;
    Uint8 *pixels;
    int x;
    int y;

    if (size <= 0)
        return NULL;
    surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        return NULL;
    pixels = (Uint8 *)surface->pixels;
    if (!pixels) {
        SDL_DestroySurface(surface);
        return NULL;
    }

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            Uint8 *pixel = pixels + (size_t)y * surface->pitch + (size_t)x * 4;
            pixel[0] = 255;
            pixel[1] = 255;
            pixel[2] = 255;
            pixel[3] = corner_coverage((float)x, (float)y, (float)size,
                                       border_width, outline);
        }
    }

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture)
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

bool panel_init(Panel *panel, SDL_Renderer *renderer, float display_scale)
{
    memset(panel, 0, sizeof *panel);
    if (display_scale <= 0.0f)
        display_scale = 1.0f;

    panel->radius = geometry_corner_radius(display_scale);
    panel->border_width = 1.5f * display_scale;
    if (panel->border_width < 1.0f)
        panel->border_width = 1.0f;

    panel->fill_corner = build_corner_mask(renderer, panel->radius,
                                           panel->border_width, false);
    panel->border_corner = build_corner_mask(renderer, panel->radius,
                                             panel->border_width, true);
    if (!panel->fill_corner || !panel->border_corner) {
        panel_free(panel);
        return false;
    }
    return true;
}

void panel_free(Panel *panel)
{
    if (panel->fill_corner)
        SDL_DestroyTexture(panel->fill_corner);
    if (panel->border_corner)
        SDL_DestroyTexture(panel->border_corner);
    memset(panel, 0, sizeof *panel);
}

static void fill_rectangle(SDL_Renderer *renderer, float x, float y,
                           float width, float height, SDL_Color color)
{
    SDL_FRect rectangle;

    if (width <= 0.0f || height <= 0.0f)
        return;
    rectangle.x = x;
    rectangle.y = y;
    rectangle.w = width;
    rectangle.h = height;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rectangle);
}

static void draw_corner(SDL_Renderer *renderer, SDL_Texture *mask,
                        int x, int y, int size, SDL_Color color,
                        SDL_FlipMode flip)
{
    SDL_FRect destination;

    destination.x = (float)x;
    destination.y = (float)y;
    destination.w = (float)size;
    destination.h = (float)size;
    SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(mask, color.a);
    SDL_RenderTextureRotated(renderer, mask, NULL, &destination, 0.0, NULL,
                             flip);
}

void panel_draw(const Panel *panel, SDL_Renderer *renderer,
                int width, int height, int corner_radius,
                SDL_Color fill, SDL_Color border)
{
    int radius = corner_radius;
    float panel_width = (float)width;
    float panel_height = (float)height;

    /* A square frame: one fill and four straight border runs. */
    if (radius <= 0) {
        fill_rectangle(renderer, 0.0f, 0.0f, panel_width, panel_height, fill);
        fill_rectangle(renderer, 0.0f, 0.0f, panel_width,
                       panel->border_width, border);
        fill_rectangle(renderer, 0.0f, panel_height - panel->border_width,
                       panel_width, panel->border_width, border);
        fill_rectangle(renderer, 0.0f, 0.0f, panel->border_width,
                       panel_height, border);
        fill_rectangle(renderer, panel_width - panel->border_width, 0.0f,
                       panel->border_width, panel_height, border);
        return;
    }

    /* A window smaller than two corners has no room for them. */
    if (radius * 2 > width)
        radius = width / 2;
    if (radius * 2 > height)
        radius = height / 2;
    if (radius < 1)
        return;

    /* Fill: the middle band, the two outer bands, then the four corners. */
    fill_rectangle(renderer, (float)radius, 0.0f,
                   panel_width - (float)(2 * radius), (float)radius, fill);
    fill_rectangle(renderer, 0.0f, (float)radius, panel_width,
                   panel_height - (float)(2 * radius), fill);
    fill_rectangle(renderer, (float)radius, panel_height - (float)radius,
                   panel_width - (float)(2 * radius), (float)radius, fill);
    draw_corner(renderer, panel->fill_corner, 0, 0, radius, fill,
                SDL_FLIP_NONE);
    draw_corner(renderer, panel->fill_corner, width - radius, 0, radius, fill,
                SDL_FLIP_HORIZONTAL);
    draw_corner(renderer, panel->fill_corner, 0, height - radius, radius, fill,
                SDL_FLIP_VERTICAL);
    draw_corner(renderer, panel->fill_corner, width - radius, height - radius,
                radius, fill, SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);

    /* Border: the four straight runs, then the four arcs. */
    fill_rectangle(renderer, (float)radius, 0.0f,
                   panel_width - (float)(2 * radius), panel->border_width,
                   border);
    fill_rectangle(renderer, (float)radius, panel_height - panel->border_width,
                   panel_width - (float)(2 * radius), panel->border_width,
                   border);
    fill_rectangle(renderer, 0.0f, (float)radius, panel->border_width,
                   panel_height - (float)(2 * radius), border);
    fill_rectangle(renderer, panel_width - panel->border_width,
                   (float)radius, panel->border_width,
                   panel_height - (float)(2 * radius), border);
    draw_corner(renderer, panel->border_corner, 0, 0, radius, border,
                SDL_FLIP_NONE);
    draw_corner(renderer, panel->border_corner, width - radius, 0, radius,
                border, SDL_FLIP_HORIZONTAL);
    draw_corner(renderer, panel->border_corner, 0, height - radius, radius,
                border, SDL_FLIP_VERTICAL);
    draw_corner(renderer, panel->border_corner, width - radius,
                height - radius, radius, border,
                SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
}
