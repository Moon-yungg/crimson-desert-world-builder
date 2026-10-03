// Which level ("scene") the player is in, by name: the game's level name table (fieldlevelnametableinfo, row rootlevel:
// every level with its box, Framework docs/formats/levels.md) read once through the game's own loader, the player
// position, and the .palevel files the resource loader was asked for (phase variants share a box: the one loaded most
// recently is the one in the world). No game structures are touched; a table layout this parser does not know leaves
// the feature off with a message.
#include "core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace core {

namespace {

struct LevelBox {
    std::string name;   // as the game writes it (mixed case)
    std::string lower;  // file stem
    bool sector = false;
    float mn[3], mx[3];
};

std::mutex g_mx;
std::vector<LevelBox> g_levels;
std::map<std::string, size_t> g_byLower;  // file stem -> g_levels index
std::atomic<int> g_state{0};  // 0 not read, 1 reading, 2 ready, 3 failed
std::string g_error;
std::map<std::string, uint64_t> g_loaded;  // level file stem -> load time (ms)
std::map<std::string, std::string> g_paths; // level file stem -> pack path as requested

uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// fieldlevelnametableinfo body: rows {u32 key, string _stringKey, u8, string, u32 n, n x {u32 hash, u32 hash, string name,
// u8 _isSectorLevel, float3 min, float3 max}}; strings are u32 length + bytes
bool Parse(const std::vector<uint8_t>& d, std::vector<LevelBox>& out, std::string& err) {
    size_t p = 0;
    auto u32 = [&](uint32_t& v) { if (p + 4 > d.size()) return false; memcpy(&v, d.data() + p, 4); p += 4; return true; };
    auto str = [&](std::string& s) { uint32_t n; if (!u32(n) || n > 4096 || p + n > d.size()) return false; s.assign((const char*)d.data() + p, n); p += n; return true; };
    while (p < d.size()) {
        uint32_t key, n; std::string sk, fl;
        if (!u32(key) || !str(sk) || p >= d.size()) { err = "row header"; return false; }
        p += 1;
        if (!str(fl) || !u32(n) || n > 200000) { err = "row header"; return false; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a, b; LevelBox lb;
            if (!u32(a) || !u32(b) || !str(lb.name) || p + 25 > d.size()) { err = "entry"; return false; }
            lb.sector = d[p] != 0; p += 1;
            memcpy(lb.mn, d.data() + p, 12); memcpy(lb.mx, d.data() + p + 12, 12); p += 24;
            if (sk != "rootlevel") continue;
            lb.lower = Lower(lb.name);
            const size_t slash = lb.lower.rfind('/');
            if (slash != std::string::npos) lb.lower = lb.lower.substr(slash + 1);
            out.push_back(std::move(lb));
        }
    }
    if (out.size() < 1000) { err = "only " + std::to_string(out.size()) + " levels"; return false; }
    return true;
}

void ReadTable() {
    std::vector<uint8_t> d; bool notFound = false;
    std::vector<LevelBox> levels; std::string err;
    const char* path = "gamedata/binarystaticinfo__/bin/fieldlevelnametableinfo.staticinfobody";
    if (!GameReadFile(path, d, &notFound)) err = notFound ? "level name table not in the packs" : "level name table unreadable";
    else if (!Parse(d, levels, err)) err = "level name table layout unknown (" + err + ")";
    std::lock_guard<std::mutex> l(g_mx);
    if (!err.empty()) { g_error = err; g_state = 3; Log("[scene] %s: current scene names off", err.c_str()); return; }
    g_levels = std::move(levels);
    for (size_t i = 0; i < g_levels.size(); i++) g_byLower.emplace(g_levels[i].lower, i);
    g_state = 2;
    Log("[scene] level name table: %zu levels", g_levels.size());
}

void EnsureTable() {
    int expected = 0;
    if (g_state.load() != 0 || !GameReadAvailable()) return;
    if (g_state.compare_exchange_strong(expected, 1)) std::thread(ReadTable).detach();
}

std::string Stem(const std::string& path) {
    std::string s = Lower(path);
    for (auto& c : s) if (c == '\\') c = '/';
    const size_t slash = s.rfind('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.size() > 8 && s.compare(s.size() - 8, 8, ".palevel") == 0) s.resize(s.size() - 8);
    return s;
}

}  // namespace

void SceneInfoOnResLoad(const std::string& path) {
    if (path.size() < 9 || path.compare(path.size() - 8, 8, ".palevel") != 0) return;
    const std::string s = Stem(path);
    std::lock_guard<std::mutex> l(g_mx);
    if (g_loaded.size() > 30000) g_loaded.clear();
    g_loaded[s] = NowMs();
    g_paths[s] = Lower(path);
}

SceneInfo CurrentScene(const Vec3& pos) {
    SceneInfo r;
    EnsureTable();
    const int st = g_state.load();
    if (st == 3) { std::lock_guard<std::mutex> l(g_mx); r.error = g_error; return r; }
    if (st != 2) { r.error = "reading the level table"; return r; }
    std::lock_guard<std::mutex> l(g_mx);
    // levels whose box holds the player; world-size layers (spawn / trigger lists, the root) and the proxy cluster nodes
    // (no level file, y = +-FLT_MAX) are no scene
    struct Hit { const LevelBox* lb; float area; uint64_t loaded; };
    std::vector<Hit> hits;
    for (const auto& lb : g_levels) {
        if (pos.x < lb.mn[0] || pos.x > lb.mx[0] || pos.z < lb.mn[2] || pos.z > lb.mx[2]) continue;
        if (pos.y < lb.mn[1] - 50 || pos.y > lb.mx[1] + 50 || lb.mx[1] - lb.mn[1] > 1e6f) continue;
        const float w = lb.mx[0] - lb.mn[0], dz = lb.mx[2] - lb.mn[2];
        if (w > 1200 || dz > 1200) continue;
        auto it = g_loaded.find(lb.lower);
        hits.push_back({&lb, w * dz, it == g_loaded.end() ? 0 : it->second});
    }
    // phase variants share a box: of equal boxes only the one loaded last is in the world (none loaded: keep all)
    std::vector<Hit> keep;
    for (const auto& h : hits) {
        bool shadowed = false;
        for (const auto& o : hits)
            if (&o != &h && o.loaded > h.loaded && std::abs(o.area - h.area) < 1.0f && !memcmp(o.lb->mn, h.lb->mn, 12)) shadowed = true;
        if (!shadowed) keep.push_back(h);
    }
    std::sort(keep.begin(), keep.end(), [](const Hit& a, const Hit& b) { return a.area < b.area; });
    const uint64_t now = NowMs();
    for (const auto& h : keep) {
        SceneLevel s;
        s.name = h.lb->name;
        s.sector = h.lb->sector;
        auto p = g_paths.find(h.lb->lower);
        s.path = p != g_paths.end() ? p->second
                 : std::string("leveldata/bin__/rootlevel/") + (h.lb->sector ? "sectorlevel/" : "") + h.lb->lower + ".palevel";
        s.loadedAgo = h.loaded ? (double)(now - h.loaded) / 1000.0 : -1.0;
        r.levels.push_back(std::move(s));
    }
    // the scene: the smallest level loaded in this session (else the smallest), a named area level preferred for the label
    for (const auto& s : r.levels) if (s.loadedAgo >= 0) { r.current = s; break; }
    if (r.current.name.empty() && !r.levels.empty()) r.current = r.levels.front();
    // the area: of the named levels that are places (not an effect / spawn / road / trigger / shop layer) and loaded
    // (phase twins: the loaded variant), the one whose level without its quadtree cell (_sub_A_B, _indoor_A_B,
    // _extra_A_B) is the largest; smaller than 128 m it is a group of objects (a quest prop layer), no area
    static const char* kLayers[] = { "fx_", "levelsequencerspawn", "levelactionpoint", "roadlevel", "gameplaytrigger",
                                     "regioninfo", "shop_", "levelgimmick", "levelspawn" };
    auto place = [](const SceneLevel& s) {
        if (s.sector || s.name.rfind("sector_", 0) == 0) return false;
        const std::string low = Lower(s.name);
        for (const char* k : kLayers) if (low.rfind(k, 0) == 0) return false;
        return true;
    };
    auto root = [](std::string n) {
        for (const char* tag : { "_sub_", "_indoor_", "_extra_" }) {
            const size_t at = Lower(n).find(tag);
            if (at != std::string::npos) { n.resize(at); break; }
        }
        return n;
    };
    float best = 0;
    for (const auto& s : r.levels) {
        if (!place(s) || s.loadedAgo < 0) continue;
        const std::string a = root(s.name);
        auto it = g_byLower.find(Lower(a));
        if (it == g_byLower.end()) continue;
        // a place is cut into quadtree cells of its own (Calphade_After_Area_01_sub_1_0 ...); object and phase layers
        // (Calphade_0001_Phase00_01) are not, however large their box
        const std::string cell = it->first + "_sub_";
        auto c = g_byLower.lower_bound(cell);
        if (c == g_byLower.end() || c->first.compare(0, cell.size(), cell) != 0) continue;
        const LevelBox& b = g_levels[it->second];
        const float extent = std::max(b.mx[0] - b.mn[0], b.mx[2] - b.mn[2]);
        if (extent < 128 || extent > 1200 || extent <= best) continue;
        best = extent;
        r.area = b.name;
    }
    if (!r.area.empty()) r.areaPath = "leveldata/bin__/rootlevel/" + Lower(r.area) + ".palevel";
    r.sectorX = (int)std::floor(pos.x / 256.0f);
    r.sectorZ = (int)std::floor(pos.z / 256.0f);
    r.sectorPath = "leveldata/bin__/rootlevel/sectorlevel/sector_" + std::to_string(r.sectorX) + "_" + std::to_string(r.sectorZ) + ".palevel";
    r.ok = true;
    return r;
}

}  // namespace core
