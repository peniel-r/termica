/*
 * termica - the console of whoever launched us.
 *
 * The app is a GUI-subsystem executable: starting it from Explorer or from a
 * shortcut does not create a console window. When it is started *from* a
 * terminal that gave it no standard handles, attaching to that terminal's
 * console puts the startup diagnostics (renderer, cell size, sanitizer
 * reports, the self test) on screen instead of losing them. Anything the
 * launcher already wired up - a console, a pipe, a file - is left alone.
 *
 * Strict C99.
 */

#ifndef TERMICA_CONSOLE_H
#define TERMICA_CONSOLE_H

/* Attach to the launching console if there is one and point stdout/stderr at
 * it. A no-op when the app was started without a parent console. */
void console_reclaim(void);

#endif /* TERMICA_CONSOLE_H */
