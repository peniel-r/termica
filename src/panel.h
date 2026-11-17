/*
 * termica - the rounded panel.
 *
 * The window is drawn by the app, so the rounded corners are too. A panel is
 * three rectangles plus four pre-rendered corner masks for the fill, and the
 * same for the border, whose curve follows the same squircle. The masks are
 * supersampled, so the edge is antialiased without a geometry pipeline.
 *
 * Strict C99.
 */

#ifndef TERMICA_PANEL_H
#define TERMICA_PANEL_H

#include <SDL3/SDL.h>

#include <stdbool.h>

#define PANEL_BORDER_ALPHA 170

typedef struct Panel {
    int radius;             /* corner radius in pixels              */
    float border_width;     /* border thickness in pixels           */
    SDL_Texture *fill_corner;    /* one corner, white, alpha = shape  */
    SDL_Texture *border_corner;  /* the same corner as an outline     */
} Panel;

/* Build the corner masks for the given display scale. */
bool panel_init(Panel *panel, SDL_Renderer *renderer, float display_scale);

/* Free the masks. Safe on a zeroed struct. */
void panel_free(Panel *panel);

/* Draw the panel at the size of the render target. `corner_radius` is 0 for
 * a square frame; otherwise it is clamped to the available space. The two
 * colors carry their own alpha: the fill is the panel color, the border the
 * outline. */
void panel_draw(const Panel *panel, SDL_Renderer *renderer,
                int width, int height, int corner_radius,
                SDL_Color fill, SDL_Color border);

#endif /* TERMICA_PANEL_H */
