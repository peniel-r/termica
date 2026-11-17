/*
 * termica - cell styling. See style.h.
 */

#include "style.h"

#include <string.h>

/* Faint text keeps 55% of its alpha, which is close to what other
 * terminals do and stays readable on a dark panel. */
#define FAINT_ALPHA 0.55f

SDL_Color style_color(GhosttyColorRgb color, float alpha)
{
    SDL_Color out;

    out.r = color.r;
    out.g = color.g;
    out.b = color.b;
    out.a = (Uint8)(alpha * 255.0f + 0.5f);
    return out;
}

/* Resolve a style color (a palette index or a direct RGB value). Returns
 * false when the style does not set one. */
static bool resolve_style_color(const GhosttyStyleColor *color,
                                const GhosttyRenderStateColors *state_colors,
                                GhosttyColorRgb *out)
{
    switch (color->tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE:
        *out = state_colors->palette[color->value.palette];
        return true;
    case GHOSTTY_STYLE_COLOR_RGB:
        *out = color->value.rgb;
        return true;
    default:
        return false;
    }
}

void style_resolve(CellStyle *out,
                   const GhosttyStyle *style,
                   bool has_foreground, GhosttyColorRgb foreground,
                   bool has_background, GhosttyColorRgb background,
                   const GhosttyRenderStateColors *state_colors,
                   SDL_Color default_foreground, SDL_Color default_background,
                   float background_alpha, bool selected)
{
    bool inverse;
    GhosttyColorRgb decoration_color;

    memset(out, 0, sizeof *out);

    out->foreground = has_foreground ? style_color(foreground, 1.0f)
                                     : default_foreground;
    out->background = has_background ? style_color(background, background_alpha)
                                     : default_background;

    inverse = (style->flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0 || selected;
    out->has_background = has_background || inverse;
    if (inverse) {
        SDL_Color swap = out->foreground;
        out->foreground = out->background;
        out->background = swap;
    }
    if (style->flags & GHOSTTY_STYLE_FLAG_FAINT)
        out->foreground.a = (Uint8)(out->foreground.a * FAINT_ALPHA);

    out->bold = (style->flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
    out->italic = (style->flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
    out->hidden = (style->flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0;
    out->underline_style = GHOSTTY_STYLE_UNDERLINE(*style);
    out->underline = out->underline_style != GHOSTTY_SGR_UNDERLINE_NONE;
    out->overline = (style->flags & GHOSTTY_STYLE_FLAG_OVERLINE) != 0;
    out->strikethrough = (style->flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0;

    out->decoration = out->foreground;
    if (resolve_style_color(&style->underline_color, state_colors,
                            &decoration_color))
        out->decoration = style_color(decoration_color, 1.0f);
}
