/*
 * termica - squircle geometry.
 *
 * macOS windows do not use circular corners: their corners have continuous
 * curvature. The usual approximation is a superellipse, a "squircle":
 *
 *     |u / r|^4 + |v / r|^4 <= 1
 *
 * where (u, v) is measured in pixels from the corner centre and r is the
 * corner radius. Everything here is plain math, no SDL and no Windows, so
 * the same curve is used for the drawn corner masks and for the window
 * region, and it is easy to test.
 *
 * Strict C99.
 */

#ifndef TERMICA_GEOMETRY_H
#define TERMICA_GEOMETRY_H

#include <stdbool.h>

/* macOS Big Sur and later use a 10 pt corner. Returns that in pixels for
 * the given display scale, never less than 4 pixels. */
int geometry_corner_radius(float display_scale);

/* Is (offset_x, offset_y), measured in pixels from the corner centre along
 * both axes, inside the squircle of the given radius? */
bool geometry_inside_squircle(float offset_x, float offset_y, float radius);

/* The same test for the outline: inside the squircle, but outside the
 * smaller one `thickness` pixels in. */
bool geometry_inside_squircle_ring(float offset_x, float offset_y,
                                   float radius, float thickness);

/* Sample one quarter of the squircle outline. Writes `steps + 1` points to
 * xs and ys (both must have room), walking from `from_angle` to `to_angle`
 * in radians with y pointing down. Returns the number of points written. */
int geometry_squircle_arc(float centre_x, float centre_y, float radius,
                          float from_angle, float to_angle, int steps,
                          float *xs, float *ys);

#endif /* TERMICA_GEOMETRY_H */
