/*
 * termica - resolve a font setting to a file on disk.
 *
 * The settings file and the command line accept either a path to a font
 * file or the family name of an installed font. This module turns the
 * latter into the regular face's file path.
 *
 * Strict C99. The registry lookup is Windows-only; elsewhere a name that
 * is not already a readable file is simply not resolved.
 */

#ifndef TERMICA_FONT_LOOKUP_H
#define TERMICA_FONT_LOOKUP_H

#include <stdbool.h>
#include <stddef.h>

/* Copy the path for `name` into `out`. `name` may be a file path or an
 * installed family name. False when it is neither. */
bool font_lookup_path(const char *name, char *out, size_t capacity);

#endif /* TERMICA_FONT_LOOKUP_H */
