/*
 * termica - reclaiming the launching console. See console.h.
 */

#include "console.h"

#include <stdio.h>

#ifdef _WIN32

#include <windows.h>

void console_reclaim(void)
{
    HANDLE standard = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = standard ? GetFileType(standard) : FILE_TYPE_UNKNOWN;

    /* Only reclaim when the process started with nothing. A launcher that
     * handed us the console keeps console output, and a launcher that
     * redirected stdout to a file or a pipe must not be overruled - the
     * self test and the sanitizer runs depend on that capture. */
    if (type != FILE_TYPE_UNKNOWN)
        return;

    /* A GUI-subsystem process started from Explorer has no parent console
     * either; then this just fails and the app stays silent. */
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;

    /* CONOUT$ is the console screen buffer, opened for writing. Unbuffered,
     * so a crash or a sanitizer report is not stuck in a CRT buffer that
     * nobody ever flushes. */
    if (freopen("CONOUT$", "w", stdout))
        setvbuf(stdout, NULL, _IONBF, 0);
    if (freopen("CONOUT$", "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

#else /* !_WIN32 */

/*
 * Elsewhere the process already has whatever standard streams it was
 * started with; there is nothing to reclaim.
 */

void console_reclaim(void)
{
}

#endif /* _WIN32 */
