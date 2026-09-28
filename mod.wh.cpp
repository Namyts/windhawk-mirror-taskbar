// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Reverses the taskbar order: on a left/right taskbar Start goes to the bottom, apps stack upwards from it, and the clock goes to the top
// @version         3.3
// @author          namyts
// @include         explorer.exe
// @include         StartMenuExperienceHost.exe
// @include         ShellHost.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Mirrored Taskbar Layout

Reverses the order of the Windows 11 taskbar along its length, without flipping
any icons or text.

* Left/right taskbar: Start at the bottom, apps stacked upwards from it, clock
  and notification area at the top.
* Top/bottom taskbar: Start on the right, clock and notification area on the
  left.

The Start menu, quick settings, and the tray's hidden-icons flyout open on the
matching side.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <roapi.h>
#include <windowsx.h>
#include <winstring.h>
#include <xamlom.h>

#include <atomic>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

enum class Target { explorer, startMenu, shell };
Target g_target;

std::atomic<bool> g_unloading;

bool GetTaskbarRect(RECT* rect) {
    HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    return taskbar && GetWindowRect(taskbar, rect);
}

bool IsTaskbarVertical() {
    RECT rect;
    return GetTaskbarRect(&rect) &&
           rect.bottom - rect.top > rect.right - rect.left;
}

wux::DependencyObject GetParent(const wux::DependencyObject& element) {
    return wuxm::VisualTreeHelper::GetParent(element);
}

bool IsAncestorOf(const wux::DependencyObject& ancestor,
                  wux::DependencyObject element) {
    for (; element; element = GetParent(element)) {
        if (element == ancestor) {
            return true;
        }
    }
    return false;
}

wux::FrameworkElement FindChild(const wux::DependencyObject& element,
                                std::wstring_view name) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i)
                         .try_as<wux::FrameworkElement>();
        if (child && child.Name() == name) {
            return child;
        }
    }
    return nullptr;
}

template <typename Predicate>
wux::FrameworkElement FindDescendant(const wux::DependencyObject& element,
                                     Predicate predicate) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        if (auto fe = child.try_as<wux::FrameworkElement>();
            fe && predicate(fe)) {
            return fe;
        }
        if (auto found = FindDescendant(child, predicate)) {
            return found;
        }
    }
    return nullptr;
}

HWND FindProcessWindow(PCWSTR className) {
    struct Param {
        PCWSTR className;
        HWND found;
    } param{className, nullptr};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* param = reinterpret_cast<Param*>(lParam);
            DWORD processId = 0;
            WCHAR name[64];
            if (GetWindowThreadProcessId(hwnd, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassName(hwnd, name, ARRAYSIZE(name)) &&
                _wcsicmp(name, param->className) == 0) {
                param->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&param));
    return param.found;
}

////////////////////////////////////////////////////////////////////////////////
// Taskbar. The element that holds both the taskbar and the tray is scaled by
// -1 along the bar, which reverses their order. Each button and tray icon is
// scaled back so it stays upright. The axis follows the bar's shape.

struct FlippedElement {
    winrt::weak_ref<wux::FrameworkElement> element;
    wuxm::ScaleTransform scale{nullptr};
    wuxm::Transform originalTransform{nullptr};
    wf::Point originalOrigin{};
};

std::vector<winrt::weak_ref<wux::FrameworkElement>> g_taskbarFrames;
winrt::weak_ref<wux::FrameworkElement> g_trayFrame;
winrt::weak_ref<wux::FrameworkElement> g_root;
winrt::event_token g_rootSizeChangedToken;
bool g_vertical;
std::vector<FlippedElement> g_flipped;

void SetScaleAxis(const wuxm::ScaleTransform& scale) {
    scale.ScaleX(g_vertical ? 1 : -1);
    scale.ScaleY(g_vertical ? -1 : 1);
}

bool IsFlipped(const wux::FrameworkElement& element) {
    for (const auto& flipped : g_flipped) {
        if (flipped.element.get() == element) {
            return true;
        }
    }
    return false;
}

