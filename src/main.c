/*
 * termica - application entry point, setup, timers and event loop.
 *
 * The pieces live in their own files:
 *   app.h       shared App state and constants
 *   session.c   per-session terminal, ConPTY, selection and IME state
 *   input.c     keyboard, text/IME, pointer, focus and clipboard policy
 *   render.c    frame rendering from libghostty's render state
 *   chrome.c    window chrome geometry, title bar, scrollbar and status bar
 *   term.c      the libghostty-vt wrapper (screen, encoders, replies,
 *               scrolling, selection)
 *   pty.c       ConPTY: the child process, the pipes, the reader thread
 *   embed.c     the fonts compiled into the executable
 *   glyph.c     fonts, the glyph cache, drawing text
 *   style.c     terminal cell style -> colors and flags
 *   panel.c     the rounded translucent panel
 *   geometry.c  the squircle corner curve
 *   window.c    the SDL window and the Windows 11 chrome
 *
 * Usage:  termica [--selftest] [font path or family name] [point-size]
 *
 * Strict C99. Windows-only bits are behind #ifdef _WIN32.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "app.h"
#include "chrome.h"
#include "console.h"
#include "embed.h"
#include "font_lookup.h"
#include "input.h"
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Grid layout and pty/timer wake callbacks                                   */
/* ------------------------------------------------------------------------- */

/* Fit the grid to the window: below the drag strip, above the status line,
 * inside the side padding. Resizes the terminal when the grid changed. */
static void fit_grid(App *app)
{
    int pixel_width;
    int pixel_height;
    int padding;
    int columns;
    int rows;
    int grid_right;
    int grid_bottom;

    SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    padding = chrome_padding(app);
    app->grid_left = padding;
    app->grid_top = chrome_top(app);

    columns = (pixel_width - 2 * padding) / app->glyphs.cell_width;
    rows = (pixel_height - app->grid_top - padding -
            chrome_statusbar_height(app)) /
           app->glyphs.cell_height;
    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;

    if (columns != app->columns || rows != app->rows) {
        app->columns = columns;
        app->rows = rows;
        session_resize(&app->session, (uint16_t)columns, (uint16_t)rows,
                       (uint32_t)app->glyphs.cell_width,
                       (uint32_t)app->glyphs.cell_height);
    }
    /* The window region follows the window size. */
    window_sync_chrome(app->window, app->backdrop, app->decoration,
                       chrome_corner_radius(app));

    grid_right = app->grid_left + app->columns * app->glyphs.cell_width;
    grid_bottom = app->grid_top + app->rows * app->glyphs.cell_height;
    session_update_geometry(&app->session, (uint32_t)pixel_width,
                            (uint32_t)pixel_height, (uint32_t)app->grid_left,
                            (uint32_t)app->grid_top, (uint32_t)grid_right,
                            (uint32_t)grid_bottom);
    app->needs_redraw = true;
}

static void open_statusbar_font(App *app, const char *grid_font_path);

void app_set_font_size(App *app, float point_size)
{
    const char *path = app->font_path[0] ? app->font_path : NULL;
    Glyphs next;

    if (point_size < 4.0f)
        point_size = 4.0f;
    if (point_size > 200.0f)
        point_size = 200.0f;
    if (fabsf(point_size - app->current_point_size) < 0.01f)
        return;

    if (!glyphs_open(&next, app->renderer, path, point_size,
                     app->display_scale))
        return;
    glyphs_close(&app->glyphs);
    app->glyphs = next;
    app->current_point_size = point_size;
    fit_grid(app);
}

static void handle_display_scale_changed(App *app)
{
    float new_scale = SDL_GetWindowDisplayScale(app->window);
    const char *path = app->font_path[0] ? app->font_path : NULL;
    Glyphs next_glyphs;
    Panel next_panel;

    if (new_scale <= 0.0f || fabsf(new_scale - app->display_scale) < 0.001f)
        return;

    app->display_scale = new_scale;
    if (glyphs_open(&next_glyphs, app->renderer, path,
                    app->current_point_size, new_scale)) {
        glyphs_close(&app->glyphs);
        app->glyphs = next_glyphs;
    }
    if (panel_init(&next_panel, app->renderer, new_scale)) {
        panel_free(&app->panel);
        app->panel = next_panel;
    }
    glyphs_close(&app->statusbar_glyphs);
    open_statusbar_font(app, path);
    fit_grid(app);
}

