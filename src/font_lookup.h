/*
 * termica - resolve a font setting to a file on disk.
 *
 * The settings file and the command line accept either a path to a font
 * file or the family name of an installed font. This module turns the
 * latter into the regular face's file path.
 *
 * Installed families are resolved with the native font registry, fontconfig,
 * or CoreText. Paths and family names are UTF-8.
 */

#ifndef TERMICA_FONT_LOOKUP_H
#define TERMICA_FONT_LOOKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copy the path for `name` into `out`. `name` may be a file path or an
 * installed family name. False when it is neither. */
bool font_lookup_path(const char *name, char *out, size_t capacity);

/* Resolve a face and its zero-based collection index. A file path selects
 * its first face for regular; installed families select their regular face.
 * Bold and italic requests require a real matching face, not a substitute. */
bool font_lookup_face(const char *name, bool bold, bool italic,
                      char *out, size_t capacity, int *face_index);

/* Find an installed font carrying a Unicode scalar, with its collection
 * index. Used after the configured and platform fallback families. */
bool font_lookup_codepoint(uint32_t codepoint, char *out, size_t capacity,
                           int *face_index);

#endif /* TERMICA_FONT_LOOKUP_H */
