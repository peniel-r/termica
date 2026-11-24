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
 * Usage:  termica [--version|--selftest] [font path or family name] [point-size]
 *
 * Strict C99. Windows-only bits are behind #ifdef _WIN32.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "app.h"
#include "chrome.h"
#include "clipboard.h"
#include "desktop_signal.h"
#include "diagnostics.h"
#include "config_watch.h"
#include "toast.h"
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
    int i;

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
        for (i = 0; i < app->session_count; i++) {
            session_resize(app->sessions[i], (uint16_t)columns, (uint16_t)rows,
                           (uint32_t)app->glyphs.cell_width,
                           (uint32_t)app->glyphs.cell_height);
        }
    }
    /* The window region and clickable tab region follow the window size. */
    window_sync_chrome(app->window, app->backdrop, app->decoration,
                       chrome_corner_radius(app));
    chrome_sync_tab_hit_region(app);

    grid_right = app->grid_left + app->columns * app->glyphs.cell_width;
    grid_bottom = app->grid_top + app->rows * app->glyphs.cell_height;
    for (i = 0; i < app->session_count; i++) {
        session_update_geometry(app->sessions[i], (uint32_t)pixel_width,
                                (uint32_t)pixel_height,
                                (uint32_t)app->grid_left,
                                (uint32_t)app->grid_top, (uint32_t)grid_right,
                                (uint32_t)grid_bottom);
    }
    app->needs_redraw = true;
}

static void open_statusbar_font(App *app, const char *grid_font_path);

static bool open_grid_font(App *app, Glyphs *out, float point_size,
                           float scale)
{
    const char *path = app->font_path[0] ? app->font_path : NULL;
    const char *fallbacks[SETTINGS_MAX_FALLBACKS];
    int i;

    for (i = 0; i < app->settings.font_fallback_count; i++)
        fallbacks[i] = app->settings.font_fallback[i];
    return glyphs_open_ex(out, app->renderer, path,
                          app->settings.font_fallback_count > 0 ? fallbacks
                                                                : NULL,
                          app->settings.font_fallback_count, point_size, scale);
}

void app_set_font_size(App *app, float point_size)
{
    Glyphs next;

    if (point_size < 4.0f)
        point_size = 4.0f;
    if (point_size > 200.0f)
        point_size = 200.0f;
    if (fabsf(point_size - app->current_point_size) < 0.01f)
        return;

    if (!open_grid_font(app, &next, point_size, app->display_scale))
        return;
    glyphs_close(&app->glyphs);
    app->glyphs = next;
    render_invalidate(app);
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
    if (open_grid_font(app, &next_glyphs, app->current_point_size, new_scale)) {
        glyphs_close(&app->glyphs);
        app->glyphs = next_glyphs;
        render_invalidate(app);
    }
    if (panel_init(&next_panel, app->renderer, new_scale)) {
        panel_free(&app->panel);
        app->panel = next_panel;
    }
    glyphs_close(&app->statusbar_glyphs);
    open_statusbar_font(app, path);
    fit_grid(app);
}

static void on_pty_output(void *userdata);

static void refresh_git_for_active(App *app)
{
    const Session *s = app_active_session_const(app);

    app->git_have = false;
    if (app->git && s) {
        git_request(app->git, term_pwd(&s->term));
        app->last_git_request_ms = SDL_GetTicks();
    }
}

