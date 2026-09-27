// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Mirrors the Windows taskbar layout: on a left/right taskbar the Start button goes to the bottom and the clock to the top
// @version         1.1
// @author          namyts
// @include         explorer.exe
// @architecture    x86
// @architecture    x86-64
// @compilerOptions -lgdi32 -lcomctl32 -loleacc -loleaut32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Mirrored Taskbar Layout

Mirrors the order of the Windows taskbar along its length.

* Vertical taskbar (left/right): Start at the bottom, apps above it, clock and
  notification area at the top.
* Horizontal taskbar (top/bottom): Start on the right, clock and notification
  area on the left.

The taskbar window itself stays where Explorer put it. Only the parts inside
it are reordered. App buttons are reversed as well, so the first app stays
next to the Start button.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <commctrl.h>
#include <oleacc.h>
#include <windowsx.h>

#include <atomic>

constexpr UINT_PTR kTimerId = 0x4D52;
constexpr UINT_PTR kSubclassId = 0x4D52;
constexpr DWORD_PTR kPlaceSubclass = 1;
constexpr DWORD_PTR kTaskListSubclass = 2;

constexpr WCHAR kSeenProp[] = L"WhTaskbarMirror.Seen";
constexpr WCHAR kOrigXProp[] = L"WhTaskbarMirror.OrigX";
constexpr WCHAR kOrigYProp[] = L"WhTaskbarMirror.OrigY";

std::atomic<bool> g_enabled;
std::atomic<int> g_buttonExtent;
int g_wasVertical = -1;
bool g_inTaskListPaint = false;
bool g_inThumbnailMove = false;

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;

bool ClassIs(HWND hwnd, const WCHAR* name) {
    WCHAR cls[64];
    return GetClassName(hwnd, cls, ARRAYSIZE(cls)) && _wcsicmp(cls, name) == 0;
}

bool IsTaskbarWnd(HWND hwnd) {
    return ClassIs(hwnd, L"Shell_TrayWnd") ||
           ClassIs(hwnd, L"Shell_SecondaryTrayWnd");
}

RECT GetRectInParent(HWND hwnd, HWND parent) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

bool IsVerticalTaskbar(HWND taskbar) {
    RECT client{};
    GetClientRect(taskbar, &client);
    return client.bottom - client.top > client.right - client.left;
}

// The composition background and the off-screen core window fill the taskbar
// but are not buttons. Moving them makes Explorer undo the whole layout.
bool IsIgnored(HWND hwnd, HWND parent) {
    if (ClassIs(hwnd, L"Windows.UI.Composition.DesktopWindowContentBridge") ||
        ClassIs(hwnd, L"Windows.UI.Core.CoreWindow") ||
        ClassIs(hwnd, L"Windows.UI.Input.InputSite.WindowClass")) {
        return true;
    }

    RECT rect = GetRectInParent(hwnd, parent);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || rect.left < -100 || rect.top < -100) {
        return true;
    }

    RECT client{};
    GetClientRect(parent, &client);
    if (width > client.right * 3 / 2 || height > client.bottom * 3 / 2) {
        return true;
    }
    if (width >= client.right - 2 && height >= client.bottom - 2) {
        return true;
    }
    return false;
}

bool IsTray(HWND hwnd) {
    return ClassIs(hwnd, L"TrayNotifyWnd");
}

bool IsRebar(HWND hwnd) {
    return ClassIs(hwnd, L"ReBarWindow32");
}

bool IsEndDock(HWND hwnd, HWND parent) {
    return !IsIgnored(hwnd, parent) && !IsTray(hwnd) && !IsRebar(hwnd);
}

void RememberOriginal(HWND hwnd, HWND parent) {
    if (GetProp(hwnd, kSeenProp)) {
        return;
    }

    RECT rect = GetRectInParent(hwnd, parent);
    SetProp(hwnd, kSeenProp, (HANDLE)1);
    SetProp(hwnd, kOrigXProp, (HANDLE)(LONG_PTR)rect.left);
    SetProp(hwnd, kOrigYProp, (HANDLE)(LONG_PTR)rect.top);
}

int OrigAxis(HWND hwnd, HWND parent, bool vertical) {
    if (GetProp(hwnd, kSeenProp)) {
        return (int)(LONG_PTR)GetProp(hwnd, vertical ? kOrigYProp : kOrigXProp);
    }

    RECT rect = GetRectInParent(hwnd, parent);
    return vertical ? rect.top : rect.left;
}

int ChildExtent(HWND hwnd, HWND parent, bool vertical) {
    RECT rect = GetRectInParent(hwnd, parent);
    return vertical ? rect.bottom - rect.top : rect.right - rect.left;
}

