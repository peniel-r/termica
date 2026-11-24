/*
 * termica - the terminal half of the app. See term.h.
 */

#include "term.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* The program rang the bell (BEL, 0x07). Callbacks must not mutate the
 * terminal or touch UI; record the flag for the main loop. */
static void on_bell(GhosttyTerminal terminal, void *userdata)
{
    Term *term = (Term *)userdata;

    (void)terminal;
    if (term)
        term->bell_rang = true;
}

/* The program asked for a new window title (OSC 0 or OSC 2). The title
 * itself is read back from the terminal; the callback only says it
 * changed. */
static void on_title_changed(GhosttyTerminal terminal, void *userdata)
{
    Term *term = (Term *)userdata;
    GhosttyString title = { 0 };
    size_t length;

    if (!term)
        return;
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE,
                             &title) != GHOSTTY_SUCCESS)
        return;

    length = title.len;
    if (length >= sizeof term->title)
        length = sizeof term->title - 1;
    if (length > 0)
        memcpy(term->title, title.ptr, length);
    term->title[length] = '\0';
    term->title_changed = true;
}

/* One hex digit, or -1 when the character is not one. */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode what the library stored as the pwd. OSC 7 gives a file:// URI
 * (typically file://C:/Users/... or file:///home/...), OSC 9 and OSC 1337
 * give a bare path that may still be percent-encoded. The result is a bare
 * path with percent escapes resolved, so the status bar and git runner can
 * use it without knowing which sequence produced it. */
static void term_decode_pwd(const GhosttyString *raw, char *out, size_t cap)
{
    const char *src = raw && raw->ptr ? (const char *)raw->ptr : "";
    size_t length = raw ? raw->len : 0;
    size_t in = 0;
    size_t used = 0;

    if (cap == 0)
        return;
    out[0] = '\0';
    if (length == 0)
        return;
    /* OSC 9;9 emitters on Windows commonly quote the whole native path.
     * Quotes cannot occur in a Windows path; leave URI-encoded quotes and
     * ordinary unquoted paths alone. */
    if (length >= 5 && src[0] == '"' && src[length - 1] == '"' &&
        (((src[1] >= 'A' && src[1] <= 'Z') ||
          (src[1] >= 'a' && src[1] <= 'z')) && src[2] == ':' &&
         (src[3] == '\\' || src[3] == '/'))) {
        in = 1;
        length--;
    } else if (length >= 5 && src[0] == '"' && src[length - 1] == '"' &&
               src[1] == '\\' && src[2] == '\\') {
        in = 1;
        length--;
    }

    /* Strip a file:// URI down to its path part. */
    if (length >= 7 && memcmp(src, "file://", 7) == 0) {
        in = 7;
        if (in < length && src[in] == '/') {
            /* file:///path: an empty authority. */
            in++;
        } else {
            /* An authority. file://C:/... is a Windows drive, not a host,
             * so keep it; anything else is a hostname to skip up to the
             * next slash. */
            if (!(in + 2 < length &&
                  ((src[in] >= 'A' && src[in] <= 'Z') ||
                   (src[in] >= 'a' && src[in] <= 'z')) &&
                  src[in + 1] == ':' && src[in + 2] == '/')) {
                while (in < length && src[in] != '/')
                    in++;
                if (in < length)
                    in++;   /* the slash after the host */
            }
        }
    }

    /* Percent-decode the rest. Invalid escapes are copied literally. */
    while (in < length && used + 1 < cap) {
        unsigned char c = (unsigned char)src[in];

        if (c == '%' && in + 2 < length) {
            int high = hex_nibble(src[in + 1]);
            int low = hex_nibble(src[in + 2]);

            if (high >= 0 && low >= 0) {
                c = (unsigned char)((high << 4) | low);
                in += 3;
            } else {
                in++;
            }
        } else {
            in++;
        }
        out[used++] = (char)c;
    }
    out[used] = '\0';

#ifdef _WIN32
    /* Windows OSC 7 arrives as a file:// URI with '/' separators. A Windows
     * path never legitimately contains '/', and the rest of the app (the bar
     * display and `git -C`) uses the native separator, so convert them. */
    for (size_t i = 0; i < used; i++) {
        if (out[i] == '/')
            out[i] = '\\';
    }
#endif
}

/* The shell reported a new working directory (OSC 7, OSC 9 or OSC 1337).
 * Read the value back and store it decoded. */
static void on_pwd_changed(GhosttyTerminal terminal, void *userdata)
{
    Term *term = (Term *)userdata;
    GhosttyString pwd = { 0 };

    if (!term)
        return;
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd)
            != GHOSTTY_SUCCESS)
        return;
    term_decode_pwd(&pwd, term->pwd, sizeof term->pwd);
    term->pwd_changed = true;
}

