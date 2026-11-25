/* Consumer-level checks against real POSIX pseudoterminals. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TEST_INPUT_LIMIT (16u * 1024u * 1024u)

static int failures;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) ++failures;
}

static uint64_t ticks_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return 0;
    return (uint64_t)time.tv_sec * 1000u + (uint64_t)time.tv_nsec / 1000000u;
}

static void pause_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static bool collect(Pty *pty, char *out, size_t capacity, uint32_t *exit_code,
                    unsigned timeout_ms)
{
    uint64_t deadline = ticks_ms() + timeout_ms;
    size_t used = 0;
    bool overflow = false;
    if (capacity) out[0] = '\0';
    while (ticks_ms() < deadline) {
        char buffer[8192];
        size_t got;
        while ((got = pty_read(pty, buffer, sizeof buffer)) != 0) {
            size_t room = capacity ? capacity - 1 - used : 0;
            size_t copy = got < room ? got : room;
            if (copy) { memcpy(out + used, buffer, copy); used += copy; }
            if (copy != got) overflow = true;
        }
        if (capacity) out[used] = '\0';
        if (pty_output_drained(pty) && pty_exit_code(pty, exit_code))
            return !overflow;
        if (!pty_wait(pty, 20)) continue;
        /* Once the watcher reports exit, pty_wait is immediately ready while
         * the reader may still be catching up. Do not busy-spin. */
        if (pty_exit_code(pty, NULL)) pause_ms(1);
    }
    return false;
}

static bool wait_for_text(Pty *pty, const char *needle, char *out, size_t cap)
{
    uint64_t deadline = ticks_ms() + 5000;
    size_t used = 0;
    out[0] = '\0';
    while (ticks_ms() < deadline) {
        size_t got = pty_read(pty, out + used, cap - 1 - used);
        used += got;
        out[used] = '\0';
        if (strstr(out, needle)) return true;
        if (used == cap - 1) return false;
        (void)pty_wait(pty, 20);
    }
    return false;
}

static void test_spawn_and_environment(const char *directory)
{
    char error[256], output[8192], actual_cwd[4096], script[1024];
    char *resolved = realpath(directory, actual_cwd);
    int fd = open("/dev/null", O_RDONLY);
    int sentinel = fd >= 0 ? fcntl(fd, F_DUPFD, 200) : -1;
    const char *argv[] = {
        "sh", "-c", script, "termica-test", "space \"quote\" \\ slash",
        "", "unicode \\; $() \xe9\x9b\xaa"
    };
    PtyEnvEntry env[] = {
        { "PATH", "/bin:/usr/bin" }, { "TERM", "xterm-custom" },
        { "Term", "case-sensitive" }, { "TERMICA_TEST", "value with spaces" }
    };
    Pty *pty;
    uint32_t code = 0;
    bool collected;
    snprintf(script, sizeof script,
             "printf 'ARG1:<%%s>\nARG2:<%%s>\nARG3:<%%s>\nENV:<%%s>|<%%s>|<%%s>|<%%s>\n' "
             "\"$1\" \"$2\" \"$3\" \"$TERM\" \"$Term\" \"$TERMICA_TEST\" \"$TERMICA\"; "
             "pwd; if [ -e /dev/fd/%d ]; then printf LEAK; else printf CLOSED; fi; exit 23",
             sentinel);
    pty = pty_spawn_posix(argv, 7, directory, env, 4, 80, 24,
                          NULL, NULL, error, sizeof error);
    if (sentinel >= 0) close(sentinel);
    if (fd >= 0) close(fd);
    check(pty != NULL, "POSIX spawn uses the overridden PATH");
    if (!pty) return;
    collected = collect(pty, output, sizeof output, &code, 5000);
    check(collected && code == 23, "POSIX direct-child exit status");
    check(strstr(output, "ARG1:<space \"quote\" \\ slash>") &&
          strstr(output, "ARG2:<>") &&
          strstr(output, "ARG3:<unicode \\; $() \xe9\x9b\xaa>"),
          "POSIX argv preserves empty, quoted and Unicode arguments without evaluation");
    check(strstr(output, "ENV:<xterm-custom>|<case-sensitive>|<value with spaces>|<1>"),
          "POSIX environment overlays are case-sensitive and retain defaults");
    check(resolved && strstr(output, actual_cwd), "POSIX child uses the UTF-8 working directory");
    check(sentinel >= 0 && strstr(output, "CLOSED") && !strstr(output, "LEAK"),
          "POSIX child closes unrelated inherited descriptors");
    pty_close(pty);
}

