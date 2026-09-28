// Reverse-engineering aids for the terrain / travel research (notes/FORMATS.md, "Terrain"). Nothing here runs unless the
// research API installs it (/api/research/terrain with textrace, tiletask, rettrace, teletrace, rstrace, rssend, crtrace,
// jobtrace, loadtrace, reloadobj, texreload, synctrace): the rvas are given at runtime and valid for build 1.0.0.2976 only.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace core {

static std::mutex g_mx;
static volatile LONG g_retTraceOn = 0;
static bool ParseTilePath(const std::string& s, int* tx, int* tz) {
    const size_t p = s.find("height16f/terrain_"); if (p == std::string::npos) return false;
    return sscanf(s.c_str() + p, "height16f/terrain_%d_%d_height_h.dds", tx, tz) == 2;
}

// Research: the resource job's execute function (0x13f2570 on 2976): job +0 -> the request (+8 path), +0x170 / +0x1a0 state.
typedef void(__fastcall* JobExecFn)(uintptr_t job, uint8_t* result); static JobExecFn g_origJobExec = nullptr;
static void __fastcall HookJobExec(uintptr_t job, uint8_t* result) {
    uintptr_t req = 0; ReadBytes(job, &req, 8); std::string s; int tx = 0, tz = 0;
    static volatile LONG s_probe = 0;
    if (req && InterlockedIncrement(&s_probe) <= 40) {   // where does the job keep its path? probe the loader object's fields
        std::string hit; for (int o = 0; o < 0x100; o += 8) { const std::string t = PathObjText((void*)(req + o)); if (!t.empty()) { char b[300]; snprintf(b, sizeof b, " +%x '%s'", o, t.c_str()); hit += b; } }
        Log("[terrain] job %p obj %p (%s):%s", (void*)job, (void*)req, RttiName(req) ? RttiName(req) : "?", hit.c_str());
    }
    for (int o = 0; o < 0x100 && s.empty(); o += 8) { const std::string t = PathObjText((void*)(req + o)); if (t.find("height16f/") != std::string::npos) s = t; }
    const bool mine = !s.empty() && ParseTilePath(s, &tx, &tz);
    uint32_t st0 = 0; uint8_t f0[2] = {}; if (mine) { ReadBytes(job + 0x170, &st0, 4); ReadBytes(job + 0x1a0, f0, 2); }
    g_origJobExec(job, result);
    if (mine) { uint32_t st1 = 0; uint8_t f1[2] = {}; ReadBytes(job + 0x170, &st1, 4); ReadBytes(job + 0x1a0, f1, 2);
        Log("[terrain] job %p tile %d,%d exec: state %u -> %u, +1a0 %u/%02x -> %u/%02x, result %u, thread %lu", (void*)job, tx, tz, st0, st1, f0[0], f0[1], f1[0], f1[1], result ? *result : 255, GetCurrentThreadId()); }
}
// Research: BasicsResourceLoader vtable slots given at runtime; logs calls whose 4th argument is a height tile path, with the stack.
typedef uintptr_t(__fastcall* Gen10)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static Gen10 g_origLoadAsync = nullptr;
static uintptr_t __fastcall HookLoadAsync(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    std::string s; int tx = 0, tz = 0;
    for (uintptr_t cand : { d, c, b }) { if (!cand) continue; s = PathObjText((void*)cand); if (!s.empty()) break; }
    if (!s.empty() && ParseTilePath(s, &tx, &tz)) {
        void* fr[24]; const USHORT k = RtlCaptureStackBackTrace(1, 24, fr, nullptr); std::string st;
        for (USHORT q = 0; q < k; q++) { const uintptr_t x = (uintptr_t)fr[q]; if (x >= g_base && x < g_base + 0x8000000) { char t[24]; snprintf(t, sizeof t, " %llx", (unsigned long long)(x - g_base)); st += t; } }
        Log("[terrain] loadAsync tile %d,%d args %llx %llx %llx off/len %llx %llx mode %llx, thread %lu, stack:%s", tx, tz, (unsigned long long)e, (unsigned long long)f,
            (unsigned long long)g, (unsigned long long)g, (unsigned long long)h, (unsigned long long)i, GetCurrentThreadId(), st.c_str());
    }
    return g_origLoadAsync(a, b, c, d, e, f, g, h, i, j);
}
void TerrainLoadTrace(int slot) {
    static bool s_done = false; if (s_done) return; s_done = true;
    const uintptr_t vt = VtableByName(".?AVBasicsResourceLoader@pa@@"); uintptr_t fn = 0;
    if (!vt || !ReadBytes(vt + slot * 8, &fn, 8) || !fn) { Log("[terrain] loader vtable not found"); return; }
    if (InstallInternalHook((void*)fn, (void*)HookLoadAsync, (void**)&g_origLoadAsync, "terrain loadAsync (research)")) Log("[terrain] load trace on slot %d (rva 0x%llx)", slot, (unsigned long long)(fn - g_base));
}
// Research: call a texture manager's reload-by-name (vtable slot 78 of the manager objects found by their vtable) on the game thread.
static DWORD g_lastFaultCode = 0; static uintptr_t g_lastFaultAddr = 0, g_lastFaultInfo = 0;
static uint8_t CallReloadGuarded(uintptr_t fn, uintptr_t obj, const char* name) {
    uint8_t r = 0xEE;
    CDK_GUARD_BEGIN
        r = ((uint8_t(__fastcall*)(uintptr_t, const char*))fn)(obj, name);
    CDK_GUARD_FAIL r = 0xFF; g_lastFaultCode = cdk::t_fault.rec.ExceptionCode; g_lastFaultAddr = (uintptr_t)cdk::t_fault.rec.ExceptionAddress;
        g_lastFaultInfo = cdk::t_fault.rec.NumberParameters > 1 ? (uintptr_t)cdk::t_fault.rec.ExceptionInformation[1] : 0;
    CDK_GUARD_END
    return r;
}
void TerrainReloadCall(uintptr_t obj, int slot, const std::string& name) {
    RunOnGameThread([obj, slot, name]() {
        uintptr_t vt = 0, fn = 0; if (!ReadBytes(obj, &vt, 8) || !ReadBytes(vt + slot * 8, &fn, 8) || !fn) { Log("[terrain] reload: bad object %p", (void*)obj); return; }
        Log("[terrain] reload: calling rva 0x%llx on %p with '%s'", (unsigned long long)(fn - g_base), (void*)obj, name.c_str());
        const uint8_t r = CallReloadGuarded(fn, obj, name.c_str());
        Log("[terrain] reload returned %u (fault %08lx at %p = rva 0x%llx, address %p)", r, g_lastFaultCode, (void*)g_lastFaultAddr,
            (unsigned long long)(g_lastFaultAddr >= g_base ? g_lastFaultAddr - g_base : 0), (void*)g_lastFaultInfo);
    });
}
// Research: the texture creation from streamed data (0x3770440 on 2976): r9 = SharedPtr<texture>*; logs the texture's name.
static Gen10 g_origTexCreate = nullptr; static volatile LONG g_texNames = 0; static std::map<std::pair<int, int>, uintptr_t> g_texOf;
static const char* TexNameGuarded(uintptr_t tex) {
    const char* r = nullptr;
    CDK_GUARD_BEGIN
        const uintptr_t nameObj = *(uintptr_t*)(tex + 0x78);
        if (nameObj) r = ((const char* (__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)nameObj + 8)))(nameObj);
    CDK_GUARD_FAIL r = nullptr;
    CDK_GUARD_END
    return r;
}
static uintptr_t __fastcall HookTexCreate(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    uintptr_t tex = 0; if (d) ReadBytes(d, &tex, 8);
    const uintptr_t r = g_origTexCreate(a, b, c, d, e, f, g, h, i, j);
    if (tex) { const char* n = TexNameGuarded(tex); char nm[200] = { 0 }; if (n) ReadBytes((uintptr_t)n, nm, sizeof nm - 1);
        if (InterlockedIncrement(&g_texNames) <= 15 || strstr(nm, "height16f")) Log("[terrain] texture %p name '%s' (create returned %llx)", (void*)tex, nm, (unsigned long long)(r & 0xFF));
        int tx = 0, tz = 0; if (ParseTilePath(nm, &tx, &tz)) { std::lock_guard<std::mutex> l(g_mx); g_texOf[{ tx, tz }] = tex; } }
    return r;
}
void TerrainTexTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookTexCreate, (void**)&g_origTexCreate, "terrain texture create (research)")) Log("[terrain] texture trace on rva 0x%llx", (unsigned long long)rva);
}
// Research: the texture resource's synchronous reload (0x3772ee0 on 2976): (texture, [manager+0x10], 0, [manager+0x48]).
static uint8_t CallTexReloadGuarded(uintptr_t fn, uintptr_t tex, uintptr_t dev, uintptr_t extra) {
    uint8_t r = 0xEE;
    CDK_GUARD_BEGIN
        r = ((uint8_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))fn)(tex, dev, 0, extra);
    CDK_GUARD_FAIL r = 0xFF; g_lastFaultCode = cdk::t_fault.rec.ExceptionCode; g_lastFaultAddr = (uintptr_t)cdk::t_fault.rec.ExceptionAddress;
    CDK_GUARD_END
    return r;
}
void TerrainTexReload(int tx, int tz, uintptr_t mgr, uintptr_t rva) {
    uintptr_t tex = 0; { std::lock_guard<std::mutex> l(g_mx); auto it = g_texOf.find({ tx, tz }); if (it != g_texOf.end()) tex = it->second; }
    if (!tex || !mgr || !rva) { Log("[terrain] texreload: tile %d,%d texture %p manager %p rva 0x%llx (all required)", tx, tz, (void*)tex, (void*)mgr, (unsigned long long)rva); return; }
    RunOnGameThread([tex, mgr, rva, tx, tz]() {
        uintptr_t dev = 0, extra = 0; ReadBytes(mgr + 0x10, &dev, 8); ReadBytes(mgr + 0x48, &extra, 8);
        Log("[terrain] texreload tile %d,%d: texture %p, manager %p (+10 %p, +48 %p)", tx, tz, (void*)tex, (void*)mgr, (void*)dev, (void*)extra);
        g_retTraceOn = 1; const uint8_t r = CallTexReloadGuarded(g_base + rva, tex, dev, extra); g_retTraceOn = 0;
        Log("[terrain] texreload returned %u (fault %08lx at rva 0x%llx)", r, g_lastFaultCode, (unsigned long long)(g_lastFaultAddr >= g_base ? g_lastFaultAddr - g_base : 0));
    });
}
// Research: log arguments and return value of up to 6 functions (rva given at runtime) while g_retTraceOn is set.
static Gen10 g_retOrig[6] = {}; static uintptr_t g_retRva[6] = {};
template<int K> static uintptr_t __fastcall RetThunk(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    const uintptr_t r = g_retOrig[K](a, b, c, d, e, f, g, h, i, j);
    if (g_retTraceOn) { const uintptr_t ret = (uintptr_t)_ReturnAddress();
        Log("[terrain] fn 0x%llx(%llx, %llx, %llx, %llx, %llx, %llx) -> %llx (from 0x%llx)", (unsigned long long)g_retRva[K], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
            (unsigned long long)d, (unsigned long long)e, (unsigned long long)f, (unsigned long long)r, (unsigned long long)(ret - g_base)); }
    return r;
}
void TerrainRetTrace(uintptr_t rva) {
    static void* th[6] = { (void*)&RetThunk<0>, (void*)&RetThunk<1>, (void*)&RetThunk<2>, (void*)&RetThunk<3>, (void*)&RetThunk<4>, (void*)&RetThunk<5> };
    for (int k = 0; k < 6; k++) if (!g_retRva[k]) {
        if (InstallInternalHook((void*)(g_base + rva), th[k], (void**)&g_retOrig[k], "terrain ret trace (research)")) { g_retRva[k] = rva; Log("[terrain] ret trace %d on rva 0x%llx", k, (unsigned long long)rva); }
        return;
    }
}
void TerrainRetTraceOn(bool on) { g_retTraceOn = on ? 1 : 0; }
// Research: the terrain tile load task (0x3518580 on 2976): logs its arguments, their RTTI and the first qwords.
static Gen10 g_origTileTask = nullptr; static volatile LONG g_tileTaskLines = 0;
static std::string DescribePtr(uintptr_t p) {
    char b[200]; uint64_t q[6] = {}; ReadBytes(p, q, sizeof q); const char* rt = RttiName(p);
    snprintf(b, sizeof b, "%llx(%s: %llx %llx %llx %llx %llx %llx)", (unsigned long long)p, rt ? rt : "-", q[0], q[1], q[2], q[3], q[4], q[5]); return b;
}
static uintptr_t __fastcall HookTileTask(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    const bool log = InterlockedIncrement(&g_tileTaskLines) <= 30;
    if (log) { uint64_t cq[6] = {}; ReadBytes(c, cq, sizeof cq);
        Log("[terrain] tile task a=%llx c=%s", (unsigned long long)a, DescribePtr(c).c_str());
        for (int k : { 2, 4 }) if (cq[k]) { uint64_t eq[4] = {}; ReadBytes(cq[k], eq, sizeof eq);
            Log("[terrain]   c[%d] -> %s", k, DescribePtr(cq[k]).c_str()); for (int m = 0; m < 4; m++) if (eq[m] > 0x10000000000ull && eq[m] < 0x7ff000000000ull) Log("[terrain]     [%d] -> %s", m, DescribePtr(eq[m]).c_str()); } }
    const uintptr_t r = g_origTileTask(a, b, c, d, e, f, g, h, i, j);
    if (log) { uint8_t st = 0; ReadBytes(d, &st, 1); Log("[terrain] tile task done, status %u", st); }
    return r;
}
void TerrainTileTaskTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookTileTask, (void**)&g_origTileTask, "terrain tile task (research)")) Log("[terrain] tile task trace on rva 0x%llx", (unsigned long long)rva);
}
// Research: the client's teleport handling (DoTeleportAck execute -> 0x9fea40(transform component, &result, &pos12, &extra8)).
static Gen10 g_origTele = nullptr;
static uintptr_t __fastcall HookTele(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    float pos[3] = {}; uint8_t ex[8] = {}; ReadBytes(c, pos, 12); ReadBytes(d, ex, 8);
    void* fr[16]; const USHORT k = RtlCaptureStackBackTrace(1, 16, fr, nullptr); std::string st;
    for (USHORT q = 0; q < k; q++) { const uintptr_t x = (uintptr_t)fr[q]; if (x >= g_base && x < g_base + 0x8000000) { char t[24]; snprintf(t, sizeof t, " %llx", (unsigned long long)(x - g_base)); st += t; } }
    const char* rt = RttiName(a);
    Log("[tele] comp %p (%s) pos %.3f %.3f %.3f extra %02x%02x%02x%02x %02x%02x%02x%02x (as i16: %d %d %d %d) thread %lu stack:%s", (void*)a, rt ? rt : "?", pos[0], pos[1], pos[2],
        ex[0], ex[1], ex[2], ex[3], ex[4], ex[5], ex[6], ex[7], *(int16_t*)ex, *(int16_t*)(ex + 2), *(int16_t*)(ex + 4), *(int16_t*)(ex + 6), GetCurrentThreadId(), st.c_str());
    const uintptr_t r = g_origTele(a, b, c, d, e, f, g, h, i, j);
    uint32_t res = 0; ReadBytes(r ? r : b, &res, 4); Log("[tele] -> result %u", res);
    return r;
}
void TeleTraceInstall(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookTele, (void**)&g_origTele, "teleport (research)")) Log("[tele] trace on rva 0x%llx", (unsigned long long)rva);
}
bool ResearchWatchWrites(const uintptr_t addr[4], int seconds) { return WatchWrites(addr, seconds, "watch"); }
// Research: TrocTrReloadStageStartReq::execute (0x2a5f580 on 2976) - the fast travel request as the server gets it.
// Logs handler, message struct and payload, and keeps a copy for a replay with another position.
static Gen10 g_origReload = nullptr; static uint8_t g_rsMsg[0x40]; static uint8_t g_rsPay[256]; static uint32_t g_rsPayLen = 0; static uintptr_t g_rsThis = 0;
static uintptr_t __fastcall HookReloadStage(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    uint8_t msg[0x40] = {}; ReadBytes(c, msg, sizeof msg); uintptr_t pay = *(uintptr_t*)(msg + 0x18); uint16_t tot = *(uint16_t*)(msg + 0x10);
    uint8_t pb[256] = {}; const uint32_t pl = tot < sizeof pb ? tot : sizeof pb; if (pay) ReadBytes(pay, pb, pl);
    char hx[600] = { 0 }; for (uint32_t k = 0; k < pl && k < 250; k++) snprintf(hx + 2 * k, 3, "%02x", pb[k]);
    char mh[140] = { 0 }; for (int k = 0; k < 0x40; k++) snprintf(mh + 2 * k, 3, "%02x", msg[k]);
    Log("[reloadstage] this %p (+c %04x) msg %s total %u payload %s thread %lu", (void*)a, a ? *(uint16_t*)(a + 0xc) : 0, mh, tot, hx, GetCurrentThreadId());
    memcpy(g_rsMsg, msg, sizeof msg); memcpy(g_rsPay, pb, pl); g_rsPayLen = pl; g_rsThis = a;
    const uintptr_t r = g_origReload(a, b, c, d, e, f, g, h, i, j);
    uint32_t res = 0; ReadBytes(b, &res, 4); Log("[reloadstage] result %u", res);
    return r;
}
void ReloadStageTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookReloadStage, (void**)&g_origReload, "reload stage (research)")) Log("[reloadstage] trace on rva 0x%llx", (unsigned long long)rva);
}
// Research: replay the recorded ReloadStageStartReq with another destination (payload data +12: position x, y, z).
static uint8_t g_rsReplayPay[256]; static uint8_t g_rsReplayMsg[0x40];
static uint32_t CallReloadGuarded2(uintptr_t fn, uintptr_t self, uintptr_t msg) {
    uint32_t res = 0xEEEE;
    CDK_GUARD_BEGIN
        ((uintptr_t(__fastcall*)(uintptr_t, uint32_t*, uintptr_t))fn)(self, &res, msg);
    CDK_GUARD_FAIL res = 0xFFFF; g_lastFaultCode = cdk::t_fault.rec.ExceptionCode; g_lastFaultAddr = (uintptr_t)cdk::t_fault.rec.ExceptionAddress;
    CDK_GUARD_END
    return res;
}
void ReloadStageReplay(float x, float y, float z) {
    if (!g_rsPayLen || !g_origReload) { Log("[reloadstage] nothing recorded"); return; }
    RunOnGameThread([x, y, z]() {
        memcpy(g_rsReplayPay, g_rsPay, sizeof g_rsPay); memcpy(g_rsReplayMsg, g_rsMsg, sizeof g_rsMsg);
        float* pos = (float*)(g_rsReplayPay + 5 + 12); pos[0] = x; pos[1] = y; pos[2] = z;
        *(uintptr_t*)(g_rsReplayMsg + 0x18) = (uintptr_t)g_rsReplayPay;
        Log("[reloadstage] replay to (%.2f %.2f %.2f) on %p", x, y, z, (void*)g_rsThis);
        const uint32_t r = CallReloadGuarded2((uintptr_t)g_origReload, g_rsThis, (uintptr_t)g_rsReplayMsg);
        Log("[reloadstage] replay result %u (fault %08lx at rva 0x%llx)", r, g_lastFaultCode, (unsigned long long)(g_lastFaultAddr >= g_base ? g_lastFaultAddr - g_base : 0));
    });
}
// Research: the client's ReloadStageStartReq sender (0xc04d20 on 2976): (conn, edx, &a, &b, &c, transform*, &flag).
static Gen10 g_origRsSend = nullptr;
static uintptr_t __fastcall HookRsSend(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i, uintptr_t j) {
    uint32_t A = 0, B = 0, C = 0; uint8_t fl = 0; float tf[10] = {}; ReadBytes(c, &A, 4); ReadBytes(d, &B, 4); ReadBytes(e, &C, 4); ReadBytes(f, tf, 40); ReadBytes(g, &fl, 1);
    void* fr[24]; const USHORT k = RtlCaptureStackBackTrace(1, 24, fr, nullptr); std::string st;
    for (USHORT q = 0; q < k; q++) { const uintptr_t x = (uintptr_t)fr[q]; if (x >= g_base && x < g_base + 0x8000000) { char t[24]; snprintf(t, sizeof t, " %llx", (unsigned long long)(x - g_base)); st += t; } }
    const char* rt = RttiName(a);
    Log("[rssend] conn %p (%s) edx %llx A %u B %u C %u pos %.2f %.2f %.2f quat %.3f %.3f %.3f %.3f flag %u thread %lu stack:%s", (void*)a, rt ? rt : "?", (unsigned long long)b, A, B, C,
        tf[7], tf[8], tf[9], tf[3], tf[4], tf[5], tf[6], fl, GetCurrentThreadId(), st.c_str());
    return g_origRsSend(a, b, c, d, e, f, g, h, i, j);
}
void RsSendTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookRsSend, (void**)&g_origRsSend, "reload stage send (research)")) Log("[rssend] trace on rva 0x%llx", (unsigned long long)rva);
}
// Research: the client's "reload stage at transform" (0xa9a860 on 2976): (obj, key, b, c, transform* {scale3, quat4, pos3}).
// Recorded on a real fast travel, replayed with another position: the game's own teleport with loading screen.
typedef void(__fastcall* ClientReloadFn)(uintptr_t, uint32_t, uint32_t, uint32_t, const float*);
static ClientReloadFn g_origCReload = nullptr; static uintptr_t g_crObj = 0; static uint32_t g_crKey = 0, g_crB = 0, g_crC = 0; static float g_crTf[10] = {};
static void __fastcall HookCReload(uintptr_t obj, uint32_t key, uint32_t b, uint32_t c, const float* tf) {
    float t[10] = {}; if (tf) ReadBytes((uintptr_t)tf, t, sizeof t);
    Log("[creload] obj %p (%s) key %u b %u c %u scale %.2f quat %.3f %.3f %.3f %.3f pos %.2f %.2f %.2f thread %lu", (void*)obj, RttiName(obj) ? RttiName(obj) : "?", key, b, c, t[0], t[3], t[4], t[5], t[6], t[7], t[8], t[9], GetCurrentThreadId());
    g_crObj = obj; g_crKey = key; g_crB = b; g_crC = c; memcpy(g_crTf, t, sizeof t);
    g_origCReload(obj, key, b, c, tf);
}
static void CallCReloadGuarded(float* tf) {
    CDK_GUARD_BEGIN
        g_origCReload(g_crObj, g_crKey, g_crB, g_crC, tf);
    CDK_GUARD_FAIL g_lastFaultCode = cdk::t_fault.rec.ExceptionCode; g_lastFaultAddr = (uintptr_t)cdk::t_fault.rec.ExceptionAddress;
    CDK_GUARD_END
}
void ClientReloadTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookCReload, (void**)&g_origCReload, "client reload stage (research)")) Log("[creload] trace on rva 0x%llx", (unsigned long long)rva);
}
void ClientReloadReplay(float x, float y, float z) {
    if (!g_crObj || !g_origCReload) { Log("[creload] nothing recorded"); return; }
    RunOnGameThread([x, y, z]() {
        static float tf[10]; memcpy(tf, g_crTf, sizeof tf); tf[7] = x; tf[8] = y; tf[9] = z; g_lastFaultCode = 0;
        Log("[creload] replay to (%.2f %.2f %.2f)", x, y, z); CallCReloadGuarded(tf);
        Log("[creload] replay done (fault %08lx at rva 0x%llx)", g_lastFaultCode, (unsigned long long)(g_lastFaultAddr >= g_base ? g_lastFaultAddr - g_base : 0));
    });
}
void TerrainJobTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookJobExec, (void**)&g_origJobExec, "terrain job exec (research)")) Log("[terrain] job trace on rva 0x%llx", (unsigned long long)rva);
}
// BasicsResourceLoader slot 8: the synchronous read into a game buffer object {u8* data, u32 size, u32 capacity, ...}
// (loader, const NormalizedPathA* path, buffer*, allocator, u32 a, u32 b, u32 flags). Used by the texture's own reload.
typedef uint8_t(__fastcall* SyncLoadFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t, uint32_t);
static SyncLoadFn g_origSyncLoad = nullptr;
static uint8_t __fastcall HookSyncLoad(uintptr_t self, uintptr_t path, uintptr_t buf, uintptr_t alloc, uint32_t a, uint32_t b, uint32_t fl) {
    const uint8_t r = g_origSyncLoad(self, path, buf, alloc, a, b, fl);
    if (g_retTraceOn) { uint8_t pb[48] = {}; ReadBytes(path, pb, sizeof pb); uintptr_t data = 0; uint32_t sz[2] = {}; ReadBytes(buf, &data, 8); ReadBytes(buf + 8, sz, 8);
        char hx[100]; for (int i = 0; i < 48; i++) snprintf(hx + 2 * i, 3, "%02x", pb[i]);
        Log("[terrain] sync read during reload: path obj %s text '%s' a %u b %u -> %u, buffer %p size %u", hx, PathObjText((void*)path).c_str(), a, b, r, (void*)data, sz[0]); }
    if (path && buf) {
        const std::string s = PathObjText((void*)path); int tx = 0, tz = 0;
        if (!s.empty() && ParseTilePath(s, &tx, &tz)) {
            uintptr_t data = 0; uint32_t sz[2] = {}; ReadBytes(buf, &data, 8); ReadBytes(buf + 8, sz, 8);
            Log("[terrain] sync read tile %d,%d: a %u b %u flags %x -> %u, buffer %p size %u cap %u", tx, tz, a, b, fl, r, (void*)data, sz[0], sz[1]);
        }
    }
    return r;
}
void TerrainSyncTrace() {   // research: log the loader's synchronous reads of height tiles (BasicsResourceLoader slot 8)
    static bool s_done = false; if (s_done) return; s_done = true;
    const uintptr_t lvt = VtableByName(".?AVBasicsResourceLoader@pa@@"); uintptr_t sl = 0;
    if (lvt && ReadBytes(lvt + 8 * 8, &sl, 8) && sl) InstallInternalHook((void*)sl, (void*)HookSyncLoad, (void**)&g_origSyncLoad, "terrain sync read (research)");
}

}   // namespace core
