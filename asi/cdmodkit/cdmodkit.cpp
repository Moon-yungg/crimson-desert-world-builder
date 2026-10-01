// cdmodkit.asi core (v0.20): console, player position, createSceneObjectFrom hook, game-thread pump, spawn registry.
// Loaded by Ultimate ASI Loader (winmm.dll) from bin64\. Log: bin64\cdmodkit\cdmodkit.log
// Offsets are for CrimsonDesert.exe 1.0.0.2850. See notes/FORMATS.md for provenance.
#include "core.h"
#include "i18n.h"
#include <cstdio>
#include <cmath>
#include <deque>
#include <mutex>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <limits>
#include <algorithm>
#include <intrin.h>
#include <winver.h>
#include "MinHook.h"
#pragma comment(lib, "version.lib")
#include "core_internal.h"
#include "guard.h"
namespace cdk { thread_local FaultInfo t_fault;
#ifndef _MSC_VER
thread_local GuardFrame* t_guardTop = nullptr;
#endif
}
#include "thumbgen.h"
#include "input.h"
#include "http_api.h"
#include "proj_codec.h"
#include "wb_group_math.h"
#ifdef WB_UNIFIED_HOST_TEST
// Host seam (plan wb079-unified-77f68967): production_core_host.cpp includes this TU once with
// WB_UNIFIED_HOST_TEST and the boundaries marked below call through host::Seam(). Nothing else changes
// meaning: registry, queues, pump dispatch, lifecycle, projects and History stay production code.
#include "../../tests/wb_groups/production_host.h"
#endif

namespace core {

#ifdef WB_UNIFIED_HOST_TEST
ManagedNpcNativeTest g_managedNpcNativeTest;
#endif
uintptr_t g_base = 0;
bool      g_menuOpen = false;
bool      g_uiWantsMouse = false;
bool      g_uiWantsKeyboard = false;
bool      g_uiTextInput = false, g_uiMouseOverUi = false;
bool      g_placing = false;
static HMODULE g_self = nullptr;
static FILE*   g_log = nullptr;
static bool    g_console = false;
static std::string g_modDir;
bool g_httpEnabled = false;   // settings.txt http_api=1; off by default, the Settings tab starts and stops the server at runtime
int g_httpPort = 8765;

void Log(const char* fmt, ...) {
    SYSTEMTIME st; GetLocalTime(&st);
    char line[4096];
    va_list a; va_start(a, fmt); vsnprintf(line, sizeof line, fmt, a); va_end(a);
    if (g_log) { fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line); fflush(g_log); }
    if (g_console) { printf("%s\n", line); fflush(stdout); }
}
std::string ModDir() { return g_modDir; }
bool EmbeddedResource(int resourceId, const uint8_t** data, size_t* size) {
    if (data) *data = nullptr;
    if (size) *size = 0;
    if (!g_self || resourceId <= 0) return false;
    HRSRC r = FindResourceA(g_self, MAKEINTRESOURCEA(resourceId), RT_RCDATA);
    if (!r) return false;
    HGLOBAL h = LoadResource(g_self, r);
    const void* p = h ? LockResource(h) : nullptr;
    const DWORD n = SizeofResource(g_self, r);
    if (!p || !n) return false;
    if (data) *data = static_cast<const uint8_t*>(p);
    if (size) *size = static_cast<size_t>(n);
    return true;
}

static std::string DirOf(HMODULE m) {
    char buf[MAX_PATH]; GetModuleFileNameA(m, buf, MAX_PATH);
    std::string s(buf); size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? "." : s.substr(0, p);
}
static std::string ExeName() {
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf); size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? s : s.substr(p + 1);
}

// ---- guarded memory access (SEH; locals must stay POD) ----
static thread_local bool t_guardedRead = false;   // probing reads fault on purpose; the vectored handler stays quiet for them
bool ReadBytes(uintptr_t a, void* out, size_t n) {
    if (a < 0x10000 || (a >> 47) != 0) return false;
    t_guardedRead = true;
    CDK_GUARD_BEGIN memcpy(out, (const void*)a, n); t_guardedRead = false; return true;
    CDK_GUARD_FAIL t_guardedRead = false; return false;
    CDK_GUARD_END
}
bool WriteBytes(uintptr_t a, const void* src, size_t n) {
    if (a < 0x10000 || (a >> 47) != 0) return false;
    CDK_GUARD_BEGIN memcpy((void*)a, src, n); return true;
    CDK_GUARD_FAIL return false;
    CDK_GUARD_END
}
static bool ReadPtr(uintptr_t a, uintptr_t* out) {
    uintptr_t v = 0; if (!ReadBytes(a, &v, 8)) return false;
    if (v < 0x10000 || (v >> 47) != 0) return false;
    *out = v; return true;
}
static uintptr_t Deref(uintptr_t p, unsigned off) { uintptr_t v = 0; return (p && ReadPtr(p + off, &v)) ? v : 0; }
static bool Read32(uintptr_t a, uint32_t* out) { return ReadBytes(a, out, 4); }
static bool ReadF3(uintptr_t a, float* out) { return ReadBytes(a, out, 12); }
static bool InImage(uintptr_t p) { return p >= g_base && p < g_base + 0x17000000; }

static bool ReadCStr(uintptr_t a, char* out, size_t n) {
    if (!ReadBytes(a, out, n)) {
        size_t i = 0; for (; i + 1 < n; i++) { if (!ReadBytes(a + i, out + i, 1) || !out[i]) break; } out[i] = 0;
    }
    out[n - 1] = 0;
    size_t len = strnlen(out, n);
    if (len < 3) return false;
    for (size_t i = 0; i < len; i++) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) return false;
    return true;
}

const char* RttiName(uintptr_t obj) {
    uintptr_t vt = 0, col = 0; uint32_t tdRva = 0;
    if (!ReadPtr(obj, &vt) || !InImage(vt) || !ReadPtr(vt - 8, &col) || !InImage(col) || !Read32(col + 12, &tdRva)) return nullptr;
    uintptr_t td = g_base + tdRva; char c[4] = {0};
    if (!ReadBytes(td + 16, c, 4) || c[0] != '.' || c[1] != '?') return nullptr;
    return (const char*)(td + 16);
}

// ---- game layout: resolved at runtime (signatures verified on 1.0.0.2850 and 1.0.0.2944) ----
// Reference values (2850 / 2944): createSceneObjectFrom 0x3A6E600 / 0x3B58120, setWorldTransform 0x261AF00 / 0x26D4AC0,
// setEnable 0x261B9A0 / 0x26D5560, StringDataAlloc 0x1391FD0 / 0x1420470, PrefabPathCtor 0x1319EA0 / 0x13A7C50,
// PathNormalizeCtor 0x1238AB0 / 0x12C6B00, WorldGlobal 0x6C2D9F0 / 0x6D691B0.
static uintptr_t kRva_WorldGlobal = 0;            // [g] -> ClientActorManager -> ClientUserActor ; [g] -> +0xE0 -> +0xEB0 SceneObjectManager
static uintptr_t kRva_CreateSceneObjectFrom = 0;  // SceneObjectManager::createSceneObjectFrom
static uintptr_t kRva_PrefabPathCtor = 0;         // ResourceReferencePath_Prefab(this, const StringObj*)
static uintptr_t kRva_StringDataAlloc = 0;        // StringData* alloc(int len)
static uintptr_t kRva_PathNormalizeCtor = 0;      // NormalizedPath(this8, const StringObj* src)
static uintptr_t kRva_SetWorldTransform = 0;      // SceneObject::setWorldTransform(this, const Transform*, u8 a=0, u8 b=1)
static uintptr_t kRva_SetEnable = 0;              // SceneObject::setEnable(this, u8)
static uintptr_t kRva_ProbeCollector = 0;         // vtable of the collector class the ground probe uses, see ResolveProbeCollectorVtable()
static uintptr_t g_probeVtableOverride = 0;       // settings.txt probe_vtable=<rva>: manual fallback, read only, never written back
// struct offsets: defaults from 2850, the player chain is re-discovered through RTTI names at runtime
constexpr unsigned kOff_Ent_Eid = 0x60;
static unsigned kOff_Ent_Comps = 0x68, kOff_Comps_Transform = 0x1A0;
constexpr unsigned kOff_Tf_Pos = 0xB4, kOff_Tf_Tile = 0xC0;
constexpr float    kTileSize = 1000.0f;

static long  g_pumpTicks = 0;
static DWORD g_gameThread = 0;

// first qword in [base, base+maxOff) that points to an object whose RTTI name contains `cls`
static uintptr_t FindPtrWithRtti(uintptr_t base, unsigned maxOff, const char* cls) {
    if (!base) return 0;
    for (unsigned off = 0; off < maxOff; off += 8) {
        uintptr_t p = Deref(base, off); if (!p) continue;
        const char* n = RttiName(p); if (n && strstr(n, cls)) return p;
    }
    return 0;
}
static uintptr_t UserActor() {
    uintptr_t root = Deref(g_base + kRva_WorldGlobal, 0);
    uintptr_t am = FindPtrWithRtti(root, 0x100, "ClientActorManager@");
    return FindPtrWithRtti(am, 0x200, "ClientUserActor@");
}
uintptr_t PlayerActor() {
    // cached: the RTTI walk probes dozens of pointers with guarded reads (each miss is an SEH exception), so it is
    // only repeated when the cached actor stops looking like one, and at most twice a second while there is no player
    static uintptr_t s_child = 0; static DWORD s_lastScan = 0;
    const DWORD now = GetTickCount();
    // The controlled actor (user actor +0xD8 in the CrimsonRoute notes, found here by RTTI) carries the TransformSync at
    // components +0x68 / +0x1A0. Riding is handled by the parent part of the transform snapshot, see ReadPos.
    if (s_child) { const char* cn = RttiName(s_child); if (!cn || !strstr(cn, "ClientChildOnlyInGameActor@")) s_child = 0; }
    // the game has three playable characters and the old one keeps existing after a switch, so the cached actor still looks
    // valid: re-resolve the controlled actor from the user actor once a second and follow it when it changed
    if (s_child && now - s_lastScan < 1000) return s_child;
    if (!s_child && now - s_lastScan < 500) return 0;
    s_lastScan = now;
    uintptr_t user = UserActor(); if (!user) return s_child;
    uintptr_t child = FindPtrWithRtti(user, 0x200, "ClientChildOnlyInGameActor@");
    if (!child) return s_child ? s_child : user;
    if (child == s_child) return s_child;
    if (s_child) Log("player actor changed: %p -> %p (character switch)", (void*)s_child, (void*)child);
    s_child = child;
    // component table: verify the cached offsets, otherwise search for them once
    uintptr_t comps = Deref(child, kOff_Ent_Comps);
    uintptr_t tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    const char* n = tf ? RttiName(tf) : nullptr;
    if (!n || !strstr(n, "TransformSyncActorComponent")) {
        for (unsigned co = 0x40; co < 0x100 && (!n || !strstr(n, "TransformSyncActorComponent")); co += 8) {
            uintptr_t table = Deref(child, co); if (!table) continue;
            for (unsigned to = 0; to < 0x300; to += 8) {
                uintptr_t c = Deref(table, to); const char* cn = c ? RttiName(c) : nullptr;
                if (cn && strstr(cn, "TransformSyncActorComponent")) { kOff_Ent_Comps = co; kOff_Comps_Transform = to; n = cn; Log("player layout: comps +0x%X, transform +0x%X", co, to); break; }
            }
        }
    }
    return child;
}
static bool ReadTransformPos(uintptr_t tf, PosInfo* out, bool logParent) {
    if (!tf || !out) return false;
    // Committed transform snapshot, 0x48 bytes at +0xB4 (layout confirmed with the CrimsonRoute author): local xyz, int16 sector x/z,
    // scale (+0x1C), quaternion xyzw (+0x28), parent translation (+0x38), parent sector (+0x44). Unparented actors have parent 0 and
    // the plain sector*1000 + local is the world position. On a mount the local part is relative to the horse: world =
    // rotate(scale * (sector*1000 + local)) + parentSector*1000 + parent. That is what turned into 0/0/0 before.
    uint8_t snap[0x48]; if (!ReadBytes(tf + kOff_Tf_Pos, snap, sizeof snap)) return false;
    float f[18]; memcpy(f, snap, sizeof f); int16_t tile[2], psec[2]; memcpy(tile, snap + 0x0C, 4); memcpy(psec, snap + 0x44, 4);
    for (int i = 0; i < 3; i++) if (!std::isfinite(f[i])) return false;
    float v[3] = { f[0], f[1], f[2] };
    const float sx = f[7], sy = f[8], sz = f[9], qx = f[10], qy = f[11], qz = f[12], qw = f[13], px = f[14], py = f[15], pz = f[16];
    const bool parentFinite = std::isfinite(px) && std::isfinite(py) && std::isfinite(pz) && std::isfinite(qx) && std::isfinite(qy) && std::isfinite(qz) && std::isfinite(qw) && std::isfinite(sx) && std::isfinite(sy) && std::isfinite(sz);
    const bool hasParent = parentFinite && (psec[0] != 0 || psec[1] != 0 || fabsf(px) > 0.001f || fabsf(py) > 0.001f || fabsf(pz) > 0.001f) && fabsf(px) < 200000 && fabsf(pz) < 200000 && fabsf(py) < 50000;
    if (hasParent) {   // mounted / attached: compose with the parent transform and report the result as a plain world position
        const double qn = (double)qx * qx + (double)qy * qy + (double)qz * qz + (double)qw * qw;
        double vx = (tile[0] * 1000.0 + v[0]) * (sx > 0.001f ? sx : 1.0f), vy = v[1] * (double)(sy > 0.001f ? sy : 1.0f), vz = (tile[1] * 1000.0 + v[2]) * (sz > 0.001f ? sz : 1.0f);
        if (fabs(qn - 1.0) < 0.05) {
            const double tx = 2.0 * (qy * vz - qz * vy), ty = 2.0 * (qz * vx - qx * vz), tz = 2.0 * (qx * vy - qy * vx);
            const double rx = vx + qw * tx + qy * tz - qz * ty, ry = vy + qw * ty + qz * tx - qx * tz, rz = vz + qw * tz + qx * ty - qy * tx;
            vx = rx; vy = ry; vz = rz;
        }
        const double wx = vx + psec[0] * 1000.0 + px, wy = vy + py, wz = vz + psec[1] * 1000.0 + pz;
        if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz) || fabs(wx) > 200000 || fabs(wz) > 200000 || fabs(wy) > 50000) return false;
        static bool s_parentLogged = false; if (logParent && !s_parentLogged) { s_parentLogged = true; Log("player transform has a parent (mounted?): local (%.2f %.2f %.2f) sector %d,%d parent (%.2f %.2f %.2f) sector %d,%d -> world (%.2f %.2f %.2f)", v[0], v[1], v[2], tile[0], tile[1], px, py, pz, psec[0], psec[1], wx, wy, wz); }
        const int tx2 = (int)(wx * 0.001), tz2 = (int)(wz * 0.001);
        out->world = { (float)wx, (float)wy, (float)wz }; out->tileX = tx2; out->tileZ = tz2; out->tiled = { (float)(wx - tx2 * 1000.0), (float)wy, (float)(wz - tz2 * 1000.0) };
        return true;
    }
    if (fabsf(v[0]) > 1500 || fabsf(v[2]) > 1500 || fabsf(v[1]) > 20000 || abs(tile[0]) > 500 || abs(tile[1]) > 500) {
        static bool s_warned = false; if (!s_warned) { s_warned = true; Log("WARNING: transform component layout looks different (tiled %.1f %.1f %.1f tile %d,%d)", v[0], v[1], v[2], tile[0], tile[1]); }
    }
    out->tiled = { v[0], v[1], v[2] }; out->tileX = tile[0]; out->tileZ = tile[1];
    out->world = { v[0] + tile[0] * kTileSize, v[1], v[2] + tile[1] * kTileSize };
    return true;
}
bool ReadPos(uintptr_t actor, PosInfo* out) {
    uintptr_t comps = Deref(actor, kOff_Ent_Comps); uintptr_t tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    return ReadTransformPos(tf, out, true);
}
static uintptr_t FindActorTransform(uintptr_t actor) {
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.transform) return g_managedNpcNativeTest.transform(actor);
#endif
    if (!actor) return 0;
    uintptr_t comps = Deref(actor, kOff_Ent_Comps), tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    const char* n = tf ? RttiName(tf) : nullptr;
    if (n && strstr(n, "TransformSyncActorComponent@")) return tf;
    // ServerNormalInGameActor uses the same component-table concept as the client actor, but its transform slot can move
    // independently between builds. Discover it once per spawned NPC from RTTI rather than assuming the client slot.
    for (unsigned co = 0x40; co < 0x100; co += 8) {
        uintptr_t table = Deref(actor, co); if (!table) continue;
        for (unsigned to = 0; to < 0x300; to += 8) {
            uintptr_t c = Deref(table, to); const char* cn = c ? RttiName(c) : nullptr;
            if (cn && strstr(cn, "TransformSyncActorComponent@")) return c;
        }
    }
    return 0;
}
bool PlayerPosInfo(PosInfo* out) {
#ifdef WB_UNIFIED_HOST_TEST
    if (host::Seam().playerWorldPos) {   // OS/engine boundary: the game-memory transform read is unavailable in a host run
        Vec3 w{}; if (!host::Seam().playerWorldPos(&w)) return false;
        out->world = w; out->tiled = w; out->tileX = (int)floorf(w.x / kTileSize); out->tileZ = (int)floorf(w.z / kTileSize);
        return true;
    }
#endif
    uintptr_t a = PlayerActor(); return a && ReadPos(a, out);
}
// Research: fall watcher. Polls the player every 50 ms; a drop of more than 3 m below the last stable height counts as a fall
// (logged with position), then hardware write breakpoints on the transform snapshot's local x/y/z and tile words show which code
// moves the character (physics integration vs. a rescue teleport). A jump of more than 20 m afterwards is logged as a respawn.
static volatile bool g_fallWatch = false; static volatile uintptr_t g_fallWatchAddr = 0;   // 0: the client transform snapshot
static volatile bool g_fallBreak = true; static volatile uintptr_t g_fallProbe = 0; static volatile unsigned g_fallProbeLen = 0;   // probe: sampled every poll
void SetFallWatch(bool on, uintptr_t addr, bool breakpoints, uintptr_t probe, unsigned probeLen) {
    if (on && g_fallWatch) { g_fallWatch = false; Sleep(200); }   // restart with the new settings
    if (on == g_fallWatch) return;
    g_fallWatchAddr = addr; g_fallBreak = breakpoints; g_fallProbe = probe; g_fallProbeLen = probe ? std::min(probeLen ? probeLen : 0x400u, 0x1000u) & ~3u : 0;
    g_fallWatch = on; Log("[fallwatch] %s%s%s", on ? "on" : "off", on && addr ? " (explicit address)" : "", on && !breakpoints ? " (no breakpoints)" : "");
    if (on && probe) Log("[fallwatch] probe %p, 0x%x bytes (%s)", (void*)probe, g_fallProbeLen, RttiName(probe) ? RttiName(probe) : "-");
    if (!on) return;
    std::thread([]() {
        float stableY = NAN; DWORD stableTick = 0; bool falling = false; PosInfo last{}; bool haveLast = false;
        uintptr_t watchedTf = 0; DWORD lastRefresh = 0;
        static const int kRing = 48; std::vector<std::vector<uint8_t>> ring(kRing); int ringPos = 0, ringFill = 0;   // ~2.4 s of probe samples
        auto dumpProbe = [&](const char* why) {   // every dword that differs between the oldest kept sample and the newest
            if (!g_fallProbe || ringFill < 2) return;
            const auto& oldS = ring[(ringPos - ringFill + kRing) % kRing]; const auto& newS = ring[(ringPos - 1 + kRing) % kRing];
            int n = 0;
            for (size_t o = 0; o + 4 <= oldS.size() && o + 4 <= newS.size() && n < 120; o += 4) {
                uint32_t a, b; memcpy(&a, &oldS[o], 4); memcpy(&b, &newS[o], 4); if (a == b) continue;
                float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                Log("[fallwatch] probe %s +0x%03zX: %08x -> %08x (%g -> %g)", why, o, a, b, fa, fb); n++;
            }
            Log("[fallwatch] probe %s: %d changed dwords over %d samples", why, n, ringFill);
        };
        while (g_fallWatch) {
            Sleep(50);
            if (g_fallProbe) {
                auto& slot = ring[ringPos]; slot.resize(g_fallProbeLen);
                if (ReadBytes(g_fallProbe, slot.data(), slot.size())) { ringPos = (ringPos + 1) % kRing; ringFill = std::min(ringFill + 1, kRing); }
            }
            const uintptr_t actor = PlayerActor(); PosInfo p{};
            if (!actor || !ReadPos(actor, &p)) continue;
            const DWORD now = GetTickCount();
            {   // keep the write watch on the current player's transform snapshot (x, y, z, tile), re-armed for new threads
                uintptr_t comps = Deref(actor, kOff_Ent_Comps), tf = comps ? Deref(comps, kOff_Comps_Transform) + kOff_Tf_Pos : 0;
                if (g_fallWatchAddr) tf = g_fallWatchAddr;   // an explicit field (x, y, z, next dword)
                if (!g_fallBreak) tf = 0;
                if (tf && tf != watchedTf) {
                    if (watchedTf) StopWatch();
                    const uintptr_t a[4] = { tf, tf + 4, tf + 8, tf + 0x0C };
                    if (StartWatch(a)) { watchedTf = tf; Log("[fallwatch] watching writes to %p (%s)", (void*)tf, g_fallWatchAddr ? "explicit" : "client transform snapshot"); }
                } else if (now - lastRefresh > 2000) { RefreshWatch(); lastRefresh = now; }
            }
            if (haveLast && (fabsf(p.world.x - last.world.x) > 20.0f || fabsf(p.world.z - last.world.z) > 20.0f || p.world.y - last.world.y > 20.0f)) {
                Log("[fallwatch] RESPAWN / teleport: (%.2f %.2f %.2f) -> (%.2f %.2f %.2f); writers in the last 1.5 s:", last.world.x, last.world.y, last.world.z, p.world.x, p.world.y, p.world.z);
                DumpWatch("fallwatch", GetTickCount64() - 1500, GetTickCount64()); dumpProbe("at respawn");
                falling = false; stableY = p.world.y; stableTick = now;
            }
            if (!falling) {
                if (!std::isfinite(stableY) || fabsf(p.world.y - stableY) < 0.5f) { if (!std::isfinite(stableY) || now - stableTick > 300) { stableY = p.world.y; } stableTick = now; }
                else if (p.world.y > stableY) { stableY = p.world.y; stableTick = now; }   // walking up: follow
                if (p.world.y < stableY - 3.0f) {
                    falling = true;
                    Log("[fallwatch] FALL: y %.2f is %.2f m below the last stable %.2f at (%.2f %.2f), falling for %lu ms; writers in the 2.5 s before:", p.world.y, stableY - p.world.y, stableY, p.world.x, p.world.z, now - stableTick);
                    DumpWatch("fallwatch", GetTickCount64() - 2500, GetTickCount64()); dumpProbe("before the fall");
                }
            } else if (p.world.y < -2000.0f || now - stableTick > 60000) { falling = false; stableY = NAN; }
            last = p; haveLast = true;
        }
        StopWatch();
    }).detach();
}
bool PlayerWorldPos(Vec3* out) { PosInfo p; if (!PlayerPosInfo(&p)) return false; *out = p.world; return true; }

// ---- strings / describe ----
static std::string StringObjText(uintptr_t stringObj) {
    char buf[300]; uintptr_t data = Deref(stringObj, 0); uintptr_t cs = data ? Deref(data, 0) : 0;
    if (cs && ReadCStr(cs, buf, sizeof buf)) return buf;
    if (data && ReadBytes(data, buf, 1) && buf[0] == 0) return "";
    return "<?>";
}
static std::string PrefabPathText(uintptr_t rrp) { return StringObjText(rrp + 0x28); }
static std::string Describe(uintptr_t p) {
    char buf[256]; std::string s;
    if (!p) return "null";
    if (const char* n = RttiName(p)) { s += " obj:"; s += n; }
    uint8_t raw[48] = {0};
    if (ReadBytes(p, raw, 48)) { s += " bytes="; for (int i = 0; i < 48; i++) { snprintf(buf, sizeof buf, "%02x%s", raw[i], (i % 8 == 7) ? " " : ""); s += buf; } }
    return s;
}

// ---- hook: SceneObjectManager::createSceneObjectFrom ----
using CreateFn = void* (__fastcall*)(void* mgr, void* tag, void* b, void* c, void* d, void* transform, uint8_t f1, uint8_t f2, uint8_t f3);
static CreateFn g_origCreate = nullptr;
static long g_createCalls = 0;
static bool g_trace = false;                 // see SetTrace
static uintptr_t g_lastGameObj = 0;         // last SceneObject the game itself created
static int  g_createLogged = 0;
static bool g_inOurSpawn = false;
static void* g_lastMgr = nullptr;

static void NameGimmickCapture(const std::string& prefab, float x, float y, float z);
extern uintptr_t kRva_GimmickSpawn_;
// The prepare's callers, found at startup by scanning the image for calls to it (build 2949 had them at fixed addresses:
// level streaming 0x2af537e, housing 0x2b65ea0, item drop 0x2b4d733; build 2976 moved everything by 0x70). Classified by the
// bytes after the call (level, drop) or by the reason string the caller hashes right after it (housing and the others).
static uintptr_t g_callerLevel = 0, g_callerHousing = 0, g_callerDrop = 0;
static std::map<uintptr_t, std::string> g_callerReason;   // return address -> spawn reason string ("housing", "drop", "inspect", ...)
// interactive objects (gimmick prefabs spawned through the game's server spawn path); see the gimmick section below
bool g_gimmickSpawn = true; static bool g_traceHooks = false;
static bool IsGimmickPrefab(const std::string& p) { return p.rfind("/object/cd_gimmick/", 0) == 0; }
using MoveCompletion = std::function<void(bool)>;
static void EnqueueGimmick(int uid, uint64_t gen, const std::string& prefab, Vec3 pos, Rot rot, float scale, MoveCompletion done = {});
static void RunOnServerTick(std::function<void()> f);
static bool RemoveSpawnedActor(uintptr_t actor);
static void* __fastcall HookCreate(void* mgr, void* tag, void* b, void* c, void* d, void* transform, uint8_t f1, uint8_t f2, uint8_t f3) {
    long n = InterlockedIncrement(&g_createCalls);
    bool log = g_createLogged < 20 || g_inOurSpawn || g_trace;
    if (log) g_createLogged++;
    if (!g_inOurSpawn) g_lastMgr = mgr;
    uintptr_t retAddr = (uintptr_t)_ReturnAddress();
    if (PlayModeActive()) {   // play mode: census of who creates what, and the isolation gate for the world's own objects
        float t[10] = { 0 }; ReadBytes((uintptr_t)transform, t, 40);
        if (PlayModeOnCreate(InImage(retAddr) ? retAddr - g_base : 0, PrefabPathText((uintptr_t)d), t, f1, f2, f3, g_inOurSpawn || OurServerSpawnOnThisThread())) return nullptr;
    }
    void* r = g_origCreate(mgr, tag, b, c, d, transform, f1, f2, f3);
    if (!g_inOurSpawn && r) g_lastGameObj = (uintptr_t)r;
    if (!g_inOurSpawn && r && kRva_GimmickSpawn_) { float t[10] = {0}; if (ReadBytes((uintptr_t)transform, t, 40)) NameGimmickCapture(PrefabPathText((uintptr_t)d), t[7], t[8], t[9]); }
    if (log) {
        float t[10] = {0}; ReadBytes((uintptr_t)transform, t, 40);
        Log("[create #%ld] thread=%lu%s from=rva 0x%llx f=%d,%d,%d prefab=\"%s\" pos=(%.2f %.2f %.2f) ret=%p (%s)", n, GetCurrentThreadId(),
            GetCurrentThreadId() == g_gameThread ? "(game)" : "", InImage(retAddr) ? (unsigned long long)(retAddr - g_base) : 0ull, f1, f2, f3,
            PrefabPathText((uintptr_t)d).c_str(), t[7], t[8], t[9], r, RttiName((uintptr_t)r) ? RttiName((uintptr_t)r) : "?");
    }
    return r;
}

// ---- spawn registry ----
static std::mutex g_regMutex;
static std::vector<SpawnedObj> g_reg;
static std::vector<ManagedNpc> g_npcReg;
static std::set<uintptr_t> g_managedNpcActors;
struct PendingNpcCleanup { int uid; Vec3 pos; DWORD requestTick; };
static std::vector<PendingNpcCleanup> g_pendingNpcCleanup; // protected by g_regMutex; async spawns deleted before actor binding
static void RememberUnboundNpcCleanupLocked(const ManagedNpc& n) {
    if (!n.actor && n.spawnRequestTick) g_pendingNpcCleanup.push_back({ n.uid, n.pos, n.spawnRequestTick });
}
static std::map<int, std::string> g_groupNames;
static int g_nextUid = 1, g_nextNpcUid = 1, g_nextGroup = 1;
static void ReconcileManagedNpcActors();
#ifdef WB_UNIFIED_HOST_TEST
// Host-only lock discipline instrumentation (never compiled into the shipped ASI): the ObjectLifetime
// suite asserts that engine/server work is queued with the registry lock released. REG_LOCK is the
// registry lock plus the depth note the queue helpers read.
static thread_local int t_regLockDepth = 0;
struct RegLockNote { RegLockNote() { ++t_regLockDepth; } ~RegLockNote() { --t_regLockDepth; } };
#define REG_LOCK std::lock_guard<std::mutex> l(g_regMutex); RegLockNote regLockNote_
static void NoteWorkQueued() { if (t_regLockDepth > 0) host::NoteWorkQueued(true); }
#else
#define REG_LOCK std::lock_guard<std::mutex> l(g_regMutex)
static void NoteWorkQueued() {}
#endif
#ifdef WB_UNIFIED_HOST_TEST
SceneEnumerationStats g_sceneEnumerationStats;
#endif
std::vector<SpawnedObj> Spawned() {
    REG_LOCK;
#ifdef WB_UNIFIED_HOST_TEST
    ++g_sceneEnumerationStats.calls; g_sceneEnumerationStats.records += g_reg.size();
#endif
    return g_reg;
}
void ForgetSpawned(size_t idx) { int uid = 0; { REG_LOCK; if (idx < g_reg.size()) uid = g_reg[idx].uid; } if (uid) ForgetUid(uid); }
std::vector<ManagedNpc> ManagedNpcs() { ReconcileManagedNpcActors(); std::lock_guard<std::mutex> l(g_regMutex); return g_npcReg; }
bool ManagedNpcLivePosition(const ManagedNpc& npc, Vec3* out) {
    if (!out || npc.hidden || !npc.actor) return false;
    PosInfo p{};
    if (npc.transform && ReadTransformPos(npc.transform, &p, false)) { *out = p.world; return true; }
    if (ReadPos(npc.actor, &p)) { *out = p.world; return true; }
    return false;
}
// A project is "dirty" once one of its objects was moved, deleted or regrouped since it was loaded or saved; the scene
// tabs mark that with a star. Guarded by g_regMutex together with the registry it describes.
static std::set<int> g_projDirty;
static bool g_loading = false;          // LoadProject is running: the objects it spawns must not mark the project dirty
static void MarkDirtyLocked(int proj) { if (proj > 0) g_projDirty.insert(proj); }
static void ForgetCopyValueLocked(int uid);   // record-owned copy provenance (defined next to the project value builder)
static thread_local std::string g_projectError;   // the last save/load validation failure ("" = none), named by data-record ordinal
static std::shared_ptr<void> TrackProjectNativeWork(int proj); // file activity spanning native work even after its record is unloaded
#ifdef WB_UNIFIED_HOST_TEST
// Host-only file-fault seam for the ProjectLifecycle suite (never compiled into the shipped ASI): the production
// save/import path consults these while the fixture drives real files through it, and each fault is one-shot.
static int g_saveFaultStage = 0;                      // 0 none, 1 write abort, 2 short write, 3 flush abort, 4 replace abort, 5 close abort
static std::function<void()> g_beforeReplaceCallback;  // runs after a successful readback, before the replace (C6 approval seam)
#endif
static int IndexOfUidLocked(int uid) { for (size_t i = 0; i < g_reg.size(); i++) if (g_reg[i].uid == uid) return (int)i; return -1; }
static int NpcIndexOfUidLocked(int uid) { for (size_t i = 0; i < g_npcReg.size(); i++) if (g_npcReg[i].uid == uid) return (int)i; return -1; }
static bool IsManagedNpcActor(uintptr_t actor) { std::lock_guard<std::mutex> l(g_regMutex); return g_managedNpcActors.count(actor) != 0; }
// C3: a logical record (uid) outlives every physical object it ever had. `gen` is the lifetime token of the
// record's current materialization: it is allocated when the record is created and renewed whenever the physical
// object is replaced or invalidated (restore, re-create on a final move, the stand-in/server lane switch) and
// when it is hidden or forgotten. Every queued engine/server job carries (uid, gen) and rechecks it at the engine
// boundary, so a completion that arrives after such a change can never attach to a newer incarnation; it is
// disposed on the thread that created it instead.
static uint64_t g_nextGen = 1;
static uint64_t NewGenLocked() { return g_nextGen++; }
static bool NpcGenCurrentLocked(int uid, uint64_t gen) {
    const int i = NpcIndexOfUidLocked(uid);
    return i >= 0 && g_npcReg[(size_t)i].gen == gen && !g_npcReg[(size_t)i].hidden;
}
static bool GenCurrentLocked(int uid, uint64_t gen) {
    const int i = IndexOfUidLocked(uid);
    return i >= 0 && g_reg[(size_t)i].gen == gen && !g_reg[(size_t)i].hidden;
}
int IndexOfUid(int uid) { std::lock_guard<std::mutex> l(g_regMutex); return IndexOfUidLocked(uid); }

// ---- C7: grounding-local authority. Never held across an engine call or queue dispatch. ----------------
struct GroundOp {
    GroundView view;
    uint64_t born = 0, ticketAt = 0;
    int ticket = 0;
    bool ready = false;
};
static std::mutex g_groundOpMutex;
static void PruneGroundTickets(); // queue cleanup is always outside the operation lock
static uint64_t g_groundEpoch = 1, g_groundId = 0, g_groundFrame = 0, g_groundNotice = 0;
static unsigned g_groundWorldWriting = 0;
static GroundPlacement g_groundPlacement;
static std::vector<std::weak_ptr<GroundOp>> g_groundOps;
static std::map<int, GroundHandle> g_groundLeases;
struct GroundDeferred { std::vector<int> targets; std::function<void()> action; };
static std::vector<GroundDeferred> g_groundDeferred;
static std::map<int, int> g_groundMutationPending;
static bool GroundSamePlacement(const GroundPlacement& a, const GroundPlacement& b) {
    return a.generation == b.generation && a.transform == b.transform && a.members == b.members;
}
static void GroundEndLocked(const GroundHandle& op, GroundState state, const char* reason) {
    if (op->view.terminal() || op->view.state == GroundApplying) return;
    op->view.state = state; op->view.reason = reason;
    for (auto& m : op->view.members) { m.terminal = true; m.after = m.before; m.reason = reason; }
    ++g_groundNotice;
}
static bool GroundTouches(const GroundHandle& op, const std::vector<int>& ids) {
    if (ids.empty()) return true; // scene-wide mutation
    for (const auto& m : op->view.members) if (std::find(ids.begin(), ids.end(), m.before.uid) != ids.end()) return true;
    return false;
}
// Caller keeps the operation mutex until its registry mutation is admitted. A deferred core action owns
// copied values/UIDs, never an index or a borrowed UI pointer. Leases protect Applying, not just enqueue.
static bool GroundBeforeMutationLocked(const std::vector<int>& ids, std::function<void()> action) {
    bool busy = false;
    for (auto& w : g_groundOps) if (auto op = w.lock()) if (GroundTouches(op, ids)) {
        if (op->view.state == GroundApplying) busy = true;
        else GroundEndLocked(op, GroundInvalidated, "epoch-invalidated");
    }
    if (!busy) return true;
    std::vector<int> targets = ids;
    if (targets.empty()) for (const auto& pair : g_groundLeases) targets.push_back(pair.first);
    for (int uid : targets) ++g_groundMutationPending[uid];
    g_groundDeferred.push_back({ targets, std::move(action) });
    return false;
}
static bool GroundValidLocked(const GroundHandle& op) { // operation -> registry
    if (op->view.terminal() || op->view.state == GroundApplying) return false;
    bool valid = !g_groundWorldWriting && op->view.epoch == g_groundEpoch;
    if (op->view.carried.generation) valid &= GroundSamePlacement(op->view.carried, g_groundPlacement);
    { REG_LOCK;
      for (const auto& m : op->view.members) {
          const int i = IndexOfUidLocked(m.before.uid);
          valid &= i >= 0 && !g_groundMutationPending.count(m.before.uid) && !g_groundLeases.count(m.before.uid);
          if (i >= 0) { const auto& e = g_reg[(size_t)i]; valid &= !e.hidden && e.gen == m.before.gen && e.poseGen == m.before.poseGen; }
      }
    }
    if (!valid) GroundEndLocked(op, GroundInvalidated, "epoch-invalidated");
    return valid;
}
GroundHandle BeginGround(const std::vector<int>& uids, uint64_t serial, uint64_t branch, const GroundPlacement& carried) {
    std::lock_guard<std::mutex> lock(g_groundOpMutex);
    auto op = std::make_shared<GroundOp>();
    op->view.id = ++g_groundId; op->view.serial = serial; op->view.branch = branch;
    op->view.epoch = g_groundEpoch; op->view.carried = carried; op->born = g_groundFrame;
    bool busy = false;
    for (auto& w : g_groundOps) if (auto old = w.lock()) if (GroundTouches(old, uids)) {
        if (old->view.state == GroundApplying) busy = true;
        else GroundEndLocked(old, GroundCanceled, "canceled");
    }
    g_groundOps.erase(std::remove_if(g_groundOps.begin(), g_groundOps.end(), [](const auto& w) { return w.expired(); }), g_groundOps.end());
    g_groundOps.push_back(op);
    { REG_LOCK;
      for (int uid : uids) { const int i = IndexOfUidLocked(uid); if (i < 0) { busy = true; continue; }
          GroundMemberResult m; m.before = m.after = g_reg[(size_t)i]; op->view.members.push_back(m);
          if (m.before.hidden || !m.before.obj || m.before.standin) busy = true;
      }
    }
    if (busy || uids.empty() || std::set<int>(uids.begin(), uids.end()).size() != uids.size()) GroundEndLocked(op, GroundFailed, "refused");
    else GroundValidLocked(op);
    return op;
}
GroundView GroundStateOf(const GroundHandle& op) { std::lock_guard<std::mutex> lock(g_groundOpMutex); return op->view; }
bool GroundValidate(const GroundHandle& op) { std::lock_guard<std::mutex> lock(g_groundOpMutex); return GroundValidLocked(op); }
void GroundCancel(const GroundHandle& op, const char* reason) {
    std::lock_guard<std::mutex> lock(g_groundOpMutex);
    GroundEndLocked(op, strcmp(reason, "canceled") == 0 ? GroundCanceled : GroundFailed, reason);
}
void GroundFrame() {
    { std::lock_guard<std::mutex> lock(g_groundOpMutex); ++g_groundFrame;
    for (auto& w : g_groundOps) if (auto op = w.lock()) {
        if (op->view.terminal() || op->view.state == GroundApplying) continue;
        if (g_groundFrame - op->born >= 240) GroundEndLocked(op, GroundFailed, "result-timeout");
        else if (!op->ready && g_groundFrame - op->born >= 120) GroundEndLocked(op, GroundFailed, "ready-timeout");
        else if (op->ticket && g_groundFrame - op->ticketAt >= 120) GroundEndLocked(op, GroundFailed, "ticket-timeout");
    } }
    PruneGroundTickets();
}
std::vector<GroundView> GroundBarrier(const std::vector<GroundHandle>& batch, bool cancelUnapplied) {
    std::lock_guard<std::mutex> lock(g_groundOpMutex); std::vector<GroundView> views;
    for (auto& op : batch) { if (cancelUnapplied) GroundEndLocked(op, GroundCanceled, "canceled"); views.push_back(op->view); }
    return views;
}
bool GroundReconcile(const std::vector<GroundHandle>& batch) {
    std::lock_guard<std::mutex> lock(g_groundOpMutex);
    for (auto& op : batch) if (!op->view.terminal()) return false;
    for (auto& op : batch) op->view.state = GroundReconciled;
    return true;
}
uint64_t GroundNotice() { std::lock_guard<std::mutex> lock(g_groundOpMutex); return g_groundNotice; }
bool PublishGroundPlacement(const GroundPlacement& placement) {
    std::lock_guard<std::mutex> lock(g_groundOpMutex);
    if (GroundSamePlacement(placement, g_groundPlacement)) return true;
    for (auto& w : g_groundOps) if (auto op = w.lock())
        if (op->view.carried.generation && op->view.state == GroundApplying) return false;
    for (auto& w : g_groundOps) if (auto op = w.lock())
        if (op->view.carried.generation) GroundEndLocked(op, GroundInvalidated, "epoch-invalidated");
    g_groundPlacement = placement; return true;
}
bool GroundUpdateCarried(const GroundHandle& op, const GroundPlacement& next, std::function<void()> update) {
    std::lock_guard<std::mutex> operation(g_groundOpMutex);
    if (op->view.state != GroundSettled || !GroundSamePlacement(op->view.carried, g_groundPlacement)) return false;
    { REG_LOCK; for (const auto& m : op->view.members) {
        const int i = IndexOfUidLocked(m.after.uid); if (i < 0) return false;
        const auto& e = g_reg[(size_t)i];
        if (e.gen != m.after.gen || e.poseGen != m.after.poseGen || e.hidden != m.after.hidden) return false;
    } }
    update(); g_groundPlacement = next; return true;
}
static void GroundEpochLocked() {
    ++g_groundEpoch;
    for (auto& w : g_groundOps) if (auto op = w.lock()) GroundEndLocked(op, GroundInvalidated, "epoch-invalidated");
}
void InvalidateGroundWorld() { std::lock_guard<std::mutex> lock(g_groundOpMutex); GroundEpochLocked(); }
void RunGroundWorldChange(std::function<void()> change) {
    // Reservation outlives deferral AND the game-queue hop; a server-lane completion must never run the reload.
    struct Reservation {
        bool active = false;
        ~Reservation() { if (active) { std::lock_guard<std::mutex> lock(g_groundOpMutex); --g_groundWorldWriting; } }
    };
    auto reservation = std::make_shared<Reservation>();
    auto run = [reservation, change = std::move(change)]() { change(); };
    { std::lock_guard<std::mutex> lock(g_groundOpMutex);
      ++g_groundWorldWriting; reservation->active = true; GroundEpochLocked();
      if (!GroundBeforeMutationLocked({}, [run]() { RunOnGameThread(run); })) return;
    }
    run(); // already on the game thread; reservation releases even if the callback throws
}

// ---- C5: per-request caller-owned terminal placement results -------------------------------------------
// struct PlaceRequest lives in core.h; these two helpers are the ONLY place a row changes state. They are
// called from the real attach/cancel paths, never while g_regMutex is held (lock order: registry lock is
// released before req->m), so no engine call and no queue push happens under a request lock.
// NoteAttachOnce records every real engine attachment seen for the row (a stand-in counts as an observation)
// and performs the single Pending -> Attached transition. It returns true only for that transition.
static void NoteAttachObservation(const std::weak_ptr<PlaceRequest>& w, int rowId) {
    std::shared_ptr<PlaceRequest> req = w.lock();
    if (!req) return;
    std::lock_guard<std::mutex> l(req->m);
    for (auto& r : req->rows) if (r.rowId == rowId) { ++r.attachObservations; return; }
}
static bool NoteAttachOnce(const std::weak_ptr<PlaceRequest>& w, int rowId, int lane, int uid) {
    std::shared_ptr<PlaceRequest> req = w.lock();
    if (!req) return false;
    std::lock_guard<std::mutex> l(req->m);
    for (auto& r : req->rows) if (r.rowId == rowId) {
        ++r.attachObservations;
        if (r.state != PlacePending) return false;      // no second terminal settlement
        r.state = PlaceAttached; r.lane = lane; r.uid = uid; r.reason.clear();
        return true;
    }
    return false;
}
// Settle one row out of Pending exactly once (Excluded/Failed/Canceled). Returns true only for the transition.
static bool SettleRowOnce(const std::weak_ptr<PlaceRequest>& w, int rowId, int state, int lane, int uid, const std::string& reason) {
    std::shared_ptr<PlaceRequest> req = w.lock();
    if (!req) return false;
    std::lock_guard<std::mutex> l(req->m);
    for (auto& r : req->rows) if (r.rowId == rowId) {
        if (r.state != PlacePending) return false;      // no second terminal settlement
        r.state = state; r.lane = lane; r.uid = uid; r.reason = reason;
        return true;
    }
    return false;
}
void SetGroup(int uid, int group) { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid); if (i >= 0) { g_reg[i].group = group; MarkDirtyLocked(g_reg[i].proj); } }
int NewGroupId() { std::lock_guard<std::mutex> l(g_regMutex); return g_nextGroup++; }
std::string GroupName(int group) { std::lock_guard<std::mutex> l(g_regMutex); auto it = g_groupNames.find(group); return it == g_groupNames.end() ? std::string() : it->second; }
void SetGroupName(int group, const std::string& name) {
    if (group <= 0) return;
    std::lock_guard<std::mutex> l(g_regMutex);
    if (name.empty()) g_groupNames.erase(group); else g_groupNames[group] = name;
    for (const auto& o : g_reg) if (!o.hidden && o.group == group) MarkDirtyLocked(o.proj);
    for (const auto& n : g_npcReg) if (!n.hidden && n.group == group) MarkDirtyLocked(n.proj);
}
void SetObjectNote(int uid, const std::string& note) { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid); if (i >= 0) { g_reg[i].note = note; MarkDirtyLocked(g_reg[i].proj); } }
void SetManagedNpcGroup(int uid, int group) { std::lock_guard<std::mutex> l(g_regMutex); int i = NpcIndexOfUidLocked(uid); if (i >= 0) { g_npcReg[i].group = group; MarkDirtyLocked(g_npcReg[i].proj); } }
void SetManagedNpcNote(int uid, const std::string& note) { std::lock_guard<std::mutex> l(g_regMutex); int i = NpcIndexOfUidLocked(uid); if (i >= 0) { g_npcReg[i].note = note; MarkDirtyLocked(g_npcReg[i].proj); } }
void SetManagedNpcLabel(int uid, const std::string& label) { std::lock_guard<std::mutex> l(g_regMutex); int i = NpcIndexOfUidLocked(uid); if (i >= 0) { g_npcReg[i].label = label; MarkDirtyLocked(g_npcReg[i].proj); } }
// Forget is explicit removal: the record and its uid are gone, and any queued work that still refers to the uid is
// invalidated automatically because the lifetime lookup can no longer find it. The engine object itself is NOT
// removed - the existing semantics (the object stays in the world, untracked) are preserved.
void ForgetUid(int uid) {
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    if (!GroundBeforeMutationLocked({ uid }, [uid]() { ForgetUid(uid); })) return;
    std::weak_ptr<PlaceRequest> reqw; int rowId = -1;
    { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid);
      if (i >= 0) { reqw = g_reg[(size_t)i].placeReq; rowId = g_reg[(size_t)i].placeRow; g_reg[(size_t)i].gen = NewGenLocked(); g_reg.erase(g_reg.begin() + i); ForgetCopyValueLocked(uid); } }
    operation.unlock();
    if (rowId >= 0) SettleRowOnce(reqw, rowId, PlaceCanceled, PlaceLaneNone, uid, "the record was forgotten before it attached");
}
void ForgetManagedNpc(int uid) { std::lock_guard<std::mutex> l(g_regMutex); int i = NpcIndexOfUidLocked(uid); if (i >= 0) g_npcReg.erase(g_npcReg.begin() + i); }

// The game's SceneObject API takes a TiledTransform (44 bytes): scale3, quat4, pos3 (relative to the tile), int16 tile x/z.
// setWorldTransform reads the tile pair at +0x28; the housing code normalizes world -> tile (rva 0x50ea00) right before calling it.
// Up to v0.44 the plugin passed 40 bytes and the tile pair was stack garbage, which threw moved objects into random tiles.
static void QMul(float* o, const float* a, const float* b) {   // Hamilton product, (x, y, z, w)
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
}
static void MakeTransform(float* xf, Vec3 pos, Rot r, float scale, bool tiled = true) {   // tiled=false: plain world transform (createSceneObjectFrom)
    const float k = 3.14159265f / 360.0f, hy = r.yaw * k, hp = r.pitch * k, hr = r.roll * k;
    const float qy[4] = { 0, sinf(hy), 0, cosf(hy) }, qx[4] = { sinf(hp), 0, 0, cosf(hp) }, qz[4] = { 0, 0, sinf(hr), cosf(hr) };
    float t[4], q[4]; QMul(t, qy, qx); QMul(q, t, qz);   // Ry(yaw) * Rx(pitch) * Rz(roll); pitch = roll = 0 gives the old yaw-only quaternion
    xf[0] = xf[1] = xf[2] = scale;
    xf[3] = q[0]; xf[4] = q[1]; xf[5] = q[2]; xf[6] = q[3];
    const int tx = tiled ? (int)(pos.x * 0.001) : 0, tz = tiled ? (int)(pos.z * 0.001) : 0;   // truncation toward zero, like the game (cvttsd2si)
    xf[7] = pos.x - tx * 1000.0f; xf[8] = pos.y; xf[9] = pos.z - tz * 1000.0f;
    const int16_t tile[2] = { (int16_t)tx, (int16_t)tz }; memcpy(&xf[10], tile, 4); xf[11] = 0;
}
using SetWorldTransformFn = void (*)(void* obj, const float* xf, uint8_t a, uint8_t b);
// runs on the game thread
// SceneObject::setEnable(this, bool): toggles bit 3 of the flags at +0xFD and propagates to children and render
// instances. The housing system calls it with 1 after placing a part and with 0 before tearing it down (67 callers).
using SetEnableFn = void (*)(void* obj, uint8_t enable);
bool g_recreateOnMove = true;
bool g_liveDrag = true;   // editor details live drag: the Log tab checkbox and the console "livedrag on/off" both drive this
static bool CheckSO(uintptr_t obj, const char* what) {
    const char* n = RttiName(obj);
    if (!n || !strstr(n, "SceneObject")) { Log("%s: %p is not a SceneObject any more (%s)", what, (void*)obj, n ? n : "?"); return false; }
    return true;
}
static bool DoRemove(uintptr_t obj) {
#ifdef WB_UNIFIED_HOST_TEST
    return obj && host::Seam().remove ? host::Seam().remove(obj) : false;
#else
    if (!CheckSO(obj, "remove")) return false;
    ((SetEnableFn)(g_base + kRva_SetEnable))((void*)obj, 0);
    uint8_t fl = 0; ReadBytes(obj + 0xFD, &fl, 1);
    Log("remove: %p setEnable(0) done, flags+0xFD=0x%02x", (void*)obj, fl);
    return true;
#endif
}
static void* DoSpawn(int uid, uint64_t gen, int lane = PlaceLaneGeneric, MoveCompletion done = {}, const MoveReq* target = nullptr, bool retryStandin = false);
static bool C5RowDead(const std::weak_ptr<PlaceRequest>& req, int rowId);
// Move variant A: disable, set transform, enable (the housing sequence). Variant B: remove + re-create.
static bool DoMoveInPlace(uintptr_t obj, Vec3 pos, Rot rot, float scale) {
#ifdef WB_UNIFIED_HOST_TEST
    return host::Seam().moveInPlace && host::Seam().moveInPlace(obj, pos, rot, scale);
#else
    if (!CheckSO(obj, "move")) return false;
    alignas(16) float xf[12]; MakeTransform(xf, pos, rot, scale);
    auto setEnable = (SetEnableFn)(g_base + kRva_SetEnable);
    setEnable((void*)obj, 0);
    ((SetWorldTransformFn)(g_base + kRva_SetWorldTransform))((void*)obj, xf, 0, 1);
    setEnable((void*)obj, 1);
    Log("move: %p -> (%.2f %.2f %.2f) yaw %.0f tilt %.0f/%.0f scale %.2f (disable/set/enable)", (void*)obj, pos.x, pos.y, pos.z, rot.yaw, rot.pitch, rot.roll, scale);
    return true; // native transform is void: completion, not mere queue admission
#endif
}
extern volatile LONG g_queueCount;

// ---- reading pack files through the game's own resource loader (no decryption in the mod) ----
// ResourceLoader::load(this, Resource** out, const NormalizedPath* path, u32 flags) asks every load worker (package, local dir, ...)
// and returns a Resource whose +0x20 is a MemoryArchive {vtable, u8* data (+8), u32 size (+0x10), u32 pos (+0x14)}.
// The loader instance is captured from the game's own calls (hook on the same function), the path object is built like DoSpawn's.
static uintptr_t kRva_ResLoad = 0;
typedef void* (__fastcall* ResLoadFn)(void* self, void** out, void* path, uint32_t flags);
static void ReleaseHookPiece(); static uintptr_t FindVtableByName(const char* mangled); static uintptr_t FindPatternCount(const char* pat, int* count);
static ResLoadFn g_origResLoad = nullptr; static void* g_resLoader = nullptr;
// Research (/api/research/iotrace {"filter":"height"}): log the game's loads whose path contains the filter, plus the load
// worker's read calls (vtable slots 4 / 5, hooked on first use from the worker the game uses) for those handlers.
static char g_ioFilter[64] = ""; static volatile LONG g_ioLines = 0; static std::mutex g_ioMx; static std::set<uintptr_t> g_ioHandlers;
// research: u16 amount subtracted from every height sample of terrain height DDS files as the game reads them (0 = off)
static volatile LONG g_ioHeightDelta = 0; static std::set<uintptr_t> g_ioHeightHandlers;
typedef void* (__fastcall* IoGen8)(void*, void*, void*, void*, void*, void*, void*, void*);
static IoGen8 g_origWRead[2] = {};
static bool IoHandlerTraced(void* h) { std::lock_guard<std::mutex> l(g_ioMx); return g_ioHandlers.count((uintptr_t)h) != 0; }
static uint32_t IoShiftHeights(uint8_t* buf, uint32_t off, uint32_t len, int dlt) {   // L16 DDS: 128-byte header, then every mip as u16
    uint32_t n = 0;
    CDK_GUARD_BEGIN
        for (uint32_t fo = (off < 128 ? 128 : (off + 1) & ~1u); fo + 2 <= off + len; fo += 2) {
            uint16_t* v = (uint16_t*)(buf + (fo - off)); int x = (int)*v - dlt; *v = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); n++; }
    CDK_GUARD_FAIL
    CDK_GUARD_END
    return n;
}
template<int K> static void* __fastcall HookWorkerRead(void* w, void* h, void* a, void* b, void* c, void* d, void* e, void* f) {
    void* r = g_origWRead[K](w, h, a, b, c, d, e, f);
    if (K == 1 && r && g_ioHeightDelta) {   // slot 5: read(worker, handler, u8* buf, u32 cap, u32 offset, u32 length)
        const uint32_t off = (uint32_t)(uintptr_t)c, cap = (uint32_t)(uintptr_t)b; uint32_t len = (uint32_t)(uintptr_t)d; if (!len) len = cap;
        bool mine; { std::lock_guard<std::mutex> l(g_ioMx); mine = g_ioHeightHandlers.erase((uintptr_t)h) != 0; }   // one whole-file read per
        if (mine && (off != 0 || len < 0x10000)) mine = false;                                                      // handler, then forgotten (addresses are reused)
        if (mine && a && len) { const uint32_t n = IoShiftHeights((uint8_t*)a, off, len, (int)g_ioHeightDelta);
            Log("[io] height patch: handler %p off %u len %u, %u samples shifted by %d", h, off, len, n, -(int)g_ioHeightDelta); }
    }
    if (g_ioFilter[0] && InterlockedIncrement(&g_ioLines) < 2000 && IoHandlerTraced(h)) {
        const uintptr_t ret = (uintptr_t)_ReturnAddress();
        Log("[io] slot %d read: handler %p args %p %p %p %p -> %p (from 0x%llx, thread %lu)", K ? 5 : 4, h, a, b, c, d, r,
            InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId());
    }
    return r;
}
static std::string PathText(void* path) {   // NormalizedPath: first field points at the text or at a pointer to it
    char s[200] = { 0 }; uintptr_t p0 = 0;
    if (!ReadPtr((uintptr_t)path, &p0) || !p0) return "";
    for (int hop = 0; hop < 2; hop++) {
        if (ReadBytes(p0, s, sizeof s - 1)) { size_t n = 0; while (n < sizeof s - 1 && s[n] >= 0x20 && s[n] < 0x7f) n++;
            if (n >= 4 && (s[n] == 0)) return std::string(s, n); }
        if (!ReadPtr(p0, &p0) || !p0) break;
    }
    return "";
}
static void __fastcall IoHookWorker(uintptr_t worker) {
    static bool s_done = false; if (s_done || !worker) return;
    uintptr_t vt = 0; if (!ReadPtr(worker, &vt) || !InImage(vt)) return;
    s_done = true;
    for (int k = 0; k < 2; k++) {
        uintptr_t fn = 0; if (!ReadPtr(vt + (k ? 5 : 4) * 8, &fn) || !InImage(fn)) continue;
        ReleaseHookPiece();
        void* det = k ? (void*)&HookWorkerRead<1> : (void*)&HookWorkerRead<0>;
        if (MH_CreateHook((void*)fn, det, (void**)&g_origWRead[k]) == MH_OK && MH_EnableHook((void*)fn) == MH_OK) Log("[io] worker slot %d hooked (rva 0x%llx)", k ? 5 : 4, (unsigned long long)(fn - g_base));
        else Log("[io] hooking worker slot %d failed", k ? 5 : 4);
    }
}
static void* __fastcall HookResLoad(void* self, void** out, void* path, uint32_t flags) {
    if (!g_resLoader && self) { g_resLoader = self; Log("resource loader captured %p (%s)", self, RttiName((uintptr_t)self) ? RttiName((uintptr_t)self) : "?"); }
    alignas(16) uint8_t missing[64];   // play mode: a level file of the isolated world is loaded from an empty level file instead
    if (PlayModeActive() && PlayModeOnResLoad(PathText(path)) && kRva_StringDataAlloc && kRva_PathNormalizeCtor) {
        const char* none = PlayModeEmptyLevel(); const size_t len = strlen(none);   // an empty level the game ships (playmode.cpp)
        auto sdAlloc = (uintptr_t(*)(int))(g_base + kRva_StringDataAlloc); auto normalize = (void*(*)(void*, const void*))(g_base + kRva_PathNormalizeCtor);
        memset(missing, 0, sizeof missing); uintptr_t sd = sdAlloc((int)len);
        if (sd) { strncpy_s((char*)*(uintptr_t*)sd, len + 1, none, _TRUNCATE); uintptr_t holder = sd; normalize(missing, &holder); path = missing; }
    }
    void* r = g_origResLoad(self, out, path, flags);
    if (g_ioFilter[0] && g_ioLines < 2000) {
        const std::string s = PathText(path);
        if (!s.empty() && s.find(g_ioFilter) != std::string::npos) {
            InterlockedIncrement(&g_ioLines);
            uintptr_t h = 0, worker = 0; uint32_t s34 = 0, s38 = 0; uint8_t fl = 0; ReadPtr((uintptr_t)out, &h);
            if (h) { ReadPtr(h + 0x20, &worker); ReadBytes(h + 0x34, &s34, 4); ReadBytes(h + 0x38, &s38, 4); ReadBytes(h + 0x3c, &fl, 1); }
            const uintptr_t ret = (uintptr_t)_ReturnAddress();
            Log("[io] load %s flags 0x%x -> handler %p (%s) worker %p sizes %u/%u fl 0x%02x (from 0x%llx, thread %lu)", s.c_str(), flags, (void*)h,
                h && RttiName(h) ? RttiName(h) : "?", (void*)worker, s34, s38, fl, InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId());
            if (h) { std::lock_guard<std::mutex> l(g_ioMx); g_ioHandlers.insert(h); }
            IoHookWorker(worker);
        }
    }
    if (g_ioHeightDelta) {   // research: remember the handlers of terrain height files for the read patch above
        const std::string s = PathText(path); uintptr_t h = 0; ReadPtr((uintptr_t)out, &h);
        if (h && s.find("/height16f/") == std::string::npos) { std::lock_guard<std::mutex> l(g_ioMx); g_ioHeightHandlers.erase(h); }
        if (h && s.find("/height16f/") != std::string::npos) { { std::lock_guard<std::mutex> l(g_ioMx); g_ioHeightHandlers.insert(h); }
            uintptr_t worker = 0; ReadPtr(h + 0x20, &worker); IoHookWorker(worker);
        }
    }
    return r;
}
// Helpers for the optional modules (terrain.cpp): unique signature scan, RTTI vtable lookup, NormalizedPath text.
uintptr_t SigScanUnique(const char* pat) { int n = 0; const uintptr_t f = FindPatternCount(pat, &n); return n == 1 ? f : 0; }
uintptr_t VtableByName(const char* mangled) { return FindVtableByName(mangled); }
std::string PathObjText(void* path) { return PathText(path); }
void IoHeightDelta(int d) { g_ioHeightDelta = d; Log("[io] height read patch %d", d); }
void IoTraceSet(const std::string& filter) {
    strncpy_s(g_ioFilter, filter.c_str(), _TRUNCATE); g_ioLines = 0;
    { std::lock_guard<std::mutex> l(g_ioMx); g_ioHandlers.clear(); }
    Log("[io] load trace filter '%s'", g_ioFilter);
}
static bool GameReadFileGuarded(void* pathObj, std::vector<uint8_t>* out, char* rtti, size_t rttiLen, bool* notFound, uint32_t offset, uint32_t length, uint32_t* storedTotal) {
    // load() returns a ResourceHandler_Paz: +0x20 worker (ResourceLoadWorker_Package), +0x34 / +0x38 sizes, +0x3c flags
    // (low nibble compression: 0 none, 1 partial, 2 LZ4; high nibble crypto). The worker's slot 5 reads, decrypts and
    // decompresses the entry into a caller buffer: read(worker, handler, u8* buf, u32 capacity, u32 offset, u32 length).
    void* res = nullptr;
    CDK_GUARD_BEGIN
        g_origResLoad(g_resLoader, &res, pathObj, 0);
        if (!res) { *notFound = true; return false; }
        const uintptr_t h = (uintptr_t)res, worker = *(uintptr_t*)(h + 0x20);
        const uint8_t fl = *(uint8_t*)(h + 0x3c); const uint32_t s34 = *(uint32_t*)(h + 0x34), s38 = *(uint32_t*)(h + 0x38);
        const uint32_t need = ((fl & 0xF) == 1) ? s34 : s38, cap = s34 > s38 ? s34 : s38;
        bool ok = false;
        if (rtti) snprintf(rtti, rttiLen, "%s / worker %s, flags 0x%02x, sizes %u/%u", RttiName(h) ? RttiName(h) : "?", worker && RttiName(worker) ? RttiName(worker) : "?", fl, s34, s38);
        if (storedTotal) *storedTotal = need;
        if (worker && need && length) {   // a range of the stored entry (texture tails): offset/length as read() takes them
            if (offset < need) {
                const uint32_t len = length < need - offset ? length : need - offset; out->resize(len);
                auto read5 = *(uint8_t(__fastcall**)(void*, void*, void*, uint32_t, uint32_t, uint32_t))(*(uintptr_t*)worker + 0x28);
                ok = read5((void*)worker, res, out->data(), len, offset, len) != 0;
                if (!ok) out->clear();
            }
        } else if (worker && need && cap < (512u << 20)) {
            out->resize(cap);
            auto read5 = *(uint8_t(__fastcall**)(void*, void*, void*, uint32_t, uint32_t, uint32_t))(*(uintptr_t*)worker + 0x28);
            ok = read5((void*)worker, res, out->data(), cap, 0, 0) != 0;
            if (ok) out->resize(need); else out->clear();
            static int s_zeroLogged = 0;   // success with an untouched buffer (seen while the game streams): which handler and flags, for the next bug report
            if (ok && s_zeroLogged < 3) { bool zero = true; for (size_t i = 0; i < out->size() && i < 64; i++) if ((*out)[i]) { zero = false; break; }
                if (zero) { s_zeroLogged++; Log("game loader: read reported success but left the buffer empty (%s / worker %s, flags 0x%02x, sizes %u/%u)",
                    RttiName(h) ? RttiName(h) : "?", worker && RttiName(worker) ? RttiName(worker) : "?", fl, s34, s38); } }
        }
        (*(void(__fastcall**)(void*, int))(*(uintptr_t*)res))(res, 1);   // handler release, as the game's load() does
        return ok;
    CDK_GUARD_FAIL return false;
    CDK_GUARD_END
}
bool GameReadAvailable() { return g_resLoader && g_origResLoad && kRva_StringDataAlloc && kRva_PathNormalizeCtor; }
bool GameReadFileRange(const std::string& path, std::vector<uint8_t>& out, uint32_t offset, uint32_t length, uint32_t* storedTotal, bool* notFound);
bool GameReadFile(const std::string& path, std::vector<uint8_t>& out, bool* notFound) { return GameReadFileRange(path, out, 0, 0, nullptr, notFound); }
bool GameReadFileRange(const std::string& path, std::vector<uint8_t>& out, uint32_t offset, uint32_t length, uint32_t* storedTotal, bool* notFound) {
    bool nf = false; if (notFound) *notFound = false;
    if (!GameReadAvailable()) return false;
    auto sdAlloc = (uintptr_t(*)(int))(g_base + kRva_StringDataAlloc);
    auto normalize = (void*(*)(void*, const void*))(g_base + kRva_PathNormalizeCtor);
    alignas(16) uint8_t pathObj[64] = { 0 };
    uintptr_t sd = sdAlloc((int)path.size()); if (!sd) return false;
    strncpy_s((char*)*(uintptr_t*)sd, path.size() + 1, path.c_str(), _TRUNCATE);
    uintptr_t holder = sd; normalize(pathObj, &holder);
    static bool s_logged = false; char rtti[160] = { 0 };
    bool ok = GameReadFileGuarded(pathObj, &out, s_logged ? nullptr : rtti, sizeof rtti, &nf, offset, length, storedTotal);
    if (notFound) *notFound = nf;
    if (!s_logged) { s_logged = true; Log("game loader first read: %s -> %s, %zu bytes (%s)", path.c_str(), ok ? "ok" : "FAILED", out.size(), rtti); }
    return ok;
}

// ---- game call tracing (reverse engineering aid, console "trace on|off"): logs the game's own setWorldTransform / setEnable calls
// with the caller RVA, every object the game creates, and transform changes of the most recently created game object per tick.
static SetWorldTransformFn g_origSetXf = nullptr; static SetEnableFn g_origSetEnable = nullptr;
static std::map<uintptr_t, long> g_traceCallers; static long g_traceLines = 0; static DWORD g_traceSec = 0;
static bool TraceBudget() { DWORD s = GetTickCount() / 1000; if (s != g_traceSec) { g_traceSec = s; g_traceLines = 0; } return g_traceLines++ < 80; }
static float g_fcSoLast[11] = {}; static volatile bool g_fcSoSeen = false;   // the scene object's last game pose (free camera)
static volatile uintptr_t g_fcSceneObj = 0;   // the camera scene object while the free camera is on (set once per frame by the pose hook)
static bool FreeCamSceneXf(const float* in, float* out);   // free camera section below
static void __fastcall HookSetXf(void* obj, const float* xf, uint8_t a, uint8_t b) {
    // the camera manager holds the scene object 0x28 into it (a base subobject); setWorldTransform gets the object itself
    if (g_fcSceneObj && ((uintptr_t)obj == g_fcSceneObj - 0x28 || (uintptr_t)obj == g_fcSceneObj) && xf) {   // the game camera moves its scene object every frame: culling, LOD and sound follow it
        alignas(16) float t[12]; if (FreeCamSceneXf(xf, t)) { g_origSetXf(obj, t, a, b); return; }
    }
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    if (g_trace && InImage(ret)) {
        g_traceCallers[ret - g_base]++;
        float t[10] = { 0 }; ReadBytes((uintptr_t)xf, t, 40);
        if (TraceBudget()) Log("[trace] setWorldTransform obj=%p (%s) from rva 0x%llx flags=%d,%d pos=(%.2f %.2f %.2f) scale=%.2f", obj,
            RttiName((uintptr_t)obj) ? RttiName((uintptr_t)obj) : "?", (unsigned long long)(ret - g_base), a, b, t[7], t[8], t[9], t[0]);
    }
    g_origSetXf(obj, xf, a, b);
}
static void __fastcall HookSetEnable(void* obj, uint8_t enable) {
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    if (g_trace && InImage(ret)) {
        g_traceCallers[(ret - g_base) | 0x8000000000000000ull]++;
        if (TraceBudget()) Log("[trace] setEnable obj=%p (%s) from rva 0x%llx enable=%d", obj, RttiName((uintptr_t)obj) ? RttiName((uintptr_t)obj) : "?", (unsigned long long)(ret - g_base), enable);
    }
    g_origSetEnable(obj, enable);
}
static void TraceTick() {
    static uintptr_t s_obj = 0; static float s_last[3] = { 0, 0, 0 };
    uintptr_t obj = g_lastGameObj; if (!obj) return;
    float xf[10]; if (!ReadBytes(obj + 0x1A4, xf, 40)) return;
    if (obj != s_obj || fabsf(xf[7] - s_last[0]) > 0.001f || fabsf(xf[8] - s_last[1]) > 0.001f || fabsf(xf[9] - s_last[2]) > 0.001f) {
        if (TraceBudget()) Log("[trace] tick %ld: game obj %p (%s) +0x1A4 pos=(%.2f %.2f %.2f) scale=%.2f", g_pumpTicks, (void*)obj,
            RttiName(obj) ? RttiName(obj) : "?", xf[7], xf[8], xf[9], xf[0]);
        s_obj = obj; s_last[0] = xf[7]; s_last[1] = xf[8]; s_last[2] = xf[9];
    }
}
static void DumpServerFieldCounts();
void SetTrace(bool on) {
    g_trace = on; Log("[trace] %s", on ? "ON: drop an item from the inventory or place something in the housing editor now, then trace off" : "off");
    if (!on) { for (auto& kv : g_traceCallers) Log("[trace] caller rva 0x%llx (%s): %ld calls", (unsigned long long)(kv.first & 0x7FFFFFFFFFFFFFFFull), (kv.first >> 63) ? "setEnable" : "setWorldTransform", kv.second); g_traceCallers.clear();
        DumpServerFieldCounts(); }
}
bool Trace() { return g_trace; }
// live dragging: selectable method (console: livemode N) 0 = disable/setTransform/enable, 1 = setTransform(0,0), 2 = setTransform(0,1), 3 = setTransform(0,0)+enable
// 1 = transform only: on build 2944 the disable/enable sequence (mode 0) hides the object and the re-add happens asynchronously,
// so an object that is updated every frame never comes back. The final drop always re-creates (g_recreateOnMove) for a clean state.
// 2 = setWorldTransform(0,1): remove + re-insert per update, visible but may flicker; 1 = (0,0) leaves the object invisible until
// re-inserted; 0 = disable/enable hides it (async re-add). Release/drop always re-creates.
int g_liveMode = 2;
static void DoLiveMove(uintptr_t obj, Vec3 pos, Rot rot, float scale, DWORD queuedAt) {
#ifdef WB_UNIFIED_HOST_TEST
    const DWORD wait = GetTickCount() - queuedAt;
    if (wait > 100) Log("live move: job waited %lu ms in the game-thread queue (pump ticks %ld)", wait, g_pumpTicks);
    if (host::Seam().liveMove) host::Seam().liveMove(obj, pos, rot, scale);
#else
    if (!CheckSO(obj, "live")) return;
    const DWORD wait = GetTickCount() - queuedAt;
    if (wait > 100) Log("live move: job waited %lu ms in the game-thread queue (pump ticks %ld)", wait, g_pumpTicks);
    alignas(16) float xf[12]; MakeTransform(xf, pos, rot, scale);
    auto setEnable = (SetEnableFn)(g_base + kRva_SetEnable);
    auto setXf = (SetWorldTransformFn)(g_base + kRva_SetWorldTransform);
    switch (g_liveMode) {
    case 1: setXf((void*)obj, xf, 0, 0); break;
    case 2: setXf((void*)obj, xf, 0, 1); break;
    case 3: setXf((void*)obj, xf, 0, 0); setEnable((void*)obj, 1); break;
    default: setEnable((void*)obj, 0); setXf((void*)obj, xf, 0, 1); setEnable((void*)obj, 1); break;
    }
#endif
}
// A final move that must re-create: remove the record's current handle and materialize its next incarnation.
// The renewal of the generation invalidates every older completion of this record (including the caller's own).
// The handle stays registered until this job runs, so a hide/forget/restore that happens in between still disposes
// it (and makes the generation stale, dropping this job).
static void DoReplace(int uid, uint64_t gen, MoveCompletion done = {}, const MoveReq* target = nullptr) {
    uintptr_t old = 0;
    { REG_LOCK; if (!GenCurrentLocked(uid, gen)) return; old = g_reg[(size_t)IndexOfUidLocked(uid)].obj; }
    if (old && !DoRemove(old) && done) { done(false); return; } // actual refusal; no fictitious replacement
    uint64_t next = 0;
    { REG_LOCK; if (!GenCurrentLocked(uid, gen)) return;   // a forget/hide/restore replaced the incarnation while removing
      SpawnedObj& e = g_reg[(size_t)IndexOfUidLocked(uid)];
      e.obj = 0; e.gen = next = NewGenLocked(); }
    DoSpawn(uid, next, PlaceLaneGeneric, std::move(done), target); // same production attachment path, carrying the lease
}
// game thread: apply the record's own pose to its current handle. A move admitted before a hide/restore/
// re-create is dropped here instead of touching a newer incarnation.
static void ApplyMove(int uid, uint64_t gen, bool final, MoveCompletion done = {}, const MoveReq* target = nullptr) {
    uintptr_t obj = 0; Vec3 pos; Rot rot; float scale = 1;
    { REG_LOCK; const int i = IndexOfUidLocked(uid);
      if (i < 0 || g_reg[(size_t)i].gen != gen || g_reg[(size_t)i].hidden) return;
      const SpawnedObj& e = g_reg[(size_t)i]; obj = e.obj; pos = e.pos; rot = e.rot; scale = e.scale; }
    if (!obj) { if (done) done(false); return; }
    if (target) { pos = target->pos; rot = target->rot; scale = target->scale; }
    if (final) { const bool accepted = DoMoveInPlace(obj, pos, rot, scale); if (done) done(accepted); }
    else DoLiveMove(obj, pos, rot, scale, GetTickCount());
}
static void MoveGimmickJob(int uid, uint64_t gen, bool final, MoveCompletion done = {}, const MoveReq* target = nullptr);   // interactive lane (defined below)
static bool RotDiffers(const Rot& a, const Rot& b) { return fabsf(a.yaw - b.yaw) > 0.01f || fabsf(a.pitch - b.pitch) > 0.01f || fabsf(a.roll - b.roll) > 0.01f; }
// Admission of one move on the caller thread, with the registry lock held: the record's pose becomes the
// requested one and the returned job runs on the game thread. Only a move that replaces the physical object
// (final re-create, or the interactive lane switching between server object and stand-in) renews the
// generation; a pose-only move keeps it, and its job still rechecks it at execution.
static std::function<void()> AdmitMoveLocked(size_t i, Vec3 pos, Rot rot, float scale, bool final, MoveCompletion done = {}) {
    SpawnedObj& e = g_reg[i];
    const int uid = e.uid;
    // Ordinary edits retain their upstream admission semantics. A GroundOp publishes no successful
    // pose until its actual engine outcome: the same lanes carry a private target under its lease.
    if (!done) { e.pos = pos; e.rot = rot; e.scale = scale; }
    ++e.poseGen;
    const MoveReq target{ uid, pos, rot, scale };
    if (final && !done) MarkDirtyLocked(e.proj);
    if (e.gimmick) {
        if (final || !e.standin) e.gen = NewGenLocked();
        const uint64_t gen = e.gen;
        return [uid, gen, final, done, target]() { MoveGimmickJob(uid, gen, final, done, done ? &target : nullptr); };
    }
    const bool recreate = final && (g_recreateOnMove || RotDiffers(e.colRot, rot) || fabsf(e.colScale - scale) > 0.001f || !e.obj);
    const uint64_t gen = e.gen;
    const uint64_t pose = e.poseGen;
    return [uid, gen, pose, final, recreate, done, target]() {
        { REG_LOCK; const int ix = IndexOfUidLocked(uid);
          if (ix < 0 || g_reg[(size_t)ix].poseGen != pose || !GenCurrentLocked(uid, gen)) return; }
        if (recreate) DoReplace(uid, gen, done, done ? &target : nullptr); else ApplyMove(uid, gen, final, done, done ? &target : nullptr);
    };
}
static bool GroundDestinationValid(const GroundHandle& op, const std::vector<MoveReq>& moves);
static void GroundMovementFinished(const GroundHandle& op, size_t member, bool accepted) {
    std::vector<GroundDeferred> dispatch;
    { std::lock_guard<std::mutex> lock(g_groundOpMutex);
      auto& m = op->view.members[member];
      if (op->view.state != GroundApplying || m.terminal) return;
      { REG_LOCK; const int i = IndexOfUidLocked(m.before.uid);
        if (i >= 0) {
            auto& e = g_reg[(size_t)i];
            if (accepted) { e.pos = m.requested.pos; e.rot = m.requested.rot; e.scale = m.requested.scale; }
            else if (!e.obj) e.hidden = true; // removal completed but re-materialization failed/faulted
            if (accepted || e.hidden != m.before.hidden) MarkDirtyLocked(e.proj);
            m.after = e; // failure retains real hidden/handle/materialization outcome, not a fake successful pose
        }
      }
      m.terminal = true; m.accepted = accepted; m.reason = accepted ? "applied" : "move-refused";
      size_t finished = 0, moved = 0;
      for (const auto& row : op->view.members) { finished += row.terminal; moved += row.accepted; }
      if (finished != op->view.members.size()) return;
      op->view.state = GroundSettled;
      op->view.reason = moved == finished ? "applied" : moved ? "partial" : "move-refused";
      ++g_groundNotice; // publish the ACTUAL result before lease release, dispatch or optional UI delivery
      for (const auto& row : op->view.members) g_groundLeases.erase(row.before.uid);
      for (auto it = g_groundDeferred.begin(); it != g_groundDeferred.end();) {
          bool busy = false; for (int uid : it->targets) busy |= g_groundLeases.count(uid) != 0;
          if (busy) { ++it; continue; }
          dispatch.push_back(std::move(*it)); it = g_groundDeferred.erase(it);
      }
    }
    for (auto& d : dispatch) {
        d.action(); // no operation, registry or queue lock; target reservation prevents overtaking GroundOps
        std::lock_guard<std::mutex> lock(g_groundOpMutex);
        for (int uid : d.targets) if (--g_groundMutationPending[uid] == 0) g_groundMutationPending.erase(uid);
    }
}
static bool RunGroundMemberGuarded(std::function<void()>* job) {
    CDK_GUARD_BEGIN (*job)(); return true;
    CDK_GUARD_FAIL Log("[ground] member engine call faulted 0x%08lx", cdk::GuardCode()); return false;
    CDK_GUARD_END
}
bool GroundApply(const GroundHandle& op, const std::vector<MoveReq>& moves) {
    { std::lock_guard<std::mutex> lock(g_groundOpMutex);
      if (op->view.state != GroundProbing || !GroundValidLocked(op)) return false;
      bool valid = GameThreadReady() && moves.size() == op->view.members.size();
      float dy = 0;
      for (size_t i = 0; valid && i < moves.size(); ++i) {
          const auto& r = moves[i]; const auto& b = op->view.members[i].before;
          if (!i) dy = r.pos.y - b.pos.y;
          valid = r.uid == b.uid && std::isfinite(r.pos.y) && std::isfinite(dy) &&
                  r.pos.x == b.pos.x && r.pos.z == b.pos.z && r.scale == b.scale &&
                  r.rot.yaw == b.rot.yaw && r.rot.pitch == b.rot.pitch && r.rot.roll == b.rot.roll &&
                  fabsf((r.pos.y - b.pos.y) - dy) < 0.001f;
      }
      if (!valid || !GroundDestinationValid(op, moves)) { GroundEndLocked(op, GroundFailed, "refused"); return false; }
      for (size_t i = 0; i < moves.size(); ++i) op->view.members[i].requested = moves[i];
      op->view.state = GroundReady;
      op->view.state = GroundQueued;
    }
    RunOnGameThread([op]() {
        std::vector<std::function<void()>> jobs;
        { std::lock_guard<std::mutex> lock(g_groundOpMutex);
          if (op->view.state != GroundQueued || !GroundValidLocked(op)) return;
          // FINAL authority: the whole rigid set is checked before ANY member is admitted. A teleport,
          // live move, hide/restore, new carried transform or replaced member invalidates this point.
          op->view.state = GroundApplying;
          for (const auto& m : op->view.members) g_groundLeases[m.before.uid] = op;
          { REG_LOCK;
            for (size_t i = 0; i < op->view.members.size(); ++i) {
                const auto& r = op->view.members[i].requested;
                jobs.push_back(AdmitMoveLocked((size_t)IndexOfUidLocked(r.uid), r.pos, r.rot, r.scale, true,
                    [op, i](bool accepted) { GroundMovementFinished(op, i, accepted); }));
            }
          }
        }
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (!RunGroundMemberGuarded(&jobs[i])) GroundMovementFinished(op, i, false);
        } // existing generic/recreate/server lanes carry the same lease to completion
    });
    return true;
}

// final=false: live drag, visual only (disable/setTransform/enable). final=true: if rotation or scale changed since the
// object was created, re-create it so the collision shape (built at creation) matches; otherwise move in place.
// Legacy index entry point: the index resolves to the logical identity HERE, and only (uid, generation) is
// queued, so a registry shift after this call can never move a different record.
bool MoveSpawned(size_t idx, Vec3 pos, Rot rot, float scale, bool final) {
    if (!GameThreadReady()) return false;
    if (!final && InterlockedCompareExchange(&g_queueCount, 0, 0) > 2) return true; // upstream index API: dropped live update is true
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    int uid;
    { REG_LOCK; if (idx >= g_reg.size() || g_reg[idx].hidden) return false; uid = g_reg[idx].uid; }
    if (!GroundBeforeMutationLocked({ uid }, [uid, pos, rot, scale, final]() { MoveMany({ { uid, pos, rot, scale } }, final); })) return true;
    std::function<void()> job;
    { REG_LOCK; job = AdmitMoveLocked((size_t)IndexOfUidLocked(uid), pos, rot, scale, final); }
    operation.unlock(); RunOnGameThread(std::move(job)); return true;
}
bool MoveMany(const std::vector<MoveReq>& reqs, bool final) {
    if (!GameThreadReady() || reqs.empty()) return false;
    if (!final && InterlockedCompareExchange(&g_queueCount, 0, 0) > 2) return false;   // drop live updates when the game thread lags
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    std::vector<int> ids; for (const auto& r : reqs) ids.push_back(r.uid);
    if (!GroundBeforeMutationLocked(ids, [reqs, final]() { MoveMany(reqs, final); })) return true;
    std::vector<std::function<void()>> jobs;
    { REG_LOCK; for (const auto& r : reqs) { const int i = IndexOfUidLocked(r.uid); if (i >= 0 && !g_reg[(size_t)i].hidden) jobs.push_back(AdmitMoveLocked((size_t)i, r.pos, r.rot, r.scale, final)); } }
    operation.unlock();
    RunOnGameThread([jobs]() { for (auto& job : jobs) job(); });
    return true;
}
bool HideSpawned(size_t idx) { int uid = 0; { REG_LOCK; if (idx < g_reg.size()) uid = g_reg[idx].uid; } return uid && HideUid(uid); }
// HideUid with an optional completion that runs on the SAME lane as the actual removal, AFTER it dispatched.
// When the hide is deferred behind an Applying lease the completion travels with the deferred action, so a
// caller-visible cleanup can never settle before the member's engine removal has executed.
static bool HideUidInternal(int uid, std::function<void()> removed = {});
// A deferred hide that can no longer perform the removal must still complete the caller's cleanup, otherwise
// the outstanding mark could never clear. Returns true only when the hide actually ran or was deferred again.
static void HideUidDeferred(int uid, std::function<void()> removed) {
    const bool hid = HideUidInternal(uid, removed);
    if (!hid && removed) removed();
}
static bool HideUidInternal(int uid, std::function<void()> removed) {
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    if (!GroundBeforeMutationLocked({ uid }, [uid, removed]() { HideUidDeferred(uid, removed); })) return true;
    uintptr_t obj = 0, actor = 0, standinObj = 0; bool gimmick = false; std::weak_ptr<PlaceRequest> reqw; int rowId = -1;
    {   // an interactive object: its actor is removed on the server tick, the way the game removes a picked-up item
        REG_LOCK; const int idx = IndexOfUidLocked(uid); if (idx < 0 || g_reg[(size_t)idx].hidden) return false;
        SpawnedObj& e = g_reg[(size_t)idx];
        gimmick = e.gimmick; actor = e.gimmick ? e.actor : 0; obj = e.obj; standinObj = e.standin ? e.obj : 0;
        reqw = e.placeReq; rowId = e.placeRow; uid = e.uid;
        e.hidden = true; e.obj = 0; e.actor = 0; e.standin = false;
        e.gen = NewGenLocked();   // every in-flight completion of the hidden incarnation is invalidated
        MarkDirtyLocked(e.proj); }
    operation.unlock();
    // C5: a row hidden before it attached is a cancel of that unapplied row (an already Attached row keeps its receipt)
    if (rowId >= 0) SettleRowOnce(reqw, rowId, PlaceCanceled, PlaceLaneNone, uid, "the placement was cancelled or hidden before it attached");
    // engine and server calls happen outside the registry lock; the completion runs right after the removal on its own lane
    if (gimmick) {
        const bool serverLane = actor != 0;   // the actor removal is the visible member, so its lane owns the completion
        if (serverLane) RunOnServerTick([actor, removed]() { RemoveSpawnedActor(actor); if (removed) removed(); });
        if (standinObj && GameThreadReady()) {
            if (!serverLane) RunOnGameThread([standinObj, removed]() { DoRemove(standinObj); if (removed) removed(); });
            else RunOnGameThread([standinObj]() { DoRemove(standinObj); });
        } else if (!serverLane && removed) removed();   // nothing visible to remove: the cleanup is already complete
        return true;
    }
    if (!GameThreadReady()) return false;
    if (obj) RunOnGameThread([obj, removed]() { DoRemove(obj); if (removed) removed(); });
    else if (removed) removed();                        // hidden record with no materialized object: nothing to remove
    return true;
}
bool HideUid(int uid) { return HideUidInternal(uid); }
// C3: bring a hidden logical record back to life with a fresh physical generation. The uid and every record-owned
// value (project, group, pose, prefab, interactive lane) stay exactly as the record carries them - never an older
// History snapshot - so a later AssignProject/SaveProject adoption survives undo/redo. false when the record was
// explicitly forgotten, is already materialized, or the game thread is not ready.
bool RestoreUid(int uid) {
    if (!GameThreadReady()) return false;
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    if (!GroundBeforeMutationLocked({ uid }, [uid]() { RestoreUid(uid); })) return true;
    uint64_t gen = 0; bool gim = false; std::string prefab; Vec3 pos; Rot rot; float scale = 1;
    std::weak_ptr<PlaceRequest> request; int row = -1;
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = IndexOfUidLocked(uid);
      if (i < 0) return false;
      SpawnedObj& e = g_reg[(size_t)i];
      if (!e.hidden) return false;                 // still materialized (or never hidden): nothing to restore
      e.hidden = false; e.standin = false; e.actor = 0; e.obj = 0;
      e.gen = gen = NewGenLocked();                // fresh physical generation for this incarnation
      MarkDirtyLocked(e.proj);
      gim = e.gimmick; prefab = e.prefab; pos = e.pos; rot = e.rot; scale = e.scale; request = e.placeReq; row = e.placeRow; }
    operation.unlock();
    if (C5RowDead(request, row)) {
        // A restored incarnation is new work, not a resurrection of its canceled/failed placement receipt.
        REG_LOCK; const int i = IndexOfUidLocked(uid);
        if (i >= 0 && g_reg[(size_t)i].gen == gen) { g_reg[(size_t)i].placeReq.reset(); g_reg[(size_t)i].placeRow = -1; }
    }
    if (gim && IsGimmickPrefab(prefab)) {
        EnqueueGimmick(uid, gen, prefab, pos, rot, scale);   // server tick, once a template exists
        return true;
    }
    RunOnGameThread([uid, gen]() { DoSpawn(uid, gen); });
    return true;
}

// ---- our own spawn (must run on the game thread) ----
static uintptr_t SceneObjectMgr() {
    uintptr_t g = Deref(g_base + kRva_WorldGlobal, 0);
    uintptr_t w = g ? Deref(g, 0xE0) : 0;
    return w ? Deref(w, 0xEB0) : 0;
}
static uint8_t g_flags[3] = { 1, 1, 0 };   // middle flag = schedule the add-to-level task (what every dynamic spawner in the game uses)

// Raw engine creation of one scene object (the createSceneObjectFrom call). Only this boundary is
// substituted by a host fixture; the spawn lifecycle around it stays production code.
static uintptr_t CreateGenericSceneObject(const std::string& prefab, Vec3 pos, Rot rot, float scale) {
#ifdef WB_UNIFIED_HOST_TEST
    return host::Seam().createGeneric ? host::Seam().createGeneric(prefab, pos, rot, scale) : 0;
#else
    if (!g_origCreate) { Log("spawn: hook not installed"); return 0; }
    uintptr_t mgr = SceneObjectMgr();
    if (!mgr && g_lastMgr) mgr = (uintptr_t)g_lastMgr;
    if (!mgr) { Log("spawn: no SceneObjectManager"); return 0; }
    auto rrpCtor = (void*(*)(void*, const void*))(g_base + kRva_PrefabPathCtor);
    auto sdAlloc = (uintptr_t(*)(int))(g_base + kRva_StringDataAlloc);
    auto normalize = (void*(*)(void*, const void*))(g_base + kRva_PathNormalizeCtor);
    alignas(16) uint8_t tag[32] = {0}, pathStr[32] = {0}, rrp[0x100] = {0}, arg3[64] = {0}, arg4[64] = {0}, tagBlock[0x80] = {0};
    *(uintptr_t*)tag = (uintptr_t)tagBlock;                    // tag = pointer to zeroed block (what the sector loader passes)
    uintptr_t sd = sdAlloc((int)prefab.size());                // game StringData: {char* str; i32 len; i32 hash=-1; i32 rc=1; ...}
    if (!sd) { Log("spawn: StringData alloc failed"); return 0; }
    strncpy_s((char*)*(uintptr_t*)sd, prefab.size() + 1, prefab.c_str(), _TRUNCATE);
    uintptr_t holder = sd;
    normalize(pathStr, &holder);
    rrpCtor(rrp, pathStr);
    alignas(16) float xf[12]; MakeTransform(xf, pos, rot, scale, false);   // creation takes world coordinates
    g_inOurSpawn = true;
    void* r = g_origCreate((void*)mgr, tag, arg3, arg4, rrp, xf, g_flags[0], g_flags[1], g_flags[2]);
    g_inOurSpawn = false;
    Log("spawn: \"%s\" at (%.2f %.2f %.2f) yaw %.0f tilt %.0f/%.0f scale %.2f -> %p (%s)", prefab.c_str(), pos.x, pos.y, pos.z, rot.yaw, rot.pitch, rot.roll, scale, r, r && RttiName((uintptr_t)r) ? RttiName((uintptr_t)r) : "?");
    return (uintptr_t)r;
#endif
}
// Materialize one registered record on the game/engine thread: the record's own metadata is read under the lock
// and the created handle attaches only while the record still carries the generation the work was admitted with.
// A handle whose record changed generation (hidden, forgotten, restored, re-created) is disposed here - the
// physical object of a rejected completion never survives. C5: `lane` names the lane this materialization is
// for; a genuine attachment (or a real create refusal) settles the record's placement row exactly once, while
// an intermediate stand-in during a retry is only observed and stays Pending.
static void C5AddObligation(const std::shared_ptr<PlaceRequest>& req, int rowId);
static void C5ReleaseObligation(const std::shared_ptr<PlaceRequest>& req, int rowId);
static void* DoSpawn(int uid, uint64_t gen, int lane, MoveCompletion done, const MoveReq* target, bool retryStandin) {
    std::string prefab; Vec3 pos; Rot rot; float scale = 1; int proj = 0;
    std::weak_ptr<PlaceRequest> reqw; int rowId = -1;
    { REG_LOCK; const int i = IndexOfUidLocked(uid);
      if (i < 0 || g_reg[(size_t)i].gen != gen || g_reg[(size_t)i].hidden) return nullptr;
      const SpawnedObj& e = g_reg[(size_t)i]; prefab = e.prefab; pos = e.pos; rot = e.rot; scale = e.scale; proj = e.proj; reqw = e.placeReq; rowId = e.placeRow; }
    const auto flight = reqw.lock(); C5AddObligation(flight, rowId);
    struct FlightEnd { std::shared_ptr<PlaceRequest> request; int row; ~FlightEnd() { C5ReleaseObligation(request, row); } } flightEnd{ flight, rowId };
    auto nativeWork = TrackProjectNativeWork(proj);
    { REG_LOCK; if (!GenCurrentLocked(uid, gen)) return nullptr; }
    if (target) { pos = target->pos; rot = target->rot; scale = target->scale; }
    const uintptr_t r = CreateGenericSceneObject(prefab, pos, rot, scale);
    bool attached = false;
    { REG_LOCK; const int i = IndexOfUidLocked(uid);
      if (i >= 0 && g_reg[(size_t)i].gen == gen && !g_reg[(size_t)i].hidden) {
          SpawnedObj& e = g_reg[(size_t)i]; e.obj = r; e.colRot = rot; e.colScale = scale; e.hidden = (r == 0); attached = true;
          // B1: a refused retry-backed stand-in keeps the visible record (same generation, request
          // identity and triedTemplates untouched) so the requeued request retries the replay path
          // instead of stranding Pending on a hidden record the next server step discards. No row
          // settlement here: the row stays Pending until a genuine final lane or the fallback. A live-drag
          // stand-in (retryStandin=false) keeps the previous hide-on-refusal behavior.
          if (lane == PlaceLaneStandin && r == 0 && retryStandin) { e.hidden = false; e.standin = false; }
          reqw = e.placeReq; rowId = e.placeRow; } }
    if (!attached && r) DoRemove(r);   // rejected new physical object: disposed on the thread that created it
    if (attached && rowId >= 0) {      // C5: settle the caller's row (outside the registry lock)
        // C5-SETTLE-BEGIN
        if (lane == PlaceLaneStandin) {
            if (r) NoteAttachObservation(reqw, rowId);   // an intermediate stand-in is observed, never terminal
        } else if (r) {
            NoteAttachOnce(reqw, rowId, lane, uid);       // a genuine final attachment (generic/direct/replay/plain)
        } else {
            SettleRowOnce(reqw, rowId, PlaceFailed, PlaceLaneNone, uid, lane == PlaceLanePlain ? "the fallback object could not be created" : "the engine refused the create");
        }
        // C5-SETTLE-END
        if (C5RowDead(reqw, rowId)) { // cancellation during admission can precede publication of row.uid
            bool dispose = false;
            { REG_LOCK; const int i = IndexOfUidLocked(uid);
              if (i >= 0 && g_reg[(size_t)i].gen == gen && g_reg[(size_t)i].obj == r) {
                  auto& e = g_reg[(size_t)i]; e.obj = 0; e.hidden = true; e.gen = NewGenLocked(); dispose = true;
              }
            }
            if (dispose && r) DoRemove(r);
            attached = false;
        }
    }
    if (done && (lane != PlaceLaneStandin || !r)) done(attached && r != 0); // a refused stand-in cannot wait forever for a retry that requires it
    return attached ? (void*)r : nullptr;
}

// ---- game-thread pump: movement tick (pattern from master-looter / Trinity) ----
static std::vector<int> ParsePattern(const char* s) {
    std::vector<int> out; for (const char* p = s; *p; ) { while (*p == ' ') p++; if (!*p) break; if (*p == '?') { out.push_back(-1); while (*p == '?') p++; } else { out.push_back((int)strtoul(p, (char**)&p, 16)); } }
    return out;
}
static uintptr_t FindPattern(const char* pat) {
    auto p = ParsePattern(pat);
    if (p.empty()) return 0;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* s = (uint8_t*)(g_base + sec[i].VirtualAddress); size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + p.size() <= n; k++) {
            if (p[0] >= 0 && s[k] != p[0]) continue;   // a wildcard as the first byte matches anything (as in FindPatternCount)
            bool ok = true; for (size_t j = 1; j < p.size(); j++) if (p[j] >= 0 && s[k + j] != p[j]) { ok = false; break; }
            if (ok) return (uintptr_t)(s + k);
        }
    }
    return 0;
}
using PumpFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static PumpFn g_origPump = nullptr;
static std::mutex g_qMutex; static std::deque<std::function<void()>> g_queue;
volatile LONG g_queueCount = 0;
void RunOnGameThread(std::function<void()> f) { NoteWorkQueued(); std::lock_guard<std::mutex> l(g_qMutex); g_queue.push_back(std::move(f)); InterlockedIncrement(&g_queueCount); }
#ifdef WB_UNIFIED_HOST_TEST
bool GameThreadReady() { return host::Seam().ready; }
#else
bool GameThreadReady() { return g_origPump && g_gameThread; }
#endif
long PumpTicks() { return g_pumpTicks; }
long CreateCalls() { return g_createCalls; }
bool HooksReady() { return g_origCreate && g_origPump; }

static int LogFault(EXCEPTION_POINTERS* ep) {
    auto* r = ep->ExceptionRecord; auto* c = ep->ContextRecord;
    uintptr_t at = (uintptr_t)r->ExceptionAddress;
    Log("job faulted 0x%08x at %p%s (rva 0x%llx) addr=%p", r->ExceptionCode, (void*)at, InImage(at) ? " [game]" : "",
        InImage(at) ? (unsigned long long)(at - g_base) : 0ull, r->NumberParameters > 1 ? (void*)r->ExceptionInformation[1] : nullptr);
    Log("   rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx r14=%016llx r15=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R14, c->R15);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void RunJobGuarded(std::function<void()>* job) { CDK_GUARD_BEGIN (*job)(); CDK_GUARD_FAIL EXCEPTION_POINTERS ep = cdk::GuardInfo(); LogFault(&ep); CDK_GUARD_END }
static void AutoloadTick();
static void CheckReplayWatchdog();   // gimmick replay watchdog (below): the game thread keeps running when the server thread is stuck
static void PumpJobs() {
    g_pumpTicks++; g_gameThread = GetCurrentThreadId();
#ifndef WB_UNIFIED_HOST_TEST   // host runs are deterministic: the tick duties need the game's own state
    if ((g_pumpTicks & 31) == 0) CheckReplayWatchdog();
    if (g_trace) TraceTick();
    if ((g_pumpTicks & 15) == 0 && !PlayModeActive()) AutoloadTick();   // play mode brings its own scene
    PlayModeTick();
#endif
    std::function<void()> job;
    if (InterlockedCompareExchange(&g_queueCount, 0, 0) != 0) {
        std::lock_guard<std::mutex> l(g_qMutex);
        if (!g_queue.empty()) { job = std::move(g_queue.front()); g_queue.pop_front(); InterlockedDecrement(&g_queueCount); }
    }
    if (job) RunJobGuarded(&job);
}
static void InstallCrashFilter(const char* when);
static uint64_t HookPump(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    const uint64_t r = g_origPump(a1, a2, a3, a4, a5, a6, a7, a8);
    static bool s_filter = false; if (!s_filter) { s_filter = true; InstallCrashFilter("on the first game tick"); }
    PumpJobs();
    EnvironmentTick();
    return r;
}

// SpawnAt, optionally binding the new record to a caller-owned placement row (C5). The binding is written in
// the SAME registry-lock section that publishes the record, so a pump that runs before this function returns
// still resolves the row identity.
static int SpawnAtLinked(const std::string& prefab, Vec3 world, Rot rot, float scale, int group, int proj,
                         const std::shared_ptr<PlaceRequest>& req, int rowId) {
    if (!GameThreadReady()) { Log("spawn: game thread pump not active yet"); return 0; }
    // a character appearance is assembled by the game's actor system; as a scene object it would spawn nothing visible
    if (prefab.size() > 8 && prefab.compare(prefab.size() - 8, 8, ".app_xml") == 0) { Log("spawn: %s is a character appearance, characters cannot be spawned yet", prefab.c_str()); return 0; }
    const bool gim =g_gimmickSpawn && kRva_GimmickSpawn_ && IsGimmickPrefab(prefab);
    int uid; uint64_t gen = 0;
    { std::lock_guard<std::mutex> l(g_regMutex); uid = g_nextUid++; gen = NewGenLocked();
      SpawnedObj o{ 0, prefab, world, rot, scale, false, GetTickCount(), rot, scale, uid, group, proj, gim, 0, false, gen };
      o.placeReq = req; o.placeRow = req ? rowId : -1;
      g_reg.push_back(std::move(o)); if (!g_loading) MarkDirtyLocked(proj); }
    if (gim) { EnqueueGimmick(uid, gen, prefab, world, rot, scale); return uid; }   // spawned by the server tick once a template capture exists
    RunOnGameThread([uid, gen]() { DoSpawn(uid, gen); });
    return uid;
}
int SpawnAt(const std::string& prefab, Vec3 world, Rot rot, float scale, int group, int proj) {
    return SpawnAtLinked(prefab, world, rot, scale, group, proj, nullptr, -1);
}

// ---- prefab index ----
static std::vector<std::string> g_prefabs;
static std::vector<PrefabInfo> g_index;
static std::vector<CatNode> g_cats;
static std::vector<std::pair<std::string, int>> g_tagCounts;
static std::vector<int> g_favs;
static std::map<std::string, int> g_byPath;
const std::vector<std::string>& Prefabs() { return g_prefabs; }
std::string GameDir() {   // bin64\CrimsonDesert.exe -> game root
    char buf[MAX_PATH] = { 0 }; GetModuleFileNameA(nullptr, buf, MAX_PATH); std::string s = buf;
    size_t a = s.rfind('\\'); if (a != std::string::npos) s.erase(a);
    size_t b = s.rfind('\\'); if (b != std::string::npos) s.erase(b);
    return s;
}
void SetPrefabSize(const std::string& path, const float* d) {
    auto it = g_byPath.find(path); if (it == g_byPath.end()) return;
    PrefabInfo& pi = g_index[it->second];
    pi.sx = d[0]; pi.sy = d[1]; pi.sz = d[2]; pi.cx = d[3]; pi.cy = d[4]; pi.cz = d[5]; pi.hasCenter = true;
}
const std::vector<PrefabInfo>& PrefabIndex() { return g_index; }
const std::vector<CatNode>& Categories() { return g_cats; }
const std::vector<std::pair<std::string, int>>& TagCounts() { return g_tagCounts; }
const std::vector<int>& Favorites() { return g_favs; }
static std::string FavPath() { return g_modDir + "\\favorites.txt"; }
std::string ThumbFile(const std::string& p) {   // FNV-1a 64 over UTF-8, same as scripts/render_thumbs.py
    uint64_t h = 0xcbf29ce484222325ULL; for (unsigned char c : p) { h ^= c; h *= 0x100000001b3ULL; }
    char buf[32]; snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return g_modDir + "\\thumbs\\" + buf + ".png";
}
bool IsFavorite(int i) { for (int f : g_favs) if (f == i) return true; return false; }
void ToggleFavorite(int i) {
    bool removed = false;
    for (size_t k = 0; k < g_favs.size(); k++) if (g_favs[k] == i) { g_favs.erase(g_favs.begin() + k); removed = true; break; }
    if (!removed) g_favs.push_back(i);
    FILE* f = fopen(FavPath().c_str(), "w"); if (f) { for (int k : g_favs) if (k >= 0 && k < (int)g_index.size()) fprintf(f, "%s\n", g_index[k].path.c_str()); fclose(f); }
}
static std::string DisplayName(const std::string& path) {
    std::string n = path.substr(path.find_last_of('/') + 1);
    size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot);
    for (const char* pre : { "cd_", "gimmick_", "prefab_" }) if (n.rfind(pre, 0) == 0) n = n.substr(strlen(pre));
    for (auto& c : n) if (c == '_') c = ' ';
    return n;
}
static int CatFor(const std::string& path) {   // "/object/cd_gimmick/breakable/x.prefab" -> nodes object > cd_gimmick > breakable
    int node = 0; size_t pos = 1;
    while (true) {
        size_t next = path.find('/', pos); if (next == std::string::npos) break;
        std::string seg = path.substr(pos, next - pos); pos = next + 1;
        if (seg == "bin__" || seg.empty()) continue;
        int found = -1;
        for (int c : g_cats[node].children) if (g_cats[c].name == seg) { found = c; break; }
        if (found < 0) { g_cats.push_back({ seg, node, {}, {}, 0 }); found = (int)g_cats.size() - 1; g_cats[node].children.push_back(found); }
        node = found;
    }
    return node;
}
static void LoadPrefabs() {
    g_cats.push_back({ "all", -1, {}, {}, 0 });
    const uint8_t* packed = nullptr; size_t packedSize = 0;
    std::vector<uint8_t> decoded;
    std::string prefabText;
    if (!EmbeddedResource(kResourcePrefabs, &packed, &packedSize) || packedSize < 8 || memcmp(packed, "CDK1", 4) != 0) {
        Log("WARNING: embedded prefab index resource is missing or unreadable");
    } else {
        uint32_t rawSize = 0; memcpy(&rawSize, packed + 4, sizeof rawSize);
        if (!thumbgen::Lz4Decode(packed + 8, packedSize - 8, decoded, rawSize)) {
            Log("WARNING: embedded prefab index LZ4 decode failed");
        } else {
            prefabText.assign(reinterpret_cast<const char*>(decoded.data()), decoded.size());
            decoded.clear(); decoded.shrink_to_fit();
        }
    }
    std::istringstream f(std::move(prefabText));
    std::string line; std::map<std::string, int> tagc;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        PrefabInfo pi{}; pi.meshes = pi.children = 0;
        std::vector<std::string> col; size_t s = 0;
        while (true) { size_t t = line.find('\t', s); col.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s)); if (t == std::string::npos) break; s = t + 1; }
        if (col.size() < 5) continue;
        pi.path = col[0]; pi.tags = col[1]; pi.meshes = atoi(col[2].c_str()); pi.children = atoi(col[3].c_str()); pi.mesh = col[4];
        pi.name = DisplayName(pi.path);
        pi.cat = CatFor(pi.path);
        int idx = (int)g_index.size();
        g_cats[pi.cat].prefabs.push_back(idx);
        for (int n = pi.cat; n >= 0; n = g_cats[n].parent) g_cats[n].total++;
        // tag counts (tag or tag:N)
        size_t p = 0; while (p < pi.tags.size()) { size_t q = pi.tags.find(',', p); std::string t = pi.tags.substr(p, q == std::string::npos ? std::string::npos : q - p); size_t colon = t.find(':'); if (colon != std::string::npos) t = t.substr(0, colon); if (!t.empty()) tagc[t]++; if (q == std::string::npos) break; p = q + 1; }
        g_index.push_back(pi); g_prefabs.push_back(pi.path);
    }
    for (int i = 0; i < (int)g_index.size(); i++) g_byPath[g_index[i].path] = i;
    // bounding boxes from prefab_size.tsv (written locally by the in-game thumbnail generator)
    { std::ifstream sf(g_modDir + "\\prefab_size.tsv"); auto& byPath = g_byPath; int n = 0;
      while (std::getline(sf, line)) { size_t t = line.find('\t'); if (t == std::string::npos) continue; auto it = byPath.find(line.substr(0, t)); if (it == byPath.end()) continue;
        PrefabInfo& pi = g_index[it->second];
        int got = sscanf(line.c_str() + t + 1, "%f\t%f\t%f\t%f\t%f\t%f", &pi.sx, &pi.sy, &pi.sz, &pi.cx, &pi.cy, &pi.cz);
        pi.hasCenter = got == 6; if (got < 6) pi.cx = pi.cy = pi.cz = 0; n++;
        if (pi.hasCenter && pi.cx == 0 && pi.cy == 0 && pi.cz == 0 && pi.sy > 0) pi.cy = pi.sy * 0.5f;   // lines written for a failed re-measure: assume the pivot at the bottom center
    }
      if (n) Log("prefab sizes: %d", n); }
    for (auto& kv : tagc) g_tagCounts.push_back(kv);
    std::sort(g_tagCounts.begin(), g_tagCounts.end(), [](auto& a, auto& b) { return a.second > b.second; });
    // favorites
    std::ifstream ff(FavPath()); auto& byPath = g_byPath;
    while (std::getline(ff, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back(); auto it = byPath.find(line); if (it != byPath.end()) g_favs.push_back(it->second); }
    Log("prefab index: %zu entries (embedded RCDATA/LZ4), %zu categories, %zu tags, %zu favorites", g_index.size(), g_cats.size(), g_tagCounts.size(), g_favs.size());
    if (g_index.empty()) Log("WARNING: embedded prefab index is empty or unavailable");
}

// ---- live preview ----
static std::mutex g_prevMutex;
static struct { uintptr_t obj = 0; std::string prefab; Vec3 pos{}; float yaw = 0, scale = 1; bool pending = false; } g_prev;
bool PreviewActive() { std::lock_guard<std::mutex> l(g_prevMutex); return g_prev.obj != 0 || g_prev.pending; }
bool PreviewPending() { std::lock_guard<std::mutex> l(g_prevMutex); return g_prev.pending; }
static void DoPreviewClear() {
    uintptr_t obj; { std::lock_guard<std::mutex> l(g_prevMutex); obj = g_prev.obj; g_prev.obj = 0; g_prev.prefab.clear(); g_prev.pending = false; }
    if (obj) DoRemove(obj);
}
void PreviewClear() { if (GameThreadReady()) RunOnGameThread([]() { DoPreviewClear(); }); }
void PreviewSet(const std::string& prefab, Vec3 pos, float yawDeg, float scale, bool recreate) {
    if (!GameThreadReady()) return;
    { std::lock_guard<std::mutex> l(g_prevMutex); g_prev.pending = true; }
    RunOnGameThread([prefab, pos, yawDeg, scale, recreate]() {
        uintptr_t obj; std::string cur; float cyaw, cscale;
        { std::lock_guard<std::mutex> l(g_prevMutex); obj = g_prev.obj; cur = g_prev.prefab; cyaw = g_prev.yaw; cscale = g_prev.scale; }
        bool same = !recreate && obj && cur == prefab;   // live: rotation/scale via setWorldTransform too, the drop re-creates
        if (same) { DoLiveMove(obj, pos, Rot{ yawDeg }, scale, GetTickCount()); }
        else {
            if (obj) DoRemove(obj);
            obj = CreateGenericSceneObject(prefab, pos, Rot{ yawDeg }, scale);   // the preview is not a registry record
        }
        std::lock_guard<std::mutex> l(g_prevMutex);
        g_prev.obj = obj; g_prev.prefab = prefab; g_prev.pos = pos; g_prev.yaw = yawDeg; g_prev.scale = scale; g_prev.pending = false;
    });
}
int PreviewCommit() {
    std::lock_guard<std::mutex> l(g_prevMutex);
    if (!g_prev.obj) return 0;
    int uid = 0;
    { std::lock_guard<std::mutex> r(g_regMutex); uid = g_nextUid++; g_reg.push_back({ g_prev.obj, g_prev.prefab, g_prev.pos, Rot{ g_prev.yaw }, g_prev.scale, false, GetTickCount(), Rot{ g_prev.yaw }, g_prev.scale, uid, 0, 0, false, 0, false, NewGenLocked() }); }
    Log("preview committed: %s at (%.2f %.2f %.2f)", g_prev.prefab.c_str(), g_prev.pos.x, g_prev.pos.y, g_prev.pos.z);
    g_prev.obj = 0; g_prev.prefab.clear();
    return uid;
}

// ---- projects (save / load / autoload) ----
static std::string ProjDir() { return g_modDir + "\\projects"; }
static std::string ProjPath(const std::string& name) { return ProjDir() + "\\" + name + ".cdproj"; }
// A group file is placed through the group path, never loaded or autoloaded as a project.
static bool IsGroupFileName(const std::string& name) {
    return name.size() > 8 && _stricmp(name.c_str() + name.size() - 8, ".cdgroup") == 0;
}
// Lifecycle serializes core file admission with file mutation, never engine callbacks. Activity leases
// survive deferred LoadProject admission. The recursive mutex lets a host boundary probe the REAL gate while
// a same-thread save/export is in flight; the activity, not recursive acquisition, decides the refusal.
static std::recursive_mutex g_fileMutex;
#ifdef WB_UNIFIED_HOST_TEST
FileMutationFault g_fileMutationFault = FileMutationFault::None;
#endif
static std::wstring FileWide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}
bool FileNameEqual(const std::string& a, const std::string& b) {
    const auto x = FileWide(a), y = FileWide(b);
    if (x.empty() || y.empty()) return a == b;
    return CompareStringOrdinal(x.data(), (int)x.size(), y.data(), (int)y.size(), TRUE) == CSTR_EQUAL;
}
struct FileActivityRow { proj_codec::Kind kind; std::string name; FileReason reason; };
static std::vector<const FileActivityRow*> g_fileActivities;
struct FileActivity {
    FileActivityRow row;
    FileActivity(proj_codec::Kind kind, const std::string& name, FileReason reason) : row{ kind, name, reason } {
        std::lock_guard<std::recursive_mutex> l(g_fileMutex); g_fileActivities.push_back(&row);
    }
    ~FileActivity() {
        std::lock_guard<std::recursive_mutex> l(g_fileMutex);
        g_fileActivities.erase(std::find(g_fileActivities.begin(), g_fileActivities.end(), &row));
    }
};
static std::shared_ptr<void> TrackProjectNativeWork(int proj) {
    if (!proj) return {};
    return std::make_shared<FileActivity>(proj_codec::Kind::Project, ProjectNameOf(proj), FileReason::PendingOperation);
}
struct FilePlaceUse { std::string name; std::weak_ptr<PlaceRequest> request; };
static std::vector<FilePlaceUse> g_filePlaces; // weak ownership only; never changes/prunes the caller's receipts
struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool close() { const HANDLE old = h; h = INVALID_HANDLE_VALUE; return old == INVALID_HANDLE_VALUE || CloseHandle(old) != 0; }
};
struct FileSelection { SavedFile file; BY_HANDLE_FILE_INFORMATION info{}; std::string bytes; };
static const char* FileExtension(proj_codec::Kind kind) { return kind == proj_codec::Kind::Project ? ".cdproj" : ".cdgroup"; }
static std::string FileDirectory(proj_codec::Kind kind, bool archived) {
    return g_modDir + (kind == proj_codec::Kind::Project ? "\\projects" : "\\Groups") + (archived ? "\\.archive" : "");
}
static bool ValidFileName(const std::string& filename, proj_codec::Kind kind) {
    if (kind != proj_codec::Kind::Project && kind != proj_codec::Kind::Group) return false;
    const size_t ext = strlen(FileExtension(kind));
    if (filename.size() <= ext || filename.size() > 255 || filename.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : filename) if (c < 32) return false;
    if (!FileNameEqual(filename.substr(filename.size() - ext), FileExtension(kind))) return false;
    const std::string stem = filename.substr(0, filename.size() - ext);
    const std::string device = stem.substr(0, stem.find('.'));
    if (FileNameEqual(device, "CON") || FileNameEqual(device, "PRN") || FileNameEqual(device, "AUX") || FileNameEqual(device, "NUL")) return false;
    if (device.size() == 4 && (FileNameEqual(device.substr(0, 3), "COM") || FileNameEqual(device.substr(0, 3), "LPT")) && device[3] >= '1' && device[3] <= '9') return false;
    return true;
}
static FileResult ValidateSavedFile(const SavedFile& file) {
    if (!ValidFileName(file.filename, file.kind)) return { FileReason::InvalidName };
    if (file.path != FileDirectory(file.kind, file.archived) + "\\" + file.filename) return { FileReason::StaleTarget };
    return {};
}
static FileResult OpenFileDirectory(const std::string& path, FileHandle& handle) {
    handle.h = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return { e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? FileReason::NotFound : FileReason::UnsafePath, e };
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.h, &info)) return { FileReason::UnsafePath, GetLastError() };
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return { FileReason::UnsafePath };
    return {};
}
struct FileDirectories { FileHandle root, active, archive; };
static FileResult OpenFileDirectories(const SavedFile& file, bool createArchive, FileDirectories& dirs) {
    FileResult r = OpenFileDirectory(g_modDir, dirs.root); if (!r.ok()) return r;
    r = OpenFileDirectory(FileDirectory(file.kind, false), dirs.active); if (!r.ok()) return r;
    if (file.archived || createArchive) {
        const std::string path = FileDirectory(file.kind, true);
        if (createArchive && !CreateDirectoryA(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return { FileReason::MoveFailed, GetLastError() };
        r = OpenFileDirectory(path, dirs.archive); if (!r.ok()) return r;
        BY_HANDLE_FILE_INFORMATION a{}, b{};
        if (!GetFileInformationByHandle(dirs.active.h, &a) || !GetFileInformationByHandle(dirs.archive.h, &b)) return { FileReason::UnsafePath, GetLastError() };
        if (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber) return { FileReason::UnsafePath };
    }
    return {};
}
static bool ReadHandleBytes(HANDLE h, std::string& bytes) {
    LARGE_INTEGER zero{}; if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return false;
    bytes.clear(); char block[8192]; DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, block, sizeof block, &got, nullptr)) return false;
        if (!got) return true;
        bytes.append(block, got);
    }
}
static FileResult ReadSelectedFile(const SavedFile& file, DWORD access, DWORD sharing, FileHandle& handle, FileSelection& out) {
    WIN32_FIND_DATAA fd{}; HANDLE find = FindFirstFileA(file.path.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return { FileReason::NotFound, GetLastError() };
    FindClose(find);
    if (file.filename != fd.cFileName) return { FileReason::StaleTarget }; // exact selected filename, not an alias
    handle.h = CreateFileA(file.path.c_str(), access, sharing, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.h == INVALID_HANDLE_VALUE) return { FileReason::ReadFailed, GetLastError() };
    if (!GetFileInformationByHandle(handle.h, &out.info)) return { FileReason::ReadFailed, GetLastError() };
    if (out.info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return { FileReason::UnsafePath };
    if (!ReadHandleBytes(handle.h, out.bytes)) return { FileReason::ReadFailed, GetLastError() };
    out.file = file;
    return {};
}
static bool SameFileSelection(const FileSelection& a, const FileSelection& b) {
    return a.info.dwVolumeSerialNumber == b.info.dwVolumeSerialNumber && a.info.nFileIndexHigh == b.info.nFileIndexHigh &&
        a.info.nFileIndexLow == b.info.nFileIndexLow && a.info.nFileSizeHigh == b.info.nFileSizeHigh && a.info.nFileSizeLow == b.info.nFileSizeLow &&
        CompareFileTime(&a.info.ftLastWriteTime, &b.info.ftLastWriteTime) == 0 && a.bytes == b.bytes;
}
FileResult ListSavedFiles(proj_codec::Kind kind, bool archived, std::vector<SavedFile>& files) {
    std::lock_guard<std::recursive_mutex> l(g_fileMutex);
    files.clear();
    if (kind != proj_codec::Kind::Project && kind != proj_codec::Kind::Group) return { FileReason::InvalidName };
    SavedFile file; file.kind = kind; file.archived = archived;
    FileDirectories dirs; FileResult r = OpenFileDirectories(file, false, dirs);
    if (r.reason == FileReason::NotFound) return {};
    if (!r.ok()) return r;
    const std::string path = FileDirectory(kind, archived);
    WIN32_FIND_DATAA fd{}; HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); return e == ERROR_FILE_NOT_FOUND ? FileResult{} : FileResult{ FileReason::ReadFailed, e }; }
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!ValidFileName(fd.cFileName, kind)) continue;
        file.filename = fd.cFileName; file.path = path + "\\" + file.filename; files.push_back(file);
    } while (FindNextFileA(h, &fd));
    const DWORD e = GetLastError(); FindClose(h);
    if (e != ERROR_NO_MORE_FILES) { files.clear(); return { FileReason::ReadFailed, e }; }
    return {};
}
FileResult SelectSavedFile(const SavedFile& file, FileSelectionHandle& selection) {
    std::lock_guard<std::recursive_mutex> l(g_fileMutex);
    selection.reset(); FileResult r = ValidateSavedFile(file); if (!r.ok()) return r;
    FileDirectories dirs; r = OpenFileDirectories(file, false, dirs); if (!r.ok()) return r;
    FileHandle h; auto value = std::make_shared<FileSelection>();
    r = ReadSelectedFile(file, GENERIC_READ, FILE_SHARE_READ, h, *value);
    if (r.ok()) selection = value;
    return r;
}
SavedFile SelectedFile(const FileSelectionHandle& selection) { return selection ? selection->file : SavedFile{}; }

// ---- project membership -----------------------------------------------------------------------------------------
// Every spawned object carries the id of the project it came from (0 = placed by hand and not saved yet). That is what
// lets one project be overwritten with exactly its own objects while other loaded projects stay untouched. The ids are
// per session and only name the .cdproj files; the file format itself does not change.
static std::mutex g_projMutex;                 // guards g_projNames only - never taken while g_regMutex is held
static std::vector<std::string> g_projNames{ "" };   // index 0 = "no project"
int ProjectId(const std::string& name) {
    if (name.empty()) return 0;
    std::lock_guard<std::mutex> l(g_projMutex);
    for (size_t i = 1; i < g_projNames.size(); i++) if (_stricmp(g_projNames[i].c_str(), name.c_str()) == 0) return (int)i;
    g_projNames.push_back(name);
    return (int)g_projNames.size() - 1;
}
std::string ProjectNameOf(int id) {
    std::lock_guard<std::mutex> l(g_projMutex);
    return (id > 0 && id < (int)g_projNames.size()) ? g_projNames[id] : std::string();
}
int ProjectObjectCount(int id) {
    int n = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
#ifdef WB_UNIFIED_HOST_TEST
        ++g_sceneEnumerationStats.calls; g_sceneEnumerationStats.records += g_reg.size() + g_npcReg.size();
#endif
        for (auto& o : g_reg) if (!o.hidden && o.proj == id) n++;
        for (auto& npc : g_npcReg) if (!npc.hidden && npc.proj == id) n++;
    }
    for (const auto& t : TerrainStrokes()) if (t.proj == id) n++;
    return n;
}
bool ProjectDirty(int id) { std::lock_guard<std::mutex> l(g_regMutex); return g_projDirty.count(id) != 0; }
void MarkProjectDirty(int proj) { if (proj) { std::lock_guard<std::mutex> l(g_regMutex); g_projDirty.insert(proj); } }
void AssignProject(int uid, int proj) {
    std::lock_guard<std::mutex> l(g_regMutex);
    int i = IndexOfUidLocked(uid); if (i >= 0) g_reg[i].proj = proj;
}

struct LibraryNameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
    }
};
FileResult RefreshSavedLibrary(SavedLibrarySnapshot& snapshot) {
    std::lock_guard<std::recursive_mutex> filesLock(g_fileMutex);
    SavedLibrarySnapshot next;
    for (bool archived : { false, true }) for (auto kind : { proj_codec::Kind::Project, proj_codec::Kind::Group }) {
        std::vector<SavedFile> files;
        const auto result = ListSavedFiles(kind, archived, files);
        if (!result.ok()) return result;
        auto& totals = archived ? next.archived : next.active;
        (kind == proj_codec::Kind::Project ? totals.projects : totals.groups) = files.size();
        for (auto& file : files) next.entries.push_back({ std::move(file), {} });
    }
    std::vector<std::string> names;
    std::map<int, ProjectOwnership> byId;
    {
        // Same lock order as the lifecycle gate; never acquire project names from inside a registry scan.
        std::lock_guard<std::mutex> projectsLock(g_projMutex);
        std::lock_guard<std::mutex> registryLock(g_regMutex);
        names = g_projNames;
#ifdef WB_UNIFIED_HOST_TEST
        ++g_sceneEnumerationStats.calls; g_sceneEnumerationStats.records += g_reg.size();
#endif
        for (const auto& object : g_reg) {
            auto& counts = byId[object.proj];
            ++(object.hidden ? counts.hidden : counts.visible);
            if (!object.hidden && (!object.obj || object.standin)) ++counts.pendingObjects;
        }
        for (const auto& npc : g_npcReg) {
            auto& counts = byId[npc.proj];
            ++(npc.hidden ? counts.hiddenNpcs : counts.visibleNpcs);
            if (!npc.hidden && (npc.spawnPending || !npc.actor)) ++counts.pendingNpcs;
        }
        for (const auto& stroke : TerrainStrokes()) ++byId[stroke.proj].terrain;
        for (int id : g_projDirty) byId[id].dirty = true;
    }
    next.unassigned = byId[0];
    std::map<std::wstring, ProjectOwnership, LibraryNameLess> byName;
    for (size_t id = 1; id < names.size(); ++id) {
        const auto found = byId.find((int)id);
        if (found == byId.end()) continue;
        auto& counts = byName[FileWide(names[id])];
        counts.visible += found->second.visible; counts.hidden += found->second.hidden;
        counts.terrain += found->second.terrain;
        counts.visibleNpcs += found->second.visibleNpcs; counts.hiddenNpcs += found->second.hiddenNpcs;
        counts.pendingObjects += found->second.pendingObjects; counts.pendingNpcs += found->second.pendingNpcs;
        counts.dirty |= found->second.dirty;
    }
    for (auto& entry : next.entries) if (entry.file.kind == proj_codec::Kind::Project) {
        const auto stem = entry.file.filename.substr(0, entry.file.filename.size() - strlen(FileExtension(entry.file.kind)));
        const auto found = byName.find(FileWide(stem));
        if (found != byName.end()) entry.ownership = found->second;
    }
    std::sort(next.entries.begin(), next.entries.end(), [](const SavedLibraryEntry& a, const SavedLibraryEntry& b) {
        const auto x = FileWide(a.file.filename), y = FileWide(b.file.filename);
        const int folded = CompareStringOrdinal(x.data(), (int)x.size(), y.data(), (int)y.size(), TRUE);
        if (folded != CSTR_EQUAL) return folded == CSTR_LESS_THAN;
        const int exact = CompareStringOrdinal(x.data(), (int)x.size(), y.data(), (int)y.size(), FALSE);
        if (exact != CSTR_EQUAL) return exact == CSTR_LESS_THAN;
        if (a.file.kind != b.file.kind) return a.file.kind < b.file.kind;
        if (a.file.archived != b.file.archived) return !a.file.archived;
        return a.file.path < b.file.path;
    });
    snapshot = std::move(next);
    return {};
}
std::vector<size_t> FilterSavedLibrary(const SavedLibrarySnapshot& snapshot, SavedLibraryKind kind,
                                      SavedLibraryLocation location, const std::string& search) {
    const auto needle = FileWide(search);
    std::vector<size_t> rows;
    for (size_t i = 0; i < snapshot.entries.size(); ++i) {
        const auto& file = snapshot.entries[i].file;
        if (kind == SavedLibraryKind::Projects && file.kind != proj_codec::Kind::Project) continue;
        if (kind == SavedLibraryKind::Groups && file.kind != proj_codec::Kind::Group) continue;
        if (location == SavedLibraryLocation::Active && file.archived) continue;
        if (location == SavedLibraryLocation::Archived && !file.archived) continue;
        if (!needle.empty()) {
            const auto name = FileWide(file.filename);
            bool match = false;
            for (size_t start = 0; start + needle.size() <= name.size(); ++start) {
                if (CompareStringOrdinal(name.data() + start, (int)needle.size(), needle.data(), (int)needle.size(), TRUE) == CSTR_EQUAL) { match = true; break; }
            }
            if (!match) continue;
        }
        rows.push_back(i);
    }
    return rows;
}

// ---- record-owned copy provenance (C4) ------------------------------------------------------------------
// A copy owns values, never a group id and never a shared prototype: the saved envelope (pivot anchor, bounds,
// quality), the narrowed values every member had when the copy was admitted, and the receiver box hints used
// to recompute a changed copy. Row ordinals survive project persistence, so a record keeps its envelope across
// save -> reload -> re-export. g_regMutex guards this map together with the registry it describes.
struct CopyEnvelope {
    proj_codec::Bounds bounds;
    std::vector<proj_codec::Record> source;
    std::vector<wb_group_math::PrefabBox> hints;
};
struct CopyMember { std::shared_ptr<CopyEnvelope> copy; size_t ordinal = 0; };
static std::map<int, CopyMember> g_copyMembers;
static void ForgetCopyValueLocked(int uid) { g_copyMembers.erase(uid); }
static proj_codec::Record ValueRecord(const SpawnedObj& o) {
    proj_codec::Record r;
    r.prefab = o.prefab; r.pos = { o.pos.x, o.pos.y, o.pos.z };
    r.yaw = o.rot.yaw; r.pitch = o.rot.pitch; r.roll = o.rot.roll; r.scale = o.scale; r.group = o.group; r.note = o.note;
    return r;
}
static wb_group_math::PrefabBox BoxHint(const std::string& prefab) {
    wb_group_math::PrefabBox box;
    for (const auto& p : g_index) if (p.path == prefab) {
        if (p.hasCenter && p.sx > 0 && p.sy > 0 && p.sz > 0) { box.known = true; box.size = { p.sx, p.sy, p.sz }; box.center = { p.cx, p.cy, p.cz }; }
        break;
    }
    return box;
}
static bool SamePose(const proj_codec::Record& a, const proj_codec::Record& b) {
    return a.prefab == b.prefab && a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z &&
        a.yaw == b.yaw && a.pitch == b.pitch && a.roll == b.roll && a.scale == b.scale;
}
static bool NearEngine(double a, double b) {
    return std::abs(a - b) <= 1e-4 + 4 * (std::numeric_limits<float>::epsilon)() * (std::max)(std::abs(a), std::abs(b));
}
// The envelope of one saved copy: the saved bounds are authoritative while the copy is unchanged, and a rigid
// yaw/positive uniform scale/translation preserves them through the same transform math - the receiver never
// recenters and never overrides saved values. Anything else is recomputed from the member boxes and marked
// approximate, because a reduced or explicitly edited copy cannot claim measured bounds.
static bool CopyBounds(const std::vector<wb_group_math::Member>& members,
                       const std::vector<size_t>& ordinals, const CopyEnvelope* copy, proj_codec::Bounds& bounds) {
    bool usable = copy && members.size() == copy->source.size();
    if (usable) for (size_t ordinal : ordinals) if (ordinal >= copy->source.size()) { usable = false; break; }
    if (usable) {
        bool same = true;
        for (size_t i = 0; i < members.size(); ++i) same &= SamePose(members[i].record, copy->source[ordinals[i]]);
        if (same) return wb_group_math::ComputeBounds(members, &copy->bounds, bounds);
        // A whole-copy yaw/scale/translation preserves the saved envelope, even without receiver boxes; member
        // tilt or a non-rigid edit must recompute.
        const auto& a = copy->source[ordinals[0]]; const auto& b = members[0].record;
        const double factor = b.scale / a.scale, yaw = b.yaw - a.yaw;
        std::vector<proj_codec::Record> turned;
        if (wb_group_math::AnchorTransform({ a }, copy->bounds, {}, yaw, factor, turned)) {
            proj_codec::Point target{ b.pos.x - turned[0].pos.x, b.pos.y - turned[0].pos.y, b.pos.z - turned[0].pos.z };
            if (wb_group_math::AnchorTransform(copy->source, copy->bounds, target, yaw, factor, turned)) {
                bool rigid = true;
                for (size_t i = 0; i < members.size(); ++i) {
                    const auto& expected = turned[ordinals[i]]; const auto& actual = members[i].record;
                    rigid &= expected.prefab == actual.prefab && NearEngine(expected.pos.x, actual.pos.x) && NearEngine(expected.pos.y, actual.pos.y) &&
                        NearEngine(expected.pos.z, actual.pos.z) && NearEngine(expected.yaw, actual.yaw) && NearEngine(expected.scale, actual.scale) &&
                        expected.pitch == actual.pitch && expected.roll == actual.roll;
                }
                if (rigid) {
                    const auto& old = copy->bounds; wb_group_math::Member envelope;
                    envelope.record.pos = target; envelope.record.yaw = yaw; envelope.record.scale = factor;
                    envelope.box.known = true;
                    envelope.box.center = { (old.min.x + old.max.x) * .5 - old.anchor.x, (old.min.y + old.max.y) * .5 - old.anchor.y, (old.min.z + old.max.z) * .5 - old.anchor.z };
                    envelope.box.size = { old.max.x - old.min.x, old.max.y - old.min.y, old.max.z - old.min.z };
                    if (wb_group_math::ComputeBounds({ envelope }, nullptr, bounds)) {
                        bounds.anchor = target; bounds.approximate = old.approximate; return true;
                    }
                }
            }
        }
    }
    if (!wb_group_math::ComputeBounds(members, nullptr, bounds)) return false;
    if (copy) bounds.approximate = true;
    return true;
}
// The authoritative value builder (C6): one document for a captured object list, one envelope per copy, records
// without provenance get a fresh one-member envelope (never a shared null identity). Runs under g_regMutex.
static bool ValueDocumentLocked(const std::vector<SpawnedObj>& objects, proj_codec::Document& doc, std::string& error) {
    doc = proj_codec::Document();
    doc.kind = proj_codec::Kind::Project;
    if (objects.empty()) return true;
    std::map<const CopyEnvelope*, int> ids;
    std::vector<std::vector<wb_group_math::Member>> members;
    std::vector<std::vector<size_t>> ordinals;
    std::vector<const CopyEnvelope*> copies;
    for (const auto& o : objects) {
        const auto name = g_groupNames.find(o.group);
        if (name != g_groupNames.end()) doc.groupNames.emplace(name->first, name->second);
        const auto saved = g_copyMembers.find(o.uid);
        const CopyEnvelope* copy = saved == g_copyMembers.end() ? nullptr : saved->second.copy.get();
        int id = (int)members.size() + 1;
        if (copy) id = ids.emplace(copy, id).first->second;
        if (id == (int)members.size() + 1) { members.emplace_back(); ordinals.emplace_back(); copies.push_back(copy); }
        wb_group_math::Member member; member.record = ValueRecord(o); member.record.envelope = id;
        const size_t ordinal = copy ? saved->second.ordinal : 0;
        member.box = copy && ordinal < copy->hints.size() ? copy->hints[ordinal] : BoxHint(o.prefab);
        members[(size_t)id - 1].push_back(member); ordinals[(size_t)id - 1].push_back(ordinal); doc.records.push_back(member.record);
    }
    for (size_t i = 0; i < members.size(); ++i) {
        proj_codec::Bounds bounds;
        if (!CopyBounds(members[i], ordinals[i], copies[i], bounds)) { error = "record 0: invalid copy bounds or transform"; return false; }
        doc.envelopes.push_back(proj_codec::Envelope{ (int)i + 1, bounds });
    }
    doc.hasBounds = true; doc.bounds = doc.envelopes.front().bounds;
    if (doc.envelopes.size() > 1) {
        for (const auto& envelope : doc.envelopes) {
            const auto& bounds = envelope.bounds;
            doc.bounds.min.x = (std::min)(doc.bounds.min.x, bounds.min.x); doc.bounds.max.x = (std::max)(doc.bounds.max.x, bounds.max.x);
            doc.bounds.min.y = (std::min)(doc.bounds.min.y, bounds.min.y); doc.bounds.max.y = (std::max)(doc.bounds.max.y, bounds.max.y);
            doc.bounds.min.z = (std::min)(doc.bounds.min.z, bounds.min.z); doc.bounds.max.z = (std::max)(doc.bounds.max.z, bounds.max.z);
            doc.bounds.approximate |= bounds.approximate;
        }
        doc.bounds.anchor = { doc.bounds.min.x * .5 + doc.bounds.max.x * .5, doc.bounds.min.y, doc.bounds.min.z * .5 + doc.bounds.max.z * .5 };
    }
    return true;
}
static bool GroundDestinationValid(const GroundHandle& op, const std::vector<MoveReq>& moves) {
    std::vector<SpawnedObj> objects;
    for (size_t i = 0; i < moves.size(); ++i) { auto o = op->view.members[i].before; o.pos = moves[i].pos; objects.push_back(o); }
    proj_codec::Document doc; std::string error;
    { REG_LOCK; if (!ValueDocumentLocked(objects, doc, error)) return false; }
    std::vector<proj_codec::EngineRow> rows; return proj_codec::NarrowForEngine(doc, rows, error);
}
bool GroundBounds(const GroundHandle& op, proj_codec::Bounds& bounds) {
    std::lock_guard<std::mutex> operation(g_groundOpMutex);
    if (!GroundValidLocked(op)) return false;
    std::vector<SpawnedObj> objects; for (const auto& m : op->view.members) objects.push_back(m.before);
    proj_codec::Document doc; std::string error;
    { REG_LOCK; if (!ValueDocumentLocked(objects, doc, error) || !doc.hasBounds) return false; }
    std::vector<proj_codec::EngineRow> rows;
    if (!proj_codec::NarrowForEngine(doc, rows, error)) return false;
    bounds = doc.bounds; return true;
}
// Bind loaded/admitted records to the document's copy envelopes: the document's bounds and values become the
// record's provenance, so the pivot is never recomputed from receiver measurements. Runs under g_regMutex.
static bool RetainGroupValues(const proj_codec::Document& doc, const std::vector<int>& uids, std::string& error) {
    std::string bytes;
    if (doc.records.size() != uids.size() || !proj_codec::Serialize(doc, bytes, error)) return false;
    std::lock_guard<std::mutex> lock(g_regMutex);
    std::set<int> unique;
    for (int uid : uids) if (uid && (!unique.insert(uid).second || IndexOfUidLocked(uid) < 0)) { error = "record 0: invalid copy binding"; return false; }
    std::map<int, std::shared_ptr<CopyEnvelope>> copies;
    for (const auto& envelope : doc.envelopes) {
        std::shared_ptr<CopyEnvelope> copy = std::make_shared<CopyEnvelope>();
        copy->bounds = envelope.bounds;
        copies.emplace(envelope.id, copy);
    }
    for (size_t i = 0; i < doc.records.size(); ++i) {
        const auto found = copies.find(doc.records[i].envelope);
        if (found == copies.end()) { error = "record 0: invalid copy binding"; return false; }
        const std::shared_ptr<CopyEnvelope>& copy = found->second;
        const size_t ordinal = copy->source.size();
        proj_codec::Record record = doc.records[i];
        const int index = IndexOfUidLocked(uids[i]);
        if (index >= 0) record = ValueRecord(g_reg[(size_t)index]);   // the actual narrowed value is the unchanged-pose baseline
        copy->source.push_back(record);
        copy->hints.push_back(BoxHint(record.prefab));
        if (uids[i]) g_copyMembers[uids[i]] = CopyMember{ copy, ordinal };
    }
    return true;
}

// ---- C4/C5: validated group admission (independent copies) ----------------------------------------------
// The receiver's prefab cache: a prefab the index does not know is a named exclusion at admission, never a
// late engine refusal mislabeled as a missing-prefab parse success.
static bool PrefabKnown(const std::string& prefab) {
    for (const auto& p : g_index) if (p.path == prefab) return true;
    return false;
}
// Rigid transform of one saved envelope about the document's saved anchor: the box is yaw-rotated and
// uniformly scaled (member tilt is irrelevant for the envelope box), its AABB is recomputed and the exact
// rigid anchor is kept. This is the same math the per-copy envelope transform uses - no second transform engine.
static bool TransformGroupBounds(const proj_codec::Bounds& source, const proj_codec::Bounds& document,
                                proj_codec::Point target, double deltaYaw, double factor, proj_codec::Bounds& out) {
    proj_codec::Record one; one.prefab = "point"; one.pos = source.anchor; one.scale = 1;
    std::vector<proj_codec::Record> moved;
    if (!wb_group_math::AnchorTransform({ one }, document, target, deltaYaw, factor, moved) || moved.size() != 1) return false;
    const proj_codec::Point anchor = moved[0].pos;
    wb_group_math::Member box;
    box.record.pos = anchor; box.record.yaw = deltaYaw; box.record.scale = factor;
    box.box.known = true;
    box.box.center = { (source.min.x + source.max.x) * .5 - source.anchor.x,
                       (source.min.y + source.max.y) * .5 - source.anchor.y,
                       (source.min.z + source.max.z) * .5 - source.anchor.z };
    box.box.size = { source.max.x - source.min.x, source.max.y - source.min.y, source.max.z - source.min.z };
    proj_codec::Bounds computed;
    if (!wb_group_math::ComputeBounds({ box }, nullptr, computed)) return false;
    computed.anchor = anchor;                  // the rigid anchor, not the recomputed AABB bottom center
    computed.approximate = source.approximate;
    out = computed;
    return true;
}
GroupAdmissionReport AdmitGroupCopy(const proj_codec::Document& doc, Vec3 target, double deltaYaw, double factor,
                                    std::vector<int>& uids, Vec3& pivot) {
    GroupAdmissionReport rep;
    uids.clear();
    auto fail = [&](const std::string& why) { rep.valid = false; rep.error = why; Log("group admission: %s", why.c_str()); return rep; };
    if (!GameThreadReady()) return fail("record 0: game thread pump not active yet");
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
        !std::isfinite(deltaYaw) || !std::isfinite(factor) || factor <= 0) return fail("record 0: invalid placement transform");
    if (doc.kind != proj_codec::Kind::Group) return fail("record 0: not a group document");
    if (!doc.hasBounds || doc.envelopes.empty() || doc.records.empty()) return fail("record 0: incomplete group metadata");
    // Every destination value is computed here, BEFORE any UID/group/queue mutation.
    const proj_codec::Point dest{ target.x, target.y, target.z };
    // C4-TRANSFORM-BEGIN
    std::vector<proj_codec::Record> moved;
    if (!wb_group_math::AnchorTransform(doc.records, doc.bounds, dest, deltaYaw, factor, moved)) return fail("record 0: invalid destination transform");
    proj_codec::Document placed = doc;
    for (size_t i = 0; i < moved.size(); ++i) { placed.records[i].pos = moved[i].pos; placed.records[i].yaw = moved[i].yaw; placed.records[i].scale = moved[i].scale; }
    // C4-TRANSFORM-END
    // C4-ENVELOPE-BEGIN
    for (auto& envelope : placed.envelopes) {
        proj_codec::Bounds transformed;
        if (!TransformGroupBounds(envelope.bounds, doc.bounds, dest, deltaYaw, factor, transformed)) return fail("record 0: invalid envelope transform");
        envelope.bounds = transformed;
    }
    { proj_codec::Bounds transformed;
      if (!TransformGroupBounds(doc.bounds, doc.bounds, dest, deltaYaw, factor, transformed)) return fail("record 0: invalid document transform");
      placed.bounds = transformed; }
    // C4-ENVELOPE-END
    std::string error;
    // C4-VALIDATE-BEGIN
    if (!proj_codec::Validate(placed, error)) return fail(error);
    std::vector<proj_codec::EngineRow> narrowed;   // double->float and tiled-int16 representability before any mutation
    if (!proj_codec::NarrowForEngine(placed, narrowed, error)) return fail(error);
    // C4-VALIDATE-END
    // One C5 row per data record: the caller-owned receipt of the actual engine outcomes.
    std::vector<std::string> prefabs;
    prefabs.reserve(placed.records.size());
    for (const auto& record : placed.records) prefabs.push_back(record.prefab);
    rep.request = BeginPlaceRequest(prefabs);
    rep.requested = (int)placed.records.size();
    std::map<int, int> groups;   // source partition -> fresh independent session id (zero stays zero)
    std::vector<int> admitted(placed.records.size(), 0);
    for (size_t i = 0; i < placed.records.size(); ++i) {
        const std::string& prefab = placed.records[i].prefab;
        if (!PrefabKnown(prefab)) {   // a valid missing prefab is a NAMED exclusion
            PlaceRowExclude(rep.request, (int)i, "missing prefab");
            rep.excludedPrefabs.push_back(prefab);
            continue;
        }
        // C4-PARTITION-BEGIN
        int group = 0;
        if (placed.records[i].group) {
            auto it = groups.find(placed.records[i].group);
            if (it == groups.end()) it = groups.emplace(placed.records[i].group, NewGroupId()).first;
            group = it->second;
        }
        placed.records[i].group = group;
        // C4-PARTITION-END
        const proj_codec::EngineRow& row = narrowed[i];
        const int uid = SubmitPlaceRow(rep.request, (int)i, { row.x, row.y, row.z }, Rot{ row.yaw, row.pitch, row.roll }, row.scale, group);
        admitted[i] = uid;
        if (uid) { uids.push_back(uid); SetObjectNote(uid, placed.records[i].note); }
        else rep.excludedPrefabs.push_back(prefab);   // the admission itself refused: a named exclusion, not a success
    }
    placed.groupNames.clear();
    for (const auto& name : doc.groupNames) {
        const auto mapped = groups.find(name.first);
        if (mapped != groups.end()) { placed.groupNames.emplace(mapped->second, name.second); SetGroupName(mapped->second, name.second); }
    }
    rep.admitted = (int)uids.size();
    rep.excluded = (int)rep.excludedPrefabs.size();
    if (uids.empty()) return fail("record 0: no placeable members");
    // Bind the per-copy envelope provenance: the saved bounds stay authoritative and the pivot is never
    // recomputed from the receiver's measurements (not even for a single surviving member).
    if (!RetainGroupValues(placed, admitted, error)) {
        for (int uid : uids) { HideUid(uid); ForgetUid(uid); }
        uids.clear(); rep.admitted = 0;
        return fail(error);
    }
    pivot = target;
    rep.valid = true;
    Log("group admission: %d requested, %d admitted, %d excluded at (%.2f %.2f %.2f) yaw %.1f scale %.3f",
        rep.requested, rep.admitted, rep.excluded, target.x, target.y, target.z, deltaYaw, factor);
    return rep;
}
#ifdef WB_UNIFIED_HOST_TEST
// One-shot file fault + pre-replace hook for the ProjectLifecycle suite (never compiled into the shipped ASI).
static void InstallSaveFaultHooks(proj_codec::WriteHooks& hooks) {
    hooks.fault = [](proj_codec::WriteStage stage, const std::string&, size_t& length, std::string& faultError) -> bool {
        const int fault = g_saveFaultStage;
        if (fault == 0) return true;
        if (fault == 1 && stage == proj_codec::WriteStage::Write) { g_saveFaultStage = 0; faultError = "record 0: injected write failure"; return false; }
        if (fault == 2 && stage == proj_codec::WriteStage::Write) { g_saveFaultStage = 0; length /= 2; return true; }   // short write: the readback catches it
        if (fault == 3 && stage == proj_codec::WriteStage::Flush) { g_saveFaultStage = 0; faultError = "record 0: injected flush failure"; return false; }
        if (fault == 4 && stage == proj_codec::WriteStage::Replace) { g_saveFaultStage = 0; faultError = "record 0: injected replace failure"; return false; }
        if (fault == 5 && stage == proj_codec::WriteStage::Close) { g_saveFaultStage = 0; faultError = "record 0: injected close failure"; return false; }
        return true;
    };
    hooks.beforeReplace = [](std::string&) -> bool {
        if (g_beforeReplaceCallback) { std::function<void()> callback = std::move(g_beforeReplaceCallback); g_beforeReplaceCallback = {}; callback(); }
        return true;
    };
}
#endif

static bool SameTerrainStrokes(const std::vector<TerrainStroke>& a, const std::vector<TerrainStroke>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i]; const auto& y = b[i];
        if (x.mode != y.mode || x.proj != y.proj || x.x != y.x || x.z != y.z || x.r != y.r ||
            x.amount != y.amount || x.strength != y.strength || x.ax != y.ax || x.az != y.az || x.y != y.y ||
            x.tileScoped != y.tileScoped || x.tileX != y.tileX || x.tileZ != y.tileZ) return false;
    }
    return true;
}

static bool InSaveScope(int scope, int pid, int proj) {
    return scope == SaveWholeScene || (scope == SaveProjectAndNew && (proj == pid || proj == 0)) ||
        (scope == SaveNewOnly && proj == 0) || (scope == SaveProjectOnly && proj == pid);
}
static proj_codec::NpcRecord NpcValue(const ManagedNpc& n) {
    return { n.key, { n.pos.x, n.pos.y, n.pos.z }, n.type, n.extra, n.aiEnabled, n.behavior, n.group, n.label, n.note };
}
static bool SameObjectSnapshot(const SpawnedObj& a, const SpawnedObj& b) {
    return a.uid == b.uid && a.gen == b.gen && a.proj == b.proj && a.hidden == b.hidden && a.group == b.group &&
        a.note == b.note && SamePose(ValueRecord(a), ValueRecord(b));
}
static bool SameNpcSnapshot(const ManagedNpc& a, const ManagedNpc& b) {
    return a.uid == b.uid && a.gen == b.gen && a.proj == b.proj && a.hidden == b.hidden && a.key == b.key &&
        a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z && a.type == b.type && a.extra == b.extra &&
        a.aiEnabled == b.aiEnabled && a.behavior == b.behavior && a.group == b.group && a.label == b.label && a.note == b.note;
}
bool ProjectMutationPending(int id) {
    std::lock_guard<std::recursive_mutex> files(g_fileMutex);
    const std::string name = ProjectNameOf(id);
    for (const auto* work : g_fileActivities)
        if (work->kind == proj_codec::Kind::Project && FileNameEqual(work->name, name)) return true;
    std::vector<PlaceRequestHandle> requests;
    { std::lock_guard<std::mutex> operation(g_groundOpMutex);
      if (g_groundWorldWriting) return true;
      REG_LOCK;
      for (const auto& o : g_reg) if (o.proj == id) {
          if (g_groundLeases.count(o.uid) || g_groundMutationPending.count(o.uid) || (!o.hidden && (!o.obj || o.standin))) return true;
          if (auto request = o.placeReq.lock()) requests.push_back(std::move(request));
      }
      for (const auto& n : g_npcReg) if (n.proj == id && !n.hidden &&
          (n.spawnPending || (!n.actor && n.spawnRequestTick) || n.editMoving || n.liveMovePending)) return true;
    }
    for (const auto& request : requests) if (!PlaceRequestState(request).settled) return true;
    return false;
}

bool SaveProject(const std::string& rawName, int scope) {
    // spaces inside the name are fine, but leading/trailing ones are cut: autoload.txt is one name per line and trims blanks,
    // so " camp" on disk could never be autoloaded again
    size_t b = rawName.find_first_not_of(" \t"), e = rawName.find_last_not_of(" \t");
    if (b == std::string::npos) return false;
    const std::string name = rawName.substr(b, e - b + 1);
    if (IsGroupFileName(name) || !ValidFileName(name + ".cdproj", proj_codec::Kind::Project) || scope < SaveWholeScene || scope > SaveProjectOnly) {
        g_projectError = "record 0: invalid project name or save scope"; Log("save: %s", g_projectError.c_str()); return false;
    }
    FileActivity fileWork(proj_codec::Kind::Project, name, FileReason::PendingOperation);
    const int pid = ProjectId(name);
    // 1. authoritative value builder: build the whole document from the live records under the registry lock and
    //    remember each record's project at capture time, so adoption can never overwrite a newer adoption.
    proj_codec::Document doc; std::string text, error;
    std::vector<SpawnedObj> objects; std::vector<ManagedNpc> npcs;
    ReconcileManagedNpcActors();
    {
        std::lock_guard<std::mutex> operation(g_groundOpMutex);
        std::lock_guard<std::mutex> l(g_regMutex);
        if (g_groundWorldWriting) { g_projectError = "record 0: world transition in progress"; return false; }
        for (const auto& o : g_reg) if (!o.hidden && InSaveScope(scope, pid, o.proj)) {
            if (g_groundLeases.count(o.uid) || g_groundMutationPending.count(o.uid)) { g_projectError = "record 0: grounding still applying"; return false; }
            objects.push_back(o);
        }
        if (!ValueDocumentLocked(objects, doc, error)) { g_projectError = error; Log("save: %s", error.c_str()); return false; }
        for (const auto& n : g_npcReg) if (!n.hidden && InSaveScope(scope, pid, n.proj)) {
            if (n.editMoving || n.liveMovePending) { g_projectError = "record 0: NPC edit still applying"; return false; }
            npcs.push_back(n); doc.npcs.push_back(NpcValue(n));
            const auto group = g_groupNames.find(n.group);
            if (group != g_groupNames.end()) doc.groupNames.emplace(group->first, group->second);
        }
    }
    const auto terrain = TerrainStrokes();
    for (const auto& t : terrain) {
        if (scope == SaveProjectAndNew && !(t.proj == pid || t.proj == 0)) continue;
        if (scope == SaveNewOnly && t.proj != 0) continue;
        if (scope == SaveProjectOnly && t.proj != pid) continue;
        doc.terrain.push_back({ t.mode, t.x, t.z, t.r, t.amount, t.strength, t.ax, t.az, t.y,
                                t.tileScoped, t.tileX, t.tileZ });
    }
    std::vector<proj_codec::EngineRow> narrowed;
    if (!proj_codec::NarrowForEngine(doc, narrowed, error) || !proj_codec::Serialize(doc, text, error)) { g_projectError = error; Log("save: invalid document: %s", error.c_str()); return false; }
    const std::string path = ProjPath(name);
    if (!CreateDirectoryA(ProjDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { g_projectError = "record 0: cannot create the projects directory"; Log("save: %s", g_projectError.c_str()); return false; }
    // 2. transactional write: same-directory temp, flush, readback against the intended bytes, then replace.
    //    Every failure leaves the previous file bytes untouched and removes only the owned temp file.
    proj_codec::WriteHooks hooks; proj_codec::WriteHooks* writeHooks = nullptr;
#ifdef WB_UNIFIED_HOST_TEST
    InstallSaveFaultHooks(hooks);
    writeHooks = &hooks;
#endif
    if (!proj_codec::WriteTransactional(path, proj_codec::Kind::Project, text, writeHooks, error)) {
        g_projectError = error; Log("save: write or replace failed: %s (%s)", path.c_str(), error.c_str()); return false;
    }
    // 3. adoption: a record becomes a member only while its project still is what was captured; if a newer
    //    adoption happened during the write it is kept and the project stays dirty (its file may not match).
    bool stale = !SameTerrainStrokes(terrain, TerrainStrokes());
    if (!stale) {
        if (scope == SaveWholeScene) { for (const auto& t : terrain) if (t.proj != pid) TerrainSetProject(t.proj, pid); }
        else if (scope != SaveProjectOnly) TerrainSetProject(0, pid);
    } // never adopt a newer terrain snapshot that was not written
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        size_t currentObjects = 0, currentNpcs = 0;
        for (const auto& o : g_reg) if (!o.hidden && InSaveScope(scope, pid, o.proj)) ++currentObjects;
        for (const auto& n : g_npcReg) if (!n.hidden && InSaveScope(scope, pid, n.proj)) ++currentNpcs;
        stale |= currentObjects != objects.size() || currentNpcs != npcs.size();
        std::set<int> usedGroups;
        for (const auto& o : objects) if (o.group) usedGroups.insert(o.group);
        for (const auto& n : npcs) if (n.group) usedGroups.insert(n.group);
        for (int group : usedGroups) {
            const auto saved = doc.groupNames.find(group), current = g_groupNames.find(group);
            const std::string before = saved == doc.groupNames.end() ? std::string() : saved->second;
            const std::string after = current == g_groupNames.end() ? std::string() : current->second;
            stale |= before != after; // adding the first name is also a newer metadata edit
        }
        for (const auto& saved : objects) {
            const int i = IndexOfUidLocked(saved.uid);
            if (i < 0 || !SameObjectSnapshot(saved, g_reg[(size_t)i])) { stale = true; continue; }
            g_reg[(size_t)i].proj = pid;
        }
        for (const auto& saved : npcs) {
            const int i = NpcIndexOfUidLocked(saved.uid);
            if (i < 0 || !SameNpcSnapshot(saved, g_npcReg[(size_t)i])) { stale = true; continue; }
            g_npcReg[(size_t)i].proj = pid;
        }
        if (stale) g_projDirty.insert(pid);
        else if (scope == SaveWholeScene) g_projDirty.clear(); else g_projDirty.erase(pid);
    }
    g_projectError.clear();
    Log("save: %zu objects, %zu NPCs, %zu terrain strokes (scope %d) -> %s%s", objects.size(), npcs.size(), doc.terrain.size(), scope, path.c_str(), stale ? " (newer scene data was kept; the project stays dirty)" : "");
    return true;
}

// ---- C6: protected group export ------------------------------------------------------------------------
// The two authorities of an export are the editor's published selection/project/placement context and the
// core's registry. Neither is reduced to a revision counter: each final boundary re-derives the semantic
// document with the SAME authoritative builder (ValueDocumentLocked) and compares values, and the protected
// replacement holds both authorities across the atomic rename.
static std::mutex g_exportCtxMutex;   // guards g_exportContext only; the export guard takes it BEFORE g_regMutex
static ExportContext g_exportContext;
#ifdef WB_UNIFIED_HOST_TEST
// Observation seam of the Export suite (never compiled into the shipped ASI): runs inside the protected region,
// after the last authoritative comparison and before the rename. Not a substitute writer.
static std::function<void()> g_exportReplaceProbe;
#endif
void PublishExportContext(const ExportContext& context) {
    std::lock_guard<std::mutex> l(g_exportCtxMutex);
    g_exportContext = context;
}
ExportContext PublishedExportContext() {
    std::lock_guard<std::mutex> l(g_exportCtxMutex);
    return g_exportContext;
}
static std::string GroupDir() { return g_modDir + "\\Groups"; }
static std::string GroupPath(const std::string& name) { return GroupDir() + "\\" + name + ".cdgroup"; }
static bool FileAt(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
// One path component inside Groups\: no separators, no reserved characters, no extension (the export adds
// .cdgroup), never a directory name. An invalid name must never become a path outside the group folder.
static bool ValidateExportName(const std::string& name, std::string& error) {
    if (name.empty()) { error = "record 0: empty group name"; return false; }
    if (name.size() > 100) { error = "record 0: group name too long"; return false; }
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) { error = "record 0: invalid group name"; return false; }
    if (name == "." || name == ".." || name.back() == '.' || name.back() == ' ') { error = "record 0: invalid group name"; return false; }
    const size_t n = name.size();
    const bool groupExt = n >= 8 && _stricmp(name.c_str() + n - 8, ".cdgroup") == 0;
    const bool projExt = n >= 7 && _stricmp(name.c_str() + n - 7, ".cdproj") == 0;
    if (groupExt || projExt) { error = "record 0: the name must not carry a file extension"; return false; }
    return true;
}
static bool SameUidSet(std::vector<int> a, std::vector<int> b) {
    std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end());
    std::sort(b.begin(), b.end()); b.erase(std::unique(b.begin(), b.end()), b.end());
    return a == b;
}
static bool SameContext(const ExportContext& a, const ExportContext& b) {
    return a.name == b.name && a.placing == b.placing && SameUidSet(a.selection, b.selection) && SameUidSet(a.carried, b.carried);
}
// Compare semantic values before canonical id remapping: swapping partitions must invalidate an approval.
static bool SameDocument(const proj_codec::Document& a, const proj_codec::Document& b) { return proj_codec::SameValues(a, b); }
// What one selection becomes: the included records (registry order, with their project ids) and one named
// exclusion per selected object that cannot be exported. Runs under g_regMutex.
struct ExportPlan {
    std::vector<int> included;
    std::vector<int> projects;
    std::vector<ExportExclusion> excluded;
    std::vector<SpawnedObj> objects;
};
static void ExportPlanLocked(const std::vector<int>& selection, ExportPlan& plan) {
    plan = ExportPlan();
    std::set<int> selected(selection.begin(), selection.end());
    selected.erase(0);
    for (const auto& o : g_reg) {
        if (!selected.erase(o.uid)) continue;
        std::string reason;
        if (o.hidden) reason = "hidden";
        else if (o.standin) reason = "stand-in during a retry";
        else if (!PrefabKnown(o.prefab)) reason = "missing prefab";
        if (!reason.empty()) { plan.excluded.push_back(ExportExclusion{ o.uid, o.prefab, reason }); continue; }
        plan.included.push_back(o.uid);
        plan.projects.push_back(o.proj);
        plan.objects.push_back(o);
    }
    for (int uid : selected) plan.excluded.push_back(ExportExclusion{ uid, std::string(), "forgotten selection" });
}
// The guard both write boundaries run: the published context must still be the one the approval was bound to,
// the inclusion/exclusion plan must re-derive exactly, the values and envelopes must rebuild identically, and a
// destination that exists right now needs the explicit overwrite approval. The caller holds g_exportCtxMutex
// and g_regMutex (that order); no engine call, no queue push happens here.
static bool ExportAuthoritativeLocked(const GroupExportApproval& approval, std::string& error) {
    // C6-GUARD-BEGIN
    if (!SameContext(approval.context, g_exportContext)) { error = "record 0: the selection or placement context changed; review the export again"; return false; }
    ExportPlan plan;
    ExportPlanLocked(approval.context.selection, plan);
    if (plan.included != approval.included || plan.projects != approval.projects) { error = "record 0: the included objects changed; review the export again"; return false; }
    if (plan.excluded.size() != approval.excluded.size()) { error = "record 0: the excluded objects changed; review the export again"; return false; }
    for (size_t i = 0; i < plan.excluded.size(); ++i)
        if (plan.excluded[i].uid != approval.excluded[i].uid || plan.excluded[i].prefab != approval.excluded[i].prefab || plan.excluded[i].reason != approval.excluded[i].reason)
        { error = "record 0: the exclusion reasons changed; review the export again"; return false; }
    proj_codec::Document now;
    if (!ValueDocumentLocked(plan.objects, now, error)) return false;   // names the failing data record
    now.kind = proj_codec::Kind::Group;
    if (!proj_codec::Validate(now, error)) return false;
    std::vector<proj_codec::EngineRow> narrowed;
    if (!proj_codec::NarrowForEngine(now, narrowed, error)) return false;
    if (!SameDocument(now, approval.document)) { error = "record 0: the approved values or envelopes changed; review the export again"; return false; }
    if (FileAt(approval.path) && !approval.overwriteApproved) { error = "record 0: the destination file exists; explicit overwrite approval required"; return false; }
    // C6-GUARD-END
    return true;
}
// The protected replacement: both authorities are held from the last comparison through the atomic rename, so
// nothing can change in between (only the comparison and the file rename run under these locks).
static bool ExportReplaceGuarded(const GroupExportApproval& approval, const std::string& temp, const std::string& destination, std::string& error) {
    // C6-PROTECT-BEGIN
    std::lock_guard<std::mutex> ctx(g_exportCtxMutex);
    std::lock_guard<std::mutex> reg(g_regMutex);
    if (!ExportAuthoritativeLocked(approval, error)) return false;
#ifdef WB_UNIFIED_HOST_TEST
    if (g_exportReplaceProbe) { std::function<void()> probe = std::move(g_exportReplaceProbe); g_exportReplaceProbe = {}; probe(); }
#endif
    if (MoveFileExA(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "record 0: replace failed"; return false;
    }
    // C6-PROTECT-END
    return true;
}

bool PrepareGroupExport(const std::vector<int>& selection, const std::string& name, bool overwriteApproved,
                        GroupExportApproval& approval) {
    approval = GroupExportApproval();
    auto fail = [&](const std::string& why) { approval.error = why; Log("export preflight: %s", why.c_str()); return false; };
    if (!ValidateExportName(name, approval.error)) return fail(approval.error);
    approval.name = name;
    approval.path = GroupPath(name);
    approval.overwriteApproved = overwriteApproved;
    if (selection.empty()) return fail("record 0: no selected objects");
    // The approval binds the PUBLISHED context: a selection or destination name the editor did not publish
    // could not have been reviewed, so preflight refuses it instead of guessing.
    // C6-CONTEXT-BEGIN
    const ExportContext live = PublishedExportContext();
    if (!SameUidSet(live.selection, selection)) return fail("record 0: the selection was not published");
    if (live.name != name) return fail("record 0: the export context was not published for this name");
    approval.context = live;
    // C6-CONTEXT-END
    // 1. authoritative value builder, one document for the captured records (same builder SaveProject uses)
    proj_codec::Document doc;
    std::string error;
    bool built = false;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        ExportPlan plan;
        ExportPlanLocked(selection, plan);
        // The named inclusions/exclusions are part of the approval even when nothing placeable is left, so the
        // editor can show WHY the selection cannot be exported.
        approval.included = plan.included; approval.projects = plan.projects; approval.excluded = plan.excluded;
        // C6-NONEMPTY-BEGIN
        if (plan.included.empty()) error = "record 0: no placeable selected objects";
        else if (ValueDocumentLocked(plan.objects, doc, error)) {
            doc.kind = proj_codec::Kind::Group;
            built = true;
        }
        // C6-NONEMPTY-END
    }
    if (!built) return fail(error);
    std::string text;   // Serialize validates the approved document and re-parses its own canonical output
    if (!proj_codec::Serialize(doc, text, error)) return fail(error);
    approval.document = std::move(doc);
    // C6-OVERWRITE-BEGIN
    if (FileAt(approval.path) && !overwriteApproved) return fail("record 0: the destination file exists; explicit overwrite approval required");
    // C6-OVERWRITE-END
    approval.error.clear();
    approval.valid = true;
    Log("export preflight: %zu selected, %d included, %d excluded -> %s%s", selection.size(), (int)approval.included.size(),
        (int)approval.excluded.size(), approval.path.c_str(), overwriteApproved ? " (overwrite approved)" : "");
    return true;
}

bool WriteGroupExport(const GroupExportApproval& approval, std::string& error) {
    FileActivity fileWork(proj_codec::Kind::Group, approval.name, FileReason::InFlightExport);
    error.clear();
    if (!approval.valid) { error = "record 0: the approval is not valid"; Log("export: %s", error.c_str()); return false; }
    if (approval.document.kind != proj_codec::Kind::Group || approval.document.records.empty()) {
        error = "record 0: not a group document"; Log("export: %s", error.c_str()); return false;
    }
    if (!ValidateExportName(approval.name, error)) { Log("export: %s", error.c_str()); return false; }
    if (approval.path != GroupPath(approval.name)) { error = "record 0: the approval path does not match its name"; Log("export: %s", error.c_str()); return false; }
    // 1. BEFORE-WRITE: both authorities, before any file is touched.
    {
        std::lock_guard<std::mutex> ctx(g_exportCtxMutex);
        std::lock_guard<std::mutex> reg(g_regMutex);
        if (!ExportAuthoritativeLocked(approval, error)) { Log("export refused before writing: %s", error.c_str()); return false; }
    }
    std::string text;
    if (!proj_codec::Serialize(approval.document, text, error)) { Log("export: %s", error.c_str()); return false; }
    if (!CreateDirectoryA(GroupDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "record 0: cannot create the Groups directory"; Log("export: %s", error.c_str()); return false;
    }
    // 2. same-directory temporary through the ONE transactional writer; the final replacement revalidates both
    //    authorities and renames while they are held (hooks.replace), so the pre-replace callback runs first and
    //    the guard sees its mutation.
    proj_codec::WriteHooks hooks;
    hooks.replace = [&approval](const std::string& temp, const std::string& destination, std::string& err) {
        return ExportReplaceGuarded(approval, temp, destination, err);
    };
#ifdef WB_UNIFIED_HOST_TEST
    InstallSaveFaultHooks(hooks);   // the one-shot file faults and the deterministic pre-replace callback
#endif
    if (!proj_codec::WriteTransactional(approval.path, proj_codec::Kind::Group, text, &hooks, error)) {
        Log("export failed: %s (%s)", approval.path.c_str(), error.c_str());
        return false;
    }
    error.clear();
    Log("export: wrote %s: %zu records, %d included, %d excluded", approval.path.c_str(), approval.document.records.size(),
        (int)approval.included.size(), (int)approval.excluded.size());
    return true;
}
static void ClearSceneContents(std::unique_lock<std::mutex>& operation) {
    // Caller holds the ground-operation lock after synchronous admission or owns the validated-load reservation.
    // v0.97 TerrainClear marks projects dirty after releasing its own lock. Never call it under the registry lock.
    TerrainClear();
    std::vector<SpawnedObj> removed; std::vector<ManagedNpc> npcs;
    { REG_LOCK; removed.swap(g_reg); npcs.swap(g_npcReg); for (const auto& n : npcs) if (!n.hidden) RememberUnboundNpcCleanupLocked(n); g_copyMembers.clear(); g_groupNames.clear(); g_projDirty.clear(); }
    operation.unlock();
    for (const auto& o : removed) {
        SettleRowOnce(o.placeReq, o.placeRow, PlaceCanceled, PlaceLaneNone, o.uid, "the scene was cleared before attachment");
        auto work = TrackProjectNativeWork(o.proj);
        if (!o.hidden && o.gimmick && !o.standin) { if (o.actor) RunOnServerTick([actor = o.actor, work]() { RemoveSpawnedActor(actor); }); }
        else if (!o.hidden && o.obj) RunOnGameThread([obj = o.obj, work]() { DoRemove(obj); });
    }
    for (const auto& n : npcs) if (!n.hidden && n.actor) {
        auto work = TrackProjectNativeWork(n.proj);
        RunOnServerTick([actor = n.actor, work]() { RemoveSpawnedActor(actor); });
    }
    Log("delete all: %zu objects, %zu NPCs", removed.size(), npcs.size());
}
bool ClearScene() {
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    if (g_groundWorldWriting || !g_groundLeases.empty() || !g_groundMutationPending.empty()) {
        g_projectError = "record 0: project mutation busy; retry after grounding or travel settles"; return false;
    }
    GroundEpochLocked();
    ClearSceneContents(operation);
    g_projectError.clear();
    return true;
}
void DeleteAllSpawned() {
    if (!ClearScene()) Log("delete all refused: %s", g_projectError.c_str());
}
static bool UnloadProjectImpl(int id, bool ownsWorld) {
    if (id <= 0) { g_projectError = "record 0: invalid project id"; return false; }
    auto work = std::make_shared<FileActivity>(proj_codec::Kind::Project, ProjectNameOf(id), FileReason::PendingOperation);
    std::unique_lock<std::mutex> operation(g_groundOpMutex);
    std::vector<int> ids;
    { REG_LOCK; for (const auto& o : g_reg) if (o.proj == id) ids.push_back(o.uid); }
    if ((!ownsWorld && g_groundWorldWriting) || std::any_of(ids.begin(), ids.end(), [](int uid) {
            return g_groundLeases.count(uid) || g_groundMutationPending.count(uid);
        })) { g_projectError = "record 0: project mutation busy; retry after grounding or travel settles"; return false; }
    if (!ids.empty()) for (auto& weak : g_groundOps) if (auto op = weak.lock())
        if (GroundTouches(op, ids)) GroundEndLocked(op, GroundInvalidated, "epoch-invalidated");
    std::vector<SpawnedObj> removed; std::vector<ManagedNpc> npcs;
    int terrainCount = 0; for (const auto& t : TerrainStrokes()) if (t.proj == id) ++terrainCount;
    { REG_LOCK;
      for (auto it = g_reg.begin(); it != g_reg.end();) {
          if (it->proj != id) { ++it; continue; }
          ForgetCopyValueLocked(it->uid); removed.push_back(*it); it = g_reg.erase(it);
      }
      for (auto it = g_npcReg.begin(); it != g_npcReg.end();) {
          if (it->proj != id) { ++it; continue; }
          if (!it->hidden) RememberUnboundNpcCleanupLocked(*it);
          npcs.push_back(*it); it = g_npcReg.erase(it);
      }
      if (!removed.empty() || !npcs.empty() || terrainCount) g_projDirty.erase(id);
    }
    if (terrainCount) TerrainReplaceProject(id, {});
    operation.unlock();
    for (const auto& o : removed) {
        SettleRowOnce(o.placeReq, o.placeRow, PlaceCanceled, PlaceLaneNone, o.uid, "the project was unloaded before attachment");
        if (!o.hidden && o.gimmick && !o.standin) { if (o.actor) RunOnServerTick([actor = o.actor, work]() { RemoveSpawnedActor(actor); }); }
        else if (!o.hidden && o.obj) RunOnGameThread([obj = o.obj, work]() { DoRemove(obj); });
    }
    for (const auto& n : npcs) if (!n.hidden && n.actor) RunOnServerTick([actor = n.actor, work]() { RemoveSpawnedActor(actor); });
    const bool changed = !removed.empty() || !npcs.empty() || terrainCount != 0;
    g_projectError = changed ? std::string() : "record 0: project has no loaded entities";
    Log("unload project %d: %zu objects, %zu NPCs, %d terrain strokes", id, removed.size(), npcs.size(), terrainCount);
    return changed;
}
bool UnloadProject(int id) { return UnloadProjectImpl(id, false); }
static bool g_autoDone = false; static DWORD g_worldSince = 0, g_worldLast = 0;
// Reads and fully validates a project document: the codec parse (kind/grammar/metadata/records) plus the SAME
// double->float and tiled-int16 narrowing the engine consumes. Nothing here mutates the scene, the registry,
// the project table, the queues, the selection or the History.
static bool ReadProjectDocument(const std::string& path, proj_codec::Document& doc, std::vector<proj_codec::EngineRow>& rows, std::string& error) {
    error.clear();
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) { error = "record 0: cannot open the project file"; return false; }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) { error = "record 0: cannot read the project file"; return false; }
    if (!proj_codec::Parse(bytes, path, proj_codec::Kind::Project, doc, error)) return false;
    return proj_codec::NarrowForEngine(doc, rows, error);
}
static std::vector<TerrainStroke> ProjectTerrain(const proj_codec::Document& doc, int pid) {
    std::vector<TerrainStroke> strokes;
    for (const auto& t : doc.terrain)
        strokes.push_back({ t.mode, (float)t.x, (float)t.z, (float)t.r, (float)t.amount, (float)t.strength,
                            (float)t.ax, (float)t.az, (float)t.y, pid, t.tileScoped, t.tileX, t.tileZ });
    return strokes; // ReadProjectDocument validated every float before any scene mutation
}
static std::map<std::string, ProjectLoadReport> g_projectLoads;   // guarded by g_regMutex
bool PreflightProject(const std::string& name) {
    if (IsGroupFileName(name) || !ValidFileName(name + ".cdproj", proj_codec::Kind::Project)) { g_projectError = "record 0: invalid project name"; return false; }
    proj_codec::Document doc; std::vector<proj_codec::EngineRow> rows;
    return ReadProjectDocument(ProjPath(name), doc, rows, g_projectError);
}
static bool AdmitProjectDocument(const std::string& name, const proj_codec::Document& doc,
                                 const std::vector<proj_codec::EngineRow>& rows, bool clearFirst, bool replace) {
    std::string error;
    // Bool is a synchronous mutation outcome, never deferred admission. Busy leaves the scene and allocators untouched.
    { std::lock_guard<std::mutex> operation(g_groundOpMutex);
      if (g_groundWorldWriting || !g_groundLeases.empty() || !g_groundMutationPending.empty()) {
          g_projectError = "record 0: project mutation busy; retry after grounding or travel settles"; return false;
      }
      ++g_groundWorldWriting; GroundEpochLocked();
    }
    struct WorldWrite { ~WorldWrite() { std::lock_guard<std::mutex> lock(g_groundOpMutex); --g_groundWorldWriting; } } worldWrite;
    g_autoDone = true;   // a validated manual load (or replace) counts: the autoload must not add a second copy later
    if (clearFirst) { std::unique_lock<std::mutex> operation(g_groundOpMutex); ClearSceneContents(operation); }
    const int pid = ProjectId(name);
    if (replace) UnloadProjectImpl(pid, true); // this call owns the world reservation; an empty old project is valid
    struct Loading { bool previous = g_loading; Loading() { g_loading = true; } ~Loading() { g_loading = previous; } } loading;
    std::map<int, int> groups;   // ONE map shared by objects, NPCs and named groups
    auto mapGroup = [&](int source) {
        if (!source) return 0;
        const auto found = groups.find(source);
        return found == groups.end() ? groups.emplace(source, NewGroupId()).first->second : found->second;
    };
    std::vector<int> loaded; std::vector<std::string> excludedPrefabs;
    for (size_t i = 0; i < doc.records.size(); ++i) {
        const proj_codec::Record& record = doc.records[i];
        int uid = 0;
        if (rows[i].placeable) {
            uid = SpawnAt(record.prefab, { rows[i].x, rows[i].y, rows[i].z }, Rot{ rows[i].yaw, rows[i].pitch, rows[i].roll }, rows[i].scale, mapGroup(record.group), pid);
            if (uid) SetObjectNote(uid, record.note);
        }
        // A valid row the legacy state wrapper marked hidden/missing, or a refused admission: a named exclusion,
        // never a parser success over malformed data.
        if (!uid) excludedPrefabs.push_back(record.prefab);
        loaded.push_back(uid);
    }
    std::vector<uint32_t> excludedNpcs;
    for (const auto& n : doc.npcs) {
        const int uid = SpawnManagedNpc(n.key, { (float)n.pos.x, (float)n.pos.y, (float)n.pos.z }, n.type, n.extra,
                                       n.aiEnabled, n.behavior, mapGroup(n.group), pid, n.label, n.note);
        if (uid) SetManagedNpcControl(uid, n.aiEnabled, n.behavior); // exact persisted desired state, not the spawn preset's default
        else excludedNpcs.push_back(n.key);
    }
    for (const auto& group : doc.groupNames) SetGroupName(mapGroup(group.first), group.second);
    TerrainReplaceProject(pid, ProjectTerrain(doc, pid));
    if (doc.hasBounds && !RetainGroupValues(doc, loaded, error)) Log("load: copy metadata: %s", error.c_str());
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        ProjectLoadReport report;
        report.valid = true;
        report.requested = (int)doc.records.size();
        report.queued = report.requested - (int)excludedPrefabs.size();
        report.excluded = (int)excludedPrefabs.size();
        report.excludedPrefabs = excludedPrefabs;
        report.requestedNpcs = (int)doc.npcs.size(); report.excludedNpcs = (int)excludedNpcs.size();
        report.queuedNpcs = report.requestedNpcs - report.excludedNpcs; report.excludedNpcKeys = excludedNpcs;
        report.terrainStrokes = (int)doc.terrain.size();
        g_projectLoads[name] = report;
        g_projDirty.erase(pid);   // freshly loaded = in sync with the file
    }
    g_projectError.clear();
    Log("load: %zu requested, %d queued, %zu excluded from %s", doc.records.size(), (int)(doc.records.size() - excludedPrefabs.size()), excludedPrefabs.size(), ProjPath(name).c_str());
    for (const auto& prefab : excludedPrefabs) Log("load excluded: %s", prefab.c_str());
    return true;
}
static bool LoadProjectSnapshot(const std::string& name, bool clearFirst, bool replace) {
    if (IsGroupFileName(name) || !ValidFileName(name + ".cdproj", proj_codec::Kind::Project)) { g_projectError = "record 0: invalid project name"; return false; }
    auto fileWork = std::make_shared<FileActivity>(proj_codec::Kind::Project, name, FileReason::PendingOperation);
    proj_codec::Document doc; std::vector<proj_codec::EngineRow> rows;
    if (!ReadProjectDocument(ProjPath(name), doc, rows, g_projectError)) { Log("load: %s", g_projectError.c_str()); return false; }
    return AdmitProjectDocument(name, doc, rows, clearFirst, replace);
}
bool LoadProject(const std::string& name, bool clearFirst) { return LoadProjectSnapshot(name, clearFirst, false); }
bool ReloadProject(const std::string& name) { return LoadProjectSnapshot(name, false, true); }
std::vector<std::string> ListProjects() {
    std::vector<std::string> out; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((ProjDir() + "\\*.cdproj").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { std::string n = fd.cFileName; out.push_back(n.substr(0, n.size() - 7)); } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}
std::string ProjectError() { return g_projectError; }
// The last successful load receipt for that name; a failed/malformed load publishes none (valid stays false),
// so "malformed data" can never be mistaken for a successful load with exclusions.
ProjectLoadReport ProjectLoadReportFor(const std::string& name) {
    std::lock_guard<std::mutex> l(g_regMutex);
    const auto it = g_projectLoads.find(name);
    return it == g_projectLoads.end() ? ProjectLoadReport() : it->second;
}
// validate-first import: a shared .cdproj is copied into the projects folder only when its bytes parse and
// narrow; the write is transactional, so a failure leaves an existing file of that name untouched.
bool ImportProjectFile(const std::string& path) {
    const std::string base = path.substr(path.find_last_of("\\/") + 1);
    if (base.empty()) { g_projectError = "record 0: empty import file name"; return false; }
    if (IsGroupFileName(base)) { g_projectError = "record 0: group files cannot be imported as projects"; return false; }
    if (base.size() < 7 || _stricmp(base.c_str() + base.size() - 7, ".cdproj") != 0) { g_projectError = "record 0: the import needs a .cdproj file"; return false; }
    FileActivity fileWork(proj_codec::Kind::Project, base.substr(0, base.size() - 7), FileReason::PendingOperation);
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) { g_projectError = "record 0: cannot open the import file"; return false; }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) { g_projectError = "record 0: cannot read the import file"; return false; }
    file.close(); // bytes are the owned snapshot; retaining this handle prevents replacing an already-local source on Windows
    if (file.fail()) { g_projectError = "record 0: cannot close the import file"; return false; }
    proj_codec::Document doc; std::vector<proj_codec::EngineRow> rows; std::string error;
    if (!proj_codec::Parse(bytes, path, proj_codec::Kind::Project, doc, error) || !proj_codec::NarrowForEngine(doc, rows, error)) {
        g_projectError = error; Log("import: %s", error.c_str()); return false;
    }
    if (!CreateDirectoryA(ProjDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { g_projectError = "record 0: cannot create the projects directory"; return false; }
    proj_codec::WriteHooks hooks; proj_codec::WriteHooks* writeHooks = nullptr;
#ifdef WB_UNIFIED_HOST_TEST
    InstallSaveFaultHooks(hooks);
    writeHooks = &hooks;
#endif
    if (!proj_codec::WriteTransactional(ProjDir() + "\\" + base, proj_codec::Kind::Project, bytes, writeHooks, error)) {
        g_projectError = error; Log("import: write or replace failed: %s", error.c_str()); return false;
    }
    g_projectError.clear();
    return true;
}
// validate-first import: shared .cdgroup bytes enter Groups only after parsing and narrowing succeed.
bool ImportGroupFile(const std::string& path) {
    const std::string base = path.substr(path.find_last_of("\\/") + 1);
    if (base.empty()) { g_projectError = "record 0: empty import file name"; return false; }
    if (base.size() < 9 || _stricmp(base.c_str() + base.size() - 8, ".cdgroup") != 0) { g_projectError = "record 0: the import needs a .cdgroup file"; return false; }
    const std::string name = base.substr(0, base.size() - 8);
    std::string nameError;
    if (!ValidateExportName(name, nameError) || !ValidFileName(base, proj_codec::Kind::Group)) {
        g_projectError = nameError.empty() ? "record 0: invalid blueprint file name" : nameError; return false;
    }
    FileActivity fileWork(proj_codec::Kind::Group, name, FileReason::PendingOperation);
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) { g_projectError = "record 0: cannot open the import file"; return false; }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) { g_projectError = "record 0: cannot read the import file"; return false; }
    file.close();
    if (file.fail()) { g_projectError = "record 0: cannot close the import file"; return false; }
    proj_codec::Document doc; std::vector<proj_codec::EngineRow> rows; std::string error;
    if (!proj_codec::Parse(bytes, path, proj_codec::Kind::Group, doc, error) || !proj_codec::NarrowForEngine(doc, rows, error)) {
        g_projectError = error; Log("blueprint import: %s", error.c_str()); return false;
    }
    const std::string destination = GroupDir() + "\\" + base;
    if (GetFileAttributesA(destination.c_str()) != INVALID_FILE_ATTRIBUTES && _stricmp(path.c_str(), destination.c_str()) != 0) {
        g_projectError = "record 0: a blueprint with that filename already exists; rename the imported file first"; return false;
    }
    if (_stricmp(path.c_str(), destination.c_str()) == 0) { g_projectError.clear(); return true; }
    if (!CreateDirectoryA(GroupDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_projectError = "record 0: cannot create the Groups directory"; return false;
    }
    proj_codec::WriteHooks hooks; proj_codec::WriteHooks* writeHooks = nullptr;
#ifdef WB_UNIFIED_HOST_TEST
    InstallSaveFaultHooks(hooks);
    writeHooks = &hooks;
#endif
    if (!proj_codec::WriteTransactional(destination, proj_codec::Kind::Group, bytes, writeHooks, error)) {
        g_projectError = error; Log("blueprint import: write failed: %s", error.c_str()); return false;
    }
    g_projectError.clear();
    return true;
}
// autoload.txt: one project name per line (without .cdproj); lines starting with '#' and blank lines are skipped.
// '#' is only a comment at the start of a line, so a project whose name contains one still works.
// Any number of projects can be listed; they are all loaded into the same scene, in file order.
static std::string AutoloadPath() { return g_modDir + "\\autoload.txt"; }
static std::string AutoloadName(std::string s, bool first) {
    if (first && s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    const size_t b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return {};
    s = s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    if (s[0] == '#') return {};
    if (s.size() > 7 && FileNameEqual(s.substr(s.size() - 7), ".cdproj")) s.resize(s.size() - 7);
    return s;
}
static std::vector<std::string> ParseAutoload(const std::string& bytes) {
    std::vector<std::string> names;
    for (size_t pos = 0; pos < bytes.size();) {
        size_t end = bytes.find('\n', pos); if (end == std::string::npos) end = bytes.size(); else ++end;
        const std::string name = AutoloadName(bytes.substr(pos, end - pos), pos == 0);
        if (!name.empty() && std::none_of(names.begin(), names.end(), [&](const std::string& n) { return FileNameEqual(n, name); })) names.push_back(name);
        pos = end;
    }
    return names;
}
// A missing list means OFF; an unreadable/directory/reparse/malformed list NEVER means OFF for a file gate.
static FileResult ReadAutoload(std::string& bytes, std::vector<std::string>& names) {
    FileHandle h;
    h.h = CreateFileA(AutoloadPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h.h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) { bytes.clear(); names.clear(); return {}; }
        return { FileReason::AutoloadReadFailed, e };
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.h, &info)) return { FileReason::AutoloadReadFailed, GetLastError() };
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return { FileReason::AutoloadReadFailed };
    if (!ReadHandleBytes(h.h, bytes)) return { FileReason::AutoloadReadFailed, GetLastError() };
    if (bytes.find('\0') != std::string::npos) return { FileReason::AutoloadReadFailed };
    names = ParseAutoload(bytes);
    return {};
}
std::vector<std::string> Autoload() {
    std::lock_guard<std::recursive_mutex> l(g_fileMutex);
    std::string bytes; std::vector<std::string> names;
    const FileResult r = ReadAutoload(bytes, names);
    if (!r.ok()) Log("autoload: %s (%lu)", FileReasonCode(r.reason), r.systemError);
    return names;
}
struct AutoloadTemp {
    std::string path;
    ~AutoloadTemp() { if (!path.empty() && !DeleteFileA(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) Log("autoload: temporary cleanup failed: %s (%lu)", path.c_str(), GetLastError()); }
};
static bool AutoloadFault(int stage) {
#ifdef WB_UNIFIED_HOST_TEST
    if (g_saveFaultStage == stage) { g_saveFaultStage = 0; return true; }
#else
    (void)stage;
#endif
    return false;
}
static FileResult WriteAutoload(const std::string& original, const std::string& bytes, const std::vector<std::string>& expected) {
    FileHandle dir; FileResult r = OpenFileDirectory(g_modDir, dir); if (!r.ok()) return r;
    char path[MAX_PATH]{};
    if (!GetTempFileNameA(g_modDir.c_str(), "wba", 0, path)) return { FileReason::WriteFailed, GetLastError() };
    AutoloadTemp temp{ path }; FileHandle h;
    h.h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h.h == INVALID_HANDLE_VALUE) return { FileReason::WriteFailed, GetLastError() };
    if (AutoloadFault(1)) return { FileReason::WriteFailed };
    const size_t length = AutoloadFault(2) ? bytes.size() / 2 : bytes.size();
    for (size_t at = 0; at < length;) {
        const DWORD chunk = (DWORD)(std::min)(length - at, (size_t)65536); DWORD written = 0;
        if (!WriteFile(h.h, bytes.data() + at, chunk, &written, nullptr) || written != chunk) return { FileReason::WriteFailed, GetLastError() };
        at += written;
    }
    if (AutoloadFault(3) || !FlushFileBuffers(h.h)) return { FileReason::WriteFailed, GetLastError() };
    if (!h.close() || AutoloadFault(5)) return { FileReason::WriteFailed, GetLastError() };
    FileHandle verify;
    verify.h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string actual;
    if (verify.h == INVALID_HANDLE_VALUE || !ReadHandleBytes(verify.h, actual) || actual != bytes) return { FileReason::VerifyFailed, GetLastError() };
#ifdef WB_UNIFIED_HOST_TEST
    if (g_beforeReplaceCallback) { auto callback = std::move(g_beforeReplaceCallback); g_beforeReplaceCallback = {}; callback(); }
#endif
    std::vector<std::string> current;
    r = ReadAutoload(actual, current); if (!r.ok()) return r;
    if (actual != original) return { FileReason::StaleTarget };
    if (AutoloadFault(4)) return { FileReason::WriteFailed };
    if (expected.empty()) {
        // Deleting the last enabled list is one atomic namespace operation, preserving v0.95's no-list state.
        if (!DeleteFileA(AutoloadPath().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return { FileReason::WriteFailed, GetLastError() };
    } else if (!MoveFileExA(path, AutoloadPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return { FileReason::WriteFailed, GetLastError() };
    r = ReadAutoload(actual, current); if (!r.ok()) return r;
    if (current != expected || (!expected.empty() && actual != bytes)) return { FileReason::VerifyFailed };
    return {};
}
FileResult SetAutoload(const std::string& rawName, bool on) {
    std::lock_guard<std::recursive_mutex> l(g_fileMutex);
    const std::string name = AutoloadName(rawName, false);
    if (!ValidFileName(name + ".cdproj", proj_codec::Kind::Project) || IsGroupFileName(name) || name.find_first_of("\r\n") != std::string::npos) return { FileReason::InvalidName };
    if (on) {
        const DWORD attrs = GetFileAttributesA(ProjPath(name).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return { FileReason::NotFound, GetLastError() };
        if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return { FileReason::UnsafePath };
        FileDirectories dirs; SavedFile file; file.kind = proj_codec::Kind::Project;
        const FileResult r = OpenFileDirectories(file, false, dirs); if (!r.ok()) return r;
    }
    std::string original; std::vector<std::string> list;
    FileResult r = ReadAutoload(original, list); if (!r.ok()) return r;
    const bool present = std::any_of(list.begin(), list.end(), [&](const std::string& n) { return FileNameEqual(n, name); });
    if (present == on) return {}; // already verified from a fresh disk read, not a cached checkbox
    std::string bytes;
    if (on) { bytes = original; if (!bytes.empty() && bytes.back() != '\n') bytes += '\n'; bytes += name + "\n"; }
    else {
        // Remove every Windows-case alias of ONLY this name; retain all other bytes, comments and file order.
        for (size_t pos = 0; pos < original.size();) {
            size_t end = original.find('\n', pos); if (end == std::string::npos) end = original.size(); else ++end;
            const std::string line = original.substr(pos, end - pos);
            if (!FileNameEqual(AutoloadName(line, pos == 0), name)) bytes += line;
            pos = end;
        }
    }
    r = WriteAutoload(original, bytes, ParseAutoload(bytes));
    Log("autoload: %s %s: %s (%lu)", name.c_str(), on ? "on" : "off", FileReasonCode(r.reason), r.systemError);
    return r;
}

// Install-time preload uses the same validated project document as manual load and import.
// Invalid object OR terrain data must not partially modify the initial streamed world.
static void PreloadAutoloadTerrain() {
    if (!TerrainAvailable()) return;
    int total = 0;
    for (const auto& name : Autoload()) {
        proj_codec::Document doc; std::vector<proj_codec::EngineRow> rows; std::string error;
        if (!ReadProjectDocument(ProjPath(name), doc, rows, error)) { Log("terrain preload: %s: %s", name.c_str(), error.c_str()); continue; }
        if (!doc.terrain.empty()) {
            const int pid = ProjectId(name);
            TerrainReplaceProject(pid, ProjectTerrain(doc, pid)); total += (int)doc.terrain.size();
        }
    }
    TerrainMarkApplied();
    if (total) Log("terrain: %d strokes of the autoload projects preloaded", total);
}

const char* FileReasonCode(FileReason reason) {
    switch (reason) {
#define FILE_REASON(x) case FileReason::x: return #x
        FILE_REASON(None); FILE_REASON(InvalidName); FILE_REASON(InvalidAction); FILE_REASON(NotFound);
        FILE_REASON(StaleTarget); FILE_REASON(ConfirmationMismatch); FILE_REASON(GuardMissing); FILE_REASON(SelectionChanged);
        FILE_REASON(VisibleReference); FILE_REASON(HiddenReference); FILE_REASON(UndoReference); FILE_REASON(RedoReference);
        FILE_REASON(DirtyProject); FILE_REASON(PendingOperation); FILE_REASON(InFlightExport); FILE_REASON(InFlightPlace);
        FILE_REASON(AutoloadEnabled); FILE_REASON(AutoloadReadFailed); FILE_REASON(Collision); FILE_REASON(UnsafePath);
        FILE_REASON(ReadFailed); FILE_REASON(WriteFailed); FILE_REASON(MoveFailed); FILE_REASON(DeleteFailed); FILE_REASON(VerifyFailed);
#undef FILE_REASON
    }
    return "InvalidReason";
}
static FileResult FileCollision(const std::string& directory, const std::string& filename) {
    WIN32_FIND_DATAA fd{}; HANDLE find = FindFirstFileA((directory + "\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); return e == ERROR_FILE_NOT_FOUND ? FileResult{} : FileResult{ FileReason::ReadFailed, e }; }
    bool collision = false;
    do { if (FileNameEqual(fd.cFileName, filename) || (fd.cAlternateFileName[0] && FileNameEqual(fd.cAlternateFileName, filename))) { collision = true; break; } } while (FindNextFileA(find, &fd));
    const DWORD e = GetLastError(); FindClose(find);
    if (collision) return { FileReason::Collision };
    if (e != ERROR_NO_MORE_FILES) return { FileReason::ReadFailed, e };
    return {};
}
FileResult ExecuteFileAction(const FileSelectionHandle& selection, FileAction action,
                             const std::string& typedFilename, const FileExecutionGuard& guard) {
    if (!selection) return { FileReason::StaleTarget };
    const SavedFile& file = selection->file;
    FileResult r = ValidateSavedFile(file); if (!r.ok()) return r;
    const bool moving = action == FileAction::Archive || action == FileAction::Restore;
    if ((action != FileAction::Archive && action != FileAction::Restore && action != FileAction::Purge && action != FileAction::Delete) ||
        (file.archived != (action == FileAction::Restore || action == FileAction::Purge))) return { FileReason::InvalidAction };
    if (!moving && typedFilename != file.filename) return { FileReason::ConfirmationMismatch };
    if (!guard) return { FileReason::GuardMissing };
    // The UI owns History/selection and executes synchronously on its thread. No callback under registry locks.
    const FileReason editor = guard(file); if (editor != FileReason::None) return { editor };
    std::lock_guard<std::recursive_mutex> lifecycle(g_fileMutex);
    const std::string name = file.filename.substr(0, file.filename.size() - strlen(FileExtension(file.kind)));
    for (const auto* work : g_fileActivities) if (work->kind == file.kind && FileNameEqual(work->name, name)) return { work->reason };
    if (file.kind == proj_codec::Kind::Group) {
        for (auto it = g_filePlaces.begin(); it != g_filePlaces.end();) {
            const auto req = it->request.lock();
            if (!req || PlaceRequestState(req).settled) { it = g_filePlaces.erase(it); continue; }
            if (FileNameEqual(it->name, name)) return { FileReason::InFlightPlace };
            ++it;
        }
    }
    FileDirectories dirs; r = OpenFileDirectories(file, false, dirs); if (!r.ok()) return r;
    FileHandle source; FileSelection now;
    // Exclusive open + handle-based rename/delete binds execution to THIS object, not a swapped pathname.
    r = ReadSelectedFile(file, GENERIC_READ | DELETE, 0, source, now); if (!r.ok()) return r;
    if (!SameFileSelection(*selection, now)) return { FileReason::StaleTarget };
    std::string autoBytes; std::vector<std::string> autoNames;
    if (file.kind == proj_codec::Kind::Project) {
        r = ReadAutoload(autoBytes, autoNames); if (!r.ok()) return r;
        for (const auto& n : autoNames) if (FileNameEqual(n, name)) return { FileReason::AutoloadEnabled };
    }
    // Project ids are looked up, never allocated as a side effect of a row/action. Keep both authorities held
    // until the actual namespace change, so an assignment/spawn/dirty mark cannot pass the final check.
    std::lock_guard<std::mutex> projects(g_projMutex);
    std::lock_guard<std::mutex> registry(g_regMutex);
    if (file.kind == proj_codec::Kind::Project) {
        // ProjectId preserves v0.95's CRT name matching. Windows Unicode case aliases can therefore have
        // multiple session ids; ALL aliases must be checked, not just the first (possibly empty) identity.
        std::set<int> ids;
        for (size_t i = 1; i < g_projNames.size(); ++i) if (FileNameEqual(g_projNames[i], name)) ids.insert((int)i);
        for (const auto& o : g_reg) if (ids.count(o.proj)) {
            if (!o.hidden && (!o.obj || o.standin)) return { FileReason::PendingOperation };
            return { o.hidden ? FileReason::HiddenReference : FileReason::VisibleReference };
        }
        for (const auto& n : g_npcReg) if (ids.count(n.proj)) {
            if (!n.hidden && (n.spawnPending || !n.actor || n.editMoving || n.liveMovePending)) return { FileReason::PendingOperation };
            return { n.hidden ? FileReason::HiddenReference : FileReason::VisibleReference };
        }
        for (const auto& stroke : TerrainStrokes()) if (ids.count(stroke.proj)) return { FileReason::VisibleReference };
        for (int pid : ids) if (g_projDirty.count(pid)) return { FileReason::DirtyProject };
    }
    if (moving) {
        FileDirectories destinationDirs;
        SavedFile destination = file; destination.archived = !file.archived;
        r = OpenFileDirectories(destination, action == FileAction::Archive, destinationDirs); if (!r.ok()) return r;
        const std::string directory = FileDirectory(file.kind, destination.archived);
        r = FileCollision(directory, file.filename); if (!r.ok()) return r;
        const std::wstring path = FileWide(directory + "\\" + file.filename);
        const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (!n) return { FileReason::MoveFailed, GetLastError() };
        std::vector<wchar_t> full(n);
        if (!GetFullPathNameW(path.c_str(), n, full.data(), nullptr)) return { FileReason::MoveFailed, GetLastError() };
        const DWORD bytes = (DWORD)(wcslen(full.data()) * sizeof(wchar_t));
        // Win32's wrapper requires a terminated FileName even though FileNameLength excludes the terminator.
        std::vector<unsigned char> buffer(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr; rename->FileNameLength = bytes;
        memcpy(rename->FileName, full.data(), bytes);
        // No REPLACE and no COPY_ALLOWED: a rename on this open file, on the proven same volume, or refusal.
#ifdef WB_UNIFIED_HOST_TEST
        if (g_fileMutationFault == FileMutationFault::Move) { g_fileMutationFault = FileMutationFault::None; return { FileReason::MoveFailed, ERROR_ACCESS_DENIED }; }
#endif
        if (!SetFileInformationByHandle(source.h, FileRenameInfo, rename, (DWORD)buffer.size())) {
            const DWORD e = GetLastError(); return { e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? FileReason::Collision : FileReason::MoveFailed, e };
        }
    } else {
        FILE_DISPOSITION_INFO disposition{ TRUE };
#ifdef WB_UNIFIED_HOST_TEST
        if (g_fileMutationFault == FileMutationFault::Delete) { g_fileMutationFault = FileMutationFault::None; return { FileReason::DeleteFailed, ERROR_ACCESS_DENIED }; }
#endif
        if (!SetFileInformationByHandle(source.h, FileDispositionInfo, &disposition, sizeof disposition)) return { FileReason::DeleteFailed, GetLastError() };
    }
    if (!source.close()) return { FileReason::VerifyFailed, GetLastError() };
    return {};
}
GroupAdmissionReport AdmitGroupFileCopy(const FileSelectionHandle& selection, Vec3 target, double deltaYaw,
                                       double factor, std::vector<int>& uids, Vec3& pivot) {
    std::lock_guard<std::recursive_mutex> l(g_fileMutex);
    GroupAdmissionReport report;
    uids.clear();
    auto fail = [&](FileResult r) { report.error = FileReasonCode(r.reason); return report; };
    if (!selection || selection->file.kind != proj_codec::Kind::Group || selection->file.archived) return fail({ FileReason::InvalidAction });
    const SavedFile& file = selection->file;
    FileResult r = ValidateSavedFile(file); if (!r.ok()) return fail(r);
    FileDirectories dirs; r = OpenFileDirectories(file, false, dirs); if (!r.ok()) return fail(r);
    FileHandle h; FileSelection now;
    r = ReadSelectedFile(file, GENERIC_READ, FILE_SHARE_READ, h, now); if (!r.ok()) return fail(r);
    if (!SameFileSelection(*selection, now)) return fail({ FileReason::StaleTarget });
    proj_codec::Document doc;
    if (!proj_codec::Parse(now.bytes, file.path, proj_codec::Kind::Group, doc, report.error)) return report;
    FileActivity admission(proj_codec::Kind::Group, file.filename.substr(0, file.filename.size() - 8), FileReason::InFlightPlace);
    report = AdmitGroupCopy(doc, target, deltaYaw, factor, uids, pivot);
    if (report.request) g_filePlaces.push_back({ admission.row.name, report.request });
    return report;
}
int PendingSpawns() { return (int)InterlockedCompareExchange(&g_queueCount, 0, 0); }

// ---- C5: per-request caller-owned terminal placement results (public API) --------------------------------
// The request handle is caller-owned: the UI keeps it and reads its own receipt. Rows are bound to records by
// (request, rowId) carried by SpawnedObj, so two requests sharing a basename never share a result and a late
// completion resolves its own row. Nothing here consults PendingSpawns() - that is a queue diagnostic only.
PlaceRequestHandle BeginPlaceRequest(const std::vector<std::string>& prefabs) {
    auto req = std::make_shared<PlaceRequest>();
    for (size_t i = 0; i < prefabs.size(); ++i) { PlaceRow r; r.rowId = (int)i; r.prefab = prefabs[i]; req->rows.push_back(std::move(r)); }
    return req;
}
// Caller-side preflight exclusion (missing/hidden/nonplaceable prefab): the row never spawns.
bool PlaceRowExclude(const PlaceRequestHandle& req, int rowId, const std::string& reason) {
    if (!req) return false;
    return SettleRowOnce(req, rowId, PlaceExcluded, PlaceLaneNone, 0, reason.empty() ? std::string("preflight exclusion") : reason);
}
#ifdef WB_UNIFIED_HOST_TEST
std::function<void()> g_admissionInterrupt;   // B2-R1 admission-interrupt seam (see core.h); null unless a host test arms it
std::function<void()> g_replayRevalidateProbe;   // D1 replay-revalidation interleave seam (host tests only); null unless a host test arms it
#endif
int SubmitPlaceRow(const PlaceRequestHandle& req, int rowId, Vec3 world, Rot rot, float scale, int group) {
    if (!req) return 0;
    std::string prefab; bool found = false;
    { std::lock_guard<std::mutex> l(req->m); for (const auto& r : req->rows) if (r.rowId == rowId && r.state == PlacePending) { prefab = r.prefab; found = true; break; } }
    if (!found) return 0;                                          // unknown row, or already terminal
    if (prefab.empty()) { SettleRowOnce(req, rowId, PlaceExcluded, PlaceLaneNone, 0, "missing prefab"); return 0; }
    const int uid = SpawnAtLinked(prefab, world, rot, scale, group, 0, req, rowId);
    if (!uid) { SettleRowOnce(req, rowId, PlaceExcluded, PlaceLaneNone, 0, "the spawn was not admitted"); return 0; }
#ifdef WB_UNIFIED_HOST_TEST
    if (g_admissionInterrupt) g_admissionInterrupt();   // B2-R1 seam (null unless a test arms it): the record and its work are published, the row UID is not yet written
#endif
    { std::lock_guard<std::mutex> l(req->m); for (auto& r : req->rows) if (r.rowId == rowId) { r.uid = uid; break; } }
    return uid;
}
// Counted per-row cleanup obligations (B2): req->cleanupOutstanding is always the sum of the rows'
// cleanupObligations. Every queued engine removal that must precede finality holds exactly one obligation,
// released by that removal's own lane completion AFTER it dispatched - never at enqueue time. A hide with no
// queued removal releases its obligation synchronously; a hide that never ran releases it on the spot, so no
// obligation is ever left dangling and no second cancel can clear another obligation's share.
static void C5AddObligation(const std::shared_ptr<PlaceRequest>& req, int rowId) {
    if (!req || rowId < 0) return;
    std::lock_guard<std::mutex> l(req->m);
    for (auto& r : req->rows) if (r.rowId == rowId) { ++r.cleanupObligations; r.cleanupPending = true; ++req->cleanupOutstanding; return; }
}
static void C5ReleaseObligation(const std::shared_ptr<PlaceRequest>& req, int rowId) {
    if (!req || rowId < 0) return;
    std::lock_guard<std::mutex> l(req->m);
    for (auto& r : req->rows) if (r.rowId == rowId && r.cleanupObligations > 0) {
        --r.cleanupObligations; r.cleanupPending = r.cleanupObligations > 0;
        if (req->cleanupOutstanding > 0) --req->cleanupOutstanding; return; }
}
// One charged game-lane removal: the obligation is registered BEFORE the queue push and released by the lane
// completion after the removal dispatched. No engine or queue call happens while the request mutex is held.
static void C5QueueChargedGameRemove(const std::shared_ptr<PlaceRequest>& req, int rowId, uintptr_t handle) {
    if (!handle || !GameThreadReady()) return;
    C5AddObligation(req, rowId);
    RunOnGameThread([handle, req, rowId]() { DoRemove(handle); C5ReleaseObligation(req, rowId); });
}
static void C5QueueChargedActorRemove(const std::shared_ptr<PlaceRequest>& req, int rowId, uintptr_t actor) {
    if (!actor) return;
    C5AddObligation(req, rowId);
    RunOnServerTick([actor, req, rowId]() { RemoveSpawnedActor(actor); C5ReleaseObligation(req, rowId); });
}
// B2-R1: whether the owning row is already terminally dead (Canceled/Excluded/Failed). A row still
// Pending, an already Attached row (e.g. a drag-finalize re-attachment), a requestless completion and
// an unknown row all report NOT dead, so their completions keep the previous behavior. No locks held
// on entry; only the request mutex is taken, never while the registry lock is held.
static bool C5RowDead(const std::weak_ptr<PlaceRequest>& w, int rowId) {
    if (rowId < 0) return false;
    const std::shared_ptr<PlaceRequest> req = w.lock();
    if (!req) return false;
    std::lock_guard<std::mutex> l(req->m);
    for (const auto& r : req->rows) if (r.rowId == rowId) return r.state != PlacePending && r.state != PlaceAttached;
    return false;
}
// Remove one member through the existing cancel semantics (HideUid invalidates unapplied work and queues the
// visible member's removal) and attach the cleanup completion to that removal's own lane. A deferred hide
// carries the completion with it, so the obligation cannot clear before the deferred removal executed; when no
// hide happened the obligation is dropped, so no cleanup is ever left dangling.
static void C5CleanupMember(const std::shared_ptr<PlaceRequest>& req, int rowId, int uid) {
    // The hide obligation is reserved by the cancel caller atomically with publishing Canceled (below), never
    // here: the completion travels with the hide, so the obligation cannot clear before the dispatched removal.
    const bool hid = HideUidInternal(uid, [req, rowId]() { C5ReleaseObligation(req, rowId); });
    if (!hid) C5ReleaseObligation(req, rowId);
}
static void C5CleanupAttached(const std::shared_ptr<PlaceRequest>& req, int rowId, int uid) {
    C5CleanupMember(req, rowId, uid);
}
// C5-CANCEL-BEGIN
// The hide obligation is reserved atomically with publishing Canceled under the request mutex (before it is
// released), so a racing in-flight native completion can never observe a transient settled=true: the row's sum
// stays nonzero from the moment it leaves Pending until every queued removal dispatched. A hide that queued no
// removal releases it synchronously; a repeated cancel is a true no-op that neither reserves nor releases.
bool PlaceRowCancel(const PlaceRequestHandle& req, int rowId) {
    if (!req) return false;
    int uid = 0, state = PlacePending; bool found = false, alreadyRemoved = false;
    { std::lock_guard<std::mutex> l(req->m);
      for (auto& r : req->rows) if (r.rowId == rowId) {
          found = true; uid = r.uid; state = r.state;
          if (r.state == PlacePending) { r.state = PlaceCanceled; r.reason = "the row was canceled"; ++r.cleanupObligations; r.cleanupPending = true; ++req->cleanupOutstanding; }
          else if (r.state == PlaceAttached) { if (r.removedAfterAttach) alreadyRemoved = true; else { r.removedAfterAttach = true; ++r.cleanupObligations; r.cleanupPending = true; ++req->cleanupOutstanding; } }
          break; } }
    if (!found || alreadyRemoved) return false;   // B2-R2: an already removed Attached receipt keeps its history; the repeat changes nothing
    if (state == PlaceAttached) { C5CleanupAttached(req, rowId, uid); return true; }
    if (state == PlacePending) {
        C5CleanupMember(req, rowId, uid);
        return true;
    }
    return false;
}
int PlaceRequestCancel(const PlaceRequestHandle& req) {
    if (!req) return 0;
    std::vector<std::pair<int, int>> pending, attached;   // (rowId, uid)
    { std::lock_guard<std::mutex> l(req->m);
      if (req->canceled) return 0;
      req->canceled = true;
      for (auto& r : req->rows) {
          if (r.state == PlacePending) { r.state = PlaceCanceled; r.reason = "the request was canceled"; if (r.uid) { ++r.cleanupObligations; r.cleanupPending = true; ++req->cleanupOutstanding; pending.push_back({ r.rowId, r.uid }); } }
          else if (r.state == PlaceAttached) { r.removedAfterAttach = true; ++r.cleanupObligations; r.cleanupPending = true; ++req->cleanupOutstanding; attached.push_back({ r.rowId, r.uid }); } } }
    int moved = 0;
    for (const auto& p : pending) {
        C5CleanupMember(req, p.first, p.second);
        ++moved;
    }
    for (const auto& a : attached) { C5CleanupAttached(req, a.first, a.second); ++moved; }
    return moved;
}
// C5-CANCEL-END
PlaceRequestView PlaceRequestState(const PlaceRequestHandle& req) {
    PlaceRequestView v;
    if (!req) return v;
    std::lock_guard<std::mutex> l(req->m);
    v.requested = (int)req->rows.size();
    v.requestCanceled = req->canceled;
    // C5-VIEW-BEGIN
    for (const auto& r : req->rows) {
        switch (r.state) {
        case PlaceAttached: ++v.attached; break;
        case PlaceExcluded: ++v.excluded; break;
        case PlaceFailed:   ++v.failed;   break;
        case PlaceCanceled: ++v.canceled; break;
        default:            ++v.pending;  break;
        }
        if (r.cleanupPending) v.cleanupPending = true;
    }
    // C5-VIEW-END
    v.rows = req->rows;
    v.settled = (v.pending == 0 && req->cleanupOutstanding == 0);
    return v;
}
// called from the pump: once the player has been in the world for ~3 s, load the autoload projects once per session.
// A single failed position read (loading screen, camera cut, mount transition) only sets the counter back a little, so the
// load happens seconds after the world is up, not minutes later when the user may already have loaded the scene by hand.
// The settled autoload action: every listed project is loaded in file order. LoadProject validates before it
// changes anything (and rejects a group file), so a bad entry never spawns, clears or allocates. The per-session
// marker is checked and set here, so a manual LoadProject (which sets it too) keeps autoload from adding a second
// copy of the scene. Returns true when this call was the one that settled the listing.
static bool AutoloadProjects() {
    std::lock_guard<std::recursive_mutex> lifecycle(g_fileMutex); // bind the persisted list to its load admissions
    if (g_autoDone) return false;   // a manual load/replace already settled this session
    g_autoDone = true;
    // several projects are loaded into the same scene: every LoadProject only queues spawns and hands out fresh group ids,
    // so the lists simply add up
    std::vector<std::string> names = Autoload();
    for (auto& name : names) { Log("autoload: loading project %s", name.c_str()); LoadProject(name, false); }
    return true;
}
static void AutoloadTick() {
    if (g_autoDone) return;
    const DWORD now = GetTickCount();
    Vec3 p; if (!PlayerWorldPos(&p)) { if (now - g_worldLast > 2000) g_worldSince = 0; return; }   // a dropout of up to 2 s keeps the clock
    g_worldLast = now; if (!g_worldSince) g_worldSince = now;
    if (now - g_worldSince < 3000) return;
    AutoloadProjects();
}



// ---- ray cast tracing (reverse engineering aid): records the game's own hknpWorld::castRay calls (context, world, query,
// collector before/after) so a replay with our own origin can be built from a real call. Console "raytrace".
static uintptr_t kRva_CastRay = 0, kRva_WorldCastRay = 0, kRva_CastShape = 0, kRva_WorldCastShape = 0;
// all four are declared with 8 integer arguments: 4 registers + 4 stack slots are forwarded untouched, so the wrapper is safe for
// functions with up to 8 pointer/integer arguments (castShape uses 7, worldCastRay 3). None of them takes float arguments in xmm.
typedef void* (__fastcall* CastRayFn)(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h);
static CastRayFn g_origCastRay = nullptr, g_origWorldCastRay = nullptr, g_origCastShape = nullptr, g_origWorldCastShape = nullptr;
static volatile LONG g_rayTraceLeft = 0, g_shapeTraceLeft = 0;   // separate budgets: walking floods the shape casts, interaction/aiming produces the ray casts
static std::recursive_mutex g_traceMutex;   // serializes traced calls so the dumps of one call stay together; recursive: worldCastShape calls castShape on the same thread
// dumps the block and, for the first few pointer-sized slots that point somewhere readable, the target's RTTI name and first bytes
static void DumpBlock(const char* what, uintptr_t p, unsigned n);
static void DumpDeep(const char* what, uintptr_t p, unsigned n, unsigned slots) {
    DumpBlock(what, p, n);
    for (unsigned i = 0; i < slots; i++) {
        uintptr_t q = 0; if (!ReadBytes(p + i * 8, &q, 8)) break;
        if (q < 0x10000 || (q >> 47) != 0) continue;
        uint8_t probe[16]; if (!ReadBytes(q, probe, 16)) continue;
        const char* rt = RttiName(q); char sub[48]; snprintf(sub, sizeof sub, "  %s+%X ->", what, i * 8);
        Log("[ray]%s %p in image: %s rtti: %s", sub, (void*)q, InImage(q) ? "yes" : "no", rt ? rt : "-");
        if (!InImage(q)) DumpBlock(sub, q, 0x60);
    }
}
static void DumpBlock(const char* what, uintptr_t p, unsigned n) {
    std::vector<uint8_t> b(n); if (!ReadBytes(p, b.data(), n)) { Log("[ray] %s %p unreadable", what, (void*)p); return; }
    std::string hex, flt; char t[64];
    for (unsigned i = 0; i < n; i++) { snprintf(t, sizeof t, "%02X%s", b[i], (i % 16 == 15) ? " | " : " "); hex += t; }
    for (unsigned i = 0; i + 4 <= n; i += 4) { float f; memcpy(&f, &b[i], 4); if (std::isfinite(f) && fabsf(f) > 1e-5f && fabsf(f) < 1e6f) { snprintf(t, sizeof t, "+%X=%.3f ", i, f); flt += t; } }
    const char* rt = RttiName(p);
    Log("[ray] %s %p (%s): %s", what, (void*)p, rt ? rt : "-", hex.c_str());
    if (!flt.empty()) Log("[ray]   floats: %s", flt.c_str());
}
static void* __fastcall HookCastRay(void* ctx, void* world, void* query, void* collector, void* e, void* f, void* g, void* h) {
    const bool trace = g_rayTraceLeft > 0 && InterlockedDecrement(&g_rayTraceLeft) >= 0;
    if (!trace) return g_origCastRay(ctx, world, query, collector, e, f, g, h);
    std::lock_guard<std::recursive_mutex> lock(g_traceMutex);
    if (trace) {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        Log("[ray] castRay from rva 0x%llx thread %lu: a=%p b=%p c=%p d=%p e=%p f=%p", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(), ctx, world, query, collector, e, f);
        DumpBlock("a", (uintptr_t)ctx, 0x80); DumpBlock("b", (uintptr_t)world, 0x80); DumpBlock("c", (uintptr_t)query, 0x80); DumpBlock("d", (uintptr_t)collector, 0x80); DumpBlock("e", (uintptr_t)e, 0x80);
    }
    void* r = g_origCastRay(ctx, world, query, collector, e, f, g, h);
    if (trace) { DumpBlock("c after", (uintptr_t)query, 0x80); DumpBlock("d after", (uintptr_t)collector, 0x80); Log("[ray] returned %p", r); }
    return r;
}
static void* __fastcall HookWorldCastRay(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    const bool trace = g_rayTraceLeft > 0 && InterlockedDecrement(&g_rayTraceLeft) >= 0;
    if (!trace) return g_origWorldCastRay(a, b, c, d, e, f, g, h);
    std::lock_guard<std::recursive_mutex> lock(g_traceMutex);
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    // signature (from the disassembly): worldCastRay(this = world object, query, collector); d.. are leftovers
    Log("[ray] ===== worldCastRay from rva 0x%llx thread %lu: world=%p query=%p collector=%p", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(), a, b, c);
    DumpDeep("query", (uintptr_t)b, 0xA0, 6); DumpDeep("collector before", (uintptr_t)c, 0x100, 6);
    void* r = g_origWorldCastRay(a, b, c, d, e, f, g, h);
    DumpBlock("collector after", (uintptr_t)c, 0x100); DumpBlock("query after", (uintptr_t)b, 0xA0);
    { uintptr_t buf = 0; if (ReadBytes((uintptr_t)c + 0x20, &buf, 8) && buf) DumpBlock("collector+20 -> hits after", buf, 0x100); }
    Log("[ray] worldCastRay returned %p", r);
    return r;
}
// the character controller probes the ground with shape casts (the exe says so: "performs shapecasts. Using an LOD shape will allow the
// character controller to..."), so a walk records these; ray casts come from interaction prompts, the crosshair and the bow
static void* __fastcall HookCastShape(void* ctx, void* world, void* query, void* collector, void* e, void* f, void* g, void* h) {
    const bool trace = g_shapeTraceLeft > 0 && InterlockedDecrement(&g_shapeTraceLeft) >= 0;
    if (!trace) return g_origCastShape(ctx, world, query, collector, e, f, g, h);
    std::lock_guard<std::recursive_mutex> lock(g_traceMutex);
    if (trace) {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        Log("[shape] castShape from rva 0x%llx thread %lu: a=%p b=%p c=%p d=%p e=%p f=%p g=%p", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(), ctx, world, query, collector, e, f, g);
        DumpBlock("a", (uintptr_t)ctx, 0x80); DumpBlock("b", (uintptr_t)world, 0xA0); DumpBlock("c", (uintptr_t)query, 0xA0); DumpBlock("d", (uintptr_t)collector, 0x80);
        DumpBlock("e", (uintptr_t)e, 0x120); DumpBlock("f (query struct)", (uintptr_t)f, 0x120); DumpBlock("g", (uintptr_t)g, 0x40);
    }
    void* r = g_origCastShape(ctx, world, query, collector, e, f, g, h);
    if (trace) { DumpBlock("e after", (uintptr_t)e, 0x120); DumpBlock("f after", (uintptr_t)f, 0x120); DumpBlock("collector d after", (uintptr_t)collector, 0x100); Log("[shape] returned %p", r); }
    return r;
}
// ---- ground probe (experimental): a real worldCastShape call is copied as a template (query, transform, collector, hit buffer,
// sphere shape) and replayed with our own start point and a downward displacement. Console "probe", button in the Log tab.
// the collector object is at least 0x120 bytes (addHit copies 0xA0 bytes to +0x80) and the query at least 0xC0 (the inner code reads
// +0x8E/+0x90/+0xB8): the copies are generous, a too small collector copy let the game write past our buffer and crash on return
struct CastTemplate { bool have = false; void* world = nullptr; uint8_t query[0x200], xform[0x100], collector[0x200], hits[0x100], shape[0x200]; uintptr_t collAddr = 0, hitsAddr = 0, shapeAddr = 0; };
static CastTemplate g_tpl;
static std::mutex g_tplMutex;
float g_probeRadius = 0.0f; static bool g_probeCalibrated = false; // protected by g_tplMutex
struct GroundReq { int id; Vec3 start; float len; bool verbose; uint64_t epoch; std::weak_ptr<GroundOp> op; bool owned = false; Vec3 dir{ 0, -1, 0 }; bool robust = false; };
struct GroundStored { GroundHit hit; uint64_t epoch; std::weak_ptr<GroundOp> op; bool owned; GroundProbeStatus status = GroundProbeStatus::Pending; };
static std::mutex g_groundMutex; static std::vector<GroundReq> g_groundQueue; static std::map<int, GroundStored> g_groundResults; static int g_groundNext = 0;
static volatile LONG g_groundQueued = 0;
static void PruneGroundTickets() {
    std::set<int> canceled; uint64_t epoch;
    { std::lock_guard<std::mutex> lock(g_groundOpMutex); epoch = g_groundEpoch;
      for (auto& w : g_groundOps) if (auto op = w.lock()) if (op->view.terminal() && op->view.probe) {
          canceled.insert(op->view.probe); op->ticket = 0;
      }
    }
    std::lock_guard<std::mutex> queue(g_groundMutex);
    for (auto it = g_groundResults.begin(); it != g_groundResults.end();) {
        const auto& r = it->second;
        // Owned operations expose invalidation through their receipt. Raw consumers need a terminal ticket
        // even when the queued cast was pruned, so retain its epoch until consumption (or bounded eviction).
        if (r.owned && (canceled.count(it->first) || r.epoch != epoch || r.op.expired())) it = g_groundResults.erase(it); else ++it;
    }
    g_groundQueue.erase(std::remove_if(g_groundQueue.begin(), g_groundQueue.end(), [&](const GroundReq& r) {
        return canceled.count(r.id) || r.epoch != epoch || (r.owned && r.op.expired());
    }), g_groundQueue.end());
}
static void ServiceGroundQueue(void* world);
static void CaptureTemplate(void* world, void* q, void* xf, void* col) {
    { std::lock_guard<std::mutex> lock(g_tplMutex); if (g_tpl.have && g_tpl.world == world) return; }
    uintptr_t hits = 0, shape = 0; ReadBytes((uintptr_t)col + 0x20, &hits, 8); ReadBytes((uintptr_t)q + 0x28, &shape, 8);
    if (!hits || !shape) return;
    { const char* sn = RttiName(shape); if (!sn || !strstr(sn, "hknpSphereShape")) return; }   // the character's ground probe uses a small sphere
    if (hits != (uintptr_t)col + 0x30) return;                                                   // inline hit buffer, as in the captured layout
    {   // only the character's own probe: query start (tile-local) within 3 m of the player. Other sphere casts (camera, cinematics,
        // loading) use other collectors / contexts and replaying those froze or crashed the game.
        PosInfo pi{}; if (!PlayerPosInfo(&pi)) return;
        float qs[3]; if (!ReadBytes((uintptr_t)q + 0x30, qs, 12)) return;
        const float dx = qs[0] - pi.tiled.x, dz = qs[2] - pi.tiled.z, dy = qs[1] - pi.tiled.y;
        if (dx * dx + dz * dz > 9.0f || fabsf(dy) > 3.0f) return;
        if (fabsf(pi.world.x) < 1.0f && fabsf(pi.world.z) < 1.0f) return;   // (0, 1000, 0) is the placeholder while loading
        uintptr_t vt = 0; ReadBytes((uintptr_t)col, &vt, 8); if (!InImage(vt)) return;
        // only the collector class whose hit handling was verified (resolved at startup, see ResolveProbeCollectorVtable)
        if (!kRva_ProbeCollector || vt - g_base != kRva_ProbeCollector) {
            static int s_other = 0;
            if (s_other++ < 3) Log("[probe] sphere cast with another collector class (vtable rva 0x%llx, expected 0x%llx), skipped", (unsigned long long)(vt - g_base), (unsigned long long)kRva_ProbeCollector);
            return;
        }
        static int s_logged = 0; if (s_logged++ < 3) Log("[probe] template candidate: query start local (%.2f %.2f %.2f) player (%.2f %.2f %.2f) collector vtable rva 0x%llx", qs[0], qs[1], qs[2], pi.tiled.x, pi.tiled.y, pi.tiled.z, (unsigned long long)(vt - g_base));
    }
    CastTemplate next;
    if (!ReadBytes((uintptr_t)q, next.query, sizeof next.query) || !ReadBytes((uintptr_t)xf, next.xform, sizeof next.xform) || !ReadBytes((uintptr_t)col, next.collector, sizeof next.collector)
        || !ReadBytes(hits, next.hits, sizeof next.hits) || !ReadBytes(shape, next.shape, sizeof next.shape)) return;
    next.world = world; next.collAddr = (uintptr_t)col; next.hitsAddr = hits; next.shapeAddr = shape; next.have = true;
    { std::lock_guard<std::mutex> operation(g_groundOpMutex); std::lock_guard<std::mutex> lock(g_tplMutex);
      if (g_tpl.world != world || g_tpl.have) return; // superseded capture; publish only to its observed world
      g_tpl = next; g_probeCalibrated = false;
    }
    Log("[probe] template captured: collector %p hits %p (delta 0x%llx) shape %p (%s)", col, (void*)hits, (unsigned long long)(hits - (uintptr_t)col), (void*)shape, RttiName(shape) ? RttiName(shape) : "?");
}
static uintptr_t FindPatternCount(const char* pat, int* count);

// ---- server gimmick spawn (research) ------------------------------------------------------------------------------
// One server-side function creates every functional gimmick actor: it is called with a large parameter block and a spawn
// reason ("housing", "drop", "discardfrominventory", "inspect", "buff", "docking", ...) from 42 places, the housing editor
// and item drops among them. Hooked for tracing only: while "trace game calls" is on, every call dumps the parameter block
// before and after, the out block and the extra stack arguments, so the block layout can be learned from the game's own
// spawns and replayed later for World Builder's objects (the goal: torches that switch, doors that open).
static void ReleaseHookPiece();
static uintptr_t kRva_GimmickSpawn = 0;
extern volatile LONG g_soCreated_; extern volatile uintptr_t g_lastSoCreated_; extern volatile uintptr_t g_lastActorCreated_;
struct SpawnedGimmick { uintptr_t so, actor; char prefab[200]; float pos[3]; DWORD when; };
extern volatile uintptr_t g_replayActor;
// The actor class shares its methods with the player's actor (base class), so every distinct ServerNormalInGameActor whose
// method runs on the spawn thread during a replay is collected; after the create, the one that refers to the new scene object
// (directly or one pointer deep) is the actor the replay made.
static uintptr_t g_replayActors[8]; static int g_replayActorCount = 0; static uintptr_t g_replayCtorActor = 0;
static uintptr_t g_ourActors[256]; static int g_ourActorsN = 0;   // every actor constructed during one of our replays (ring)
static bool IsOurActor(uintptr_t a) { for (int i = 0; i < 256; i++) if (g_ourActors[i] == a) return true; return false; }
static void NoteReplayActorRaw(uintptr_t a) {   // from the constructor hook: the vtable is not installed yet, so no RTTI check here
    if (g_replayActorCount >= 8) return;
    for (int i = 0; i < g_replayActorCount; i++) if (g_replayActors[i] == a) return;
    g_replayActors[g_replayActorCount++] = a; if (!g_replayCtorActor) g_replayCtorActor = a; g_ourActors[g_ourActorsN++ % 256] = a; Log("[vt] replay actor candidate %d: %p (constructed during the replay)", g_replayActorCount, (void*)a);
}
static void NoteReplayActor(uintptr_t a, int slot) {
    if (g_replayActorCount >= 8) return;
    for (int i = 0; i < g_replayActorCount; i++) if (g_replayActors[i] == a) return;
    const char* n = RttiName(a); if (!n || strcmp(n, ".?AVServerNormalInGameActor@pa@@") != 0) return;
    g_replayActors[g_replayActorCount++] = a; Log("[vt] replay actor candidate %d: %p first seen in slot %d", g_replayActorCount, (void*)a, slot);
}
static bool ActorRefersTo(uintptr_t actor, uintptr_t so, int* where) {   // the scene object pointer, up to three pointers deep (offsets moved between builds)
    auto ptrAt = [](uintptr_t at, uintptr_t* v) { return ReadBytes(at, v, 8) && *v > 0x10000 && !(*v >> 47); };
    for (uintptr_t o = 0; o < 0x800; o += 8) { uintptr_t v = 0; if (ReadBytes(actor + o, &v, 8) && v == so) { *where = (int)o; return true; } }
    for (uintptr_t o = 0; o < 0x800; o += 8) { uintptr_t v = 0; if (!ptrAt(actor + o, &v)) continue;
        for (uintptr_t o2 = 0; o2 < 0x200; o2 += 8) { uintptr_t v2 = 0; if (!ptrAt(v + o2, &v2)) continue; if (v2 == so) { *where = (int)(o | (o2 << 12)); return true; }
            for (uintptr_t o3 = 0; o3 < 0x100; o3 += 8) { uintptr_t v3 = 0; if (ReadBytes(v2 + o3, &v3, 8) && v3 == so) { *where = (int)(o | (o2 << 12) | (o3 << 20)); return true; } } } }
    return false;
}
static uintptr_t PickReplayActor(uintptr_t so) {
    for (int i = 0; i < g_replayActorCount; i++) { const char* n = RttiName(g_replayActors[i]); Log("[gimmick] actor candidate %p is a %s", (void*)g_replayActors[i], n ? n : "(no RTTI)"); }
    for (int i = 0; i < g_replayActorCount; i++) { int w = 0; if (ActorRefersTo(g_replayActors[i], so, &w)) { Log("[gimmick] actor %p refers to the new scene object %p at actor+0x%x / +0x%x / +0x%x", (void*)g_replayActors[i], (void*)so, w & 0xFFF, (w >> 12) & 0xFF, (w >> 20) & 0xFF); return g_replayActors[i]; } }
    Log("[gimmick] none of %d actor candidates refers to the new scene object %p by pointer", g_replayActorCount, (void*)so);
    for (int i = 0; i < g_replayActorCount; i++) if (g_replayCtorActor == g_replayActors[i]) { Log("[gimmick] taking the actor constructed during the replay: %p", (void*)g_replayActors[i]); return g_replayActors[i]; }
    return 0;
}
static std::vector<SpawnedGimmick> g_spawned; static std::mutex g_spawnedMutex;   // the server scene objects our replays created (for the removal research)
static void NoteSpawned(uintptr_t so, uintptr_t actor, const char* prefab, const float* pos) { if (!so) return; std::lock_guard<std::mutex> l(g_spawnedMutex); SpawnedGimmick g{}; g.so = so; g.actor = actor; strncpy_s(g.prefab, prefab ? prefab : "", _TRUNCATE); memcpy(g.pos, pos, 12); g.when = GetTickCount(); g_spawned.push_back(g); Log("[gimmick] spawned object %p actor %p (%s) noted, %zu in the list", (void*)so, (void*)actor, g.prefab, g_spawned.size()); }
static DWORD g_spawnWindowTick, g_spawnWindowThread;
uintptr_t kRva_GimmickSpawn_ = 0;
static bool g_inGimmickReplay = false;   // set while the replay runs (the core hook marks its lines)
typedef uint32_t (__fastcall* GameHashFn)(const void* key, size_t len);   // the game's lookup3 string hash (spawn reasons are hashed names)
static GameHashFn g_gameHash = nullptr;
using GimmickSpawnFn = void* (__fastcall*)(void* param, void* out, void* mgr, void* owner, void* s5, void* s6, void* s7, void* s8, void* s9, void* s10, void* s11, void* s12);
static GimmickSpawnFn g_origGimmickSpawn = nullptr;
// Replay experiment: every spawn the game makes is captured (the parameter block, the descriptor and save data it points to,
// the extra stack blocks). "Arm" then re-issues the last capture right after the next game spawn, on the game's own server
// thread with all its context still valid, with the transform moved to a spot the editor chose. Pointers inside the blocks
// that referred to the captured blocks themselves are redirected to the copies; everything else (managers, statics) is
// left as it was. Mode 1 additionally takes the identity words of the capture before the last one (swap test).
// The "param" is not a struct of its own: it is a region of the caller's stack frame (descriptor 0xC8 bytes before it, the
// transform block, the context blocks and the result int all around it, and locals it points to up to 0x800 beyond). So the
// capture takes one window of the frame, [param-0x400, param+0xA00), plus the heap save data, and the replay remaps every
// pointer that referred into that window to the copy. Pointers to managers, statics and live actors are left as they were.
// The housing placement turned out to be the wrong template (its gimmick is bound to the placed inventory item: a replay is
// refused as eErrNoDuplicateUniqueGimmick), so every spawn the game makes is kept in a ring, named by the scene object the
// game creates for it right afterwards, and the editor picks which one to repeat: level gimmicks (torches, doors ...) stream
// in through the same function while walking and depend on nothing but their GimmickInfoKey and transform.
static const size_t kFrameBefore = 0x400, kFrameSize = 0x2000, kSaveSize = 0x240;   // the window is sized per capture (c.size), kFrameSize is the buffer
struct GimmickCapture { bool valid = false; int id = 0; DWORD when = 0; uintptr_t caller = 0; uintptr_t base = 0, param = 0, save = 0; size_t size = 0; char ownerPath[200] = {}; void* mgr = nullptr; void* owner = nullptr; void* out = nullptr; void* s5 = nullptr; void* s6 = nullptr; void* s7 = nullptr; void* s8 = nullptr; void* s9 = nullptr; void* s10 = nullptr; void* s11 = nullptr; void* s12 = nullptr;
    uint32_t k1 = 0, k2 = 0; float pos[3] = { 0, 0, 0 }; char name[96] = { 0 };
    uint8_t frame[0x2000], bSave[0x240]; };
static const int kGimmickRing = 32;
static GimmickCapture g_gring[kGimmickRing]; static int g_gringNext = 0, g_gringIds = 0; static std::mutex g_gringMutex;
static volatile LONG g_gimmickReplayArmed = 0; static Vec3 g_gimmickReplayAt{}; static int g_gimmickReplayId = 0;
void ArmGimmickReplay(Vec3 at, int id) { g_gimmickReplayAt = at; g_gimmickReplayId = id; InterlockedExchange(&g_gimmickReplayArmed, 1); Log("[gimmick] replay of capture %d armed at (%.2f %.2f %.2f): the next spawn the game makes triggers it (walk a bit, or drop an item)", id, at.x, at.y, at.z); }
bool GimmickReplayArmed() { return g_gimmickReplayArmed != 0; }
int GimmickCaptureList(GimmickCapInfo* out, int max) {
    std::lock_guard<std::mutex> l(g_gringMutex); int n = 0;
    for (int k = 0; k < kGimmickRing && n < max; k++) {   // newest first
        const GimmickCapture& c = g_gring[(g_gringNext - 1 - k + 2 * kGimmickRing) % kGimmickRing]; if (!c.valid) continue;
        GimmickCapInfo& o = out[n++]; o.id = c.id; strncpy_s(o.path, c.ownerPath, _TRUNCATE); o.caller = c.caller; o.k1 = c.k1; o.k2 = c.k2; o.pos = { c.pos[0], c.pos[1], c.pos[2] }; o.ageMs = GetTickCount() - c.when; strncpy_s(o.name, c.name, _TRUNCATE);
    }
    return n;
}
// the scene object the game creates right after a gimmick spawn carries the prefab name; matched by position and time
static void NameGimmickCapture(const std::string& prefab, float x, float y, float z) {
    if (prefab.find("gimmick") == std::string::npos) return;
    std::lock_guard<std::mutex> l(g_gringMutex); const DWORD now = GetTickCount();
    for (int k = 0; k < kGimmickRing; k++) {
        GimmickCapture& c = g_gring[k]; if (!c.valid || c.name[0] || now - c.when > 8000) continue;
        if (fabsf(c.pos[0] - x) < 0.75f && fabsf(c.pos[1] - y) < 2.0f && fabsf(c.pos[2] - z) < 0.75f) { std::string n = prefab.substr(prefab.find_last_of('/') + 1); strncpy_s(c.name, n.c_str(), _TRUNCATE); return; }
    }
}
static void WatchObject(uintptr_t o);
static bool InWindow(uintptr_t v, uintptr_t base, size_t n) { return v >= base && v < base + n; }
static __forceinline void CaptureGimmick(void* param, void* out, void* mgr, void* owner, void* s5, void* s6, void* s7, void* s8, void* s9, void* s10, void* s11, void* s12) {
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    std::lock_guard<std::mutex> l(g_gringMutex);
    GimmickCapture& c = g_gring[g_gringNext % kGimmickRing]; c.valid = false;
    c.param = (uintptr_t)param; c.mgr = mgr; c.owner = owner; c.out = out;
    c.ownerPath[0] = 0;
    if (owner) { char buf[200] = {}; size_t n = 0; while (n < sizeof buf - 1 && ReadBytes((uintptr_t)owner + n, buf + n, 1) && buf[n] >= 0x20 && buf[n] < 0x7F) n++; if (n >= 8 && n < sizeof buf - 1 && buf[n] == 0 && buf[0] == '/') memcpy(c.ownerPath, buf, n + 1); } c.s5 = s5; c.s6 = s6; c.s7 = s7; c.s8 = s8; c.s9 = s9; c.s10 = s10; c.s11 = s11; c.s12 = s12; c.name[0] = 0;
    // the window: from 0x400 below the lowest of the blocks to 0xA00 above the highest (callers differ in their frame layout)
    uintptr_t lo = c.param, hi = c.param;
    for (void* q : { out, s5, s6, s8 }) { const uintptr_t v = (uintptr_t)q; if (v > 0x10000 && !(v >> 47) && v > c.param - 0x4000 && v < c.param + 0x4000) { if (v < lo) lo = v; if (v > hi) hi = v; } }
    c.base = (lo - kFrameBefore) & ~(uintptr_t)15; c.size = (hi + 0xA00) - c.base; if (c.size > kFrameSize) c.size = kFrameSize;
    // the frame sits near the top of the thread's stack for some callers (item drop: s8 = param+0x768): the window must not run
    // past the end of the mapped stack, so it is clamped to the region the base lies in
    { MEMORY_BASIC_INFORMATION mbi; if (VirtualQuery((void*)c.base, &mbi, sizeof(mbi)) == sizeof(mbi)) { const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize; if (c.base + c.size > end) c.size = end - c.base; } }
    if (!ReadBytes(c.base, c.frame, c.size)) { Log("[gimmick] capture skipped: frame window %p+0x%zx of caller rva 0x%llx not readable", (void*)c.base, c.size, InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull); return; }
    memcpy(&c.save, c.frame + (c.param - c.base) + 0x18, 8);
    if (!c.save || !ReadBytes(c.save, c.bSave, kSaveSize)) { Log("[gimmick] capture skipped: save data %p of caller rva 0x%llx not readable", (void*)c.save, InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull); return; }
    for (void* q : { out, s5, s6, s8 }) if (q && !InWindow((uintptr_t)q, c.base, c.size)) { uintptr_t ret = (uintptr_t)_ReturnAddress(); Log("[gimmick] capture skipped: a block of caller rva 0x%llx lies far from its frame (%p vs param %p)", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, q, param); return; }
    c.caller = InImage(ret) ? ret - g_base : 0; c.when = GetTickCount(); c.id = ++g_gringIds; c.valid = true; g_gringNext++;
    const uint8_t* b8 = c.frame + ((uintptr_t)s8 - c.base); memcpy(&c.k1, b8 + 0x30, 4); memcpy(&c.k2, b8 + 0x38, 4); memcpy(c.pos, b8 + 0x1C, 12);
    if (c.ownerPath[0]) { const char* fn = strrchr(c.ownerPath, '/'); strncpy_s(c.name, fn ? fn + 1 : c.ownerPath, _TRUNCATE); Log("[gimmick] capture %d: prefab path (r9) %s", c.id, c.ownerPath); }
    Log("[gimmick] captured spawn %d (caller rva 0x%llx): words s8+30 = %u, s8+38 = %u, pos (%.2f %.2f %.2f)", c.id, (unsigned long long)c.caller, c.k1, c.k2, c.pos[0], c.pos[1], c.pos[2]);
    if (g_trace) for (size_t o = 0; o + 8 <= c.size; o += 8) {   // where does the frame refer to server scene objects?
        uintptr_t v; memcpy(&v, c.frame + o, 8); if (v < 0x10000 || (v >> 47)) continue;
        const char* n = RttiName(v); if (!n || !strstr(n, "SceneObjectServer")) continue;
        uint32_t uuid[4] = { 0, 0, 0, 0 }; ReadBytes(v + 0x1D8, uuid, 16); WatchObject(v);
        Log("[gimmick]   frame+0x%zx (param%+ld) -> %p (%s) uuid %08x %08x %08x %08x", o, (long)o - (long)(c.param - c.base), (void*)v, n, uuid[0], uuid[1], uuid[2], uuid[3]);
    }
}
static void FindStringsForHash(uint32_t code);
// Error codes of the server spawn path are hashed names (the same lookup3 the spawn reasons use). The eErr* name list is
// linked into the ASI as RCDATA so decoding never depends on a sidecar file.
static std::string DecodeErr(uint32_t code) {
    if (!code || !g_gameHash) return code ? "?" : "ok";
    static std::vector<std::string> names; static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const uint8_t* data = nullptr; size_t size = 0;
        if (EmbeddedResource(kResourceErrNames, &data, &size)) {
            std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
            std::string n;
            while (std::getline(in, n)) {
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
                if (!n.empty()) names.push_back(std::move(n));
            }
        } else {
            Log("[gimmick] embedded error-name table resource is missing");
        }
    }
    for (const auto& n : names) {
        if (g_gameHash(n.c_str(), n.size()) == code) return n;
        if (n.size() > 6 && g_gameHash(n.c_str() + 6, n.size() - 6) == code) return n;   // without the eErrNo prefix
        if (n.size() > 4 && g_gameHash(n.c_str() + 4, n.size() - 4) == code) return n;   // without eErr
    }
    char b[32]; snprintf(b, sizeof b, "unknown 0x%08x", code); return b;
}
// One prepared copy of a capture: frame + save data with pointers remapped, transform moved to 'at'
struct GimmickCopy { uint8_t* frame = nullptr; uint8_t* save = nullptr; uint8_t* param = nullptr; uint8_t* desc = nullptr; uint8_t* out = nullptr; uint8_t* s5 = nullptr; uint8_t* s6 = nullptr; uint8_t* s8 = nullptr; };
static void LogSpawnTransforms(const char* what, uintptr_t save, uintptr_t s8, uintptr_t s5) {
    float t[10] = {}, o[10] = {}, a[10] = {}, b[10] = {};
    if (save) { ReadBytes(save + 0x1CC, t, 40); ReadBytes(save + 0x1F4, o, 40); }
    if (s8) ReadBytes(s8, a, 40);
    if (s5) ReadBytes(s5 + 0x28, b, 40);
    Log("[gimmick] %s transforms: save+1CC pos (%.2f %.2f %.2f) scale %.2f | save+1F4 pos (%.2f %.2f %.2f) | s8 pos (%.2f %.2f %.2f) quat (%.2f %.2f %.2f %.2f) | s5+28 pos (%.2f %.2f %.2f)",
        what, t[7], t[8], t[9], t[0], o[7], o[8], o[9], a[7], a[8], a[9], a[3], a[4], a[5], a[6], b[7], b[8], b[9]);
}
// ---- interactive objects: the editor's gimmick prefabs spawned through the game's own spawn path -----------------------------
// SpawnAt queues them here; the ServerField tick (slot 9, server thread) spawns one per tick from the newest usable capture
// (a level streaming spawn, else a housing placement) with the prefab, position, rotation and scale swapped in. A refused replay
// keeps a plain client stand-in visible while newer/different captures are retried. Moves remove + respawn (final only), deletes remove the actor.
struct GimmickReq { int uid; uint64_t gen; std::string prefab; Vec3 pos; Rot rot; float scale; std::vector<int> triedTemplates; bool noDirect = false; MoveCompletion done; };
static const size_t kGimmickMaxTries = 3;   // refusals with different templates before an interactive prefab becomes a plain object
static std::deque<GimmickReq> g_gimmickQueue; static std::mutex g_gimmickQueueMutex;
static std::deque<std::function<void()>> g_serverJobs; static std::mutex g_serverJobsMutex;
static void RunOnServerTick(std::function<void()> f) { NoteWorkQueued(); std::lock_guard<std::mutex> l(g_serverJobsMutex); g_serverJobs.push_back(std::move(f)); }
static void EnqueueGimmick(int uid, uint64_t gen, const std::string& prefab, Vec3 pos, Rot rot, float scale, MoveCompletion done) { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.push_back({ uid, gen, prefab, pos, rot, scale, {}, false, std::move(done) }); }
int GimmickPending() { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); return (int)g_gimmickQueue.size(); }
// game thread: the interactive lane of an admitted move. The job carries (uid, generation); the pose was stored by
// the admission. Only the lane transitions (server object -> client stand-in on the first live move, stand-in ->
// server object on the final move) replace the physical object, and the admission renewed the generation for those.
static void MoveGimmickJob(int uid, uint64_t gen, bool final, MoveCompletion done, const MoveReq* target) {
    // While an interactive object is dragged it cannot follow the mouse (a server object only moves by remove + respawn), so
    // the first live move takes it away and puts a plain client object of the same prefab in its place; that one follows the
    // drag like any other object, and the release removes it and spawns the interactive object at the final transform.
    uintptr_t actor = 0, obj = 0; std::string prefab; bool standin = false; Vec3 pos; Rot rot; float scale = 1;
    { REG_LOCK; const int i = IndexOfUidLocked(uid);
      if (i < 0 || g_reg[(size_t)i].gen != gen || g_reg[(size_t)i].hidden || !g_reg[(size_t)i].gimmick) return;
      SpawnedObj& e = g_reg[(size_t)i];
      actor = e.actor; obj = e.obj; prefab = e.prefab; standin = e.standin;
      pos = e.pos; rot = e.rot; scale = e.scale;   // the admitted pose, stored by AdmitMoveLocked
      if (!final) { if (!standin) { e.standin = true; e.actor = 0; e.obj = 0; } }
      else { e.standin = false; e.actor = 0; e.obj = 0; } }
    if (target) { pos = target->pos; rot = target->rot; scale = target->scale; }
    if (!final) {
        if (!standin) {
            { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); for (auto it = g_gimmickQueue.begin(); it != g_gimmickQueue.end(); ) it = it->uid == uid ? g_gimmickQueue.erase(it) : it + 1; }   // not yet spawned: it must not appear under the stand-in
            if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
            if (GameThreadReady()) RunOnGameThread([uid, gen]() { DoSpawn(uid, gen, PlaceLaneStandin); });   // the client stand-in, same generation
        } else if (obj && GameThreadReady() && InterlockedCompareExchange(&g_queueCount, 0, 0) <= 2) RunOnGameThread([uid, gen]() { ApplyMove(uid, gen, false); });
        return;
    }
    if (done) {
        auto refused = [uid, gen, obj, actor, standin, done]() {
            { REG_LOCK; if (GenCurrentLocked(uid, gen)) { auto& e = g_reg[(size_t)IndexOfUidLocked(uid)]; e.obj = obj; e.actor = actor; e.standin = standin; } }
            done(false);
        };
        if (standin && obj && !DoRemove(obj)) { refused(); return; }
        if (actor) RunOnServerTick([uid, gen, prefab, pos, rot, scale, actor, done, refused]() {
            if (!RemoveSpawnedActor(actor)) { refused(); return; }
            EnqueueGimmick(uid, gen, prefab, pos, rot, scale, done);
        });
        else EnqueueGimmick(uid, gen, prefab, pos, rot, scale, done);
        return;
    }
    if (standin && obj && GameThreadReady()) RunOnGameThread([obj]() { DoRemove(obj); });
    if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); for (auto it = g_gimmickQueue.begin(); it != g_gimmickQueue.end(); ) it = it->uid == uid ? g_gimmickQueue.erase(it) : it + 1; }   // an older pending spawn of it is void
    EnqueueGimmick(uid, gen, prefab, pos, rot, scale, std::move(done));
    return;
}
static float g_replayQuat[4] = { 0, 0, 0, 1 }; static float g_replayScale = 1.0f; static bool g_replayUseRot = false;   // MakeCopy: also swap rotation and scale
static uintptr_t g_replayResultSo = 0, g_replayResultActor = 0;   // what the last replay created
extern uintptr_t g_replayInnerSo_;
#define g_replayInnerSo g_replayInnerSo_
static GimmickCopy MakeCopy(const GimmickCapture& c, uint8_t* mem, Vec3 at) {
    GimmickCopy g; g.frame = mem; g.save = mem + 0x2000;
    memset(mem, 0, 0x2400); memcpy(g.frame, c.frame, c.size); memcpy(g.save, c.bSave, kSaveSize);
    const uintptr_t NB = (uintptr_t)g.frame;
    auto remap = [&](uint8_t* block, size_t n) {
        for (size_t o = 0; o + 8 <= n; o += 8) {
            uintptr_t v; memcpy(&v, block + o, 8);
            if (InWindow(v, c.base, c.size)) { const uintptr_t nv = NB + (v - c.base); memcpy(block + o, &nv, 8); }
            else if (InWindow(v, c.save, kSaveSize)) { const uintptr_t nv = (uintptr_t)g.save + (v - c.save); memcpy(block + o, &nv, 8); }
        }
    };
    remap(g.frame, c.size); remap(g.save, kSaveSize);
    auto atp = [&](void* old) -> uint8_t* { return g.frame + ((uintptr_t)old - c.base); };
    g.param = g.frame + (c.param - c.base); g.desc = g.param - 0xC8; g.out = atp(c.out); g.s5 = atp(c.s5); g.s6 = atp(c.s6); g.s8 = atp(c.s8);
    // The target position: the transform block s8 carries it at +0x1C (scale3 quat4 pos3); other blocks may hold a copy (the
    // housing / drop stack arg 5 at +0x44). Callers lay their frames out differently (level: s5 is the desc minus 0x10, and
    // s5+0x44 is the desc's self pointer, which a blind write turned into floats and crashed the create), so every further
    // occurrence is found by its exact bytes and only those are replaced.
    const float pos[3] = { at.x, at.y, at.z }; uint8_t was[12]; memcpy(was, g.s8 + 0x1C, 12); memcpy(g.s8 + 0x1C, pos, 12);
    int n = 0;
    for (size_t o = 0; o + 12 <= c.size; o += 4) if (memcmp(g.frame + o, was, 12) == 0) { memcpy(g.frame + o, pos, 12); n++; }
    for (size_t o = 0; o + 12 <= kSaveSize; o += 4) if (memcmp(g.save + o, was, 12) == 0) { memcpy(g.save + o, pos, 12); n++; }
    Log("[gimmick] copy: target position written to s8+1C and %d further exact cop%s of the captured position", n, n == 1 ? "y" : "ies");
    if (g_replayUseRot) {   // rotation (quaternion at s8+0x0C, copies replaced like the position) and scale (s8+0 only: 1,1,1 is too common to hunt for)
        uint8_t wq[16]; memcpy(wq, g.s8 + 0x0C, 16); memcpy(g.s8 + 0x0C, g_replayQuat, 16); int nq = 0;
        for (size_t o = 0; o + 16 <= c.size; o += 4) if (memcmp(g.frame + o, wq, 16) == 0) { memcpy(g.frame + o, g_replayQuat, 16); nq++; }
        for (size_t o = 0; o + 16 <= kSaveSize; o += 4) if (memcmp(g.save + o, wq, 16) == 0) { memcpy(g.save + o, g_replayQuat, 16); nq++; }
        const float sc[3] = { g_replayScale, g_replayScale, g_replayScale }; memcpy(g.s8, sc, 12);
        Log("[gimmick] copy: rotation (%.2f %.2f %.2f %.2f) written, %d further cop%s, scale %.2f", g_replayQuat[0], g_replayQuat[1], g_replayQuat[2], g_replayQuat[3], nq, nq == 1 ? "y" : "ies", g_replayScale);
    }
    return g;
}
// FieldGimmickSaveData (reflection offsets from the property setters): +0x28 key, +0x30 fieldSaveDataReason, +0x4C levelOriginSceneObjectUuid (16),
// +0x1C0 gimmickInfoKey (u16), +0x1CC transform (scale3 quat4 pos3), +0x1F4 originSpawnTransform, +0x220 spawnReason hash
static const char* SpawnReasonOf(uintptr_t caller) {
    auto it = g_callerReason.find(caller); return it == g_callerReason.end() ? nullptr : it->second.c_str();
}
const char* GimmickCallerName(uintptr_t caller) {
    if (caller && caller == g_callerLevel) return "level";
    if (caller && caller == g_callerDrop) return "item drop";
    if (const char* r = SpawnReasonOf(caller)) return r;
    return nullptr;
}
// The prepare derives the scene object's sync key (u16 at param+8) from the spawn's identity, so a replay gets the key of the
// object it was copied from and later fails as eErrNoInvalidSceneObjectUUID. Experiment: hand out a fresh key instead.
static uint16_t g_freshKey = 60000;
static void FreshKey(uint8_t* param, const char* what) {
    uint16_t k = 0; memcpy(&k, param + 8, 2);
    if (k == 0xFFFF) { Log("[gimmick] %s: no sync key in param+8 after prepare", what); return; }
    const uint16_t nk = g_freshKey++; memcpy(param + 8, &nk, 2);
    Log("[gimmick] %s: sync key %u replaced by fresh %u", what, k, nk);
}
// The scene object UUID of the spawn (two words: an id and a session constant, e.g. 5039bd3f 0000134e) sits at the start of
// stack arg 5; the prepare copies it into the SceneObjectServer (+0x1D8) and the field create then refuses a UUID it already
// knows (eErrNoInvalidSceneObjectUUID, traced). The replay therefore gets a fresh id word.
static uint32_t g_freshUuid = 0x7E000000;
static void FreshUuid(uint8_t* s5, const char* what) {
    if (!s5) { Log("[gimmick] %s: no stack arg 5, uuid left alone", what); return; }
    uint32_t u[2]; memcpy(u, s5, 8);
    if (!u[0] && !u[1]) { Log("[gimmick] %s: uuid at s5 is zero, left alone", what); return; }
    const uint32_t nu = g_freshUuid++; memcpy(s5, &nu, 4);
    Log("[gimmick] %s: uuid %08x %08x replaced by fresh %08x %08x", what, u[0], u[1], nu, u[1]);
}
// The prepare's 4th argument (r9, "owner") is the prefab path the server scene object is created from (0x278f488 ->
// 0x129cb80: strlen + string + create). A replay may therefore ask for another prefab: the Log tab's override.
static char g_replayPrefab[256] = {};
void SetGimmickReplayPrefab(const char* path) { strncpy_s(g_replayPrefab, path ? path : "", _TRUNCATE); }
const char* GimmickReplayPrefab() { return g_replayPrefab; }
static void* ReplayOwner(const GimmickCapture& c) {
    if (g_replayPrefab[0] && c.ownerPath[0]) { Log("[gimmick] replay prefab override: %s (captured: %s)", g_replayPrefab, c.ownerPath); return g_replayPrefab; }
    return c.owner;
}
static int RunSpawnSteps(const GimmickCapture& c, GimmickCopy& g, const char* what) {
    const LONG before = g_soCreated_;
    FreshUuid(g.s5, what);
    void* r = g_origGimmickSpawn(g.param, g.out, c.mgr, ReplayOwner(c), g.s5, g.s6, c.s7, g.s8, c.s9, c.s10, c.s11, c.s12);
    int code0 = 0; memcpy(&code0, g.out, 4);
    uint16_t key = 0; memcpy(&key, g.save + 0x1C0, 2); int reasonEnum = 0; memcpy(&reasonEnum, g.save + 0x30, 4);
    Log("[gimmick] %s: prepare returned %p, out[0] = %d (%s); save: gimmickInfoKey %u, fieldSaveDataReason %d, uuid %08x..", what, r, code0, DecodeErr((uint32_t)code0).c_str(), key, reasonEnum, *(uint32_t*)(g.save + 0x4C));
    if (code0) return code0;
    FreshKey(g.param, what);
    if (false) {   // (build 2949, caller 0x2aae2cb) the summon of a spawner gimmick (flags): the reason hash sits in its summon data, [[param+0x5D8]+0x58]+0x50; not classified at runtime yet
        uintptr_t o = 0; memcpy(&o, g.param + 0x5D8, 8); uintptr_t o2 = 0; uint32_t h = 0;
        if (o && ReadPtr(o + 0x58, &o2) && o2 && ReadBytes(o2 + 0x50, &h, 4)) { memcpy(g.param + 0x3F8, &h, 4); memcpy(g.save + 0x220, &h, 4); Log("[gimmick] %s: summon reason hash 0x%08x from the summon data", what, h); }
    }
    if (const char* reason = SpawnReasonOf(c.caller)) { if (g_gameHash) { const uint32_t h = g_gameHash(reason, strlen(reason)); memcpy(g.param + 0x3F8, &h, 4); memcpy(g.save + 0x220, &h, 4); Log("[gimmick] %s: reason \"%s\" hash 0x%08x", what, reason, h); } }
    else Log("[gimmick] %s: caller rva 0x%llx has no known reason, hash left as captured", what, (unsigned long long)c.caller);
    int* result = (int*)(g.param + 0x540); *result = 0;
    uintptr_t descVt = 0; memcpy(&descVt, g.desc, 8);
    typedef void* (__fastcall* DescCommitFn)(void* desc, int* result);
    DescCommitFn commit = nullptr; if (!descVt || !ReadPtr(descVt + 0x20, (uintptr_t*)&commit) || !commit) { Log("[gimmick] %s: no commit slot", what); return -1; }
    commit(g.desc, result);
    Log("[gimmick] %s: commit result = %d (%s)", what, *result, DecodeErr((uint32_t)*result).c_str());
    if (*result) return *result;
    void* arr[1] = { g.desc }; struct { void* p; uint32_t n; uint32_t pad; } list = { arr, 1, 0 };
    typedef int* (__fastcall* FieldCreateFn)(void* field, int* result, void* list, void* zero);
    uintptr_t fieldVt = 0; FieldCreateFn create = nullptr;
    if (ReadPtr((uintptr_t)c.mgr, &fieldVt) && fieldVt) ReadPtr(fieldVt + 0x88, (uintptr_t*)&create);
    if (!create) { Log("[gimmick] %s: no create slot on the field", what); return -1; }
    int* res = create(c.mgr, result, &list, nullptr);
    int code = -1; if (res) ReadBytes((uintptr_t)res, &code, 4);
    Log("[gimmick] %s: field create -> code %d (%s)", what, code, DecodeErr((uint32_t)code).c_str());
    if (!code && (g_replayInnerSo || g_soCreated_ != before)) { float ps[3]; memcpy(ps, g.s8 + 0x1C, 12); const uintptr_t so = g_replayInnerSo ? g_replayInnerSo : g_lastSoCreated_; const uintptr_t act = PickReplayActor(so); g_replayResultSo = so; g_replayResultActor = act; NoteSpawned(so, act, g_replayPrefab[0] ? g_replayPrefab : c.ownerPath, ps); }
    return code;
}
static bool FindCapture(int id, GimmickCapture& out) { std::lock_guard<std::mutex> l(g_gringMutex); for (int k = 0; k < kGimmickRing; k++) if (g_gring[k].valid && g_gring[k].id == id) { out = g_gring[k]; return true; } return false; }
static bool FindLatestHousingCapture(GimmickCapture& out) { std::lock_guard<std::mutex> l(g_gringMutex); int best = -1; for (int k = 0; k < kGimmickRing; k++) if (g_gring[k].valid && g_gring[k].caller == g_callerHousing && g_gring[k].id > best) { best = g_gring[k].id; out = g_gring[k]; } return best >= 0; }
static void ReplayGimmickBody();
static void ReplayGimmick() { g_lastActorCreated_ = 0; g_replayActor = 0; g_replayActorCount = 0; g_replayCtorActor = 0; g_replayResultSo = 0; g_replayResultActor = 0; g_replayInnerSo = 0; g_inGimmickReplay = true; g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount(); const bool tr = g_trace; g_trace = tr || g_traceHooks; ReplayGimmickBody(); g_trace = tr; g_inGimmickReplay = false; }   // the replay traces itself
// The item-drop caller (rva 0x2b4d733 in this build) does, after the prepare: register(&param+0x60, &param-0x198) - a function in
// the protected code section that registers the server scene object the prepare created (SceneObjectServer slots 142..144 and
// the sync manager's slots 15/5/4, as traced) - then the reason hash from param+0x558 into param+0x3F8 and save+0x220, two
// mirrored words, the commit into an int at param+0x528, and the field create. Replaying it step by step, target taken from
// the caller's own call instruction (research only).
static void ReplayDrop(const GimmickCapture& c, uint8_t* mem) {
    GimmickCopy g = MakeCopy(c, mem, g_gimmickReplayAt);
    FreshUuid(g.s5, "drop replay");
    const LONG before = g_soCreated_;
    void* r = g_origGimmickSpawn(g.param, g.out, c.mgr, ReplayOwner(c), g.s5, g.s6, c.s7, g.s8, c.s9, c.s10, c.s11, c.s12);
    int code0 = 0; memcpy(&code0, g.out, 4);
    Log("[gimmick] drop replay: prepare returned %p, out[0] = %d (%s); scene objects created during it: %ld", r, code0, DecodeErr((uint32_t)code0).c_str(), g_soCreated_ - before);
    DumpDeep("replay param after prepare", (uintptr_t)g.param, 0x100, 8);
    LogSpawnTransforms("drop replay after prepare", (uintptr_t)g.save, (uintptr_t)g.s8, (uintptr_t)g.s5);
    { uint32_t u[4]; memcpy(u, g.desc + 0x1D8, 16); Log("[gimmick] drop replay: desc uuid@+1D8 after prepare: %08x %08x %08x %08x", u[0], u[1], u[2], u[3]); }
    { uintptr_t d20 = 0; memcpy(&d20, g.desc + 0x20, 8); if (d20) DumpDeep("replay desc+20 object", d20, 0x60, 4); }
    if (code0) return;
    uint8_t ins[5] = { 0 }; const uintptr_t callSite = g_base + c.caller + 0xB;
    if (!ReadBytes(callSite, ins, 5) || ins[0] != 0xE8) { Log("[gimmick] drop replay: no call at the expected spot (byte 0x%02x), stopping", ins[0]); return; }
    int32_t rel = 0; memcpy(&rel, ins + 1, 4); const uintptr_t reg = callSite + 5 + rel;
    FreshKey(g.param, "drop replay");
    typedef void* (__fastcall* RegFn)(void* a, void* b);
    Log("[gimmick] drop replay: register step at rva 0x%llx", (unsigned long long)(reg - g_base));
    ((RegFn)reg)(g.param + 0x60, g.param - 0x198);
    { uint32_t u[4]; memcpy(u, g.desc + 0x1D8, 16); Log("[gimmick] drop replay: desc uuid@+1D8 after register: %08x %08x %08x %08x", u[0], u[1], u[2], u[3]); }
    uint32_t reason = 0; memcpy(&reason, g.param + 0x558, 4); memcpy(g.param + 0x3F8, &reason, 4); memcpy(g.save + 0x220, &reason, 4);
    uintptr_t o1 = 0, o2 = 0; memcpy(&o1, g.param + 0x538, 8); memcpy(&o2, g.param - 0x1D8, 8);
    uint32_t w = 0; if (o1 && ReadBytes(o1 + 0x88, &w, 4)) memcpy(g.save + 0x21C, &w, 4);
    if (o2 && ReadBytes(o2 + 0x60, &w, 4)) memcpy(g.param + 0x2F8, &w, 4);
    Log("[gimmick] drop replay: reason hash 0x%08x, mirrored words done", reason);
    int* result = (int*)(g.param + 0x528); *result = 0;
    uintptr_t descVt = 0; memcpy(&descVt, g.desc, 8);
    typedef void* (__fastcall* DescCommitFn)(void* desc, int* result);
    DescCommitFn commit = nullptr; if (!descVt || !ReadPtr(descVt + 0x20, (uintptr_t*)&commit) || !commit) { Log("[gimmick] drop replay: no commit slot"); return; }
    commit(g.desc, result);
    Log("[gimmick] drop replay: commit result = %d (%s)", *result, DecodeErr((uint32_t)*result).c_str());
    { uint32_t u[4]; memcpy(u, g.desc + 0x1D8, 16); Log("[gimmick] drop replay: desc uuid@+1D8 after commit: %08x %08x %08x %08x", u[0], u[1], u[2], u[3]); }
    if (*result) return;
    void* arr[1] = { g.desc }; struct { void* p; uint32_t n; uint32_t pad; } list = { arr, 1, 0 };
    typedef int* (__fastcall* FieldCreateFn)(void* field, int* result, void* list, void* zero);
    uintptr_t fieldVt = 0; FieldCreateFn create = nullptr;
    if (ReadPtr((uintptr_t)c.mgr, &fieldVt) && fieldVt) ReadPtr(fieldVt + 0x88, (uintptr_t*)&create);
    if (!create) { Log("[gimmick] drop replay: no create slot on the field"); return; }
    int* res = create(c.mgr, result, &list, nullptr);
    int code = -1; if (res) ReadBytes((uintptr_t)res, &code, 4);
    Log("[gimmick] drop replay: field create -> code %d (%s)", code, DecodeErr((uint32_t)code).c_str());
    if (!code && (g_replayInnerSo || g_soCreated_ != before)) { float ps[3]; memcpy(ps, g.s8 + 0x1C, 12); const uintptr_t so = g_replayInnerSo ? g_replayInnerSo : g_lastSoCreated_; const uintptr_t act = PickReplayActor(so); g_replayResultSo = so; g_replayResultActor = act; NoteSpawned(so, act, g_replayPrefab[0] ? g_replayPrefab : c.ownerPath, ps); }
    LogSpawnTransforms("drop replay after create", (uintptr_t)g.save, (uintptr_t)g.s8, (uintptr_t)g.s5);
}
static void ReplayGimmickBody() {
    GimmickCapture c; if (!FindCapture(g_gimmickReplayId, c)) { Log("[gimmick] replay: capture %d is gone", g_gimmickReplayId); return; }
    static uint8_t* memA = nullptr; static uint8_t* memB = nullptr;
    if (!memA) memA = (uint8_t*)VirtualAlloc(nullptr, 0x2400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memB) memB = (uint8_t*)VirtualAlloc(nullptr, 0x2400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memA || !memB) return;
    Log("[gimmick] replay of capture %d (%s, caller rva 0x%llx) at (%.2f %.2f %.2f)", c.id, c.name[0] ? c.name : "unnamed", (unsigned long long)c.caller, g_gimmickReplayAt.x, g_gimmickReplayAt.y, g_gimmickReplayAt.z);
    if (c.caller == g_callerDrop) { ReplayDrop(c, memA); return; }   // an item drop: the full sequence of that caller
    if (c.caller != g_callerLevel) { GimmickCopy g = MakeCopy(c, memA, g_gimmickReplayAt); RunSpawnSteps(c, g, "direct replay"); return; }   // housing, buff / summon, inspect, ...: prepare, reason, commit, create
    // A level capture as the template: the level's origin scene object UUID (stack arg 6, copied to save+0x4C by the prepare) is
    // cleared so the copy is not bound to the level's own object, the spawn UUID gets a fresh id (RunSpawnSteps), and the
    // reason stays the level's own (a "housing" reason made the create look the housing item up and crash at 0x389570 with the
    // level frame's item pointer). With the prefab override this makes any
    // walk-by streaming spawn a usable template, no housing placement needed.
    GimmickCopy g = MakeCopy(c, memA, g_gimmickReplayAt);
    if (g.s6) { uint32_t u[4]; memcpy(u, g.s6, 16); Log("[gimmick] level replay: origin uuid at s6 %08x %08x %08x %08x cleared", u[0], u[1], u[2], u[3]); memset(g.s6, 0, 16); }
    RunSpawnSteps(c, g, "level replay");
}
// Which capture serves as the template. A user's log showed the game's server thread never returning from a replay whose
// template the game had captured 57 ms earlier (its own spawn was apparently still in progress), after seven good replays
// from an older capture. So: the capture whose last replay succeeded is used again as long as it is in the ring; otherwise a
// settled one (at least kTemplateSettleMs old: the newest of those, level streaming before housing before any other); a
// younger one only when there is nothing else (right after loading a save), and then the oldest of them.
static volatile int g_goodTemplateId = 0;
static volatile DWORD g_replayWatchTick = 0; static volatile bool g_replayWatchLogged = false;   // the replay in progress (0 = none)
static char g_replayWatchPrefab[256] = {}; static int g_replayWatchTemplate = 0; static DWORD g_replayWatchAge = 0; static Vec3 g_replayWatchPos{};
// Where a stuck server thread waits: suspend it briefly and unwind its stack through the PE unwind table (game frames as rvas).
static void LogStuckStack(DWORD tid) {
    if (!tid) return;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { Log("[gimmick] WATCHDOG: cannot open thread %lu", tid); return; }
    if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); return; }
    CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_FULL;
    char line[1024]; int len = 0; line[0] = 0;
    if (GetThreadContext(th, &ctx)) {
        for (int i = 0; i < 28 && ctx.Rip; i++) {
            const uintptr_t pc = ctx.Rip;
            len += snprintf(line + len, sizeof line - len, InImage(pc) ? " %llx" : " [%llx]", (unsigned long long)(InImage(pc) ? pc - g_base : pc));
            if (len >= (int)sizeof line - 24) break;
            DWORD64 imageBase = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fe) { uintptr_t ret = 0; if (!ReadBytes(ctx.Rsp, &ret, 8)) break; ctx.Rip = ret; ctx.Rsp += 8; continue; }   // leaf
            void* handlerData = nullptr; DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
        }
    }
    ResumeThread(th); CloseHandle(th);
    Log("[gimmick] WATCHDOG: stuck thread %lu stack (rva, [outside the game]):%s", tid, line);
}
static void CheckReplayWatchdog() {
    const DWORD t0 = g_replayWatchTick;
    if (!t0 || g_replayWatchLogged || GetTickCount() - t0 < 5000) return;
    g_replayWatchLogged = true;
    Log("[gimmick] WATCHDOG: the replay of %s at (%.1f %.1f %.1f) with template %d (captured %lu ms before) has not returned for 5 s: "
        "the game's server thread is stuck inside it. Interactive objects, NPC spawns and the game's own world spawns stop until the "
        "game is restarted. Please report this log.", g_replayWatchPrefab, g_replayWatchPos.x, g_replayWatchPos.y, g_replayWatchPos.z,
        g_replayWatchTemplate, (unsigned long)g_replayWatchAge);
    LogStuckStack(g_spawnWindowThread);
}
static const DWORD kTemplateSettleMs = 2000;
#ifndef WB_UNIFIED_HOST_TEST
static bool FindTemplateCapture(GimmickCapture& out, const std::vector<int>* avoid = nullptr) {
    std::lock_guard<std::mutex> l(g_gringMutex);
    auto usable = [&](const GimmickCapture& c) {
        return c.valid && c.ownerPath[0] && c.caller != g_callerDrop && !(avoid && std::find(avoid->begin(), avoid->end(), c.id) != avoid->end());
    };
    if (const int good = g_goodTemplateId)
        for (int k = 0; k < kGimmickRing; k++) if (g_gring[k].id == good && usable(g_gring[k])) { out = g_gring[k]; return true; }
    const DWORD now = GetTickCount();
    for (int pass = 0; pass < 2; pass++) {   // 0: settled only, newest first; 1: young ones, oldest first
        for (uintptr_t want : { g_callerLevel, g_callerHousing, (uintptr_t)0 }) {
            int best = -1; DWORD bestWhen = 0;
            for (int k = 0; k < kGimmickRing; k++) {
                const GimmickCapture& c = g_gring[k]; if (!usable(c) || (want && c.caller != want)) continue;
                const bool settled = now - c.when >= kTemplateSettleMs;
                if (settled != (pass == 0)) continue;
                const bool newer = (DWORD)(c.when - bestWhen) < 0x80000000u;
                if (best < 0 || (pass == 0 ? newer : !newer)) { best = k; bestWhen = c.when; }
            }
            if (best >= 0) { out = g_gring[best]; return true; }
        }
    }
    return false;
}
#else
// Host: a template capture is the seam's business; the retry/stand-in/attachment bookkeeping below stays production.
static bool FindTemplateCapture(GimmickCapture& out, const std::vector<int>* avoid = nullptr) {
    (void)avoid;
    if (!host::Seam().templateReady || !host::Seam().templateReady()) return false;
    out = GimmickCapture{};
    out.id = host::Seam().nextTemplateId++; out.valid = true; out.when = GetTickCount();
    return true;
}
#endif
bool GimmickTemplateReady() { GimmickCapture c; return FindTemplateCapture(c); }
static void ProcessServerJobs() {   // a bounded number per tick: a batch of hundreds of NPC spawns must not stall the server thread in one tick
    for (int n = 0; n < 8; n++) { std::function<void()> job; { std::lock_guard<std::mutex> l(g_serverJobsMutex); if (g_serverJobs.empty()) return; job = std::move(g_serverJobs.front()); g_serverJobs.pop_front(); } job(); }
}
static uintptr_t kRva_GimmickFromSave = 0, g_scopeAttacherVt = 0;
static volatile DWORD g_gameSpawnTick = 0;   // last spawn the game made itself (HookGimmickSpawn)
static bool GimmickWorldQuiet(DWORD now, DWORD since, DWORD lastSpawn) {
    return since && now - since >= 10000 && now - lastSpawn >= 3000;
}
static volatile uintptr_t g_serverFieldObj = 0;   // the ServerField whose slot 9 tick runs our server jobs
static volatile DWORD g_serverFieldTick = 0;      // when that tick last ran (play mode: a game is loading or running)
bool ServerFieldTicking() { const DWORD t = g_serverFieldTick; return g_serverFieldObj && t && GetTickCount() - t < 3000; }
static bool DirectGimmickSpawn(uint32_t key, Vec3 pos, Rot rot, float scale, uintptr_t* soOut, uintptr_t* actOut);   // below
static void ProcessGimmickQueue() {   // server thread, one object per tick
    GimmickReq r; size_t pending = 0;
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); if (g_gimmickQueue.empty()) return; r = g_gimmickQueue.front(); pending = g_gimmickQueue.size(); }
    int proj = 0; bool current;
    { REG_LOCK; current = GenCurrentLocked(r.uid, r.gen); if (current) proj = g_reg[(size_t)IndexOfUidLocked(r.uid)].proj; }
    if (!current) {
        std::lock_guard<std::mutex> l(g_gimmickQueueMutex);
        if (!g_gimmickQueue.empty() && g_gimmickQueue.front().uid == r.uid && g_gimmickQueue.front().gen == r.gen) g_gimmickQueue.pop_front();
        return;
    }
    auto nativeWork = TrackProjectNativeWork(proj);
    // A refused replay queued a plain stand-in on the game thread. Until it exists, a retry would spawn a second stand-in whose
    // registration overwrites the first one (left in the world, no longer selectable or deletable): wait for it.
    bool standinPending = false;
    { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
      standinPending = i >= 0 && !g_reg[(size_t)i].hidden && g_reg[(size_t)i].gen == r.gen && g_reg[(size_t)i].standin && !g_reg[(size_t)i].obj && !r.triedTemplates.empty(); }
    if (standinPending) {
        std::lock_guard<std::mutex> l(g_gimmickQueueMutex);
        if (!g_gimmickQueue.empty() && g_gimmickQueue.front().uid == r.uid && g_gimmickQueue.front().gen == r.gen) { g_gimmickQueue.pop_front(); g_gimmickQueue.push_back(std::move(r)); }
        return;
    }
    // Template-free first: the game's own "gimmick from save data" builder with the prefab's gimmickinfo key and the level spawn
    // reason. It needs no captured spawn (works right after loading, no walking) and none of the replay's patching; the template
    // replay stays as the fallback when the key is unknown or the builder fails.
    // Not while the world is still streaming in: the create path takes an exclusive lock (0x2791cd0 -> 0x2a997e0 -> 0x1851010) that
    // the loading side holds while it waits for this server thread, so a spawn a few seconds after loading waited forever (user
    // logs: tower and airship parts, stuck stack ...278d6d3 -> 2af9d17 -> 27a4a7c -> 2792ae4 -> 27a208f -> 2a99a95 -> 1851084).
    // Wait until the player has been in the world for 10 s and the game spawned nothing of its own for 3 s.
    const DWORD nowQ = GetTickCount();
    const bool worldQuiet = GimmickWorldQuiet(nowQ, g_worldSince, g_gameSpawnTick);
    const bool directWanted = !r.noDirect && kRva_GimmickFromSave && g_serverFieldObj;
    if (directWanted && !worldQuiet) {
        static DWORD s_lastWaitLog = 0; if (nowQ - s_lastWaitLog > 10000) { s_lastWaitLog = nowQ; Log("[gimmick] %zu interactive object%s waiting until the world has finished loading", pending, pending == 1 ? "" : "s"); }
        return;
    }
    if (const uint32_t gkey = directWanted ? thumbgen::GimmickKey(r.prefab) : 0) {
        { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); if (g_gimmickQueue.empty() || g_gimmickQueue.front().uid != r.uid || g_gimmickQueue.front().gen != r.gen) return; g_gimmickQueue.pop_front(); }
        // B2: capture the owning row under the registry lock, then reserve one strong in-flight attempt
        // obligation under the request mutex (no engine or queue calls while holding it), then revalidate the
        // incarnation. The reservation is held across the native call so a cancel racing inside it can never
        // observe a transient settled=true.
        std::weak_ptr<PlaceRequest> flightReqw; int flightRow = -1;
        { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
          if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].gen != r.gen) return;   // hidden, forgotten or restored meanwhile
          flightReqw = g_reg[(size_t)i].placeReq; flightRow = g_reg[(size_t)i].placeRow; }
        const std::shared_ptr<PlaceRequest> flightReq = flightReqw.lock();
        C5AddObligation(flightReq, flightRow);
        uintptr_t standin = 0; bool staleDirect = false;
        { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
          if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].gen != r.gen) staleDirect = true;
          else if (g_reg[(size_t)i].standin) { standin = g_reg[(size_t)i].obj; g_reg[(size_t)i].obj = 0; g_reg[(size_t)i].standin = false; } }
        if (staleDirect) { C5ReleaseObligation(flightReq, flightRow); return; }   // outside REG_LOCK: no registry -> request nesting
        if (standin) {   // the retired placeholder's removal is tracked like any other outstanding cleanup
            if (flightReq) C5QueueChargedGameRemove(flightReq, flightRow, standin);
            else if (GameThreadReady()) RunOnGameThread([standin]() { DoRemove(standin); });
        }
        strncpy_s(g_replayWatchPrefab, r.prefab.c_str(), _TRUNCATE);
        uintptr_t so = 0, actor = 0;
#ifdef WB_UNIFIED_HOST_TEST
        const bool directOk = host::Seam().directGimmick ? host::Seam().directGimmick(r.prefab, r.pos, r.rot, r.scale, &so, &actor) : false;
#else
        const bool directOk = DirectGimmickSpawn(gkey, r.pos, r.rot, r.scale, &so, &actor);
#endif
        if (directOk) {
            bool attached = false; std::weak_ptr<PlaceRequest> reqw; int rowId = -1;
            { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
              if (i >= 0 && !g_reg[(size_t)i].hidden && g_reg[(size_t)i].gen == r.gen && !g_reg[(size_t)i].standin) {
                  g_reg[(size_t)i].obj = so; g_reg[(size_t)i].actor = actor; g_reg[(size_t)i].colRot = r.rot; g_reg[(size_t)i].colScale = r.scale; attached = true;
                  reqw = g_reg[(size_t)i].placeReq; rowId = g_reg[(size_t)i].placeRow; } }
            if (!attached) {   // deleted, picked up, restored or superseded while spawning: dispose BOTH rejected handles, outside the lock
                // B2: one obligation per rejected handle BEFORE queueing its removal; each lane completion
                // releases its own after dispatch, and the in-flight reservation is released only after the
                // handoff, so the sum never touches zero while cleanup is still queued.
                if (flightReq) {
                    if (actor) C5AddObligation(flightReq, flightRow);
                    if (so && GameThreadReady()) C5AddObligation(flightReq, flightRow);
                }
                if (actor) RunOnServerTick([actor, flightReq, flightRow]() { RemoveSpawnedActor(actor); C5ReleaseObligation(flightReq, flightRow); });
                if (so && GameThreadReady()) RunOnGameThread([so, flightReq, flightRow]() { DoRemove(so); C5ReleaseObligation(flightReq, flightRow); });
                C5ReleaseObligation(flightReq, flightRow);
                return;
            }
            if (r.done) r.done(true);
            const bool notedDirect = rowId >= 0 ? NoteAttachOnce(reqw, rowId, PlaceLaneDirect, r.uid) : true;
            if (!notedDirect && C5RowDead(reqw, rowId)) {
                // B2-R1: the row died while the native call ran (e.g. canceled with row.uid == 0, so no hide
                // could invalidate the record) and the handles were just written into it. Take them back only
                // if this attempt still owns them, tombstone the record, and dispose both through the charged
                // rejected-handle path. An already Attached row (e.g. a drag-finalize re-attachment) keeps them.
                bool rolledBack = false;
                { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
                  if (i >= 0 && g_reg[(size_t)i].gen == r.gen && g_reg[(size_t)i].obj == so && g_reg[(size_t)i].actor == actor) {
                      g_reg[(size_t)i].obj = 0; g_reg[(size_t)i].actor = 0; g_reg[(size_t)i].hidden = true; g_reg[(size_t)i].gen = NewGenLocked();
                      rolledBack = true; } }
                if (rolledBack) {
                    if (flightReq) {
                        if (actor) C5AddObligation(flightReq, flightRow);
                        if (so && GameThreadReady()) C5AddObligation(flightReq, flightRow);
                    }
                    if (actor) RunOnServerTick([actor, flightReq, flightRow]() { RemoveSpawnedActor(actor); C5ReleaseObligation(flightReq, flightRow); });
                    if (so && GameThreadReady()) RunOnGameThread([so, flightReq, flightRow]() { DoRemove(so); C5ReleaseObligation(flightReq, flightRow); });
                }
                // else: a racing hide already took the handles and owns their cleanup; only the attempt ends.
                C5ReleaseObligation(flightReq, flightRow);
                return;
            }
            C5ReleaseObligation(flightReq, flightRow);
            Log("[gimmick] object %d spawned directly (no template): %s key %u (scene object %p, actor %p)", r.uid, r.prefab.c_str(), gkey, (void*)so, (void*)actor);
            return;
        }
        // B2-R1: a row that died while the native call ran must not fall through to the template replay;
        // tombstone the record instead.
        const bool deadDirect = C5RowDead(flightReqw, flightRow);
        C5ReleaseObligation(flightReq, flightRow);   // the builder refused: no handles, the template replay takes over
        if (deadDirect) { HideUid(r.uid); return; }
        Log("[gimmick] object %d: direct spawn of %s failed, the template replay takes over", r.uid, r.prefab.c_str());
        r.noDirect = true; std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.push_back(std::move(r)); return;
    }
    GimmickCapture t;
    if (!FindTemplateCapture(t, r.triedTemplates.empty() ? nullptr : &r.triedTemplates)) {
        static DWORD lastLog = 0; if (GetTickCount() - lastLog > 15000) { lastLog = GetTickCount();
            Log(r.triedTemplates.empty() ? "[gimmick] %zu interactive object%s waiting for a spawn template (the game spawns one when you walk)" : "[gimmick] %zu interactive object%s waiting for a fresh spawn template after a replay was refused", pending, pending == 1 ? "" : "s"); }
        if (!r.triedTemplates.empty()) { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); if (!g_gimmickQueue.empty() && g_gimmickQueue.front().uid == r.uid && g_gimmickQueue.front().gen == r.gen) { g_gimmickQueue.pop_front(); g_gimmickQueue.push_back(std::move(r)); } }
        return;
    }
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.pop_front(); }
    // B2: the same in-flight attempt reservation as the direct path above.
    std::weak_ptr<PlaceRequest> flightReqw; int flightRow = -1;
    { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
      if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].gen != r.gen) return;
      flightReqw = g_reg[(size_t)i].placeReq; flightRow = g_reg[(size_t)i].placeRow; }
    const std::shared_ptr<PlaceRequest> flightReq = flightReqw.lock();
    C5AddObligation(flightReq, flightRow);
#ifdef WB_UNIFIED_HOST_TEST
    if (g_replayRevalidateProbe) g_replayRevalidateProbe();   // D1 seam (null unless a host test arms it): runs with no locks held, between flight capture and replay revalidation
#endif
    uintptr_t waitingStandin = 0; bool staleReplay = false;
    { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
      if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].gen != r.gen) staleReplay = true;
      // A stand-in plus a queue entry means a previous server replay failed and left a visible client placeholder. Active dragging
      // removes the queue entry, so it never reaches this path. Retire the placeholder before retrying the interactive spawn.
      // D1: guarded — a stale record must never retire a current-generation stand-in.
      else if (g_reg[(size_t)i].standin) { waitingStandin = g_reg[(size_t)i].obj; g_reg[(size_t)i].obj = 0; g_reg[(size_t)i].standin = false; } }
    if (staleReplay) { C5ReleaseObligation(flightReq, flightRow); return; }   // outside REG_LOCK: no registry -> request nesting
    if (waitingStandin) {   // the retired placeholder's removal is tracked like any other outstanding cleanup
        if (flightReq) C5QueueChargedGameRemove(flightReq, flightRow, waitingStandin);
        else if (GameThreadReady()) RunOnGameThread([waitingStandin]() { DoRemove(waitingStandin); });
    }
    uintptr_t so = 0, actor = 0;
#ifdef WB_UNIFIED_HOST_TEST
    g_gimmickReplayId = t.id; g_gimmickReplayAt = r.pos;
    if (host::Seam().replay) host::Seam().replay(t.id, r.prefab, r.pos, r.rot, r.scale, &so, &actor);
#else
    char saved[256]; memcpy(saved, g_replayPrefab, sizeof saved); strncpy_s(g_replayPrefab, r.prefab.c_str(), _TRUNCATE);
    float xf[12]; MakeTransform(xf, r.pos, r.rot, r.scale, false); memcpy(g_replayQuat, xf + 3, 16); g_replayScale = r.scale; g_replayUseRot = true;
    g_gimmickReplayId = t.id; g_gimmickReplayAt = r.pos;
    strncpy_s(g_replayWatchPrefab, r.prefab.c_str(), _TRUNCATE); g_replayWatchTemplate = t.id; g_replayWatchAge = GetTickCount() - t.when; g_replayWatchPos = r.pos;
    g_replayWatchLogged = false; g_replayWatchTick = GetTickCount();
    ReplayGimmick();
    g_replayWatchTick = 0;
    g_replayUseRot = false; memcpy(g_replayPrefab, saved, sizeof saved);
    so = g_replayResultSo; actor = g_replayResultActor;
#endif
    if (so) {
        bool attached = false; std::weak_ptr<PlaceRequest> reqw; int rowId = -1;
        { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
          if (i >= 0 && !g_reg[(size_t)i].hidden && g_reg[(size_t)i].gen == r.gen && !g_reg[(size_t)i].standin) {
              g_reg[(size_t)i].obj = so; g_reg[(size_t)i].actor = actor; g_reg[(size_t)i].colRot = r.rot; g_reg[(size_t)i].colScale = r.scale; attached = true;
              reqw = g_reg[(size_t)i].placeReq; rowId = g_reg[(size_t)i].placeRow; } }
        if (!attached) {   // superseded while replaying: dispose BOTH rejected handles, outside the lock
            // B2: one obligation per rejected handle BEFORE queueing its removal; each lane completion
            // releases its own after dispatch, and the in-flight reservation is released only after the
            // handoff, so the sum never touches zero while cleanup is still queued.
            if (flightReq) {
                if (actor) C5AddObligation(flightReq, flightRow);
                if (so && GameThreadReady()) C5AddObligation(flightReq, flightRow);
            }
            if (actor) RunOnServerTick([actor, flightReq, flightRow]() { RemoveSpawnedActor(actor); C5ReleaseObligation(flightReq, flightRow); });
            if (so && GameThreadReady()) RunOnGameThread([so, flightReq, flightRow]() { DoRemove(so); C5ReleaseObligation(flightReq, flightRow); });
            C5ReleaseObligation(flightReq, flightRow);
            return;
        }
        if (r.done) r.done(true);
        const bool notedReplay = rowId >= 0 ? NoteAttachOnce(reqw, rowId, PlaceLaneReplay, r.uid) : true;
        if (!notedReplay && C5RowDead(reqw, rowId)) {
            // B2-R1: same late-rejection as the direct path above: the row died while the native call ran
            // and the handles were just written into it. An already Attached row keeps them.
            bool rolledBack = false;
            { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
              if (i >= 0 && g_reg[(size_t)i].gen == r.gen && g_reg[(size_t)i].obj == so && g_reg[(size_t)i].actor == actor) {
                  g_reg[(size_t)i].obj = 0; g_reg[(size_t)i].actor = 0; g_reg[(size_t)i].hidden = true; g_reg[(size_t)i].gen = NewGenLocked();
                  rolledBack = true; } }
            if (rolledBack) {
                if (flightReq) {
                    if (actor) C5AddObligation(flightReq, flightRow);
                    if (so && GameThreadReady()) C5AddObligation(flightReq, flightRow);
                }
                if (actor) RunOnServerTick([actor, flightReq, flightRow]() { RemoveSpawnedActor(actor); C5ReleaseObligation(flightReq, flightRow); });
                if (so && GameThreadReady()) RunOnGameThread([so, flightReq, flightRow]() { DoRemove(so); C5ReleaseObligation(flightReq, flightRow); });
            }
            C5ReleaseObligation(flightReq, flightRow);
            return;
        }
        C5ReleaseObligation(flightReq, flightRow);
        if (g_goodTemplateId != t.id) { g_goodTemplateId = t.id; Log("[gimmick] capture %d is the proven template now", t.id); }
        Log("[gimmick] object %d spawned through the game: %s (scene object %p, actor %p)", r.uid, r.prefab.c_str(), (void*)so, (void*)actor);
    } else {
        // B2-R1: a row that died while the native call ran must not be retried behind a fresh stand-in;
        // tombstone the record instead.
        const bool deadReplay = C5RowDead(flightReqw, flightRow);
        C5ReleaseObligation(flightReq, flightRow);   // refused: no handles, the stand-in retry takes over
        if (deadReplay) { HideUid(r.uid); return; }
        r.triedTemplates.push_back(t.id);
        if (g_goodTemplateId == t.id) g_goodTemplateId = 0;   // the proven template failed: the next object searches a new one
        // A refusal can depend on the template (captures change as the player moves), so a few different ones are tried; a
        // prefab the game keeps refusing becomes a plain object instead of being retried forever.
        const bool giveUp = r.triedTemplates.size() >= kGimmickMaxTries;
        if (giveUp) Log("[gimmick] object %d: %s refused with %zu different templates, placing it as a plain object", r.uid, r.prefab.c_str(), r.triedTemplates.size());
        else Log("[gimmick] object %d: replay with template %d was refused; a plain stand-in waits for another template (try %zu of %d)", r.uid, t.id, r.triedTemplates.size(), kGimmickMaxTries);
        { REG_LOCK; const int i = IndexOfUidLocked(r.uid);
          if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].gen != r.gen) return;
          SpawnedObj& e = g_reg[(size_t)i]; e.standin = !giveUp; e.obj = 0; e.actor = 0; if (giveUp) e.gimmick = false; }
        if (GameThreadReady()) RunOnGameThread([uid = r.uid, gen = r.gen, lane = giveUp ? PlaceLanePlain : PlaceLaneStandin, retry = !giveUp, done = r.done, target = MoveReq{ r.uid, r.pos, r.rot, r.scale }]() { DoSpawn(uid, gen, lane, done, done ? &target : nullptr, retry); });   // the plain stand-in (retry) or the accepted plain-object fallback (budget spent)
        if (!giveUp) { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.push_back(std::move(r)); }
    }
}
// ---- template-free gimmick spawn (research) ----
// The game's own "gimmick from save data" builder: bool fn(ServerField* field, FieldGimmickSaveData* save, u32 a, u32 b, u8 reason2,
// ScopeAttacher<CommonActor>* out). The callers pass reason2 6 or 8 (only read when b != 0) and a stack ScopeAttacher that receives
// the created actor (out+8 actor, +0x10 attached flag); its slot 0x10 is the reset / detach the builder itself calls first. It looks
// save+0x1C0 (gimmickinfo row key) up in the gimmick table, builds a CreateServerActorDesc_InstantGimmick (reason byte save+0x25C,
// flags save+0x28, uuid save+0x4C, reason hash save+0x220), runs the spawn prepare (0x278f490 in 2976, our HookGimmickSpawn) with a
// default transform block (scale 1, identity rotation, position 0), commits the desc and hands it to ServerField slot 17 (field
// create). Called with our own save record, the prepare hook swaps in position / rotation / scale: no captured template needed.
static uintptr_t FindVtableByName(const char* mangled);   // RTTI lookup (below)
static volatile bool g_directArmed = false; static DWORD g_directThread = 0; static float g_directXf[10] = {};   // scale3 quat4 pos3
static void ResolveGimmickFromSave() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 B0 FA FF FF 48 81 EC 50 06 00 00 41 8B F1 45 8B F0 4C 8B FA 4C 8B E1 44 8B 92 C0 01 00 00", &n);
    if (!f || n != 1) { Log("[gimmick] save-data builder: %d matches, template-free spawn off", n); return; }
    g_scopeAttacherVt = FindVtableByName(".?AV?$ScopeAttacher@VCommonActor@pa@@@pa@@");
    if (!g_scopeAttacherVt) { Log("[gimmick] ScopeAttacher<CommonActor> vtable not found, template-free spawn off"); return; }
    kRva_GimmickFromSave = f - g_base; Log("resolved %-20s rva 0x%llx (via signature), result holder vtable rva 0x%llx", "gimmick from save", (unsigned long long)kRva_GimmickFromSave, (unsigned long long)(g_scopeAttacherVt - g_base));
}
static bool CallGimmickFromSave(void* field, uint8_t* save, void* holder, bool* ok) {   // guarded: a wrong record must not take the game down
    typedef bool (__fastcall* Fn)(void*, void*, uint32_t, uint32_t, uint8_t, void*);
    CDK_GUARD_BEGIN *ok = ((Fn)(g_base + kRva_GimmickFromSave))(field, save, 0, 0, 8, holder); return true;
    CDK_GUARD_FAIL { EXCEPTION_POINTERS ep = cdk::GuardInfo(); LogFault(&ep); } return false;
    CDK_GUARD_END
}
// The spawn reason byte (desc+0xA) decides what the gimmick becomes: with 0 a campfire cannot be cooked on and a bed not slept
// in (boxes and torches work either way); the level spawn passes its own (0x1F in 2976). Read from the level caller's code:
// "mov r8b, imm8; lea rcx, [rbp-0x40]; call <desc ctor>" shortly before its call of the spawn prepare.
static int LevelSpawnReason() {
    static int s_reason = -1; if (s_reason >= 0) return s_reason;
    int found = -1;
    if (g_callerLevel) for (uintptr_t a = g_base + g_callerLevel - 0x80; a < g_base + g_callerLevel && found < 0; a++) {
        uint8_t b[8] = {}; if (!ReadBytes(a, b, 8)) break;
        if (b[0] == 0x41 && b[1] == 0xB0 && b[3] == 0x48 && b[4] == 0x8D && b[5] == 0x4D && b[7] == 0xE8) found = b[2];
    }
    s_reason = found >= 0 ? found : 0x1F;
    Log("[gimmick] level spawn reason %u (%s)", s_reason, found >= 0 ? "read from the level caller" : "NOT found in the level caller, using 0x1F of build 2976");
    return s_reason;
}
// Server thread. True with the new scene object and actor when the game built the interactive object.
static bool DirectGimmickSpawn(uint32_t key, Vec3 pos, Rot rot, float scale, uintptr_t* soOut, uintptr_t* actOut) {
    *soOut = *actOut = 0;
    void* field = (void*)g_serverFieldObj;
    if (!kRva_GimmickFromSave || !field) return false;
    alignas(16) static uint8_t save[0x400]; memset(save, 0, sizeof save);
    memcpy(save + 0x1C0, &key, 4); save[0x25C] = (uint8_t)LevelSpawnReason();
    float xf[12]; MakeTransform(xf, pos, rot, scale, false);
    memcpy(g_directXf, xf, 40); memcpy(save + 0x1CC, xf, 40);   // the record's own transform too (the builder does not read it)
    g_directThread = GetCurrentThreadId(); g_directArmed = true;
    g_lastActorCreated_ = 0; g_replayActor = 0; g_replayActorCount = 0; g_replayCtorActor = 0; g_replayResultSo = 0; g_replayResultActor = 0; g_replayInnerSo = 0;
    g_inGimmickReplay = true; g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount();
    const LONG before = g_soCreated_;
    g_replayWatchTemplate = 0; g_replayWatchAge = 0; g_replayWatchPos = pos; g_replayWatchLogged = false; g_replayWatchTick = GetTickCount();
    alignas(16) uintptr_t holder[8] = { g_scopeAttacherVt };   // ScopeAttacher<CommonActor>: vtable, actor, flags ...
    bool ok = false; const bool ran = CallGimmickFromSave(field, save, holder, &ok);
    g_replayWatchTick = 0; g_inGimmickReplay = false; const bool hookSaw = !g_directArmed; g_directArmed = false;
    const uintptr_t heldActor = holder[1]; const uint8_t attached = (uint8_t)(holder[2] & 0xFF);
    if (ran && attached) {   // let go of the attachment the builder handed us (its own detach); the actor stays in the field
        typedef void (__fastcall* ResetFn)(void*); ResetFn reset = nullptr;
        if (ReadPtr(g_scopeAttacherVt + 0x10, (uintptr_t*)&reset) && InImage((uintptr_t)reset)) { CDK_GUARD_BEGIN reset(holder); CDK_GUARD_FAIL Log("[gimmick] direct: detach faulted"); CDK_GUARD_END }
    }
    const uintptr_t so = g_replayInnerSo ? g_replayInnerSo : (g_soCreated_ != before ? g_lastSoCreated_ : 0);
    const uintptr_t act = heldActor ? heldActor : (so ? PickReplayActor(so) : 0);
    if (!ran || !ok || !hookSaw || !so) {
        Log("[gimmick] direct spawn of key %u at (%.2f %.2f %.2f) failed: %s, builder %s, prepare hook %s, scene object %p",
            key, pos.x, pos.y, pos.z, ran ? "returned" : "FAULTED", ok ? "true" : "false", hookSaw ? "reached" : "NOT reached", (void*)so);
        return false;
    }
    *soOut = so; *actOut = act; return true;
}
bool OurServerSpawnOnThisThread() { return g_inGimmickReplay && GetCurrentThreadId() == g_spawnWindowThread; }
uint32_t GameHashOf(const char* s) { return g_gameHash && s ? g_gameHash(s, strlen(s)) : 0; }
static void* __fastcall HookGimmickSpawn(void* param, void* out, void* mgr, void* owner, void* s5, void* s6, void* s7, void* s8, void* s9, void* s10, void* s11, void* s12) {
    g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount();
    if (!g_directArmed && !g_inGimmickReplay) g_gameSpawnTick = GetTickCount();   // the game's own spawns: the world is still loading
    if (g_directArmed && GetCurrentThreadId() == g_directThread && s8) {   // our template-free spawn: transform block scale3 quat4 pos3
        g_directArmed = false;
        float was[10] = {}; ReadBytes((uintptr_t)s8, was, 40);
        memcpy(s8, g_directXf, 40);
        Log("[gimmick] direct: prepare transform was scale %.2f quat (%.2f %.2f %.2f %.2f) pos (%.2f %.2f %.2f), now pos (%.2f %.2f %.2f)", was[0], was[3], was[4], was[5], was[6], was[7], was[8], was[9], g_directXf[7], g_directXf[8], g_directXf[9]);
        void* r = g_origGimmickSpawn(param, out, mgr, owner, s5, s6, s7, s8, s9, s10, s11, s12);
        return r;
    }
    CaptureGimmick(param, out, mgr, owner, s5, s6, s7, s8, s9, s10, s11, s12);
    if (!g_trace) {
        void* r0 = g_origGimmickSpawn(param, out, mgr, owner, s5, s6, s7, s8, s9, s10, s11, s12);
        if (InterlockedCompareExchange(&g_gimmickReplayArmed, 0, 1) == 1) ReplayGimmick();
        return r0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_traceMutex);
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    Log("[gimmick] ===== spawn from rva 0x%llx thread %lu: param=%p out=%p mgr=%p (%s) owner=%p (%s) stack: %p %p %p %p %p", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(),
        param, out, mgr, mgr && RttiName((uintptr_t)mgr) ? RttiName((uintptr_t)mgr) : "?", owner, owner && RttiName((uintptr_t)owner) ? RttiName((uintptr_t)owner) : "?", s5, s6, s7, s8, s9);
    DumpDeep("param", (uintptr_t)param, 0x580, 8);
    { uintptr_t desc = 0, save = 0;   // param+0 -> CreateServerActorDesc_InstantGimmick, param+0x18 -> FieldGimmickSaveData (reflected: GimmickInfoKey, Transform, ...)
      if (ReadBytes((uintptr_t)param, &desc, 8) && desc) DumpDeep("desc (param+0)", desc, 0x100, 8);
      if (ReadBytes((uintptr_t)param + 0x18, &save, 8) && save) DumpDeep("save data (param+18)", save, 0x240, 8); }
    DumpBlock("out before", (uintptr_t)out, 0x40);
    if (s5) DumpDeep("stack arg 5", (uintptr_t)s5, 0x80, 4);
    if (s6) DumpDeep("stack arg 6", (uintptr_t)s6, 0x80, 4);
    if (s8) DumpDeep("stack arg 8", (uintptr_t)s8, 0x80, 4);
    const LONG before0 = g_soCreated_;
    void* r = g_origGimmickSpawn(param, out, mgr, owner, s5, s6, s7, s8, s9, s10, s11, s12);
    Log("[gimmick] spawn returned %p (%s); scene objects created during it: %ld", r, r && RttiName((uintptr_t)r) ? RttiName((uintptr_t)r) : "?", g_soCreated_ - before0);
    DumpBlock("out after", (uintptr_t)out, 0x40);
    DumpBlock("param after (first 0x100)", (uintptr_t)param, 0x100);
    { uintptr_t sv = 0; ReadBytes((uintptr_t)param + 0x18, &sv, 8); LogSpawnTransforms("original after prepare", sv, (uintptr_t)s8, (uintptr_t)s5); }
    if (InterlockedCompareExchange(&g_gimmickReplayArmed, 0, 1) == 1) ReplayGimmick();
    return r;
}
// which string in the exe hashes to a given code? Scans every readable section of the image for identifier-like runs and
// hashes each run and each of its '_' / '.' / ':' separated tails with the game's function. Research aid for unknown codes.
static void FindStringsForHash(uint32_t code) {
    if (!g_gameHash) return;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    int found = 0; size_t runs = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && found < 10; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t secBase = g_base + sec[i].VirtualAddress; const size_t secSize = sec[i].Misc.VirtualSize;
        static std::vector<uint8_t> chunk; chunk.resize(1 << 20);
        for (size_t off = 0; off < secSize && found < 10; off += chunk.size()) {   // 1 MB at a time through the guarded read; runs crossing a chunk edge are simply split
        const size_t n = secSize - off < chunk.size() ? secSize - off : chunk.size(); if (!ReadBytes(secBase + off, chunk.data(), n)) continue;
        const uint8_t* p = chunk.data();
        size_t start = 0; bool in = false;
        for (size_t k = 0; k <= n; k++) {
            const uint8_t ch = k < n ? p[k] : 0;
            const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' || ch == ' ';
            if (ok && !in) { in = true; start = k; }
            else if (!ok && in) {
                in = false; const size_t len = k - start;
                if (len >= 4 && len <= 96) {
                    char buf[100]; memcpy(buf, p + start, len); buf[len] = 0; runs++;
                    for (size_t t = 0; t < len; t++) {   // the run and every tail after a separator
                        if (t == 0 || buf[t - 1] == '_' || buf[t - 1] == '.' || buf[t - 1] == ':' || buf[t - 1] == ' ') {
                            if (g_gameHash(buf + t, len - t) == code) { Log("[hash] 0x%08x = \"%s\" (in \"%s\")", code, buf + t, buf); found++; break; }
                        }
                    }
                }
            }
        }
        }
    }
    Log("[hash] scan for 0x%08x: %d match%s over %zu runs", code, found, found == 1 ? "" : "es", runs);
}
// ---- server actor creation core (research) ------------------------------------------------------------------------
// The field's create ends in one function that validates the "info object" of the spawn (it must carry a scene object UUID at
// +0x1D8, else eErrNoInvalidSceneObjectUUID) and creates the actor. Its wrapper takes that object as the first stack argument.
// Hooked for logging only: which object it is (RTTI), its UUID, and the flags, for the game's own spawns and for the replay.
static uintptr_t kRva_ActorCreateCore = 0; static int g_coreLogged = 0;
using ActorCoreFn = void* (__fastcall*)(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9, void* a10, void* a11, void* a12);
static ActorCoreFn g_origActorCore = nullptr;
static void* __fastcall HookActorCore(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9, void* a10, void* a11, void* a12) {
    const bool log = g_trace || g_inGimmickReplay || g_coreLogged < 3;
    if (log) {
        g_coreLogged++;
        uintptr_t payload = 0; if (a5) ReadBytes((uintptr_t)a5, &payload, 8);
        uint32_t uuid[4] = { 0, 0, 0, 0 }; if (payload) ReadBytes(payload + 0x1D8, uuid, 16);
        const char* n1 = payload ? RttiName(payload) : nullptr; const char* n2 = payload ? RttiName(payload - 0x28) : nullptr;
        Log("[core] actor create%s: a1=%p (%s) a2=%p a3=%p a4=%p | info holder a5=%p -> payload %p (%s / -0x28: %s) uuid %08x %08x %08x %08x | a6=%p flag a7=%d a8=%p",
            g_inGimmickReplay ? " (REPLAY)" : "", a1, a1 && RttiName((uintptr_t)a1) ? RttiName((uintptr_t)a1) : "?", a2, a3, a4, a5, (void*)payload, n1 ? n1 : "-", n2 ? n2 : "-", uuid[0], uuid[1], uuid[2], uuid[3], a6, (int)(intptr_t)a7 & 0xFF, a8);
        if (payload) DumpDeep("info payload", payload, 0x80, 6);
    }
    void* r = g_origActorCore(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    if (log) Log("[core] actor create returned %p", r);
    return r;
}
static void ResolveActorCore() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 8B C4 48 89 58 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 4D 8B E9 49 8B F0 4C 8B C2 8B 94 24 F8 00 00 00", &n);
    if (!f || n != 1) { Log("[core] actor create wrapper: %d matches, not hooked", n); return; }
    kRva_ActorCreateCore = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "actor create core", (unsigned long long)kRva_ActorCreateCore);
}

// ---- inner actor create (research) -------------------------------------------------------------------------------
// 0x2827ce0 in this build: called by the wrapper above (level streaming) and directly by the field create of the drop /
// housing path (0x2a999de). Its 6th argument points at an array entry whose pointee (a desc for the field create, a scene object
// for the level path) must carry a non-zero UUID at +0x1D8, else eErrNoInvalidSceneObjectUUID right at the start. Logged: the
// pointee, its UUID, and whether the check would pass, for the game's own spawns and for the replay.
static uintptr_t kRva_ActorCreateInner = 0;
using ActorInnerFn = void* (__fastcall*)(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9);
static ActorInnerFn g_origActorInner = nullptr;
uintptr_t g_replayInnerSo_ = 0;   // the first scene object the field create handed to the inner create during a replay
static void* __fastcall HookActorInner(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9) {
    const bool log = g_trace;
    if (g_inGimmickReplay && !g_replayInnerSo && a6 && GetCurrentThreadId() == g_spawnWindowThread) { uintptr_t so = 0; if (ReadBytes((uintptr_t)a6, &so, 8) && so && RttiName(so) && strstr(RttiName(so), "SceneObjectServer")) g_replayInnerSo = so; }
    if (log) {
        uintptr_t payload = 0; if (a6) ReadBytes((uintptr_t)a6, &payload, 8);
        uint32_t uuid[4] = { 0, 0, 0, 0 }; if (payload) ReadBytes(payload + 0x1D8, uuid, 16);
        const char* n1 = payload ? RttiName(payload) : nullptr;
        Log("[core] inner create%s: a1=%p (%s) a2=%p a3=%p a4=%p a5=%p | entry a6=%p -> %p (%s) uuid@+1D8 %08x %08x %08x %08x (%s) | a7=%p a8=%d a9=%d",
            g_inGimmickReplay ? " (REPLAY)" : "", a1, a1 && RttiName((uintptr_t)a1) ? RttiName((uintptr_t)a1) : "?", a2, a3, a4, a5, a6, (void*)payload, n1 ? n1 : "-",
            uuid[0], uuid[1], uuid[2], uuid[3], (uuid[0] | uuid[1] | uuid[2]) ? "ok" : "ZERO -> eErrNoInvalidSceneObjectUUID", a7, (int)(intptr_t)a8 & 0xFF, (int)(intptr_t)a9);
        if (payload) DumpBlock("inner create entry pointee", payload, 0x60);
    }
    void* r = g_origActorInner(a1, a2, a3, a4, a5, a6, a7, a8, a9);
    {   // the out-params (a4, a5 point at stack slots) receive the created actor: remembered for the removal research
        uintptr_t o4 = 0, o5 = 0; if (a4) ReadBytes((uintptr_t)a4, &o4, 8); if (a5) ReadBytes((uintptr_t)a5, &o5, 8);
        const char* n4 = o4 > 0x10000 ? RttiName(o4) : nullptr; const char* n5 = o5 > 0x10000 ? RttiName(o5) : nullptr;
        if (n4 && strstr(n4, "Actor")) g_lastActorCreated_ = o4; else if (n5 && strstr(n5, "Actor")) g_lastActorCreated_ = o5;
        if (log) Log("[core] inner create out-params: [a4] %p (%s) [a5] %p (%s)", (void*)o4, n4 ? n4 : "-", (void*)o5, n5 ? n5 : "-");
        if (log && o4 > 0x10000 && !(o4 >> 47)) DumpDeep("inner create [a4] pointee", o4, 0x60, 8);
    }
    if (log) { int code = 0; if (r) ReadBytes((uintptr_t)r, &code, 4); Log("[core] inner create returned %p -> code %d (%s)", r, code, DecodeErr((uint32_t)code).c_str()); }
    return r;
}
static void ResolveActorInner() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 F8 F4 FF FF 48 81 EC C8 0B 00 00", &n);
    if (!f || n != 1) { Log("[core] inner create: %d matches, not hooked", n); return; }
    kRva_ActorCreateInner = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "actor create inner", (unsigned long long)kRva_ActorCreateInner);
}

// ---- actor removal (research) -------------------------------------------------------------------------------------------
// An item pickup does not destroy the actor at once: the actor is queued and the server tick runs the loop below
// (0x27803e0 in this build: for each entry {word, actor} of the list at this+0x1B0: lock actor+0x18, check the state word at
// actor+0x5E, set actor+0x98 = the "removed" reason (global), actor+0x9C = entry byte, actor+0x5C = state, vtable[16](actor),
// unlock, vtable[34](actor, &result)), which ends in ServerSyncSceneObjectManager slot 2/3 (traced). Hooked to log the actors it
// removes; RemoveSpawnedActor repeats the loop body for one of our objects (experiment, from the ServerField tick).
static uintptr_t kRva_RemovalLoop = 0;
typedef void* (__fastcall* RemovalLoopFn)(void* container, void* b);
static RemovalLoopFn g_origRemovalLoop = nullptr;
static uintptr_t g_removalReasonGlobal = 0;   // rva of the dword the loop stores into actor+0x98
static void* __fastcall HookRemovalLoop(void* container, void* b) {
    uintptr_t list = 0; uint32_t n = 0; ReadBytes((uintptr_t)container + 0x1B0, &list, 8); ReadBytes((uintptr_t)container + 0x1B8, &n, 4);
    if (n && n < 64) {
        Log("[remove] pending removal loop: container %p (%s), %u actor%s", container, RttiName((uintptr_t)container) ? RttiName((uintptr_t)container) : "?", n, n == 1 ? "" : "s");
        for (uint32_t i = 0; i < n; i++) { uint64_t w = 0; uintptr_t actor = 0; ReadBytes(list + i * 16, &w, 8); ReadBytes(list + i * 16 + 8, &actor, 8); uint16_t st = 0; if (actor) ReadBytes(actor + 0x5E, &st, 2);
            Log("[remove]   entry %u: word 0x%llx actor %p (%s) state %u", i, (unsigned long long)w, (void*)actor, actor && RttiName(actor) ? RttiName(actor) : "?", st);
            if (actor && g_trace) {   // the actor's layout: which offsets hold objects (the scene object, components, the sync data)
                char line[900]; int k = 0;
                for (uintptr_t o = 0; o < 0x400 && k < (int)sizeof line - 80; o += 8) { uintptr_t v = 0; if (!ReadBytes(actor + o, &v, 8) || v < 0x10000 || (v >> 47)) continue; const char* n = RttiName(v); if (n) k += snprintf(line + k, sizeof line - k, " +%llx=%s", (unsigned long long)o, n + (strncmp(n, ".?AV", 4) == 0 ? 4 : 0)); }
                Log("[remove]   actor layout:%s", line);
            } }
    }
    return g_origRemovalLoop(container, b);
}
static void ResolveRemovalLoop() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 89 5C 24 10 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 4C 8B FA 48 8B B1 B0 01 00 00 8B A9 B8 01 00 00 48 C1", &n);
    if (!f || n != 1) { Log("[remove] pending removal loop: %d matches, not hooked", n); return; }
    kRva_RemovalLoop = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "removal loop", (unsigned long long)kRva_RemovalLoop);
    // the reason global: "mov edi, [rip+disp]" right after the entry copy (opcode 8B 3D at loop+0x4A)
    uint8_t op[2] = {}; int32_t disp = 0;
    if (ReadBytes(f + 0x4A, op, 2) && op[0] == 0x8B && op[1] == 0x3D && ReadBytes(f + 0x4C, &disp, 4)) { g_removalReasonGlobal = (f + 0x50 + disp) - g_base; uint32_t v = 0; ReadBytes(g_base + g_removalReasonGlobal, &v, 4); Log("[remove] removal reason global at rva 0x%llx = %u (%s)", (unsigned long long)g_removalReasonGlobal, v, DecodeErr(v).c_str()); }
    else Log("[remove] reason global not found (bytes %02x %02x)", op[0], op[1]);
}
static volatile uintptr_t g_removeActorRequest = 0;
void RequestRemoveSpawned(uintptr_t actor) { g_removeActorRequest = actor; Log("[remove] removal of actor %p requested (runs on the next server tick)", (void*)actor); }
static bool RemoveSpawnedActor(uintptr_t actor) {
#ifdef WB_UNIFIED_HOST_TEST
    const bool removed = actor && host::Seam().removeActor && host::Seam().removeActor(actor);
    if (removed) { REG_LOCK; g_managedNpcActors.erase(actor); }
    return removed;
#else
    if (!actor || !RttiName(actor)) { Log("[remove] actor %p: no RTTI, not touched", (void*)actor); return false; }
    {   // does this actor refer to the scene object our replay made? (a wrong actor must not be touched)
        uintptr_t so = 0; { std::lock_guard<std::mutex> l(g_spawnedMutex); for (const auto& g : g_spawned) if (g.actor == actor) so = g.so; }
        int found = -1; { int w = 0; if (ActorRefersTo(actor, so, &w)) found = w; }
        const bool managedNpc = IsManagedNpcActor(actor);
        Log("[remove] actor %p refers to scene object %p: %s", (void*)actor, (void*)so, found < 0 ? (managedNpc ? "managed World Builder NPC" : IsOurActor(actor) ? "not by pointer, but it was constructed during our replay" : "NOT FOUND (wrong actor?)") : "yes");
        if (found >= 0) Log("[remove]   at actor+0x%x%s%s", found & 0xFFF, (found >> 12) & 0xFF ? " -> +0x.." : "", (found >> 20) ? " -> +0x.. (two pointers deep)" : "");
        if (found < 0 && !IsOurActor(actor) && !managedNpc) return false;
    }
    const bool tr = g_trace; g_trace = true; g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount(); WatchObject(actor);   // the removal traces itself
    struct Restore { bool tr; ~Restore() { g_trace = tr; } } restore{ tr };
    uint16_t st = 0; ReadBytes(actor + 0x5E, &st, 2); uint32_t reason = 0; if (g_removalReasonGlobal) ReadBytes(g_base + g_removalReasonGlobal, &reason, 4);
    Log("[remove] actor %p (%s) state %u: reason %u, vtable[16] then vtable[34]", (void*)actor, RttiName(actor), st, reason);
    typedef void* (__fastcall* F1)(void*); typedef void* (__fastcall* F2)(void*, void*);
    uintptr_t lockObj = actor + 0x18; uintptr_t lvt = 0; ReadBytes(lockObj, &lvt, 8); uintptr_t vt = 0; ReadBytes(actor, &vt, 8);
    uintptr_t lockF = 0, unlockF = 0, f16 = 0, f34 = 0; ReadBytes(lvt + 8, &lockF, 8); ReadBytes(lvt + 0x10, &unlockF, 8); ReadBytes(vt + 0x80, &f16, 8); ReadBytes(vt + 0x110, &f34, 8);
    if (!InImage(lockF) || !InImage(unlockF) || !InImage(f16) || !InImage(f34)) { Log("[remove] unexpected vtables, not touched"); return false; }
    ((F1)lockF)((void*)lockObj);
    memcpy((void*)(actor + 0x98), &reason, 4); uint8_t zero = 0; memcpy((void*)(actor + 0x9C), &zero, 1); memcpy((void*)(actor + 0x5C), &st, 2);
    ((F1)f16)((void*)actor);
    ((F1)unlockF)((void*)lockObj);
    int result = 0; ((F2)f34)((void*)actor, &result);
    Log("[remove] vtable[34] result %d (%s)", result, DecodeErr((uint32_t)result).c_str());
    { std::lock_guard<std::mutex> l(g_regMutex); g_managedNpcActors.erase(actor); }
    std::lock_guard<std::mutex> l(g_spawnedMutex); for (auto it = g_spawned.begin(); it != g_spawned.end(); ++it) if (it->actor == actor) { g_spawned.erase(it); break; }
    return result == 0;
#endif
}
int SpawnedList(SpawnedInfo* out, int max) {
    std::lock_guard<std::mutex> l(g_spawnedMutex); int n = 0;
    for (int i = (int)g_spawned.size() - 1; i >= 0 && n < max; i--) { const SpawnedGimmick& g = g_spawned[(size_t)i]; SpawnedInfo& o = out[n++]; o.so = g.so; o.actor = g.actor; strncpy_s(o.prefab, g.prefab, _TRUNCATE); o.pos = { g.pos[0], g.pos[1], g.pos[2] }; o.ageMs = GetTickCount() - g.when; }
    return n;
}

// ---- actor constructor (research) ----------------------------------------------------------------------------------------
// The field's actor factory (0x2a82b80 in this build) allocates 0x100 bytes and runs the ServerActor base constructor
// (0x28f2380) on them before it installs the ServerNormalInGameActor vtable. Hooked: `this` of that constructor on the spawn
// thread during one of our replays is the actor the replay is creating (build-specific signature, research only).
static uintptr_t kRva_ActorCtor = 0;
typedef void* (__fastcall* ActorCtorFn)(void* self);
static ActorCtorFn g_origActorCtor = nullptr;
static void NoteReplayActorRaw(uintptr_t a);
static volatile LONG g_npcActorCapture = 0;
static DWORD g_npcActorCaptureThread = 0;
static uintptr_t g_npcActorCandidates[8] = {};
static int g_npcActorCandidateCount = 0;
struct RecentServerActor { uintptr_t actor = 0; DWORD tick = 0; };
static SRWLOCK g_recentActorLock = SRWLOCK_INIT;
static RecentServerActor g_recentActors[4096] = {};
static unsigned g_recentActorWrite = 0;
static void NoteRecentServerActor(uintptr_t a) {
    if (!a) return;
    AcquireSRWLockExclusive(&g_recentActorLock);
    g_recentActors[g_recentActorWrite++ % (unsigned)(sizeof g_recentActors / sizeof g_recentActors[0])] = { a, GetTickCount() };
    ReleaseSRWLockExclusive(&g_recentActorLock);
}
static void NoteNpcActorRaw(uintptr_t a) {
    if (!a || g_npcActorCandidateCount >= (int)(sizeof g_npcActorCandidates / sizeof g_npcActorCandidates[0])) return;
    for (int i = 0; i < g_npcActorCandidateCount; ++i) if (g_npcActorCandidates[i] == a) return;
    g_npcActorCandidates[g_npcActorCandidateCount++] = a;
}
static void* __fastcall HookActorCtor(void* self) {
    void* r = g_origActorCtor(self);
    NoteRecentServerActor((uintptr_t)self);
    if (g_inGimmickReplay && GetCurrentThreadId() == g_spawnWindowThread) NoteReplayActorRaw((uintptr_t)self);
    if (g_npcActorCapture && GetCurrentThreadId() == g_npcActorCaptureThread) NoteNpcActorRaw((uintptr_t)self);
    return r;
}
static void ResolveActorCtor() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 89 4C 24 08 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 03 33 C0 48 89 83 A0 00 00 00 C6 83 A8 00 00 00 FF", &n);
    if (!f || n != 1) { Log("[core] actor base constructor: %d matches, not hooked", n); return; }
    kRva_ActorCtor = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "actor constructor", (unsigned long long)kRva_ActorCtor);
}

// ---- vtable tracer (research) --------------------------------------------------------------------------------------
// Hooks every slot of one class's vtable with a logging thunk (only while "trace game calls" is on) to see which method the
// game calls with which arguments, e.g. how ServerSyncSceneObjectManager creates the server scene object of a dropped item.
// The vtable is found through the RTTI the game ships: TypeDescriptor (mangled name) -> CompleteObjectLocator -> vtable.
static uintptr_t FindVtableByName(const char* mangled) {
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t td = 0; const size_t nlen = strlen(mangled);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !td; i++) {   // the descriptor: {vtable, spare, name[]} with the name at +0x10
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + nlen + 1 <= n; k++) { if (p[k] == mangled[0] && memcmp(p + k, mangled, nlen) == 0 && p[k + nlen] == 0) { td = (uintptr_t)(p + k) - 0x10; break; } }
    }
    if (!td) return 0;
    const uint32_t tdRva = (uint32_t)(td - g_base); uintptr_t col = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !col; i++) {   // the locator: {signature 1, offset 0, cdOffset, tdRva, cdRva, selfRva}
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 24 <= n; k += 4) { uint32_t v; memcpy(&v, p + k + 12, 4); if (v != tdRva) continue; uint32_t sig, off; memcpy(&sig, p + k, 4); memcpy(&off, p + k + 4, 4); if (sig == 1 && off == 0) { col = (uintptr_t)(p + k); break; } }
    }
    if (!col) return 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {   // the vtable: the qword before it points at the locator
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 16 <= n; k += 8) { uintptr_t v; memcpy(&v, p + k, 8); if (v == col) return (uintptr_t)(p + k + 8); }
    }
    return 0;
}
// Research: every live object of an RTTI class, found by scanning committed private read-write memory for its vtable
// pointer (background thread, chunked guarded copies). Logs each hit with a dump of its first bytes and pointer fields.
void ResearchVtScan(const std::string& mangled, int maxHits, int dumpBytes) {
    const uintptr_t vt = FindVtableByName(mangled.c_str());
    if (!vt) { Log("[vtscan] %s: no vtable", mangled.c_str()); return; }
    std::thread([vt, mangled, maxHits, dumpBytes]() {
        const DWORD t0 = GetTickCount(); int hits = 0; uint64_t scanned = 0;
        std::vector<uint8_t> buf(1 << 20);
        MEMORY_BASIC_INFORMATION mbi{}; uintptr_t a = 0x10000;
        while (hits < maxHits && VirtualQuery((void*)a, &mbi, sizeof mbi) == sizeof mbi) {
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize; a = end;
            if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
            for (uintptr_t c = base; c < end && hits < maxHits; c += buf.size()) {
                const size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - c);
                if (!ReadBytes(c, buf.data(), n)) continue;
                scanned += n;
                for (size_t k = 0; k + 8 <= n && hits < maxHits; k += 8) {
                    uintptr_t v; memcpy(&v, buf.data() + k, 8); if (v != vt) continue;
                    char tag[48]; snprintf(tag, sizeof tag, "vtscan hit %d", hits); hits++;
                    DumpDeep(tag, c + k, (unsigned)dumpBytes, (unsigned)(dumpBytes / 8));
                }
            }
        }
        Log("[vtscan] %s (vtable rva 0x%llx): %d hits in %.1f GB, %lu ms", mangled.c_str(), (unsigned long long)(vt - g_base), hits, scanned / 1e9, GetTickCount() - t0);
    }).detach();
}
bool ReadMem(uintptr_t addr, void* out, size_t n) { return ReadBytes(addr, out, n); }
bool WriteMem(uintptr_t addr, const void* in, size_t n) { return WriteBytes(addr, in, n); }
void ResearchPeek(uintptr_t addr, int bytes, bool u16) {   // research: raw memory (guarded), optionally as unsigned 16-bit words
    if (!u16) { DumpDeep("peek", addr, (unsigned)bytes, (unsigned)std::min(bytes / 8, 32)); return; }
    std::vector<uint16_t> w((size_t)bytes / 2); if (!ReadBytes(addr, w.data(), w.size() * 2)) { Log("[peek] %p unreadable", (void*)addr); return; }
    std::string line; char t[16];
    for (size_t i = 0; i < w.size(); i++) { snprintf(t, sizeof t, "%u ", w[i]); line += t; if (i % 32 == 31) { Log("[peek] %p+%zx: %s", (void*)addr, (i - 31) * 2, line.c_str()); line.clear(); } }
    if (!line.empty()) Log("[peek] %p: %s", (void*)addr, line.c_str());
}
// Research: every occurrence of a byte pattern in committed private read-write memory (background thread, logged).
void ResearchFind(const std::vector<uint8_t>& pat, int maxHits) {
    if (pat.size() < 4) return;
    std::thread([pat, maxHits]() {
        const DWORD t0 = GetTickCount(); int hits = 0; uint64_t scanned = 0;
        std::vector<uint8_t> buf((1 << 20) + pat.size());
        MEMORY_BASIC_INFORMATION mbi{}; uintptr_t a = 0x10000;
        while (hits < maxHits && VirtualQuery((void*)a, &mbi, sizeof mbi) == sizeof mbi) {
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize; a = end;
            if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
            for (uintptr_t c = base; c < end && hits < maxHits; c += (1 << 20)) {
                const size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - c);
                if (n < pat.size() || !ReadBytes(c, buf.data(), n)) continue;
                scanned += n;
                for (size_t k = 0; k + pat.size() <= n && hits < maxHits; k += 2) {
                    if (buf[k] != pat[0] || memcmp(buf.data() + k, pat.data(), pat.size()) != 0) continue;
                    Log("[find] hit %d at %p (region %p, size %zx)", hits, (void*)(c + k), (void*)base, (size_t)mbi.RegionSize); hits++;
                }
            }
        }
        Log("[find] %zu-byte pattern: %d hits in %.1f GB, %lu ms", pat.size(), hits, scanned / 1e9, GetTickCount() - t0);
    }).detach();
}
static const int kVtMax = 160; static const int kVtClasses = 6;
static void* g_vtOrig[kVtClasses][kVtMax] = {}; static const char* g_vtClass[kVtClasses] = { "", "", "", "", "", "" };
// class 5: the game's terrain heightfield geometry (no RTTI name; vtable given at runtime), logged only on a thread that is
// inside one of our traced ground casts (research: which geometry calls a character-style cast makes and what they return)
static thread_local bool t_geoTracing = false; static volatile LONG g_geoTraceArm = 0;
// class 4 (TrocTrSpawnCharacterCheatReq, a static handler object): slot 2 = execute(handler, &result, packet). Always logged with
// the packet object, its sender and its buffer, to learn the request format from a mod that uses it (NPC spawn research).
// class 3 (ServerNormalInGameActor): the first actor whose method runs during one of our replays is the actor the replay created
volatile uintptr_t g_replayActor = 0;
// class 2 (ServerField) is counted per slot (per-tick slots would flood the log); a slot is logged with arguments only for its first 20 calls of a trace
static volatile LONG g_vtCount[kVtMax] = {}; static DWORD g_vtCountThread[kVtMax] = {};
static void DumpServerFieldCounts() { for (int i = 0; i < kVtMax; i++) if (g_vtCount[i]) { Log("[vt] ServerField slot %d: %ld calls (thread %lu)", i, (long)g_vtCount[i], g_vtCountThread[i]); g_vtCount[i] = 0; } }
// class 1 (SceneObjectServer) is only logged for objects on the watch list: the ones the manager just created or that a spawn frame referred to
static uintptr_t g_vtWatch[16] = {}; static int g_vtWatchNext = 0;
// g_spawnWindowTick / g_spawnWindowThread (declared with the spawn hook): set by the gimmick spawn hook: the drop / housing flow sets its scene object up right around the spawn call
static void WatchObject(uintptr_t o) { if (!o) return; for (uintptr_t w : g_vtWatch) if (w == o) return; g_vtWatch[g_vtWatchNext++ % 16] = o; }
static bool Watched(uintptr_t o) { for (uintptr_t w : g_vtWatch) if (w && w == o) return true; return false; }
static std::string ArgText(void* a) {   // what an argument might be: an object (RTTI), a prefab path, a float triple, or a plain value
    char b[160]; const uintptr_t v = (uintptr_t)a;
    if (v < 0x10000 || (v >> 47)) { snprintf(b, sizeof b, "%llu", (unsigned long long)v); return b; }
    const char* n = RttiName(v); if (n) { snprintf(b, sizeof b, "%p (%s)", a, n); return b; }
    std::string path = PrefabPathText(v); if (path.size() > 4 && path.find("/") != std::string::npos) { snprintf(b, sizeof b, "%p path %s", a, path.c_str()); return b; }
    float f[4] = { 0, 0, 0, 0 }; if (ReadBytes(v, f, 16) && std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]) && fabsf(f[0]) < 20000 && fabsf(f[1]) < 5000 && fabsf(f[2]) < 20000 && (fabsf(f[0]) > 1 || fabsf(f[2]) > 1)) { snprintf(b, sizeof b, "%p floats %.2f %.2f %.2f %.2f", a, f[0], f[1], f[2], f[3]); return b; }
    snprintf(b, sizeof b, "%p", a); return b;
}
template<int C, int N> static void* __fastcall VtThunk(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    typedef void* (__fastcall* Fn)(void*, void*, void*, void*, void*, void*, void*, void*);
    if (C == 5) {
        if (!t_geoTracing) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h);
        Log("[geo] slot %d: this=%p rdx=%s r8=%s r9=%s s5=%s s6=%s", N, a, ArgText(b).c_str(), ArgText(c).c_str(), ArgText(d).c_str(), ArgText(e).c_str(), ArgText(f).c_str());
        void* r = ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h);
        float q[4] = {}; uint16_t mat = 0; uint8_t flag = 0;
        if (N == 4) { ReadBytes((uintptr_t)d, q, 16); ReadBytes((uintptr_t)e, &mat, 2); ReadBytes((uintptr_t)f, &flag, 1); }
        if (N == 4) Log("[geo] slot 4 (%lld, %lld) -> corners %.3f %.3f %.3f %.3f material %u flag %u", (long long)(intptr_t)b, (long long)(intptr_t)c, q[0], q[1], q[2], q[3], mat, flag);
        { void* fr[10] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 10, fr, nullptr); char chain[240]; int k = 0;
          for (USHORT i = 0; i < n && k < (int)sizeof chain - 16; i++) { const uintptr_t v = (uintptr_t)fr[i]; k += snprintf(chain + k, sizeof chain - k, InImage(v) ? " %llx" : " ?", InImage(v) ? (unsigned long long)(v - g_base) : 0ull); }
          Log("[geo] slot %d chain:%s", N, chain); }
        if (N != 4) Log("[geo] slot %d returned %s", N, ArgText(r).c_str());
        return r;
    }
    if (C == 4) {
        Log("[troc] %s slot %d: handler=%p result=%p packet=%p thread %lu", g_vtClass[C], N, a, b, c, GetCurrentThreadId());
        if (N == 2 && c) { DumpBlock("troc packet", (uintptr_t)c, 0x60); uintptr_t sess = 0, buf = 0; uint16_t len = 0; ReadBytes((uintptr_t)c, &sess, 8); ReadBytes((uintptr_t)c + 0x10, &len, 2); ReadBytes((uintptr_t)c + 0x18, &buf, 8);
            Log("[troc]   sender %p (%s), total length %u, buffer %p", (void*)sess, sess && RttiName(sess) ? RttiName(sess) : "-", len, (void*)buf); if (sess) DumpDeep("troc sender", sess, 0x80, 6); if (buf) DumpBlock("troc buffer", buf, len ? (len < 0x80 ? len : 0x80) : 0x40); }
        void* r = ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); int code = 0; if (N == 2 && b) ReadBytes((uintptr_t)b, &code, 4);
        Log("[troc] %s slot %d returned %p, result %d (%s)", g_vtClass[C], N, r, code, DecodeErr((uint32_t)code).c_str()); return r; }
    if (C == 3) { if (g_inGimmickReplay && GetCurrentThreadId() == g_spawnWindowThread) NoteReplayActor((uintptr_t)a, N); if (!g_trace || !g_inGimmickReplay) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); }
    if (C == 2 && N == 9 && g_gimmickReplayArmed) { if (InterlockedCompareExchange(&g_gimmickReplayArmed, 0, 1) == 1) ReplayGimmick(); }
    if (C == 2 && N == 9 && g_removeActorRequest) { const uintptr_t a = (uintptr_t)InterlockedExchangePointer((void* volatile*)&g_removeActorRequest, nullptr); if (a) RemoveSpawnedActor(a); }
    if (C == 2 && N == 9) { g_serverFieldObj = (uintptr_t)a; g_serverFieldTick = GetTickCount(); ProcessServerJobs(); ProcessGimmickQueue(); }   // ServerField slot 9 runs ~18x per second on the server thread: armed replays run here, no game spawn needed
    if (C == 2) { if (!g_trace) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); const LONG k = InterlockedIncrement(&g_vtCount[N]); g_vtCountThread[N] = GetCurrentThreadId(); if (k > 20) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); }
    const bool log = g_trace && (C == 0 || C == 2 || Watched((uintptr_t)a) || (GetCurrentThreadId() == g_spawnWindowThread && GetTickCount() - g_spawnWindowTick < 500));
    if (!log) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h);
    Log("[vt] %s slot %d: this=%p rdx=%s r8=%s r9=%s s5=%s s6=%s", g_vtClass[C], N, a, ArgText(b).c_str(), ArgText(c).c_str(), ArgText(d).c_str(), ArgText(e).c_str(), ArgText(f).c_str());
    if (C == 0 && (N == 2 || N == 3)) { void* fr[14] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 14, fr, nullptr); char chain[300]; int k = 0; for (USHORT i = 0; i < n && k < (int)sizeof chain - 16; i++) { const uintptr_t v = (uintptr_t)fr[i]; k += snprintf(chain + k, sizeof chain - k, " %llx", InImage(v) ? (unsigned long long)(v - g_base) : 0ull); } Log("[vt] %s slot %d chain:%s", g_vtClass[C], N, chain); }
    void* r = ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h);
    if (C == 1) { uint32_t u[4] = { 0, 0, 0, 0 }; ReadBytes((uintptr_t)a + 0x1D8, u, 16); Log("[vt] %s slot %d returned %s; uuid now %08x %08x %08x %08x", g_vtClass[C], N, ArgText(r).c_str(), u[0], u[1], u[2], u[3]); }
    else Log("[vt] %s slot %d returned %s", g_vtClass[C], N, ArgText(r).c_str());
    if (C == 0) { for (void* q : { b, c }) { const uintptr_t v = (uintptr_t)q; if (v > 0x10000 && !(v >> 47)) { const char* n = RttiName(v); if (n && strstr(n, "SceneObjectServer@")) WatchObject(v); } } }
    if (C == 0 && N == 15 && b) { uintptr_t made = 0; if (ReadBytes((uintptr_t)b, &made, 8) && made) { WatchObject(made); Log("[vt] watching new %s %p", RttiName(made) ? RttiName(made) : "object", (void*)made); } }
    return r;
}
template<int C, int N> struct VtThunkTable { static void fill(void** out) { out[N] = (void*)&VtThunk<C, N>; VtThunkTable<C, N - 1>::fill(out); } };
template<int C> struct VtThunkTable<C, -1> { static void fill(void**) {} };
static bool SharedStub(uintptr_t f) {   // pure-virtual placeholders and tiny thunks are shared by hundreds of vtables: never hook those
    DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(f, &base, nullptr);
    if (!rf) return true;
    return (rf->EndAddress - rf->BeginAddress) < 32 || base + rf->BeginAddress != f;
}
static void InstallVtableTracer(int cls, const char* mangled, const char* shortName, int slots, uintptr_t vtGiven = 0) {
    const uintptr_t vt = vtGiven ? vtGiven : FindVtableByName(mangled); if (!vt) { Log("[vt] %s: vtable not found", shortName); return; }
    void* thunks[kVtMax]; if (cls == 0) VtThunkTable<0, kVtMax - 1>::fill(thunks); else if (cls == 1) VtThunkTable<1, kVtMax - 1>::fill(thunks); else if (cls == 2) VtThunkTable<2, kVtMax - 1>::fill(thunks); else if (cls == 3) VtThunkTable<3, kVtMax - 1>::fill(thunks); else if (cls == 4) VtThunkTable<4, kVtMax - 1>::fill(thunks); else VtThunkTable<5, kVtMax - 1>::fill(thunks);
    g_vtClass[cls] = shortName; int ok = 0, skipped = 0;
    for (int i = 0; i < slots && i < kVtMax; i++) {
        uintptr_t f = 0; if (!ReadPtr(vt + (uintptr_t)i * 8, &f) || !InImage(f)) continue;
        if (SharedStub(f)) { skipped++; continue; }
        ReleaseHookPiece();
        if (MH_CreateHook((void*)f, thunks[i], &g_vtOrig[cls][i]) == MH_OK && MH_EnableHook((void*)f) == MH_OK) ok++;
    }
    Log("[vt] %s: vtable at rva 0x%llx, %d of %d slots traced (%d shared stubs skipped)", shortName, (unsigned long long)(vt - g_base), ok, slots, skipped);
}

// ---- native render camera: the renderer's own camera object instead of guessing among constant-buffer copies ----
// Path found by CrimsonDesertTelemetry (github.com/fabianviol/CrimsonDesertTelemetry, MIT): a global holds the renderer camera
// (read by "mov rax,[rip+X]; vmovsd xmm6,[rax+0xC8]; mov ebx,[rax+0xD0]"), camera+0x428 points to the scene constants of the
// frame being rendered: +0x20 frame number, +0x30/+0x34 screen size (+0x38/+0x3C reciprocals), +0x80 eye, view matrix at +0x3E0
// (columns = right / up / forward, row 3 = -R*eye), projection at +0x4E0 (m00 +0x4E0, m11 +0x4F4, +0x50C = 1), +0xAC0 = 6360000
// (earth radius, a layout signature). The camera class has no RTTI, so it is recognised by two vtable slot fingerprints
// (slot 2 unchanged since 1.0.0.2658, slot 1 with two register-allocation bytes wildcarded).
static uintptr_t g_natCamGlobal = 0, g_natCamVt = 0;
static uintptr_t FindVtableBySlots(uintptr_t f1, uintptr_t f2) {   // read-only image data holding {slot1, slot2} = {f1, f2}; must be unique
    auto dos = (PIMAGE_DOS_HEADER)g_base; auto nt = (PIMAGE_NT_HEADERS)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0; int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const DWORD ch = sec[i].Characteristics;
        if ((ch & IMAGE_SCN_MEM_EXECUTE) || (ch & IMAGE_SCN_MEM_WRITE) || !(ch & IMAGE_SCN_MEM_READ)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 8; k + 16 <= sz; k += 8) {
            uintptr_t a, b; memcpy(&a, p + k, 8); if (a != f1) continue; memcpy(&b, p + k + 8, 8); if (b != f2) continue;
            found = (uintptr_t)(p + k - 8); n++;
        }
    }
    return n == 1 ? found : 0;
}
static void ResolveNativeCamera() {
    int n = 0; const uintptr_t ref = FindPatternCount("48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00", &n);
    if (!ref || n != 1) { Log("[rendercam] native camera: global load %d matches, renderer camera unavailable", n); return; }
    int32_t disp = 0; memcpy(&disp, (const void*)(ref + 3), 4); const uintptr_t global = ref + 7 + disp;
    int n1 = 0, n2 = 0;
    const uintptr_t f1 = FindPatternCount("48 8B C4 48 89 58 10 48 89 70 18 41 54 41 56 41 57 48 81 EC 10 01 00 00 8B B1 A8 02 00 00 4D 8B ?? 4D 8B ?? 4C 8B F2 48 8B D9 85 F6", &n1);
    const uintptr_t f2 = FindPatternCount("40 53 48 83 EC 20 48 8B 01 48 8B DA FF 50 68 4C 8B C8 48 63 48 08 85 C9 75 08 32 C0 48 83 C4 20 5B C3 48 89 7C 24 30 41 B0 01 33 FF 4C 8B D1 C5 F8 57 C0 48 83 F9 04", &n2);
    if (!f1 || !f2 || n1 != 1 || n2 != 1) { Log("[rendercam] native camera: slot fingerprints %d / %d matches, renderer camera unavailable", n1, n2); return; }
    const uintptr_t vt = FindVtableBySlots(f1, f2);
    if (!vt) { Log("[rendercam] native camera: no unique vtable with both slots, renderer camera unavailable"); return; }
    g_natCamGlobal = global; g_natCamVt = vt;
    Log("resolved %-20s rva 0x%llx (global rva 0x%llx, vtable rva 0x%llx)", "native render camera", (unsigned long long)(ref - g_base), (unsigned long long)(global - g_base), (unsigned long long)(vt - g_base));
}
uintptr_t NativeCameraObject() {
    uintptr_t cam = 0, vt = 0;
    return g_natCamGlobal && ReadPtr(g_natCamGlobal, &cam) && cam && ReadPtr(cam, &vt) && vt == g_natCamVt ? cam : 0;
}
bool RenderCamera(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd, float* m00, float* m11) {
    if (!g_natCamGlobal) return false;
    uintptr_t cam = 0, vt = 0, src = 0;
    if (!ReadPtr(g_natCamGlobal, &cam) || !cam || !ReadPtr(cam, &vt) || vt != g_natCamVt || !ReadPtr(cam + 0x428, &src) || !src) return false;
    // two reads of the same frame number around the fields: the renderer may be writing the next frame into this block
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t f0 = 0, f1 = 0; float sig = 0, v[16], p[16], eye[3];
        if (!ReadBytes(src + 0x20, &f0, 4) || !ReadBytes(src + 0xAC0, &sig, 4) || sig != 6360000.0f) return false;
        if (!ReadBytes(src + 0x80, eye, 12) || !ReadBytes(src + 0x3E0, v, sizeof v) || !ReadBytes(src + 0x4E0, p, sizeof p) || !ReadBytes(src + 0x20, &f1, 4)) return false;
        if (f0 != f1) continue;
        const Vec3 r = { v[0], v[4], v[8] }, u = { v[1], v[5], v[9] }, f = { v[2], v[6], v[10] };
        auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        if (fabsf(dot(r, r) - 1) > 0.01f || fabsf(dot(u, u) - 1) > 0.01f || fabsf(dot(f, f) - 1) > 0.01f || fabsf(dot(r, u)) > 0.01f || fabsf(dot(r, f)) > 0.01f || fabsf(dot(u, f)) > 0.01f) return false;
        if (!(p[0] > 0.1f && p[0] < 20.0f && p[5] > 0.3f && p[5] < 20.0f && fabsf(p[11] - 1.0f) < 1e-4f && std::isfinite(eye[0]) && fabsf(eye[0]) < 1e6f)) return false;
        if (pos) *pos = { eye[0], eye[1], eye[2] }; if (right) *right = r; if (up) *up = u; if (fwd) *fwd = f;
        if (m00) *m00 = p[0]; if (m11) *m11 = p[5];
        return true;
    }
    return false;
}

// ---- free-fly camera ----
// The renderer camera's pose is set once per frame by one function (found with camwatch): (camera, float rot[16], float pos[3],
// float a[3], float tilePos[3], float eye[3], ...). rot holds the view rotation with right / up / forward as columns (m[0],m[4],m[8] =
// right), pos (world) goes to camera+0xC8 (what the renderer and our gizmo read), tilePos is the same position relative to its
// world tile (+0xEC), eye is (0,0,0): the view translation is camera-relative (rendering happens around the camera). While the free
// camera is on, the hook passes our own rotation and position instead; everything built from the camera (scene constants,
// culling, the gizmo) follows. The game keeps simulating its own follow camera, which comes back the moment we let go.
typedef void* (__fastcall* SetCamPoseFn)(void*, const float*, const float*, const float*, const float*, const float*, void*);
static SetCamPoseFn g_origSetCamPose = nullptr; static uintptr_t kRva_SetCamPose = 0;
static volatile bool g_fcOn = false; static volatile bool g_fcInit = false;
static float g_fcPos[3] = {}, g_fcYaw = 0, g_fcPitch = 0, g_fcRightSign = 1, g_fcUpSign = 1;
static LARGE_INTEGER g_fcLast = {};
static volatile float g_fcDollyPending = 0;   // mouse-wheel metres from the render thread, applied by the game-thread step
static volatile bool g_fcFocusPending = false; static float g_fcFocusTarget[3] = {}; static float g_fcFocusRadius = 1.0f;
static volatile int g_fcViewPresetPending = -1;
// camera scene object vs. view: S = V * C and p_so = p_view + V * d, both measured on the first free frame and kept
static float g_fcC[9] = {}, g_fcD[3] = {}; static volatile bool g_fcRel = false;
static void QToM(const float* q, float* m) {   // columns = rotated basis (same as CameraBasis), m[col*3+row]
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w); m[2] = 2 * (x * z - y * w);
    m[3] = 2 * (x * y - z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + x * w);
    m[6] = 2 * (x * z + y * w); m[7] = 2 * (y * z - x * w); m[8] = 1 - 2 * (x * x + y * y);
}
static void MToQ(const float* m, float* q) {   // inverse of QToM; R[row][col] = m[col*3+row]
    auto R = [&](int r, int c) { return m[c * 3 + r]; };
    const float tr = R(0, 0) + R(1, 1) + R(2, 2);
    if (tr > 0) { const float s = sqrtf(tr + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (R(2, 1) - R(1, 2)) / s; q[1] = (R(0, 2) - R(2, 0)) / s; q[2] = (R(1, 0) - R(0, 1)) / s; }
    else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) { const float s = sqrtf(1.0f + R(0, 0) - R(1, 1) - R(2, 2)) * 2; q[3] = (R(2, 1) - R(1, 2)) / s; q[0] = 0.25f * s; q[1] = (R(0, 1) + R(1, 0)) / s; q[2] = (R(0, 2) + R(2, 0)) / s; }
    else if (R(1, 1) > R(2, 2)) { const float s = sqrtf(1.0f + R(1, 1) - R(0, 0) - R(2, 2)) * 2; q[3] = (R(0, 2) - R(2, 0)) / s; q[0] = (R(0, 1) + R(1, 0)) / s; q[1] = 0.25f * s; q[2] = (R(1, 2) + R(2, 1)) / s; }
    else { const float s = sqrtf(1.0f + R(2, 2) - R(0, 0) - R(1, 1)) * 2; q[3] = (R(1, 0) - R(0, 1)) / s; q[0] = (R(0, 2) + R(2, 0)) / s; q[1] = (R(1, 2) + R(2, 1)) / s; q[2] = 0.25f * s; }
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (l > 1e-6f) for (int i = 0; i < 4; i++) q[i] /= l;
}
static void M3Mul(const float* a, const float* b, float* o) { for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) o[c * 3 + r] = a[0 * 3 + r] * b[c * 3 + 0] + a[1 * 3 + r] * b[c * 3 + 1] + a[2 * 3 + r] * b[c * 3 + 2]; }
static void M3TMul(const float* a, const float* b, float* o) { for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) o[c * 3 + r] = a[r * 3 + 0] * b[c * 3 + 0] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2]; }   // a^T * b
static void FcBasis(float* r, float* u, float* f);
// one step of the free camera (mouse turn, key movement). Runs where the camera scene object gets its pose, the first camera
// update of a frame, so culling and the renderer's view use the same pose; a step in the later renderer hook would leave the
// culling one frame behind and cut the world's edges while turning.
static void FcStep() {
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    float dt = (float)(now.QuadPart - g_fcLast.QuadPart) / (float)freq.QuadPart; g_fcLast = now; if (dt < 0 || dt > 0.1f) dt = 0.1f;
    float dx = 0, dy = 0; input::TakeLookDelta(&dx, &dy);
    g_fcYaw += dx * g_fcSens; g_fcPitch -= dy * g_fcSens;
    const int viewPreset = g_fcViewPresetPending; g_fcViewPresetPending = -1;
    if (viewPreset == 1) g_fcPitch = 0.0f;
    else if (viewPreset == 2) g_fcPitch = -89.0f;
    else if (viewPreset == 3) g_fcPitch = 89.0f;
    if (g_fcPitch > 89.0f) g_fcPitch = 89.0f; if (g_fcPitch < -89.0f) g_fcPitch = -89.0f;
    if (g_fcYaw > 180.0f) g_fcYaw -= 360.0f; if (g_fcYaw < -180.0f) g_fcYaw += 360.0f;
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    if (g_fcFocusPending) {
        const float tx = g_fcFocusTarget[0], ty = g_fcFocusTarget[1], tz = g_fcFocusTarget[2];
        float dx = tx - g_fcPos[0], dy = ty - g_fcPos[1], dz = tz - g_fcPos[2];
        float l = sqrtf(dx * dx + dy * dy + dz * dz);
        if (l < 0.01f) { dx = f[0]; dy = f[1]; dz = f[2]; l = 1.0f; }
        dx /= l; dy /= l; dz /= l;
        g_fcYaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
        g_fcPitch = asinf(std::max(-1.0f, std::min(1.0f, dy))) * 180.0f / 3.14159265f;
        const float dist = std::max(3.0f, std::min(120.0f, std::max(1.0f, g_fcFocusRadius) * 2.6f));
        g_fcPos[0] = tx - dx * dist; g_fcPos[1] = ty - dy * dist; g_fcPos[2] = tz - dz * dist;
        g_fcFocusPending = false;
        FcBasis(r, u, f);
    }
    { const float d = g_fcDollyPending; g_fcDollyPending = 0; if (d != 0 && std::isfinite(d)) for (int i = 0; i < 3; i++) g_fcPos[i] += f[i] * d; }
    // editor shortcuts (Ctrl+Z, Ctrl+D ...) must not fly the camera: no movement while Ctrl is held with a shortcut letter
    const bool ctrl = input::ScanDown(0x1D, false) || input::ScanDown(0x1D, true);
    const bool ctrlCommand = ctrl && (input::ScanDown(0x2C, false) || input::ScanDown(0x15, false) || input::ScanDown(0x2E, false) || input::ScanDown(0x2D, false) ||
                                      input::ScanDown(0x2F, false) || input::ScanDown(0x20, false) || input::ScanDown(0x22, false) || input::ScanDown(0x1E, false));
    if (!g_uiTextInput && !g_fcHoldMove && !ctrlCommand) {   // W/S along the view, A/D sideways, E/Space up, Q down; Shift x4. Ctrl stays free for editor multi-select/shortcuts.
        float v = g_fcSpeed * dt * (input::ScanDown(0x2A, false) ? 4.0f : 1.0f);
        const float fw = (input::ScanDown(0x11, false) ? 1.0f : 0.0f) - (input::ScanDown(0x1F, false) ? 1.0f : 0.0f);
        const float sd = (input::ScanDown(0x20, false) ? 1.0f : 0.0f) - (input::ScanDown(0x1E, false) ? 1.0f : 0.0f);
        const float up = (input::ScanDown(0x12, false) || input::ScanDown(0x39, false) ? 1.0f : 0.0f) - (input::ScanDown(0x10, false) ? 1.0f : 0.0f);
        for (int i = 0; i < 3; i++) g_fcPos[i] += v * (f[i] * fw + r[i] * sd) + (i == 1 ? v * up : 0.0f);
    }
}
static bool FreeCamSceneXf(const float* in, float* out) {
    float t[11]; if (!ReadBytes((uintptr_t)in, t, 44)) return false;
    if (!g_fcRel) { memcpy(g_fcSoLast, t, sizeof t); g_fcSoSeen = true; return false; }   // not yet related: the game's pose passes, and is remembered
    FcStep();
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    const float V[9] = { r[0], r[1], r[2], u[0], u[1], u[2], f[0], f[1], f[2] }; float S[9]; M3Mul(V, g_fcC, S);
    float q[4]; MToQ(S, q);
    Vec3 p = { g_fcPos[0], g_fcPos[1], g_fcPos[2] };
    for (int i = 0; i < 3; i++) { const float d = V[0 * 3 + i] * g_fcD[0] + V[1 * 3 + i] * g_fcD[1] + V[2 * 3 + i] * g_fcD[2]; (&p.x)[i] += d; }
    memcpy(out, t, 44); out[3] = q[0]; out[4] = q[1]; out[5] = q[2]; out[6] = q[3];
    const int tx = (int)(p.x * 0.001), tz = (int)(p.z * 0.001);   // like MakeTransform
    out[7] = p.x - tx * 1000.0f; out[8] = p.y; out[9] = p.z - tz * 1000.0f;
    const int16_t tile[2] = { (int16_t)tx, (int16_t)tz }; memcpy(&out[10], tile, 4); out[11] = 0;
    return true;
}
float g_fcSpeed = 10.0f, g_fcSens = 0.12f;   // m/s, degrees per mouse count
volatile bool g_fcHoldMove = false;   // set by the editor: a context menu is open or a field is being edited
static void V3Cross(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void V3Norm(float* v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void FcBasis(float* r, float* u, float* f) {   // from yaw/pitch, with the game's handedness learned at the start
    const float y = g_fcYaw * 3.14159265f / 180.0f, p = g_fcPitch * 3.14159265f / 180.0f;
    f[0] = cosf(p) * sinf(y); f[1] = sinf(p); f[2] = cosf(p) * cosf(y);
    const float wu[3] = { 0, 1, 0 }; V3Cross(wu, f, r); V3Norm(r); for (int i = 0; i < 3; i++) r[i] *= g_fcRightSign;
    V3Cross(f, r, u); V3Norm(u); for (int i = 0; i < 3; i++) u[i] *= g_fcUpSign;
}
static void* __fastcall HookSetCamPose(void* cam, const float* rot, const float* pos, const float* a, const float* b, const float* eye, void* c) {
    if (!g_fcOn || (uintptr_t)cam != NativeCameraObject() || !rot || !pos || !eye) { g_fcInit = false; g_fcRel = false; g_fcSceneObj = 0; return g_origSetCamPose(cam, rot, pos, a, b, eye, c); }
    g_fcSceneObj = CameraSceneObject();
    float m[16]; if (!ReadBytes((uintptr_t)rot, m, sizeof m)) return g_origSetCamPose(cam, rot, pos, a, b, eye, c);
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    if (!g_fcInit) {   // start where the game camera is, looking the same way; learn which way its right / up vectors point
        float p[3]; if (!ReadBytes((uintptr_t)pos, p, sizeof p)) return g_origSetCamPose(cam, rot, pos, a, b, eye, c);
        const float f0[3] = { m[2], m[6], m[10] }, r0[3] = { m[0], m[4], m[8] }, u0[3] = { m[1], m[5], m[9] };
        memcpy(g_fcPos, p, sizeof p);
        g_fcYaw = atan2f(f0[0], f0[2]) * 180.0f / 3.14159265f; g_fcPitch = asinf(f0[1] < -1.0f ? -1.0f : f0[1] > 1.0f ? 1.0f : f0[1]) * 180.0f / 3.14159265f;
        g_fcRightSign = g_fcUpSign = 1; float r[3], u[3], f[3]; FcBasis(r, u, f);
        g_fcRightSign = (r[0] * r0[0] + r[1] * r0[1] + r[2] * r0[2]) >= 0 ? 1.0f : -1.0f; FcBasis(r, u, f);
        g_fcUpSign = (u[0] * u0[0] + u[1] * u0[1] + u[2] * u0[2]) >= 0 ? 1.0f : -1.0f;
        float ea[3] = {}, ba[3] = {}, aa[3] = {}; ReadBytes((uintptr_t)eye, ea, 12); ReadBytes((uintptr_t)b, ba, 12); ReadBytes((uintptr_t)a, aa, 12);
        Log("[freecam] on at (%.2f %.2f %.2f) yaw %.1f pitch %.1f, right %+.0f up %+.0f; eye (%.2f %.2f %.2f) a (%.3f %.3f %.3f) b (%.3f %.3f %.3f)",
            p[0], p[1], p[2], g_fcYaw, g_fcPitch, g_fcRightSign, g_fcUpSign, ea[0], ea[1], ea[2], aa[0], aa[1], aa[2], ba[0], ba[1], ba[2]);
        g_fcLast = now; g_fcInit = true;
    }
    if (!g_fcRel && g_fcSoSeen) {   // relation camera scene object <-> view from this frame's game poses (the scene object was set just before)
        float p[3]; ReadBytes((uintptr_t)pos, p, sizeof p);
        const float V0[9] = { m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10] }; float S0[9]; QToM(&g_fcSoLast[3], S0);
        M3TMul(V0, S0, g_fcC);
        int16_t tile[2]; memcpy(tile, &g_fcSoLast[10], 4);
        const float ps[3] = { g_fcSoLast[7] + tile[0] * 1000.0f, g_fcSoLast[8], g_fcSoLast[9] + tile[1] * 1000.0f }, dw[3] = { ps[0] - p[0], ps[1] - p[1], ps[2] - p[2] };
        for (int i = 0; i < 3; i++) g_fcD[i] = V0[i * 3 + 0] * dw[0] + V0[i * 3 + 1] * dw[1] + V0[i * 3 + 2] * dw[2];
        Log("[freecam] camera scene object related: offset in view space (%.2f %.2f %.2f), C diag %.2f %.2f %.2f", g_fcD[0], g_fcD[1], g_fcD[2], g_fcC[0], g_fcC[4], g_fcC[8]);
        g_fcRel = true;
    }
    if (!g_fcRel) FcStep();   // normally advanced earlier in the frame, where the camera scene object gets its pose
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    m[0] = r[0]; m[4] = r[1]; m[8] = r[2]; m[1] = u[0]; m[5] = u[1]; m[9] = u[2]; m[2] = f[0]; m[6] = f[1]; m[10] = f[2];
    // tile-relative position: the game's own offset between its world and tile position, applied to ours (a flight across a tile
    // border keeps the old tile's origin, which only costs float precision far away)
    float po[3], bo[3]; static float s_pos[3], s_tile[3], s_rot[16];   // the game thread is the only caller
    if (!ReadBytes((uintptr_t)pos, po, sizeof po) || !b || !ReadBytes((uintptr_t)b, bo, sizeof bo)) return g_origSetCamPose(cam, rot, pos, a, b, eye, c);
    for (int i = 0; i < 3; i++) { s_pos[i] = g_fcPos[i]; s_tile[i] = g_fcPos[i] - (po[i] - bo[i]); }
    memcpy(s_rot, m, sizeof m);
    return g_origSetCamPose(cam, s_rot, s_pos, a, s_tile, eye, c);
}
static void ResolveSetCamPose() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 81 EC 88 00 00 00 C5 FC 10 02 C5 FC 11 41 48 C5 FC 10 4A 20 C5 FA 10 2D ?? ?? ?? ?? 48 8B 84 24 B8 00 00 00", &n);
    if (!f || n != 1) { Log("[freecam] camera pose function: %d matches, free camera off", n); return; }
    kRva_SetCamPose = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "camera pose", (unsigned long long)kRva_SetCamPose);
}
bool FreeCamAvailable() { return g_origSetCamPose != nullptr && g_natCamGlobal != 0; }
bool FreeCamActive() { return g_fcOn; }
void SetFreeCam(bool on) {
    if (on && !FreeCamAvailable()) { Log("[freecam] not available in this game build (see the log)"); return; }
    if (on == g_fcOn) return;
    g_fcInit = false; g_fcRel = false; g_fcSoSeen = false; g_fcDollyPending = 0; g_fcFocusPending = false; g_fcViewPresetPending = -1; if (!on) g_fcSceneObj = 0; input::SetFreeCam(on); g_fcOn = on;
    Log("[freecam] %s", on ? "requested" : "off");
}
bool FreeCamBasis(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd) {
    if (!g_fcOn || !g_fcInit) return false;
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    if (pos) *pos = { g_fcPos[0], g_fcPos[1], g_fcPos[2] }; if (right) *right = { r[0], r[1], r[2] }; if (up) *up = { u[0], u[1], u[2] }; if (fwd) *fwd = { f[0], f[1], f[2] };
    return true;
}
void FreeCamDolly(float meters) {   // mouse wheel: along the view, applied by FcStep on the game thread
    if (!g_fcOn || !g_fcInit || !std::isfinite(meters)) return;
    g_fcDollyPending = g_fcDollyPending + meters;
}
void FreeCamFocus(Vec3 target, float radius) {
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return;
    g_fcFocusTarget[0] = target.x; g_fcFocusTarget[1] = target.y; g_fcFocusTarget[2] = target.z;
    g_fcFocusRadius = std::isfinite(radius) ? std::max(0.25f, radius) : 1.0f; g_fcFocusPending = true;
}
void FreeCamViewPreset(int preset) { if (preset >= 1 && preset <= 3) g_fcViewPresetPending = preset; }
void FreeCamTurn(float dyaw, float dpitch) { g_fcYaw += dyaw; g_fcPitch += dpitch; }   // tests without a mouse
bool FreeCamSetPosition(Vec3 pos) {
    if (!g_fcOn || !g_fcInit || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
    g_fcPos[0] = pos.x; g_fcPos[1] = pos.y; g_fcPos[2] = pos.z; return true;
}
bool FreeCamMove(float forward, float right, float up) {
    if (!g_fcOn || !g_fcInit || !std::isfinite(forward) || !std::isfinite(right) || !std::isfinite(up)) return false;
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    for (int i = 0; i < 3; i++) g_fcPos[i] += f[i] * forward + r[i] * right + u[i] * up;
    return true;
}
bool FreeCamPose(Vec3* pos, Vec3* fwd) {
    if (!g_fcOn || !g_fcInit) return false;
    float r[3], u[3], f[3]; FcBasis(r, u, f);
    if (pos) *pos = { g_fcPos[0], g_fcPos[1], g_fcPos[2] }; if (fwd) *fwd = { f[0], f[1], f[2] };
    return true;
}

// ---- NPC and creature spawn: the game's own "spawn character" cheat request, executed on the server thread ----
// TrocTrSpawnCharacterCheatReq::execute(handler, int* result, packet) (vtable slot 2): packet {+0 sender = the player's server
// actor (ServerChildOnlyInGameActor), +0x10 u16 total length, +0x18 u8* buffer}; buffer = 5 header bytes (u16 payload length
// at +3) + payload {u32 characterKey (characterinfo row), u32 (read, not passed on), float3 position, u8 spawn type}; all
// payload bytes must be consumed. The type becomes the actor desc's reason byte: 0 faults deep in the actor creation, 1, 12,
// 13, 39, 40 all spawn. The worker asks the sender for the player actor, so the sender is taken from the requests the client
// really sends while walking (MoveActorReq, EchoMoveSessionIDReq) and re-taken whenever it changes (save loaded, respawn).
// If the handler's byte +0x21 is set it answers ok and does nothing.
static uintptr_t g_npcHandler = 0; static void* g_npcExecute = nullptr;
static uintptr_t g_npcAiHandler = 0; static void* g_npcAiExecute = nullptr;
static uintptr_t g_npcAiTerminate = 0;   // AIFunction_TerminateAi target: (actor, terminate)
static uintptr_t g_actorRegistryGlobal = 0;   // address of the game's singleton-holder pointer, derived from AiControlChangeCheatReq
static SRWLOCK g_npcLock = SRWLOCK_INIT;
static uintptr_t g_serverSession = 0, g_sessionVt = 0; static uint8_t g_pktTemplate[0x40];   // under g_npcLock
static volatile uintptr_t g_lastSender = 0;   // fast path of the capture: same sender as last time, nothing to do
static void* g_capOrig[2] = {}; static const char* g_capName[2] = { "", "" };
static void QueuePendingManagedNpcs();
static void QueueManagedNpcAiReconcile();
template<int K> static void* __fastcall CapThunk(void* h, void* res, void* pkt, void* d, void* e, void* f, void* g, void* i) {
    uintptr_t s = 0;
    if (pkt && ReadPtr((uintptr_t)pkt, &s) && s && s != g_lastSender) {
        // a real client packet: u16 total at +0x10, buffer at +0x18 whose u16 at +3 is total - 5, sender with RTTI
        uintptr_t buf = 0, vt = 0; uint16_t total = 0, plen = 0; const char* sn = nullptr;
        if (ReadBytes((uintptr_t)pkt + 0x10, &total, 2) && ReadPtr((uintptr_t)pkt + 0x18, &buf) && buf && ReadBytes(buf + 3, &plen, 2) &&
            total >= 5 && plen == total - 5 && ReadPtr(s, &vt) && (sn = RttiName(s)) != nullptr) {
            AcquireSRWLockExclusive(&g_npcLock);
            ReadBytes((uintptr_t)pkt, g_pktTemplate, sizeof g_pktTemplate); g_serverSession = s; g_sessionVt = vt;
            ReleaseSRWLockExclusive(&g_npcLock);
            g_lastSender = s;
            Log("[npc] player actor %p (%s) from %s", (void*)s, sn, g_capName[K]);
            QueuePendingManagedNpcs();
            QueueManagedNpcAiReconcile();
        }
    }
    return ((void* (__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*))g_capOrig[K])(h, res, pkt, d, e, f, g, i);
}
static uintptr_t FindObjectWithVtable(uintptr_t vt, int* count) {   // static handler objects live in the image's writable data
    auto dos = (PIMAGE_DOS_HEADER)g_base; auto nt = (PIMAGE_NT_HEADERS)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t first = 0; *count = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 8 <= n; k += 8) { uintptr_t v; memcpy(&v, p + k, 8); if (v == vt) { if (!first) first = (uintptr_t)(p + k); (*count)++; } }
    }
    return first;
}
uintptr_t StaticObjectWithVtable(uintptr_t vt, int* count) { return FindObjectWithVtable(vt, count); }
static uintptr_t RelCallTarget(uintptr_t p) {
    uint8_t op = 0; int32_t rel = 0;
    if (!ReadBytes(p, &op, 1) || op != 0xE8 || !ReadBytes(p + 1, &rel, 4)) return 0;
    const uintptr_t t = p + 5 + rel; return InImage(t) ? t : 0;
}
static void ResolveNpcAiControl() {
    // AIFunction_TerminateAi writes the NPC AI object's termination flag and notifies its current action.
    // The control-ownership cheat request below is a different operation and cannot confirm this flag.
    int termHits = 0;
    g_npcAiTerminate = FindPatternCount("48 89 5C 24 08 57 48 83 EC 30 0F B6 FA 48 8B 41 68 48 8B 98 90 01 00 00 48 8B 43 08", &termHits);
    if (g_npcAiTerminate && termHits == 1) Log("[npc] AI terminate helper rva 0x%llx", (unsigned long long)(g_npcAiTerminate - g_base));
    else { Log("[npc] AI terminate helper not resolved (%d matches)", termHits); g_npcAiTerminate = 0; }

    // AiControlChangeCheatReq is the game's persistent control-ownership toggle. It consumes one u32 actor id.
    const uintptr_t vt = FindVtableByName(".?AVTrocTrAiControlChangeCheatReq@pa@@");
    if (!vt) { Log("[npc] AI-control request vtable not found"); return; }
    uintptr_t exec = 0; ReadPtr(vt + 2 * 8, &exec); int n = 0; const uintptr_t handler = FindObjectWithVtable(vt, &n);
    if (!InImage(exec) || !handler) { Log("[npc] AI-control handler object not found (%d instances, execute %p)", n, (void*)exec); return; }
    // The request execute calls a helper which resolves the actor id. That helper loads the registry as:
    //   mov rcx,[rip+global]; mov rcx,[rcx]; call lookup
    // Do NOT require the manager/buckets to be initialized here: this resolver runs at plugin startup, before the player
    // necessarily enters a world. ManagedNpcActorId validates the live manager later, when an NPC actually needs the id.
    uintptr_t registryGlobal = 0;
    for (uintptr_t p = exec; p < exec + 0x220 && !registryGlobal; ++p) {
        const uintptr_t helper = RelCallTarget(p);
        if (!helper || !InImage(helper)) continue;
        for (uintptr_t q = helper; q < helper + 0x380; ++q) {
            uint8_t b[10] = {}; if (!ReadBytes(q, b, sizeof b)) break;
            if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x0D || b[7] != 0x48 || b[8] != 0x8B || b[9] != 0x09) continue;
            int32_t d = 0; memcpy(&d, b + 3, 4); const uintptr_t g = q + 7 + d;
            if (InImage(g)) { registryGlobal = g; break; }
        }
    }
    g_npcAiHandler = handler; g_npcAiExecute = (void*)exec; g_actorRegistryGlobal = registryGlobal;
    Log("[npc] control-ownership handler rva 0x%llx execute rva 0x%llx actor-registry %s",
        (unsigned long long)(handler - g_base), (unsigned long long)(exec - g_base),
        registryGlobal ? (std::string("rva 0x") + [&]() { char b[32]; snprintf(b, sizeof b, "%llx", (unsigned long long)(registryGlobal - g_base)); return std::string(b); }()).c_str() : "not resolved");
}
static void InstallNpcSpawn() {
    const uintptr_t vt = FindVtableByName(".?AVTrocTrSpawnCharacterCheatReq@pa@@"); if (!vt) { Log("[npc] spawn cheat handler vtable not found: NPC spawning off"); return; }
    uintptr_t exec = 0; ReadPtr(vt + 2 * 8, &exec); int n = 0; const uintptr_t handler = FindObjectWithVtable(vt, &n);
    if (!InImage(exec) || !handler) { Log("[npc] handler object not found (%d instances, execute %p): NPC spawning off", n, (void*)exec); return; }
    static void* thunks[2] = { (void*)&CapThunk<0>, (void*)&CapThunk<1> };
    static const char* reqs[2][2] = { { ".?AVTrocTrMoveActorReq@pa@@", "MoveActorReq" }, { ".?AVTrocTrEchoMoveSessionIDReq@pa@@", "EchoMoveSessionIDReq" } };
    int hooked = 0;
    for (int k = 0; k < 2; k++) {
        const uintptr_t v = FindVtableByName(reqs[k][0]); uintptr_t f = 0; if (!v || !ReadPtr(v + 2 * 8, &f) || !InImage(f)) { Log("[npc] %s not found", reqs[k][1]); continue; }
        g_capName[k] = reqs[k][1]; ReleaseHookPiece();
        if (MH_CreateHook((void*)f, thunks[k], &g_capOrig[k]) == MH_OK && MH_EnableHook((void*)f) == MH_OK) hooked++;
    }
    if (!hooked) { Log("[npc] no request to take the player actor from: NPC spawning off"); return; }
    g_npcHandler = handler; g_npcExecute = (void*)exec;
    ResolveNpcAiControl();
    Log("[npc] spawn handler at rva 0x%llx (%d instance(s)), execute rva 0x%llx, %d request hook(s)", (unsigned long long)(handler - g_base), n, (unsigned long long)(exec - g_base), hooked);
}
static void NpcUnwind(const CONTEXT* fault) {   // call chain of a fault via the unwind tables, for bug reports
    CONTEXT c = *fault; char line[700]; int k = 0;
    for (int i = 0; i < 16 && c.Rip && k < (int)sizeof line - 24; i++) {
        k += snprintf(line + k, sizeof line - k, InImage(c.Rip) ? " %llx" : " ?%llx", InImage(c.Rip) ? (unsigned long long)(c.Rip - g_base) : (unsigned long long)c.Rip);
        DWORD64 imageBase = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!rf) { uintptr_t ret = 0; if (!ReadPtr(c.Rsp, &ret)) break; c.Rip = ret; c.Rsp += 8; continue; }
        void* hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
    }
    Log("[npc] fault chain:%s", line);
}
static bool CallNpcExecute(void* pkt, int* res) {   // guarded: a wrong packet must not take the game down
    typedef void* (__fastcall* Exec)(void*, int*, void*);
    CDK_GUARD_BEGIN ((Exec)g_npcExecute)((void*)g_npcHandler, res, pkt); return true;
    CDK_GUARD_FAIL { EXCEPTION_POINTERS ep = cdk::GuardInfo(); LogFault(&ep); NpcUnwind(ep.ContextRecord); } return false;
    CDK_GUARD_END
}
static bool CallNpcAiExecute(void* pkt, int* res) {
    if (!g_npcAiExecute || !g_npcAiHandler) return false;
    typedef void* (__fastcall* Exec)(void*, int*, void*);
    CDK_GUARD_BEGIN ((Exec)g_npcAiExecute)((void*)g_npcAiHandler, res, pkt); return true;
    CDK_GUARD_FAIL { EXCEPTION_POINTERS ep = cdk::GuardInfo(); LogFault(&ep); NpcUnwind(ep.ContextRecord); } return false;
    CDK_GUARD_END
}
static uint32_t ManagedNpcActorId(uintptr_t actor) {
    if (!actor || !g_actorRegistryGlobal) return 0;
    uintptr_t holder = 0, manager = 0, bucketsPtr = 0, nodesPtr = 0; uint32_t bucketCount = 0;
    if (!ReadPtr(g_actorRegistryGlobal, &holder) || !holder || !ReadPtr(holder, &manager) || !manager ||
        !ReadBytes(manager + 0x98, &bucketCount, 4) || !ReadPtr(manager + 0xA8, &bucketsPtr) || !ReadPtr(manager + 0xB0, &nodesPtr) ||
        !bucketsPtr || !nodesPtr || bucketCount == 0 || bucketCount > (1u << 20)) return 0;
    for (uint32_t b = 0; b < bucketCount; ++b) {
        const uintptr_t bucket = bucketsPtr + (uintptr_t)b * 0x100; uint32_t count = 0;
        if (!ReadBytes(bucket, &count, 4) || count > 31) continue;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t key = 0, index = 0; if (!ReadBytes(bucket + 8 + j * 8, &key, 4) || !ReadBytes(bucket + 12 + j * 8, &index, 4)) continue;
            uintptr_t node = 0, foundActor = 0; uint32_t nodeKey = 0;
            if (!ReadPtr(nodesPtr + (uintptr_t)index * 8, &node) || !node || !ReadBytes(node + 4, &nodeKey, 4) || !ReadPtr(node + 8, &foundActor)) continue;
            if (foundActor == actor) {
                const uint32_t low = nodeKey ? nodeKey : key;
                // ServerNormalInGameActor keeps the complete entity handle next to the low registry key.
                // The hash table indexes only the low 20 bits, but native requests carry the full handle.
                uint32_t full58 = 0, full60 = 0;
                ReadBytes(actor + 0x58, &full58, 4); ReadBytes(actor + 0x60, &full60, 4);
                if (full58 && (full58 & 0x000FFFFFu) == (low & 0x000FFFFFu)) return full58;
                if (full60 && (full60 & 0x000FFFFFu) == (low & 0x000FFFFFu)) return full60;
                return low;
            }
        }
    }
    return 0;
}
bool NpcAiControlAvailable() {
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.toggleAi) return true;
#endif
    return g_npcAiTerminate != 0;
}
static bool ReadNpcAiTerminated(uintptr_t actor, bool* terminated) {
    uintptr_t components = 0, ai = 0; uint8_t value = 0;
    if (!actor || !terminated || !ReadPtr(actor + 0x68, &components) || !components ||
        !ReadPtr(components + 0x190, &ai) || !ai || !ReadBytes(ai + 0x28, &value, 1)) return false;
    *terminated = value != 0;
    return true;
}
static bool SetNpcAiTerminatedNow(uintptr_t actor, bool terminated) {
    if (!actor || !g_npcAiTerminate) return false;
    bool current = false;
    if (!ReadNpcAiTerminated(actor, &current)) {
        Log("[npc] AI control unavailable for actor %p: AI state unreadable", (void*)actor);
        return false;
    }
    if (current == terminated) return true;
    typedef void (__fastcall* Fn)(void*, bool);
    CDK_GUARD_BEGIN
        ((Fn)g_npcAiTerminate)((void*)actor, terminated);
        bool applied = false;
        if (!ReadNpcAiTerminated(actor, &applied) || applied != terminated) {
            Log("[npc] AI %s actor %p but termination flag was not confirmed", terminated ? "terminated" : "resumed", (void*)actor);
            return false;
        }
        Log("[npc] AI %s actor %p; termination flag confirmed", terminated ? "terminated" : "resumed", (void*)actor);
        return true;
    CDK_GUARD_FAIL {
        EXCEPTION_POINTERS ep = cdk::GuardInfo(); LogFault(&ep); NpcUnwind(ep.ContextRecord);
        Log("[npc] AI terminate helper faulted for actor %p", (void*)actor);
    }
    CDK_GUARD_END
    return false;
}
static bool ToggleNpcAiControlNow(uint32_t actorId) {
    if (!actorId || !NpcAiControlAvailable()) return false;
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.toggleAi) return g_managedNpcNativeTest.toggleAi(actorId);
#endif
    alignas(16) uint8_t pkt[0x40]; uintptr_t s = 0, vt = 0, cur = 0;
    AcquireSRWLockShared(&g_npcLock);
    memcpy(pkt, g_pktTemplate, sizeof pkt); s = g_serverSession; vt = g_sessionVt;
    ReleaseSRWLockShared(&g_npcLock);
    if (!s || !ReadPtr(s, &cur) || cur != vt) {
        Log("[npc] AI control: player session is stale; waiting for the next movement packet");
        g_lastSender = 0;
        return false;
    }
    uint8_t buf[5 + 4] = {}; const uint16_t plen = 4; memcpy(buf + 3, &plen, 2); memcpy(buf + 5, &actorId, 4);
    const uint16_t total = sizeof buf; memcpy(pkt + 0x10, &total, 2); uint8_t* bp = buf; memcpy(pkt + 0x18, &bp, 8);
    int res = -1; const bool ok = CallNpcAiExecute(pkt, &res);
    Log("[npc] AI control toggle id 0x%08x: %s, result %d (%s)", actorId, ok ? "executed" : "FAULTED", res, DecodeErr((uint32_t)res).c_str());
    return ok && res == 0;
}
static bool SetManagedNpcRuntimeAiNow(int uid, bool enabled, uint64_t expectedGen = 0) {
    uintptr_t actor = 0; uint32_t actorId = 0; bool desired = true; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0) return false;
        const ManagedNpc& n = g_npcReg[i];
        if (n.hidden || !n.actor || (expectedGen && n.gen != expectedGen)) return false;
#ifdef WB_UNIFIED_HOST_TEST
        if (n.aiApplied == enabled) return true;
#endif
        actor = n.actor; actorId = n.actorId; desired = enabled; gen = n.gen;
    }
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.toggleAi) {
        if (!actorId) actorId = ManagedNpcActorId(actor);
        if (!actorId || !ToggleNpcAiControlNow(actorId)) return false;
    } else
#endif
    if (!SetNpcAiTerminatedNow(actor, !desired)) return false;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0) return false;
        ManagedNpc& n = g_npcReg[i];
        if (n.hidden || n.actor != actor || n.gen != gen) return false;
        n.actorId = actorId;
        n.aiApplied = desired;
    }
    return true;
}
static void SyncManagedNpcAiNow(int uid) {
    bool desired = true;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0) return;
        const ManagedNpc& n = g_npcReg[i];
        if (n.hidden || !n.actor || n.editMoving || n.aiApplied == n.aiEnabled) return;
        desired = n.aiEnabled;
    }
    SetManagedNpcRuntimeAiNow(uid, desired);
}
static void ReconcileManagedNpcActors() {
    static DWORD s_last = 0, s_lastAiAudit = 0; const DWORD now = GetTickCount();
    if (now - s_last < 50) return;
    s_last = now;

    bool needCandidates = false;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        for (const auto& n : g_npcReg) if (!n.hidden && !n.actor && n.spawnRequestTick && now - n.spawnRequestTick < 30000) { needCandidates = true; break; }
        if (!g_pendingNpcCleanup.empty()) needCandidates = true;
    }
    RecentServerActor recent[4096]; unsigned recentN = 0;
    if (needCandidates) {
        AcquireSRWLockShared(&g_recentActorLock);
        for (const auto& r : g_recentActors) if (r.actor && now - r.tick < 30000)
            recent[recentN++] = r;
        ReleaseSRWLockShared(&g_recentActorLock);
    }
    struct Candidate { uintptr_t actor, transform; DWORD tick; Vec3 pos; };
    std::vector<Candidate> candidates; candidates.reserve(recentN);
    for (unsigned ri = 0; ri < recentN; ++ri) {
        const auto& r = recent[ri];
        const char* rn = RttiName(r.actor);
        if (!rn || !strstr(rn, "ServerNormalInGameActor@")) continue;
        const uintptr_t tf = FindActorTransform(r.actor); PosInfo p{};
        if (tf && ReadTransformPos(tf, &p, false)) candidates.push_back({ r.actor, tf, r.tick, p.world });
    }

    std::vector<int> aiSync;
    std::vector<std::pair<int, uintptr_t>> cleanup;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        // Reserve actors belonging to deleted requests before live records can claim nearby candidates.
        for (auto it = g_pendingNpcCleanup.begin(); it != g_pendingNpcCleanup.end();) {
            uintptr_t best = 0; float bestScore = 1e9f;
            for (const auto& r : candidates) {
                const int dt = (int)(r.tick - it->requestTick);
                if (dt < -100 || dt > 30000 || g_managedNpcActors.count(r.actor)) continue;
                const float dx = r.pos.x - it->pos.x, dy = r.pos.y - it->pos.y, dz = r.pos.z - it->pos.z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > 1.5f) continue;
                const float score = d + std::max(0, dt) * 0.0005f;
                if (score < bestScore) { bestScore = score; best = r.actor; }
            }
            if (best) {
                g_managedNpcActors.insert(best);
                cleanup.push_back({ it->uid, best });
                it = g_pendingNpcCleanup.erase(it);
            } else if (now - it->requestTick >= 30000) {
                Log("[npc] deleted managed #%d never resolved its async actor within 30 s", it->uid);
                it = g_pendingNpcCleanup.erase(it);
            } else ++it;
        }
        for (auto& n : g_npcReg) {
            if (n.hidden || n.actor || !n.spawnRequestTick) continue;
            if (now - n.spawnRequestTick >= 30000 && !n.bindTimeoutLogged) {
                Log("[npc] managed #%d actor not bound after 30 s; spawn request retained to prevent a duplicate", n.uid);
                n.bindTimeoutLogged = true;
            }
            if (now - n.spawnRequestTick >= 30000) continue;
            uintptr_t best = 0, bestTf = 0; float bestScore = 1e9f; Vec3 bestPos{};
            for (const auto& r : candidates) {
                const int dt = (int)(r.tick - n.spawnRequestTick);
                if (dt < -100 || dt > 30000 || g_managedNpcActors.count(r.actor)) continue;
                const float dx = r.pos.x - n.pos.x, dy = r.pos.y - n.pos.y, dz = r.pos.z - n.pos.z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > 30.0f) continue;
                const float score = d + std::max(0, dt) * 0.0005f;
                if (score < bestScore) { bestScore = score; best = r.actor; bestTf = r.transform; bestPos = r.pos; }
            }
            if (!best) continue;
            n.actor = best; n.transform = bestTf; n.actorId = ManagedNpcActorId(best);
            n.spawnPending = false; n.spawnRequestTick = 0; n.bindTimeoutLogged = false;
            n.editMoving = false; n.liveMovePending = false; n.aiApplied = true; g_managedNpcActors.insert(best);
            Log("[npc] managed #%d late-bound actor %p (%s) at (%.2f %.2f %.2f), spawn (%.2f %.2f %.2f)",
                n.uid, (void*)best, RttiName(best) ? RttiName(best) : "?", bestPos.x, bestPos.y, bestPos.z, n.pos.x, n.pos.y, n.pos.z);
            if (n.aiApplied != n.aiEnabled) aiSync.push_back(n.uid);
        }
#ifndef WB_UNIFIED_HOST_TEST
        if (now - s_lastAiAudit >= 1000) {
            s_lastAiAudit = now;
            for (auto& n : g_npcReg) {
                if (n.hidden || !n.actor || n.editMoving) continue;
                bool terminated = false;
                if (!ReadNpcAiTerminated(n.actor, &terminated)) continue;
                const bool applied = !terminated;
                if (n.aiApplied != applied) {
                    Log("[npc] managed #%d AI state changed outside editor: enabled=%d, desired=%d", n.uid, applied ? 1 : 0, n.aiEnabled ? 1 : 0);
                    n.aiApplied = applied;
                    if (applied != n.aiEnabled) aiSync.push_back(n.uid);
                }
            }
        }
#endif
    }
    for (int uid : aiSync) RunOnServerTick([uid]() { SyncManagedNpcAiNow(uid); });
    for (const auto& item : cleanup) RunOnServerTick([uid = item.first, actor = item.second]() {
        Log("[npc] removing late actor %p for deleted managed #%d", (void*)actor, uid);
        if (!RemoveSpawnedActor(actor)) Log("[npc] late actor removal failed for deleted managed #%d", uid);
    });
}
static void QueueManagedNpcAiReconcile() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        for (const auto& n : g_npcReg) if (!n.hidden && n.actor && !n.editMoving && n.aiApplied != n.aiEnabled) ids.push_back(n.uid);
    }
    for (int uid : ids) RunOnServerTick([uid]() { SyncManagedNpcAiNow(uid); });
}
int NpcState() {
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.state) return g_managedNpcNativeTest.state();
#endif
    if (!g_npcExecute) return 0;
    AcquireSRWLockShared(&g_npcLock); const bool have = g_serverSession != 0; ReleaseSRWLockShared(&g_npcLock);
    return have ? 2 : 1;
}
static uintptr_t PickNpcSpawnActor(Vec3 expected, uintptr_t innerActor) {
    uintptr_t candidates[9] = {}; int count = 0;
    if (innerActor) candidates[count++] = innerActor;
    for (int i = 0; i < g_npcActorCandidateCount && count < (int)(sizeof candidates / sizeof candidates[0]); ++i) {
        const uintptr_t a = g_npcActorCandidates[i]; bool dup = false;
        for (int j = 0; j < count; ++j) if (candidates[j] == a) { dup = true; break; }
        if (!dup) candidates[count++] = a;
    }
    uintptr_t best = 0; float bestScore = -1e9f;
    for (int i = 0; i < count; ++i) {
        const uintptr_t a = candidates[i]; if (!a) continue;
        const char* rn = RttiName(a); float score = 0;
        if (rn && strstr(rn, "ServerNormalInGameActor@")) score += 500.0f;
        else if (rn && strstr(rn, "Actor@")) score += 100.0f;
        const uintptr_t tf = FindActorTransform(a); PosInfo p{};
        if (tf && ReadTransformPos(tf, &p, false)) {
            const float dx = p.world.x - expected.x, dy = p.world.y - expected.y, dz = p.world.z - expected.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < 50.0f) score += 500.0f - d * 5.0f; else score -= std::min(d, 500.0f);
        }
        if (score > bestScore) { bestScore = score; best = a; }
    }
    if (best) {
        const char* rn = RttiName(best); const uintptr_t tf = FindActorTransform(best); PosInfo p{};
        if (tf && ReadTransformPos(tf, &p, false))
            Log("[npc] bound actor %p (%s), live pos (%.2f %.2f %.2f), %d ctor candidate%s",
                (void*)best, rn ? rn : "?", p.world.x, p.world.y, p.world.z, count, count == 1 ? "" : "s");
        else
            Log("[npc] bound actor %p (%s), transform unresolved, %d ctor candidate%s",
                (void*)best, rn ? rn : "?", count, count == 1 ? "" : "s");
    }
    return best;
}
static bool SpawnNpcNow(uint32_t key, Vec3 pos, int type, uint32_t extra, uintptr_t* actorOut = nullptr, uint32_t* actorIdOut = nullptr) {
    if (actorOut) *actorOut = 0; if (actorIdOut) *actorIdOut = 0;
#ifdef WB_UNIFIED_HOST_TEST
    if (g_managedNpcNativeTest.spawn) return g_managedNpcNativeTest.spawn(key, pos, type, extra, actorOut, actorIdOut);
#endif
    if (type < 1 || type > 255) type = 1;   // 0 is no valid reason and faults inside the game
    alignas(16) uint8_t pkt[0x40]; uintptr_t s = 0, vt = 0, cur = 0;
    AcquireSRWLockShared(&g_npcLock); memcpy(pkt, g_pktTemplate, sizeof pkt); s = g_serverSession; vt = g_sessionVt; ReleaseSRWLockShared(&g_npcLock);
    if (!s || !ReadPtr(s, &cur) || cur != vt) {   // the actor was replaced (save loaded) and no move has been seen since
        Log("[npc] player actor %p is gone: walk a few steps, then spawn again", (void*)s); g_lastSender = 0;
        AcquireSRWLockExclusive(&g_npcLock); if (g_serverSession == s) g_serverSession = 0; ReleaseSRWLockExclusive(&g_npcLock);
        return false;
    }
    uint8_t buf[5 + 21] = {}; const uint16_t plen = 21; memcpy(buf + 3, &plen, 2);
    memcpy(buf + 5, &key, 4); memcpy(buf + 9, &extra, 4); memcpy(buf + 13, &pos, 12); buf[25] = (uint8_t)type;
    const uint16_t total = sizeof buf; memcpy(pkt + 0x10, &total, 2); uint8_t* bp = buf; memcpy(pkt + 0x18, &bp, 8);
    g_lastActorCreated_ = 0; g_npcActorCandidateCount = 0; memset(g_npcActorCandidates, 0, sizeof g_npcActorCandidates);
    g_npcActorCaptureThread = GetCurrentThreadId(); InterlockedExchange(&g_npcActorCapture, 1);
    int res = -1; const bool ok = CallNpcExecute(pkt, &res);
    InterlockedExchange(&g_npcActorCapture, 0);
    const uintptr_t actor = PickNpcSpawnActor(pos, g_lastActorCreated_);
    const uint32_t actorId = actor ? ManagedNpcActorId(actor) : 0;
    if (actorOut) *actorOut = actor; if (actorIdOut) *actorIdOut = actorId;
    Log("[npc] spawn character %u at (%.2f %.2f %.2f) type %d: %s, result %d (%s), actor %p id 0x%08x",
        key, pos.x, pos.y, pos.z, type, ok ? "executed" : "FAULTED", res, DecodeErr((uint32_t)res).c_str(), (void*)actor, actorId);
    return ok && res == 0;
}
// Research: where the player's SERVER actor keeps its position. Takes the client snapshot's tile-local x/y/z (and the world
// position) and searches the server actor, every object it points to and every object those point to (0x800 bytes each) for
// three consecutive floats within 0.5 m; hits are logged with their pointer path and RTTI.
void ResearchFindPos() {
    uintptr_t s = 0; AcquireSRWLockShared(&g_npcLock); s = g_serverSession; ReleaseSRWLockShared(&g_npcLock);
    const uintptr_t cl = PlayerActor(); PosInfo p{};
    if (!s || !cl || !ReadPos(cl, &p)) { Log("[findpos] no server actor yet (walk a few steps) or no player"); return; }
    uintptr_t comps = Deref(cl, kOff_Ent_Comps), tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    float loc[3] = {}; if (!tf || !ReadBytes(tf + kOff_Tf_Pos, loc, 12)) return;
    const float want[2][3] = { { loc[0], loc[1], loc[2] }, { p.world.x, p.world.y, p.world.z } };
    Log("[findpos] server actor %p (%s); client local (%.2f %.2f %.2f) world (%.2f %.2f %.2f)", (void*)s, RttiName(s) ? RttiName(s) : "?", loc[0], loc[1], loc[2], p.world.x, p.world.y, p.world.z);
    int hits = 0;
    auto scan = [&](uintptr_t obj, const char* path) {
        uint8_t b[0x800]; if (!ReadBytes(obj, b, sizeof b)) return;
        for (unsigned o = 0; o + 12 <= sizeof b && hits < 80; o += 4) {
            float f[3]; memcpy(f, b + o, 12);
            for (int w = 0; w < 2; w++)
                if (fabsf(f[0] - want[w][0]) < 0.5f && fabsf(f[1] - want[w][1]) < 0.5f && fabsf(f[2] - want[w][2]) < 0.5f) {
                    Log("[findpos] %s %p (%s) +0x%X = %s (%.3f %.3f %.3f) -> field %p", path, (void*)obj, RttiName(obj) ? RttiName(obj) : "-", o, w ? "world" : "local", f[0], f[1], f[2], (void*)(obj + o)); hits++;
                }
        }
    };
    auto heapPtr = [](uintptr_t q) { return q > 0x10000 && !(q >> 47) && !InImage(q) && !(q & 7); };
    scan(s, "actor");
    uint8_t a1[0x800]; if (!ReadBytes(s, a1, sizeof a1)) return;
    for (unsigned i = 0; i < sizeof a1; i += 8) {
        uintptr_t q; memcpy(&q, a1 + i, 8); if (!heapPtr(q)) continue;
        char path[64]; snprintf(path, sizeof path, "actor+%X", i); scan(q, path);
        uint8_t a2[0x400]; if (!ReadBytes(q, a2, sizeof a2)) continue;
        for (unsigned k = 0; k < sizeof a2; k += 8) {
            uintptr_t r; memcpy(&r, a2 + k, 8); if (!heapPtr(r) || r == s) continue;
            char p2[64]; snprintf(p2, sizeof p2, "actor+%X+%X", i, k); scan(r, p2);
        }
    }
    Log("[findpos] %d hits", hits);
    if (comps) {   // the client player's components (the character control component is the next research target)
        for (unsigned i = 0; i < 0x400; i += 8) {
            uintptr_t c = 0; if (!ReadPtr(comps + i, &c) || !c || InImage(c)) continue;
            if (const char* rn = RttiName(c)) if (strstr(rn, "Component")) Log("[findpos] client component list+0x%X %p %s", i, (void*)c, rn);
        }
    }

}
bool SpawnNpc(uint32_t key, Vec3 pos, int type, uint32_t extra) {
    if (!g_npcExecute) { Log("[npc] not available (handler not resolved)"); return false; }
    if (NpcState() < 2) { Log("[npc] player actor not known yet: walk a few steps"); return false; }
    RunOnServerTick([key, pos, type, extra]() { SpawnNpcNow(key, pos, type, extra); });
    return true;
}

static void SpawnManagedNpcNow(int uid, uint64_t gen) {
    ManagedNpc n;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (!NpcGenCurrentLocked(uid, gen)) return;
        g_npcReg[i].spawnRequestTick = GetTickCount(); g_npcReg[i].bindTimeoutLogged = false;
        n = g_npcReg[i];
    }
    auto nativeWork = TrackProjectNativeWork(n.proj);
    { REG_LOCK; if (!NpcGenCurrentLocked(uid, gen)) return; }
    uintptr_t actor = 0; uint32_t actorId = 0;
    if (!SpawnNpcNow(n.key, n.pos, n.type, n.extra, &actor, &actorId)) {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (NpcGenCurrentLocked(uid, gen)) { g_npcReg[i].spawnPending = false; g_npcReg[i].spawnRequestTick = 0; }
        return;
    }
    bool discard = false, needAiSync = false; uintptr_t staleBoundActor = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (actor) g_managedNpcActors.insert(actor);
        if (!NpcGenCurrentLocked(uid, gen)) discard = true;
        else {
            ManagedNpc& live = g_npcReg[i];
            if (actor || !live.actor) {
                if (actor && live.actor && live.actor != actor) staleBoundActor = live.actor;
                live.actor = actor; live.actorId = actorId; live.transform = actor ? FindActorTransform(actor) : 0;
                live.spawnPending = actor == 0;   // asynchronous SpawnCharacter creation is reconciled from recent ServerActor constructors
                live.editMoving = false; live.liveMovePending = false; live.aiApplied = true;
            } else {
                // UI reconciliation may bind an asynchronous actor before the native spawn request returns.
                // A zero synchronous capture must not erase that live binding.
                live.spawnPending = false;
            }
            needAiSync = live.aiApplied != live.aiEnabled;
        }
    }
    if (discard) { if (actor) RemoveSpawnedActor(actor); return; }
    if (staleBoundActor) {
        Log("[npc] managed #%d replaced early actor binding %p with synchronous actor %p", uid, (void*)staleBoundActor, (void*)actor);
        RemoveSpawnedActor(staleBoundActor);
    }
    if (needAiSync) {
        SyncManagedNpcAiNow(uid);
        bool stillPending = false; uintptr_t curActor = 0; uint32_t curActorId = 0;
        {
            std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
            if (i >= 0) { const auto& cur = g_npcReg[i]; stillPending = cur.aiApplied != cur.aiEnabled; curActor = cur.actor; curActorId = cur.actorId; }
        }
        if (stillPending) {
            Log("[npc] managed #%d AI desired=%d still not applied (actor %p id 0x%08x); will retry on AI reconciliation",
                uid, n.aiEnabled ? 1 : 0, (void*)curActor, curActorId);
        }
    }
}
static bool QueueManagedNpcIfReady(int uid) {
    if (NpcState() < 2) return false;
    bool queue = false; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i >= 0) {
            ManagedNpc& n = g_npcReg[i];
            if (!n.hidden && !n.actor && !n.spawnPending && !n.spawnRequestTick) { n.spawnPending = true; gen = n.gen; queue = true; }
        }
    }
    if (queue) RunOnServerTick([uid, gen]() { SpawnManagedNpcNow(uid, gen); });
    return queue;
}
int SpawnManagedNpc(uint32_t key, Vec3 pos, int type, uint32_t extra, bool aiEnabled, int behavior, int group, int proj, const std::string& label, const std::string& note) {
    if (NpcState() == 0 || !key) return 0;
    if (type < 1 || type > 255) type = 1; behavior = behavior == 1 ? 1 : 0; if (behavior == 1) aiEnabled = false;
    int uid = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); uid = g_nextNpcUid++;
        ManagedNpc n; n.uid = uid; n.gen = NewGenLocked(); n.key = key; n.pos = pos; n.type = type; n.extra = extra; n.aiEnabled = aiEnabled; n.behavior = behavior; n.tick = GetTickCount(); n.group = group; n.proj = proj; n.label = label; n.note = note;
        g_npcReg.push_back(std::move(n)); if (!g_loading) MarkDirtyLocked(proj);
    }
    QueueManagedNpcIfReady(uid);
    return uid;
}
static void QueuePendingManagedNpcs() {
    std::vector<std::pair<int, uint64_t>> pending;
    {
        std::lock_guard<std::mutex> l(g_regMutex);
        for (auto& n : g_npcReg) if (!n.hidden && !n.actor && !n.spawnPending && !n.spawnRequestTick) { n.spawnPending = true; pending.push_back({ n.uid, n.gen }); }
    }
    for (const auto& p : pending) RunOnServerTick([p]() { SpawnManagedNpcNow(p.first, p.second); });
}
bool HideManagedNpc(int uid) {
    uintptr_t actor = 0;
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (i < 0 || g_npcReg[i].hidden) return false;
      ManagedNpc& n = g_npcReg[i]; RememberUnboundNpcCleanupLocked(n); n.hidden = true; n.gen = NewGenLocked(); n.spawnPending = false; actor = n.actor; n.actor = 0; n.actorId = 0; n.transform = 0; n.spawnRequestTick = 0; n.editMoving = false; n.liveMovePending = false; n.aiApplied = true; MarkDirtyLocked(n.proj); }
    if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
    return true;
}
bool RestoreManagedNpc(int uid) {
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (i < 0 || !g_npcReg[i].hidden) return false;
      g_npcReg[i].hidden = false; g_npcReg[i].gen = NewGenLocked(); g_npcReg[i].spawnPending = false; MarkDirtyLocked(g_npcReg[i].proj); }
    QueueManagedNpcIfReady(uid); return true;
}
static bool WriteManagedNpcTransformNow(uintptr_t tf, Vec3 world) {
    if (!tf) return false;
    uint8_t snap[0x48] = {};
    if (!ReadBytes(tf + kOff_Tf_Pos, snap, sizeof snap)) return false;
    float parent[3] = {}; int16_t parentSector[2] = {};
    memcpy(parent, snap + 0x38, sizeof parent); memcpy(parentSector, snap + 0x44, sizeof parentSector);
    // Do not tear an attached/mounted actor away from its parent with a plain world-space write.
    if (parentSector[0] || parentSector[1] || fabsf(parent[0]) > 0.001f || fabsf(parent[1]) > 0.001f || fabsf(parent[2]) > 0.001f)
        return false;
    const int tx = (int)(world.x * 0.001f), tz = (int)(world.z * 0.001f);
    struct PosTile { float x, y, z; int16_t tileX, tileZ; } packed{
        world.x - tx * kTileSize, world.y, world.z - tz * kTileSize, (int16_t)tx, (int16_t)tz
    };
    static_assert(sizeof(PosTile) == 16, "TransformSync position/tile layout changed");
    return WriteBytes(tf + kOff_Tf_Pos, &packed, sizeof packed);
}
bool BeginManagedNpcMove(int uid) {
    bool queuePause = false; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0) return false; ManagedNpc& n = g_npcReg[i];
        if (n.hidden || !n.actor || !n.transform || n.editMoving) { Log("[npc] managed #%d live move not ready (hidden=%d actor=%p transform=%p moving=%d)", uid, n.hidden, (void*)n.actor, (void*)n.transform, n.editMoving); return false; }
        uint8_t snap[0x48] = {}; if (!ReadBytes(n.transform + kOff_Tf_Pos, snap, sizeof snap)) { Log("[npc] managed #%d live move: TransformSync snapshot unreadable", uid); return false; }
        float parent[3] = {}; int16_t psec[2] = {}; memcpy(parent, snap + 0x38, 12); memcpy(psec, snap + 0x44, 4);
        if (psec[0] || psec[1] || fabsf(parent[0]) > 0.001f || fabsf(parent[1]) > 0.001f || fabsf(parent[2]) > 0.001f) { Log("[npc] managed #%d live move refused: actor is parented/attached", uid); return false; }
        // An NPC whose runtime AI is already off needs no native control request just to move. If AI is still active,
        // however, the edit transaction must be able to take control before queued transform writes begin.
        queuePause = true;
        if (queuePause) {
            if (!NpcAiControlAvailable()) { Log("[npc] managed #%d live move: AI control is not available to pause the actor", uid); return false; }
        }
        n.editMoving = true; n.liveMoveTarget = n.pos; n.liveMovePending = false; gen = n.gen;
    }
    if (queuePause) RunOnServerTick([uid, gen]() { SetManagedNpcRuntimeAiNow(uid, false, gen); });
    return true;
}
static void QueueManagedNpcLiveWrite(int uid, uintptr_t tf, uint64_t gen) {
    RunOnServerTick([uid, tf, gen]() {
        Vec3 world{}; int proj = 0;
        {
            std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
            if (!NpcGenCurrentLocked(uid, gen)) return; ManagedNpc& n = g_npcReg[i];
            if (n.transform == tf) n.liveMovePending = false;
            if (!n.editMoving || n.transform != tf) return;
            world = n.liveMoveTarget; proj = n.proj;
        }
        auto work = TrackProjectNativeWork(proj);
        if (!WriteManagedNpcTransformNow(tf, world))
            Log("[npc] managed #%d live move could not write TransformSync", uid);
    });
}
bool MoveManagedNpcLive(int uid, Vec3 world) {
    uintptr_t tf = 0; bool queue = false; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0 || !g_npcReg[i].editMoving || g_npcReg[i].hidden) return false;
        ManagedNpc& n = g_npcReg[i]; tf = n.transform; n.liveMoveTarget = world; gen = n.gen;
        if (!n.liveMovePending) { n.liveMovePending = true; queue = true; }
    }
    if (queue) QueueManagedNpcLiveWrite(uid, tf, gen);
    return true;
}
bool CommitManagedNpcMove(int uid, Vec3 world) {
    uintptr_t tf = 0; bool queue = false; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0 || !g_npcReg[i].editMoving || g_npcReg[i].hidden) return false;
        ManagedNpc& n = g_npcReg[i]; n.pos = world; n.liveMoveTarget = world; tf = n.transform; gen = n.gen; MarkDirtyLocked(n.proj);
        if (!n.liveMovePending) { n.liveMovePending = true; queue = true; }
    }
    if (queue) QueueManagedNpcLiveWrite(uid, tf, gen);
    return true;
}
bool EndManagedNpcMove(int uid, Vec3 world) {
    uintptr_t tf = 0; bool moving = false; uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0 || g_npcReg[i].hidden) return false; ManagedNpc& n = g_npcReg[i];
        moving = n.editMoving; tf = n.transform; gen = n.gen;
    }
    if (!moving || !tf) return MoveManagedNpc(uid, world);
    {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
        if (i < 0 || g_npcReg[i].hidden) return false;
        g_npcReg[i].pos = world; g_npcReg[i].liveMoveTarget = world; MarkDirtyLocked(g_npcReg[i].proj);
    }
    RunOnServerTick([uid, tf, world, gen]() {
        int proj = 0;
        {
            std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
            if (!NpcGenCurrentLocked(uid, gen) || !g_npcReg[i].editMoving || g_npcReg[i].transform != tf) return;
            proj = g_npcReg[i].proj;
        }
        auto work = TrackProjectNativeWork(proj);
        const bool moved = WriteManagedNpcTransformNow(tf, world);
        bool desired = true; uintptr_t actor = 0; uint64_t nextGen = gen;
        {
            std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid);
            if (i < 0) return; ManagedNpc& n = g_npcReg[i];
            if (n.hidden || n.gen != gen || !n.editMoving || n.transform != tf) return;
            desired = n.aiEnabled; n.editMoving = false; n.liveMovePending = false;
            if (!moved) {
                actor = n.actor;
                n.actor = 0; n.actorId = 0; n.transform = 0; n.spawnRequestTick = 0; n.aiApplied = true;
                n.spawnPending = true; n.gen = nextGen = NewGenLocked();
            }
        }
        if (!moved) {
            Log("[npc] managed #%d final live move write failed; respawning at the committed position", uid);
            if (actor) RemoveSpawnedActor(actor);
            SpawnManagedNpcNow(uid, nextGen);
            return;
        }
        SetManagedNpcRuntimeAiNow(uid, desired, gen);
    });
    return true;
}
bool MoveManagedNpc(int uid, Vec3 world) {
    // Bound actors can move in place through their server TransformSync component. Use an edit transaction so autonomous
    // AI cannot immediately walk over the editor's position while the write is being committed.
    if (BeginManagedNpcMove(uid)) {
        MoveManagedNpcLive(uid, world);
        return EndManagedNpcMove(uid, world);
    }
    uintptr_t actor = 0; bool pending = false; uint64_t gen = 0; int proj = 0;
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (i < 0 || g_npcReg[i].hidden) return false;
      ManagedNpc& n = g_npcReg[i]; if (n.pos.x == world.x && n.pos.y == world.y && n.pos.z == world.z) return true;
      if (!n.actor && n.spawnRequestTick) { Log("[npc] managed #%d move deferred: async actor has not been bound", uid); return false; }
      pending = n.spawnPending && !n.spawnRequestTick; // a queued, not yet executing spawn can still read this latest pose
      if (!pending) n.gen = NewGenLocked();
      gen = n.gen; proj = n.proj; n.pos = world; actor = n.actor;
      n.actor = 0; n.actorId = 0; n.transform = 0; n.spawnRequestTick = 0; n.editMoving = false; n.liveMovePending = false; n.aiApplied = true;
      n.spawnPending = pending || actor != 0; MarkDirtyLocked(n.proj); }
    if (pending) return true;
    if (actor) { auto work = TrackProjectNativeWork(proj); RunOnServerTick([uid, actor, gen, work]() { RemoveSpawnedActor(actor); SpawnManagedNpcNow(uid, gen); }); }
    else QueueManagedNpcIfReady(uid);
    return true;
}
bool SetManagedNpcControl(int uid, bool enabled, int behavior) {
    behavior = behavior == 1 ? 1 : 0; bool needSync = false, changed = false;
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (i < 0 || g_npcReg[i].hidden) return false;
      ManagedNpc& n = g_npcReg[i]; changed = n.behavior != behavior || n.aiEnabled != enabled;
      n.behavior = behavior; n.aiEnabled = enabled; if (changed) MarkDirtyLocked(n.proj);
      needSync = n.actor && n.aiApplied != n.aiEnabled; }
    if (needSync) RunOnServerTick([uid]() { SyncManagedNpcAiNow(uid); });
    return true;
}
bool SetManagedNpcAi(int uid, bool enabled) {
    int behavior = 0;
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = NpcIndexOfUidLocked(uid); if (i < 0 || g_npcReg[i].hidden) return false;
      behavior = g_npcReg[i].behavior; if (enabled && behavior == 1) behavior = 0; }
    return SetManagedNpcControl(uid, enabled, behavior);
}
bool SetManagedNpcBehavior(int uid, int behavior) {
    behavior = behavior == 1 ? 1 : 0;
    return SetManagedNpcControl(uid, behavior == 0, behavior);
}

// SceneObjectServer objects are born in the reflection factory's create() (the entry of the class meta table that follows the
// "... for Server" strings); hooked to log who creates one (caller rva) and to put it on the watch list at birth
static uintptr_t kRva_SoServerCreate = 0; volatile LONG g_soCreated_ = 0; volatile uintptr_t g_lastSoCreated_ = 0; volatile uintptr_t g_lastActorCreated_ = 0;
#define g_soCreated g_soCreated_
typedef void* (__fastcall* SoServerCreateFn)();
static SoServerCreateFn g_origSoServerCreate = nullptr;
static void* __fastcall HookSoServerCreate() {
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    void* r = g_origSoServerCreate();
    InterlockedIncrement(&g_soCreated); g_lastSoCreated_ = (uintptr_t)r;
    if (g_trace) {
        WatchObject((uintptr_t)r);
        void* frames[16] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
        char chain[400]; int k = 0; for (USHORT i = 0; i < n && k < (int)sizeof chain - 16; i++) { const uintptr_t f = (uintptr_t)frames[i]; k += snprintf(chain + k, sizeof chain - k, " %llx", InImage(f) ? (unsigned long long)(f - g_base) : 0ull); }
        Log("[vt] SceneObjectServer created %p by rva 0x%llx (thread %lu) chain:%s", r, InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(), chain);
    }
    return r;
}
static void ResolveSoServerCreate() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 89 5C 24 08 57 48 83 EC 20 48 8D 3D 9F 8E 53 04 48 89 7C 24 38 48 8B 05 93 8E 53 04 48 8B CF FF 50 08 90 48 8D 0D", &n);   // research only: the displacements make it build-specific, no match = not hooked
    if (!f || n != 1) { Log("[vt] SceneObjectServer create: %d matches, not hooked", n); return; }
    kRva_SoServerCreate = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "SceneObjectServer new", (unsigned long long)kRva_SoServerCreate);
}

// ---- scene object lookup by UUID (research) --------------------------------------------------------------------------
// The field create ends by looking the new actor's scene object up by its 16-byte UUID (function 0x282e630 -> this lookup);
// a miss is eErrNoInvalidSceneObjectUUID. Logged (key, hit) for the game's own spawns and for the replay, to see which UUID is
// asked for and whether the registry knows it.
static uintptr_t kRva_UuidLookup = 0;
typedef void* (__fastcall* UuidLookupFn)(void* registry, void* out, const void* uuid);
static UuidLookupFn g_origUuidLookup = nullptr;
static void* __fastcall HookUuidLookup(void* registry, void* out, const void* uuid) {
    void* r = g_origUuidLookup(registry, out, uuid);
    if (g_trace || g_inGimmickReplay) {
        uint32_t u[4] = { 0, 0, 0, 0 }; ReadBytes((uintptr_t)uuid, u, 16);
        uintptr_t hit = 0; uint8_t flag = 0; ReadBytes((uintptr_t)out, &hit, 8); ReadBytes((uintptr_t)out + 0x10, &flag, 1);
        Log("[uuid] lookup%s in %p (%s): %08x %08x %08x %08x -> %p flag %d (%s)", g_inGimmickReplay ? " (REPLAY)" : "", registry, RttiName((uintptr_t)registry) ? RttiName((uintptr_t)registry) : "?", u[0], u[1], u[2], u[3], (void*)hit, flag, hit && RttiName(hit) ? RttiName(hit) : "-");
    }
    return r;
}
static void ResolveUuidLookup() {
    int n = 0;
    const uintptr_t f = FindPatternCount("48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56 48 83 EC 30 49 8B E8 48 8B FA 48 8B F1 45 33 C9 44 89 4C 24 20", &n);
    if (!f || n != 1) { Log("[uuid] lookup function: %d matches, not hooked", n); return; }
    kRva_UuidLookup = f - g_base; Log("resolved %-20s rva 0x%llx (via signature)", "uuid lookup", (unsigned long long)kRva_UuidLookup);
}
static void ResolveSpawnCallers() {
    if (!kRva_GimmickSpawn_) return;
    const uintptr_t target = g_base + kRva_GimmickSpawn_; int found = 0;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    auto matches = [](const uint8_t* at, const char* pat) { auto p = ParsePattern(pat); for (size_t i = 0; i < p.size(); i++) if (p[i] >= 0 && at[i] != p[i]) return false; return true; };
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* b = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 5 <= n; k++) {
            if (b[k] != 0xE8) continue;
            int32_t rel; memcpy(&rel, b + k + 1, 4); const uintptr_t ret = (uintptr_t)(b + k + 5);
            if (ret + (intptr_t)rel != target) continue;
            found++; const uint8_t* at = (const uint8_t*)ret;
            if (matches(at, "8B 00 85 C0 74 ?? 41 89 06 48 8D 4D C0 E8")) g_callerLevel = ret - g_base;          // mov eax,[rax]; test; je; mov [r14],eax; lea rcx,[rbp-0x40]; call
            else if (matches(at, "48 8D 55 10 48 8D 8D 08 02 00 00 E8")) g_callerDrop = ret - g_base;           // lea rdx,[rbp+0x10]; lea rcx,[rbp+0x208]; call register
            for (size_t j = 0; j < 0x90 && at + j + 7 <= b + n; j++) {   // a reason string: lea rcx,[rip+disp] to a short identifier, hashed right after the call
                if (at[j] != 0x48 || at[j + 1] != 0x8D || at[j + 2] != 0x0D) continue;
                int32_t d; memcpy(&d, at + j + 3, 4); const uintptr_t str = (uintptr_t)(at + j + 7) + (intptr_t)d;
                if (!InImage(str)) continue;
                char buf[24] = {}; size_t m = 0; while (m < 22 && ReadBytes(str + m, buf + m, 1) && buf[m] && ((buf[m] >= 'a' && buf[m] <= 'z') || (buf[m] >= 'A' && buf[m] <= 'Z') || buf[m] == '_')) m++;
                if (m >= 3 && m < 22 && buf[m] == 0) { g_callerReason[ret - g_base] = buf; if (strcmp(buf, "housing") == 0) g_callerHousing = ret - g_base; break; }
            }
        }
    }
    Log("[gimmick] %d callers of the spawn prepare: level 0x%llx, housing 0x%llx, item drop 0x%llx, %zu with a reason string", found, (unsigned long long)g_callerLevel, (unsigned long long)g_callerHousing, (unsigned long long)g_callerDrop, g_callerReason.size());
    if (!g_callerLevel || !g_callerHousing) Log("[gimmick] a caller was not recognized in this build: the level / housing templates are limited to what is recognized");
}
static void ResolveGimmickSpawn() {
    int n = 0;
    { int m = 0; const uintptr_t h = FindPatternCount("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 8D 9A CD 1D BA DE 48 8B FA 44 8B CB 44 8B DB F6 C1 03", &m);
      if (h && m == 1) { g_gameHash = (GameHashFn)h; Log("resolved %-20s rva 0x%llx (via signature)", "string hash", (unsigned long long)(h - g_base)); if (g_traceHooks) { Log("[gimmick] earlier replay codes: 0x20D51DB5 = %s, 0x615A8292 = %s", DecodeErr(0x20D51DB5).c_str(), DecodeErr(0x615A8292).c_str()); FindStringsForHash(0x20D51DB5); } } else Log("[gimmick] string hash: %d matches", m); }
    const uintptr_t f = FindPatternCount("48 8B C4 48 89 58 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 C1 48 81 EC B0 00 00 00 C5 F8 29 70 B8 C5 F8 29 78 A8 49 8B D9 49 8B F0 4C 8B FA 4C 8B F1 48 8B 49 18 48 83 C1 40", &n);
    if (!f || n != 1) { Log("[gimmick] spawn function: %d matches, not hooked (research feature only)", n); return; }
    kRva_GimmickSpawn = f - g_base; kRva_GimmickSpawn_ = kRva_GimmickSpawn;
    Log("resolved %-20s rva 0x%llx (via signature)", "gimmick spawn", (unsigned long long)kRva_GimmickSpawn);
}
// Research: compact log of the character's own ground probes (sphere casts starting within 4 m of the player) for a few
// seconds: caller, start y, displacement, length, hit count, fraction and the resulting hit height. Walking into an edited dip
// shows whether the probe is too short or does not see the edited ground at all.
static volatile ULONGLONG g_groundTraceUntil = 0;
static std::mutex g_dbgMutex; static std::vector<DebugPt> g_dbgPts;
std::vector<DebugPt> DebugPoints() { std::lock_guard<std::mutex> l(g_dbgMutex); return g_dbgPts; }
size_t DebugPointCount() { std::lock_guard<std::mutex> l(g_dbgMutex); return g_dbgPts.size(); }
int LoadDebugPoints(bool clear) {   // research overlay: world points (e.g. the collision map of an edited area) drawn in game
    std::vector<DebugPt> v;
    if (!clear) {
        FILE* f = fopen((ModDir() + "\\debugpoints.txt").c_str(), "r");
        if (f) { char line[128]; while (fgets(line, sizeof line, f) && v.size() < 50000) { DebugPt d{}; unsigned c = 0xFFFFFF;
            if (sscanf(line, "%f %f %f %x", &d.p.x, &d.p.y, &d.p.z, &c) >= 3) { d.col = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); v.push_back(d); } }   // rrggbb -> ImGui ABGR
            fclose(f); }
    }
    std::lock_guard<std::mutex> l(g_dbgMutex); g_dbgPts.swap(v); Log("[points] %zu debug points", g_dbgPts.size()); return (int)g_dbgPts.size();
}
void GroundTrace(int seconds) { g_groundTraceUntil = GetTickCount64() + (ULONGLONG)std::clamp(seconds, 1, 60) * 1000; Log("[groundtrace] %d s", seconds); }
static void LogGroundProbe(void* q, void* col, uintptr_t ret) {
    uintptr_t shape = 0; float s[4] = {}, dv[8] = {};
    if (!ReadBytes((uintptr_t)q + 0x28, &shape, 8) || !shape || !ReadBytes((uintptr_t)q + 0x30, s, 16) || !ReadBytes((uintptr_t)q + 0x40, dv, 32)) return;
    const char* sn = RttiName(shape); if (!sn || !strstr(sn, "hknpSphereShape")) return;
    PosInfo pi{}; if (!PlayerPosInfo(&pi)) return;
    if (fabsf(s[0] - pi.tiled.x) > 1.0f || fabsf(s[2] - pi.tiled.z) > 1.0f || fabsf(s[1] - pi.tiled.y) > 6.0f) return;   // the player's own probe
    if (fabsf(dv[0]) > 0.01f || fabsf(dv[2]) > 0.01f || dv[1] > -0.5f) return;   // only the straight-down ground cast (the sideways ones are step / wall checks)
    uint32_t nh = 0; double frac = 0; ReadBytes((uintptr_t)col + 0x0C, &nh, 4); ReadBytes((uintptr_t)col + 0x10, &frac, 8);
    const float hitY = nh ? s[1] + (float)frac * dv[1] : NAN;
    Log("[groundtrace] from rva 0x%llx: start y %.2f (player %.2f) disp (%.2f %.2f %.2f %.2f) len %.2f | hits %u frac %.4f -> y %.2f",
        InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, s[1], pi.tiled.y, dv[0], dv[1], dv[2], dv[3], dv[7], nh, nh ? frac : 0.0, hitY);
}
static void* __fastcall HookWorldCastShape(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    const bool trace = g_shapeTraceLeft > 0 && InterlockedDecrement(&g_shapeTraceLeft) >= 0;
    { std::lock_guard<std::mutex> operation(g_groundOpMutex); std::lock_guard<std::mutex> lock(g_tplMutex);
      if (g_tpl.world != a) {
          if (g_tpl.world) GroundEpochLocked(); // initial readiness is not replacement of an older template
          g_tpl.world = a; g_tpl.have = false; g_probeCalibrated = false;
      }
    }
    CaptureTemplate(a, b, c, d); // native reads happen without operation/registry/probe-queue locks   // automatic, once per world
    if (g_groundTraceUntil && GetTickCount64() < g_groundTraceUntil) {
        void* r = g_origWorldCastShape(a, b, c, d, e, f, g, h);
        LogGroundProbe(b, d, (uintptr_t)_ReturnAddress());
        return r;
    }
    if (!trace) return g_origWorldCastShape(a, b, c, d, e, f, g, h);
    std::lock_guard<std::recursive_mutex> lock(g_traceMutex);
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    // signature (from the disassembly): worldCastShape(this = world object, query, transform, collector, stackArg)
    Log("[shape] ===== worldCastShape from rva 0x%llx thread %lu: world=%p query=%p xform=%p collector=%p e=%p", InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, GetCurrentThreadId(), a, b, c, d, e);
    DumpDeep("query", (uintptr_t)b, 0xA0, 6); DumpBlock("xform", (uintptr_t)c, 0x80); DumpDeep("collector before", (uintptr_t)d, 0x100, 6); DumpBlock("e", (uintptr_t)e, 0x80);
    void* r = g_origWorldCastShape(a, b, c, d, e, f, g, h);
    DumpBlock("collector after", (uintptr_t)d, 0x100);
    { uintptr_t buf = 0; if (ReadBytes((uintptr_t)d + 0x20, &buf, 8) && buf) DumpBlock("collector+20 -> hits after", buf, 0x100); }
    Log("[shape] worldCastShape returned %p", r);
    return r;
}
static bool g_probeZeroVel = false;   // dev variant: second vector zeroed (proved irrelevant)
// Replays the captured sphere cast from start straight down (game thread). The collector keeps the closest hit: +0x0C hit
// count, +0x10 the hit fraction as a double (0..1 of the displacement), +0x80 the normal. Sphere center at the hit =
// start - fraction * len (y); the surface is one sphere radius further down.
static bool CallCastGuarded(void* world, void* q, void* xf, void* col, void** r) {   // plain function: SEH must not share a frame with C++ objects
    CDK_GUARD_BEGIN *r = g_origWorldCastShape(world, q, xf, col, col, nullptr, nullptr, nullptr); return true;
    CDK_GUARD_FAIL return false;
    CDK_GUARD_END
}
static bool RunGroundCast(void* world, Vec3 start, float len, int tileX, int tileZ, GroundHit* out, bool verbose, Vec3 dir = Vec3{ 0, -1, 0 }) {
#ifdef WB_UNIFIED_HOST_TEST
    return host::Seam().groundCast && host::Seam().groundCast(start, len, out);
#else
    alignas(16) uint8_t q[0x200], xf[0x100], col[0x300], shp[0x200];
    memset(col, 0, sizeof col);
    { std::lock_guard<std::mutex> l(g_tplMutex);
      if (!g_tpl.have || !g_origWorldCastShape) return false;
      world = g_tpl.world;
      memcpy(q, g_tpl.query, sizeof q); memcpy(xf, g_tpl.xform, sizeof xf); memcpy(col, g_tpl.collector, sizeof g_tpl.collector); memcpy(shp, g_tpl.shape, sizeof shp); }
    uintptr_t pShape = (uintptr_t)shp, pHits = (uintptr_t)col + 0x30; memcpy(q + 0x28, &pShape, 8); memcpy(col + 0x20, &pHits, 8);   // inline hit buffer at +0x30, as in the original object
    float* fq = (float*)q;
    fq[0x30 / 4] = start.x - tileX * 1000.0f; fq[0x34 / 4] = start.y; fq[0x38 / 4] = start.z - tileZ * 1000.0f; fq[0x3C / 4] = 0;
    fq[0x40 / 4] = dir.x * len; fq[0x44 / 4] = dir.y * len; fq[0x48 / 4] = dir.z * len; fq[0x4C / 4] = 1.0f;   // displacement
    fq[0x50 / 4] = g_probeZeroVel ? 0.0f : dir.x * len; fq[0x54 / 4] = g_probeZeroVel ? 0.0f : dir.y * len; fq[0x58 / 4] = g_probeZeroVel ? 0.0f : dir.z * len; fq[0x5C / 4] = len;
    { const double big = 1e19; memcpy(col + 0x10, &big, 8); uint32_t zero = 0; memcpy(col + 0x0C, &zero, 4); }   // reset: no hit, early-out far away
    if (verbose) { Log("[probe] cast: start (%.2f %.2f %.2f) tile %d,%d local (%.2f %.2f %.2f) down %.1f m", start.x, start.y, start.z, tileX, tileZ, fq[0x30 / 4], fq[0x34 / 4], fq[0x38 / 4], len); }
    void* r = nullptr;
    if (!CallCastGuarded(world, q, xf, col, &r)) { const uintptr_t fa = (uintptr_t)cdk::t_fault.rec.ExceptionAddress; Log("[probe] replay crashed (caught): %08lx at %p (rva 0x%llx), address %p", cdk::t_fault.rec.ExceptionCode, (void*)fa, (unsigned long long)(InImage(fa) ? fa - g_base : 0), cdk::t_fault.rec.NumberParameters > 1 ? (void*)cdk::t_fault.rec.ExceptionInformation[1] : nullptr); return false; }
    uint32_t nh = 0; double frac = 0; memcpy(&nh, col + 0x0C, 4); memcpy(&frac, col + 0x10, 8);
    const float* fc = (const float*)col;
    out->done = true; out->hit = nh > 0 && std::isfinite(frac) && frac <= 1.0; out->fraction = (float)frac;   // negative fraction = the cast started inside a body (penetration), reported as a hit so the caller can step lower
    out->centerY = start.y + dir.y * (float)frac * len; out->normal = { fc[0x80 / 4], fc[0x84 / 4], fc[0x88 / 4] };
    out->center = { start.x + dir.x * (float)frac * len, out->centerY, start.z + dir.z * (float)frac * len };
    if (verbose) Log("[probe] RESULT hits %u fraction %.4f -> sphere center y %.3f (%.2f m below start), normal (%.3f %.3f %.3f), returned %p", nh, frac, out->centerY, (float)frac * len, out->normal.x, out->normal.y, out->normal.z, r);
    return true;
#endif
}
static int QueueGround(Vec3 start, float len, bool verbose, uint64_t epoch, const GroundHandle& op = {}, Vec3 dir = Vec3{ 0, -1, 0 }, bool robust = false) {
    std::lock_guard<std::mutex> l(g_groundMutex); const int id = ++g_groundNext;
    g_groundQueue.push_back({ id, start, len, verbose, epoch, op, bool(op), dir, robust });
    g_groundResults[id] = { {}, epoch, op, bool(op) }; // admission identity survives queue dispatch and world invalidation
    if (g_groundResults.size() > 40000) g_groundResults.erase(g_groundResults.begin()); // evicted -> explicit Unknown, never Pending
    InterlockedExchange(&g_groundQueued, 1); return id;
}
// ---- ground queries for the editor
bool GroundProbeReady() {
#ifdef WB_UNIFIED_HOST_TEST
    return host::Seam().probeReady;
#else
    std::lock_guard<std::mutex> lock(g_tplMutex); return g_tpl.have && g_origWorldCastShape != nullptr;
#endif
}
static void CalibrateProbe(void* world) {   // cast at the player's feet; the sphere center stops one radius above the ground
    uint64_t epoch; { std::lock_guard<std::mutex> operation(g_groundOpMutex); epoch = g_groundEpoch; }
    PosInfo pi{}; if (!PlayerPosInfo(&pi)) return;
    GroundHit h; Vec3 s = { pi.world.x, pi.world.y + 3.0f, pi.world.z };
    if (!RunGroundCast(world, s, 10.0f, pi.tileX, pi.tileZ, &h, false) || !h.hit) return;
    const float r = h.centerY - pi.world.y;
    if (r > -0.5f && r < 1.0f) { std::lock_guard<std::mutex> operation(g_groundOpMutex); if (epoch != g_groundEpoch) return; std::lock_guard<std::mutex> lock(g_tplMutex); g_probeRadius = r > 0.0f ? r : 0.0f; g_probeCalibrated = true; Log("[ground] probe calibrated at the player: sphere center %.3f above the feet -> radius %.3f", r, g_probeRadius); }
}
static void ServiceGroundQueue(void* world) {   // physics thread, inside the game's own worldCastShape call
    static thread_local bool s_inside = false; if (s_inside) return; s_inside = true;
    std::vector<GroundReq> batch;
    { std::lock_guard<std::mutex> l(g_groundMutex); batch.swap(g_groundQueue); InterlockedExchange(&g_groundQueued, 0); }
    bool calibrated; { std::lock_guard<std::mutex> lock(g_tplMutex); calibrated = g_probeCalibrated; }
    if (!calibrated && !batch.empty()) CalibrateProbe(world);
    for (const auto& rq : batch) {
        auto op = rq.op.lock();
        { std::lock_guard<std::mutex> lock(g_groundOpMutex);
          const bool invalid = rq.epoch != g_groundEpoch || g_groundWorldWriting || (rq.owned && (!op || !GroundValidLocked(op)));
          std::lock_guard<std::mutex> queue(g_groundMutex);
          const auto stored = g_groundResults.find(rq.id);
          if (stored == g_groundResults.end()) continue; // consumed invalidation or eviction: never resurrect it
          if (invalid) { stored->second.status = GroundProbeStatus::Invalidated; continue; }
        }
        float radius; { std::lock_guard<std::mutex> lock(g_tplMutex); radius = g_probeRadius; }
        const int tx = (int)(rq.start.x * 0.001), tz = (int)(rq.start.z * 0.001);
        t_geoTracing = InterlockedExchange(&g_geoTraceArm, 0) != 0;   // research: log the geometry calls of this one cast
        if (t_geoTracing) Log("[geo] ---- traced cast from (%.2f %.2f %.2f) %.2f m down", rq.start.x, rq.start.y, rq.start.z, rq.len);
        GroundHit h;
        if (!RunGroundCast(world, rq.start, rq.len, tx, tz, &h, rq.verbose, rq.dir)) { h.done = true; h.hit = false; }
        // The captured character sphere cast can occasionally fall through one terrain heightfield quad (documented in
        // notes/FORMATS.md) and return a perfectly valid hit on geometry below the ground. For editor ground queries only,
        // compare the centre cast with four nearby casts. Preserve a normal centre hit (important for bridges/platforms);
        // only replace it when at least two neighbours agree on a surface clearly ABOVE the centre result, or when the centre
        // missed entirely. 0.36 m crosses a 0.5 m terrain quad without moving the placement visibly.
        if (rq.robust && rq.dir.y < -0.99f && fabsf(rq.dir.x) < 0.01f && fabsf(rq.dir.z) < 0.01f) {
            struct S { GroundHit h{}; float y = 0; };
            S ns[4]; int nn = 0;
            static const float off[4][2] = { { 0.36f, 0 }, { -0.36f, 0 }, { 0, 0.36f }, { 0, -0.36f } };
            for (int k = 0; k < 4; ++k) {
                const Vec3 s{ rq.start.x + off[k][0], rq.start.y, rq.start.z + off[k][1] };
                const int stx = (int)(s.x * 0.001f), stz = (int)(s.z * 0.001f);
                GroundHit qh;
                if (RunGroundCast(world, s, rq.len, stx, stz, &qh, false, rq.dir) && qh.hit && std::isfinite(qh.centerY)) {
                    ns[nn].h = qh; ns[nn].y = qh.centerY - radius; nn++;
                }
            }
            if (nn >= 2) {
                float ys[4]; for (int k = 0; k < nn; ++k) ys[k] = ns[k].y;
                std::sort(ys, ys + nn); const float median = ys[nn / 2];
                int agree = 0, pick = -1; float best = 1e9f;
                for (int k = 0; k < nn; ++k) {
                    const float d = fabsf(ns[k].y - median);
                    if (d <= 0.45f) agree++;
                    if (d < best) { best = d; pick = k; }
                }
                const float cy = h.centerY - radius;
                const bool centreBad = !h.hit || !std::isfinite(cy);
                const bool fellThrough = !centreBad && agree >= 2 && median > cy + 0.55f;
                if ((centreBad && agree >= 2) || fellThrough) {
                    if (pick >= 0) {
                        GroundHit fixed = ns[pick].h;
                        // Return the neighbour's surface height but keep the requested x/z as the semantic query point.
                        fixed.center.x = rq.start.x; fixed.center.z = rq.start.z;
                        h = fixed;
                        static int s_fixLogs = 0;
                        if (s_fixLogs++ < 30)
                            Log("[ground] robust probe corrected %s: center surface %.2f -> %.2f (%d/%d neighbours agree)",
                                centreBad ? "miss" : "low fall-through", centreBad ? -99999.0f : cy, median, agree, nn);
                    }
                }
            }
        }
        if (t_geoTracing) { Log("[geo] ---- result: %s y %.2f", h.hit ? "hit" : "MISS", h.centerY); t_geoTracing = false; }
        h.radius = radius;
        { std::lock_guard<std::mutex> lock(g_groundOpMutex);
          const bool invalid = rq.epoch != g_groundEpoch || g_groundWorldWriting || (rq.owned && (!op || !GroundValidLocked(op)));
          std::lock_guard<std::mutex> queue(g_groundMutex);
          const auto stored = g_groundResults.find(rq.id);
          if (stored == g_groundResults.end()) continue; // a consumed invalidation cannot publish a late physical hit
          stored->second.hit = h;
          stored->second.status = invalid ? GroundProbeStatus::Invalidated : h.hit ? GroundProbeStatus::Hit : GroundProbeStatus::Miss;
        }
    }
    s_inside = false;
}
// Research: the collision height on a grid (the character's own sphere cast, replayed straight down from 'top' over 'len' m),
// queued in one batch and served on the game thread like GroundProbe. Returns the sphere centre heights (NAN = nothing found).
bool GroundGrid(float x0, float z0, int nx, int nz, float step, float top, float len, std::vector<float>* out) {
    if (!GroundProbeReady() || !GameThreadReady() || nx < 1 || nz < 1 || nx > 20000 / nz) return false;
    uint64_t epoch; { std::lock_guard<std::mutex> lock(g_groundOpMutex); if (g_groundWorldWriting) return false; epoch = g_groundEpoch; }
    std::vector<int> ids; ids.reserve((size_t)nx * nz);
    for (int j = 0; j < nz; j++) for (int i = 0; i < nx; i++) ids.push_back(QueueGround({ x0 + i * step, top, z0 + j * step }, len, false, epoch));
    RunOnGameThread([]() { ServiceGroundQueue(nullptr); });
    out->assign(ids.size(), NAN);
    const ULONGLONG until = GetTickCount64() + 20000; size_t done = 0;
    while (done < ids.size() && GetTickCount64() < until) {
        Sleep(20); done = 0;
        for (size_t k = 0; k < ids.size(); k++) {
            if (ids[k] == 0) { done++; continue; }
            GroundHit h; const auto status = GroundResultState(ids[k], &h);
            if (status == GroundProbeStatus::Invalidated || status == GroundProbeStatus::Unknown) return false;
            if (status != GroundProbeStatus::Pending) { (*out)[k] = status == GroundProbeStatus::Hit ? h.centerY : NAN; ids[k] = 0; done++; }
        }
    }
    return done == ids.size();
}
int GroundProbe(Vec3 start, float len) {
    if (!GroundProbeReady() || !GameThreadReady()) return 0;
    uint64_t epoch; { std::lock_guard<std::mutex> lock(g_groundOpMutex); if (g_groundWorldWriting) return 0; epoch = g_groundEpoch; }
    const int id = QueueGround(start, len, false, epoch, {}, Vec3{ 0, -1, 0 }, true);
    RunOnGameThread([]() { ServiceGroundQueue(nullptr); });   // game thread, between the game's own casts (the only context that worked so far)
    return id;
}
int RayProbe(Vec3 start, Vec3 dir, float len) {   // the same cast along any direction (the terrain brush: from the camera along the mouse ray)
    if (!GroundProbeReady() || !GameThreadReady()) return 0;
    const float l = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z); if (l < 1e-6f) return 0;
    uint64_t epoch; { std::lock_guard<std::mutex> lock(g_groundOpMutex); if (g_groundWorldWriting) return 0; epoch = g_groundEpoch; }
    const int id = QueueGround(start, len, false, epoch, {}, Vec3{ dir.x / l, dir.y / l, dir.z / l });
    RunOnGameThread([]() { ServiceGroundQueue(nullptr); });
    return id;
}
GroundProbeStatus GroundResultState(int ticket, GroundHit* out) {
    std::lock_guard<std::mutex> operation(g_groundOpMutex);
    std::lock_guard<std::mutex> queue(g_groundMutex);
    const auto it = g_groundResults.find(ticket);
    if (it == g_groundResults.end()) return GroundProbeStatus::Unknown;
    const auto status = it->second.epoch != g_groundEpoch ? GroundProbeStatus::Invalidated : it->second.status;
    if (status == GroundProbeStatus::Pending) return status;
    if (status == GroundProbeStatus::Hit || status == GroundProbeStatus::Miss) *out = it->second.hit;
    g_groundResults.erase(it); // invalidation is consumed as its own outcome, never synthesized as a miss
    return status;
}
bool GroundResult(int ticket, GroundHit* out) {
    const auto status = GroundResultState(ticket, out);
    return status == GroundProbeStatus::Hit || status == GroundProbeStatus::Miss;
}
int GroundTicket(const GroundHandle& op, Vec3 start, float length) {
    if (!GroundProbeReady() || !GameThreadReady()) return 0;
    int id;
    { std::lock_guard<std::mutex> lock(g_groundOpMutex);
      if (op->ticket || op->view.state != GroundProbing || !GroundValidLocked(op)) return 0;
      id = QueueGround(start, length, false, op->view.epoch, op, Vec3{ 0, -1, 0 }, true);
      op->ready = true; op->ticketAt = g_groundFrame; op->ticket = op->view.probe = id;
    }
    RunOnGameThread([]() { ServiceGroundQueue(nullptr); });
    return id;
}
bool GroundPoll(const GroundHandle& op, GroundHit* hit) {
    int ticket;
    { std::lock_guard<std::mutex> lock(g_groundOpMutex); ticket = op->ticket; }
    if (!ticket) return false;
    const auto status = GroundResultState(ticket, hit);
    if (status == GroundProbeStatus::Pending) return false;
    std::lock_guard<std::mutex> lock(g_groundOpMutex); op->ticket = 0;
    if (status == GroundProbeStatus::Invalidated || status == GroundProbeStatus::Unknown) {
        GroundEndLocked(op, GroundInvalidated, "epoch-invalidated"); return false;
    }
    return GroundValidLocked(op);
}
void ProbeGround(float above, float len) {
    PosInfo pi{}; if (!PlayerPosInfo(&pi)) { Log("[probe] no player position"); return; }
    Vec3 start = { pi.world.x, pi.world.y + above, pi.world.z }; const int tx = pi.tileX, tz = pi.tileZ;
    (void)tx; (void)tz;
    if (!GroundProbeReady()) { Log("[probe] no template yet (the character has to be in the world for a moment)"); return; }
    Log("[probe] player at (%.2f %.2f %.2f); replays: %.0f m above / %.0f m down, %.0f m above / %.0f m down, %.0f m above / 30 m down (served inside the game's next cast)", pi.world.x, pi.world.y, pi.world.z, above, len, above + 3, len, above + 3);
    uint64_t epoch; { std::lock_guard<std::mutex> lock(g_groundOpMutex); epoch = g_groundEpoch; }
    QueueGround(start, len, true, epoch); Vec3 s2 = { start.x, start.y + 3.0f, start.z }; QueueGround(s2, len, true, epoch); QueueGround(s2, 30.0f, true, epoch);
    if (GameThreadReady()) RunOnGameThread([]() { ServiceGroundQueue(nullptr); });
}
// Research: up to 6 arbitrary functions hooked by rva; a call is logged (integer args, return value, caller) only on a thread
// that is inside one of our traced ground casts. Nothing is logged before the original returns, so float / vector arguments
// in xmm registers reach the original untouched.
static const int kFnTraces = 6; static void* g_fnOrig[kFnTraces] = {}; static uintptr_t g_fnRva[kFnTraces] = {};
template<int K> static void* __fastcall FnTraceThunk(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    typedef void* (__fastcall* Fn)(void*, void*, void*, void*, void*, void*, void*, void*);
    void* r = ((Fn)g_fnOrig[K])(a, b, c, d, e, f, g, h);
    if (t_geoTracing) { const uintptr_t ret = (uintptr_t)_ReturnAddress();
        Log("[fn] rva 0x%llx from 0x%llx: a=%p b=%p c=%p d=%p e=%p f=%p -> %p", (unsigned long long)g_fnRva[K], InImage(ret) ? (unsigned long long)(ret - g_base) : 0ull, a, b, c, d, e, f, r);
        float fe[8] = {}, ff[8] = {};   // output blocks behind the 5th / 6th argument (e.g. decoded node bounds), as floats
        if (ReadBytes((uintptr_t)e, fe, 32) && ReadBytes((uintptr_t)f, ff, 32))
            Log("[fn]   e: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f   f: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", fe[0], fe[1], fe[2], fe[3], fe[4], fe[5], fe[6], fe[7], ff[0], ff[1], ff[2], ff[3], ff[4], ff[5], ff[6], ff[7]); }
    return r;
}
void FnTraceInstall(uintptr_t rva) {
    static void* thunks[kFnTraces] = { (void*)&FnTraceThunk<0>, (void*)&FnTraceThunk<1>, (void*)&FnTraceThunk<2>, (void*)&FnTraceThunk<3>, (void*)&FnTraceThunk<4>, (void*)&FnTraceThunk<5> };
    for (int k = 0; k < kFnTraces; k++) if (g_fnRva[k] == rva) { Log("[fn] rva 0x%llx already hooked", (unsigned long long)rva); return; }
    for (int k = 0; k < kFnTraces; k++) if (!g_fnRva[k]) {
        const uintptr_t f = g_base + rva; if (!InImage(f)) return;
        ReleaseHookPiece();
        if (MH_CreateHook((void*)f, thunks[k], &g_fnOrig[k]) == MH_OK && MH_EnableHook((void*)f) == MH_OK) { g_fnRva[k] = rva; Log("[fn] hooked rva 0x%llx", (unsigned long long)rva); }
        else Log("[fn] hooking rva 0x%llx failed", (unsigned long long)rva);
        return;
    }
    Log("[fn] no free trace slot");
}
void GeoTraceInstall(uintptr_t vt, int slots) {
    static bool s_done = false; if (s_done) { Log("[geo] already installed"); return; }
    if (!InImage(vt)) { Log("[geo] %p is not a vtable in the image", (void*)vt); return; }
    s_done = true; InstallVtableTracer(5, "", "TerrainHeightFieldGeometry", std::clamp(slots, 1, kVtMax), vt);
}
void GeoTraceArm() { InterlockedExchange(&g_geoTraceArm, 1); }
void RayTrace(int calls) { InterlockedExchange(&g_rayTraceLeft, calls); InterlockedExchange(&g_shapeTraceLeft, calls);
    Log("[ray] tracing the next %d ray casts and %d shape casts (walk a few steps for the ground probe, then aim / interact for ray casts)", calls, calls); }

// ---- experimental: teleport by writing the player's transform component; camera field discovery ----
static bool SetPlayerPosWrites(Vec3 world) {
#ifdef WB_UNIFIED_HOST_TEST
    return host::Seam().teleport && host::Seam().teleport(world);
#else
    uintptr_t actor = PlayerActor(); if (!actor) return false;
    uintptr_t comps = Deref(actor, kOff_Ent_Comps);
    uintptr_t tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    if (!tf) return false;
    const int tx = (int)(world.x * 0.001), tz = (int)(world.z * 0.001);
    float v[3] = { world.x - tx * kTileSize, world.y, world.z - tz * kTileSize }; int16_t tile[2] = { (int16_t)tx, (int16_t)tz };
    bool ok = WriteBytes(tf + kOff_Tf_Pos, v, 12) && WriteBytes(tf + kOff_Tf_Tile, tile, 4);
    Log("teleport -> (%.1f %.1f %.1f) tile %d,%d: %s", world.x, world.y, world.z, tx, tz, ok ? "written" : "FAILED");
    return ok;
#endif
}
bool SetPlayerPos(Vec3 world) {
    { std::lock_guard<std::mutex> lock(g_groundOpMutex);
      if (!g_groundLeases.empty() || g_groundWorldWriting) return false; // synchronous busy; NEVER delayed teleport
      ++g_groundWorldWriting; GroundEpochLocked();
    }
    const bool ok = SetPlayerPosWrites(world); // even a partial write invalidates every older probe
    { std::lock_guard<std::mutex> lock(g_groundOpMutex); --g_groundWorldWriting; }
    return ok;
}
static uintptr_t FindComponent(const char* cls) {   // walks the player's component table
    uintptr_t actor = PlayerActor(); if (!actor) return 0;
    uintptr_t comps = Deref(actor, kOff_Ent_Comps); if (!comps) return 0;
    for (unsigned to = 0; to < 0x400; to += 8) { uintptr_t c = Deref(comps, to); const char* n = c ? RttiName(c) : nullptr; if (n && strstr(n, cls)) return c; }
    return 0;
}
static void FindCameraObjects(std::vector<std::pair<std::string, uintptr_t>>& out);
void ExpandCameraManager(std::vector<std::pair<std::string, uintptr_t>>& out) {
    // the CameraManager itself does not change while rotating; the live camera is one of the objects it points to
    std::vector<std::pair<std::string, uintptr_t>> found; FindCameraObjects(found);
    std::vector<std::pair<std::string, uintptr_t>> res; std::set<uintptr_t> seen;
    for (auto& f : found) {
        if (f.first.find("CameraManager") == std::string::npos) continue;
        res.push_back(f); seen.insert(f.second);
        for (unsigned o = 0; o < 0x600; o += 8) {
            uintptr_t p = Deref(f.second, o); if (!p || seen.count(p)) continue;
            const char* n = RttiName(p); if (!n) continue;
            seen.insert(p); res.push_back({ std::string(n) + " (mgr+0x" + std::to_string(o) + ")", p });
            for (unsigned o2 = 0; o2 < 0x400; o2 += 8) { uintptr_t q = Deref(p, o2); if (!q || seen.count(q)) continue; const char* m = RttiName(q); if (m && strstr(m, "Camera")) { seen.insert(q); res.push_back({ std::string(m) + " (2nd level)", q }); } }
        }
    }
    if (res.empty()) res = found;
    out = res;
}
static void FindCameraObjects(std::vector<std::pair<std::string, uintptr_t>>& out) {
    // candidates: anything reachable within two pointer hops from the world root, the user actor and the player actor whose RTTI name contains "Camera"
    uintptr_t roots[3] = { Deref(g_base + kRva_WorldGlobal, 0), UserActor(), PlayerActor() };
    std::set<uintptr_t> seen;
    for (uintptr_t r : roots) {
        if (!r) continue;
        for (unsigned o1 = 0; o1 < 0x800; o1 += 8) {
            uintptr_t p = Deref(r, o1); if (!p || seen.count(p)) continue;
            const char* n = RttiName(p);
            if (n && strstr(n, "Camera")) { seen.insert(p); out.push_back({ n, p }); continue; }
            if (!n) continue;   // only descend into real objects
            for (unsigned o2 = 0; o2 < 0x400; o2 += 8) { uintptr_t q = Deref(p, o2); if (!q || seen.count(q)) continue; const char* m = RttiName(q); if (m && strstr(m, "Camera")) { seen.insert(q); out.push_back({ m, q }); } }
        }
    }
}
// The CameraManager (reachable from the world root) keeps the active camera's SceneObject at +0x40; its world transform
// sits at +0x1A4 like every SceneObject (scale3, quat4 at +0x1B0, pos3). Guarded by a mutex: the free camera asks for it on
// the game thread every frame, the editor on the render thread.
static uintptr_t g_camMgr = 0; static DWORD g_camSearchAt = 0; static std::mutex g_camMgrMutex;
static uintptr_t CameraManagerPtr() {
    std::lock_guard<std::mutex> lock(g_camMgrMutex);
    if (g_camMgr) {
        const char* n = RttiName(g_camMgr);
        if (n && strstr(n, "CameraManager@")) return g_camMgr;
        g_camMgr = 0;
    }
    const DWORD now = GetTickCount();
    if (now - g_camSearchAt < 2000) return 0;
    g_camSearchAt = now;
    std::vector<std::pair<std::string, uintptr_t>> found;
    FindCameraObjects(found);
    for (auto& f : found) if (f.first.find("CameraManager@") != std::string::npos) {
        g_camMgr = f.second;
        Log("camera manager %p", (void*)g_camMgr);
        break;
    }
    return g_camMgr;
}
uintptr_t CameraSceneObject() {
    uintptr_t mgr = CameraManagerPtr(); if (!mgr) return 0;
    uintptr_t so = Deref(mgr, 0x40); const char* n = so ? RttiName(so) : nullptr;
    return n && strstr(n, "SceneObject") ? so : 0;
}
bool CameraBasis(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd) {
    uintptr_t so = CameraSceneObject(); if (!so) return false;
    float xf[10]; int16_t tile[2];
    if (!ReadBytes(so + 0x1A4, xf, 40) || !ReadBytes(so + 0x1CC, tile, 4)) return false;
    const float x = xf[3], y = xf[4], z = xf[5], w = xf[6];
    if (!std::isfinite(x + y + z + w) || fabsf(x * x + y * y + z * z + w * w - 1.0f) > 0.1f) return false;
    // rotation matrix columns = rotated basis vectors
    if (right) *right = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w) };
    if (up)    *up    = { 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w) };
    if (fwd)   *fwd   = { 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
    if (pos) { *pos = { xf[7] + tile[0] * kTileSize, xf[8], xf[9] + tile[1] * kTileSize }; if (!std::isfinite(pos->x) || fabsf(pos->x) > 1e6f) return false; }
    return true;
}
bool CameraPose(Vec3* fwd, Vec3* pos);
static bool ReadDeg(uintptr_t obj, uintptr_t off, float* out) { float f; if (!obj || !ReadBytes(obj + off, &f, 4) || !std::isfinite(f) || f < 20.0f || f > 150.0f) return false; *out = f; return true; }
bool CameraFov(float* deg) {
    uintptr_t mgr = CameraManagerPtr(); if (!mgr) return false;
    uintptr_t cam = Deref(mgr, 0x10);
    if (cam) { const char* n = RttiName(cam); if (n && strstr(n, "Camera") && ReadDeg(cam, 0x1DC, deg)) return true; }
    // fovtrace (build 2944): ClientNormalInGameActor at mgr+0x218, +0x10C follows the zoom (45 .. 90 degrees)
    uintptr_t actor = Deref(mgr, 0x218);
    if (actor) { const char* n = RttiName(actor); if (n && strstr(n, "Actor") && ReadDeg(actor, 0x10C, deg)) return true; }
    return false;
}
bool CameraPose(Vec3* fwd, Vec3* pos) {
    {   // flying: the free camera is the camera
        Vec3 fp, ff;
        if (FreeCamPose(&fp, &ff)) { const float l = sqrtf(ff.x * ff.x + ff.z * ff.z); if (l >= 0.05f) { if (pos) *pos = fp; if (fwd) *fwd = { ff.x / l, 0, ff.z / l }; return true; } }
    }
    uintptr_t so = CameraSceneObject(); if (!so) return false;
    float xf[10]; int16_t tile[2];
    if (!ReadBytes(so + 0x1A4, xf, 40) || !ReadBytes(so + 0x1CC, tile, 4)) return false;   // world TiledTransform: scale3, quat4, pos3, tile
    const float x = xf[3], y = xf[4], z = xf[5], w = xf[6];
    float fx = 2 * (x * z + y * w), fz = 1 - 2 * (x * x + y * y);   // local +Z rotated by the quaternion, flattened
    const float l = sqrtf(fx * fx + fz * fz); if (!std::isfinite(l) || l < 0.05f) return false;
    if (fwd) *fwd = { fx / l, 0, fz / l };
    if (pos) *pos = { xf[7] + tile[0] * kTileSize, xf[8], xf[9] + tile[1] * kTileSize };
    if (pos && (!std::isfinite(pos->x) || fabsf(pos->x) > 1e6f)) return false;
    return true;
}
// fovtrace / camtrace live in diag.cpp (console-only reverse-engineering aids)


// ---- configurable hotkeys (bin64\cdmodkit\settings.txt: key_toggle=INSERT, key_mode=HOME) ----
int g_keyToggle = VK_INSERT, g_keyMode = VK_HOME; bool g_showConsole = false;
bool g_keyboardPlacement = false;
bool g_projectAutoSave = false;
bool g_autoFreeCamOnOpen = false;
bool g_showSelectionDetails = true;
float g_fovDeg = 55.0f; bool g_camMirror = false; bool g_fovAuto = true;
struct KeyEntry { const char* name; int vk; };
static const KeyEntry kKeyNames[] = {
    { "INSERT", VK_INSERT }, { "HOME", VK_HOME }, { "END", VK_END }, { "DELETE", VK_DELETE }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
    { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 }, { "F8", VK_F8 }, { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
    { "SCROLLLOCK", VK_SCROLL }, { "PAUSE", VK_PAUSE }, { "BACKQUOTE", VK_OEM_3 }, { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS }, { "BACKSLASH", VK_OEM_5 },
    { "NUMPAD*", VK_MULTIPLY }, { "NUMPAD/", VK_DIVIDE }, { "NUMLOCK", VK_NUMLOCK }, { "CAPSLOCK", VK_CAPITAL }, { "TAB", VK_TAB },
    { "NUMPAD0", VK_NUMPAD0 }, { "NUMPAD1", VK_NUMPAD1 }, { "NUMPAD2", VK_NUMPAD2 }, { "NUMPAD3", VK_NUMPAD3 }, { "NUMPAD4", VK_NUMPAD4 }, { "NUMPAD5", VK_NUMPAD5 }, { "NUMPAD6", VK_NUMPAD6 }, { "NUMPAD7", VK_NUMPAD7 }, { "NUMPAD8", VK_NUMPAD8 }, { "NUMPAD9", VK_NUMPAD9 },
    { "NUMPAD+", VK_ADD }, { "NUMPAD-", VK_SUBTRACT }, { "NUMPAD.", VK_DECIMAL }, { "ENTER", VK_RETURN }, { "BACKSPACE", VK_BACK }, { "SPACE", VK_SPACE }, { "SHIFT", VK_SHIFT }, { "CTRL", VK_CONTROL }, { "ALT", VK_MENU },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "A", 'A' }, { "B", 'B' }, { "C", 'C' }, { "D", 'D' }, { "E", 'E' }, { "F", 'F' }, { "G", 'G' }, { "H", 'H' }, { "I", 'I' }, { "J", 'J' }, { "K", 'K' }, { "L", 'L' }, { "M", 'M' },
    { "N", 'N' }, { "O", 'O' }, { "P", 'P' }, { "Q", 'Q' }, { "R", 'R' }, { "S", 'S' }, { "T", 'T' }, { "U", 'U' }, { "V", 'V' }, { "W", 'W' }, { "X", 'X' }, { "Y", 'Y' }, { "Z", 'Z' },
    { "0", '0' }, { "1", '1' }, { "2", '2' }, { "3", '3' }, { "4", '4' }, { "5", '5' }, { "6", '6' }, { "7", '7' }, { "8", '8' }, { "9", '9' },
    { nullptr, 0 } };
int g_placeKeys[PK_COUNT] = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD1, VK_ADD, VK_SUBTRACT, VK_NUMPAD0, VK_DECIMAL, VK_NUMPAD5, VK_MULTIPLY, VK_DIVIDE, VK_RETURN, VK_BACK, VK_SHIFT };
static const char* kPlaceKeyIds[PK_COUNT] = { "move_fwd", "move_back", "move_left", "move_right", "move_up", "move_down", "rotate_left", "rotate_right", "scale_up", "scale_down", "fetch", "snap", "mouse", "level", "ground", "drop", "cancel", "fast" };
static const char* kPlaceKeyLabels[PK_COUNT] = { "move away from me", "move closer", "move left", "move right", "move up", "move down", "rotate left", "rotate right", "scale up", "scale down", "bring it in front of me", "snapping on / off", "mouse to gizmo / camera", "level (remove the tilt)", "snap to ground", "drop", "cancel / put back", "fast (hold)" };
const char* PlaceKeyId(int i) { return (i >= 0 && i < PK_COUNT) ? kPlaceKeyIds[i] : ""; }
const char* PlaceKeyLabel(int i) { return (i >= 0 && i < PK_COUNT) ? kPlaceKeyLabels[i] : ""; }
void ApplyPlaceKeys() { input::SetPlaceVks(g_placeKeys, PK_COUNT); }
int KeyCount() { int n = 0; while (kKeyNames[n].name) n++; return n; }
const char* KeyNameAt(int i) { return kKeyNames[i].name; }
int KeyVkAt(int i) { return kKeyNames[i].vk; }
const char* KeyName(int vk) { for (int i = 0; kKeyNames[i].name; i++) if (kKeyNames[i].vk == vk) return kKeyNames[i].name; return "?"; }
static int KeyFromName(const std::string& n) { for (int i = 0; kKeyNames[i].name; i++) if (_stricmp(kKeyNames[i].name, n.c_str()) == 0) return kKeyNames[i].vk; if (n.size() == 1 && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]); return 0; }
static std::string SettingsPath() { return g_modDir + "\\settings.txt"; }
static void LoadSettings() {
    std::ifstream f(SettingsPath()); const bool missing = !f.good(); std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('='); if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "language") { i18n::SetPreference(v.c_str()); continue; }
        if (k == "console") { g_showConsole = v != "0" && v != "off" && v != "false"; continue; }
        if (k == "http_api") { g_httpEnabled = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "http_port") { char* end = nullptr; long port = strtol(v.c_str(), &end, 10); if (end && !*end && port >= 1 && port <= 65535) g_httpPort = (int)port; continue; }
        if (k == "fov") { float f = (float)atof(v.c_str()); if (f >= 10 && f <= 150) g_fovDeg = f; continue; }
        if (k == "mirror") { g_camMirror = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "fovauto") { g_fovAuto = v != "0" && v != "off" && v != "false"; continue; }
        if (k == "gimmick_spawn") { g_gimmickSpawn = v != "0" && v != "off" && v != "false"; continue; }
        if (k == "trace_hooks") { g_traceHooks = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "preview_quality") { thumbgen::SetQuality(atoi(v.c_str())); continue; }
        if (k == "freecam_speed") { const float s = (float)atof(v.c_str()); if (s >= 0.5f && s <= 200.0f) g_fcSpeed = s; continue; }
        if (k == "freecam_sens") { const float s = (float)atof(v.c_str()); if (s >= 0.01f && s <= 2.0f) g_fcSens = s; continue; }
        if (k == "keyboard_placement") { g_keyboardPlacement = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "project_autosave") { g_projectAutoSave = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "project_autosave_seconds") continue;   // legacy timed-auto-save setting; real-time auto-save no longer uses an interval
        if (k == "auto_freecam_on_open") { g_autoFreeCamOnOpen = v == "1" || v == "on" || v == "true"; continue; }
        if (k == "show_selection_details") { g_showSelectionDetails = v != "0" && v != "off" && v != "false"; continue; }
        // manual fallback for snap to ground if the collector vtable cannot be resolved after a game patch; deliberately
        // never written back by SaveSettings, otherwise a stale value would outrank the signature on the next build
        if (k == "probe_vtable") { g_probeVtableOverride = (uintptr_t)strtoull(v.c_str(), nullptr, 0); continue; }
        if (k == "groundsnap") continue;   // 0.74: snap to ground is always on, the old switch is ignored
        int vk = KeyFromName(v);
        if (!vk) continue;
        if (k == "key_toggle") g_keyToggle = vk; else if (k == "key_mode") g_keyMode = vk;
        else if (k.rfind("key_", 0) == 0) { for (int i = 0; i < PK_COUNT; i++) if (k == std::string("key_") + kPlaceKeyIds[i]) g_placeKeys[i] = vk; }
    }
    ApplyPlaceKeys();
    if (missing) SaveSettings();
    Log("settings: toggle %s, mode %s, keyboard placement %s, console %s", KeyName(g_keyToggle), KeyName(g_keyMode), g_keyboardPlacement ? "on" : "off", g_showConsole ? "on" : "off");
}
void SaveSettings() {
    FILE* f = fopen(SettingsPath().c_str(), "w"); if (!f) return;
    fprintf(f, "# World Builder hotkeys. Names: INSERT HOME END DELETE PAGEUP PAGEDOWN F1..F12 SCROLLLOCK PAUSE BACKQUOTE MINUS EQUALS BACKSLASH NUMPAD* NUMPAD/ NUMLOCK CAPSLOCK TAB or a single letter/digit\n");
    fprintf(f, "key_toggle=%s\nkey_mode=%s\n# console=0 hides the console window (log file only)\nconsole=%d\n# projection for the gizmo: vertical field of view in degrees and horizontal mirror (calibrate in the Settings tab)\nfov=%.1f\nmirror=%d\n# fovauto=1 reads the field of view from the game's renderer camera (fov= is the fallback)\nfovauto=%d\n", KeyName(g_keyToggle), KeyName(g_keyMode), g_showConsole ? 1 : 0, g_fovDeg, g_camMirror ? 1 : 0, g_fovAuto ? 1 : 0);
    fprintf(f, "# preview_quality: 0 base colour, 1 + dye colours, 2 + normal maps, 3 + specular/emissive. Lower renders the background pass faster\npreview_quality=%d\n", thumbgen::Quality());
    fprintf(f, "# gimmick_spawn=0: place gimmick prefabs (/object/cd_gimmick/...) as plain objects instead of through the game spawn path\ngimmick_spawn=%d\n", g_gimmickSpawn ? 1 : 0);
    fprintf(f, "# free camera (camera mode, key_mode in the editor): speed in m/s, mouse sensitivity in degrees per count\nfreecam_speed=%.1f\nfreecam_sens=%.3f\n", g_fcSpeed, g_fcSens);
    fprintf(f, "# Optional keyboard object placement. Off by default; enable in Settings or set keyboard_placement=1.\nkeyboard_placement=%d\n", g_keyboardPlacement ? 1 : 0);
    fprintf(f, "# Save dirty loaded projects immediately after each committed edit. New/unassigned objects are not attached to a project automatically.\nproject_autosave=%d\n", g_projectAutoSave ? 1 : 0);
    fprintf(f, "# Editor UI behavior.\nauto_freecam_on_open=%d\nshow_selection_details=%d\n", g_autoFreeCamOnOpen ? 1 : 0, g_showSelectionDetails ? 1 : 0);
    for (int i = 0; i < PK_COUNT; i++) fprintf(f, "key_%s=%s\n", kPlaceKeyIds[i], KeyName(g_placeKeys[i]));
    fprintf(f, "# Interface language: auto, en, zh-CN, zh-TW, de, fr, ko, ja, es, pt-BR, ru, tr\nlanguage=%s\n", i18n::Preference());
    fprintf(f, "# HTTP API for programs on this PC (127.0.0.1 only, see HTTP_API.md): http_api=1 runs it, http_port= its port. The Settings tab switches it at once\nhttp_api=%d\nhttp_port=%d\n", g_httpEnabled ? 1 : 0, g_httpPort);
    ApplyPlaceKeys(); fclose(f);
    Log("settings saved: toggle %s, mode %s, console %s", KeyName(g_keyToggle), KeyName(g_keyMode), g_showConsole ? "on" : "off");
}

// ---- console ----
static void CmdPos() {
    uintptr_t actor = PlayerActor();
    if (!actor) { Log("pos: no player actor yet"); return; }
    uint32_t eid = 0; Read32(actor + kOff_Ent_Eid, &eid);
    PosInfo p{}; const char* n = RttiName(actor);
    if (ReadPos(actor, &p)) Log("pos: actor=%p eid=%08X class=%s  world=(%.2f %.2f %.2f)  tile=(%d,%d) tiled=(%.2f %.2f %.2f)", (void*)actor, eid, n ? n : "?",
                                p.world.x, p.world.y, p.world.z, p.tileX, p.tileZ, p.tiled.x, p.tiled.y, p.tiled.z);
    else Log("pos: actor=%p eid=%08X class=%s (no transform)", (void*)actor, eid, n ? n : "?");
}
static void CmdStatus() {
    Log("status: creates=%ld, pump ticks=%ld game thread=%lu, hooks: create=%s pump=%s, spawned=%zu, prefabs=%zu, menu=%d",
        g_createCalls, g_pumpTicks, g_gameThread, g_origCreate ? "on" : "off", g_origPump ? "on" : "off", Spawned().size(), g_prefabs.size(), g_menuOpen);
}
static void CmdList() {
    auto l = Spawned();
    for (size_t i = 0; i < l.size(); i++) Log("  #%zu %p %s (%.1f %.1f %.1f)%s", i, (void*)l[i].obj, l[i].prefab.c_str(), l[i].pos.x, l[i].pos.y, l[i].pos.z, l[i].hidden ? " hidden" : "");
    if (l.empty()) Log("  (nothing spawned yet)");
}
// One console line -> one production command. ConsoleThread feeds every line it reads to this function
// (AllocConsole/stdin stay its OS boundary), and the host suites drive the SAME function through
// host::ConsoleDispatch - there is no second, copied test dispatcher. Returns true when the console loop
// must end (quit). Every mutating branch keeps the core entry point it always used (SpawnAt / MoveSpawned /
// HideSpawned / SetPlayerPos / LoadProject carry the C7 pre-mutation reconciliation); malformed input only logs.
static bool ConsoleDispatch(const std::string& cmd) {
    if (cmd == "pos") CmdPos();
    else if (cmd == "status") CmdStatus();
    else if (cmd == "list") CmdList();
    else if (cmd.rfind("flags ", 0) == 0) { int a = 1, b = 1, c = 0; sscanf(cmd.c_str() + 6, "%d %d %d", &a, &b, &c); g_flags[0] = a; g_flags[1] = b; g_flags[2] = c; Log("spawn flags now %d,%d,%d", a, b, c); }
    else if (cmd.rfind("spawn ", 0) == 0) {
        char path[400] = {0}; float dx = 2, dy = 0, dz = 0;
        if (sscanf(cmd.c_str() + 6, "%399s %f %f %f", path, &dx, &dy, &dz) < 1) { Log("usage: spawn <prefab> [dx dy dz]"); return false; }
        Vec3 p{}; if (!PlayerWorldPos(&p)) { Log("spawn: no player position"); return false; }
        SpawnAt(path, { p.x + dx, p.y + dy, p.z + dz });
    }
    else if (cmd.rfind("move ", 0) == 0) { int i = 0; float x, y, z, yaw = 0, sc = 1; if (sscanf(cmd.c_str() + 5, "%d %f %f %f %f %f", &i, &x, &y, &z, &yaw, &sc) >= 4) Log("move #%d -> %s", i, MoveSpawned(i, { x, y, z }, Rot{ yaw }, sc) ? "queued" : "failed"); else Log("usage: move <idx> x y z [yaw] [scale]"); }
    else if (cmd.rfind("hide ", 0) == 0) { int i = atoi(cmd.c_str() + 5); Log("hide #%d -> %s", i, HideSpawned(i) ? "ok" : "failed"); }
    else if (cmd.rfind("livemode ", 0) == 0) { g_liveMode = atoi(cmd.c_str() + 9); Log("livemode = %d", g_liveMode); }
    else if (cmd == "livedrag on" || cmd == "livedrag off") { g_liveDrag = cmd == "livedrag on"; Log("livedrag = %d", g_liveDrag ? 1 : 0); }
    else if (cmd == "recreate on" || cmd == "recreate off") { g_recreateOnMove = cmd == "recreate on"; Log("recreate = %d", g_recreateOnMove ? 1 : 0); }
    else if (cmd == "gimmicks on" || cmd == "gimmicks off") { g_gimmickSpawn = cmd == "gimmicks on"; Log("gimmicks = %d", g_gimmickSpawn ? 1 : 0); }
    else if (cmd == "captures") {
        GimmickCapInfo caps[16]; const int count = GimmickCaptureList(caps, 16);
        Log("captures=%d replay_armed=%d prefab=%s", count, GimmickReplayArmed() ? 1 : 0, GimmickReplayPrefab());
        for (int i = 0; i < count; ++i) { const auto& c = caps[i]; const char* caller = GimmickCallerName(c.caller);
            Log("id=%d age_ms=%lu name=%s path=%s caller=%s address=%p words=%u/%u", c.id, c.ageMs, c.name, c.path, caller ? caller : "?", (void*)c.caller, c.k1, c.k2); }
    }
    else if (cmd == "replays") {
        SpawnedInfo objects[16]; const int count = SpawnedList(objects, 16);
        for (int i = 0; i < count; ++i) { const auto& o = objects[i]; Log("actor=%p age_ms=%lu prefab=%s", (void*)o.actor, o.ageMs, o.prefab); }
        Log("replays=%d", count);
    }
    else if (cmd == "replayprefab" || cmd.rfind("replayprefab ", 0) == 0) { SetGimmickReplayPrefab(cmd.size() > 12 ? cmd.c_str() + 13 : ""); Log("replay prefab=%s", GimmickReplayPrefab()); }
    else if (cmd.rfind("replay ", 0) == 0) { int id = 0; Vec3 at{};
        if (sscanf(cmd.c_str() + 7, "%d %f %f %f", &id, &at.x, &at.y, &at.z) == 4 && id > 0) ArmGimmickReplay(at, id);
        else Log("usage: replay <capture id> x y z (captures lists ids; replayprefab sets the override)"); }
    else if (cmd.rfind("removereplay ", 0) == 0) { unsigned long long actor = 0;
        if (sscanf(cmd.c_str() + 13, "%llx", &actor) == 1 && actor) RequestRemoveSpawned((uintptr_t)actor);
        else Log("usage: removereplay <hex actor from replays>"); }
    else if (cmd == "trace on" || cmd == "trace off") SetTrace(cmd == "trace on");
    else if (cmd.rfind("tp ", 0) == 0) { Vec3 w{}; if (sscanf(cmd.c_str() + 3, "%f %f %f", &w.x, &w.y, &w.z) == 3) SetPlayerPos(w); else Log("usage: tp x y z"); }
    else if (cmd == "camtrace") CamTrace(16);
    else if (cmd == "fovtrace") FovTrace(12);
    else if (cmd == "raytrace") RayTrace(12);
    else if (cmd == "probe") ProbeGround(3.0f, 10.0f);
    else if (cmd.rfind("camwatch", 0) == 0) { int s = 8, m = 0; sscanf(cmd.c_str() + 8, "%d %d", &s, &m); CamWatch(s, m); }
    else if (cmd == "traceio on" || cmd == "traceio off") SetIoTrace(cmd == "traceio on");
    else if (cmd.rfind("npc ", 0) == 0) {   // npc <characterKey> [type]: spawn 4 m east of the player
        unsigned key = 0; int type = 1; Vec3 p{};
        if (sscanf(cmd.c_str() + 4, "%u %d", &key, &type) >= 1 && PlayerWorldPos(&p)) { p.x += 4.0f; SpawnNpc(key, p, type); } else Log("usage: npc <characterKey> [type]");
    }
    else if (cmd == "thumbs on" || cmd == "thumbs off") { thumbgen::SetBackground(cmd == "thumbs on"); Log("background preview generation %s", thumbgen::Background() ? "on" : "off"); }
    else if (cmd.rfind("save ", 0) == 0) SaveProject(cmd.substr(5));
    else if (cmd.rfind("load ", 0) == 0) LoadProject(cmd.substr(5), false);
    else if (cmd == "projects") { for (auto& p : ListProjects()) Log("  %s", p.c_str()); }
    else if (cmd == "help") Log("commands: pos | status | list | spawn <prefab> [dx dy dz] | move <idx> x y z [yaw] [scale] | hide <idx> | save <name> | load <name> | projects | flags a b c | livemode <0..3> | livedrag on/off | recreate on/off | gimmicks on/off | captures | replays | replayprefab [path] | replay <id> x y z | removereplay <hex actor> | trace on/off | tp x y z | camtrace | fovtrace | raytrace | probe | camwatch [s] [mode] | traceio on/off | thumbs on/off | npc <key> [type] | quit");
    else if (cmd == "quit") return true;
    else if (!cmd.empty()) Log("unknown command '%s'", cmd.c_str());
    return false;
}
static DWORD WINAPI ConsoleThread(LPVOID) {
    AllocConsole();
    FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleTitleA("cdmodkit console");
    g_console = true;
    Log("cdmodkit console ready. base=%p. type 'help'. Insert = editor overlay", (void*)g_base);
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        std::string cmd(line); while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
        if (ConsoleDispatch(cmd)) break;
    }
    return 0;
}

static bool g_buildOk = false;
static std::string g_buildMsg;
static bool ResolveGame();


// pack I/O tracing lives in diag.cpp (declared in core_internal.h)

// MinHook wrapper: the status is logged by name, and a failed hook is tried once more after a short pause (one session saw
// every hook fail at once right after the overlay had hooked Present on another thread)
static bool HookFn(void* target, void* detour, void** orig, const char* name) {
    for (int attempt = 0; attempt < 2; attempt++) {
        ReleaseHookPiece();
        MH_STATUS c = MH_CreateHook(target, detour, orig);
        if (c == MH_ERROR_ALREADY_CREATED) c = MH_OK;
        MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : MH_OK;
        if (c == MH_OK && (e == MH_OK || e == MH_ERROR_ENABLED)) { if (attempt) Log("hook %s ok on the second attempt", name); return true; }
        Log("hook %s failed: create %s, enable %s%s", name, MH_StatusToString(c), MH_StatusToString(e), attempt ? "" : " (retrying)");
        Sleep(300);
    }
    return false;
}

bool InstallInternalHook(void* target, void* detour, void** original, const char* name) {
    return HookFn(target, detour, original, name);
}
// MinHook needs a free page within 2 GB of the hooked game function for its trampolines. The game fills the address space
// around its 385 MB image during the first seconds; by the time the signatures are resolved (~9 s) there may be no gap left
// and every MH_CreateHook returns MH_ERROR_MEMORY_ALLOC (seen twice). So the plugin reserves such a gap at attach, before
// the game allocates, and releases it right before the hooks are created: MinHook's search then finds exactly that hole.
// Reserved as 64 separate 64 KB pieces so that one piece can be handed to MinHook right before each hook is created: releasing
// the whole gap at once left it open for seconds while the vtable tracers were installed, and the game grabbed it (seen).
static const int kHookPieces = 64; static void* g_hookPieces[kHookPieces] = {}; static int g_hookPieceNext = 0;
static void ReserveHookGap() {
    int got = 0;
    auto tryAt = [&](uintptr_t a) { for (int i = 0; i < kHookPieces; i++) { void* r = VirtualAlloc((void*)(a + (uintptr_t)i * 0x10000), 0x10000, MEM_RESERVE, PAGE_NOACCESS); if (!r) { for (int j = 0; j < i; j++) { VirtualFree(g_hookPieces[j], 0, MEM_RELEASE); g_hookPieces[j] = nullptr; } return false; } g_hookPieces[i] = r; } got = kHookPieces; return true; };
    for (uintptr_t a = g_base - 0x1000000; a > g_base - 0x70000000ull; a -= 0x400000) if (tryAt(a)) { Log("hook gap reserved: %d x 64 KB at %p (%.0f MB below the image)", got, g_hookPieces[0], (g_base - (uintptr_t)g_hookPieces[0]) / 1048576.0); return; }
    for (uintptr_t a = g_base + 0x20000000; a < g_base + 0x70000000ull; a += 0x400000) if (tryAt(a)) { Log("hook gap reserved: %d x 64 KB at %p (above the image base)", got, g_hookPieces[0]); return; }
    Log("hook gap: nothing free within 2 GB of the image at attach time");
}
static void ReleaseHookPiece() {   // one piece per hook creation; MinHook takes a whole 64 KB region for each of its trampoline blocks
    if (g_hookPieceNext >= kHookPieces || !g_hookPieces[g_hookPieceNext]) return;
    VirtualFree(g_hookPieces[g_hookPieceNext], 0, MEM_RELEASE); g_hookPieces[g_hookPieceNext] = nullptr; g_hookPieceNext++;
}
static void ReleaseHookGap() { ReleaseHookPiece(); }
static DWORD WINAPI InitThread(LPVOID) {
    if (MH_Initialize() != MH_OK) { Log("MinHook init failed"); return 0; }
    InstallIoTrace();
    overlay::Install();          // first: must be in place before the game creates its swapchain
    LoadPrefabs();
    if (g_httpEnabled) httpapi::Start(g_httpPort);   // opt-in; before ResolveGame on purpose: /api/status reports a failed build, writes answer 503
    thumbgen::Start();           // background: renders prefab previews from the pack files into bin64\cdmodkit\thumbs
    g_buildOk = ResolveGame();
    if (!g_buildOk) { Log("signature resolution failed; game functions will NOT be hooked or called"); return 0; }
    ReleaseHookGap();
    uintptr_t target = g_base + kRva_CreateSceneObjectFrom;
    {
        if (HookFn((void*)target, (void*)HookCreate, (void**)&g_origCreate, "createSceneObjectFrom")) Log("hooked createSceneObjectFrom at %p", (void*)target);
    }
    if (kRva_ResLoad) {   // capture the resource loader instance from the game's own load calls
        void* t = (void*)(g_base + kRva_ResLoad);
        HookFn(t, (void*)HookResLoad, (void**)&g_origResLoad, "ResourceLoader.load");
    }
    {   // tracing hooks (inactive unless "trace on"); our own calls pass through them as well
        void* t1 = (void*)(g_base + kRva_SetWorldTransform); void* t2 = (void*)(g_base + kRva_SetEnable);
        HookFn(t1, (void*)HookSetXf, (void**)&g_origSetXf, "setWorldTransform (trace)");
        HookFn(t2, (void*)HookSetEnable, (void**)&g_origSetEnable, "setEnable (trace)");
        if (kRva_CastRay) { void* t3 = (void*)(g_base + kRva_CastRay); HookFn(t3, (void*)HookCastRay, (void**)&g_origCastRay, "castRay (trace)"); }
        if (kRva_CastShape) { void* t5 = (void*)(g_base + kRva_CastShape); HookFn(t5, (void*)HookCastShape, (void**)&g_origCastShape, "castShape (trace)"); }
        if (kRva_WorldCastShape) { void* t6 = (void*)(g_base + kRva_WorldCastShape); HookFn(t6, (void*)HookWorldCastShape, (void**)&g_origWorldCastShape, "worldCastShape (trace)"); }
        if (kRva_GimmickSpawn) {   // ServerField slot 9 is the server tick our spawns and removals run from; the other tracers are research (settings.txt trace_hooks=1)
            InstallVtableTracer(2, ".?AVServerField@pa@@", "ServerField", g_traceHooks ? 39 : 10);
            if (g_traceHooks) { InstallVtableTracer(0, ".?AVServerSyncSceneObjectManager@pa@@", "ServerSyncSceneObjectManager", 19); InstallVtableTracer(1, ".?AVSceneObjectServer@pa@@", "SceneObjectServer", 156); InstallVtableTracer(3, ".?AVServerNormalInGameActor@pa@@", "ServerNormalInGameActor", 145); InstallVtableTracer(4, ".?AVTrocTrSpawnCharacterCheatReq@pa@@", "TrocTrSpawnCharacterCheatReq", 3); }
        }   // ServerField: which slots run per tick (a place to run our own spawns) and which run on a pickup (removal)   // research: how the server makes and fills its scene objects
        ResolveNativeCamera();   // gizmo projection and free camera use the renderer's own camera
        ResolveSetCamPose(); if (kRva_SetCamPose && g_natCamGlobal) { void* t14 = (void*)(g_base + kRva_SetCamPose); HookFn(t14, (void*)HookSetCamPose, (void**)&g_origSetCamPose, "camera pose (free camera)"); }
        InstallNpcSpawn();   // NPC spawn research: the game's spawn-character cheat request
        EnvironmentInstall(); // optional time-of-day / weather bridge; failures do not affect the editor or spawning
        TerrainInstall();     // optional terrain editing through the streamed height textures; failures only disable it
        TravelInstall();      // optional: the game's own fast travel to any position (travel tab, terrain apply)
        TerrainPhysInstall(); // optional: live terrain edits reach the collision patches (before the world streams: every patch is seen)
        PreloadAutoloadTerrain();   // strokes of the autoload projects before the world streams: no apply needed at startup
        if (g_traceHooks && kRva_UuidLookup) { void* t10 = (void*)(g_base + kRva_UuidLookup); HookFn(t10, (void*)HookUuidLookup, (void**)&g_origUuidLookup, "uuid lookup (trace)"); }
        if (kRva_SoServerCreate) { void* t9 = (void*)(g_base + kRva_SoServerCreate); HookFn(t9, (void*)HookSoServerCreate, (void**)&g_origSoServerCreate, "SceneObjectServer new (trace)"); }
        if (kRva_ActorCtor) { void* t13 = (void*)(g_base + kRva_ActorCtor); HookFn(t13, (void*)HookActorCtor, (void**)&g_origActorCtor, "actor constructor (trace)"); }
        if (g_traceHooks && kRva_RemovalLoop) { void* t12 = (void*)(g_base + kRva_RemovalLoop); HookFn(t12, (void*)HookRemovalLoop, (void**)&g_origRemovalLoop, "actor removal loop (trace)"); }
        if (kRva_ActorCreateInner) { void* t11 = (void*)(g_base + kRva_ActorCreateInner); HookFn(t11, (void*)HookActorInner, (void**)&g_origActorInner, "actor create inner (trace)"); }
        if (kRva_ActorCreateCore) { void* t8 = (void*)(g_base + kRva_ActorCreateCore); HookFn(t8, (void*)HookActorCore, (void**)&g_origActorCore, "actor create core (trace)"); }
        if (kRva_GimmickSpawn) { void* t7 = (void*)(g_base + kRva_GimmickSpawn); HookFn(t7, (void*)HookGimmickSpawn, (void**)&g_origGimmickSpawn, "gimmick spawn (trace)"); }
        if (kRva_WorldCastRay) { void* t4 = (void*)(g_base + kRva_WorldCastRay); if (HookFn(t4, (void*)HookWorldCastRay, (void**)&g_origWorldCastRay, "worldCastRay (trace)")) Log("resolved worldCastRay        rva 0x%llx", (unsigned long long)kRva_WorldCastRay); }
    }
    PlayModeInstall();           // only with a consumed play mode request
    uintptr_t pump = FindPattern("48 8B C4 4C 89 48 ?? 48 89 50 ?? 55 41 56");
    if (pump) {
        ReleaseHookPiece();
        if (MH_CreateHook((void*)pump, (void*)HookPump, (void**)&g_origPump) == MH_OK && MH_EnableHook((void*)pump) == MH_OK)
            Log("hooked movement tick (pump) at %p (rva 0x%llx)", (void*)pump, (unsigned long long)(pump - g_base));
        else Log("hook pump failed");
    } else Log("pump pattern not found");
    return 0;
}

// ---- runtime signature resolution (survives game updates as long as the prologues stay) ----
// counts pattern hits in executable sections; returns the first hit
static uintptr_t FindPatternCount(const char* pat, int* count) {
    auto p = ParsePattern(pat); uintptr_t first = 0; int n = 0;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* s = (uint8_t*)(g_base + sec[i].VirtualAddress); size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + p.size() <= sz; k++) {
            if (p[0] >= 0 && s[k] != p[0]) continue;
            bool ok = true; for (size_t j = 1; j < p.size(); j++) if (p[j] >= 0 && s[k + j] != p[j]) { ok = false; break; }
            if (ok) { if (!first) first = (uintptr_t)(s + k); n++; if (n > 64) break; }
        }
    }
    *count = n; return first;
}
static bool ResolveSig(const char* name, const char* sig, uintptr_t* outRva, bool mustBeUnique = true) {
    int n = 0; uintptr_t hit = FindPatternCount(sig, &n);
    if (!hit || (mustBeUnique && n != 1)) { char b[160]; snprintf(b, sizeof b, "%s: %d matches", name, n); g_buildMsg = b; Log("RESOLVE FAILED %s", b); return false; }
    *outRva = hit - g_base; Log("resolved %-20s rva 0x%llx", name, (unsigned long long)*outRva); return true;
}
// function start for an address inside it, via the PE exception directory (chained unwind info is followed to the primary)
static uintptr_t FuncStartOf(uintptr_t rva) {
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* rf = (RUNTIME_FUNCTION*)(g_base + dir.VirtualAddress); size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (rf[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid; }
    if (lo == 0) return 0;
    RUNTIME_FUNCTION cur = rf[lo - 1];
    if (!(cur.BeginAddress <= rva && rva < cur.EndAddress)) return 0;
    for (int i = 0; i < 16; i++) {
        // MinGW names the same RUNTIME_FUNCTION field UnwindData.
#ifdef __MINGW32__
        uint8_t* ui = (uint8_t*)(g_base + cur.UnwindData);
#else
        uint8_t* ui = (uint8_t*)(g_base + cur.UnwindInfoAddress);
#endif
        uint8_t flags = ui[0] >> 3, codes = ui[2];
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        memcpy(&cur, ui + 4 + ((codes + 1) & ~1) * 2, sizeof cur);
    }
    return cur.BeginAddress;
}
// find `lea r64, [rip+disp]` instructions that reference `target`; returns the function containing the first one
static uintptr_t FuncReferencingString(const char* s) {
    uintptr_t str = 0;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(s) + 1;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !str; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        uint8_t* b = (uint8_t*)(g_base + sec[i].VirtualAddress); size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + len <= sz; k++) if (b[k] == (uint8_t)s[0] && memcmp(b + k, s, len) == 0) { str = (uintptr_t)(b + k); break; }
    }
    if (!str) return 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = (uint8_t*)(g_base + sec[i].VirtualAddress); size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 7 <= sz; k++) {
            if (b[k] != 0x48 && b[k] != 0x4C) continue;
            if (b[k + 1] != 0x8D || (b[k + 2] & 0xC7) != 0x05) continue;         // lea reg, [rip+disp32]
            int32_t disp; memcpy(&disp, b + k + 3, 4);
            if ((uintptr_t)(b + k + 7) + disp == str) return FuncStartOf((uintptr_t)(b + k) - g_base);
        }
    }
    return 0;
}
// The character's ground probe hands its hits to a collector class that carries no RTTI (verified against the exe:
// the slot before its vtable does not lead to a type descriptor, while the neighbouring Havok collectors do have one).
// It can therefore not be recognised by name like every other class in here, and an RVA would be wrong after the next
// patch. Instead one of its own methods is located by signature and the vtable is derived from the pointer to it:
//   method (slot 5, with the call / jump displacements masked) -> qword pointing at it in the read-only data -> vtable
// and the vtable is only accepted when its shape matches (12 code pointers, slot0 == slot9, slot3 == slot7, slot6 == slot8).
// Both steps were dry-run against build 2944 offline: one match each, giving 0x5d12ed8 - the value that used to be hardcoded.
// Nothing is guessed: no match means no template, which leaves snap to ground switched off instead of replaying an
// unverified collector. settings.txt probe_vtable=<rva> overrides the result by hand.
static bool InCode(uintptr_t p) {
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const uintptr_t start = g_base + sec[i].VirtualAddress;
        if (p >= start && p < start + sec[i].Misc.VirtualSize) return (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    return false;
}
static bool ProbeVtableShapeOk(uintptr_t vt, uintptr_t method, int slot) {
    uintptr_t f[12];
    for (int i = 0; i < 12; i++) if (!ReadPtr(vt + (uintptr_t)i * 8, &f[i]) || !InCode(f[i])) return false;
    return f[0] == f[9] && f[3] == f[7] && f[6] == f[8] && f[slot] == method;
}
static void ResolveProbeCollectorVtable() {
    constexpr int kSlot = 5;
    int n = 0;
    const uintptr_t method = FindPatternCount("48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 3C 01 0F 84 ?? ?? ?? ?? C5 FB 10 43 10 C5 F9 2F 47 30", &n);
    if (!method || n != 1) { Log("[probe] collector method: %d matches, vtable not resolved (snap to ground stays off)", n); return; }
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0; int hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        const uintptr_t* p = (const uintptr_t*)(g_base + sec[i].VirtualAddress);
        for (size_t k = kSlot, cnt = sec[i].Misc.VirtualSize / 8; k < cnt; k++) {
            if (p[k] != method) continue;
            const uintptr_t vt = (uintptr_t)(p + k - kSlot);
            if (!ProbeVtableShapeOk(vt, method, kSlot)) continue;
            if (!found) found = vt;
            hits++;
        }
    }
    if (hits != 1) { Log("[probe] collector vtable: %d candidates for method rva 0x%llx, not resolved", hits, (unsigned long long)(method - g_base)); return; }
    kRva_ProbeCollector = found - g_base;
    Log("resolved %-20s rva 0x%llx (method rva 0x%llx, via signature + vtable shape)", "probe collector", (unsigned long long)kRva_ProbeCollector, (unsigned long long)(method - g_base));
}
static bool ResolveGame() {
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    Log("exe SizeOfImage 0x%x", nt->OptionalHeader.SizeOfImage);
    bool ok = true;
    ok &= ResolveSig("setWorldTransform", "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC E0 00 00 00 C5 F8 29 70 E8 C5 F8 29 78 D8 C5 78 29 40 C8 41 0F B6 E8", &kRva_SetWorldTransform);
    ok &= ResolveSig("setEnable", "40 57 48 83 EC 20 4C 8B 89 40 01 00 00 48 8B F9 44 0F B6 C2 4D 85 C9 74 76 0F B6 81 50 01 00 00 84 C0 74 5A", &kRva_SetEnable);
    ok &= ResolveSig("StringDataAlloc", "48 89 5C 24 08 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 8B F9 41 B8 FD 01 00 00 83 C1 19 48 8B 10 41 80 3C 10 00 BA 10 00 00 00", &kRva_StringDataAlloc);
    ok &= ResolveSig("PrefabPathCtor", "48 89 5C 24 10 48 89 4C 24 08 55 56 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 01 33 ED 48 89 69 30 48 8D 41 38", &kRva_PrefabPathCtor);
    ResolveSig("ResourceLoader.load", "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC 90 00 00 00 41 8B E9 4D 8B F0 48 8B F2 48 8B F9 45 33 FF", &kRva_ResLoad);   // optional: previews via the game's loader
    ok &= ResolveSig("PathNormalizeCtor", "4C 8B DC 49 89 5B 10 49 89 4B 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 40 01 00 00 48 8B DA 48 8B F9 4C 8D 2D ?? ?? ?? ??", &kRva_PathNormalizeCtor);
    // world global: `mov rax,[rip+d]; mov rcx,[rax+0x30]; mov rax,[rbx+0xa0]; cmp [rcx+0x58],rax` (take-or-steal check, 15 copies)
    uintptr_t ref = 0;
    if (ResolveSig("WorldGlobalRef", "48 8B 05 ?? ?? ?? ?? 48 8B 48 30 48 8B 83 A0 00 00 00 48 39 41 58", &ref, false)) {
        int32_t disp; ReadBytes(g_base + ref + 3, &disp, 4);
        kRva_WorldGlobal = ref + 7 + disp; Log("resolved %-20s rva 0x%llx", "WorldGlobal", (unsigned long long)kRva_WorldGlobal);
    } else ok = false;
    kRva_CreateSceneObjectFrom = FuncReferencingString("SceneObjectManager::createSceneObjectFrom()");
    kRva_CastRay = FuncReferencingString("TtCastRay");   // hknpWorld::castRay (profiler tag inside the function)
    kRva_WorldCastRay = FuncReferencingString("TtWorldCastRay");
    kRva_CastShape = FuncReferencingString("TtCastShape"); kRva_WorldCastShape = FuncReferencingString("TtWorldCastShape");
    if (kRva_CastShape) Log("resolved castShape           rva 0x%llx (via profiler tag)", (unsigned long long)kRva_CastShape);
    if (kRva_WorldCastShape) Log("resolved worldCastShape      rva 0x%llx (via profiler tag)", (unsigned long long)kRva_WorldCastShape);
    if (kRva_CastRay) Log("resolved castRay             rva 0x%llx (via profiler tag)", (unsigned long long)kRva_CastRay);
    if (kRva_CreateSceneObjectFrom) Log("resolved %-20s rva 0x%llx (via name string + unwind table)", "createSceneObjectFrom", (unsigned long long)kRva_CreateSceneObjectFrom);
    else { ok = false; g_buildMsg = "createSceneObjectFrom: name string not referenced"; Log("RESOLVE FAILED createSceneObjectFrom"); }
    uint8_t head[4] = {0}; ReadBytes(g_base + kRva_CreateSceneObjectFrom, head, 4);
    if (ok && !(head[0] == 0x4C && head[1] == 0x89 && head[2] == 0x4C && head[3] == 0x24)) { ok = false; g_buildMsg = "createSceneObjectFrom prologue unexpected"; Log("RESOLVE FAILED: create prologue %02x %02x %02x %02x", head[0], head[1], head[2], head[3]); }
    ResolveProbeCollectorVtable();   // optional: only snap to ground depends on it
    ResolveGimmickSpawn(); ResolveSpawnCallers();           // research: traced only, nothing depends on it
    ResolveActorCore();              // research: traced only
    ResolveActorInner();             // research: traced only
    ResolveRemovalLoop();            // research: traced only
    ResolveActorCtor();              // research: traced only
    ResolveSoServerCreate();         // research: traced only
    ResolveUuidLookup();             // research: traced only
    ResolveGimmickFromSave();        // research: template-free gimmick spawn
    if (g_probeVtableOverride) { kRva_ProbeCollector = g_probeVtableOverride; Log("[probe] collector vtable overridden by settings.txt: rva 0x%llx", (unsigned long long)kRva_ProbeCollector); }
    return ok;
}
bool BuildOk() { return g_buildOk; }
const char* BuildMessage() { return g_buildMsg.c_str(); }

// Game build number, read from the exe's version resource. Purely informational: every address is resolved by signature, so
// the mod never decides anything from this. It is the first line of a bug report after a game patch ("which build are you on?").
static std::string g_gameVersion;
const char* GameVersion() { return g_gameVersion.c_str(); }
static void ReadGameVersion() {
    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    DWORD ignored = 0; const DWORD sz = GetFileVersionInfoSizeA(exe, &ignored);
    if (!sz || sz > (1u << 20)) return;
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoA(exe, 0, sz, buf.data())) return;
    VS_FIXEDFILEINFO* fi = nullptr; UINT n = 0;
    if (!VerQueryValueA(buf.data(), "\\", (LPVOID*)&fi, &n) || !fi || n < sizeof *fi) return;
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    g_gameVersion = b;
}

// Fault logging. First chance: the handler sees every OS error exception in the process, also the ones other mods catch on
// purpose (guarded reads of their own) - those filled the old 40-line budget and would have hidden a later real crash.
// Logged now: only our faults (FaultIsOurs), each address once. Unhandled: the exception nobody caught, i.e. the crash
// itself, from any module, logged as [crash].
static void LogFaultContext(const char* tag, EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr; char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m)) GetModuleFileNameA(m, mod, MAX_PATH);
    Log("[%s] 0x%08x at %p (%s+0x%llx) thread %lu addr %p", tag, code, (void*)at, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod,
        (unsigned long long)(at - (uintptr_t)m), GetCurrentThreadId(), ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
    {   // the call chain from the faulting context (image-relative), and the argument registers: which caller handed the bad pointer down
        CONTEXT c = *ep->ContextRecord; char chain[600] = { 0 }; int k = 0;
        Log("[%s] rcx=%p rdx=%p r8=%p r9=%p rax=%p rbx=%p rsi=%p rdi=%p rbp=%p rsp=%p", tag, (void*)c.Rcx, (void*)c.Rdx, (void*)c.R8, (void*)c.R9, (void*)c.Rax, (void*)c.Rbx, (void*)c.Rsi, (void*)c.Rdi, (void*)c.Rbp, (void*)c.Rsp);
        for (int i = 0; i < 24 && c.Rip; i++) {
            DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
            if (!rf) { k += snprintf(chain + k, sizeof chain - k, " [leaf %llx]", (unsigned long long)(InImage(c.Rip) ? c.Rip - g_base : c.Rip)); c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }
            else { void* hd = nullptr; DWORD64 est = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &est, nullptr); }
            if (!c.Rip || k > (int)sizeof chain - 24) break;
            k += snprintf(chain + k, sizeof chain - k, " %llx", (unsigned long long)(InImage(c.Rip) ? c.Rip - g_base : c.Rip));
        }
        Log("[%s] chain:%s", tag, chain);
    }
}
static uintptr_t g_selfBase = 0, g_selfEnd = 0;   // our own image, set at attach
// Our fault = the faulting address or one of the next few callers is in cdmodkit.asi. That also catches a game function we
// called with bad data (the fault itself is then in the exe). Only the top frames count: the pump and Present hooks sit
// below every game tick and frame, so deeper frames would claim the game's own faults as ours.
static bool FaultIsOurs(EXCEPTION_POINTERS* ep) {
    if (!g_selfBase) return true;
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 8 && c.Rip; i++) {
        if (c.Rip >= g_selfBase && c.Rip < g_selfEnd) return true;
        DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &ib, nullptr);
        if (!rf) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }   // leaf: return address on top (no IsBadReadPtr: it faults itself and would re-enter this handler)
        else { void* hd = nullptr; DWORD64 est = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &est, nullptr); }
    }
    return false;
}
static volatile LONG64 g_faultSites[64];   // fault addresses already logged (lock-free: the handler can run on any thread at once)
static volatile LONG g_faultRepeats = 0, g_faultForeign = 0;
static LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // every error-class OS exception (severity bits 11, customer bit clear: access violation, illegal instruction, in-page
    // error, alignment, ...) plus breakpoints; C++ throws (0xE06D7363 MSVC, 0x20474343 GCC) and debugger codes pass through
    const bool osError = (code & 0xE0000000u) == 0xC0000000u;
    if (!osError && code != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
#ifndef _MSC_VER
    if (osError && code != EXCEPTION_STACK_OVERFLOW) cdk::GuardDispatch(ep);   // does not return when a guard is active on this thread (MSVC: __except catches the same set)
#endif
    if (t_guardedRead) return EXCEPTION_CONTINUE_SEARCH;
    if (!FaultIsOurs(ep)) { InterlockedIncrement(&g_faultForeign); return EXCEPTION_CONTINUE_SEARCH; }   // other mods and the game: only an unhandled one matters, CrashFilter logs it
    const LONG64 at = (LONG64)ep->ExceptionRecord->ExceptionAddress; int slot = -1;
    for (int i = 0; i < 64; i++) {
        const LONG64 v = g_faultSites[i];
        if (v == at) { InterlockedIncrement(&g_faultRepeats); return EXCEPTION_CONTINUE_SEARCH; }   // known site: counted, not logged again
        if (v == 0 && InterlockedCompareExchange64(&g_faultSites[i], at, 0) == 0) { slot = i; break; }
        if (g_faultSites[i] == at) { InterlockedIncrement(&g_faultRepeats); return EXCEPTION_CONTINUE_SEARCH; }   // another thread claimed it with this address
    }
    if (slot < 0) return EXCEPTION_CONTINUE_SEARCH;   // 64 distinct sites logged: the [crash] line still comes from the unhandled filter
    LogFaultContext("fault", ep);
    if (slot == 0) Log("[fault] (first-chance, caught or not: only faults with cdmodkit.asi near the top of the call chain, each address once; a real crash from anywhere is logged as [crash])");
    return EXCEPTION_CONTINUE_SEARCH;
}
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter = nullptr;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG s_once = 0;
    if (InterlockedExchange(&s_once, 1) == 0) { LogFaultContext("crash", ep); Log("[crash] unhandled, the game terminates (not logged before: %ld repeats of our faults, %ld faults outside cdmodkit)", g_faultRepeats, g_faultForeign); }
    return g_prevCrashFilter ? g_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;   // the game's own crash reporter still runs
}
// The game may install its own filter after our attach, which replaces ours: called again once it is running, then chained.
static void InstallCrashFilter(const char* when) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev == CrashFilter) return;
    if (g_prevCrashFilter && prev != g_prevCrashFilter) Log("crash filter re-installed %s (the game had replaced it), chained to %p", when, (void*)prev);
    g_prevCrashFilter = prev;
}

static void Attach(HMODULE h) {
    g_self = h;
    g_selfBase = (uintptr_t)h; { auto dos = (const IMAGE_DOS_HEADER*)h; auto nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)h + dos->e_lfanew); g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage; }
    AddVectoredExceptionHandler(1, VectoredHandler);
    InstallCrashFilter("at attach");
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    g_modDir = DirOf(h) + "\\cdmodkit";
    CreateDirectoryA(g_modDir.c_str(), nullptr);
    g_log = fopen((g_modDir + "\\cdmodkit.log").c_str(), "a");
    ReadGameVersion();
    Log("cdmodkit.asi v0.97 attached, base=%p, game build %s", (void*)g_base, g_gameVersion.empty() ? "unknown" : g_gameVersion.c_str());
    LoadSettings();
    PlayModeLoad();              // bin64\cdmodkit\playmode.json: consumed here, before the game reads its save
    ReserveHookGap();            // before the game fills the address space around its image (see ReserveHookGap)
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (g_showConsole) CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, nullptr); else Log("console window hidden (settings.txt console=0)");
}

} // namespace core

// ---- public C API for other mods (see cdmodkit_api.h) ----
extern "C" {
__declspec(dllexport) uint32_t cdk_version(void) { return 1; }
__declspec(dllexport) int cdk_ready(void) { return core::HooksReady() && core::GameThreadReady() ? 1 : 0; }
__declspec(dllexport) int cdk_player_pos(float* xyz) { Vec3 p; if (!core::PlayerWorldPos(&p)) return 0; xyz[0] = p.x; xyz[1] = p.y; xyz[2] = p.z; return 1; }
__declspec(dllexport) int cdk_spawn(const char* prefab, float x, float y, float z, float yawDeg, float scale) {
    if (!prefab || !core::GameThreadReady()) return -1;
    int id = (int)core::Spawned().size();           // the entry is appended on the game thread in this order
    core::SpawnAt(prefab, { x, y, z }, Rot{ yawDeg }, scale);
    return id;
}
__declspec(dllexport) int cdk_move(int id, float x, float y, float z, float yawDeg, float scale) { return core::MoveSpawned((size_t)id, { x, y, z }, Rot{ yawDeg }, scale, true) ? 1 : 0; }
__declspec(dllexport) int cdk_remove(int id) { return core::HideSpawned((size_t)id) ? 1 : 0; }
__declspec(dllexport) int cdk_count(void) { return (int)core::Spawned().size(); }
__declspec(dllexport) int cdk_get(int id, char* prefabOut, int prefabCap, float* xyz, float* yawDeg, float* scale, int* hidden) {
    auto l = core::Spawned(); if (id < 0 || id >= (int)l.size()) return 0;
    const auto& o = l[id];
    if (prefabOut && prefabCap > 0) strncpy_s(prefabOut, prefabCap, o.prefab.c_str(), _TRUNCATE);
    if (xyz) { xyz[0] = o.pos.x; xyz[1] = o.pos.y; xyz[2] = o.pos.z; }
    if (yawDeg) *yawDeg = o.rot.yaw; if (scale) *scale = o.scale; if (hidden) *hidden = o.hidden ? 1 : 0;
    return 1;
}
__declspec(dllexport) void cdk_log(const char* text) { core::Log("[api] %s", text ? text : ""); }
}

void CdImguiAssert(const char* expr, const char* file, int line) {
    static int n = 0; if (n++ < 50) core::Log("[imgui assert] %s  (%s:%d)", expr, strrchr(file, '\\') ? strrchr(file, '\\') + 1 : file, line);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const char* exe = strrchr(buf, '\\'); exe = exe ? exe + 1 : buf;
        if (_stricmp(exe, "CrimsonDesert.exe") != 0) return TRUE;   // ASI loader also lands in crashpad_handler.exe
        core::Attach(h);
    }
    return TRUE;
}
