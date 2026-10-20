// Travel through the game's own fast travel: the client's ClientSequencerStageManager "reload stage at transform" shows the
// loading screen, sends TrocTrReloadStageStartReq to the (in-process) server, which stores the destination and applies it
// once the client's loading starts (notes/FORMATS.md, "THE GAME'S TELEPORT"). Unlike writing the player position, this streams
// the world around the destination, so any distance works - and tiles that were edited stream again (terrain "apply").
//
// The manager is fetched fresh for every travel the way the game's own fast travel does it (caller rva 0x655110 on 2976):
// world root +0xC8 -> ClientField, ClientField vslot 9 = getStageManager. No memory scan: up to v0.97 a 35-50 s scan for the
// manager's vtable ran on first use and its result was cached, but the game replaces the manager on loads, so the cached
// pointer went stale and later scans found nothing while the status still said "ready" (notes/FORMATS.md, "Travel: stage manager without a memory scan").
// Found at startup: the reload function by signature, manager and field classes through RTTI, the getter slot by its prologue.
// The key / b / c arguments default to the values a real fast travel passes (3 / 1 / 0); a fast travel of the player's own
// replaces them with what the game used, and its manager is the fallback when the getter is not found.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace core {

typedef void(__fastcall* StageReloadFn)(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf);
typedef uintptr_t(__fastcall* GetStageMgrFn)(uintptr_t field);
static StageReloadFn g_origStage = nullptr; static uintptr_t g_stageFn = 0, g_mgrVt = 0, g_fieldVt = 0;
static GetStageMgrFn g_getMgr = nullptr; static int g_getSlot = -1;
static unsigned g_fieldOff = 0xC8;              // world root -> ClientField holder; re-found in the root when a patch moves it
static std::atomic<uintptr_t> g_captured{ 0 };  // the manager of the player's own last fast travel (fallback only)
static std::atomic<uint32_t> g_key{ 3 }, g_argB{ 1 }, g_argC{ 0 };
static std::atomic<bool> g_prepared{ false }, g_probeQueued{ false };
static std::atomic<DWORD> g_lastProbe{ 0 };
static std::mutex g_tmx; static std::string g_status = "not installed";
static bool g_travelOk = false;

static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(g_tmx); g_status = s; }
std::string TravelStatus() { std::lock_guard<std::mutex> l(g_tmx); return g_status; }
bool TravelAvailable() { return g_travelOk; }

static bool VtIs(uintptr_t obj, uintptr_t vt) { uintptr_t v = 0; return obj && vt && ReadBytes(obj, &v, 8) && v == vt; }
static bool MgrValid(uintptr_t m) { return VtIs(m, g_mgrVt); }

// ClientField::getStageManager (rva 0x9a5be0 on 2976): reads its field id at +0x180 and asks the global actor table.
static bool IsStageMgrGetter(uintptr_t fn) {
    static const int pat[] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x6C,0x24,0x18, 0x56, 0x57, 0x41,0x56, 0x48,0x83,0xEC,0x70, 0x33,0xFF,
                               0x44,0x8B,0x81,-1,-1,-1,-1, 0x48,0x8D,0x54,0x24,0x48, 0x48,0x8B,0x0D };
    uint8_t b[sizeof pat / sizeof pat[0]];
    if (!fn || !ReadBytes(fn, b, sizeof b)) return false;
    for (size_t i = 0; i < sizeof b; i++) if (pat[i] >= 0 && b[i] != (uint8_t)pat[i]) return false;
    return true;
}

// A fast travel of the player's own: remember the manager and the arguments the game uses right now.
static void __fastcall HookStage(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf) {
    if (MgrValid(mgr)) { g_captured = mgr; g_key = key; g_argB = b; g_argC = c; }
    std::array<float, 10> transform{};
    if (!ReadBytes((uintptr_t)tf, transform.data(), sizeof transform)) { Log("[travel] native stage transform unreadable; reload refused"); return; }
    if (PlayModeActive()) {   // play mode: every stage reload of the game's own is logged; one during the first load is redirected
        Log("[travel] game stage reload: key %u b %u c %u to (%.1f %.1f %.1f)", key, b, c, transform[7], transform[8], transform[9]);
        PlayModeStageOverride(transform.data());
    }
    RunGroundWorldChange([mgr, key, b, c, transform]() {
        if (!MgrValid(mgr)) { Log("[travel] native stage manager lost while waiting for grounding; reload refused"); return; }
        g_origStage(mgr, key, b, c, transform.data());
    }); // normally synchronous; an Applying ground member defers the copied request to a game tick
}

void TravelInstall() {
    g_stageFn = SigScanUnique("44 89 4C 24 20 44 89 44 24 18 89 54 24 10 55 53 56 57 41 56 48 8D AC 24 10 FF FF FF 48 81 EC F0 01 00 00 48 8B F1 48 8D 54 24 48 48 8B 0D");
    g_mgrVt = VtableByName(".?AVClientSequencerStageManager@pa@@");
    if (!g_stageFn || !g_mgrVt) { SetStatus(!g_stageFn ? "stage reload function not found" : "stage manager class not found"); Log("[travel] disabled: %s", TravelStatus().c_str()); return; }
    // the getter: slot 9 on 2976, any other slot of the field's vtable with the same prologue after a patch
    g_fieldVt = VtableByName(".?AVClientField@pa@@");
    for (int pass = 0; pass < 2 && g_fieldVt && g_getSlot < 0; pass++)
        for (int s = pass ? 0 : 9; s < (pass ? 64 : 10); s++) {
            uintptr_t fn = 0; if (!ReadBytes(g_fieldVt + (uintptr_t)s * 8, &fn, 8) || !IsStageMgrGetter(fn)) continue;
            g_getMgr = (GetStageMgrFn)fn; g_getSlot = s; break;
        }
    if (!InstallInternalHook((void*)g_stageFn, (void*)HookStage, (void**)&g_origStage, "stage reload (travel)")) { SetStatus("hook failed"); return; }
    g_travelOk = true;
    SetStatus(g_getMgr ? "waiting for the game world" : "stage manager getter not found (use one fast travel of the game, then try again)");
    if (g_getMgr) Log("[travel] installed: stage reload rva 0x%llx, manager vtable rva 0x%llx, ClientField getter slot %d rva 0x%llx",
        (unsigned long long)(g_stageFn - g_base), (unsigned long long)(g_mgrVt - g_base), g_getSlot, (unsigned long long)((uintptr_t)g_getMgr - g_base));
    else Log("[travel] installed without the manager getter (field vtable %s): only after a fast travel of the game's own",
        g_fieldVt ? "found, no slot matched" : "not found");
}

