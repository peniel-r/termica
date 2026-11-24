#include "hyperlink.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_link(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) failures++;
}

static void put(Term *term, const char *text)
{
    term_write(term, text, strlen(text));
}

static bool allowed(const char *text)
{
    return hyperlink_uri_allowed(text, strlen(text));
}

static void test_schemes(void)
{
    static const char embedded_nul[] = { 'h','t','t','p',':','/','/',
                                         'a','\0','b' };
    static const char invalid_utf8[] = { 'h','t','t','p',':','/','/',
                                        'a','/',(char)0xc0,(char)0x80 };
    char huge[HYPERLINK_URI_MAX + 2];
    check_link(allowed("http://example.test/a") &&
               allowed("HTTPS://example.test:443/x") &&
               allowed("mailto:user@example.test?subject=Hi") &&
               allowed("https://[::1]:443/a") &&
               allowed("https://example.test/%20"),
               "hyperlink permits supported schemes and valid URI forms");
    check_link(!allowed("file:///C:/Windows") && !allowed("javascript:alert(1)") &&
               !allowed("http:example.test") && !allowed("http:///a") &&
               !allowed("https://user@/a") && !allowed("mailto:") &&
               !allowed("http://a:abc"),
               "hyperlink rejects unsupported schemes and missing hosts");
    check_link(!allowed("https://example.test/a\r\nb") &&
               !allowed("https://example.test/%0a") &&
               !allowed("https://example.test/%00") &&
               !allowed("https://example.test/%xy") &&
               !allowed("https://example.test/\\foo") &&
               !hyperlink_uri_allowed(embedded_nul, sizeof embedded_nul) &&
               !hyperlink_uri_allowed(invalid_utf8, sizeof invalid_utf8),
               "hyperlink rejects controls, invalid escapes and invalid UTF-8");
    check_link(!allowed("https://example.test/\xe2\x80\xa8"),
               "hyperlink rejects Unicode line separators");
    memset(huge, 'a', sizeof huge);
    memcpy(huge, "https://", 8);
    check_link(!hyperlink_uri_allowed(huge, sizeof huge),
               "hyperlink rejects oversized URIs without truncating them");
    check_link(!hyperlink_open("file:///C:/Windows") && !hyperlink_open(NULL),
               "open rejects an unsafe URI before calling the shell");
}

static void test_auto_links(void)
{
    Term term;
    Hyperlink link;
    const char *uri = "https://example.com/a_(b)";
    bool found;
    if (!term_init(&term, 80, 4)) {
        check_link(false, "automatic hyperlink test terminal initializes");
        return;
    }
    put(&term, "see (https://example.com/a_(b)), now");
    found = hyperlink_at(&term, 8, 0, &link);
    check_link(found && strcmp(link.uri, uri) == 0 && link.start_x == 5 &&
               link.end_x == 5 + (int)strlen(uri) - 1 &&
               link.start_y == 0 && link.end_y == 0,
               "automatic URL range strips surrounding punctuation only");
    check_link(found && hyperlink_contains(&link, 5, 0) &&
               hyperlink_contains(&link, link.end_x, 0) &&
               !hyperlink_contains(&link, 4, 0) &&
               !hyperlink_contains(&link, link.end_x + 1, 0) &&
               !hyperlink_at(&term, 4, 0, &link),
               "automatic URL click range excludes wrapper characters");
    check_link(term_selection_cells(&term, 0, 0, 2, 0, false) &&
               hyperlink_at(&term, 8, 0, &link) &&
               term_selection_has(&term),
               "hyperlink lookup does not disturb the terminal selection");
    check_link(!hyperlink_at(&term, -1, 0, &link) &&
               !hyperlink_at(&term, 80, 0, &link) &&
               !hyperlink_at(&term, 0, 4, &link),
               "hyperlink lookup rejects viewport coordinates out of bounds");
    term_free(&term);

    if (!term_init(&term, 80, 3)) {
        check_link(false, "multiple hyperlink test terminal initializes");
        return;
    }
    put(&term, "http://a HTTPS://b/X mailto:user@example.test");
    check_link(hyperlink_at(&term, 1, 0, &link) &&
               strcmp(link.uri, "http://a") == 0 &&
               hyperlink_at(&term, 12, 0, &link) &&
               strcmp(link.uri, "HTTPS://b/X") == 0 &&
               hyperlink_at(&term, 25, 0, &link) &&
               strcmp(link.uri, "mailto:user@example.test") == 0,
               "automatic lookup selects the hovered URL and recognizes schemes case-insensitively");
    term_free(&term);

    if (!term_init(&term, 80, 3)) {
        check_link(false, "adjacent hyperlink test terminal initializes");
        return;
    }
    put(&term, "(https://a),(https://b)");
    check_link(hyperlink_at(&term, 2, 0, &link) &&
               strcmp(link.uri, "https://a") == 0 && link.end_x == 9 &&
               hyperlink_at(&term, 16, 0, &link) &&
               strcmp(link.uri, "https://b") == 0 && link.start_x == 13,
               "adjacent parenthesized URLs have separate click ranges");
    term_free(&term);

    if (!term_init(&term, 80, 3)) {
        check_link(false, "Unicode hyperlink test terminal initializes");
        return;
    }
    put(&term, "https://e.test/\xe4\xb8\xad");
    check_link(hyperlink_at(&term, 16, 0, &link) &&
               strcmp(link.uri, "https://e.test/\xe4\xb8\xad") == 0 &&
               link.end_x == 16 && hyperlink_contains(&link, 15, 0),
               "automatic Unicode URL includes the full wide cell");
    term_free(&term);
}

