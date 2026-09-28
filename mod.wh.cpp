// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Reverses the taskbar order: on a left/right taskbar Start goes to the bottom, apps stack upwards from it, and the clock goes to the top
// @version         2.1
// @author          namyts
// @include         explorer.exe
// @include         StartMenuExperienceHost.exe
// @include         ShellExperienceHost.exe
// @include         ShellHost.exe
// @include         EarTrumpet.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Mirrored Taskbar Layout

Reverses the order of the Windows 11 taskbar along its length, without flipping
any icons or text.

* Left/right taskbar: Start at the bottom, apps stacked upwards from it (first
  app next to Start), clock and notification area at the top.
* Top/bottom taskbar: Start on the right, clock and notification area on the
  left.

The Start menu and the tray flyouts (hidden icons, quick settings, notification
center) open on the matching side.

Tray apps which place their popup where the tray used to be (EarTrumpet is
included) are fixed the same way: a popup touching the taskbar on the far half
is mirrored next to the tray. Other tray apps can be added in the mod's
Advanced tab, under the custom process inclusion list.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <roapi.h>
#include <windowsx.h>
#include <winstring.h>
#include <xamlom.h>

#include <algorithm>
#include <atomic>
#include <optional>
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

////////////////////////////////////////////////////////////////////////////////
// Mirroring.
//
// The element holding both the taskbar frame and the tray frame (the "root")
// gets a -1 scale along the taskbar's length, which reverses the order of
// everything in it. Each taskbar button and each tray icon get the same -1
// scale again, so their contents end up the right way round. Render transforms
// are honored by hit testing, so clicks follow what's on screen.

struct FlippedElement {
    winrt::weak_ref<wux::FrameworkElement> element;
    wuxm::ScaleTransform scale{nullptr};
    wuxm::Transform originalTransform{nullptr};
    wf::Point originalOrigin{};
    size_t rootIndex;
};

struct MirrorRoot {
    winrt::weak_ref<wux::FrameworkElement> element;
    winrt::weak_ref<wux::FrameworkElement> taskbarFrame;
    winrt::weak_ref<wux::FrameworkElement> trayFrame;
    winrt::event_token sizeChangedToken{};
    bool vertical = false;
};

// Only touched from the taskbar's UI thread.
std::vector<winrt::weak_ref<wux::FrameworkElement>> g_taskbarFrames;
std::vector<winrt::weak_ref<wux::FrameworkElement>> g_trayFrames;
std::vector<MirrorRoot> g_roots;
std::vector<FlippedElement> g_flipped;

bool IsVertical(const wux::FrameworkElement& element) {
    return element.ActualHeight() > element.ActualWidth();
}

void SetScaleAxis(const wuxm::ScaleTransform& scale, bool vertical) {
    scale.ScaleX(vertical ? 1 : -1);
    scale.ScaleY(vertical ? -1 : 1);
}

bool IsFlipped(const wux::FrameworkElement& element) {
    for (const auto& flipped : g_flipped) {
        if (flipped.element.get() == element) {
            return true;
        }
    }
    return false;
}

