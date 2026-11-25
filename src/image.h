/*
 * termica - image placements from libghostty's render state.
 *
 * The terminal library owns the Kitty graphics protocol: it parses the
 * sequences, decodes PNG and zlib payloads and stores the pixels. This file
 * only turns those pixels into SDL textures and draws the placements the
 * render state reports.
 *
 * Textures are cached by image id and image generation, so a frame with
 * unchanged images uploads nothing. A placement carries its own source and
 * destination rectangles; here they are clipped to the cell grid and scaled
 * through that source rectangle.
 *
 * Strict C99.
 */

#ifndef TERMICA_IMAGE_H
#define TERMICA_IMAGE_H

#include <SDL3/SDL.h>
#include <ghostty/vt.h>

#include <stdbool.h>

#include "term.h"

typedef struct ImageCache ImageCache;

/* Draw every placement in `layer`, clipped to the grid rectangle at
 * (left, top, width, height). Placements are sorted by z-index, then image
 * id, and drawn in that order. The cache is created on first use. Images are
 * optional, so a failure to allocate or draw one never fails the frame. */
void images_draw(ImageCache **cache, SDL_Renderer *renderer, Term *term,
                 GhosttyImageLayer layer, int left, int top, int width,
                 int height);

/* Drop every texture and the cache itself. Safe on NULL or a NULL cache. */
void images_free(ImageCache **cache);

#endif /* TERMICA_IMAGE_H */
