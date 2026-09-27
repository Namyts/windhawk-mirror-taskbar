// ==WindhawkMod==
// @id              taskbar-mirrored-layout
// @name            Mirrored Taskbar Layout
// @description     Mirrors the Windows 10 taskbar layout: on a left/right taskbar the Start button goes to the bottom and the clock to the top
// @version         1.0
// @author          you
// @include         explorer.exe
// @architecture    x86
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Mirrored Taskbar Layout

Mirrors the order of the Windows 10 taskbar along its length.

* Vertical taskbar (left/right): Start at the bottom, clock and notification
  area at the top.
* Horizontal taskbar (top/bottom): Start on the right, clock and notification
  area on the left.

Only the positions of the taskbar's parts are changed, their contents are drawn
as usual.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <atomic>

std::atomic<bool> g_enabled;

bool IsTaskbarWnd(HWND hWnd) {
    WCHAR className[32];
    return GetClassName(hWnd, className, ARRAYSIZE(className)) &&
           (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0);
}

bool IsTrayNotifyWnd(HWND hWnd) {
    WCHAR className[32];
    return GetClassName(hWnd, className, ARRAYSIZE(className)) &&
           _wcsicmp(className, L"TrayNotifyWnd") == 0 &&
           IsTaskbarWnd(GetAncestor(hWnd, GA_PARENT));
}

// The parent within which the window's position is mirrored, or null if the
// window isn't one of the mirrored taskbar parts.
HWND GetMirrorParent(HWND hWnd) {
    HWND parent = GetAncestor(hWnd, GA_PARENT);
    if (parent && (IsTaskbarWnd(parent) || IsTrayNotifyWnd(parent))) {
        return parent;
    }

    return nullptr;
}

// Mirrors a rect given in the parent's client coordinates along the taskbar's
// long axis. Applying it twice gives back the original rect.
void MirrorRect(HWND parent, int& x, int& y, int cx, int cy) {
    HWND taskbar = IsTaskbarWnd(parent) ? parent : GetAncestor(parent, GA_PARENT);

    RECT taskbarRect;
    RECT parentRect;
    if (!GetWindowRect(taskbar, &taskbarRect) ||
        !GetClientRect(parent, &parentRect)) {
        return;
    }

    if (taskbarRect.bottom - taskbarRect.top >
        taskbarRect.right - taskbarRect.left) {
        y = parentRect.bottom - y - cy;
    } else {
        x = parentRect.right - x - cx;
    }
}