/* Shell integration (OSC 133) reports the steps of a command. The status
 * bar's spinner, duration and exit status all come from these edges. */
static void on_semantic_prompt(GhosttyTerminal terminal, void *userdata,
                               const GhosttyTerminalSemanticPrompt *event)
{
    Term *term = (Term *)userdata;

    (void)terminal;
    if (!term || !event)
        return;
    switch (event->kind) {
    case GHOSTTY_SEMANTIC_PROMPT_PROMPT_START:
        if (event->prompt_kind == GHOSTTY_SEMANTIC_PROMPT_PROMPT_PRIMARY) {
            term->prompt_input_active = false;
            term->command_start_pending = false;
        }
        break;
    case GHOSTTY_SEMANTIC_PROMPT_INPUT_START:
        term->prompt_input_active = true;
        break;
    case GHOSTTY_SEMANTIC_PROMPT_OUTPUT_START:
        if (!term->command_running)
            term->command_started_at = SDL_GetTicks();
        term->command_running = true;
        term->prompt_input_active = false;
        term->command_start_pending = false;
        break;
    case GHOSTTY_SEMANTIC_PROMPT_COMMAND_END:
        term->last_duration_known = term->command_running ||
                                    term->command_start_pending;
        if (term->last_duration_known)
            term->last_command_ms = SDL_GetTicks() - term->command_started_at;
        term->command_running = false;
        term->prompt_input_active = false;
        term->command_start_pending = false;
        if (event->has_exit_code) {
            term->last_exit_code = event->exit_code;
            term->last_command_known = true;
        } else {
            term->last_command_known = false;
        }
        break;
    default:
        break;
    }
}

/* A hard reset (RIS) clears the title and pwd without firing their changed
 * callbacks, so our cached copies must be cleared here too. */
static void on_reset(GhosttyTerminal terminal, void *userdata)
{
    Term *term = (Term *)userdata;

    (void)terminal;
    if (!term)
        return;
    term->pwd[0] = '\0';
    term->pwd_changed = true;
    term->command_running = false;
    term->last_command_known = false;
    term->last_duration_known = false;
    term->prompt_input_active = false;
    term->command_start_pending = false;
    term->last_exit_code = 0;
    term->last_command_ms = 0;
    term->notification_head = 0;
    term->notification_count = 0;
    term->progress.state = GHOSTTY_TERMINAL_PROGRESS_STATE_REMOVE;
    term->progress.progress = -1;
    term->progress_changed = true;
}

/* The library needs to answer device status reports, mode queries and
 * XTWINOPS; those bytes belong in the program's input, so hand them to the
 * writer the app installed. */
static void on_write_pty(GhosttyTerminal terminal, void *userdata,
                         const uint8_t *data, size_t length)
{
    Term *term = (Term *)userdata;

    (void)terminal;
    if (term && term->writer && length > 0)
        term->writer(term->writer_userdata, data, length);
}

void term_set_writer(Term *term, TermWriteFn writer, void *userdata)
{
    if (!term)
        return;
    term->writer = writer;
    term->writer_userdata = userdata;
}

/* A program asked for the clipboard (OSC 52, iTerm2 OSC 1337, kitty OSC
 * 5522). The library has already decoded the payload and normalized it to
 * MIME representations; the app decides what to do, and the answer has to
 * reach the library before this callback returns.
 *
 * The text is copied out of the borrowed representation so the app handler
 * gets a NUL-terminated string it can hand straight to the clipboard API.
 * Anything past the cap is dropped rather than reallocating inside a
 * callback that must not block. */
#define CLIPBOARD_TEXT_MAX 65536

