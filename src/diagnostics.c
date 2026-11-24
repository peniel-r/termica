#include "diagnostics.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <string.h>

#ifdef TERMICA_GENERATED_REVISION
#include "termica_build_revision.h"
#endif

#ifndef TERMICA_VERSION
#define TERMICA_VERSION "unknown"
#endif
#ifndef TERMICA_BUILD_REVISION
#define TERMICA_BUILD_REVISION "unknown"
#endif

const char *diagnostics_version(void)
{
    static char version[512];
    const char *ghostty = ghostty_version();
    if (!ghostty || !*ghostty)
        ghostty = "unknown";
    (void)snprintf(version, sizeof version,
                   "termica %s (revision %s; libghostty-vt %s)",
                   TERMICA_VERSION, TERMICA_BUILD_REVISION, ghostty);
    return version;
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <wchar.h>

/* Use the known folder, not the environment, and extended-length paths. */
#define DIAG_PATH_CAP 32768
static const GUID local_appdata_id = {
    0xf1b32785, 0x6fba, 0x4fcf,
    {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}
};

static wchar_t log_directory[DIAG_PATH_CAP];
static HANDLE log_file = INVALID_HANDLE_VALUE;
static HANDLE request_event;
static HANDLE done_event;
static HANDLE stop_event;
static SRWLOCK log_lock = SRWLOCK_INIT;
static HANDLE worker_thread;
static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
static volatile LONG active;
static volatile LONG exception_guard;
static EXCEPTION_POINTERS *crash_pointers;
static DWORD crash_thread_id;

static void debug_error(const char *operation, DWORD error)
{
    char line[160];
    (void)snprintf(line, sizeof line, "termica diagnostics: %s failed (%lu)\n",
                   operation, (unsigned long)error);
    OutputDebugStringA(line);
}

static bool append_path(wchar_t *path, size_t capacity, const wchar_t *part)
{
    size_t length = wcslen(path);
    size_t extra = wcslen(part);
    bool separator = length != 0 && path[length - 1] != L'\\';
    if (length + (separator ? 1u : 0u) + extra >= capacity) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    if (separator)
        path[length++] = L'\\';
    memcpy(path + length, part, (extra + 1) * sizeof *path);
    return true;
}

static bool extended_path(wchar_t *out, size_t capacity, const wchar_t *path)
{
    const wchar_t *prefix;
    size_t prefix_len, path_len;
    if (!path || !*path) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
        prefix = L"";
    } else if (path[0] == L'\\' && path[1] == L'\\') {
        prefix = L"\\\\?\\UNC\\";
        path += 2;
    } else if (path[0] && path[1] == L':' && path[2] == L'\\') {
        prefix = L"\\\\?\\";
    } else {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    prefix_len = wcslen(prefix);
    path_len = wcslen(path);
    if (prefix_len + path_len >= capacity) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    memcpy(out, prefix, prefix_len * sizeof *out);
    memcpy(out + prefix_len, path, (path_len + 1) * sizeof *out);
    return true;
}

static bool ensure_directory(const wchar_t *path)
{
    DWORD attributes;
    if (CreateDirectoryW(path, NULL))
        return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetLastError(ERROR_DIRECTORY);
        return false;
    }
    return true;
}

static bool prepare_directory(void)
{
    PWSTR local = NULL;
    HRESULT result = SHGetKnownFolderPath(&local_appdata_id, 0, NULL, &local);
    bool ok;
    if (FAILED(result)) {
        SetLastError(HRESULT_CODE(result));
        return false;
    }
    ok = extended_path(log_directory, DIAG_PATH_CAP, local);
    CoTaskMemFree(local);
    if (!ok || !append_path(log_directory, DIAG_PATH_CAP, L"termica") ||
        !ensure_directory(log_directory) ||
        !append_path(log_directory, DIAG_PATH_CAP, L"logs") ||
        !ensure_directory(log_directory))
        return false;
    return true;
}

/* A timestamp, PID and collision suffix plus CREATE_NEW make concurrent starts
 * independent. The directory is established before installing the filter. */
