/*
 * termica - per-session terminal, ConPTY, selection and IME state.
 * See session.h.
 */
#include "session.h"

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Send bytes to the program: key encoding, pastes, and the terminal's own
 * replies (device status reports, mode queries) all end up here. A no-op
 * before the shell exists or after it is gone. */
static void on_term_write_pty(void *userdata, const void *data, size_t length)
{
    session_send((Session *)userdata, data, length);
}

bool session_init(Session *s, uint16_t columns, uint16_t rows,
                  TermClipboardWriteFn clipboard_fn, void *clipboard_userdata)
{
    memset(s, 0, sizeof *s);
    if (!term_init(&s->term, columns, rows))
        return false;
    /* Replies from the parser belong in the program's input, like every
     * other keystroke. */
    term_set_writer(&s->term, on_term_write_pty, s);
    term_set_clipboard_write(&s->term, clipboard_fn, clipboard_userdata);
    return true;
}

void session_free(Session *s)
{
    pty_close(s->pty);
    s->pty = NULL;
    search_free(&s->search);
    term_free(&s->term);
}

void session_send(Session *s, const void *data, size_t length)
{
    if (s->pty && length > 0)
        pty_write(s->pty, data, length);
}

void session_show_message(Session *s, const char *text)
{
    term_write(&s->term, text, strlen(text));
    search_output_changed(&s->search, &s->term);
}

void session_spawn_shell(Session *s, const Settings *settings, uint16_t columns,
                         uint16_t rows, PtyNotifyFn on_output,
                         void *output_userdata)
{
    wchar_t *command = NULL;
    PtyEnvEntry env_entries[SETTINGS_MAX_ENV];
    const char *cwd_opt = NULL;
    char error[256];
    int i;

    if (settings && settings->command_argc > 0) {
        const char *argv[SETTINGS_MAX_ARGS];

        for (i = 0; i < settings->command_argc; i++)
            argv[i] = settings->command[i];
        command = pty_build_command_line(argv, settings->command_argc);
        if (!command) {
            session_show_message(s,
                                 "\x1b[31minvalid [terminal] command in "
                                 "settings\x1b[0m\r\n");
        }
    } else {
        int shell_setting = settings ? (int)settings->shell
                                     : (int)SETTINGS_SHELL_AUTO;

        command = pty_shell_for(shell_setting);
        if (!command) {
            if (shell_setting == SETTINGS_SHELL_AUTO)
                session_show_message(s,
                                     "\x1b[31mno shell found: pwsh.exe, "
                                     "powershell.exe and cmd.exe are all "
                                     "missing\x1b[0m\r\n");
            else
                session_show_message(s,
                                     "\x1b[31mthe shell named in the settings "
                                     "is not installed\x1b[0m\r\n");
        }
    }

    if (settings && settings->cwd[0])
        cwd_opt = settings->cwd;

    if (command) {
        int env_count = settings ? settings->env_count : 0;

        for (i = 0; i < env_count; i++) {
            env_entries[i].key = settings->env[i].key;
            env_entries[i].value = settings->env[i].value;
        }
        s->pty = pty_spawn_ex(command, cwd_opt,
                              env_count > 0 ? env_entries : NULL, env_count,
                              columns, rows, on_output, output_userdata,
                              error, sizeof error);
        free(command);
        if (!s->pty) {
            char message[400];

            SDL_snprintf(message, sizeof message,
                         "\x1b[31mcould not start a shell: %s\x1b[0m\r\n",
                         error);
            session_show_message(s, message);
        }
    }

    /* Until the shell reports a directory over OSC 7, the directory module
     * shows the configured cwd or the directory termica started in. */
    if (cwd_opt) {
        snprintf(s->term.pwd, sizeof s->term.pwd, "%s", cwd_opt);
    } else {
        char *cwd = SDL_GetCurrentDirectory();

        if (cwd) {
            snprintf(s->term.pwd, sizeof s->term.pwd, "%s", cwd);
            SDL_free(cwd);
        }
    }
}

void session_resize(Session *s, uint16_t columns, uint16_t rows,
                    uint32_t cell_width, uint32_t cell_height)
{
    term_resize(&s->term, columns, rows, cell_width, cell_height);
    search_invalidate(&s->search);
    /* The console follows the grid, so full-screen programs see the new
     * size and reflow. */
    if (s->pty)
        pty_resize(s->pty, columns, rows);
}

void session_update_geometry(Session *s, uint32_t pixel_width,
                             uint32_t pixel_height, uint32_t grid_left,
                             uint32_t grid_top, uint32_t grid_right,
                             uint32_t grid_bottom)
{
    term_set_geometry(&s->term, pixel_width, pixel_height, grid_left, grid_top,
                      pixel_width > grid_right ? pixel_width - grid_right : 0,
                      pixel_height > grid_bottom ? pixel_height - grid_bottom
                                                 : 0);
    /* The selection is left alone: the terminal owns a tracked copy of it,
     * so it keeps naming the same text through a reflow. Only the pointer
     * state is reset, because the cell under the cursor changed. */
    s->select.dragging = false;
    s->bar_drag = false;
    s->mouse_held = false;
    term_selection_drag_end(&s->term);
    term_set_mouse_button(&s->term, false);
}

