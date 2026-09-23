// thumbgen.cpp - prefab thumbnails generated in-game from the installed pack files.
// Pipeline: file data via the game's resource loader -> prefab (PARC chunks, reflection serializer) -> SceneObject tree (_worldTransform, _path)
//   -> .pami (XML, names the .pam) -> .pam (quantized static mesh) -> flat shaded software render
//   -> bin64\cdmodkit\thumbs\<fnv64(prefab path)>.png (+ bounding box in prefab_size.tsv)
// Format knowledge ported from pycrimson (reflection) and CDMW mesh_parser (pam layouts).
#define NOMINMAX
#include "guard.h"
#include "core.h"
#include "thumbgen.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <set>
#include <atomic>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <fstream>
void* CdHeapAlloc(size_t n); void* CdHeapRealloc(void* p, size_t n); void CdHeapFree(void* p);   // heap.cpp
#define STBIW_MALLOC(sz)        CdHeapAlloc(sz)
#define STBIW_REALLOC(p, newsz) CdHeapRealloc(p, newsz)
#define STBIW_FREE(p)           CdHeapFree(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace thumbgen {
using core::Log;

// ---------------------------------------------------------------- byte helpers
static inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t  rdi32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }
static inline int16_t  rdi16(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }
static inline float    rdf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
static bool EndsWith(const std::string& s, const char* suf) { size_t n = strlen(suf); return s.size() >= n && s.compare(s.size() - n, n, suf) == 0; }

// Pack files are read through the game's own resource loader (core::GameReadFile): the mod contains no archive
// format code and no key; the game decrypts and decompresses its files exactly as it does for itself.
static std::atomic<int> g_reads{ 0 };
static bool GetFile(std::string path, std::vector<uint8_t>& out) {
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (core::GameReadFile(path, out)) { g_reads++; return true; }
        Sleep(15);
    }
    return false;
}

// ---------------------------------------------------------------- reflection serializer (prefab / level objects)
struct RProp { std::string name, typeName; uint16_t type, fixed; uint32_t flags; };
struct RType { std::string name; std::vector<RProp> props; };
struct Node { bool hasXf = false; float xf[10] = { 0 }; std::string path; std::vector<Node> kids; };

struct Reader {
    const uint8_t* d; size_t n, pos;
    void need(size_t k) const { if (pos + k > n) throw std::runtime_error("eof"); }
    uint8_t  u8() { need(1); return d[pos++]; }
    uint16_t u16() { need(2); uint16_t v = rd16(d + pos); pos += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = rd32(d + pos); pos += 4; return v; }
    int32_t  i32() { need(4); int32_t v = rdi32(d + pos); pos += 4; return v; }
    int16_t  i16() { need(2); int16_t v = rdi16(d + pos); pos += 2; return v; }
    int64_t  i64() { need(8); int64_t v; memcpy(&v, d + pos, 8); pos += 8; return v; }
    const uint8_t* bytes(size_t k) { need(k); const uint8_t* p = d + pos; pos += k; return p; }
    std::string str(size_t k) { need(k); std::string s((const char*)d + pos, k); pos += k; return s; }
    void seek(size_t p) { if (p > n) throw std::runtime_error("seek"); pos = p; }
};

class Parser {
public:
    std::vector<Node> objects;
    Parser(const uint8_t* d, size_t n, size_t start, bool hasShared) : r{ d, n, start }, base(start) {
        uint16_t tc = r.u16();
        if (tc == 0xFFFF) { metaVer = r.u32(); if (metaVer >= 4) r.i64(); serVer = r.u32(); tc = r.u16(); }
        types.resize(tc);
        for (auto& t : types) {
            t.name = r.str(r.u32()); uint16_t pc = r.u16(); t.props.resize(pc);
            for (auto& p : t.props) { p.name = r.str(r.u32()); p.typeName = r.str(r.u32()); p.type = r.u16(); p.fixed = r.u16(); p.flags = serVer >= 9 ? r.u32() : 0; }
        }
        if (serVer > 0xD) { uint32_t c = r.u32(); for (uint32_t i = 0; i < c; i++) r.str(r.u32()); }
        if (hasShared) { uint32_t c = r.u32(); for (uint32_t i = 0; i < c; i++) { uint32_t idx = r.u32(); shared[(int32_t)idx] = r.str(r.u32()); } }
        uint32_t objCount = r.u32();
        if (metaVer >= 2) r.u32();
        if (objCount > 1000000) throw std::runtime_error("object count");
        if (metaVer >= 3) {
            infos.resize(objCount);
            for (auto& in : infos) {
                in.type = r.u16(); if (serVer >= 8) r.u16();
                if (serVer >= 11) r.i64(); else if (serVer >= 8) r.i32(); else r.i16();
                in.off = r.u32(); in.size = r.u32();
            }
            for (uint32_t i = 0; i < objCount; i++) { Node n; ParseObject(ReadMeta((int)i, false), n); objects.push_back(std::move(n)); }
        } else {
            for (uint32_t i = 0; i < objCount; i++) { Node n; ParseObject(ReadMeta(-1, false), n); objects.push_back(std::move(n)); }
        }
    }
private:
    Reader r; size_t base; uint32_t metaVer = 0, serVer = 0;
    std::vector<RType> types; std::unordered_map<int32_t, std::string> shared;
    struct Info { uint16_t type; uint32_t off, size; }; std::vector<Info> infos;
    int depth = 0;
    struct Meta { std::vector<uint8_t> bitmap; const RType* type; bool hasMeta; };
    static bool IsArray(uint16_t t) { return t == 3 || t == 6 || t == 7 || t == 9 || t == 10; }

    Meta ReadMeta(int index, bool hasNameIndex) {
        Meta m; uint16_t ti = 0; m.hasMeta = true;
        if (metaVer >= 3 && index >= 0) { ti = infos[index].type; m.hasMeta = false; r.seek(base + infos[index].off); }
        uint16_t bl = r.u16(); const uint8_t* b = r.bytes(bl); m.bitmap.assign(b, b + bl);
        if (m.hasMeta) {
            ti = r.u16();
            if (serVer >= 3) {
                if (serVer >= 0xB) { r.u8(); r.i64(); if (serVer >= 0xE && hasNameIndex) r.i32(); }
                else if (serVer >= 8) r.i32(); else r.i16();
            }
            uint32_t vo = r.u32(); r.seek(base + vo);
        }
        if (ti >= types.size()) throw std::runtime_error("type index");
        m.type = &types[ti];
        return m;
    }
    void ParseObject(const Meta& m, Node& out) {
        if (++depth > 200) throw std::runtime_error("depth");
        if (serVer >= 0xA) r.u8();
        bool noTags = false; if (serVer >= 5) noTags = r.u8() == 1;
        if (!noTags && serVer >= 6) { uint16_t c = r.u16(); for (uint16_t i = 0; i < c; i++) { r.u16(); r.str(r.u32()); } }
        const auto& props = m.type->props;
        for (size_t i = 0; i < props.size(); i++) {
            const RProp& p = props[i];
            bool missing = (i / 8 >= m.bitmap.size()) || ((m.bitmap[i / 8] >> (i & 7)) & 1) == 0;
            if (serVer < 7 && missing) continue;
            if (serVer >= 9 && (p.flags & ((1u << 7) | (1u << 1)))) continue;
            if (!IsArray(p.type) && missing) continue;
            ParseProp(p, out);
        }
        if (m.hasMeta) r.u32();   // stored object size
        depth--;
    }
    void ParseProp(const RProp& p, Node& obj) {
        if (serVer >= 0xF && IsArray(p.type) && r.u8() == 1) return;   // empty array marker
        switch (p.type) {
        case 0: {   // fixed size value
            const uint8_t* b = r.bytes(p.fixed);
            if (p.fixed >= 40 && p.name == "_worldTransform" && p.typeName == "Transform") { obj.hasXf = true; for (int i = 0; i < 10; i++) obj.xf[i] = rdf(b + 4 * i); }
            break; }
        case 1: {   // size prefixed (strings), shared string table when present
            std::string s; bool got = false;
            if (!shared.empty()) { int32_t idx = r.i32(); if (idx != -1) { auto it = shared.find(idx); if (it != shared.end()) s = it->second; got = true; } }
            if (!got) { uint32_t len = r.u32(); s = r.str(len); }
            if (p.name == "_path") obj.path = s;
            break; }
        case 2: r.bytes(p.fixed); break;
        case 3: { uint32_t c = r.u32(); r.bytes((size_t)c * p.fixed); break; }
        case 4: { Node k; ParseObject(ReadMeta(-1, false), k); obj.kids.push_back(std::move(k)); break; }
        case 5: { if (r.u8() == 0) break; Node k; ParseObject(ReadMeta(-1, false), k); obj.kids.push_back(std::move(k)); break; }
        case 6: case 7: {
            uint32_t c = r.u32();
            bool named = serVer >= 0xE ? r.u8() == 1 : false;
            if (serVer >= 0xB) r.i64(); else if (serVer >= 8) r.i32(); else if (serVer >= 4) r.i16();
            if (serVer >= 0xB) { int32_t uc = r.i32(); if (uc > 0) { r.bytes((size_t)uc * 8); if (named) r.bytes((size_t)uc * 4); } }
            if (c > 1000000) throw std::runtime_error("array count");
            for (uint32_t i = 0; i < c; i++) { Node k; ParseObject(ReadMeta(-1, named), k); obj.kids.push_back(std::move(k)); }
            break; }
        case 10: {
            uint32_t c = r.u32();
            for (uint32_t i = 0; i < c; i++) { if (!shared.empty() && r.i32() != -1) continue; uint32_t len = r.u32(); r.bytes((size_t)len * p.fixed); }
            break; }
        default: throw std::runtime_error("property type");
        }
    }
};

