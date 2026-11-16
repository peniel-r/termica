/*
 * termica - access to the font embedded in the executable. See embed.h.
 *
 * On Windows the TTFs are RCDATA resources linked into the .exe by
 * src/fonts.rc.in; the loader maps them, so there is no file to find, copy
 * or lose.
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

/*
 * The .exe resource mechanism is Windows-only; on other platforms the app
 * uses a font from the filesystem.
 */

bool embedded_font(EmbeddedFontFace face, const void **data, size_t *size)
{
    (void)face;
    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    return false;
}

bool embedded_font_available(void)
{
    return false;
}

#endif /* _WIN32 */
