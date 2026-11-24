/* Link lookup in the current terminal viewport. All calls run on the same
 * thread that owns the terminal; results are snapshots, not tracked refs. */
#ifndef TERMICA_HYPERLINK_H
#define TERMICA_HYPERLINK_H

#include <stdbool.h>
#include <stddef.h>

#include "term.h"

#define HYPERLINK_URI_MAX 8192

typedef struct Hyperlink {
    char uri[HYPERLINK_URI_MAX + 1];
    /* Inclusive, ordered viewport cell coordinates. OSC 8 ranges are
     * clipped to the viewport; auto URLs may extend beyond it. */
    int start_x, start_y;
    int end_x, end_y;
} Hyperlink;

/* Find the allowed OSC 8 or automatically detected URL under a viewport
 * cell. The snapshot becomes stale after output, scroll, resize or tab
 * switch. Does not change terminal selection or mouse tracking. */
bool hyperlink_at(Term *term, int x, int y, Hyperlink *out);

/* Test a viewport cell against the inclusive link range. */
bool hyperlink_contains(const Hyperlink *link, int x, int y);

/* Validate an explicit byte string; only http, https and mailto are
 * permitted, and malformed, control-containing or empty URIs are denied. */
bool hyperlink_uri_allowed(const char *uri, size_t length);

/* Revalidate and open a UTF-8 URI with the Windows default handler. Returns
 * false for invalid input, conversion errors, or a failed shell invocation. */
bool hyperlink_open(const char *uri);

/* Returns the number of failed consumer-observable checks. */
int hyperlink_selftest(void);

#endif