// PARC container: chunks "PARC" + u16 kind + 10 zero bytes + reflection block (with shared strings)
static void ParsePrefab(const std::vector<uint8_t>& d, std::vector<Node>& roots) {
    if (d.size() >= 4 && memcmp(d.data(), "PARC", 4) == 0) {
        for (size_t i = 0; i + 18 <= d.size(); i++) {
            const void* hit = memchr(d.data() + i, 'P', d.size() - i - 17); if (!hit) break;
            i = (const uint8_t*)hit - d.data();
            if (memcmp(d.data() + i, "PARC", 4) != 0) continue;
            bool zeros = true; for (int k = 6; k < 16; k++) if (d[i + k]) { zeros = false; break; }
            if (!zeros || d[i + 16] != 0xFF || d[i + 17] != 0xFF) continue;
            try { Parser p(d.data(), d.size(), i + 16, true); for (auto& o : p.objects) roots.push_back(std::move(o)); } catch (const std::exception&) {}
        }
    } else {
        Parser p(d.data(), d.size(), 0, false); roots = std::move(p.objects);
    }
}

struct Inst { std::string path; float m[9]; float t[3]; };
static void QuatToMat(const float* q, float* r) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}
static void Collect(const Node& o, const float* pm, const float* pt, std::vector<Inst>& out, int depth = 0) {
    float m[9], t[3]; memcpy(m, pm, sizeof m); memcpy(t, pt, sizeof t);
    if (o.hasXf) {
        float r[9]; QuatToMat(o.xf + 3, r); float local[9];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) local[i * 3 + j] = r[i * 3 + j] * o.xf[j];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float s = 0; for (int k = 0; k < 3; k++) s += pm[i * 3 + k] * local[k * 3 + j]; m[i * 3 + j] = s; }
        for (int i = 0; i < 3; i++) t[i] = pm[i * 3] * o.xf[7] + pm[i * 3 + 1] * o.xf[8] + pm[i * 3 + 2] * o.xf[9] + pt[i];
    }
    if (EndsWith(o.path, ".pami") || EndsWith(o.path, ".pam")) { Inst in; in.path = o.path; memcpy(in.m, m, sizeof m); memcpy(in.t, t, sizeof t); out.push_back(std::move(in)); }
    if (depth < 200) for (const auto& k : o.kids) Collect(k, m, t, out, depth + 1);
}

// ---------------------------------------------------------------- .pam static mesh (CDMW mesh_parser.parse_pam)
struct Mesh { std::vector<float> v; std::vector<uint32_t> f; bool compressed = false;   // compressed: geometry block was LZ4 inside the file
              std::vector<float> uv; std::vector<uint8_t> fm; std::vector<std::string> mats; };   // uv per vertex (0..1), fm = submesh per face, mats = material name per submesh
static const uint32_t kStrides[] = { 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 36, 40, 44, 48, 52, 56, 60, 64 };
static const uint32_t kScanStrides[] = { 6, 8, 10, 12, 14, 16, 20, 24, 28, 32 };
struct RawEntry { uint32_t i, nv, ni, ve, ie; };
struct PamCtx { const uint8_t* d; size_t n; uint32_t geomOff; float bmin[3], bmax[3]; bool invalidOffsets = false; };
static inline float DequantU16(uint16_t v, float mn, float mx) { return mn + (v / 65535.0f) * (mx - mn); }
static inline float DequantI16(int16_t v, float mn, float mx) { return mn + ((v + 32768) / 65536.0f) * (mx - mn); }

