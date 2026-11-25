/* POSIX integration checks for the asynchronous git status provider. */
#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "git.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <ftw.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void git_check(bool passed, const char *description, int *failures)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed)
        ++*failures;
}

static bool write_file(const char *path, const char *contents, mode_t mode)
{
    FILE *file = fopen(path, "w");
    bool ok;

    if (!file)
        return false;
    ok = fputs(contents, file) >= 0;
    if (fclose(file) != 0)
        ok = false;
    return ok && chmod(path, mode) == 0;
}

static bool init_repository(const char *directory)
{
    char *argv[] = {"git", "-C", (char *)directory, "-c",
                    "init.defaultBranch=termica-test", "init", "-q", NULL};
    pid_t pid, result;
    int status;

    if (posix_spawnp(&pid, "git", NULL, NULL, argv, environ) != 0)
        return false;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int remove_entry(const char *path, const struct stat *info, int type,
                        struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}

static bool wait_for_file(const char *path, Uint64 until)
{
    while (SDL_GetTicks() < until) {
        if (access(path, F_OK) == 0)
            return true;
        SDL_Delay(10);
    }
    return access(path, F_OK) == 0;
}
int git_posix_selftest(void)
{
    char root[] = "/tmp/termica git test.XXXXXX";
    char repo[512], dirty_file[512], bin[512], fake_git[512], missing[512];
    char parent_file[512], survivor_file[512];
    const char *path = getenv("PATH");
    char *saved_path = path ? strdup(path) : NULL;
    Git *first = NULL, *second = NULL;
    GitStatus status, first_result, second_result;
    bool first_ready = false, second_ready = false, have_root = false;
    bool changed_path = false;
    int failures = 0;
    Uint64 until;

    if (path && !saved_path) {
        git_check(false, "git test saves PATH", &failures);
        return failures;
    }
    if (!mkdtemp(root)) {
        git_check(false, "git test creates temporary directory", &failures);
        goto done;
    }
    have_root = true;
    snprintf(repo, sizeof repo, "%s/repo \xc3\xa9 $(false)", root);
    snprintf(dirty_file, sizeof dirty_file, "%s/repo \xc3\xa9 $(false)/untracked \xc3\xa9", root);
    snprintf(bin, sizeof bin, "%s/bin", root);
    snprintf(missing, sizeof missing, "%s/missing", root);
    snprintf(fake_git, sizeof fake_git, "%s/bin/git", root);
    snprintf(parent_file, sizeof parent_file, "%s/repo \xc3\xa9 $(false)/git-parent.pid", root);
    snprintf(survivor_file, sizeof survivor_file, "%s/repo \xc3\xa9 $(false)/group-survivor", root);
    if (mkdir(repo, 0700) != 0 || mkdir(bin, 0700) != 0 ||
        !init_repository(repo)) {
        git_check(false, "git initializes temporary repository", &failures);
        goto done;
    }

    git_check(git_status_for(repo, &status) && status.in_repo && status.clean &&
              status.branch[0],
              "git status handles UTF-8, spaces and shell metacharacters",
              &failures);
    if (!write_file(dirty_file, "untracked\n", 0600)) {
        git_check(false, "git test creates untracked file", &failures);
        goto done;
    }
    git_check(git_status_for(repo, &status) && status.in_repo && !status.clean &&
              status.dirty == 1,
              "git status counts an untracked UTF-8 filename", &failures);
    memset(&status, 0xff, sizeof status);
    git_check(!git_status_for(root, &status) && !status.in_repo && !status.branch[0] &&
              !git_status_for(missing, &status) &&
              !status.in_repo && !git_status_for(NULL, &status) &&
              !git_status_for(repo, NULL),
              "git rejects non-repositories, missing directories and invalid arguments",
              &failures);

    if (!git_init(&first) || !git_init(&second)) {
        git_check(false, "concurrent git workers initialize", &failures);
        goto done;
    }
    git_request(first, repo);
    git_request(second, root);
    until = SDL_GetTicks() + 5000;
    while ((!first_ready || !second_ready) && SDL_GetTicks() < until) {
        if (!first_ready)
            first_ready = git_poll(first, &first_result);
        if (!second_ready)
            second_ready = git_poll(second, &second_result);
        if (!first_ready || !second_ready)
            SDL_Delay(10);
    }
    git_check(first_ready && second_ready && first_result.in_repo &&
              first_result.dirty == 1 && !second_result.in_repo,
              "concurrent git workers deliver independent results", &failures);
    git_close(first); first = NULL;
    git_close(second); second = NULL;

    if (setenv("PATH", bin, 1) != 0) {
        git_check(false, "git test overrides PATH", &failures);
        goto done;
    }
    changed_path = true;
    memset(&status, 0xff, sizeof status);
    git_check(!git_status_for(repo, &status) && !status.in_repo && !status.branch[0],
              "missing git fails without stale status", &failures);

    if (!write_file(fake_git,
                    "#!/bin/sh\n"
                    "printf '# branch.head fake\\n'\n"
                    "exit 1\n", 0700)) {
        git_check(false, "git test creates failing executable", &failures);
        goto done;
    }
    git_check(!git_status_for(repo, &status) && !status.in_repo,
              "git rejects valid-looking output from a failed process", &failures);

    if (!write_file(fake_git,
                    "#!/bin/sh\n"
                    "while :; do\n"
                    "  printf '12345678901234567890123456789012\\n'\n"
                    "done\n", 0700)) {
        git_check(false, "git test creates noisy executable", &failures);
        goto done;
    }
    until = SDL_GetTicks();
    git_check(!git_status_for(repo, &status) && SDL_GetTicks() - until < 5000,
              "git bounds output and rejects truncation", &failures);

    if (!write_file(fake_git,
                    "#!/bin/sh\n"
                    "/bin/sh -c '/bin/sleep 4; : > \"$1/group-survivor\"' sh \"$2\" &\n"
                    "printf '%s\\n' \"$$\" > \"$2/git-parent.pid\"\n"
                    "wait\n", 0700)) {
        git_check(false, "git test creates hanging executable", &failures);
        goto done;
    }
    if (!git_init(&first)) {
        git_check(false, "hanging git worker initializes", &failures);
        goto done;
    }
    until = SDL_GetTicks();
    git_request(first, repo);
    if (wait_for_file(parent_file, until + 2000)) {
        FILE *file = fopen(parent_file, "r");
        long child = -1;
        Uint64 closing = SDL_GetTicks();
        if (file) {
            if (fscanf(file, "%ld", &child) != 1)
                child = -1;
            fclose(file);
        }
        git_close(first); first = NULL;
        git_check(SDL_GetTicks() - closing < 5000,
                  "git worker shutdown is bounded for a hung process", &failures);
        errno = 0;
        git_check(child > 0 && waitpid((pid_t)child, NULL, WNOHANG) == -1 &&
                  errno == ECHILD,
                  "hung git child is reaped", &failures);
        while (SDL_GetTicks() < until + 5000)
            SDL_Delay(20);
        git_check(access(survivor_file, F_OK) != 0,
                  "git timeout terminates its descendant process group", &failures);
    } else {
        git_check(false, "hanging git process starts", &failures);
    }

done:
    git_close(first);
    git_close(second);
    if (changed_path) {
        if (saved_path)
            (void)setenv("PATH", saved_path, 1);
        else
            (void)unsetenv("PATH");
    }
    free(saved_path);
    if (have_root && nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
        git_check(false, "git test removes temporary repository", &failures);
    return failures;
}
#endif /* !_WIN32 */
