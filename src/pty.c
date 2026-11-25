/*
 * termica - the ConPTY side. See pty.h.
 *
 * The shape of the thing:
 *
 *   main thread                 reader thread
 *   -----------                 -------------
 *   pty_write  --> input pipe
 *   pty_read   <-- ring  <--    ReadFile(output pipe)
 *                     \---->   notify (SDL_PushEvent)
 *
 * The ring is bounded; when it is full the reader waits, which stalls
 * conhost, which stalls the program. That is the backpressure a terminal
 * wants: a program that floods the screen slows down instead of growing
 * the terminal's memory without limit.
 */

#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* CreatePseudoConsole and PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE are gated
 * behind the Windows 10 RS5 SDK level. The default _WIN32_WINNT in MinGW is
 * Windows 7, so ask for the version whose headers declare them. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006  /* NTDDI_WIN10_RS5 */
#endif

#include <windows.h>

/* 256 KiB of pending output. Enough for a burst like "dir C:\Windows",
 * small enough that a stopped UI thread does not pin megabytes. */
#define PTY_RING_BYTES (256u * 1024u)
#define PTY_READ_CHUNK 8192

/* How long pty_close waits for the reader to see the pseudoconsole go
 * away before giving up on it. */
#define PTY_READER_TIMEOUT_MS 5000
#define PTY_EXIT_TIMEOUT_MS 3000

struct Pty {
    HPCON  console;
    HANDLE input;          /* we write keys and replies here             */
    HANDLE output;         /* the reader thread reads program output here */
    HANDLE process;
    HANDLE job;
    HANDLE reader;
    HANDLE writer;

    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE has_space;   /* reader waits for room              */
    CONDITION_VARIABLE has_data;    /* pty_wait waits for bytes or EOF    */
    bool               lock_ready;

    uint8_t *ring;
    size_t   ring_capacity;
    size_t   ring_head;
    size_t   ring_length;

    CRITICAL_SECTION   wlock;
    CONDITION_VARIABLE wcond;       /* writer waits for queued bytes      */
    bool               wlock_ready;
    uint8_t           *wbuf;
    size_t             wcap;
    size_t             whead;
    size_t             wlen;
    bool               wclosing;
    bool               wbroken;

    bool eof;
    bool closing;
    bool wake_pending;      /* one notify per drain, not per chunk    */

    PtyNotifyFn notify;
    void       *notify_userdata;
};

/* ------------------------------------------------------------------------- */
/* Error text                                                                 */
/* ------------------------------------------------------------------------- */

static void trim_line_end(char *text)
{
    size_t length = strlen(text);

    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' '))
        text[--length] = '\0';
}

static void describe_win32_error(char *buffer, size_t size, const char *what)
{
    DWORD code = GetLastError();
    char  message[256] = "";

    if (!buffer || size == 0)
        return;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, message, (DWORD)sizeof message, NULL);
    trim_line_end(message);
    if (message[0])
        snprintf(buffer, size, "%s failed (error %lu): %s", what,
                 (unsigned long)code, message);
    else
        snprintf(buffer, size, "%s failed (error %lu)", what,
                 (unsigned long)code);
}

static void describe_hresult(char *buffer, size_t size, const char *what,
                             HRESULT result)
{
    DWORD code = HRESULT_FACILITY(result) == FACILITY_WIN32
                     ? HRESULT_CODE(result)
                     : (DWORD)result;
    char message[256] = "";

    if (!buffer || size == 0)
        return;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, message, (DWORD)sizeof message, NULL);
    trim_line_end(message);
    if (message[0])
        snprintf(buffer, size, "%s failed (0x%08lx): %s", what,
                 (unsigned long)result, message);
    else
        snprintf(buffer, size, "%s failed (0x%08lx)", what,
                 (unsigned long)result);
}

static void set_error_text(char *buffer, size_t size, const char *text)
{
    if (!buffer || size == 0)
        return;
    snprintf(buffer, size, "%s", text);
}

/* ------------------------------------------------------------------------- */
/* The ring buffer. Callers hold the lock.                                    */
/* ------------------------------------------------------------------------- */

static void ring_push(Pty *pty, const uint8_t *data, size_t length)
{
    size_t tail = (pty->ring_head + pty->ring_length) % pty->ring_capacity;
    size_t first = pty->ring_capacity - tail;

    if (first > length)
        first = length;
    memcpy(pty->ring + tail, data, first);
    if (first < length)
        memcpy(pty->ring, data + first, length - first);
    pty->ring_length += length;
}

