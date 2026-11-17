/*
 * termica - the git status provider. See git.h.
 */

#include "git.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#include <windows.h>
#endif

/* Longest `git status --porcelain=v2 --branch` output we read. */
#define GIT_OUTPUT_BYTES 65536

/* Hard cap on one git run, so a wedged subprocess can never block shutdown
 * (the worker thread's result is polled and the main loop waits on it in
 * git_close). */
#define GIT_TIMEOUT_MS 3000

struct Git {
    SDL_Thread    *thread;
    SDL_Mutex     *lock;
    SDL_Condition *wake;
    bool           closing;
    bool           has_request;
    char           directory[4096];
    bool           result_ready;
    GitStatus      result;
};

/* ------------------------------------------------------------------------- */
/* Parser                                                                     */
/* ------------------------------------------------------------------------- */

bool git_parse_status(const char *text, size_t length, GitStatus *out)
{
    const char *p = text;
    const char *end = text + length;
    bool saw_head = false;

    if (!out)
        return false;
    memset(out, 0, sizeof *out);

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        const char *line = p;
        size_t line_len = (size_t)(line_end - p);

        while (line_len > 0 &&
               (line[line_len - 1] == '\r' || line[line_len - 1] == '\n'))
            line_len--;

        if (line_len >= 14 && memcmp(line, "# branch.head ", 14) == 0) {
            size_t name_len = line_len - 14;

            if (name_len >= sizeof out->branch)
                name_len = sizeof out->branch - 1;
            memcpy(out->branch, line + 14, name_len);
            out->branch[name_len] = '\0';
            saw_head = true;
        } else if (line_len >= 12 && memcmp(line, "# branch.ab ", 12) == 0) {
            const char *plus = strchr(line + 12, '+');
            const char *minus = strchr(line + 12, '-');

            if (plus)
                out->ahead = (int)strtol(plus + 1, NULL, 10);
            if (minus)
                out->behind = (int)strtol(minus + 1, NULL, 10);
            if (out->ahead < 0)
                out->ahead = 0;
            if (out->behind < 0)
                out->behind = 0;
        } else if (line_len >= 2 && line[0] == 'u' && line[1] == ' ') {
            out->conflicted++;
        } else if (line_len >= 2 && line[1] == ' ' &&
                   (line[0] == '1' || line[0] == '2')) {
            out->dirty++;
        } else if (line_len >= 2 && line[0] == '?' && line[1] == ' ') {
            out->dirty++;
        }

        if (!nl)
            break;
        p = nl + 1;
    }

    out->in_repo = saw_head;
    out->clean = out->dirty == 0 && out->conflicted == 0;
    return saw_head;
}

/* ------------------------------------------------------------------------- */
/* Running git                                                                */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32

static wchar_t *to_wide(const char *utf8)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *wide;

    if (length <= 0)
        return NULL;
    wide = (wchar_t *)malloc((size_t)length * sizeof *wide);
    if (!wide)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, length);
    return wide;
}

