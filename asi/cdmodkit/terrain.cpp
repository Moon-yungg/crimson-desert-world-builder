// Optional terrain editing: brush strokes applied to the terrain height textures while the game streams them in.
//
// How the game gets its terrain heights (notes/FORMATS.md, "Terrain"): every tile is a 512x512 L16 DDS
// (leveldata/rootlevel/terrain/height16f/terrain_X_Z_height_h.dds, 2 m per texel, 10 mips, height = offset + v / 65535 * range
// from heighttable/sector_X_Z.xml). Tile X,Z covers world [X*1024, X*1024+1024) x [Z*1024, ...); texel column = +x, row = -z.
// The texture streamer reads the pixel data (whole, or only the small mips) through an async file request; the renderer draws
// from that copy, and the collision heightfields are captured from the rendered terrain, so both follow an edit made here.
//
// Hook points, both found at startup (a game patch leaves the module disabled with a log line, never calling a wrong address):
//   - the streamer's request function (unique signature): remembers, for a tile with strokes, the request's completion event
//     (+0x30), its buffer holder (+0x10, filled with the read buffer during the call), file offset (+0x24) and length (+0x20)
//   - BindableEventFileIO slot 5 (RTTI vtable), the completion poll: the consumer only learns that a read finished through it,
//     so the samples are changed here, after the read completed and before anybody uses them.
// A tile only streams when it comes into range, so strokes on tiles that are already loaded show after TerrainApply (a fast
// travel away and back, travel.cpp).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace core {

static const int kTile = 512;                  // texels per side of mip 0
static const int kMips = 10;
static const uint32_t kHeader = 128;           // DDS header in front of the pixel data (these files carry no DX10 block)

struct TileInfo { float range = 0, offset = 0; bool tried = false; };
// CPU copy of a tile's mip 0 (metres) for the editor's shape preview: the original from the game's own file and the same
// strokes applied the way the stream patch applies them, so the preview shows what the ground will look like after Apply.
struct TileCpu {
    std::vector<float> orig, edit; std::vector<uint8_t> chain; bool ok = false, tried = false;   // chain: the file's 10 mips (from offset 128)
    std::vector<TerrainStroke> gpuStrokes;   // the strokes the texture got when it last streamed in
    std::vector<float> gpu; bool gpuValid = false;   // the heights on the GPU after the last live upload (else orig + gpuStrokes)
};
struct PendingRead { int tx, tz; uintptr_t holder; uint32_t off, len; };

static std::mutex g_mx;                                   // guards everything below
static std::vector<TerrainStroke> g_strokes;              // in painting order; applied in that order
static std::set<std::pair<int, int>> g_edited;            // tiles touched by at least one stroke
static std::map<std::pair<int, int>, TileInfo> g_info;    // height table per tile
static std::map<uintptr_t, PendingRead> g_pending;        // completion event -> read waiting to be patched
static std::map<std::pair<int, int>, TileCpu> g_cpu;      // preview copies of edited tiles
static std::atomic<int> g_gen{ 0 };                       // bumped whenever a preview copy changes
static bool g_ok = false, g_needsApply = false; static std::string g_why = "not installed";
static volatile LONG g_patched = 0, g_missed = 0;
static std::atomic<int> g_applyStep{ 0 }; static std::string g_applyText;   // 0 idle; text guarded by g_mx

typedef uint8_t(__fastcall* StreamReqFn)(uintptr_t req); static StreamReqFn g_origReq = nullptr;
typedef uint8_t(__fastcall* EvPollFn)(uintptr_t ev); static EvPollFn g_origPoll = nullptr;

