/*
 * termica - shared application state and constants.
 *
 * App owns the window, renderer, fonts, settings, status bar and the active
 * Session. The event loop (main.c), input routing (input.c), frame rendering
 * (render.c) and window chrome (chrome.c) all operate on App.
 */
#ifndef TERMICA_APP_H
#define TERMICA_APP_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "git.h"
#include "glyph.h"
#include "panel.h"
#include "session.h"
#include "settings.h"
#include "statusbar.h"
#include "window.h"

/* Over a DWM backdrop the panel is only a light tint: the material is the
 * background, the tint just keeps the text readable. */
#define BACKDROP_TINT 0.24f

/* The name shown in the title bar, both custom-drawn and OS-managed. */
#define APP_TITLE "Termica"

/* What the built-in family is called, for the startup line. */
#define BUILT_IN_FONT "Maple Mono NF (built in)"

/* The status bar band: vertical padding around its text and the refresh
 * cadence of the spinner and clock. */
#define STATUSBAR_VPAD 4
#define STATUSBAR_TIMER_MS 100
#define GIT_REFRESH_MS 500
#define STATUSBAR_MAX_FRAMES 16
#define STATUSBAR_LINE_BYTES 4096
#define STATUSBAR_MAX_RUNS 64

/* SDL_EVENT_USER codes pushed by timers and the pty reader thread. */
enum {
    APP_EVENT_BLINK      = 1,
    APP_EVENT_PTY        = 2,
    APP_EVENT_AUTOSCROLL = 3,
    APP_EVENT_STATUSBAR  = 4
};

typedef struct App {
    SDL_Window      *window;
    SDL_Renderer    *renderer;
    Glyphs           glyphs;
    Panel            panel;
    Session          session;

    float            display_scale;
    char             font_path[512];  /* resolved grid font, or "" built-in */
    float            base_point_size; /* configured/CLI size for Ctrl+0     */
    float            current_point_size; /* live zoomed size                */
    float            panel_opacity;   /* 0..1                              */
    WindowBackdrop   backdrop;
    WindowDecoration decoration;
    bool             title_bar;       /* the custom title bar is drawn     */
    int              title_hover;     /* button under the pointer, or -1   */

    int              grid_left;       /* pixel position of cell 0,0        */
    int              grid_top;
    int              columns;
    int              rows;

    Settings         settings;
    Statusbar        statusbar;
    Glyphs           statusbar_glyphs;
    Git             *git;

    /* Cached git result for the current directory, and when we last asked
     * for a refresh. */
    GitStatus        git_status;
    bool             git_have;
    uint64_t         last_git_request_ms;

    /* Spinner frames from `[busy] symbols`, and which one is showing. */
    char             busy_frames[STATUSBAR_MAX_FRAMES][8];
    int              busy_frame_count;
    int              busy_frame_index;

    /* Last clock second the bar drew, so the clock only redraws on a
     * rollover. */
    int              last_time_second;
    bool             command_was_running;

    /* The pointer shape the program last asked for, and the cursor made
     * from it. NULL cursor means the system default. */
    GhosttyMouseShape shape;
    SDL_Cursor       *cursor;

    SDL_TimerID      autoscroll_timer;
    SDL_TimerID      blink_timer;
    SDL_TimerID      statusbar_timer;

    bool             running;
    bool             needs_redraw;
    bool             cursor_lit;      /* blink phase                       */
    bool             cursor_blinks;   /* the terminal asked for a blink    */
    uint64_t         bell_flash_until_ms; /* visual bell end tick, or 0    */
} App;

/* Live font zoom (PLAN 1.3): re-open grid glyphs at `point_size`, refit the
 * grid and resize the pty. Does not modify settings.point_size. */
void app_set_font_size(App *app, float point_size);

#endif /* TERMICA_APP_H */