static void on_clipboard_write(GhosttyTerminal terminal, void *userdata,
                               const GhosttyClipboardWrite *write)
{
    Term *term = (Term *)userdata;
    GhosttyClipboardWriteReply reply =
        GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply);
    static char text[CLIPBOARD_TEXT_MAX];
    char name[128];
    size_t index;
    size_t chosen = 0;
    size_t length = 0;
    TermClipboardResult result;

    (void)terminal;
    if (!term || !term->clipboard_write || !write)
        return;

    /* Prefer an explicit text/plain, then the first other text subtype,
     * then whatever came first. */
    for (index = 0; index < write->contents_len; index++) {
        const GhosttyClipboardContent *content = &write->contents[index];

        if (content->mime.len == strlen("text/plain") &&
            memcmp(content->mime.ptr, "text/plain", content->mime.len) == 0) {
            chosen = index;
            break;
        }
        if (content->mime.len > 5 &&
            memcmp(content->mime.ptr, "text/", 5) == 0)
            chosen = index;
    }
    if (write->contents_len == 0) {
        /* A request to clear the clipboard: no payload to hand over. */
        result = term->clipboard_write(term->clipboard_userdata, "", 0, "");
    } else {
        const GhosttyClipboardContent *content = &write->contents[chosen];

        length = content->data.len;
        if (length >= sizeof text)
            length = sizeof text - 1;
        if (length > 0)
            memcpy(text, content->data.ptr, length);
        text[length] = '\0';

        if (write->name.len > 0 && write->name.ptr) {
            size_t copy = write->name.len < sizeof name - 1
                              ? write->name.len
                              : sizeof name - 1;

            memcpy(name, write->name.ptr, copy);
            name[copy] = '\0';
        } else {
            name[0] = '\0';
        }
        result = term->clipboard_write(term->clipboard_userdata, text, length,
                                       name);
    }

    switch (result) {
    case TERM_CLIPBOARD_ALLOWED:
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
        break;
    case TERM_CLIPBOARD_UNSUPPORTED:
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;
        break;
    case TERM_CLIPBOARD_DENIED:
    default:
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
        break;
    }
    reply.remember = false;
    if (write->reply)
        write->reply(write, &reply);
}

void term_set_clipboard_write(Term *term, TermClipboardWriteFn write,
                              void *userdata)
{
    if (!term)
        return;
    term->clipboard_write = write;
    term->clipboard_userdata = userdata;
}

static void on_clipboard_read(GhosttyTerminal terminal, void *userdata,
                              const GhosttyClipboardRead *request)
{
    Term *term = (Term *)userdata;
    GhosttyClipboardReadReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardReadReply);
    GhosttyClipboardContent content = { 0 };
    static const char mime[] = "text/plain";
    char name[128];
    char *text = NULL;
    TermClipboardResult result = TERM_CLIPBOARD_DENIED;
    size_t n;

    (void)terminal;
    if (!request || !request->reply)
        return;
    n = request->name.ptr && request->name.len < sizeof name - 1
            ? request->name.len : (request->name.ptr ? sizeof name - 1 : 0);
    if (n)
        memcpy(name, request->name.ptr, n);
    name[n] = '\0';

    if (request->location != GHOSTTY_CLIPBOARD_LOCATION_STANDARD) {
        result = TERM_CLIPBOARD_UNSUPPORTED;
    } else if (term && term->clipboard_read) {
        result = term->clipboard_read(term->clipboard_read_userdata, name,
                                      request->granted, &text);
    }
    if (result == TERM_CLIPBOARD_ALLOWED && text) {
        content.mime.ptr = (const uint8_t *)mime;
        content.mime.len = sizeof mime - 1;
        content.data.ptr = (const uint8_t *)text;
        content.data.len = strlen(text);
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS;
        reply.contents = &content;
        reply.contents_len = 1;
        reply.available = &content.mime;
        reply.available_len = 1;
    } else if (result == TERM_CLIPBOARD_UNSUPPORTED) {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_UNSUPPORTED;
    } else {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_DENIED;
    }
    request->reply(request, &reply);
    SDL_free(text);
}

void term_set_clipboard_read(Term *term, TermClipboardReadFn read, void *userdata)
{
    if (!term)
        return;
    term->clipboard_read = read;
    term->clipboard_read_userdata = userdata;
}

bool term_set_scrollback_lines(Term *term, size_t lines)
{
    size_t unlimited = SIZE_MAX;

    if (!term || !term->handle)
        return false;
    /* A byte limit would otherwise silently truncate the requested lines. */
    return ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,
                                &unlimited) == GHOSTTY_SUCCESS &&
           ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,
                                &lines) == GHOSTTY_SUCCESS;
}

static void copy_ghostty_string(char *out, size_t capacity, GhosttyString in)
{
    size_t length = in.ptr ? in.len : 0;
    if (length >= capacity) {
        length = capacity - 1;
        /* Do not leave a partial UTF-8 sequence at the buffer boundary. */
        while (length > 0 && (in.ptr[length] & 0xc0) == 0x80)
            length--;
    }
    if (length)
        memcpy(out, in.ptr, length);
    out[length] = '\0';
}