void Flip(const wux::FrameworkElement& element, size_t rootIndex) {
    std::erase_if(g_flipped, [](const FlippedElement& flipped) {
        return !flipped.element.get();
    });

    if (IsFlipped(element)) {
        return;
    }

    FlippedElement flipped;
    flipped.element = winrt::make_weak(element);
    flipped.originalTransform = element.RenderTransform();
    flipped.originalOrigin = element.RenderTransformOrigin();
    flipped.rootIndex = rootIndex;
    flipped.scale = wuxm::ScaleTransform();
    SetScaleAxis(flipped.scale, g_roots[rootIndex].vertical);

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

void Unflip(FlippedElement& flipped) {
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

wux::DependencyObject GetParent(const wux::DependencyObject& element) {
    return wuxm::VisualTreeHelper::GetParent(element);
}

bool IsAncestorOf(const wux::DependencyObject& ancestor,
                  wux::DependencyObject element) {
    while (element) {
        if (element == ancestor) {
            return true;
        }
        element = GetParent(element);
    }
    return false;
}

wux::FrameworkElement FindByName(const wux::DependencyObject& element,
                                 std::wstring_view name,
                                 int depth = 0) {
    if (depth > 12) {
        return nullptr;
    }

    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        if (auto fe = child.try_as<wux::FrameworkElement>();
            fe && fe.Name() == name) {
            return fe;
        }
        if (auto found = FindByName(child, name, depth + 1)) {
            return found;
        }
    }
    return nullptr;
}

// Index of the root that contains the element, or -1.
int FindRootIndex(const wux::DependencyObject& element) {
    for (size_t i = 0; i < g_roots.size(); i++) {
        auto root = g_roots[i].element.get();
        if (root && IsAncestorOf(root, element)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Tray icons (the chevron, app icons, network/volume, the clock, ...) are all
// SystemTray.*IconView elements.
bool IsTrayIconType(std::wstring_view type) {
    return type.starts_with(L"SystemTray.") && type.ends_with(L"IconView");
}

// A tray icon inside an already flipped one must not be flipped again, or it
// would end up upside down.
bool HasFlippedAncestor(const wux::DependencyObject& element,
                        const wux::DependencyObject& root) {
    for (auto ancestor = GetParent(element); ancestor && ancestor != root;
         ancestor = GetParent(ancestor)) {
        if (auto fe = ancestor.try_as<wux::FrameworkElement>();
            fe && IsFlipped(fe)) {
            return true;
        }
    }
    return false;
}

void FlipTrayIcons(const wux::DependencyObject& element,
                   size_t rootIndex,
                   int depth = 0) {
    if (depth > 20) {
        return;
    }

    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        auto fe = child.try_as<wux::FrameworkElement>();
        if (fe && IsTrayIconType(winrt::get_class_name(fe))) {
            Flip(fe, rootIndex);
            continue;
        }
        FlipTrayIcons(child, rootIndex, depth + 1);
    }
}

void FlipTaskbarButtons(size_t rootIndex) {
    auto taskbarFrame = g_roots[rootIndex].taskbarFrame.get();
    if (!taskbarFrame) {
        return;
    }

    auto repeater = FindByName(taskbarFrame, L"TaskbarFrameRepeater");
    if (!repeater) {
        Wh_Log(L"TaskbarFrameRepeater not found");
        return;
    }

    int count = wuxm::VisualTreeHelper::GetChildrenCount(repeater);
    for (int i = 0; i < count; i++) {
        if (auto button = wuxm::VisualTreeHelper::GetChild(repeater, i)
                              .try_as<wux::FrameworkElement>()) {
            Flip(button, rootIndex);
        }
    }
}

void OnRootSizeChanged(size_t rootIndex) {
    if (rootIndex >= g_roots.size()) {
        return;
    }

    auto root = g_roots[rootIndex].element.get();
    if (!root) {
        return;
    }

    bool vertical = IsVertical(root);
    if (vertical == g_roots[rootIndex].vertical) {
        return;
    }

    Wh_Log(L"Taskbar orientation: %s", vertical ? L"vertical" : L"horizontal");
    g_roots[rootIndex].vertical = vertical;
    for (auto& flipped : g_flipped) {
        if (flipped.rootIndex == rootIndex && flipped.element.get()) {
            SetScaleAxis(flipped.scale, vertical);
        }
    }
}

// The root is the closest element that contains both frames.
void TryCreateRoots() {
    for (const auto& weakTaskbarFrame : g_taskbarFrames) {
        auto taskbarFrame = weakTaskbarFrame.get();
        if (!taskbarFrame || FindRootIndex(taskbarFrame) != -1) {
            continue;
        }

        for (const auto& weakTrayFrame : g_trayFrames) {
            auto trayFrame = weakTrayFrame.get();
            if (!trayFrame) {
                continue;
            }

            wux::DependencyObject ancestor = GetParent(taskbarFrame);
            while (ancestor && !IsAncestorOf(ancestor, trayFrame)) {
                ancestor = GetParent(ancestor);
            }

            auto root = ancestor.try_as<wux::FrameworkElement>();
            if (!root) {
                continue;
            }

            Wh_Log(L"Mirroring taskbar, root %s",
                   winrt::get_class_name(root).c_str());

            size_t rootIndex = g_roots.size();
            MirrorRoot mirrorRoot;
            mirrorRoot.element = winrt::make_weak(root);
            mirrorRoot.taskbarFrame = winrt::make_weak(taskbarFrame);
            mirrorRoot.trayFrame = winrt::make_weak(trayFrame);
            mirrorRoot.vertical = IsVertical(root);
            mirrorRoot.sizeChangedToken = root.SizeChanged(
                [rootIndex](const wf::IInspectable&,
                            const wux::SizeChangedEventArgs&) {
                    OnRootSizeChanged(rootIndex);
                });
            g_roots.push_back(std::move(mirrorRoot));

            Flip(root, rootIndex);
            FlipTrayIcons(trayFrame, rootIndex);
            FlipTaskbarButtons(rootIndex);
            break;
        }
    }
}

void OnElementAdded(const wux::FrameworkElement& element,
                    const wux::FrameworkElement& parent,
                    std::wstring_view type) {
    if (type == L"Taskbar.TaskbarFrame") {
        g_taskbarFrames.push_back(winrt::make_weak(element));
        TryCreateRoots();
        return;
    }

    if (type == L"SystemTray.SystemTrayFrame") {
        g_trayFrames.push_back(winrt::make_weak(element));
        TryCreateRoots();
        return;
    }

    // Taskbar buttons (Start, search, apps) are created and recycled by the
    // repeater over time.
    if (parent && parent.Name() == L"TaskbarFrameRepeater") {
        int rootIndex = FindRootIndex(parent);
        if (rootIndex != -1) {
            Flip(element, rootIndex);
        }
        return;
    }

    if (IsTrayIconType(type)) {
        int rootIndex = FindRootIndex(element);
        if (rootIndex == -1) {
            return;
        }

        auto trayFrame = g_roots[rootIndex].trayFrame.get();
        auto root = g_roots[rootIndex].element.get();
        if (trayFrame && IsAncestorOf(trayFrame, element) &&
            !HasFlippedAncestor(element, root)) {
            Flip(element, rootIndex);
        }
    }
}

void RestoreAll() {
    for (auto& flipped : g_flipped) {
        Unflip(flipped);
    }
    g_flipped.clear();

    for (auto& root : g_roots) {
        if (auto element = root.element.get()) {
            element.SizeChanged(root.sizeChangedToken);
        }
    }
    g_roots.clear();
    g_taskbarFrames.clear();
    g_trayFrames.clear();
}

////////////////////////////////////////////////////////////////////////////////
// XAML diagnostics: gets a callback for every element in explorer's XAML trees.

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
// visuals at once. Only element mutations are needed, so the composition
// diagnostics are turned off by answering the single registry read which
// AdviseVisualTreeChange makes for DisableCompositionDiag.
thread_local bool g_reportCompositionDiagAsDisabled;

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher,
                               IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : m_xamlDiagnostics(site.as<IXamlDiagnostics>()) {
        // Advising from the calling thread can hang, so it's done from a new
        // thread.
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
        if (mutationType != Add) {
            return S_OK;
        }

        auto frameworkElement =
            FromHandle(element.Handle).try_as<wux::FrameworkElement>();
        if (!frameworkElement) {
            return S_OK;
        }

        wux::FrameworkElement parent{nullptr};
        if (relation.Parent) {
            parent = FromHandle(relation.Parent).try_as<wux::FrameworkElement>();
        }

        OnElementAdded(frameworkElement, parent,
                       element.Type ? element.Type : L"");
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
    if (g_tapInjected.exchange(true)) {
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
    if (!initializeXamlDiagnosticsEx) {
        Wh_Log(L"InitializeXamlDiagnosticsEx not found");
        return;
    }

    // There's no way to know which connection name is free, so try them in
    // order.
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
    Wh_Log(L"InitializeXamlDiagnosticsEx: %08X", hr);
}

////////////////////////////////////////////////////////////////////////////////
// Hooks.

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

bool IsTaskbarXamlHost(HWND hwnd, HWND parent) {
    WCHAR className[64];
    return parent &&
           GetClassName(hwnd, className, ARRAYSIZE(className)) &&
           _wcsicmp(className,
                    L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 &&
           GetClassName(parent, className, ARRAYSIZE(className)) &&
           _wcsicmp(className, L"Shell_TrayWnd") == 0;
}

// Picks up the taskbar when explorer starts with the mod already enabled.
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
    if (hwnd && IsTaskbarXamlHost(hwnd, parent)) {
        Wh_Log(L"Taskbar XAML host created");
        InjectWindhawkTAP();
    }
    return hwnd;
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

bool RunFromWindowThread(HWND hwnd, RunFromWindowThreadProc_t proc, PVOID param) {
    if (!g_runFromWindowThreadMsg) {
        g_runFromWindowThreadMsg =
            RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    }

    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    if (!threadId) {
        return false;
    }
    if (threadId == GetCurrentThreadId()) {
        proc(param);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(WH_CALLWNDPROC, RunFromWindowThreadHook,
                                  nullptr, threadId);
    if (!hook) {
        return false;
    }

    RunFromWindowThreadParam data{proc, param};
    SendMessage(hwnd, g_runFromWindowThreadMsg, 0, (LPARAM)&data);
    UnhookWindowsHookEx(hook);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Tray flyouts (hidden icons, quick settings, notification center).
//
// Windows still positions them for where the tray was. The tray is now on the
// other half of the taskbar, so a flyout which would open on the far half is
// mirrored across the taskbar's middle, which puts it next to the tray again.
// Mirroring only when it's on the far half keeps repeated moves stable.

std::atomic<bool> g_unloading;

bool GetTaskbarRect(RECT* rect) {
    HWND taskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    return taskbar && GetWindowRect(taskbar, rect);
}

std::wstring GetThreadDescriptionOf(DWORD threadId) {
    std::wstring result;
    HANDLE thread =
        OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (thread) {
        PWSTR description;
        if (SUCCEEDED(GetThreadDescription(thread, &description))) {
            result = description;
            LocalFree(description);
        }
        CloseHandle(thread);
    }
    return result;
}

// explorer: the taskbar itself. startMenu: StartMenuExperienceHost.exe.
// shellFlyouts: ShellHost.exe and ShellExperienceHost.exe. app: any other
// included process, such as tray apps like EarTrumpet.
enum class Target { explorer, startMenu, shellFlyouts, app };
Target g_target;

enum class FlyoutKind {
    none,
    // Always belongs next to the tray.
    tray,
    // A generic explorer popup, also used for app thumbnails and Alt+Tab. Only
    // treated as a tray flyout when the tray area was just clicked.
    trayIfClicked,
    // A tray app's popup. Treated as a tray flyout when it sits against the
    // taskbar.
    trayIfTouchingTaskbar,
};

FlyoutKind GetFlyoutKind(HWND hwnd) {
    if (g_target == Target::app) {
        bool topLevel = !(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD) &&
                        GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow();
        return topLevel ? FlyoutKind::trayIfTouchingTaskbar : FlyoutKind::none;
    }

    WCHAR className[64];
    if (!GetClassName(hwnd, className, ARRAYSIZE(className))) {
        return FlyoutKind::none;
    }

    if (_wcsicmp(className, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
        _wcsicmp(className, L"ControlCenterWindow") == 0) {
        return FlyoutKind::tray;
    }

    if (_wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0) {
        return FlyoutKind::trayIfClicked;
    }

    // Jump lists are core windows too, and they belong next to their app, so
    // only the tray's core windows are picked by their thread.
    if (_wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
        DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
        std::wstring description = GetThreadDescriptionOf(threadId);
        if (description == L"ActionCenter" || description == L"QuickActions") {
            return FlyoutKind::tray;
        }
    }

    return FlyoutKind::none;
}

void AdjustFlyoutPos(HWND hwnd, int& x, int& y, int cx, int cy) {
    if (g_unloading) {
        return;
    }

    FlyoutKind kind = GetFlyoutKind(hwnd);
    if (kind == FlyoutKind::none) {
        return;
    }

    RECT taskbar;
    if (!GetTaskbarRect(&taskbar)) {
        return;
    }

    bool vertical =
        taskbar.bottom - taskbar.top > taskbar.right - taskbar.left;
    int middle = vertical ? (taskbar.top + taskbar.bottom) / 2
                          : (taskbar.left + taskbar.right) / 2;

    if (kind == FlyoutKind::trayIfClicked) {
        DWORD messagePos = GetMessagePos();
        POINT pt{GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos)};
        bool clickedTrayHalf = PtInRect(&taskbar, pt) &&
                               (vertical ? pt.y < middle : pt.x < middle);
        if (!clickedTrayHalf) {
            return;
        }
    }

    if (kind == FlyoutKind::trayIfTouchingTaskbar) {
        // Gap between the window and the taskbar, across the taskbar.
        int gap = vertical
                      ? std::max<int>(taskbar.left - (x + cx), x - taskbar.right)
                      : std::max<int>(taskbar.top - (y + cy), y - taskbar.bottom);
        int maxGap = MulDiv(48, GetDpiForWindow(hwnd), 96);
        if (gap > maxGap || cx <= 0 || cy <= 0) {
            return;
        }
    }

    int center = vertical ? y + cy / 2 : x + cx / 2;
    if (center <= middle) {
        return;
    }

    if (vertical) {
        y = taskbar.top + taskbar.bottom - y - cy;
    } else {
        x = taskbar.left + taskbar.right - x - cx;
    }

    WCHAR className[64]{};
    GetClassName(hwnd, className, ARRAYSIZE(className));
    Wh_Log(L"Moved flyout %p (%s) to %d,%d", hwnd, className, x, y);
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;

// For windows which were positioned earlier and are only being shown now.
void AdjustCurrentPos(HWND hwnd) {
    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return;
    }

    int x = rect.left;
    int y = rect.top;
    AdjustFlyoutPos(hwnd, x, y, rect.right - rect.left,
                    rect.bottom - rect.top);
    if (x != rect.left || y != rect.top) {
        SetWindowPos_Original(hwnd, nullptr, x, y, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

BOOL WINAPI SetWindowPos_Hook(HWND hwnd,
                              HWND insertAfter,
                              int x,
                              int y,
                              int cx,
                              int cy,
                              UINT flags) {
    if ((flags & SWP_NOMOVE) && (flags & SWP_SHOWWINDOW) &&
        g_target != Target::explorer) {
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        int newX = rect.left;
        int newY = rect.top;
        int width = (flags & SWP_NOSIZE) ? rect.right - rect.left : cx;
        int height = (flags & SWP_NOSIZE) ? rect.bottom - rect.top : cy;
        AdjustFlyoutPos(hwnd, newX, newY, width, height);
        if (newX != rect.left || newY != rect.top) {
            x = newX;
            y = newY;
            flags &= ~SWP_NOMOVE;
        }
    } else if (!(flags & SWP_NOMOVE)) {
        int width = cx;
        int height = cy;
        if (flags & SWP_NOSIZE) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        AdjustFlyoutPos(hwnd, x, y, width, height);
    }
    return SetWindowPos_Original(hwnd, insertAfter, x, y, cx, cy, flags);
}

using MoveWindow_t = decltype(&MoveWindow);
MoveWindow_t MoveWindow_Original;
BOOL WINAPI MoveWindow_Hook(HWND hwnd,
                            int x,
                            int y,
                            int width,
                            int height,
                            BOOL repaint) {
    AdjustFlyoutPos(hwnd, x, y, width, height);
    return MoveWindow_Original(hwnd, x, y, width, height, repaint);
}

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original;
BOOL WINAPI ShowWindow_Hook(HWND hwnd, int cmdShow) {
    if (cmdShow != SW_HIDE && cmdShow != SW_MINIMIZE &&
        cmdShow != SW_SHOWMINIMIZED && cmdShow != SW_SHOWMINNOACTIVE &&
        cmdShow != SW_FORCEMINIMIZE) {
        AdjustCurrentPos(hwnd);
    }
    return ShowWindow_Original(hwnd, cmdShow);
}

// Shell flyouts are kept alive and hidden by cloaking, so uncloaking is when
// they're shown.
using DwmSetWindowAttribute_t = decltype(&DwmSetWindowAttribute);
DwmSetWindowAttribute_t DwmSetWindowAttribute_Original;
HRESULT WINAPI DwmSetWindowAttribute_Hook(HWND hwnd,
                                          DWORD attribute,
                                          LPCVOID value,
                                          DWORD size) {
    if (attribute == DWMWA_CLOAK && size == sizeof(BOOL) && value &&
        !*static_cast<const BOOL*>(value)) {
        AdjustCurrentPos(hwnd);
    }
    return DwmSetWindowAttribute_Original(hwnd, attribute, value, size);
}

////////////////////////////////////////////////////////////////////////////////
// Start menu (runs in StartMenuExperienceHost.exe).
//
// The Start menu window covers the whole monitor, and the menu is placed inside
// it with XAML, so it's moved there. The value Windows picked is mirrored:
// top-aligned becomes bottom-aligned, left becomes right.

namespace StartMenu {

template <typename T>
class PropertyOverride {
   public:
    using PropertyGetter = wux::DependencyProperty (*)();

    explicit PropertyOverride(PropertyGetter property) : m_property(property) {}

    // The value Windows set, as opposed to the one set here.
    std::optional<T> WindowsValue(const wux::DependencyObject& element) {
        auto local = element.ReadLocalValue(m_property()).try_as<T>();
        if (m_element.get() == element && local == m_value) {
            return m_windowsValue;
        }
        return local;
    }

    void Set(const wux::DependencyObject& element, T value) {
        auto local = element.ReadLocalValue(m_property()).try_as<T>();
        if (m_element.get() != element || local != m_value) {
            m_element = winrt::make_weak(element);
            m_windowsValue = local;
        }
        m_value = value;
        element.SetValue(m_property(), winrt::box_value(value));
    }

    void Restore() {
        auto element = m_element.get();
        m_element = nullptr;
        if (!element ||
            element.ReadLocalValue(m_property()).try_as<T>() != m_value) {
            return;
        }
        if (m_windowsValue) {
            element.SetValue(m_property(), winrt::box_value(*m_windowsValue));
        } else {
            element.ClearValue(m_property());
        }
    }

   private:
    PropertyGetter m_property;
    winrt::weak_ref<wux::DependencyObject> m_element;
    std::optional<T> m_windowsValue;
    T m_value{};
};

PropertyOverride<double> g_canvasTop{&wux::Controls::Canvas::TopProperty};
PropertyOverride<double> g_canvasLeft{&wux::Controls::Canvas::LeftProperty};
PropertyOverride<wux::VerticalAlignment> g_verticalAlignment{
    &wux::FrameworkElement::VerticalAlignmentProperty};
PropertyOverride<wux::HorizontalAlignment> g_horizontalAlignment{
    &wux::FrameworkElement::HorizontalAlignmentProperty};
PropertyOverride<wux::Thickness> g_margin{
    &wux::FrameworkElement::MarginProperty};

winrt::event_token g_visibilityChangedToken;
winrt::weak_ref<wux::DependencyObject> g_watchedElement;
std::vector<std::pair<wux::DependencyProperty, int64_t>> g_watchTokens;
bool g_inApply;

void Apply();

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

wux::FrameworkElement FindByClass(const wux::DependencyObject& element,
                                  std::wstring_view className,
                                  int depth = 0) {
    if (depth > 12) {
        return nullptr;
    }

    int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(element, i);
        if (auto fe = child.try_as<wux::FrameworkElement>();
            fe && winrt::get_class_name(fe) == className) {
            return fe;
        }
        if (auto found = FindByClass(child, className, depth + 1)) {
            return found;
        }
    }
    return nullptr;
}

// Windows resets the position when the menu opens or the monitor changes, so
// the mirrored value is put back whenever that happens.
void Watch(const wux::DependencyObject& element,
           std::initializer_list<wux::DependencyProperty> properties) {
    if (g_watchedElement.get() == element) {
        return;
    }

    g_watchedElement = winrt::make_weak(element);
    for (const auto& property : properties) {
        int64_t token = element.RegisterPropertyChangedCallback(
            property, [](const wux::DependencyObject&,
                         const wux::DependencyProperty&) {
                if (!g_inApply) {
                    Apply();
                }
            });
        g_watchTokens.emplace_back(property, token);
    }
}

void Unwatch() {
    if (auto element = g_watchedElement.get()) {
        for (const auto& [property, token] : g_watchTokens) {
            element.UnregisterPropertyChangedCallback(property, token);
        }
    }
    g_watchTokens.clear();
    g_watchedElement = nullptr;
}

bool IsTaskbarVertical() {
    RECT taskbar;
    return GetTaskbarRect(&taskbar) &&
           taskbar.bottom - taskbar.top > taskbar.right - taskbar.left;
}

// Older Start menu: the menu sits on a canvas at Canvas.Top/Canvas.Left.
void ApplyCanvas(const wux::FrameworkElement& content, bool vertical) {
    auto frame = FindByClass(content, L"StartDocked.StartSizingFrame");
    if (!frame) {
        Wh_Log(L"StartDocked.StartSizingFrame not found");
        return;
    }

    if (vertical) {
        double windowsTop = g_canvasTop.WindowsValue(frame).value_or(0);
        g_canvasTop.Set(frame, content.ActualHeight() - frame.ActualHeight() -
                                   windowsTop);
    } else {
        double windowsLeft = g_canvasLeft.WindowsValue(frame).value_or(0);
        g_canvasLeft.Set(frame, content.ActualWidth() - frame.ActualWidth() -
                                    windowsLeft);
    }

    Watch(frame, {wux::Controls::Canvas::TopProperty(),
                  wux::Controls::Canvas::LeftProperty()});
}

// Redesigned Start menu: the menu is aligned inside its root.
void ApplyAligned(const wux::FrameworkElement& content, bool vertical) {
    auto frameRoot = FindByName(content, L"FrameRoot");
    if (!frameRoot) {
        Wh_Log(L"FrameRoot not found");
        return;
    }

    auto margin = g_margin.WindowsValue(frameRoot).value_or(frameRoot.Margin());
    if (vertical) {
        auto alignment = g_verticalAlignment.WindowsValue(frameRoot).value_or(
            frameRoot.VerticalAlignment());
        if (alignment == wux::VerticalAlignment::Top) {
            alignment = wux::VerticalAlignment::Bottom;
        } else if (alignment == wux::VerticalAlignment::Bottom) {
            alignment = wux::VerticalAlignment::Top;
        }
        std::swap(margin.Top, margin.Bottom);
        g_verticalAlignment.Set(frameRoot, alignment);
    } else {
        auto alignment = g_horizontalAlignment.WindowsValue(frameRoot).value_or(
            frameRoot.HorizontalAlignment());
        if (alignment == wux::HorizontalAlignment::Left) {
            alignment = wux::HorizontalAlignment::Right;
        } else if (alignment == wux::HorizontalAlignment::Right) {
            alignment = wux::HorizontalAlignment::Left;
        }
        std::swap(margin.Left, margin.Right);
        g_horizontalAlignment.Set(frameRoot, alignment);
    }
    g_margin.Set(frameRoot, margin);

    Watch(frameRoot, {wux::FrameworkElement::VerticalAlignmentProperty(),
                      wux::FrameworkElement::HorizontalAlignmentProperty(),
                      wux::FrameworkElement::MarginProperty()});
}

void RestoreAll() {
    g_canvasTop.Restore();
    g_canvasLeft.Restore();
    g_verticalAlignment.Restore();
    g_horizontalAlignment.Restore();
    g_margin.Restore();
}

void Apply() try {
    auto window = wux::Window::Current();
    auto content = window ? window.Content().try_as<wux::FrameworkElement>()
                          : nullptr;
    if (!content) {
        return;
    }

    g_inApply = true;

    // Restore first, so that the Windows values are read fresh and switching
    // the taskbar between vertical and horizontal doesn't mirror twice.
    RestoreAll();

    if (!g_unloading) {
        auto className = winrt::get_class_name(content);
        bool vertical = IsTaskbarVertical();
        Wh_Log(L"Start menu content %s, vertical %d", className.c_str(),
               vertical);
        if (className == L"Windows.UI.Xaml.Controls.Canvas") {
            ApplyCanvas(content, vertical);
        } else {
            ApplyAligned(content, vertical);
        }
    }

    g_inApply = false;
} catch (...) {
    g_inApply = false;
    Wh_Log(L"Error %08X", winrt::to_hresult());
}

void Init() try {
    if (g_visibilityChangedToken) {
        return;
    }

    auto window = wux::Window::Current();
    if (!window) {
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
    Unwatch();
    if (g_visibilityChangedToken) {
        if (auto window = wux::Window::Current()) {
            window.VisibilityChanged(g_visibilityChangedToken);
        }
        g_visibilityChangedToken = {};
    }
    RestoreAll();
} catch (...) {
    Wh_Log(L"Error %08X", winrt::to_hresult());
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
// Entry points.

BOOL Wh_ModInit() {
    Wh_Log(L">");

    g_target = Target::app;
    WCHAR path[MAX_PATH];
    if (GetModuleFileName(nullptr, path, ARRAYSIZE(path))) {
        PCWSTR name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (_wcsicmp(name, L"explorer.exe") == 0) {
            g_target = Target::explorer;
        } else if (_wcsicmp(name, L"StartMenuExperienceHost.exe") == 0) {
            g_target = Target::startMenu;
        } else if (_wcsicmp(name, L"ShellExperienceHost.exe") == 0 ||
                   _wcsicmp(name, L"ShellHost.exe") == 0) {
            g_target = Target::shellFlyouts;
        }
        Wh_Log(L"Process %s, target %d", name, static_cast<int>(g_target));
    }

    if (g_target == Target::startMenu) {
        HMODULE winrt = GetModuleHandle(L"api-ms-win-core-winrt-l1-1-0.dll");
        WindhawkUtils::SetFunctionHook(
            (StartMenu::RoGetActivationFactory_t)GetProcAddress(winrt,
                                                     "RoGetActivationFactory"),
            StartMenu::RoGetActivationFactory_Hook,
            &StartMenu::RoGetActivationFactory_Original);
        return TRUE;
    }

    WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                   &SetWindowPos_Original);
    WindhawkUtils::SetFunctionHook(MoveWindow, MoveWindow_Hook,
                                   &MoveWindow_Original);

    if (g_target != Target::explorer) {
        WindhawkUtils::SetFunctionHook(ShowWindow, ShowWindow_Hook,
                                       &ShowWindow_Original);

        HMODULE dwmapi = LoadLibraryEx(L"dwmapi.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (auto pDwmSetWindowAttribute =
                dwmapi ? (DwmSetWindowAttribute_t)GetProcAddress(
                             dwmapi, "DwmSetWindowAttribute")
                       : nullptr) {
            WindhawkUtils::SetFunctionHook(pDwmSetWindowAttribute,
                                           DwmSetWindowAttribute_Hook,
                                           &DwmSetWindowAttribute_Original);
        }
        return TRUE;
    }

    WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                   &CreateWindowExW_Original);

    HMODULE kernelBase = GetModuleHandle(L"kernelbase.dll");
    WindhawkUtils::SetFunctionHook(
        (RegOpenKeyExW_t)GetProcAddress(kernelBase, "RegOpenKeyExW"),
        RegOpenKeyExW_Hook, &RegOpenKeyExW_Original);
    WindhawkUtils::SetFunctionHook(
        (RegQueryValueExW_t)GetProcAddress(kernelBase, "RegQueryValueExW"),
        RegQueryValueExW_Hook, &RegQueryValueExW_Original);

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    if (g_target == Target::startMenu) {
        if (HWND coreWindow = StartMenu::FindCoreWindow()) {
            RunFromWindowThread(
                coreWindow, [](PVOID) { StartMenu::Init(); }, nullptr);
        }
        return;
    }

    if (g_target == Target::explorer && FindTaskbarXamlHost()) {
        InjectWindhawkTAP();
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");

    g_unloading = true;

    if (g_target == Target::startMenu) {
        if (HWND coreWindow = StartMenu::FindCoreWindow()) {
            RunFromWindowThread(
                coreWindow, [](PVOID) { StartMenu::Uninit(); }, nullptr);
        }
        return;
    }

    if (g_target != Target::explorer) {
        return;
    }

    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->Unadvise();
        g_visualTreeWatcher = nullptr;
    }

    if (HWND host = FindTaskbarXamlHost()) {
        RunFromWindowThread(host, [](PVOID) { RestoreAll(); }, nullptr);
    }
}
