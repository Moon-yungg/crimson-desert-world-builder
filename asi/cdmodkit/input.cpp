// Adapted from master-looter (MIT, Copyright (c) 2026 Seth) / Trinity (MIT, XeTrinityz).
// The game clips and hides the OS cursor and recentres it every frame, so the menu uses a virtual
// cursor driven by the raw mouse deltas (WM_INPUT) the game already receives.
#include "input.h"
#include "core.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imm.h>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace input {
    static WNDPROC g_original = nullptr;
    static HWND    g_hwnd = nullptr;
    static CRITICAL_SECTION g_cs; static bool g_csReady = false;
    static float g_vx = 0, g_vy = 0;
    static float g_pendingDx = 0, g_pendingDy = 0;
    static bool  g_rawButtons = false;
    static int   g_pendingButtons[5][2];
    static float g_pendingWheel = 0;
    static bool  g_weRegistered = false;
    typedef BOOL (WINAPI* FnGetCursorPos)(LPPOINT);
    static FnGetCursorPos oGetCursorPos = nullptr;
    static DWORD g_renderTid = 0;
    // free-fly camera: its movement keys and the mouse (all of it with the menu closed, while the right button is held with the
    // menu open) go to World Builder instead of the game; the mouse deltas are collected for the camera's turn
    static volatile bool g_freeCam = false; static volatile bool g_rmb = false;
    static float g_lookDx = 0, g_lookDy = 0;
    static std::string g_imeComposition;
    static bool FreeCamLookingNow() { return g_freeCam && (!core::g_menuOpen || (g_rmb && !core::g_uiMouseOverUi)); }

    static void Lock() { EnterCriticalSection(&g_cs); }
    static void Unlock() { LeaveCriticalSection(&g_cs); }
    static void ClientSize(int* w, int* h) {
        RECT rc = {};
        if (g_hwnd && GetClientRect(g_hwnd, &rc)) { *w = rc.right - rc.left; *h = rc.bottom - rc.top; }
        else { *w = 1920; *h = 1080; }
    }

    static void OnRawInput(HRAWINPUT h) {
        UINT size = 0;
        if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0 || size > 1024) return;
        alignas(8) unsigned char buf[1024];
        if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return;
        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType != RIM_TYPEMOUSE) return;
        const RAWMOUSE& m = ri->data.mouse;
        int w, hgt; ClientSize(&w, &hgt);
        Lock();
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
            const bool virt = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int sw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int sh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            POINT p = { static_cast<LONG>(m.lLastX * sw / 65535.0), static_cast<LONG>(m.lLastY * sh / 65535.0) };
            ScreenToClient(g_hwnd, &p);
            g_vx = static_cast<float>(p.x); g_vy = static_cast<float>(p.y);
        } else if (FreeCamLookingNow()) {
            g_lookDx += static_cast<float>(m.lLastX);
            g_lookDy += static_cast<float>(m.lLastY);
            g_pendingDx += static_cast<float>(m.lLastX);   // the editor tells a right click from a right drag by this motion
            g_pendingDy += static_cast<float>(m.lLastY);
        } else {
            g_vx += static_cast<float>(m.lLastX);
            g_vy += static_cast<float>(m.lLastY);
            g_pendingDx += static_cast<float>(m.lLastX);
            g_pendingDy += static_cast<float>(m.lLastY);
        }
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) g_rmb = true;
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) g_rmb = false;
        if (g_vx < 0) g_vx = 0;
        if (g_vy < 0) g_vy = 0;
        if (g_vx > w - 1) g_vx = static_cast<float>(w - 1);
        if (g_vy > hgt - 1) g_vy = static_cast<float>(hgt - 1);
        if (g_rawButtons) {
            static const USHORT down[5] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
            static const USHORT up[5]   = { RI_MOUSE_LEFT_BUTTON_UP,   RI_MOUSE_RIGHT_BUTTON_UP,   RI_MOUSE_MIDDLE_BUTTON_UP,   RI_MOUSE_BUTTON_4_UP,   RI_MOUSE_BUTTON_5_UP };
            for (int b = 0; b < 5; ++b) {
                if (m.usButtonFlags & down[b]) ++g_pendingButtons[b][0];
                if (m.usButtonFlags & up[b])   ++g_pendingButtons[b][1];
            }
            if (m.usButtonFlags & RI_MOUSE_WHEEL) g_pendingWheel += static_cast<short>(m.usButtonData) / static_cast<float>(WHEEL_DELTA);
        }
        Unlock();
    }

    void FeedMouse(ImGuiIO& io) {
        g_renderTid = GetCurrentThreadId();
        Lock();
        io.AddMousePosEvent(g_vx, g_vy);
        const bool toUi = core::g_uiWantsMouse;
        for (int b = 0; b < 5; ++b) {
            if (toUi) {
                for (int i = 0; i < g_pendingButtons[b][0]; ++i) io.AddMouseButtonEvent(b, true);
                for (int i = 0; i < g_pendingButtons[b][1]; ++i) io.AddMouseButtonEvent(b, false);
            }
            g_pendingButtons[b][0] = g_pendingButtons[b][1] = 0;
        }
        if (g_pendingWheel != 0) { if (toUi) io.AddMouseWheelEvent(0, g_pendingWheel); g_pendingWheel = 0; }
        Unlock();
    }

    void TakeMouseDelta(float* dx, float* dy) {
        Lock();
        if (dx) *dx = g_pendingDx;
        if (dy) *dy = g_pendingDy;
        g_pendingDx = g_pendingDy = 0;
        Unlock();
    }

    // ImGui's Win32 backend polls GetCursorPos every frame; on the render thread while the menu is open it gets the virtual cursor.
    static BOOL WINAPI hkGetCursorPos(LPPOINT p) {
        if (p && core::g_menuOpen && g_renderTid && GetCurrentThreadId() == g_renderTid && g_hwnd) {
            Lock(); POINT c = { static_cast<LONG>(g_vx), static_cast<LONG>(g_vy) }; Unlock();
            ClientToScreen(g_hwnd, &c); *p = c; return TRUE;
        }
        return oGetCursorPos(p);
    }

    static void EnsureRawInput() {
        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> devs(n);
        if (n) GetRegisteredRawInputDevices(devs.data(), &n, sizeof(RAWINPUTDEVICE));
        for (UINT i = 0; i < n; ++i) {
            if (devs[i].usUsagePage == 0x01 && devs[i].usUsage == 0x02) {
                g_rawButtons = (devs[i].dwFlags & RIDEV_NOLEGACY) != 0;
                core::Log("[input] game registered raw mouse (flags 0x%X)%s", devs[i].dwFlags, g_rawButtons ? ", no legacy buttons" : "");
                return;
            }
        }
        RAWINPUTDEVICE rid = { 0x01, 0x02, 0, g_hwnd };
        g_weRegistered = RegisterRawInputDevices(&rid, 1, sizeof rid) != 0;
        core::Log("[input] raw mouse %s", g_weRegistered ? "registered by us" : "registration FAILED");
    }

    void MenuOpened() {
        int w, h; ClientSize(&w, &h);
        Lock(); g_vx = w * 0.5f; g_vy = h * 0.5f; g_pendingDx = g_pendingDy = 0; for (auto& b : g_pendingButtons) b[0] = b[1] = 0; g_pendingWheel = 0; Unlock();
        if (ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            for (int b = 0; b < 5; ++b) if (io.MouseDown[b]) io.AddMouseButtonEvent(b, false);
        }
    }
    void MenuClosed() {
        if (ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            for (int b = 0; b < 5; ++b) if (io.MouseDown[b]) io.AddMouseButtonEvent(b, false);
            io.ClearInputKeys();
        }
        ClearKeys();
        Lock(); g_pendingDx = g_pendingDy = 0; for (auto& b : g_pendingButtons) b[0] = b[1] = 0; g_pendingWheel = 0; Unlock();
    }

    // scan code key state for the free camera: set on key-down, cleared on key-up/focus loss and on mode transitions.
    // No time-out: Windows auto-repeats only the last pressed key, so a held key may stay silent.
    static bool g_scanDown[512] = { false };
    static void TrackKey(UINT msg, LPARAM lParam) {
        const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) g_scanDown[scan | (ext << 8)] = true;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { g_scanDown[scan] = false; g_scanDown[scan | 256] = false; }
    }
    bool ScanDown(int scan, bool ext) { return g_scanDown[(scan & 0xFF) | (ext ? 256 : 0)]; }
    void ClearKeys() { memset(g_scanDown, 0, sizeof g_scanDown); }
    static bool IsFreeCamScan(int scan) { return scan == 0x11 || scan == 0x1E || scan == 0x1F || scan == 0x20 || scan == 0x10 || scan == 0x12 || scan == 0x2A || scan == 0x1D || scan == 0x39; }   // W A S D Q E Shift Ctrl Space
    void SetFreeCam(bool on) { g_freeCam = on; Lock(); g_lookDx = g_lookDy = 0; Unlock(); }
    bool FreeCamLooking() { return FreeCamLookingNow(); }
    void TakeLookDelta(float* dx, float* dy) { Lock(); *dx = g_lookDx; *dy = g_lookDy; g_lookDx = g_lookDy = 0; Unlock(); }
    // 1 = a mouse message the free camera takes, 2 = one of its keys (the game may read the keyboard as raw input too),
    // 0 = leave it to the normal routing. Raw key state also feeds the scan-code table, for games that turn legacy key messages off.
    static int FreeCamRaw(HRAWINPUT h) {
        UINT size = 0;
        if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0 || size > 1024) return 0;
        alignas(8) unsigned char buf[1024];
        if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return 0;
        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& k = ri->data.keyboard; const int scan = k.MakeCode & 0xFF, ext = (k.Flags & RI_KEY_E0) ? 1 : 0;
            if (!IsFreeCamScan(scan) || core::g_uiTextInput) return 0;
            g_scanDown[scan | (ext << 8)] = !(k.Flags & RI_KEY_BREAK);
            return (k.Flags & RI_KEY_BREAK) ? 0 : 2;   // releases still reach the game: a key held when flying starts must not stay down there
        }
        if (ri->header.dwType != RIM_TYPEMOUSE) return 0;
        OnRawInput(h);
        static DWORD s_t0 = 0; static int s_msgs = 0; s_msgs++;   // diagnosis: does the mouse reach the free camera at all
        if (GetTickCount() - s_t0 > 3000) { Lock(); const float lx = g_lookDx, ly = g_lookDy; Unlock(); core::Log("[freecam] %d mouse messages in 3 s, menu %d, right button %d, over ui %d, pending look %.0f %.0f", s_msgs, (int)core::g_menuOpen, (int)g_rmb, (int)core::g_uiMouseOverUi, lx, ly); s_msgs = 0; s_t0 = GetTickCount(); }
        static const USHORT ups = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        if (ri->data.mouse.usButtonFlags & ups) return 0;   // button releases still reach the game (see keys)
        return 1;   // the game's own camera must not turn while flying: its view decides what gets culled
    }
    static bool IsMouse(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
    static bool IsKeyboard(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR; }
    static bool IsIme(UINT m) {
        return m == WM_IME_STARTCOMPOSITION || m == WM_IME_ENDCOMPOSITION || m == WM_IME_COMPOSITION || m == WM_IME_CHAR
            || m == WM_IME_NOTIFY || m == WM_IME_SETCONTEXT || m == WM_IME_REQUEST || m == WM_IME_CONTROL || m == WM_IME_SELECT
            || m == WM_IME_KEYDOWN || m == WM_IME_KEYUP;
    }
    static std::string WideToUtf8(const wchar_t* s, int n) {
        if (!s || n <= 0) return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr); if (bytes <= 0) return {};
        std::string out((size_t)bytes, '\0'); WideCharToMultiByte(CP_UTF8, 0, s, n, out.data(), bytes, nullptr, nullptr); return out;
    }
    static void TrackImeComposition(HWND hwnd, UINT msg, LPARAM lParam) {
        if (msg == WM_IME_STARTCOMPOSITION) { Lock(); g_imeComposition.clear(); Unlock(); return; }
        if (msg == WM_IME_ENDCOMPOSITION) { Lock(); g_imeComposition.clear(); Unlock(); return; }
        if (msg != WM_IME_COMPOSITION) return;
        std::string next;
        if (!(lParam & GCS_RESULTSTR) && (lParam & GCS_COMPSTR)) {
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                if (bytes > 0 && bytes < 64 * 1024) { std::vector<wchar_t> w((size_t)bytes / sizeof(wchar_t)); if (ImmGetCompositionStringW(himc, GCS_COMPSTR, w.data(), (DWORD)bytes) == bytes) next = WideToUtf8(w.data(), (int)w.size()); }
                ImmReleaseContext(hwnd, himc);
            }
        }
        Lock(); g_imeComposition = std::move(next); Unlock();
    }
    std::string ImeComposition() { Lock(); std::string s = g_imeComposition; Unlock(); return s; }
    // While the menu is open the game keeps running and stays controllable: the mouse belongs to the menu only while the
    // cursor is over a World Builder window (core::g_uiWantsMouse), the keyboard only while a text field is active (g_uiWantsKeyboard).
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (IsKeyboard(msg)) TrackKey(msg, lParam);
        if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) { memset(g_scanDown, 0, sizeof g_scanDown); g_rmb = false; }
        // Keep the platform backend's keyboard code page in sync even when the game's WndProc would otherwise consume the change.
        if (core::g_menuOpen && msg == WM_INPUTLANGCHANGE) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (g_freeCam) {
            if (msg == WM_INPUT) {
                const int take = FreeCamRaw(reinterpret_cast<HRAWINPUT>(lParam));
                if (take) return DefWindowProcW(hwnd, msg, wParam, lParam);   // the game neither turns nor walks
                if (core::g_menuOpen) { if (core::g_uiWantsMouse) return DefWindowProcW(hwnd, msg, wParam, lParam); }
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR) && !core::g_uiTextInput && IsFreeCamScan((int)((lParam >> 16) & 0xFF))) {
                // not to the game; the editor still sees Ctrl / Shift (Ctrl-click, shortcuts). Key-ups pass below: the game must see
                // a key released that it saw pressed
                if (core::g_menuOpen && msg != WM_CHAR) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                return 0;
            }
            if (IsMouse(msg) && (!core::g_menuOpen || ((g_rmb || msg == WM_RBUTTONDOWN) && !core::g_uiMouseOverUi))) {
                if (msg == WM_RBUTTONDOWN) g_rmb = true; else if (msg == WM_RBUTTONUP) g_rmb = false;
                if (core::g_menuOpen && !g_rawButtons) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);   // right click / drag in the world: the editor's context menu
                if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP) return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;   // no attack / aim while flying
            }
        }
        if (core::g_menuOpen) {
            const bool mouseToUi = core::g_uiWantsMouse, keysToUi = core::g_uiWantsKeyboard;
            if (core::g_uiTextInput && IsIme(msg)) {
                TrackImeComposition(hwnd, msg, lParam);
                // The game owns the real HWND and may consume IME composition messages. Give them to DefWindowProc instead;
                // it drives the native composition/candidate window and emits WM_CHAR for committed UTF-16 text, which the
                // normal ImGui Win32 path below consumes. ImGui's default Win32 IME callback positions the candidate window.
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            if (msg == WM_INPUT) {
                OnRawInput(reinterpret_cast<HRAWINPUT>(lParam));        // the virtual cursor always follows the mouse
                if (mouseToUi) return DefWindowProcW(hwnd, msg, wParam, lParam);   // the game does not look around while we use the menu
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if (msg == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
            if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE) return mouseToUi ? 0 : CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            if (IsMouse(msg)) {
                if (mouseToUi) { if (!g_rawButtons) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); return 0; }
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if (IsKeyboard(msg)) {
                if (keysToUi) {
                    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                    return 0;
                }
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
        } else if (msg == WM_INPUT && g_weRegistered) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
    }

    void Init(HWND hwnd) {
        if (g_original) return;
        if (!g_csReady) { InitializeCriticalSection(&g_cs); g_csReady = true; }
        g_hwnd = hwnd;
        g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
        EnsureRawInput();
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
            if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&hkGetCursorPos), reinterpret_cast<void**>(&oGetCursorPos)) != MH_OK || MH_EnableHook(target) != MH_OK)
                core::Log("[input] could not hook GetCursorPos; the menu cursor may jump");
        }
        core::Log("[input] window %p subclassed", (void*)hwnd);
    }

    void Shutdown() {
        if (g_original && g_hwnd) { SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original)); g_original = nullptr; g_hwnd = nullptr; }
    }
}