/* The reader thread only announces that bytes are waiting. SDL_PushEvent is
 * thread-safe; a full queue means the main thread is already awake, so
 * retry briefly and let it drain. */
static void on_pty_output(void *userdata)
{
    SDL_Event event;
    int attempt;

    (void)userdata;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = APP_EVENT_PTY;
    for (attempt = 0; attempt < 100; attempt++) {
        if (SDL_PushEvent(&event))
            return;
        SDL_Delay(1);
    }
}

/* The cursor blink. A timer wakes the event loop twice a second so the
 * cursor can be drawn and not drawn. */
static Uint32 SDLCALL on_blink_timer(void *userdata, SDL_TimerID timer_id,
                                     Uint32 interval)
{
    SDL_Event event;

    (void)userdata;
    (void)timer_id;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = APP_EVENT_BLINK;
    SDL_PushEvent(&event);
    return interval;
}

/* The status bar timer: 100 ms wakes the event loop so the spinner can
 * advance and the clock can roll over. */
static Uint32 SDLCALL on_statusbar_timer(void *userdata, SDL_TimerID timer_id,
                                         Uint32 interval)
{
    SDL_Event event;

    (void)userdata;
    (void)timer_id;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = APP_EVENT_STATUSBAR;
    SDL_PushEvent(&event);
    return interval;
}

/* ------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* ------------------------------------------------------------------------- */

/* Is this argument a point size rather than a font path? */
static bool is_point_size(const char *text, float *out)
{
    char *end = NULL;
    double value = strtod(text, &end);

    if (!end || end == text || *end != '\0' || value <= 0.0 || value > 200.0)
        return false;
    if (out)
        *out = (float)value;
    return true;
}

/* The font is optional. Order: a path on the command line, then the one in
 * the settings file, then the built-in Maple Mono NF. A build without the
 * embedded font (anything but Windows) falls back to a monospace font from
 * the system. */
static const char *pick_font(const char *requested, const char *from_settings)
{
    static const char *fallbacks[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        NULL
    };
    int index;

    if (requested)
        return requested;
    if (from_settings && from_settings[0])
        return from_settings;
    if (embedded_font_available())
        return NULL;
    for (index = 0; fallbacks[index]; index++) {
        SDL_IOStream *probe = SDL_IOFromFile(fallbacks[index], "rb");
        if (probe) {
            SDL_CloseIO(probe);
            return fallbacks[index];
        }
    }
    return NULL;
}

/* The status bar font is separate: smaller (never below 9 pt) and, when
 * installed, Geist Mono NF. It falls back to the grid font and then the
 * built-in Maple Mono NF, so the bar always has Nerd glyphs. */
