/*
 * termica - squircle geometry. See geometry.h.
 */

#include "geometry.h"

#include <SDL3/SDL_stdinc.h>

/* The macOS corner is 10 points; a point is 1/72 inch and SDL works in
 * pixels at 96 dpi before the display scale is applied. */
#define CORNER_RADIUS_POINTS 10.0f
#define POINTS_PER_INCH      72.0f
#define PIXELS_PER_INCH      96.0f

int geometry_corner_radius(float display_scale)
{
    int radius;

    if (display_scale <= 0.0f)
        display_scale = 1.0f;
    radius = (int)(CORNER_RADIUS_POINTS * (PIXELS_PER_INCH / POINTS_PER_INCH) *
                   display_scale + 0.5f);
    return radius < 4 ? 4 : radius;
}

bool geometry_inside_squircle(float offset_x, float offset_y, float radius)
{
    float x = offset_x / radius;
    float y = offset_y / radius;
    float x_squared = x * x;
    float y_squared = y * y;

    /* x^4 + y^4 <= 1, with the squares reused. */
    return x_squared * x_squared + y_squared * y_squared <= 1.0f;
}

bool geometry_inside_squircle_ring(float offset_x, float offset_y,
                                   float radius, float thickness)
{
    float inner_radius = radius - thickness;

    if (!geometry_inside_squircle(offset_x, offset_y, radius))
        return false;
    if (inner_radius <= 0.0f)
        return true;
    return !geometry_inside_squircle(offset_x, offset_y, inner_radius);
}

int geometry_squircle_arc(float centre_x, float centre_y, float radius,
                          float from_angle, float to_angle, int steps,
                          float *xs, float *ys)
{
    int point;

    if (steps < 1)
        steps = 1;
    for (point = 0; point <= steps; point++) {
        float angle = from_angle + (to_angle - from_angle) *
                      ((float)point / (float)steps);
        float cosine = SDL_cosf(angle);
        float sine = SDL_sinf(angle);
        /* Superellipse parametrisation for the exponent 4:
         * sign(c) * |c|^(2/4), which is a square root. */
        float x = SDL_sqrtf(SDL_fabsf(cosine));
        float y = SDL_sqrtf(SDL_fabsf(sine));

        if (cosine < 0.0f)
            x = -x;
        if (sine < 0.0f)
            y = -y;
        xs[point] = centre_x + radius * x;
        ys[point] = centre_y + radius * y;
    }
    return steps + 1;
}
