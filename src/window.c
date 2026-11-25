/*
 * termica - the SDL window and its Windows chrome. See window.h.
 */

#include "window.h"

#include "geometry.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

/* A few pixels at the window edge resize, the top strip drags the window.
 * These are SDL window coordinates, not pixels, so they do not scale. */
#define RESIZE_BORDER 8

/* Segments per corner when the squircle is turned into a window region. */
#define REGION_CORNER_STEPS 12

/* What the hit test needs to know. Only one window exists, so this is set
 * once by window_create and read by the callback. */
static bool hit_custom_frame;
static int hit_drag_strip;
static int hit_buttons_width;
static int hit_tabs_width;

void window_set_tabs_width(int logical_width)
{
    hit_tabs_width = logical_width > 0 ? logical_width : 0;
}

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *window,
                                          const SDL_Point *point, void *data)
{
    int width;
    int height;
    bool left;
    bool right;
    bool top;
    bool bottom;

    (void)data;
    if (!hit_custom_frame)
        return SDL_HITTEST_NORMAL;

    SDL_GetWindowSize(window, &width, &height);
    left = point->x < RESIZE_BORDER;
    right = point->x >= width - RESIZE_BORDER;
    top = point->y < RESIZE_BORDER;
    bottom = point->y >= height - RESIZE_BORDER;

    if (top && left)     return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)    return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left)            return SDL_HITTEST_RESIZE_LEFT;
    if (right)           return SDL_HITTEST_RESIZE_RIGHT;
    if (top)             return SDL_HITTEST_RESIZE_TOP;
    if (bottom)          return SDL_HITTEST_RESIZE_BOTTOM;
    if (point->y < hit_drag_strip) {
        /* The title bar buttons and tab strip are ordinary clicks, not a
         * window drag. */
        if (hit_buttons_width > 0 && point->x >= width - hit_buttons_width)
            return SDL_HITTEST_NORMAL;
        if (hit_tabs_width > 0 && point->x < hit_tabs_width)
            return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

#ifdef _WIN32
/* Written out here so older MinGW headers still compile. */
enum {
    TT_DWMWA_USE_IMMERSIVE_DARK_MODE = 20,
    TT_DWMWA_WINDOW_CORNER_PREFERENCE = 33,
    TT_DWMWA_SYSTEMBACKDROP_TYPE = 38
};
enum { TT_DWMWCP_ROUND = 2 };
enum {
    TT_DWMSBT_NONE = 1,
    TT_DWMSBT_MAINWINDOW = 2,      /* Mica                 */
    TT_DWMSBT_TRANSIENTWINDOW = 3, /* Acrylic              */
    TT_DWMSBT_TABBEDWINDOW = 4     /* Mica Alt             */
};

/* Live clear behind-window blur for Liquid Glass via user32's composition
 * policy: blurs live windows and wallpaper behind us without Acrylic's heavy
 * matte tint or grain, and stays translucent when unfocused. */
enum {
    TT_WCA_ACCENT_POLICY = 19,
    TT_ACCENT_DISABLED = 0,
    TT_ACCENT_ENABLE_BLURBEHIND = 3
};

typedef struct {
    int AccentState;
    int AccentFlags;
    DWORD GradientColor;
    int AnimationId;
} TtAccentPolicy;

typedef struct {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
} TtWindowCompositionAttribData;

typedef BOOL (WINAPI *TtSetWindowCompositionAttributeFn)(
    HWND, TtWindowCompositionAttribData *);

/* What is currently applied, so a resize does not redo the frame work. */
static WindowBackdrop applied_backdrop;
static bool applied_backdrop_valid;
static bool applied_accent_blur;
static int region_radius;         /* 0 when the window has no region */
static int region_width;
static int region_height;

static bool set_accent_blur(HWND handle, bool enable)
{
    static TtSetWindowCompositionAttributeFn fn;
    static bool resolved;
    TtAccentPolicy policy;
    TtWindowCompositionAttribData data;

    if (!resolved) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            union {
                FARPROC proc;
                TtSetWindowCompositionAttributeFn typed;
            } cast;
            cast.proc = GetProcAddress(user32, "SetWindowCompositionAttribute");
            fn = cast.typed;
        }
        resolved = true;
    }
    if (!fn)
        return false;

    ZeroMemory(&policy, sizeof policy);
    policy.AccentState = enable ? TT_ACCENT_ENABLE_BLURBEHIND
                                : TT_ACCENT_DISABLED;
    data.Attrib = TT_WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof policy;
    if (!fn(handle, &data))
        return false;

    /* Disabling the accent policy also clears the empty blur-behind region
     * SDL3 installed for per-pixel swapchain alpha; restore it so `panel`
     * mode keeps its transparent background. */
    if (!enable) {
        HRGN empty_rgn = CreateRectRgn(-1, -1, 0, 0);
        if (empty_rgn) {
            DWM_BLURBEHIND bb;
            ZeroMemory(&bb, sizeof bb);
            bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
            bb.fEnable = TRUE;
            bb.hRgnBlur = empty_rgn;
            DwmEnableBlurBehindWindow(handle, &bb);
            DeleteObject(empty_rgn);
        }
    }
    return true;
}