static bool artifact_path(wchar_t *out, size_t capacity, const wchar_t *kind,
                          const wchar_t *extension, unsigned int attempt)
{
    SYSTEMTIME now;
    wchar_t name[128];
    int n;
    GetSystemTime(&now);
    n = swprintf(name, sizeof name / sizeof name[0],
                 L"%ls-%04u%02u%02uT%02u%02u%02u%03uZ-%lu-%u.%ls",
                 kind, (unsigned)now.wYear, (unsigned)now.wMonth,
                 (unsigned)now.wDay, (unsigned)now.wHour,
                 (unsigned)now.wMinute, (unsigned)now.wSecond,
                 (unsigned)now.wMilliseconds, (unsigned long)GetCurrentProcessId(),
                 attempt, extension);
    if (n < 0 || (size_t)n >= sizeof name / sizeof name[0]) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    if (wcslen(log_directory) >= capacity) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    wcscpy(out, log_directory);
    return append_path(out, capacity, name);
}

static bool open_log(void)
{
    wchar_t path[DIAG_PATH_CAP];
    unsigned int attempt;
    for (attempt = 0; attempt < 100; ++attempt) {
        DWORD error;
        if (!artifact_path(path, DIAG_PATH_CAP, L"termica", L"log", attempt))
            return false;
        log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                               NULL);
        if (log_file != INVALID_HANDLE_VALUE)
            return true;
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            return false;
    }
    SetLastError(ERROR_FILE_EXISTS);
    return false;
}

static bool valid_event(const char *event)
{
    size_t i;
    if (!event || !*event)
        return false;
    for (i = 0; event[i]; ++i) {
        unsigned char c = (unsigned char)event[i];
        if (i >= 64 || !((c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') ||
                         c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static void log_record(const char *event, unsigned long code, bool crash)
{
    SYSTEMTIME now;
    char line[192];
    int length;
    DWORD written;
    if (log_file == INVALID_HANDLE_VALUE || !valid_event(event))
        return;
    if (crash) {
        if (!TryAcquireSRWLockExclusive(&log_lock))
            return;
    } else {
        AcquireSRWLockExclusive(&log_lock);
    }
    GetSystemTime(&now);
    length = snprintf(line, sizeof line,
                      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s code=%lu\r\n",
                      (unsigned)now.wYear, (unsigned)now.wMonth,
                      (unsigned)now.wDay, (unsigned)now.wHour,
                      (unsigned)now.wMinute, (unsigned)now.wSecond,
                      (unsigned)now.wMilliseconds, event, code);
    if (length > 0 && (size_t)length < sizeof line) {
        if (!WriteFile(log_file, line, (DWORD)length, &written, NULL) ||
            written != (DWORD)length)
            debug_error("log write", GetLastError());
        else if (!FlushFileBuffers(log_file))
            debug_error("log flush", GetLastError());
    }
    ReleaseSRWLockExclusive(&log_lock);
}

void diagnostics_log(const char *event, unsigned long code)
{
    log_record(event, code, false);
}

/* Never expose a partial dump as .dmp: create a unique temporary file, flush,
 * then rename without replacing any existing dump. MiniDumpNormal deliberately
 * excludes full process memory, but stacks can still contain private data. */
static DWORD write_dump(EXCEPTION_POINTERS *pointers, DWORD thread_id)
{
    wchar_t final_path[DIAG_PATH_CAP];
    wchar_t temporary_path[DIAG_PATH_CAP];
    MINIDUMP_EXCEPTION_INFORMATION exception;
    HANDLE file;
    unsigned int attempt;
    DWORD error;
    for (attempt = 0; attempt < 100; ++attempt) {
        if (!artifact_path(final_path, DIAG_PATH_CAP, L"crash", L"dmp", attempt))
            return GetLastError();
        if (wcslen(final_path) + 4 >= DIAG_PATH_CAP)
            return ERROR_FILENAME_EXCED_RANGE;
        wcscpy(temporary_path, final_path);
        wcscat(temporary_path, L".tmp");
        file = CreateFileW(temporary_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            return error;
        }
        exception.ThreadId = thread_id;
        exception.ExceptionPointers = pointers;
        exception.ClientPointers = FALSE;
        if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                               MiniDumpNormal, &exception, NULL, NULL)) {
            error = GetLastError();
            CloseHandle(file);
            DeleteFileW(temporary_path);
            return error ? error : ERROR_WRITE_FAULT;
        }
        if (!FlushFileBuffers(file)) {
            error = GetLastError();
            CloseHandle(file);
            DeleteFileW(temporary_path);
            return error;
        }
        if (!CloseHandle(file)) {
            error = GetLastError();
            DeleteFileW(temporary_path);
            return error;
        }
        if (MoveFileExW(temporary_path, final_path, MOVEFILE_WRITE_THROUGH))
            return ERROR_SUCCESS;
        error = GetLastError();
        DeleteFileW(temporary_path);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            return error;
    }
    return ERROR_FILE_EXISTS;
}
static DWORD WINAPI dump_worker(void *unused)
{
    HANDLE events[2];
    (void)unused;
    events[0] = stop_event;
    events[1] = request_event;
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0 + 1)
            break;
        if (crash_pointers && crash_pointers->ExceptionRecord)
            log_record("crash.exception",
                       (unsigned long)crash_pointers->ExceptionRecord->ExceptionCode, true);
        result = write_dump(crash_pointers, crash_thread_id);
        log_record(result == ERROR_SUCCESS ? "crash.dump_written" :
                   "crash.dump_failed", (unsigned long)result, true);
        if (result != ERROR_SUCCESS)
            debug_error("minidump", result);
        SetEvent(done_event);
    }
    return 0;
}

/* This is a top-level (second-chance) filter, not a vectored/first-chance
 * handler. Respect any prior handler that resumes execution, and otherwise
 * preserve its return value. Use a separate stack for overflow exceptions. */
static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *pointers)
{
    LONG disposition = EXCEPTION_CONTINUE_SEARCH;
    if (previous_filter && previous_filter != unhandled_filter)
        disposition = previous_filter(pointers);
    if (disposition == EXCEPTION_CONTINUE_EXECUTION)
        return disposition;
    if (!InterlockedCompareExchange(&active, 1, 1) ||
        InterlockedCompareExchange(&exception_guard, 1, 0) != 0)
        return disposition;
    if (InterlockedCompareExchange(&active, 1, 1)) {
        crash_pointers = pointers;
        crash_thread_id = GetCurrentThreadId();
        ResetEvent(done_event);
        if (SetEvent(request_event))
            (void)WaitForSingleObject(done_event, 30000);
    }
    InterlockedExchange(&exception_guard, 0);
    return disposition;
}

