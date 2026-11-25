/* Native font lookup checks, included in --selftest on Linux and macOS. */
#define _POSIX_C_SOURCE 200809L
#include "font_lookup.h"
#include "glyph.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(bool passed, const char *description)
{
    printf("%s  %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) ++failures;
}

static TTF_Font *open_face(const char *path, int index)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    TTF_Font *font = NULL;
    if (!props) return NULL;
    if (SDL_SetStringProperty(props, TTF_PROP_FONT_CREATE_FILENAME_STRING, path) &&
        SDL_SetFloatProperty(props, TTF_PROP_FONT_CREATE_SIZE_FLOAT, 16.0f) &&
        SDL_SetNumberProperty(props, TTF_PROP_FONT_CREATE_FACE_NUMBER, index))
        font = TTF_OpenFontWithProperties(props);
    SDL_DestroyProperties(props);
    return font;
}

static void test_utf8_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char directory[1024], path[1200], resolved[1200], fallback[1][256];
    const char *user[1];
    int fd, index = -1;
    if (!tmp || !tmp[0]) tmp = "/tmp";
    if (snprintf(directory, sizeof directory, "%s/termica-font-XXXXXX", tmp) >=
            (int)sizeof directory || !mkdtemp(directory)) {
        check(false, "create temporary directory for UTF-8 font path");
        return;
    }
    if (snprintf(path, sizeof path, "%s/f\xc3\xb3nt.ttf", directory) >= (int)sizeof path) {
        check(false, "construct UTF-8 font path");
        rmdir(directory);
        return;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        check(false, "create UTF-8 font path");
        rmdir(directory);
        return;
    }
    close(fd);
    check(font_lookup_path(path, resolved, sizeof resolved) && !strcmp(path, resolved),
          "UTF-8 font file paths are returned byte-for-byte");
    check(font_lookup_face(path, false, false, resolved, sizeof resolved, &index) &&
          !strcmp(path, resolved) && index == 0,
          "explicit font file selects collection face zero");
    check(!font_lookup_path(path, resolved, strlen(path)),
          "font lookup refuses an undersized path buffer");
    user[0] = path;
    if (strlen(path) < sizeof fallback[0])
        check(glyphs_resolve_fallback_paths(user, 1, fallback, 1) == 1 &&
              !strcmp(path, fallback[0]), "user fallback precedes system fonts");
    unlink(path);
    rmdir(directory);
}

static void test_installed_font(void)
{
#if defined(__APPLE__)
    const char *family = "Menlo";
#else
    const char *family = "monospace";
#endif
    char path[2048], variant[2048], family_copy[512];
    int index, variant_index, style;
    TTF_Font *font;
    const char *actual_family;

    if (!font_lookup_face(family, false, false, path, sizeof path, &index)) {
        printf("SKIP  no installed %s font\n", family);
        return;
    }
    font = open_face(path, index);
    check(font != NULL, "native installed family opens at its collection index");
    if (!font) return;
    actual_family = TTF_GetFontFamilyName(font);
    check(actual_family && actual_family[0] && TTF_FontHasGlyph(font, 'M'),
          "native face exposes its family and a Latin glyph");
#if defined(__APPLE__)
    check(actual_family && SDL_strcasecmp(actual_family, family) == 0,
          "CoreText collection index selects the requested family");
#endif
    if (!actual_family || strlen(actual_family) >= sizeof family_copy) {
        TTF_CloseFont(font);
        return;
    }
    memcpy(family_copy, actual_family, strlen(actual_family) + 1);
    TTF_CloseFont(font);

    check(font_lookup_face(family_copy, false, false, path, sizeof path, &index),
          "actual installed family resolves by name");
    for (style = 1; style < 4; ++style) {
        bool bold = (style & 1) != 0, italic = (style & 2) != 0;
        if (!font_lookup_face(family_copy, bold, italic, variant,
                              sizeof variant, &variant_index))
            continue; /* Installed families need not have every style. */
        font = open_face(variant, variant_index);
        check(font != NULL, "installed style opens at its collection index");
        if (font) {
            actual_family = TTF_GetFontFamilyName(font);
            check(actual_family && SDL_strcasecmp(actual_family, family_copy) == 0,
                  "installed style remains in the requested family");
            TTF_CloseFont(font);
        }
    }
}

static void test_character_fallback(void)
{
    char path[2048];
    int index;
    TTF_Font *font;
    bool found = font_lookup_codepoint('M', path, sizeof path, &index);
    if (!found) {
        char available_path[2048];
        int available_index;
#if defined(__APPLE__)
        const char *family = "Menlo";
#else
        const char *family = "monospace";
#endif
        if (font_lookup_face(family, false, false, available_path,
                             sizeof available_path, &available_index))
            check(false, "native fallback locates an installed Latin glyph");
        else
            printf("SKIP  no installed font for native fallback\n");
        return;
    }
    font = open_face(path, index);
    check(font && TTF_FontHasGlyph(font, 'M'),
          "native fallback face contains the requested glyph");
    if (font) TTF_CloseFont(font);
}

static void test_color_emoji(void)
{
#if defined(__APPLE__)
    const char *family = "Apple Color Emoji";
#else
    const char *family = "Noto Color Emoji";
#endif
    char path[2048];
    int index;
    TTF_Font *font;
    SDL_Surface *image;
    TTF_ImageType type = TTF_IMAGE_INVALID;
    if (!font_lookup_face(family, false, false, path, sizeof path, &index)) {
        printf("SKIP  no installed %s font\n", family);
        return;
    }
    font = open_face(path, index);
    check(font != NULL, "installed color emoji face opens");
    if (!font) return;
    image = TTF_GetGlyphImage(font, 0x1f600, &type);
    check(image && type == TTF_IMAGE_COLOR,
          "installed emoji face retains a color glyph");
    if (image) SDL_DestroySurface(image);
    TTF_CloseFont(font);
}

int font_posix_selftest(void)
{
    char path[128];
    int index;
    failures = 0;
    check(!font_lookup_path("termica-no-such-font-family", path, sizeof path) &&
          !font_lookup_face("termica-no-such-font-family", false, false,
                            path, sizeof path, &index),
          "unknown family is not replaced by an unrelated font");
    check(!font_lookup_face(NULL, false, false, path, sizeof path, &index) &&
          !font_lookup_face("monospace", false, false, path, 0, &index) &&
          !font_lookup_face("monospace", false, false, path, sizeof path, NULL),
          "font lookup rejects invalid arguments");
    check(!font_lookup_codepoint(0x110000, path, sizeof path, &index) &&
          !font_lookup_codepoint(0xd800, path, sizeof path, &index),
          "native fallback rejects invalid Unicode scalars");
    test_utf8_file();
    if (!TTF_Init()) {
        check(false, "initialize SDL_ttf for native font tests");
        return failures;
    }
    test_installed_font();
    test_character_fallback();
    test_color_emoji();
    TTF_Quit();
    return failures;
}
