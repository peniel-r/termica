/*
 * termica - frame rendering from libghostty's render state.
 */
#ifndef TERMICA_RENDER_H
#define TERMICA_RENDER_H

#include "app.h"

/* Draw one complete frame: panel, title bar, terminal grid, IME preedit,
 * scrollbar and status bar, then present. */
void render_frame(App *app);

#endif /* TERMICA_RENDER_H */
