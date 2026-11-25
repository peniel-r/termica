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
 * Linux uses fontconfig; macOS uses CoreText. Both retain collection face
 * indices so that a family inside a TTC opens as the intended font.
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

#if defined(__linux__)
#include <fontconfig/fontconfig.h>

static bool fontconfig_family_is(const FcPattern *font, const char *family)
{
    FcChar8 *candidate;
    int i;
    for (i = 0; FcPatternGetString(font, FC_FAMILY, i, &candidate) == FcResultMatch; i++) {
        if (FcStrCmpIgnoreCase(candidate, (const FcChar8 *)family) == 0)
            return true;
    }
    return false;
}

static bool lookup_fontconfig(const char *name, bool bold, bool italic,
                              char *out, size_t capacity, int *face_index)
{
    SDL_PathInfo info;
    FcPattern *query = NULL, *file_font = NULL;
    FcFontSet *fonts = NULL;
    const char *family = name;
    FcChar8 *file_family;
    FcResult result;
    bool generic, found = false;
    int i, count;

    if (SDL_GetPathInfo(name, &info) && info.type == SDL_PATHTYPE_FILE) {
        if (!bold && !italic) {
            size_t length = strlen(name);
            if (length >= capacity)
                return false;
            memcpy(out, name, length + 1);
            *face_index = 0;
            return true;
        }
        file_font = FcFreeTypeQuery((const FcChar8 *)name, 0, NULL, &count);
        if (!file_font || FcPatternGetString(file_font, FC_FAMILY, 0,
                                             &file_family) != FcResultMatch)
            goto done;
        family = (const char *)file_family;
    }
    generic = SDL_strcasecmp(family, "monospace") == 0 ||
              SDL_strcasecmp(family, "sans-serif") == 0 ||
              SDL_strcasecmp(family, "serif") == 0;
    query = FcPatternCreate();
    if (!query || !FcPatternAddString(query, FC_FAMILY, (const FcChar8 *)family) ||
        !FcPatternAddInteger(query, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR) ||
        !FcPatternAddInteger(query, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN) ||
        !FcConfigSubstitute(NULL, query, FcMatchPattern))
        goto done;
    FcDefaultSubstitute(query);
    fonts = FcFontSort(NULL, query, FcFalse, NULL, &result);
    if (!fonts)
        goto done;
    for (i = 0; i < fonts->nfont; i++) {
        FcPattern *font = fonts->fonts[i];
        FcChar8 *file;
        int weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN, index = 0;
        size_t length;

        if (!generic && !fontconfig_family_is(font, family))
            continue;
        (void)FcPatternGetInteger(font, FC_WEIGHT, 0, &weight);
        (void)FcPatternGetInteger(font, FC_SLANT, 0, &slant);
        if ((bold || italic) &&
            ((weight >= FC_WEIGHT_DEMIBOLD) != bold ||
             (slant != FC_SLANT_ROMAN) != italic))
            continue;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
            continue;
        (void)FcPatternGetInteger(font, FC_INDEX, 0, &index);
        length = strlen((const char *)file);
        if (length >= capacity)
            break;
        memcpy(out, file, length + 1);
        *face_index = index;
        found = true;
        break;
    }
done:
    if (fonts) FcFontSetDestroy(fonts);
    if (query) FcPatternDestroy(query);
    if (file_font) FcPatternDestroy(file_font);
    return found;
}

