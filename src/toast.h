#ifndef TERMICA_TOAST_H
#define TERMICA_TOAST_H

#include <stdbool.h>
#include "app.h"

/* All calls belong to the UI thread. */
void toast_show(const char *message);
bool toast_tick(void);
void toast_render(App *app);

#endif
