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
#include <array>
#include <cstdint>
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
// While the game streams heavily (startup, loading screens, fast travel) the loader can report success with an unfilled
// buffer: zeros parse as an empty prefab ("no meshes") and got recorded as permanent failures. A read that failed without
// the entry being missing sets g_readError, so the render is retried later instead of being recorded or cached.
static std::atomic<int> g_reads{ 0 }, g_badReads{ 0 };
static bool g_readError = false;   // worker thread only: a read in the current Generate() failed for a reason other than a missing entry
static bool LooksUnfilled(const std::vector<uint8_t>& d) {   // no pack file starts with 64 zero bytes (prefab, pam, pami, dds headers)
    const size_t n = std::min<size_t>(d.size(), 64); if (!n) return true;
    for (size_t i = 0; i < n; i++) if (d[i]) return false;
    return true;
}
static bool GetFile(std::string path, std::vector<uint8_t>& out) {
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    bool notFound = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (core::GameReadFile(path, out, &notFound)) {
            if (!LooksUnfilled(out)) { g_reads++; return true; }
            if (g_badReads++ < 5) Log("[thumbs] empty buffer from the game loader for %s (%zu bytes), attempt %d", path.c_str(), out.size(), attempt + 1);
            out.clear(); notFound = false;
        }
        if (notFound) return false;   // not in the packs: retrying cannot help, and it is not a transient error
        Sleep(15 + attempt * 100);
    }
    g_readError = true;
    return false;
}

// ---------------------------------------------------------------- reflection serializer (prefab / level objects)
struct RProp { std::string name, typeName; uint16_t type, fixed; uint32_t flags; };
struct RType { std::string name; std::vector<RProp> props; };
struct Node { bool hasXf = false; float xf[10] = { 0 }; std::string path, sub; std::vector<Node> kids;   // sub: instance of another prefab (its type name is the prefab path)
              bool hasDecalXf = false; float decalXf[10] = { 0 }; std::string decalTex; };   // DecalComponent _offsetTransform (box: scale, quat, pos) / DecalInfo _textureFilename

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
        if (!m.type->name.empty() && m.type->name[0] == '/' && EndsWith(m.type->name, ".prefab")) out.sub = m.type->name;
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
            if (p.fixed >= 40 && p.name == "_offsetTransform") { obj.hasDecalXf = true; for (int i = 0; i < 10; i++) obj.decalXf[i] = rdf(b + 4 * i); }
            break; }
        case 1: {   // size prefixed (strings), shared string table when present
            std::string s; bool got = false;
            if (!shared.empty()) { int32_t idx = r.i32(); if (idx != -1) { auto it = shared.find(idx); if (it != shared.end()) s = it->second; got = true; } }
            if (!got) { uint32_t len = r.u32(); s = r.str(len); }
            if (p.name == "_path") obj.path = s;
            else if (p.name == "_textureFilename") obj.decalTex = s;
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
// a prefab's root objects; logical "/object/x/y.prefab" is stored as "object/bin__/x/y.prefab" (older ones without bin__)
static bool LoadPrefabRoots(const std::string& logical, std::vector<Node>& roots, std::string* why) {
    std::string phys = logical; if (!phys.empty() && phys[0] == '/') phys.erase(0, 1);
    const size_t sl = phys.find('/');
    const std::string physBin = sl == std::string::npos ? phys : phys.substr(0, sl) + "/bin__/" + phys.substr(sl + 1);
    std::vector<uint8_t> data;
    if (!GetFile(physBin, data) && !GetFile(phys, data)) { if (why) *why = g_readError ? "read error" : "prefab missing"; return false; }
    try { ParsePrefab(data, roots); } catch (const std::exception& e) { if (why) *why = std::string("prefab: ") + e.what(); return false; }
    if (roots.empty() && why) {   // an empty object list from a non-empty file is suspicious: the first bytes tell a bad read from an empty prefab
        static int s_logged = 0;
        if (s_logged < 5) { s_logged++; char hex[64] = { 0 }; for (size_t i = 0; i < 16 && i < data.size(); i++) snprintf(hex + i * 3, 4, "%02X ", data[i]);
            Log("[thumbs] %s: no objects in %zu bytes (%s)", logical.c_str(), data.size(), hex); }
    }
    return true;
}
// Character appearance (.app_xml): the game assembles the character at runtime from the prefabs listed under <Nude>, <Head>,
// <Hair> and <Armor> (names without path, mixed case). All of them are loaded as roots of one preview. The flight cloak is
// left out: its spread glider covers the whole figure.
static bool LoadAppearanceRoots(const std::string& logical, std::vector<Node>& roots, std::string* why) {
    static std::unordered_map<std::string, std::string> byName;   // lower-case prefab base name -> logical path (worker thread only)
    if (byName.empty()) for (const auto& pi : core::PrefabIndex()) if (EndsWith(pi.path, ".prefab")) {
        std::string n = pi.path.substr(pi.path.rfind('/') + 1); n.resize(n.size() - 7); for (char& c : n) c = (char)tolower((unsigned char)c); byName.emplace(n, pi.path); }
    std::vector<uint8_t> x; std::string phys = logical[0] == '/' ? logical.substr(1) : logical;
    if (!GetFile(phys, x)) { if (why) *why = g_readError ? "read error" : "prefab missing"; return false; }
    const std::string s((const char*)x.data(), x.size()); static const char kTag[] = "<Prefab Name=\"";
    int parts = 0;
    for (size_t p = s.find(kTag); p != std::string::npos; p = s.find(kTag, p + 1)) {
        const size_t b = p + sizeof(kTag) - 1, e = s.find('"', b); if (e == std::string::npos) break;
        std::string n = s.substr(b, e - b); for (char& c : n) c = (char)tolower((unsigned char)c);
        if (n.find("cloak_flight") != std::string::npos) continue;
        auto it = byName.find(n); if (it == byName.end()) continue;
        std::vector<Node> r; if (LoadPrefabRoots(it->second, r, nullptr)) { for (auto& nd : r) roots.push_back(std::move(nd)); parts++; }
    }
    if (!parts && why) *why = "no meshes";
    return parts > 0;
}
// Sub-prefab instances (a child whose type is another prefab's path) are expanded in place with their transform, so the
// preview shows what spawning the parent shows. chain guards against a prefab that contains itself.
struct CollectCtx { std::vector<std::string> chain; int subs = 0; };
static void Collect(const Node& o, const float* pm, const float* pt, std::vector<Inst>& out, CollectCtx& cx, int depth = 0, bool decalDone = false) {
    float m[9], t[3]; memcpy(m, pm, sizeof m); memcpy(t, pt, sizeof t);
    if (o.hasXf) {
        float r[9]; QuatToMat(o.xf + 3, r); float local[9];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) local[i * 3 + j] = r[i * 3 + j] * o.xf[j];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float s = 0; for (int k = 0; k < 3; k++) s += pm[i * 3 + k] * local[k * 3 + j]; m[i * 3 + j] = s; }
        for (int i = 0; i < 3; i++) t[i] = pm[i * 3] * o.xf[7] + pm[i * 3 + 1] * o.xf[8] + pm[i * 3 + 2] * o.xf[9] + pt[i];
    }
    if (EndsWith(o.path, ".pami") || EndsWith(o.path, ".pam") || EndsWith(o.path, ".pac")) { Inst in; in.path = o.path; memcpy(in.m, m, sizeof m); memcpy(in.t, t, sizeof t); out.push_back(std::move(in)); }
    if (o.hasDecalXf) {   // decal: a texture projected down its box; previewed as the texture on a flat quad of the box's footprint
        std::string tex = o.decalTex;
        for (const auto& k : o.kids) { if (!tex.empty()) break; if (!k.decalTex.empty()) tex = k.decalTex; for (const auto& kk : k.kids) if (tex.empty() && !kk.decalTex.empty()) tex = kk.decalTex; }
        if (!tex.empty()) {
            float r[9], local[9], dm[9], dt[3]; QuatToMat(o.decalXf + 3, r);
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) local[i * 3 + j] = r[i * 3 + j] * o.decalXf[j];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float s = 0; for (int k = 0; k < 3; k++) s += m[i * 3 + k] * local[k * 3 + j]; dm[i * 3 + j] = s; }
            for (int i = 0; i < 3; i++) dt[i] = m[i * 3] * o.decalXf[7] + m[i * 3 + 1] * o.decalXf[8] + m[i * 3 + 2] * o.decalXf[9] + t[i];
            Inst in; in.path = "decal:" + tex; memcpy(in.m, dm, sizeof dm); memcpy(in.t, dt, sizeof dt); out.push_back(std::move(in));
        }
    } else if (!decalDone && !o.decalTex.empty()) {   // DecalInfo whose component has no _offsetTransform: default 1 m box
        Inst in; in.path = "decal:" + o.decalTex; memcpy(in.m, m, sizeof m); memcpy(in.t, t, sizeof t); out.push_back(std::move(in));
    }
    if (!o.sub.empty() && cx.subs < 64 && cx.chain.size() < 8 && std::find(cx.chain.begin(), cx.chain.end(), o.sub) == cx.chain.end()) {
        std::vector<Node> roots; cx.subs++;
        if (LoadPrefabRoots(o.sub, roots, nullptr)) { cx.chain.push_back(o.sub); for (const auto& r : roots) Collect(r, m, t, out, cx, depth + 1); cx.chain.pop_back(); }
    }
    if (depth < 200) for (const auto& k : o.kids) Collect(k, m, t, out, cx, depth + 1, decalDone || o.hasDecalXf || !o.decalTex.empty());
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

