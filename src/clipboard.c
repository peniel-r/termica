/* Policy and system clipboard access for program-initiated reads. */
#include "clipboard.h"

#include "app.h"

#include <stdio.h>

TermClipboardResult clipboard_handle_read(void *userdata, const char *name,
                                           bool granted, char **text)
{
    App *app = (App *)userdata;

    if (!text || !app)
        return TERM_CLIPBOARD_DENIED;
    *text = NULL;
    if (!granted && app->settings.osc52 == SETTINGS_OSC52_DENY)
        return TERM_CLIPBOARD_DENIED;
    if (!granted && app->settings.osc52 == SETTINGS_OSC52_ASK) {
        static const SDL_MessageBoxButtonData buttons[] = {
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
              SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Deny" },
            { 0, 0, "Allow" }
        };
        char detail[512];
        int pressed = 1;
        const SDL_MessageBoxData box = {
            SDL_MESSAGEBOX_WARNING, app->window, "Allow clipboard read?",
            detail, SDL_arraysize(buttons), buttons, NULL
        };

        if (name && name[0])
            snprintf(detail, sizeof detail,
                     "\"%s\" wants to read the clipboard contents.", name);
        else
            snprintf(detail, sizeof detail,
                     "The program wants to read the clipboard contents.");
        if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 0)
            return TERM_CLIPBOARD_DENIED;
    }
    *text = SDL_GetClipboardText();
    return *text ? TERM_CLIPBOARD_ALLOWED : TERM_CLIPBOARD_UNSUPPORTED;
}