static void test_errors(void)
{
    char error[256];
    const char *missing[] = { "/termica-no-such-program" };
    const char *shell[] = { "/bin/sh", "-c", "exit 0" };
    PtyEnvEntry invalid[] = { { "bad=key", "value" } };
    Pty *pty = pty_spawn_posix(missing, 1, NULL, NULL, 0, 80, 24,
                               NULL, NULL, error, sizeof error);
    check(!pty && strstr(error, "exec"), "POSIX exec failure is reported synchronously");
    pty_close(pty);
    pty = pty_spawn_posix(shell, 3, "/termica-no-such-directory", NULL, 0,
                          80, 24, NULL, NULL, error, sizeof error);
    check(!pty && strstr(error, "working directory"), "POSIX cwd failure is reported synchronously");
    pty_close(pty);
    pty = pty_spawn_posix(shell, 3, NULL, invalid, 1, 80, 24,
                          NULL, NULL, error, sizeof error);
    check(!pty && strstr(error, "environment"), "POSIX invalid environment key is rejected");
    pty_close(pty);
    {
        struct sigaction original, ignored;
        memset(&ignored, 0, sizeof ignored);
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        if (sigaction(SIGCHLD, &ignored, &original) == 0) {
            pty = pty_spawn_posix(shell, 3, NULL, NULL, 0, 80, 24,
                                  NULL, NULL, error, sizeof error);
            check(!pty && strstr(error, "SIGCHLD"),
                  "POSIX spawn rejects automatic child reaping");
            pty_close(pty);
            (void)sigaction(SIGCHLD, &original, NULL);
        } else {
            check(false, "set up POSIX child-reaping policy test");
        }
    }
    {
        char *original = getenv("SHELL") ? strdup(getenv("SHELL")) : NULL;
        char *selected;
        setenv("SHELL", "/termica-no-such-shell", 1);
        selected = pty_default_shell_posix();
        check(selected && strcmp(selected, "/bin/sh") == 0,
              "POSIX automatic shell falls back when SHELL is unusable");
        free(selected);
        setenv("SHELL", "/bin/sh", 1);
        selected = pty_default_shell_posix();
        check(selected && strcmp(selected, "/bin/sh") == 0,
              "POSIX automatic shell honors a usable SHELL");
        free(selected);
        if (original) { setenv("SHELL", original, 1); free(original); }
        else unsetenv("SHELL");
    }
}

typedef struct TestWake {
    pthread_mutex_t lock;
    unsigned count;
} TestWake;

static void record_wake(void *userdata)
{
    TestWake *wake = (TestWake *)userdata;
    pthread_mutex_lock(&wake->lock);
    wake->count++;
    pthread_mutex_unlock(&wake->lock);
}

static void test_io_and_concurrency(void)
{
    char error[256], first[4096], output[4096], second[1024];
    const char *interactive[] = {
        "/bin/sh", "-c",
        "stty -echo; printf READY; IFS= read -r line; "
        "printf '|FIFO:<%s>|SIZE:' \"$line\"; stty size; exit 7"
    };
    const char *other[] = { "/bin/sh", "-c", "printf OTHER; exit 3" };
    uint32_t code1 = 0, code2 = 0;
    TestWake wake;
    Pty *one, *two;
    wake.count = 0;
    if (pthread_mutex_init(&wake.lock, NULL) != 0) {
        check(false, "initialize POSIX notification test");
        return;
    }
    one = pty_spawn_posix(interactive, 3, NULL, NULL, 0, 90, 31,
                          NULL, NULL, error, sizeof error);
    two = pty_spawn_posix(other, 3, NULL, NULL, 0, 80, 24,
                          record_wake, &wake, error, sizeof error);
    check(one && two, "POSIX concurrent sessions spawn independently");
    if (!one || !two) {
        pty_close(one); pty_close(two);
        pthread_mutex_destroy(&wake.lock);
        return;
    }
    check(wait_for_text(one, "READY", first, sizeof first), "interactive POSIX child becomes ready");
    check(collect(two, second, sizeof second, &code2, 5000) &&
          code2 == 3 && strstr(second, "OTHER"),
          "second POSIX session completes while the first is waiting");
    check(!pty_resize(one, 0, 24) && pty_resize(one, 110, 42),
          "POSIX PTY validates and applies resize");
    check(pty_write(one, "first ", 6) && pty_write(one, "second", 6) &&
          pty_write(one, "\n", 1), "POSIX asynchronous writer accepts ordered chunks");
    check(collect(one, output, sizeof output, &code1, 5000) && code1 == 7 &&
          strstr(output, "|FIFO:<first second>|SIZE:42 110"),
          "POSIX FIFO writes and resized dimensions reach the child");
    pty_close(one);
    pty_close(two);
    check(wake.count > 0, "POSIX output or exit notifies the owner");
    pthread_mutex_destroy(&wake.lock);
}

