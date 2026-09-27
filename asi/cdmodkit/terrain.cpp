// Optional terrain editing: height changes applied to the terrain height textures while the game streams them in.
//
// How the game gets its terrain heights (notes/FORMATS.md, "Terrain"): every tile is a 512x512 L16 DDS
// (leveldata/rootlevel/terrain/height16f/terrain_X_Z_height_h.dds, 2 m per texel, 10 mips, height = offset + v / 65535 * range
// from heighttable/sector_X_Z.xml). The texture streamer reads the pixel data (whole, or only the small mips) through an async
// file request; the renderer draws from that copy, and the collision heightfields are captured from the rendered terrain, so
// both follow an edit made here. (A second, CPU-side copy loaded by TerrainHeightTextureCache is not what is drawn.)
//
// Hook points, both found at startup (a game patch leaves the module disabled with a log line, never calling a wrong address):
//   - the streamer's request function (unique signature): remembers, for a height tile with edits, the request's completion
//     event (+0x30), its buffer holder (+0x10, filled with the read buffer during the call), file offset (+0x24) and length (+0x20)
//   - BindableEventFileIO slot 5 (RTTI vtable), the completion poll: the consumer only learns that a read finished through it,
//     so the samples are shifted here, after the read completed and before anybody uses them.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace core {

static const int kTile = 512;                  // texels per side of mip 0
static const int kMips = 10;
static const uint32_t kDataTotal = 349525;     // u16 samples of all 10 mips (512^2 + 256^2 + ... + 1)
static const uint32_t kHeader = 128;           // DDS header in front of the pixel data (these files carry no DX10 block)

struct TileEdit {
    float range = 0, offset = 0;               // from the tile's sector xml
    std::vector<float> m;                      // mip 0 height change in metres, row-major [z][x]
    std::vector<int32_t> units;                // all mips in u16 units, in file order (rebuilt when m changes)
};
struct PendingRead { int tx, tz; uintptr_t holder; uint32_t off, len; };

static std::mutex g_mx;
static std::map<std::pair<int, int>, TileEdit> g_tiles;
static std::map<uintptr_t, PendingRead> g_pending;   // completion event -> read waiting to be patched
static bool g_ok = false; static std::string g_why = "not installed";
static volatile LONG g_patched = 0, g_missed = 0;
static volatile LONG g_watchNext = 0;   // research: arm a read watch on the next patched whole-tile buffer (who consumes it)
void TerrainWatchNext() { InterlockedExchange(&g_watchNext, 1); }

typedef uint8_t(__fastcall* StreamReqFn)(uintptr_t req); static StreamReqFn g_origReq = nullptr;
typedef uint8_t(__fastcall* EvPollFn)(uintptr_t ev); static EvPollFn g_origPoll = nullptr;

static void BuildUnits(TileEdit& t) {   // mip i = mean of the 2^i x 2^i block of mip 0, in u16 units
    t.units.assign(kDataTotal, 0);
    const float k = t.range > 0 ? 65535.0f / t.range : 0.0f;
    std::vector<float> cur(t.m.begin(), t.m.end()); int dim = kTile; uint32_t base = 0;
    for (int mip = 0; mip < kMips; mip++) {
        for (int i = 0; i < dim * dim; i++) t.units[base + i] = (int32_t)std::lround(cur[i] * k);
        base += (uint32_t)(dim * dim);
        if (dim == 1) break;
        const int nd = dim / 2; std::vector<float> nx((size_t)nd * nd);
        for (int r = 0; r < nd; r++) for (int c = 0; c < nd; c++)
            nx[(size_t)r * nd + c] = 0.25f * (cur[(size_t)(2 * r) * dim + 2 * c] + cur[(size_t)(2 * r) * dim + 2 * c + 1] + cur[(size_t)(2 * r + 1) * dim + 2 * c] + cur[(size_t)(2 * r + 1) * dim + 2 * c + 1]);
        cur.swap(nx); dim = nd;
    }
}

// Shift the samples of one completed read: buf holds file bytes [off, off + len) of the DDS. Plain function (SEH guard).
static uint32_t ApplyGuarded(uint8_t* buf, uint32_t off, uint32_t len, const int32_t* units) {
    uint32_t n = 0;
    CDK_GUARD_BEGIN
        uint32_t fo = off < kHeader ? kHeader : off; if ((fo - kHeader) & 1) fo++;
        for (; fo + 2 <= off + len; fo += 2) {
            const uint32_t k = (fo - kHeader) / 2; if (k >= kDataTotal) break;
            const int32_t d = units[k]; if (!d) continue;
            uint16_t* v = (uint16_t*)(buf + (fo - off)); const int32_t x = (int32_t)*v + d;
            *v = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); n++;
        }
    CDK_GUARD_FAIL n = 0;
    CDK_GUARD_END
    return n;
}