static void on_desktop_notification(GhosttyTerminal terminal, void *userdata,
                                    const GhosttyTerminalDesktopNotification *event)
{
    Term *term = (Term *)userdata;
    TermNotification *item;
    unsigned index;

    (void)terminal;
    if (!term || !event || term->notification_count == TERM_NOTIFICATION_QUEUE)
        return;
    if (!term->notifications) {
        term->notifications = malloc(TERM_NOTIFICATION_QUEUE * sizeof *term->notifications);
        if (!term->notifications)
            return;
    }
    index = (term->notification_head + term->notification_count) % TERM_NOTIFICATION_QUEUE;
    item = &term->notifications[index];
    copy_ghostty_string(item->title, sizeof item->title, event->title);
    copy_ghostty_string(item->body, sizeof item->body, event->body);
    term->notification_count++;
}

bool term_take_notification(Term *term, TermNotification *out)
{
    if (!term || !out || term->notification_count == 0)
        return false;
    *out = term->notifications[term->notification_head];
    term->notification_head = (term->notification_head + 1) % TERM_NOTIFICATION_QUEUE;
    term->notification_count--;
    return true;
}

static void on_progress_report(GhosttyTerminal terminal, void *userdata,
                               const GhosttyTerminalProgressReport *event)
{
    Term *term = (Term *)userdata;
    (void)terminal;
    if (term && event) {
        term->progress = *event;
        term->progress_changed = true;
    }
}

bool term_init(Term *term, uint16_t columns, uint16_t rows)
{
    bool ok = false;

    memset(term, 0, sizeof *term);
    term->columns = columns;
    term->rows = rows;

    if (ghostty_terminal_new(NULL, &term->handle, columns, rows)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_new(NULL, &term->render_state) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_row_iterator_new(NULL, &term->row_iterator)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_render_state_row_cells_new(NULL, &term->cell_iterator)
            != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_key_encoder_new(NULL, &term->key_encoder) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_key_event_new(NULL, &term->key_event) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_mouse_encoder_new(NULL, &term->mouse_encoder) != GHOSTTY_SUCCESS)
        goto done;
    if (ghostty_mouse_event_new(NULL, &term->mouse_event) != GHOSTTY_SUCCESS)
        goto done;
    term->mouse_size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);

    /* The only effect we act on so far is the window title; replies go
     * through the writer, which the app points at the pty. */
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_USERDATA, term);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_BELL,
                         (const void *)on_bell);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         (const void *)on_title_changed);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
                         (const void *)on_pwd_changed);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,
                         (const void *)on_semantic_prompt);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_RESET,
                         (const void *)on_reset);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         (const void *)on_write_pty);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ,
                         (const void *)on_clipboard_read);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                         (const void *)on_desktop_notification);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_PROGRESS_REPORT,
                         (const void *)on_progress_report);
    /* Only useful once the app installs a handler; registering it always is
     * what lets the kitty protocol know this terminal can answer at all. */
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
                         (const void *)on_clipboard_write);
    ok = true;

done:
    if (!ok)
        term_free(term);
    return ok;
}

void term_free(Term *term)
{
    /* The render state borrows the terminal, and the iterators borrow the
     * render state, so free in that order. */
    term_selection_drag_end(term);
    ghostty_mouse_event_free(term->mouse_event);
    ghostty_mouse_encoder_free(term->mouse_encoder);
    ghostty_key_event_free(term->key_event);
    ghostty_key_encoder_free(term->key_encoder);
    ghostty_render_state_row_cells_free(term->cell_iterator);
    ghostty_render_state_row_iterator_free(term->row_iterator);
    ghostty_render_state_free(term->render_state);
    ghostty_terminal_free(term->handle);
    free(term->notifications);
    memset(term, 0, sizeof *term);
}

void term_set_colors(Term *term, const ColorScheme *scheme)
{
    GhosttyColorRgb palette[256];
    GhosttyColorRgb fg;
    GhosttyColorRgb bg;
    GhosttyColorRgb cursor;
    int i;

    if (!term || !term->handle || !scheme)
        return;

    fg.r = scheme->foreground.r;
    fg.g = scheme->foreground.g;
    fg.b = scheme->foreground.b;
    bg.r = scheme->background.r;
    bg.g = scheme->background.g;
    bg.b = scheme->background.b;
    cursor.r = scheme->cursor.r;
    cursor.g = scheme->cursor.g;
    cursor.b = scheme->cursor.b;

    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,
                         &fg);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,
                         &bg);
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR,
                         &cursor);

    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT,
                             palette) == GHOSTTY_SUCCESS) {
        for (i = 0; i < 16; i++) {
            palette[i].r = scheme->palette[i].r;
            palette[i].g = scheme->palette[i].g;
            palette[i].b = scheme->palette[i].b;
        }
        ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE,
                             &palette);
    }
}