static uintptr_t CallGetterGuarded(uintptr_t field) {
    uintptr_t m = 0;
    CDK_GUARD_BEGIN
        m = g_getMgr(field);
    CDK_GUARD_FAIL m = 0;
    CDK_GUARD_END
    return m;
}

// Game thread only (the getter takes references in the game's actor table). Returns the live manager or 0 with 'why' set.
static uintptr_t ResolveManager(const char** why) {
    *why = "the game world is not loaded";
    const uintptr_t root = WorldRoot(); if (!root) return 0;
    uintptr_t field = 0, p = 0;
    if (ReadBytes(root + g_fieldOff, &p, 8) && ReadBytes(p, &field, 8) && !VtIs(field, g_fieldVt)) field = 0;
    if (!field && g_fieldVt) for (unsigned off = 0; off < 0x400; off += 8) {   // the holder moved in a patch: 128 reads, not a scan of memory
        uintptr_t f = 0; if (!ReadBytes(root + off, &p, 8) || !ReadBytes(p, &f, 8) || !VtIs(f, g_fieldVt)) continue;
        Log("[travel] ClientField holder moved: root +0x%X (was +0x%X)", off, g_fieldOff); g_fieldOff = off; field = f; break;
    }
    if (field && g_getMgr) {
        const uintptr_t m = CallGetterGuarded(field);
        if (MgrValid(m)) return m;
        *why = m ? "the field returned no stage manager" : "no stage manager yet (loading?)";
    } else if (!g_getMgr) *why = "stage manager getter not found";
    const uintptr_t c = g_captured;   // fallback: what the game's own last fast travel used, if it still lives
    if (MgrValid(c)) return c;
    if (!g_getMgr) *why = "stage manager getter not found (use one fast travel of the game, then try again)";
    return 0;
}

bool TravelPrepared() { return g_prepared; }
// Checks on the next game tick that a travel would find the manager; the UI calls this every frame, so at most once a second.
void TravelPrepare() {
    if (!g_travelOk) return;
    const DWORD now = GetTickCount(); if (now - g_lastProbe < 1000) return;
    bool expected = false; if (!g_probeQueued.compare_exchange_strong(expected, true)) return;
    g_lastProbe = now;
    RunOnGameThread([]() {
        const char* why = ""; const bool ok = ResolveManager(&why) != 0;
        if (ok != g_prepared.load()) Log("[travel] %s%s", ok ? "ready" : "not ready: ", ok ? "" : why);
        g_prepared = ok;
        const std::string st = TravelStatus();
        if (st != "travelling" || ok) SetStatus(ok ? "ready" : std::string("not ready: ") + why);
        g_probeQueued = false;
    });
}

static bool CallStageGuarded(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf) {
    bool ok = true;
    CDK_GUARD_BEGIN
        g_origStage(mgr, key, b, c, tf);
    CDK_GUARD_FAIL ok = false;
    CDK_GUARD_END
    return ok;
}

// Starts a fast travel to 'pos' (world metres) facing 'yawDeg'. False when travel is not installed or the last check on the
// game tick found no manager (a new check is queued, TravelStatus says why); callers retry, the check takes one tick, not a scan.
bool TravelTo(Vec3 pos, float yawDeg) {
    if (!g_travelOk) return false;
    if (!g_prepared) { TravelPrepare(); return false; }
    RunOnGameThread([pos, yawDeg]() { RunGroundWorldChange([pos, yawDeg]() {
        const char* why = ""; const uintptr_t mgr = ResolveManager(&why);
        if (!mgr) { g_prepared = false; SetStatus(std::string("travel not possible: ") + why); Log("[travel] fast travel to (%.1f %.1f %.1f) refused: %s", pos.x, pos.y, pos.z, why); return; }
        const float h = yawDeg * 3.14159265f / 360.0f;
        static float tf[10]; tf[0] = tf[1] = tf[2] = 1.0f; tf[3] = 0.0f; tf[4] = sinf(h); tf[5] = 0.0f; tf[6] = cosf(h); tf[7] = pos.x; tf[8] = pos.y; tf[9] = pos.z;   // scale, quat (x y z w), position
        const bool ok = CallStageGuarded(mgr, g_key, g_argB, g_argC, tf);
        Log("[travel] fast travel to (%.1f %.1f %.1f) via manager %p: %s", pos.x, pos.y, pos.z, (void*)mgr, ok ? "started" : "FAILED");
        SetStatus(ok ? "travelling" : "travel call failed");
    }); }); // core holds world authority through any Applying-lease wait and the actual native call
    return true;
}

}   // namespace core
