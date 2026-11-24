#ifndef TERMICA_UI_SETTINGS_H
#define TERMICA_UI_SETTINGS_H

#include "app.h"

/* Save a UI-edited window key without rewriting the rest of the file. */
void ui_settings_save(App *app, const char *key);

#endif
