/*
 * termica - window chrome: layout geometry, title bar, scrollbar and status bar.
 */
#ifndef TERMICA_CHROME_H
#define TERMICA_CHROME_H

#include "app.h"

/* Shared chrome colours. The panel background also serves as the terminal's
 * default background, so inverse video and the cursor stay consistent. */
extern const SDL_Color CHROME_PANEL_BG;
extern const SDL_Color CHROME_PANEL_BORDER;
extern const SDL_Color CHROME_STATUS_TEXT;
extern const SDL_Color CHROME_TITLE_TEXT;

/* Small drawing helper shared by chrome and grid rendering. */
void chrome_fill_rect(SDL_Renderer *renderer, float x, float y, float width,
                      float height, SDL_Color color);

/* Frame preset geometry helpers. */
bool chrome_custom_frame(const App *app);
int chrome_corner_radius(const App *app);
int chrome_padding(const App *app);
int chrome_title_bar_height(const App *app);
int chrome_top(const App *app);
int chrome_statusbar_height(const App *app);
SDL_Color chrome_panel_fill_color(const App *app);

/* The scrollback bar. It lives in the right margin, outside the cell grid,
 * so it is app chrome even when a program has asked for the pointer. */
typedef struct ScrollBar {
    bool     visible;
    float    x;
    float    width;
    float    track_top;
    float    track_height;
    float    thumb_top;
    float    thumb_height;
    uint64_t first_row;    /* the row currently at the top of the viewport */
    uint64_t page_rows;    /* rows in the viewport                         */
    uint64_t scrollable;   /* total - page                                 */
} ScrollBar;

void chrome_scrollbar_geometry(App *app, ScrollBar *bar);
uint64_t chrome_scrollbar_row_at(const ScrollBar *bar, float y);
bool chrome_scrollbar_hit(const ScrollBar *bar, float x, float y,
                          float window_width);
bool chrome_scrollbar_press(App *app, const ScrollBar *bar, float y);
void chrome_draw_scrollbar(App *app);

/* Custom title bar hit testing, button actions and rendering. */
bool chrome_title_button_at(const App *app, float x, float y, int *index);
void chrome_title_button_action(App *app, int index);
void chrome_update_title_hover(App *app, float x, float y);
void chrome_draw_title_bar(App *app, int pixel_width, int pixel_height);

/* Status bar timer tick and rendering. */
void chrome_statusbar_tick(App *app);
void chrome_draw_statusbar(App *app, int pixel_width, int pixel_height,
                           SDL_Color default_fg);

#endif /* TERMICA_CHROME_H */
