/*
 * termica - frame rendering from libghostty's render state.
 */
#ifndef TERMICA_RENDER_H
#define TERMICA_RENDER_H

#include "app.h"

/* Invalidate cached grid rows after replacing the grid font, changing tabs,
 * or SDL_EVENT_RENDER_TARGETS_RESET. Other grid changes are detected by the
 * renderer. Call on the UI thread, before the next frame. */
void render_invalidate(App *app);

/* Release cached render targets before destroying the SDL renderer. */
void render_free(App *app);

/* Draw a frame: panel, title bar, cached grid, IME preedit, scrollbar and
 * status bar, then present. Unsupported targets fall back to a full grid. */
void render_frame(App *app);

#endif /* TERMICA_RENDER_H */