// indices at idxOff (u16), unique vertices gathered from vertBase + gi*stride (xyz u16), faces remapped
static void AppendIndexed(PamCtx& c, size_t idxOff, uint32_t ni, int64_t vertBase, uint32_t stride, Mesh& out, uint8_t mat = 0) {
    if (ni == 0 || idxOff + (size_t)ni * 2 > c.n) return;
    std::vector<uint16_t> idx(ni); memcpy(idx.data(), c.d + idxOff, (size_t)ni * 2);
    std::vector<uint16_t> uniq(idx); std::sort(uniq.begin(), uniq.end()); uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    std::vector<int32_t> remap(65536, -1);
    uint32_t base = (uint32_t)(out.v.size() / 3), cnt = 0;
    for (uint16_t gi : uniq) {
        int64_t off = vertBase + (int64_t)gi * stride;
        if (off < 0 || (size_t)off + 6 > c.n) break;
        if (off < (int64_t)c.geomOff) c.invalidOffsets = true;
        const uint8_t* p = c.d + off;
        out.v.push_back(DequantU16(rd16(p), c.bmin[0], c.bmax[0])); out.v.push_back(DequantU16(rd16(p + 2), c.bmin[1], c.bmax[1])); out.v.push_back(DequantU16(rd16(p + 4), c.bmin[2], c.bmax[2]));
        // vertex layout (stride 20): pos u16x3, normal i8x4, uv u16x2 (0..1), 2 bytes, color rgba8
        if (stride >= 14 && (size_t)off + 14 <= c.n) { out.uv.push_back(rd16(p + 10) / 65535.0f); out.uv.push_back(rd16(p + 12) / 65535.0f); } else { out.uv.push_back(0); out.uv.push_back(0); }
        remap[gi] = (int32_t)(base + cnt++);
    }
    for (uint32_t j = 0; j + 2 < ni; j += 3) {
        int32_t a = remap[idx[j]], b = remap[idx[j + 1]], cc = remap[idx[j + 2]];
        if (a < 0 || b < 0 || cc < 0) continue;
        out.f.push_back(a); out.f.push_back(b); out.f.push_back(cc); out.fm.push_back(mat);
    }
}
static bool IndicesBelow(const PamCtx& c, size_t off, uint32_t count, uint32_t limit) {
    if (off + (size_t)count * 2 > c.n) return false;
    for (uint32_t j = 0; j < count; j++) if (rd16(c.d + off + (size_t)j * 2) >= limit) return false;
    return true;
}
static void ParseIndependent(PamCtx& c, const std::vector<RawEntry>& entries, Mesh& out) {
    const int64_t idxAvail = ((int64_t)c.n - 0x19840) / 2;
    for (const auto& e : entries) {
        bool local = false;
        for (uint32_t s : kStrides) {   // per-mesh layout: indices follow the vertex block
            size_t vs = (size_t)c.geomOff + e.ve, io = vs + (size_t)e.nv * s;
            if (io + (size_t)e.ni * 2 > c.n) continue;
            if (IndicesBelow(c, io, e.ni, e.nv)) { AppendIndexed(c, io, e.ni, (int64_t)vs, s, out, (uint8_t)e.i); local = true; break; }
        }
        if (local) continue;
        if ((int64_t)e.ie + e.ni <= idxAvail) {   // global layout: i16 positions, index table at 0x19840
            size_t io = 0x19840 + (size_t)e.ie * 2;
            if (io + (size_t)e.ni * 2 > c.n) continue;
            std::vector<uint16_t> idx(e.ni); memcpy(idx.data(), c.d + io, (size_t)e.ni * 2);
            std::vector<uint16_t> uniq(idx); std::sort(uniq.begin(), uniq.end()); uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
            std::vector<int32_t> remap(65536, -1); uint32_t base = (uint32_t)(out.v.size() / 3), cnt = 0;
            for (uint16_t gi : uniq) {
                int64_t off = (int64_t)c.geomOff + ((int64_t)gi - 3068) * 6;
                if (off < 0) { c.invalidOffsets = true; continue; }
                if ((size_t)off + 6 > c.n) break;
                const uint8_t* p = c.d + off;
                out.v.push_back(DequantI16(rdi16(p), c.bmin[0], c.bmax[0])); out.v.push_back(DequantI16(rdi16(p + 2), c.bmin[1], c.bmax[1])); out.v.push_back(DequantI16(rdi16(p + 4), c.bmin[2], c.bmax[2]));
                out.uv.push_back(0); out.uv.push_back(0);
                remap[gi] = (int32_t)(base + cnt++);
            }
            for (uint32_t j = 0; j + 2 < e.ni; j += 3) { int32_t a = remap[idx[j]], b = remap[idx[j + 1]], cc = remap[idx[j + 2]]; if (a < 0 || b < 0 || cc < 0) continue; out.f.push_back(a); out.f.push_back(b); out.f.push_back(cc); out.fm.push_back((uint8_t)e.i); }
        }
    }
}
static void ParseCombined(PamCtx& c, const std::vector<RawEntry>& entries, Mesh& out) {
    uint64_t totalV = 0, totalI = 0; for (auto& e : entries) { totalV += e.nv; totalI += e.ni; }
    if (!totalV) return;
    double avail = (double)c.n - c.geomOff, target = (avail - (double)totalI * 2) / (double)totalV;
    uint32_t stride = kStrides[0]; double best = 1e30;
    for (uint32_t s : kStrides) { double dd = fabs(s - target); if (dd < best) { best = dd; stride = s; } }
    {   // verify the guess with the index values (files may carry extra blocks after the indices, which pulls the size estimate off)
        auto sane = [&](uint32_t st) { if ((uint64_t)c.geomOff + totalV * st + totalI * 2 > c.n) return false; size_t ib = (size_t)c.geomOff + (size_t)(totalV * st);
            for (auto& e : entries) if (!IndicesBelow(c, ib + (size_t)e.ie * 2, std::min<uint32_t>(e.ni, 300), e.nv)) return false; return true; };
        if (!sane(stride)) { for (uint32_t st : kStrides) if (sane(st)) { stride = st; break; } }
    }
    if ((uint64_t)c.geomOff + totalV * stride + totalI * 2 > c.n) return;
    size_t idxBase = (size_t)c.geomOff + (size_t)(totalV * stride);
    for (auto& e : entries) AppendIndexed(c, idxBase + (size_t)e.ie * 2, e.ni, (int64_t)c.geomOff + (int64_t)e.ve * stride, stride, out, (uint8_t)e.i);
}
static void ParseScanFallback(PamCtx& c, const std::vector<RawEntry>& entries, Mesh& out) {
    uint64_t totalV = 0, totalI = 0; for (auto& e : entries) { totalV += e.nv; totalI += e.ni; }
    if (totalV < 3 || totalI < 3 || totalV > 65535 || c.n < 100) return;
    const size_t g = c.geomOff;
    if (g >= c.n) return;
    size_t searchLimit = std::min(c.n - 100, g + std::min<size_t>(c.n / 2, 2000000));
    size_t step = (searchLimit > g && searchLimit - g < 500000) ? 2 : 4;
    for (size_t scan = g; scan < searchLimit; scan += step) {
        if (scan + 60 > c.n) break;
        uint16_t mn = 0xFFFF, mx = 0; for (int j = 0; j < 30; j++) { uint16_t v = rd16(c.d + scan + j * 2); mn = std::min(mn, v); mx = std::max(mx, v); }
        if (mx - mn < 5000) continue;
        for (uint32_t ts : kScanStrides) {
            size_t tio = scan + (size_t)(totalV * ts);
            if (tio + (size_t)(totalI * 2) > c.n) continue;
            if (!IndicesBelow(c, tio, (uint32_t)std::min<uint64_t>(500, totalI), (uint32_t)totalV)) continue;
            for (auto& e : entries) AppendIndexed(c, tio + (size_t)e.ie * 2, e.ni, (int64_t)scan + (int64_t)e.ve * ts, ts, out, (uint8_t)e.i);
            return;
        }
    }
    // backward: locate the index block from the end of the file
    size_t lower = g + (size_t)(totalV * 6);
    if (c.n < 2 || lower >= c.n - 2) return;
    for (size_t scanEnd = c.n - 2; scanEnd > lower; scanEnd -= 2) {
        if (scanEnd + 2 < (size_t)(totalI * 2)) break;
        size_t testStart = scanEnd - (size_t)(totalI * 2) + 2;
        if (testStart < g) break;
        if (rd16(c.d + testStart) >= totalV) continue;
        if (!IndicesBelow(c, testStart, (uint32_t)std::min<uint64_t>(300, totalI), (uint32_t)totalV)) continue;
        if (!IndicesBelow(c, testStart, (uint32_t)totalI, (uint32_t)totalV)) continue;
        uint32_t best = 0;
        for (uint32_t ts : kScanStrides) { size_t expectedEnd = g + (size_t)(totalV * ts); if (expectedEnd <= testStart && testStart - expectedEnd < 16384) { best = ts; break; } }
        if (!best) { best = (uint32_t)((testStart - g) / totalV); if (best < 6) best = 6; }
        for (auto& e : entries) AppendIndexed(c, testStart + (size_t)e.ie * 2, e.ni, (int64_t)g + (int64_t)e.ve * best, best, out, (uint8_t)e.i);
        return;
    }
}
// LZ4 block decoder (written from the public block format description: token = literal length | match length, little-endian
// 2-byte match offset, 4-byte minimum match). Returns false on malformed input.
static bool Lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear(); out.reserve(expect); size_t i = 0;
    while (i < n) {
        const uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; lit += b; } while (b == 255); }
        if (i + lit > n) return false;
        out.insert(out.end(), src + i, src + i + lit); i += lit;
        if (i >= n) break;                                   // the last sequence has literals only
        if (i + 2 > n) return false;
        const size_t off = src[i] | (src[i + 1] << 8); i += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off == 0 || off > out.size()) return false;
        const size_t start = out.size() - off;
        for (size_t k = 0; k < ml; k++) out.push_back(out[start + k]);   // byte-wise: overlapping matches are the normal case
        if (out.size() > expect + 64) return false;
    }
    return out.size() == expect;
}
bool Lz4Decode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expect) { return Lz4Block(src, n, out, expect); }
static bool ParsePam(const std::vector<uint8_t>& dataIn, Mesh& out) {
    if (dataIn.size() < 0x48 || memcmp(dataIn.data(), "PAR ", 4) != 0) return false;
    // some meshes keep the geometry block LZ4-compressed inside the file: dword 0x40 = uncompressed size, dword 0x44 = compressed
    // size (0 = stored). The file is padded to the uncompressed size, which is why the old code read garbage from them.
    std::vector<uint8_t> unpacked; const std::vector<uint8_t>* dp = &dataIn;
    {
        const uint32_t geomOff = rd32(dataIn.data() + 0x3C), usz = rd32(dataIn.data() + 0x40), csz = rd32(dataIn.data() + 0x44);
        if (csz != 0 && geomOff < dataIn.size() && (size_t)geomOff + csz <= dataIn.size() && usz < (256u << 20)) {
            std::vector<uint8_t> geo;
            if (Lz4Block(dataIn.data() + geomOff, csz, geo, usz)) { out.compressed = true;
                unpacked.assign(dataIn.begin(), dataIn.begin() + geomOff); unpacked.insert(unpacked.end(), geo.begin(), geo.end()); unpacked.resize(unpacked.size() + 12, 0);
                dp = &unpacked;
            } else return false;
        }
    }
    const std::vector<uint8_t>& data = *dp;
    PamCtx c; c.d = data.data(); c.n = data.size();
    for (int i = 0; i < 3; i++) { c.bmin[i] = rdf(c.d + 0x14 + 4 * i); c.bmax[i] = rdf(c.d + 0x20 + 4 * i); }
    c.geomOff = rd32(c.d + 0x3C); uint32_t meshCount = rd32(c.d + 0x10);
    std::vector<RawEntry> entries;
    for (uint32_t i = 0; i < meshCount; i++) {
        size_t off = 0x410 + (size_t)i * 0x218; if (off + 0x218 > c.n) break;
        entries.push_back({ i, rd32(c.d + off), rd32(c.d + off + 4), rd32(c.d + off + 8), rd32(c.d + off + 12) });
        const char* nm = (const char*)c.d + off + 0x110; out.mats.push_back(std::string(nm, strnlen(nm, 0x40)));   // material primitive name (matches the .pami)
    }
    bool combined = false;
    if (meshCount > 1 && !entries.empty()) {
        combined = true; uint64_t ve = 0, ie = 0;
        for (auto& e : entries) { if (e.ve != ve || e.ie != ie) { combined = false; break; } ve += e.nv; ie += e.ni; }
    }
    if (combined) ParseCombined(c, entries, out); else ParseIndependent(c, entries, out);
    if (meshCount > 0 && (out.v.empty() || c.invalidOffsets)) { out.v.clear(); out.f.clear(); out.uv.clear(); out.fm.clear(); c.invalidOffsets = false; ParseScanFallback(c, entries, out); }
    return !out.v.empty() && !out.f.empty();
}

