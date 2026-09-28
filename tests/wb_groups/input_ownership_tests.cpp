// input_ownership_tests.cpp - InputOwnership host suite (plan wb079-unified-77f68967, Task 8 / C1 observation).
//
// WHAT RUNS FOR REAL HERE
//   * asi/cdmodkit/input.cpp as a linked translation unit: the production WndProc subclass with ALL its upstream
//     route branches (play/edit, placement, free camera, IME/text, raw WM_INPUT, focus reset), the D1 latch, the
//     hotkey consumer and the hkGetCursorPos detour. The routing policy under test is the production one.
//   * tools/imgui/backends/imgui_impl_win32.cpp: the real backend. Its ImGuiIO state (IsKeyDown/MouseDown/
//     GetCapture) is the ACTUAL terminal recipient of a UI delivery, not a label the fixture predicted.
//   * A real hidden HWND created on the fixture's window-owner thread and driven with real SendMessageW, so the
//     production WndProc runs on the window-owner thread exactly like a game's message thread. The fixture's first
//     window procedure records what the ORIGINAL (game) procedure actually received.
//   * asi/cdmodkit/cdmodkit.cpp compiled read-only INTO this fixture TU for its settings path only
//     (production LoadSettings/SaveSettings/KeyName/KeyFromName; attach/engine/pump/hook code never
//     runs). File-static settings state (g_modDir/SettingsPath/LoadSettings) is reachable exactly because
//     it shares the translation unit, with no core edit and no second parser.
//
// WHAT THE FIXTURE SUBSTITUTES (test-owned OS/native seam; never a reimplementation of routed logic)
//   * MinHook: MH_CreateHook installs a real in-process absolute-jump patch at GetCursorPos (VirtualProtect +
//     FlushInstructionCache) and hands the fixture substitute back as the saved original. Never a policy mock.
//   * GetRawInputData valid-payload patch for one magic handle (other handles reach the real API).
//   * i18n/thumbgen settings collaborators (record-only stubs); the core settings globals are production.
//   * The raw mouse packet source: WM_INPUT is delivered with an OS-level synthetic handle, so GetRawInputData
//     fails closed and only the production routing decision (DefWindowProc vs CallWindowProc) is exercised. The
//     RIDEV_NOLEGACY distinction is proven by a real OS device registration, not by a flag the fixture asserts.
//
// HOW THE CASES ARE ORDERED (explicit event barriers only; no sleep, no poll-delay)
//   Every case compares three independent records: the production trace (input::TakeRouted), the messages the
//   original window procedure actually received, and the real backend's ImGuiIO state. The IO-SHORT-LOCK case
//   arms an exact event handshake before the trigger: the original procedure signals that a synthetic cleanup
//   release arrived and then WAITS (bounded) for a second thread to report whether it could take the router
//   section. That reports whether the release repair ran under the section or outside it. There is no sleep
//   anywhere and no assertion depends on elapsed time.
//
// HONEST LIMITS
//   * Host-only: this suite proves the production input layer's route/ownership contract on a real hidden window
//     and real backend; it is not a game-live result and installs no real MinHook library.
//   * No game is running, so the raw mouse registration is the router's own (rawNoLegacy=false); the NOLEGACY
//     field is proven with a real OS registration the fixture installs/removes itself.
//   * WB_IO_CASES selects a comma-separated case list; an empty list runs all.
#define NOMINMAX
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // the included production TU builds with this (asi/cdmodkit/build.bat)
#endif
#include <windows.h>
#include <imm.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../tools/imgui/imgui.h"
#include "../../tools/imgui/backends/imgui_impl_win32.h"
#include "../../asi/cdmodkit/core.h"
#include "../../asi/cdmodkit/input.h"
#include "../../tools/minhook/include/MinHook.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")