static inline float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 0.0f : ldexpf((float)(m | 1024), (int)e - 25);   // inf/nan -> 0, like CDMW
    return s ? -v : v;
}
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
        // uv: two halves at +8 (as CDMW reads them). The old u16/65535 at +10 was wrong everywhere but only showed on atlas
        // textures (cloth, props); tiling ones looked plausible with any uv.
        if (stride >= 12 && (size_t)off + 12 <= c.n) { out.uv.push_back(HalfToFloat(rd16(p + 8))); out.uv.push_back(HalfToFloat(rd16(p + 10))); } else { out.uv.push_back(0); out.uv.push_back(0); }
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

// ---------------------------------------------------------------- .pac skinned mesh (CDMW mesh_parser.parse_pac)
// PAR header: 8 section slots at 0x10 {u32 compressed size (0 = stored), u32 size}, sections follow 0x50 back to back, each
// LZ4 on its own when compressed ("partial" pack entries arrive like that). Section 0 holds one descriptor per submesh
// (bbox, per-LOD vertex and index counts), sections 4..1 the geometry of LOD 0..3: 40-byte vertex records (pos u16x3 over
// the descriptor bbox / 32767, uv f16x2 at +8), then u16 indices. Only the rest pose is drawn, bone weights are not needed.
struct PacDesc { std::string name, material; float bmin[3], bext[3]; uint32_t vc[10] = { 0 }, ic[10] = { 0 }; size_t start; };
static std::string PacName(const uint8_t* r, size_t n, size_t cursor, bool* ok) {   // length-prefixed ASCII record ending at cursor
    *ok = false;
    for (size_t back = 1; back < 200 && back <= cursor; back++) {
        const size_t pos = cursor - back; const uint8_t len = r[pos];
        if (len == 0 || len != back - 1) continue;
        bool ascii = true; for (size_t k = pos + 1; k < cursor; k++) if (r[k] < 32 || r[k] >= 127) { ascii = false; break; }
        if (!ascii) continue;
        *ok = true; return std::string((const char*)r + pos + 1, cursor - pos - 1);
    }
    return "";
}
static void PacNames(const uint8_t* r, size_t n, size_t start, PacDesc& d) {
    if (start > 1 && r[start - 1] == 0) {   // newer layout: one shared record "len + ASCII + NUL" before the descriptor
        bool ok; std::string s = PacName(r, n, start - 1, &ok); if (ok && !s.empty()) { d.name = d.material = s; return; }
    }
    size_t cursor = start; std::string found[2];   // older layout: name record, then material record, both right before the descriptor
    for (int k = 0; k < 2; k++) { bool ok; found[k] = PacName(r, n, cursor, &ok); if (!ok) break; cursor -= found[k].size() + 1; }
    d.material = found[0]; d.name = found[1];
}
static bool ParsePac(const std::vector<uint8_t>& in, Mesh& out) {
    if (in.size() < 0x50 || memcmp(in.data(), "PAR ", 4) != 0) return false;
    struct Sec { size_t off = 0, size = 0; bool present = false; } sec[8];
    std::vector<uint8_t> d(in.begin(), in.begin() + 0x50);
    size_t fo = 0x50;
    for (int s = 0; s < 8; s++) {
        const uint32_t cs = rd32(in.data() + 0x10 + s * 8), ds = rd32(in.data() + 0x14 + s * 8);
        if (!ds) continue;
        const size_t stored = cs ? cs : ds; if (fo + stored > in.size() || ds > (256u << 20)) return false;
        sec[s].off = d.size(); sec[s].size = ds; sec[s].present = true;
        if (cs) { std::vector<uint8_t> x; if (!Lz4Block(in.data() + fo, cs, x, ds)) return false; d.insert(d.end(), x.begin(), x.end()); out.compressed = true; }
        else d.insert(d.end(), in.begin() + fo, in.begin() + fo + ds);
        fo += stored;
    }
    if (!sec[0].present || sec[0].size < 5) return false;
    const uint8_t* R = d.data() + sec[0].off; const size_t RN = sec[0].size;
    const int nLods = R[4]; if (nLods <= 0 || nLods > 10) return false;
    // descriptors: found by the LOD index pattern that sits 35 bytes into each one
    std::vector<PacDesc> descs; std::set<size_t> seen;
    struct Pat { uint8_t b[5]; int len, lods, vcOff, icOff, rule; };
    static const Pat pats[] = { { { 4, 0, 1, 2, 3 }, 5, 4, 40, 48, 0 }, { { 3, 0, 1, 1, 2 }, 5, 3, 40, 46, 0 }, { { 3, 0, 1, 2 }, 4, 3, 40, 46, 1 }, { { 2, 0, 1 }, 3, 2, 40, 44, 2 } };
    for (const Pat& p : pats) {
        for (size_t idx = 0; idx + p.len <= RN; idx++) {
            if (memcmp(R + idx, p.b, p.len) != 0) continue;
            if (p.rule == 1 && idx >= 1 && R[idx - 1] == 4) continue;
            if (p.rule == 2 && idx >= 1 && (R[idx - 1] == 3 || R[idx - 1] == 4)) continue;
            if (idx < 35) continue; const size_t st = idx - 35;
            if (seen.count(st) || st + p.icOff + (size_t)p.lods * 4 > RN || R[st] != 1) continue;
            PacDesc ds; ds.start = st; float fl[8]; memcpy(fl, R + st + 3, sizeof fl);
            for (int k = 0; k < 3; k++) { ds.bmin[k] = fl[2 + k]; ds.bext[k] = fl[5 + k]; }
            bool any = false, bad = false;
            for (int l = 0; l < p.lods; l++) { ds.vc[l] = rd16(R + st + p.vcOff + l * 2); ds.ic[l] = rd32(R + st + p.icOff + l * 4); if (ds.vc[l]) any = true; if (ds.vc[l] > 200000 || ds.ic[l] > 20000000) bad = true; }
            if (!any || bad) continue;
            PacNames(R, RN, st, ds); seen.insert(st); descs.push_back(ds);
        }
    }
    if (descs.empty()) return false;
    std::sort(descs.begin(), descs.end(), [](const PacDesc& a, const PacDesc& b) { return a.start < b.start; });
    {   // drop trailing false matches when a prefix of the descriptors fills every LOD section exactly
        for (size_t cnt = descs.size(); cnt >= 2; cnt--) {
            bool exact = true, anySec = false;
            for (int s = 1; s <= 4; s++) { if (!sec[s].present) continue; anySec = true; const int lod = 4 - s; uint64_t need = 0;
                for (size_t i = 0; i < cnt; i++) need += (uint64_t)descs[i].vc[lod] * 40 + (uint64_t)descs[i].ic[lod] * 2;
                if (need != sec[s].size) { exact = false; break; } }
            if (!anySec) break;
            if (exact) { descs.resize(cnt); break; }
        }
    }
    for (const auto& ds : descs) out.mats.push_back(ds.name.empty() ? ds.material : ds.name);
    for (int s = 4; s >= 1; s--) {   // LOD 0 first; a lower LOD only when it fails
        if (!sec[s].present) continue;
        const int lod = 4 - s; const size_t so = sec[s].off, ss = sec[s].size; const uint8_t* S = d.data() + so;
        uint64_t totalV = 0, totalI = 0; for (auto& ds : descs) { totalV += ds.vc[lod]; totalI += ds.ic[lod]; }
        const size_t primary = (size_t)totalV * 40, idxBytes = (size_t)totalI * 2;
        size_t vBase = 0, iStart = primary;
        if (primary + idxBytes < ss) {   // extra records before the vertices: find the split whose first triangles are the shortest
            const size_t gap = ss - primary - idxBytes;
            const PacDesc* first = nullptr; for (auto& ds : descs) if (ds.vc[lod]) { first = &ds; break; }
            if (first) {
                const uint32_t fvc = first->vc[lod];
                auto quality = [&](size_t vs, size_t is) -> double {
                    if (is + idxBytes > ss) return 1e300;
                    uint32_t fic = 0; for (auto& ds : descs) if (ds.ic[lod]) { fic = ds.ic[lod]; break; }
                    const uint32_t nt = fic / 3; if (!nt) return 0;
                    const uint32_t step = std::max<uint32_t>(1, nt / 30); std::set<uint32_t> tri; for (uint32_t t = 0; t < std::min<uint32_t>(12, nt); t++) tri.insert(t); for (uint32_t t = 0; t < nt; t += step) tri.insert(t);
                    uint32_t mx = 0; std::vector<std::array<uint16_t, 3>> st;
                    for (uint32_t t : tri) { if (is + (size_t)t * 6 + 6 > ss) return 1e300; std::array<uint16_t, 3> a = { rd16(S + is + t * 6), rd16(S + is + t * 6 + 2), rd16(S + is + t * 6 + 4) }; st.push_back(a); mx = std::max<uint32_t>({ mx, a[0], a[1], a[2] }); }
                    const uint32_t need = std::max<uint32_t>(fvc, mx + 1); if (is <= vs || need > (is - vs) / 40) return 1e300;
                    auto pos = [&](uint16_t i, float* p) { const uint8_t* q = S + vs + (size_t)i * 40; for (int k = 0; k < 3; k++) p[k] = fabsf(first->bext[k]) < 1e-8f ? first->bmin[k] : first->bmin[k] + rd16(q + k * 2) / 32767.0f * first->bext[k]; };
                    double tot = 0; for (auto& a : st) { float p[3][3]; for (int k = 0; k < 3; k++) pos(a[k], p[k]);
                        auto dist = [](const float* x, const float* y) { return sqrt((double)(x[0] - y[0]) * (x[0] - y[0]) + (double)(x[1] - y[1]) * (x[1] - y[1]) + (double)(x[2] - y[2]) * (x[2] - y[2])); };
                        tot += std::max({ dist(p[0], p[1]), dist(p[1], p[2]), dist(p[2], p[0]) }); }
                    return tot;
                };
                size_t bestV = 0, bestI = primary + (gap / 40) * 40; double best = quality(bestV, bestI);
                for (size_t nsec = 0; nsec <= gap / 40; nsec++) {
                    const size_t vs = nsec * 40, end = vs + primary; if (end >= ss) break;
                    size_t found = SIZE_MAX;
                    for (size_t t = end; t + 6 <= ss; t += 2) if (rd16(S + t) == 0 && rd16(S + t + 2) < fvc && rd16(S + t + 4) < fvc) { found = t; break; }
                    if (found == SIZE_MAX || found + idxBytes > ss) continue;
                    const double q = quality(vs, found); if (q < best) { best = q; bestV = vs; bestI = found; }
                }
                vBase = bestV; iStart = bestI;
            }
        }
        Mesh m; m.mats = out.mats; m.compressed = out.compressed;
        std::vector<size_t> vOff; size_t cur = vBase; for (auto& ds : descs) { vOff.push_back(cur); cur += (size_t)ds.vc[lod] * 40; }
        size_t io = iStart;
        for (size_t di = 0; di < descs.size(); di++) {
            const PacDesc& ds = descs[di]; const uint32_t vc = ds.vc[lod], ic = ds.ic[lod];
            if (!vc && !ic) continue;
            const size_t icn = io >= ss ? 0 : std::min<size_t>(ic, (ss - io) / 2);
            std::vector<uint16_t> idx(icn); if (icn) memcpy(idx.data(), S + io, icn * 2);
            size_t owner = di; uint32_t ownerVc = vc; uint32_t mxI = 0; for (uint16_t x : idx) mxI = std::max<uint32_t>(mxI, x);
            if (icn && mxI >= vc) {   // a submesh can index the vertex block of another one
                size_t partner = SIZE_MAX; for (size_t pj = 0; pj < descs.size(); pj++) if (pj != di && descs[pj].vc[lod] > mxI) { partner = pj; break; }
                if (partner != SIZE_MAX) { owner = partner; ownerVc = descs[partner].vc[lod]; }
                else { const size_t avail = iStart > vOff[di] ? (iStart - vOff[di]) / 40 : 0; if (mxI < avail) ownerVc = mxI + 1; }
            }
            const PacDesc& od = descs[owner]; (void)od;
            const uint32_t base = (uint32_t)(m.v.size() / 3); uint32_t nv = 0;
            for (uint32_t vi = 0; vi < ownerVc; vi++) {
                const size_t ro = vOff[owner] + (size_t)vi * 40; if (so + ro + 40 > d.size()) break;
                const uint8_t* q = S + ro;
                for (int k = 0; k < 3; k++) m.v.push_back(fabsf(ds.bext[k]) < 1e-8f ? ds.bmin[k] : ds.bmin[k] + rd16(q + k * 2) / 32767.0f * ds.bext[k]);   // decoded with this submesh's bbox, as CDMW does
                m.uv.push_back(HalfToFloat(rd16(q + 8))); m.uv.push_back(HalfToFloat(rd16(q + 10)));
                nv++;
            }
            for (size_t j = 0; j + 2 < icn; j += 3) {
                const uint16_t a = idx[j], b = idx[j + 1], c = idx[j + 2];
                if (a >= nv || b >= nv || c >= nv || a == b || b == c || a == c) continue;
                m.f.push_back(base + a); m.f.push_back(base + b); m.f.push_back(base + c); m.fm.push_back((uint8_t)std::min<size_t>(di, 255));
            }
            io += (size_t)ic * 2;
        }
        if (!m.v.empty() && !m.f.empty()) { out = std::move(m); return true; }
    }
    return false;
}