static bool fontconfig_codepoint(uint32_t codepoint, char *out, size_t capacity,
                                 int *face_index)
{
    FcPattern *query = FcPatternCreate();
    FcCharSet *charset = FcCharSetCreate();
    FcFontSet *fonts = NULL;
    FcResult result;
    bool found = false;
    int i;

    if (!query || !charset || !FcCharSetAddChar(charset, codepoint) ||
        !FcPatternAddCharSet(query, FC_CHARSET, charset) ||
        !FcConfigSubstitute(NULL, query, FcMatchPattern))
        goto done;
    FcDefaultSubstitute(query);
    fonts = FcFontSort(NULL, query, FcFalse, NULL, &result);
    if (!fonts) goto done;
    for (i = 0; i < fonts->nfont; ++i) {
        FcCharSet *coverage;
        FcChar8 *file;
        int index = 0;
        size_t length;
        if (FcPatternGetCharSet(fonts->fonts[i], FC_CHARSET, 0, &coverage) != FcResultMatch ||
            !FcCharSetHasChar(coverage, codepoint) ||
            FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
            continue;
        length = strlen((const char *)file);
        if (length >= capacity) continue;
        (void)FcPatternGetInteger(fonts->fonts[i], FC_INDEX, 0, &index);
        memcpy(out, file, length + 1);
        *face_index = index;
        found = true;
        break;
    }
done:
    if (fonts) FcFontSetDestroy(fonts);
    if (charset) FcCharSetDestroy(charset);
    if (query) FcPatternDestroy(query);
    return found;
}
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <limits.h>

/* CoreText descriptors carry URLs, not FreeType collection indices. Match
 * the PostScript name against the file's descriptor array to recover it. */
static bool coretext_file_face(CTFontDescriptorRef descriptor, char *out,
                               size_t capacity, int *face_index)
{
    CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute);
    CFStringRef name = (CFStringRef)CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute);
    CFArrayRef faces = NULL;
    bool found = false;
    CFIndex i;

    if (!url || !name || capacity > LONG_MAX ||
        !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)out, (CFIndex)capacity))
        goto done;
    faces = CTFontManagerCreateFontDescriptorsFromURL(url);
    if (!faces)
        goto done;
    for (i = 0; i < CFArrayGetCount(faces) && i <= INT_MAX; i++) {
        CTFontDescriptorRef item = (CTFontDescriptorRef)CFArrayGetValueAtIndex(faces, i);
        CFStringRef item_name = (CFStringRef)CTFontDescriptorCopyAttribute(item, kCTFontNameAttribute);
        if (item_name && CFEqual(name, item_name)) {
            *face_index = (int)i;
            found = true;
        }
        if (item_name) CFRelease(item_name);
        if (found) break;
    }
done:
    if (faces) CFRelease(faces);
    if (name) CFRelease(name);
    if (url) CFRelease(url);
    return found;
}

static bool lookup_coretext(const char *name, bool bold, bool italic,
                            char *out, size_t capacity, int *face_index)
{
    SDL_PathInfo info;
    CFStringRef family = NULL;
    CFURLRef url = NULL;
    CFArrayRef file_faces = NULL, matches = NULL;
    CFDictionaryRef attributes = NULL;
    CTFontDescriptorRef query = NULL;
    CFSetRef required = NULL;
    const void *key = kCTFontFamilyNameAttribute, *value;
    bool found = false;
    CFIndex i;
    int pass;

    if (SDL_GetPathInfo(name, &info) && info.type == SDL_PATHTYPE_FILE) {
        if (!bold && !italic) {
            size_t length = strlen(name);
            if (length >= capacity) return false;
            memcpy(out, name, length + 1);
            *face_index = 0;
            return true;
        }
        url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)name,
                                                       (CFIndex)strlen(name), false);
        if (!url) goto done;
        file_faces = CTFontManagerCreateFontDescriptorsFromURL(url);
        if (!file_faces || CFArrayGetCount(file_faces) == 0) goto done;
        family = (CFStringRef)CTFontDescriptorCopyAttribute(
            (CTFontDescriptorRef)CFArrayGetValueAtIndex(file_faces, 0), kCTFontFamilyNameAttribute);
    } else {
        family = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    }
    if (!family) goto done;
    value = family;
    attributes = CFDictionaryCreate(NULL, &key, &value, 1,
                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    required = CFSetCreate(NULL, &key, 1, &kCFTypeSetCallBacks);
    if (!attributes || !required) goto done;
    query = CTFontDescriptorCreateWithAttributes(attributes);
    if (!query) goto done;
    matches = CTFontDescriptorCreateMatchingFontDescriptors(query, required);
    if (!matches) goto done;
    /* If the family has no regular face, keep the same last-resort behavior
     * as the Windows and fontconfig resolvers. */
    for (pass = 0; pass < ((!bold && !italic) ? 2 : 1) && !found; ++pass) {
        for (i = 0; i < CFArrayGetCount(matches); i++) {
            CTFontDescriptorRef candidate = (CTFontDescriptorRef)CFArrayGetValueAtIndex(matches, i);
            CFStringRef candidate_family = (CFStringRef)CTFontDescriptorCopyAttribute(candidate, kCTFontFamilyNameAttribute);
            CFDictionaryRef traits = (CFDictionaryRef)CTFontDescriptorCopyAttribute(candidate, kCTFontTraitsAttribute);
            CFNumberRef symbolic = traits ? (CFNumberRef)CFDictionaryGetValue(traits, kCTFontSymbolicTrait) : NULL;
            int bits = 0;
            bool same, is_bold, is_italic;

            if (symbolic) CFNumberGetValue(symbolic, kCFNumberIntType, &bits);
            same = candidate_family && CFStringCompare(family, candidate_family,
                                                        kCFCompareCaseInsensitive) == kCFCompareEqualTo;
            is_bold = (bits & kCTFontBoldTrait) != 0;
            is_italic = (bits & kCTFontItalicTrait) != 0;
            if (candidate_family) CFRelease(candidate_family);
            if (traits) CFRelease(traits);
            if (same && (pass || (is_bold == bold && is_italic == italic)) &&
                coretext_file_face(candidate, out, capacity, face_index)) {
                found = true;
                break;
            }
        }
    }
done:
    if (matches) CFRelease(matches);
    if (query) CFRelease(query);
    if (required) CFRelease(required);
    if (attributes) CFRelease(attributes);
    if (file_faces) CFRelease(file_faces);
    if (url) CFRelease(url);
    if (family) CFRelease(family);
    return found;
}