void term_write(Term *term, const void *data, size_t length)
{
    if (!term->handle || length == 0)
        return;
    ghostty_terminal_vt_write(term->handle, (const uint8_t *)data, length);
}

const char *term_pwd(const Term *term)
{
    return term && term->pwd[0] ? term->pwd : NULL;
}

void term_resize(Term *term, uint16_t columns, uint16_t rows,
                 uint32_t cell_width, uint32_t cell_height)
{
    if (!term->handle)
        return;
    term->columns = columns;
    term->rows = rows;
    term->cell_width = cell_width;
    term->cell_height = cell_height;
    ghostty_terminal_resize(term->handle, columns, rows, cell_width,
                            cell_height);
}

size_t term_encode_key(Term *term, GhosttyKeyAction action, GhosttyKey key,
                       GhosttyMods modifiers, uint32_t unshifted_codepoint,
                       uint8_t *buffer, size_t capacity)
{
    size_t byte_count = 0;

    if (!term->key_encoder)
        return 0;

    /* The encoder follows the modes the program set (cursor keys, keypad,
     * kitty keyboard protocol, ...), so refresh it before every key. */
    ghostty_key_encoder_set_from_terminal(term->key_encoder, term->handle);

    ghostty_key_event_set_action(term->key_event, action);
    ghostty_key_event_set_key(term->key_event, key);
    ghostty_key_event_set_mods(term->key_event, modifiers);
    /* Plain text arrives as SDL_EVENT_TEXT_INPUT, not on the key event, so
     * there is no UTF-8 here. The unshifted codepoint (layout dependent,
     * e.g. 'a' for the A key) is what makes Ctrl+letter encode as
     * 0x01-0x1a. */
    ghostty_key_event_set_utf8(term->key_event, (GhosttyString){ 0 });
    ghostty_key_event_set_unshifted_codepoint(term->key_event,
                                              unshifted_codepoint);

    if (ghostty_key_encoder_encode(term->key_encoder, term->key_event,
                                   buffer, capacity, &byte_count)
            != GHOSTTY_SUCCESS)
        return 0;
    return byte_count;
}

/* The clipboard text handed to ghostty_terminal_paste is streamed through
 * this reader; the library asks for it once. */
typedef struct PasteText {
    const char *data;
    size_t      length;
} PasteText;

static bool paste_write(void *userdata, GhosttyString mime,
                        GhosttyWriter writer)
{
    const PasteText *text = (const PasteText *)userdata;

    (void)mime;
    if (text->length == 0)
        return true;
    return writer.write(writer.userdata, (const uint8_t *)text->data,
                        text->length);
}

bool term_paste(Term *term, const char *text, size_t length,
                bool allow_unsafe, bool *rejected)
{
    GhosttyPaste  paste = GHOSTTY_INIT_SIZED(GhosttyPaste);
    GhosttyString mime;
    PasteText     source;
    GhosttyResult result;
    bool          pasted = false;

    if (rejected)
        *rejected = false;
    if (!term->handle || !term->writer || !text || length == 0)
        return false;

    mime.ptr = (const uint8_t *)"text/plain";
    mime.len = 10;
    source.data = text;
    source.length = length;

    paste.location = GHOSTTY_CLIPBOARD_LOCATION_STANDARD;
    paste.source = GHOSTTY_PASTE_SOURCE_CLIPBOARD;
    paste.mimes = &mime;
    paste.mimes_len = 1;
    paste.reader.read = paste_write;
    paste.reader.userdata = &source;
    paste.allow_unsafe = allow_unsafe;

    result = ghostty_terminal_paste(term->handle, &paste, &pasted);
    if (result == GHOSTTY_REJECTED) {
        if (rejected)
            *rejected = true;
        return false;
    }
    return result == GHOSTTY_SUCCESS && pasted;
}

/* ------------------------------------------------------------------------- */
/* Mouse reporting                                                            */
/* ------------------------------------------------------------------------- */

/* The encoder turns surface-space pixels into the cell coordinates a
 * program expects, so it has to know where the grid is. Cell size is the
 * one term_resize already keeps. */
