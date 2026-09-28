// Adapted from master-looter (MIT, Copyright (c) 2026 Seth) / Trinity (MIT, XeTrinityz).
// The game clips and hides the OS cursor and recentres it every frame, so the menu uses a virtual
// cursor driven by the raw mouse deltas (WM_INPUT) the game already receives.
// Configured hotkeys and delivered-input ownership share the router lock. Optional placement keys retain
// the v0.97 policy; their scan-state recipient joins the same release ledger as the game and ImGui.
#include "input.h"
#include "core.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imm.h>
#include <string>
#include <vector>
#include <atomic>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace input {
    static WNDPROC g_original = nullptr;
    static HWND    g_hwnd = nullptr;
    static CRITICAL_SECTION g_cs; static bool g_csReady = false;
    // Created at load time so the admission scopes are valid before Init (a late frame or an editor callback can
    // construct one while the module is still starting); entering a CRITICAL_SECTION before it exists is undefined.
    static struct CriticalSectionInit { CriticalSectionInit() { InitializeCriticalSection(&g_cs); g_csReady = true; } } g_csInit;
    static float g_vx = 0, g_vy = 0;
    static float g_pendingDx = 0, g_pendingDy = 0;
    static bool  g_rawButtons = false;
    static int   g_pendingButtons[5][2];
    // Raw-button ownership across frames. A raw down submitted to the UI makes the backend the holder;
    // a later raw up must repair that holder even when the CURRENT policy no longer routes to the UI.
    // Guarded by g_cs with the pending counters; cleared by MenuClosed alongside the backend state.
    static bool  g_rawUiHeld[5] = { false };
    static unsigned long long g_rawUiEpoch[5] = { 0 };
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
    // Optional admission: a CRITICAL_SECTION is re-entrant for the thread that owns it, so the owning thread
    // always admits (Windows tracks the recursion count) while another owner fails the try instead of blocking.
    FrameScope::FrameScope() : owns(true) { Lock(); }
    FrameScope::FrameScope(std::try_to_lock_t) : owns(TryEnterCriticalSection(&g_cs) != 0) {}
    FrameScope::~FrameScope() { if (owns) Unlock(); }   // releases only its own acquisition, exactly once
    void FrameScope::Release() noexcept { if (owns) { owns = false; Unlock(); } }
    // Independent admission counters: atomics, read only by diagnostics/evidence (never by a routing decision).
    static std::atomic<unsigned long long> g_cursorCalls{0}, g_cursorAdmitted{0}, g_cursorBypassed{0}, g_hotkeyConsumed{0};
    InputAdmission AdmissionStats() {
        InputAdmission a;
        a.cursorCalls = g_cursorCalls.load(std::memory_order_relaxed);
        a.cursorAdmitted = g_cursorAdmitted.load(std::memory_order_relaxed);
        a.cursorBypassed = g_cursorBypassed.load(std::memory_order_relaxed);
        a.hotkeyConsumed = g_hotkeyConsumed.load(std::memory_order_relaxed);
        return a;
    }
    // ---- Ownership observation state. Guarded by the same recursive g_cs as the router state; the
    // immutable snapshot and the ring are copies/values, so a consumer never observes a torn route identity.
    static Ownership g_ownership;
    static RouteEvent g_routes[kRouteCapacity];
    static int g_routeStart = 0, g_routeLen = 0;
    static unsigned long long g_routeDropped = 0;
    static bool g_rawRegisteredByRouter = false;   // what THIS router registered (not the stale g_weRegistered)
    // A record is not observable until the terminal callbacks return. In particular a Present sampling
    // during CallWindowProc must not call an in-flight delivery "Tested". Nested window messages get
    // their own batch; each event keeps the epoch captured at its actual routing decision.
    static thread_local std::vector<RouteEvent>* g_delivery = nullptr;
    struct DeliveryBatch {
        std::vector<RouteEvent> events;
        std::vector<RouteEvent>* previous = g_delivery;
        DeliveryBatch() { g_delivery = &events; }
        ~DeliveryBatch() {
            g_delivery = previous;
            FrameScope lock;
            for (const auto& event : events) {
                g_routes[(g_routeStart + g_routeLen) % kRouteCapacity] = event;
                if (g_routeLen < kRouteCapacity) ++g_routeLen;
                else { g_routeStart = (g_routeStart + 1) % kRouteCapacity; ++g_routeDropped; }
            }
        }
    };
    static void RecordRouteLocked(UINT msg, WPARAM wParam, Route target, unsigned recipients, bool cleanup = false,
                                  unsigned long long pressEpoch = 0, USHORT rawButtons = 0, ULONG rawTag = 0) {
        RouteEvent e;
        e.epoch = g_ownership.epoch; e.msg = msg; e.wParam = wParam; e.target = target; e.recipients = recipients;
        e.cleanup = cleanup; e.pressEpoch = pressEpoch; e.rawButtons = rawButtons; e.rawTag = rawTag;
        g_delivery->push_back(e); // WndProc owns this batch; no external call is made under g_cs.
    }
    Ownership CurrentOwnership() { FrameScope lock; return g_ownership; }
    void PublishOwnership(const OwnershipPolicy& p) {
        FrameScope lock;
        const bool menuOpen = p.menuOpen;
        const bool mouseToUi = menuOpen && p.mouseToUi, keysToUi = menuOpen && p.keysToUi;
        const bool placing = menuOpen && p.placing, textInput = menuOpen && p.textInput;
        const bool mouseOverUi = menuOpen && p.mouseOverUi, play = menuOpen && p.play;
        const bool keyboardPlacement = placing && core::g_keyboardPlacement && !keysToUi;
        if (g_ownership.menuOpen != menuOpen || g_ownership.mouseToUi != mouseToUi || g_ownership.keysToUi != keysToUi
            || g_ownership.placing != placing || g_ownership.textInput != textInput || g_ownership.mouseOverUi != mouseOverUi
            || g_ownership.play != play || g_ownership.keyboardPlacement != keyboardPlacement) ++g_ownership.epoch;
        g_ownership.menuOpen = menuOpen; g_ownership.mouseToUi = mouseToUi; g_ownership.keysToUi = keysToUi;
        g_ownership.placing = placing; g_ownership.textInput = textInput; g_ownership.mouseOverUi = mouseOverUi;
        g_ownership.play = play; g_ownership.keyboardPlacement = keyboardPlacement;
    }
    int TakeRouted(RouteEvent* out, int max) {
        FrameScope lock;
        if (max <= 0 || !out) return 0;
        const int n = g_routeLen < max ? g_routeLen : max;
        for (int i = 0; i < n; ++i) out[i] = g_routes[(g_routeStart + i) % kRouteCapacity];
        g_routeStart = (g_routeStart + n) % kRouteCapacity; g_routeLen -= n;
        return n;
    }
    unsigned long long RoutedDropped() { FrameScope lock; return g_routeDropped; }
    bool Matches(const Ownership& sampled, const RouteEvent& event) { return sampled.epoch == event.epoch; }
    // ---- Release ledger. One entry per scan-code variant / mouse button holding the ACTUAL
    // delivery recipients of a press and the epoch it was pressed under. A release unions those recipients, so a
    // mode/focus switch can never swallow the release of an input a previous recipient still holds. Guarded by
    // g_cs; the release dispatch itself always runs outside the section.
    struct HeldSlot { unsigned recipients = 0; unsigned long long epoch = 0; WPARAM vk = 0; LPARAM keyData = 0; };
    static HeldSlot g_heldKeys[512];
    static HeldSlot g_heldButtons[5];
    static unsigned PairHeldLocked(HeldSlot& held, unsigned recipients, bool up, unsigned long long& pressEpoch) {
        if (up) {
            if (held.recipients) pressEpoch = held.epoch;
            recipients |= held.recipients;
            held = HeldSlot{};
        } else {
            if (!held.recipients) held.epoch = g_ownership.epoch;
            held.recipients |= recipients;
            pressEpoch = held.epoch;
        }
        return recipients;
    }
    // Game/backend releases dispatch outside the lock. Placement receives its release through TrackKey's
    // scan-state clear, so retain that recipient in the trace even after the current bindings change.
    static unsigned PairHeldReleaseLocked(HeldSlot& held, unsigned recipients, unsigned long long& pressEpoch) {
        if (held.recipients) pressEpoch = held.epoch;
        recipients |= (held.recipients & (RecipientUi | RecipientGame | RecipientPlacement));
        held = HeldSlot{};
        return recipients;
    }
    static int MouseButtonIndex(UINT msg, WPARAM wParam) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 1;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 2;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: return (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 3 : 4;
        default: return -1;
        }
    }
    static bool MouseButtonUpMessage(UINT msg) { return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP; }
    static UINT ButtonMessageFor(int button, bool up) {
        static const UINT down[5] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN };
        static const UINT release[5] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP };
        return up ? release[button] : down[button];
    }
    static WPARAM ButtonWParamFor(int button) { return button < 3 ? 0 : MAKEWPARAM(0, button == 3 ? XBUTTON1 : XBUTTON2); }
    struct PendingRelease { UINT msg; WPARAM wParam; LPARAM lParam; unsigned recipients; unsigned long long pressEpoch; };
    // Caller owns g_cs. Records the explicit cleanup trace for every held input and clears the ledger; the
    // dispatch of the returned releases happens after the caller has left the section.
    static void CollectHeldReleasesLocked(std::vector<PendingRelease>& out) {
        for (int i = 0; i < 512; ++i) {
            HeldSlot& held = g_heldKeys[i];
            if (!held.recipients) continue;
            const UINT up = (held.keyData & ((LPARAM)1 << 29)) ? WM_SYSKEYUP : WM_KEYUP;
            out.push_back({ up, held.vk, held.keyData | (LPARAM)0xC0000000ULL, held.recipients, held.epoch });
            held = HeldSlot{};
        }
        for (int b = 0; b < 5; ++b) {
            HeldSlot& held = g_heldButtons[b];
            if (!held.recipients) continue;
            out.push_back({ ButtonMessageFor(b, true), ButtonWParamFor(b), 0, held.recipients, held.epoch });
            held = HeldSlot{};
        }
        for (const PendingRelease& r : out)
            RecordRouteLocked(r.msg, r.wParam, r.recipients == RecipientPlacement ? RouteBlocked :
                              (r.recipients & RecipientUi) && !(r.recipients & RecipientGame) ? RouteUi : RouteGame,
                              r.recipients, true, r.pressEpoch);
    }
    static void ResetLedgerLocked() { memset(g_heldKeys, 0, sizeof g_heldKeys); memset(g_heldButtons, 0, sizeof g_heldButtons); }
    static void ClientSize(int* w, int* h) {
        RECT rc = {};
        if (g_hwnd && GetClientRect(g_hwnd, &rc)) { *w = rc.right - rc.left; *h = rc.bottom - rc.top; }
        else { *w = 1920; *h = 1080; }
    }
    static bool WindowedClient() {
        if (!g_hwnd) return false;
        RECT rc{}; if (!GetClientRect(g_hwnd, &rc)) return false;
        POINT tl{0, 0}; if (!ClientToScreen(g_hwnd, &tl)) return false;
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi)) return false;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        return tl.x != mi.rcMonitor.left || tl.y != mi.rcMonitor.top ||
               w != mi.rcMonitor.right - mi.rcMonitor.left || h != mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    static void SetVirtualCursorClient(LONG x, LONG y) {
        int w, h; ClientSize(&w, &h);
        const float maxX = (float)(w > 0 ? w - 1 : 0);
        const float maxY = (float)(h > 0 ? h - 1 : 0);
        Lock();
        g_vx = (float)x < 0.0f ? 0.0f : ((float)x > maxX ? maxX : (float)x);
        g_vy = (float)y < 0.0f ? 0.0f : ((float)y > maxY ? maxY : (float)y);
        Unlock();
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
            const int sx = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
            const int sy = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
            POINT p = { sx + static_cast<LONG>(m.lLastX * sw / 65535.0), sy + static_cast<LONG>(m.lLastY * sh / 65535.0) };
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
            const int downs = g_pendingButtons[b][0], ups = g_pendingButtons[b][1];
            if (toUi) {
                for (int i = 0; i < downs; ++i) io.AddMouseButtonEvent(b, true);
                if (downs > 0) { g_rawUiHeld[b] = true; g_rawUiEpoch[b] = input::CurrentOwnership().epoch; }
                for (int i = 0; i < ups; ++i) io.AddMouseButtonEvent(b, false);
                if (ups > 0) g_rawUiHeld[b] = false;
            } else if (ups > 0 && g_rawUiHeld[b]) {
                // The policy moved away while the backend still holds this button (e.g. edit -> play with
                // the menu open): the release belongs to the prior UI owner, not to the current route.
                for (int i = 0; i < ups; ++i) io.AddMouseButtonEvent(b, false);
                g_rawUiHeld[b] = false;
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

    // ImGui's Win32 backend polls GetCursorPos every frame; on the render thread while the menu is open it gets the
    // virtual cursor. The optional admission must not block the caller: when the input scope is owned elsewhere
    // this detour reads no router state (no menu flag, no HWND, no virtual cursor, no render thread id) and delegates
    // to the saved original exactly once. After an admission that is not the virtual-cursor branch, this detour's own
    // acquisition is released before the original runs (an outer recursive scope keeps its own).
    static BOOL WINAPI hkGetCursorPos(LPPOINT p) {
        ++g_cursorCalls;
        FrameScope frame(std::try_to_lock);
        if (!frame.OwnsLock()) { ++g_cursorBypassed; return oGetCursorPos(p); }
        ++g_cursorAdmitted;
        if (p && core::g_menuOpen && g_renderTid && GetCurrentThreadId() == g_renderTid && g_hwnd) {
            POINT c = { static_cast<LONG>(g_vx), static_cast<LONG>(g_vy) };
            ClientToScreen(g_hwnd, &c); *p = c; return TRUE;
        }
        frame.Release();
        return oGetCursorPos(p);
    }

    static void EnsureRawInput() {
        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> devs(n);
        if (n) GetRegisteredRawInputDevices(devs.data(), &n, sizeof(RAWINPUTDEVICE));
        bool registeredByRouter = false;
        bool found = false;
        for (UINT i = 0; i < n; ++i) {
            if (devs[i].usUsagePage == 0x01 && devs[i].usUsage == 0x02) {
                found = true;
                g_rawButtons = (devs[i].dwFlags & RIDEV_NOLEGACY) != 0;
                core::Log("[input] game registered raw mouse (flags 0x%X)%s", devs[i].dwFlags, g_rawButtons ? ", no legacy buttons" : "");
                break;
            }
        }
        if (!found) {
            g_rawButtons = false;   // the router's own registration is legacy-capable; a previous NOLEGACY state must not leak
            RAWINPUTDEVICE rid = { 0x01, 0x02, 0, g_hwnd };
            g_weRegistered = RegisterRawInputDevices(&rid, 1, sizeof rid) != 0;
            registeredByRouter = g_weRegistered;
            core::Log("[input] raw mouse %s", g_weRegistered ? "registered by us" : "registration FAILED");
        }
        {
            FrameScope lock;   // the snapshot identity changes with the actual registration, under the same boundary
            if (g_ownership.rawNoLegacy != g_rawButtons || g_ownership.rawRegisteredByRouter != registeredByRouter) ++g_ownership.epoch;
            g_ownership.rawNoLegacy = g_rawButtons; g_ownership.rawRegisteredByRouter = registeredByRouter;
            g_rawRegisteredByRouter = registeredByRouter;
        }
    }

    void MenuOpened() {
        int w, h; ClientSize(&w, &h);
        POINT p{}; const bool haveWindowCursor = WindowedClient() && oGetCursorPos && oGetCursorPos(&p) && ScreenToClient(g_hwnd, &p);
        Lock(); g_vx = haveWindowCursor ? (float)p.x : w * 0.5f; g_vy = haveWindowCursor ? (float)p.y : h * 0.5f; g_pendingDx = g_pendingDy = 0; for (auto& b : g_pendingButtons) b[0] = b[1] = 0; g_pendingWheel = 0; Unlock();
        if (ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            for (int b = 0; b < 5; ++b) if (io.MouseDown[b]) io.AddMouseButtonEvent(b, false);
        }
    }
    // Collects UI-held releases for menu-close dispatch. Game-held entries are dropped without dispatch
    // (a closed menu forwards every physical message to the game, so natural ups complete there; keeping
    // them would union a stale Game recipient into future UI presses). Raw-held buttons join as synthesized
    // button releases. Every slot clears either way. Caller owns g_cs.
    static void CollectUiReleasesLocked(std::vector<PendingRelease>& out) {
        for (int i = 0; i < 512; ++i) {
            HeldSlot& held = g_heldKeys[i];
            if (!held.recipients) continue;
            if (held.recipients & RecipientUi) {
                const UINT up = (held.keyData & ((LPARAM)1 << 29)) ? WM_SYSKEYUP : WM_KEYUP;
                out.push_back({ up, held.vk, held.keyData | (LPARAM)0xC0000000ULL, held.recipients, held.epoch });
            }
            held = HeldSlot{};
        }
        for (int b = 0; b < 5; ++b) {
            HeldSlot& held = g_heldButtons[b];
            if (held.recipients & RecipientUi)
                out.push_back({ ButtonMessageFor(b, true), ButtonWParamFor(b), 0, held.recipients, held.epoch });
            held = HeldSlot{};
            if (g_rawUiHeld[b]) {
                out.push_back({ ButtonMessageFor(b, true), ButtonWParamFor(b), 0, RecipientUi, g_rawUiEpoch[b] });
                g_rawUiHeld[b] = false;
            }
        }
    }
    void MenuClosed() {
        // Return-to-game repair (B1 r3). Dispatch is driven by DELIVERED held recipients, never by the
        // last-frame IO state: a press delivered since the last NewFrame is queued in the backend (bit
        // set, IO.MouseDown still false) and lives only in the ledger. Every collected UI release goes
        // through the real backend handler outside the router acquisition (buttons) or the bulk key clear
        // (keys hold no capture); cleanup traces are recorded only for releases actually delivered above.
        // The trace commits after the dispatches (DeliveryBatch destructor order).
        DeliveryBatch batch;
        ClearKeys(); // record placement scan-state releases before the menu-close ledger is discarded
        std::vector<PendingRelease> ui;
        {
            FrameScope lock;
            CollectUiReleasesLocked(ui);
        }
        if (ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearInputKeys();   // keys hold no capture; the bulk clear delivers every UI-held key
            for (const PendingRelease& r : ui) {
                if (MouseButtonIndex(r.msg, r.wParam) >= 0)
                    ImGui_ImplWin32_WndProcHandler(g_hwnd, r.msg, r.wParam, r.lParam);
            }
            FrameScope lock;   // short: delivered-cleanup records only; no backend call under it
            for (const PendingRelease& r : ui)
                RecordRouteLocked(r.msg, r.wParam, RouteUi, r.recipients, true, r.pressEpoch);
        }
        Lock(); g_pendingDx = g_pendingDy = 0; for (auto& b : g_pendingButtons) b[0] = b[1] = 0; g_pendingWheel = 0; Unlock();
    }

    // scan code key state for free camera and optional keyboard placement: set on key-down, cleared on key-up/focus loss and mode transitions.
    // No time-out: Windows auto-repeats only the last pressed key, so a held key may stay silent.
    static bool g_scanDown[512] = { false };
    // Bounded hotkey latch: one byte per VK holding a Pending and a Down bit. It is not a queue and keeps no
    // history, so several taps before one consume saturate to a single pending action by construction.
    enum { kHotkeyPending = 1, kHotkeyDown = 2 };
    static unsigned char g_hotkeyState[256] = {};
    // Windows delivers key messages only to the focused window, so a live subclassed game window starts focused; the
    // WndProc keeps this accurate and resets the latch on any focus change (a press must not survive the gap).
    static bool g_focused = true;
    // Same-VK configuration support: the overlay samples both configured actions before either side effect, so a
    // consume may feed exactly the immediately following identical query (and nothing later, hence the sequence).
    static unsigned long long g_hotkeyQuerySeq = 0, g_hotkeyShareAt = 0;
    static unsigned char g_hotkeyShareVk = 0;
    // Bounded live diagnostics: which message produced the most recent pending latch. Every writer and reader of the
    // compound latch/scan/focus state takes the same recursive g_cs boundary in short scopes: TrackKey and the
    // focus/reset paths on the WndProc thread, HotkeyPressed (and the whole WB frame admission in overlay.cpp) on
    // the Present thread. Nothing here feeds a routing decision.
    static unsigned char g_hotkeyLatchVk = 0;
    static bool g_hotkeyLatchSys = false;
    static unsigned long long g_hotkeyLatched = 0;
    static bool IsConfiguredHotkey(WPARAM vk) {
        return vk != 0 && vk <= 0xFF && (vk == static_cast<WPARAM>(core::g_keyToggle) || vk == static_cast<WPARAM>(core::g_keyMode));
    }
    // Discards Pending and Down without synthesizing a press: the next action needs a fresh non-repeat down. The
    // caller owns g_cs: the latch, the shared-key sequence and the scan state are one compound unit.
    static void ResetHotkeyLatchLocked() { memset(g_hotkeyState, 0, sizeof g_hotkeyState); g_hotkeyShareVk = 0; g_hotkeyShareAt = 0; }
    // Producer side of the compound state. The whole body is a short acquisition of the same recursive section the
    // Present frame admission and the consumer use, so a key message can never interleave with a consume or with a
    // frame that owns the section. No OS/backend/engine call happens under this scope: ImGui's handler, the original
    // WndProc and every other external callback run only after it has been released.
    static void TrackKey(UINT msg, WPARAM vk, LPARAM lParam) {
        FrameScope lock;
        const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) g_scanDown[scan | (ext << 8)] = true;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            // Shift+numpad inserts a fake extended Shift-up around the numpad event. Do not clear the real Shift for that event.
            if ((scan == 0x2A || scan == 0x36) && ext) g_scanDown[scan | 256] = false;
            else { g_scanDown[scan] = false; g_scanDown[scan | 256] = false; }
        }
        // Latch only a configured key, only while the real WndProc is live and the window is focused: a fresh
        // (non-repeat) down records Pending + Down, an auto-repeat changes nothing, an up clears Down only.
        if (!IsConfiguredHotkey(vk) || !g_original || !g_hwnd) return;
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if ((lParam & (static_cast<LPARAM>(1) << 30)) != 0) return;
            if (g_hotkeyState[vk] & kHotkeyDown) return;
            if (!g_focused) return;
            g_hotkeyState[vk] |= (kHotkeyPending | kHotkeyDown);
            g_hotkeyLatchVk = static_cast<unsigned char>(vk);
            g_hotkeyLatchSys = (msg == WM_SYSKEYDOWN);
            ++g_hotkeyLatched;
        } else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            g_hotkeyState[vk] &= static_cast<unsigned char>(~kHotkeyDown);
        }
    }
    bool ScanDown(int scan, bool ext) { FrameScope lock; return g_scanDown[(scan & 0xFF) | (ext ? 256 : 0)]; }
    // Reset local consumers, but keep Game/UI holders until their natural release or focus cleanup.
    void ClearKeys() {
        DeliveryBatch batch;
        FrameScope lock;
        memset(g_scanDown, 0, sizeof g_scanDown); ResetHotkeyLatchLocked();
        for (HeldSlot& held : g_heldKeys) {
            if (!(held.recipients & RecipientPlacement)) continue;
            const UINT up = (held.keyData & ((LPARAM)1 << 29)) ? WM_SYSKEYUP : WM_KEYUP;
            RecordRouteLocked(up, held.vk, RouteBlocked, RecipientPlacement, true, held.epoch);
            held.recipients &= ~RecipientPlacement;
            if (!held.recipients) held = HeldSlot{};
        }
    }
    // Configured keys are event-first: the pending bit recorded from the real WndProc is consumed once per tap. The
    // wasDown output mirrors the event Down state and is never a second action source. Other VKs keep the
    // asynchronous rising-edge policy (GetAsyncKeyState), unchanged from upstream.
    bool HotkeyPressed(int vk, bool& wasDown) {
        const bool configured = IsConfiguredHotkey(static_cast<WPARAM>(vk));
        bool consumed = false, shared = false;
        unsigned char latchVk = 0; bool latchSys = false; unsigned long long latched = 0;
        {
            // Consumer side: the state copy/consume is one short acquisition of the same boundary the producer and
            // the focus/reset paths use. GetAsyncKeyState and the log below stay outside it.
            FrameScope lock;
            const unsigned long long seq = ++g_hotkeyQuerySeq;
            if (configured) {
                const unsigned char state = g_hotkeyState[vk];
                wasDown = (state & kHotkeyDown) != 0;
                if (g_original && g_hwnd) {   // no live WndProc: no event source, so no synthesized press
                    if (state & kHotkeyPending) {
                        g_hotkeyState[vk] = static_cast<unsigned char>(state & ~kHotkeyPending);   // one consume per pending action
                        // One VK carrying BOTH configured actions shares that single consume with the immediately following
                        // identical query (the overlay samples both actions before either side effect). With distinct VKs no
                        // share exists: a repeated query for the same key is a second, separate action and stays false.
                        if (core::g_keyToggle == core::g_keyMode) { g_hotkeyShareVk = static_cast<unsigned char>(vk); g_hotkeyShareAt = seq; }
                        ++g_hotkeyConsumed;
                        latchVk = g_hotkeyLatchVk; latchSys = g_hotkeyLatchSys; latched = g_hotkeyLatched;
                        consumed = true;
                    } else if (core::g_keyToggle == core::g_keyMode && g_hotkeyShareVk == static_cast<unsigned char>(vk) && seq == g_hotkeyShareAt + 1) {
                        g_hotkeyShareVk = 0;   // both configured actions of the same VK share the one consume of this frame
                        shared = true;
                    }
                }
            }
        }
        if (!configured) {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool pressed = down && !wasDown;
            wasDown = down;
            return pressed;
        }
        if (shared) return true;
        if (!consumed) return false;
        core::Log("[input] hotkey consumed vk=%u origin=%s latched_vk=%u latched=%llu wasDown=%d share=%d consumed=%llu",
                  (unsigned)vk, latchSys ? "syskeydown" : "keydown", (unsigned)latchVk,
                  latched, wasDown ? 1 : 0, core::g_keyToggle == core::g_keyMode ? 1 : 0,
                  (unsigned long long)g_hotkeyConsumed.load(std::memory_order_relaxed));
        return true;
    }
    bool ScanDownAny(int scan) {
        FrameScope lock;
        if (scan == 0x1C || scan == 0x35) return ScanDown(scan, false) || ScanDown(scan, true); // Enter, /
        const bool shift = ScanDown(0x2A, false) || ScanDown(0x36, false);
        return ScanDown(scan, false) || (shift && ScanDown(scan, true));
    }
    bool VkDown(int vk) {
        FrameScope lock;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return ScanDown(0x2A, false) || ScanDown(0x36, false);
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return ScanDown(0x1D, false) || ScanDown(0x1D, true);
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return ScanDown(0x38, false) || ScanDown(0x38, true);
        const int scan = (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC); if (!scan) return false;
        const bool numpad = (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_DECIMAL || vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_MULTIPLY;
        const bool ext = vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE;
        if (vk == VK_RETURN) return ScanDown(scan, false) || ScanDown(scan, true);
        if (numpad) return ScanDownAny(scan);
        return ScanDown(scan, ext);
    }
    static bool IsFreeCamScan(int scan) { return scan == 0x11 || scan == 0x1E || scan == 0x1F || scan == 0x20 || scan == 0x10 || scan == 0x12 || scan == 0x2A || scan == 0x1D || scan == 0x39; }   // W A S D Q E Shift Ctrl Space
    void SetFreeCam(bool on) {
        FrameScope lock;
        if (g_ownership.camera != on) ++g_ownership.epoch;   // a route transition gets a fresh identity
        g_ownership.camera = on;
        g_freeCam = on; g_lookDx = g_lookDy = 0;
    }
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
            FrameScope lock;
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
    // Publish the whole fixed binding list under the router lock; no transient empty/partially updated list.
    static const int kMaxPlaceVks = 32; static int g_placeVkN = 0, g_placeVkArr[kMaxPlaceVks] = {};
    void SetPlaceVks(const int* vks, int count) {
        const int n = count < 0 ? 0 : count > kMaxPlaceVks ? kMaxPlaceVks : count;
        FrameScope lock;
        bool changed = n != g_placeVkN;
        for (int i = 0; i < n; ++i) if (g_placeVkArr[i] != vks[i]) changed = true;
        if (!changed) return;
        for (int i = 0; i < n; ++i) g_placeVkArr[i] = vks[i];
        g_placeVkN = n;
        ++g_ownership.bindingRevision; ++g_ownership.epoch;
        ClearKeys();
    }
    static bool IsPlaceKeyFixed(WPARAM vk) { return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_RETURN || vk == VK_BACK || vk == VK_DECIMAL || vk == VK_CLEAR || vk == VK_MULTIPLY || vk == VK_DIVIDE || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9); }
    static bool IsPlaceKey(WPARAM vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL) return false;
        const int n = (int)g_placeVkN;
        if (n == 0) return IsPlaceKeyFixed(vk);
        for (int i = 0; i < n; i++) if ((WPARAM)g_placeVkArr[i] == vk) return true;
        static const int nav[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_INSERT, VK_DELETE, VK_CLEAR };
        static const int np[]  = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD1, VK_NUMPAD0, VK_DECIMAL, VK_NUMPAD5 };
        for (int j = 0; j < 11; j++) if (vk == (WPARAM)nav[j]) for (int i = 0; i < n; i++) if (g_placeVkArr[i] == np[j]) return true;
        return false;
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
    // ---- Observation: which messages the routing boundary accounts for (the fixture's own control
    // messages and every unrelated window notification are never recorded as input delivery). ----
    static bool IsObservable(UINT msg) {
        return IsKeyboard(msg) || IsMouse(msg) || IsIme(msg) || msg == WM_INPUT || msg == WM_SETCURSOR
            || msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE
            || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ACTIVATE;
    }
    static void RecordRouted(UINT msg, WPARAM wParam, Route target, unsigned recipients) {
        FrameScope lock;
        RecordRouteLocked(msg, wParam, target, recipients);
    }
    // While the menu is open the game keeps running and stays controllable: the mouse belongs to the menu only while the
    // cursor is over a World Builder window (core::g_uiWantsMouse), the keyboard only while a text field is active (g_uiWantsKeyboard).
    // Optional placement consumes only configured keys when keyboard capture is off. Releases still repair every
    // earlier recipient, including a Game/UI press from before placement or a binding change.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        DeliveryBatch delivery;
        if (IsKeyboard(msg)) TrackKey(msg, wParam, lParam);
        // Focus is part of the compound state: one short acquisition each, so a focus reset cannot interleave with a
        // consume or with the frame that owns the section. Everything after these branches runs outside the boundary.
        if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)) { FrameScope focus; if (!g_focused) ++g_ownership.epoch; g_focused = true; g_ownership.focused = true; }
        if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) {
            // Release repair: the ledger is cleared and its cleanup trace recorded under the short acquisition, and
            // the releases themselves are dispatched only AFTER the section has been left, so a game-side release
            // callback can never run behind the router lock (and can never block the window thread on a frame).
            std::vector<PendingRelease> releases;
            {
                FrameScope focus;
                if (g_focused) ++g_ownership.epoch;
                memset(g_scanDown, 0, sizeof g_scanDown); g_rmb = false;
                g_focused = false; g_ownership.focused = false;
                ResetHotkeyLatchLocked();
                CollectHeldReleasesLocked(releases);
            }
            for (const PendingRelease& r : releases) {
                if (r.recipients & RecipientUi) ImGui_ImplWin32_WndProcHandler(hwnd, r.msg, r.wParam, r.lParam);
                if (r.recipients & RecipientGame) CallWindowProc(g_original, hwnd, r.msg, r.wParam, r.lParam);
            }
        }
        // Keep the platform backend's keyboard code page in sync even when the game's WndProc would otherwise consume the change.
        if (core::g_menuOpen && msg == WM_INPUTLANGCHANGE) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (g_freeCam) {
            if (msg == WM_INPUT) {
                const int take = FreeCamRaw(reinterpret_cast<HRAWINPUT>(lParam));
                if (take) { RecordRouted(msg, wParam, RouteCamera, RecipientCamera); return DefWindowProcW(hwnd, msg, wParam, lParam); }   // the game neither turns nor walks
                if (core::g_menuOpen && core::g_uiWantsMouse) { RecordRouted(msg, wParam, RouteSystem, RecipientSystem); return DefWindowProcW(hwnd, msg, wParam, lParam); }
                RecordRouted(msg, wParam, RouteGame, RecipientGame);
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR) && !core::g_uiTextInput && IsFreeCamScan((int)((lParam >> 16) & 0xFF))) {
                // not to the game; the editor still sees Ctrl / Shift (Ctrl-click, shortcuts). Key-ups pass below: the game must see
                // a key released that it saw pressed
                unsigned rec = RecipientCamera;
                if (core::g_menuOpen && msg != WM_CHAR) { ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); rec |= RecipientUi; }
                if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
                    // The paired release arrives through ordinary keyboard routing (key-ups are never
                    // camera-consumed) and through focus repair; the ledger carries the actual recipients
                    // to both, so the backend press above always meets its release.
                    FrameScope lock;
                    const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
                    const int slot = scan | (ext << 8);
                    unsigned long long pressEpoch = 0;
                    rec = PairHeldLocked(g_heldKeys[slot], rec, false, pressEpoch);
                    g_heldKeys[slot].vk = wParam; g_heldKeys[slot].keyData = lParam;
                    RecordRouteLocked(msg, wParam, RouteCamera, rec, false, pressEpoch);
                } else {
                    RecordRouted(msg, wParam, RouteCamera, rec);
                }
                return 0;
            }
            if (IsMouse(msg) && (!core::g_menuOpen || ((g_rmb || msg == WM_RBUTTONDOWN) && !core::g_uiMouseOverUi))) {
                if (msg == WM_RBUTTONDOWN) g_rmb = true; else if (msg == WM_RBUTTONUP) g_rmb = false;
                const int button = MouseButtonIndex(msg, wParam);
                const bool up = MouseButtonUpMessage(msg);
                unsigned rec = RecipientCamera;
                if (core::g_menuOpen && !g_rawButtons) { ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); rec |= RecipientUi; }   // right click / drag in the world: the editor's context menu
                unsigned long long pressEpoch = 0;
                {
                    FrameScope lock;   // short: ledger + route record only
                    if (button >= 0) {
                        if (up) rec = PairHeldReleaseLocked(g_heldButtons[button], rec, pressEpoch);
                        else rec = PairHeldLocked(g_heldButtons[button], rec, false, pressEpoch);
                    }
                    if (up) rec |= RecipientGame;
                    RecordRouteLocked(msg, wParam, RouteCamera, rec, false, pressEpoch);
                }
                if (up) return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;   // no attack / aim while flying
            }
        }
        if (core::g_menuOpen) {
            const bool mouseToUi = core::g_uiWantsMouse, keysToUi = core::g_uiWantsKeyboard;
            if (msg == WM_MOUSEMOVE && WindowedClient()) {
                SetVirtualCursorClient((short)LOWORD(lParam), (short)HIWORD(lParam));
            }
            if (core::g_uiTextInput && IsIme(msg)) {
                TrackImeComposition(hwnd, msg, lParam);
                // The game owns the real HWND and may consume IME composition messages. Give them to DefWindowProc instead;
                // it drives the native composition/candidate window and emits WM_CHAR for committed UTF-16 text, which the
                // normal ImGui Win32 path below consumes. ImGui's default Win32 IME callback positions the candidate window.
                RecordRouted(msg, wParam, RouteSystem, RecipientSystem);
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            if (msg == WM_INPUT) {
                OnRawInput(reinterpret_cast<HRAWINPUT>(lParam));        // the virtual cursor always follows the mouse
                if (mouseToUi) { RecordRouted(msg, wParam, RouteUi, RecipientUi | RecipientSystem); return DefWindowProcW(hwnd, msg, wParam, lParam); }   // the game does not look around while we use the menu
                RecordRouted(msg, wParam, RouteGame, RecipientGame);
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if (msg == WM_SETCURSOR) { RecordRouted(msg, wParam, RouteSystem, RecipientSystem); SetCursor(nullptr); return TRUE; }
            if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE) {
                if (mouseToUi) { RecordRouted(msg, wParam, RouteUi, RecipientUi); return 0; }
                RecordRouted(msg, wParam, RouteGame, RecipientGame);
                return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
            }
            if (IsMouse(msg)) {
                const int button = MouseButtonIndex(msg, wParam);
                const bool up = MouseButtonUpMessage(msg);
                Route target = mouseToUi ? RouteUi : RouteGame;
                unsigned recipients = mouseToUi ? RecipientUi : RecipientGame;
                unsigned long long pressEpoch = 0;
                {
                    FrameScope lock;   // short: ledger + route record only; the backend/game callbacks run outside
                    if (button >= 0) {
                        if (up) recipients = PairHeldReleaseLocked(g_heldButtons[button], recipients, pressEpoch);
                        else recipients = PairHeldLocked(g_heldButtons[button], recipients, false, pressEpoch);
                    }
                    RecordRouteLocked(msg, wParam, target, recipients, false, pressEpoch);
                }
                if (recipients & RecipientUi) { if (!g_rawButtons) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); }
                if (recipients & RecipientGame) return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;
            }
            if (IsKeyboard(msg)) {
                const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                const bool up = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
                unsigned recipients = keysToUi ? RecipientUi : RecipientGame;
                unsigned long long pressEpoch = 0;
                {
                    FrameScope lock;   // one decision/ledger snapshot; callbacks run outside
                    const bool placement = !keysToUi && core::g_keyboardPlacement && core::g_placing && IsPlaceKey(wParam);
                    const Route target = keysToUi ? RouteUi : placement ? RouteBlocked : RouteGame;
                    if (placement) recipients = RecipientPlacement; // TrackKey delivered the scan state polled by placement
                    if (down || up) {
                        const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
                        const int slot = scan | (ext << 8);
                        if (up) recipients = PairHeldReleaseLocked(g_heldKeys[slot], recipients, pressEpoch);
                        else {
                            // Remember old holders for release, never send a new down to a previous route.
                            PairHeldLocked(g_heldKeys[slot], recipients, false, pressEpoch);
                            g_heldKeys[slot].vk = wParam; g_heldKeys[slot].keyData = lParam;
                        }
                    }
                    if (up && keysToUi) recipients |= RecipientGame;   // upstream: the UI key-up is still forwarded to the game
                    RecordRouteLocked(msg, wParam, target, recipients, false, pressEpoch);
                }
                if (recipients & RecipientUi) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                // A release whose prior recipient was the game is forwarded even after a route switch: a new route
                // must not swallow the release of a key the game actually saw pressed.
                if (recipients & RecipientGame) return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;
            }
        } else if (msg == WM_INPUT && g_weRegistered) {
            RecordRouted(msg, wParam, RouteSystem, RecipientSystem);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        // Closed-menu fallthrough: everything goes to the game, but keyboard/button presses still pair
        // through the ledger. A release therefore unions (then clears) any stale slot — e.g. a free-camera
        // press from before the menu closed — instead of leaving a dead held identity behind. The masked
        // union keeps the recorded recipients exactly Game in the common case.
        if (IsKeyboard(msg) || (IsMouse(msg) && MouseButtonIndex(msg, wParam) >= 0)) {
            const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            const bool up = (msg == WM_KEYUP || msg == WM_SYSKEYUP) || MouseButtonUpMessage(msg);
            unsigned rec = RecipientGame;
            unsigned long long pressEpoch = 0;
            if ((IsKeyboard(msg) && (down || up)) || IsMouse(msg)) {
                FrameScope lock;   // short: ledger + route record only; the game callback runs outside
                if (IsMouse(msg)) {
                    const int button = MouseButtonIndex(msg, wParam);
                    if (up) rec = PairHeldReleaseLocked(g_heldButtons[button], rec, pressEpoch);
                    else rec = PairHeldLocked(g_heldButtons[button], rec, false, pressEpoch);
                } else if (up) {
                    const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
                    rec = PairHeldReleaseLocked(g_heldKeys[scan | (ext << 8)], rec, pressEpoch);
                } else {
                    const int scan = (int)((lParam >> 16) & 0xFF), ext = (int)((lParam >> 24) & 1);
                    const int slot = scan | (ext << 8);
                    rec = PairHeldLocked(g_heldKeys[slot], rec, false, pressEpoch);
                    g_heldKeys[slot].vk = wParam; g_heldKeys[slot].keyData = lParam;
                }
                RecordRouteLocked(msg, wParam, RouteGame, rec, false, pressEpoch);
            } else {
                if (IsObservable(msg)) RecordRouted(msg, wParam, RouteGame, RecipientGame);
            }
            return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
        }
        if (IsObservable(msg)) RecordRouted(msg, wParam, RouteGame, RecipientGame);
        return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
    }

    void Init(HWND hwnd) {
        if (g_original) return;
        if (!g_csReady) { InitializeCriticalSection(&g_cs); g_csReady = true; }
        {
            FrameScope lock;   // binding state changes under the same boundary the producer/consumer use
            g_hwnd = hwnd;
            ResetHotkeyLatchLocked();   // keys already held at install time never become a synthesized press
            ResetLedgerLocked();        // no phantom release for an input nobody delivered under this router
        }
        const WNDPROC previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));   // OS call outside the boundary
        { FrameScope lock; g_original = previous; }
        EnsureRawInput();
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
            if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&hkGetCursorPos), reinterpret_cast<void**>(&oGetCursorPos)) != MH_OK || MH_EnableHook(target) != MH_OK)
                core::Log("[input] could not hook GetCursorPos; the menu cursor may jump");
        }
        core::Log("[input] window %p subclassed", (void*)hwnd);
    }

    void Shutdown() {
        WNDPROC original = nullptr; HWND hwnd = nullptr;
        {
            FrameScope lock;   // after Shutdown the configured keys report no action at all; the binding state clears under the boundary
            ResetHotkeyLatchLocked();
            ResetLedgerLocked();
            original = g_original; hwnd = g_hwnd;
            g_original = nullptr; g_hwnd = nullptr;
        }
        if (original && hwnd) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));   // OS call outside the boundary
    }
}
