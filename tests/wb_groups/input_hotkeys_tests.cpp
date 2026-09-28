// input_hotkeys_tests.cpp - InputHotkeys host suite (plan wb079-unified-77f68967, Task 2 / D1 compound input sync).
//
// WHAT RUNS FOR REAL HERE
//   * asi/cdmodkit/input.cpp as a linked translation unit: the production WndProc subclass, the TrackKey latch,
//     the HotkeyPressed consumer, the ClearKeys/Init/Shutdown reset paths and the hkGetCursorPos detour.
//   * tools/imgui/backends/imgui_impl_win32.cpp: the real backend, initialized on the hidden HWND, so the
//     production WndProc's UI keyboard path reaches the actual backend (IH-BACKEND-ROUTE asserts io.KeysData).
//   * A real hidden HWND created on the fixture's window-owner thread; key messages are dispatched with real
//     SendMessageW, so the window procedure runs on its owner thread exactly like a game's message thread.
//
// WHAT THE FIXTURE SUBSTITUTES (test-owned OS/native-hook seam; never a reimplementation of asserted logic)
//   * MinHook: MH_CreateHook installs a real in-process absolute-jump patch at GetCursorPos (VirtualProtect +
//     FlushInstructionCache) and hands a fixture substitute back as the saved original; MH_EnableHook is a no-op.
//     The original bytes are restored before exit. The latch/scan/consume policy itself is production input.cpp.
//   * core::Log and the core:: input globals input.cpp reads.
//
// HOW THE OVERLAP IS SCHEDULED (explicit barriers/events, no sleeps, no timing luck)
//   Every overlap case simulates the Present frame admission with the production FrameScope(std::try_to_lock)
//   pattern used by overlay.cpp's OverlayFrame while another thread owns the same recursive CRITICAL_SECTION, and
//   the window-owner thread dispatches a real key message. The producer's completion is detected by an event the
//   fixture sets from its original WndProc, which production input.cpp reaches only after TrackKey returned. With
//   an unlocked producer the message completes while the section is owned (IH-*-SERIALIZED fails and the owning
//   frame can consume the tap in-hold); with a short producer scope around the compound state the message cannot
//   complete until the owner releases. The test-owned native lock wrapper signals an actual failed try-acquire
//   on the producer thread before delegating to the real blocking lock. Once that exact event arrives, the
//   owning frame checks completion/state and releases; no elapsed no-progress window is an oracle.
//
// HONEST LIMITS
//   * Host-only: this suite proves the production input layer's threading contract on a real hidden window; it is
//     not a game-live result and it installs no real MinHook hook library.
//   * WB_IH_CASES selects a comma-separated case list (diagnostics/contract harness); an empty list runs all.
#define NOMINMAX
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
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../tools/imgui/imgui.h"
#include "../../tools/imgui/backends/imgui_impl_win32.h"
#include "../../tools/minhook/include/MinHook.h"
#include "../../asi/cdmodkit/core.h"
#include "../../asi/cdmodkit/input.h"
#include "input_admission_seam.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")

namespace fx {
    // ----------------------------------------------------------------------------------------------------
    // Evidence + assertion accumulation. Failures are collected (never thrown) so one broken schedule cannot
    // hide the state of the others; the fixture exits nonzero if anything was recorded.
    // ----------------------------------------------------------------------------------------------------
    static std::mutex g_logMutex;
    static std::vector<std::string> g_log;
    static std::atomic<int> g_assertions{ 0 };
    static std::mutex g_failMutex;
    static std::vector<std::string> g_failures;

    static std::string Fmt(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        return std::string(buf);
    }
    static void Log(const std::string& line) {
        {
            std::lock_guard<std::mutex> l(g_logMutex);
            g_log.push_back(line);
        }
        std::cout << line << "\n";
    }
    static void Check(const char* caseId, bool ok, const std::string& label) {
        g_assertions.fetch_add(1, std::memory_order_relaxed);
        if (ok) return;
        {
            std::lock_guard<std::mutex> l(g_failMutex);
            g_failures.push_back(std::string(caseId) + ": " + label);
        }
        Log(std::string("FAIL ") + caseId + ": " + label);
    }
    static void CheckValue(const char* caseId, unsigned long long actual, unsigned long long expected, const std::string& what) {
        Check(caseId, actual == expected, Fmt("%s (expected %llu, got %llu)", what.c_str(), expected, actual));
    }

    // ----------------------------------------------------------------------------------------------------
    // Events. Only bounded must-arrive handshakes; no negative timing windows or sleeps.
    // ----------------------------------------------------------------------------------------------------
    struct Event {
        HANDLE h = nullptr;
        Event() { h = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
        ~Event() { if (h) CloseHandle(h); }
        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;
        void set() const { if (h) SetEvent(h); }
        void reset() const { if (h) ResetEvent(h); }
        bool wait(unsigned ms) const { return h && WaitForSingleObject(h, ms) == WAIT_OBJECT_0; }
        bool signalled() const { return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0; }
    };
    static const unsigned kMustArriveMs = 10000;   // ordinary barrier deadline

    static bool WaitArrives(const char* caseId, const Event& e, const std::string& what, unsigned ms = kMustArriveMs) {
        const bool ok = e.wait(ms);
        Check(caseId, ok, "barrier did not arrive within the bounded deadline: " + what);
        return ok;
    }