static bool ParseTilePath(const std::string& s, int* tx, int* tz) {
    const size_t p = s.find("height16f/terrain_"); if (p == std::string::npos) return false;
    return sscanf(s.c_str() + p, "height16f/terrain_%d_%d_height_h.dds", tx, tz) == 2;
}
static bool TileRange(int tx, int tz, float* range, float* offset) {   // heighttable/sector_X_Z.xml: _heightRange / _heightOffset
    char path[128]; snprintf(path, sizeof path, "leveldata/rootlevel/terrain/heighttable/sector_%d_%d.xml", tx, tz);
    std::vector<uint8_t> x; if (!GameReadFile(path, x) || x.empty()) return false;
    const std::string s(x.begin(), x.end());
    const size_t a = s.find("_heightRange=\""), b = s.find("_heightOffset=\"");
    if (a == std::string::npos || b == std::string::npos) return false;
    *range = (float)atof(s.c_str() + a + 14); *offset = (float)atof(s.c_str() + b + 15);
    return *range > 0;
}
static void TilesOf(const TerrainStroke& s, std::vector<std::pair<int, int>>* out) {
    if (s.tileScoped) { out->push_back({ s.tileX, s.tileZ }); return; }
    const int tx0 = (int)std::floor((s.x - s.r) / 1024.0f), tx1 = (int)std::floor((s.x + s.r) / 1024.0f);
    const int tz0 = (int)std::floor((s.z - s.r) / 1024.0f), tz1 = (int)std::floor((s.z + s.r) / 1024.0f);
    for (int tx = tx0; tx <= tx1; tx++) for (int tz = tz0; tz <= tz1; tz++) out->push_back({ tx, tz });
}
static std::pair<int, int> StrokeOwnerTile(const TerrainStroke& s) {
    if (s.tileScoped) return { s.tileX, s.tileZ };
    return { (int)std::floor(s.x / 1024.0f), (int)std::floor(s.z / 1024.0f) };
}
static std::vector<TerrainStroke> SplitStroke(const TerrainStroke& stroke) {
    if (stroke.tileScoped) return { stroke };
    std::vector<std::pair<int, int>> tiles; TilesOf(stroke, &tiles);
    std::vector<TerrainStroke> result; result.reserve(tiles.size());
    for (const auto& tile : tiles) {
        TerrainStroke part = stroke; part.tileScoped = true; part.tileX = tile.first; part.tileZ = tile.second;
        result.push_back(part);
    }
    return result;
}
static std::vector<std::pair<int, int>> UniqueTiles(const std::vector<std::pair<int, int>>& tiles) {
    std::set<std::pair<int, int>> unique(tiles.begin(), tiles.end());
    return { unique.begin(), unique.end() };
}
static bool StrokeTouchesTile(const TerrainStroke& s, int tx, int tz) {
    if (s.tileScoped && (s.tileX != tx || s.tileZ != tz)) return false;
    const float x0 = tx * 1024.0f, z0 = tz * 1024.0f;
    return s.x + s.r >= x0 && s.x - s.r < x0 + 1024.0f && s.z + s.r >= z0 && s.z - s.r < z0 + 1024.0f;
}
// One stroke on a mip 0 copy in metres: the same falloff, anchor and order as ApplyGuarded does on the streamed samples.
static void ApplyStrokeCpu(std::vector<float>& e, int tx, int tz, const TerrainStroke& s) {
    if (s.r <= 0 || e.size() != (size_t)kTile * kTile) return;
    const float ox = tx * 1024.0f, oz = tz * 1024.0f, size = 2.0f; const int dim = kTile;
    float target = 0;
    if (s.mode == TerrainFlatten) {
        int ac = (int)std::floor((s.ax - ox) / size), ar = dim - 1 - (int)std::floor((s.az - oz) / size);
        ac = std::clamp(ac, 0, dim - 1); ar = std::clamp(ar, 0, dim - 1);
        target = e[(size_t)ar * dim + ac];
    }
    const int c0 = std::max(0, (int)std::floor((s.x - s.r - ox) / size)), c1 = std::min(dim - 1, (int)std::floor((s.x + s.r - ox) / size));
    const int r0 = std::max(0, dim - 1 - (int)std::floor((s.z + s.r - oz) / size)), r1 = std::min(dim - 1, dim - 1 - (int)std::floor((s.z - s.r - oz) / size));
    for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) {
        const float wx = ox + (c + 0.5f) * size, wz = oz + (dim - 1 - r + 0.5f) * size;
        const float q = std::sqrt((wx - s.x) * (wx - s.x) + (wz - s.z) * (wz - s.z)) / s.r; if (q >= 1.0f) continue;
        const float f = 0.5f * (1.0f + std::cos(3.14159265f * q)); float& h = e[(size_t)r * dim + c];
        if (s.mode == TerrainFlatten) h += (target - h) * f * std::min(1.0f, std::max(0.0f, s.strength)); else h += s.amount * f;
    }
}
static void RecomputeCpuLocked() {   // after undo / clear / replace: every loaded copy from its original again
    for (auto& kv : g_cpu) { if (!kv.second.ok) continue; kv.second.edit = kv.second.orig;
        for (const auto& s : g_strokes) if (StrokeTouchesTile(s, kv.first.first, kv.first.second)) ApplyStrokeCpu(kv.second.edit, kv.first.first, kv.first.second, s); }
    g_gen++;
}
static void RebuildEditedLocked() {
    g_edited.clear(); std::vector<std::pair<int, int>> t;
    for (const auto& s : g_strokes) { t.clear(); TilesOf(s, &t); for (auto& k : t) g_edited.insert(k); }
}
// Height tables are read through the game's loader; that needs the loader captured, so a helper thread fills them in as soon
// as it can (in the menus, long before the world streams). A read that finds none yet fetches it on the spot.
static std::atomic<bool> g_fetchRunning{ false };
static void QueueLive(const std::vector<std::pair<int, int>>& tiles);
static void FetchTables() {
    bool expected = false; if (!g_fetchRunning.compare_exchange_strong(expected, true)) return;
    std::thread([]() {
        for (int wait = 0; wait < 1200 && !GameReadAvailable(); wait++) Sleep(250);
        for (;;) {
            std::pair<int, int> want{}; bool any = false, needRange = false;
            { std::lock_guard<std::mutex> l(g_mx);
              for (auto& k : g_edited) { auto& ti = g_info[k]; auto& tc = g_cpu[k]; if (!ti.tried || !tc.tried) { needRange = !ti.tried; ti.tried = true; tc.tried = true; want = k; any = true; break; } } }
            if (!any) break;
            float range = 0, offset = 0;
            if (needRange) { const bool ok = TileRange(want.first, want.second, &range, &offset);
                std::lock_guard<std::mutex> l(g_mx); auto& ti = g_info[want]; if (ok) { ti.range = range; ti.offset = offset; } else Log("[terrain] tile %d,%d: no height table", want.first, want.second); }
            { std::lock_guard<std::mutex> l(g_mx); range = g_info[want].range; offset = g_info[want].offset; }
            if (range <= 0) continue;
            char path[128]; snprintf(path, sizeof path, "leveldata/rootlevel/terrain/height16f/terrain_%d_%d_height_h.dds", want.first, want.second);
            std::vector<uint8_t> dds;   // header + mip 0 is all the preview needs; the game's own loader, the original file
            if (!GameReadFile(path, dds) || dds.size() < kHeader + (size_t)kTile * kTile * 2) { Log("[terrain] tile %d,%d: no height texture for the preview", want.first, want.second); continue; }
            std::vector<float> orig((size_t)kTile * kTile); const float k = range / 65535.0f;
            for (size_t i = 0; i < orig.size(); i++) { uint16_t v; memcpy(&v, dds.data() + kHeader + i * 2, 2); orig[i] = offset + v * k; }
            std::lock_guard<std::mutex> l(g_mx); auto& tc = g_cpu[want]; tc.orig.swap(orig); tc.edit = tc.orig; tc.ok = true;
            tc.chain.assign(dds.begin() + kHeader, dds.begin() + std::min(dds.size(), (size_t)kHeader + 699050));
            for (const auto& s : g_strokes) if (StrokeTouchesTile(s, want.first, want.second)) ApplyStrokeCpu(tc.edit, want.first, want.second, s);
            g_gen++;
            QueueLive({ want });   // strokes painted before the copy was there reach the texture now
        }
        g_fetchRunning = false;
    }).detach();
}