/* Move a bounded amount of pending output on the main thread. The reader
 * may refill the ring while it drains, so the ring size is not a work bound. */
bool session_pump(Session *s, bool *exited_now, bool *more)
{
    uint8_t buffer[16384];
    bool parsed = false;
    size_t bytes = 0;
    bool drained = false;

    if (exited_now)
        *exited_now = false;
    if (more)
        *more = false;
    if (!s->pty)
        return false;

    while (bytes < 65536) {
        size_t got = pty_read(s->pty, buffer, sizeof buffer);

        if (got == 0) {
            drained = true;
            break;
        }
        term_write(&s->term, buffer, got);
        bytes += got;
        parsed = true;
    }
    if (more && !drained)
        *more = pty_pending(s->pty) > 0;
    if (parsed)
        search_output_changed(&s->search, &s->term);

    if (drained && !s->shell_exited) {
        uint32_t code = 0;

        if (pty_exit_code(s->pty, &code)) {
            s->shell_exited = true;
            if (exited_now)
                *exited_now = true;
        }
    }

    return parsed;
}

/* Anything the user types is meant for the live view, so a scrolled-back
 * viewport snaps to the bottom first. The library ignores it when the
 * viewport already follows the active area. */
void session_jump_to_bottom(Session *s)
{
    if (!term_viewport_at_bottom(&s->term))
        term_scroll_bottom(&s->term);
}

void session_copy_selection(Session *s)
{
    char *text = term_selection_text(&s->term, NULL);

    if (!text)
        return;
    if (!SDL_SetClipboardText(text))
        fprintf(stderr, "clipboard: %s\n", SDL_GetError());
    free(text);
}

/* Rebuild the selection from the current gesture. The anchor is the tracked
 * reference from the press, the focus is the cell under the pointer; the
 * library owns the semantics (word boundaries, line extents). */
void session_selection_update(Session *s)
{
    SelectionDrag *drag = &s->select;

    switch (drag->mode) {
    case SELECT_WORDS:
        /* The nearest word between the anchor and the pointer, so a drag
         * that passes between words does not collapse to nothing. */
        if (!term_selection_drag_words(&s->term, drag->focus_x,
                                       drag->focus_y))
            term_selection_word(&s->term, drag->focus_x, drag->focus_y);
        break;
    case SELECT_LINES:
        if (!term_selection_drag_line(&s->term))
            term_selection_line(&s->term, drag->anchor_x, drag->anchor_y);
        break;
    default:
        if (!term_selection_drag_cells(&s->term, drag->focus_x,
                                       drag->focus_y, drag->rectangle))
            term_selection_cells(&s->term, drag->anchor_x, drag->anchor_y,
                                 drag->focus_x, drag->focus_y,
                                 drag->rectangle);
        break;
    }
}

/* Send text to the program (layout text, an IME commit). SDL reports Enter
 * as "\r" in SDL_EVENT_TEXT_INPUT as well as on the key event, and the key
 * encoder already sent it, so the carriage returns are dropped here instead
 * of typing twice. */
void session_send_text(Session *s, const char *text)
{
    const char *start;
    const char *cursor;

    if (!text)
        return;
    session_jump_to_bottom(s);
    start = text;
    cursor = text;
    while (*cursor) {
        if (*cursor == '\r') {
            if (cursor > start)
                session_send(s, start, (size_t)(cursor - start));
            start = cursor + 1;
        }
        cursor++;
    }
    if (cursor > start)
        session_send(s, start, (size_t)(cursor - start));
}

void session_set_composing(Session *s, const char *text)
{
    size_t length;

    if (!text || text[0] == '\0') {
        session_clear_composing(s);
        return;
    }

    length = strlen(text);
    if (length >= SESSION_COMPOSING_MAX)
        length = SESSION_COMPOSING_MAX - 1;
    memcpy(s->composing, text, length);
    s->composing[length] = '\0';
    s->composing_length = (int)length;
}

void session_clear_composing(Session *s)
{
    s->composing_length = 0;
    s->composing[0] = '\0';
}

/* Ask before pasting text that could run commands: line breaks outside
 * bracketed paste, or an embedded end-of-paste marker. */
static bool confirm_unsafe_paste(SDL_Window *window)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Paste" }
    };
    const SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_WARNING,
        window,
        "Paste anyway?",
        "The clipboard contains line breaks or an end-of-paste marker that a "
        "shell can run as commands.",
        SDL_arraysize(buttons),
        buttons,
        NULL
    };
    int pressed = 1;

    if (!SDL_ShowMessageBox(&box, &pressed))
        return false;
    return pressed == 0;
}

/* Paste the clipboard the way the program asked for it. The library frames
 * bracketed paste and rejects unsafe text; the rejection is confirmed with
 * the user and then retried. */
bool session_paste(Session *s, SDL_Window *window)
{
    char *clipboard = SDL_GetClipboardText();
    size_t length;
    bool rejected = false;

    if (!clipboard)
        return false;
    length = strlen(clipboard);
    session_jump_to_bottom(s);
    if (!term_paste(&s->term, clipboard, length, false, &rejected) &&
        rejected && confirm_unsafe_paste(window))
        term_paste(&s->term, clipboard, length, true, NULL);
    SDL_free(clipboard);
    return true;
}