static size_t ring_pop(Pty *pty, uint8_t *data, size_t capacity)
{
    size_t take = pty->ring_length < capacity ? pty->ring_length : capacity;
    size_t first = pty->ring_capacity - pty->ring_head;

    if (take == 0)
        return 0;
    if (first > take)
        first = take;
    memcpy(data, pty->ring + pty->ring_head, first);
    if (first < take)
        memcpy(data + first, pty->ring, take - first);
    pty->ring_head = (pty->ring_head + take) % pty->ring_capacity;
    pty->ring_length -= take;
    return take;
}

/* ------------------------------------------------------------------------- */
/* Reader thread                                                              */
/* ------------------------------------------------------------------------- */

/* Announce data to the embedder once per drain: the flag is cleared by
 * pty_read when the ring runs empty, so a flood of 8 KiB reads turns into
 * one wake-up per batch instead of thousands of SDL events. */
static void wake_main(Pty *pty)
{
    bool notify;

    EnterCriticalSection(&pty->lock);
    notify = !pty->wake_pending;
    pty->wake_pending = true;
    WakeAllConditionVariable(&pty->has_data);
    LeaveCriticalSection(&pty->lock);

    if (notify && pty->notify)
        pty->notify(pty->notify_userdata);
}

static DWORD WINAPI reader_main(LPVOID parameter)
{
    Pty *pty = (Pty *)parameter;
    uint8_t chunk[PTY_READ_CHUNK];
    DWORD read = 0;

    for (;;) {
        size_t offset = 0;
        bool stop = false;

        /* The output pipe is synchronous; a zero-length read is EOF, and
         * a failed read is EOF too (ERROR_BROKEN_PIPE when the child and
         * conhost are gone). */
        if (!ReadFile(pty->output, chunk, (DWORD)sizeof chunk, &read, NULL) ||
            read == 0)
            break;

        EnterCriticalSection(&pty->lock);
        while (offset < read && !pty->closing) {
            size_t room;
            size_t take;

            while (pty->ring_length == pty->ring_capacity && !pty->closing)
                SleepConditionVariableCS(&pty->has_space, &pty->lock, 100);
            if (pty->closing)
                break;
            room = pty->ring_capacity - pty->ring_length;
            take = read - offset < room ? read - offset : room;
            ring_push(pty, chunk + offset, take);
            offset += take;
        }
        stop = pty->closing;
        LeaveCriticalSection(&pty->lock);

        if (stop)
            break;
        wake_main(pty);
    }

    EnterCriticalSection(&pty->lock);
    pty->eof = true;
    LeaveCriticalSection(&pty->lock);
    wake_main(pty);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Asynchronous writer thread (PLAN 1.7)                                      */
/* ------------------------------------------------------------------------- */

/* Both keystrokes and multi-megabyte pastes go through this single FIFO
 * queue, so byte ordering is strictly preserved and the UI thread never
 * blocks waiting on conhost to drain the input pipe. */
static bool wqueue_push(Pty *pty, const uint8_t *data, size_t length)
{
    size_t tail;
    size_t first;

    if (pty->wclosing || pty->wbroken)
        return false;
    if (pty->wlen + length > pty->wcap) {
        size_t next_cap = pty->wcap ? pty->wcap : 16384u;
        uint8_t *next_buf;

        while (next_cap < pty->wlen + length) {
            if (next_cap > ((size_t)-1) / 2u)
                return false;
            next_cap *= 2u;
        }
        next_buf = (uint8_t *)malloc(next_cap);
        if (!next_buf)
            return false;
        if (pty->wlen > 0) {
            size_t head_part = pty->wcap - pty->whead;

            if (head_part > pty->wlen)
                head_part = pty->wlen;
            memcpy(next_buf, pty->wbuf + pty->whead, head_part);
            if (head_part < pty->wlen)
                memcpy(next_buf + head_part, pty->wbuf, pty->wlen - head_part);
        }
        free(pty->wbuf);
        pty->wbuf = next_buf;
        pty->wcap = next_cap;
        pty->whead = 0;
    }

    tail = (pty->whead + pty->wlen) % pty->wcap;
    first = pty->wcap - tail;
    if (first > length)
        first = length;
    memcpy(pty->wbuf + tail, data, first);
    if (first < length)
        memcpy(pty->wbuf, data + first, length - first);
    pty->wlen += length;
    return true;
}

static size_t wqueue_pop(Pty *pty, uint8_t *out, size_t capacity)
{
    size_t take = pty->wlen < capacity ? pty->wlen : capacity;
    size_t first;

    if (take == 0)
        return 0;
    first = pty->wcap - pty->whead;
    if (first > take)
        first = take;
    memcpy(out, pty->wbuf + pty->whead, first);
    if (first < take)
        memcpy(out + first, pty->wbuf, take - first);
    pty->whead = (pty->whead + take) % pty->wcap;
    pty->wlen -= take;
    return take;
}

static DWORD WINAPI writer_main(LPVOID parameter)
{
    Pty *pty = (Pty *)parameter;
    uint8_t chunk[16384];

    for (;;) {
        size_t take;
        size_t offset = 0;

        EnterCriticalSection(&pty->wlock);
        while (pty->wlen == 0 && !pty->wclosing)
            SleepConditionVariableCS(&pty->wcond, &pty->wlock, 100);
        if (pty->wclosing) {
            LeaveCriticalSection(&pty->wlock);
            break;
        }
        take = wqueue_pop(pty, chunk, sizeof chunk);
        LeaveCriticalSection(&pty->wlock);

        while (offset < take) {
            DWORD written = 0;

            if (!WriteFile(pty->input, chunk + offset,
                           (DWORD)(take - offset), &written, NULL) ||
                written == 0) {
                EnterCriticalSection(&pty->wlock);
                pty->wbroken = true;
                LeaveCriticalSection(&pty->wlock);
                return 0;
            }
            offset += written;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Spawn                                                                      */
/* ------------------------------------------------------------------------- */

static void pty_free(Pty *pty);

/* Case-insensitive compare of an ASCII environment variable name. */
static bool name_matches(const wchar_t *entry, const wchar_t *name,
                         size_t name_length)
{
    size_t index;

    for (index = 0; index < name_length; index++) {
        wchar_t left = entry[index];
        wchar_t right = name[index];

        if (left >= L'A' && left <= L'Z')
            left = (wchar_t)(left + 32);
        if (right >= L'A' && right <= L'Z')
            right = (wchar_t)(right + 32);
        if (left != right)
            return false;
    }
    return true;
}

static bool is_dumb_terminal(const wchar_t *value)
{
    return name_matches(value, L"dumb", 4) && value[4] == L'\0';
}

static wchar_t *utf8_to_wide(const char *text)
{
    int needed;
    wchar_t *wide;

    if (!text)
        return NULL;
    needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (needed <= 0)
        return NULL;
    wide = (wchar_t *)malloc((size_t)needed * sizeof *wide);
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed) != needed) {
        free(wide);
        return NULL;
    }
    return wide;
}

static bool arg_needs_quotes(const wchar_t *arg)
{
    size_t i;

    if (!arg[0])
        return true;
    for (i = 0; arg[i]; i++) {
        if (arg[i] == L' ' || arg[i] == L'\t' || arg[i] == L'\n' ||
            arg[i] == L'\v' || arg[i] == L'"')
            return true;
    }
    return false;
}

/* Append one argument using standard Windows CommandLineToArgvW quoting.
 * When `out` is NULL, only returns the number of wide characters needed. */
static size_t append_quoted_arg(wchar_t *out, size_t offset,
                                const wchar_t *arg)
{
    size_t i = 0;

    if (!arg_needs_quotes(arg)) {
        size_t len = wcslen(arg);

        if (out)
            memcpy(out + offset, arg, len * sizeof *out);
        return offset + len;
    }

    if (out)
        out[offset] = L'"';
    offset++;

    while (arg[i]) {
        size_t bs = 0;

        while (arg[i] == L'\\') {
            bs++;
            i++;
        }
        if (arg[i] == L'\0') {
            size_t n = bs * 2;
            while (n--) {
                if (out)
                    out[offset] = L'\\';
                offset++;
            }
            break;
        }
        if (arg[i] == L'"') {
            size_t n = bs * 2 + 1;
            while (n--) {
                if (out)
                    out[offset] = L'\\';
                offset++;
            }
            if (out)
                out[offset] = L'"';
            offset++;
            i++;
        } else {
            while (bs--) {
                if (out)
                    out[offset] = L'\\';
                offset++;
            }
            if (out)
                out[offset] = arg[i];
            offset++;
            i++;
        }
    }

    if (out)
        out[offset] = L'"';
    return offset + 1;
}

wchar_t *pty_build_command_line(const char *const *argv, int argc)
{
    wchar_t **wide_args;
    wchar_t *result = NULL;
    size_t total = 0;
    size_t offset = 0;
    int i;

    if (!argv || argc <= 0 || !argv[0] || !argv[0][0])
        return NULL;

    wide_args = (wchar_t **)calloc((size_t)argc, sizeof *wide_args);
    if (!wide_args)
        return NULL;

    for (i = 0; i < argc; i++) {
        if (!argv[i])
            goto fail;
        wide_args[i] = utf8_to_wide(argv[i]);
        if (!wide_args[i])
            goto fail;
        if (i > 0)
            total++;
        total = append_quoted_arg(NULL, total, wide_args[i]);
    }

    result = (wchar_t *)malloc((total + 1) * sizeof *result);
    if (!result)
        goto fail;

    for (i = 0; i < argc; i++) {
        if (i > 0)
            result[offset++] = L' ';
        offset = append_quoted_arg(result, offset, wide_args[i]);
    }
    result[offset] = L'\0';

fail:
    for (i = 0; i < argc; i++)
        free(wide_args[i]);
    free(wide_args);
    return result;
}

static bool entry_in_extra(const wchar_t *entry, wchar_t *const *extra_wide,
                           int extra_count)
{
    int i;

    for (i = 0; i < extra_count; i++) {
        const wchar_t *eq;
        size_t prefix_len;

        if (!extra_wide[i])
            continue;
        eq = wcschr(extra_wide[i], L'=');
        if (!eq)
            continue;
        prefix_len = (size_t)(eq - extra_wide[i]) + 1;
        if (name_matches(entry, extra_wide[i], prefix_len))
            return true;
    }
    return false;
}

/* The child inherits this process's environment, except that TERM,
 * COLORTERM, TERMICA and TERMICA_SHELL_INTEGRATION are filled in: TERM
 * when missing or "dumb", COLORTERM when missing or empty, the two TERMICA
 * markers always; then any user-configured `extra` entries overlay the
 * result (case-insensitive replacement). The result is a
 * GetEnvironmentStringsW()-style block (double NUL) owned by the caller. */
wchar_t *pty_build_environment(const PtyEnvEntry *extra, int extra_count)
{
    static const wchar_t term_entry[] = L"TERM=xterm-256color";
    static const wchar_t colorterm_entry[] = L"COLORTERM=truecolor";
    static const wchar_t termica_entry[] = L"TERMICA=1";
    static const wchar_t integration_entry[] = L"TERMICA_SHELL_INTEGRATION=1";
    wchar_t *block = GetEnvironmentStringsW();
    wchar_t **extra_wide = NULL;
    wchar_t *result = NULL;
    wchar_t *cursor;
    size_t   chars = 1;             /* the block's final NUL */
    size_t   offset = 0;
    bool     replace_term = true;
    bool     replace_colorterm = true;
    bool     replace_termica = true;
    bool     replace_integration = true;
    int      i;

    if (!block)
        return NULL;

    if (extra && extra_count > 0) {
        extra_wide = (wchar_t **)calloc((size_t)extra_count,
                                        sizeof *extra_wide);
        if (!extra_wide) {
            FreeEnvironmentStringsW(block);
            return NULL;
        }
        for (i = 0; i < extra_count; i++) {
            wchar_t *wk;
            wchar_t *wv;
            size_t lk;
            size_t lv;

            if (!extra[i].key || !extra[i].key[0] || !extra[i].value)
                continue;
            wk = utf8_to_wide(extra[i].key);
            wv = utf8_to_wide(extra[i].value);
            if (wk && wv) {
                lk = wcslen(wk);
                lv = wcslen(wv);
                extra_wide[i] = (wchar_t *)malloc((lk + 1 + lv + 1) *
                                                  sizeof(wchar_t));
                if (extra_wide[i]) {
                    memcpy(extra_wide[i], wk, lk * sizeof(wchar_t));
                    extra_wide[i][lk] = L'=';
                    memcpy(extra_wide[i] + lk + 1, wv,
                           (lv + 1) * sizeof(wchar_t));
                }
            }
            free(wk);
            free(wv);
        }
    }

    /* Pass 1: what is already there? */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        size_t length = wcslen(cursor);

        if (name_matches(cursor, L"TERM=", 5))
            replace_term = length == 5 || is_dumb_terminal(cursor + 5);
        else if (name_matches(cursor, L"COLORTERM=", 10))
            replace_colorterm = length == 10;
        else if (name_matches(cursor, L"TERMICA=", 8))
            replace_termica = true;
        else if (name_matches(cursor, L"TERMICA_SHELL_INTEGRATION=", 26))
            replace_integration = true;
    }

    /* Explicit user overrides in `extra` take precedence over defaults. */
    if (extra_wide) {
        if (entry_in_extra(term_entry, extra_wide, extra_count))
            replace_term = false;
        if (entry_in_extra(colorterm_entry, extra_wide, extra_count))
            replace_colorterm = false;
        if (entry_in_extra(termica_entry, extra_wide, extra_count))
            replace_termica = false;
        if (entry_in_extra(integration_entry, extra_wide, extra_count))
            replace_integration = false;
    }

    /* Pass 2: size the copy, leaving out the entries being replaced. */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        if (extra_wide && entry_in_extra(cursor, extra_wide, extra_count))
            continue;
        if (name_matches(cursor, L"TERM=", 5) && replace_term)
            continue;
        if (name_matches(cursor, L"COLORTERM=", 10) && replace_colorterm)
            continue;
        if (name_matches(cursor, L"TERMICA=", 8) && replace_termica)
            continue;
        if (name_matches(cursor, L"TERMICA_SHELL_INTEGRATION=", 26) &&
            replace_integration)
            continue;
        chars += wcslen(cursor) + 1;
    }
    /* Each appended entry needs room for its own NUL as well; the block's
     * final NUL is already counted in `chars`. */
    if (replace_term)
        chars += sizeof term_entry / sizeof term_entry[0];
    if (replace_colorterm)
        chars += sizeof colorterm_entry / sizeof colorterm_entry[0];
    if (replace_termica)
        chars += sizeof termica_entry / sizeof termica_entry[0];
    if (replace_integration)
        chars += sizeof integration_entry / sizeof integration_entry[0];
    if (extra_wide) {
        for (i = 0; i < extra_count; i++) {
            if (extra_wide[i])
                chars += wcslen(extra_wide[i]) + 1;
        }
    }

    result = (wchar_t *)malloc(chars * sizeof *result);
    if (!result) {
        FreeEnvironmentStringsW(block);
        goto done;
    }

    /* Pass 3: copy what stays, then append the replacements and extras. */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        size_t length = wcslen(cursor) + 1;

        if (extra_wide && entry_in_extra(cursor, extra_wide, extra_count))
            continue;
        if (name_matches(cursor, L"TERM=", 5) && replace_term)
            continue;
        if (name_matches(cursor, L"COLORTERM=", 10) && replace_colorterm)
            continue;
        if (name_matches(cursor, L"TERMICA=", 8) && replace_termica)
            continue;
        if (name_matches(cursor, L"TERMICA_SHELL_INTEGRATION=", 26) &&
            replace_integration)
            continue;
        memcpy(result + offset, cursor, length * sizeof *result);
        offset += length;
    }
    FreeEnvironmentStringsW(block);

    /* Every entry, original or appended, ends with its own NUL; the last
     * NUL of the block is written after them all. */
    if (replace_term) {
        size_t length = sizeof term_entry / sizeof term_entry[0];

        memcpy(result + offset, term_entry, length * sizeof *result);
        offset += length;
    }
    if (replace_colorterm) {
        size_t length = sizeof colorterm_entry / sizeof colorterm_entry[0];

        memcpy(result + offset, colorterm_entry, length * sizeof *result);
        offset += length;
    }
    if (replace_termica) {
        size_t length = sizeof termica_entry / sizeof termica_entry[0];

        memcpy(result + offset, termica_entry, length * sizeof *result);
        offset += length;
    }
    if (replace_integration) {
        size_t length = sizeof integration_entry / sizeof integration_entry[0];

        memcpy(result + offset, integration_entry, length * sizeof *result);
        offset += length;
    }
    if (extra_wide) {
        for (i = 0; i < extra_count; i++) {
            if (extra_wide[i]) {
                size_t length = wcslen(extra_wide[i]) + 1;

                memcpy(result + offset, extra_wide[i], length * sizeof *result);
                offset += length;
            }
        }
    }
    result[offset] = L'\0';

