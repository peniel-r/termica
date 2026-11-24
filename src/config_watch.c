/* Directory monitoring and targeted TOML edits. UI ownership stays on the
 * main thread: the overlapped directory request is only harvested by take(). */
#include "config_watch.h"
#include "settings.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <wchar.h>

struct ConfigWatch {
    HANDLE directory;
    HANDLE event;
    OVERLAPPED operation;
    union { DWORD align; unsigned char bytes[16384]; } buffer;
    bool pending;
};

static bool watch_arm(ConfigWatch *watch)
{
    memset(&watch->operation, 0, sizeof watch->operation);
    watch->operation.hEvent = watch->event;
    ResetEvent(watch->event);
    watch->pending = ReadDirectoryChangesW(watch->directory, watch->buffer.bytes,
        (DWORD)sizeof watch->buffer.bytes, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
        NULL, &watch->operation, NULL) != 0;
    return watch->pending;
}

ConfigWatch *config_watch_start(void)
{
    char path[512], *slash;
    wchar_t directory[512];
    ConfigWatch *watch;
    if (!settings_path(path, sizeof path)) return NULL;
    config_create_directory(path);
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    if (!slash) return NULL;
    *slash = '\0';
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                             directory, (int)(sizeof directory / sizeof directory[0])))
        return NULL;
    watch = (ConfigWatch *)calloc(1, sizeof *watch);
    if (!watch) return NULL;
    watch->directory = CreateFileW(directory, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (watch->directory == INVALID_HANDLE_VALUE) { free(watch); return NULL; }
    watch->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!watch->event || !watch_arm(watch)) {
        if (watch->event) CloseHandle(watch->event);
        CloseHandle(watch->directory);
        free(watch);
        return NULL;
    }
    return watch;
}

