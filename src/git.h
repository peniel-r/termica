/*
 * termica - the git status provider for the status bar.
 *
 * `git status --porcelain=v2 --branch` is run in the shell's reported
 * working directory on a background thread, so a slow or unresponsive
 * repository never stalls the UI thread. Results are polled from the main
 * loop; the worker keeps only the latest request and the latest result.
 *
 * The parser is exposed separately so the self test can check it without
 * git being installed.
 *
 * Strict C99.
 */

#ifndef TERMICA_GIT_H
#define TERMICA_GIT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct GitStatus {
    bool in_repo;
    char branch[128];
    int  ahead;
    int  behind;
    int  dirty;       /* changed tracked files + untracked files  */
    int  conflicted;  /* unmerged entries                          */
    bool clean;       /* no dirty or conflicted entries            */
} GitStatus;

typedef struct Git Git;

/* Start the worker thread. False on failure. */
bool git_init(Git **out);

/* Ask for a fresh status of `directory` (a UTF-8 path, NULL or empty to
 * clear the current result). The latest request wins. */
void git_request(Git *git, const char *directory);

/* True when a result arrived; copies it into `out` and clears it. */
bool git_poll(Git *git, GitStatus *out);

/* Stop the worker and free it. Safe on NULL. */
void git_close(Git *git);

/* Parse `git status --porcelain=v2 --branch` output. Exposed for the self
 * test. Returns false when the output is not from inside a repository. */
bool git_parse_status(const char *text, size_t length, GitStatus *out);

/* Run git synchronously in `directory`. Blocks; the worker thread uses it
 * and the self test can too. False when the directory is not a repository,
 * git cannot run or the run times out. POSIX also rejects oversized output. */
bool git_status_for(const char *directory, GitStatus *out);

#endif /* TERMICA_GIT_H */
