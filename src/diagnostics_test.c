#include "diagnostics.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check_diagnostics(bool ok, const char *description)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok)
        ++failures;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <wchar.h>

#define TEST_PATH_CAP 32768
#define TEST_EXCEPTION_CODE ((DWORD)0xe0012345u)

static int count_files(const wchar_t *dir, const wchar_t *pattern);

static LONG WINAPI resume_exception(EXCEPTION_POINTERS *pointers)
{
    (void)pointers;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* The wide command line is authoritative: the temporary directory can contain
 * characters that aren't representable in the active Windows code page. */
int diagnostics_test_dispatch(int argc, char **argv)
{
    int wide_argc;
    LPWSTR *wide_argv;
    const wchar_t *directory;
    bool crash, handled, lifecycle;
    LPTOP_LEVEL_EXCEPTION_FILTER filter;
    if (argc < 2)
        return -1;
    crash = strcmp(argv[1], "--diagnostics-test-crash") == 0;
    handled = strcmp(argv[1], "--diagnostics-test-handled") == 0;
    lifecycle = strcmp(argv[1], "--diagnostics-test-lifecycle") == 0;
    if (!crash && !handled && !lifecycle)
        return -1;
    wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv || wide_argc != 3) {
        if (wide_argv)
            LocalFree(wide_argv);
        return 2;
    }
    directory = wide_argv[2];
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    if (handled)
        SetUnhandledExceptionFilter(resume_exception);
    if (!diagnostics_init(false)) {
        LocalFree(wide_argv);
        return 3;
    }
    if (lifecycle) {
        static wchar_t invalid[TEST_PATH_CAP];
        int n = swprintf(invalid, TEST_PATH_CAP, L"%ls\\missing\\child", directory);
        if (count_files(directory, L"*.log") != 0 || n < 0 ||
            (size_t)n >= TEST_PATH_CAP || diagnostics_test_init_at(invalid)) {
            diagnostics_shutdown();
            LocalFree(wide_argv);
            return 7;
        }
    }
    if (!diagnostics_test_init_at(directory)) {
        LocalFree(wide_argv);
        return 3;
    }
    if (crash) {
        /* An actual second-chance software exception, not a direct call to
         * the filter or dump writer. No UI/interactive debugger is needed. */
        RaiseException(TEST_EXCEPTION_CODE, EXCEPTION_NONCONTINUABLE, 0, NULL);
        LocalFree(wide_argv);
        return 4;
    }
    if (handled) {
        RaiseException(TEST_EXCEPTION_CODE, 0, 0, NULL);
        diagnostics_shutdown();
        filter = SetUnhandledExceptionFilter(NULL);
        LocalFree(wide_argv);
        return filter == resume_exception ? 0 : 5;
    }
    diagnostics_log("startup.sdl_failed", 1234);
    diagnostics_log("secret with spaces\r\nforged", 99);
    diagnostics_shutdown();
    if (!diagnostics_test_init_at(directory)) {
        LocalFree(wide_argv);
        return 6;
    }
    diagnostics_log("restart", 42);
    diagnostics_shutdown();
    LocalFree(wide_argv);
    return 0;
}

static bool child_process(const wchar_t *mode, const wchar_t *directory,
                          DWORD *exit_code)
{
    static wchar_t exe[TEST_PATH_CAP];
    static wchar_t command[TEST_PATH_CAP * 2];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD n, wait;
    int length;
    n = GetModuleFileNameW(NULL, exe, TEST_PATH_CAP);
    if (!n || n >= TEST_PATH_CAP)
        return false;
    length = swprintf(command, sizeof command / sizeof command[0],
                      L"\"%ls\" %ls \"%ls\"", exe, mode, directory);
    if (length < 0 || (size_t)length >= sizeof command / sizeof command[0])
        return false;
    memset(&startup, 0, sizeof startup);
    memset(&process, 0, sizeof process);
    startup.cb = sizeof startup;
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process))
        return false;
    wait = WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    n = GetExitCodeProcess(process.hProcess, exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && n != 0;
}

static bool file_path(wchar_t *out, size_t cap, const wchar_t *dir,
                      const wchar_t *name)
{
    int n = swprintf(out, cap, L"%ls\\%ls", dir, name);
    return n >= 0 && (size_t)n < cap;
}

