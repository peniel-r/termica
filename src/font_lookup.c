/*
 * termica - resolve a font setting to a file on disk. See font_lookup.h.
 *
 * SDL_ttf wants a path; people naturally write the family name they see in
 * a font picker. On Windows the installed fonts are listed in the same
 * registry keys the font control panel writes:
 *
 *   HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts
 *
 * Each value is "<Display Name> (TrueType)" -> "<file or path>". The display
 * name is normally "<Family> <Style>", so a family name matches either the
 * whole name or a prefix followed by a known style word. Regular wins over
 * the other styles because it is the face the app treats as the base.
 *
 * Relative file names (system fonts are stored that way) are looked for in
 * %WINDIR%\Fonts and %LOCALAPPDATA%\Microsoft\Windows\Fonts.
 *
 * Strict C99. The registry half is Windows-only; elsewhere only a name that
 * is already a readable file resolves.
 */

#include "font_lookup.h"

#include <SDL3/SDL_filesystem.h>

#include <string.h>

#ifdef _WIN32

#include <limits.h>
#include <windows.h>

#define FONT_NAME_MAX 512
#define FONT_PATH_MAX 1024

#define FONT_REGISTRY_KEY \
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"

typedef struct FontMatch {
    int rank;                    /* -1 = nothing found yet              */
    wchar_t path[FONT_PATH_MAX];
} FontMatch;

static bool copy_wide(const wchar_t *text, wchar_t *out, size_t capacity)
{
    size_t length = wcslen(text);

    if (length >= capacity)
        return false;
    memcpy(out, text, (length + 1) * sizeof(wchar_t));
    return true;
}

static bool utf8_to_wide(const char *text, wchar_t *out, size_t capacity)
{
    int length;

    if (capacity > (size_t)INT_MAX)
        return false;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out,
                                 (int)capacity);
    return length > 0;
}

static bool wide_to_utf8(const wchar_t *text, char *out, size_t capacity)
{
    int length;

    if (capacity > (size_t)INT_MAX)
        return false;
    length = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)capacity, NULL,
                                 NULL);
    return length > 0;
}

static bool file_exists_wide(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void trim_wide(wchar_t *text)
{
    size_t length = wcslen(text);
    size_t start = 0;

    while (text[start] == L' ' || text[start] == L'\t')
        start++;
    while (length > start &&
           (text[length - 1] == L' ' || text[length - 1] == L'\t'))
        length--;
    if (start > 0)
        memmove(text, text + start, (length - start) * sizeof(wchar_t));
    text[length - start] = L'\0';
}

/* The registry appends the font type to the display name. */
static void strip_display_suffix(wchar_t *display)
{
    static const wchar_t *const SUFFIXES[] = {
        L" (TrueType)", L" (OpenType)", L" (Type 1)", NULL
    };
    size_t length = wcslen(display);
    int index;

    for (index = 0; SUFFIXES[index]; index++) {
        size_t suffix_length = wcslen(SUFFIXES[index]);

        if (length >= suffix_length &&
            _wcsicmp(display + length - suffix_length, SUFFIXES[index]) == 0) {
            display[length - suffix_length] = L'\0';
            return;
        }
    }
    /* Unknown spelling of the type: a trailing parenthetical is not part of
     * the family name either. */
    {
        wchar_t *open = wcsrchr(display, L'(');

        if (open && open > display && open[-1] == L' ')
            open[-1] = L'\0';
    }
}

static bool is_style_word(const wchar_t *style)
{
    static const wchar_t *const STYLES[] = {
        L"Bold", L"Italic", L"Bold Italic", L"BoldItalic", L"Oblique",
        L"Bold Oblique", L"Light", L"ExtraLight", L"Extra Light",
        L"SemiBold", L"Semi Bold", L"DemiBold", L"Demi Bold", L"Medium",
        L"Black", L"Heavy", L"Thin", L"UltraLight", L"Ultra Light",
        L"Condensed", L"Narrow", L"Stencil", L"Rounded", NULL
    };
    int index;

    for (index = 0; STYLES[index]; index++) {
        if (_wcsicmp(style, STYLES[index]) == 0)
            return true;
    }
    return false;
}

/* 0 = the name is the family itself, 1 = the regular face, 2 = another
 * style of the family, -1 = not this family. */
static int family_match_rank(const wchar_t *display, const wchar_t *family)
{
    size_t family_length = wcslen(family);
    size_t display_length = wcslen(display);
    const wchar_t *style;

    if (display_length == family_length &&
        _wcsicmp(display, family) == 0)
        return 0;
    if (display_length <= family_length ||
        _wcsnicmp(display, family, family_length) != 0)
        return -1;
    if (display[family_length] != L' ' && display[family_length] != L'\t')
        return -1;

    style = display + family_length;
    while (*style == L' ' || *style == L'\t')
        style++;
    if (_wcsicmp(style, L"Regular") == 0 ||
        _wcsicmp(style, L"Book") == 0 ||
        _wcsicmp(style, L"Normal") == 0)
        return 1;
    if (is_style_word(style))
        return 2;
    return -1;
}

static bool is_absolute_path(const wchar_t *path)
{
    if ((path[0] == L'\\' && path[1] == L'\\') || path[0] == L'\\')
        return true;
    return ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':';
}

static bool join_font_path(const wchar_t *directory, const wchar_t *file,
                           wchar_t *out, size_t capacity)
{
    int written = swprintf(out, capacity, L"%ls\\%ls", directory, file);

    return written > 0 && (size_t)written < capacity && file_exists_wide(out);
}

/* Turn a registry data value into an existing file. */
static bool resolve_font_value(const wchar_t *value, wchar_t *out,
                               size_t capacity)
{
    wchar_t windows[MAX_PATH];
    wchar_t local[MAX_PATH];
    wchar_t fonts[MAX_PATH];
    DWORD length;

    if (!value || !value[0] || wcslen(value) >= capacity)
        return false;
    if (is_absolute_path(value)) {
        if (!file_exists_wide(value))
            return false;
        return copy_wide(value, out, capacity);
    }

    length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length > 0 && length < MAX_PATH &&
        swprintf(fonts, MAX_PATH, L"%ls\\Fonts", windows) > 0 &&
        join_font_path(fonts, value, out, capacity))
        return true;

    length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length > 0 && length < MAX_PATH &&
        swprintf(fonts, MAX_PATH, L"%ls\\Microsoft\\Windows\\Fonts", local) > 0 &&
        join_font_path(fonts, value, out, capacity))
        return true;

    return false;
}