// Distance from the far end to the far side of this window: its own size plus
// every end-docked sibling that Explorer originally placed before it. The
// window Explorer put nearest the start edge (Start) therefore lands nearest
// the far edge.
int EndDockPrefix(HWND hwnd, HWND parent, bool vertical) {
    int mine = OrigAxis(hwnd, parent, vertical);
    int sum = 0;
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsEndDock(child, parent)) {
            continue;
        }
        int extent = ChildExtent(child, parent, vertical);
        if (extent <= 0) {
            continue;
        }
        int orig = OrigAxis(child, parent, vertical);
        if (orig < mine || child == hwnd) {
            sum += extent;
        }
    }
    return sum;
}

int EndDockTotal(HWND parent, bool vertical) {
    int sum = 0;
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsEndDock(child, parent)) {
            int extent = ChildExtent(child, parent, vertical);
            if (extent > 0) {
                sum += extent;
            }
        }
    }
    return sum;
}

bool TargetPos(HWND hwnd, HWND parent, int cx, int cy, int& x, int& y) {
    RECT client{};
    GetClientRect(parent, &client);
    bool vertical = client.bottom > client.right;
    int limit = vertical ? client.bottom : client.right;
    int extent = vertical ? cy : cx;
    if (extent <= 0 || limit <= 0) {
        return false;
    }

    int pos = 0;
    if (IsTray(hwnd)) {
        pos = 0;
    } else if (IsRebar(hwnd)) {
        pos = limit - EndDockTotal(parent, vertical) - extent;
    } else if (IsEndDock(hwnd, parent)) {
        pos = limit - EndDockPrefix(hwnd, parent, vertical);
    } else {
        return false;
    }
    if (pos < 0) {
        pos = 0;
    }

    if (vertical) {
        y = pos;
    } else {
        x = pos;
    }
    return true;
}

void RewritePlacement(HWND hwnd, WINDOWPOS* wp) {
    HWND parent = GetParent(hwnd);
    if (!parent || !IsTaskbarWnd(parent)) {
        return;
    }

    RECT now = GetRectInParent(hwnd, parent);
    int cx = (wp->flags & SWP_NOSIZE) ? now.right - now.left : wp->cx;
    int cy = (wp->flags & SWP_NOSIZE) ? now.bottom - now.top : wp->cy;
    if ((wp->flags & SWP_NOMOVE) && (wp->flags & SWP_NOSIZE)) {
        return;
    }

    int x = (wp->flags & SWP_NOMOVE) ? now.left : wp->x;
    int y = (wp->flags & SWP_NOMOVE) ? now.top : wp->y;
    if (!TargetPos(hwnd, parent, cx, cy, x, y)) {
        return;
    }

    wp->x = x;
    wp->y = y;
    wp->flags &= ~SWP_NOMOVE;
}

HWND FindTaskList(HWND taskbar) {
    HWND rebar = FindWindowExW(taskbar, nullptr, L"ReBarWindow32", nullptr);
    HWND taskSwitch =
        rebar ? FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr)
              : nullptr;
    return taskSwitch
               ? FindWindowExW(taskSwitch, nullptr, L"MSTaskListWClass", nullptr)
               : nullptr;
}

int MapExtent(int pos, int total) {
    int button = g_buttonExtent.load();
    if (button <= 0) {
        return pos;
    }

    int count = total / button;
    int used = count * button;
    if (count < 2 || pos < 0 || pos >= used) {
        return pos;
    }

    int strip = pos / button;
    int offset = pos % button;
    return (count - 1 - strip) * button + offset;
}

POINT MapClientPoint(HWND taskList, POINT pt) {
    RECT client{};
    GetClientRect(taskList, &client);
    HWND taskbar = GetAncestor(taskList, GA_ROOT);
    if (IsVerticalTaskbar(taskbar)) {
        pt.y = MapExtent(pt.y, client.bottom);
    } else {
        pt.x = MapExtent(pt.x, client.right);
    }
    return pt;
}

void ReverseStrips(HWND taskList) {
    int button = g_buttonExtent.load();
    if (button <= 0) {
        return;
    }

    RECT client{};
    GetClientRect(taskList, &client);
    HWND taskbar = GetAncestor(taskList, GA_ROOT);
    bool vertical = IsVerticalTaskbar(taskbar);
    int total = vertical ? client.bottom : client.right;
    int cross = vertical ? client.right : client.bottom;
    int count = total / button;
    if (count < 2 || cross <= 0) {
        return;
    }

    HDC hdc = GetDC(taskList);
    if (!hdc) {
        return;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, vertical ? cross : button,
                                            vertical ? button : cross);
    HGDIOBJ previous = SelectObject(mem, bitmap);

    for (int i = 0; i < count / 2; i++) {
        int a = i * button;
        int b = (count - 1 - i) * button;
        if (vertical) {
            BitBlt(mem, 0, 0, cross, button, hdc, 0, a, SRCCOPY);
            BitBlt(hdc, 0, a, cross, button, hdc, 0, b, SRCCOPY);
            BitBlt(hdc, 0, b, cross, button, mem, 0, 0, SRCCOPY);
        } else {
            BitBlt(mem, 0, 0, button, cross, hdc, a, 0, SRCCOPY);
            BitBlt(hdc, a, 0, button, cross, hdc, b, 0, SRCCOPY);
            BitBlt(hdc, b, 0, button, cross, mem, 0, 0, SRCCOPY);
        }
    }

    SelectObject(mem, previous);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(taskList, hdc);
}

