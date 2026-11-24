/* Desktop notification and taskbar backend; see desktop_signal.h. */
#include "desktop_signal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool decimal(const char *data, size_t length, size_t *at,
                    unsigned maximum, int *out)
{
    unsigned value = 0;
    size_t start = *at;
    while (*at < length && data[*at] >= '0' && data[*at] <= '9') {
        unsigned digit = (unsigned)(data[*at] - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10))
            return false;
        value = value * 10 + digit;
        ++*at;
    }
    if (*at == start)
        return false;
    *out = (int)value;
    return true;
}

bool desktop_signal_parse_progress(const char *data, size_t length,
                                   DesktopSignalProgress *out)
{
    DesktopSignalProgress parsed;
    size_t at = 4;
    if (!data || !out || length < 5 || memcmp(data, "9;4;", 4) != 0 ||
        !decimal(data, length, &at, 4, &parsed.state))
        return false;
    parsed.progress = -1;
    if (at < length) {
        if (data[at++] != ';' ||
            !decimal(data, length, &at, 100, &parsed.progress))
            return false;
    }
    if (at != length)
        return false;
    *out = parsed;
    return true;
}

/* Validate UTF-8 and append escaped XML text, or measure it when dst is NULL.
 * XML 1.0 excludes C0 controls other than tab/newline/CR and U+FFFE/U+FFFF. */
