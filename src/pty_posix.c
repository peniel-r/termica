/* POSIX pseudoterminals. The owner never performs blocking terminal I/O:
 * one thread drains a bounded output ring, another writes a bounded FIFO,
 * and a third observes the direct child without reaping its session leader. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _DARWIN_C_SOURCE

#include "pty.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/param.h>
#include <sys/user.h>
#include <util.h>
#else
#include <sys/syscall.h>
/* The project also has a pty.h; declare the system function here rather
 * than relying on include-path ordering to select libc's <pty.h>. */
extern int openpty(int *, int *, char *, const struct termios *,
                   const struct winsize *);
#endif

extern char **environ;

#define OUTPUT_CAPACITY (256u * 1024u)
#define INPUT_LIMIT (16u * 1024u * 1024u)
#define IO_CHUNK 8192u

struct Pty {
    int master;
    int reader_wake[2];
    int writer_wake[2];
    pid_t pid;
    pthread_t reader, writer, watcher;
    bool reader_started, writer_started, watcher_started;

    pthread_mutex_t lock;
    pthread_cond_t has_data, has_space;
    bool lock_ready, data_ready, space_ready;
    uint8_t *output;
    size_t output_head, output_length;
    bool closing, eof, exited, output_done, wake_pending;
    uint32_t exit_code;

    pthread_mutex_t write_lock;
    pthread_cond_t write_ready;
    bool write_lock_ready, write_cond_ready, write_closing, write_broken;
    uint8_t *input;
    size_t input_capacity, input_head, input_length;

    PtyNotifyFn notify;
    void *notify_userdata;
};

