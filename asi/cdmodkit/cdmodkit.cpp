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
#include <map>
#include <set>
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

namespace core {

uintptr_t g_base = 0;
bool      g_menuOpen = false;
bool      g_uiWantsMouse = false;
bool      g_uiWantsKeyboard = false;
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
    return false;
}
bool WriteBytes(uintptr_t a, const void* src, size_t n) {
    if (a < 0x10000 || (a >> 47) != 0) return false;
    CDK_GUARD_BEGIN memcpy((void*)a, src, n); return true;
    CDK_GUARD_FAIL return false;
    CDK_GUARD_END
    return false;
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
bool ReadPos(uintptr_t actor, PosInfo* out) {
    uintptr_t comps = Deref(actor, kOff_Ent_Comps); uintptr_t tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    if (!tf) return false;
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
        static bool s_parentLogged = false; if (!s_parentLogged) { s_parentLogged = true; Log("player transform has a parent (mounted?): local (%.2f %.2f %.2f) sector %d,%d parent (%.2f %.2f %.2f) sector %d,%d -> world (%.2f %.2f %.2f)", v[0], v[1], v[2], tile[0], tile[1], px, py, pz, psec[0], psec[1], wx, wy, wz); }
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
bool PlayerPosInfo(PosInfo* out) { uintptr_t a = PlayerActor(); return a && ReadPos(a, out); }
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
static void EnqueueGimmick(int uid, const std::string& prefab, Vec3 pos, Rot rot, float scale);
static void RunOnServerTick(std::function<void()> f);
static void RemoveSpawnedActor(uintptr_t actor);
static bool MoveGimmick(size_t idx, Vec3 pos, Rot rot, float scale, bool final);
static void* __fastcall HookCreate(void* mgr, void* tag, void* b, void* c, void* d, void* transform, uint8_t f1, uint8_t f2, uint8_t f3) {
    long n = InterlockedIncrement(&g_createCalls);
    bool log = g_createLogged < 20 || g_inOurSpawn || g_trace;
    if (log) g_createLogged++;
    if (!g_inOurSpawn) g_lastMgr = mgr;
    uintptr_t retAddr = (uintptr_t)_ReturnAddress();
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
static int g_nextUid = 1, g_nextGroup = 1;
std::vector<SpawnedObj> Spawned() { std::lock_guard<std::mutex> l(g_regMutex); return g_reg; }
void ForgetSpawned(size_t idx) { std::lock_guard<std::mutex> l(g_regMutex); if (idx < g_reg.size()) g_reg.erase(g_reg.begin() + idx); }
// A project is "dirty" once one of its objects was moved, deleted or regrouped since it was loaded or saved; the scene
// tabs mark that with a star. Guarded by g_regMutex together with the registry it describes.
static std::set<int> g_projDirty;
static bool g_loading = false;          // LoadProject is running: the objects it spawns must not mark the project dirty
static void MarkDirtyLocked(int proj) { if (proj > 0) g_projDirty.insert(proj); }
static int IndexOfUidLocked(int uid) { for (size_t i = 0; i < g_reg.size(); i++) if (g_reg[i].uid == uid) return (int)i; return -1; }
int IndexOfUid(int uid) { std::lock_guard<std::mutex> l(g_regMutex); return IndexOfUidLocked(uid); }
void SetGroup(int uid, int group) { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid); if (i >= 0) { g_reg[i].group = group; MarkDirtyLocked(g_reg[i].proj); } }
int NewGroupId() { std::lock_guard<std::mutex> l(g_regMutex); return g_nextGroup++; }
void ForgetUid(int uid) { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid); if (i >= 0) g_reg.erase(g_reg.begin() + i); }

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
static bool CheckSO(uintptr_t obj, const char* what) {
    const char* n = RttiName(obj);
    if (!n || !strstr(n, "SceneObject")) { Log("%s: %p is not a SceneObject any more (%s)", what, (void*)obj, n ? n : "?"); return false; }
    return true;
}
static bool DoRemove(uintptr_t obj) {
    if (!CheckSO(obj, "remove")) return false;
    ((SetEnableFn)(g_base + kRva_SetEnable))((void*)obj, 0);
    uint8_t fl = 0; ReadBytes(obj + 0xFD, &fl, 1);
    Log("remove: %p setEnable(0) done, flags+0xFD=0x%02x", (void*)obj, fl);
    return true;
}
static void* DoSpawn(std::string prefab, Vec3 pos, Rot rot, float scale, int registerUid);
// Move variant A: disable, set transform, enable (the housing sequence). Variant B: remove + re-create.
static void DoMoveInPlace(uintptr_t obj, Vec3 pos, Rot rot, float scale) {
    if (!CheckSO(obj, "move")) return;
    alignas(16) float xf[12]; MakeTransform(xf, pos, rot, scale);
    auto setEnable = (SetEnableFn)(g_base + kRva_SetEnable);
    setEnable((void*)obj, 0);
    ((SetWorldTransformFn)(g_base + kRva_SetWorldTransform))((void*)obj, xf, 0, 1);
    setEnable((void*)obj, 1);
    Log("move: %p -> (%.2f %.2f %.2f) yaw %.0f tilt %.0f/%.0f scale %.2f (disable/set/enable)", (void*)obj, pos.x, pos.y, pos.z, rot.yaw, rot.pitch, rot.roll, scale);
}
extern volatile LONG g_queueCount;

// ---- reading pack files through the game's own resource loader (no decryption in the mod) ----
// ResourceLoader::load(this, Resource** out, const NormalizedPath* path, u32 flags) asks every load worker (package, local dir, ...)
// and returns a Resource whose +0x20 is a MemoryArchive {vtable, u8* data (+8), u32 size (+0x10), u32 pos (+0x14)}.
// The loader instance is captured from the game's own calls (hook on the same function), the path object is built like DoSpawn's.
static uintptr_t kRva_ResLoad = 0;
typedef void* (__fastcall* ResLoadFn)(void* self, void** out, void* path, uint32_t flags);
static ResLoadFn g_origResLoad = nullptr; static void* g_resLoader = nullptr;
static void* __fastcall HookResLoad(void* self, void** out, void* path, uint32_t flags) {
    if (!g_resLoader && self) { g_resLoader = self; Log("resource loader captured %p (%s)", self, RttiName((uintptr_t)self) ? RttiName((uintptr_t)self) : "?"); }
    return g_origResLoad(self, out, path, flags);
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
    return false;
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
static void __fastcall HookSetXf(void* obj, const float* xf, uint8_t a, uint8_t b) {
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
}
static void DoReplace(size_t idx, Vec3 pos, Rot rot, float scale) {
    uintptr_t obj = 0; std::string prefab;
    { std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size()) return; obj = g_reg[idx].obj; prefab = g_reg[idx].prefab; }
    if (obj) DoRemove(obj);
    void* r = DoSpawn(prefab, pos, rot, scale, 0);
    std::lock_guard<std::mutex> l(g_regMutex);
    if (idx < g_reg.size()) { g_reg[idx].obj = (uintptr_t)r; g_reg[idx].hidden = (r == nullptr); g_reg[idx].colRot = rot; g_reg[idx].colScale = scale; }
}
// final=false: live drag, visual only (disable/setTransform/enable). final=true: if rotation or scale changed since the
// object was created, re-create it so the collision shape (built at creation) matches; otherwise move in place.
static bool RotDiffers(const Rot& a, const Rot& b) { return fabsf(a.yaw - b.yaw) > 0.01f || fabsf(a.pitch - b.pitch) > 0.01f || fabsf(a.roll - b.roll) > 0.01f; }
bool MoveSpawned(size_t idx, Vec3 pos, Rot rot, float scale, bool final) {
    if (MoveGimmick(idx, pos, rot, scale, final)) return true;
    uintptr_t obj = 0; bool needRecreate = false;
    { std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size()) return false; obj = g_reg[idx].obj;
      g_reg[idx].pos = pos; g_reg[idx].rot = rot; g_reg[idx].scale = scale; if (final) MarkDirtyLocked(g_reg[idx].proj);
      needRecreate = RotDiffers(g_reg[idx].colRot, rot) || fabsf(g_reg[idx].colScale - scale) > 0.001f; }
    if (!GameThreadReady()) return false;
    if (final && (g_recreateOnMove || needRecreate || !obj)) RunOnGameThread([idx, pos, rot, scale]() { DoReplace(idx, pos, rot, scale); });
    else if (obj && final) RunOnGameThread([obj, pos, rot, scale]() { DoMoveInPlace(obj, pos, rot, scale); });
    else if (obj) { if (InterlockedCompareExchange(&g_queueCount, 0, 0) > 2) return true;   // drop live updates when the game thread lags
                    const DWORD now = GetTickCount();
                    RunOnGameThread([obj, pos, rot, scale, now]() { DoLiveMove(obj, pos, rot, scale, now); }); }
    return true;
}
static void ApplyMove(size_t idx, Vec3 pos, Rot rot, float scale, bool final) {   // game thread
    if (MoveGimmick(idx, pos, rot, scale, final)) return;
    uintptr_t obj = 0; bool needRecreate = false;
    { std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size()) return; obj = g_reg[idx].obj;
      g_reg[idx].pos = pos; g_reg[idx].rot = rot; g_reg[idx].scale = scale; if (final) MarkDirtyLocked(g_reg[idx].proj);
      needRecreate = RotDiffers(g_reg[idx].colRot, rot) || fabsf(g_reg[idx].colScale - scale) > 0.001f; }
    if (final && (g_recreateOnMove || needRecreate || !obj)) DoReplace(idx, pos, rot, scale);
    else if (obj && final) DoMoveInPlace(obj, pos, rot, scale);
    else if (obj) DoLiveMove(obj, pos, rot, scale, GetTickCount());
}
bool MoveMany(const std::vector<MoveReq>& reqs, bool final) {
    if (!GameThreadReady() || reqs.empty()) return false;
    if (!final && InterlockedCompareExchange(&g_queueCount, 0, 0) > 2) return false;   // drop live updates when the game thread lags
    std::vector<MoveReq> copy = reqs;
    RunOnGameThread([copy, final]() { for (auto& r : copy) { int i = IndexOfUid(r.uid); if (i >= 0) ApplyMove((size_t)i, r.pos, r.rot, r.scale, final); } });
    return true;
}
bool HideUid(int uid) { int i = IndexOfUid(uid); return i >= 0 && HideSpawned((size_t)i); }
bool HideSpawned(size_t idx) {
    {   // an interactive object: its actor is removed on the server tick, the way the game removes a picked-up item
        std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size() || g_reg[idx].hidden) return false;
        if (g_reg[idx].gimmick) { const uintptr_t actor = g_reg[idx].actor, so = g_reg[idx].standin ? g_reg[idx].obj : 0; g_reg[idx].hidden = true; g_reg[idx].obj = 0; g_reg[idx].actor = 0; g_reg[idx].standin = false; MarkDirtyLocked(g_reg[idx].proj); if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); }); if (so && GameThreadReady()) RunOnGameThread([so]() { DoRemove(so); }); return true; }
    }
    uintptr_t obj = 0;
    { std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size() || g_reg[idx].hidden) return false; obj = g_reg[idx].obj; g_reg[idx].hidden = true; g_reg[idx].obj = 0; MarkDirtyLocked(g_reg[idx].proj); }
    if (!GameThreadReady()) return false;
    RunOnGameThread([obj]() { DoRemove(obj); });
    return true;
}

