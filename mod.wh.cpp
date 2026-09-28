// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Reverses the taskbar order: on a left/right taskbar Start goes to the bottom, apps stack upwards from it, and the clock goes to the top
// @version         2.0
// @author          namyts
// @include         explorer.exe
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
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <xamlom.h>

#include <atomic>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
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
// everything in it. Each button and the tray as a whole get the same -1 scale
// again, so their contents end up the right way round. Render transforms are
// honored by hit testing, so clicks follow what's on screen.

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
            Flip(trayFrame, rootIndex);
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

BOOL Wh_ModInit() {
    Wh_Log(L">");

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

    if (FindTaskbarXamlHost()) {
        InjectWindhawkTAP();
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->Unadvise();
        g_visualTreeWatcher = nullptr;
    }

    if (HWND host = FindTaskbarXamlHost()) {
        RunFromWindowThread(host, [](PVOID) { RestoreAll(); }, nullptr);
    }
}