// ---------------------------------------------------------------- materials (.pami XML) and textures (DDS)
struct MatInfo { std::string tex; float rc[3] = { 0.72f, 0.66f, 0.56f }; float uvScale = 1.0f; bool decal = false; };   // decal: MeshDecal shader, drawn on top of other geometry
static std::unordered_map<std::string, std::unordered_map<std::string, MatInfo>> g_pamiMats;
static std::string XmlAttr(const std::string& s, size_t from, size_t to, const char* key) {   // value of key="..." inside [from,to)
    size_t p = s.find(key, from); if (p == std::string::npos || p >= to) return "";
    p += strlen(key); size_t q = s.find('"', p); if (q == std::string::npos || q > to) return "";
    return s.substr(p, q - p);
}
// <Material PrimitiveName="x"> ... <RepresentColor x= y= z=/> ... <MaterialParameterTexture Name="_baseColorTexture" Value="..."/> ... <MaterialParameterFloat Name="_uvScale" Value=.../>
static const std::unordered_map<std::string, MatInfo>* LoadMaterials(const std::string& pamiPath) {
    if (!EndsWith(pamiPath, ".pami")) return nullptr;
    auto it = g_pamiMats.find(pamiPath); if (it != g_pamiMats.end()) return &it->second;
    std::unordered_map<std::string, MatInfo> mats; std::vector<uint8_t> x;
    if (GetFile(pamiPath, x)) {
        std::string s((const char*)x.data(), x.size()); size_t p = 0;
        static const char kTag[] = "<Material PrimitiveName=\""; const size_t tagLen = sizeof(kTag) - 1;
        while ((p = s.find(kTag, p)) != std::string::npos) {
            size_t q = s.find('"', p + tagLen); if (q == std::string::npos) break;
            std::string name = s.substr(p + tagLen, q - (p + tagLen)); size_t end = s.find("</Material>", q); if (end == std::string::npos) end = s.size();
            MatInfo m;
            size_t rc = s.find("<RepresentColor", q);
            if (rc != std::string::npos && rc < end) { std::string xs = XmlAttr(s, rc, end, " x=\""), ys = XmlAttr(s, rc, end, " y=\""), zs = XmlAttr(s, rc, end, " z=\""); if (!xs.empty() && !ys.empty() && !zs.empty()) { m.rc[0] = (float)atof(xs.c_str()); m.rc[1] = (float)atof(ys.c_str()); m.rc[2] = (float)atof(zs.c_str()); } }
            size_t bt = s.find("Name=\"_baseColorTexture\"", q); if (bt != std::string::npos && bt < end) m.tex = XmlAttr(s, bt, end, "Value=\"");
            size_t mn = s.find("MaterialName=\"", q); if (mn != std::string::npos && mn < end) { std::string shader = XmlAttr(s, mn, end, "MaterialName=\""); if (shader.find("Decal") != std::string::npos) m.decal = true; }
            size_t us = s.find("Name=\"_uvScale\"", q); if (us != std::string::npos && us < end) { std::string v = XmlAttr(s, us, end, "Value=\""); if (!v.empty()) m.uvScale = (float)atof(v.c_str()); if (!(m.uvScale > 0.01f && m.uvScale < 100.0f)) m.uvScale = 1.0f; }
            mats[name] = m; p = end;
        }
    }
    if (g_pamiMats.size() > 4000) g_pamiMats.clear();
    return &(g_pamiMats[pamiPath] = mats);
}
struct TexImg { int w = 0, h = 0; bool alpha = false; std::vector<uint8_t> rgba; };
static std::unordered_map<std::string, std::shared_ptr<TexImg>> g_texs; static size_t g_texBytes = 0;
static inline void Rgb565(uint16_t c, int* o) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; }
// BC1 (DXT1) / BC3 (DXT5) block decoder for one mip level
static void DecodeBc(const uint8_t* src, size_t n, int w, int h, bool bc3, TexImg& out) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = bc3 ? 16 : 8;
    out.w = w; out.h = h; out.alpha = bc3; out.rgba.assign((size_t)w * h * 4, 255);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const size_t bo = ((size_t)by * bw + bx) * bs; if (bo + bs > n) return;
        const uint8_t* b = src + bo; uint8_t a[16]; for (int i = 0; i < 16; i++) a[i] = 255;
        if (bc3) {   // alpha block: two endpoints + 16 3-bit indices
            const int a0 = b[0], a1 = b[1]; int pal[8] = { a0, a1 };
            if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) a[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
            b += 8;
        }
        const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
        int p0[3], p1[3], pal[4][3]; Rgb565(c0, p0); Rgb565(c1, p1);
        for (int k = 0; k < 3; k++) { pal[0][k] = p0[k]; pal[1][k] = p1[k]; if (c0 > c1 || bc3) { pal[2][k] = (2 * p0[k] + p1[k]) / 3; pal[3][k] = (p0[k] + 2 * p1[k]) / 3; } else { pal[2][k] = (p0[k] + p1[k]) / 2; pal[3][k] = 0; } }
        for (int r = 0; r < 4; r++) for (int cI = 0; cI < 4; cI++) {
            const int x = bx * 4 + cI, y = by * 4 + r; if (x >= w || y >= h) continue;
            const int sel = (b[4 + r] >> (2 * cI)) & 3; uint8_t* o = &out.rgba[((size_t)y * w + x) * 4];
            o[0] = (uint8_t)pal[sel][0]; o[1] = (uint8_t)pal[sel][1]; o[2] = (uint8_t)pal[sel][2]; o[3] = (!bc3 && c0 <= c1 && sel == 3) ? 0 : a[r * 4 + cI];
        }
    }
}
// DDS: the mip level with at most 256 px on the long side is decoded (BC1 and BC3 only; everything else falls back to the material colour)
static std::shared_ptr<TexImg> LoadTexture(const std::string& path) {
    auto it = g_texs.find(path); if (it != g_texs.end()) return it->second;
    if (g_texBytes > (192u << 20)) { g_texs.clear(); g_texBytes = 0; }
    std::shared_ptr<TexImg> t; std::vector<uint8_t> d;
    if (GetFile(path, d) && d.size() >= 128 && memcmp(d.data(), "DDS ", 4) == 0) {
        int h = (int)rd32(d.data() + 12), w = (int)rd32(d.data() + 16), mips = std::max(1, (int)rd32(d.data() + 28));
        const uint32_t fourcc = rd32(d.data() + 84); size_t off = 128; bool bc3 = false, ok = true;
        if (fourcc == 0x30315844) {   // "DX10": dxgi format follows
            const uint32_t dxgi = rd32(d.data() + 128); off = 148;
            if (dxgi == 71 || dxgi == 72) bc3 = false; else if (dxgi == 77 || dxgi == 78) bc3 = true; else ok = false;
        } else if (fourcc == 0x31545844) bc3 = false; else if (fourcc == 0x35545844) bc3 = true; else ok = false;   // DXT1 / DXT5
        if (ok && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            while ((w > 256 || h > 256) && mips > 1) { off += (size_t)((w + 3) / 4) * ((h + 3) / 4) * (bc3 ? 16 : 8); w = std::max(1, w / 2); h = std::max(1, h / 2); mips--; }
            if (off < d.size()) { t = std::make_shared<TexImg>(); DecodeBc(d.data() + off, d.size() - off, w, h, bc3, *t); g_texBytes += t->rgba.size(); }
        }
    }
    static int s_logged = 0; if (s_logged < 6) { s_logged++; Log("[thumbs] texture %s: %s%s", path.c_str(), t ? "ok" : "FAILED", t ? (std::string(" ") + std::to_string(t->w) + "x" + std::to_string(t->h) + (t->alpha ? " bc3" : " bc1")).c_str() : (d.empty() ? " (read failed)" : " (format)")); }
    g_texs[path] = t;
    return t;
}
struct Surface { std::shared_ptr<TexImg> tex; float col[3]; float uvScale; bool skip = false; };

