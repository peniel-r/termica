/* Windows crash diagnostics. Logs and dumps may contain private information. */
#ifndef TERMICA_DIAGNOSTICS_H
#define TERMICA_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>

/* Call once, before starting application threads. A false enabled argument is a
 * no-op. On failure the app may continue; a debugger receives a short error. */
bool diagnostics_init(bool enabled);

/* event must be a fixed, non-sensitive identifier (letters, digits, '.', '_',
 * or '-'), not a path, command, environment value or library error text.
 * code is a numeric status or error code. Safe to call from multiple threads.
 * Each record is flushed before this function returns. */
void diagnostics_log(const char *event, unsigned long code);

/* Call after application threads have stopped. Restores the previous filter. */
void diagnostics_shutdown(void);

/* Process-lifetime string, intended for the main thread. Format:
 * termica <version> (revision <revision>; libghostty-vt <loaded version>) */
const char *diagnostics_version(void);

/* Self-test entry points; the dispatcher must run before diagnostics_init. */
int diagnostics_selftest(void);
int diagnostics_test_dispatch(int argc, char **argv);

#ifdef _WIN32
#include <wchar.h>
/* Test-only variant: directory already exists and is not the user's log dir. */
bool diagnostics_test_init_at(const wchar_t *directory);
#endif

#endif
