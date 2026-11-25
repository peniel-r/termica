/*
 * termica - access to the fonts embedded in the executable. Windows uses
 * RCDATA; Unix builds link the same bytes via src/fonts.S.in.
 */

#include "embed.h"

#ifdef _WIN32

#include <windows.h>

bool embedded_font(EmbeddedFontFace face, const void **data, size_t *size)
{
    HRSRC  resource;
    HGLOBAL loaded;
    void  *bytes;
    DWORD  length;

    if (data)
        *data = NULL;
    if (size)
        *size = 0;

    resource = FindResourceW(NULL, MAKEINTRESOURCEW((WORD)face),
                             MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!resource)
        return false;
    length = SizeofResource(NULL, resource);
    if (length == 0)
        return false;
    loaded = LoadResource(NULL, resource);
    if (!loaded)
        return false;
    bytes = LockResource(loaded);
    if (!bytes)
        return false;

    if (data)
        *data = bytes;
    if (size)
        *size = (size_t)length;
    return true;
}

bool embedded_font_available(void)
{
    const void *data;
    size_t size;

    return embedded_font(EMBEDDED_FONT_REGULAR, &data, &size);
}

#else /* !_WIN32 */

#include <stdint.h>

extern const unsigned char termica_font_regular_start[], termica_font_regular_end[];
extern const unsigned char termica_font_bold_start[], termica_font_bold_end[];
extern const unsigned char termica_font_italic_start[], termica_font_italic_end[];
extern const unsigned char termica_font_bold_italic_start[], termica_font_bold_italic_end[];
extern const unsigned char termica_font_symbols_start[], termica_font_symbols_end[];

bool embedded_font(EmbeddedFontFace face, const void **data, size_t *size)
{
    const unsigned char *start = NULL, *end = NULL;

    switch (face) {
    case EMBEDDED_FONT_REGULAR:
        start = termica_font_regular_start; end = termica_font_regular_end; break;
    case EMBEDDED_FONT_BOLD:
        start = termica_font_bold_start; end = termica_font_bold_end; break;
    case EMBEDDED_FONT_ITALIC:
        start = termica_font_italic_start; end = termica_font_italic_end; break;
    case EMBEDDED_FONT_BOLD_ITALIC:
        start = termica_font_bold_italic_start; end = termica_font_bold_italic_end; break;
    case EMBEDDED_FONT_SYMBOLS_MONO:
        start = termica_font_symbols_start; end = termica_font_symbols_end; break;
    default:
        break;
    }
    if (data)
        *data = start;
    if (size)
        *size = start ? (size_t)((uintptr_t)end - (uintptr_t)start) : 0;
    return start && (uintptr_t)end > (uintptr_t)start;
}

bool embedded_font_available(void)
{
    return embedded_font(EMBEDDED_FONT_REGULAR, NULL, NULL);
}

#endif /* _WIN32 */