static void close_resources(void)
{
    if (worker_thread) {
        SetEvent(stop_event);
        (void)WaitForSingleObject(worker_thread, INFINITE);
        CloseHandle(worker_thread);
        worker_thread = NULL;
    }
    if (request_event) { CloseHandle(request_event); request_event = NULL; }
    if (done_event) { CloseHandle(done_event); done_event = NULL; }
    if (stop_event) { CloseHandle(stop_event); stop_event = NULL; }
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
}

static bool start_diagnostics(void)
{
    DWORD error;
    if (!open_log())
        goto fail;
    request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!request_event)
        goto fail;
    done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!done_event)
        goto fail;
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_event)
        goto fail;
    worker_thread = CreateThread(NULL, 0, dump_worker, NULL, 0, NULL);
    if (!worker_thread)
        goto fail;
    InterlockedExchange(&active, 1);
    previous_filter = SetUnhandledExceptionFilter(unhandled_filter);
    diagnostics_log("startup", 0);
    return true;
fail:
    error = GetLastError();
    debug_error("initialization", error);
    close_resources();
    SetLastError(error);
    return false;
}

bool diagnostics_init(bool enabled)
{
    DWORD error;
    if (!enabled)
        return true;
    if (InterlockedCompareExchange(&active, 1, 1))
        return true;
    if (!prepare_directory()) {
        error = GetLastError();
        debug_error("log directory", error);
        SetLastError(error);
        return false;
    }
    return start_diagnostics();
}

bool diagnostics_test_init_at(const wchar_t *directory)
{
    if (InterlockedCompareExchange(&active, 1, 1)) {
        SetLastError(ERROR_BUSY);
        return false;
    }
    if (!extended_path(log_directory, DIAG_PATH_CAP, directory) ||
        !ensure_directory(log_directory))
        return false;
    return start_diagnostics();
}

void diagnostics_shutdown(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER current;
    if (!InterlockedExchange(&active, 0))
        return;
    /* Do not replace a filter installed by somebody else since startup. */
    current = SetUnhandledExceptionFilter(previous_filter);
    if (current != unhandled_filter)
        SetUnhandledExceptionFilter(current);
    while (InterlockedCompareExchange(&exception_guard, 1, 0) != 0)
        Sleep(1);
    diagnostics_log("shutdown", 0);
    close_resources();
    InterlockedExchange(&exception_guard, 0);
}

#else

bool diagnostics_init(bool enabled)
{
    (void)enabled;
    return true;
}
void diagnostics_log(const char *event, unsigned long code)
{
    (void)event;
    (void)code;
}
void diagnostics_shutdown(void) { }

#endif