static void test_backpressure_and_signals(void)
{
    char error[256], output[1024], *large;
    const char *flood[] = {
        "/bin/sh", "-c", "dd if=/dev/zero bs=1024 count=400 2>/dev/null | tr '\\000' x; printf '|END|'"
    };
    const char *signalled[] = { "/bin/sh", "-c", "kill -TERM $$" };
    Pty *pty = pty_spawn_posix(flood, 3, NULL, NULL, 0, 80, 24,
                               NULL, NULL, error, sizeof error);
    uint32_t code = 0;
    bool ok;
    check(pty != NULL, "POSIX output flood starts");
    if (pty) {
        uint64_t deadline = ticks_ms() + 5000;
        while (pty_pending(pty) < 256u * 1024u && ticks_ms() < deadline) pause_ms(2);
        check(pty_pending(pty) <= 256u * 1024u, "POSIX output buffering is bounded");
        large = (char *)malloc(409600u + 16u);
        ok = large && collect(pty, large, 409600u + 16u, &code, 10000);
        check(ok && code == 0 && strlen(large) == 409605u &&
              memcmp(large + 409600u, "|END|", 5) == 0,
              "POSIX bounded output preserves a flood and its final bytes");
        free(large);
        pty_close(pty);
    }
    pty = pty_spawn_posix(signalled, 3, NULL, NULL, 0, 80, 24,
                          NULL, NULL, error, sizeof error);
    ok = pty && collect(pty, output, sizeof output, &code, 5000);
    check(ok && code == 128u + SIGTERM, "POSIX signal exits use 128 plus signal number");
    pty_close(pty);
}

static bool process_terminated(pid_t pid)
{
    if (kill(pid, 0) < 0 && errno == ESRCH) return true;
#ifdef __linux__
    /* A killed orphan can remain a zombie until the host's init reaps it. */
    {
        char path[64], line[512], *end;
        FILE *file;
        snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
        file = fopen(path, "r");
        if (file) {
            bool zombie = false;
            if (fgets(line, sizeof line, file)) {
                end = strrchr(line, ')');
                zombie = end && end[1] == ' ' && end[2] == 'Z';
            }
            fclose(file);
            if (zombie) return true;
        }
    }
#endif
    return false;
}

static void test_cleanup(const char *directory)
{
    char error[256], ready[1024], pid_path[4096];
    const char *argv[] = {
        "/bin/sh", "-c",
        "stty -echo; set -m; /bin/sh -c 'trap \"\" HUP TERM; echo $$ > \"$1\"; "
        "while :; do sleep 1; done' job \"$1\" & "
        "while [ ! -s \"$1\" ]; do sleep 0.01; done; printf READY; wait",
        "termica-test", pid_path
    };
    Pty *pty;
    FILE *file;
    long pid = 0;
    char *bytes;
    uint64_t deadline;
    snprintf(pid_path, sizeof pid_path, "%s/job.pid", directory);
    pty = pty_spawn_posix(argv, 5, NULL, NULL, 0, 80, 24,
                          NULL, NULL, error, sizeof error);
    check(pty && wait_for_text(pty, "READY", ready, sizeof ready),
          "POSIX cleanup test starts a background job");
    if (!pty) return;
    file = fopen(pid_path, "r");
    if (file) { if (fscanf(file, "%ld", &pid) != 1) pid = 0; fclose(file); }
    bytes = (char *)malloc(TEST_INPUT_LIMIT + 1u);
    if (bytes) memset(bytes, 'x', TEST_INPUT_LIMIT + 1u);
    check(bytes && !pty_write(pty, bytes, TEST_INPUT_LIMIT + 1u),
          "POSIX writer rejects an oversized queue without blocking");
    if (bytes) check(pty_write(pty, bytes, 2u * 1024u * 1024u),
                     "POSIX writer queues input when the child is not reading");
    free(bytes);
    pty_close(pty);
    deadline = ticks_ms() + 5000;
    while (pid > 0 && !process_terminated((pid_t)pid) && ticks_ms() < deadline) pause_ms(10);
    check(pid > 0 && process_terminated((pid_t)pid),
          "POSIX close terminates background jobs while input is blocked");
    unlink(pid_path);
}

int pty_posix_selftest(void)
{
    char directory[] = "/tmp/termica-\xe9\x9b\xaa-XXXXXX";
    failures = 0;
    if (!mkdtemp(directory)) {
        check(false, "create UTF-8 PTY test directory");
        return failures;
    }
    test_spawn_and_environment(directory);
    test_errors();
    test_io_and_concurrency();
    test_backpressure_and_signals();
    test_cleanup(directory);
    rmdir(directory);
    return failures;
}
#endif
