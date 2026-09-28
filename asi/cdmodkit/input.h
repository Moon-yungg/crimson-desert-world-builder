// Window subclass + virtual cursor for the overlay (adapted from master-looter / Trinity, MIT).
#pragma once
#include <windows.h>
#include <mutex>
#include <string>
struct ImGuiIO;
namespace input {
    void Init(HWND hwnd);
    void Shutdown();
    void MenuOpened();          // centres the virtual cursor
    void MenuClosed();
    void FeedMouse(ImGuiIO& io);   // render thread, once per frame while the menu is open
    void TakeMouseDelta(float* dx, float* dy); // consumes raw relative motion accumulated since the previous frame
    // Serializes the router state, the WndProc and the added overlay frame. It is the same re-entrant critical
    // section the router uses (a CRITICAL_SECTION may be entered again by its owning thread), so an inner
    // acquisition by the same thread always succeeds. The producer side (TrackKey), the focus/reset paths and the
    // HotkeyPressed consumer take short acquisitions of this same section, so the compound latch/scan/focus state
    // has exactly one boundary between the window-owner thread and the present thread. The optional
    // `std::try_to_lock` form instead reports
    // failure while another thread owns it: the added WB frame is skipped, and the cursor detour delegates
    // without reading any router state, rather than blocking the present thread on a scope another owner holds.
    // OwnsLock() reports whether THIS scope acquired the section; the destructor releases only its own
    // acquisition, exactly once, never another scope's recursion level.
    struct FrameScope {
        FrameScope();
        explicit FrameScope(std::try_to_lock_t);
        ~FrameScope();
        bool OwnsLock() const noexcept { return owns; }
        void Release() noexcept;   // gives up this scope's own acquisition early (never another scope's)
        FrameScope(const FrameScope&) = delete; FrameScope& operator=(const FrameScope&) = delete;
    private:
        bool owns;
    };
    // key state by scan code, tracked from the window messages; ext = extended key flag
    bool ScanDown(int scan, bool ext);
    bool ScanDownAny(int scan);
    void ClearKeys();
    bool VkDown(int vk);           // virtual-key state; numpad bindings also accept their navigation-key variants
    void SetPlaceVks(const int* vks, int count);
    // Configured keys (core::g_keyToggle/g_keyMode) are event-first while the real WndProc is live: a bounded
    // pending latch recorded from the window messages is consumed once per tap, never from a sampled edge. Other
    // VKs keep the asynchronous rising-edge policy this function replaces. wasDown mirrors the event Down state.
    bool HotkeyPressed(int vk, bool& wasDown);
    // Independent admission counters (atomics). Diagnostics/evidence only: no routing decision reads them.
    struct InputAdmission {
        unsigned long long cursorCalls = 0;      // hkGetCursorPos entries
        unsigned long long cursorAdmitted = 0;   // try-lock admission succeeded
        unsigned long long cursorBypassed = 0;   // try-lock failed: original delegated without reading router state
        unsigned long long hotkeyConsumed = 0;   // pending hotkey actions consumed
    };
    InputAdmission AdmissionStats();
    // ---------------------------------------------------------------------------------------------------
    // Ownership observation. The added WB frame publishes the routing policy
    // it installed; the real WndProc stamps every routed message with the terminal recipients it actually
    // delivered to and with the epoch of the snapshot it routed under. Events enter the ring only after
    // the terminal callbacks return (including focus cleanup), never while delivery is still in flight.
    // This is bookkeeping/evidence only: the
    // policy fields mirror exactly the core:: flags the upstream router reads, so no decision here is a second
    // router. An immutable snapshot (a copy) lets an indicator/consumer pair its sample with the events
    // delivered under it via Matches(); the bounded 256-event ring drops the oldest event with an explicit
    // cumulative counter. Everything below is read/written under the same recursive g_cs as the router state.
    // ---------------------------------------------------------------------------------------------------
    enum Route { RouteGame = 0, RouteUi = 1, RouteBlocked = 2, RouteCamera = 3, RouteSystem = 4 };
    enum Recipient : unsigned {
        RecipientGame = 1,        // the original (game) window procedure received it
        RecipientUi = 2,          // the ImGui Win32 backend received it (or the virtual cursor consumed it)
        RecipientSystem = 4,      // DefWindowProc/the OS handled it (raw-packet cleanup, IME, cursor)
        RecipientCamera = 8,      // the WB free camera consumed it (the game saw nothing)
        RecipientPlacement = 16   // optional placement consumed the scan state; no game/backend message dispatch
    };
    struct Ownership {
        unsigned long long epoch = 0;   // routing/focus identity; a change here is never reused for a later route
        bool menuOpen = false;          // World Builder owns the routing at all (editor open or placement active)
        bool play = false;              // the editor is in play mode (menu open, input belongs to the game)
        bool camera = false;            // free camera active: its keys/mouse go to World Builder
        bool placing = false;           // a carried set is active
        bool keyboardPlacement = false; // optional placement keys enabled for that carried set
        bool focused = true;            // actual focus messages, not a capture prediction
        bool mouseToUi = false;
        bool keysToUi = false;
        bool textInput = false;         // a text field is active (IME/system path)
        bool mouseOverUi = false;       // the cursor is over a World Builder window
        unsigned long long bindingRevision = 0;
        bool rawNoLegacy = false;       // the registered raw mouse device has RIDEV_NOLEGACY
        bool rawRegisteredByRouter = false;   // the router itself registered the raw mouse device
    };
    struct OwnershipPolicy {   // the frame's route decision for the next delivered messages
        bool menuOpen = false, mouseToUi = false, keysToUi = false, placing = false;
        bool textInput = false, mouseOverUi = false, play = false;
    };
    struct RouteEvent {
        unsigned long long epoch = 0; UINT msg = 0; WPARAM wParam = 0; Route target = RouteGame;
        unsigned recipients = 0; bool cleanup = false; unsigned long long pressEpoch = 0;
        USHORT rawButtons = 0; ULONG rawTag = 0;
    };
    static const int kRouteCapacity = 256;
    Ownership CurrentOwnership();                 // the snapshot in effect at the routing boundary (immutable copy)
    void PublishOwnership(const OwnershipPolicy& policy);   // the frame installs the route decision
    int TakeRouted(RouteEvent* out, int max);     // single consumer, oldest first, preserves unread entries
    unsigned long long RoutedDropped();           // cumulative drop-oldest count for the bounded ring
    bool Matches(const Ownership& sampled, const RouteEvent& event);   // rejects stale indicator samples
    void SetFreeCam(bool on);      // free-fly camera: WASD/QE/Shift/Ctrl/Space and the look mouse go to World Builder
    bool FreeCamLooking();         // the mouse currently turns the free camera (menu closed, or right button held over the world)
    void TakeLookDelta(float* dx, float* dy);   // raw mouse movement collected for the free camera since the last call
    std::string ImeComposition();  // current IME pre-edit/composition text (e.g. pinyin), UTF-8; empty when not composing
}