// ---------------------------------------------------------------- materials (.pami XML) and textures (DDS)
// Preview quality: 0 = base colour only, 1 = + dye / tint colours, 2 = + normal maps, 3 = + specular and emissive.
// Each level loads more textures per surface, so it is also a speed setting for the background pass.
static int g_quality = 3;
struct MatInfo {
    std::string tex, norm, spec, emi, mask, overlay;   // _baseColorTexture, _normalTexture, _materialTexture (r ao, g roughness, b metal), emissive, dye mask, overlay
    float rc[3] = { 0.72f, 0.66f, 0.56f };             // RepresentColor: fallback when there is no texture
    float tint[3] = { 1, 1, 1 }; bool hasTint = false;  // .pami _tintColor: multiplies the base colour
    float zone[3][3] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } }; bool hasZones = false;   // _tintColorR/G/B: colours of the mask's r/g/b zones (characters)
    float emiCol[3] = { 1, 1, 1 }; float emiInt = 1.0f;
    float uvScale = 1.0f; bool decal = false;           // decal: MeshDecal shader, drawn on top of other geometry
};
static std::unordered_map<std::string, std::unordered_map<std::string, MatInfo>> g_pamiMats;
static std::string XmlAttr(const std::string& s, size_t from, size_t to, const char* key) {   // value of key="..." inside [from,to)
    size_t p = s.find(key, from); if (p == std::string::npos || p >= to) return "";
    p += strlen(key); size_t q = s.find('"', p); if (q == std::string::npos || q > to) return "";
    return s.substr(p, q - p);
}
static bool ParseColor(const std::string& v, float* c) {   // "#rrggbbaa" (.pac_xml) or "r g b" floats (.pami)
    if (v.size() >= 7 && v[0] == '#') { for (int k = 0; k < 3; k++) c[k] = strtol(v.substr(1 + k * 2, 2).c_str(), nullptr, 16) / 255.0f; return true; }
    float r, g, b; if (sscanf(v.c_str(), "%f %f %f", &r, &g, &b) == 3) { c[0] = r; c[1] = g; c[2] = b; return true; }
    return false;
}
static void ApplyMatParam(MatInfo& m, const std::string& name, const std::string& value) {
    const bool none = value.empty() || value.find("nonetexture") != std::string::npos;
    if (name == "_baseColorTexture") { if (!none) m.tex = value; }
    else if (name == "_normalTexture") { if (!none) m.norm = value; }
    else if (name == "_materialTexture") { if (!none) m.spec = value; }
    else if (name == "_emissiveTexture" || name == "_emissiveIntensityTexture") { if (!none) m.emi = value; }
    else if (name == "_colorBlendingMaskTexture") { if (!none) m.mask = value; }
    else if (name == "_overlayColorTexture") { if (!none) m.overlay = value; }
    else if (name == "_tintColor") m.hasTint = ParseColor(value, m.tint);
    else if (name == "_tintColorR" || name == "_tintColorG" || name == "_tintColorB") { if (ParseColor(value, m.zone[name.back() == 'R' ? 0 : name.back() == 'G' ? 1 : 2])) m.hasZones = true; }
    else if (name == "_emissiveColor") ParseColor(value, m.emiCol);
    else if (name == "_emissiveIntensity") { if (!value.empty()) m.emiInt = (float)atof(value.c_str()); }
    else if (name == "_uvScale") { if (!value.empty()) m.uvScale = (float)atof(value.c_str()); if (!(m.uvScale > 0.01f && m.uvScale < 100.0f)) m.uvScale = 1.0f; }
}
// <Material PrimitiveName="x"> ... <RepresentColor x= y= z=/> ... <MaterialParameterTexture Name="_baseColorTexture" Value="..."/> ... <MaterialParameterFloat Name="_uvScale" Value=.../>
// .pac: character/model/<p>.pac -> character/modelproperty/<p>.pac_xml, <SkinnedMeshMaterialWrapper _subMeshName="x"> ... _baseColorTexture ... _path="...dds"
static void LoadPacMaterials(const std::string& s, std::unordered_map<std::string, MatInfo>& mats) {
    static const char kTag[] = "<SkinnedMeshMaterialWrapper"; size_t p = 0;
    while ((p = s.find(kTag, p)) != std::string::npos) {
        size_t end = s.find("</SkinnedMeshMaterialWrapper>", p); if (end == std::string::npos) end = s.size();
        const size_t tagEnd = s.find('>', p); std::string name = XmlAttr(s, p, tagEnd == std::string::npos ? end : tagEnd, "_subMeshName=\"");
        MatInfo m;   // <MaterialParameterX ... _name="n" _value="v"/>, textures: _path="..." inside <MaterialParameterTexture>...</MaterialParameterTexture>
        for (size_t q = s.find("<MaterialParameter", p); q != std::string::npos && q < end; q = s.find("<MaterialParameter", q + 1)) {
            const size_t te = s.find('>', q); if (te == std::string::npos || te > end) break;
            const std::string pn = XmlAttr(s, q, te, "_name=\""); if (pn.empty()) continue;
            std::string pv = XmlAttr(s, q, te, "_value=\"");
            if (s.compare(q, 25, "<MaterialParameterTexture") == 0) { const size_t ce = s.find("</MaterialParameterTexture>", te); pv = XmlAttr(s, te, ce == std::string::npos ? end : std::min(ce, end), "_path=\""); }
            ApplyMatParam(m, pn, pv);
        }
        if (!name.empty()) { for (char& c : name) c = (char)tolower((unsigned char)c); mats.emplace(name, m); }   // lower case: descriptor names differ in case; emplace: the first ModelProperty is the default look
        p = end;
    }
}
static const std::unordered_map<std::string, MatInfo>* LoadMaterials(const std::string& pamiPath) {
    if (pamiPath.compare(0, 6, "decal:") == 0) {   // the decal texture itself; alpha cuts the shape out
        auto it = g_pamiMats.find(pamiPath); if (it != g_pamiMats.end()) return &it->second;
        MatInfo m; m.tex = pamiPath.substr(6); return &(g_pamiMats[pamiPath] = { { "decal", m } });
    }
    const bool pac = EndsWith(pamiPath, ".pac");
    if (!pac && !EndsWith(pamiPath, ".pami")) return nullptr;
    auto it = g_pamiMats.find(pamiPath); if (it != g_pamiMats.end()) return &it->second;
    std::unordered_map<std::string, MatInfo> mats; std::vector<uint8_t> x;
    if (pac) {
        std::string xml = pamiPath; const size_t mp = xml.find("/model/"); if (mp != std::string::npos) xml.replace(mp, 7, "/modelproperty/"); xml += "_xml";
        if (GetFile(xml, x)) LoadPacMaterials(std::string((const char*)x.data(), x.size()), mats);
    }
    else if (GetFile(pamiPath, x)) {
        std::string s((const char*)x.data(), x.size()); size_t p = 0;
        static const char kTag[] = "<Material PrimitiveName=\""; const size_t tagLen = sizeof(kTag) - 1;
        while ((p = s.find(kTag, p)) != std::string::npos) {
            size_t q = s.find('"', p + tagLen); if (q == std::string::npos) break;
            std::string name = s.substr(p + tagLen, q - (p + tagLen)); size_t end = s.find("</Material>", q); if (end == std::string::npos) end = s.size();
            MatInfo m;
            size_t rc = s.find("<RepresentColor", q);
            if (rc != std::string::npos && rc < end) { std::string xs = XmlAttr(s, rc, end, " x=\""), ys = XmlAttr(s, rc, end, " y=\""), zs = XmlAttr(s, rc, end, " z=\""); if (!xs.empty() && !ys.empty() && !zs.empty()) { m.rc[0] = (float)atof(xs.c_str()); m.rc[1] = (float)atof(ys.c_str()); m.rc[2] = (float)atof(zs.c_str()); } }
            for (size_t t = s.find("<MaterialParameter", q); t != std::string::npos && t < end; t = s.find("<MaterialParameter", t + 1)) {   // <MaterialParameterX Name="n" Value="v"/>
                const size_t te = s.find('>', t); if (te == std::string::npos || te > end) break;
                ApplyMatParam(m, XmlAttr(s, t, te, " Name=\""), XmlAttr(s, t, te, " Value=\""));
            }
            size_t mn = s.find("MaterialName=\"", q); if (mn != std::string::npos && mn < end) { std::string shader = XmlAttr(s, mn, end, "MaterialName=\""); if (shader.find("Decal") != std::string::npos) m.decal = true; }
            mats[name] = m; p = end;
        }
    }
    if (g_pamiMats.size() > 4000) g_pamiMats.clear();
    if (x.empty() && g_readError) { static std::unordered_map<std::string, MatInfo> s_none; return &s_none; }   // read error: not cached, the retry reads it again
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
// BC4 block (one channel: two endpoints + 16 3-bit indices, same as the BC3 alpha block) into channel ch of rgba
static void DecodeBc4Block(const uint8_t* b, int bx, int by, int ch, TexImg& out) {
    const int a0 = b[0], a1 = b[1]; int pal[8] = { a0, a1 };
    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) { const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x >= out.w || y >= out.h) continue; out.rgba[((size_t)y * out.w + x) * 4 + ch] = (uint8_t)pal[(bits >> (3 * i)) & 7]; }
}
// BC4 (emissive masks: r) / BC5 (normal maps: r = x, g = y)
static void DecodeBc45(const uint8_t* src, size_t n, int w, int h, bool bc5, TexImg& out) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = bc5 ? 16 : 8;
    out.w = w; out.h = h; out.alpha = false; out.rgba.assign((size_t)w * h * 4, 255);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const size_t bo = ((size_t)by * bw + bx) * bs; if (bo + bs > n) return;
        DecodeBc4Block(src + bo, bx, by, 0, out); if (bc5) DecodeBc4Block(src + bo + 8, bx, by, 1, out);
    }
}
// DDS: the mip level with at most 256 px on the long side is decoded (BC1, BC3, BC4, BC5; anything else falls back to the material colour)
static std::shared_ptr<TexImg> LoadTexture(const std::string& path) {
    auto it = g_texs.find(path); if (it != g_texs.end()) return it->second;
    if (g_texBytes > (192u << 20)) { g_texs.clear(); g_texBytes = 0; }
    std::shared_ptr<TexImg> t; std::vector<uint8_t> d; const char* kind = "?";
    if (GetFile(path, d) && d.size() >= 128 && memcmp(d.data(), "DDS ", 4) == 0) {
        int h = (int)rd32(d.data() + 12), w = (int)rd32(d.data() + 16), mips = std::max(1, (int)rd32(d.data() + 28));
        const uint32_t fourcc = rd32(d.data() + 84); size_t off = 128; int fmt = 0;   // 1 BC1, 3 BC3, 4 BC4, 5 BC5
        if (fourcc == 0x30315844) {   // "DX10": dxgi format follows
            const uint32_t dxgi = rd32(d.data() + 128); off = 148;
            fmt = (dxgi == 71 || dxgi == 72) ? 1 : (dxgi == 77 || dxgi == 78) ? 3 : (dxgi == 80 || dxgi == 81) ? 4 : (dxgi == 83 || dxgi == 84) ? 5 : 0;
        } else fmt = fourcc == 0x31545844 ? 1 : fourcc == 0x35545844 ? 3 : (fourcc == 0x55344342 || fourcc == 0x31495441) ? 4 : (fourcc == 0x55354342 || fourcc == 0x32495441) ? 5 : 0;   // DXT1 DXT5 BC4U/ATI1 BC5U/ATI2
        if (fmt && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            const int bs = (fmt == 1 || fmt == 4) ? 8 : 16;
            while ((w > 256 || h > 256) && mips > 1) { off += (size_t)((w + 3) / 4) * ((h + 3) / 4) * bs; w = std::max(1, w / 2); h = std::max(1, h / 2); mips--; }
            if (off < d.size()) {
                t = std::make_shared<TexImg>();
                if (fmt <= 3) DecodeBc(d.data() + off, d.size() - off, w, h, fmt == 3, *t); else DecodeBc45(d.data() + off, d.size() - off, w, h, fmt == 5, *t);
                g_texBytes += t->rgba.size(); kind = fmt == 1 ? "bc1" : fmt == 3 ? "bc3" : fmt == 4 ? "bc4" : "bc5";
            }
        }
    }
    if (d.empty() && g_readError) return t;   // read error: not cached
    static int s_logged = 0; if (s_logged < 6) { s_logged++; Log("[thumbs] texture %s: %s%s", path.c_str(), t ? "ok" : "FAILED", t ? (std::string(" ") + std::to_string(t->w) + "x" + std::to_string(t->h) + " " + kind).c_str() : (d.empty() ? " (read failed)" : " (format)")); }
    g_texs[path] = t;
    return t;
}
struct Surface {
    std::shared_ptr<TexImg> tex, norm, spec, emi, mask, overlay;   // only the ones g_quality asks for are loaded
    float col[3]; float uvScale; bool skip = false;
    float tint[3] = { 1, 1, 1 }; bool hasTint = false;
    float zone[3][3] = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } }; bool hasZones = false;
    float emiCol[3] = { 1, 1, 1 }; float emiInt = 1.0f;
};