// ---- our own spawn (must run on the game thread) ----
static uintptr_t SceneObjectMgr() {
    uintptr_t g = Deref(g_base + kRva_WorldGlobal, 0);
    uintptr_t w = g ? Deref(g, 0xE0) : 0;
    return w ? Deref(w, 0xEB0) : 0;
}
static uint8_t g_flags[3] = { 1, 1, 0 };   // middle flag = schedule the add-to-level task (what every dynamic spawner in the game uses)

static void* DoSpawn(std::string prefab, Vec3 pos, Rot rot, float scale, int registerUid) {
    if (!g_origCreate) { Log("spawn: hook not installed"); return nullptr; }
    uintptr_t mgr = SceneObjectMgr();
    if (!mgr && g_lastMgr) mgr = (uintptr_t)g_lastMgr;
    if (!mgr) { Log("spawn: no SceneObjectManager"); return nullptr; }
    auto rrpCtor = (void*(*)(void*, const void*))(g_base + kRva_PrefabPathCtor);
    auto sdAlloc = (uintptr_t(*)(int))(g_base + kRva_StringDataAlloc);
    auto normalize = (void*(*)(void*, const void*))(g_base + kRva_PathNormalizeCtor);
    alignas(16) uint8_t tag[32] = {0}, pathStr[32] = {0}, rrp[0x100] = {0}, arg3[64] = {0}, arg4[64] = {0}, tagBlock[0x80] = {0};
    *(uintptr_t*)tag = (uintptr_t)tagBlock;                    // tag = pointer to zeroed block (what the sector loader passes)
    uintptr_t sd = sdAlloc((int)prefab.size());                // game StringData: {char* str; i32 len; i32 hash=-1; i32 rc=1; ...}
    if (!sd) { Log("spawn: StringData alloc failed"); return nullptr; }
    strncpy_s((char*)*(uintptr_t*)sd, prefab.size() + 1, prefab.c_str(), _TRUNCATE);
    uintptr_t holder = sd;
    normalize(pathStr, &holder);
    rrpCtor(rrp, pathStr);
    alignas(16) float xf[12]; MakeTransform(xf, pos, rot, scale, false);   // creation takes world coordinates
    g_inOurSpawn = true;
    void* r = g_origCreate((void*)mgr, tag, arg3, arg4, rrp, xf, g_flags[0], g_flags[1], g_flags[2]);
    g_inOurSpawn = false;
    Log("spawn: \"%s\" at (%.2f %.2f %.2f) yaw %.0f tilt %.0f/%.0f scale %.2f -> %p (%s)", prefab.c_str(), pos.x, pos.y, pos.z, rot.yaw, rot.pitch, rot.roll, scale, r, r && RttiName((uintptr_t)r) ? RttiName((uintptr_t)r) : "?");
    if (registerUid) {
        bool discarded = false;
        { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(registerUid);
          if (i >= 0 && !g_reg[i].hidden && !(g_reg[i].gimmick && !g_reg[i].standin)) { g_reg[i].obj = (uintptr_t)r; if (!r) g_reg[i].hidden = true; }
          else discarded = true; }
        if (discarded && r) DoRemove((uintptr_t)r);   // a queued spawn may finish after an HTTP client hides or forgets it, or a stand-in after the drag already ended
    }
    return r;
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
void RunOnGameThread(std::function<void()> f) { std::lock_guard<std::mutex> l(g_qMutex); g_queue.push_back(std::move(f)); InterlockedIncrement(&g_queueCount); }
bool GameThreadReady() { return g_origPump && g_gameThread; }
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
static void PumpJobs() {
    g_pumpTicks++; g_gameThread = GetCurrentThreadId();
    if (g_trace) TraceTick();
    if ((g_pumpTicks & 15) == 0) AutoloadTick();
    if (InterlockedCompareExchange(&g_queueCount, 0, 0) == 0) return;
    std::function<void()> job;
    { std::lock_guard<std::mutex> l(g_qMutex); if (!g_queue.empty()) { job = std::move(g_queue.front()); g_queue.pop_front(); InterlockedDecrement(&g_queueCount); } }
    if (job) RunJobGuarded(&job);
}
static void InstallCrashFilter(const char* when);
static uint64_t HookPump(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    const uint64_t r = g_origPump(a1, a2, a3, a4, a5, a6, a7, a8);
    static bool s_filter = false; if (!s_filter) { s_filter = true; InstallCrashFilter("on the first game tick"); }
    PumpJobs();
    return r;
}

int SpawnAt(const std::string& prefab, Vec3 world, Rot rot, float scale, int group, int proj) {
    if (!GameThreadReady()) { Log("spawn: game thread pump not active yet"); return 0; }
    // a character appearance is assembled by the game's actor system; as a scene object it would spawn nothing visible
    if (prefab.size() > 8 && prefab.compare(prefab.size() - 8, 8, ".app_xml") == 0) { Log("spawn: %s is a character appearance, characters cannot be spawned yet", prefab.c_str()); return 0; }
    const bool gim =g_gimmickSpawn && kRva_GimmickSpawn_ && IsGimmickPrefab(prefab);
    int uid; { std::lock_guard<std::mutex> l(g_regMutex); uid = g_nextUid++; g_reg.push_back({ 0, prefab, world, rot, scale, false, GetTickCount(), rot, scale, uid, group, proj, gim, 0 }); if (!g_loading) MarkDirtyLocked(proj); }
    if (gim) { EnqueueGimmick(uid, prefab, world, rot, scale); return uid; }   // spawned by the server tick once a template capture exists
    std::string p = prefab;
    RunOnGameThread([p, world, rot, scale, uid]() {
        { std::lock_guard<std::mutex> l(g_regMutex); int i = IndexOfUidLocked(uid); if (i < 0 || g_reg[i].hidden) return; }
        DoSpawn(p, world, rot, scale, uid);
    });
    return uid;
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
// prefabs.tsv is linked into the plugin as a resource (scripts/pack_index.py -> cdmodkit.rc): written out when the file is
// missing next to the plugin, which happens with mod managers that install only the .asi.
static void EnsurePrefabIndexFile() {
    const std::string path = g_modDir + "\\prefabs.tsv";
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HRSRC r = FindResourceA(g_self, MAKEINTRESOURCEA(101), RT_RCDATA); if (!r) { Log("prefabs.tsv missing and no embedded copy"); return; }
    HGLOBAL h = LoadResource(g_self, r); const uint8_t* p = h ? (const uint8_t*)LockResource(h) : nullptr; const DWORD n = SizeofResource(g_self, r);
    if (!p || n < 8 || memcmp(p, "CDK1", 4) != 0) { Log("embedded prefab index unreadable"); return; }
    uint32_t raw = 0; memcpy(&raw, p + 4, 4);
    std::vector<uint8_t> out;
    if (!thumbgen::Lz4Decode(p + 8, n - 8, out, raw)) { Log("embedded prefab index: decode failed"); return; }
    FILE* f = fopen(path.c_str(), "wb"); if (!f) { Log("cannot write %s", path.c_str()); return; }
    fwrite(out.data(), 1, out.size(), f); fclose(f);
    Log("prefabs.tsv was missing: written from the embedded copy (%u bytes) -> %s", raw, path.c_str());
}
static void LoadPrefabs() {
    EnsurePrefabIndexFile();
    g_cats.push_back({ "all", -1, {}, {}, 0 });
    std::ifstream f(g_modDir + "\\prefabs.tsv"); bool tsv = f.good();
    if (!tsv) f.open(g_modDir + "\\prefabs.txt");
    std::string line; std::map<std::string, int> tagc;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        PrefabInfo pi{}; pi.meshes = pi.children = 0;
        if (tsv) {
            std::vector<std::string> col; size_t s = 0;
            while (true) { size_t t = line.find('\t', s); col.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s)); if (t == std::string::npos) break; s = t + 1; }
            if (col.size() < 5) continue;
            pi.path = col[0]; pi.tags = col[1]; pi.meshes = atoi(col[2].c_str()); pi.children = atoi(col[3].c_str()); pi.mesh = col[4];
        } else { pi.path = line.rfind("/bin__/") != std::string::npos ? line : line; size_t b = pi.path.find("/bin__/"); if (b != std::string::npos) pi.path.erase(b, 6); }
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
    Log("prefab index: %zu entries (%s), %zu categories, %zu tags, %zu favorites", g_index.size(), tsv ? "tsv" : "txt", g_cats.size(), g_tagCounts.size(), g_favs.size());
    if (g_index.empty()) Log("WARNING: no prefabs loaded. Expected %s\\prefabs.tsv (shipped in the zip as bin64\\cdmodkit\\prefabs.tsv). Mod managers such as DMM install only the .asi: copy the cdmodkit folder from the zip into bin64 by hand.", g_modDir.c_str());
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
            void* r = DoSpawn(prefab, pos, Rot{ yawDeg }, scale, 0);
            obj = (uintptr_t)r;
        }
        std::lock_guard<std::mutex> l(g_prevMutex);
        g_prev.obj = obj; g_prev.prefab = prefab; g_prev.pos = pos; g_prev.yaw = yawDeg; g_prev.scale = scale; g_prev.pending = false;
    });
}
bool PreviewCommit() {
    std::lock_guard<std::mutex> l(g_prevMutex);
    if (!g_prev.obj) return false;
    { std::lock_guard<std::mutex> r(g_regMutex); g_reg.push_back({ g_prev.obj, g_prev.prefab, g_prev.pos, Rot{ g_prev.yaw }, g_prev.scale, false, GetTickCount(), Rot{ g_prev.yaw }, g_prev.scale, g_nextUid++, 0 }); }
    Log("preview committed: %s at (%.2f %.2f %.2f)", g_prev.prefab.c_str(), g_prev.pos.x, g_prev.pos.y, g_prev.pos.z);
    g_prev.obj = 0; g_prev.prefab.clear();
    return true;
}

