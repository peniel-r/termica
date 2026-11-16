/*
 * termica - the SDL window and its Windows chrome.
 *
 * The window is borderless, resizable and per-pixel transparent: the app
 * draws the panel, and transparent pixels show what is behind the window.
 * On Windows 11 the DWM can draw Mica, Mica Alt or Acrylic behind it
 * instead.
 *
 * A DWM backdrop fills the whole window rectangle, corners included, and a
 * drawn mask cannot cut into it, so with a backdrop the window is shaped
 * with a region. The region uses the same squircle as the drawn panel, so
 * the corner shape does not change when the backdrop does.
 *
 * Strict C99; the Windows parts are behind #ifdef _WIN32.
 */

#ifndef TERMICA_WINDOW_H
#define TERMICA_WINDOW_H

#include <SDL3/SDL.h>

#include <stdbool.h>

typedef enum {
    WINDOW_BACKDROP_PANEL = 0,   /* our own translucent panel       */
    WINDOW_BACKDROP_MICA,        /* Windows 11 Mica                 */
    WINDOW_BACKDROP_MICA_ALT,    /* Windows 11 Mica Alt             */
    WINDOW_BACKDROP_ACRYLIC,     /* Windows 11 Acrylic              */
    WINDOW_BACKDROP_COUNT
} WindowBackdrop;

/* Create the borderless, transparent, resizable window with the drag and
 * resize hit test installed. NULL on failure (SDL's error is set). */
SDL_Window *window_create(const char *title, int width, int height);

/* Apply the backdrop material and the matching rounded window region.
 * Call when the mode changes and after every resize, because the region
 * follows the window size. Returns the mode that is actually in effect: if
 * the OS does not know the material (before Windows 11 22H2) the window
 * stays on WINDOW_BACKDROP_PANEL. */
WindowBackdrop window_sync_chrome(SDL_Window *window, WindowBackdrop backdrop,
                                  int corner_radius);

/* Human readable name for the status line. */
const char *window_backdrop_name(WindowBackdrop backdrop);

#endif /* TERMICA_WINDOW_H */
