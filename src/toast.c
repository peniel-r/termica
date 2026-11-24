/* Brief in-window feedback for reload and configuration errors. */
#include "toast.h"
#include "chrome.h"

#include <string.h>

static char toast_message[256];
static Uint64 toast_until;
static bool toast_was_visible;

void toast_show(const char *message)
{
    SDL_strlcpy(toast_message, message ? message : "Configuration error",
                sizeof toast_message);
    toast_until = SDL_GetTicks() + 5000;
}

bool toast_tick(void)
{
    bool visible = toast_until && SDL_GetTicks() < toast_until;
    bool redraw = visible || toast_was_visible;
    toast_was_visible = visible;
    return redraw;
}

void toast_render(App *app)
{
    int width, height;
    int count = 0;
    const char *p = toast_message;
    const char *end = p + strlen(p);
    float x, y, box_width;
    SDL_Color bg = { 35, 35, 35, 255 };
    SDL_Color border = { 210, 80, 80, 255 };
    SDL_Color fg = { 255, 255, 255, 255 };

    if (!app || !toast_until || SDL_GetTicks() >= toast_until ||
        app->glyphs.cell_width <= 0 || app->glyphs.cell_height <= 0)
        return;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    while (p < end && count < (width - 24) / app->glyphs.cell_width) {
        if (!SDL_StepUTF8(&p, NULL))
            break;
        count++;
    }
    if (count <= 0)
        return;
    box_width = (float)(count * app->glyphs.cell_width + 16);
    x = (float)width - box_width - 8;
    y = (float)(height - chrome_statusbar_height(app) - app->glyphs.cell_height - 18);
    if (y < app->grid_top)
        y = (float)app->grid_top;
    chrome_fill_rect(app->renderer, x, y, box_width,
                     (float)app->glyphs.cell_height + 12, border);
    chrome_fill_rect(app->renderer, x + 1, y + 1, box_width - 2,
                     (float)app->glyphs.cell_height + 10, bg);
    p = toast_message;
    x += 8;
    for (int i = 0; i < count && p < end; i++) {
        const char *start = p;
        if (!SDL_StepUTF8(&p, NULL))
            break;
        glyphs_draw_text(&app->glyphs, start, (size_t)(p - start), x, y + 6,
                         false, false, fg);
        x += app->glyphs.cell_width;
    }
}
