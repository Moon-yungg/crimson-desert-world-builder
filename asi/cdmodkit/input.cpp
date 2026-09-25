// Adapted from master-looter (MIT, Copyright (c) 2026 Seth) / Trinity (MIT, XeTrinityz).
// The game clips and hides the OS cursor and recentres it every frame, so the menu uses a virtual
// cursor driven by the raw mouse deltas (WM_INPUT) the game already receives.
#include "input.h"
#include "core.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
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

    // scan code key state: set on key-down, cleared on key-up (both extended variants: Shift+Numpad flips the flag), on focus loss
    // and when the placement mode starts/ends. No time-out: Windows auto-repeats only the last pressed key, so a held key may stay silent.
    static bool g_scanDown[512] = { false };
    static void TrackKey(UINT msg, LPARAM lParam) {
        const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) g_scanDown[scan | (ext << 8)] = true;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            // Shift + numpad: Windows inserts a fake extended Shift-up (E0 2A) around the key; only that variant is cleared for
            // the shift keys so the real Shift stays 'down'. Other keys clear both variants (the up event may arrive in the other form).
            if ((scan == 0x2A || scan == 0x36) && ext) g_scanDown[scan | 256] = false;
            else { g_scanDown[scan] = false; g_scanDown[scan | 256] = false; }
        }
    }
    bool ScanDown(int scan, bool ext) { return g_scanDown[(scan & 0xFF) | (ext ? 256 : 0)]; }
    void ClearKeys() { memset(g_scanDown, 0, sizeof g_scanDown); }
    // Numpad keys share their scan code with the navigation keys (Home = E0 47 = Numpad 7 etc.). The extended variant only counts
    // while Shift is held, because Shift + numpad arrives as the extended code; a plain Home / End / PgUp / Insert / Delete is ignored.
    // Numpad Enter (E0 1C) and Numpad / (E0 35) are always extended and always accepted.
    bool ScanDownAny(int scan) {
        if (scan == 0x1C || scan == 0x35) return ScanDown(scan, false) || ScanDown(scan, true);
        const bool shift = ScanDown(0x2A, false) || ScanDown(0x36, false);
        return ScanDown(scan, false) || (shift && ScanDown(scan, true));
    }
    bool VkDown(int vk) {
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return ScanDown(0x2A, false) || ScanDown(0x36, false);
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return ScanDown(0x1D, false) || ScanDown(0x1D, true);
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return ScanDown(0x38, false) || ScanDown(0x38, true);
        const int scan = (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC); if (!scan) return false;
        const bool numpad = (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_DECIMAL || vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_MULTIPLY;
        const bool ext = vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE;
        if (vk == VK_RETURN) return ScanDown(scan, false) || ScanDown(scan, true);   // main Enter or numpad Enter
        if (numpad) return ScanDownAny(scan);                                         // Shift + numpad arrives as the extended code
        return ScanDown(scan, ext);
    }
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
    static std::vector<int> g_placeVks;
    void SetPlaceVks(const int* vks, int count) { g_placeVks.assign(vks, vks + count); }
    static bool IsMouse(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
    static bool IsPlaceKeyFixed(WPARAM vk) { return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_RETURN || vk == VK_BACK || vk == VK_DECIMAL || vk == VK_CLEAR || vk == VK_MULTIPLY || vk == VK_DIVIDE || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9); }   // Shift stays with the game (sprint)
    // keys that belong to World Builder while carrying an object: the bound placement keys (modifiers stay with the game, Shift = sprint)
    // plus the navigation variants of bound numpad keys (NumLock off / Shift turns Numpad 8 into Up, Numpad . into Delete, ...)
    static bool IsPlaceKey(WPARAM vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL) return false;
        if (g_placeVks.empty()) return IsPlaceKeyFixed(vk);
        for (int k : g_placeVks) if ((WPARAM)k == vk) return true;
        static const int nav[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_INSERT, VK_DELETE, VK_CLEAR };
        static const int np[]  = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD1, VK_NUMPAD0, VK_DECIMAL, VK_NUMPAD5 };
        for (int j = 0; j < 11; j++) if (vk == (WPARAM)nav[j]) for (int k : g_placeVks) if (k == np[j]) return true;
        return false;
    }
    static bool IsKeyboard(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR; }
    static bool IsIme(UINT m) {
        return m == WM_IME_STARTCOMPOSITION || m == WM_IME_ENDCOMPOSITION || m == WM_IME_COMPOSITION || m == WM_IME_CHAR
            || m == WM_IME_NOTIFY || m == WM_IME_SETCONTEXT || m == WM_IME_REQUEST || m == WM_IME_CONTROL || m == WM_IME_SELECT
            || m == WM_IME_KEYDOWN || m == WM_IME_KEYUP;
    }
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
                if (!keysToUi && core::g_placing && IsPlaceKey(wParam)) return 0;   // placement keys belong to World Builder while carrying an object
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