static HWND window_handle(SDL_Window *window)
{
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* Dark title bar and rounded frame corners. The corner preference is kept
 * even though this window draws its own corners: it costs nothing and
 * makes the window look right if the app ever stops drawing them. */
static void apply_dark_frame(SDL_Window *window)
{
    HWND handle = window_handle(window);
    int corner = TT_DWMWCP_ROUND;
    BOOL dark = TRUE;

    if (!handle)
        return;
    DwmSetWindowAttribute(handle, TT_DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof corner);
    DwmSetWindowAttribute(handle, TT_DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof dark);
}

/* Drop the caption button styles while a backdrop is on: extending the
 * frame for Mica brings the system minimize/maximize/close buttons with
 * it, and this window has its own drag strip instead. */
static void set_caption_styles(HWND handle, WindowBackdrop backdrop,
                               bool native)
{
    LONG_PTR style = GetWindowLongPtr(handle, GWL_STYLE);

    if (!native && (backdrop == WINDOW_BACKDROP_MICA ||
                    backdrop == WINDOW_BACKDROP_MICA_ALT ||
                    window_backdrop_is_glass(backdrop)))
        style &= ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    else
        style |= WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(handle, GWL_STYLE, style);
    SetWindowPos(handle, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE);
}

/* Pick the DWM system backdrop. Mica and Mica Alt only cover the frame, so
 * the frame is extended over the client area; Acrylic needs no extension.
 * False when the OS does not know the material (before Windows 11 22H2). */
static bool apply_backdrop(SDL_Window *window, WindowBackdrop backdrop,
                           bool native)
{
    HWND handle = window_handle(window);
    MARGINS margins;
    HRESULT result;
    int type;

    if (!handle)
        return false;
    if (applied_backdrop_valid && applied_backdrop == backdrop)
        return true;

    if (applied_accent_blur && !window_backdrop_is_glass(backdrop)) {
        set_accent_blur(handle, false);
        applied_accent_blur = false;
    }

    if (window_backdrop_is_glass(backdrop)) {
        MARGINS no_margins = { 0, 0, 0, 0 };
        int none = TT_DWMSBT_NONE;

        if (!applied_accent_blur) {
            DwmExtendFrameIntoClientArea(handle, &no_margins);
            DwmSetWindowAttribute(handle, TT_DWMWA_SYSTEMBACKDROP_TYPE,
                                  &none, sizeof none);
            applied_accent_blur = set_accent_blur(handle, true);
            if (!applied_accent_blur) {
                int transient = TT_DWMSBT_TRANSIENTWINDOW;
                DwmSetWindowAttribute(handle, TT_DWMWA_SYSTEMBACKDROP_TYPE,
                                      &transient, sizeof transient);
            }
        }
        applied_backdrop = backdrop;
        applied_backdrop_valid = true;
        set_caption_styles(handle, backdrop, native);
        return true;
    }

    switch (backdrop) {
    case WINDOW_BACKDROP_MICA:
        type = TT_DWMSBT_MAINWINDOW;
        break;
    case WINDOW_BACKDROP_MICA_ALT:
        type = TT_DWMSBT_TABBEDWINDOW;
        break;
    case WINDOW_BACKDROP_ACRYLIC:
        type = TT_DWMSBT_TRANSIENTWINDOW;
        break;
    default:
        type = TT_DWMSBT_NONE;
        break;
    }

    margins.cxLeftWidth = 0;
    margins.cxRightWidth = 0;
    margins.cyTopHeight = 0;
    margins.cyBottomHeight = 0;
    if (backdrop == WINDOW_BACKDROP_MICA ||
        backdrop == WINDOW_BACKDROP_MICA_ALT) {
        margins.cxLeftWidth = -1;
        margins.cxRightWidth = -1;
        margins.cyTopHeight = -1;
        margins.cyBottomHeight = -1;
    }
    DwmExtendFrameIntoClientArea(handle, &margins);
    result = DwmSetWindowAttribute(handle, TT_DWMWA_SYSTEMBACKDROP_TYPE,
                                   &type, sizeof type);
    if (FAILED(result) && backdrop != WINDOW_BACKDROP_PANEL) {
        MARGINS no_margins = { 0, 0, 0, 0 };
        int none = TT_DWMSBT_NONE;

        DwmExtendFrameIntoClientArea(handle, &no_margins);
        DwmSetWindowAttribute(handle, TT_DWMWA_SYSTEMBACKDROP_TYPE,
                              &none, sizeof none);
        applied_backdrop = WINDOW_BACKDROP_PANEL;
        applied_backdrop_valid = true;
        return false;
    }
    applied_backdrop = backdrop;
    applied_backdrop_valid = true;

    set_caption_styles(handle, backdrop, native);
    return true;
}

static int append_corner(POINT *points, int count, float centre_x,
                         float centre_y, float radius, float from_angle,
                         float to_angle)
{
    float xs[REGION_CORNER_STEPS + 1];
    float ys[REGION_CORNER_STEPS + 1];
    int written = geometry_squircle_arc(centre_x, centre_y, radius, from_angle,
                                        to_angle, REGION_CORNER_STEPS, xs, ys);
    int point;

    for (point = 0; point < written; point++) {
        points[count].x = (LONG)(xs[point] + 0.5f);
        points[count].y = (LONG)(ys[point] + 0.5f);
        count++;
    }
    return count;
}

/* Shape the window itself: the squircle as a polygon region. */
static void apply_region(SDL_Window *window, int corner_radius)
{
    HWND handle = window_handle(window);
    POINT points[4 * (REGION_CORNER_STEPS + 1)];
    HRGN region;
    int width;
    int height;
    int count = 0;
    float radius;

    if (!handle)
        return;
    if (corner_radius <= 0) {
        if (region_radius > 0) {
            SetWindowRgn(handle, NULL, TRUE);
            region_radius = 0;
        }
        return;
    }

    SDL_GetWindowSizeInPixels(window, &width, &height);
    radius = (float)corner_radius;
    if (radius * 2.0f > (float)width || radius * 2.0f > (float)height) {
        SetWindowRgn(handle, NULL, TRUE);
        region_radius = 0;
        return;
    }
    if (corner_radius == region_radius && width == region_width &&
        height == region_height)
        return;

    /* Clockwise from the top-left corner: an arc, then a straight run to
     * the next one. */
    count = append_corner(points, count, radius, radius, radius,
                          -SDL_PI_F / 2.0f, -SDL_PI_F);
    count = append_corner(points, count, radius, (float)height - radius, radius,
                          SDL_PI_F, SDL_PI_F / 2.0f);
    count = append_corner(points, count, (float)width - radius,
                          (float)height - radius, radius,
                          SDL_PI_F / 2.0f, 0.0f);
    count = append_corner(points, count, (float)width - radius, radius, radius,
                          0.0f, -SDL_PI_F / 2.0f);

    region = CreatePolygonRgn(points, count, WINDING);
    if (!region)
        return;
    SetWindowRgn(handle, region, TRUE);  /* the system owns the region now */
    region_radius = corner_radius;
    region_width = width;
    region_height = height;
}

void window_flash_taskbar(SDL_Window *window)
{
    HWND hwnd = window_handle(window);
    FLASHWINFO info;

    if (!hwnd)
        return;
    ZeroMemory(&info, sizeof info);
    info.cbSize = sizeof info;
    info.hwnd = hwnd;
    info.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    info.uCount = 3;
    info.dwTimeout = 0;
    FlashWindowEx(&info);
}
#else
static void apply_dark_frame(SDL_Window *window) { (void)window; }
static bool apply_backdrop(SDL_Window *window, WindowBackdrop backdrop,
                           bool native)
{
    (void)window;
    (void)native;
    /* DWM and Windows wallpaper refraction are not available here. Do not
     * claim a backdrop was installed when the platform cannot supply it. */
    return backdrop == WINDOW_BACKDROP_PANEL;
}
static void apply_region(SDL_Window *window, int corner_radius)
{
    (void)window;
    (void)corner_radius;
}
void window_flash_taskbar(SDL_Window *window)
{
    if (window)
        (void)SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
}
#endif

SDL_Window *window_create(const char *title, int width, int height,
                          WindowDecoration decoration, bool title_bar)
{
    SDL_Window *window;
    SDL_WindowFlags flags = SDL_WINDOW_TRANSPARENT | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    bool native = decoration == WINDOW_DECORATION_NATIVE;

    hit_custom_frame = !native;
    title_bar = title_bar && !native;
    if (native) {
        hit_drag_strip = 0;
        hit_buttons_width = 0;
    } else {
        flags |= SDL_WINDOW_BORDERLESS;
        hit_drag_strip = title_bar ? WINDOW_TITLE_BAR_HEIGHT
                                   : (decoration == WINDOW_DECORATION_COMPACT
                                          ? WINDOW_COMPACT_DRAG_STRIP
                                          : WINDOW_DRAG_STRIP);
        hit_buttons_width = title_bar
                                ? WINDOW_TITLE_BAR_BUTTONS *
                                      WINDOW_TITLE_BAR_BUTTON_WIDTH
                                : 0;
    }

    window = SDL_CreateWindow(title, width, height, flags);
    if (!window)
        return NULL;
    if (!native)
        SDL_SetWindowHitTest(window, hit_test, NULL);
    apply_dark_frame(window);
    return window;
}

bool window_set_decoration(SDL_Window *window, WindowDecoration decoration,
                           bool title_bar)
{
    bool native = decoration == WINDOW_DECORATION_NATIVE;
    bool old_native = !hit_custom_frame;

    if (!window || !SDL_SetWindowBordered(window, native))
        return false;
    if (!SDL_SetWindowHitTest(window, native ? NULL : hit_test, NULL)) {
        SDL_SetWindowBordered(window, old_native);
        return false;
    }
    hit_custom_frame = !native;
    title_bar = title_bar && !native;
    hit_drag_strip = native ? 0 : (title_bar ? WINDOW_TITLE_BAR_HEIGHT :
                     (decoration == WINDOW_DECORATION_COMPACT ?
                      WINDOW_COMPACT_DRAG_STRIP : WINDOW_DRAG_STRIP));
    hit_buttons_width = title_bar ? WINDOW_TITLE_BAR_BUTTONS *
                                   WINDOW_TITLE_BAR_BUTTON_WIDTH : 0;
    return true;
}

WindowBackdrop window_sync_chrome(SDL_Window *window, WindowBackdrop backdrop,
                                  WindowDecoration decoration,
                                  int corner_radius)
{
    bool native = decoration == WINDOW_DECORATION_NATIVE;
    bool rounded_by_backdrop = !native &&
                               (backdrop == WINDOW_BACKDROP_MICA ||
                                backdrop == WINDOW_BACKDROP_MICA_ALT ||
                                window_backdrop_is_glass(backdrop));

    if (decoration == WINDOW_DECORATION_SQUARE || native)
        corner_radius = 0;
    if (!apply_backdrop(window, backdrop, native))
        return WINDOW_BACKDROP_PANEL;
    /* The drawn panel rounds itself; only a DWM backdrop needs a region. */
    apply_region(window, rounded_by_backdrop ? corner_radius : 0);
    return backdrop;
}
