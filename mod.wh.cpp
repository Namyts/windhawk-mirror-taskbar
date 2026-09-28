// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Reverses the taskbar order: on a left/right taskbar Start goes to the bottom, apps stack upwards from it, and the clock goes to the top
// @version         3.2
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

////////////////////////////////////////////////////////////////////////////////
// Taskbar (explorer.exe).
//
// The element holding both the taskbar frame and the tray frame (the root)
// gets a -1 scale along the taskbar's length, which reverses the order of
// everything in it. Each taskbar button and tray icon gets the same -1 scale
// again, so its contents end up the right way round. Hit testing honors render
// transforms, so clicks follow what's on screen.

struct FlippedElement {
    winrt::weak_ref<wux::FrameworkElement> element;
    wuxm::ScaleTransform scale;
    wuxm::Transform originalTransform{nullptr};
    wf::Point originalOrigin;
};

// Only touched from the taskbar's UI thread.
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

// Adds to any transform already there, so that styling mods keep working.
void Flip(const wux::FrameworkElement& element) {
    std::erase_if(g_flipped,
                  [](const FlippedElement& f) { return !f.element.get(); });
    if (IsFlipped(element)) {
        return;
    }

    FlippedElement flipped{
        .element = winrt::make_weak(element),
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

// Taskbar buttons (Start, search, apps) are the repeater's children. Tray
// icons (chevron, app icons, network/volume, clock, ...) are
// SystemTray.*IconView elements, and one inside another must be left alone or
// it would end up upside down.
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

// The root is the closest element containing both frames. Secondary taskbars
// have no tray frame, so only the main taskbar gets one.
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

// Quick settings are a full-height window with the panel aligned to the
// bottom, which is where the tray sits before mirroring. The panel is pinned
// to the top instead. Windows sets the alignment again whenever the panel
// opens, so it's put back each time.
struct PinnedElement {
    winrt::weak_ref<wux::FrameworkElement> element;
    wux::VerticalAlignment alignment;
    wux::VerticalAlignment original;
    int64_t token{};
};

std::vector<PinnedElement> g_pinned;
bool g_pinning;

bool IsQuickSettingsPage(const wux::DependencyObject& element) {
    return element &&
           winrt::get_class_name(element) == L"ControlCenter.ControlCenterPage";
}

bool HasName(const wux::DependencyObject& element, std::wstring_view name) {
    auto fe = element.try_as<wux::FrameworkElement>();
    return fe && fe.Name() == name;
}

wux::FrameworkElement FindChild(const wux::DependencyObject& element,
                                std::wstring_view name) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        if (HasName(child, name)) {
            return child.as<wux::FrameworkElement>();
        }
    }
    return nullptr;
}

void Pin(const wux::FrameworkElement& element, wux::VerticalAlignment alignment) {
    for (const auto& pinned : g_pinned) {
        if (pinned.element.get() == element) {
            if (element.VerticalAlignment() != alignment) {
                g_pinning = true;
                element.VerticalAlignment(alignment);
                g_pinning = false;
            }
            return;
        }
    }

    auto original = element.VerticalAlignment();
    g_pinning = true;
    element.VerticalAlignment(alignment);
    g_pinning = false;

    int64_t token = element.RegisterPropertyChangedCallback(
        wux::FrameworkElement::VerticalAlignmentProperty(),
        [](const wux::DependencyObject& sender, const wux::DependencyProperty&) {
            if (g_pinning || g_unloading) {
                return;
            }
            auto fe = sender.try_as<wux::FrameworkElement>();
            for (const auto& pinned : g_pinned) {
                if (fe && pinned.element.get() == fe &&
                    fe.VerticalAlignment() != pinned.alignment) {
                    g_pinning = true;
                    fe.VerticalAlignment(pinned.alignment);
                    g_pinning = false;
                    break;
                }
            }
        });
    g_pinned.push_back({winrt::make_weak(element), alignment, original, token});
}