// ---- Production settings boundary (B3). The fixture compiles the production core TU read-only for its
// settings path only (LoadSettings/SaveSettings/KeyName/KeyFromName); attach/engine/pump/hook code is
// never executed. The TU builds at the production warning level (same bar as asi/cdmodkit/build.bat).
// LoadSettings/SettingsPath/g_modDir are file-static in that TU, so including it here is the only way to
// reach the real load boundary without editing core or adding a second parser. Engine-service stubs below
// are link-time only and never run on the settings path.
#pragma warning(push, 3)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // production TU builds with this (asi/cdmodkit/build.bat); fixture-local only
#endif
#include "../../asi/cdmodkit/cdmodkit.cpp"
#pragma warning(pop)
namespace thumbgen {
// Service stubs for boundaries the settings path never runs (same "unavailable" class as
// production_core_host.cpp): the thumbnail worker, overlay install/texture cache, loopback HTTP API,
// MinHook installation, and console-camera diagnostics. The i18n collaborator is the REAL i18n.cpp TU
// (manifest); only record-free services are substituted here.
static int s_quality = 3;
void SetQuality(int q) { s_quality = q; }
int Quality() { return s_quality; }
void Start() {}
void SetBackground(bool) {}
bool Background() { return false; }
uint32_t GimmickKey(const std::string&) { return 0; }
bool Lz4Decode(const unsigned char*, size_t, std::vector<unsigned char>&, size_t) { return false; }
}
namespace overlay {
void Install() {}
namespace discovery {
struct ImageInfo;
bool ImageCoreBounds(uint64_t, ImageInfo*) { return false; }
uint64_t GameBoundaryScan(const ImageInfo&, bool, const unsigned char*, const unsigned char*, int, int*, bool*) { return 0; }
}
}
namespace httpapi { bool Start(int) { return false; } }
namespace core {
void CamWatch(int, int) {}
void FovTrace(int) {}
void CamTrace(int) {}
void SetIoTrace(bool) {}
void InstallIoTrace() {}
void EnvironmentInstall() {}
void EnvironmentTick() {}
}
// MinHook installation never runs in a host run (same unavailable answer as production_core_host.cpp).
extern "C" {
MH_STATUS WINAPI MH_Initialize(VOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_Uninitialize(VOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_ERROR_DISABLED; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_ERROR_NOT_CREATED; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "MinHook is not installed in a host run"; }
}

// Placement and optional-key globals are the real U core definitions in the included TU.

namespace fx {
    // ----------------------------------------------------------------------------------------------------
    // Evidence + assertion accumulation. Failures are collected (never thrown) so one broken route cannot hide
    // the state of the others; the fixture exits nonzero if anything was recorded.
    // ----------------------------------------------------------------------------------------------------
    static std::mutex g_logMutex;
    static std::vector<std::string> g_log;
    static void Log(const std::string& line) {
        { std::lock_guard<std::mutex> l(g_logMutex); g_log.push_back(line); }
        std::cout << line << "\n";
    }
    static std::string Num(unsigned long long v) { return std::to_string(v); }

    struct Event {
        HANDLE h = nullptr;
        Event() { h = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
        ~Event() { if (h) CloseHandle(h); }
        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;
        void set() const { if (h) SetEvent(h); }
        void reset() const { if (h) ResetEvent(h); }
        bool wait(unsigned ms) const { return h && WaitForSingleObject(h, ms) == WAIT_OBJECT_0; }
    };
    static const unsigned kMustArriveMs = 5000;   // an event the fixture requires must not need this long
    static const unsigned kProbeMs = 3000;        // the bounded verdict window of the short-lock handshake

    struct Case {
        std::string id;
        int assertions = 0;
        std::vector<std::string> failures;
        std::vector<input::RouteEvent> events;
        void Check(bool ok, const std::string& what) {
            ++assertions;
            if (ok) return;
            failures.push_back(what);
            Log("FAIL " + id + ": " + what);
        }
        void Value(unsigned long long actual, unsigned long long expected, const std::string& what) {
            Check(actual == expected, what + " (expected " + Num(expected) + ", got " + Num(actual) + ")");
        }
        bool Pass() const { return failures.empty(); }
        const input::RouteEvent* Last(UINT msg, WPARAM w) const {
            for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->msg == msg && it->wParam == w) return &*it;
            return nullptr;
        }
        int Count(UINT msg, WPARAM w) const {
            int n = 0;
            for (const auto& e : events) if (e.msg == msg && e.wParam == w) ++n;
            return n;
        }
        int CountCleanup() const {
            int n = 0;
            for (const auto& e : events) if (e.cleanup) ++n;
            return n;
        }
    };

    // ----------------------------------------------------------------------------------------------------
    // The real hidden window: the fixture's own procedure is the ORIGINAL (game) procedure the production
    // WndProc delegates to, so game delivery is observed where it actually lands.
    // ----------------------------------------------------------------------------------------------------
    struct Sent { UINT msg; WPARAM wParam; LPARAM lParam; };
    static std::mutex g_gameMutex;
    static std::vector<Sent> g_game;
    static HWND g_hwnd = nullptr;
    static Event g_ownerReady;
    static std::atomic<unsigned> g_releaseSeen{ 0 };
    static std::atomic<bool> g_probeArmed{ false };
    static Event g_releaseSeenEvent;
    static Event g_probeDone;
    static std::atomic<bool> g_probeAdmitted{ false };
    static std::atomic<bool> g_probeCompletedInCallback{ false };

    static bool Tracked(UINT m) {
        if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) return true;
        if (m == WM_INPUT) return true;
        if (m == WM_IME_COMPOSITION) return true;
        return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
    }
    static void DrainGame() { std::lock_guard<std::mutex> l(g_gameMutex); g_game.clear(); }
    static int GameCount(UINT msg, WPARAM w) {
        std::lock_guard<std::mutex> l(g_gameMutex);
        int n = 0;
        for (const auto& s : g_game) if (s.msg == msg && s.wParam == w) ++n;
        return n;
    }
    static int GameCount(UINT msg) {
        std::lock_guard<std::mutex> l(g_gameMutex);
        int n = 0;
        for (const auto& s : g_game) if (s.msg == msg) ++n;
        return n;
    }
    static LRESULT CALLBACK OriginalProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
        if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
        if (Tracked(msg)) { std::lock_guard<std::mutex> l(g_gameMutex); g_game.push_back({ msg, wparam, lparam }); }
        // IO-SHORT-LOCK handshake: a synthetic cleanup key-up is the trigger; the verdict comes from a second
        // thread that tries to take the router section while this procedure is still running.
        if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && g_probeArmed.exchange(false)) {
            ++g_releaseSeen;
            g_releaseSeenEvent.set();
            g_probeCompletedInCallback.store(g_probeDone.wait(kProbeMs));
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    static void OwnerThread() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = OriginalProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"wb079-input-ownership";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { g_ownerReady.set(); return; }
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"wb079-input-ownership", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
        g_ownerReady.set();
        if (!g_hwnd) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    static void SendMsg(UINT msg, WPARAM wparam, LPARAM lparam) { if (g_hwnd) SendMessageW(g_hwnd, msg, wparam, lparam); }
    static LPARAM KeyLparam(int vk, bool ext, bool repeat = false) {
        const int scan = (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
        LPARAM lp = (LPARAM)1 | (((LPARAM)scan & 0xFF) << 16);
        if (ext) lp |= ((LPARAM)1 << 24);
        if (repeat) lp |= ((LPARAM)1 << 30);
        return lp;
    }
    static void SendKey(int vk, bool down, bool ext = false) { SendMsg(down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, KeyLparam(vk, ext)); }
    static void SendTap(int vk, bool ext = false) { SendKey(vk, true, ext); SendKey(vk, false, ext); }
    static void SendFocus(bool gained) { SendMsg(gained ? WM_SETFOCUS : WM_KILLFOCUS, (WPARAM)g_hwnd, 0); }
    static void SendRaw() { SendMsg(WM_INPUT, RIM_INPUT, (LPARAM)1); }   // synthetic handle: GetRawInputData fails closed

    // ----------------------------------------------------------------------------------------------------
    // The frame's route decision. The production overlay installs the policy into the core:: flags the router
    // reads AND publishes the matching immutable snapshot; the fixture mirrors exactly that pair of calls.
    // ----------------------------------------------------------------------------------------------------
    static bool g_wasOpen = false;
    static void SetPolicy(bool menuOpen, bool mouseToUi, bool keysToUi, bool placing = false,
                          bool textInput = false, bool mouseOverUi = false, bool play = false) {
        if (menuOpen != g_wasOpen) { if (menuOpen) input::MenuOpened(); else input::MenuClosed(); g_wasOpen = menuOpen; }
        core::g_menuOpen = menuOpen;
        core::g_uiWantsMouse = menuOpen && mouseToUi;
        core::g_uiWantsKeyboard = menuOpen && keysToUi;
        core::g_uiTextInput = menuOpen && textInput;
        core::g_uiMouseOverUi = menuOpen && mouseOverUi;
        core::g_placing = placing;
        input::OwnershipPolicy p;
        p.menuOpen = menuOpen; p.mouseToUi = mouseToUi; p.keysToUi = keysToUi; p.placing = placing;
        p.textInput = textInput; p.mouseOverUi = mouseOverUi; p.play = play;
        input::PublishOwnership(p);
    }
    static void Frame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        input::FeedMouse(io);
        ImGui::NewFrame();
        ImGui::EndFrame();
    }
    static void DrainInto(Case& c) {
        input::RouteEvent buf[input::kRouteCapacity];
        const int n = input::TakeRouted(buf, input::kRouteCapacity);
        for (int i = 0; i < n; ++i) c.events.push_back(buf[i]);
    }
    static void Settle(Case& c) {   // a clean starting point for the next case (not a timed wait)
        input::SetFreeCam(false);
        SetPolicy(false, false, false);
        DrainGame();
        Frame();
        SendFocus(true);
        DrainInto(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // OS/native seam: a real in-process GetCursorPos patch whose saved original is the fixture substitute.
    // ----------------------------------------------------------------------------------------------------
    static std::atomic<int> g_substituteCalls{ 0 };
    static BOOL WINAPI SubstituteGetCursorPos(LPPOINT p) {
        ++g_substituteCalls;
        if (p) { p->x = 0x5EED; p->y = 0x7E57; }
        return TRUE;
    }
    static void* g_patchTarget = nullptr;
    static unsigned char g_patchSaved[16] = {};
    static bool g_patched = false;
    static void RestoreCursorPatch() {
        if (!g_patched) return;
        DWORD old = 0;
        if (VirtualProtect(g_patchTarget, sizeof g_patchSaved, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(g_patchTarget, g_patchSaved, sizeof g_patchSaved);
            DWORD tmp = 0;
            VirtualProtect(g_patchTarget, sizeof g_patchSaved, old, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_patchTarget, sizeof g_patchSaved);
        }
        g_patched = false;
        g_patchTarget = nullptr;
    }

    // ----------------------------------------------------------------------------------------------------
    // GetRawInputData seam for VALID raw payloads (B2a). Same in-process patch class as the cursor seam:
    // a magic handle is answered with a fixture-controlled RAWINPUT mouse packet; every other handle is
    // forwarded to the real API resolved before patching. Production OnRawInput/FeedMouse stay real, and
    // the pre-existing synthetic handle (LPARAM)1 keeps failing closed through the real API.
    // ----------------------------------------------------------------------------------------------------
    static void* g_rawReal = nullptr;
    static void* g_rawPatchTarget = nullptr;
    static unsigned char g_rawPatchSaved[16] = {};
    static bool g_rawPatched = false;
    static USHORT g_rawPacketButtons = 0;
    static const ULONG_PTR kRawMagic = (ULONG_PTR)0x1ED1F1ED;
    typedef UINT (WINAPI* FnGetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    static bool PatchRawInput();
    static void RestoreRawPatch();
    static UINT WINAPI SubstituteGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
        if ((ULONG_PTR)h != kRawMagic || cmd != RID_INPUT || !size || hdr != sizeof(RAWINPUTHEADER)) {
            // Nonmagic fallback WITHOUT recursion: the saved address is the patched body (it jumps back
            // here), so restore the original bytes, call through, and re-arm. This runs only on the
            // window thread that owns all production GetRawInputData callers (OnRawInput/FreeCamRaw),
            // hence no cross-thread unpatch race; the real API never calls back into the substitute.
            RestoreRawPatch();
            const UINT r = ((FnGetRawInputData)g_rawReal)(h, cmd, data, size, hdr);
            PatchRawInput();
            return r;
        }
        if (!data) { *size = sizeof(RAWINPUT); return 0; }
        if (*size < sizeof(RAWINPUT)) return (UINT)-1;
        RAWINPUT pkt = {};
        pkt.header.dwType = RIM_TYPEMOUSE;
        pkt.data.mouse.usButtonFlags = g_rawPacketButtons;
        std::memcpy(data, &pkt, sizeof pkt);
        *size = sizeof(RAWINPUT);
        return sizeof(RAWINPUT);
    }
    static bool PatchRawInput() {
        if (g_rawPatched) return true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        void* target = user32 ? (void*)GetProcAddress(user32, "GetRawInputData") : nullptr;
        if (!target) return false;
        g_rawReal = target;
        std::memcpy(g_rawPatchSaved, target, sizeof g_rawPatchSaved);
        DWORD old = 0;
        if (!VirtualProtect(target, sizeof g_rawPatchSaved, PAGE_EXECUTE_READWRITE, &old)) return false;
        const uint64_t detour = (uint64_t)&SubstituteGetRawInputData;
        unsigned char* p = (unsigned char*)target;
        p[0] = 0x48; p[1] = 0xB8;
        std::memcpy(p + 2, &detour, sizeof detour);
        p[10] = 0xFF; p[11] = 0xE0;
        DWORD tmp = 0;
        VirtualProtect(target, sizeof g_rawPatchSaved, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof g_rawPatchSaved);
        g_rawPatchTarget = target;
        g_rawPatched = true;
        return true;
    }
    static void RestoreRawPatch() {
        if (!g_rawPatched) return;
        DWORD old = 0;
        if (VirtualProtect(g_rawPatchTarget, sizeof g_rawPatchSaved, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(g_rawPatchTarget, g_rawPatchSaved, sizeof g_rawPatchSaved);
            DWORD tmp = 0;
            VirtualProtect(g_rawPatchTarget, sizeof g_rawPatchSaved, old, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_rawPatchTarget, sizeof g_rawPatchSaved);
        }
        g_rawPatched = false;
        g_rawPatchTarget = nullptr;
    }
    static void SendRawValid(USHORT buttons) {
        g_rawPacketButtons = buttons;
        SendMsg(WM_INPUT, RIM_INPUT, (LPARAM)kRawMagic);
    }
    static std::filesystem::path g_evidenceDir;

    // ---- routes.json / cases.json receipts -----------------------------------------------------------------
    struct CaseReceipt { std::string id; bool pass = false; int assertions = 0; std::vector<std::string> failures; };
    static std::vector<CaseReceipt> g_receipts;
    static std::vector<std::pair<std::string, std::vector<input::RouteEvent>>> g_caseEvents;

    static void RunCase(const char* id, void (*fn)(Case&)) {
        Case c;
        c.id = id;
        Log(std::string("CASE: ") + id);
        fn(c);
        CaseReceipt r;
        r.id = c.id; r.pass = c.Pass(); r.assertions = c.assertions; r.failures = c.failures;
        g_receipts.push_back(r);
        // Keep the drained production trace for this case; the receipts carry the case status.
        g_caseEvents.push_back({ c.id, c.events });
        Log(std::string(c.Pass() ? "PASS " : "FAIL ") + id + " assertions=" + Num((unsigned long long)c.assertions));
    }

    // Cases are defined below; forward declarations keep the registry readable.
    static void CaseRoutesPlay(Case& c);
    static void CaseRoutesEdit(Case& c);
    static void CaseRoutesCarried(Case& c);
    static void CaseMenuCloseHeld(Case& c);
    static void CaseMenuCloseQueued(Case& c);
    static void CaseNoLegacyBindings(Case& c);
    static void CaseRoutesCamera(Case& c);
    static void CaseCameraOpenKey(Case& c);
    static void CaseRoutesText(Case& c);
    static void CaseRoutesRaw(Case& c);
    static void CaseRawHeld(Case& c);
    static void CaseRawFallback(Case& c);
    static void CaseStaleSnapshot(Case& c);
    static void CaseReleaseModeSwitch(Case& c);
    static void CaseFocusReset(Case& c);
    static void CaseShortLockRepair(Case& c);
    static void CaseSettingsLegacy(Case& c);
    static void CaseRingOverflow(Case& c);
}

// All core input globals, including g_placing, come from the actual production TU above.
// Each case controls those real values; no duplicate placement-policy mirror exists.

// MinHook seam used by input::Init. The fixture owns the substitute and the patch; no real MinHook is linked.
// A repeat Init (the NOLEGACY sub-case re-inits the router) re-arms the same detour instead of failing.
MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal) {
    if (!pTarget || !pDetour || !ppOriginal) return MH_ERROR_NOT_INITIALIZED;
    if (fx::g_patched && fx::g_patchTarget == pTarget) { *ppOriginal = reinterpret_cast<LPVOID>(&fx::SubstituteGetCursorPos); return MH_OK; }
    if (fx::g_patched) return MH_ERROR_ALREADY_CREATED;
    std::memcpy(fx::g_patchSaved, pTarget, sizeof fx::g_patchSaved);
    DWORD old = 0;
    if (!VirtualProtect(pTarget, sizeof fx::g_patchSaved, PAGE_EXECUTE_READWRITE, &old)) return MH_ERROR_MEMORY_PROTECT;
    const uint64_t detour = reinterpret_cast<uint64_t>(pDetour);
    unsigned char* p = static_cast<unsigned char*>(pTarget);
    p[0] = 0x48; p[1] = 0xB8;                       // mov rax, imm64
    std::memcpy(p + 2, &detour, sizeof detour);
    p[10] = 0xFF; p[11] = 0xE0;                     // jmp rax
    DWORD tmp = 0;
    VirtualProtect(pTarget, sizeof fx::g_patchSaved, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), pTarget, sizeof fx::g_patchSaved);
    *ppOriginal = reinterpret_cast<LPVOID>(&fx::SubstituteGetCursorPos);
    fx::g_patchTarget = pTarget;
    fx::g_patched = true;
    return MH_OK;
}
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_OK; }

namespace fx {
    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-PLAY: menu closed. Keyboard, buttons and the registered raw packet belong to the game; the raw
    // packet is OS cleanup (DefWindowProc), never a false game delivery.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesPlay(Case& c) {
        SetPolicy(false, false, false);
        Frame();
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        const auto own = input::CurrentOwnership();
        DrainGame();
        SendTap('W');
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        SendMsg(WM_LBUTTONUP, 0, 0);
        SendRaw();
        Frame();
        c.Check(!own.menuOpen && !own.mouseToUi && !own.keysToUi, "menu closed snapshot has no WB ownership");
        c.Value(GameCount(WM_KEYDOWN, 'W'), 1, "the game received the key-down");
        c.Value(GameCount(WM_KEYUP, 'W'), 1, "the game received the paired key-up");
        c.Value(GameCount(WM_LBUTTONDOWN, MK_LBUTTON), 1, "the game received the mouse down");
        c.Value(GameCount(WM_LBUTTONUP, 0), 1, "the game received the mouse up");
        c.Value(GameCount(WM_INPUT), 0, "the registered raw packet is OS cleanup, not game delivery");
        c.Check(!input::ScanDown(0x11, false), "scan state cleared by the paired release");
        c.Check(!ImGui::IsKeyDown(ImGuiKey_W) && !ImGui::GetIO().MouseDown[0], "the real backend received no UI input");
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, 'W');
        const auto* ku = c.Last(WM_KEYUP, 'W');
        const auto* raw = c.Last(WM_INPUT, RIM_INPUT);
        c.Check(kd && kd->target == input::RouteGame && kd->recipients == input::RecipientGame && kd->epoch == epoch,
                "key-down routed to the game under its actual epoch");
        c.Check(ku && ku->target == input::RouteGame && ku->recipients == input::RecipientGame, "key-up routed to the game");
        c.Check(raw && raw->target == input::RouteSystem && raw->recipients == input::RecipientSystem,
                "closed-menu raw packet is recorded as system cleanup");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-EDIT: menu open, UI claims mouse and keyboard. Upstream release semantics stay: a key-up is
    // still forwarded to the game.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesEdit(Case& c) {
        SetPolicy(true, true, true);
        Frame();
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        DrainGame();
        SendKey('A', true);
        Frame();
        c.Check(ImGui::IsKeyDown(ImGuiKey_A), "the text-field key-down reached the real backend");
        c.Value(GameCount(WM_KEYDOWN, 'A'), 0, "the game owns no keyboard while the UI captures it");
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the captured widget owns the mouse");
        SendKey('A', false);
        SendMsg(WM_LBUTTONUP, 0, 0);
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_A), "the paired key-up cleared the real backend key state");
        c.Check(!ImGui::GetIO().MouseDown[0] && GetCapture() != g_hwnd, "the paired button-up cleared state and capture");
        c.Value(GameCount(WM_KEYUP, 'A'), 1, "the upstream key-up forwarding to the game is preserved");
        c.Value(GameCount(WM_LBUTTONUP, 0), 0, "the captured button release is not leaked to the game");
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, 'A');
        const auto* ku = c.Last(WM_KEYUP, 'A');
        const auto* mu = c.Last(WM_LBUTTONUP, 0);
        c.Check(kd && kd->target == input::RouteUi && kd->recipients == input::RecipientUi && kd->epoch == epoch,
                "key-down routed to the UI under the published epoch");
        c.Check(ku && ku->target == input::RouteUi && ku->recipients == (input::RecipientUi | input::RecipientGame),
                "key-up exposed both actual recipients (UI and game)");
        c.Check(mu && mu->target == input::RouteUi && mu->recipients == input::RecipientUi, "button-up routed to the UI");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-CARRIED (v0.95 mouse-only placement): while a carried set is active the mouse belongs to the
    // gizmo/HUD (clickable Drop/Cancel) even in play mode, every key stays with the game (no placement-key
    // consumption exists), and a release that belongs to a previous owner is not swallowed by the switch.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesCarried(Case& c) {
        SetPolicy(true, true, false, true);
        Frame();
        const auto own = input::CurrentOwnership();
        const unsigned long long epoch = own.epoch;
        c.Check(own.placing, "the snapshot exposes the carried-set identity");
        DrainGame();
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the carried gizmo owns the mouse for Drop/Cancel clicks");
        c.Value(GameCount(WM_LBUTTONDOWN, MK_LBUTTON), 0, "the game received no mouse down while carrying");
        SendMsg(WM_LBUTTONUP, 0, 0);
        Frame();
        c.Check(!ImGui::GetIO().MouseDown[0] && GetCapture() != g_hwnd, "the paired button-up cleared state and capture");
        c.Value(GameCount(WM_LBUTTONUP, 0), 0, "the captured button release is not leaked to the game");
        // Legacy placement keys must NOT be consumed: arrows and numpad stay with the game while carrying.
        SendTap(VK_LEFT);
        SendTap(VK_NUMPAD8);
        SendTap(VK_RETURN);
        c.Value(GameCount(WM_KEYDOWN, VK_LEFT), 1, "the arrow key reached the game while carrying");
        c.Value(GameCount(WM_KEYUP, VK_LEFT), 1, "the arrow release reached the game while carrying");
        c.Value(GameCount(WM_KEYDOWN, VK_NUMPAD8), 1, "the numpad key reached the game while carrying");
        c.Value(GameCount(WM_KEYUP, VK_NUMPAD8), 1, "the numpad release reached the game while carrying");
        c.Value(GameCount(WM_KEYDOWN, VK_RETURN), 1, "the confirm key reached the game while carrying");
        c.Value(GameCount(WM_KEYUP, VK_RETURN), 1, "the confirm release reached the game while carrying");
        c.Check(!ImGui::IsKeyDown(ImGuiKey_LeftArrow) && !ImGui::IsKeyDown(ImGuiKey_Enter), "the real backend received no carried key input");
        DrainInto(c);
        const auto* md = c.Last(WM_LBUTTONDOWN, MK_LBUTTON);
        const auto* kd = c.Last(WM_KEYDOWN, VK_LEFT);
        c.Check(md && md->target == input::RouteUi && md->recipients == input::RecipientUi && md->epoch == epoch,
                "carried mouse-down routed to the UI under the published epoch");
        c.Check(kd && kd->target == input::RouteGame && kd->recipients == input::RecipientGame && kd->epoch == epoch,
                "carried key-down routed to the game under the same epoch");
        // Adversarial: a key the game holds across the carried switch still gets its release.
        SetPolicy(true, false, false, false);
        DrainGame();
        SendKey('K', true);
        c.Value(GameCount(WM_KEYDOWN, 'K'), 1, "the game holds a key before the carried switch");
        const auto held = input::CurrentOwnership();
        SetPolicy(true, true, false, true);
        SendKey('K', false);
        c.Value(GameCount(WM_KEYUP, 'K'), 1, "the prior game owner received its release across the carried switch");
        DrainInto(c);
        bool sawHeldRelease = false;
        for (const auto& e : c.events)
            if (e.msg == WM_KEYUP && e.wParam == 'K' && e.recipients == input::RecipientGame && e.pressEpoch == held.epoch) sawHeldRelease = true;
        c.Check(sawHeldRelease, "the carried switch did not conceal the prior game release");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-MENU-CLOSE-HELD (B1): a mouse button held by the carried gizmo must meet the real backend on menu
    // close (button-up through the backend handler so MouseButtonsDown clears), and the later physical up
    // must hand off to the game. Direct IO events are not equivalent. Host limit, documented: this session
    // refuses mouse capture even for a shown window (explicit SetCapture probe returned NULL), so the
    // vendor handler's ReleaseCapture branch cannot execute here; the routed handler invocation that
    // contains it, the button-bit clearing, the handoff and the cleanup trace are proven below.
    // ----------------------------------------------------------------------------------------------------
    static void CaseMenuCloseHeld(Case& c) {
        SetPolicy(true, true, false, true);
        Frame();
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        DrainGame();
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the carried gizmo holds the mouse before close");
        c.Value(GameCount(WM_LBUTTONDOWN, MK_LBUTTON), 0, "the game saw no down while carrying");
        input::MenuClosed();                       // overlay close order: release first, then the flags
        SetPolicy(false, false, false);
        Frame();
        c.Check(!ImGui::GetIO().MouseDown[0], "the backend released the button on menu close");
        SendMsg(WM_LBUTTONUP, 0, 0);               // the physical up after close belongs to the game
        Frame();
        c.Value(GameCount(WM_LBUTTONUP, 0), 1, "the physical release reached the game after close");
        c.Check(!ImGui::GetIO().MouseDown[0], "the late physical up did not re-press the backend");
        DrainInto(c);
        const auto* up = c.Last(WM_LBUTTONUP, 0);
        c.Check(up && up->target == input::RouteGame && up->recipients == input::RecipientGame,
                "the post-close release is recorded as plain game delivery");
        bool sawCleanup = false;
        for (const auto& e : c.events)
            if (e.cleanup && e.msg == WM_LBUTTONUP && (e.recipients & input::RecipientUi) && e.pressEpoch == epoch) sawCleanup = true;
        c.Check(sawCleanup, "menu close left an explicit cleanup trace for the held button");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-MENU-CLOSE-QUEUED (B1 r3): the close-before-NewFrame ordering. The down is delivered to the real
    // backend (queued event + ledger recipient) but no ImGui frame runs before MenuClosed, so last-frame
    // IO.MouseDown is still false. Close must dispatch from the delivered ledger recipient, not the stale
    // IO state; the next frame must show no stuck button after the physical up goes to the game.
    // ----------------------------------------------------------------------------------------------------
    static void CaseMenuCloseQueued(Case& c) {
        SetPolicy(true, true, false, true);
        Frame();
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        DrainGame();
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        // No Frame here: the backend queued the down (bit set, event pending) but IO.MouseDown is false.
        c.Check(!ImGui::GetIO().MouseDown[0], "the delivered press is queued, not yet framed");
        c.Value(GameCount(WM_LBUTTONDOWN, MK_LBUTTON), 0, "the game saw no down while carrying");
        input::MenuClosed();                       // overlay close order: release first, then the flags
        SetPolicy(false, false, false);
        SendMsg(WM_LBUTTONUP, 0, 0);               // the physical up after close belongs to the game
        Frame();                                   // queued down, repair up, then nothing left over
        c.Value(GameCount(WM_LBUTTONUP, 0), 1, "the physical release reached the game after close");
        c.Check(!ImGui::GetIO().MouseDown[0], "no stuck button after queued close and handoff");
        DrainInto(c);
        bool sawCleanup = false;
        for (const auto& e : c.events)
            if (e.cleanup && e.msg == WM_LBUTTONUP && (e.recipients & input::RecipientUi) && e.pressEpoch == epoch) sawCleanup = true;
        c.Check(sawCleanup, "the queued close delivered a cleanup release with the press epoch");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-CAMERA: the free camera consumes its keys/mouse (the game sees nothing) while releases keep
    // reaching the game, and the snapshot exposes the camera identity.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesCamera(Case& c) {
        SetPolicy(false, false, false);
        Frame();
        const unsigned long long before = input::CurrentOwnership().epoch;
        input::SetFreeCam(true);
        Frame();
        const auto own = input::CurrentOwnership();
        c.Check(own.camera, "the snapshot exposes the free-camera identity");
        c.Check(own.epoch > before, "entering the camera is a fresh route identity");
        DrainGame();
        SendKey('W', true);
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        c.Value(GameCount(WM_KEYDOWN, 'W'), 0, "the camera key did not reach the game");
        c.Value(GameCount(WM_LBUTTONDOWN, MK_LBUTTON), 0, "the camera took the button down");
        SendMsg(WM_LBUTTONUP, 0, 0);
        SendKey('W', false);
        c.Value(GameCount(WM_LBUTTONUP, 0), 1, "the button release still reached the game");
        c.Value(GameCount(WM_KEYUP, 'W'), 1, "the camera key release still reached the game");
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, 'W');
        const auto* ku = c.Last(WM_KEYUP, 'W');
        c.Check(kd && kd->target == input::RouteCamera && (kd->recipients & input::RecipientCamera) != 0,
                "camera key-down recorded as a camera consumption");
        c.Check(ku && ku->recipients == input::RecipientGame, "the camera key release recorded as game delivery");
        // Stale-identity probe (r3 note): the closed-menu up above must resolve the freecam key slot
        // instead of leaving a dead held identity; a fresh closed pair carries its own press epoch.
        SendKey('W', true);
        SendKey('W', false);
        SendKey('W', true);
        DrainInto(c);
        const auto* kd3 = c.Last(WM_KEYDOWN, 'W');
        SendKey('W', false);
        DrainInto(c);
        const auto* ku3 = c.Last(WM_KEYUP, 'W');
        c.Check(ku3 && ku3->recipients == input::RecipientGame && ku3->pressEpoch != 0,
                "a closed-menu pair resolves the ledger with a press epoch");
        c.Check(kd3 && ku3 && ku3->pressEpoch == kd3->epoch,
                "the resolved pair carries its own down epoch, not a stale one");
        input::SetFreeCam(false);
        c.Check(!input::CurrentOwnership().camera, "leaving the camera clears the identity");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-CAMERA-OPEN-KEY (B2b): with the menu open the free camera sends its keys to the backend, so the
    // paired release (ordinary routing) and focus loss must repair that backend holder through the ledger.
    // The closed-menu camera case above cannot cover this: it has no backend recipient to release.
    // ----------------------------------------------------------------------------------------------------
    static void CaseCameraOpenKey(Case& c) {
        SetPolicy(true, false, false);
        input::SetFreeCam(true);
        Frame();
        c.Check(input::CurrentOwnership().camera, "the snapshot exposes the free-camera identity while open");
        DrainGame();
        SendKey('W', true);
        Frame();
        c.Check(ImGui::IsKeyDown(ImGuiKey_W), "the camera key reached the backend while open");
        c.Value(GameCount(WM_KEYDOWN, 'W'), 0, "the camera key did not reach the game");
        SendFocus(false);                          // focus loss must repair the camera-origin UI press
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_W), "focus loss released the camera-origin backend key");
        DrainInto(c);
        c.Check(c.CountCleanup() >= 1, "the focus repair left an explicit cleanup trace");
        SendFocus(true);
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        SendKey('W', true);
        Frame();
        c.Check(ImGui::IsKeyDown(ImGuiKey_W), "the second camera press reached the backend");
        SendKey('W', false);
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_W), "the paired release cleared the backend key");
        c.Value(GameCount(WM_KEYUP, 'W'), 1, "the paired release still reached the game");
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, 'W');
        const auto* ku = c.Last(WM_KEYUP, 'W');
        c.Check(kd && kd->target == input::RouteCamera && (kd->recipients & input::RecipientCamera) != 0
                && (kd->recipients & input::RecipientUi) != 0,
                "camera key-down recorded with camera and UI recipients");
        c.Check(ku && ku->recipients == (input::RecipientGame | input::RecipientUi),
                "camera key release exposes both game and backend recipients");
        bool sawPressEpoch = false;
        for (const auto& e : c.events)
            if (e.msg == WM_KEYUP && e.wParam == 'W' && e.pressEpoch == epoch && (e.recipients & input::RecipientUi)) sawPressEpoch = true;
        c.Check(sawPressEpoch, "the repaired release carries the original press epoch");
        input::SetFreeCam(false);
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-TEXT: an active text field routes IME composition to the OS/system path (DefWindowProc), and a
    // normal key to the UI. Without the text field the same IME message falls through to the game.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesText(Case& c) {
        SetPolicy(true, true, true, false, true, true);
        Frame();
        const unsigned long long epoch = input::CurrentOwnership().epoch;
        DrainGame();
        SendMsg(WM_IME_COMPOSITION, 0, (LPARAM)GCS_COMPSTR);
        SendKey('A', true);
        SendKey('A', false);
        Frame();
        c.Value(GameCount(WM_IME_COMPOSITION), 0, "the composition message was taken by the system/IME path");
        c.Check(input::CurrentOwnership().textInput, "the text-field identity is published for the routing boundary");
        c.Value(GameCount(WM_KEYDOWN, 'A'), 0, "the text-field key went to the UI, not the game");
        DrainInto(c);
        const auto* ime = c.Last(WM_IME_COMPOSITION, 0);
        const auto* kd = c.Last(WM_KEYDOWN, 'A');
        c.Check(ime && ime->target == input::RouteSystem && ime->recipients == input::RecipientSystem && ime->epoch == epoch,
                "IME composition recorded as the system recipient at the text epoch");
        c.Check(kd && kd->target == input::RouteUi && kd->recipients == input::RecipientUi, "text-field key routed to the UI");
        SetPolicy(true, true, true, false, false, false);
        DrainGame();
        SendMsg(WM_IME_COMPOSITION, 0, (LPARAM)GCS_COMPSTR);
        c.Value(GameCount(WM_IME_COMPOSITION), 1, "without a text field the composition falls through to the game");
        DrainInto(c);
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-ROUTES-RAW: the router's own registration is visible in the snapshot, the closed-menu packet is OS
    // cleanup, and a real OS NOLEGACY registration changes the reported identity.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRoutesRaw(Case& c) {
        SetPolicy(false, false, false);
        Frame();
        const auto own = input::CurrentOwnership();
        c.Check(own.rawRegisteredByRouter, "the snapshot reports the router's own raw-mouse registration");
        c.Check(!own.rawNoLegacy, "a router registration without RIDEV_NOLEGACY reports legacy-capable buttons");
        DrainGame();
        SendRaw();
        c.Value(GameCount(WM_INPUT), 0, "the closed-menu registered packet is OS cleanup, not game delivery");
        DrainInto(c);
        const auto raw = c.Last(WM_INPUT, RIM_INPUT);
        const unsigned long long rawEpoch = raw ? raw->epoch : own.epoch;
        c.Check(raw && raw->target == input::RouteSystem, "registered closed-menu raw packet recorded as system");
        // Real OS NOLEGACY distinction: install a real device the router then observes on re-init.
        input::Shutdown();
        RAWINPUTDEVICE device = { 0x01, 0x02, RIDEV_NOLEGACY, g_hwnd };
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "a real NOLEGACY raw-mouse device was registered");
        input::Init(g_hwnd);
        const auto nolegacy = input::CurrentOwnership();
        c.Check(nolegacy.rawNoLegacy, "the snapshot reports the real RIDEV_NOLEGACY registration");
        c.Check(nolegacy.epoch > rawEpoch, "the registration change is a fresh snapshot identity");
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "the NOLEGACY device was removed again");
        input::Shutdown();
        input::Init(g_hwnd);
        SetPolicy(false, false, false);
        c.Check(!input::CurrentOwnership().rawNoLegacy, "removing the device restores the legacy-capable identity");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-RAW-HELD (B2a): with a real RIDEV_NOLEGACY mouse and a VALID raw packet, a raw down submitted to
    // the UI makes the backend the holder; switching to play (menu still open) must repair that holder on
    // the raw up even though the current policy no longer routes to the UI. FeedMouse used to drop it.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRawHeld(Case& c) {
        c.Check(PatchRawInput(), "the valid-payload raw seam installed");
        input::Shutdown();
        RAWINPUTDEVICE device = { 0x01, 0x02, RIDEV_NOLEGACY, g_hwnd };
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "a real NOLEGACY device was registered");
        input::Init(g_hwnd);
        SetPolicy(true, true, false);
        Frame();
        DrainGame();
        SendRawValid(RI_MOUSE_LEFT_BUTTON_DOWN);
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the valid raw down reached the backend");
        c.Value(GameCount(WM_INPUT), 0, "the UI-owned raw packet did not reach the game");
        SetPolicy(true, false, false, false, false, false, true);   // edit -> play: repair, not drop
        SendRawValid(RI_MOUSE_LEFT_BUTTON_UP);
        c.Value(GameCount(WM_INPUT), 1, "the raw packet itself still reached the game");
        Frame();
        c.Check(!ImGui::GetIO().MouseDown[0], "the raw release repaired the prior backend holder");
        DrainInto(c);
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "the NOLEGACY device was removed again");
        input::Shutdown();
        input::Init(g_hwnd);
        RestoreRawPatch();
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-RAW-FALLBACK: while the valid-payload seam is patched, a NONMAGIC handle must reach the real API
    // (fail closed, no recursion into the substitute) and the magic path must keep working afterwards.
    // A re-entrant substitute would hang here and trip the bounded Job timeout instead of passing.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRawFallback(Case& c) {
        c.Check(PatchRawInput(), "the valid-payload raw seam installed");
        input::Shutdown();
        RAWINPUTDEVICE device = { 0x01, 0x02, RIDEV_NOLEGACY, g_hwnd };
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "a real NOLEGACY device was registered");
        input::Init(g_hwnd);
        SetPolicy(true, true, false);
        Frame();
        DrainGame();
        SendRaw();                               // nonmagic synthetic handle: real API fails closed
        Frame();
        c.Check(!ImGui::GetIO().MouseDown[0], "the nonmagic packet queued no backend press");
        c.Value(GameCount(WM_INPUT), 0, "the UI-owned nonmagic packet did not reach the game");
        SendRawValid(RI_MOUSE_LEFT_BUTTON_DOWN);   // magic path intact after a fallback call
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the magic down still reached the backend");
        SendRawValid(RI_MOUSE_LEFT_BUTTON_UP);
        Frame();
        c.Check(!ImGui::GetIO().MouseDown[0], "the magic release cleared the backend");
        DrainInto(c);
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;
        c.Check(RegisterRawInputDevices(&device, 1, sizeof device) != 0, "the NOLEGACY device was removed again");
        input::Shutdown();
        input::Init(g_hwnd);
        RestoreRawPatch();
        Settle(c);
    }
    // Optional placement OFF retains the legacy passthrough requirement: every placement VK and paired
    // release reaches the game while carrying; no UI claim or invented binding revision.
    // ----------------------------------------------------------------------------------------------------
    static void CaseNoLegacyBindings(Case& c) {
        core::g_keyboardPlacement = false;
        const auto revision = input::CurrentOwnership().bindingRevision;
        SetPolicy(true, true, false, true);
        Frame();
        c.Value(input::CurrentOwnership().bindingRevision, revision, "disabled placement policy does not revise configured bindings");
        c.Check(!input::CurrentOwnership().keyboardPlacement, "optional placement OFF claims no placement recipient");
        DrainGame();
        // Mid-hold proof (not just post-tap up-state): while the keys are physically held the backend must
        // not see them. Shift keeps its old sprint passthrough under the same rule.
        SendKey(VK_LEFT, true);
        SendKey(VK_NUMPAD8, true);
        SendKey(VK_SHIFT, true);
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_LeftArrow), "the held arrow never reached the backend");
        c.Check(!ImGui::IsKeyDown(ImGuiKey_Keypad8), "the held numpad key never reached the backend");
        c.Check(!ImGui::IsKeyDown(ImGuiKey_ModShift), "the held Shift never reached the backend");
        c.Value(GameCount(WM_KEYDOWN, VK_LEFT), 1, "the held arrow reached the game");
        c.Value(GameCount(WM_KEYDOWN, VK_NUMPAD8), 1, "the held numpad key reached the game");
        c.Value(GameCount(WM_KEYDOWN, VK_SHIFT), 1, "the held Shift stayed with the game");
        SendKey(VK_LEFT, false);
        SendKey(VK_NUMPAD8, false);
        SendKey(VK_SHIFT, false);
        c.Value(GameCount(WM_KEYUP, VK_LEFT), 1, "the arrow release reached the game while carrying");
        c.Value(GameCount(WM_KEYUP, VK_NUMPAD8), 1, "the numpad release reached the game while carrying");
        c.Value(GameCount(WM_KEYUP, VK_SHIFT), 1, "the Shift release reached the game while carrying");
        const int legacy[] = { VK_RIGHT, VK_UP, VK_DOWN, VK_NEXT, VK_NUMPAD2, VK_ADD, VK_SUBTRACT, VK_RETURN, VK_BACK, VK_DECIMAL };
        for (int vk : legacy) SendTap(vk);
        for (int vk : legacy) {
            c.Value(GameCount(WM_KEYDOWN, (WPARAM)vk), 1, "legacy VK reached the game while carrying");
            c.Value(GameCount(WM_KEYUP, (WPARAM)vk), 1, "legacy VK release reached the game while carrying");
        }
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_LeftArrow) && !ImGui::IsKeyDown(ImGuiKey_RightArrow)
                && !ImGui::IsKeyDown(ImGuiKey_UpArrow) && !ImGui::IsKeyDown(ImGuiKey_DownArrow)
                && !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_Backspace),
                "the real backend received none of the legacy keys");
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, VK_NUMPAD8);
        c.Check(kd && kd->target == input::RouteGame && kd->recipients == input::RecipientGame,
                "a legacy numpad key-down is recorded as plain game delivery");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-STALE-SNAPSHOT: a sample taken before a route change can never match an event delivered under the
    // later route; the event's epoch is the route actually in effect.
    // ----------------------------------------------------------------------------------------------------
    static void CaseStaleSnapshot(Case& c) {
        SetPolicy(true, true, false);
        Frame();
        const auto stale = input::CurrentOwnership();
        SendKey('K', true);
        DrainInto(c);
        const auto* kd = c.Last(WM_KEYDOWN, 'K');
        c.Check(kd && input::Matches(stale, *kd), "the key-down matches the sample that was in effect");
        SetPolicy(true, false, true);
        const auto now = input::CurrentOwnership();
        c.Check(now.epoch > stale.epoch, "the route change advanced the identity");
        c.Check(now.keysToUi != stale.keysToUi && now.mouseToUi != stale.mouseToUi, "the stale fields no longer describe the route");
        SendKey('K', false);
        DrainInto(c);
        const auto* ku = c.Last(WM_KEYUP, 'K');
        c.Check(ku && ku->epoch == now.epoch, "the later event carries the epoch actually in effect");
        c.Check(ku && input::Matches(now, *ku) && !input::Matches(stale, *ku), "a stale sample is detectable and rejects the event");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-RELEASE (adversarial): an edit -> play switch inside the open menu (no MenuOpened/MenuClosed) must
    // release the keys/buttons the UI was holding to BOTH the prior UI owner and the current game owner.
    // ----------------------------------------------------------------------------------------------------
    static void CaseReleaseModeSwitch(Case& c) {
        SetPolicy(true, true, true);
        Frame();
        const auto edit = input::CurrentOwnership();
        DrainGame();
        SendKey('A', true);
        Frame();
        c.Check(ImGui::IsKeyDown(ImGuiKey_A), "the real UI holds the key before the switch");
        SendMsg(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        Frame();
        c.Check(ImGui::GetIO().MouseDown[0], "the real UI holds the button before the switch");
        SetPolicy(true, false, false, false, false, false, true);   // edit -> play, menu stays open
        const auto play = input::CurrentOwnership();
        c.Check(play.menuOpen, "the menu stayed open (no ClearInputKeys path was taken)");
        c.Check(!play.keysToUi && !play.mouseToUi && play.play, "the route moved to the game while the UI still held the input");
        c.Check(play.epoch > edit.epoch, "the mode switch is a fresh route identity");
        SendKey('A', false);
        SendMsg(WM_LBUTTONUP, 0, 0);
        Frame();
        c.Check(!ImGui::IsKeyDown(ImGuiKey_A), "the prior UI owner received the held key's release after the switch");
        c.Check(!ImGui::GetIO().MouseDown[0] && GetCapture() != g_hwnd, "the prior UI owner received the button release and capture cleared");
        c.Value(GameCount(WM_KEYUP, 'A'), 1, "the same release also reached the current game owner");
        c.Value(GameCount(WM_LBUTTONUP, 0), 1, "the same button release also reached the game owner");
        DrainInto(c);
        const auto* ku = c.Last(WM_KEYUP, 'A');
        const auto* mu = c.Last(WM_LBUTTONUP, 0);
        c.Check(ku && ku->recipients == (input::RecipientUi | input::RecipientGame) && ku->pressEpoch == edit.epoch,
                "the key release exposes both recipients and the original press epoch");
        c.Check(mu && mu->recipients == (input::RecipientUi | input::RecipientGame) && mu->pressEpoch == edit.epoch,
                "the button release exposes both recipients and the original press epoch");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-FOCUS (adversarial): losing focus must release every held input to the recipients that actually have
    // it, then clear the compound state; regaining focus is a fresh identity.
    // ----------------------------------------------------------------------------------------------------
    static void CaseFocusReset(Case& c) {
        SetPolicy(true, true, true);
        Frame();
        SendKey('A', true);                      // the UI holds a key
        Frame();
        SetPolicy(true, false, false, false, false, false, true);
        SendKey(VK_SPACE, true);                 // the game holds a key
        c.Value(GameCount(WM_KEYDOWN, VK_SPACE), 1, "the game holds a key before focus loss");
        const auto before = input::CurrentOwnership();
        DrainGame();
        SendFocus(false);
        Frame();
        const auto after = input::CurrentOwnership();
        c.Check(!after.focused && after.epoch > before.epoch, "focus loss is a fresh, unfocused identity");
        c.Check(!input::ScanDown(0x39, false), "the scan state was cleared on focus loss");
        c.Value(GameCount(WM_KEYUP, VK_SPACE), 1, "the game's held key received a synthetic release");
        c.Check(!ImGui::IsKeyDown(ImGuiKey_A), "the UI's held key received its release");
        DrainInto(c);
        c.Check(c.CountCleanup() >= 2, "both held inputs produced an explicit cleanup trace");
        for (const auto& e : c.events)
            if (e.cleanup) c.Check(e.recipients != 0 && e.pressEpoch < e.epoch, "a cleanup trace carries prior press identity and actual recipients");
        SendFocus(true);
        c.Check(input::CurrentOwnership().focused, "regaining focus restores the focused identity");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-SHORT-LOCK: the cleanup release must be dispatched OUTSIDE the router section. The original procedure
    // signals that a synthetic release arrived and waits (bounded) for a second thread's try-lock verdict.
    // ----------------------------------------------------------------------------------------------------
    static void CaseShortLockRepair(Case& c) {
        SetPolicy(true, false, false, false, false, false, true);
        Frame();
        DrainGame();
        SendKey('W', true);
        c.Value(GameCount(WM_KEYDOWN, 'W'), 1, "the game holds a key before the handshake");
        g_releaseSeen.store(0);
        g_probeAdmitted.store(false);
        g_probeCompletedInCallback.store(false);
        g_probeDone.reset();        // fresh events: subscribed BEFORE the trigger (no wait, no poll)
        g_releaseSeenEvent.reset();
        g_probeArmed.store(true);
        std::thread probe([]() {
            const bool sawRelease = g_releaseSeenEvent.wait(kProbeMs);
            if (sawRelease) {
                input::FrameScope scope(std::try_to_lock);
                g_probeAdmitted.store(scope.OwnsLock());
            }
            g_probeDone.set();
        });
        SendFocus(false);            // the production repair dispatches the synthetic release here
        probe.join();
        g_probeArmed.store(false);
        c.Check(g_releaseSeen.load() >= 1, "the cleanup release actually reached the original window procedure");
        c.Check(g_probeCompletedInCallback.load(), "the probe completed before the cleanup callback returned");
        c.Check(g_probeAdmitted.load(), "the router section was free while the cleanup release was dispatched (short lock)");
        SendFocus(true);
        DrainInto(c);
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-RING-OVERFLOW: the 256-event ring drops the oldest with an explicit counter; partial and zero drains
    // never destroy unread events.
    // ----------------------------------------------------------------------------------------------------
    static void CaseRingOverflow(Case& c) {
        SetPolicy(false, false, false);
        Frame();
        DrainInto(c);
        const unsigned long long dropped0 = input::RoutedDropped();
        for (int i = 0; i < 300; ++i) SendMsg(WM_CHAR, (WPARAM)i, 0);
        DrainInto(c);
        const size_t burst = c.events.size();
        c.Value((unsigned long long)burst, 256, "the burst retained exactly the newest 256 events");
        c.Check(burst == (size_t)256 && c.events.front().wParam == 44 && c.events.back().wParam == 299, "drop-oldest retained the newest window");
        c.Value(input::RoutedDropped(), dropped0 + 44, "the overflow loss is explicitly counted");
        c.Value((unsigned long long)input::TakeRouted(nullptr, 0), 0, "a zero drain consumes nothing");
        SendMsg(WM_CHAR, 1, 0);
        SendMsg(WM_CHAR, 2, 0);
        SendMsg(WM_CHAR, 3, 0);
        input::RouteEvent one = {};
        c.Value((unsigned long long)input::TakeRouted(&one, 1), 1, "a partial drain returns the first event");
        c.Value((unsigned long long)one.wParam, 1, "the partial drain returned the oldest unread event");
        DrainInto(c);
        const size_t tail = c.events.size() - burst;
        c.Value((unsigned long long)tail, 2, "the remaining unread events are preserved in order");
        c.Check(tail == 2 && c.events[burst].wParam == 2 && c.events[burst + 1].wParam == 3, "partial drain preserved order");
        Settle(c);
    }

    // ----------------------------------------------------------------------------------------------------
    // IO-SETTINGS-LEGACY (B3): the REAL production settings boundary. A scratch settings.txt carrying
    // supported optional placement entries plus retired keys goes through production LoadSettings;
    // production SaveSettings writes it back. Machine-field assertions: current bindings (including a
    // numpad VK as toggle) load exactly, supported placement persists, and retired keys are not written. The scratch dir
    // lives under the evidence root; no owner file is touched. No second parser: values are compared as
    // exact key=value lines, never interpreted.
    // ----------------------------------------------------------------------------------------------------
    static void CaseSettingsLegacy(Case& c) {
        std::error_code ec;
        std::filesystem::path dir = g_evidenceDir / "settings";
        std::filesystem::create_directories(dir, ec);
        c.Check(!ec, "a scratch settings directory was created under the evidence root");
        // The input suites share this process: save every global the production load path can mutate.
        const int saveToggle = core::g_keyToggle, saveMode = core::g_keyMode;
        const bool saveKeyboardPlacement = core::g_keyboardPlacement;
        const std::vector<int> savePlaceKeys(core::g_placeKeys, core::g_placeKeys + core::PK_COUNT);
        const bool saveConsole = core::g_showConsole, saveHttp = core::g_httpEnabled;
        const int savePort = core::g_httpPort;
        const float saveFov = core::g_fovDeg;
        const bool saveMirror = core::g_camMirror, saveAuto = core::g_fovAuto;
        const bool saveGimmick = core::g_gimmickSpawn;
        const float saveSpd = core::g_fcSpeed, saveSens = core::g_fcSens;
        const std::string saveMod = core::g_modDir;
        const std::string savePref = i18n::Preference();
        const int saveQ = thumbgen::Quality();
        core::g_modDir = dir.string();
        {
            std::ofstream f(dir / "settings.txt", std::ios::trunc);
            f << "keyboard_placement=1\nkey_toggle=NUMPAD5\nkey_mode=F2\nkey_move_fwd=NUMPAD8\nkey_move_back=NUMPAD2\n"
                 "key_rotate_left=NUMPAD4\nkey_drop=ENTER\nkey_cancel=BACKSPACE\nkey_snap=NUMPAD0\n"
                 "key_pos=F9\ngroundsnap=0\n";
            c.Check(!!f, "the legacy-laden scratch settings file was written");
        }
        core::g_placeKeys[core::PK_FWD] = VK_F8; core::g_placeKeys[core::PK_DROP] = VK_F9;
        core::LoadSettings();
        c.Check(core::g_keyboardPlacement && core::g_placeKeys[core::PK_FWD] == VK_NUMPAD8 &&
                core::g_placeKeys[core::PK_BACK] == VK_NUMPAD2 && core::g_placeKeys[core::PK_ROT_L] == VK_NUMPAD4 &&
                core::g_placeKeys[core::PK_DROP] == VK_RETURN && core::g_placeKeys[core::PK_CANCEL] == VK_BACK &&
                core::g_placeKeys[core::PK_SNAP] == VK_NUMPAD0, "supported U optional placement settings load exact values");
        c.Value((unsigned long long)core::g_keyToggle, (unsigned long long)VK_NUMPAD5,
                "a numpad VK stays legitimate as key_toggle beside legacy entries");
        c.Value((unsigned long long)core::g_keyMode, (unsigned long long)VK_F2,
                "key_mode loads exactly beside legacy entries");
        core::SaveSettings();
        std::vector<std::string> keyLines;
        bool keyboardEnabled = false, retiredKeyWritten = false;
        {
            std::ifstream f(dir / "settings.txt");
            std::string line;
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (line.rfind("key_", 0) == 0) keyLines.push_back(line);
                keyboardEnabled |= line == "keyboard_placement=1";
                retiredKeyWritten |= line.rfind("key_pos=", 0) == 0 || line.rfind("groundsnap=", 0) == 0;
            }
        }
        c.Value((unsigned long long)keyLines.size(), (unsigned long long)core::PK_COUNT + 2,
                "the saved file carries two primary bindings and every supported optional placement binding");
        c.Check(keyboardEnabled && !retiredKeyWritten, "optional placement persists without resurrecting retired settings");
        bool hasToggle = false, hasMode = false;
        for (const auto& l : keyLines) {
            hasToggle |= (l == "key_toggle=NUMPAD5");
            hasMode |= (l == "key_mode=F2");
        }
        c.Check(hasToggle && hasMode, "the saved bindings round-trip with exact machine values");
        c.Check(std::find(keyLines.begin(), keyLines.end(), "key_drop=ENTER") != keyLines.end() &&
                std::find(keyLines.begin(), keyLines.end(), "key_snap=NUMPAD0") != keyLines.end(), "supported placement bindings write canonical machine values");
        core::g_keyToggle = saveToggle; core::g_keyMode = saveMode;
        core::g_keyboardPlacement = saveKeyboardPlacement;
        std::copy(savePlaceKeys.begin(), savePlaceKeys.end(), core::g_placeKeys); core::ApplyPlaceKeys();
        core::g_showConsole = saveConsole; core::g_httpEnabled = saveHttp; core::g_httpPort = savePort;
        core::g_fovDeg = saveFov; core::g_camMirror = saveMirror; core::g_fovAuto = saveAuto;
        core::g_gimmickSpawn = saveGimmick; core::g_fcSpeed = saveSpd; core::g_fcSens = saveSens;
        core::g_modDir = saveMod;
        i18n::SetPreference(savePref.c_str()); thumbgen::SetQuality(saveQ);
        Settle(c);
    }
}

namespace fx {
    static std::vector<std::string> SelectedCases() {
        std::vector<std::string> selected;
        char buf[512] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof buf, "WB_IO_CASES") == 0 && len > 0) {
            std::string list(buf), item;
            for (char ch : list + ",") {
                if (ch == ',') { if (!item.empty()) selected.push_back(item); item.clear(); }
                else if (ch != ' ' && ch != '\t') item.push_back(ch);
            }
        }
        return selected;
    }
    static bool Wanted(const std::vector<std::string>& selected, const char* id) {
        if (selected.empty()) return true;
        for (const std::string& s : selected) if (s == id) return true;
        return false;
    }
    static void WriteEvidence(const std::filesystem::path& dir, int exitCode) {
        if (dir.empty()) return;
        int assertions = 0;
        int failed = 0;
        for (const CaseReceipt& r : g_receipts) { assertions += r.assertions; if (!r.pass) ++failed; }
        {
            std::ofstream log(dir / "input_ownership.log");
            std::lock_guard<std::mutex> l(g_logMutex);
            for (const std::string& line : g_log) log << line << "\n";
        }
        std::ofstream routes(dir / "routes.json");
        routes << "{\"schema\":\"wb079.input-ownership.routes.v1\",\"target\":\"production input.cpp WndProc on a real hidden HWND\",\"cases\":[";
        for (size_t i = 0; i < g_caseEvents.size(); ++i) {
            const auto& entry = g_caseEvents[i];
            if (i) routes << ",";
            routes << "{\"case\":\"" << entry.first << "\",\"events\":[";
            for (size_t j = 0; j < entry.second.size(); ++j) {
                const auto& e = entry.second[j];
                if (j) routes << ",";
                routes << "{\"epoch\":" << e.epoch << ",\"msg\":" << e.msg << ",\"wparam\":" << (unsigned long long)e.wParam
                       << ",\"target\":" << (int)e.target << ",\"recipients\":" << e.recipients << ",\"cleanup\":" << (e.cleanup ? "true" : "false")
                       << ",\"pressEpoch\":" << e.pressEpoch << ",\"rawButtons\":" << (unsigned)e.rawButtons << ",\"rawTag\":" << e.rawTag << "}";
            }
            routes << "]}";
        }
        routes << "],\"exitCode\":" << exitCode << ",\"assertions\":" << assertions << "}\n";
        std::ofstream cases(dir / "cases.json");
        cases << "{\n  \"suite\": \"InputOwnership\",\n  \"mode\": \"HostDeterministicWindow\",\n";
        cases << "  \"status\": \"" << (failed == 0 && exitCode == 0 ? "PASS" : "FAIL") << "\",\n";
        cases << "  \"assertions\": " << assertions << ",\n";
        cases << "  \"cases\": [";
        for (size_t i = 0; i < g_receipts.size(); ++i) {
            const CaseReceipt& r = g_receipts[i];
            if (i) cases << ", ";
            cases << "{\"id\": \"" << r.id << "\", \"status\": \"" << (r.pass ? "PASS" : "FAIL") << "\", \"assertions\": " << r.assertions << ", \"failures\": [";
            for (size_t j = 0; j < r.failures.size(); ++j) { if (j) cases << ", "; cases << "\"" << r.failures[j] << "\""; }
            cases << "]}";
        }
        cases << "],\n";
        cases << "  \"realRegions\": [\"production input.cpp WndProc route branches, latch, raw repair and menu-close release\", \"real imgui_impl_win32 backend (ImGuiIO is the actual UI recipient)\", \"real hidden HWND on a window-owner thread with real SendMessageW\", \"real OS raw-mouse registration incl. RIDEV_NOLEGACY with valid raw payloads\", \"production cdmodkit.cpp settings load/save boundary\"],\n";
        cases << "  \"substitutedRegions\": [\"MinHook install (real in-process GetCursorPos patch + fixture substitute original)\", \"GetRawInputData valid-payload patch for a magic handle (other handles reach the real API)\", \"core settings globals driven per case (saved/restored)\", \"i18n/thumbgen settings collaborators (record-only)\", \"synthetic WM_INPUT handle (GetRawInputData fails closed)\"],\n";
        cases << "  \"failures\": [";
        bool first = true;
        for (const CaseReceipt& r : g_receipts) for (const std::string& f : r.failures) { cases << (first ? "" : ", "); first = false; cases << "\"" << r.id << ": " << f << "\""; }
        cases << "]\n}\n";
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::filesystem::path evidence;
    std::error_code ec;
    if (argc > 1) {
        evidence = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(evidence, ec);
    }
    int exitCode = 0;
    std::thread owner;
    bool contextCreated = false, backendInit = false, inputInit = false;
    try {
        fx::Log("InputOwnership host suite: production input.cpp + real imgui_impl_win32 + real hidden HWND, event-barrier cases");
        fx::g_evidenceDir = evidence;   // the settings case stages its scratch mod dir here
        core::g_console = true;         // production Log mirrors to stdout (same TU); g_log stays untouched
        ImGui::CreateContext();
        contextCreated = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1920, 1080);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int texW = 0, texH = 0;
        io.Fonts->AddFontDefault();
        io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
        owner = std::thread(fx::OwnerThread);
        if (!fx::g_ownerReady.wait(fx::kMustArriveMs)) throw std::runtime_error("the window-owner thread did not become ready");
        if (!fx::g_hwnd) throw std::runtime_error("the hidden window could not be created");
        backendInit = ImGui_ImplWin32_Init(fx::g_hwnd);
        if (!backendInit) throw std::runtime_error("the real imgui_impl_win32 backend could not initialize");
        input::Init(fx::g_hwnd);
        inputInit = true;
        fx::SendFocus(true);
        fx::Frame();

        const std::vector<std::string> selected = fx::SelectedCases();
        struct Row { const char* id; void (*fn)(fx::Case&); };
        const Row rows[] = {
            { "IO-ROUTES-PLAY", fx::CaseRoutesPlay },
            { "IO-ROUTES-EDIT", fx::CaseRoutesEdit },
            { "IO-ROUTES-CARRIED", fx::CaseRoutesCarried },
            { "IO-MENU-CLOSE-HELD", fx::CaseMenuCloseHeld },
            { "IO-MENU-CLOSE-QUEUED", fx::CaseMenuCloseQueued },
            { "IO-ROUTES-CAMERA", fx::CaseRoutesCamera },
            { "IO-CAMERA-OPEN-KEY", fx::CaseCameraOpenKey },
            { "IO-ROUTES-TEXT", fx::CaseRoutesText },
            { "IO-ROUTES-RAW", fx::CaseRoutesRaw },
            { "IO-RAW-HELD", fx::CaseRawHeld },
            { "IO-RAW-FALLBACK", fx::CaseRawFallback },
            { "IO-NO-LEGACY-BINDINGS", fx::CaseNoLegacyBindings },
            { "IO-STALE-SNAPSHOT", fx::CaseStaleSnapshot },
            { "IO-RELEASE", fx::CaseReleaseModeSwitch },
            { "IO-FOCUS", fx::CaseFocusReset },
            { "IO-SHORT-LOCK", fx::CaseShortLockRepair },
            { "IO-SETTINGS-LEGACY", fx::CaseSettingsLegacy },
            { "IO-RING-OVERFLOW", fx::CaseRingOverflow },
        };
        for (const Row& row : rows) {
            if (!fx::Wanted(selected, row.id)) continue;
            fx::RunCase(row.id, row.fn);
        }
    } catch (const std::exception& e) {
        fx::Log(std::string("FATAL: ") + e.what());
        exitCode = 1;
    }
    try {
        if (inputInit) input::Shutdown();
        fx::RestoreRawPatch();
        fx::RestoreCursorPatch();
        if (backendInit) ImGui_ImplWin32_Shutdown();
        if (fx::g_hwnd) { PostMessageW(fx::g_hwnd, WM_CLOSE, 0, 0); fx::g_hwnd = nullptr; }
        if (owner.joinable()) owner.join();
        if (contextCreated) ImGui::DestroyContext();
    } catch (const std::exception& e) {
        fx::Log(std::string("FATAL(teardown): ") + e.what());
        exitCode = 1;
    }
    for (const fx::CaseReceipt& r : fx::g_receipts) if (!r.pass) exitCode = 1;
    int assertions = 0;
    for (const fx::CaseReceipt& r : fx::g_receipts) assertions += r.assertions;
    fx::WriteEvidence(evidence, exitCode);
    std::cout << "ASSERTIONS=" << assertions << "\n";
    return exitCode;
}
