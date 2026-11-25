/* End-to-end watcher checks, isolated from the user's configuration. */
#if defined(__linux__) || defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#include "config_watch.h"
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool write_config(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    bool ok;
    if (!f) return false;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool expect_changes(ConfigWatch *watch, unsigned expected)
{
    struct timespec delay = {0, 2000000};
    unsigned seen = 0;
    int i;
    for (i = 0; i < 100; ++i) {
        seen |= config_watch_take(watch);
        if ((seen & expected) == expected) return true;
        nanosleep(&delay, NULL);
    }
    return false;
}

int config_watch_posix_selftest(void)
{
    char base[] = "/tmp/termica-watch-XXXXXX";
    char directory[256], old_directory[256], settings[256], status[256], temporary[256];
    char old_settings[256], old_status[256];
    const char *original = SDL_getenv("XDG_CONFIG_HOME");
    char *saved = NULL;
    ConfigWatch *watch = NULL;
    int failures = 0;
    if (original) {
        saved = malloc(strlen(original) + 1);
        if (!saved) return 1;
        strcpy(saved, original);
    }
    if (!mkdtemp(base)) { free(saved); return 1; }
    snprintf(directory, sizeof directory, "%s/termica", base);
    snprintf(old_directory, sizeof old_directory, "%s/old", base);
    snprintf(settings, sizeof settings, "%s/termica/termica.toml", base);
    snprintf(status, sizeof status, "%s/termica/statusbar.toml", base);
    snprintf(temporary, sizeof temporary, "%s/termica/replacement", base);
    snprintf(old_settings, sizeof old_settings, "%s/old/termica.toml", base);
    snprintf(old_status, sizeof old_status, "%s/old/statusbar.toml", base);
    if (!SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "XDG_CONFIG_HOME", base, true) ||
        !(watch = config_watch_start())) {
        printf("FAIL  POSIX configuration watcher starts\n");
        ++failures;
        goto done;
    }
    if (!write_config(settings, "first") || !expect_changes(watch, CONFIG_WATCH_SETTINGS)) {
        printf("FAIL  POSIX watcher observes creation of a missing settings file\n");
        ++failures;
    }
    if (!write_config(status, "status") || !expect_changes(watch, CONFIG_WATCH_STATUSBAR)) {
        printf("FAIL  POSIX watcher observes creation of a missing statusbar file\n");
        ++failures;
    }
    if (!write_config(settings, "modified") || !expect_changes(watch, CONFIG_WATCH_SETTINGS)) {
        printf("FAIL  POSIX watcher observes in-place writes\n");
        ++failures;
    }
    if (!write_config(temporary, "replacement") || rename(temporary, settings) != 0 ||
        !expect_changes(watch, CONFIG_WATCH_SETTINGS)) {
        printf("FAIL  POSIX watcher observes atomic replacement\n");
        ++failures;
    }
    if (unlink(status) != 0 || !expect_changes(watch, CONFIG_WATCH_STATUSBAR)) {
        printf("FAIL  POSIX watcher observes deletion\n");
        ++failures;
    }
    if (rename(directory, old_directory) != 0 ||
        !expect_changes(watch, CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR) ||
        mkdir(directory, 0700) != 0 || !write_config(settings, "new directory") ||
        !expect_changes(watch, CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR)) {
        printf("FAIL  POSIX watcher recovers after directory replacement\n");
        ++failures;
    }
done:
    config_watch_stop(watch);
    if (saved) {
        (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "XDG_CONFIG_HOME", saved, true);
        free(saved);
    } else {
        (void)SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "XDG_CONFIG_HOME");
    }
    (void)unlink(settings);
    (void)unlink(status);
    (void)unlink(temporary);
    (void)unlink(old_settings);
    (void)unlink(old_status);
    (void)rmdir(directory);
    (void)rmdir(old_directory);
    (void)rmdir(base);
    return failures;
}
#else
int config_watch_posix_selftest(void) { return 0; }
#endif