    // ----------------------------------------------------------------------------------------------------
    // Window-owner side: hidden window, original WndProc recording/seams, on-demand send script.
    // ----------------------------------------------------------------------------------------------------
    static const UINT kGoSend = WM_APP + 0x21;     // driver -> owner: run the pending send script
    static const UINT kMarker = WM_APP + 0x22;     // driver -> owner: external (non-input) callback probe
    static HWND g_hwnd = nullptr;
    static Event g_ownerReady;
    static Event g_sendStarted;                    // set by the owner before the first scripted send
    static Event g_ownerDone;                      // set by the owner after the last scripted send returned
    static Event g_watched;                        // set when a watched message reaches the original WndProc
    static Event g_marker;                         // set when the marker message reaches the original WndProc
    static std::atomic<UINT> g_watchMsg{ 0 };
    static std::atomic<WPARAM> g_watchVk{ 0 };
    static std::atomic<unsigned> g_originalSeen{ 0 };
    static std::atomic<unsigned> g_keydownSeen{ 0 };
    static std::atomic<unsigned> g_killFocusSeen{ 0 };
    static std::atomic<unsigned> g_markerSeen{ 0 };
    struct SendItem { UINT msg; WPARAM wparam; LPARAM lparam; };
    static std::mutex g_scriptMutex;
    static std::vector<SendItem> g_script;
    static void ArmWatch(UINT msg, WPARAM vk) {
        g_watched.reset();
        g_watchMsg.store(msg);
        g_watchVk.store(vk);
    }
    static void PostScript(const std::vector<SendItem>& items) {
        {
            std::lock_guard<std::mutex> l(g_scriptMutex);
            g_script = items;
        }
        g_sendStarted.reset();
        g_ownerDone.reset();
        PostMessageW(g_hwnd, kGoSend, 0, 0);
    }
    static LRESULT CALLBACK FixtureWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }        // the creating thread owns destruction
        if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
        if (msg == kGoSend) {
            std::vector<SendItem> items;
            {
                std::lock_guard<std::mutex> l(g_scriptMutex);
                items.swap(g_script);
            }
            g_sendStarted.set();
            for (const SendItem& it : items) SendMessageW(hwnd, it.msg, it.wparam, it.lparam);   // own window: direct dispatch on this thread
            g_ownerDone.set();
            return 0;
        }
        if (msg == kMarker) {
            ++g_markerSeen;
            g_marker.set();
            return 0;
        }
        ++g_originalSeen;
        if (msg == WM_KEYDOWN) ++g_keydownSeen;
        if (msg == WM_KILLFOCUS) ++g_killFocusSeen;
        if (msg == g_watchMsg.load(std::memory_order_relaxed) && (g_watchVk.load(std::memory_order_relaxed) == 0 || wparam == g_watchVk.load(std::memory_order_relaxed))) g_watched.set();
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    static DWORD g_ownerTid = 0;
    static void OwnerThread() {
        g_ownerTid = GetCurrentThreadId();
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = FixtureWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"wb079-input-hotkeys";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { g_ownerReady.set(); return; }
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"wb079-input-hotkeys", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
        g_ownerReady.set();
        if (!g_hwnd) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

    // Message helpers. Key messages go through the real window procedure of the real hidden window.
    static LPARAM KeyLparam(int vk, bool ext, bool repeat) {
        const int scan = (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
        LPARAM lp = (LPARAM)1 | (((LPARAM)scan & 0xFF) << 16);
        if (ext) lp |= ((LPARAM)1 << 24);
        if (repeat) lp |= ((LPARAM)1 << 30);
        return lp;
    }
    static void SendMsg(UINT msg, WPARAM wparam, LPARAM lparam) {
        if (g_hwnd) SendMessageW(g_hwnd, msg, wparam, lparam);
    }
    static void SendKey(int vk, bool down, bool ext) { SendMsg(down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, KeyLparam(vk, ext, false)); }
    static void SendRepeatKey(int vk, bool ext) { SendMsg(WM_KEYDOWN, (WPARAM)vk, KeyLparam(vk, ext, true)); }
    static void SendSysKey(int vk, bool down, bool ext) { SendMsg(down ? WM_SYSKEYDOWN : WM_SYSKEYUP, (WPARAM)vk, KeyLparam(vk, ext, false)); }
    static void SendTap(int vk, bool ext) { SendKey(vk, true, ext); SendKey(vk, false, ext); }
    static void SendFocus(bool gained) { SendMsg(gained ? WM_SETFOCUS : WM_KILLFOCUS, (WPARAM)g_hwnd, 0); }
    static SendItem Item(int vk, bool down, bool ext) { return SendItem{ (UINT)(down ? WM_KEYDOWN : WM_KEYUP), (WPARAM)vk, KeyLparam(vk, ext, false) }; }

    // ----------------------------------------------------------------------------------------------------
    // GetCursorPos OS seam: a real in-process patch with a fixture substitute as the saved original.
    // ----------------------------------------------------------------------------------------------------
    static std::atomic<int> g_substituteCalls{ 0 };
    static std::atomic<bool> g_probeArmed{ false };
    static Event g_originalEntered;
    static Event g_probeDone;
    static std::atomic<bool> g_probeAdmitted{ false };
    static std::atomic<bool> g_probeCompletedInOriginal{ false };
    static BOOL WINAPI SubstituteGetCursorPos(LPPOINT p) {
        ++g_substituteCalls;
        if (p) { p->x = 0x5EED; p->y = 0x7E57; }
        if (g_probeArmed.exchange(false)) {   // IH-CURSOR-RELEASE: probe the lock while the saved original runs
            g_originalEntered.set();
            g_probeCompletedInOriginal.store(g_probeDone.wait(kMustArriveMs));
        }
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
}

// MinHook seam used by input::Init. The fixture owns the substitute and the patch; no real MinHook is linked.
MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal) {
    if (!pTarget || !pDetour || !ppOriginal) return MH_ERROR_NOT_INITIALIZED;
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

// core symbols the production input.cpp references. The fixture never defines latch/consume policy.
namespace core {
    void Log(const char* fmt, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fx::Log(std::string("[core] ") + buf);
    }
    bool g_menuOpen = false, g_uiWantsMouse = false, g_uiWantsKeyboard = false, g_placing = false;
    bool g_uiTextInput = false, g_uiMouseOverUi = false;
    bool g_keyboardPlacement = false; // actual U default; optional placement is tested separately when enabled
    int g_keyToggle = VK_INSERT, g_keyMode = VK_HOME;
}

namespace fx {
    using Stats = input::InputAdmission;

    // An admission attempt from a thread that does not own the section: the production overlay pattern
    // (FrameScope(std::try_to_lock) -> skipped frame whole). Returns true when the admission was skipped.
    static bool TryAdmissionSkipped(const char* id, const std::string& label) {
        bool skipped = false;
        std::thread probe([&skipped]() {
            input::FrameScope scope(std::try_to_lock);
            skipped = !scope.OwnsLock();
        });
        probe.join();
        fx::Check(id, skipped, label);
        return skipped;
    }

    // ----------------------------------------------------------------------------------------------------
    // Cases.
    // ----------------------------------------------------------------------------------------------------
    static void CasePreInit() {
        const char* id = "IH-PRE-INIT";
        fx::Log("CASE: IH-PRE-INIT: before input::Init the window owner has no event source");
        const unsigned seen0 = fx::g_keydownSeen.load();
        fx::SendTap(VK_INSERT, true);
        bool wasDown = false;
        const bool pressed = input::HotkeyPressed(VK_INSERT, wasDown);
        fx::Check(id, !pressed, "no synthesized press before the real WndProc is installed");
        fx::Check(id, !wasDown, "wasDown mirrors the empty event state before init");
        fx::Check(id, fx::g_keydownSeen.load() == seen0 + 1, "the fixture window procedure (not production input.cpp) saw the pre-init keydown");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, 0, "no hotkey was consumed before init");
    }

    static void CaseInitLive() {
        const char* id = "IH-INIT-LIVE";
        fx::Log("CASE: IH-INIT-LIVE: after Init/SETFOCUS a fresh tap is latched and consumed exactly once");
        input::Init(fx::g_hwnd);
        fx::SendFocus(true);
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::ArmWatch(WM_KEYDOWN, VK_INSERT);
        fx::SendTap(VK_INSERT, true);
        fx::Check(id, fx::g_watched.signalled(), "the keydown reached the original WndProc after the production subclass");
        bool wd1 = false, wd2 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd1), "one tap produces one configured press");
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "the tap is consumed exactly once");
        fx::Check(id, wd1 == false && wd2 == false, "wasDown mirrors the event Down state cleared by the tap keyup");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "consumed counter advanced once");
    }

    static void CaseRepeatHeld() {
        const char* id = "IH-REPEAT-HELD";
        fx::Log("CASE: IH-REPEAT-HELD: auto-repeat and held-down keydowns are not fresh presses");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::SendKey(VK_INSERT, true, true);
        bool wd = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd), "the fresh keydown is consumed once");
        fx::SendRepeatKey(VK_INSERT, true);
        bool wd2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "an auto-repeat keydown (bit 30) adds no press");
        fx::SendKey(VK_INSERT, true, true);   // bit 30 clear, but the key is still down
        bool wd3 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd3), "a second down without keyup is not a new press");
        fx::Check(id, wd2 && wd3, "wasDown stays down while the key is held");
        fx::SendKey(VK_INSERT, false, true);
        fx::SendKey(VK_INSERT, true, true);
        bool wd4 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd4), "after keyup a new down is a fresh press");
        fx::SendKey(VK_INSERT, false, true);
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 2, "exactly two presses consumed");
    }

    static void CaseBurstSaturation() {
        const char* id = "IH-BURST-SATURATION";
        fx::Log("CASE: IH-BURST-SATURATION: several taps before one consume saturate to a single action");
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        for (int i = 0; i < 3; ++i) fx::SendTap(VK_INSERT, true);
        bool wd = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd), "a burst still yields exactly one action");
        bool wd2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "the saturated latch is empty afterwards");
        fx::SendTap(VK_INSERT, true);
        bool wd3 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd3), "a later tap is a fresh action again");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 2, "two consumes for four taps");
    }

    static void CaseSystemKey() {
        const char* id = "IH-SYSTEM-KEY";
        fx::Log("CASE: IH-SYSTEM-KEY: WM_SYSKEYDOWN feeds the same latch and a repeat adds nothing");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_MENU;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::SendSysKey(VK_MENU, true, false);
        fx::SendRepeatKey(VK_MENU, false);
        bool wd = false;
        fx::Check(id, input::HotkeyPressed(VK_MENU, wd), "a system key is a configured action");
        bool wd2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_MENU, wd2), "the system key tap is consumed once");
        fx::SendSysKey(VK_MENU, false, false);
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "one system-key consume");
        core::g_keyMode = VK_HOME;
    }

    static void CaseShareSequence() {
        const char* id = "IH-SHARE-SEQUENCE";
        fx::Log("CASE: IH-SHARE-SEQUENCE: one VK carrying both actions shares exactly the immediately following query");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_INSERT;
        fx::SendTap(VK_INSERT, true);
        bool w1 = false, w2 = false, w3 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, w1), "the shared VK consumes the tap once");
        fx::Check(id, input::HotkeyPressed(VK_INSERT, w2), "the immediately following identical query shares that consume");
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w3), "no third query shares the same consume");
        fx::SendTap(VK_INSERT, true);
        bool i0 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, i0), "a new tap consumes again");
        bool x0 = false;
        fx::Check(id, !input::HotkeyPressed(VK_F8, x0), "an unrelated query does not share (and is not configured)");
        bool w4 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w4), "an unrelated query breaks the immediately-following share");
        // Distinct VKs: a repeated query for the same key is a second, separate action and stays false.
        core::g_keyMode = VK_HOME;
        fx::SendTap(VK_INSERT, true);
        bool d1 = false, d2 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, d1), "distinct VKs: the first configured query consumes");
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, d2), "distinct VKs: a repeated query never shares");
        fx::SendTap(VK_HOME, true);
        bool d3 = false, d4 = false;
        fx::Check(id, input::HotkeyPressed(VK_HOME, d3), "the second configured VK consumes its own tap");
        fx::Check(id, !input::HotkeyPressed(VK_HOME, d4), "and only once");
    }

    static void CaseRebindReset() {
        const char* id = "IH-REBIND-RESET";
        fx::Log("CASE: IH-REBIND-RESET: a rebind does not resurrect stale latch/sharing state through the reset entry");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::SendTap(VK_INSERT, true);              // pending latch for the old VK
        core::g_keyToggle = VK_F9;                 // the editor's keyCombo writes the bound VK
        bool w1 = false, w2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w1), "the old VK is no longer an event source");
        fx::Check(id, !input::HotkeyPressed(VK_F9, w2), "the new VK has no stale pending");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0, "the rebind consumed nothing");
        input::ClearKeys();                        // production reset entry used by the editor's input-mode switches
        core::g_keyToggle = VK_INSERT;
        bool w3 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w3), "the stale pending was dropped, no phantom press");
        fx::SendTap(VK_INSERT, true);
        bool w4 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, w4), "a fresh tap after the reset works");
        // Sharing state is cleared by the same reset.
        core::g_keyMode = VK_INSERT;
        fx::SendTap(VK_INSERT, true);
        bool s1 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, s1), "shared VK consumes");
        input::ClearKeys();
        bool s2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, s2), "ClearKeys cleared the paired-query sharing state");
        core::g_keyMode = VK_HOME;
    }

    static void CaseFocusResetSequential() {
        const char* id = "IH-FOCUS-RESET";
        fx::Log("CASE: IH-FOCUS-RESET: focus loss discards Pending/Down and the scan state, and a late keyup is inert");
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::SendKey(VK_INSERT, true, true);
        fx::Check(id, input::ScanDown(0x52, true), "the scan state tracks the held configured key");
        fx::SendFocus(false);
        bool w1 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w1), "the press did not survive the focus gap");
        fx::Check(id, !w1, "wasDown is cleared with the Down bit");
        fx::Check(id, !input::ScanDown(0x52, true), "focus loss clears the scan state");
        fx::SendKey(VK_INSERT, false, true);       // the late keyup must not create anything
        bool w2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, w2), "a keyup after focus loss is inert");
        fx::SendFocus(true);
        fx::SendKey(VK_INSERT, true, true);
        fx::SendKey(VK_INSERT, false, true);
        bool w3 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, w3), "after focus returns a fresh tap is one action");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "one consume across the focus cycle");
    }

    // Overlap schedule: the driver thread owns the input section (as another owner would), the window-owner thread
    // runs a scripted send, and the simulated Present admission is attempted from a non-owning thread.
    static void HeldOverlapSchedule(const char* id, const std::vector<fx::SendItem>& script, UINT watchMsg, WPARAM watchVk,
                                    const std::function<void()>& afterOwnerDone) {
        fx::ArmWatch(watchMsg, watchVk);
        input::FrameScope held(std::try_to_lock);
        fx::Check(id, held.OwnsLock(), "the driver owns the input section for the overlap");
        if (!held.OwnsLock()) return;
        TryAdmissionSkipped(id, "a simulated Present frame admission fails while another owner holds the section");
        input_host::ArmContention(fx::g_ownerTid);
        fx::PostScript(script);
        fx::WaitArrives(id, fx::g_sendStarted, "the window owner started the scripted send");
        fx::Check(id, input_host::WaitContention(fx::kMustArriveMs), "the producer reached the actually contended router lock");
        const bool detected = fx::g_watched.signalled();
        fx::Check(id, !detected, "the key message did not complete while another thread owned the section (the producer must be inside the shared boundary)");
        held.Release();
        fx::WaitArrives(id, fx::g_ownerDone, "the scripted sends completed after the release");
        input_host::DisarmContention();
        afterOwnerDone();
    }

    static void CaseOverlapConsume() {
        const char* id = "IH-OVERLAP-CONSUME";
        fx::Log("CASE: IH-OVERLAP-CONSUME: a keydown can never complete inside a frame that owns the section");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        fx::ArmWatch(WM_KEYDOWN, VK_INSERT);
        struct Out { bool admitted = false, detected = false, inHold = false, post = false; } out;
        fx::Event held;
        std::thread consumer([&out, &held, id]() {
            input::FrameScope frame(std::try_to_lock);
            out.admitted = frame.OwnsLock();
            fx::Check(id, out.admitted, "the simulated Present frame owns the input section");
            if (!out.admitted) return;
            input_host::ArmContention(fx::g_ownerTid);
            held.set();
            fx::Check(id, input_host::WaitContention(fx::kMustArriveMs), "the key producer attempted the held native lock");
            out.detected = fx::g_watched.signalled();
            bool wd = false;
            out.inHold = input::HotkeyPressed(VK_INSERT, wd);          // recursive: this frame queries while holding
            fx::Check(id, !out.inHold, "a tap that arrives during the frame is not visible to that same frame");
            frame.Release();
            fx::WaitArrives(id, fx::g_watched, "the keydown reached the original WndProc after the release");
            bool wd1 = false;
            out.post = input::HotkeyPressed(VK_INSERT, wd1);
            bool wd2 = false;
            fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "the tap is consumed exactly once after the release");
        });
        fx::WaitArrives(id, held, "the consumer admitted the frame");
        fx::SendKey(VK_INSERT, true, true);        // window-owner thread dispatches this while the frame owns the section
        consumer.join();
        input_host::DisarmContention();
        fx::Check(id, !out.detected, "the producer's key message was serialized behind the held frame section");
        fx::Check(id, out.post, "the tap survives the interrupted frame and is applied afterwards");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "exactly one consume for the interleaved tap");
        fx::SendKey(VK_INSERT, false, true);
        fx::SendTap(VK_INSERT, true);
        bool wd3 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd3), "a later tap still consumes once");
    }

    static void CaseSkipTap() {
        const char* id = "IH-SKIP-TAP-SURVIVES";
        fx::Log("CASE: IH-SKIP-TAP-SURVIVES: a tap during skipped admission survives and is consumed once by the next admitted frame");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        const std::vector<fx::SendItem> script = { fx::Item(VK_INSERT, true, true), fx::Item(VK_INSERT, false, true) };
        HeldOverlapSchedule(id, script, WM_KEYDOWN, VK_INSERT, [id]() {
            input::FrameScope admitted(std::try_to_lock);
            fx::Check(id, admitted.OwnsLock(), "the next frame admission succeeds after the release");
            bool wd1 = false, wd2 = false;
            fx::Check(id, input::HotkeyPressed(VK_INSERT, wd1), "the tap from the skipped frame is consumed once");
            fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "and not twice");
            fx::Check(id, wd1 == false && wd2 == false, "wasDown mirrors the event Down state after the completed tap");
        });
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "one consume for the skipped-frame tap");
    }

    static void CaseConsecutiveSkips() {
        const char* id = "IH-CONSECUTIVE-SKIPS";
        fx::Log("CASE: IH-CONSECUTIVE-SKIPS: two taps over consecutive skipped admissions saturate to one preserved latch");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        const std::vector<fx::SendItem> script = {
            fx::Item(VK_INSERT, true, true), fx::Item(VK_INSERT, false, true),
            fx::Item(VK_INSERT, true, true), fx::Item(VK_INSERT, false, true),
        };
        fx::ArmWatch(WM_KEYDOWN, VK_INSERT);
        input::FrameScope held(std::try_to_lock);
        fx::Check(id, held.OwnsLock(), "the driver owns the input section for the double skip");
        if (held.OwnsLock()) {
            TryAdmissionSkipped(id, "first admission is skipped");
            TryAdmissionSkipped(id, "second admission is skipped");
            input_host::ArmContention(fx::g_ownerTid);
            fx::PostScript(script);
            fx::WaitArrives(id, fx::g_sendStarted, "the window owner started the two taps");
            fx::Check(id, input_host::WaitContention(fx::kMustArriveMs), "the two-tap producer reached the held native lock");
            const bool detected = fx::g_watched.signalled();
            fx::Check(id, !detected, "the taps did not complete while the section was held");
            held.Release();
            fx::WaitArrives(id, fx::g_ownerDone, "both taps completed after the release");
            input_host::DisarmContention();
        }
        input::FrameScope admitted(std::try_to_lock);
        fx::Check(id, admitted.OwnsLock(), "the next frame admission succeeds");
        bool wd1 = false, wd2 = false;
        fx::Check(id, input::HotkeyPressed(VK_INSERT, wd1), "the saturated latch yields exactly one action");
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "the second tap did not become a second action");
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "two taps, one consume");
    }

    static void CaseFocusResetWhileHeld() {
        const char* id = "IH-FOCUS-RESET-HELD";
        fx::Log("CASE: IH-FOCUS-RESET-HELD: focus loss is serialized with a held frame and clears the latch it waited behind");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        const unsigned long long consumed0 = input::AdmissionStats().hotkeyConsumed;
        const std::vector<fx::SendItem> script = {
            fx::Item(VK_INSERT, true, true),
            fx::SendItem{ WM_KILLFOCUS, (WPARAM)fx::g_hwnd, 0 },
        };
        HeldOverlapSchedule(id, script, WM_KILLFOCUS, 0, [id]() {
            bool wd = false;
            fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd), "the reset cleared the pending press");
            fx::Check(id, !input::ScanDown(0x52, true), "the reset cleared the scan state");
            fx::SendFocus(true);
            fx::SendTap(VK_INSERT, true);
            bool wd2 = false;
            fx::Check(id, input::HotkeyPressed(VK_INSERT, wd2), "a fresh press after focus returns is one action");
        });
        fx::CheckValue(id, input::AdmissionStats().hotkeyConsumed, consumed0 + 1, "only the post-focus tap was consumed");
    }

    static void CaseExternalUnderHold() {
        const char* id = "IH-EXTERNAL-UNDER-HOLD";
        fx::Log("CASE: IH-EXTERNAL-UNDER-HOLD: non-input callbacks reach the game while the section is held (no whole-WndProc lock)");
        fx::g_marker.reset();
        struct Out { bool admitted = false, markerWhileHeld = false; } out;
        fx::Event held;
        std::thread consumer([&out, &held, id]() {
            input::FrameScope frame(std::try_to_lock);
            out.admitted = frame.OwnsLock();
            fx::Check(id, out.admitted, "the frame owns the section");
            if (!out.admitted) return;
            held.set();
            out.markerWhileHeld = fx::g_marker.wait(fx::kMustArriveMs);
            frame.Release();
        });
        fx::WaitArrives(id, held, "the consumer admitted the frame");
        fx::SendMsg(fx::kMarker, 0, 0);            // the game-side callback must not need the input section
        consumer.join();
        fx::Check(id, out.markerWhileHeld, "the external callback was delivered while the frame owned the section");
        fx::Check(id, fx::g_markerSeen.load() > 0, "the fixture window procedure saw the external callback");
    }

    static void CaseBackendRoute() {
        const char* id = "IH-BACKEND-ROUTE";
        fx::Log("CASE: IH-BACKEND-ROUTE: the production WndProc UI keyboard path reaches the real imgui_impl_win32 backend");
        core::g_menuOpen = true;
        core::g_uiWantsKeyboard = true;
        const unsigned seen0 = fx::g_keydownSeen.load();
        fx::SendKey(VK_INSERT, true, true);
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        const bool backendDown = ImGui::IsKeyDown(ImGuiKey_Insert);
        ImGui::EndFrame();
        fx::Check(id, backendDown, "the real backend recorded the UI-owned keydown (io.KeysData)");
        fx::Check(id, fx::g_keydownSeen.load() == seen0, "the UI-owned keydown was not forwarded to the game");
        fx::SendKey(VK_INSERT, false, true);
        fx::Check(id, fx::g_keydownSeen.load() == seen0, "the keyup was forwarded without a second keydown reaching the game");
        core::g_uiWantsKeyboard = false;
        core::g_menuOpen = false;
        bool wd = false;
        input::HotkeyPressed(VK_INSERT, wd);      // drain whatever the UI path latched
    }

    static void CaseCursorVirtual() {
        const char* id = "IH-CURSOR-VIRTUAL";
        fx::Log("CASE: IH-CURSOR-VIRTUAL: an admitted cursor detour on the render thread answers with the virtual cursor");
        const Stats before = input::AdmissionStats();
        const int calls0 = fx::g_substituteCalls.load();
        core::g_menuOpen = true;
        input::MenuOpened();
        fx::Check(id, fx::g_substituteCalls.load() == calls0 + 1, "windowed menu opening samples the saved OS cursor once");
        fx::SendMsg(WM_MOUSEMOVE, 0, MAKELPARAM(23, 47)); // U corrects the virtual cursor from real client coordinates
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        input::FeedMouse(io);                      // sets the production render-thread id to this thread
        const int beforeQuery = fx::g_substituteCalls.load();
        POINT expect = {23, 47};
        ClientToScreen(fx::g_hwnd, &expect);
        POINT got = {};
        const BOOL ok = ::GetCursorPos(&got);
        fx::Check(id, ok == TRUE && got.x == expect.x && got.y == expect.y, fx::Fmt("the admitted detour returned the virtual cursor (got %ld,%ld want %ld,%ld)", (long)got.x, (long)got.y, (long)expect.x, (long)expect.y));
        fx::Check(id, fx::g_substituteCalls.load() == beforeQuery, "the virtual-cursor branch did not call the saved original");
        const Stats after = input::AdmissionStats();
        fx::CheckValue(id, after.cursorAdmitted - before.cursorAdmitted, 1, "one admitted cursor call");
        fx::CheckValue(id, after.cursorBypassed - before.cursorBypassed, 0, "no bypass while the section was free");
    }

    static void CaseCursorBypass() {
        const char* id = "IH-CURSOR-BYPASS";
        fx::Log("CASE: IH-CURSOR-BYPASS: a foreign owner makes the cursor detour delegate the original without reading router state");
        const Stats before = input::AdmissionStats();
        const int calls0 = fx::g_substituteCalls.load();
        struct Out { bool admitted = false; } out;
        fx::Event held, releaseNow;
        std::thread consumer([&out, &held, &releaseNow, id]() {
            input::FrameScope frame(std::try_to_lock);
            out.admitted = frame.OwnsLock();
            fx::Check(id, out.admitted, "the foreign owner holds the input section");
            held.set();
            fx::Check(id, releaseNow.wait(fx::kMustArriveMs), "cursor bypass owner received its release event");
        });
        fx::WaitArrives(id, held, "the foreign owner admitted the frame");
        POINT got = {};
        const BOOL ok = ::GetCursorPos(&got);
        releaseNow.set();
        consumer.join();
        fx::Check(id, ok == TRUE && got.x == 0x5EED && got.y == 0x7E57, "the bypass delegated the saved original exactly with its own value");
        fx::Check(id, fx::g_substituteCalls.load() == calls0 + 1, "the saved original ran exactly once");
        const Stats after = input::AdmissionStats();
        fx::CheckValue(id, after.cursorBypassed - before.cursorBypassed, 1, "one bypassed cursor call");
        fx::CheckValue(id, after.cursorAdmitted - before.cursorAdmitted, 0, "no admission while another thread owned the section");
    }

    static void CaseCursorReleaseBeforeOriginal() {
        const char* id = "IH-CURSOR-RELEASE";
        fx::Log("CASE: IH-CURSOR-RELEASE: a non-virtual admitted detour releases its own acquisition before the original runs");
        const Stats before = input::AdmissionStats();
        const int calls0 = fx::g_substituteCalls.load();
        fx::g_probeAdmitted.store(false);
        fx::g_probeCompletedInOriginal.store(false);
        fx::g_probeDone.reset();
        fx::g_originalEntered.reset();
        std::thread prober([id]() {
            fx::WaitArrives(id, fx::g_originalEntered, "the saved original started");
            input::FrameScope probe(std::try_to_lock);
            fx::g_probeAdmitted.store(probe.OwnsLock());
            probe.Release();
            fx::g_probeDone.set();
        });
        fx::g_probeArmed.store(true);
        std::thread caller([]() {
            POINT got = {};
            ::GetCursorPos(&got);
        });
        caller.join();
        prober.join();
        fx::Check(id, fx::g_probeCompletedInOriginal.load(), "the probe completed before the saved original returned");
        fx::Check(id, fx::g_probeAdmitted.load(), "the section was free while the saved original ran");
        fx::Check(id, fx::g_substituteCalls.load() == calls0 + 1, "the original ran once for the non-virtual admission");
        const Stats after = input::AdmissionStats();
        fx::CheckValue(id, after.cursorAdmitted - before.cursorAdmitted, 1, "the call was admitted");
    }

    static void CasePostShutdown() {
        const char* id = "IH-POST-SHUTDOWN";
        fx::Log("CASE: IH-POST-SHUTDOWN: Shutdown clears the latch and the configured keys report no action");
        core::g_keyToggle = VK_INSERT; core::g_keyMode = VK_HOME;
        core::g_menuOpen = false;
        input::MenuClosed();
        fx::SendTap(VK_INSERT, true);              // drain anything left over
        bool wd0 = false;
        input::HotkeyPressed(VK_INSERT, wd0);
        fx::SendTap(VK_INSERT, true);              // now leave one pending
        input::Shutdown();
        bool wd1 = false, wd2 = false;
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd1), "the pending latch did not survive Shutdown");
        const unsigned seen0 = fx::g_keydownSeen.load();
        fx::SendTap(VK_INSERT, true);
        fx::Check(id, fx::g_keydownSeen.load() == seen0 + 1, "after Shutdown the game's own window procedure receives the key directly");
        fx::Check(id, !input::HotkeyPressed(VK_INSERT, wd2), "after Shutdown the configured keys report no action at all");
    }

    typedef void (*CaseFn)();
    struct CaseDef { const char* id; CaseFn fn; };
    static const CaseDef kCases[] = {
        { "IH-PRE-INIT", CasePreInit },
        { "IH-INIT-LIVE", CaseInitLive },
        { "IH-REPEAT-HELD", CaseRepeatHeld },
        { "IH-BURST-SATURATION", CaseBurstSaturation },
        { "IH-SYSTEM-KEY", CaseSystemKey },
        { "IH-SHARE-SEQUENCE", CaseShareSequence },
        { "IH-REBIND-RESET", CaseRebindReset },
        { "IH-FOCUS-RESET", CaseFocusResetSequential },
        { "IH-OVERLAP-CONSUME", CaseOverlapConsume },
        { "IH-SKIP-TAP-SURVIVES", CaseSkipTap },
        { "IH-CONSECUTIVE-SKIPS", CaseConsecutiveSkips },
        { "IH-FOCUS-RESET-HELD", CaseFocusResetWhileHeld },
        { "IH-EXTERNAL-UNDER-HOLD", CaseExternalUnderHold },
        { "IH-BACKEND-ROUTE", CaseBackendRoute },
        { "IH-CURSOR-VIRTUAL", CaseCursorVirtual },
        { "IH-CURSOR-BYPASS", CaseCursorBypass },
        { "IH-CURSOR-RELEASE", CaseCursorReleaseBeforeOriginal },
        { "IH-POST-SHUTDOWN", CasePostShutdown },
    };

    static std::vector<std::string> SelectedCases() {
        std::vector<std::string> selected;
        char buf[512] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof buf, "WB_IH_CASES") == 0 && len > 0) {
            std::string list(buf), item;
            for (char c : list + ",") {
                if (c == ',') { if (!item.empty()) selected.push_back(item); item.clear(); }
                else if (c != ' ' && c != '\t') item.push_back(c);
            }
        }
        return selected;
    }

    static void WriteEvidence(const std::filesystem::path& dir, int exitCode) {
        if (dir.empty()) return;
        std::vector<std::string> failures;
        {
            std::lock_guard<std::mutex> l(fx::g_failMutex);
            failures = fx::g_failures;
        }
        {
            std::ofstream log(dir / "input_hotkeys.log");
            std::lock_guard<std::mutex> l(fx::g_logMutex);
            for (const std::string& line : fx::g_log) log << line << "\n";
            for (const std::string& f : failures) log << "FAILURE: " << f << "\n";
        }
        std::ofstream cases(dir / "cases.json");
        cases << "{\n  \"suite\": \"InputHotkeys\",\n  \"mode\": \"HostDeterministicWindow\",\n";
        cases << "  \"status\": \"" << (failures.empty() && exitCode == 0 ? "PASS" : "FAIL") << "\",\n";
        cases << "  \"assertions\": " << fx::g_assertions.load() << ",\n";
        cases << "  \"realRegions\": [\"production input.cpp WndProc/TrackKey/HotkeyPressed\", \"real imgui_impl_win32 backend on a hidden HWND\", \"real window-owner thread SendMessage dispatch\", \"admission FrameScope try-lock used by overlay.cpp\"],\n";
        cases << "  \"substitutedRegions\": [\"MinHook install (real in-process GetCursorPos patch + fixture substitute original)\", \"core::Log and core:: input globals\"],\n";
        cases << "  \"failures\": [";
        for (size_t i = 0; i < failures.size(); ++i) { cases << (i ? ", " : "") << "\"" << failures[i] << "\""; }
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
    bool contextCreated = false;
    bool backendInit = false;
    bool inputInit = false;
    try {
        fx::Log("InputHotkeys host suite: real input.cpp + real imgui_impl_win32 + hidden HWND, deterministic overlap schedules");
        ImGui::CreateContext();
        contextCreated = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920, 1080);
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int texW = 0, texH = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
        owner = std::thread(fx::OwnerThread);
        if (!fx::g_ownerReady.wait(fx::kMustArriveMs)) throw std::runtime_error("the window-owner thread did not become ready");
        if (!fx::g_hwnd) throw std::runtime_error("the hidden window could not be created");
        backendInit = ImGui_ImplWin32_Init(fx::g_hwnd);
        if (!backendInit) throw std::runtime_error("the real imgui_impl_win32 backend could not initialize");

        std::vector<std::string> selected = fx::SelectedCases();
        const bool all = selected.empty();
        for (const fx::CaseDef& c : fx::kCases) {
            bool want = all;
            for (const std::string& s : selected) if (s == c.id) want = true;
            if (!want) continue;
            const std::string cid(c.id);
            if (cid == "IH-PRE-INIT") { c.fn(); continue; }
            if (!inputInit) {
                input::Init(fx::g_hwnd);
                fx::SendFocus(true);
                inputInit = true;
            }
            c.fn();
        }
    } catch (const std::exception& e) {
        fx::Log(std::string("FATAL: ") + e.what());
        exitCode = 1;
    }
    // Teardown mirrors the L0 host seam: restore the window procedure before backend/context destruction.
    try {
        if (inputInit) input::Shutdown();
        fx::RestoreCursorPatch();
        if (backendInit) ImGui_ImplWin32_Shutdown();
        if (fx::g_hwnd) {
            PostMessageW(fx::g_hwnd, WM_CLOSE, 0, 0);   // the window-owner thread destroys its own window and quits its loop
            fx::g_hwnd = nullptr;
        }
        if (owner.joinable()) owner.join();
        if (contextCreated) ImGui::DestroyContext();
    } catch (const std::exception& e) {
        fx::Log(std::string("FATAL(teardown): ") + e.what());
        exitCode = 1;
    }
    {
        std::lock_guard<std::mutex> l(fx::g_failMutex);
        if (!fx::g_failures.empty()) exitCode = 1;
    }
    fx::WriteEvidence(evidence, exitCode);
    std::cout << "ASSERTIONS=" << fx::g_assertions.load() << "\n";
    return exitCode;
}
