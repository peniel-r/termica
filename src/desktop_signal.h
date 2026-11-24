/* Windows notifications and taskbar progress. All platform calls belong on
 * the UI thread, never inside a libghostty callback. No COM objects or input
 * buffers are retained between calls. */
#ifndef TERMICA_DESKTOP_SIGNAL_H
#define TERMICA_DESKTOP_SIGNAL_H

#include <stdbool.h>
#include <stddef.h>

/* The numeric states match GhosttyTerminalProgressState. An omitted progress
 * is -1; otherwise it is a percentage from 0 to 100. */
typedef struct DesktopSignalProgress {
    int state;
    int progress;
} DesktopSignalProgress;

/* Parse the complete OSC payload, without ESC/OSC/BEL/ST: 9;4;state[;value].
 * Decimal fields must be nonempty and in range. No output is changed on
 * failure. This parser is pure; libghostty's callback is the runtime source. */
bool desktop_signal_parse_progress(const char *data, size_t length,
                                   DesktopSignalProgress *out);

/* Build a ToastGeneric XML document from UTF-8 byte strings. An empty title
 * becomes "termica". XML metacharacters are escaped and XML-disallowed control
 * characters become U+FFFD. Invalid UTF-8 is rejected. A NULL input is valid
 * only with zero length. On success, *xml is malloc-owned, NUL-terminated,
 * and *xml_length excludes the terminator; outputs are unchanged on failure. */
bool desktop_signal_notification_xml(const char *title, size_t title_length,
                                     const char *body, size_t body_length,
                                     char **xml, size_t *xml_length);

/* Call once on the UI thread before creating any windows. Sets the process
 * AppUserModelID without creating files. Return false on failure; callers may
 * still use taskbar progress. No matching shutdown is needed. */
bool desktop_signal_init(void);

/* The caller implements ask/allow/deny and must pass true only after approval.
 * An approved request creates or validates the per-user Start-menu shortcut
 * required for unpackaged Windows toast notifications; denied requests do not
 * touch it. Returns false if denied, invalid, unsupported, or a Windows API
 * fails. True means Show succeeded, not that the OS chose to display the toast
 * (Windows notification settings / focus assist may suppress it). Input is
 * borrowed. */
bool desktop_signal_notify(bool allowed, const char *title, size_t title_length,
                           const char *body, size_t body_length);

/* native_hwnd is the SDL window's SDL_PROP_WINDOW_WIN32_HWND_POINTER. Values
 * are GhosttyTerminalProgressReport.state and .progress. For -1, the current
 * taskbar percentage is left unchanged. Returns false on invalid arguments or
 * a Windows API failure. No policy is applied to taskbar progress. */
bool desktop_signal_progress(void *native_hwnd, int state, int progress);

/* Called by --selftest; prints checks and returns the number of failures. */
int desktop_signal_selftest(void);

#endif