done:
    if (extra_wide) {
        for (i = 0; i < extra_count; i++)
            free(extra_wide[i]);
        free(extra_wide);
    }
    return result;
}

Pty *pty_spawn_ex(const wchar_t *command_line, const char *cwd_utf8,
                  const PtyEnvEntry *env, int env_count,
                  uint16_t columns, uint16_t rows,
                  PtyNotifyFn notify, void *notify_userdata,
                  char *error, size_t error_size)
{
    Pty                   *pty;
    SECURITY_ATTRIBUTES    inheritable;
    HANDLE                 input_read = NULL;
    HANDLE                 output_write = NULL;
    STARTUPINFOEXW         startup;
    PROCESS_INFORMATION    process;
    SIZE_T                 attribute_bytes = 0;
    wchar_t               *mutable_line = NULL;
    wchar_t               *environment = NULL;
    wchar_t               *wide_cwd = NULL;
    COORD                  size;
    HRESULT                result;
    bool                   attributes_ready = false;
    bool                   ok = false;

    if (error && error_size)
        error[0] = '\0';
    if (!command_line || command_line[0] == L'\0') {
        set_error_text(error, error_size, "no shell command line");
        return NULL;
    }
    if (columns < 1)
        columns = 1;
    if (rows < 1)
        rows = 1;

    pty = (Pty *)calloc(1, sizeof *pty);
    if (!pty) {
        set_error_text(error, error_size, "out of memory");
        return NULL;
    }
    pty->input = INVALID_HANDLE_VALUE;
    pty->output = INVALID_HANDLE_VALUE;
    pty->ring_capacity = PTY_RING_BYTES;
    pty->ring = (uint8_t *)malloc(pty->ring_capacity);
    if (!pty->ring) {
        set_error_text(error, error_size, "out of memory");
        pty_free(pty);
        return NULL;
    }
    InitializeCriticalSection(&pty->lock);
    InitializeConditionVariable(&pty->has_space);
    InitializeConditionVariable(&pty->has_data);
    pty->lock_ready = true;
    InitializeCriticalSection(&pty->wlock);
    InitializeConditionVariable(&pty->wcond);
    pty->wlock_ready = true;
    pty->notify = notify;
    pty->notify_userdata = notify_userdata;

    ZeroMemory(&startup, sizeof startup);
    ZeroMemory(&process, sizeof process);

    /* The two pseudoconsole ends are inheritable so conhost can pick them
     * up; our own ends must not leak into the child. */
    inheritable.nLength = sizeof inheritable;
    inheritable.lpSecurityDescriptor = NULL;
    inheritable.bInheritHandle = TRUE;

    if (!CreatePipe(&input_read, &pty->input, &inheritable, 0)) {
        describe_win32_error(error, error_size, "CreatePipe (input)");
        goto done;
    }
    if (!CreatePipe(&pty->output, &output_write, &inheritable, 0)) {
        describe_win32_error(error, error_size, "CreatePipe (output)");
        goto done;
    }
    if (!SetHandleInformation(pty->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(pty->output, HANDLE_FLAG_INHERIT, 0)) {
        describe_win32_error(error, error_size, "SetHandleInformation");
        goto done;
    }

    size.X = (SHORT)columns;
    size.Y = (SHORT)rows;
    result = CreatePseudoConsole(size, input_read, output_write, 0,
                                 &pty->console);
    if (FAILED(result)) {
        describe_hresult(error, error_size, "CreatePseudoConsole", result);
        goto done;
    }

    /* The pseudoconsole owns its ends now. Closing our copies is what makes
     * the output pipe report EOF once conhost exits. */
    CloseHandle(input_read);
    input_read = NULL;
    CloseHandle(output_write);
    output_write = NULL;

    /* termica runs as a console program, so it has real stdin/stdout/stderr
     * handles of its own. Without this, the child inherits them and writes
     * past the pseudoconsole straight to the terminal that launched
     * termica. Explicit NULL handles make conhost give the child the
     * pseudoconsole's console handles instead - the same state a GUI
     * terminal's child starts in. */
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = NULL;
    startup.StartupInfo.hStdOutput = NULL;
    startup.StartupInfo.hStdError = NULL;

    startup.StartupInfo.cb = sizeof startup;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
    startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)
        HeapAlloc(GetProcessHeap(), 0, attribute_bytes);
    if (!startup.lpAttributeList ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attribute_bytes)) {
        describe_win32_error(error, error_size, "InitializeProcThreadAttributeList");
        goto done;
    }
    attributes_ready = true;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   (PVOID)pty->console, sizeof(HPCON),
                                   NULL, NULL)) {
        describe_win32_error(error, error_size, "UpdateProcThreadAttribute");
        goto done;
    }

    /* CreateProcessW may modify the command line, so it needs a copy. */
    {
        size_t length = wcslen(command_line);

        mutable_line = (wchar_t *)malloc((length + 1) * sizeof *mutable_line);
        if (!mutable_line) {
            set_error_text(error, error_size, "out of memory");
            goto done;
        }
        memcpy(mutable_line, command_line,
               (length + 1) * sizeof *mutable_line);
    }

    if (cwd_utf8 && cwd_utf8[0]) {
        wide_cwd = utf8_to_wide(cwd_utf8);
        if (!wide_cwd) {
            set_error_text(error, error_size, "invalid working directory");
            goto done;
        }
    }

    environment = pty_build_environment(env, env_count);

    /* Suspended first, so the process cannot fork before it is in the job
     * that guarantees the tree dies with the window. */
    if (!CreateProcessW(NULL, mutable_line, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT |
                            CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        environment, wide_cwd, &startup.StartupInfo, &process)) {
        describe_win32_error(error, error_size, "CreateProcessW");
        goto done;
    }
    pty->process = process.hProcess;

    pty->job = CreateJobObjectW(NULL, NULL);
    if (pty->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;

        ZeroMemory(&limits, sizeof limits);
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(pty->job, JobObjectExtendedLimitInformation,
                                    &limits, sizeof limits))
            AssignProcessToJobObject(pty->job, pty->process);
    }

    if (ResumeThread(process.hThread) == (DWORD)-1) {
        describe_win32_error(error, error_size, "ResumeThread");
        goto done;
    }
    CloseHandle(process.hThread);
    process.hThread = NULL;

    pty->reader = CreateThread(NULL, 0, reader_main, pty, 0, NULL);
    if (!pty->reader) {
        describe_win32_error(error, error_size, "CreateThread (reader)");
        goto done;
    }
    pty->writer = CreateThread(NULL, 0, writer_main, pty, 0, NULL);
    if (!pty->writer) {
        describe_win32_error(error, error_size, "CreateThread (writer)");
        goto done;
    }
    ok = true;

