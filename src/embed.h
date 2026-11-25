/*
 * termica - the font compiled into the executable.
 *
 * Maple Mono NF (SIL OFL 1.1) and Symbols Nerd Font Mono (MIT) are linked
 * into the executable as platform resources on Windows and binary sections
 * on Unix. A configured or command-line font still takes precedence.
 *
 * Strict C99.
 */

#ifndef TERMICA_EMBED_H
#define TERMICA_EMBED_H

#include <stdbool.h>
#include <stddef.h>

/* Resource IDs, kept in step with src/fonts.rc.in. */
typedef enum {
    EMBEDDED_FONT_REGULAR        = 1,
    EMBEDDED_FONT_BOLD           = 2,
    EMBEDDED_FONT_ITALIC         = 3,
    EMBEDDED_FONT_BOLD_ITALIC    = 4,
    EMBEDDED_FONT_SYMBOLS_MONO   = 5
} EmbeddedFontFace;

/* The bytes of one face belong to the executable and stay valid for the
 * lifetime of the process. False for an invalid face or missing resource. */
bool embedded_font(EmbeddedFontFace face, const void **data, size_t *size);

/* Is the built-in font present in this executable? */
bool embedded_font_available(void);

#endif /* TERMICA_EMBED_H */
