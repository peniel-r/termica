/*
 * termica - cell styling.
 *
 * libghostty-vt reports a cell as a GhosttyStyle plus, separately, the
 * colors it resolved from the palette. This turns that into the handful of
 * values the renderer needs: one foreground, an optional background, and
 * the decoration flags. Inverse video, faint text, hidden text and the
 * underline color are resolved here so the drawing loop stays a drawing
 * loop.
 *
 * Strict C99.
 */

#ifndef TERMICA_STYLE_H
#define TERMICA_STYLE_H

#include <SDL3/SDL.h>
#include <ghostty/vt.h>

#include <stdbool.h>

typedef struct CellStyle {
    SDL_Color foreground;
    SDL_Color background;
    bool      has_background;  /* the cell paints its own background   */
    bool      bold;
    bool      italic;
    bool      hidden;          /* invisible text                       */
    bool      underline;
    bool      overline;
    bool      strikethrough;
    SDL_Color decoration;      /* underline / strike / overline color  */
} CellStyle;

/* Convert one terminal cell style for drawing.
 *
 * `has_foreground` / `has_background` say whether the render state resolved
 * a color for the cell; when it did not, the defaults are used. Backgrounds
 * are drawn with `background_alpha` (the window's panel opacity) so text
 * stays opaque while the surface behind it can be translucent.
 *
 * `selected` highlights the cell the way inverse video does: the terminal
 * has no selection color of its own, and swapping the two colors the cell
 * already has works on any background. */
void style_resolve(CellStyle *out,
                   const GhosttyStyle *style,
                   bool has_foreground, GhosttyColorRgb foreground,
                   bool has_background, GhosttyColorRgb background,
                   const GhosttyRenderStateColors *state_colors,
                   SDL_Color default_foreground, SDL_Color default_background,
                   float background_alpha, bool selected);

/* A terminal color as an SDL color with the given alpha. */
SDL_Color style_color(GhosttyColorRgb color, float alpha);

#endif /* TERMICA_STYLE_H */