static void open_statusbar_font(App *app, const char *grid_font_path)
{
    const char *face = app->statusbar.font[0] ? app->statusbar.font : NULL;
    const char *sb_font = NULL;
    char resolved[512];

    if (face) {
        if (font_lookup_path(face, resolved, sizeof resolved))
            sb_font = resolved;
        else
            sb_font = face;
    }
    if (!sb_font)
        sb_font = grid_font_path;

    if (!glyphs_open(&app->statusbar_glyphs, app->renderer, sb_font,
                     app->statusbar.point_size, app->display_scale)) {
        /* A bad statusbar face must not kill the app: retry the grid font,
         * then give up quietly with an empty band. */
        if (sb_font != grid_font_path &&
            glyphs_open(&app->statusbar_glyphs, app->renderer, grid_font_path,
                        app->statusbar.point_size, app->display_scale)) {
            /* retried with the grid font */
        } else {
            fprintf(stderr, "statusbar font: %s\n", SDL_GetError());
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Event dispatch and per-tick state sync                                     */
/* ------------------------------------------------------------------------- */

static void dispatch_event(App *app, const SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_QUIT:
        app->running = false;
        break;
    case SDL_EVENT_USER:
        if (event->user.code == APP_EVENT_BLINK) {
            app->cursor_lit = !app->cursor_lit;
            if (app->cursor_blinks)
                app->needs_redraw = true;
        } else if (event->user.code == APP_EVENT_AUTOSCROLL) {
            input_autoscroll_tick(app);
        } else if (event->user.code == APP_EVENT_STATUSBAR) {
            chrome_statusbar_tick(app);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        input_handle_key_down(app, &event->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        input_handle_text_input(app, &event->text);
        break;
    case SDL_EVENT_TEXT_EDITING:
        input_handle_text_editing(app, &event->edit);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        input_handle_mouse_motion(app, &event->motion);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input_handle_mouse_button(app, &event->button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        input_handle_mouse_wheel(app, &event->wheel);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        input_reset_mouse_shape(app);
        app->title_hover = -1;
        app->needs_redraw = true;
        input_autoscroll_start(app, 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        input_handle_focus(app, true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_handle_focus(app, false);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        fit_grid(app);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        handle_display_scale_changed(app);
        break;
    default:
        break;
    }
}

static void sync_session_and_git(App *app)
{
    Session *s = &app->session;
    bool exited_now = false;
    GitStatus result;

    /* Drain the program's output: the pty event, but also the blink timer,
     * which is what notices an exit that produced no output. */
    if (session_pump(s, &exited_now))
        app->needs_redraw = true;

    /* The direct child is gone. Close the app with it: the window is only
     * hosting that shell, and leaving it up with a dead shell is not useful. */
    if (exited_now)
        app->running = false;

    /* Consume the program's OSC title change, but keep the title bar
     * showing the app name rather than the shell-provided title. */
    if (s->term.title_changed) {
        s->term.title_changed = false;
        SDL_SetWindowTitle(app->window, APP_TITLE);
    }

    /* A fresh directory or a finished command both invalidate the git view,
     * so ask the worker again (throttled). */
    if (s->term.pwd_changed) {
        s->term.pwd_changed = false;
        app->git_have = false;
        if (SDL_GetTicks() - app->last_git_request_ms >= GIT_REFRESH_MS) {
            git_request(app->git, term_pwd(&s->term));
            app->last_git_request_ms = SDL_GetTicks();
        }
    }
    if (app->command_was_running && !s->term.command_running &&
        SDL_GetTicks() - app->last_git_request_ms >= GIT_REFRESH_MS) {
        git_request(app->git, term_pwd(&s->term));
        app->last_git_request_ms = SDL_GetTicks();
    }
    app->command_was_running = s->term.command_running;

    if (git_poll(app->git, &result)) {
        app->git_status = result;
        app->git_have = true;
        app->needs_redraw = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    App app;
    char resolved_font[512];
    const char *font_path;
    const char *font_label;
    const char *requested_font = NULL;
    float point_size = 0.0f;   /* 0 means "whatever the settings say" */
    int exit_code = 1;
    int index;

    /* The executable is a GUI one, so a console only exists when something
     * launched us from one. Claim it before anything is printed. */
    console_reclaim();

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return term_selftest() == 0 ? 0 : 1;

    memset(&app, 0, sizeof app);
    app.running = true;
    app.cursor_lit = true;
    app.last_time_second = -1;
    settings_load(&app.settings);
    statusbar_load(&app.statusbar);

    /* Command-line arguments win over the file: the file is what the app
     * remembers, the arguments are what the user asked for this once. */
    for (index = 1; index < argc; index++) {
        float size;

        if (is_point_size(argv[index], &size))
            point_size = size;
        else if (!requested_font)
            requested_font = argv[index];
    }
    if (point_size <= 0.0f)
        point_size = app.settings.point_size;
    app.panel_opacity = app.settings.panel_opacity;
    app.backdrop = (WindowBackdrop)app.settings.backdrop;
    app.decoration = (WindowDecoration)app.settings.decoration;
    if (app.decoration >= WINDOW_DECORATION_COUNT)
        app.decoration = WINDOW_DECORATION_ROUNDED;
    app.title_bar = app.settings.title_bar &&
                    app.decoration != WINDOW_DECORATION_NATIVE;
    app.title_hover = -1;

    font_path = pick_font(requested_font, app.settings.font);
    /* A settings file may name the installed family instead of a file. */
    if (font_path &&
        font_lookup_path(font_path, resolved_font, sizeof resolved_font))
        font_path = resolved_font;
    if (!font_path && !embedded_font_available()) {
        fprintf(stderr, "usage: %s [--selftest] "
                "[font-face-or-path] [point-size]\n", argv[0]);
        return 1;
    }
    font_label = font_path ? font_path : BUILT_IN_FONT;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        goto cleanup;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        goto cleanup;
    }

    app.window = window_create(APP_TITLE, 900, 480, app.decoration,
                               app.title_bar);
    if (!app.window) {
        fprintf(stderr, "window_create: %s\n", SDL_GetError());
        goto cleanup;
    }
    SDL_StartTextInput(app.window);  /* deliver SDL_EVENT_TEXT_INPUT */

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        goto cleanup;
    }
    printf("renderer: %s\n", SDL_GetRendererName(app.renderer));

    app.display_scale = SDL_GetWindowDisplayScale(app.window);
    if (app.display_scale <= 0.0f)
        app.display_scale = 1.0f;

    app.base_point_size = point_size;
    app.current_point_size = point_size;
    if (font_path)
        snprintf(app.font_path, sizeof app.font_path, "%s", font_path);

    if (!glyphs_open(&app.glyphs, app.renderer, font_path, point_size,
                     app.display_scale)) {
        fprintf(stderr, "font: %s: %s\n", font_label, SDL_GetError());
        goto cleanup;
    }
    if (app.glyphs.built_in)
        font_label = BUILT_IN_FONT;
    printf("font: %s  cell: %dx%d px  scale: %.2f  bold: %s  italic: %s\n",
           font_label, app.glyphs.cell_width, app.glyphs.cell_height,
           (double)app.display_scale, app.glyphs.bold ? "yes" : "no",
           app.glyphs.italic ? "yes" : "no");
    fflush(stdout);

    if (!panel_init(&app.panel, app.renderer, app.display_scale)) {
        fprintf(stderr, "panel_init: %s\n", SDL_GetError());
        goto cleanup;
    }

    open_statusbar_font(&app, font_path);

    /* Session: create at a default size, then fit to the window. */
    if (!session_init(&app.session, 80, 24, input_handle_clipboard_write,
                      &app)) {
        fprintf(stderr, "libghostty-vt setup failed\n");
        goto cleanup;
    }

    /* Keep the material that is really in effect: Mica may be unavailable
     * on an older Windows, and the window falls back to the panel. */
    app.backdrop = window_sync_chrome(app.window, app.backdrop,
                                      app.decoration,
                                      chrome_corner_radius(&app));
    fit_grid(&app);

    /* The grid is final now, so the console starts at the right size. */
    session_spawn_shell(&app.session, &app.settings,
                        (uint16_t)app.columns, (uint16_t)app.rows,
                        on_pty_output, &app);

    /* The status bar's git status comes from a background worker. */
    git_init(&app.git);
    app.busy_frame_count =
        statusbar_busy_frames(app.busy_frames, STATUSBAR_MAX_FRAMES);
    git_request(app.git, term_pwd(&app.session.term));
    app.last_git_request_ms = SDL_GetTicks();

    app.blink_timer = SDL_AddTimer(500, on_blink_timer, &app);
    app.statusbar_timer = SDL_AddTimer(STATUSBAR_TIMER_MS, on_statusbar_timer,
                                       &app);

    while (app.running) {
        SDL_Event event;

        if (!SDL_WaitEvent(&event))
            break;
        do {
            dispatch_event(&app, &event);
        } while (app.running && SDL_PollEvent(&event));

        sync_session_and_git(&app);

        if (app.running && app.needs_redraw) {
            render_frame(&app);
            app.needs_redraw = false;
        }
    }
    exit_code = 0;

cleanup:
    /* Every step is safe on a partly initialised app, so a failure in the
     * middle of setup leaks nothing either. What the user changed while the
     * window was open (opacity, backdrop) is remembered for next time. */
    if (app.window) {
        settings_save(&app.settings);
        statusbar_save(&app.statusbar);
    }
    if (app.statusbar_timer)
        SDL_RemoveTimer(app.statusbar_timer);
    if (app.blink_timer)
        SDL_RemoveTimer(app.blink_timer);
    if (app.autoscroll_timer)
        SDL_RemoveTimer(app.autoscroll_timer);
    if (app.cursor)
        SDL_DestroyCursor(app.cursor);
    git_close(app.git);
    session_free(&app.session);
    panel_free(&app.panel);
    glyphs_close(&app.glyphs);
    glyphs_close(&app.statusbar_glyphs);
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();
    return exit_code;
}