void term_set_geometry(Term *term, uint32_t screen_width,
                       uint32_t screen_height, uint32_t padding_left,
                       uint32_t padding_top, uint32_t padding_right,
                       uint32_t padding_bottom)
{
    if (!term->mouse_encoder)
        return;

    term->mouse_size.screen_width = screen_width;
    term->mouse_size.screen_height = screen_height;
    term->mouse_size.cell_width = term->cell_width > 0 ? term->cell_width : 1;
    term->mouse_size.cell_height =
        term->cell_height > 0 ? term->cell_height : 1;
    term->mouse_size.padding_left = padding_left;
    term->mouse_size.padding_top = padding_top;
    term->mouse_size.padding_right = padding_right;
    term->mouse_size.padding_bottom = padding_bottom;

    ghostty_mouse_encoder_set(term->mouse_encoder,
                              GHOSTTY_MOUSE_ENCODER_OPT_SIZE,
                              &term->mouse_size);
}

/* Whether a button is held changes what a motion event means (a drag is
 * reported, a hover only under any-event tracking), so the encoder has to
 * be told. Letting go also clears the last-cell deduplication. */
void term_set_mouse_button(Term *term, bool pressed)
{
    if (!term->mouse_encoder)
        return;
    ghostty_mouse_encoder_set(term->mouse_encoder,
                              GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                              &pressed);
    if (!pressed)
        ghostty_mouse_encoder_reset(term->mouse_encoder);
}

size_t term_encode_mouse(Term *term, GhosttyMouseAction action,
                         GhosttyMouseButton button, GhosttyMods modifiers,
                         float x, float y, uint8_t *buffer, size_t capacity)
{
    GhosttyMousePosition position;
    size_t byte_count = 0;

    if (!term->mouse_encoder || !term->mouse_event)
        return 0;

    /* Tracking mode and output format belong to the program: set them from
     * the terminal before every event, the same way the key encoder is
     * refreshed. */
    ghostty_mouse_encoder_set_from_terminal(term->mouse_encoder,
                                            term->handle);

    if (ghostty_mouse_event_set_action(term->mouse_event, action)
            != GHOSTTY_SUCCESS)
        return 0;
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
        ghostty_mouse_event_clear_button(term->mouse_event);
    else if (ghostty_mouse_event_set_button(term->mouse_event, button)
                 != GHOSTTY_SUCCESS)
        return 0;
    ghostty_mouse_event_set_mods(term->mouse_event, modifiers);

    position.x = x;
    position.y = y;
    ghostty_mouse_event_set_position(term->mouse_event, position);

    if (ghostty_mouse_encoder_encode(term->mouse_encoder, term->mouse_event,
                                     buffer, capacity, &byte_count)
            != GHOSTTY_SUCCESS)
        return 0;
    return byte_count;
}

bool term_mouse_tracking(Term *term)
{
    bool tracking = false;

    if (!term->handle)
        return false;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                             &tracking) != GHOSTTY_SUCCESS)
        return false;
    return tracking;
}

bool term_mouse_shape(Term *term, GhosttyMouseShape *out_shape)
{
    GhosttyMouseShape shape = GHOSTTY_MOUSE_SHAPE_DEFAULT;

    if (!term->handle || !out_shape)
        return false;
    if (ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE,
                             &shape) != GHOSTTY_SUCCESS)
        return false;
    *out_shape = shape;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Focus reporting                                                            */
/* ------------------------------------------------------------------------- */

bool term_focus_reporting(Term *term)
{
    GhosttyTerminalModeConfig config;

    if (!term->handle)
        return false;
    config.mode = GHOSTTY_MODE_FOCUS_EVENT;
    config.value = false;
    if (ghostty_terminal_get(term->handle, GHOSTTY_TERMINAL_DATA_MODE,
                             &config) != GHOSTTY_SUCCESS)
        return false;
    return config.value;
}

size_t term_encode_focus(Term *term, bool gained, uint8_t *buffer,
                         size_t capacity)
{
    size_t written = 0;

    (void)term;
    if (ghostty_focus_encode(gained ? GHOSTTY_FOCUS_GAINED
                                    : GHOSTTY_FOCUS_LOST,
                             buffer, capacity, &written) != GHOSTTY_SUCCESS)
        return 0;
    return written;
}

/* ------------------------------------------------------------------------- */
/* Scrollback                                                                 */
/* ------------------------------------------------------------------------- */