// Keeps a transform another mod already set, and adds the mirror to it.
void Flip(const wux::FrameworkElement& element) {
    std::erase_if(g_flipped,
                  [](const FlippedElement& flipped) { return !flipped.element.get(); });
    if (IsFlipped(element)) {
        return;
    }

    FlippedElement flipped{
        .element = winrt::make_weak(element),
        .scale = wuxm::ScaleTransform(),
        .originalTransform = element.RenderTransform(),
        .originalOrigin = element.RenderTransformOrigin(),
    };
    SetScaleAxis(flipped.scale);

    if (flipped.originalTransform) {
        wuxm::TransformGroup group;
        group.Children().Append(flipped.originalTransform);
        group.Children().Append(flipped.scale);
        element.RenderTransform(group);
    } else {
        element.RenderTransform(flipped.scale);
    }
    element.RenderTransformOrigin({0.5f, 0.5f});
    g_flipped.push_back(std::move(flipped));
}

void Unflip(const FlippedElement& flipped) {
    auto element = flipped.element.get();
    if (!element) {
        return;
    }

    if (auto group = element.RenderTransform().try_as<wuxm::TransformGroup>();
        group && flipped.originalTransform) {
        group.Children().Clear();
    }
    element.RenderTransform(flipped.originalTransform);
    element.RenderTransformOrigin(flipped.originalOrigin);
}

bool HasFlippedAncestor(const wux::DependencyObject& element) {
    auto root = g_root.get();
    for (auto ancestor = GetParent(element); ancestor && ancestor != root;
         ancestor = GetParent(ancestor)) {
        if (auto fe = ancestor.try_as<wux::FrameworkElement>();
            fe && IsFlipped(fe)) {
            return true;
        }
    }
    return false;
}

// Buttons are the repeater's children. Tray icons are SystemTray.*IconView.
// An icon inside one that is already flipped must be left alone.
bool ShouldFlip(const wux::FrameworkElement& element,
                const wux::DependencyObject& parent,
                std::wstring_view type) {
    if (auto parentFe = parent.try_as<wux::FrameworkElement>();
        parentFe && parentFe.Name() == L"TaskbarFrameRepeater") {
        return true;
    }

    auto trayFrame = g_trayFrame.get();
    return type.starts_with(L"SystemTray.") && type.ends_with(L"IconView") &&
           trayFrame && IsAncestorOf(trayFrame, element) &&
           !HasFlippedAncestor(element);
}

void FlipDescendants(const wux::DependencyObject& element) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        auto fe = child.try_as<wux::FrameworkElement>();
        if (fe && ShouldFlip(fe, element, winrt::get_class_name(fe))) {
            Flip(fe);
        } else {
            FlipDescendants(child);
        }
    }
}

void OnRootSizeChanged() {
    auto root = g_root.get();
    bool vertical = root && root.ActualHeight() > root.ActualWidth();
    if (!root || vertical == g_vertical) {
        return;
    }

    g_vertical = vertical;
    for (auto& flipped : g_flipped) {
        SetScaleAxis(flipped.scale);
    }
}

// Secondary taskbars have no tray, so only the main one is mirrored.
void TryCreateRoot() {
    auto trayFrame = g_trayFrame.get();
    if (g_root.get() || !trayFrame) {
        return;
    }

    for (const auto& weakTaskbarFrame : g_taskbarFrames) {
        auto taskbarFrame = weakTaskbarFrame.get();
        if (!taskbarFrame) {
            continue;
        }

        auto ancestor = GetParent(taskbarFrame);
        while (ancestor && !IsAncestorOf(ancestor, trayFrame)) {
            ancestor = GetParent(ancestor);
        }

        auto root = ancestor.try_as<wux::FrameworkElement>();
        if (!root) {
            continue;
        }

        g_root = winrt::make_weak(root);
        g_vertical = root.ActualHeight() > root.ActualWidth();
        g_rootSizeChangedToken = root.SizeChanged(
            [](const wf::IInspectable&, const wux::SizeChangedEventArgs&) {
                OnRootSizeChanged();
            });
        Flip(root);
        FlipDescendants(root);
        g_taskbarFrames.clear();
        return;
    }
}

