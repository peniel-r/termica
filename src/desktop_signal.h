/* Desktop notifications and taskbar/Dock progress. All platform calls belong
 * on the UI thread, never inside a libghostty callback. Input buffers are not
 * retained between calls. */
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

/* Call once on the UI thread before creating any windows. On Windows this
 * sets the process AppUserModelID without creating files. No matching shutdown
 * is needed. */
bool desktop_signal_init(void);

/* The caller implements ask/allow/deny and must pass true only after approval.
 * Windows creates or validates the per-user Start-menu shortcut needed for
 * unpackaged toasts only after approval. Linux uses the freedesktop session
 * notification service; macOS submits to the native notification center.
 * Returns false if denied, invalid, unsupported or the platform API fails.
 * On Windows and Linux true means the platform accepted the request. On macOS
 * it means the authorization request was queued; permission can be denied
 * asynchronously. No platform guarantees display. Input is borrowed. macOS
 * notification availability depends on application identity and settings. */
bool desktop_signal_notify(bool allowed, const char *title, size_t title_length,
                           const char *body, size_t body_length);

/* On Windows native_hwnd is SDL_PROP_WINDOW_WIN32_HWND_POINTER; on other
 * platforms it is ignored. Values are GhosttyTerminalProgressReport.state and
 * .progress. For -1, the current percentage is left unchanged. Returns false
 * on invalid arguments, an unsupported platform, or a platform API failure.
 * macOS draws an app-wide Dock tile; Linux has no standard taskbar protocol.
 * No policy is applied to taskbar progress. */
bool desktop_signal_progress(void *native_hwnd, int state, int progress);

/* Called by --selftest; prints checks and returns the number of failures. */
int desktop_signal_selftest(void);

#endif