// ---------------------------------------------------------------- mesh cache
static std::unordered_map<std::string, std::shared_ptr<Mesh>> g_meshes;
static std::unordered_map<std::string, std::string> g_pami;
static size_t g_meshBytes = 0;

static std::string ResolvePam(const std::string& path) {
    if (EndsWith(path, ".pam") || EndsWith(path, ".pac")) return path;
    auto it = g_pami.find(path); if (it != g_pami.end()) return it->second;
    size_t dot = path.rfind('.'); std::string res = (dot == std::string::npos ? path : path.substr(0, dot)) + ".pam";
    std::vector<uint8_t> x;
    if (GetFile(path, x)) {   // .pami is XML: <StaticMesh Path="....pam"/>
        static const char kTag[] = "<StaticMesh Path=\"";
        std::string s((const char*)x.data(), x.size());
        size_t p = s.find(kTag);
        if (p != std::string::npos) { p += sizeof(kTag) - 1; size_t q = s.find('"', p); if (q != std::string::npos) { std::string v = s.substr(p, q - p); if (EndsWith(v, ".pam")) res = v; } }
    }
    if (x.empty() && g_readError) return res;   // read error: guessed name only, not cached
    g_pami[path] = res;
    return res;
}
static std::shared_ptr<Mesh> LoadMesh(const std::string& path) {
    if (path.compare(0, 6, "decal:") == 0) {   // unit quad in the box's XZ plane (decals project along their local Y), uv 0..1
        static std::shared_ptr<Mesh> quad = [] { auto q = std::make_shared<Mesh>();
            q->v = { -0.5f, 0, -0.5f, 0.5f, 0, -0.5f, 0.5f, 0, 0.5f, -0.5f, 0, 0.5f }; q->uv = { 0, 0, 1, 0, 1, 1, 0, 1 };
            q->f = { 0, 1, 2, 0, 2, 3 }; q->fm = { 0, 0 }; q->mats = { "decal" }; return q; }();
        return quad;
    }
    std::string pam = ResolvePam(path);
    auto it = g_meshes.find(pam); if (it != g_meshes.end()) return it->second;
    if (g_meshBytes > (400u << 20)) { g_meshes.clear(); g_meshBytes = 0; }
    std::shared_ptr<Mesh> m;
    std::vector<uint8_t> data;
    if (GetFile(pam, data)) { auto mm = std::make_shared<Mesh>(); if (EndsWith(pam, ".pac") ? ParsePac(data, *mm) : ParsePam(data, *mm)) { m = mm; g_meshBytes += mm->v.size() * 4 + mm->f.size() * 4; } }
    else if (g_readError) return m;   // read error: not cached, a later render reads it again
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
static bool RenderPng(const Mesh& mesh, const std::vector<uint16_t>& fs, const std::vector<Surface>& surf, const std::string& file, float azDeg = 35.0f, float elDeg = 25.0f) {
    const int S = 256, SS = 2, W = S * SS;
    const size_t nv = mesh.v.size() / 3; if (!nv || mesh.f.size() < 3) return false;
    const bool haveUv = mesh.uv.size() == nv * 2;
    // camera: azimuth 35 deg around Y, then elevation 25 deg around X (matches scripts/render_thumbs.py)
    const float az = azDeg * 3.14159265f / 180.0f, el = elDeg * 3.14159265f / 180.0f;
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
    auto texel = [](const TexImg* t, float u, float v) -> const uint8_t* {   // nearest sample, wrapping
        u -= floorf(u); v -= floorf(v);
        const int tx = std::min(t->w - 1, (int)(u * t->w)), ty = std::min(t->h - 1, (int)(v * t->h));
        return &t->rgba[((size_t)ty * t->w + tx) * 4];
    };
    const float hv[3] = { light[0], light[1], light[2] + 1.0f }; float half[3]; { const float l = sqrtf(hv[0] * hv[0] + hv[1] * hv[1] + hv[2] * hv[2]); for (int k = 0; k < 3; k++) half[k] = hv[k] / l; }   // Blinn half vector, the view looks down -z
    for (const Tri& tr : tris) {   // z-buffered rasterizer with barycentric uv interpolation (orthographic view, so no perspective correction)
        float xs[3], ys[3], zs[3], us[3] = { 0, 0, 0 }, vs[3] = { 0, 0, 0 };
        const Surface* sf = (tr.i < fs.size() && fs[tr.i] < surf.size()) ? &surf[fs[tr.i]] : nullptr;
        auto ok = [](const std::shared_ptr<TexImg>& t) -> const TexImg* { return t && t->w > 0 ? t.get() : nullptr; };
        const TexImg* tex = sf ? ok(sf->tex) : nullptr;
        const TexImg* mask = sf && sf->hasZones ? ok(sf->mask) : nullptr; const TexImg* ovl = mask ? ok(sf->overlay) : nullptr;
        const TexImg* nrm = sf && haveUv ? ok(sf->norm) : nullptr; const TexImg* spc = sf ? ok(sf->spec) : nullptr; const TexImg* emi = sf ? ok(sf->emi) : nullptr;
        const bool anyTex = tex || mask || nrm || spc || emi;
        for (int k = 0; k < 3; k++) { const uint32_t vi = mesh.f[tr.i * 3 + k]; const float* P = &p[vi * 3]; xs[k] = (P[0] - cx) * scale + W / 2.0f; ys[k] = W / 2.0f - (P[1] - cy) * scale; zs[k] = P[2];
            if (anyTex && haveUv) { us[k] = mesh.uv[vi * 2] * sf->uvScale; vs[k] = mesh.uv[vi * 2 + 1] * sf->uvScale; } }
        const float det = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]); if (fabsf(det) < 1e-6f) continue;
        const int x0 = std::max(0, (int)floorf(std::min({ xs[0], xs[1], xs[2] }))), x1 = std::min(W - 1, (int)ceilf(std::max({ xs[0], xs[1], xs[2] })));
        const int y0 = std::max(0, (int)floorf(std::min({ ys[0], ys[1], ys[2] }))), y1 = std::min(W - 1, (int)ceilf(std::max({ ys[0], ys[1], ys[2] })));
        float base[3] = { 214, 196, 168 }; if (sf) { base[0] = sf->col[0] * 255; base[1] = sf->col[1] * 255; base[2] = sf->col[2] * 255; }
        // tangent frame in view space for the normal map: N faces the camera, T/B follow the uv directions of this triangle
        float N[3] = { 0, 0, 1 }, T[3] = { 1, 0, 0 }, Bt[3] = { 0, 1, 0 }; bool frame = false;
        if (nrm || spc) {
            const float* A = &p[mesh.f[tr.i * 3] * 3]; const float* B = &p[mesh.f[tr.i * 3 + 1] * 3]; const float* C = &p[mesh.f[tr.i * 3 + 2] * 3];
            const float e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            N[0] = e1[1] * e2[2] - e1[2] * e2[1]; N[1] = e1[2] * e2[0] - e1[0] * e2[2]; N[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float ln = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]); if (ln > 1e-12f) { for (float& x : N) x /= ln; if (N[2] < 0) for (float& x : N) x = -x; }
            const float du1 = us[1] - us[0], dv1 = vs[1] - vs[0], du2 = us[2] - us[0], dv2 = vs[2] - vs[0], dd = du1 * dv2 - du2 * dv1;
            if (nrm && fabsf(dd) > 1e-12f) {
                const float r = 1.0f / dd; float t[3], b[3];
                for (int k = 0; k < 3; k++) { t[k] = (e1[k] * dv2 - e2[k] * dv1) * r; b[k] = (e2[k] * du1 - e1[k] * du2) * r; }
                const float tn = t[0] * N[0] + t[1] * N[1] + t[2] * N[2]; for (int k = 0; k < 3; k++) t[k] -= N[k] * tn;   // Gram-Schmidt
                const float lt = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (lt > 1e-12f) { for (int k = 0; k < 3; k++) T[k] = t[k] / lt;
                    float c3[3] = { N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2], N[0] * T[1] - N[1] * T[0] };
                    const float hand = (c3[0] * b[0] + c3[1] * b[1] + c3[2] * b[2]) < 0 ? -1.0f : 1.0f;
                    for (int k = 0; k < 3; k++) Bt[k] = c3[k] * hand; frame = true; }
            }
        }
        for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
            const float fx = px + 0.5f, fy = py + 0.5f;
            const float l1 = ((fx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (fy - ys[0])) / det;
            const float l2 = ((xs[1] - xs[0]) * (fy - ys[0]) - (fx - xs[0]) * (ys[1] - ys[0])) / det;
            const float l0 = 1.0f - l1 - l2; if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float z = l0 * zs[0] + l1 * zs[1] + l2 * zs[2]; float& zb = zbuf[(size_t)py * W + px]; if (z <= zb) continue;
            float c[3] = { base[0], base[1], base[2] };
            const float u = l0 * us[0] + l1 * us[1] + l2 * us[2], v = l0 * vs[0] + l1 * vs[1] + l2 * vs[2];
            if (tex) {
                const uint8_t* q = texel(tex, u, v);
                if (tex->alpha && q[3] < 128) continue;   // cut-out (leaves, grates)
                c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
                if (sf->hasTint) for (int k = 0; k < 3; k++) c[k] *= sf->tint[k];
            } else if (mask) {   // dye zones: mask r/g/b pick the zone colours, the grey overlay texture shades them (overlay blend)
                const uint8_t* m = texel(mask, u, v); const float w0 = m[0] / 255.0f, w1 = m[1] / 255.0f, w2 = m[2] / 255.0f, ws = w0 + w1 + w2;
                for (int k = 0; k < 3; k++) {
                    float col = ws > 1e-3f ? (w0 * sf->zone[0][k] + w1 * sf->zone[1][k] + w2 * sf->zone[2][k]) / ws : sf->zone[0][k];
                    if (ovl) { const float o = texel(ovl, u, v)[0] / 255.0f; col = col < 0.5f ? 2 * col * o : 1 - 2 * (1 - col) * (1 - o); }
                    c[k] = col * 255.0f;
                }
            }
            float shade = tr.shade, spec = 0, ao = 1, metal = 0;
            if (frame || spc) {
                float n[3] = { N[0], N[1], N[2] };
                if (frame) {   // BC5 normal map: x, y in r, g (DirectX: green points down the texture), z rebuilt
                    const uint8_t* q = texel(nrm, u, v); const float nx = q[0] / 127.5f - 1.0f, ny = -(q[1] / 127.5f - 1.0f), nz = sqrtf(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                    for (int k = 0; k < 3; k++) n[k] = T[k] * nx + Bt[k] * ny + N[k] * nz;
                    const float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (ln > 1e-6f) for (float& x : n) x /= ln;
                    shade = fabsf(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) * 0.75f + 0.25f;
                }
                if (spc) {   // _materialTexture: r ambient occlusion, g roughness, b metal -> Blinn-Phong highlight
                    const uint8_t* q = texel(spc, u, v); ao = 0.75f + 0.25f * q[0] / 255.0f; const float rough = q[1] / 255.0f; metal = q[2] / 255.0f;
                    const float nh = std::max(0.0f, n[0] * half[0] + n[1] * half[1] + n[2] * half[2]), gloss = (1 - rough) * (1 - rough);
                    spec = powf(nh, 4.0f + gloss * 124.0f) * (0.08f + gloss * 0.9f);
                }
            }
            float outc[3];
            for (int k = 0; k < 3; k++) {
                const float specCol = 255.0f * (1 - metal) + c[k] * metal;   // metals tint their highlight
                outc[k] = c[k] * shade * ao + specCol * spec;   // no environment to reflect: metals keep their diffuse light, they only tint the highlight
                if (emi) outc[k] += texel(emi, u, v)[0] / 255.0f * sf->emiCol[k] * sf->emiInt * 255.0f;
            }
            zb = z; uint8_t* o = &buf[((size_t)py * W + px) * 4];
            o[0] = (uint8_t)std::min(255.0f, outc[0]); o[1] = (uint8_t)std::min(255.0f, outc[1]); o[2] = (uint8_t)std::min(255.0f, outc[2]); o[3] = 255;
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
// browser requests: last time the tile asked (every frame while visible); 0 = sticky (Refresh). The worker takes the newest
// and drops the ones not asked for in 1.5 s, so after scrolling through hundreds of tiles the ones on screen come first.
static std::unordered_map<std::string, DWORD> g_reqSeen;
static void RequestSticky(const std::string& p);
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
    g_readError = false;
    std::vector<Node> roots; if (!(EndsWith(logical, ".app_xml") ? LoadAppearanceRoots(logical, roots, &why) : LoadPrefabRoots(logical, roots, &why))) return false;
    std::vector<Inst> inst; const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, Z[3] = { 0, 0, 0 };
    CollectCtx cx; cx.chain.push_back(logical);
    for (const auto& r : roots) Collect(r, I, Z, inst, cx);
    if (g_readError) { why = "read error"; return false; }   // a sub-prefab could not be read
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
            if (mats && si < m->mats.size()) { auto mi = mats->find(m->mats[si]);
                if (mi == mats->end()) { std::string lk = m->mats[si]; for (char& ch : lk) ch = (char)tolower((unsigned char)ch); mi = mats->find(lk); }   // .pac_xml names are lower case
                if (mi == mats->end() && mats->size() == 1) mi = mats->begin();   // one material in the file: it is this submesh's
                if (mi != mats->end()) { const MatInfo& mt = mi->second; g_statMat++; memcpy(sfc.col, mt.rc, sizeof sfc.col); sfc.uvScale = mt.uvScale; sfc.skip = mt.decal;
                    if (!sfc.skip) {
                        if (!mt.tex.empty()) { sfc.tex = LoadTexture(mt.tex); if (sfc.tex) g_statTex++; }
                        if (g_quality >= 1) {   // dye: characters colour the mask's zones, props multiply their base colour with one tint
                            sfc.hasTint = mt.hasTint; memcpy(sfc.tint, mt.tint, sizeof sfc.tint);
                            if (mt.hasZones && !mt.mask.empty() && !sfc.tex) { sfc.hasZones = true; memcpy(sfc.zone, mt.zone, sizeof sfc.zone); sfc.mask = LoadTexture(mt.mask); if (!mt.overlay.empty()) sfc.overlay = LoadTexture(mt.overlay); }
                        }
                        if (g_quality >= 2 && !mt.norm.empty()) sfc.norm = LoadTexture(mt.norm);
                        if (g_quality >= 3) { if (!mt.spec.empty()) sfc.spec = LoadTexture(mt.spec); if (!mt.emi.empty()) { sfc.emi = LoadTexture(mt.emi); memcpy(sfc.emiCol, mt.emiCol, sizeof sfc.emiCol); sfc.emiInt = mt.emiInt; } }
                    } } }
            g_statSurf++;
            if (surf.size() >= 0xFFFE) return 0; surf.push_back(sfc); return subSurf[si] = (uint16_t)(surf.size() - 1);
        };
        const bool mFm = m->fm.size() == m->f.size() / 3;
        for (size_t t = 0; t < m->f.size() / 3; t++) { const uint16_t si = surfaceFor(mFm ? m->fm[t] : 0); if (si < surf.size() && surf[si].skip) continue;
            all.f.push_back(m->f[t * 3] + base); all.f.push_back(m->f[t * 3 + 1] + base); all.f.push_back(m->f[t * 3 + 2] + base); fs.push_back(si); }
    }
    if (g_readError) { why = "read error"; return false; }   // a mesh, material or texture could not be read: no incomplete image
    if (all.v.empty() || all.f.empty()) { why = "no geometry"; return false; }
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < all.v.size(); i += 3) for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], all.v[i + k]); mx[k] = std::max(mx[k], all.v[i + k]); }
    for (int k = 0; k < 3; k++) { dims[k] = mx[k] - mn[k]; dims[3 + k] = (mn[k] + mx[k]) * 0.5f; }   // size + center relative to the pivot
    if (g_measureOnly && GetFileAttributesA(core::ThumbFile(logical).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (g_recheckOnly && !g_recheckForce && !anyCompressed && GetFileAttributesA(core::ThumbFile(logical).c_str()) != INVALID_FILE_ATTRIBUTES) { g_lastSkipped = true; return true; }
    // characters face the other way than static objects: a preview made only of skinned meshes is seen from the front
    const bool skinnedOnly = std::all_of(inst.begin(), inst.end(), [](const Inst& in) { return EndsWith(in.path, ".pac"); });
    const bool decalOnly = std::all_of(inst.begin(), inst.end(), [](const Inst& in) { return in.path.compare(0, 6, "decal:") == 0; });   // flat: seen from above
    if (!RenderPng(all, fs, surf, core::ThumbFile(logical), skinnedOnly ? 215.0f : 35.0f, decalOnly ? 70.0f : 25.0f)) { why = "render"; return false; }
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
        // up to v0.86 an unfilled read from the game loader was recorded as "no meshes" for good (thousands of prefabs when the
        // pass ran during a loading screen): every recorded failure is tried once more, the real ones are recorded again
        // marker 2: skinned meshes (.pac) and sub-prefabs render since 0.91, their old "no meshes" lines are tried again as well
        const std::string retryPath = core::ModDir() + "\\thumbs_retry.txt"; int retryVer = 0;
        if (FILE* rf = fopen(retryPath.c_str(), "r")) { if (fscanf(rf, "%d", &retryVer) != 1) retryVer = 0; fclose(rf); }
        if (!failedSet.empty() && retryVer < 1) {
            for (const auto& p : failedSet) g_processed.erase(p);
            Log("[thumbs] %zu prefabs without a preview are tried again (earlier failures may have been read errors)", failedSet.size());
            failedSet.clear();
        } else if (!failedSet.empty() && retryVer < 3) {   // marker 3: decals render since 0.93
            size_t n = 0;
            for (const auto& pi : core::PrefabIndex()) {
                const bool again = (retryVer < 2 && (pi.tags.find("SkinnedMesh") != std::string::npos || pi.tags.find("SubPrefab") != std::string::npos)) || pi.tags.find("Decal") != std::string::npos;
                if (again && failedSet.erase(pi.path)) { g_processed.erase(pi.path); n++; }
            }
            if (n) Log("[thumbs] %zu prefabs that earlier versions could not preview are rendered now", n);
        }
        if (FILE* rf = fopen(retryPath.c_str(), "w")) { fprintf(rf, "3\n"); fclose(rf); }
        g_failed = (int)failedSet.size(); g_done = (int)g_processed.size() - g_failed;
    }
    g_sizes = fopen(sizesPath.c_str(), "a");
    const auto& idx = core::PrefabIndex();
    {   // unique paths: the packs hold 32 prefabs twice (with and without bin__), older prefabs.tsv list them twice, and the
        // progress then stopped 32 short of the total forever
        std::unordered_set<std::string> u; for (const auto& pi : idx) u.insert(pi.path); g_total = (int)u.size();
    }
    int noMesh = 0;   // index rows without any mesh (never a preview), for the log lines
    {   // prefabs.tsv counts the .pam/.pami _path entries: 0 without a SkinnedMesh or SubPrefab tag (the .pac and the meshes of
        // referenced prefabs are not counted there) means
        // "no meshes" without reading the file. ~1.6k of them (decals, effects, logic objects) would otherwise be read one by
        // one. A parse-error row is read anyway.
        std::lock_guard<std::mutex> l(g_mu); int skipped = 0;
        for (const auto& pi : idx) if (pi.meshes == 0 && pi.tags.find("SkinnedMesh") == std::string::npos && pi.tags.find("SubPrefab") == std::string::npos && pi.tags.find("Decal") == std::string::npos && pi.tags.find("Appearance") == std::string::npos && pi.tags.find("parse-error") == std::string::npos) { noMesh++; if (g_processed.insert(pi.path).second) skipped++; }
        g_failed += skipped;
        Log("[thumbs] %d prefabs have no mesh in the index (decals, effects, logic objects): no preview, not read", noMesh);
    }
    Log("[thumbs] worker ready: %d prefabs, %d with preview, %d without (%d no mesh), %d to go", g_total.load(), g_done.load(), g_failed.load(), noMesh,
        std::max(0, g_total.load() - g_done.load() - g_failed.load()));
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
    const int kCacheVersion = 4;   // 2 = LZ4 meshes decoded, 3 = textured previews, 4 = .pam uv read as halves (every textured image was off)
    // progress file: one prefab per line that this pass already rendered. Without it every session started the pass from
    // the beginning and re-rendered the same first few thousand prefabs, so the rest never got their textured image.
    const std::string passPath = core::ModDir() + "\\thumbs_pass.txt"; FILE* passOut = nullptr;
    if (cacheVer < kCacheVersion) { std::lock_guard<std::mutex> l(g_mu);
        { FILE* pf = fopen(passPath.c_str(), "r"); if (pf) { char line[1024]; while (fgets(line, sizeof line, pf)) { std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) g_passDone.insert(s); } fclose(pf); } }
        for (auto& pi : idx) if (g_processed.count(pi.path) && pi.sx > 0 && !g_passDone.count(pi.path)) recheck.push_back(pi.path);
        g_recheckForce = cacheVer < 4; g_passActive = !recheck.empty();
        if (!g_passDone.empty()) Log("[thumbs] re-render pass continues: %zu done in earlier sessions, %zu to go", g_passDone.size(), recheck.size());
        if (!recheck.empty()) passOut = fopen(passPath.c_str(), "a");
        if (!recheck.empty()) Log("[thumbs] cache version %d -> %d: %zu rendered prefabs are %s in the background", cacheVer, kCacheVersion, recheck.size(), g_recheckForce ? "rendered again with textures" : "checked for LZ4 meshes"); }
    auto writeVersion = [&]() { FILE* vf = fopen(verPath.c_str(), "w"); if (vf) { fprintf(vf, "%d\n", kCacheVersion); fclose(vf); } if (passOut) { fclose(passOut); passOut = nullptr; } DeleteFileA(passPath.c_str()); };
    if (recheck.empty()) writeVersion();
    std::deque<std::string> remeasure;   // rendered before the center columns existed: measure again without rendering
    for (auto& pi : idx) if (pi.sx > 0 && !pi.hasCenter) remeasure.push_back(pi.path);
    if (!remeasure.empty()) Log("[thumbs] %zu cached entries lack the bounding box center, measuring them again (no rendering)", remeasure.size());
    std::deque<std::string> retry;   // background prefabs that hit a read error, tried again once the pass reaches the end
    std::unordered_map<std::string, int> retries; int errStreak = 0, streaksLogged = 0;
    for (;;) {
        std::string path; bool prio = false; bool measure = false, check = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (!g_requests.empty()) {   // newest request first; tiles that scrolled away (not asked for in 1.5 s) leave the queue
                const DWORD now = GetTickCount(); size_t best = SIZE_MAX; DWORD bestT = 0;
                for (size_t i = 0; i < g_requests.size();) {
                    auto it = g_reqSeen.find(g_requests[i]); const DWORD t = it == g_reqSeen.end() ? now : it->second;
                    if (t && now - t > 1500) { g_pending.erase(g_requests[i]); if (it != g_reqSeen.end()) g_reqSeen.erase(it); g_requests.erase(g_requests.begin() + i); continue; }
                    const DWORD key = t ? t : now + 1;   // sticky requests rank above everything
                    if (best == SIZE_MAX || key >= bestT) { best = i; bestT = key; }
                    i++;
                }
                if (best != SIZE_MAX) { path = g_requests[best]; g_requests.erase(g_requests.begin() + best); g_reqSeen.erase(path); prio = true; }
            }
            if (!path.empty()) {}
            else if (!remeasure.empty()) { path = remeasure.front(); remeasure.pop_front(); measure = true; }
            else if (!recheck.empty()) { path = recheck.front(); recheck.pop_front(); check = true; }
            else if (g_background) { while (cursor < idx.size() && g_processed.count(idx[cursor].path)) cursor++; if (cursor < idx.size()) path = idx[cursor++].path;
                                     else if (!retry.empty()) { path = retry.front(); retry.pop_front(); } }
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
        if (!ok && why == "read error") {   // the loader is busy (loading screen, fast travel): nothing is recorded, the prefab comes back after a pause
            reasons[why]++; const DWORD pause = std::min<DWORD>(10000, 500u << std::min(errStreak, 5));
            if (errStreak == 0 && streaksLogged < 20) { streaksLogged++; Log("[thumbs] %s: read error from the game loader, pausing the preview worker", path.c_str()); }
            errStreak++;
            { std::lock_guard<std::mutex> l(g_mu); g_pending.erase(path); const bool again = ++retries[path] <= 3;   // an entry that never reads is left for the next session
              if (!again) {} else if (measure) remeasure.push_back(path); else if (check) recheck.push_back(path); else if (!prio) retry.push_back(path); }
            Sleep(pause); continue;   // a browser request comes back on its own, the tile asks again every frame
        }
        if (errStreak) { if (streaksLogged <= 20) Log("[thumbs] game loader reads work again after %d failed renders", errStreak); errStreak = 0; }
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
        { std::lock_guard<std::mutex> l(g_mu); g_processed.insert(path); g_pending.erase(path); if (ok) g_refreshed.push_back(core::ThumbFile(path)); }   // Refresh() of a shown image: the overlay drops its copy
        if (ok) { g_done++; g_gen++; sessionDone++; } else g_failed++;
        if (prio) Log("[thumbs] %s: %s (instances %d, surfaces %d, with material %d, with texture %d)", path.c_str(), ok ? "rendered" : why.c_str(), g_statInst, g_statSurf, g_statMat, g_statTex);
        if (GetTickCount() - lastLog > 60000) {
            std::string rs; for (auto& kv : reasons) rs += kv.first + "=" + std::to_string(kv.second) + " ";
            // worded for players who read the log: "failed" alone looked like errors piling up
            Log("[thumbs] progress: %d with preview, %d without (%d no mesh), %d to go, %d rendered this session | reads %d %s", g_done.load(), g_failed.load(), noMesh,
                std::max(0, g_total.load() - g_done.load() - g_failed.load()), sessionDone, g_reads.load(), rs.c_str());
            lastLog = GetTickCount();
        }
        if (!prio) Sleep(20);   // background pass: roughly half duty cycle, the game keeps its cores
    }
}
void SetBackground(bool on) { g_background = on; }
void SetQuality(int q) { g_quality = q < 0 ? 0 : q > 3 ? 3 : q; }   // read by the worker per surface, a plain int is enough
int Quality() { return g_quality; }
bool Background() { return g_background; }

void Start() { CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr); }
void Refresh(const std::string& p) {   // render again (e.g. an old prefab_size.tsv line without the center columns)
    { std::lock_guard<std::mutex> l(g_mu); g_processed.erase(p); }
    RequestSticky(p);
}
void Request(const std::string& p) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_pending.count(p)) { auto it = g_reqSeen.find(p); if (it != g_reqSeen.end() && it->second) it->second = GetTickCount(); return; }   // still visible: keep it fresh
    if (g_processed.count(p) && !(g_passActive && !g_passDone.count(p))) return;   // during a re-render pass a processed prefab may still be queued once
    g_pending.insert(p); g_requests.push_back(p); g_reqSeen[p] = GetTickCount();
}
static void RequestSticky(const std::string& p) {   // one-shot callers (Refresh): never expires
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_pending.count(p)) { g_pending.insert(p); g_requests.push_back(p); }
    g_reqSeen[p] = 0;
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