void RestoreTaskbar() {
    for (const auto& flipped : g_flipped) {
        Unflip(flipped);
    }
    g_flipped.clear();

    if (auto root = g_root.get()) {
        root.SizeChanged(g_rootSizeChangedToken);
    }
    g_root = nullptr;
    g_trayFrame = nullptr;
    g_taskbarFrames.clear();
}

////////////////////////////////////////////////////////////////////////////////
// Quick settings fill the screen along the taskbar. The panel inside is moved
// to the end the tray moved to, and put back there whenever Windows resets it.

struct WatchedAlignment {
    winrt::weak_ref<wux::FrameworkElement> element;
    int64_t token;
};

std::vector<WatchedAlignment> g_watched;
bool g_aligning;

wux::FrameworkElement QuickSettingsPage(wux::DependencyObject element) {
    for (; element; element = GetParent(element)) {
        if (winrt::get_class_name(element) == L"ControlCenter.ControlCenterPage") {
            return element.as<wux::FrameworkElement>();
        }
    }
    return nullptr;
}

void AlignQuickSettings(const wux::FrameworkElement& page) {
    if (g_unloading || !IsTaskbarVertical()) {
        return;
    }

    g_aligning = true;
    page.VerticalAlignment(wux::VerticalAlignment::Stretch);
    if (auto grid = FindChild(page, L"RootGrid")) {
        grid.VerticalAlignment(wux::VerticalAlignment::Stretch);
        if (auto content = FindChild(grid, L"RootContent")) {
            content.VerticalAlignment(wux::VerticalAlignment::Top);
        }
    }
    g_aligning = false;
}

void WatchAlignment(const wux::FrameworkElement& element) {
    for (const auto& watched : g_watched) {
        if (watched.element.get() == element) {
            return;
        }
    }

    int64_t token = element.RegisterPropertyChangedCallback(
        wux::FrameworkElement::VerticalAlignmentProperty(),
        [](const wux::DependencyObject& sender, const wux::DependencyProperty&) {
            if (!g_aligning && !g_unloading) {
                if (auto page = QuickSettingsPage(sender)) {
                    AlignQuickSettings(page);
                }
            }
        });
    g_watched.push_back({winrt::make_weak(element), token});
}

void OnQuickSettings(const wux::FrameworkElement& element) {
    auto page = QuickSettingsPage(element);
    if (!page) {
        return;
    }

    AlignQuickSettings(page);
    WatchAlignment(page);
    if (auto grid = FindChild(page, L"RootGrid")) {
        WatchAlignment(grid);
        if (auto content = FindChild(grid, L"RootContent")) {
            WatchAlignment(content);
        }
    }
}

void RestoreQuickSettings() {
    for (const auto& watched : g_watched) {
        if (auto element = watched.element.get()) {
            element.UnregisterPropertyChangedCallback(
                wux::FrameworkElement::VerticalAlignmentProperty(),
                watched.token);
        }
    }
    g_watched.clear();
}

void OnElementAdded(const wux::FrameworkElement& element,
                    const wux::DependencyObject& parent,
                    std::wstring_view type) {
    if (g_target == Target::shell) {
        if (type == L"ControlCenter.ControlCenterPage" ||
            element.Name() == L"RootGrid" || element.Name() == L"RootContent") {
            OnQuickSettings(element);
        }
        return;
    }

    if (type == L"Taskbar.TaskbarFrame") {
        g_taskbarFrames.push_back(winrt::make_weak(element));
        TryCreateRoot();
    } else if (type == L"SystemTray.SystemTrayFrame") {
        g_trayFrame = winrt::make_weak(element);
        TryCreateRoot();
    } else if (g_root.get() && parent && ShouldFlip(element, parent, type)) {
        Flip(element);
    }
}

////////////////////////////////////////////////////////////////////////////////
// XAML diagnostics: reports every element added to a process's XAML trees.

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