// Apply the strokes to one completed read: buf holds file bytes [off, off + len) of the tile's DDS. Mip by mip, stroke by
// stroke (the painting order), each sample at its texel centre. Raise adds metres with a cosine falloff; flatten pulls the
// samples toward the height found at the stroke's anchor in the same mip. Plain function (SEH guard, no C++ objects).
static uint32_t ApplyGuarded(uint8_t* buf, uint32_t off, uint32_t len, int tx, int tz, float range, float offset, const TerrainStroke* st, int ns) {
    uint32_t changed = 0;
    CDK_GUARD_BEGIN
        const float toU = 65535.0f / range, toM = range / 65535.0f, ox = tx * 1024.0f, oz = tz * 1024.0f;
        uint32_t mipBase = kHeader;
        for (int mip = 0, dim = kTile; mip < kMips; mip++, dim >>= 1) {
            const uint32_t mipEnd = mipBase + (uint32_t)dim * dim * 2;
            if (mipEnd > off && mipBase < off + len) {
                const float size = 2.0f * (float)(1 << mip);   // metres per texel
                auto sample = [&](int r, int c, uint16_t** p) -> bool {
                    if (r < 0 || c < 0 || r >= dim || c >= dim) return false;
                    const uint32_t fo = mipBase + ((uint32_t)r * dim + c) * 2; if (fo < off || fo + 2 > off + len) return false;
                    *p = (uint16_t*)(buf + (fo - off)); return true;
                };
                for (int k = 0; k < ns; k++) {
                    const TerrainStroke& s = st[k]; if (s.r <= 0) continue;
                    float target = 0;
                    if (s.mode == TerrainFlatten) {   // nearest sample to the anchor in this tile, before this stroke
                        uint16_t* p = nullptr;
                        int ac = (int)std::floor((s.ax - ox) / size), ar = dim - 1 - (int)std::floor((s.az - oz) / size);
                        ac = std::clamp(ac, 0, dim - 1); ar = std::clamp(ar, 0, dim - 1);
                        if (!sample(ar, ac, &p)) continue;
                        target = offset + *p * toM;
                    }
                    const int c0 = std::max(0, (int)std::floor((s.x - s.r - ox) / size)), c1 = std::min(dim - 1, (int)std::floor((s.x + s.r - ox) / size));
                    const int r0 = std::max(0, dim - 1 - (int)std::floor((s.z + s.r - oz) / size)), r1 = std::min(dim - 1, dim - 1 - (int)std::floor((s.z - s.r - oz) / size));
                    for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) {
                        uint16_t* p = nullptr; if (!sample(r, c, &p)) continue;
                        const float wx = ox + (c + 0.5f) * size, wz = oz + (dim - 1 - r + 0.5f) * size;
                        const float q = std::sqrt((wx - s.x) * (wx - s.x) + (wz - s.z) * (wz - s.z)) / s.r; if (q >= 1.0f) continue;
                        const float f = 0.5f * (1.0f + std::cos(3.14159265f * q));
                        float h = offset + *p * toM;
                        if (s.mode == TerrainFlatten) h += (target - h) * f * std::min(1.0f, std::max(0.0f, s.strength));
                        else h += s.amount * f;
                        const float v = std::round((h - offset) * toU);
                        *p = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); changed++;
                    }
                }
            }
            mipBase = mipEnd;
            if (dim == 1) break;
        }
    CDK_GUARD_FAIL changed = 0;
    CDK_GUARD_END
    return changed;
}