void RestorePins() {
    g_pinning = true;
    for (const auto& pinned : g_pinned) {
        auto element = pinned.element.get();
        if (!element) {
            continue;
        }
        element.UnregisterPropertyChangedCallback(
            wux::FrameworkElement::VerticalAlignmentProperty(), pinned.token);
        if (element.VerticalAlignment() == pinned.alignment) {
            element.VerticalAlignment(pinned.original);
        }
    }
    g_pinned.clear();
    g_pinning = false;
}

// Elements keep being created over time, e.g. a button for each new app.
void OnElementAdded(const wux::FrameworkElement& element,
                    const wux::DependencyObject& parent,
                    std::wstring_view type) {
    if (g_target == Target::shell) {
        if (!IsTaskbarVertical()) {
            return;
        }

        // The page and its RootGrid are stretched to the full height, and the
        // RootContent inside (the visible panel) is aligned to the top.
        // Other elements named RootGrid exist deeper inside and are left alone.
        auto pinContent = [](const wux::FrameworkElement& rootGrid) {
            Pin(rootGrid, wux::VerticalAlignment::Stretch);
            if (auto content = FindChild(rootGrid, L"RootContent")) {
                Pin(content, wux::VerticalAlignment::Top);
            }
        };

        if (type == L"ControlCenter.ControlCenterPage") {
            Pin(element, wux::VerticalAlignment::Stretch);
            if (auto rootGrid = FindChild(element, L"RootGrid")) {
                pinContent(rootGrid);
            }
        } else if (element.Name() == L"RootGrid" && IsQuickSettingsPage(parent)) {
            pinContent(element);
        } else if (element.Name() == L"RootContent" && parent &&
                   HasName(parent, L"RootGrid") &&
                   IsQuickSettingsPage(GetParent(parent))) {
            Pin(element, wux::VerticalAlignment::Top);
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

// The XAML composition diagnostics corrupt the heap when several UI threads add
// visuals at once. Only element changes are needed, so they're turned off by
// answering the one registry read AdviseVisualTreeChange makes for
// DisableCompositionDiag.
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

    HMODULE wux = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initializeXamlDiagnosticsEx =
        wux ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
                  GetProcAddress(wux, "InitializeXamlDiagnosticsEx"))
            : nullptr;
    if (!initializeXamlDiagnosticsEx || g_tapInjected.exchange(true)) {
        return;
    }

    // There's no way to know which connection name is free, so try them in
    // order. A failure leaves another attempt possible, since ShellHost's XAML
    // may not exist yet the first time this runs.
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

// Catches the taskbar being created when explorer starts with the mod enabled.
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
// The hidden-icons flyout. Windows still places it for where the tray was, so
// one that would open on the far half of the taskbar is mirrored across the
// middle. Quick settings are not a movable window: they fill the screen height
// and the panel inside them is moved instead, above.

bool IsTrayFlyout(HWND hwnd, const RECT& taskbar, bool vertical, int middle) {
    WCHAR className[64];
    if (!GetClassName(hwnd, className, ARRAYSIZE(className))) {
        return false;
    }

    if (_wcsicmp(className, L"TopLevelWindowForOverflowXamlIsland") == 0) {
        return true;
    }

    // Explorer also uses this class for app thumbnails and Alt+Tab, so it
    // only counts when the tray's half of the taskbar was just clicked.
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
// Start menu (StartMenuExperienceHost.exe).
//
// The Start menu window covers the whole monitor and the menu is placed inside
// it with XAML. Windows anchors it to the Start button's end of the taskbar
// (the top, here), so the menu is anchored to the other end instead. The
// position is set outright, which makes it safe to apply again when the mod is
// reloaded: a menu already at the bottom stays there.

namespace StartMenu {

// The redesigned menu is aligned inside FrameRoot, the older one is positioned
// on a canvas.
struct State {
    winrt::weak_ref<wux::FrameworkElement> element;
    bool vertical = false;
    std::vector<std::pair<wux::DependencyProperty, int64_t>> watchTokens;
};

State g_state;
winrt::event_token g_visibilityChangedToken;
bool g_applying;

// Puts the menu at the far end of the taskbar, or back at Windows' end when
// undoing. Both are absolute, so repeating either one changes nothing.
void Place(const wux::FrameworkElement& element, bool vertical, bool undo) {
    auto content = GetParent(element).try_as<wux::FrameworkElement>();
    if (content && content.try_as<wux::Controls::Canvas>()) {
        double span = vertical ? content.ActualHeight() : content.ActualWidth();
        double size = vertical ? element.ActualHeight() : element.ActualWidth();
        if (span <= 0 || size <= 0) {
            return;
        }

        double current = vertical ? wux::Controls::Canvas::GetTop(element)
                                  : wux::Controls::Canvas::GetLeft(element);
        double mirrored = span - size - current;
        double desired = undo ? (current < mirrored ? current : mirrored)
                              : (current > mirrored ? current : mirrored);
        if (current != desired) {
            vertical ? wux::Controls::Canvas::SetTop(element, desired)
                     : wux::Controls::Canvas::SetLeft(element, desired);
        }
        return;
    }

    auto margin = element.Margin();
    if (vertical) {
        double gap = margin.Top + margin.Bottom;
        margin.Top = undo ? 0 : gap;
        margin.Bottom = undo ? gap : 0;
        auto alignment = undo ? wux::VerticalAlignment::Top
                              : wux::VerticalAlignment::Bottom;
        if (element.VerticalAlignment() != alignment) {
            element.VerticalAlignment(alignment);
        }
    } else {
        double gap = margin.Left + margin.Right;
        margin.Left = undo ? 0 : gap;
        margin.Right = undo ? gap : 0;
        auto alignment = undo ? wux::HorizontalAlignment::Left
                              : wux::HorizontalAlignment::Right;
        if (element.HorizontalAlignment() != alignment) {
            element.HorizontalAlignment(alignment);
        }
    }
    auto current = element.Margin();
    if (current.Left != margin.Left || current.Top != margin.Top ||
        current.Right != margin.Right || current.Bottom != margin.Bottom) {
        element.Margin(margin);
    }
}

void Apply();

void Watch(const wux::FrameworkElement& element) {
    for (auto property : {wux::FrameworkElement::VerticalAlignmentProperty(),
                          wux::FrameworkElement::HorizontalAlignmentProperty(),
                          wux::FrameworkElement::MarginProperty(),
                          wux::Controls::Canvas::TopProperty(),
                          wux::Controls::Canvas::LeftProperty()}) {
        int64_t token = element.RegisterPropertyChangedCallback(
            property,
            [](const wux::DependencyObject&, const wux::DependencyProperty&) {
                if (!g_applying) {
                    Apply();
                }
            });
        g_state.watchTokens.emplace_back(property, token);
    }
}

void Restore() {
    auto element = g_state.element.get();
    if (element) {
        Place(element, g_state.vertical, /*undo=*/true);
        for (const auto& [property, token] : g_state.watchTokens) {
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
        return e.Name() == L"FrameRoot" ||
               winrt::get_class_name(e) == L"StartDocked.StartSizingFrame";
    });
    if (!element) {
        Wh_Log(L"Start menu element not found");
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

HWND FindCoreWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD processId = 0;
            WCHAR className[64];
            if (GetWindowThreadProcessId(hwnd, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassName(hwnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
                *reinterpret_cast<HWND*>(param) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// The Start menu's XAML is created on demand. Creating its island is the
// earliest point where Window::Current is available on its thread.
using RoGetActivationFactory_t = decltype(&RoGetActivationFactory);
RoGetActivationFactory_t RoGetActivationFactory_Original;
HRESULT WINAPI RoGetActivationFactory_Hook(HSTRING classId,
                                           REFIID iid,
                                           void** factory) {
    thread_local bool inHook;
    if (!inHook) {
        inHook = true;
        if (wcscmp(WindowsGetStringRawBuffer(classId, nullptr),
                   L"Windows.UI.Xaml.Hosting.XamlIsland") == 0) {
            Init();
        }
        inHook = false;
    }
    return RoGetActivationFactory_Original(classId, iid, factory);
}

}  // namespace StartMenu

////////////////////////////////////////////////////////////////////////////////
// Quick settings live in ShellHost.exe. Its XAML is created on demand, and
// creating an island is the moment it's ready to be connected to.

using ShellRoGetActivationFactory_t = decltype(&RoGetActivationFactory);
ShellRoGetActivationFactory_t ShellRoGetActivationFactory_Original;
HRESULT WINAPI ShellRoGetActivationFactory_Hook(HSTRING classId,
                                               REFIID iid,
                                               void** factory) {
    thread_local bool inHook;
    if (!inHook && classId) {
        inHook = true;
        if (wcscmp(WindowsGetStringRawBuffer(classId, nullptr),
                   L"Windows.UI.Xaml.Hosting.XamlIsland") == 0) {
            InjectWindhawkTAP();
        }
        inHook = false;
    }
    return ShellRoGetActivationFactory_Original(classId, iid, factory);
}

HWND FindShellWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD processId = 0;
            WCHAR className[64];
            if (GetWindowThreadProcessId(hwnd, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassName(hwnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"ControlCenterWindow") == 0) {
                *reinterpret_cast<HWND*>(param) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

////////////////////////////////////////////////////////////////////////////////
// Entry points.

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

BOOL Wh_ModInit() {
    WCHAR path[MAX_PATH]{};
    GetModuleFileName(nullptr, path, ARRAYSIZE(path));
    PCWSTR name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    g_target = _wcsicmp(name, L"explorer.exe") == 0 ? Target::explorer
               : _wcsicmp(name, L"StartMenuExperienceHost.exe") == 0
                   ? Target::startMenu
                   : Target::shell;

    if (g_target == Target::startMenu) {
        HMODULE winrt = GetModuleHandle(L"api-ms-win-core-winrt-l1-1-0.dll");
        WindhawkUtils::SetFunctionHook(
            (StartMenu::RoGetActivationFactory_t)GetProcAddress(
                winrt, "RoGetActivationFactory"),
            StartMenu::RoGetActivationFactory_Hook,
            &StartMenu::RoGetActivationFactory_Original);
        return TRUE;
    }

    if (g_target == Target::explorer) {
        WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                       &SetWindowPos_Original);
        WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                       &CreateWindowExW_Original);
    } else if (_wcsicmp(name, L"ShellHost.exe") != 0) {
        return FALSE;
    } else if (HMODULE winrt =
                   GetModuleHandle(L"api-ms-win-core-winrt-l1-1-0.dll")) {
        if (auto roGetActivationFactory =
                (ShellRoGetActivationFactory_t)GetProcAddress(
                    winrt, "RoGetActivationFactory")) {
            WindhawkUtils::SetFunctionHook(
                roGetActivationFactory, ShellRoGetActivationFactory_Hook,
                &ShellRoGetActivationFactory_Original);
        }
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
    if (HWND coreWindow = StartMenu::FindCoreWindow()) {
        RunFromWindowThread(coreWindow, [](PVOID) { StartMenu::Init(); });
    }
}

// The Start menu's window often doesn't exist yet when the mod loads, and it
// isn't recreated just because Explorer restarted. Keep trying briefly so
// enabling the mod updates the menu that's already running.
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
            HANDLE thread =
                CreateThread(nullptr, 0, StartMenuAttachThread, nullptr, 0,
                             nullptr);
            if (thread) {
                CloseHandle(thread);
            }
        }
    } else if (g_target == Target::explorer) {
        if (FindTaskbarXamlHost()) {
            InjectWindhawkTAP();
        }
    } else if (FindShellWindow()) {
        InjectWindhawkTAP();
    }
}

void Wh_ModUninit() {
    g_unloading = true;

    if (g_target == Target::startMenu) {
        if (HWND coreWindow = StartMenu::FindCoreWindow()) {
            RunFromWindowThread(coreWindow,
                                [](PVOID) { StartMenu::Uninit(); });
        }
    } else if (g_target == Target::explorer) {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->Unadvise();
            g_visualTreeWatcher = nullptr;
        }
        if (HWND host = FindTaskbarXamlHost()) {
            RunFromWindowThread(host, [](PVOID) { RestoreTaskbar(); });
        }
    } else if (g_target == Target::shell) {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->Unadvise();
            g_visualTreeWatcher = nullptr;
        }
        if (HWND window = FindShellWindow()) {
            RunFromWindowThread(window, [](PVOID) { RestorePins(); });
        }
    }
}