done:
    if (input_read)
        CloseHandle(input_read);
    if (output_write)
        CloseHandle(output_write);
    if (startup.lpAttributeList) {
        if (attributes_ready)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    if (process.hThread)
        CloseHandle(process.hThread);
    free(mutable_line);
    free(environment);
    free(wide_cwd);
    if (!ok) {
        pty_free(pty);
        return NULL;
    }
    return pty;
}

Pty *pty_spawn(const wchar_t *command_line, uint16_t columns, uint16_t rows,
               PtyNotifyFn notify, void *notify_userdata,
               char *error, size_t error_size)
{
    return pty_spawn_ex(command_line, NULL, NULL, 0, columns, rows,
                        notify, notify_userdata, error, error_size);
}

/* ------------------------------------------------------------------------- */
/* Shell discovery                                                            */
/* ------------------------------------------------------------------------- */

/* Build a quoted command line for one located program: PowerShell gets
 * -NoLogo, cmd needs nothing to be interactive. */
static wchar_t *shell_line(const wchar_t *path, int candidate)
{
    size_t needed = wcslen(path) + 32;
    wchar_t *line = (wchar_t *)malloc(needed * sizeof *line);

    if (!line)
        return NULL;
    if (candidate < 2)
        swprintf(line, needed, L"\"%ls\" -NoLogo", path);
    else
        swprintf(line, needed, L"\"%ls\"", path);
    return line;
}