// ---------------------------------------------------------------- mesh cache
static std::unordered_map<std::string, std::shared_ptr<Mesh>> g_meshes;
static std::unordered_map<std::string, std::string> g_pami;
static size_t g_meshBytes = 0;

static std::string ResolvePam(const std::string& path) {
    if (EndsWith(path, ".pam")) return path;
    auto it = g_pami.find(path); if (it != g_pami.end()) return it->second;
    size_t dot = path.rfind('.'); std::string res = (dot == std::string::npos ? path : path.substr(0, dot)) + ".pam";
    std::vector<uint8_t> x;
    if (GetFile(path, x)) {   // .pami is XML: <StaticMesh Path="....pam"/>
        static const char kTag[] = "<StaticMesh Path=\"";
        std::string s((const char*)x.data(), x.size());
        size_t p = s.find(kTag);
        if (p != std::string::npos) { p += sizeof(kTag) - 1; size_t q = s.find('"', p); if (q != std::string::npos) { std::string v = s.substr(p, q - p); if (EndsWith(v, ".pam")) res = v; } }
    }
    g_pami[path] = res;
    return res;
}
static std::shared_ptr<Mesh> LoadMesh(const std::string& path) {
    std::string pam = ResolvePam(path);
    auto it = g_meshes.find(pam); if (it != g_meshes.end()) return it->second;
    if (g_meshBytes > (400u << 20)) { g_meshes.clear(); g_meshBytes = 0; }
    std::shared_ptr<Mesh> m;
    std::vector<uint8_t> data;
    if (GetFile(pam, data)) { auto mm = std::make_shared<Mesh>(); if (ParsePam(data, *mm)) { m = mm; g_meshBytes += mm->v.size() * 4 + mm->f.size() * 4; } }
    g_meshes[pam] = m;
    return m;
}