bool git_status_for(const char *directory, GitStatus *out)
{
    wchar_t exe[1024];
    wchar_t command[1280];
    wchar_t *wide_dir = NULL;
    SECURITY_ATTRIBUTES inheritable;
    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 0;
    DWORD length;
    char buffer[GIT_OUTPUT_BYTES];
    size_t used = 0;
    bool ok = false;

    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    if (!directory || !directory[0])
        return false;

    length = SearchPathW(NULL, L"git.exe", NULL,
                         (DWORD)(sizeof exe / sizeof exe[0]), exe, NULL);
    if (length == 0 || length >= sizeof exe / sizeof exe[0])
        return false;

    wide_dir = to_wide(directory);
    if (!wide_dir)
        return false;

    ZeroMemory(&inheritable, sizeof inheritable);
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) {
        free(wide_dir);
        return false;
    }
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        free(wide_dir);
        return false;
    }

    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;

    /* The working directory is the process current directory, so the path
     * never has to be quoted into the command line. */
    swprintf(command, sizeof command / sizeof command[0],
             L"\"%ls\" status --porcelain=v2 --branch", exe);

    ZeroMemory(&process, sizeof process);
    if (CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                       NULL, wide_dir, &startup, &process)) {
        Uint64 deadline;

        CloseHandle(process.hThread);

        /* Drop our copy of the write end now. A pipe reaches EOF only when
         * every handle to its write side is closed; keeping ours open while
         * we read is what wedged ReadFile forever. */
        CloseHandle(write_end);
        write_end = NULL;

        /* Read without blocking: poll what the pipe holds, stop once the
         * child is gone, and give up at the deadline so a stalled git can
         * not hang the worker (and therefore shutdown). */
        deadline = SDL_GetTicks() + GIT_TIMEOUT_MS;
        for (;;) {
            DWORD available = 0;
            DWORD read = 0;

            if (used + 1 >= sizeof buffer)
                break;
            if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL))
                break;   /* child gone and drained: EOF */
            if (available == 0) {
                if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
                    break;
                if ((Sint64)(deadline - SDL_GetTicks()) <= 0)
                    break;
                SDL_Delay(10);
                continue;
            }
            if (!ReadFile(read_end, buffer + used,
                          (DWORD)(sizeof buffer - 1 - used), &read, NULL) ||
                read == 0)
                break;
            used += read;
        }

        if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        } else if (GetExitCodeProcess(process.hProcess, &exit_code) &&
                   exit_code == 0) {
            buffer[used] = '\0';
            ok = git_parse_status(buffer, used, out);
        }
        CloseHandle(process.hProcess);
    }

    CloseHandle(read_end);
    if (write_end)
        CloseHandle(write_end);
    free(wide_dir);
    return ok;
}

#else /* !_WIN32 */

bool git_status_for(const char *directory, GitStatus *out)
{
    (void)directory;
    if (out)
        memset(out, 0, sizeof *out);
    return false;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------------- */
/* Worker                                                                     */
/* ------------------------------------------------------------------------- */

static int SDLCALL git_thread(void *userdata)
{
    Git *git = (Git *)userdata;

    for (;;) {
        char directory[4096];
        GitStatus status;
        bool run = false;

        SDL_LockMutex(git->lock);
        while (!git->has_request && !git->closing)
            SDL_WaitCondition(git->wake, git->lock);
        if (git->closing) {
            SDL_UnlockMutex(git->lock);
            break;
        }
        memcpy(directory, git->directory, sizeof directory);
        git->has_request = false;
        SDL_UnlockMutex(git->lock);

        memset(&status, 0, sizeof status);
        run = git_status_for(directory, &status);

        SDL_LockMutex(git->lock);
        git->result = status;
        git->result.in_repo = run && status.in_repo;
        git->result_ready = true;
        SDL_UnlockMutex(git->lock);
    }
    return 0;
}

bool git_init(Git **out)
{
    Git *git;

    if (!out)
        return false;
    *out = NULL;
    git = (Git *)calloc(1, sizeof *git);
    if (!git)
        return false;

    git->lock = SDL_CreateMutex();
    if (!git->lock)
        goto fail;
    git->wake = SDL_CreateCondition();
    if (!git->wake)
        goto fail;
    git->thread = SDL_CreateThread(git_thread, "termica-git", git);
    if (!git->thread)
        goto fail;

    *out = git;
    return true;

fail:
    if (git->wake)
        SDL_DestroyCondition(git->wake);
    if (git->lock)
        SDL_DestroyMutex(git->lock);
    free(git);
    return false;
}

void git_request(Git *git, const char *directory)
{
    if (!git)
        return;
    SDL_LockMutex(git->lock);
    if (!directory || !directory[0])
        git->directory[0] = '\0';
    else
        snprintf(git->directory, sizeof git->directory, "%s", directory);
    git->has_request = true;
    SDL_SignalCondition(git->wake);
    SDL_UnlockMutex(git->lock);
}

bool git_poll(Git *git, GitStatus *out)
{
    bool ready;

    if (!git)
        return false;
    SDL_LockMutex(git->lock);
    ready = git->result_ready;
    if (ready) {
        if (out)
            *out = git->result;
        git->result_ready = false;
    }
    SDL_UnlockMutex(git->lock);
    return ready;
}

void git_close(Git *git)
{
    if (!git)
        return;
    SDL_LockMutex(git->lock);
    git->closing = true;
    SDL_SignalCondition(git->wake);
    SDL_UnlockMutex(git->lock);

    if (git->thread)
        SDL_WaitThread(git->thread, NULL);
    SDL_DestroyCondition(git->wake);
    SDL_DestroyMutex(git->lock);
    free(git);
}
