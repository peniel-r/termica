/* Pure parser and notification-content checks, included in --selftest. */
#include "desktop_signal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) ++failures;
}

static bool parses(const char *text, int state, int progress)
{
    DesktopSignalProgress result = {-9, -9};
    return desktop_signal_parse_progress(text, strlen(text), &result) &&
           result.state == state && result.progress == progress;
}

static bool rejects(const char *text)
{
    DesktopSignalProgress result = {-9, -9};
    return !desktop_signal_parse_progress(text, strlen(text), &result) &&
           result.state == -9 && result.progress == -9;
}

int desktop_signal_selftest(void)
{
    DesktopSignalProgress result = {-9, -9};
    char *xml = NULL;
    size_t xml_length = 0;
    static const char controls[] = {'A', '\0', '\1', '\t', '\n', '\r', 'B'};
    static const char invalid[] = {(char)0xf0, (char)0x80, (char)0x80, (char)0x80};
    static const char valid[] = "h\xc3\xa9 \xf0\x9f\x98\x80";
    failures = 0;
    check(parses("9;4;0", 0, -1) && parses("9;4;1;0", 1, 0) &&
          parses("9;4;1;100", 1, 100), "OSC 9;4 clear and progress boundaries");
    check(parses("9;4;2;45", 2, 45) && parses("9;4;3", 3, -1) &&
          parses("9;4;4;99", 4, 99) && parses("9;4;4", 4, -1),
          "OSC 9;4 error, indeterminate, paused and omitted values");
    check(parses("9;4;01;001", 1, 1), "OSC 9;4 decimal leading zeroes");
    check(rejects("") && rejects("9;4;") && rejects("9;4;5") &&
          rejects("9;4;-1") && rejects("9;4;1;") &&
          rejects("9;4;1;-1") && rejects("9;4;1;101") &&
          rejects("9;4;99999999999999999999999999999999"),
          "OSC 9;4 rejects missing, negative, oversized and overflow values");
    check(rejects("9;4;1;1;") && rejects("9;4;1;1junk") &&
          rejects("9;4;1 1") && rejects("9;4; 1") && rejects("9;5;1") &&
          rejects("\033]9;4;1\a") &&
          !desktop_signal_parse_progress("9;4;1", 4, &result) &&
          !desktop_signal_parse_progress(NULL, 5, &result) &&
          !desktop_signal_parse_progress("9;4;1", 5, NULL),
          "OSC 9;4 requires an exact bounded payload");
    check(desktop_signal_notification_xml("<&>", 3, "&<tag>", 6, &xml, &xml_length) &&
          strlen(xml) == xml_length && strstr(xml, "<text>&lt;&amp;&gt;</text>") &&
          strstr(xml, "<text>&amp;&lt;tag&gt;</text>"),
          "toast content escapes XML and cannot inject elements");
    free(xml); xml = NULL;
    check(desktop_signal_notification_xml(NULL, 0, controls, sizeof(controls), &xml, &xml_length) &&
          strstr(xml, "<text>termica</text>") &&
          strstr(xml, "A\xef\xbf\xbd\xef\xbf\xbd\t\n\rB"),
          "toast empty title and XML-illegal controls");
    free(xml); xml = NULL;
    check(desktop_signal_notification_xml(valid, sizeof(valid) - 1, NULL, 0, &xml, &xml_length) &&
          strstr(xml, valid), "toast retains Unicode and accepts empty body");
    free(xml); xml = NULL;
    check(desktop_signal_notification_xml("\xef\xbf\xbe\xef\xbf\xbf", 6,
                                          NULL, 0, &xml, &xml_length) &&
          strstr(xml, "\xef\xbf\xbd\xef\xbf\xbd"),
          "toast replaces XML 1.0 noncharacters");
    free(xml); xml = NULL;
    check(!desktop_signal_notification_xml(invalid, sizeof(invalid), NULL, 0, &xml, &xml_length) &&
          !desktop_signal_notification_xml(NULL, 1, NULL, 0, &xml, &xml_length) &&
          !desktop_signal_notification_xml("\xed\xa0\x80", 3, NULL, 0, &xml, &xml_length) &&
          !desktop_signal_notification_xml("\xc2", 1, NULL, 0, &xml, &xml_length) &&
          xml == NULL, "toast rejects invalid UTF-8 and invalid buffers");
    return failures;
}