// Composition diagnostics corrupt the heap when several UI threads add visuals
// at once. Element changes are enough, so the one registry read made for
// DisableCompositionDiag is answered here.
thread_local bool g_reportCompositionDiagAsDisabled;

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher,
                               IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : m_xamlDiagnostics(site.as<IXamlDiagnostics>()) {
        // Advising from the calling thread can hang.
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID param) -> DWORD {
                auto watcher = reinterpret_cast<VisualTreeWatcher*>(param);
                g_reportCompositionDiagAsDisabled = true;
                HRESULT hr = watcher->m_xamlDiagnostics.as<IVisualTreeService3>()
                                 ->AdviseVisualTreeChange(watcher);
                g_reportCompositionDiagAsDisabled = false;
                watcher->Release();
                if (FAILED(hr)) {
                    Wh_Log(L"AdviseVisualTreeChange failed: %08X", hr);
                }
                return 0;
            },
            this, 0, nullptr);
        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    void Unadvise() {
        m_xamlDiagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(
            this);
    }

   private:
    HRESULT STDMETHODCALLTYPE
    OnVisualTreeChange(ParentChildRelation relation,
                       VisualElement element,
                       VisualMutationType mutationType) noexcept override try {
        if (mutationType == Add) {
            if (auto fe = FromHandle(element.Handle)
                              .try_as<wux::FrameworkElement>()) {
                wux::DependencyObject parent{nullptr};
                if (relation.Parent) {
                    parent = FromHandle(relation.Parent)
                                 .try_as<wux::DependencyObject>();
                }
                OnElementAdded(fe, parent, element.Type ? element.Type : L"");
            }
        }
        return S_OK;
    } catch (...) {
        Wh_Log(L"Error %08X", winrt::to_hresult());
        // An error return stops further notifications.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                    VisualElementState,
                                                    LPCWSTR) noexcept override {
        return S_OK;
    }

    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable object;
        winrt::check_hresult(m_xamlDiagnostics->GetIInspectableFromHandle(
            handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
        return object;
    }

    winrt::com_ptr<IXamlDiagnostics> m_xamlDiagnostics;
};

winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;

// {C85D8CC7-5463-40E8-A432-F5916B6427E5}
constexpr CLSID CLSID_WindhawkTAP = {
    0xc85d8cc7,
    0x5463,
    0x40e8,
    {0xa4, 0x32, 0xf5, 0x91, 0x6b, 0x64, 0x27, 0xe5}};

class WindhawkTAP
    : public winrt::implements<WindhawkTAP, IObjectWithSite, winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override try {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->Unadvise();
            g_visualTreeWatcher = nullptr;
        }

        m_site.copy_from(site);
        if (m_site) {
            // Undo the reference taken by InitializeXamlDiagnosticsEx.
            FreeLibrary(GetCurrentModuleHandle());
            g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(m_site);
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid,
                                      void** site) noexcept override {
        return m_site.as(riid, site);
    }

   private:
    winrt::com_ptr<IUnknown> m_site;
};

struct TapFactory
    : winrt::implements<TapFactory, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
                                             REFIID riid,
                                             void** object) noexcept override try {
        *object = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        return winrt::make<WindhawkTAP>().as(riid, object);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport) _Use_decl_annotations_ STDAPI
    DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    *ppv = nullptr;
    if (rclsid != CLSID_WindhawkTAP) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return winrt::make<TapFactory>().as(riid, ppv);
} catch (...) {
    return winrt::to_hresult();
}

__declspec(dllexport) _Use_decl_annotations_ STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

std::atomic<bool> g_tapInjected;