static uint8_t __fastcall HookStreamReq(uintptr_t req) {
    const std::string s = PathObjText((void*)(req + 8)); int tx = 0, tz = 0;
    if (PlayModeActive()) PlayModeOnStream(s);   // play mode: census of what the streamer reads (proxy meshes of the levels)
    if (!s.empty() && ParseTilePath(s, &tx, &tz)) {
        TerrainLiveInstall();   // needs the game's device, which exists by the time tiles stream
        bool edited; { std::lock_guard<std::mutex> l(g_mx); edited = g_edited.count({ tx, tz }) != 0; }
        if (TerrainLiveAvailable()) edited = true;   // also unedited tiles: their read tells which texture they use
        uint32_t lenOff[2] = {}; uintptr_t holder = 0, ev = 0;
        if (edited && ReadBytes(req + 0x20, lenOff, 8) && ReadBytes(req + 0x10, &holder, 8) && ReadBytes(req + 0x30, &ev, 8) && holder && ev) {
            std::lock_guard<std::mutex> l(g_mx); g_pending[ev] = PendingRead{ tx, tz, holder, lenOff[1], lenOff[0] };
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
            TileInfo ti; std::vector<TerrainStroke> mineStrokes;
            {
                std::lock_guard<std::mutex> l(g_mx); g_pending.erase(ev); ti = g_info[{ pr.tx, pr.tz }];
                for (const auto& s : g_strokes) if (StrokeTouchesTile(s, pr.tx, pr.tz)) mineStrokes.push_back(s);
                if (st == 0 && pr.off == kHeader && pr.len >= 699050) { auto& tc = g_cpu[{ pr.tx, pr.tz }]; tc.gpuStrokes = mineStrokes; tc.gpuValid = false; }   // a fresh texture
            }
            if (ti.range <= 0 && GameReadAvailable()) {   // not fetched yet: read it now (the game's own loader, outside any of its locks here)
                float range = 0, offset = 0;
                if (TileRange(pr.tx, pr.tz, &range, &offset)) { ti.range = range; ti.offset = offset; std::lock_guard<std::mutex> l(g_mx); auto& t = g_info[{ pr.tx, pr.tz }]; t.range = range; t.offset = offset; t.tried = true; }
            }
            if (st == 0 && buf && pr.off == kHeader && pr.len >= 699050 && mineStrokes.empty()) TerrainLiveNoteRead(pr.tx, pr.tz, (const uint8_t*)buf, pr.len);
            if (st == 0 && buf && ti.range > 0 && !mineStrokes.empty()) {
                const uint32_t n = ApplyGuarded((uint8_t*)buf, pr.off, pr.len, pr.tx, pr.tz, ti.range, ti.offset, mineStrokes.data(), (int)mineStrokes.size());
                InterlockedIncrement(&g_patched);
                Log("[terrain] tile %d,%d: read of %u bytes at +%u, %zu strokes, %u samples changed", pr.tx, pr.tz, pr.len, pr.off, mineStrokes.size(), n);
                if (pr.off == kHeader && pr.len >= 699050) TerrainLiveNoteRead(pr.tx, pr.tz, (const uint8_t*)buf, pr.len);
            } else if (!mineStrokes.empty() && ti.range <= 0) { InterlockedIncrement(&g_missed); Log("[terrain] tile %d,%d: read at +%u not patched (status %llx, buffer %p, range %.1f)", pr.tx, pr.tz, pr.off, (unsigned long long)st, (void*)buf, ti.range); }
        }
    }
    return g_origPoll(ev);
}