static void scroll_viewport(Term *term, GhosttyTerminalScrollViewport scroll)
{
    if (!term->handle)
        return;
    ghostty_terminal_scroll_viewport(term->handle, scroll);
}

void term_scroll_rows(Term *term, intptr_t delta)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = delta;
    scroll_viewport(term, scroll);
}

void term_scroll_top(Term *term)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_TOP;
    scroll_viewport(term, scroll);
}

void term_scroll_bottom(Term *term)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM;
    scroll_viewport(term, scroll);
}

void term_scroll_row(Term *term, uint32_t row)
{
    GhosttyTerminalScrollViewport scroll = { 0 };

    scroll.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_ROW;
    scroll.value.row = (size_t)row;
    scroll_viewport(term, scroll);
}

bool term_viewport_at_bottom(Term *term)
{
    bool active = true;

    if (!term->handle)
        return true;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE,
                             &active) != GHOSTTY_SUCCESS)
        return true;
    return active;
}

bool term_scrollbar(Term *term, GhosttyTerminalScrollbar *out)
{
    if (!term->handle || !out)
        return false;
    return ghostty_terminal_get(term->handle,
                                GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                out) == GHOSTTY_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Selection                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Grid references are snapshots: they are only valid until the next call
 * that changes the terminal. So every gesture resolves fresh viewport
 * points here and nothing is cached; a selection that has to survive new
 * output is rebuilt by the caller.
 */

static bool viewport_ref(Term *term, int x, int y, GhosttyGridRef *out_ref)
{
    GhosttyPoint point = { 0 };

    if (!term->handle || x < 0 || y < 0)
        return false;
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = (uint16_t)x;
    point.value.coordinate.y = (uint32_t)y;

    return ghostty_terminal_grid_ref(term->handle, point, out_ref)
           == GHOSTTY_SUCCESS;
}

static bool install_selection(Term *term, const GhosttySelection *selection)
{
    if (!term->handle)
        return false;
    return ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SELECTION,
                                selection) == GHOSTTY_SUCCESS;
}

void term_selection_clear(Term *term)
{
    if (!term->handle)
        return;
    ghostty_terminal_set(term->handle, GHOSTTY_TERMINAL_OPT_SELECTION, NULL);
}

bool term_selection_cells(Term *term, int start_x, int start_y, int end_x,
                          int end_y, bool rectangle)
{
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, start_x, start_y, &selection.start) ||
        !viewport_ref(term, end_x, end_y, &selection.end))
        return false;
    selection.rectangle = rectangle;
    return install_selection(term, &selection);
}