void InjectWindhawkTAP() {
    if (g_tapInjected.load()) {
        return;
    }

    WCHAR location[MAX_PATH];
    HMODULE module = GetCurrentModuleHandle();
    if (!module || !GetModuleFileName(module, location, ARRAYSIZE(location))) {
        return;
    }

    HMODULE wuxDll = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initializeXamlDiagnosticsEx =
        wuxDll ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
                     GetProcAddress(wuxDll, "InitializeXamlDiagnosticsEx"))
               : nullptr;
    if (!initializeXamlDiagnosticsEx || g_tapInjected.exchange(true)) {
        return;
    }

    // The free connection name isn't known, so try them in order. Failure
    // leaves another attempt possible.
    HRESULT hr = E_FAIL;
    for (int i = 1; i <= 10000; i++) {
        WCHAR connectionName[64];
        wsprintf(connectionName, L"VisualDiagConnection%d", i);
        hr = initializeXamlDiagnosticsEx(connectionName, GetCurrentProcessId(),
                                         L"", location, CLSID_WindhawkTAP,
                                         nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }
    if (FAILED(hr)) {
        g_tapInjected = false;
        Wh_Log(L"InitializeXamlDiagnosticsEx failed: %08X", hr);
    }
}

using RegOpenKeyExW_t = decltype(&RegOpenKeyExW);
RegOpenKeyExW_t RegOpenKeyExW_Original;
LSTATUS WINAPI RegOpenKeyExW_Hook(HKEY key,
                                  LPCWSTR subKey,
                                  DWORD options,
                                  REGSAM samDesired,
                                  PHKEY result) {
    LSTATUS status =
        RegOpenKeyExW_Original(key, subKey, options, samDesired, result);
    if (status == ERROR_SUCCESS || !g_reportCompositionDiagAsDisabled ||
        key != HKEY_LOCAL_MACHINE || !subKey ||
        _wcsicmp(subKey, L"Software\\Microsoft\\XAML\\Debug") != 0) {
        return status;
    }

    // The value is only queried if the key opens, so hand out one that exists.
    return RegOpenKeyExW_Original(HKEY_LOCAL_MACHINE, L"Software\\Microsoft",
                                  options, samDesired, result);
}

using RegQueryValueExW_t = decltype(&RegQueryValueExW);
RegQueryValueExW_t RegQueryValueExW_Original;
LSTATUS WINAPI RegQueryValueExW_Hook(HKEY key,
                                     LPCWSTR valueName,
                                     LPDWORD reserved,
                                     LPDWORD type,
                                     LPBYTE data,
                                     LPDWORD dataSize) {
    if (!g_reportCompositionDiagAsDisabled || !valueName ||
        _wcsicmp(valueName, L"DisableCompositionDiag") != 0) {
        return RegQueryValueExW_Original(key, valueName, reserved, type, data,
                                         dataSize);
    }

    if (type) {
        *type = REG_DWORD;
    }
    if (data && (!dataSize || *dataSize < sizeof(DWORD))) {
        if (dataSize) {
            *dataSize = sizeof(DWORD);
        }
        return ERROR_MORE_DATA;
    }
    if (data) {
        *reinterpret_cast<DWORD*>(data) = 1;
    }
    if (dataSize) {
        *dataSize = sizeof(DWORD);
    }
    return ERROR_SUCCESS;
}

HWND FindTaskbarXamlHost() {
    HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (!taskbar || !GetWindowThreadProcessId(taskbar, &processId) ||
        processId != GetCurrentProcessId()) {
        return nullptr;
    }
    return FindWindowEx(taskbar, nullptr,
                        L"Windows.UI.Composition.DesktopWindowContentBridge",
                        nullptr);
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD exStyle,
                                 LPCWSTR className,
                                 LPCWSTR windowName,
                                 DWORD style,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 HWND parent,
                                 HMENU menu,
                                 HINSTANCE instance,
                                 LPVOID param) {
    HWND hwnd = CreateWindowExW_Original(exStyle, className, windowName, style,
                                         x, y, width, height, parent, menu,
                                         instance, param);
    WCHAR parentClass[32];
    if (hwnd && parent &&
        GetClassName(parent, parentClass, ARRAYSIZE(parentClass)) &&
        _wcsicmp(parentClass, L"Shell_TrayWnd") == 0 &&
        FindTaskbarXamlHost() == hwnd) {
        InjectWindhawkTAP();
    }
    return hwnd;
}