bool RemapMouse(HWND taskList, UINT msg, LPARAM& lParam) {
    bool screen = false;
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_CONTEXTMENU:
        case WM_NCHITTEST:
            screen = true;
            break;
        default:
            return false;
    }

    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (screen) {
        ScreenToClient(taskList, &pt);
    }
    pt = MapClientPoint(taskList, pt);
    if (screen) {
        ClientToScreen(taskList, &pt);
    }
    lParam = MAKELPARAM((SHORT)pt.x, (SHORT)pt.y);
    return true;
}

LRESULT CALLBACK MirrorSubclassProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    UINT_PTR id,
                                    DWORD_PTR ref) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, MirrorSubclassProc, id);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if (!g_enabled) {
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if (ref == kTaskListSubclass) {
        if (msg == WM_PAINT && !g_inTaskListPaint) {
            RECT client{};
            GetClientRect(hwnd, &client);
            InvalidateRect(hwnd, &client, FALSE);

            g_inTaskListPaint = true;
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            g_inTaskListPaint = false;

            ReverseStrips(hwnd);
            return result;
        }

        RemapMouse(hwnd, msg, lParam);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_WINDOWPOSCHANGING) {
        RewritePlacement(hwnd, reinterpret_cast<WINDOWPOS*>(lParam));
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void PlaceWindow(HWND hwnd, HWND parent) {
    RECT now = GetRectInParent(hwnd, parent);
    int x = now.left;
    int y = now.top;
    int cx = now.right - now.left;
    int cy = now.bottom - now.top;
    if (!TargetPos(hwnd, parent, cx, cy, x, y)) {
        return;
    }
    if (x == now.left && y == now.top) {
        return;
    }

    Wh_Log(L"Move %p -> %d,%d", hwnd, x, y);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int MeasureButtonExtent(HWND taskList, bool vertical) {
    DWORD toolbar = (DWORD)SendMessage(taskList, TB_GETBUTTONSIZE, 0, 0);
    int fromToolbar = vertical ? HIWORD(toolbar) : LOWORD(toolbar);
    if (fromToolbar >= 24 && fromToolbar <= 96) {
        return fromToolbar;
    }

    static const GUID kIID_IAccessible = {
        0x618736e0,
        0x3c3d,
        0x11cf,
        {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    IAccessible* accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(
            taskList, OBJID_CLIENT, kIID_IAccessible,
            reinterpret_cast<void**>(&accessible))) ||
        !accessible) {
        return 0;
    }

    long count = 0;
    accessible->get_accChildCount(&count);
    int extent = 0;
    for (long i = 1; i <= count && i <= 8; i++) {
        VARIANT child;
        VariantInit(&child);
        child.vt = VT_I4;
        child.lVal = i;

        long x = 0;
        long y = 0;
        long width = 0;
        long height = 0;
        if (SUCCEEDED(accessible->accLocation(&x, &y, &width, &height, child))) {
            int candidate = vertical ? height : width;
            if (candidate >= 24 && candidate <= 96) {
                extent = candidate;
                VariantClear(&child);
                break;
            }
        }
        VariantClear(&child);
    }

    accessible->Release();
    return extent;
}

void EnsureTaskbar(HWND taskbar) {
    bool vertical = IsVerticalTaskbar(taskbar);
    if (g_wasVertical != (int)vertical) {
        if (g_wasVertical != -1) {
            for (HWND child = GetWindow(taskbar, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                RemoveProp(child, kSeenProp);
                RemoveProp(child, kOrigXProp);
                RemoveProp(child, kOrigYProp);
            }
        }
        g_wasVertical = (int)vertical;
    }

    for (HWND child = GetWindow(taskbar, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsIgnored(child, taskbar)) {
            continue;
        }
        if (!IsTray(child) && !IsRebar(child) && !IsEndDock(child, taskbar)) {
            continue;
        }

        RememberOriginal(child, taskbar);
        SetWindowSubclass(child, MirrorSubclassProc, kSubclassId, kPlaceSubclass);
    }

    for (HWND child = GetWindow(taskbar, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsIgnored(child, taskbar) &&
            (IsTray(child) || IsRebar(child) || IsEndDock(child, taskbar))) {
            PlaceWindow(child, taskbar);
        }
    }

    HWND taskList = FindTaskList(taskbar);
    if (!taskList) {
        return;
    }

    SetWindowSubclass(taskList, MirrorSubclassProc, kSubclassId,
                      kTaskListSubclass);

    int extent = MeasureButtonExtent(taskList, vertical);
    if (extent > 0 && extent != g_buttonExtent.load()) {
        g_buttonExtent = extent;
        Wh_Log(L"Button extent %d", extent);
        InvalidateRect(taskList, nullptr, TRUE);
    }
}

void RestoreTaskbar(HWND taskbar) {
    KillTimer(taskbar, kTimerId);

    HWND taskList = FindTaskList(taskbar);
    if (taskList) {
        RemoveWindowSubclass(taskList, MirrorSubclassProc, kSubclassId);
    }

    for (HWND child = GetWindow(taskbar, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!GetProp(child, kSeenProp)) {
            continue;
        }

        int x = (int)(LONG_PTR)GetProp(child, kOrigXProp);
        int y = (int)(LONG_PTR)GetProp(child, kOrigYProp);
        RemoveWindowSubclass(child, MirrorSubclassProc, kSubclassId);
        RemoveProp(child, kSeenProp);
        RemoveProp(child, kOrigXProp);
        RemoveProp(child, kOrigYProp);
        SetWindowPos(child, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (taskList) {
        InvalidateRect(taskList, nullptr, TRUE);
    }
}

void CALLBACK EnforceTimerProc(HWND hwnd, UINT, UINT_PTR, DWORD) {
    if (!g_enabled || !IsTaskbarWnd(hwnd)) {
        KillTimer(hwnd, kTimerId);
        return;
    }
    EnsureTaskbar(hwnd);
}

BOOL CALLBACK TaskbarEnumProc(HWND hwnd, LPARAM lParam) {
    DWORD processId = 0;
    bool enable = lParam != 0;
    if (!GetWindowThreadProcessId(hwnd, &processId) ||
        processId != GetCurrentProcessId() || !IsTaskbarWnd(hwnd)) {
        return TRUE;
    }

    if (enable) {
        SetTimer(hwnd, kTimerId, 400, EnforceTimerProc);
        EnsureTaskbar(hwnd);
    } else {
        RestoreTaskbar(hwnd);
    }
    return TRUE;
}

void ApplyAll(bool enable) {
    EnumWindows(TaskbarEnumProc, enable ? 1 : 0);
}

BOOL WINAPI SetWindowPos_Hook(HWND hwnd,
                              HWND insertAfter,
                              int x,
                              int y,
                              int cx,
                              int cy,
                              UINT flags) {
    if (g_enabled && !g_inThumbnailMove && !(flags & SWP_NOMOVE) &&
        ClassIs(hwnd, L"TaskListThumbnailWnd") && g_buttonExtent.load() > 0) {
        HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        HWND taskList = taskbar ? FindTaskList(taskbar) : nullptr;
        if (taskList) {
            POINT anchor{x + cx / 2, y + cy / 2};
            POINT mapped = anchor;
            ScreenToClient(taskList, &mapped);
            mapped = MapClientPoint(taskList, mapped);
            ClientToScreen(taskList, &mapped);
            x += mapped.x - anchor.x;
            y += mapped.y - anchor.y;
        }
    }

    g_inThumbnailMove = true;
    BOOL result =
        SetWindowPos_Original(hwnd, insertAfter, x, y, cx, cy, flags);
    g_inThumbnailMove = false;
    return result;
}

using RunFromWindowThreadProc_t = void (*)(PVOID);

struct RunFromWindowThreadParam {
    RunFromWindowThreadProc_t proc;
    PVOID procParam;
};

UINT g_runFromWindowThreadMsg;

LRESULT CALLBACK RunFromWindowThreadHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* msg = reinterpret_cast<const CWPSTRUCT*>(lParam);
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

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, RunFromWindowThreadHook,
                                   nullptr, threadId);
    if (!hook) {
        return false;
    }

    RunFromWindowThreadParam data{proc, param};
    SendMessage(hwnd, g_runFromWindowThreadMsg, 0, (LPARAM)&data);
    UnhookWindowsHookEx(hook);
    return true;
}

void SetEnabled(bool enable) {
    g_enabled = enable;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (!taskbar || !GetWindowThreadProcessId(taskbar, &processId) ||
        processId != GetCurrentProcessId()) {
        return;
    }

    auto proc = [](PVOID p) { ApplyAll(*static_cast<bool*>(p)); };
    if (!RunFromWindowThread(taskbar, proc, &enable)) {
        ApplyAll(enable);
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    if (!WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                        &SetWindowPos_Original)) {
        Wh_Log(L"SetWindowPos hook failed");
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    SetEnabled(true);
}

void Wh_ModUninit() {
    Wh_Log(L">");
    SetEnabled(false);
}