// ---- projects (save / load / autoload) ----
static std::string ProjDir() { return g_modDir + "\\projects"; }
static std::string ProjPath(const std::string& name) { return ProjDir() + "\\" + name + ".cdproj"; }
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
    std::lock_guard<std::mutex> l(g_regMutex);
    int n = 0; for (auto& o : g_reg) if (!o.hidden && o.proj == id) n++;
    return n;
}
bool ProjectDirty(int id) { std::lock_guard<std::mutex> l(g_regMutex); return g_projDirty.count(id) != 0; }
void AssignProject(int uid, int proj) {
    std::lock_guard<std::mutex> l(g_regMutex);
    int i = IndexOfUidLocked(uid); if (i >= 0) g_reg[i].proj = proj;
}

bool SaveProject(const std::string& rawName, int scope) {
    // spaces inside the name are fine, but leading/trailing ones are cut: autoload.txt is one name per line and trims blanks,
    // so " camp" on disk could never be autoloaded again
    size_t b = rawName.find_first_not_of(" \t"), e = rawName.find_last_not_of(" \t");
    if (b == std::string::npos) return false;
    const std::string name = rawName.substr(b, e - b + 1);
    const int pid = ProjectId(name);
    CreateDirectoryA(ProjDir().c_str(), nullptr);
    FILE* f = fopen(ProjPath(name).c_str(), "w");
    if (!f) { Log("save: cannot write %s", ProjPath(name).c_str()); return false; }
    fprintf(f, "# cdmodkit project v3: prefab|x|y|z|yawDeg|scale|group|pitchDeg|rollDeg (absolute world coordinates)\n");
    auto l = Spawned(); int n = 0; std::vector<int> written;
    for (auto& o : l) {
        if (o.hidden) continue;
        if (scope == SaveProjectAndNew && !(o.proj == pid || o.proj == 0)) continue;   // other projects stay where they are
        if (scope == SaveNewOnly && o.proj != 0) continue;
        if (scope == SaveProjectOnly && o.proj != pid) continue;   // exactly what the project tab shows
        fprintf(f, "%s|%.3f|%.3f|%.3f|%.2f|%.3f|%d|%.2f|%.2f\n", o.prefab.c_str(), o.pos.x, o.pos.y, o.pos.z, o.rot.yaw, o.scale, o.group, o.rot.pitch, o.rot.roll);
        written.push_back(o.uid); n++;
    }
    fclose(f);
    for (int uid : written) AssignProject(uid, pid);   // what was written is now part of that project
    { std::lock_guard<std::mutex> l(g_regMutex); if (scope == SaveWholeScene) g_projDirty.clear(); else g_projDirty.erase(pid); }
    Log("save: %d objects (scope %d) -> %s", n, scope, ProjPath(name).c_str());
    return true;
}
void DeleteAllSpawned() {
    { std::lock_guard<std::mutex> l(g_regMutex); g_projDirty.clear(); }   // nothing left that could differ from a file
    std::vector<uintptr_t> objs, actors;
    { std::lock_guard<std::mutex> l(g_regMutex); for (auto& o : g_reg) if (!o.hidden) { if (o.gimmick && !o.standin) { if (o.actor) actors.push_back(o.actor); } else if (o.obj) objs.push_back(o.obj); } g_reg.clear(); }
    for (auto actor : actors) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
    if (!GameThreadReady()) return;
    for (auto obj : objs) RunOnGameThread([obj]() { DoRemove(obj); });
    Log("delete all: %zu objects", objs.size());
}
static bool g_autoDone = false; static DWORD g_worldSince = 0, g_worldLast = 0;
bool LoadProject(const std::string& name, bool clearFirst) {
    FILE* f = fopen(ProjPath(name).c_str(), "r");
    if (!f) { Log("load: cannot open %s", ProjPath(name).c_str()); return false; }
    g_autoDone = true;   // a manual load (or replace) counts: the autoload must not add a second copy of the scene later
    if (clearFirst) DeleteAllSpawned();
    const int pid = ProjectId(name);   // the objects remember where they came from, so this project can be overwritten on its own
    g_loading = true;
    char line[1024]; int n = 0; std::map<int, int> groups;   // file group ids -> fresh ids
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char prefab[600] = {0}; float x, y, z, yaw = 0, sc = 1, pitch = 0, roll = 0; int grp = 0;
        char* bar = strchr(line, '|'); if (!bar) continue;
        *bar = 0; strncpy_s(prefab, line, _TRUNCATE);
        if (sscanf(bar + 1, "%f|%f|%f|%f|%f|%d|%f|%f", &x, &y, &z, &yaw, &sc, &grp, &pitch, &roll) < 3) continue;
        int g = 0; if (grp > 0) { auto it = groups.find(grp); if (it == groups.end()) it = groups.emplace(grp, NewGroupId()).first; g = it->second; }
        SpawnAt(prefab, { x, y, z }, Rot{ yaw, pitch, roll }, sc, g, pid); n++;
    }
    fclose(f);
    g_loading = false;
    { std::lock_guard<std::mutex> l(g_regMutex); g_projDirty.erase(pid); }   // freshly loaded = in sync with the file
    Log("load: %d objects queued from %s", n, ProjPath(name).c_str());
    return true;
}
std::vector<std::string> ListProjects() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((ProjDir() + "\\*.cdproj").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { std::string n = fd.cFileName; out.push_back(n.substr(0, n.size() - 7)); } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}
// autoload.txt: one project name per line (without .cdproj); lines starting with '#' and blank lines are skipped.
// '#' is only a comment at the start of a line, so a project whose name contains one still works.
// Any number of projects can be listed; they are all loaded into the same scene, in file order.
static std::string AutoloadPath() { return g_modDir + "\\autoload.txt"; }
std::vector<std::string> Autoload() {
    std::vector<std::string> out;
    FILE* f = fopen(AutoloadPath().c_str(), "r"); if (!f) return out;
    char line[512]; bool firstLine = true;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        if (firstLine) {   // Notepad can save the file as "UTF-8 with BOM"; those three bytes are not part of the name
            firstLine = false;
            if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
        }
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        s = s.substr(b);
        if (s[0] == '#') continue;
        if (s.size() > 7 && _stricmp(s.c_str() + s.size() - 7, ".cdproj") == 0) s.resize(s.size() - 7);   // a pasted file name works too
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);   // never load the same project twice
    }
    fclose(f);
    return out;
}
static void WriteAutoload(const std::vector<std::string>& list) {
    if (list.empty()) { DeleteFileA(AutoloadPath().c_str()); return; }
    FILE* f = fopen(AutoloadPath().c_str(), "w"); if (!f) { Log("autoload: cannot write %s", AutoloadPath().c_str()); return; }
    fprintf(f, "# World Builder autoload: one project name per line (without .cdproj). Lines starting with '#' are ignored.\n");
    for (auto& n : list) fprintf(f, "%s\n", n.c_str());
    fclose(f);
}
void SetAutoload(const std::string& name, bool on) {
    if (name.empty()) return;
    std::vector<std::string> list = Autoload();
    auto it = std::find(list.begin(), list.end(), name);
    if (on) { if (it == list.end()) list.push_back(name); }
    else    { if (it != list.end()) list.erase(it); }
    WriteAutoload(list);
    Log("autoload: %s %s (%zu active)", name.c_str(), on ? "on" : "off", list.size());
}
int PendingSpawns() { return (int)InterlockedCompareExchange(&g_queueCount, 0, 0); }
// called from the pump: once the player has been in the world for ~3 s, load the autoload projects once per session.
// A single failed position read (loading screen, camera cut, mount transition) only sets the counter back a little, so the
// load happens seconds after the world is up, not minutes later when the user may already have loaded the scene by hand.
static void AutoloadTick() {
    if (g_autoDone) return;
    const DWORD now = GetTickCount();
    Vec3 p; if (!PlayerWorldPos(&p)) { if (now - g_worldLast > 2000) g_worldSince = 0; return; }   // a dropout of up to 2 s keeps the clock
    g_worldLast = now; if (!g_worldSince) g_worldSince = now;
    if (now - g_worldSince < 3000) return;
    g_autoDone = true;
    // several projects are loaded into the same scene: every LoadProject only queues spawns and hands out fresh group ids,
    // so the lists simply add up (LoadProject sets g_autoDone itself, which is already true here)
    std::vector<std::string> names = Autoload();
    for (auto& name : names) { Log("autoload: loading project %s", name.c_str()); LoadProject(name, false); }
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
struct GroundReq { int id; Vec3 start; float len; bool verbose; };
static std::mutex g_groundMutex; static std::vector<GroundReq> g_groundQueue; static std::map<int, GroundHit> g_groundResults; static int g_groundNext = 0;
static volatile LONG g_groundQueued = 0;
static void ServiceGroundQueue(void* world);
static void CaptureTemplate(void* world, void* q, void* xf, void* col) {
    if (g_tpl.have && g_tpl.world == world) return;
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
    g_tpl.have = false;
    if (!ReadBytes((uintptr_t)q, g_tpl.query, sizeof g_tpl.query) || !ReadBytes((uintptr_t)xf, g_tpl.xform, sizeof g_tpl.xform) || !ReadBytes((uintptr_t)col, g_tpl.collector, sizeof g_tpl.collector)
        || !ReadBytes(hits, g_tpl.hits, sizeof g_tpl.hits) || !ReadBytes(shape, g_tpl.shape, sizeof g_tpl.shape)) return;
    g_tpl.world = world; g_tpl.collAddr = (uintptr_t)col; g_tpl.hitsAddr = hits; g_tpl.shapeAddr = shape; g_tpl.have = true;
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
// error codes of the server spawn path are hashed names (the same lookup3 the spawn reasons use); bin64\cdmodkit\errnames.txt
// lists every eErr* name found in the exe, so a code can be turned back into its name
static std::string DecodeErr(uint32_t code) {
    if (!code || !g_gameHash) return code ? "?" : "ok";
    static std::vector<std::string> names; static bool loaded = false;
    if (!loaded) { loaded = true; FILE* f = fopen((g_modDir + "\\errnames.txt").c_str(), "r"); if (f) { char line[256]; while (fgets(line, sizeof line, f)) { std::string n = line; while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back(); if (!n.empty()) names.push_back(n); } fclose(f); } }
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
// (a level streaming spawn, else a housing placement) with the prefab, position, rotation and scale swapped in. On failure the
// object falls back to a plain client object. Moves remove + respawn (final only), deletes remove the actor.
struct GimmickReq { int uid; std::string prefab; Vec3 pos; Rot rot; float scale; };
static std::deque<GimmickReq> g_gimmickQueue; static std::mutex g_gimmickQueueMutex;
static std::deque<std::function<void()>> g_serverJobs; static std::mutex g_serverJobsMutex;
static void RunOnServerTick(std::function<void()> f) { std::lock_guard<std::mutex> l(g_serverJobsMutex); g_serverJobs.push_back(std::move(f)); }
static void EnqueueGimmick(int uid, const std::string& prefab, Vec3 pos, Rot rot, float scale) { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.push_back({ uid, prefab, pos, rot, scale }); }
int GimmickPending() { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); return (int)g_gimmickQueue.size(); }
static bool MoveGimmick(size_t idx, Vec3 pos, Rot rot, float scale, bool final) {
    // While an interactive object is dragged it cannot follow the mouse (a server object only moves by remove + respawn), so
    // the first live move takes it away and puts a plain client object of the same prefab in its place; that one follows the
    // drag like any other object, and the release removes it and spawns the interactive object at the final transform.
    int uid = 0; uintptr_t actor = 0, obj = 0; std::string prefab; bool standin = false;
    { std::lock_guard<std::mutex> l(g_regMutex); if (idx >= g_reg.size() || !g_reg[idx].gimmick) return false;
      SpawnedObj& e = g_reg[idx]; e.pos = pos; e.rot = rot; e.scale = scale; uid = e.uid; actor = e.actor; obj = e.obj; prefab = e.prefab; standin = e.standin;
      if (!final) { if (!standin) { e.standin = true; e.actor = 0; e.obj = 0; } }
      else { MarkDirtyLocked(e.proj); e.standin = false; e.actor = 0; e.obj = 0; } }
    if (!final) {
        if (!standin) {
            { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); for (auto it = g_gimmickQueue.begin(); it != g_gimmickQueue.end(); ) it = it->uid == uid ? g_gimmickQueue.erase(it) : it + 1; }   // not yet spawned: it must not appear under the stand-in
            if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
            if (GameThreadReady()) RunOnGameThread([prefab, pos, rot, scale, uid]() { DoSpawn(prefab, pos, rot, scale, uid); });
        } else if (obj && GameThreadReady() && InterlockedCompareExchange(&g_queueCount, 0, 0) <= 2) { const DWORD now = GetTickCount(); RunOnGameThread([obj, pos, rot, scale, now]() { DoLiveMove(obj, pos, rot, scale, now); }); }
        return true;
    }
    if (standin && obj && GameThreadReady()) RunOnGameThread([obj]() { DoRemove(obj); });
    if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); });
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); for (auto it = g_gimmickQueue.begin(); it != g_gimmickQueue.end(); ) it = it->uid == uid ? g_gimmickQueue.erase(it) : it + 1; }   // an older pending spawn of it is void
    EnqueueGimmick(uid, prefab, pos, rot, scale);
    return true;
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
static bool FindTemplateCapture(GimmickCapture& out) {   // newest level streaming spawn, else newest housing placement, else any other non-drop spawn with a path
    std::lock_guard<std::mutex> l(g_gringMutex);
    for (uintptr_t want : { g_callerLevel, g_callerHousing, (uintptr_t)0 }) {
        int best = -1; DWORD bestWhen = 0;
        for (int k = 0; k < kGimmickRing; k++) { const GimmickCapture& c = g_gring[k]; if (!c.valid || !c.ownerPath[0] || c.caller == g_callerDrop) continue; if (want && c.caller != want) continue; if (best < 0 || (DWORD)(c.when - bestWhen) < 0x80000000u) { best = k; bestWhen = c.when; } }
        if (best >= 0) { out = g_gring[best]; return true; }
    }
    return false;
}
bool GimmickTemplateReady() { GimmickCapture c; return FindTemplateCapture(c); }
static void ProcessServerJobs() {
    for (;;) { std::function<void()> job; { std::lock_guard<std::mutex> l(g_serverJobsMutex); if (g_serverJobs.empty()) return; job = std::move(g_serverJobs.front()); g_serverJobs.pop_front(); } job(); }
}
static void ProcessGimmickQueue() {   // server thread, one object per tick
    GimmickReq r; size_t pending = 0;
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); if (g_gimmickQueue.empty()) return; r = g_gimmickQueue.front(); pending = g_gimmickQueue.size(); }
    GimmickCapture t;
    if (!FindTemplateCapture(t)) { static DWORD lastLog = 0; if (GetTickCount() - lastLog > 15000) { lastLog = GetTickCount(); Log("[gimmick] %zu interactive object%s waiting for a spawn template (the game spawns one when you walk)", pending, pending == 1 ? "" : "s"); } return; }
    { std::lock_guard<std::mutex> l(g_gimmickQueueMutex); g_gimmickQueue.pop_front(); }
    { std::lock_guard<std::mutex> l(g_regMutex); const int i = IndexOfUidLocked(r.uid); if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].standin) return; }   // deleted or being dragged meanwhile
    char saved[256]; memcpy(saved, g_replayPrefab, sizeof saved); strncpy_s(g_replayPrefab, r.prefab.c_str(), _TRUNCATE);
    float xf[12]; MakeTransform(xf, r.pos, r.rot, r.scale, false); memcpy(g_replayQuat, xf + 3, 16); g_replayScale = r.scale; g_replayUseRot = true;
    g_gimmickReplayId = t.id; g_gimmickReplayAt = r.pos;
    ReplayGimmick();
    g_replayUseRot = false; memcpy(g_replayPrefab, saved, sizeof saved);
    const uintptr_t so = g_replayResultSo, actor = g_replayResultActor;
    if (so) {
        std::lock_guard<std::mutex> l(g_regMutex); const int i = IndexOfUidLocked(r.uid);
        if (i < 0 || g_reg[(size_t)i].hidden || g_reg[(size_t)i].standin) { if (actor) RunOnServerTick([actor]() { RemoveSpawnedActor(actor); }); return; }   // deleted or picked up while spawning
        g_reg[(size_t)i].obj = so; g_reg[(size_t)i].actor = actor; g_reg[(size_t)i].colRot = r.rot; g_reg[(size_t)i].colScale = r.scale;
        Log("[gimmick] object %d spawned through the game: %s (scene object %p, actor %p)", r.uid, r.prefab.c_str(), (void*)so, (void*)actor);
    } else {
        Log("[gimmick] object %d: the game's spawn path refused %s, placing it as a plain object instead", r.uid, r.prefab.c_str());
        { std::lock_guard<std::mutex> l(g_regMutex); const int i = IndexOfUidLocked(r.uid); if (i >= 0) g_reg[(size_t)i].gimmick = false; }
        if (GameThreadReady()) RunOnGameThread([r]() { DoSpawn(r.prefab, r.pos, r.rot, r.scale, r.uid); });
    }
}
static void* __fastcall HookGimmickSpawn(void* param, void* out, void* mgr, void* owner, void* s5, void* s6, void* s7, void* s8, void* s9, void* s10, void* s11, void* s12) {
    g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount();
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
static void RemoveSpawnedActor(uintptr_t actor) {
    if (!actor || !RttiName(actor)) { Log("[remove] actor %p: no RTTI, not touched", (void*)actor); return; }
    {   // does this actor refer to the scene object our replay made? (a wrong actor must not be touched)
        uintptr_t so = 0; { std::lock_guard<std::mutex> l(g_spawnedMutex); for (const auto& g : g_spawned) if (g.actor == actor) so = g.so; }
        int found = -1; { int w = 0; if (ActorRefersTo(actor, so, &w)) found = w; }
        Log("[remove] actor %p refers to scene object %p: %s", (void*)actor, (void*)so, found < 0 ? (IsOurActor(actor) ? "not by pointer, but it was constructed during our replay" : "NOT FOUND (wrong actor?)") : "yes");
        if (found >= 0) Log("[remove]   at actor+0x%x%s%s", found & 0xFFF, (found >> 12) & 0xFF ? " -> +0x.." : "", (found >> 20) ? " -> +0x.. (two pointers deep)" : "");
        if (found < 0 && !IsOurActor(actor)) return;
    }
    const bool tr = g_trace; g_trace = true; g_spawnWindowThread = GetCurrentThreadId(); g_spawnWindowTick = GetTickCount(); WatchObject(actor);   // the removal traces itself
    struct Restore { bool tr; ~Restore() { g_trace = tr; } } restore{ tr };
    uint16_t st = 0; ReadBytes(actor + 0x5E, &st, 2); uint32_t reason = 0; if (g_removalReasonGlobal) ReadBytes(g_base + g_removalReasonGlobal, &reason, 4);
    Log("[remove] actor %p (%s) state %u: reason %u, vtable[16] then vtable[34]", (void*)actor, RttiName(actor), st, reason);
    typedef void* (__fastcall* F1)(void*); typedef void* (__fastcall* F2)(void*, void*);
    uintptr_t lockObj = actor + 0x18; uintptr_t lvt = 0; ReadBytes(lockObj, &lvt, 8); uintptr_t vt = 0; ReadBytes(actor, &vt, 8);
    uintptr_t lockF = 0, unlockF = 0, f16 = 0, f34 = 0; ReadBytes(lvt + 8, &lockF, 8); ReadBytes(lvt + 0x10, &unlockF, 8); ReadBytes(vt + 0x80, &f16, 8); ReadBytes(vt + 0x110, &f34, 8);
    if (!InImage(lockF) || !InImage(unlockF) || !InImage(f16) || !InImage(f34)) { Log("[remove] unexpected vtables, not touched"); return; }
    ((F1)lockF)((void*)lockObj);
    memcpy((void*)(actor + 0x98), &reason, 4); uint8_t zero = 0; memcpy((void*)(actor + 0x9C), &zero, 1); memcpy((void*)(actor + 0x5C), &st, 2);
    ((F1)f16)((void*)actor);
    ((F1)unlockF)((void*)lockObj);
    int result = 0; ((F2)f34)((void*)actor, &result);
    Log("[remove] vtable[34] result %d (%s)", result, DecodeErr((uint32_t)result).c_str());
    std::lock_guard<std::mutex> l(g_spawnedMutex); for (auto it = g_spawned.begin(); it != g_spawned.end(); ++it) if (it->actor == actor) { g_spawned.erase(it); break; }
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
static void* __fastcall HookActorCtor(void* self) {
    void* r = g_origActorCtor(self);
    if (g_inGimmickReplay && GetCurrentThreadId() == g_spawnWindowThread) NoteReplayActorRaw((uintptr_t)self);
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
static const int kVtMax = 160; static const int kVtClasses = 5;
static void* g_vtOrig[kVtClasses][kVtMax] = {}; static const char* g_vtClass[kVtClasses] = { "", "", "", "", "" };
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
    if (C == 4) {
        Log("[troc] %s slot %d: handler=%p result=%p packet=%p thread %lu", g_vtClass[C], N, a, b, c, GetCurrentThreadId());
        if (N == 2 && c) { DumpBlock("troc packet", (uintptr_t)c, 0x60); uintptr_t sess = 0, buf = 0; uint16_t len = 0; ReadBytes((uintptr_t)c, &sess, 8); ReadBytes((uintptr_t)c + 0x10, &len, 2); ReadBytes((uintptr_t)c + 0x18, &buf, 8);
            Log("[troc]   sender %p (%s), total length %u, buffer %p", (void*)sess, sess && RttiName(sess) ? RttiName(sess) : "-", len, (void*)buf); if (sess) DumpDeep("troc sender", sess, 0x80, 6); if (buf) DumpBlock("troc buffer", buf, len ? (len < 0x80 ? len : 0x80) : 0x40); }
        void* r = ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); int code = 0; if (N == 2 && b) ReadBytes((uintptr_t)b, &code, 4);
        Log("[troc] %s slot %d returned %p, result %d (%s)", g_vtClass[C], N, r, code, DecodeErr((uint32_t)code).c_str()); return r; }
    if (C == 3) { if (g_inGimmickReplay && GetCurrentThreadId() == g_spawnWindowThread) NoteReplayActor((uintptr_t)a, N); if (!g_trace || !g_inGimmickReplay) return ((Fn)g_vtOrig[C][N])(a, b, c, d, e, f, g, h); }
    if (C == 2 && N == 9 && g_gimmickReplayArmed) { if (InterlockedCompareExchange(&g_gimmickReplayArmed, 0, 1) == 1) ReplayGimmick(); }
    if (C == 2 && N == 9 && g_removeActorRequest) { const uintptr_t a = (uintptr_t)InterlockedExchangePointer((void* volatile*)&g_removeActorRequest, nullptr); if (a) RemoveSpawnedActor(a); }
    if (C == 2 && N == 9) { ProcessServerJobs(); ProcessGimmickQueue(); }   // ServerField slot 9 runs ~18x per second on the server thread: armed replays run here, no game spawn needed
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
static void InstallVtableTracer(int cls, const char* mangled, const char* shortName, int slots) {
    const uintptr_t vt = FindVtableByName(mangled); if (!vt) { Log("[vt] %s: vtable not found", shortName); return; }
    void* thunks[kVtMax]; if (cls == 0) VtThunkTable<0, kVtMax - 1>::fill(thunks); else if (cls == 1) VtThunkTable<1, kVtMax - 1>::fill(thunks); else if (cls == 2) VtThunkTable<2, kVtMax - 1>::fill(thunks); else if (cls == 3) VtThunkTable<3, kVtMax - 1>::fill(thunks); else VtThunkTable<4, kVtMax - 1>::fill(thunks);
    g_vtClass[cls] = shortName; int ok = 0, skipped = 0;
    for (int i = 0; i < slots && i < kVtMax; i++) {
        uintptr_t f = 0; if (!ReadPtr(vt + (uintptr_t)i * 8, &f) || !InImage(f)) continue;
        if (SharedStub(f)) { skipped++; continue; }
        ReleaseHookPiece();
        if (MH_CreateHook((void*)f, thunks[i], &g_vtOrig[cls][i]) == MH_OK && MH_EnableHook((void*)f) == MH_OK) ok++;
    }
    Log("[vt] %s: vtable at rva 0x%llx, %d of %d slots traced (%d shared stubs skipped)", shortName, (unsigned long long)(vt - g_base), ok, slots, skipped);
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
static void* __fastcall HookWorldCastShape(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    const bool trace = g_shapeTraceLeft > 0 && InterlockedDecrement(&g_shapeTraceLeft) >= 0;
    if (!g_tpl.have || g_tpl.world != a) { std::lock_guard<std::mutex> l(g_tplMutex); CaptureTemplate(a, b, c, d); }   // automatic, once per world
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
    return false;
}
static bool RunGroundCast(void* world, Vec3 start, float len, int tileX, int tileZ, GroundHit* out, bool verbose) {
    if (!g_tpl.have || !g_origWorldCastShape) { if (verbose) Log("[probe] no template yet (the character has to be in the world for a moment)"); return false; }
    alignas(16) uint8_t q[0x200], xf[0x100], col[0x300], shp[0x200];
    memset(col, 0, sizeof col);
    { std::lock_guard<std::mutex> l(g_tplMutex); memcpy(q, g_tpl.query, sizeof q); memcpy(xf, g_tpl.xform, sizeof xf); memcpy(col, g_tpl.collector, sizeof g_tpl.collector); memcpy(shp, g_tpl.shape, sizeof shp); }
    uintptr_t pShape = (uintptr_t)shp, pHits = (uintptr_t)col + 0x30; memcpy(q + 0x28, &pShape, 8); memcpy(col + 0x20, &pHits, 8);   // inline hit buffer at +0x30, as in the original object
    float* fq = (float*)q;
    fq[0x30 / 4] = start.x - tileX * 1000.0f; fq[0x34 / 4] = start.y; fq[0x38 / 4] = start.z - tileZ * 1000.0f; fq[0x3C / 4] = 0;
    fq[0x40 / 4] = 0; fq[0x44 / 4] = -len; fq[0x48 / 4] = 0; fq[0x4C / 4] = 1.0f;
    fq[0x50 / 4] = 0; fq[0x54 / 4] = g_probeZeroVel ? 0.0f : -len; fq[0x58 / 4] = 0; fq[0x5C / 4] = len;
    { const double big = 1e19; memcpy(col + 0x10, &big, 8); uint32_t zero = 0; memcpy(col + 0x0C, &zero, 4); }   // reset: no hit, early-out far away
    if (verbose) { Log("[probe] cast: start (%.2f %.2f %.2f) tile %d,%d local (%.2f %.2f %.2f) down %.1f m", start.x, start.y, start.z, tileX, tileZ, fq[0x30 / 4], fq[0x34 / 4], fq[0x38 / 4], len); }
    void* r = nullptr;
    if (!CallCastGuarded(world, q, xf, col, &r)) { Log("[probe] replay crashed (caught)"); return false; }
    uint32_t nh = 0; double frac = 0; memcpy(&nh, col + 0x0C, 4); memcpy(&frac, col + 0x10, 8);
    const float* fc = (const float*)col;
    out->done = true; out->hit = nh > 0 && std::isfinite(frac) && frac <= 1.0; out->fraction = (float)frac;   // negative fraction = the cast started inside a body (penetration), reported as a hit so the caller can step lower
    out->centerY = start.y - (float)frac * len; out->normal = { fc[0x80 / 4], fc[0x84 / 4], fc[0x88 / 4] };
    if (verbose) Log("[probe] RESULT hits %u fraction %.4f -> sphere center y %.3f (%.2f m below start), normal (%.3f %.3f %.3f), returned %p", nh, frac, out->centerY, (float)frac * len, out->normal.x, out->normal.y, out->normal.z, r);
    return true;
}
static int QueueGround(Vec3 start, float len, bool verbose) {
    std::lock_guard<std::mutex> l(g_groundMutex); const int id = ++g_groundNext;
    g_groundQueue.push_back({ id, start, len, verbose }); InterlockedExchange(&g_groundQueued, 1); return id;
}
// ---- ground queries for the editor
float g_probeRadius = 0.0f; static bool g_probeCalibrated = false;
bool GroundProbeReady() { return g_tpl.have && g_origWorldCastShape != nullptr; }
static void CalibrateProbe(void* world) {   // cast at the player's feet; the sphere center stops one radius above the ground
    PosInfo pi{}; if (!PlayerPosInfo(&pi)) return;
    GroundHit h; Vec3 s = { pi.world.x, pi.world.y + 3.0f, pi.world.z };
    if (!RunGroundCast(world, s, 10.0f, pi.tileX, pi.tileZ, &h, false) || !h.hit) return;
    const float r = h.centerY - pi.world.y;
    if (r > -0.5f && r < 1.0f) { g_probeRadius = r > 0.0f ? r : 0.0f; g_probeCalibrated = true; Log("[ground] probe calibrated at the player: sphere center %.3f above the feet -> radius %.3f", r, g_probeRadius); }
}
static void ServiceGroundQueue(void* world) {   // physics thread, inside the game's own worldCastShape call
    static thread_local bool s_inside = false; if (s_inside) return; s_inside = true;
    std::vector<GroundReq> batch;
    { std::lock_guard<std::mutex> l(g_groundMutex); batch.swap(g_groundQueue); InterlockedExchange(&g_groundQueued, 0); }
    if (!g_probeCalibrated && !batch.empty()) CalibrateProbe(world);
    for (const auto& rq : batch) {
        const int tx = (int)(rq.start.x * 0.001), tz = (int)(rq.start.z * 0.001);
        GroundHit h; if (!RunGroundCast(world, rq.start, rq.len, tx, tz, &h, rq.verbose)) { h.done = true; h.hit = false; }
        std::lock_guard<std::mutex> l(g_groundMutex); g_groundResults[rq.id] = h; if (g_groundResults.size() > 200) g_groundResults.erase(g_groundResults.begin());
    }
    s_inside = false;
}
int GroundProbe(Vec3 start, float len) {
    if (!GroundProbeReady() || !GameThreadReady()) return 0;
    const int id = QueueGround(start, len, false);
    RunOnGameThread([]() { ServiceGroundQueue(g_tpl.world); });   // game thread, between the game's own casts (the only context that worked so far)
    return id;
}
bool GroundResult(int ticket, GroundHit* out) {
    std::lock_guard<std::mutex> l(g_groundMutex);
    auto it = g_groundResults.find(ticket); if (it == g_groundResults.end() || !it->second.done) return false;
    *out = it->second; g_groundResults.erase(it); return true;
}
void ProbeGround(float above, float len) {
    PosInfo pi{}; if (!PlayerPosInfo(&pi)) { Log("[probe] no player position"); return; }
    Vec3 start = { pi.world.x, pi.world.y + above, pi.world.z }; const int tx = pi.tileX, tz = pi.tileZ;
    (void)tx; (void)tz;
    if (!GroundProbeReady()) { Log("[probe] no template yet (the character has to be in the world for a moment)"); return; }
    Log("[probe] player at (%.2f %.2f %.2f); replays: %.0f m above / %.0f m down, %.0f m above / %.0f m down, %.0f m above / 30 m down (served inside the game's next cast)", pi.world.x, pi.world.y, pi.world.z, above, len, above + 3, len, above + 3);
    QueueGround(start, len, true); Vec3 s2 = { start.x, start.y + 3.0f, start.z }; QueueGround(s2, len, true); QueueGround(s2, 30.0f, true);
    if (GameThreadReady()) RunOnGameThread([]() { ServiceGroundQueue(g_tpl.world); });
}
void RayTrace(int calls) { InterlockedExchange(&g_rayTraceLeft, calls); InterlockedExchange(&g_shapeTraceLeft, calls);
    Log("[ray] tracing the next %d ray casts and %d shape casts (walk a few steps for the ground probe, then aim / interact for ray casts)", calls, calls); }

// ---- experimental: teleport by writing the player's transform component; camera field discovery ----
bool SetPlayerPos(Vec3 world) {
    uintptr_t actor = PlayerActor(); if (!actor) return false;
    uintptr_t comps = Deref(actor, kOff_Ent_Comps);
    uintptr_t tf = comps ? Deref(comps, kOff_Comps_Transform) : 0;
    if (!tf) return false;
    const int tx = (int)(world.x * 0.001), tz = (int)(world.z * 0.001);
    float v[3] = { world.x - tx * kTileSize, world.y, world.z - tz * kTileSize }; int16_t tile[2] = { (int16_t)tx, (int16_t)tz };
    bool ok = WriteBytes(tf + kOff_Tf_Pos, v, 12) && WriteBytes(tf + kOff_Tf_Tile, tile, 4);
    Log("teleport -> (%.1f %.1f %.1f) tile %d,%d: %s", world.x, world.y, world.z, tx, tz, ok ? "written" : "FAILED");
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
// sits at +0x1A4 like every SceneObject (scale3, quat4 at +0x1B0, pos3). camtrace showed the quaternion turning with the view.
static uintptr_t g_camMgr = 0; static DWORD g_camSearchAt = 0;
static uintptr_t CameraManagerPtr() {
    if (g_camMgr) { const char* n = RttiName(g_camMgr); if (n && strstr(n, "CameraManager@")) return g_camMgr; g_camMgr = 0; }
    const DWORD now = GetTickCount(); if (now - g_camSearchAt < 2000) return 0; g_camSearchAt = now;
    std::vector<std::pair<std::string, uintptr_t>> found; FindCameraObjects(found);
    for (auto& f : found) if (f.first.find("CameraManager@") != std::string::npos) { g_camMgr = f.second; Log("camera manager %p", (void*)g_camMgr); break; }
    return g_camMgr;
}
bool CameraBasis(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd) {
    uintptr_t mgr = CameraManagerPtr(); if (!mgr) return false;
    uintptr_t so = Deref(mgr, 0x40); if (!so) return false;
    const char* n = RttiName(so); if (!n || !strstr(n, "SceneObject")) return false;
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
    uintptr_t mgr = CameraManagerPtr(); if (!mgr) return false;
    uintptr_t so = Deref(mgr, 0x40); if (!so) return false;
    const char* n = RttiName(so); if (!n || !strstr(n, "SceneObject")) return false;
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
// fovtrace / viewscan / camtrace live in diag.cpp (console-only reverse-engineering aids)


// ---- configurable hotkeys (bin64\cdmodkit\settings.txt: key_toggle=INSERT, key_mode=HOME, key_pos=F9) ----
int g_keyToggle = VK_INSERT, g_keyMode = VK_HOME; bool g_showConsole = false;
float g_fovDeg = 55.0f; bool g_camMirror = false; bool g_fovAuto = true; int g_camLag = 0;
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
    std::ifstream f(SettingsPath()); std::string line;
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
        if (k == "camlag") { const int c = atoi(v.c_str()); g_camLag = c < 0 ? 0 : c > 4 ? 4 : c; continue; }
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
    Log("settings: toggle %s, mode %s, console %s", KeyName(g_keyToggle), KeyName(g_keyMode), g_showConsole ? "on" : "off");
}
void SaveSettings() {
    FILE* f = fopen(SettingsPath().c_str(), "w"); if (!f) return;
    fprintf(f, "# World Builder hotkeys. Names: INSERT HOME END DELETE PAGEUP PAGEDOWN F1..F12 SCROLLLOCK PAUSE BACKQUOTE MINUS EQUALS BACKSLASH NUMPAD* NUMPAD/ NUMLOCK CAPSLOCK TAB or a single letter/digit\n");
    fprintf(f, "key_toggle=%s\nkey_mode=%s\n# console=0 hides the console window (log file only)\nconsole=%d\n# projection for the gizmo: vertical field of view in degrees and horizontal mirror (calibrate in the Settings tab)\nfov=%.1f\nmirror=%d\n# fovauto=1 reads the field of view from the game's camera object (fov= is the fallback)\nfovauto=%d\n# camlag: frames the overlay camera trails the game camera (0..4). Outlines run ahead while panning: raise it; they lag: lower it\ncamlag=%d\n", KeyName(g_keyToggle), KeyName(g_keyMode), g_showConsole ? 1 : 0, g_fovDeg, g_camMirror ? 1 : 0, g_fovAuto ? 1 : 0, g_camLag);
    fprintf(f, "# preview_quality: 0 base colour, 1 + dye colours, 2 + normal maps, 3 + specular/emissive. Lower renders the background pass faster\npreview_quality=%d\n", thumbgen::Quality());
    fprintf(f, "# gimmick_spawn=0: place gimmick prefabs (/object/cd_gimmick/...) as plain objects instead of through the game spawn path\ngimmick_spawn=%d\n", g_gimmickSpawn ? 1 : 0);
    fprintf(f, "# placement keys (any key name from the list above, NUMPAD0..9, NUMPAD+ NUMPAD- NUMPAD. NUMPAD* NUMPAD/, ENTER, BACKSPACE, SPACE, UP/DOWN/LEFT/RIGHT, SHIFT/CTRL/ALT for 'fast')\n");
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
static DWORD WINAPI ConsoleThread(LPVOID) {
    AllocConsole();
    FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleTitleA("cdmodkit console");
    g_console = true;
    Log("cdmodkit console ready. base=%p. type 'help'. Insert = editor overlay", (void*)g_base);
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        std::string cmd(line); while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
        if (cmd == "pos") CmdPos();
        else if (cmd == "status") CmdStatus();
        else if (cmd == "list") CmdList();
        else if (cmd.rfind("flags ", 0) == 0) { int a = 1, b = 1, c = 0; sscanf(cmd.c_str() + 6, "%d %d %d", &a, &b, &c); g_flags[0] = a; g_flags[1] = b; g_flags[2] = c; Log("spawn flags now %d,%d,%d", a, b, c); }
        else if (cmd.rfind("spawn ", 0) == 0) {
            char path[400] = {0}; float dx = 2, dy = 0, dz = 0;
            if (sscanf(cmd.c_str() + 6, "%399s %f %f %f", path, &dx, &dy, &dz) < 1) { Log("usage: spawn <prefab> [dx dy dz]"); continue; }
            Vec3 p{}; if (!PlayerWorldPos(&p)) { Log("spawn: no player position"); continue; }
            SpawnAt(path, { p.x + dx, p.y + dy, p.z + dz });
        }
        else if (cmd.rfind("move ", 0) == 0) { int i = 0; float x, y, z, yaw = 0, sc = 1; if (sscanf(cmd.c_str() + 5, "%d %f %f %f %f %f", &i, &x, &y, &z, &yaw, &sc) >= 4) Log("move #%d -> %s", i, MoveSpawned(i, { x, y, z }, Rot{ yaw }, sc) ? "queued" : "failed"); else Log("usage: move <idx> x y z [yaw] [scale]"); }
        else if (cmd.rfind("hide ", 0) == 0) { int i = atoi(cmd.c_str() + 5); Log("hide #%d -> %s", i, HideSpawned(i) ? "ok" : "failed"); }
        else if (cmd.rfind("livemode ", 0) == 0) { g_liveMode = atoi(cmd.c_str() + 9); Log("livemode = %d", g_liveMode); }
        else if (cmd == "trace on" || cmd == "trace off") SetTrace(cmd == "trace on");
        else if (cmd.rfind("tp ", 0) == 0) { Vec3 w{}; if (sscanf(cmd.c_str() + 3, "%f %f %f", &w.x, &w.y, &w.z) == 3) SetPlayerPos(w); else Log("usage: tp x y z"); }
        else if (cmd == "camtrace") CamTrace(16);
        else if (cmd == "fovtrace") FovTrace(12);
        else if (cmd == "raytrace") RayTrace(12);
        else if (cmd == "probe") ProbeGround(3.0f, 10.0f);
        else if (cmd == "traceio on" || cmd == "traceio off") SetIoTrace(cmd == "traceio on");
        else if (cmd == "thumbs on" || cmd == "thumbs off") { thumbgen::SetBackground(cmd == "thumbs on"); Log("background preview generation %s", thumbgen::Background() ? "on" : "off"); }
        else if (cmd.rfind("save ", 0) == 0) SaveProject(cmd.substr(5));
        else if (cmd.rfind("load ", 0) == 0) LoadProject(cmd.substr(5), false);
        else if (cmd == "projects") { for (auto& p : ListProjects()) Log("  %s", p.c_str()); }
        else if (cmd == "help") Log("commands: pos | status | list | spawn <prefab> [dx dy dz] | move <idx> x y z [yaw] [scale] | hide <idx> | save <name> | load <name> | projects | flags a b c | quit");
        else if (cmd == "quit") break;
        else if (!cmd.empty()) Log("unknown command '%s'", cmd.c_str());
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
        if (g_traceHooks && kRva_UuidLookup) { void* t10 = (void*)(g_base + kRva_UuidLookup); HookFn(t10, (void*)HookUuidLookup, (void**)&g_origUuidLookup, "uuid lookup (trace)"); }
        if (kRva_SoServerCreate) { void* t9 = (void*)(g_base + kRva_SoServerCreate); HookFn(t9, (void*)HookSoServerCreate, (void**)&g_origSoServerCreate, "SceneObjectServer new (trace)"); }
        if (kRva_ActorCtor) { void* t13 = (void*)(g_base + kRva_ActorCtor); HookFn(t13, (void*)HookActorCtor, (void**)&g_origActorCtor, "actor constructor (trace)"); }
        if (g_traceHooks && kRva_RemovalLoop) { void* t12 = (void*)(g_base + kRva_RemovalLoop); HookFn(t12, (void*)HookRemovalLoop, (void**)&g_origRemovalLoop, "actor removal loop (trace)"); }
        if (kRva_ActorCreateInner) { void* t11 = (void*)(g_base + kRva_ActorCreateInner); HookFn(t11, (void*)HookActorInner, (void**)&g_origActorInner, "actor create inner (trace)"); }
        if (kRva_ActorCreateCore) { void* t8 = (void*)(g_base + kRva_ActorCreateCore); HookFn(t8, (void*)HookActorCore, (void**)&g_origActorCore, "actor create core (trace)"); }
        if (kRva_GimmickSpawn) { void* t7 = (void*)(g_base + kRva_GimmickSpawn); HookFn(t7, (void*)HookGimmickSpawn, (void**)&g_origGimmickSpawn, "gimmick spawn (trace)"); }
        if (kRva_WorldCastRay) { void* t4 = (void*)(g_base + kRva_WorldCastRay); if (HookFn(t4, (void*)HookWorldCastRay, (void**)&g_origWorldCastRay, "worldCastRay (trace)")) Log("resolved worldCastRay        rva 0x%llx", (unsigned long long)kRva_WorldCastRay); }
    }
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
    Log("cdmodkit.asi v0.92 attached, base=%p, game build %s", (void*)g_base, g_gameVersion.empty() ? "unknown" : g_gameVersion.c_str());
    LoadSettings();
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