static void set_error(char *error, size_t size, const char *what, int code)
{
    if (error && size)
        snprintf(error, size, "%s: %s", what, strerror(code));
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* The child reserves fd 3 for reporting setup/exec errors. All PTY and
 * handshake descriptors must therefore be above it, even if the application
 * was started with one of its standard descriptors closed. */
static int move_above_stdio(int fd)
{
    if (fd < 4) {
        int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 4);
        int saved = errno;
        close(fd);
        errno = saved;
        return replacement;
    }
    if (set_cloexec(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool make_pipe(int fds[2], bool nonblocking)
{
    int raw[2];
    fds[0] = fds[1] = -1;
    if (pipe(raw) < 0)
        return false;
    fds[0] = move_above_stdio(raw[0]);
    if (fds[0] < 0) {
        int saved = errno;
        close(raw[1]);
        errno = saved;
        return false;
    }
    fds[1] = move_above_stdio(raw[1]);
    if (fds[1] < 0 || (nonblocking &&
        (set_nonblocking(fds[0]) < 0 || set_nonblocking(fds[1]) < 0))) {
        int saved = errno;
        close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
        fds[0] = fds[1] = -1;
        errno = saved;
        return false;
    }
    return true;
}

static void wake_fd(int fd)
{
    char byte = 0;
    ssize_t result;
    do { result = write(fd, &byte, 1); } while (result < 0 && errno == EINTR);
    /* A full nonblocking pipe is already a wake-up. */
}

static void drain_fd(int fd)
{
    char bytes[64];
    ssize_t n;
    do { n = read(fd, bytes, sizeof bytes); } while (n > 0 || (n < 0 && errno == EINTR));
}

/* Called with lock held; the callback itself is always invoked unlocked. */
static bool announce_locked(Pty *pty)
{
    bool notify = !pty->closing && !pty->wake_pending;
    if (!pty->closing) pty->wake_pending = true;
    pthread_cond_broadcast(&pty->has_data);
    return notify;
}

static void notify_owner(Pty *pty, bool notify)
{
    if (notify && pty->notify)
        pty->notify(pty->notify_userdata);
}

static void *reader_main(void *argument)
{
    Pty *pty = (Pty *)argument;
    uint8_t chunk[IO_CHUNK];

    for (;;) {
        size_t room;
        ssize_t n;
        bool notify;
        struct pollfd fds[2];
        int result;

        pthread_mutex_lock(&pty->lock);
        while (pty->output_length == OUTPUT_CAPACITY && !pty->closing)
            pthread_cond_wait(&pty->has_space, &pty->lock);
        if (pty->closing) {
            pthread_mutex_unlock(&pty->lock);
            break;
        }
        room = OUTPUT_CAPACITY - pty->output_length;
        pthread_mutex_unlock(&pty->lock);

        n = read(pty->master, chunk, room < sizeof chunk ? room : sizeof chunk);
        if (n > 0) {
            size_t tail, first;
            pthread_mutex_lock(&pty->lock);
            tail = (pty->output_head + pty->output_length) % OUTPUT_CAPACITY;
            first = OUTPUT_CAPACITY - tail;
            if (first > (size_t)n) first = (size_t)n;
            memcpy(pty->output + tail, chunk, first);
            if (first < (size_t)n)
                memcpy(pty->output, chunk + first, (size_t)n - first);
            pty->output_length += (size_t)n;
            pty->output_done = false;
            notify = announce_locked(pty);
            pthread_mutex_unlock(&pty->lock);
            notify_owner(pty, notify);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            break; /* Linux reports EIO after the last slave closes. */

        pthread_mutex_lock(&pty->lock);
        notify = false;
        if (pty->exited && !pty->output_done) {
            /* read reached EAGAIN after waitid observed exit. Everything
             * written by the direct child is now in the ring or consumed. */
            pty->output_done = true;
            notify = announce_locked(pty);
        }
        pthread_mutex_unlock(&pty->lock);
        notify_owner(pty, notify);

        fds[0].fd = pty->master;
        fds[0].events = POLLIN;
        fds[1].fd = pty->reader_wake[0];
        fds[1].events = POLLIN;
        do { result = poll(fds, 2, -1); } while (result < 0 && errno == EINTR);
        if (result < 0) break;
        if (fds[1].revents) drain_fd(pty->reader_wake[0]);
        /* Retry read for both readability and hangup; a wake also lets us
         * notice exit or close without relying on another terminal event. */
    }
    pthread_mutex_lock(&pty->lock);
    pty->eof = true;
    pty->output_done = true;
    {
        bool notify = announce_locked(pty);
        pthread_mutex_unlock(&pty->lock);
        notify_owner(pty, notify);
    }
    pthread_mutex_lock(&pty->write_lock);
    pty->write_broken = true;
    pthread_cond_signal(&pty->write_ready);
    pthread_mutex_unlock(&pty->write_lock);
    wake_fd(pty->writer_wake[1]);
    return NULL;
}

static void *writer_main(void *argument)
{
    Pty *pty = (Pty *)argument;
    for (;;) {
        ssize_t n;
        size_t part;
        struct pollfd fds[2];
        int result;
        pthread_mutex_lock(&pty->write_lock);
        while (!pty->input_length && !pty->write_closing && !pty->write_broken)
            pthread_cond_wait(&pty->write_ready, &pty->write_lock);
        if (pty->write_closing || pty->write_broken) {
            pthread_mutex_unlock(&pty->write_lock);
            break;
        }
        part = pty->input_capacity - pty->input_head;
        if (part > pty->input_length) part = pty->input_length;
        if (part > IO_CHUNK) part = IO_CHUNK;
        n = write(pty->master, pty->input + pty->input_head, part);
        if (n > 0) {
            pty->input_head = (pty->input_head + (size_t)n) % pty->input_capacity;
            pty->input_length -= (size_t)n;
            pthread_mutex_unlock(&pty->write_lock);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            pthread_mutex_unlock(&pty->write_lock);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            pty->write_broken = true;
            pthread_mutex_unlock(&pty->write_lock);
            break;
        }
        pthread_mutex_unlock(&pty->write_lock);
        fds[0].fd = pty->master;
        fds[0].events = POLLOUT;
        fds[1].fd = pty->writer_wake[0];
        fds[1].events = POLLIN;
        do { result = poll(fds, 2, -1); } while (result < 0 && errno == EINTR);
        if (result < 0) {
            pthread_mutex_lock(&pty->write_lock);
            pty->write_broken = true;
            pthread_mutex_unlock(&pty->write_lock);
            break;
        }
        if (fds[1].revents) drain_fd(pty->writer_wake[0]);
    }
    return NULL;
}

static void *watcher_main(void *argument)
{
    Pty *pty = (Pty *)argument;
    siginfo_t info;
    int result;
    memset(&info, 0, sizeof info);
    do { result = waitid(P_PID, (id_t)pty->pid, &info, WEXITED | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    if (result == 0) {
        bool notify;
        pthread_mutex_lock(&pty->lock);
        pty->exited = true;
        /* Darwin waitid can return more than eight bits for CLD_EXITED,
         * unlike waitpid/WEXITSTATUS and normal shell exit semantics. */
        pty->exit_code = info.si_code == CLD_EXITED ? (uint32_t)(info.si_status & 0xff)
                                                    : (uint32_t)(128 + info.si_status);
        notify = announce_locked(pty);
        pthread_mutex_unlock(&pty->lock);
        wake_fd(pty->reader_wake[1]);
        notify_owner(pty, notify);
    }
    return NULL;
}

static void free_strings(char **items)
{
    size_t i;
    if (!items) return;
    for (i = 0; items[i]; i++) free(items[i]);
    free(items);
}

static const char *env_value(char *const *items, const char *key)
{
    size_t i, length = strlen(key);
    for (i = 0; items[i]; i++)
        if (!strncmp(items[i], key, length) && items[i][length] == '=')
            return items[i] + length + 1;
    return NULL;
}

static bool env_set(char **items, const char *key, const char *value)
{
    size_t i, keylen = strlen(key), vallen = strlen(value);
    char *entry;
    if (vallen > SIZE_MAX - 2 || keylen > SIZE_MAX - vallen - 2) {
        errno = ENOMEM;
        return false;
    }
    entry = (char *)malloc(keylen + vallen + 2);
    if (!entry) return false;
    memcpy(entry, key, keylen);
    entry[keylen] = '=';
    memcpy(entry + keylen + 1, value, vallen + 1);
    for (i = 0; items[i]; i++) {
        if (!strncmp(items[i], key, keylen) && items[i][keylen] == '=') {
            free(items[i]);
            items[i] = entry;
            return true;
        }
    }
    items[i] = entry;
    items[i + 1] = NULL;
    return true;
}

static char **build_environment(const PtyEnvEntry *extra, int count)
{
    size_t inherited = 0, i;
    char **items;
    const char *value;
    while (environ && environ[inherited]) inherited++;
    if (count < 0 || (count && !extra) ||
        (size_t)count > SIZE_MAX / sizeof *items - 5 ||
        inherited > SIZE_MAX / sizeof *items - (size_t)count - 5) {
        errno = EINVAL;
        return NULL;
    }
    items = (char **)calloc(inherited + (size_t)count + 5, sizeof *items);
    if (!items) return NULL;
    for (i = 0; i < inherited; i++) {
        items[i] = strdup(environ[i]);
        if (!items[i]) goto fail;
    }
    value = env_value(items, "TERM");
    if ((!value || !*value || !strcasecmp(value, "dumb")) &&
        !env_set(items, "TERM", "xterm-256color")) goto fail;
    value = env_value(items, "COLORTERM");
    if ((!value || !*value) && !env_set(items, "COLORTERM", "truecolor")) goto fail;
    if (!env_set(items, "TERMICA", "1") ||
        !env_set(items, "TERMICA_SHELL_INTEGRATION", "1")) goto fail;
    for (i = 0; i < (size_t)count; i++) {
        if (!extra[i].key || !*extra[i].key || strchr(extra[i].key, '=') ||
            !extra[i].value) {
            errno = EINVAL;
            goto fail;
        }
        if (!env_set(items, extra[i].key, extra[i].value)) goto fail;
    }
    return items;
fail:
    free_strings(items);
    return NULL;
}

/* Resolve PATH into candidate strings before fork. Relative PATH components
 * stay relative so execve resolves them after the child's chdir. */
static char **build_candidates(const char *program, char *const *environment)
{
    const char *path, *cursor;
    char **result;
    size_t count = 1, index = 0, plen = strlen(program);
    if (strchr(program, '/')) {
        result = (char **)calloc(2, sizeof *result);
        if (result) {
            result[0] = strdup(program);
            if (!result[0]) { free(result); return NULL; }
        }
        return result;
    }
    path = env_value(environment, "PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    for (cursor = path; *cursor; cursor++) if (*cursor == ':') count++;
    result = (char **)calloc(count + 1, sizeof *result);
    if (!result) return NULL;
    cursor = path;
    for (;;) {
        const char *end = strchr(cursor, ':');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        size_t prefix = length ? length : 1;
        if (prefix > SIZE_MAX - plen - 2) { errno = ENOMEM; goto fail; }
        result[index] = (char *)malloc(prefix + plen + 2);
        if (!result[index]) goto fail;
        if (length) memcpy(result[index], cursor, length);
        else result[index][0] = '.';
        result[index][prefix] = '/';
        memcpy(result[index] + prefix + 1, program, plen + 1);
        index++;
        if (!end) break;
        cursor = end + 1;
    }
    return result;
fail:
    free_strings(result);
    return NULL;
}

char *pty_default_shell_posix(void)
{
    const char *shell = getenv("SHELL");
    struct stat status;
    if (!shell || shell[0] != '/' || stat(shell, &status) < 0 ||
        !S_ISREG(status.st_mode) || access(shell, X_OK) < 0)
        shell = "/bin/sh";
    return strdup(shell);
}

struct child_error { int stage, code; };
enum { CHILD_SESSION = 1, CHILD_TTY, CHILD_DUP, CHILD_CWD, CHILD_EXEC, CHILD_CLOSE };

/* Only async-signal-safe operations are performed in the forked child. */
static void child_fail(int stage, int code)
{
    struct child_error error;
    const char *bytes = (const char *)&error;
    size_t remaining = sizeof error;
    error.stage = stage;
    error.code = code;
    while (remaining) {
        ssize_t n = write(3, bytes, remaining);
        if (n > 0) { bytes += n; remaining -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    _exit(127);
}

static void child_close_descriptors(void)
{
#if !defined(__APPLE__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4u, ~0u, 0u) == 0) return;
#endif
    /* Darwin has no closefrom. Enumerate the child's descriptor table with
     * the kernel directory-entry syscall, marking rather than closing entries
     * so enumeration remains stable. Linux uses this on older kernels that
     * lack close_range. No libc directory streams or allocation after fork. */
    {
        union { uint64_t alignment; char bytes[4096]; } buffer;
        int directory;
#ifdef __APPLE__
        off_t position = 0;
        directory = open("/dev/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
        struct linux_dirent64 {
            uint64_t ino;
            int64_t offset;
            unsigned short reclen;
            unsigned char type;
            char name[];
        };
        directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
        if (directory < 0) child_fail(CHILD_CLOSE, errno);
        for (;;) {
            ssize_t count;
            size_t offset = 0;
#ifdef __APPLE__
            /* XNU's getdirentries64 syscall is 344; libc's legacy
             * getdirentries is unavailable with 64-bit inodes. */
            do { count = syscall(344, directory, buffer.bytes, sizeof buffer.bytes, &position); }
            while (count < 0 && errno == EINTR);
#elif defined(SYS_getdents64)
            do { count = syscall(SYS_getdents64, directory, buffer.bytes, sizeof buffer.bytes); }
            while (count < 0 && errno == EINTR);
#else
            count = -1;
            errno = ENOTSUP;
#endif
            if (count < 0) child_fail(CHILD_CLOSE, errno);
            if (!count) break;
            while (offset < (size_t)count) {
                const char *name;
                size_t record, name_offset, j;
                int fd = 0;
                bool numeric = true;
#ifdef __APPLE__
                const struct dirent *entry = (const struct dirent *)(buffer.bytes + offset);
                record = entry->d_reclen;
                name_offset = offsetof(struct dirent, d_name);
                name = entry->d_name;
#else
                const struct linux_dirent64 *entry =
                    (const struct linux_dirent64 *)(buffer.bytes + offset);
                record = entry->reclen;
                name_offset = offsetof(struct linux_dirent64, name);
                name = entry->name;
#endif
                if (record <= name_offset || record > (size_t)count - offset)
                    child_fail(CHILD_CLOSE, EIO);
                for (j = 0; j < record - name_offset && name[j]; j++) {
                    int digit = name[j] - '0';
                    if (digit < 0 || digit > 9 || fd > (INT_MAX - digit) / 10) {
                        numeric = false;
                        break;
                    }
                    fd = fd * 10 + digit;
                }
                if (j == record - name_offset) child_fail(CHILD_CLOSE, EIO);
                if (numeric && j && fd >= 4 && fd != directory && set_cloexec(fd) < 0)
                    child_fail(CHILD_CLOSE, errno);
                offset += record;
            }
        }
        close(directory);
    }
}

static void child_exec(int slave, int error_fd, const char *cwd,
                       char *const *argv, char *const *environment,
                       char *const *candidates,
                       const struct sigaction *default_action,
                       const sigset_t *empty_mask)
{
    int signal_number, saved = ENOENT;
    size_t i;
    pid_t group;
    if (dup2(error_fd, 3) < 0 || set_cloexec(3) < 0) _exit(127);
    if (setsid() < 0) child_fail(CHILD_SESSION, errno);
    if (ioctl(slave, TIOCSCTTY, 0) < 0) child_fail(CHILD_TTY, errno);
    group = getpid();
    if (ioctl(slave, TIOCSPGRP, &group) < 0) child_fail(CHILD_TTY, errno);
    if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0) child_fail(CHILD_DUP, errno);
    if (cwd && *cwd && chdir(cwd) < 0) child_fail(CHILD_CWD, errno);
    child_close_descriptors();
    for (signal_number = 1; signal_number < NSIG; signal_number++)
        if (signal_number != SIGKILL && signal_number != SIGSTOP)
            (void)sigaction(signal_number, default_action, NULL);
    (void)sigprocmask(SIG_SETMASK, empty_mask, NULL);
    for (i = 0; candidates[i]; i++) {
        execve(candidates[i], argv, environment);
        if (errno == EACCES) saved = EACCES;
        else if (errno != ENOENT && errno != ENOTDIR) child_fail(CHILD_EXEC, errno);
    }
    child_fail(CHILD_EXEC, saved);
}

static void kill_session_member(pid_t session, pid_t pid)
{
    pid_t group;
    if (pid <= 0 || getsid(pid) != session) return;
    group = getpgid(pid);
    if (group > 0) (void)kill(-group, SIGKILL);
    (void)kill(pid, SIGKILL);
}

/* Jobs may have their own process groups. Kill all groups in this PTY's
 * session, not just the shell's original group. The unreaped session leader
 * keeps its PID reserved until cleanup, preventing session-ID reuse. */
static void kill_session_jobs(pid_t session)
{
#ifdef __APPLE__
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t size = 0, i;
    struct kinfo_proc *list;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) == 0 && size) {
        size += 16 * sizeof *list;
        list = (struct kinfo_proc *)malloc(size);
        if (list) {
            if (sysctl(mib, 4, list, &size, NULL, 0) == 0)
                for (i = 0; i < size / sizeof *list; i++)
                    kill_session_member(session, list[i].kp_proc.p_pid);
            free(list);
        }
    }
#else
    DIR *dir = opendir("/proc");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char *end;
            long value;
            errno = 0;
            value = strtol(entry->d_name, &end, 10);
            if (!errno && *entry->d_name && !*end && value > 0 && value <= INT_MAX)
                kill_session_member(session, (pid_t)value);
        }
        closedir(dir);
    }
#endif
    (void)kill(-session, SIGKILL);
    (void)kill(session, SIGKILL);
}

void pty_close(Pty *pty)
{
    int status;
    if (!pty) return;
    if (pty->lock_ready) {
        pthread_mutex_lock(&pty->lock);
        pty->closing = true;
        if (pty->space_ready) pthread_cond_broadcast(&pty->has_space);
        if (pty->data_ready) pthread_cond_broadcast(&pty->has_data);
        pthread_mutex_unlock(&pty->lock);
    }
    if (pty->write_lock_ready) {
        pthread_mutex_lock(&pty->write_lock);
        pty->write_closing = true;
        if (pty->write_cond_ready) pthread_cond_broadcast(&pty->write_ready);
        pthread_mutex_unlock(&pty->write_lock);
    }
    if (pty->reader_wake[1] >= 0) wake_fd(pty->reader_wake[1]);
    if (pty->writer_wake[1] >= 0) wake_fd(pty->writer_wake[1]);
    if (pty->pid > 0) {
        if (pty->master >= 0) {
            pid_t foreground = tcgetpgrp(pty->master);
            if (foreground > 0) kill_session_member(pty->pid, foreground);
        }
        kill_session_jobs(pty->pid);
        kill_session_jobs(pty->pid);
    }
    if (pty->reader_started) pthread_join(pty->reader, NULL);
    if (pty->writer_started) pthread_join(pty->writer, NULL);
    if (pty->watcher_started) pthread_join(pty->watcher, NULL);
    if (pty->pid > 0) {
        while (waitpid(pty->pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (pty->master >= 0) close(pty->master);
    if (pty->reader_wake[0] >= 0) close(pty->reader_wake[0]);
    if (pty->reader_wake[1] >= 0) close(pty->reader_wake[1]);
    if (pty->writer_wake[0] >= 0) close(pty->writer_wake[0]);
    if (pty->writer_wake[1] >= 0) close(pty->writer_wake[1]);
    if (pty->write_cond_ready) pthread_cond_destroy(&pty->write_ready);
    if (pty->write_lock_ready) pthread_mutex_destroy(&pty->write_lock);
    if (pty->space_ready) pthread_cond_destroy(&pty->has_space);
    if (pty->data_ready) pthread_cond_destroy(&pty->has_data);
    if (pty->lock_ready) pthread_mutex_destroy(&pty->lock);
    free(pty->output);
    free(pty->input);
    free(pty);
}

Pty *pty_spawn_posix(const char *const *argv, int argc, const char *cwd_utf8,
                     const PtyEnvEntry *env, int env_count,
                     uint16_t columns, uint16_t rows,
                     PtyNotifyFn notify, void *notify_userdata,
                     char *error, size_t error_size)
{
    Pty *pty = NULL;
    char **environment = NULL, **candidates = NULL, **child_argv = NULL;
    int slave = -1, handshake[2] = {-1, -1}, rc, i;
    struct winsize dimensions;
    struct child_error child_error;
    size_t received = 0;
    sigset_t blocked, previous, empty;
    struct sigaction default_action, child_action;
    const char *failure = "spawn";
    int failure_code = ENOMEM;
    bool success = false;

    if (error && error_size) error[0] = '\0';
    if (!argv || argc <= 0 || !argv[0] || !argv[0][0] ||
        !columns || !rows || env_count < 0 || (env_count && !env)) {
        set_error(error, error_size, "invalid PTY spawn arguments", EINVAL);
        return NULL;
    }
    for (i = 0; i < argc; i++) if (!argv[i]) {
        set_error(error, error_size, "invalid PTY argv", EINVAL);
        return NULL;
    }
    /* With either policy the OS may reap this PID automatically, making
     * exit reporting and later session-ID-safe cleanup impossible. Do not
     * mutate process-wide SIGCHLD policy, which other subsystems may own. */
    if (sigaction(SIGCHLD, NULL, &child_action) < 0) {
        set_error(error, error_size, "SIGCHLD disposition", errno);
        return NULL;
    }
    if (child_action.sa_handler == SIG_IGN || (child_action.sa_flags & SA_NOCLDWAIT)) {
        set_error(error, error_size, "SIGCHLD discards child status", EINVAL);
        return NULL;
    }
    child_argv = (char **)calloc((size_t)argc + 1, sizeof *child_argv);
    if (!child_argv) goto done;
    for (i = 0; i < argc; i++) child_argv[i] = (char *)argv[i];
    environment = build_environment(env, env_count);
    if (!environment) { failure = "environment"; failure_code = errno; goto done; }
    candidates = build_candidates(argv[0], environment);
    if (!candidates) { failure = "command path"; failure_code = errno; goto done; }
    pty = (Pty *)calloc(1, sizeof *pty);
    if (!pty) goto done;
    pty->master = -1;
    pty->reader_wake[0] = pty->reader_wake[1] = -1;
    pty->writer_wake[0] = pty->writer_wake[1] = -1;
    pty->notify = notify;
    pty->notify_userdata = notify_userdata;
    pty->output = (uint8_t *)malloc(OUTPUT_CAPACITY);
    if (!pty->output) goto done;
    rc = pthread_mutex_init(&pty->lock, NULL);
    if (rc) { failure = "mutex"; failure_code = rc; goto done; }
    pty->lock_ready = true;
    rc = pthread_cond_init(&pty->has_data, NULL);
    if (rc) { failure = "condition"; failure_code = rc; goto done; }
    pty->data_ready = true;
    rc = pthread_cond_init(&pty->has_space, NULL);
    if (rc) { failure = "condition"; failure_code = rc; goto done; }
    pty->space_ready = true;
    rc = pthread_mutex_init(&pty->write_lock, NULL);
    if (rc) { failure = "mutex"; failure_code = rc; goto done; }
    pty->write_lock_ready = true;
    rc = pthread_cond_init(&pty->write_ready, NULL);
    if (rc) { failure = "condition"; failure_code = rc; goto done; }
    pty->write_cond_ready = true;
    if (!make_pipe(pty->reader_wake, true) || !make_pipe(pty->writer_wake, true) ||
        !make_pipe(handshake, false)) {
        failure = "pipe"; failure_code = errno; goto done;
    }
    memset(&dimensions, 0, sizeof dimensions);
    dimensions.ws_col = columns;
    dimensions.ws_row = rows;
    if (openpty(&pty->master, &slave, NULL, NULL, &dimensions) < 0) {
        failure = "openpty"; failure_code = errno; goto done;
    }
    pty->master = move_above_stdio(pty->master);
    if (pty->master < 0) { failure = "PTY descriptor"; failure_code = errno; goto done; }
    slave = move_above_stdio(slave);
    if (slave < 0 || set_nonblocking(pty->master) < 0) {
        failure = "PTY descriptor"; failure_code = errno; goto done;
    }
    memset(&default_action, 0, sizeof default_action);
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigfillset(&blocked);
    sigemptyset(&empty);
    rc = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (rc) { failure = "block signals"; failure_code = rc; goto done; }
    pty->pid = fork();
    if (pty->pid == 0)
        child_exec(slave, handshake[1], cwd_utf8, child_argv, environment,
                   candidates, &default_action, &empty);
    failure_code = errno;
    (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (pty->pid < 0) { pty->pid = 0; failure = "fork"; goto done; }
    close(slave); slave = -1;
    close(handshake[1]); handshake[1] = -1;
    for (;;) {
        ssize_t n = read(handshake[0], (char *)&child_error + received,
                         sizeof child_error - received);
        if (n > 0) {
            received += (size_t)n;
            if (received == sizeof child_error) break;
        } else if (n == 0) break;
        else if (errno != EINTR) { failure = "exec handshake"; failure_code = errno; goto done; }
    }
    if (received) {
        if (received != sizeof child_error) { failure = "exec handshake"; failure_code = EIO; }
        else {
            failure_code = child_error.code;
            switch (child_error.stage) {
            case CHILD_SESSION: failure = "setsid"; break;
            case CHILD_TTY: failure = "controlling terminal"; break;
            case CHILD_DUP: failure = "terminal descriptors"; break;
            case CHILD_CWD: failure = "working directory"; break;
            case CHILD_CLOSE: failure = "inherited descriptors"; break;
            default: failure = "exec"; break;
            }
        }
        goto done;
    }
    rc = pthread_create(&pty->reader, NULL, reader_main, pty);
    if (rc) { failure = "reader thread"; failure_code = rc; goto done; }
    pty->reader_started = true;
    rc = pthread_create(&pty->writer, NULL, writer_main, pty);
    if (rc) { failure = "writer thread"; failure_code = rc; goto done; }
    pty->writer_started = true;
    rc = pthread_create(&pty->watcher, NULL, watcher_main, pty);
    if (rc) { failure = "child watcher"; failure_code = rc; goto done; }
    pty->watcher_started = true;
    success = true;
done:
    if (slave >= 0) close(slave);
    if (handshake[0] >= 0) close(handshake[0]);
    if (handshake[1] >= 0) close(handshake[1]);
    free(child_argv);
    free_strings(environment);
    free_strings(candidates);
    if (!success) {
        set_error(error, error_size, failure, failure_code ? failure_code : ENOMEM);
        pty_close(pty);
        return NULL;
    }
    return pty;
}

size_t pty_read(Pty *pty, void *buffer, size_t capacity)
{
    size_t take, first;
    if (!pty || !buffer || !capacity) return 0;
    pthread_mutex_lock(&pty->lock);
    take = pty->output_length < capacity ? pty->output_length : capacity;
    first = OUTPUT_CAPACITY - pty->output_head;
    if (first > take) first = take;
    if (take) {
        memcpy(buffer, pty->output + pty->output_head, first);
        if (first < take) memcpy((uint8_t *)buffer + first, pty->output, take - first);
        pty->output_head = (pty->output_head + take) % OUTPUT_CAPACITY;
        pty->output_length -= take;
        pthread_cond_signal(&pty->has_space);
    }
    if (!pty->output_length) pty->wake_pending = false;
    pthread_mutex_unlock(&pty->lock);
    return take;
}

size_t pty_pending(Pty *pty)
{
    size_t length;
    if (!pty) return 0;
    pthread_mutex_lock(&pty->lock);
    length = pty->output_length;
    pthread_mutex_unlock(&pty->lock);
    return length;
}

bool pty_write(Pty *pty, const void *data, size_t length)
{
    size_t tail, first;
    if (!pty || !data) return false;
    pthread_mutex_lock(&pty->write_lock);
    if (pty->write_closing || pty->write_broken || length > INPUT_LIMIT - pty->input_length) {
        pthread_mutex_unlock(&pty->write_lock);
        return false;
    }
    if (!length) { pthread_mutex_unlock(&pty->write_lock); return true; }
    if (pty->input_length + length > pty->input_capacity) {
        size_t cap = pty->input_capacity ? pty->input_capacity : 16384u;
        uint8_t *next;
        while (cap < pty->input_length + length) cap *= 2;
        next = (uint8_t *)malloc(cap);
        if (!next) { pthread_mutex_unlock(&pty->write_lock); return false; }
        if (pty->input_length) {
            first = pty->input_capacity - pty->input_head;
            if (first > pty->input_length) first = pty->input_length;
            memcpy(next, pty->input + pty->input_head, first);
            if (first < pty->input_length)
                memcpy(next + first, pty->input, pty->input_length - first);
        }
        free(pty->input);
        pty->input = next;
        pty->input_capacity = cap;
        pty->input_head = 0;
    }
    tail = (pty->input_head + pty->input_length) % pty->input_capacity;
    first = pty->input_capacity - tail;
    if (first > length) first = length;
    memcpy(pty->input + tail, data, first);
    if (first < length) memcpy(pty->input, (const uint8_t *)data + first, length - first);
    pty->input_length += length;
    pthread_cond_signal(&pty->write_ready);
    pthread_mutex_unlock(&pty->write_lock);
    return true;
}

bool pty_resize(Pty *pty, uint16_t columns, uint16_t rows)
{
    struct winsize size;
    if (!pty || !columns || !rows) return false;
    memset(&size, 0, sizeof size);
    size.ws_col = columns;
    size.ws_row = rows;
    return ioctl(pty->master, TIOCSWINSZ, &size) == 0;
}

bool pty_wait(Pty *pty, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ready;
    int rc = 0;
    if (!pty || clock_gettime(CLOCK_REALTIME, &deadline) < 0) return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&pty->lock);
    while (!pty->output_length && !pty->eof && !pty->exited && !pty->closing && !rc)
        rc = pthread_cond_timedwait(&pty->has_data, &pty->lock, &deadline);
    ready = pty->output_length || pty->eof || pty->exited;
    pthread_mutex_unlock(&pty->lock);
    return ready;
}

bool pty_exit_code(Pty *pty, uint32_t *code)
{
    bool exited;
    if (!pty) return false;
    pthread_mutex_lock(&pty->lock);
    exited = pty->exited;
    if (exited && code) *code = pty->exit_code;
    pthread_mutex_unlock(&pty->lock);
    return exited;
}

bool pty_output_drained(Pty *pty)
{
    bool drained;
    if (!pty) return false;
    pthread_mutex_lock(&pty->lock);
    drained = pty->exited && pty->output_done && !pty->output_length;
    pthread_mutex_unlock(&pty->lock);
    return drained;
}
#endif