static void test_wrapped_links(void)
{
    Term term;
    Hyperlink link;
    if (!term_init(&term, 10, 4)) {
        check_link(false, "wrapped hyperlink test terminal initializes");
        return;
    }
    put(&term, "https://example.com/a");
    check_link(hyperlink_at(&term, 2, 1, &link) &&
               strcmp(link.uri, "https://example.com/a") == 0 &&
               link.start_x == 0 && link.start_y == 0 &&
               link.end_x == 0 && link.end_y == 2 &&
               hyperlink_contains(&link, 9, 0) &&
               hyperlink_contains(&link, 0, 2) &&
               !hyperlink_contains(&link, 1, 2),
               "automatic URL crosses soft-wrapped rows with accurate cell range");
    term_free(&term);

    if (!term_init(&term, 10, 2)) {
        check_link(false, "scrolled hyperlink test terminal initializes");
        return;
    }
    put(&term, "https://example.com/\r\nnext");
    check_link(hyperlink_at(&term, 2, 0, &link) &&
               strcmp(link.uri, "https://example.com/") == 0 &&
               link.start_x == 0 && link.start_y == -1 &&
               link.end_x == 9 && link.end_y == 0,
               "automatic URL can start above the visible viewport");
    term_free(&term);

    if (!term_init(&term, 10, 3)) {
        check_link(false, "hard line break test terminal initializes");
        return;
    }
    put(&term, "https://ab\r\ncdef");
    check_link(hyperlink_at(&term, 3, 0, &link) &&
               strcmp(link.uri, "https://ab") == 0 &&
               link.end_x == 9 && link.end_y == 0 &&
               !hyperlink_at(&term, 0, 1, &link),
               "automatic URL does not cross a hard line break");
    term_free(&term);
}

static void test_osc8(void)
{
    Term term;
    Hyperlink link;
    if (!term_init(&term, 20, 3)) {
        check_link(false, "OSC 8 test terminal initializes");
        return;
    }
    put(&term, "x\x1b]8;;https://example.test/target\x1b\\label\x1b]8;;\x1b\\ y");
    check_link(hyperlink_at(&term, 2, 0, &link) &&
               strcmp(link.uri, "https://example.test/target") == 0 &&
               link.start_x == 1 && link.end_x == 5 &&
               link.start_y == 0 && link.end_y == 0 &&
               !hyperlink_contains(&link, 0, 0) &&
               !hyperlink_contains(&link, 6, 0),
               "OSC 8 label resolves to its URI and exact linked cells");
    term_free(&term);

    if (!term_init(&term, 30, 3)) {
        check_link(false, "disallowed OSC 8 test terminal initializes");
        return;
    }
    put(&term, "\x1b]8;;file:///C:/secret\x1b\\https://safe.test\x1b]8;;\x1b\\");
    check_link(!hyperlink_at(&term, 3, 0, &link),
               "disallowed OSC 8 target cannot fall through to its URL label");
    term_free(&term);

    if (!term_init(&term, 10, 3)) {
        check_link(false, "wrapped OSC 8 test terminal initializes");
        return;
    }
    put(&term, "12345678\x1b]8;;https://example.test\x1b\\abcd\x1b]8;;\x1b\\");
    check_link(hyperlink_at(&term, 0, 1, &link) &&
               strcmp(link.uri, "https://example.test") == 0 &&
               link.start_x == 8 && link.start_y == 0 &&
               link.end_x == 1 && link.end_y == 1,
               "OSC 8 range follows a wrapped label");
    term_free(&term);

    if (!term_init(&term, 20, 3)) {
        check_link(false, "wide OSC 8 test terminal initializes");
        return;
    }
    put(&term, "\x1b]8;;https://example.test\x1b\\"
               "A\xe4\xb8\xad" "B\x1b]8;;\x1b\\");
    check_link(hyperlink_at(&term, 2, 0, &link) &&
               strcmp(link.uri, "https://example.test") == 0 &&
               link.start_x == 0 && link.end_x == 3,
               "OSC 8 lookup covers the trailing half of a wide glyph");
    term_free(&term);
}

int hyperlink_selftest(void)
{
    failures = 0;
    test_schemes();
    test_auto_links();
    test_wrapped_links();
    test_osc8();
    return failures;
}