static bool ParseTilePath(const std::string& s, int* tx, int* tz) {
    const size_t p = s.find("height16f/terrain_"); if (p == std::string::npos) return false;
    return sscanf(s.c_str() + p, "height16f/terrain_%d_%d_height_h.dds", tx, tz) == 2;
}

static uint8_t __fastcall HookStreamReq(uintptr_t req) {
    if (g_ok) {
        const std::string s = PathObjText((void*)(req + 8)); int tx = 0, tz = 0;
        if (!s.empty() && ParseTilePath(s, &tx, &tz)) {
            bool edited; { std::lock_guard<std::mutex> l(g_mx); edited = g_tiles.count({ tx, tz }) != 0; }
            uint32_t lenOff[2] = {}; uintptr_t holder = 0, ev = 0;
            if (edited && ReadBytes(req + 0x20, lenOff, 8) && ReadBytes(req + 0x10, &holder, 8) && ReadBytes(req + 0x30, &ev, 8) && holder && ev) {
                std::lock_guard<std::mutex> l(g_mx); g_pending[ev] = PendingRead{ tx, tz, holder, lenOff[1], lenOff[0] };
            }
        }
    }
    return g_origReq(req);
}

static uint8_t __fastcall HookEvPoll(uintptr_t ev) {
    PendingRead pr{}; bool mine = false;
    { std::lock_guard<std::mutex> l(g_mx); auto it = g_pending.find(ev); if (it != g_pending.end()) { pr = it->second; mine = true; } }
    if (mine) {
        LONG64 st = 0x103; ReadBytes(ev + 0x40, &st, 8);   // OVERLAPPED.Internal: STATUS_PENDING until the read is done
        if (st != 0x103) {
            uintptr_t buf = 0; ReadBytes(pr.holder, &buf, 8);
            std::lock_guard<std::mutex> l(g_mx); g_pending.erase(ev);
            auto t = g_tiles.find({ pr.tx, pr.tz });
            if (st == 0 && buf && t != g_tiles.end() && !t->second.units.empty()) {
                const uint32_t n = ApplyGuarded((uint8_t*)buf, pr.off, pr.len, t->second.units.data());
                InterlockedIncrement(&g_patched);
                Log("[terrain] tile %d,%d: read of %u bytes at +%u patched (%u samples)", pr.tx, pr.tz, pr.len, pr.off, n);
                if (pr.off == kHeader && pr.len > 0x80000 && InterlockedExchange(&g_watchNext, 0)) {   // mip 0 middle, mip 1, last mips
                    const uintptr_t w[4] = { buf + 0x40000, buf + 0x80000 + 0x100, buf + 0xA0000, buf + pr.len - 8 };
                    Log("[terrain] watching buffer %p", (void*)buf); WatchAccessSync(w, 5, "texwatch"); }
            } else { InterlockedIncrement(&g_missed); Log("[terrain] tile %d,%d: read at +%u not patched (status %llx, buffer %p)", pr.tx, pr.tz, pr.off, (unsigned long long)st, (void*)buf); }
        }
    }
    return g_origPoll(ev);
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
void TerrainJobTrace(uintptr_t rva) {
    static bool s_done = false; if (s_done || !rva) return; s_done = true;
    if (InstallInternalHook((void*)(g_base + rva), (void*)HookJobExec, (void**)&g_origJobExec, "terrain job exec (research)")) Log("[terrain] job trace on rva 0x%llx", (unsigned long long)rva);
}
void TerrainInstall() {
    // the streamer's request: stores the path at +8 and hands buffer holder / length / offset / event to the async worker read
    const uintptr_t req = SigScanUnique("48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 48 8B D9 4C 8D 3D");
    if (!req) { g_why = "stream request function not found"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    const uintptr_t vt = VtableByName(".?AVBindableEventFileIO@pa@@"); uintptr_t poll = 0;
    if (!vt || !ReadBytes(vt + 5 * 8, &poll, 8) || !poll) { g_why = "BindableEventFileIO not found"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    // the poll must test the OVERLAPPED status at +0x40 against STATUS_PENDING the way we read it
    uint8_t code[0x50] = {}; static const uint8_t want[] = { 0x48, 0x8D, 0x51, 0x40, 0x33, 0xC9, 0x33, 0xC0, 0xF0, 0x48, 0x0F, 0xB1, 0x0A, 0x81, 0x3A, 0x03, 0x01, 0x00, 0x00 };
    if (!ReadBytes(poll, code, sizeof code) || !std::search(code, code + sizeof code, want, want + sizeof want)) { g_why = "completion poll layout changed"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    if (!InstallInternalHook((void*)req, (void*)HookStreamReq, (void**)&g_origReq, "terrain stream request")) { g_why = "hook failed"; return; }
    if (!InstallInternalHook((void*)poll, (void*)HookEvPoll, (void**)&g_origPoll, "terrain read completion")) { g_why = "hook failed"; return; }
    g_ok = true; g_why = "ready";
    Log("[terrain] ready: stream request rva 0x%llx, completion poll rva 0x%llx", (unsigned long long)(req - g_base), (unsigned long long)(poll - g_base));
}

bool TerrainAvailable() { return g_ok; }

static bool TileRange(int tx, int tz, float* range, float* offset) {   // heighttable/sector_X_Z.xml: _heightRange / _heightOffset
    char path[128]; snprintf(path, sizeof path, "leveldata/rootlevel/terrain/heighttable/sector_%d_%d.xml", tx, tz);
    std::vector<uint8_t> x; if (!GameReadFile(path, x) || x.empty()) return false;
    const std::string s(x.begin(), x.end());
    const size_t a = s.find("_heightRange=\""), b = s.find("_heightOffset=\"");
    if (a == std::string::npos || b == std::string::npos) return false;
    *range = (float)atof(s.c_str() + a + 14); *offset = (float)atof(s.c_str() + b + 15);
    return *range > 0;
}

// World -> texel of tile (tx, tz): the tile covers [tx * 1024, tx * 1024 + 1024) x [tz * 1024, ...), 2 m per texel (seen in game:
// shifting tile -10,-5 moved the ground at z -4527). Columns run along +x, rows along -z (row 0
// is the tile's north edge); found with test dips measured through the collision.
int TerrainEditDisc(float x, float z, float radius, float metres) {
    if (!g_ok || radius <= 0) return 0;
    int touched = 0;
    const int tx0 = (int)std::floor((x - radius) / 1024.0f), tx1 = (int)std::floor((x + radius) / 1024.0f);
    const int tz0 = (int)std::floor((z - radius) / 1024.0f), tz1 = (int)std::floor((z + radius) / 1024.0f);
    for (int tx = tx0; tx <= tx1; tx++) for (int tz = tz0; tz <= tz1; tz++) {
        float range = 0, offset = 0;
        { std::lock_guard<std::mutex> l(g_mx); auto it = g_tiles.find({ tx, tz }); if (it != g_tiles.end()) { range = it->second.range; offset = it->second.offset; } }
        if (range <= 0 && !TileRange(tx, tz, &range, &offset)) { Log("[terrain] tile %d,%d: no height table", tx, tz); continue; }
        std::lock_guard<std::mutex> l(g_mx);
        TileEdit& t = g_tiles[{ tx, tz }]; t.range = range; t.offset = offset; if (t.m.empty()) t.m.assign((size_t)kTile * kTile, 0.0f);
        const float ox = tx * 1024.0f, oz = tz * 1024.0f;
        for (int r = 0; r < kTile; r++) for (int c = 0; c < kTile; c++) {
            const float wx = ox + c * 2.0f + 1.0f, wz = oz + (kTile - 1 - r) * 2.0f + 1.0f, q = std::hypot(wx - x, wz - z) / radius;   // texel centre
            if (q < 1.0f) t.m[(size_t)r * kTile + c] += metres * 0.5f * (1.0f + std::cos(3.14159265f * q));
        }
        BuildUnits(t); touched++;
        Log("[terrain] tile %d,%d: disc at (%.1f %.1f) r %.1f, %+.2f m (range %.1f, offset %.1f)", tx, tz, x, z, radius, metres, range, offset);
    }
    return touched;
}

int TerrainEditTile(int tx, int tz, float metres) {   // research: shift a whole tile uniformly (which tile covers what)
    float range = 0, offset = 0; if (!g_ok || !TileRange(tx, tz, &range, &offset)) return 0;
    std::lock_guard<std::mutex> l(g_mx);
    TileEdit& t = g_tiles[{ tx, tz }]; t.range = range; t.offset = offset; if (t.m.empty()) t.m.assign((size_t)kTile * kTile, 0.0f);
    for (auto& v : t.m) v += metres;
    BuildUnits(t); Log("[terrain] tile %d,%d shifted %+.2f m", tx, tz, metres); return 1;
}

void TerrainEditClear() { std::lock_guard<std::mutex> l(g_mx); g_tiles.clear(); g_pending.clear(); Log("[terrain] edits cleared"); }

std::string TerrainStatus() {
    std::lock_guard<std::mutex> l(g_mx); char b[160];
    snprintf(b, sizeof b, "%s, %zu edited tiles, %ld reads patched, %ld missed", g_why.c_str(), g_tiles.size(), (long)g_patched, (long)g_missed);
    return b;
}

}   // namespace core
