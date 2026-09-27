// Live terrain collision: when a tile's texture is rewritten live (terrain_live.cpp), the Havok heightfield patches around the
// player get the same height change, so the new shape is walkable without a reload.
//
// The collision patches are hknpHeightFieldShape objects (notes/FORMATS.md, "Terrain"): 65 x 65 float heights 0.5 m apart
// covering a 32 x 32 m cell. heights[a * 65 + b] lies at world (I * 32 + a / 2, J * 32 + b / 2) for the cell (I, J) - found by
// matching every live patch against the tile heights. The game captures them from the rendered terrain, so a patch made
// after a live upload already carries it; the ones that exist get "old height + (new field - previous field)" (keeps their
// detail), then their bounding-volume tree and AABB are rebuilt from the new heights the way the game builds them.
//
// Patches are collected by a hook on the shape's constructor (signature); every access checks the vtable and the heights
// pointer first, because patches are freed when they stream out.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace core {

struct Patch { uintptr_t heights = 0; int I = 0, J = 0; bool placed = false, tried = false; };
static std::mutex g_pmx; static std::map<uintptr_t, Patch> g_patches;   // shape -> what we know about it
static uintptr_t g_shapeVt = 0; static bool g_physOk = false;
typedef uintptr_t(__fastcall* ShapeCtorFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static ShapeCtorFn g_origCtor = nullptr;

static int64_t Rel(uintptr_t field) { int64_t v = 0; ReadBytes(field, &v, 8); return v; }
// shape -> heights array (4225 floats) through geometry (+0x38 relative) and its data object (+0x38, heights at +0x18); 0 = gone
static uintptr_t HeightsOf(uintptr_t sh) {
    uintptr_t vt = 0; uint32_t tag = 0; if (!ReadBytes(sh, &vt, 8) || vt != g_shapeVt || !ReadBytes(sh + 8, &tag, 4) || tag != 0xFFFFFFFFu) return 0;
    const int64_t rg = Rel(sh + 0x38); if (!rg) return 0;
    uintptr_t data = 0; if (!ReadBytes(sh + 0x38 + rg + 0x38, &data, 8) || !data) return 0;
    uintptr_t hp = 0; uint32_t n = 0; if (!ReadBytes(data + 0x18, &hp, 8) || !ReadBytes(data + 0x20, &n, 4) || n != 4225) return 0;
    return hp;
}

static uintptr_t __fastcall HookCtor(uintptr_t self, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {
    const uintptr_t r = g_origCtor(self, b, c, d, e, f, g, h);
    { std::lock_guard<std::mutex> l(g_pmx); g_patches[self] = Patch{}; if (g_patches.size() > 4000) g_patches.clear(); }
    return r;
}

void TerrainPhysInstall() {
    g_shapeVt = VtableByName(".?AVhknpHeightFieldShape@@");
    const uintptr_t ctor = SigScanUnique("48 8B C4 48 89 58 10 48 89 70 18 55 57 41 54 41 55 41 57 48 8B EC 48 83 EC 70 48 C7 41 10 01 00 00 00 45 33 ED 0F 29 70 C8 48 8B F9 4C 89 70 08");
    if (!g_shapeVt || !ctor) { Log("[terrain] live collision unavailable (%s)", !g_shapeVt ? "shape class not found" : "shape constructor not found"); return; }
    g_physOk = InstallInternalHook((void*)ctor, (void*)HookCtor, (void**)&g_origCtor, "heightfield shape constructor (terrain)");
    Log("[terrain] live collision %s", g_physOk ? "ready" : "unavailable");
}

// bilinear sample of a 512 x 512 tile field (texel centres at ox + 2c + 1, oz + 2(511 - r) + 1)
static float Field(const float* f, int tx, int tz, float x, float z) {
    const float c = (x - tx * 1024.0f - 1.0f) / 2.0f, r = 511.0f - (z - tz * 1024.0f - 1.0f) / 2.0f;
    int c0 = (int)std::floor(c), r0 = (int)std::floor(r); c0 = std::min(510, std::max(0, c0)); r0 = std::min(510, std::max(0, r0));
    const float fc = std::min(1.0f, std::max(0.0f, c - c0)), fr = std::min(1.0f, std::max(0.0f, r - r0));
    auto v = [&](int rr, int cc) { return f[(size_t)rr * 512 + cc]; };
    return v(r0, c0) * (1 - fc) * (1 - fr) + v(r0, c0 + 1) * fc * (1 - fr) + v(r0 + 1, c0) * (1 - fc) * fr + v(r0 + 1, c0 + 1) * fc * fr;
}

// Which 32 m cell of tile (tx, tz) a patch covers: the cell whose field heights match the patch's (every 4th sample).
static bool Place(Patch& p, const float* hs, const float* field, int tx, int tz, Vec3 nearPos) {
    float best = 1e9f, second = 1e9f; int bi = 0, bj = 0;
    const int ci = (int)std::floor(nearPos.x / 32.0f), cj = (int)std::floor(nearPos.z / 32.0f);
    for (int I = ci - 6; I <= ci + 6; I++) for (int J = cj - 6; J <= cj + 6; J++) {
        if (I * 32 < tx * 1024 || I * 32 + 32 > tx * 1024 + 1024 || J * 32 < tz * 1024 || J * 32 + 32 > tz * 1024 + 1024) continue;
        float d[289]; int n = 0;
        for (int a = 0; a < 65; a += 4) for (int b = 0; b < 65; b += 4) d[n++] = hs[a * 65 + b] - Field(field, tx, tz, I * 32.0f + a * 0.5f, J * 32.0f + b * 0.5f);
        std::nth_element(d, d + n / 2, d + n); const float med = d[n / 2]; float dev[289];
        for (int k = 0; k < n; k++) dev[k] = std::fabs(d[k] - med);
        std::nth_element(dev, dev + n / 2, dev + n); const float e = dev[n / 2] + 0.25f * std::fabs(med);
        if (e < best) { second = best; best = e; bi = I; bj = J; } else if (e < second) second = e;
    }
    if (best > 0.6f || second < best * 1.5f) return false;   // no clear match (not in this tile, or flat ground that fits anywhere)
    p.I = bi; p.J = bj; p.placed = true; return true;
}

// The game's bounding-volume tree for 65 x 65 heights: levels of 8x8 / 4x4 / 2x2 / 1x1 nodes, each 4 child minima then 4 child
// maxima as u16 over [base, base + 65535 * scale]; children (0,0) (1,0) (1,1) (0,1); min floor - 1, max floor + 1 (as the game).
static void BuildTree(const float* hs, float* base, float* scale, std::vector<uint16_t> lv[4]) {
    float mn = hs[0], mx = hs[0]; for (int i = 1; i < 4225; i++) { mn = std::min(mn, hs[i]); mx = std::max(mx, hs[i]); }
    *base = mn; *scale = std::max(1e-6f, (mx - mn) / 65535.0f);
    static const int ch[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int l = 0, w = 8; l < 4; l++, w >>= 1) {
        const int q = 64 / (2 * w); lv[l].assign((size_t)w * w * 8, 0);
        for (int x = 0; x < w; x++) for (int z = 0; z < w; z++) for (int k = 0; k < 4; k++) {
            const int a = 2 * x + ch[k][0], b = 2 * z + ch[k][1]; float bmin = 1e30f, bmax = -1e30f;
            for (int i = a * q; i <= (a + 1) * q; i++) for (int j = b * q; j <= (b + 1) * q; j++) { const float v = hs[i * 65 + j]; bmin = std::min(bmin, v); bmax = std::max(bmax, v); }
            const float lo = std::floor((bmin - *base) / *scale) - 1.0f, hi = std::floor((bmax - *base) / *scale) + 1.0f;
            lv[l][((size_t)x * w + z) * 8 + k] = (uint16_t)std::min(65535.0f, std::max(0.0f, lo));
            lv[l][((size_t)x * w + z) * 8 + 4 + k] = (uint16_t)std::min(65535.0f, std::max(0.0f, hi));
        }
    }
}

// Writes new heights into a live patch: first a tree that covers everything (no query is cut off while the heights change),
// then the heights, then the exact tree and the shape AABB.
static bool WritePatch(uintptr_t sh, uintptr_t hp, const float* hs) {
    const int64_t rb = Rel(sh + 0x40); if (!rb) return false;
    const uintptr_t bv = sh + 0x40 + rb;
    const int64_t ra = Rel(bv + 0x20); uint32_t nl = 0; ReadBytes(bv + 0x28, &nl, 4); if (!ra || nl != 4) return false;
    const uintptr_t arr = bv + 0x20 + ra; uintptr_t lvData[4]; uint32_t lvBytes[4];
    for (int k = 0; k < 4; k++) { const uintptr_t e = arr + k * 24; const int64_t r = Rel(e); uint32_t cnt = 0; ReadBytes(e + 8, &cnt, 4); lvData[k] = e + r; lvBytes[k] = cnt * 4; }
    static const uint32_t want[4] = { 1024, 256, 64, 16 };
    for (int k = 0; k < 4; k++) if (lvBytes[k] != want[k]) return false;
    float base = 0, scale = 1; std::vector<uint16_t> lv[4]; BuildTree(hs, &base, &scale, lv);
    float mn = hs[0], mx = hs[0]; for (int i = 1; i < 4225; i++) { mn = std::min(mn, hs[i]); mx = std::max(mx, hs[i]); }
    float oldBase = 0; ReadBytes(bv + 0x30, &oldBase, 4); float oldScale = 0; ReadBytes(bv + 0x40, &oldScale, 4);
    const float wideLo = std::min(mn, oldBase) - 1.0f, wideHi = std::max(mx, oldBase + oldScale * 65535.0f) + 1.0f, ws = (wideHi - wideLo) / 65535.0f;
    std::vector<uint16_t> open[4]; for (int k = 0; k < 4; k++) { open[k].resize(lvBytes[k] / 2); for (size_t i = 0; i < open[k].size(); i++) open[k][i] = (i % 8) < 4 ? 0 : 65535; }
    auto head = [&](float b, float s) { float h[6] = { b, b, b, b, s, 1.0f / s }; return WriteMem(bv + 0x30, h, sizeof h); };
    bool ok = head(wideLo, ws);
    for (int k = 0; k < 4 && ok; k++) ok = WriteMem(lvData[k], open[k].data(), lvBytes[k]);
    float aabb[8] = {}; ReadBytes(sh + 0x90, aabb, sizeof aabb);
    aabb[1] = wideLo; aabb[5] = wideHi; ok = ok && WriteMem(sh + 0x90, aabb, sizeof aabb);
    ok = ok && WriteMem(hp, hs, 4225 * sizeof(float));
    ok = ok && head(base, scale);
    for (int k = 0; k < 4 && ok; k++) ok = WriteMem(lvData[k], lv[k].data(), lvBytes[k]);
    aabb[1] = mn; aabb[5] = mx; ok = ok && WriteMem(sh + 0x90, aabb, sizeof aabb);
    return ok;
}

// A tile's texture went from 'prev' to 'next' (512 x 512 metres each): the live patches in that tile follow.
int TerrainPhysSync(int tx, int tz, const float* prev, const float* next) {
    if (!g_physOk) return 0;
    PosInfo pi{}; const bool havePos = PlayerPosInfo(&pi); const Vec3 nearPos = havePos ? pi.world : Vec3{ tx * 1024.0f + 512.0f, 0, tz * 1024.0f + 512.0f };
    std::vector<std::pair<uintptr_t, Patch>> list;
    { std::lock_guard<std::mutex> l(g_pmx); for (auto& kv : g_patches) list.push_back(kv); }
    int written = 0; std::vector<float> hs(4225);
    for (auto& [sh, p] : list) {
        const uintptr_t hp = HeightsOf(sh); if (!hp) { std::lock_guard<std::mutex> l(g_pmx); g_patches.erase(sh); continue; }
        if (!ReadBytes(hp, hs.data(), 4225 * sizeof(float))) continue;
        if (p.heights != hp) { p = Patch{}; p.heights = hp; }
        if (!p.placed) {
            if (p.tried) continue;
            p.tried = !Place(p, hs.data(), prev, tx, tz, nearPos);
            { std::lock_guard<std::mutex> l(g_pmx); g_patches[sh] = p; }
            if (!p.placed) continue;
        }
        if (p.I * 32 < tx * 1024 || p.I * 32 >= tx * 1024 + 1024 || p.J * 32 < tz * 1024 || p.J * 32 >= tz * 1024 + 1024) continue;
        float dmax = 0;
        for (int a = 0; a < 65; a++) for (int b = 0; b < 65; b++) {
            const float x = p.I * 32.0f + a * 0.5f, z = p.J * 32.0f + b * 0.5f;
            const float d = Field(next, tx, tz, x, z) - Field(prev, tx, tz, x, z); hs[a * 65 + b] += d; dmax = std::max(dmax, std::fabs(d));
        }
        if (dmax < 0.005f) continue;
        if (WritePatch(sh, hp, hs.data())) written++; else Log("[terrain] collision patch %d,%d: write failed", p.I, p.J);
        { std::lock_guard<std::mutex> l(g_pmx); g_patches[sh] = p; }
    }
    return written;
}

}   // namespace core