// ---------------------------------------------------------------- software renderer
static void FillTri(uint8_t* buf, int W, const float* x, const float* y, uint8_t r, uint8_t g, uint8_t b) {
    float ymin = std::min({ y[0], y[1], y[2] }), ymax = std::max({ y[0], y[1], y[2] });
    int y0 = std::max(0, (int)ceilf(ymin - 0.5f)), y1 = std::min(W - 1, (int)floorf(ymax - 0.5f));
    for (int py = y0; py <= y1; py++) {
        float yc = py + 0.5f, xl = 1e30f, xr = -1e30f;
        for (int e = 0; e < 3; e++) {
            int a = e, bb = (e + 1) % 3;
            if ((y[a] <= yc) == (y[bb] <= yc)) continue;
            float xi = x[a] + (yc - y[a]) * (x[bb] - x[a]) / (y[bb] - y[a]);
            xl = std::min(xl, xi); xr = std::max(xr, xi);
        }
        if (xl > xr) continue;
        int px0 = std::max(0, (int)ceilf(xl - 0.5f)), px1 = std::min(W - 1, (int)floorf(xr - 0.5f));
        uint8_t* row = buf + ((size_t)py * W + px0) * 4;
        for (int px = px0; px <= px1; px++, row += 4) { row[0] = r; row[1] = g; row[2] = b; row[3] = 255; }
    }
}
static bool RenderPng(const Mesh& mesh, const std::vector<uint16_t>& fs, const std::vector<Surface>& surf, const std::string& file) {
    const int S = 256, SS = 2, W = S * SS;
    const size_t nv = mesh.v.size() / 3; if (!nv || mesh.f.size() < 3) return false;
    const bool haveUv = mesh.uv.size() == nv * 2;
    // camera: azimuth 35 deg around Y, then elevation 25 deg around X (matches scripts/render_thumbs.py)
    const float az = 35.0f * 3.14159265f / 180.0f, el = 25.0f * 3.14159265f / 180.0f;
    const float ca = cosf(az), sa = sinf(az), ce = cosf(el), se = sinf(el);
    std::vector<float> p(nv * 3);
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < nv; i++) {
        float x = mesh.v[i * 3], y = mesh.v[i * 3 + 1], z = mesh.v[i * 3 + 2];
        float x1 = ca * x + sa * z, y1 = y, z1 = -sa * x + ca * z;
        float x2 = x1, y2 = ce * y1 - se * z1, z2 = se * y1 + ce * z1;
        p[i * 3] = x2; p[i * 3 + 1] = y2; p[i * 3 + 2] = z2;
        for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], p[i * 3 + k]); mx[k] = std::max(mx[k], p[i * 3 + k]); }
    }
    float ext = std::max(mx[0] - mn[0], mx[1] - mn[1]); if (!(ext > 0)) ext = 1.0f;
    const float scale = (W * 0.86f) / ext, cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f;
    const size_t nf = mesh.f.size() / 3; size_t stepF = nf > 600000 ? nf / 600000 + 1 : 1;   // z-buffer cost is per pixel, so big meshes are drawn completely (the old painter cap of 40k left holes)
    struct Tri { float depth; uint32_t i; float shade; };
    std::vector<Tri> tris; tris.reserve(std::min<size_t>(nf, 600000));
    float light[3] = { 0.35f, 0.8f, 0.55f }; { float l = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]); for (float& v : light) v /= l; }
    for (size_t t = 0; t < nf; t += stepF) {
        uint32_t a = mesh.f[t * 3], b = mesh.f[t * 3 + 1], c = mesh.f[t * 3 + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        const float* A = &p[a * 3]; const float* B = &p[b * 3]; const float* C = &p[c * 3];
        float e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (ln <= 1e-9f) continue;
        float shade = fabsf((n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / ln) * 0.75f + 0.25f;
        tris.push_back({ (A[2] + B[2] + C[2]) / 3.0f, (uint32_t)t, shade });
    }
    if (tris.empty()) return false;
    std::vector<uint8_t> buf((size_t)W * W * 4, 0); std::vector<float> zbuf((size_t)W * W, -1e30f);
    for (const Tri& tr : tris) {   // z-buffered rasterizer with barycentric uv interpolation (orthographic view, so no perspective correction)
        float xs[3], ys[3], zs[3], us[3] = { 0, 0, 0 }, vs[3] = { 0, 0, 0 };
        const Surface* sf = (tr.i < fs.size() && fs[tr.i] < surf.size()) ? &surf[fs[tr.i]] : nullptr;
        const TexImg* tex = sf && sf->tex && sf->tex->w > 0 ? sf->tex.get() : nullptr;
        for (int k = 0; k < 3; k++) { const uint32_t vi = mesh.f[tr.i * 3 + k]; const float* P = &p[vi * 3]; xs[k] = (P[0] - cx) * scale + W / 2.0f; ys[k] = W / 2.0f - (P[1] - cy) * scale; zs[k] = P[2];
            if (tex && haveUv) { us[k] = mesh.uv[vi * 2] * sf->uvScale; vs[k] = mesh.uv[vi * 2 + 1] * sf->uvScale; } }
        const float det = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]); if (fabsf(det) < 1e-6f) continue;
        const int x0 = std::max(0, (int)floorf(std::min({ xs[0], xs[1], xs[2] }))), x1 = std::min(W - 1, (int)ceilf(std::max({ xs[0], xs[1], xs[2] })));
        const int y0 = std::max(0, (int)floorf(std::min({ ys[0], ys[1], ys[2] }))), y1 = std::min(W - 1, (int)ceilf(std::max({ ys[0], ys[1], ys[2] })));
        float base[3] = { 214, 196, 168 }; if (sf) { base[0] = sf->col[0] * 255; base[1] = sf->col[1] * 255; base[2] = sf->col[2] * 255; }
        for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            const float l1 = ((fx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (fy - ys[0])) / det;
            const float l2 = ((xs[1] - xs[0]) * (fy - ys[0]) - (fx - xs[0]) * (ys[1] - ys[0])) / det;
            const float l0 = 1.0f - l1 - l2; if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float z = l0 * zs[0] + l1 * zs[1] + l2 * zs[2]; float& zb = zbuf[(size_t)py * W + px]; if (z <= zb) continue;
            float c[3] = { base[0], base[1], base[2] };
            if (tex) {
                float u = l0 * us[0] + l1 * us[1] + l2 * us[2], v = l0 * vs[0] + l1 * vs[1] + l2 * vs[2];
                u -= floorf(u); v -= floorf(v);
                int tx = std::min(tex->w - 1, (int)(u * tex->w)), ty = std::min(tex->h - 1, (int)(v * tex->h));
                const uint8_t* q = &tex->rgba[((size_t)ty * tex->w + tx) * 4];
                if (tex->alpha && q[3] < 128) continue;   // cut-out (leaves, grates)
                c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
            }
            zb = z; uint8_t* o = &buf[((size_t)py * W + px) * 4];
            o[0] = (uint8_t)std::min(255.0f, c[0] * tr.shade); o[1] = (uint8_t)std::min(255.0f, c[1] * tr.shade); o[2] = (uint8_t)std::min(255.0f, c[2] * tr.shade); o[3] = 255;
        }
    }
    // 2x2 box filter, alpha weighted
    std::vector<uint8_t> out((size_t)S * S * 4, 0);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        unsigned r = 0, g = 0, b = 0, a = 0;
        for (int dy = 0; dy < SS; dy++) for (int dx = 0; dx < SS; dx++) { const uint8_t* q = &buf[(((size_t)y * SS + dy) * W + (size_t)x * SS + dx) * 4]; r += q[0] * q[3]; g += q[1] * q[3]; b += q[2] * q[3]; a += q[3]; }
        uint8_t* o = &out[((size_t)y * S + x) * 4];
        if (a) { o[0] = (uint8_t)(r / a); o[1] = (uint8_t)(g / a); o[2] = (uint8_t)(b / a); o[3] = (uint8_t)(a / (SS * SS)); }
    }
    return stbi_write_png(file.c_str(), S, S, 4, out.data(), S * 4) != 0;
}

// ---------------------------------------------------------------- worker
static std::mutex g_mu;
static std::deque<std::string> g_requests;
static std::unordered_set<std::string> g_pending, g_processed;
static std::atomic<int> g_done{ 0 }, g_failed{ 0 }, g_total{ 0 }, g_gen{ 0 };
static std::atomic<bool> g_ready{ false }, g_background{ true }, g_idle{ false };
static std::string g_error;
static FILE* g_sizes = nullptr;

