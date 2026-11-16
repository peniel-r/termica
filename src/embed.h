/*
 * termica - the font compiled into the executable.
 *
 * Google Sans Code Nerd Font Mono (SIL OFL 1.1, see assets/fonts/OFL.txt) is
 * linked into the binary as four Windows resources, so the app has a
 * working terminal font with Nerd icons even when no font is installed and
 * no font path is given. A path passed on the command line still wins.
 *
 * Strict C99.
 */

#ifndef TERMICA_EMBED_H
#define TERMICA_EMBED_H

#include <stdbool.h>
#include <stddef.h>

/* Resource IDs, kept in step with src/fonts.rc.in. */
typedef enum {
    EMBEDDED_FONT_REGULAR     = 1,
    EMBEDDED_FONT_BOLD        = 2,
    EMBEDDED_FONT_ITALIC      = 3,
    EMBEDDED_FONT_BOLD_ITALIC = 4
} EmbeddedFontFace;

/* The bytes of one face. The memory belongs to the executable and stays
 * valid for the lifetime of the process. False when this build has no
 * embedded font (anything but Windows) or the resource is missing. */
bool embedded_font(EmbeddedFontFace face, const void **data, size_t *size);

/* Is the built-in font present in this executable? */
bool embedded_font_available(void);

#endif /* TERMICA_EMBED_H */