RECT GetRectInParent(HWND hWnd, HWND parent) {
    RECT rect{};
    GetWindowRect(hWnd, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void MirrorWindowPos(HWND hWnd, int& x, int& y, int& cx, int& cy, UINT& flags) {
    if (!g_enabled || ((flags & SWP_NOMOVE) && (flags & SWP_NOSIZE))) {
        return;
    }

    HWND parent = GetMirrorParent(hWnd);
    if (!parent) {
        return;
    }

    // A mirrored position depends on the size, so a resize has to move the
    // window too, and a move needs the current size.
    if (flags & (SWP_NOMOVE | SWP_NOSIZE)) {
        RECT rect = GetRectInParent(hWnd, parent);
        int currentCx = rect.right - rect.left;
        int currentCy = rect.bottom - rect.top;

        if (flags & SWP_NOMOVE) {
            x = rect.left;
            y = rect.top;
            MirrorRect(parent, x, y, currentCx, currentCy);
            flags &= ~SWP_NOMOVE;
        }

        if (flags & SWP_NOSIZE) {
            cx = currentCx;
            cy = currentCy;
        }
    }

    MirrorRect(parent, x, y, cx, cy);
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;
BOOL WINAPI SetWindowPos_Hook(HWND hWnd,
                              HWND hWndInsertAfter,
                              int X,
                              int Y,
                              int cx,
                              int cy,
                              UINT uFlags) {
    MirrorWindowPos(hWnd, X, Y, cx, cy, uFlags);
    return SetWindowPos_Original(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

using DeferWindowPos_t = decltype(&DeferWindowPos);
DeferWindowPos_t DeferWindowPos_Original;
HDWP WINAPI DeferWindowPos_Hook(HDWP hWinPosInfo,
                                HWND hWnd,
                                HWND hWndInsertAfter,
                                int x,
                                int y,
                                int cx,
                                int cy,
                                UINT uFlags) {
    MirrorWindowPos(hWnd, x, y, cx, cy, uFlags);
    return DeferWindowPos_Original(hWinPosInfo, hWnd, hWndInsertAfter, x, y,
                                   cx, cy, uFlags);
}

using MoveWindow_t = decltype(&MoveWindow);
MoveWindow_t MoveWindow_Original;
BOOL WINAPI MoveWindow_Hook(HWND hWnd,
                            int X,
                            int Y,
                            int nWidth,
                            int nHeight,
                            BOOL bRepaint) {
    UINT flags = 0;
    MirrorWindowPos(hWnd, X, Y, nWidth, nHeight, flags);
    return MoveWindow_Original(hWnd, X, Y, nWidth, nHeight, bRepaint);
}

// Flips the parts which are already laid out, used when the mod is turned on
// and off. Since mirroring twice is a no-op, the same pass does both.
void MirrorChildren(HWND parent) {
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect = GetRectInParent(child, parent);
        int x = rect.left;
        int y = rect.top;
        MirrorRect(parent, x, y, rect.right - rect.left, rect.bottom - rect.top);
        SetWindowPos_Original(child, nullptr, x, y, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        if (IsTrayNotifyWnd(child)) {
            MirrorChildren(child);
        }
    }
}

BOOL CALLBACK MirrorAllTaskbarsEnum(HWND hWnd, LPARAM) {
    DWORD processId;
    if (GetWindowThreadProcessId(hWnd, &processId) &&
        processId == GetCurrentProcessId() && IsTaskbarWnd(hWnd)) {
        MirrorChildren(hWnd);
    }
    return TRUE;
}

void MirrorAllTaskbars() {
    EnumWindows(MirrorAllTaskbarsEnum, 0);
}

BOOL CALLBACK FindTaskbarEnum(HWND hWnd, LPARAM lParam) {
    DWORD processId;
    WCHAR className[32];
    if (GetWindowThreadProcessId(hWnd, &processId) &&
        processId == GetCurrentProcessId() &&
        GetClassName(hWnd, className, ARRAYSIZE(className)) &&
        _wcsicmp(className, L"Shell_TrayWnd") == 0) {
        *reinterpret_cast<HWND*>(lParam) = hWnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;
    EnumWindows(FindTaskbarEnum, reinterpret_cast<LPARAM>(&hTaskbarWnd));
    return hTaskbarWnd;
}

using RunFromWindowThreadProc_t = void (*)(PVOID parameter);

struct RunFromWindowThreadParam {
    RunFromWindowThreadProc_t proc;
    PVOID procParam;
};

UINT g_runFromWindowThreadMsg;

LRESULT CALLBACK RunFromWindowThreadHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
        if (cwp->message == g_runFromWindowThreadMsg) {
            auto* param = (RunFromWindowThreadParam*)cwp->lParam;
            param->proc(param->procParam);
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         PVOID procParam) {
    if (!g_runFromWindowThreadMsg) {
        g_runFromWindowThreadMsg =
            RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    }

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(WH_CALLWNDPROC, RunFromWindowThreadHook,
                                  nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RunFromWindowThreadParam param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, g_runFromWindowThreadMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

// Runs on the taskbar thread, so that the taskbar can't lay itself out halfway
// through the switch.
void SetEnabled(bool enabled) {
    struct PARAM {
        bool enabled;
    } param{enabled};

    auto proc = [](PVOID p) {
        g_enabled = static_cast<PARAM*>(p)->enabled;
        MirrorAllTaskbars();
    };

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd || !RunFromWindowThread(hTaskbarWnd, proc, &param)) {
        g_enabled = enabled;
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                   &SetWindowPos_Original);
    WindhawkUtils::SetFunctionHook(DeferWindowPos, DeferWindowPos_Hook,
                                   &DeferWindowPos_Original);
    WindhawkUtils::SetFunctionHook(MoveWindow, MoveWindow_Hook,
                                   &MoveWindow_Original);

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