/* candidate is the index into CANDIDATES; a negative value means "try each
 * in turn until one is found". */
static wchar_t *find_shell(int candidate)
{
    static const wchar_t *const CANDIDATES[] = {
        L"pwsh.exe",         /* PowerShell 7+                        */
        L"powershell.exe",   /* Windows PowerShell                   */
        L"cmd.exe"           /* always present                       */
    };
    const size_t count = sizeof CANDIDATES / sizeof CANDIDATES[0];
    wchar_t path[1024];
    size_t index;
    size_t first;
    size_t last;

    if (candidate >= 0) {
        if ((size_t)candidate >= count)
            return NULL;
        first = (size_t)candidate;
        last = first + 1;
    } else {
        first = 0;
        last = count;
    }

    for (index = first; index < last; index++) {
        DWORD length = SearchPathW(NULL, CANDIDATES[index], NULL,
                                   (DWORD)(sizeof path / sizeof path[0]),
                                   path, NULL);

        if (length == 0 || length >= sizeof path / sizeof path[0])
            continue;
        return shell_line(path, (int)index);
    }
    return NULL;
}

wchar_t *pty_default_shell(void)
{
    return find_shell(-1);
}

wchar_t *pty_shell_for(int which)
{
    if (which <= 0)
        return pty_default_shell();
    return find_shell(which - 1);
}

