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

    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE has_space;   /* reader waits for room              */
    CONDITION_VARIABLE has_data;    /* pty_wait waits for bytes or EOF    */
    bool               lock_ready;

    uint8_t *ring;
    size_t   ring_capacity;
    size_t   ring_head;
    size_t   ring_length;

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

/* The child inherits this process's environment, except that TERM and
 * COLORTERM are filled in: TERM when missing or "dumb", COLORTERM when
 * missing or empty. Programs that check whether they are talking to a real
 * terminal (git, vim, starship's prompt) key off them, and a Windows
 * process has neither by default. The result is a
 * GetEnvironmentStringsW()-style block (double NUL) owned by the caller. */
static wchar_t *child_environment(void)
{
    static const wchar_t term_entry[] = L"TERM=xterm-256color";
    static const wchar_t colorterm_entry[] = L"COLORTERM=truecolor";
    wchar_t *block = GetEnvironmentStringsW();
    wchar_t *result;
    wchar_t *cursor;
    size_t   chars = 1;             /* the block's final NUL */
    size_t   offset = 0;
    bool     replace_term = true;
    bool     replace_colorterm = true;

    if (!block)
        return NULL;

    /* Pass 1: what is already there? */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        size_t length = wcslen(cursor);

        if (name_matches(cursor, L"TERM=", 5))
            replace_term = length == 5 || is_dumb_terminal(cursor + 5);
        else if (name_matches(cursor, L"COLORTERM=", 10))
            replace_colorterm = length == 10;
    }

    /* Pass 2: size the copy, leaving out the entries being replaced. */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        if (name_matches(cursor, L"TERM=", 5) && replace_term)
            continue;
        if (name_matches(cursor, L"COLORTERM=", 10) && replace_colorterm)
            continue;
        chars += wcslen(cursor) + 1;
    }
    if (replace_term)
        chars += sizeof term_entry / sizeof term_entry[0] - 1;
    if (replace_colorterm)
        chars += sizeof colorterm_entry / sizeof colorterm_entry[0] - 1;

    result = (wchar_t *)malloc(chars * sizeof *result);
    if (!result) {
        FreeEnvironmentStringsW(block);
        return NULL;
    }

    /* Pass 3: copy what stays, then append the replacements. */
    for (cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
        size_t length = wcslen(cursor) + 1;

        if (name_matches(cursor, L"TERM=", 5) && replace_term)
            continue;
        if (name_matches(cursor, L"COLORTERM=", 10) && replace_colorterm)
            continue;
        memcpy(result + offset, cursor, length * sizeof *result);
        offset += length;
    }
    FreeEnvironmentStringsW(block);

    if (replace_term) {
        size_t length = sizeof term_entry / sizeof term_entry[0] - 1;

        memcpy(result + offset, term_entry, length * sizeof *result);
        offset += length;
    }
    if (replace_colorterm) {
        size_t length = sizeof colorterm_entry / sizeof colorterm_entry[0] - 1;

        memcpy(result + offset, colorterm_entry, length * sizeof *result);
        offset += length;
    }
    result[offset] = L'\0';
    return result;
}

Pty *pty_spawn(const wchar_t *command_line, uint16_t columns, uint16_t rows,
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

    environment = child_environment();

    /* Suspended first, so the process cannot fork before it is in the job
     * that guarantees the tree dies with the window. */
    if (!CreateProcessW(NULL, mutable_line, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT |
                            CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        environment, NULL, &startup.StartupInfo, &process)) {
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
        describe_win32_error(error, error_size, "CreateThread");
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
    if (!ok) {
        pty_free(pty);
        return NULL;
    }
    return pty;
}

/* ------------------------------------------------------------------------- */
/* Shell discovery                                                            */
/* ------------------------------------------------------------------------- */

wchar_t *pty_default_shell(void)
{
    static const wchar_t *candidates[] = {
        L"pwsh.exe",         /* PowerShell 7+                        */
        L"powershell.exe",   /* Windows PowerShell                   */
        L"cmd.exe"           /* always present, through %COMSPEC%    */
    };
    wchar_t path[1024];
    size_t  index;

    for (index = 0; index < sizeof candidates / sizeof candidates[0]; index++) {
        DWORD length = SearchPathW(NULL, candidates[index], NULL,
                                   (DWORD)(sizeof path / sizeof path[0]),
                                   path, NULL);
        wchar_t *line;
        size_t   needed;

        if (length == 0 || length >= sizeof path / sizeof path[0])
            continue;
        needed = wcslen(path) + 32;
        line = (wchar_t *)malloc(needed * sizeof *line);
        if (!line)
            return NULL;
        if (index < 2)
            swprintf(line, needed, L"\"%ls\" -NoLogo", path);
        else
            swprintf(line, needed, L"\"%ls\"", path);
        return line;
    }
    return NULL;
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

bool pty_write(Pty *pty, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (!pty || !bytes || pty->input == INVALID_HANDLE_VALUE)
        return false;
    while (length > 0) {
        DWORD chunk = length > 16384 ? 16384 : (DWORD)length;
        DWORD written = 0;

        if (!WriteFile(pty->input, bytes, chunk, &written, NULL) ||
            written == 0)
            return false;
        bytes += written;
        length -= written;
    }
    return true;
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

    if (pty->lock_ready)
        DeleteCriticalSection(&pty->lock);
    free(pty->ring);
    free(pty);
}

void pty_close(Pty *pty)
{
    pty_free(pty);
}

#else /* !_WIN32 */

/*
 * ConPTY is the Windows implementation of this module. On other platforms
 * the app still builds and runs the terminal; only the shell cannot start.
 */

struct Pty {
    int unused;
};

wchar_t *pty_default_shell(void)
{
    return NULL;
}

Pty *pty_spawn(const wchar_t *command_line, uint16_t columns, uint16_t rows,
               PtyNotifyFn notify, void *notify_userdata,
               char *error, size_t error_size)
{
    (void)command_line;
    (void)columns;
    (void)rows;
    (void)notify;
    (void)notify_userdata;
    if (error && error_size)
        snprintf(error, error_size, "ConPTY is only available on Windows");
    return NULL;
}

bool pty_resize(Pty *pty, uint16_t columns, uint16_t rows)
{
    (void)pty;
    (void)columns;
    (void)rows;
    return false;
}

size_t pty_read(Pty *pty, void *buffer, size_t capacity)
{
    (void)pty;
    (void)buffer;
    (void)capacity;
    return 0;
}

bool pty_write(Pty *pty, const void *data, size_t length)
{
    (void)pty;
    (void)data;
    (void)length;
    return false;
}

bool pty_wait(Pty *pty, uint32_t timeout_ms)
{
    (void)pty;
    (void)timeout_ms;
    return false;
}

bool pty_exit_code(Pty *pty, uint32_t *code)
{
    (void)pty;
    (void)code;
    return false;
}

void pty_close(Pty *pty)
{
    (void)pty;
}

#endif /* _WIN32 */