static bool g_measureOnly = false;   // skip the render when the image already exists (re-measuring old cache lines)
static int g_statInst = 0, g_statSurf = 0, g_statMat = 0, g_statTex = 0;   // last render: instances, surfaces, surfaces with a material, with a texture
static bool g_recheckOnly = false, g_recheckForce = false, g_lastSkipped = false;   // re-render pass: force = every prefab (textured previews), else only LZ4 meshes
static std::vector<std::string> g_refreshed;               // images overwritten by the re-render pass (the overlay drops its cached textures)
static bool g_passActive = false; static std::unordered_set<std::string> g_passDone;   // re-render pass: browser requests jump the queue
static bool Generate(const std::string& logical, float dims[6], std::string& why) {
    std::string phys = logical; if (!phys.empty() && phys[0] == '/') phys.erase(0, 1);
    size_t sl = phys.find('/');
    std::string physBin = sl == std::string::npos ? phys : phys.substr(0, sl) + "/bin__/" + phys.substr(sl + 1);
    std::vector<uint8_t> data;
    if (!GetFile(physBin, data) && !GetFile(phys, data)) { why = "prefab missing"; return false; }
    std::vector<Node> roots;
    try { ParsePrefab(data, roots); } catch (const std::exception& e) { why = std::string("prefab: ") + e.what(); return false; }
    std::vector<Inst> inst; const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, Z[3] = { 0, 0, 0 };
    for (const auto& r : roots) Collect(r, I, Z, inst);
    if (inst.empty()) { why = "no meshes"; return false; }
    Mesh all; bool anyCompressed = false; std::vector<uint16_t> fs; std::vector<Surface> surf; g_statInst = (int)inst.size(); g_statSurf = g_statMat = g_statTex = 0;
    for (size_t k = 0; k < inst.size() && k < 400; k++) {
        auto m = LoadMesh(inst[k].path); if (!m) continue; if (m->compressed) anyCompressed = true;
        const Inst& in = inst[k]; uint32_t base = (uint32_t)(all.v.size() / 3);
        const bool mUv = m->uv.size() == (m->v.size() / 3) * 2;
        for (size_t i = 0; i + 2 < m->v.size(); i += 3) {
            float x = m->v[i], y = m->v[i + 1], z = m->v[i + 2];
            all.v.push_back(in.m[0] * x + in.m[1] * y + in.m[2] * z + in.t[0]);
            all.v.push_back(in.m[3] * x + in.m[4] * y + in.m[5] * z + in.t[1]);
            all.v.push_back(in.m[6] * x + in.m[7] * y + in.m[8] * z + in.t[2]);
            if (mUv) { all.uv.push_back(m->uv[(i / 3) * 2]); all.uv.push_back(m->uv[(i / 3) * 2 + 1]); } else { all.uv.push_back(0); all.uv.push_back(0); }
        }
        // one surface per submesh of this instance: texture + fallback colour from the .pami material of the same name
        const auto* mats = LoadMaterials(in.path); std::vector<uint16_t> subSurf(m->mats.size() + 1, 0xFFFF);
        auto surfaceFor = [&](uint8_t sub) -> uint16_t {
            const size_t si = std::min<size_t>(sub, m->mats.size()); if (subSurf[si] != 0xFFFF) return subSurf[si];
            Surface sfc; sfc.col[0] = 0.72f; sfc.col[1] = 0.66f; sfc.col[2] = 0.56f; sfc.uvScale = 1.0f;
            if (mats && si < m->mats.size()) { auto mi = mats->find(m->mats[si]); if (mi != mats->end()) { g_statMat++; memcpy(sfc.col, mi->second.rc, sizeof sfc.col); sfc.uvScale = mi->second.uvScale; sfc.skip = mi->second.decal; if (!sfc.skip && !mi->second.tex.empty()) { sfc.tex = LoadTexture(mi->second.tex); if (sfc.tex) g_statTex++; } } }
            g_statSurf++;
            if (surf.size() >= 0xFFFE) return 0; surf.push_back(sfc); return subSurf[si] = (uint16_t)(surf.size() - 1);
        };
        const bool mFm = m->fm.size() == m->f.size() / 3;
        for (size_t t = 0; t < m->f.size() / 3; t++) { const uint16_t si = surfaceFor(mFm ? m->fm[t] : 0); if (si < surf.size() && surf[si].skip) continue;
            all.f.push_back(m->f[t * 3] + base); all.f.push_back(m->f[t * 3 + 1] + base); all.f.push_back(m->f[t * 3 + 2] + base); fs.push_back(si); }
    }
    if (all.v.empty() || all.f.empty()) { why = "no geometry"; return false; }
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < all.v.size(); i += 3) for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], all.v[i + k]); mx[k] = std::max(mx[k], all.v[i + k]); }
    for (int k = 0; k < 3; k++) { dims[k] = mx[k] - mn[k]; dims[3 + k] = (mn[k] + mx[k]) * 0.5f; }   // size + center relative to the pivot
    if (g_measureOnly && GetFileAttributesA(core::ThumbFile(logical).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (g_recheckOnly && !g_recheckForce && !anyCompressed && GetFileAttributesA(core::ThumbFile(logical).c_str()) != INVALID_FILE_ATTRIBUTES) { g_lastSkipped = true; return true; }
    if (!RenderPng(all, fs, surf, core::ThumbFile(logical))) { why = "render"; return false; }
    return true;
}
static bool GenerateGuarded(const std::string& logical, float dims[6], std::string& why) {
    CDK_GUARD_BEGIN return Generate(logical, dims, why);
    CDK_GUARD_FAIL why = "crash"; return false;
    CDK_GUARD_END
    return false;
}

static DWORD WINAPI Worker(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int i = 0; i < 1200 && !core::GameReadAvailable(); i++) Sleep(100);   // the loader instance is captured from the game's own loads
    if (!core::GameReadAvailable()) { g_error = "game resource loader not available"; Log("[thumbs] disabled: %s", g_error.c_str()); return 0; }
    CreateDirectoryA((core::ModDir() + "\\thumbs").c_str(), nullptr);
    const std::string sizesPath = core::ModDir() + "\\prefab_size.tsv";
    {   // everything already recorded (rendered or failed) is skipped
        std::ifstream f(sizesPath); std::string line; std::lock_guard<std::mutex> l(g_mu);
        std::set<std::string> failedSet;   // a prefab can appear more than once (re-measured lines are appended): count every path once
        while (std::getline(f, line)) {
            size_t t = line.find('\t'); if (t == std::string::npos) continue;
            std::string path = line.substr(0, t); g_processed.insert(path);
            if (line.compare(t + 1, 4, "0.00") == 0 && line.find("0.00\t0.00\t0.00") != std::string::npos) failedSet.insert(path); else failedSet.erase(path);
        }
        g_failed = (int)failedSet.size(); g_done = (int)g_processed.size() - g_failed;
    }
    g_sizes = fopen(sizesPath.c_str(), "a");
    const auto& idx = core::PrefabIndex(); g_total = (int)idx.size();
    Log("[thumbs] worker ready: %d done, %d failed, %d prefabs", g_done.load(), g_failed.load(), g_total.load());
    g_ready = true;
    {   // self test: one prefab and one mesh through the game's loader (logged once per start)
        const char* tests[] = { "object/bin__/00_common/dungeon/akapen/cd_akapen_dungeon_wall_01_broken_01_kwe.prefab", "object/00_common/ancient/cd_ancient_altarmarble_01.pam" };
        for (const char* t : tests) { std::vector<uint8_t> a; bool ok = GetFile(t, a);
            Log("[thumbs] selftest %s: %s, %zu bytes, magic %.4s", t, ok ? "ok" : "FAILED", a.size(), a.size() >= 4 ? (const char*)a.data() : "----"); }
    }
    size_t cursor = 0; DWORD lastLog = GetTickCount(); int sessionDone = 0;
    std::map<std::string, int> reasons;
    // version marker: 2 = LZ4 geometry supported. Older caches get one pass over every rendered prefab that re-renders only those
    // whose meshes were compressed (their old images were garbage); everything else is left alone.
    const std::string verPath = core::ModDir() + "\\thumbs_version.txt"; int cacheVer = 0;
    { FILE* vf = fopen(verPath.c_str(), "r"); if (vf) { if (fscanf(vf, "%d", &cacheVer) != 1) cacheVer = 0; fclose(vf); } }
    std::deque<std::string> recheck; int rechecked = 0, rerendered = 0;
    const int kCacheVersion = 3;   // 2 = LZ4 meshes decoded, 3 = textured previews
    // progress file: one prefab per line that this pass already rendered. Without it every session started the pass from
    // the beginning and re-rendered the same first few thousand prefabs, so the rest never got their textured image.
    const std::string passPath = core::ModDir() + "\\thumbs_pass.txt"; FILE* passOut = nullptr;
    if (cacheVer < kCacheVersion) { std::lock_guard<std::mutex> l(g_mu);
        { FILE* pf = fopen(passPath.c_str(), "r"); if (pf) { char line[1024]; while (fgets(line, sizeof line, pf)) { std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) g_passDone.insert(s); } fclose(pf); } }
        for (auto& pi : idx) if (g_processed.count(pi.path) && pi.sx > 0 && !g_passDone.count(pi.path)) recheck.push_back(pi.path);
        g_recheckForce = cacheVer < 3; g_passActive = !recheck.empty();
        if (!g_passDone.empty()) Log("[thumbs] re-render pass continues: %zu done in earlier sessions, %zu to go", g_passDone.size(), recheck.size());
        if (!recheck.empty()) passOut = fopen(passPath.c_str(), "a");
        if (!recheck.empty()) Log("[thumbs] cache version %d -> %d: %zu rendered prefabs are %s in the background", cacheVer, kCacheVersion, recheck.size(), g_recheckForce ? "rendered again with textures" : "checked for LZ4 meshes"); }
    auto writeVersion = [&]() { FILE* vf = fopen(verPath.c_str(), "w"); if (vf) { fprintf(vf, "%d\n", kCacheVersion); fclose(vf); } if (passOut) { fclose(passOut); passOut = nullptr; } DeleteFileA(passPath.c_str()); };
    if (recheck.empty()) writeVersion();
    std::deque<std::string> remeasure;   // rendered before the center columns existed: measure again without rendering
    for (auto& pi : idx) if (pi.sx > 0 && !pi.hasCenter) remeasure.push_back(pi.path);
    if (!remeasure.empty()) Log("[thumbs] %zu cached entries lack the bounding box center, measuring them again (no rendering)", remeasure.size());
    for (;;) {
        std::string path; bool prio = false; bool measure = false, check = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (!g_requests.empty()) { path = g_requests.front(); g_requests.pop_front(); prio = true; }
            else if (!remeasure.empty()) { path = remeasure.front(); remeasure.pop_front(); measure = true; }
            else if (!recheck.empty()) { path = recheck.front(); recheck.pop_front(); check = true; }
            else if (g_background) { while (cursor < idx.size() && g_processed.count(idx[cursor].path)) cursor++; if (cursor < idx.size()) path = idx[cursor++].path; }
            if (check && g_passDone.count(path)) continue;   // rendered earlier in this pass on request
            if (!measure && !check && !path.empty() && g_processed.count(path)) {
                if (prio && g_passActive && !g_passDone.count(path)) check = true;   // visible in the browser: render it now instead of later in the pass
                else { g_pending.erase(path); continue; }
            }
        }
        if (path.empty()) { g_idle = true; Sleep(250); continue; }
        g_idle = false;
        float dims[6] = { 0, 0, 0, 0, 0, 0 }; std::string why;
        g_measureOnly = measure; g_recheckOnly = check; g_lastSkipped = false;   // g_recheckForce stays as set at start
        bool ok = GenerateGuarded(path, dims, why);
        g_measureOnly = false; g_recheckOnly = false;
        if (check) {
            rechecked++;
            { std::lock_guard<std::mutex> l(g_mu); g_passDone.insert(path); g_pending.erase(path); }
            if (passOut) { fprintf(passOut, "%s\n", path.c_str()); if ((rechecked & 31) == 0) fflush(passOut); }
            if (rerendered < 5 && ok && !g_lastSkipped) Log("[thumbs] pass render %s: instances %d, surfaces %d, with material %d, with texture %d", path.c_str(), g_statInst, g_statSurf, g_statMat, g_statTex);
            if (ok && !g_lastSkipped) { rerendered++; g_gen++; { std::lock_guard<std::mutex> l(g_mu); g_refreshed.push_back(core::ThumbFile(path)); }
                if (g_sizes) { fprintf(g_sizes, "%s\t%.2f\t%.2f\t%.2f\t%.3f\t%.3f\t%.3f\n", path.c_str(), dims[0], dims[1], dims[2], dims[3], dims[4], dims[5]); fflush(g_sizes); } core::SetPrefabSize(path, dims); }
            if (recheck.empty()) { writeVersion(); g_recheckForce = false; g_passActive = false; Log("[thumbs] re-render pass done: %d prefabs checked, %d rendered again", rechecked, rerendered); }
            else if (GetTickCount() - lastLog > 60000) { Log("[thumbs] re-render pass: %d checked, %d rendered again, %zu to go", rechecked, rerendered, recheck.size()); lastLog = GetTickCount(); }
            if (!prio) Sleep(20);
            continue;
        }
        if (measure) { if (ok) { if (g_sizes) { fprintf(g_sizes, "%s\t%.2f\t%.2f\t%.2f\t%.3f\t%.3f\t%.3f\n", path.c_str(), dims[0], dims[1], dims[2], dims[3], dims[4], dims[5]); fflush(g_sizes); } core::SetPrefabSize(path, dims); }
                       else { int pi = -1; const auto& ix = core::PrefabIndex(); for (size_t i = 0; i < ix.size(); i++) if (ix[i].path == path) { pi = (int)i; break; }
                              if (pi >= 0 && g_sizes) { float d6[6] = { ix[pi].sx, ix[pi].sy, ix[pi].sz, 0, ix[pi].sy * 0.5f, 0 }; fprintf(g_sizes, "%s\t%.2f\t%.2f\t%.2f\t0\t%.3f\t0\n", path.c_str(), d6[0], d6[1], d6[2], d6[4]); fflush(g_sizes); core::SetPrefabSize(path, d6); } } Sleep(2); continue; }   // center unknown: pivot at the bottom center is the usual convention
        if (!ok) { for (float& d : dims) d = 0; reasons[why]++; }
        const bool record = ok || why != "prefab missing";
        if (record && g_sizes) { fprintf(g_sizes, "%s\t%.2f\t%.2f\t%.2f\t%.3f\t%.3f\t%.3f\n", path.c_str(), dims[0], dims[1], dims[2], dims[3], dims[4], dims[5]); fflush(g_sizes); }
        if (ok) core::SetPrefabSize(path, dims);
        { std::lock_guard<std::mutex> l(g_mu); g_processed.insert(path); g_pending.erase(path); }
        if (ok) { g_done++; g_gen++; sessionDone++; } else g_failed++;
        if (prio) Log("[thumbs] %s: %s (instances %d, surfaces %d, with material %d, with texture %d)", path.c_str(), ok ? "rendered" : why.c_str(), g_statInst, g_statSurf, g_statMat, g_statTex);
        if (GetTickCount() - lastLog > 60000) {
            std::string rs; for (auto& kv : reasons) rs += kv.first + "=" + std::to_string(kv.second) + " ";
            Log("[thumbs] progress %d/%d (failed %d, this session %d) reads %d %s", g_done.load(), g_total.load(), g_failed.load(), sessionDone, g_reads.load(), rs.c_str());
            lastLog = GetTickCount();
        }
        if (!prio) Sleep(20);   // background pass: roughly half duty cycle, the game keeps its cores
    }
}
void SetBackground(bool on) { g_background = on; }
bool Background() { return g_background; }

void Start() { CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr); }
void Refresh(const std::string& p) {   // render again (e.g. an old prefab_size.tsv line without the center columns)
    { std::lock_guard<std::mutex> l(g_mu); g_processed.erase(p); }
    Request(p);
}
void Request(const std::string& p) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_pending.count(p)) return;
    if (g_processed.count(p) && !(g_passActive && !g_passDone.count(p))) return;   // during a re-render pass a processed prefab may still be queued once
    g_pending.insert(p); g_requests.push_back(p);
}
bool Pending(const std::string& p) { std::lock_guard<std::mutex> l(g_mu); return g_pending.count(p) != 0; }
bool Processed(const std::string& p) { std::lock_guard<std::mutex> l(g_mu); return g_processed.count(p) != 0; }
bool Ready() { return g_ready; }
bool Idle() { return g_idle; }
std::vector<std::string> TakeRefreshed() { std::lock_guard<std::mutex> l(g_mu); std::vector<std::string> r; r.swap(g_refreshed); return r; }
int Done() { return g_done; }
int Failed() { return g_failed; }
int Total() { return g_total; }
int Generation() { return g_gen; }
const char* Error() { return g_error.c_str(); }

} // namespace thumbgen