void TerrainInstall() {
    // the streamer's request: stores the path at +8 and hands buffer holder / length / offset / event to the async worker read
    const uintptr_t req = SigScanUnique("48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 48 8B D9 4C 8D 3D");
    if (!req) { g_why = "stream request function not found"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    const uintptr_t vt = VtableByName(".?AVBindableEventFileIO@pa@@"); uintptr_t poll = 0;
    if (!vt || !ReadBytes(vt + 5 * 8, &poll, 8) || !poll) { g_why = "BindableEventFileIO not found"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    // the poll must test the OVERLAPPED status at +0x40 against STATUS_PENDING the way we read it
    uint8_t code[0x50] = {}; static const uint8_t want[] = { 0x48, 0x8D, 0x51, 0x40, 0x33, 0xC9, 0x33, 0xC0, 0xF0, 0x48, 0x0F, 0xB1, 0x0A, 0x81, 0x3A, 0x03, 0x01, 0x00, 0x00 };
    if (!ReadBytes(poll, code, sizeof code) || std::search(code, code + sizeof code, want, want + sizeof want) == code + sizeof code) { g_why = "completion poll layout changed"; Log("[terrain] disabled: %s", g_why.c_str()); return; }
    if (!InstallInternalHook((void*)req, (void*)HookStreamReq, (void**)&g_origReq, "terrain stream request")) { g_why = "hook failed"; return; }
    if (!InstallInternalHook((void*)poll, (void*)HookEvPoll, (void**)&g_origPoll, "terrain read completion")) { g_why = "hook failed"; return; }
    g_ok = true; g_why = "ready";
    Log("[terrain] ready: stream request rva 0x%llx, completion poll rva 0x%llx", (unsigned long long)(req - g_base), (unsigned long long)(poll - g_base));
}

bool TerrainAvailable() { return g_ok; }

// Live: tiles whose strokes changed get their texture rewritten (original chain + all strokes, the stream patch's own code)
static std::mutex g_liveMx; static std::set<std::pair<int, int>> g_liveDirty; static std::atomic<bool> g_liveRunning{ false };
static std::atomic<int> g_liveDone{ 0 }; static bool g_liveFailed = false;   // guarded by g_liveMx
static void MarkAppliedAtGeneration(int generation) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_gen == generation) g_needsApply = false;
}
static void QueueLive(const std::vector<std::pair<int, int>>& tiles) {
    if (!TerrainLiveAvailable() || tiles.empty()) return;
    {
        std::lock_guard<std::mutex> l(g_liveMx);
        for (auto& t : tiles) g_liveDirty.insert(t);
        if (g_liveRunning) return;
        g_liveRunning = true;
    }
    std::thread([]() {
        bool markApplied = false;
        for (;;) {
            std::pair<int, int> t;
            { std::lock_guard<std::mutex> l(g_liveMx);
              if (g_liveDirty.empty()) { markApplied = !g_liveFailed; g_liveFailed = false; g_liveRunning = false; break; }
              t = *g_liveDirty.begin(); g_liveDirty.erase(g_liveDirty.begin()); }
            if (!TerrainLiveHasTexture(t.first, t.second)) continue;
            std::vector<uint8_t> chain; std::vector<TerrainStroke> st; TileInfo ti; std::vector<float> prev, next;
            { std::lock_guard<std::mutex> l(g_mx); auto it = g_cpu.find(t); if (it == g_cpu.end() || !it->second.ok || it->second.chain.size() < 699050) continue;
              chain = it->second.chain; ti = g_info[t]; for (const auto& s : g_strokes) if (StrokeTouchesTile(s, t.first, t.second)) st.push_back(s);
              auto& tc = it->second; next = tc.edit;
              if (tc.gpuValid) prev = tc.gpu; else { prev = tc.orig; for (const auto& s : tc.gpuStrokes) ApplyStrokeCpu(prev, t.first, t.second, s); } }
            if (ti.range <= 0) continue;
            // the chain holds file bytes [128, ...): build the buffer the patch expects (file offset 128, length of the chain)
            const uint32_t n = st.empty() ? 0 : ApplyGuarded(chain.data(), kHeader, (uint32_t)chain.size(), t.first, t.second, ti.range, ti.offset, st.data(), (int)st.size());
            const bool ok = TerrainLiveUpload(t.first, t.second, chain.data(), chain.size());
            int patches = 0;
            if (ok) { patches = TerrainPhysSync(t.first, t.second, prev.data(), next.data());   // the collision takes the same change
                std::lock_guard<std::mutex> l(g_mx); auto& tc = g_cpu[t]; tc.gpu.swap(next); tc.gpuValid = true; }
            g_liveDone++; if (!ok) { std::lock_guard<std::mutex> l(g_liveMx); g_liveFailed = true; }
            Log("[terrain] live: tile %d,%d, %zu strokes, %u samples -> %s, %d collision patches", t.first, t.second, st.size(), n, ok ? "uploaded" : "FAILED", patches);
            Sleep(30);   // coalesce a drag: the next pass takes every stroke painted meanwhile
        }
        if (markApplied) {
            int generation = 0; { std::lock_guard<std::mutex> l(g_mx); generation = g_gen; }
            MarkAppliedAtGeneration(generation);
        }
    }).detach();
}
void TerrainAddStroke(const TerrainStroke& s) {
    TerrainStroke owned = s;
    if (!owned.proj) { owned.proj = EnsureEditingProject(); if (!owned.proj) { Log("[terrain] stroke refused: no editable project is available"); return; } }
    const auto parts = SplitStroke(owned);
    std::vector<std::pair<int, int>> t;
    { std::lock_guard<std::mutex> l(g_mx); for (const auto& part : parts) {
          g_strokes.push_back(part); TilesOf(part, &t);
          const auto tile = StrokeOwnerTile(part); g_edited.insert(tile);
          auto it = g_cpu.find(tile); if (it != g_cpu.end() && it->second.ok) ApplyStrokeCpu(it->second.edit, tile.first, tile.second, part);
      }
      g_needsApply = true; g_gen++; }
    MarkProjectDirty(owned.proj);
    FetchTables();
    QueueLive(UniqueTiles(t));
}
bool TerrainUndo() {
    int proj = 0; std::vector<std::pair<int, int>> touched;
    { std::lock_guard<std::mutex> l(g_mx); if (g_strokes.empty()) return false; proj = g_strokes.back().proj;
      TilesOf(g_strokes.back(), &touched); g_strokes.pop_back(); RebuildEditedLocked(); RecomputeCpuLocked(); g_needsApply = true; }
    if (proj) MarkProjectDirty(proj);
    QueueLive(UniqueTiles(touched));
    return true;
}
void TerrainClear() {
    std::set<int> dirty; std::vector<std::pair<int, int>> changedTiles;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_strokes.empty()) g_needsApply = true;
        std::vector<std::pair<int, int>> touched;
        for (const auto& s : g_strokes) { if (s.proj) dirty.insert(s.proj); TilesOf(s, &touched); }
        changedTiles = UniqueTiles(touched);
        g_strokes.clear(); RebuildEditedLocked(); RecomputeCpuLocked();
    }
    for (int proj : dirty) MarkProjectDirty(proj);
    QueueLive(changedTiles);
    Log("[terrain] strokes cleared");
}
std::vector<TerrainStroke> TerrainStrokes() { std::lock_guard<std::mutex> l(g_mx); return g_strokes; }
bool TerrainRemoveProject(int project, std::vector<TerrainStroke>& removed, std::vector<size_t>& positions) {
    removed.clear(); positions.clear();
    if (project <= 0) return false;
    std::vector<std::pair<int, int>> changedTiles;
    {
        std::lock_guard<std::mutex> l(g_mx);
        std::vector<std::pair<int, int>> touched;
        for (size_t i = 0; i < g_strokes.size(); ++i) if (g_strokes[i].proj == project) {
            removed.push_back(g_strokes[i]); positions.push_back(i); TilesOf(g_strokes[i], &touched);
        }
        if (removed.empty()) return false;
        g_strokes.erase(std::remove_if(g_strokes.begin(), g_strokes.end(), [project](const TerrainStroke& s) { return s.proj == project; }), g_strokes.end());
        changedTiles = UniqueTiles(touched);
        RebuildEditedLocked(); RecomputeCpuLocked(); g_needsApply = true;
    }
    MarkProjectDirty(project);
    QueueLive(changedTiles);
    Log("[terrain] project %d: removed %zu terrain strokes", project, removed.size());
    return true;
}
bool TerrainRemoveTile(int tx, int tz, int project, std::vector<TerrainStroke>& removed, std::vector<size_t>& positions) {
    removed.clear(); positions.clear();
    std::set<int> dirty; std::vector<std::pair<int, int>> changedTiles;
    {
        std::lock_guard<std::mutex> l(g_mx);
        std::vector<std::pair<int, int>> touched;
        for (size_t i = 0; i < g_strokes.size(); ++i) {
            const auto& s = g_strokes[i]; const auto owner = StrokeOwnerTile(s);
            if (owner.first != tx || owner.second != tz || (project >= 0 && s.proj != project)) continue;
            removed.push_back(s); positions.push_back(i);
            if (s.proj) dirty.insert(s.proj); TilesOf(s, &touched);
        }
        if (removed.empty()) return false;
        g_strokes.erase(std::remove_if(g_strokes.begin(), g_strokes.end(), [=](const TerrainStroke& s) {
            const auto owner = StrokeOwnerTile(s); return owner.first == tx && owner.second == tz && (project < 0 || s.proj == project);
        }), g_strokes.end());
        changedTiles = UniqueTiles(touched);
        RebuildEditedLocked(); RecomputeCpuLocked(); g_needsApply = true;
    }
    for (int proj : dirty) MarkProjectDirty(proj);
    QueueLive(changedTiles);
    Log("[terrain] tile %d,%d project %d: removed terrain strokes", tx, tz, project);
    return true;
}
void TerrainRestoreStrokes(const std::vector<TerrainStroke>& strokes, const std::vector<size_t>& positions) {
    if (strokes.empty()) return;
    std::set<int> dirty; std::vector<std::pair<int, int>> touched;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (size_t i = 0; i < strokes.size(); ++i) {
            const size_t at = i < positions.size() ? std::min(positions[i], g_strokes.size()) : g_strokes.size();
            g_strokes.insert(g_strokes.begin() + at, strokes[i]);
            if (strokes[i].proj) dirty.insert(strokes[i].proj); TilesOf(strokes[i], &touched);
        }
        RebuildEditedLocked(); RecomputeCpuLocked(); g_needsApply = true;
    }
    for (int proj : dirty) MarkProjectDirty(proj);
    FetchTables(); QueueLive(UniqueTiles(touched));
}
void TerrainSetProject(int from, int to) { std::lock_guard<std::mutex> l(g_mx); bool changed = false;
    for (auto& s : g_strokes) if (s.proj == from && from != to) { s.proj = to; changed = true; }
    if (changed) g_gen++;
}
// A project's strokes as loaded from its file: replaces what that project had. Identical strokes (the startup preload followed
// by the autoload of the same project) change nothing and need no apply.
void TerrainReplaceProject(int proj, const std::vector<TerrainStroke>& strokes) {
    std::vector<TerrainStroke> normalized;
    for (const auto& stroke : strokes) {
        auto parts = SplitStroke(stroke);
        for (auto& part : parts) { part.proj = proj; normalized.push_back(part); }
    }
    std::vector<std::pair<int, int>> changedTiles;
    {
        std::lock_guard<std::mutex> l(g_mx);
        std::vector<TerrainStroke> old; for (auto& s : g_strokes) if (s.proj == proj) old.push_back(s);
        bool same = old.size() == normalized.size();
        for (size_t i = 0; same && i < old.size(); i++) {
            const auto& x = old[i]; const auto& y = normalized[i];
            same = x.mode == y.mode && x.x == y.x && x.z == y.z && x.r == y.r && x.amount == y.amount &&
                x.strength == y.strength && x.ax == y.ax && x.az == y.az && x.y == y.y && x.proj == y.proj &&
                x.tileScoped == y.tileScoped && x.tileX == y.tileX && x.tileZ == y.tileZ;
        }
        if (same) return;
        std::vector<std::pair<int, int>> touched;
        for (const auto& s : old) TilesOf(s, &touched);
        for (const auto& s : normalized) TilesOf(s, &touched);
        g_strokes.erase(std::remove_if(g_strokes.begin(), g_strokes.end(), [proj](const TerrainStroke& s) { return s.proj == proj; }), g_strokes.end());
        for (const auto& s : normalized) g_strokes.push_back(s);
        changedTiles = UniqueTiles(touched);
        RebuildEditedLocked(); RecomputeCpuLocked(); g_needsApply = true;
    }
    FetchTables(); QueueLive(changedTiles);
}
void TerrainMarkApplied() { std::lock_guard<std::mutex> l(g_mx); g_needsApply = false; }
bool TerrainNeedsApply() { std::lock_guard<std::mutex> l(g_mx); return g_needsApply && g_ok; }
int TerrainEditDisc(float x, float z, float radius, float metres) {   // research API: one raise / lower stroke
    if (!g_ok || radius <= 0) return 0;
    TerrainStroke s{}; s.mode = TerrainRaise; s.x = s.ax = x; s.z = s.az = z; s.r = radius; s.amount = metres; s.strength = 1; TerrainAddStroke(s);
    std::lock_guard<std::mutex> l(g_mx); return (int)g_edited.size();
}
void TerrainEditClear() { TerrainClear(); }