static int count_files(const wchar_t *dir, const wchar_t *pattern)
{
    static wchar_t path[TEST_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int count = 0;
    if (!file_path(path, TEST_PATH_CAP, dir, pattern))
        return -1;
    find = FindFirstFileW(path, &data);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

/* Read the generated log files and reject arbitrary text injection. */
static bool check_logs(const wchar_t *dir)
{
    static wchar_t path[TEST_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    bool failure_event = false, restarted = false, ok = true;
    int logs = 0;
    if (!file_path(path, TEST_PATH_CAP, dir, L"*.log"))
        return false;
    find = FindFirstFileW(path, &data);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    do {
        HANDLE file;
        DWORD read_count;
        char contents[4096];
        if (!file_path(path, TEST_PATH_CAP, dir, data.cFileName)) {
            ok = false;
            break;
        }
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        if (!ReadFile(file, contents, sizeof contents - 1, &read_count, NULL)) {
            CloseHandle(file);
            ok = false;
            break;
        }
        CloseHandle(file);
        contents[read_count] = '\0';
        ++logs;
        if (!strstr(contents, " startup code=0\r\n") ||
            !strstr(contents, " shutdown code=0\r\n") ||
            strstr(contents, "secret") || strstr(contents, "forged"))
            ok = false;
        if (strstr(contents, " startup.sdl_failed code=1234\r\n"))
            failure_event = true;
        if (strstr(contents, " restart code=42\r\n"))
            restarted = true;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok && logs == 2 && failure_event && restarted;
}

static bool read_at(HANDLE file, DWORD offset, void *buffer, DWORD size)
{
    LARGE_INTEGER position;
    DWORD count;
    position.QuadPart = offset;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
           ReadFile(file, buffer, size, &count, NULL) && count == size;
}

/* Verify both the real minidump signature and the exception stream, not just
 * that some file was created with a .dmp extension. */
static bool check_dump(const wchar_t *dir)
{
    static wchar_t path[TEST_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find, file;
    MINIDUMP_HEADER header;
    MINIDUMP_DIRECTORY entry;
    MINIDUMP_EXCEPTION_STREAM exception;
    DWORD i;
    bool found = false;
    if (!file_path(path, TEST_PATH_CAP, dir, L"*.dmp"))
        return false;
    find = FindFirstFileW(path, &data);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    FindClose(find);
    if (!file_path(path, TEST_PATH_CAP, dir, data.cFileName))
        return false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    if (read_at(file, 0, &header, sizeof header) &&
        header.Signature == 0x504d444du && header.NumberOfStreams < 128) {
        for (i = 0; i < header.NumberOfStreams; ++i) {
            DWORD offset = header.StreamDirectoryRva + i * (DWORD)sizeof entry;
            if (!read_at(file, offset, &entry, sizeof entry))
                break;
            if (entry.StreamType == ExceptionStream &&
                entry.Location.DataSize >= sizeof exception &&
                read_at(file, entry.Location.Rva, &exception, sizeof exception) &&
                exception.ExceptionRecord.ExceptionCode == TEST_EXCEPTION_CODE) {
                found = true;
                break;
            }
        }
    }
    CloseHandle(file);
    return found;
}

static void remove_test_directory(const wchar_t *dir)
{
    static wchar_t path[TEST_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!file_path(path, TEST_PATH_CAP, dir, L"*"))
        return;
    find = FindFirstFileW(path, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                file_path(path, TEST_PATH_CAP, dir, data.cFileName))
                DeleteFileW(path);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

static void test_windows(void)
{
    static wchar_t temp[TEST_PATH_CAP];
    static wchar_t directory[TEST_PATH_CAP];
    DWORD length, exit_code = 0;
    bool launched, created = false;
    unsigned int attempt;
    length = GetTempPathW(TEST_PATH_CAP, temp);
    if (length && length < TEST_PATH_CAP) {
        for (attempt = 0; attempt < 100; ++attempt) {
            int n = swprintf(directory, TEST_PATH_CAP,
                             L"%lstermica-diag-%lu-%lu-%u-\x6d4b\x8bd5",
                             temp, (unsigned long)GetCurrentProcessId(),
                             (unsigned long)GetTickCount(), attempt);
            if (n < 0 || (size_t)n >= TEST_PATH_CAP)
                break;
            if (CreateDirectoryW(directory, NULL)) {
                created = true;
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                break;
        }
    }
    check_diagnostics(created, "diagnostics create a Unicode test directory");
    if (!created)
        return;
    launched = child_process(L"--diagnostics-test-lifecycle", directory, &exit_code);
    check_diagnostics(launched && exit_code == 0,
                      "diagnostics opt-out and repeated lifecycle complete");
    check_diagnostics(check_logs(directory),
                      "logs are durable, unique and reject unsafe event text");
    check_diagnostics(count_files(directory, L"*.dmp") == 0,
                      "normal lifecycle produces no minidump");
    exit_code = 0;
    launched = child_process(L"--diagnostics-test-handled", directory, &exit_code);
    check_diagnostics(launched && exit_code == 0 &&
                      count_files(directory, L"*.dmp") == 0,
                      "previous filter can resume and is restored without a dump");
    exit_code = 0;
    launched = child_process(L"--diagnostics-test-crash", directory, &exit_code);
    check_diagnostics(launched && exit_code == TEST_EXCEPTION_CODE,
                      "unhandled exception retains the original process outcome");
    check_diagnostics(count_files(directory, L"*.dmp") == 1 && check_dump(directory),
                      "unhandled exception produces a valid exception minidump");
    check_diagnostics(count_files(directory, L"*.tmp") == 0,
                      "completed minidump is published without temporary files");
    remove_test_directory(directory);
}
#else
int diagnostics_test_dispatch(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return -1;
}
#endif

int diagnostics_selftest(void)
{
    const char *version = diagnostics_version();
    const char *ghostty = ghostty_version();
    failures = 0;
    check_diagnostics(strncmp(version, "termica ", 8) == 0 &&
                      strstr(version, " (revision ") &&
                      strstr(version, "; libghostty-vt ") &&
                      ghostty && strstr(version, ghostty),
                      "version identifies project, revision and loaded libghostty");
#ifdef _WIN32
    test_windows();
#else
    check_diagnostics(diagnostics_init(false), "disabled diagnostics are a no-op");
    diagnostics_shutdown();
#endif
    return failures;
}

/* Also usable as a small standalone executable linked with diagnostics.c,
 * libghostty-vt, dbghelp, shell32 and ole32, without SDL or the full app. */
#ifdef DIAGNOSTICS_TEST_MAIN
int main(int argc, char **argv)
{
    int result = diagnostics_test_dispatch(argc, argv);
    if (result >= 0)
        return result;
    return diagnostics_selftest() ? 1 : 0;
}
#endif
