/*
 * termica - the process side: a real console behind the grid.
 *
 * Windows has no forkpty(); it has conhost, and ConPTY exposes it as a pair
 * of byte pipes plus a child process. This module owns that: creating the
 * pseudoconsole, starting the shell in it, reading its output on a thread,
 * writing input, resizing, and tearing the tree down.
 *
 * Threading contract:
 *
 *   - the reader thread only fills the ring buffer and calls the notify
 *     callback; it never touches the terminal
 *   - pty_read, pty_write, pty_resize, pty_close and pty_wait are called
 *     from the thread that owns the pty (the app's main thread)
 *
 * Strict C99.
 */

#ifndef TERMICA_PTY_H
#define TERMICA_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct Pty Pty;

/* One UTF-8 key/value environment override passed when spawning a child. */
typedef struct PtyEnvEntry {
    const char *key;
    const char *value;
} PtyEnvEntry;

/* Called from a background thread when output is waiting or the program
 * exits. It must be thread-safe and must not block for long. */
typedef void (*PtyNotifyFn)(void *userdata);

#ifdef _WIN32
/* The command line of the shell to start: pwsh.exe, else powershell.exe,
 * else cmd.exe. Returns a malloc'd string, or NULL if none was found. */
wchar_t *pty_default_shell(void);

/* Which shell to start when the user said. `which` is a SettingsShell
 * value (0 auto, 1 pwsh, 2 powershell, 3 cmd), because the only reason to
 * name a shell is the settings file. A named shell that is not installed
 * returns NULL rather than silently starting another one. */
wchar_t *pty_shell_for(int which);

/* Build a Windows CreateProcessW command line from a UTF-8 argv array using
 * standard Windows quoting rules. Returns a malloc'd wide string the caller
 * frees, or NULL on invalid input / allocation failure. */
wchar_t *pty_build_command_line(const char *const *argv, int argc);

/* Build a double-NUL Unicode environment block inheriting the current
 * process's environment, filling in TERM/COLORTERM/TERMICA defaults, and
 * overlaying `extra` entries (case-insensitive replacement). Exposed for the
 * self test; caller frees the returned buffer. */
wchar_t *pty_build_environment(const PtyEnvEntry *extra, int extra_count);

/* Start command_line behind a pseudoconsole of columns x rows, with an
 * optional UTF-8 working directory and optional environment overlays.
 * Returns NULL on failure and fills error. */
Pty *pty_spawn_ex(const wchar_t *command_line, const char *cwd_utf8,
                  const PtyEnvEntry *env, int env_count,
                  uint16_t columns, uint16_t rows,
                  PtyNotifyFn notify, void *notify_userdata,
                  char *error, size_t error_size);

/* Convenience wrapper for pty_spawn_ex with inherited cwd and environment. */
Pty *pty_spawn(const wchar_t *command_line, uint16_t columns, uint16_t rows,
               PtyNotifyFn notify, void *notify_userdata,
               char *error, size_t error_size);
#else
/* Spawn a POSIX program with its exact UTF-8 argv; no shell interpretation.
 * argc includes argv[0]. A command without '/' is looked up in the child
 * environment's PATH, after applying the environment overrides. */
Pty *pty_spawn_posix(const char *const *argv, int argc, const char *cwd_utf8,
                     const PtyEnvEntry *env, int env_count,
                     uint16_t columns, uint16_t rows,
                     PtyNotifyFn notify, void *notify_userdata,
                     char *error, size_t error_size);

/* A usable absolute $SHELL, or /bin/sh if it is missing or unusable.
 * The returned string is owned by the caller. */
char *pty_default_shell_posix(void);

/* True when all output available after child exit has been drained, even
 * if a background job still has the slave open. */
bool pty_output_drained(Pty *pty);
#endif

/* Change the console size. False when the pseudoconsole refused it. */
bool pty_resize(Pty *pty, uint16_t columns, uint16_t rows);

/* Copy up to capacity bytes of program output. Returns the byte count,
 * 0 when nothing is buffered. Never blocks. */
size_t pty_read(Pty *pty, void *buffer, size_t capacity);

/* Number of bytes buffered for the owner thread; never blocks. */
size_t pty_pending(Pty *pty);

/* Queue an entire write without waiting for the child. Writes are FIFO;
 * false means the pipe is gone, the queue is full, or allocation failed. */
bool pty_write(Pty *pty, const void *data, size_t length);

/* Wait until output is buffered, output ends, or timeout_ms elapses. POSIX
 * may also wake for child exit. For embedders without an event loop. */
bool pty_wait(Pty *pty, uint32_t timeout_ms);

/* True once the direct child has exited; fills its exit code. POSIX signal
 * termination is represented as 128 plus the signal number. */
bool pty_exit_code(Pty *pty, uint32_t *code);

/* Terminate remaining jobs, close the pseudoconsole, join its I/O threads,
 * and free everything. Safe on NULL. */
void pty_close(Pty *pty);

#endif /* TERMICA_PTY_H */