static bool coretext_codepoint(uint32_t codepoint, char *out, size_t capacity,
                               int *face_index)
{
    UniChar chars[2];
    CFIndex length = 1;
    CFStringRef text;
    CTFontRef base, fallback;
    CTFontDescriptorRef descriptor;
    bool found = false;

    if (codepoint < 0x10000) {
        chars[0] = (UniChar)codepoint;
    } else {
        codepoint -= 0x10000;
        chars[0] = (UniChar)(0xd800 + (codepoint >> 10));
        chars[1] = (UniChar)(0xdc00 + (codepoint & 0x3ff));
        length = 2;
    }
    text = CFStringCreateWithCharacters(NULL, chars, length);
    if (!text) return false;
    base = CTFontCreateWithName(CFSTR("Helvetica"), 12, NULL);
    if (base) {
        fallback = CTFontCreateForString(base, text, CFRangeMake(0, length));
        if (fallback) {
            descriptor = CTFontCopyFontDescriptor(fallback);
            if (descriptor) {
                found = coretext_file_face(descriptor, out, capacity, face_index);
                CFRelease(descriptor);
            }
            CFRelease(fallback);
        }
        CFRelease(base);
    }
    CFRelease(text);
    return found;
}
#endif

bool font_lookup_face(const char *name, bool bold, bool italic,
                      char *out, size_t capacity, int *face_index)
{
    if (!name || !name[0] || !out || capacity == 0 || !face_index)
        return false;
#if defined(__linux__)
    return lookup_fontconfig(name, bold, italic, out, capacity, face_index);
#elif defined(__APPLE__)
    return lookup_coretext(name, bold, italic, out, capacity, face_index);
#else
    if (bold || italic) return false;
    if (!font_lookup_path(name, out, capacity)) return false;
    *face_index = 0;
    return true;
#endif
}

bool font_lookup_codepoint(uint32_t codepoint, char *out, size_t capacity,
                           int *face_index)
{
    if (!out || !capacity || !face_index || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return false;
#if defined(__linux__)
    return fontconfig_codepoint(codepoint, out, capacity, face_index);
#elif defined(__APPLE__)
    return coretext_codepoint(codepoint, out, capacity, face_index);
#else
    return false;
#endif
}

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
#elif defined(__linux__) || defined(__APPLE__)
    {
        int face_index;
        return font_lookup_face(name, false, false, out, capacity, &face_index);
    }
#else
    return false;
#endif
}
