#include "ui_settings.h"
#include "config_watch.h"
#include "toast.h"

#include <stdio.h>
#include <string.h>

void ui_settings_save(App *app, const char *key)
{
    static const char *const backdrops[] = {
        "\"panel\"", "\"mica\"", "\"mica_alt\"", "\"acrylic\"",
        "\"glass\"", "\"glass_dark\""
    };
    char path[512];
    char value[32];

    if (!app || !key || !settings_path(path, sizeof path))
        return;
    if (strcmp(key, "opacity") == 0) {
        int hundredths = (int)(app->settings.panel_opacity * 100.0f + 0.5f);
        snprintf(value, sizeof value, "%d.%02d", hundredths / 100, hundredths % 100);
    } else if (strcmp(key, "backdrop") == 0 && app->settings.backdrop >= 0 &&
               app->settings.backdrop < (int)SDL_arraysize(backdrops)) {
        SDL_strlcpy(value, backdrops[app->settings.backdrop], sizeof value);
    } else {
        return;
    }
    if (!config_update_key(path, "window", key, value))
        toast_show("Could not save window setting; configuration was not changed");
}
