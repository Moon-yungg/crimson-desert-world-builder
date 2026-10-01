// Travel through the game's own fast travel: the client's ClientSequencerStageManager "reload stage at transform" shows the
// loading screen, sends TrocTrReloadStageStartReq to the (in-process) server, which stores the destination and applies it
// once the client's loading starts (notes/FORMATS.md, "THE GAME'S TELEPORT"). Unlike writing the player position, this streams
// the world around the destination, so any distance works - and tiles that were edited stream again (terrain "apply").
//
// Found at startup: the function by signature, the manager through its RTTI vtable (one background memory scan on first use,
// the object lives for the whole session and is re-checked before every call). The key / b / c arguments default to the
// values a real fast travel passes (3 / 1 / 0); a fast travel of the player's own replaces them with what the game used.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

typedef void(__fastcall* StageReloadFn)(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf);
static StageReloadFn g_origStage = nullptr; static uintptr_t g_stageFn = 0, g_mgrVt = 0;
static std::atomic<uintptr_t> g_mgr{ 0 };
static std::atomic<uint32_t> g_key{ 3 }, g_argB{ 1 }, g_argC{ 0 };
static std::atomic<int> g_scan{ 0 };            // 0 not started, 1 running, 2 done
static std::mutex g_tmx; static std::string g_status = "not installed";
static bool g_travelOk = false;

static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(g_tmx); g_status = s; }
std::string TravelStatus() { std::lock_guard<std::mutex> l(g_tmx); return g_status; }
bool TravelAvailable() { return g_travelOk; }

static bool MgrValid(uintptr_t m) { uintptr_t vt = 0; return m && ReadBytes(m, &vt, 8) && vt == g_mgrVt; }

// A fast travel of the player's own: remember the manager and the arguments the game uses right now.
static void __fastcall HookStage(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf) {
    if (MgrValid(mgr)) { g_mgr = mgr; g_key = key; g_argB = b; g_argC = c; }
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
    if (!InstallInternalHook((void*)g_stageFn, (void*)HookStage, (void**)&g_origStage, "stage reload (travel)")) { SetStatus("hook failed"); return; }
    g_travelOk = true; SetStatus("ready");
    Log("[travel] ready: stage reload rva 0x%llx, manager vtable rva 0x%llx", (unsigned long long)(g_stageFn - g_base), (unsigned long long)(g_mgrVt - g_base));
}

// One pass over committed private read-write memory for an object whose first qword is the manager's vtable.
static void ScanForManager() {
    SetStatus("looking for the game's travel system (first use, up to half a minute)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const DWORD t0 = GetTickCount(); std::vector<uint8_t> buf(1 << 20); MEMORY_BASIC_INFORMATION mbi{}; uintptr_t a = 0x10000; uintptr_t found = 0;
    while (!found && VirtualQuery((void*)a, &mbi, sizeof mbi) == sizeof mbi) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize; a = end;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
        for (uintptr_t c = base; c < end && !found; c += buf.size()) {
            const size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - c);
            if (!ReadBytes(c, buf.data(), n)) continue;
            for (size_t k = 0; k + 8 <= n; k += 8) {
                uintptr_t v; memcpy(&v, buf.data() + k, 8); if (v != g_mgrVt) continue;
                const uintptr_t obj = c + k; uintptr_t actor = 0; const char* rt = nullptr;   // +0x18: the client player actor (what the send uses)
                if (ReadBytes(obj + 0x18, &actor, 8) && actor && (rt = RttiName(actor)) && strstr(rt, "ClientChildOnlyInGameActor")) { found = obj; break; }
            }
        }
    }
    if (found) { uintptr_t expected = 0; g_mgr.compare_exchange_strong(expected, found); }
    Log("[travel] manager scan: %p in %lu ms", (void*)g_mgr.load(), GetTickCount() - t0);
    SetStatus(g_mgr ? "ready" : "travel system not found (use one fast travel of the game, then try again)");
    g_scan = 2;
}
bool TravelPrepared() { return MgrValid(g_mgr); }
void TravelPrepare() {
    if (!g_travelOk || MgrValid(g_mgr)) return;
    int expected = 0; if (!g_scan.compare_exchange_strong(expected, 1)) return;
    std::thread(ScanForManager).detach();
}

static bool CallStageGuarded(uintptr_t mgr, uint32_t key, uint32_t b, uint32_t c, const float* tf) {
    bool ok = true;
    CDK_GUARD_BEGIN
        g_origStage(mgr, key, b, c, tf);
    CDK_GUARD_FAIL ok = false;
    CDK_GUARD_END
    return ok;
}

// Starts a fast travel to 'pos' (world metres) facing 'yawDeg'. Returns false when the travel system is not ready yet.
bool TravelTo(Vec3 pos, float yawDeg) {
    if (!g_travelOk) return false;
    if (!MgrValid(g_mgr)) { if (g_scan == 2) g_scan = 0; TravelPrepare(); return false; }   // a finished scan that found nothing may run again
    RunOnGameThread([pos, yawDeg]() { RunGroundWorldChange([pos, yawDeg]() {
        const uintptr_t mgr = g_mgr; if (!MgrValid(mgr)) { SetStatus("travel system lost, looking again"); g_mgr = 0; g_scan = 0; TravelPrepare(); return; }
        const float h = yawDeg * 3.14159265f / 360.0f;
        static float tf[10]; tf[0] = tf[1] = tf[2] = 1.0f; tf[3] = 0.0f; tf[4] = sinf(h); tf[5] = 0.0f; tf[6] = cosf(h); tf[7] = pos.x; tf[8] = pos.y; tf[9] = pos.z;   // scale, quat (x y z w), position
        const bool ok = CallStageGuarded(mgr, g_key, g_argB, g_argC, tf);
        Log("[travel] fast travel to (%.1f %.1f %.1f): %s", pos.x, pos.y, pos.z, ok ? "started" : "FAILED");
        SetStatus(ok ? "travelling" : "travel call failed");
    }); }); // core holds world authority through any Applying-lease wait and the actual native call
    return true;
}

}   // namespace core