// Apply: a fast travel 5 km away (to a spot that has terrain) and back streams the edited tiles again.
static bool TileExists(float x, float z) { float r = 0, o = 0; return TileRange((int)std::floor(x / 1024.0f), (int)std::floor(z / 1024.0f), &r, &o); }
static void SetApply(int step, const char* text) { std::lock_guard<std::mutex> l(g_mx); g_applyText = text; g_applyStep = step; }
std::string TerrainApplyState() { std::lock_guard<std::mutex> l(g_mx); return g_applyStep ? g_applyText : std::string(); }
static bool WaitArrive(Vec3 at, int seconds) {   // the player stands near 'at' for 3 s (the loading screen is over)
    DWORD since = 0;
    for (int i = 0; i < seconds * 4; i++) {
        Sleep(250); PosInfo p{};
        if (PlayerPosInfo(&p) && std::fabs(p.world.x - at.x) < 150.0f && std::fabs(p.world.z - at.z) < 150.0f) { if (!since) since = GetTickCount(); else if (GetTickCount() - since > 3000) return true; }
        else since = 0;
    }
    return false;
}
bool TerrainApply(Vec3 back) {
    if (!g_ok || !TravelAvailable()) return false;
    int idle = 0; if (!g_applyStep.compare_exchange_strong(idle, 1)) return false;
    std::thread([back]() {
        SetApply(1, "preparing the fast travel");
        TravelPrepare(); for (int i = 0; i < 240 && !TravelPrepared(); i++) Sleep(250);
        if (!TravelPrepared()) { SetApply(0, ""); Log("[terrain] apply: travel system not ready (%s)", TravelStatus().c_str()); return; }
        Vec3 away = back; const float d = 5000.0f; const float dirs[4][2] = { { d, 0 }, { -d, 0 }, { 0, d }, { 0, -d } }; bool found = false;
        for (auto& dv : dirs) if (TileExists(back.x + dv[0], back.z + dv[1])) { away = { back.x + dv[0], back.y + 300.0f, back.z + dv[1] }; found = true; break; }
        if (!found) { SetApply(0, ""); Log("[terrain] apply: no terrain 5 km around"); return; }
        SetApply(2, "travelling away (loading screen)");
        if (!TravelTo(away, 0) || !WaitArrive(away, 120)) { SetApply(0, ""); Log("[terrain] apply: did not arrive away"); return; }
        SetApply(3, "travelling back (loading screen)");
        if (!TravelTo({ back.x, back.y + 1.0f, back.z }, 0) || !WaitArrive(back, 120)) { SetApply(0, ""); Log("[terrain] apply: did not arrive back"); return; }
        TerrainMarkApplied(); SetApply(0, ""); Log("[terrain] apply done");
    }).detach();
    return true;
}

