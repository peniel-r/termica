/* Self-test coverage for transactional reload and targeted configuration edits. */
#include "config_watch.h"
#include "settings.h"
#include "statusbar.h"

#include <stdio.h>
#include <string.h>

static void config_check(bool condition, const char *description, int *failures)
{
    if (!condition) {
        printf("FAIL: %s\n", description);
        (*failures)++;
    }
}

int config_test(void)
{
    Settings settings;
    Settings ligatures;
    Statusbar statusbar;
    unsigned diff;
    bool changed;
    int failures = 0;
    char error[256], out[4096], quoted[128];
    const char *text;

    settings_default(&settings);
    config_check(settings.osc52 == SETTINGS_OSC52_DENY &&
                 settings.notifications == SETTINGS_OSC52_DENY &&
                 settings.scrollback_lines == SETTINGS_SCROLLBACK_DEFAULT &&
                 settings.ligatures,
                 "secure policies, ligatures and scrollback default", &failures);
    settings_default(&ligatures);
    text = "[font]\nligatures = false\n";
    config_check(settings_reload_text(&ligatures, text, strlen(text), &diff,
                                      error, sizeof error) &&
                 diff == SETTINGS_DIFF_LIGATURES && !ligatures.ligatures,
                 "font ligatures can be disabled", &failures);
    text = "[font]\nligatures = \"yes\"\n";
    config_check(!settings_reload_text(&ligatures, text, strlen(text), &diff,
                                       error, sizeof error) &&
                 !ligatures.ligatures,
                 "ligatures require a boolean", &failures);
    text = "[font]\nsize = 15\n[colors]\nscheme = \"light\"\n"
           "[terminal]\nscrollback_lines = 42\n";
    config_check(settings_reload_text(&settings, text, strlen(text), &diff,
                                      error, sizeof error) &&
                 diff == (SETTINGS_DIFF_FONT | SETTINGS_DIFF_COLORS |
                          SETTINGS_DIFF_TERMINAL) &&
                 settings.point_size == 15.0f && settings.scrollback_lines == 42,
                 "reload reports effective font, color and terminal changes", &failures);
    config_check(settings_reload_text(&settings, text, strlen(text), &diff,
                                      error, sizeof error) && diff == 0,
                 "unchanged reload has empty diff", &failures);
    text = "[font]\nsize = 18\n[terminal]\nscrollback_lines = -1\n";
    config_check(!settings_reload_text(&settings, text, strlen(text), &diff,
                                       error, sizeof error) &&
                 settings.point_size == 15.0f && settings.scrollback_lines == 42 &&
                 error[0] != '\0',
                 "invalid value rejects whole settings reload", &failures);
    text = "[font]\nsize = 12\n[terminal]\nosc52 = \"bad\"\n";
    config_check(!settings_reload_text(&settings, text, strlen(text), &diff,
                                       error, sizeof error) && settings.point_size == 15.0f,
                 "invalid policy leaves old settings", &failures);
    text = "[font\nsize = 12\n";
    config_check(!settings_reload_text(&settings, text, strlen(text), &diff,
                                       error, sizeof error) && settings.point_size == 15.0f,
                 "malformed reload leaves old settings", &failures);
    text = "# empty and unknown settings are valid\n[future]\nfoo = 1\n";
    config_check(settings_reload_text(&settings, text, strlen(text), &diff,
                                      error, sizeof error) &&
                 settings.point_size == 11.0f &&
                 settings.scrollback_lines == SETTINGS_SCROLLBACK_DEFAULT,
                 "valid unknown-only reload restores defaults", &failures);
    text = "[terminal]\nscrollback_lines = 1000001\n";
    config_check(!settings_reload_text(&settings, text, strlen(text), &diff,
                                       error, sizeof error),
                 "scrollback upper bound enforced", &failures);
    settings.scrollback_lines = SETTINGS_SCROLLBACK_MAX;
    settings_format(&settings, out, sizeof out);
    config_check(strstr(out, "scrollback_lines = 1000000") != NULL,
                 "scrollback formats", &failures);
    settings.ligatures = false;
    settings_format(&settings, out, sizeof out);
    config_check(strstr(out, "ligatures = false") != NULL,
                 "disabled ligatures format", &failures);

    statusbar_default(&statusbar);
    text = "[font]\nsize = 16\n[directory]\ntruncation_length = 2\n";
    config_check(statusbar_reload_text(&statusbar, text, strlen(text), &changed,
                                       error, sizeof error) && changed &&
                 statusbar.point_size == 16.0f,
                 "statusbar reload reports changes", &failures);
    text = "[font]\nsize = 12\n[directory]\ntruncation_length = -1\n";
    config_check(!statusbar_reload_text(&statusbar, text, strlen(text), &changed,
                                        error, sizeof error) &&
                 statusbar.point_size == 16.0f,
                 "invalid statusbar reload is transactional", &failures);
    text = "# blank\n";
    config_check(statusbar_reload_text(&statusbar, text, strlen(text), &changed,
                                       error, sizeof error) && changed &&
                 statusbar.point_size == 11.0f,
                 "empty statusbar reload restores defaults", &failures);

    text = "# header\r\n[window]\r\n  opacity = 0.8   # keep this\r\n"
           "backdrop = \"mica\" # and this\r\n[font]\r\nsize = 11\r\n";
    config_check(config_update_text(text, strlen(text), "window", "opacity", "0.5",
                                    out, sizeof out) &&
                 strcmp(out, "# header\r\n[window]\r\n  opacity = 0.5   # keep this\r\n"
                             "backdrop = \"mica\" # and this\r\n[font]\r\nsize = 11\r\n") == 0,
                 "targeted edit preserves comments, whitespace and CRLF", &failures);
    config_check(config_update_text(text, strlen(text), "window", "title_bar", "false",
                                    out, sizeof out) &&
                 strstr(out, "title_bar = false\r\n[font]") != NULL &&
                 strstr(out, "# keep this") != NULL,
                 "missing key inserted in existing section", &failures);
    text = "# leave intact\n[font]\nsize = 11\n";
    config_check(config_update_text(text, strlen(text), "window", "backdrop", "\"mica\"",
                                    out, sizeof out) &&
                 strstr(out, "# leave intact\n[font]\nsize = 11\n[window]\nbackdrop = \"mica\"\n") != NULL,
                 "missing section appended without canonical rewrite", &failures);
    text = "window = { opacity = 0.8 }\n";
    config_check(config_update_text(text, strlen(text), "window", "opacity", "0.5",
                                    out, sizeof out) &&
                 strcmp(out, "window = { opacity = 0.5 }\n") == 0,
                 "inline table value changed in place", &failures);
    config_check(config_update_text(text, strlen(text), "window", "title_bar", "false",
                                    out, sizeof out) &&
                 strstr(out, "title_bar = false") != NULL,
                 "missing key inserted in inline table", &failures);
    text = "window.opacity = 0.8 # comment\n[font]\nsize = 11\n";
    config_check(config_update_text(text, strlen(text), "window", "opacity", "0.5",
                                    out, sizeof out) &&
                 strstr(out, "window.opacity = 0.5 # comment") != NULL,
                 "dotted key changed in place", &failures);
    config_check(config_update_text(text, strlen(text), "window", "title_bar", "false",
                                    out, sizeof out) &&
                 strstr(out, "\"window\".title_bar = false\n[font]") != NULL,
                 "missing dotted key inserted before tables", &failures);
    text = "[\"win\\u0064ow\"] # quoted header\nother = 1\n[font]\nsize = 11\n";
    config_check(config_update_text(text, strlen(text), "window", "opacity", "0.5",
                                    out, sizeof out) &&
                 strstr(out, "opacity = 0.5\n[font]") != NULL &&
                 strstr(out, "# quoted header") != NULL,
                 "escaped table header receives new key", &failures);
    text = "[window]\nopacity = 0.8 # keep\n[font]\nfallback = [\n  \"# inside\",\n]\n";
    config_check(config_update_text(text, strlen(text), "window", "opacity", "0.5",
                                    out, sizeof out) &&
                 strstr(out, "opacity = 0.5 # keep") != NULL &&
                 strstr(out, "\"# inside\"") != NULL,
                 "multiline value and comment remain intact", &failures);
    config_check(config_quote_string("a\"b\\c\n", quoted, sizeof quoted) &&
                 strcmp(quoted, "\"a\\\"b\\\\c\\u000A\"") == 0,
                 "TOML string quoting", &failures);
    if (failures) printf("config tests: %d failure(s)\n", failures);
    return failures;
}