////////////////////////////////////////////////////////////////////////////////
// Hidden-icons flyout. Windows still aims it at the tray's old end, so a
// flyout on that half of the taskbar is reflected across the middle.

bool IsTrayFlyout(HWND hwnd, const RECT& taskbar, bool vertical, int middle) {
    WCHAR className[64];
    if (!GetClassName(hwnd, className, ARRAYSIZE(className))) {
        return false;
    }

    if (_wcsicmp(className, L"TopLevelWindowForOverflowXamlIsland") == 0) {
        return true;
    }

    // This class is also app thumbnails and Alt+Tab, so it only counts when
    // the click was on the tray's half of the taskbar.
    if (_wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0) {
        DWORD messagePos = GetMessagePos();
        POINT pt{GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos)};
        return PtInRect(&taskbar, pt) &&
               (vertical ? pt.y < middle : pt.x < middle);
    }

    return false;
}

void MirrorFlyout(HWND hwnd, int& x, int& y, int cx, int cy) {
    RECT taskbar;
    if (g_unloading || !GetTaskbarRect(&taskbar)) {
        return;
    }

    bool vertical = taskbar.bottom - taskbar.top > taskbar.right - taskbar.left;
    int middle = vertical ? (taskbar.top + taskbar.bottom) / 2
                          : (taskbar.left + taskbar.right) / 2;
    int center = vertical ? y + cy / 2 : x + cx / 2;
    if (center <= middle || !IsTrayFlyout(hwnd, taskbar, vertical, middle)) {
        return;
    }

    if (vertical) {
        y = taskbar.top + taskbar.bottom - y - cy;
    } else {
        x = taskbar.left + taskbar.right - x - cx;
    }
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;
BOOL WINAPI SetWindowPos_Hook(HWND hwnd,
                              HWND insertAfter,
                              int x,
                              int y,
                              int cx,
                              int cy,
                              UINT flags) {
    if (!(flags & SWP_NOMOVE)) {
        int width = cx;
        int height = cy;
        if (flags & SWP_NOSIZE) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        MirrorFlyout(hwnd, x, y, width, height);
    }
    return SetWindowPos_Original(hwnd, insertAfter, x, y, cx, cy, flags);
}

////////////////////////////////////////////////////////////////////////////////
// Start menu. Its window covers the monitor and the menu is anchored inside
// it, at the Start button's end of the taskbar. The anchor is moved to the
// other end. The values are absolute, so applying them again changes nothing.

namespace StartMenu {

struct State {
    winrt::weak_ref<wux::FrameworkElement> element;
    bool vertical = false;
    std::vector<std::pair<wux::DependencyProperty, int64_t>> tokens;
};

State g_state;
winrt::event_token g_visibilityChangedToken;
bool g_applying;

void Place(const wux::FrameworkElement& element, bool vertical, bool undo) {
    auto margin = element.Margin();
    if (vertical) {
        double gap = margin.Top + margin.Bottom;
        margin.Top = undo ? 0 : gap;
        margin.Bottom = undo ? gap : 0;
        element.VerticalAlignment(undo ? wux::VerticalAlignment::Top
                                       : wux::VerticalAlignment::Bottom);
    } else {
        double gap = margin.Left + margin.Right;
        margin.Left = undo ? 0 : gap;
        margin.Right = undo ? gap : 0;
        element.HorizontalAlignment(undo ? wux::HorizontalAlignment::Left
                                         : wux::HorizontalAlignment::Right);
    }
    element.Margin(margin);
}

void Apply();

void Watch(const wux::FrameworkElement& element) {
    for (auto property : {wux::FrameworkElement::VerticalAlignmentProperty(),
                          wux::FrameworkElement::HorizontalAlignmentProperty(),
                          wux::FrameworkElement::MarginProperty()}) {
        int64_t token = element.RegisterPropertyChangedCallback(
            property,
            [](const wux::DependencyObject&, const wux::DependencyProperty&) {
                if (!g_applying) {
                    Apply();
                }
            });
        g_state.tokens.emplace_back(property, token);
    }
}

void Restore() {
    auto element = g_state.element.get();
    if (element) {
        Place(element, g_state.vertical, /*undo=*/true);
        for (const auto& [property, token] : g_state.tokens) {
            element.UnregisterPropertyChangedCallback(property, token);
        }
    }
    g_state = {};
}

void Apply() try {
    if (g_unloading) {
        return;
    }

    auto window = wux::Window::Current();
    auto content = window ? window.Content() : nullptr;
    if (!content) {
        return;
    }

    auto element = FindDescendant(content, [](const wux::FrameworkElement& e) {
        return e.Name() == L"FrameRoot";
    });
    if (!element) {
        return;
    }

    g_applying = true;

    bool vertical = IsTaskbarVertical();
    if (g_state.element.get() != element || g_state.vertical != vertical) {
        Restore();
        g_state.element = winrt::make_weak(element);
        g_state.vertical = vertical;
        Watch(element);
    }
    Place(element, vertical, /*undo=*/false);

    g_applying = false;
} catch (...) {
    g_applying = false;
    Wh_Log(L"Error %08X", winrt::to_hresult());
}

bool Ready() {
    return g_visibilityChangedToken.value != 0;
}

void Init() try {
    auto window = wux::Window::Current();
    if (g_unloading || g_visibilityChangedToken || !window) {
        return;
    }

    g_visibilityChangedToken = window.VisibilityChanged(
        [](const wf::IInspectable&,
           const winrt::Windows::UI::Core::VisibilityChangedEventArgs& args) {
            if (args.Visible()) {
                Apply();
            }
        });
    Apply();
} catch (...) {
    Wh_Log(L"Error %08X", winrt::to_hresult());
}

void Uninit() try {
    if (g_visibilityChangedToken) {
        if (auto window = wux::Window::Current()) {
            window.VisibilityChanged(g_visibilityChangedToken);
        }
        g_visibilityChangedToken = {};
    }
    g_applying = true;
    Restore();
    g_applying = false;
} catch (...) {
    Wh_Log(L"Error %08X", winrt::to_hresult());
}

}  // namespace StartMenu

////////////////////////////////////////////////////////////////////////////////
// Entry points.

using RoGetActivationFactory_t = decltype(&RoGetActivationFactory);
RoGetActivationFactory_t RoGetActivationFactory_Original;

// XAML for the Start menu and quick settings is created on demand. Creating
// its island is the moment it can be reached.
HRESULT WINAPI RoGetActivationFactory_Hook(HSTRING classId,
                                           REFIID iid,
                                           void** factory) {
    thread_local bool inHook;
    if (!inHook && classId &&
        wcscmp(WindowsGetStringRawBuffer(classId, nullptr),
               L"Windows.UI.Xaml.Hosting.XamlIsland") == 0) {
        inHook = true;
        if (g_target == Target::startMenu) {
            StartMenu::Init();
        } else {
            InjectWindhawkTAP();
        }
        inHook = false;
    }
    return RoGetActivationFactory_Original(classId, iid, factory);
}

using RunFromWindowThreadProc_t = void (*)(PVOID);

struct RunFromWindowThreadParam {
    RunFromWindowThreadProc_t proc;
    PVOID procParam;
};

UINT g_runFromWindowThreadMsg;

LRESULT CALLBACK RunFromWindowThreadHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto* msg = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (msg->message == g_runFromWindowThreadMsg) {
            auto* param = reinterpret_cast<RunFromWindowThreadParam*>(msg->lParam);
            param->proc(param->procParam);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void RunFromWindowThread(HWND hwnd, RunFromWindowThreadProc_t proc) {
    if (!g_runFromWindowThreadMsg) {
        g_runFromWindowThreadMsg =
            RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    }

    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    if (!threadId) {
        return;
    }
    if (threadId == GetCurrentThreadId()) {
        proc(nullptr);
        return;
    }

    HHOOK hook = SetWindowsHookEx(WH_CALLWNDPROC, RunFromWindowThreadHook,
                                  nullptr, threadId);
    if (hook) {
        RunFromWindowThreadParam data{proc, nullptr};
        SendMessage(hwnd, g_runFromWindowThreadMsg, 0, (LPARAM)&data);
        UnhookWindowsHookEx(hook);
    }
}

void HookActivationFactory() {
    HMODULE winrt = GetModuleHandle(L"api-ms-win-core-winrt-l1-1-0.dll");
    if (auto roGetActivationFactory =
            winrt ? (RoGetActivationFactory_t)GetProcAddress(
                        winrt, "RoGetActivationFactory")
                  : nullptr) {
        WindhawkUtils::SetFunctionHook(roGetActivationFactory,
                                       RoGetActivationFactory_Hook,
                                       &RoGetActivationFactory_Original);
    }
}

BOOL Wh_ModInit() {
    WCHAR path[MAX_PATH]{};
    GetModuleFileName(nullptr, path, ARRAYSIZE(path));
    PCWSTR name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    g_target = _wcsicmp(name, L"explorer.exe") == 0             ? Target::explorer
               : _wcsicmp(name, L"StartMenuExperienceHost.exe") == 0
                   ? Target::startMenu
                   : Target::shell;

    if (g_target == Target::startMenu) {
        HookActivationFactory();
        return TRUE;
    }
    if (g_target == Target::shell && _wcsicmp(name, L"ShellHost.exe") != 0) {
        return FALSE;
    }

    if (g_target == Target::explorer) {
        WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                       &SetWindowPos_Original);
        WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                       &CreateWindowExW_Original);
    } else {
        HookActivationFactory();
    }

    HMODULE kernelBase = GetModuleHandle(L"kernelbase.dll");
    WindhawkUtils::SetFunctionHook(
        (RegOpenKeyExW_t)GetProcAddress(kernelBase, "RegOpenKeyExW"),
        RegOpenKeyExW_Hook, &RegOpenKeyExW_Original);
    WindhawkUtils::SetFunctionHook(
        (RegQueryValueExW_t)GetProcAddress(kernelBase, "RegQueryValueExW"),
        RegQueryValueExW_Hook, &RegQueryValueExW_Original);
    return TRUE;
}