static void scan_font_key(HKEY root, const wchar_t *family, FontMatch *best)
{
    HKEY key;
    DWORD index = 0;

    if (RegOpenKeyExW(root, FONT_REGISTRY_KEY, 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return;

    for (;;) {
        wchar_t name[FONT_NAME_MAX];
        wchar_t data[FONT_PATH_MAX];
        wchar_t expanded[FONT_PATH_MAX];
        wchar_t display[FONT_NAME_MAX];
        wchar_t resolved[FONT_PATH_MAX];
        DWORD name_length = FONT_NAME_MAX;
        DWORD data_length = (DWORD)(sizeof data - sizeof(wchar_t));
        DWORD type = 0;
        DWORD expanded_length;
        const wchar_t *value;
        LONG status;
        int rank;

        status = RegEnumValueW(key, index, name, &name_length, NULL, &type,
                               (LPBYTE)data, &data_length);
        index++;
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ)
            continue;

        /* RegEnumValueW does not promise a terminator. */
        data[data_length / sizeof(wchar_t)] = L'\0';
        value = data;
        if (type == REG_EXPAND_SZ) {
            expanded_length = ExpandEnvironmentStringsW(data, expanded,
                                                        FONT_PATH_MAX);
            if (expanded_length == 0 || expanded_length > FONT_PATH_MAX)
                continue;
            value = expanded;
        }

        if (!copy_wide(name, display, FONT_NAME_MAX))
            continue;
        strip_display_suffix(display);
        trim_wide(display);
        rank = family_match_rank(display, family);
        if (rank < 0)
            continue;
        if (best->rank >= 0 && rank >= best->rank)
            continue;
        if (!resolve_font_value(value, resolved, FONT_PATH_MAX))
            continue;

        best->rank = rank;
        copy_wide(resolved, best->path, FONT_PATH_MAX);
    }
    RegCloseKey(key);
}

static bool lookup_windows(const char *name, char *out, size_t capacity)
{
    wchar_t family[FONT_NAME_MAX];
    FontMatch best;

    if (!utf8_to_wide(name, family, FONT_NAME_MAX))
        return false;
    trim_wide(family);
    if (!family[0])
        return false;

    best.rank = -1;
    best.path[0] = L'\0';
    scan_font_key(HKEY_CURRENT_USER, family, &best);
    scan_font_key(HKEY_LOCAL_MACHINE, family, &best);
    if (best.rank < 0)
        return false;
    return wide_to_utf8(best.path, out, capacity);
}

#endif /* _WIN32 */

bool font_lookup_path(const char *name, char *out, size_t capacity)
{
    SDL_PathInfo info;
    size_t length;

    if (!name || !name[0] || !out || capacity == 0)
        return false;

    /* A path that already names a file wins, and is returned untouched. */
    if (SDL_GetPathInfo(name, &info) && info.type == SDL_PATHTYPE_FILE) {
        length = strlen(name);
        if (length >= capacity)
            return false;
        memcpy(out, name, length + 1);
        return true;
    }

#ifdef _WIN32
    return lookup_windows(name, out, capacity);
#else
    return false;
#endif
}