int TerrainPreviewGen() { std::lock_guard<std::mutex> l(g_mx); return g_gen; }
bool TerrainTilePreview(int tx, int tz, int project, int dim, std::vector<float>* delta) {
    if (!delta || dim < 1 || dim > 64) return false;
    std::lock_guard<std::mutex> l(g_mx);
    const auto it = g_cpu.find({ tx, tz });
    if (it == g_cpu.end() || !it->second.ok) return false;
    const auto& original = it->second.orig;
    std::vector<float> edited = original;
    for (const auto& stroke : g_strokes)
        if (stroke.proj == project && StrokeTouchesTile(stroke, tx, tz)) ApplyStrokeCpu(edited, tx, tz, stroke);
    delta->assign((size_t)dim * dim, 0.0f);
    for (int row = 0; row < kTile; ++row) for (int col = 0; col < kTile; ++col) {
        const size_t sample = (size_t)row * kTile + col;
        const int x = col * dim / kTile, y = (kTile - 1 - row) * dim / kTile;
        float& cell = (*delta)[(size_t)y * dim + x];
        const float change = edited[sample] - original[sample];
        if (std::fabs(change) > std::fabs(cell)) cell = change;
    }
    return true;
}
// Heights on the 2 m texel grid for the preview: sample (i, j) is the texel containing (x0 + 2i, z0 + 2j); NaN where the tile's
// copy is not loaded. x0 / z0 should be even (texel edges) so the samples are the texel centres at x0 + 2i + 1.
bool TerrainPreviewGrid(float x0, float z0, int nx, int nz, std::vector<float>* orig, std::vector<float>* edit) {
    if (nx <= 0 || nz <= 0 || nx * nz > 250000) return false;
    orig->assign((size_t)nx * nz, NAN); edit->assign((size_t)nx * nz, NAN);
    std::lock_guard<std::mutex> l(g_mx); bool any = false;
    for (int j = 0; j < nz; j++) for (int i = 0; i < nx; i++) {
        const float x = x0 + 2.0f * i + 1.0f, z = z0 + 2.0f * j + 1.0f;
        const int tx = (int)std::floor(x / 1024.0f), tz = (int)std::floor(z / 1024.0f);
        auto it = g_cpu.find({ tx, tz }); if (it == g_cpu.end() || !it->second.ok) continue;
        const int c = (int)std::floor((x - tx * 1024.0f) / 2.0f), r = kTile - 1 - (int)std::floor((z - tz * 1024.0f) / 2.0f);
        if (c < 0 || r < 0 || c >= kTile || r >= kTile) continue;
        const size_t k = (size_t)r * kTile + c; (*orig)[(size_t)j * nx + i] = it->second.orig[k]; (*edit)[(size_t)j * nx + i] = it->second.edit[k]; any = true;
    }
    return any;
}
std::string TerrainStatus() {
    std::lock_guard<std::mutex> l(g_mx); char b[200];
    snprintf(b, sizeof b, "%s, %zu strokes on %zu tiles, %ld reads patched, %ld missed", g_why.c_str(), g_strokes.size(), g_edited.size(), (long)g_patched, (long)g_missed);
    return b;
}

}   // namespace core