void AttachStartMenu() {
    if (HWND coreWindow = FindProcessWindow(L"Windows.UI.Core.CoreWindow")) {
        RunFromWindowThread(coreWindow, [](PVOID) { StartMenu::Init(); });
    }
}

// The Start menu window often appears just after the mod loads.
DWORD WINAPI StartMenuAttachThread(LPVOID) {
    for (int i = 0; i < 50 && !g_unloading && !StartMenu::Ready(); i++) {
        AttachStartMenu();
        Sleep(100);
    }
    return 0;
}

void Wh_ModAfterInit() {
    if (g_target == Target::startMenu) {
        AttachStartMenu();
        if (!StartMenu::Ready()) {
            HANDLE thread = CreateThread(nullptr, 0, StartMenuAttachThread,
                                         nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
        }
    } else if (g_target == Target::explorer) {
        if (FindTaskbarXamlHost()) {
            InjectWindhawkTAP();
        }
    } else if (FindProcessWindow(L"ControlCenterWindow")) {
        InjectWindhawkTAP();
    }
}

void Wh_ModUninit() {
    g_unloading = true;

    if (g_target == Target::startMenu) {
        if (HWND coreWindow = FindProcessWindow(L"Windows.UI.Core.CoreWindow")) {
            RunFromWindowThread(coreWindow,
                                [](PVOID) { StartMenu::Uninit(); });
        }
        return;
    }

    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->Unadvise();
        g_visualTreeWatcher = nullptr;
    }

    if (g_target == Target::explorer) {
        if (HWND host = FindTaskbarXamlHost()) {
            RunFromWindowThread(host, [](PVOID) { RestoreTaskbar(); });
        }
    } else if (HWND window = FindProcessWindow(L"ControlCenterWindow")) {
        RunFromWindowThread(window, [](PVOID) { RestoreQuickSettings(); });
    }
}