/* ------------------------------------------------------------------------- */
/* Data path                                                                  */
/* ------------------------------------------------------------------------- */

size_t pty_read(Pty *pty, void *buffer, size_t capacity)
{
    size_t taken;
    bool   room = false;

    if (!pty || !buffer || capacity == 0)
        return 0;
    EnterCriticalSection(&pty->lock);
    taken = ring_pop(pty, (uint8_t *)buffer, capacity);
    if (taken > 0 && pty->ring_length < pty->ring_capacity)
        room = true;
    if (pty->ring_length == 0)
        pty->wake_pending = false;
    LeaveCriticalSection(&pty->lock);

    if (room)
        WakeAllConditionVariable(&pty->has_space);
    return taken;
}

size_t pty_pending(Pty *pty)
{
    size_t pending;
    if (!pty)
        return 0;
    EnterCriticalSection(&pty->lock);
    pending = pty->ring_length;
    LeaveCriticalSection(&pty->lock);
    return pending;
}

bool pty_write(Pty *pty, const void *data, size_t length)
{
    bool ok;

    if (!pty || !data || pty->input == INVALID_HANDLE_VALUE)
        return false;
    if (length == 0)
        return true;

    EnterCriticalSection(&pty->wlock);
    ok = wqueue_push(pty, (const uint8_t *)data, length);
    if (ok)
        WakeConditionVariable(&pty->wcond);
    LeaveCriticalSection(&pty->wlock);
    return ok;
}

