/* Exercise the real validation/open path with an intercepted desktop API.
 * No browser or desktop service is launched by this self-test. */
#ifndef _WIN32
#define hyperlink_open test_hyperlink_open
#define hyperlink_uri_allowed test_hyperlink_uri_allowed
#define hyperlink_contains test_hyperlink_contains
#define hyperlink_at test_hyperlink_at
#define SDL_OpenURL test_desktop_open_url
#include "hyperlink.c"
#undef SDL_OpenURL

#include <stdio.h>

static char opened_uri[HYPERLINK_URI_MAX + 1];
static int open_calls;

bool SDLCALL test_desktop_open_url(const char *url)
{
    ++open_calls;
    snprintf(opened_uri, sizeof opened_uri, "%s", url);
    return true;
}

int hyperlink_posix_selftest(void)
{
    static const char uri[] = "https://example.test/$(touch${IFS}owned);&x=\xc3\xa9";
    int failures = 0;
    open_calls = 0;
    opened_uri[0] = '\0';
    if (!test_hyperlink_open(uri) || open_calls != 1 || strcmp(opened_uri, uri) != 0) {
        printf("FAIL  POSIX hyperlink passes the exact URI as one desktop API argument\n");
        ++failures;
    }
    if (test_hyperlink_open("file:///tmp/owned") ||
        test_hyperlink_open("https://example.test/unsafe\nuri") || open_calls != 1) {
        printf("FAIL  POSIX hyperlink rejects unsafe URIs before the desktop API\n");
        ++failures;
    }
    return failures;
}
#else
int hyperlink_posix_selftest(void) { return 0; }
#endif
