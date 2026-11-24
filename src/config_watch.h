/* Directory monitoring and comment-preserving edits for configuration files. */
#ifndef TERMICA_CONFIG_WATCH_H
#define TERMICA_CONFIG_WATCH_H

#include <stdbool.h>
#include <stddef.h>

enum {
    CONFIG_WATCH_SETTINGS = 1u << 0,
    CONFIG_WATCH_STATUSBAR = 1u << 1
};

typedef struct ConfigWatch ConfigWatch;

/* Start after SDL initialization. The directory is watched even if either file
 * does not yet exist. Poll on the main thread (e.g. on each timer wake), and
 * perform reloads/UI work there. Stop before SDL shutdown. */
ConfigWatch *config_watch_start(void);
unsigned config_watch_take(ConfigWatch *watch);
void config_watch_stop(ConfigWatch *watch);

/* Update one key in the latest on-disk file, preserving unrelated bytes and
 * comments. section is a simple table name or "" for the root; value is one
 * complete TOML value (not a raw string). Returns false on invalid/unsupported
 * input, I/O failure or an observed concurrent edit. No write is made if
 * unchanged. config_quote_string produces a TOML basic string for string keys. */
bool config_quote_string(const char *text, char *out, size_t capacity);
bool config_update_text(const char *text, size_t length, const char *section,
                        const char *key, const char *value,
                        char *out, size_t capacity);
bool config_update_key(const char *path, const char *section,
                       const char *key, const char *value);

#endif