bool pty_resize(Pty *pty, uint16_t columns, uint16_t rows)
{
    COORD size;

    if (!pty || !pty->console || columns < 1 || rows < 1 ||
        columns > 32767 || rows > 32767)
        return false;
    size.X = (SHORT)columns;
    size.Y = (SHORT)rows;
    return SUCCEEDED(ResizePseudoConsole(pty->console, size));
}

bool pty_wait(Pty *pty, uint32_t timeout_ms)
{
    bool ready;

    if (!pty)
        return false;
    EnterCriticalSection(&pty->lock);
    ready = pty->ring_length > 0 || pty->eof;
    while (!ready) {
        BOOL woke = SleepConditionVariableCS(&pty->has_data, &pty->lock,
                                             timeout_ms);

        ready = pty->ring_length > 0 || pty->eof;
        if (!woke && !ready)
            break;
    }
    LeaveCriticalSection(&pty->lock);
    return ready;
}

bool pty_exit_code(Pty *pty, uint32_t *code)
{
    DWORD status = 0;

    if (!pty || !pty->process)
        return false;
    if (WaitForSingleObject(pty->process, 0) != WAIT_OBJECT_0)
        return false;
    if (!GetExitCodeProcess(pty->process, &status))
        return false;
    if (code)
        *code = (uint32_t)status;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Close                                                                      */
/* ------------------------------------------------------------------------- */

static void pty_free(Pty *pty)
{
    if (!pty)
        return;

    if (pty->lock_ready) {
        EnterCriticalSection(&pty->lock);
        pty->closing = true;
        WakeAllConditionVariable(&pty->has_space);
        LeaveCriticalSection(&pty->lock);
    }
    if (pty->wlock_ready) {
        EnterCriticalSection(&pty->wlock);
        pty->wclosing = true;
        WakeAllConditionVariable(&pty->wcond);
        LeaveCriticalSection(&pty->wlock);
    }

    /* Closing the pseudoconsole ends conhost's session; that closes the
     * output pipe, which unblocks ReadFile, which ends the reader thread. */
    if (pty->console) {
        ClosePseudoConsole(pty->console);
        pty->console = NULL;
    }
    if (pty->input != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->input);
        pty->input = INVALID_HANDLE_VALUE;
    }
    if (pty->output != INVALID_HANDLE_VALUE) {
        CloseHandle(pty->output);
        pty->output = INVALID_HANDLE_VALUE;
    }
    if (pty->writer) {
        WaitForSingleObject(pty->writer, PTY_READER_TIMEOUT_MS);
        CloseHandle(pty->writer);
        pty->writer = NULL;
    }
    if (pty->reader) {
        WaitForSingleObject(pty->reader, PTY_READER_TIMEOUT_MS);
        CloseHandle(pty->reader);
        pty->reader = NULL;
    }

    /* Kill whatever the shell left behind, then make sure the direct child
     * is gone before its handle is released. */
    if (pty->job) {
        CloseHandle(pty->job);
        pty->job = NULL;
    }
    if (pty->process) {
        if (WaitForSingleObject(pty->process, PTY_EXIT_TIMEOUT_MS) !=
            WAIT_OBJECT_0)
            TerminateProcess(pty->process, 1);
        CloseHandle(pty->process);
        pty->process = NULL;
    }

    if (pty->wlock_ready)
        DeleteCriticalSection(&pty->wlock);
    if (pty->lock_ready)
        DeleteCriticalSection(&pty->lock);
    free(pty->wbuf);
    free(pty->ring);
    free(pty);
}

void pty_close(Pty *pty)
{
    pty_free(pty);
}


#endif /* _WIN32 */