static bool xml_text(const char *src, size_t len, char *dst, size_t *written)
{
    size_t at = 0, used = 0;
    while (at < len) {
        const unsigned char *p = (const unsigned char *)src + at;
        unsigned cp, n, j;
        const char *replacement = NULL;
        size_t count;
        if (p[0] < 0x80) {
            cp = p[0]; n = 1;
        } else if (p[0] >= 0xc2 && p[0] <= 0xdf) {
            cp = p[0] & 0x1f; n = 2;
        } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
            cp = p[0] & 0x0f; n = 3;
        } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
            cp = p[0] & 0x07; n = 4;
        } else {
            return false;
        }
        if (n > len - at)
            return false;
        for (j = 1; j < n; ++j) {
            if ((p[j] & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (p[j] & 0x3f);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
            return false;
        if (cp == '&') replacement = "&amp;";
        else if (cp == '<') replacement = "&lt;";
        else if (cp == '>') replacement = "&gt;";
        else if ((cp < 0x20 && cp != 9 && cp != 10 && cp != 13) ||
                 cp == 0xfffe || cp == 0xffff)
            replacement = "\xef\xbf\xbd";
        count = replacement ? strlen(replacement) : n;
        if (used > SIZE_MAX - count)
            return false;
        if (dst)
            memcpy(dst + used, replacement ? replacement : src + at, count);
        used += count;
        at += n;
    }
    *written = used;
    return true;
}

bool desktop_signal_notification_xml(const char *title, size_t title_length,
                                     const char *body, size_t body_length,
                                     char **xml, size_t *xml_length)
{
    static const char prefix[] = "<toast><visual><binding template=\"ToastGeneric\"><text>";
    static const char middle[] = "</text><text>";
    static const char suffix[] = "</text></binding></visual></toast>";
    size_t title_size, body_size, total, at, n;
    char *result;
    if (!xml || !xml_length || (!title && title_length) ||
        (!body && body_length))
        return false;
    if (!title_length) {
        title = "termica";
        title_length = sizeof("termica") - 1;
    }
    if (!xml_text(title, title_length, NULL, &title_size) ||
        !xml_text(body, body_length, NULL, &body_size))
        return false;
    total = sizeof(prefix) - 1 + sizeof(middle) - 1 + sizeof(suffix) - 1;
    if (title_size > SIZE_MAX - total || body_size > SIZE_MAX - total - title_size ||
        total + title_size + body_size == SIZE_MAX)
        return false;
    total += title_size + body_size;
    result = (char *)malloc(total + 1);
    if (!result)
        return false;
    at = 0;
    memcpy(result + at, prefix, sizeof(prefix) - 1); at += sizeof(prefix) - 1;
    (void)xml_text(title, title_length, result + at, &n); at += n;
    memcpy(result + at, middle, sizeof(middle) - 1); at += sizeof(middle) - 1;
    (void)xml_text(body, body_length, result + at, &n); at += n;
    memcpy(result + at, suffix, sizeof(suffix) - 1); at += sizeof(suffix) - 1;
    result[at] = '\0';
    *xml = result;
    *xml_length = at;
    return true;
}

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wchar.h>

/* Locally defined GUIDs avoid a dependency on generated WinRT UUID libraries
 * and work with both the Windows SDK and llvm-mingw headers. */
static const GUID clsid_shell_link = {0x00021401,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_shell_link = {0x000214f9,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_persist_file = {0x0000010b,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_property_store = {0x886d8eeb,0x8cf2,0x4446,{0x8d,0x02,0xcd,0xba,0x1d,0xbd,0xcf,0x99}};
static const PROPERTYKEY app_id_key = {{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},5};
static const GUID clsid_taskbar = {0x56fdf344,0xfd6d,0x11d0,{0x95,0x8a,0,0x60,0x97,0xc9,0xa0,0x90}};
static const GUID iid_taskbar3 = {0xea1afb91,0x9e28,0x4b86,{0x90,0xe9,0x9e,0x9f,0x8a,0x5e,0xef,0xaf}};
static const GUID iid_xml_document = {0xf7f3a506,0x1e87,0x42d6,{0xbc,0xfb,0xb8,0xc8,0x09,0xfa,0x54,0x94}};
static const GUID iid_xml_io = {0x6cd0e74e,0xee65,0x4489,{0x9e,0xbf,0xca,0x43,0xe8,0x7b,0xa6,0x37}};
static const GUID iid_toast_factory = {0x04124b20,0x82c6,0x4229,{0xb1,0x09,0xfd,0x9e,0xd4,0x66,0x2b,0x53}};
static const GUID iid_toast_manager = {0x50ac103f,0xd235,0x4598,{0xbb,0xef,0x98,0xfe,0x4d,0x1a,0x3a,0xd4}};
static const WCHAR app_id[] = L"termica.Terminal";

/* COM may already be initialized with either apartment model on the UI
 * thread. Balance only a successful initialization, including S_FALSE. */
static bool com_enter(bool runtime)
{
    HRESULT hr;
    if (runtime) {
        hr = RoInitialize(RO_INIT_SINGLETHREADED);
        if (hr == RPC_E_CHANGED_MODE)
            hr = RoInitialize(RO_INIT_MULTITHREADED);
    } else {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE)
            hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
    return SUCCEEDED(hr);
}

/* Return ERROR_ALREADY_EXISTS for an unowned/indeterminate existing link so
 * the caller can try another name without touching the user's file. */
static HRESULT register_shortcut_path(const WCHAR *path, const WCHAR *directory,
                                      const WCHAR *target)
{
    IShellLinkW *link = NULL;
    IPropertyStore *store = NULL;
    IPersistFile *file = NULL;
    PROPVARIANT value;
    WCHAR old_target[32768], temporary[MAX_PATH];
    DWORD attributes, error;
    HRESULT hr;
    bool exists, owned;

    attributes = GetFileAttributesW(path);
    exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (!exists) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(error);
    }
    hr = CoCreateInstance(&clsid_shell_link, NULL, CLSCTX_INPROC_SERVER,
                          &iid_shell_link, (void **)&link);
    if (FAILED(hr)) goto done;
    hr = link->lpVtbl->QueryInterface(link, &iid_persist_file, (void **)&file);
    if (FAILED(hr)) goto done;
    if (exists) {
        hr = file->lpVtbl->Load(file, path, STGM_READ);
        if (FAILED(hr)) {
            hr = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
            goto done;
        }
    }
    hr = link->lpVtbl->QueryInterface(link, &iid_property_store, (void **)&store);
    if (FAILED(hr)) goto done;
    memset(&value, 0, sizeof(value));
    if (exists) {
        hr = store->lpVtbl->GetValue(store, &app_id_key, &value);
        owned = SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal &&
                wcscmp(value.pwszVal, app_id) == 0;
        PropVariantClear(&value);
        if (!owned) {
            hr = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
            goto done;
        }
        old_target[0] = L'\0';
        hr = link->lpVtbl->GetPath(link, old_target,
                                   (int)(sizeof(old_target) / sizeof(old_target[0])),
                                   NULL, SLGP_RAWPATH);
        if (FAILED(hr)) goto done;
        if (CompareStringOrdinal(old_target, -1, target, -1, TRUE) == CSTR_EQUAL) {
            hr = S_OK;
            goto done;
        }
    }
    hr = link->lpVtbl->SetPath(link, target);
    if (FAILED(hr)) goto done;
    if (!exists) {
        hr = link->lpVtbl->SetDescription(link, L"termica terminal");
        if (FAILED(hr)) goto done;
        value.vt = VT_LPWSTR;
        value.pwszVal = (LPWSTR)app_id; /* SetValue copies this string. */
        hr = store->lpVtbl->SetValue(store, &app_id_key, &value);
        if (FAILED(hr)) goto done;
        hr = store->lpVtbl->Commit(store);
        if (FAILED(hr)) goto done;
        /* Save to a temporary file, then publish without replacement. Another
         * process may have created an unrelated shortcut since the check. */
        if (!GetTempFileNameW(directory, L"tca", 0, temporary)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto done;
        }
        hr = file->lpVtbl->Save(file, temporary, FALSE);
        if (SUCCEEDED(hr) && !MoveFileW(temporary, path)) {
            error = GetLastError();
            hr = HRESULT_FROM_WIN32(error == ERROR_FILE_EXISTS
                                    ? ERROR_ALREADY_EXISTS : error);
        }
        DeleteFileW(temporary);
    } else {
        /* Preserve every other property of an owned link. */
        hr = file->lpVtbl->Save(file, path, TRUE);
    }
done:
    if (file) file->lpVtbl->Release(file);
    if (store) store->lpVtbl->Release(store);
    if (link) link->lpVtbl->Release(link);
    return hr;
}

static HRESULT register_shortcut(void)
{
    WCHAR directory[MAX_PATH], path[MAX_PATH], target[32768];
    DWORD target_length;
    HRESULT hr;
    size_t directory_length;
    unsigned index;
    int count;

    hr = SHGetFolderPathW(NULL, CSIDL_PROGRAMS | CSIDL_FLAG_CREATE, NULL,
                          SHGFP_TYPE_CURRENT, directory);
    if (FAILED(hr)) return hr;
    directory_length = wcslen(directory);
    target_length = GetModuleFileNameW(NULL, target, (DWORD)(sizeof(target) / sizeof(target[0])));
    if (!target_length || target_length >= sizeof(target) / sizeof(target[0]))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    for (index = 0; index < 1000; ++index) {
        memcpy(path, directory, (directory_length + 1) * sizeof(path[0]));
        if (index == 0)
            count = swprintf(path + directory_length, MAX_PATH - directory_length,
                             L"\\termica notifications.lnk");
        else
            count = swprintf(path + directory_length, MAX_PATH - directory_length,
                             L"\\termica notifications (%u).lnk", index);
        if (count < 0 || (size_t)count >= MAX_PATH - directory_length)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        hr = register_shortcut_path(path, directory, target);
        if (hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

bool desktop_signal_init(void)
{
    return SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(app_id));
}

typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument XmlDocument;
typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO XmlIO;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory ToastFactory;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics ToastManager;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification Toast;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotifier ToastNotifier;

static HRESULT runtime_string(const WCHAR *text, HSTRING *out)
{
    size_t length = wcslen(text);
    if (length > UINT32_MAX) return E_INVALIDARG;
    return WindowsCreateString(text, (UINT32)length, out);
}

bool desktop_signal_notify(bool allowed, const char *title, size_t title_length,
                           const char *body, size_t body_length)
{
    char *xml = NULL;
    size_t xml_length = 0;
    WCHAR *wide = NULL;
    int wide_length;
    HSTRING xml_string = NULL, class_string = NULL, id_string = NULL;
    IInspectable *object = NULL;
    XmlDocument *document = NULL;
    XmlIO *io = NULL;
    ToastFactory *factory = NULL;
    ToastManager *manager = NULL;
    ToastNotifier *notifier = NULL;
    Toast *toast = NULL;
    HRESULT hr = E_FAIL;
    bool initialized = false;

    if (!allowed || !desktop_signal_notification_xml(title, title_length, body,
                                                      body_length, &xml, &xml_length))
        return false;
    if (xml_length > INT_MAX) goto done;
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, xml,
                                      (int)xml_length, NULL, 0);
    if (!wide_length) goto done;
    wide = (WCHAR *)malloc((size_t)wide_length * sizeof(*wide));
    if (!wide) goto done;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, xml,
                             (int)xml_length, wide, wide_length)) goto done;
    if (!com_enter(true)) goto done;
    initialized = true;
    hr = register_shortcut();
    if (FAILED(hr)) goto done;
    hr = WindowsCreateString(wide, (UINT32)wide_length, &xml_string);
    if (FAILED(hr)) goto done;
    hr = runtime_string(L"Windows.Data.Xml.Dom.XmlDocument", &class_string);
    if (FAILED(hr)) goto done;
    hr = RoActivateInstance(class_string, &object);
    WindowsDeleteString(class_string); class_string = NULL;
    if (FAILED(hr)) goto done;
    hr = object->lpVtbl->QueryInterface(object, &iid_xml_document, (void **)&document);
    if (FAILED(hr)) goto done;
    hr = object->lpVtbl->QueryInterface(object, &iid_xml_io, (void **)&io);
    if (FAILED(hr)) goto done;
    hr = io->lpVtbl->LoadXml(io, xml_string);
    if (FAILED(hr)) goto done;
    hr = runtime_string(L"Windows.UI.Notifications.ToastNotification", &class_string);
    if (FAILED(hr)) goto done;
    hr = RoGetActivationFactory(class_string, &iid_toast_factory, (void **)&factory);
    WindowsDeleteString(class_string); class_string = NULL;
    if (FAILED(hr)) goto done;
    hr = factory->lpVtbl->CreateToastNotification(factory, document, &toast);
    if (FAILED(hr)) goto done;
    hr = runtime_string(L"Windows.UI.Notifications.ToastNotificationManager", &class_string);
    if (FAILED(hr)) goto done;
    hr = RoGetActivationFactory(class_string, &iid_toast_manager, (void **)&manager);
    if (FAILED(hr)) goto done;
    hr = runtime_string(app_id, &id_string);
    if (FAILED(hr)) goto done;
    hr = manager->lpVtbl->CreateToastNotifierWithId(manager, id_string, &notifier);
    if (FAILED(hr)) goto done;
    hr = notifier->lpVtbl->Show(notifier, toast);
done:
    if (notifier) notifier->lpVtbl->Release(notifier);
    if (manager) manager->lpVtbl->Release(manager);
    if (toast) toast->lpVtbl->Release(toast);
    if (factory) factory->lpVtbl->Release(factory);
    if (io) io->lpVtbl->Release(io);
    if (document) document->lpVtbl->Release(document);
    if (object) object->lpVtbl->Release(object);
    if (id_string) WindowsDeleteString(id_string);
    if (class_string) WindowsDeleteString(class_string);
    if (xml_string) WindowsDeleteString(xml_string);
    if (initialized) RoUninitialize();
    free(wide);
    free(xml);
    return SUCCEEDED(hr);
}

bool desktop_signal_progress(void *native_hwnd, int state, int progress)
{
    static const TBPFLAG states[] = {
        TBPF_NOPROGRESS, TBPF_NORMAL, TBPF_ERROR, TBPF_INDETERMINATE, TBPF_PAUSED
    };
    ITaskbarList3 *taskbar = NULL;
    HWND hwnd = (HWND)native_hwnd;
    HRESULT hr;
    if (!hwnd || state < 0 || state > 4 || progress < -1 || progress > 100)
        return false;
    if (!com_enter(false)) return false;
    hr = CoCreateInstance(&clsid_taskbar, NULL, CLSCTX_INPROC_SERVER,
                          &iid_taskbar3, (void **)&taskbar);
    if (SUCCEEDED(hr)) hr = taskbar->lpVtbl->HrInit(taskbar);
    /* SetProgressValue can reset the visual state to normal, so apply the
     * desired error/paused state afterwards. Clear/indeterminate have no value. */
    if (SUCCEEDED(hr) && progress >= 0 && state != 0 && state != 3)
        hr = taskbar->lpVtbl->SetProgressValue(taskbar, hwnd, (ULONGLONG)progress, 100);
    if (SUCCEEDED(hr)) hr = taskbar->lpVtbl->SetProgressState(taskbar, hwnd, states[state]);
    if (taskbar) taskbar->lpVtbl->Release(taskbar);
    CoUninitialize();
    return SUCCEEDED(hr);
}
#else
bool desktop_signal_init(void) { return false; }
bool desktop_signal_notify(bool allowed, const char *title, size_t title_length,
                           const char *body, size_t body_length)
{
    (void)allowed; (void)title; (void)title_length; (void)body; (void)body_length;
    return false;
}
bool desktop_signal_progress(void *native_hwnd, int state, int progress)
{
    (void)native_hwnd; (void)state; (void)progress;
    return false;
}
#endif