bool app_new_tab(App *app)
{
    Session *s;
    uint16_t cols = (uint16_t)(app->columns > 0 ? app->columns : 80);
    uint16_t rows = (uint16_t)(app->rows > 0 ? app->rows : 24);

    if (app->session_count >= APP_MAX_TABS)
        return false;
    s = (Session *)calloc(1, sizeof *s);
    if (!s)
        return false;
    if (!session_init(s, cols, rows, input_handle_clipboard_write, app)) {
        free(s);
        return false;
    }
    term_set_clipboard_read(&s->term, clipboard_handle_read, app);
    term_set_scrollback_lines(&s->term, (size_t)app->settings.scrollback_lines);
    s->term.progress_changed = true;
    term_set_colors(&s->term, &app->settings.colors);
    if (app->glyphs.cell_width > 0 && app->glyphs.cell_height > 0) {
        term_resize(&s->term, cols, rows, (uint32_t)app->glyphs.cell_width,
                    (uint32_t)app->glyphs.cell_height);
    }

    input_autoscroll_start(app, 0);
    app->sessions[app->session_count] = s;
    app->active_session = app->session_count;
    app->session_count++;
    render_invalidate(app);

    /* Refit first if the tab strip just appeared, so the new shell spawns at
     * the exact post-strip grid dimensions. */
    fit_grid(app);
    session_spawn_shell(s, &app->settings, (uint16_t)app->columns,
                        (uint16_t)app->rows, on_pty_output, app);
    refresh_git_for_active(app);
    app->needs_redraw = true;
    return true;
}

void app_close_tab(App *app, int index)
{
    int i;

    if (index < 0 || index >= app->session_count)
        return;
    render_invalidate(app);
    input_autoscroll_start(app, 0);
    session_free(app->sessions[index]);
    free(app->sessions[index]);
    for (i = index; i + 1 < app->session_count; i++)
        app->sessions[i] = app->sessions[i + 1];
    app->session_count--;
    app->sessions[app->session_count] = NULL;
    app->tab_hover = -1;
    app->tab_new_hover = false;

    if (app->session_count == 0) {
        app->active_session = 0;
        app->running = false;
        return;
    }
    if (app->active_session >= app->session_count)
        app->active_session = app->session_count - 1;
    else if (index < app->active_session)
        app->active_session--;
    app->sessions[app->active_session]->term.progress_changed = true;

    fit_grid(app);
    refresh_git_for_active(app);
    app->needs_redraw = true;
}

void app_switch_tab(App *app, int index)
{
    if (app->session_count <= 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= app->session_count)
        index = app->session_count - 1;
    if (index == app->active_session)
        return;
    input_autoscroll_start(app, 0);
    app->active_session = index;
    render_invalidate(app);
    app->sessions[index]->term.progress_changed = true;
    app->command_was_running = app->sessions[index]->term.command_running;
    refresh_git_for_active(app);
    app->needs_redraw = true;
}