bool term_selection_word(Term *term, int x, int y)
{
    GhosttyTerminalSelectWordOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x, y, &options.ref))
        return false;
    if (ghostty_terminal_select_word(term->handle, &options, &selection)
            != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

/* The word range under an anchor and the drag point. Asking for the
 * nearest word between the two instead of only the one under the pointer
 * is what keeps a word drag from collapsing when the pointer passes
 * between words. */
bool term_selection_word_between(Term *term, int x1, int y1, int x2, int y2)
{
    GhosttyTerminalSelectWordBetweenOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordBetweenOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x1, y1, &options.start) ||
        !viewport_ref(term, x2, y2, &options.end))
        return false;
    if (ghostty_terminal_select_word_between(term->handle, &options,
                                             &selection) != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

bool term_selection_line(Term *term, int x, int y)
{
    GhosttyTerminalSelectLineOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
    GhosttySelection selection = { 0 };

    if (!viewport_ref(term, x, y, &options.ref))
        return false;
    if (ghostty_terminal_select_line(term->handle, &options, &selection)
            != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

/* ------------------------------------------------------------------------- */
/* A drag in progress                                                         */
/* ------------------------------------------------------------------------- */

/* Create the tracked anchor for a fresh gesture. Nothing is selected yet: a
 * press that never moves should only clear, so the app decides when the
 * first range appears. */
bool term_selection_drag_start(Term *term, int x, int y)
{
    GhosttyPoint point = { 0 };

    if (!term->handle || x < 0 || y < 0)
        return false;
    term_selection_drag_end(term);

    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = (uint16_t)x;
    point.value.coordinate.y = (uint32_t)y;
    return ghostty_terminal_grid_ref_track(term->handle, point,
                                           &term->drag_anchor)
           == GHOSTTY_SUCCESS;
}

/* Cells from the anchor to the focus. The anchor comes from the tracked
 * reference, so output arriving mid-drag moves it with the text; the focus
 * is the cell under the pointer, which is exactly what the user sees. */
bool term_selection_drag_cells(Term *term, int focus_x, int focus_y,
                               bool rectangle)
{
    GhosttySelection selection = { 0 };

    if (!term->drag_anchor ||
        ghostty_tracked_grid_ref_snapshot(term->drag_anchor,
                                          &selection.start) != GHOSTTY_SUCCESS)
        return false;
    if (!viewport_ref(term, focus_x, focus_y, &selection.end))
        return false;
    selection.rectangle = rectangle;
    return install_selection(term, &selection);
}

/* The nearest word between the anchor's current cell and the pointer's. */
bool term_selection_drag_words(Term *term, int focus_x, int focus_y)
{
    GhosttyTerminalSelectWordBetweenOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordBetweenOptions);
    GhosttyPointCoordinate anchor;
    GhosttySelection selection = { 0 };

    if (!term->drag_anchor ||
        ghostty_tracked_grid_ref_point(term->drag_anchor,
                                       GHOSTTY_POINT_TAG_VIEWPORT,
                                       &anchor) != GHOSTTY_SUCCESS)
        return false;
    if (!viewport_ref(term, (int)anchor.x, (int)anchor.y, &options.start) ||
        !viewport_ref(term, focus_x, focus_y, &options.end))
        return false;
    if (ghostty_terminal_select_word_between(term->handle, &options,
                                             &selection) != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

/* The whole line the anchor sits on, wherever that line has moved to. */
bool term_selection_drag_line(Term *term)
{
    GhosttyTerminalSelectLineOptions options =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
    GhosttyPointCoordinate anchor;
    GhosttySelection selection = { 0 };

    if (!term->drag_anchor ||
        ghostty_tracked_grid_ref_point(term->drag_anchor,
                                       GHOSTTY_POINT_TAG_VIEWPORT,
                                       &anchor) != GHOSTTY_SUCCESS)
        return false;
    if (!viewport_ref(term, (int)anchor.x, (int)anchor.y, &options.ref))
        return false;
    if (ghostty_terminal_select_line(term->handle, &options, &selection)
            != GHOSTTY_SUCCESS)
        return false;
    return install_selection(term, &selection);
}

/* The gesture is over. The selection itself stays: the terminal owns its
 * own tracked copy of it. */
void term_selection_drag_end(Term *term)
{
    if (!term || !term->drag_anchor)
        return;
    ghostty_tracked_grid_ref_free(term->drag_anchor);
    term->drag_anchor = NULL;
}

bool term_selection_has(Term *term)
{
    GhosttySelection selection = { 0 };

    if (!term->handle)
        return false;
    return ghostty_terminal_get(term->handle,
                                GHOSTTY_TERMINAL_DATA_SELECTION,
                                &selection) == GHOSTTY_SUCCESS;
}

char *term_selection_text(Term *term, size_t *out_length)
{
    GhosttySelection selection = { 0 };
    GhosttyFormatter formatter = NULL;
    GhosttyFormatterFormat emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    bool trim = true;
    uint8_t *buffer = NULL;
    size_t length = 0;
    char *text;

    if (out_length)
        *out_length = 0;
    if (!term->handle)
        return NULL;
    if (ghostty_terminal_get(term->handle,
                             GHOSTTY_TERMINAL_DATA_SELECTION,
                             &selection) != GHOSTTY_SUCCESS)
        return NULL;

    /* The formatter reads the terminal's own selection, so a wrapped line
     * comes out as the rows the user saw, without the padding. */
    if (ghostty_formatter_terminal_new(NULL, &formatter, term->handle)
            != GHOSTTY_SUCCESS)
        return NULL;
    if (ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_EMIT, &emit)
            != GHOSTTY_SUCCESS ||
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim)
            != GHOSTTY_SUCCESS ||
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_SELECTION,
                              &selection) != GHOSTTY_SUCCESS ||
        ghostty_formatter_format_alloc(formatter, NULL, &buffer, &length)
            != GHOSTTY_SUCCESS) {
        ghostty_formatter_free(formatter);
        return NULL;
    }
    ghostty_formatter_free(formatter);

    text = (char *)malloc(length + 1);
    if (!text) {
        ghostty_free(NULL, buffer, length);
        return NULL;
    }
    if (length > 0)
        memcpy(text, buffer, length);
    text[length] = '\0';
    ghostty_free(NULL, buffer, length);

    if (out_length)
        *out_length = length;
    if (length == 0) {
        free(text);
        return NULL;
    }
    return text;
}
