/*
 * termica - the SDL window and its Windows chrome.
 *
 * With the custom decorations the window is borderless, resizable and
 * per-pixel transparent: the app draws the panel, and transparent pixels
 * show what is behind the window. On Windows 11 the DWM can draw Mica, Mica
 * Alt or Acrylic behind it instead.
 *
 * A DWM backdrop fills the whole window rectangle, corners included, and a
 * drawn mask cannot cut into it, so with a backdrop the window is shaped
 * with a region. The region uses the same squircle as the drawn panel, so
 * the corner shape does not change when the backdrop does.
 *
 * The "native" decoration lets Windows draw the title bar and frame; the
 * custom title bar is only used by the other three.
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

typedef enum {
    WINDOW_DECORATION_ROUNDED = 0,  /* custom frame, rounded panel   */
    WINDOW_DECORATION_SQUARE,       /* custom frame, square panel    */
    WINDOW_DECORATION_COMPACT,      /* custom frame, less top inset  */
    WINDOW_DECORATION_NATIVE,       /* the Windows title bar         */
    WINDOW_DECORATION_COUNT
} WindowDecoration;

/* Logical (not pixel) sizes. The hit test runs in window coordinates, while
 * the drawing side scales these by the display scale. */
#define WINDOW_DRAG_STRIP 30
#define WINDOW_COMPACT_DRAG_STRIP 22
#define WINDOW_TITLE_BAR_HEIGHT 30
#define WINDOW_TAB_STRIP_HEIGHT 28
#define WINDOW_TITLE_BAR_BUTTON_WIDTH 46
#define WINDOW_TITLE_BAR_BUTTONS 3

/* Create the window. A native decoration keeps the system frame; the other
 * three are borderless with the drag and resize hit test installed.
 * `title_bar` draws a custom title bar and its three buttons, and is
 * ignored when the decoration is native. */
SDL_Window *window_create(const char *title, int width, int height,
                          WindowDecoration decoration, bool title_bar);

/* Apply a new frame and hit-test policy to an existing window. */
bool window_set_decoration(SDL_Window *window, WindowDecoration decoration,
                           bool title_bar);

/* Apply the backdrop material and the matching rounded window region.
 * Call when the mode changes and after every resize, because the region
 * follows the window size. Returns the mode that is actually in effect: if
 * the OS does not know the material (before Windows 11 22H2) the window
 * stays on WINDOW_BACKDROP_PANEL. */
WindowBackdrop window_sync_chrome(SDL_Window *window, WindowBackdrop backdrop,
                                  WindowDecoration decoration,
                                  int corner_radius);

/* Flash the window's taskbar button until it gains focus (Windows
 * FlashWindowEx with FLASHW_TRAY | FLASHW_TIMERNOFG; no-op elsewhere). */
void window_flash_taskbar(SDL_Window *window);

/* Tell the custom-frame hit test how wide (in logical window coordinates)
 * the clickable tab strip is on the left side of the title bar, so clicks
 * on tabs are delivered to the app instead of starting a window drag. */
void window_set_tabs_width(int logical_width);

#endif /* TERMICA_WINDOW_H */