void app_next_tab(App *app, int delta)
{
    int next;

    if (app->session_count <= 1)
        return;
    next = (app->active_session + delta) % app->session_count;
    if (next < 0)
        next += app->session_count;
    app_switch_tab(app, next);
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

/* Apply a validated file snapshot on the UI thread. Font resources are
 * prepared before replacing the old ones, so a failed load keeps the live
 * configuration and grid intact. */
static void reload_files(App *app, unsigned changes, const char *font_override,
                         bool size_override)
{
    char error[256] = "Configuration reload failed";
    bool reload_bar_font = false;
    bool refit = false;

    if (changes & CONFIG_WATCH_SETTINGS) {
        Settings next = app->settings;
        Settings old = app->settings;
        unsigned diff = 0;
        if (!settings_reload(&next, &diff, error, sizeof error)) {
            toast_show(error);
        } else if (diff) {
            char old_path[sizeof app->font_path];
            float next_size = size_override ? app->base_point_size : next.point_size;
            Glyphs next_glyphs;
            bool font_ok = true;

            SDL_strlcpy(old_path, app->font_path, sizeof old_path);
            app->settings = next;
            if (diff & SETTINGS_DIFF_FONT) {
                if (!font_override) {
                    char resolved[512];
                    const char *path = pick_font(NULL, next.font);
                    if (path && font_lookup_path(path, resolved, sizeof resolved))
                        path = resolved;
                    SDL_strlcpy(app->font_path, path ? path : "", sizeof app->font_path);
                }
                font_ok = open_grid_font(app, &next_glyphs, next_size,
                                         app->display_scale);
            }
            if (!font_ok) {
                app->settings = old;
                SDL_strlcpy(app->font_path, old_path, sizeof app->font_path);
                toast_show("Configuration reload: unable to open font");
            } else {
                int i;
                if (diff & SETTINGS_DIFF_FONT) {
                    glyphs_close(&app->glyphs);
                    app->glyphs = next_glyphs;
                    render_invalidate(app);
                    app->base_point_size = next_size;
                    app->current_point_size = next_size;
                    reload_bar_font = true;
                    refit = true;
                }
                if (diff & SETTINGS_DIFF_COLORS) {
                    for (i = 0; i < app->session_count; i++)
                        term_set_colors(&app->sessions[i]->term, &next.colors);
                }
                if (diff & SETTINGS_DIFF_TERMINAL) {
                    for (i = 0; i < app->session_count; i++)
                        term_set_scrollback_lines(&app->sessions[i]->term,
                                                  (size_t)next.scrollback_lines);
                }
                if (diff & SETTINGS_DIFF_WINDOW) {
                    WindowDecoration decoration = (WindowDecoration)next.decoration;
                    bool title_bar = next.title_bar && decoration != WINDOW_DECORATION_NATIVE;
                    if ((decoration != app->decoration || title_bar != app->title_bar) &&
                        !window_set_decoration(app->window, decoration, title_bar)) {
                        toast_show("Configuration reload: unable to change window frame");
                    } else {
                        app->decoration = decoration;
                        app->title_bar = title_bar;
                    }
                    app->panel_opacity = next.panel_opacity;
                    app->backdrop = window_sync_chrome(app->window,
                        (WindowBackdrop)next.backdrop, app->decoration,
                        chrome_corner_radius(app));
                    refit = true;
                }
                app->needs_redraw = true;
            }
        }
    }
    if (changes & CONFIG_WATCH_STATUSBAR) {
        bool changed = false;
        if (!statusbar_reload(&app->statusbar, &changed, error, sizeof error))
            toast_show(error);
        else if (changed) {
            app->busy_frame_count = statusbar_busy_frames(app->busy_frames,
                                                           STATUSBAR_MAX_FRAMES);
            reload_bar_font = true;
            refit = true;
            app->needs_redraw = true;
        }
    }
    if (reload_bar_font) {
        glyphs_close(&app->statusbar_glyphs);
        open_statusbar_font(app, app->font_path[0] ? app->font_path : NULL);
    }
    if (refit)
        fit_grid(app);
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
        } else if (event->user.code == APP_EVENT_SEARCH) {
            Session *s = app_active_session(app);
            if (s && search_pending(&s->search))
                app->needs_redraw = true;
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
        app->tab_hover = -1;
        app->tab_new_hover = false;
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
    case SDL_EVENT_RENDER_TARGETS_RESET:
        render_invalidate(app);
        app->needs_redraw = true;
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        render_free(app);
        glyphs_reset(&app->glyphs);
        glyphs_reset(&app->statusbar_glyphs);
        panel_free(&app->panel);
        if (!panel_init(&app->panel, app->renderer, app->display_scale))
            fprintf(stderr, "panel reset: %s\n", SDL_GetError());
        app->needs_redraw = true;
        break;
    default:
        break;
    }
}

/* OSC notifications are untrusted requests from the shell. A policy prompt
 * runs only after the terminal's parser has returned to the main loop. */
static bool notification_allowed(App *app, const TermNotification *item)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
          SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Deny" },
        { 0, 0, "Allow" }
    };
    char detail[512];
    int pressed = 1;
    const SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_WARNING, app->window, "Allow notification?", detail,
        SDL_arraysize(buttons), buttons, NULL
    };

    if (app->settings.notifications == SETTINGS_OSC52_DENY)
        return false;
    if (app->settings.notifications == SETTINGS_OSC52_ALLOW)
        return true;
    snprintf(detail, sizeof detail, "A program requests a notification:\n%.400s",
             item->title[0] ? item->title : item->body);
    return SDL_ShowMessageBox(&box, &pressed) && pressed == 0;
}