unsigned config_watch_take(ConfigWatch *watch)
{
    DWORD bytes = 0;
    unsigned flags = 0;
    if (!watch) return 0;
    if (!watch->pending) {
        if (!watch_arm(watch)) return 0;
        return CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR;
    }
    if (!GetOverlappedResult(watch->directory, &watch->operation, &bytes, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return 0;
        watch->pending = false;
        watch_arm(watch);
        return CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR;
    }
    watch->pending = false;
    if (!bytes) flags = CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR;
    else {
        DWORD offset = 0;
        while (offset < bytes) {
            FILE_NOTIFY_INFORMATION *entry = (FILE_NOTIFY_INFORMATION *)(watch->buffer.bytes + offset);
            size_t count = entry->FileNameLength / sizeof(wchar_t);
            if (count == 12 && _wcsnicmp(entry->FileName, L"termica.toml", count) == 0)
                flags |= CONFIG_WATCH_SETTINGS;
            else if (count == 14 && _wcsnicmp(entry->FileName, L"statusbar.toml", count) == 0)
                flags |= CONFIG_WATCH_STATUSBAR;
            if (!entry->NextEntryOffset || entry->NextEntryOffset > bytes - offset) break;
            offset += entry->NextEntryOffset;
        }
    }
    if (!watch_arm(watch)) flags |= CONFIG_WATCH_SETTINGS | CONFIG_WATCH_STATUSBAR;
    return flags;
}

void config_watch_stop(ConfigWatch *watch)
{
    DWORD ignored;
    if (!watch) return;
    if (watch->pending) {
        CancelIoEx(watch->directory, &watch->operation);
        GetOverlappedResult(watch->directory, &watch->operation, &ignored, TRUE);
    }
    CloseHandle(watch->event);
    CloseHandle(watch->directory);
    free(watch);
}
#else
struct ConfigWatch { int unused; };
ConfigWatch *config_watch_start(void) { return NULL; }
unsigned config_watch_take(ConfigWatch *watch) { (void)watch; return 0; }
void config_watch_stop(ConfigWatch *watch) { (void)watch; }
#endif

#define CONFIG_MAX_BYTES 65536

bool config_quote_string(const char *text, char *out, size_t capacity)
{
    size_t i, n = 0;
    static const char hex[] = "0123456789ABCDEF";
    if (!text || !out || capacity < 3) return false;
    out[n++] = '"';
    for (i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            if (n + 2 >= capacity) return false;
            out[n++] = '\\'; out[n++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            if (n + 6 >= capacity) return false;
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
        } else {
            if (n + 1 >= capacity) return false;
            out[n++] = (char)c;
        }
    }
    if (n + 2 > capacity) return false;
    out[n++] = '"'; out[n] = '\0';
    return true;
}

static bool folded_equal(const char *a, size_t n, const char *b)
{
    size_t i = 0, j = 0;
    while (i < n || b[j]) {
        unsigned char x, y;
        while (i < n && (a[i] == '-' || a[i] == '_')) i++;
        while (b[j] == '-' || b[j] == '_') j++;
        if (i == n || !b[j]) return i == n && !b[j];
        x = (unsigned char)a[i++]; y = (unsigned char)b[j++];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}

static bool simple_name(const char *s)
{
    size_t i;
    if (!s || !*s) return false;
    for (i = 0; s[i]; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}

static size_t skip_space(const char *s, size_t p, size_t end)
{
    while (p < end && (s[p] == ' ' || s[p] == '\t')) p++;
    return p;
}

/* Read a simple bare or quoted TOML key without escapes. Unsupported forms
 * are never rewritten: the parsed tree below catches a missed existing key. */
static bool read_name(const char *s, size_t *p, size_t end, size_t *begin, size_t *n)
{
    char quote = 0;
    *p = skip_space(s, *p, end);
    if (*p < end && (s[*p] == '"' || s[*p] == '\'')) quote = s[(*p)++];
    *begin = *p;
    if (quote) {
        while (*p < end && s[*p] != quote && s[*p] != '\\') (*p)++;
        if (*p == end || s[*p] != quote) return false;
        *n = *p - *begin; (*p)++;
    } else {
        while (*p < end && ((s[*p] >= 'a' && s[*p] <= 'z') ||
               (s[*p] >= 'A' && s[*p] <= 'Z') || (s[*p] >= '0' && s[*p] <= '9') ||
               s[*p] == '-' || s[*p] == '_')) (*p)++;
        *n = *p - *begin;
    }
    return *n != 0;
}

/* Find the end of one value, respecting strings, nested arrays/tables and
 * comments. Leading/trailing whitespace is left untouched by the replacement. */
static size_t value_end(const char *s, size_t p, size_t end)
{
    int depth = 0;
    char quote = 0;
    bool triple = false, escape = false, comment = false;
    size_t last = p;
    for (; p < end; p++) {
        char c = s[p];
        if (comment) { if (c == '\n') comment = false; else continue; }
        if (quote) {
            last = p + 1;
            if (escape) { escape = false; continue; }
            if (quote == '"' && c == '\\') { escape = true; continue; }
            if (c == quote) {
                if (triple) {
                    if (p + 2 < end && s[p + 1] == quote && s[p + 2] == quote) {
                        p += 2; last = p + 1; quote = 0; triple = false;
                    }
                } else quote = 0;
            }
            continue;
        }
        if (c == '#' && depth == 0) break;
        if (c == '#') { comment = true; continue; }
        if ((c == '\n' || c == '\r') && depth == 0) break;
        if (depth == 0 && (c == ',' || c == ']' || c == '}')) break;
        if (c == '"' || c == '\'') {
            quote = c;
            triple = p + 2 < end && s[p + 1] == c && s[p + 2] == c;
            if (triple) p += 2;
            last = p + 1;
        } else if (c == '[' || c == '{') { depth++; last = p + 1; }
        else if (c == ']' || c == '}') { depth--; last = p + 1; }
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') last = p + 1;
    }
    return last;
}

static bool tree_has(toml_datum_t table, const char *key, toml_datum_t *out)
{
    int i;
    if (table.type != TOML_TABLE) return false;
    for (i = 0; i < table.u.tab.size; i++) {
        const char *name = table.u.tab.key[i];
        if (folded_equal(name, strlen(name), key)) {
            if (out) *out = table.u.tab.value[i];
            return true;
        }
    }
    return false;
}

static bool source_offset(const char *text, size_t length, toml_datum_t value,
                          size_t *offset)
{
    size_t p = 0;
    int line = 1;
    if (value.lineno < 1 || value.colno < 1) return false;
    while (line < value.lineno && p < length)
        if (text[p++] == '\n') line++;
    if (line != value.lineno || (size_t)(value.colno - 1) > length - p) return false;
    p += (size_t)(value.colno - 1);
    if (p >= length) return false;
    *offset = p;
    return true;
}

bool config_update_text(const char *text, size_t length, const char *section,
                        const char *key, const char *value,
                        char *out, size_t capacity)
{
    toml_result_t doc, val, result;
    toml_datum_t table, existing;
    char *probe;
    size_t value_len, p = 0, replace_start = 0, replace_end = 0;
    size_t insertion = length, root_insertion = length, header_start = length;
    size_t b, n, new_len;
    bool in_section, saw_section, exists, found = false;
    bool header_found = false;
    const char *newline = "\n";
    if (!text || !section || !simple_name(key) || (*section && !simple_name(section)) ||
        !value || !out || length > CONFIG_MAX_BYTES) return false;
    value_len = strlen(value);
    if (value_len > CONFIG_MAX_BYTES) return false;
    probe = (char *)malloc(value_len + 5);
    if (!probe) return false;
    memcpy(probe, "x = ", 4); memcpy(probe + 4, value, value_len);
    val = toml_parse(probe, (int)(value_len + 4));
    free(probe);
    if (!val.ok || val.toptab.u.tab.size != 1) { toml_free(val); return false; }
    toml_free(val);
    doc = toml_parse(text, (int)length);
    if (!doc.ok) { toml_free(doc); return false; }
    table = doc.toptab;
    saw_section = !*section;
    if (*section && tree_has(doc.toptab, section, &table)) saw_section = true;
    else if (*section) memset(&table, 0, sizeof table);
    if (*section && saw_section && table.type != TOML_TABLE) {
        toml_free(doc);
        return false;
    }
    if (*section && table.type == TOML_TABLE && (table.flag & TOML_FLAG_EXPLICIT) &&
        source_offset(text, length, table, &header_start)) {
        while (header_start > 0 && text[header_start - 1] != '\n') header_start--;
    }
    exists = tree_has(table, key, &existing);
    if (exists) {
        if (!source_offset(text, length, existing, &replace_start)) {
            toml_free(doc); return false;
        }
        replace_end = value_end(text, replace_start, length);
        found = true;
    }
    in_section = !*section;
    if (memchr(text, '\r', length)) newline = "\r\n";
    while (!found && p < length) {
        size_t start = p, q, line_end;
        while (p < length && text[p] != '\n') p++;
        line_end = p;
        if (p < length) p++;
        if (line_end > start && text[line_end - 1] == '\r') line_end--;
        q = skip_space(text, start, line_end);
        if (q == line_end || text[q] == '#') continue;
        if (text[q] == '[') {
            if (root_insertion == length) root_insertion = start;
            bool matches = start == header_start;
            q++;
            if (!matches && q < line_end && text[q] != '[' &&
                read_name(text, &q, line_end, &b, &n)) {
                q = skip_space(text, q, line_end);
                matches = q < line_end && text[q] == ']' &&
                          folded_equal(text + b, n, section);
            }
            if (in_section && !matches && insertion == length) insertion = start;
            if (matches) header_found = true;
            in_section = matches;
            continue;
        }
        if (!read_name(text, &q, line_end, &b, &n)) continue;
        q = skip_space(text, q, line_end);
        if (q >= line_end || text[q] != '=') continue;
        q = skip_space(text, q + 1, length);
        replace_end = value_end(text, q, length);
        if (in_section && folded_equal(text + b, n, key)) {
            replace_start = q; found = true; break;
        }
        /* Skip physical lines inside multiline values, which are not keys. */
        if (replace_end > p) {
            p = replace_end;
            while (p < length && text[p] != '\n') p++;
            if (p < length) p++;
        }
    }
    if (exists && !found) { toml_free(doc); return false; }
    if (found) {
        new_len = length - (replace_end - replace_start) + value_len;
        if (new_len + 1 > capacity) { toml_free(doc); return false; }
        memcpy(out, text, replace_start);
        memcpy(out + replace_start, value, value_len);
        memcpy(out + replace_start + value_len, text + replace_end, length - replace_end);
    } else {
        char addition[1024];
        int added;
        const char *prefix;
        if (saw_section && *section && !header_found && table.type == TOML_TABLE &&
            (table.flag & TOML_FLAG_INLINED)) {
            size_t start, end;
            if (!source_offset(text, length, table, &start)) { toml_free(doc); return false; }
            end = value_end(text, start, length);
            if (end <= start || text[end - 1] != '}') { toml_free(doc); return false; }
            insertion = end - 1;
            added = snprintf(addition, sizeof addition, "%s%s = %s",
                             table.u.tab.size ? ", " : "", key, value);
        } else if (saw_section && *section && !header_found && table.type == TOML_TABLE) {
            int i;
            char quoted_section[512];
            const char *actual = NULL;
            for (i = 0; i < doc.toptab.u.tab.size; i++) {
                const char *name = doc.toptab.u.tab.key[i];
                if (folded_equal(name, strlen(name), section)) { actual = name; break; }
            }
            if (!actual || !config_quote_string(actual, quoted_section, sizeof quoted_section)) {
                toml_free(doc); return false;
            }
            insertion = root_insertion;
            prefix = insertion > 0 && text[insertion - 1] != '\n' ? newline : "";
            added = snprintf(addition, sizeof addition, "%s%s.%s = %s%s", prefix,
                             quoted_section, key, value, newline);
        } else {
            prefix = insertion > 0 && text[insertion - 1] != '\n' ? newline : "";
            if (!saw_section && *section)
                added = snprintf(addition, sizeof addition, "%s[%s]%s%s = %s%s", prefix, section, newline, key, value, newline);
            else
                added = snprintf(addition, sizeof addition, "%s%s = %s%s", prefix, key, value, newline);
        }
        if (added < 0 || (size_t)added >= sizeof addition) { toml_free(doc); return false; }
        new_len = length + (size_t)added;
        if (new_len + 1 > capacity) { toml_free(doc); return false; }
        memcpy(out, text, insertion);
        memcpy(out + insertion, addition, (size_t)added);
        memcpy(out + insertion + (size_t)added, text + insertion, length - insertion);
    }
    if (new_len > CONFIG_MAX_BYTES) { toml_free(doc); return false; }
    out[new_len] = '\0';
    toml_free(doc);
    result = toml_parse(out, (int)new_len);
    if (!result.ok) { toml_free(result); return false; }
    toml_free(result);
    return true;
}

bool config_update_key(const char *path, const char *section,
                       const char *key, const char *value)
{
    SDL_PathInfo info;
    char *text = NULL, *current = NULL, *updated = NULL;
    char temporary[640];
    size_t length = 0, current_length = 0, out_length;
    SDL_IOStream *stream = NULL;
    bool exists, ok = false;
    static unsigned serial;
    if (!path || !section || !key || !value) return false;
    exists = SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
    if (exists) {
        text = (char *)SDL_LoadFile(path, &length);
        if (!text || length > CONFIG_MAX_BYTES) goto done;
    } else {
        text = (char *)SDL_malloc(1);
        if (!text) goto done;
        text[0] = '\0';
    }
    updated = (char *)malloc(CONFIG_MAX_BYTES * 2 + 2048);
    if (!updated || !config_update_text(text, length, section, key, value,
                                       updated, CONFIG_MAX_BYTES * 2 + 2048)) goto done;
    out_length = strlen(updated);
    if (out_length == length && memcmp(text, updated, length) == 0) { ok = true; goto done; }
    config_create_directory(path);
    if (snprintf(temporary, sizeof temporary, "%s.termica-%llu-%u.tmp", path,
                 (unsigned long long)SDL_GetTicks(), ++serial) >= (int)sizeof temporary) goto done;
    stream = SDL_IOFromFile(temporary, "wb");
    if (!stream) goto done;
    if (SDL_WriteIO(stream, updated, out_length) != out_length) goto remove_temp;
    if (!SDL_CloseIO(stream)) { stream = NULL; goto remove_temp; }
    stream = NULL;
    if (exists) {
        current = (char *)SDL_LoadFile(path, &current_length);
        if (!current || current_length != length || memcmp(current, text, length)) goto remove_temp;
    } else if (SDL_GetPathInfo(path, &info)) goto remove_temp;
    if (!SDL_RenamePath(temporary, path)) goto remove_temp;
    ok = true;
    goto done;
remove_temp:
    if (stream) { SDL_CloseIO(stream); stream = NULL; }
    SDL_RemovePath(temporary);
done:
    SDL_free(current);
    SDL_free(text);
    free(updated);
    return ok;
}