static void sync_session_and_git(App *app)
{
    Session *active;
    GitStatus result;
    bool more_output = false;
    int i;

    /* Drain every session's output so background tabs never stall their pty
     * ring buffers, and close any tab whose shell has exited. */
    for (i = 0; i < app->session_count; ) {
        Session *s = app->sessions[i];
        bool exited_now = false;
        bool more = false;

        if (session_pump(s, &exited_now, &more) && i == app->active_session)
            app->needs_redraw = true;
        more_output = more_output || more;
        {
            TermNotification notification;
            while (term_take_notification(&s->term, &notification)) {
                bool allowed = notification_allowed(app, &notification);
                if (allowed && !desktop_signal_notify(true, notification.title,
                        strlen(notification.title), notification.body,
                        strlen(notification.body)))
                    toast_show("Windows could not display the notification");
            }
        }

        if (s->term.title_changed) {
            s->term.title_changed = false;
            if (chrome_tab_strip_visible(app))
                app->needs_redraw = true;
            if (i == app->active_session)
                SDL_SetWindowTitle(app->window, APP_TITLE);
        }

        if (exited_now) {
            app_close_tab(app, i);
            continue;
        }
        i++;
    }
    /* A budget-limited drain leaves the PTY wake coalesced until its ring
     * empties. Queue one further turn instead of waiting for a timer; input
     * and rendering still run between bounded batches. */
    if (more_output) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_USER;
        event.user.code = APP_EVENT_PTY;
        SDL_PushEvent(&event);
    }

    active = app_active_session(app);
    if (!active)
        return;
    if (active->term.progress_changed) {
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(app->window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        desktop_signal_progress(hwnd, (int)active->term.progress.state,
                                (int)active->term.progress.progress);
        active->term.progress_changed = false;
    }

    /* Handle BEL (0x07) according to [terminal] bell. */
    if (active->term.bell_rang) {
        active->term.bell_rang = false;
        if (app->settings.bell == SETTINGS_BELL_VISUAL) {
            app->bell_flash_until_ms = SDL_GetTicks() + 120;
            app->needs_redraw = true;
        } else if (app->settings.bell == SETTINGS_BELL_TASKBAR &&
                   (SDL_GetWindowFlags(app->window) &
                    SDL_WINDOW_INPUT_FOCUS) == 0) {
            window_flash_taskbar(app->window);
        }
    }

    /* A fresh directory or a finished command both invalidate the git view,
     * so ask the worker again (throttled). */
    if (active->term.pwd_changed) {
        active->term.pwd_changed = false;
        app->git_have = false;
        if (chrome_tab_strip_visible(app))
            app->needs_redraw = true;
        if (SDL_GetTicks() - app->last_git_request_ms >= GIT_REFRESH_MS) {
            git_request(app->git, term_pwd(&active->term));
            app->last_git_request_ms = SDL_GetTicks();
        }
    }
    if (app->command_was_running && !active->term.command_running &&
        SDL_GetTicks() - app->last_git_request_ms >= GIT_REFRESH_MS) {
        git_request(app->git, term_pwd(&active->term));
        app->last_git_request_ms = SDL_GetTicks();
    }
    app->command_was_running = active->term.command_running;

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
    ConfigWatch *watch = NULL;
    bool size_override = false;
    float point_size = 0.0f;   /* 0 means "whatever the settings say" */
    int exit_code = 1;
    int index;
    int diagnostic_test;
    const char *diagnostics_option;

    /* The executable is a GUI one, so a console only exists when something
     * launched us from one. Claim it before anything is printed. */
    console_reclaim();
    diagnostic_test = diagnostics_test_dispatch(argc, argv);
    if (diagnostic_test >= 0)
        return diagnostic_test;
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", diagnostics_version());
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return term_selftest() == 0 ? 0 : 1;

    diagnostics_option = getenv("TERMICA_DIAGNOSTICS");
    if (!diagnostics_init(!diagnostics_option || strcmp(diagnostics_option, "0") != 0))
        fprintf(stderr, "Could not initialize crash diagnostics\n");

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

        if (is_point_size(argv[index], &size)) {
            point_size = size;
            size_override = true;
        } else if (!requested_font) {
            requested_font = argv[index];
        }
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
    app.tab_hover = -1;

    font_path = pick_font(requested_font, app.settings.font);
    /* A settings file may name the installed family instead of a file. */
    if (font_path &&
        font_lookup_path(font_path, resolved_font, sizeof resolved_font))
        font_path = resolved_font;
    if (!font_path && !embedded_font_available()) {
        fprintf(stderr, "usage: %s [--selftest] "
                "[font-face-or-path] [point-size]\n", argv[0]);
        diagnostics_log("startup.font_missing", 0);
        goto cleanup;
    }
    font_label = font_path ? font_path : BUILT_IN_FONT;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        diagnostics_log("startup.sdl_failed", 0);
        goto cleanup;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        diagnostics_log("startup.ttf_failed", 0);
        goto cleanup;
    }
    desktop_signal_init();

    app.window = window_create(APP_TITLE, 900, 480, app.decoration,
                               app.title_bar);
    if (!app.window) {
        fprintf(stderr, "window_create: %s\n", SDL_GetError());
        diagnostics_log("startup.window_failed", 0);
        goto cleanup;
    }
    SDL_StartTextInput(app.window);  /* deliver SDL_EVENT_TEXT_INPUT */

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        diagnostics_log("startup.renderer_failed", 0);
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

    if (!open_grid_font(&app, &app.glyphs, point_size, app.display_scale)) {
        fprintf(stderr, "font: %s: %s\n", font_label, SDL_GetError());
        diagnostics_log("startup.font_failed", 0);
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
        diagnostics_log("startup.panel_failed", 0);
        goto cleanup;
    }

    open_statusbar_font(&app, font_path);

    /* Keep the material that is really in effect: Mica may be unavailable
     * on an older Windows, and the window falls back to the panel. */
    app.backdrop = window_sync_chrome(app.window, app.backdrop,
                                      app.decoration,
                                      chrome_corner_radius(&app));

    /* The status bar's git status comes from a background worker. */
    git_init(&app.git);
    app.busy_frame_count =
        statusbar_busy_frames(app.busy_frames, STATUSBAR_MAX_FRAMES);

    /* Open the initial tab (measures the grid, spawns the shell, requests
     * initial git status). */
    if (!app_new_tab(&app)) {
        fprintf(stderr, "libghostty-vt setup failed\n");
        goto cleanup;
    }
    watch = config_watch_start();
    if (!watch)
        toast_show("Configuration watcher could not start");

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
        {
            unsigned changes = config_watch_take(watch);
            if (changes)
                reload_files(&app, changes, requested_font, size_override);
        }
        if (toast_tick())
            app.needs_redraw = true;

        if (app.running && app.needs_redraw) {
            render_frame(&app);
            app.needs_redraw = false;
        }
    }
    exit_code = 0;

cleanup:
    /* UI edits are saved when made; closing never rewrites configuration. */
    config_watch_stop(watch);
    if (app.statusbar_timer)
        SDL_RemoveTimer(app.statusbar_timer);
    if (app.blink_timer)
        SDL_RemoveTimer(app.blink_timer);
    if (app.autoscroll_timer)
        SDL_RemoveTimer(app.autoscroll_timer);
    if (app.cursor)
        SDL_DestroyCursor(app.cursor);
    git_close(app.git);
    for (index = 0; index < app.session_count; index++) {
        session_free(app.sessions[index]);
        free(app.sessions[index]);
        app.sessions[index] = NULL;
    }
    app.session_count = 0;
    render_free(&app);
    panel_free(&app.panel);
    glyphs_close(&app.glyphs);
    glyphs_close(&app.statusbar_glyphs);
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();
    diagnostics_log("exit", (unsigned long)exit_code);
    diagnostics_shutdown();
    return exit_code;
}
