#include "proj_codec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <io.h>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace proj_codec {
namespace {

bool bad(std::string& e, size_t n, const char* why) { e = "record " + std::to_string(n) + ": " + why; return false; }

std::vector<std::string_view> split(std::string_view s, char c) {
    std::vector<std::string_view> v; size_t p = 0;
    for (;;) { size_t q = s.find(c, p); v.push_back(s.substr(p, q == s.npos ? q : q - p)); if (q == s.npos) return v; p = q + 1; }
}

bool num(std::string_view s, double& d) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), d, std::chars_format::general);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::isfinite(d);
}

bool integer(std::string_view s, int& n, bool positive = false) {
    if (s.empty() || (positive && s[0] == '0')) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && (positive ? n > 0 : n >= 0);
}

bool unsignedValue(std::string_view s, uint32_t& value) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
bool textValid(const std::string& s) {
    return s.empty() || (s.find('\0') == std::string::npos && s.size() <= INT_MAX &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) != 0);
}
std::string hex(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out; out.reserve(s.size() * 2);
    for (unsigned char c : s) { out.push_back(digits[c >> 4]); out.push_back(digits[c & 15]); }
    return out;
}
bool unhex(std::string_view s, std::string& out) {
    if (s.size() % 2) return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string value; value.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int a = nibble(s[i]), b = nibble(s[i + 1]); if (a < 0 || b < 0) return false;
        value.push_back((char)((a << 4) | b));
    }
    if (!textValid(value)) return false;
    out = std::move(value); return true;
}

// Windows ordinal casefold for the extension-kind gate: exactly the policy core::FileNameEqual applies to
// library file admission (cdmodkit.cpp FileWide + CompareStringOrdinal with casefold). Only the comparison
// folds case; the caller's path text is never rewritten or lowercased. Mirrored here (not linked from core)
// so the isolated Codec suite keeps compiling this TU alone with no engine/registry surface.
bool extensionKindMatches(const std::string& path, std::string_view ext) {
    const size_t n = ext.size();
    if (path.size() < n || n == 0) return false;
    const auto wide = [](const char* s, size_t bytes) {
        const int count = MultiByteToWideChar(CP_ACP, 0, s, (int)bytes, nullptr, 0);
        std::wstring w(count > 0 ? (size_t)count : 0, L'\0');
        if (count > 0) MultiByteToWideChar(CP_ACP, 0, s, (int)bytes, &w[0], count);
        return w;
    };
    const std::wstring tail = wide(path.c_str() + path.size() - n, n);
    const std::wstring kind = wide(ext.data(), n);
    if (tail.empty() || kind.empty()) return path.compare(path.size() - n, n, ext.data(), n) == 0; // FileNameEqual's exact-bytes fallback
    return CompareStringOrdinal(tail.data(), (int)tail.size(), kind.data(), (int)kind.size(), TRUE) == CSTR_EQUAL;
}

bool terrainValid(const TerrainRecord& t) {
    if ((t.mode != 0 && t.mode != 1) || t.r <= 0) return false;
    for (double v : {t.x, t.z, t.r, t.amount, t.strength, t.ax, t.az, t.y})
        if (!std::isfinite(v) || std::abs(v) > (std::numeric_limits<float>::max)()) return false;
    return static_cast<float>(t.r) > 0;
}
bool terrainRow(std::string_view s, TerrainRecord& t) {
    const auto v = split(s, '|');
    if (v.size() < 7 || v.size() > 10 || !integer(v[1], t.mode)) return false;
    double* values[] = { &t.x, &t.z, &t.r, &t.amount, &t.strength, &t.ax, &t.az, &t.y };
    for (size_t i = 2; i < v.size(); ++i) if (!num(v[i], *values[i - 2])) return false;
    return terrainValid(t);
}
bool terrainDocumentValid(const Document& d, std::string& error) {
    if (d.kind != Kind::Project && !d.terrain.empty()) return bad(error, 0, "terrain is project-only");
    for (const auto& t : d.terrain) if (!terrainValid(t)) return bad(error, 0, "invalid terrain stroke");
    return true;
}

bool finitePoint(const Point& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

bool npcValid(const NpcRecord& n) {
    return n.key != 0 && finitePoint(n.pos) && n.type >= 1 && n.type <= 255 &&
        n.behavior >= 0 && n.behavior <= 1 && n.group >= 0 && textValid(n.label) && textValid(n.note);
}
bool npcRow(std::string_view s, NpcRecord& n) {
    const auto v = split(s, '|'); int ai = 0;
    if (v.size() < 10 || v.size() > 12 || !unsignedValue(v[1], n.key) ||
        !num(v[2], n.pos.x) || !num(v[3], n.pos.y) || !num(v[4], n.pos.z) ||
        !integer(v[5], n.type) || !unsignedValue(v[6], n.extra) || !integer(v[7], ai) || ai > 1 ||
        !integer(v[8], n.behavior) || !integer(v[9], n.group)) return false;
    n.aiEnabled = ai != 0;
    return (v.size() < 11 || unhex(v[10], n.label)) && (v.size() < 12 || unhex(v[11], n.note)) && npcValid(n);
}

bool boundsValid(const Bounds& b) {
    return finitePoint(b.anchor) && finitePoint(b.min) && finitePoint(b.max) &&
        b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z &&
        b.anchor.x >= b.min.x && b.anchor.x <= b.max.x &&
        b.anchor.y >= b.min.y && b.anchor.y <= b.max.y &&
        b.anchor.z >= b.min.z && b.anchor.z <= b.max.z;
}

bool point(std::string_view s, Point& p) {
    auto v = split(s, ',');
    return v.size() == 3 && num(v[0], p.x) && num(v[1], p.y) && num(v[2], p.z);
}

bool bounds(std::string_view s, Bounds& b) {
    auto v = split(s, ' ');
    const char* keys[] = {"anchor=", "min=", "max=", "quality="};
    if (v.size() != 4) return false;
    for (int i = 0; i < 4; ++i) { std::string_view k(keys[i]); if (v[i].substr(0, k.size()) != k) return false; v[i].remove_prefix(k.size()); }
    b.approximate = v[3] == "approx";
    return (v[3] == "approx" || v[3] == "measured") && point(v[0], b.anchor) && point(v[1], b.min) && point(v[2], b.max) && boundsValid(b);
}

bool row(std::string_view s, int width, Record& r) {
    auto v = split(s, '|');
    if (static_cast<int>(v.size()) != width || v[0].empty() || v[0][0] == '#' || v[0][0] == ' ' || v[0].find_first_of("\r\n\t") != v[0].npos) return false;
    r.prefab = std::string(v[0]);
    if (!num(v[1], r.pos.x) || !num(v[2], r.pos.y) || !num(v[3], r.pos.z)) return false;
    if (width >= 7 && (!num(v[4], r.yaw) || !num(v[5], r.scale) || r.scale <= 0 || !integer(v[6], r.group))) return false;
    if (width >= 9 && (!num(v[7], r.pitch) || !num(v[8], r.roll))) return false;
    return width != 10 || unhex(v[9], r.note);
}

bool rowValid(const Record& r) {
    return !r.prefab.empty() && r.prefab[0] != '#' && r.prefab[0] != ' ' &&
        r.prefab.find_first_of("|\r\n\t") == std::string::npos &&
        finitePoint(r.pos) && std::isfinite(r.yaw) && std::isfinite(r.pitch) && std::isfinite(r.roll) &&
        std::isfinite(r.scale) && r.scale > 0 && r.group >= 0 && textValid(r.note);
}

// Exactly the engine's tile derivation: (int)trunc((float)position * 0.001), stored
// as int16 (asi/cdmodkit/cdmodkit.cpp MakeTransform). The float narrowing happens
// first because that is the value the engine actually consumes.
bool tileOf(double v, int& tile, bool npc = false) {
    if (!std::isfinite(v)) return false;
    // NPC TransformSync writes use float multiplication; SceneObject MakeTransform uses double.
    const double t = std::trunc(npc ? static_cast<double>(static_cast<float>(v) * 0.001f) : static_cast<double>(static_cast<float>(v)) * 0.001);
    if (t < -32768.0 || t > 32767.0) return false;
    tile = static_cast<int>(t);
    return true;
}

std::string dec(double n) {
    if (n == 0) return "0";
    char b[64];
    auto r = std::to_chars(b, b + sizeof b, n, std::chars_format::general);
    return r.ec == std::errc{} ? std::string(b, r.ptr) : std::string();
}
std::string coords(const Point& p) { return dec(p.x) + "," + dec(p.y) + "," + dec(p.z); }
std::string boundsText(const Bounds& b) {
    return "anchor=" + coords(b.anchor) + " min=" + coords(b.min) + " max=" + coords(b.max) +
        " quality=" + (b.approximate ? "approx" : "measured");
}

} // namespace

bool Validate(const Document& d, std::string& error) {
    error.clear();
    if (!terrainDocumentValid(d, error)) return false;
    if (d.kind == Kind::Group && (!d.hasBounds || d.records.empty())) return bad(error, 0, "missing group bounds");
    if (d.kind == Kind::Group && !d.npcs.empty()) return bad(error, 0, "NPCs are project-only");
    std::set<int> usedGroups;
    for (const auto& r : d.records) if (r.group) usedGroups.insert(r.group);
    for (const auto& n : d.npcs) {
        if (!npcValid(n)) return bad(error, 0, "invalid NPC record");
        if (n.group) usedGroups.insert(n.group);
    }
    for (const auto& name : d.groupNames)
        if (name.first <= 0 || !usedGroups.count(name.first) || !textValid(name.second)) return bad(error, 0, "invalid named group");
    if (d.hasBounds && !boundsValid(d.bounds)) return bad(error, 0, "invalid document bounds");
    std::map<int, Bounds> byId;
    for (const auto& e : d.envelopes) {
        if (e.id <= 0 || !byId.emplace(e.id, e.bounds).second) return bad(error, 0, "duplicate envelope");
        if (!boundsValid(e.bounds)) return bad(error, 0, "invalid envelope");
    }
    if (d.hasBounds != !byId.empty()) return bad(error, 0, "incomplete metadata");
    for (size_t i = 0; i < d.records.size(); ++i) {
        const Record& r = d.records[i];
        // Legacy read-only exclusions are never emitted by the writer, so their
        // payload is not part of the persisted contract.
        if (r.state != Record::State::Placeable) continue;
        if (!rowValid(r) || (d.hasBounds ? byId.count(r.envelope) == 0 : r.envelope != 0)) return bad(error, i + 1, "invalid row");
    }
    return true;
}

bool Parse(const std::string& text, const std::string& path, Kind kind, Document& out, std::string& error) {
    error.clear();
    if (!extensionKindMatches(path, kind == Kind::Project ? ".cdproj" : ".cdgroup")) return bad(error, 0, "wrong extension for kind");
    Document d; d.kind = kind; bool header = false, modern = false, v4 = false, hasNote = false;
    int legacyWidth = 0, pending = 0; std::string pendingNote;
    std::map<int, int> members; std::set<int> ids; std::istringstream input(text); std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == line.npos) continue;
        if (!header) {
            header = true;
            if (line == "# cdproj v3 kind=project" || line == "# cdproj v3 kind=group") {
                modern = true; if ((line == "# cdproj v3 kind=project") != (kind == Kind::Project)) return bad(error, 0, "wrong kind"); continue;
            }
            if (kind == Kind::Group) return bad(error, 0, "group header required");
            if (line == "# cdmodkit project v4: object rows remain v3-compatible; optional note hex, named groups, managed NPCs and terrain strokes are comment records") { modern = v4 = true; continue; }
            if (line == "# cdmodkit project v3: prefab|x|y|z|yawDeg|scale|group|pitchDeg|rollDeg (absolute world coordinates)") { d.legacy = true; legacyWidth = 9; continue; }
            if (line == "# cdmodkit project v2: prefab|x|y|z|yawDeg|scale|group") { d.legacy = true; legacyWidth = 7; continue; }
            if (line[0] == '#') return bad(error, 0, "unsupported header");
            d.legacy = true;
        }
        // A header is not an ordinary legacy comment just because it appears later.
        // Reject reserved header families anywhere after the first nonblank line.
        if ((line.rfind("# cdproj", 0) == 0 && (line.size() == 8 || line[8] == ' ')) ||
            (line.rfind("# cdmodkit project", 0) == 0 && (line.size() == 18 || line[18] == ' ')))
            return bad(error, 0, "duplicate or conflicting header");
        if (line.rfind("#terrain", 0) == 0) {
            TerrainRecord t;
            if (kind != Kind::Project || line.rfind("#terrain|", 0) != 0 || !terrainRow(line, t)) return bad(error, 0, "invalid terrain stroke");
            d.terrain.push_back(t); continue; // no object ordinal or envelope is consumed
        }
        if (line.rfind("#npc", 0) == 0) {
            NpcRecord n;
            if (kind != Kind::Project || line.rfind("#npc|", 0) != 0 || !npcRow(line, n)) return bad(error, 0, "invalid NPC record");
            d.npcs.push_back(std::move(n)); continue;
        }
        if (line.rfind("#group", 0) == 0) {
            const auto v = split(line, '|'); int id = 0; std::string name;
            if (v.size() != 3 || v[0] != "#group" || !integer(v[1], id, true) || !unhex(v[2], name) ||
                !d.groupNames.emplace(id, std::move(name)).second) return bad(error, 0, "invalid or duplicate named group");
            continue;
        }
        if (line.rfind("# wb-note ", 0) == 0) {
            const auto v = split(std::string_view(line).substr(10), ' '); int ordinal = 0;
            if (!modern || hasNote || v.size() != 2 || v[0].substr(0, 7) != "record=" || v[1].substr(0, 4) != "hex=" ||
                !integer(v[0].substr(7), ordinal, true) || ordinal != (int)d.records.size() + 1 || !unhex(v[1].substr(4), pendingNote))
                return bad(error, 0, "invalid object note metadata");
            hasNote = true; continue;
        }
        // Known legacy state comments contain a full row; ordinary comments do not count as records.
        if (line.rfind("# wb-document ", 0) == 0) {
            if (!modern || d.hasBounds || !d.records.empty() || !bounds(std::string_view(line).substr(14), d.bounds)) return bad(error, 0, "invalid document bounds");
            d.hasBounds = true; continue;
        }
        if (line.rfind("# wb-envelope ", 0) == 0) {
            auto v = split(std::string_view(line).substr(14), ' '); Envelope e;
            if (!modern || !d.hasBounds || !d.records.empty() || v.size() != 5 || v[0].substr(0, 3) != "id=" ||
                !integer(v[0].substr(3), e.id, true) || !ids.insert(e.id).second ||
                !bounds(std::string_view(line).substr(15 + v[0].size()), e.bounds)) return bad(error, 0, "invalid envelope");
            d.envelopes.push_back(e); continue;
        }
        if (line.rfind("# wb-member ", 0) == 0) {
            auto v = split(std::string_view(line).substr(12), ' '); int ordinal = 0, id = 0;
            if (!modern || !d.hasBounds || v.size() != 2 || v[0].substr(0, 7) != "record=" || v[1].substr(0, 9) != "envelope=" ||
                !integer(v[0].substr(7), ordinal, true) || !integer(v[1].substr(9), id, true) || !ids.count(id) ||
                pending || ordinal != static_cast<int>(d.records.size() + 1) ||
                !members.emplace(ordinal, id).second) return bad(error, 0, "invalid member");
            pending = id; continue;
        }
        Record r; std::string_view payload = line;
        if (line.rfind("# hidden", 0) == 0 || line.rfind("# missing", 0) == 0) {
            if (modern) return bad(error, d.records.size() + 1, "state wrapper in new document");
            if (line.rfind("# hidden ", 0) == 0) { r.state = Record::State::Hidden; payload.remove_prefix(9); }
            else if (line.rfind("# missing ", 0) == 0) { r.state = Record::State::Missing; payload.remove_prefix(10); }
            else return bad(error, d.records.size() + 1, "malformed state wrapper");
        } else if (line[0] == '#') {
            // Ordinary comments stay comments in modern documents: ignored without
            // advancing record ordinals or clearing a pending member. Reserved wb-*
            // metadata families are never ignored.
            if (modern && line.rfind("# wb-", 0) == 0) return bad(error, 0, "unknown metadata");
            continue;
        }
        int width = v4 ? static_cast<int>(split(payload, '|').size()) : modern ? 9 : legacyWidth ? legacyWidth : static_cast<int>(split(payload, '|').size());
        if ((width != 4 && width != 7 && width != 9 && !(v4 && width == 10)) || (v4 && width < 9) || !row(payload, width, r)) return bad(error, d.records.size() + 1, "invalid row or arity");
        if (d.hasBounds && !pending) return bad(error, 0, "missing member before row");
        if (hasNote) { if (width == 10) return bad(error, d.records.size() + 1, "duplicate object note"); r.note = std::move(pendingNote); hasNote = false; }
        d.records.push_back(std::move(r)); pending = 0;
    }
    if (!header) return bad(error, 0, "missing header or records");
    if (pending || hasNote) return bad(error, 0, "dangling object metadata");
    if (modern && !v4 && !d.records.empty() && !d.hasBounds) return bad(error, 0, "missing document metadata");
    if (kind == Kind::Group && (!d.hasBounds || d.envelopes.empty() || d.records.empty())) return bad(error, 0, "incomplete group metadata");
    if (d.hasBounds || !d.envelopes.empty() || !members.empty()) {
        if (!d.hasBounds || d.envelopes.empty() || members.size() != d.records.size()) return bad(error, 0, "incomplete mapping");
        std::map<int, int> seen;
        for (size_t i = 0; i < d.records.size(); ++i) {
            auto m = members.find(static_cast<int>(i + 1)); if (m == members.end()) return bad(error, 0, "missing member");
            auto inserted = seen.emplace(m->second, static_cast<int>(seen.size() + 1));
            if (inserted.first->second != m->second) return bad(error, 0, "envelope first appearance order");
            d.records[i].envelope = m->second;
        }
        if (seen.size() != d.envelopes.size()) return bad(error, 0, "unused envelope");
        for (size_t i = 0; i < d.envelopes.size(); ++i) if (d.envelopes[i].id != static_cast<int>(i + 1)) return bad(error, 0, "nonsequential envelope");
    }
    if (!Validate(d, error)) return false;
    out = std::move(d); return true;
}

bool SameValues(const Document& a, const Document& b) {
    auto pointSame = [](const Point& x, const Point& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    auto boundsSame = [&](const Bounds& x, const Bounds& y) { return x.approximate == y.approximate && pointSame(x.anchor, y.anchor) && pointSame(x.min, y.min) && pointSame(x.max, y.max); };
    if (a.kind != b.kind || a.hasBounds != b.hasBounds || (a.hasBounds && !boundsSame(a.bounds, b.bounds)) ||
        a.records.size() != b.records.size() || a.envelopes.size() != b.envelopes.size() ||
        a.npcs.size() != b.npcs.size() || a.terrain.size() != b.terrain.size() || a.groupNames != b.groupNames) return false;
    for (size_t i = 0; i < a.records.size(); ++i) {
        const auto& x = a.records[i]; const auto& y = b.records[i];
        if (x.prefab != y.prefab || !pointSame(x.pos, y.pos) || x.yaw != y.yaw || x.pitch != y.pitch || x.roll != y.roll ||
            x.scale != y.scale || x.group != y.group || x.state != y.state || x.envelope != y.envelope || x.note != y.note) return false;
    }
    for (size_t i = 0; i < a.envelopes.size(); ++i)
        if (a.envelopes[i].id != b.envelopes[i].id || !boundsSame(a.envelopes[i].bounds, b.envelopes[i].bounds)) return false;
    for (size_t i = 0; i < a.npcs.size(); ++i) {
        const auto& x = a.npcs[i]; const auto& y = b.npcs[i];
        if (x.key != y.key || !pointSame(x.pos, y.pos) || x.type != y.type || x.extra != y.extra || x.aiEnabled != y.aiEnabled ||
            x.behavior != y.behavior || x.group != y.group || x.label != y.label || x.note != y.note) return false;
    }
    for (size_t i = 0; i < a.terrain.size(); ++i) {
        const auto& x = a.terrain[i]; const auto& y = b.terrain[i];
        if (x.mode != y.mode || x.x != y.x || x.z != y.z || x.r != y.r || x.amount != y.amount || x.strength != y.strength ||
            x.ax != y.ax || x.az != y.az || x.y != y.y) return false;
    }
    return true;
}

bool Serialize(const Document& d, std::string& text, std::string& error) {
    error.clear();
    if (!Validate(d, error)) return false;
    std::map<int, Bounds> byId;
    for (const auto& e : d.envelopes) byId.emplace(e.id, e.bounds);
    std::map<int, int> groups, env;
    for (size_t i = 0; i < d.records.size(); ++i) {
        const auto& r = d.records[i];
        if (r.state != Record::State::Placeable) continue;
        if (r.group) groups.emplace(r.group, static_cast<int>(groups.size() + 1));
        if (d.hasBounds) env.emplace(r.envelope, static_cast<int>(env.size() + 1));
    }
    for (const auto& n : d.npcs) if (n.group) groups.emplace(n.group, static_cast<int>(groups.size() + 1));
    // Build the exact semantic snapshot the text must reproduce, including the shared object/NPC remap.
    Document expected = d; expected.legacy = false; expected.records.clear(); expected.envelopes.clear(); expected.groupNames.clear();
    for (const auto& r : d.records) if (r.state == Record::State::Placeable) {
        Record next = r; next.group = r.group ? groups.at(r.group) : 0; next.envelope = d.hasBounds ? env.at(r.envelope) : 0;
        expected.records.push_back(std::move(next));
    }
    for (auto& n : expected.npcs) if (n.group) n.group = groups.at(n.group);
    for (const auto& name : d.groupNames) if (groups.count(name.first)) expected.groupNames.emplace(groups.at(name.first), name.second);
    for (size_t id = 1; id <= env.size(); ++id)
        for (const auto& e : env) if (e.second == (int)id) expected.envelopes.push_back({ (int)id, byId.at(e.first) });
    if (expected.records.empty()) { expected.hasBounds = false; expected.bounds = {}; }
    std::string s = d.kind == Kind::Group ? "# cdproj v3 kind=group\n" :
        "# cdmodkit project v4: object rows remain v3-compatible; optional note hex, named groups, managed NPCs and terrain strokes are comment records\n";
    for (const auto& name : expected.groupNames) s += "#group|" + std::to_string(name.first) + "|" + hex(name.second) + "\n";
    if (expected.hasBounds) {
        s += "# wb-document " + boundsText(d.bounds) + "\n";
        for (size_t i = 1; i <= env.size(); ++i)
            for (const auto& e : env)
                if (e.second == static_cast<int>(i)) s += "# wb-envelope id=" + std::to_string(i) + " " + boundsText(byId.at(e.first)) + "\n";
    }
    size_t written = 0;
    for (size_t i = 0; i < d.records.size(); ++i) {
        const auto& r = d.records[i];
        if (r.state != Record::State::Placeable) continue;
        ++written;
        if (d.hasBounds) s += "# wb-member record=" + std::to_string(written) + " envelope=" + std::to_string(env.at(r.envelope)) + "\n";
        if (d.kind == Kind::Group && !r.note.empty()) s += "# wb-note record=" + std::to_string(written) + " hex=" + hex(r.note) + "\n";
        s += r.prefab + "|" + dec(r.pos.x) + "|" + dec(r.pos.y) + "|" + dec(r.pos.z) + "|" + dec(r.yaw) + "|" + dec(r.scale) + "|" +
            std::to_string(r.group ? groups.at(r.group) : 0) + "|" + dec(r.pitch) + "|" + dec(r.roll) +
            (d.kind == Kind::Project ? "|" + hex(r.note) : std::string()) + "\n";
    }
    for (const auto& n : expected.npcs)
        s += "#npc|" + std::to_string(n.key) + "|" + dec(n.pos.x) + "|" + dec(n.pos.y) + "|" + dec(n.pos.z) + "|" +
            std::to_string(n.type) + "|" + std::to_string(n.extra) + "|" + (n.aiEnabled ? "1" : "0") + "|" +
            std::to_string(n.behavior) + "|" + std::to_string(n.group) + "|" + hex(n.label) + "|" + hex(n.note) + "\n";
    for (const auto& t : d.terrain)
        s += "#terrain|" + std::to_string(t.mode) + "|" + dec(t.x) + "|" + dec(t.z) + "|" + dec(t.r) + "|" +
            dec(t.amount) + "|" + dec(t.strength) + "|" + dec(t.ax) + "|" + dec(t.az) + "|" + dec(t.y) + "\n";
    Document check;
    if (!Parse(s, d.kind == Kind::Group ? "out.cdgroup" : "out.cdproj", d.kind, check, error)) return false;
    if (!SameValues(expected, check)) return bad(error, 0, "serialization changed semantic values");
    text = std::move(s);
    return true;
}

bool NarrowForEngine(const Document& document, std::vector<EngineRow>& out, std::string& error) {
    error.clear();
    if (!terrainDocumentValid(document, error)) return false;
    if (document.kind == Kind::Group && !document.npcs.empty()) return bad(error, 0, "NPCs are project-only");
    for (const auto& n : document.npcs) {
        if (!npcValid(n)) return bad(error, 0, "invalid NPC record");
        for (double v : { n.pos.x, n.pos.y, n.pos.z })
            if (std::abs(v) > (std::numeric_limits<float>::max)()) return bad(error, 0, "NPC engine float overflow");
        int tx = 0, tz = 0;
        if (!tileOf(n.pos.x, tx, true) || !tileOf(n.pos.z, tz, true)) return bad(error, 0, "NPC engine tile overflow");
    }
    std::vector<EngineRow> rows;
    rows.reserve(document.records.size());
    for (size_t i = 0; i < document.records.size(); ++i) {
        const Record& r = document.records[i];
        EngineRow e;
        if (r.state != Record::State::Placeable) { rows.push_back(e); continue; }
        for (double v : {r.pos.x, r.pos.y, r.pos.z, r.yaw, r.pitch, r.roll, r.scale}) {
            if (!std::isfinite(v) || std::abs(v) > (std::numeric_limits<float>::max)()) return bad(error, i + 1, "engine float overflow");
        }
        const float scale = static_cast<float>(r.scale);
        if (!(scale > 0) || !std::isfinite(scale)) return bad(error, i + 1, "engine scale underflow");
        e.placeable = true;
        e.x = static_cast<float>(r.pos.x);
        e.y = static_cast<float>(r.pos.y);
        e.z = static_cast<float>(r.pos.z);
        e.yaw = static_cast<float>(r.yaw);
        e.pitch = static_cast<float>(r.pitch);
        e.roll = static_cast<float>(r.roll);
        e.scale = scale;
        e.group = r.group;
        if (!tileOf(r.pos.x, e.tileX) || !tileOf(r.pos.z, e.tileZ)) return bad(error, i + 1, "engine tile overflow");
        rows.push_back(e);
    }
    out = std::move(rows);
    return true;
}

bool WriteTransactional(const std::string& path, Kind kind, const std::string& text,
                        const WriteHooks* hooks, std::string& error) {
    error.clear();
    if (path.empty()) return bad(error, 0, "empty destination path");
    if (!extensionKindMatches(path, kind == Kind::Group ? ".cdgroup" : ".cdproj"))
        return bad(error, 0, "wrong extension for kind");
    // The text must already be a valid document of this kind: nothing is created for
    // garbage, and the readback below re-checks the actual bytes.
    Document expectedDocument;
    if (!Parse(text, path, kind, expectedDocument, error)) return false;
    const size_t slash = path.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".\\") : path.substr(0, slash + 1);
    char temp[MAX_PATH] = {0};
    if (!GetTempFileNameA(dir.c_str(), "wbv", 0, temp)) return bad(error, 0, "cannot create temporary file");
    const std::string tempPath(temp);
    bool ok = true;
    auto failAt = [&](const char* why) { if (error.empty()) error = std::string("record 0: ") + why; ok = false; };
    auto stage = [&](WriteStage s, const char* name, size_t& length) {
        if (!ok) return false;
        if (hooks && hooks->fault && !hooks->fault(s, tempPath, length, error)) {
            if (error.empty()) error = std::string("record 0: ") + name + " aborted";
            ok = false;
            return false;
        }
        return true;
    };
    FILE* file = nullptr;
    if (fopen_s(&file, temp, "wb") != 0) file = nullptr;
    if (!file) failAt("cannot open temporary file");
    size_t length = text.size();
    if (ok && stage(WriteStage::Write, "write", length)) {
        if (length > text.size()) failAt("short write");
        else if (fwrite(text.data(), 1, length, file) != length) failAt("short write");
    }
    if (ok && stage(WriteStage::Flush, "flush", length)) {
        if (fflush(file) != 0 || _commit(_fileno(file)) != 0) failAt("flush failed");
    }
    if (file) {
        if (ok && stage(WriteStage::Close, "close", length)) {
            if (fclose(file) != 0) failAt("close failed");
        } else {
            fclose(file);
        }
    }
    if (ok && stage(WriteStage::Readback, "readback", length)) {
        std::ifstream in(temp, std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Document check;
        if (in.bad() || actual != text) failAt("readback mismatch");
        else if (!Parse(actual, kind == Kind::Group ? "temporary.cdgroup" : "temporary.cdproj", kind, check, error)) ok = false;
        else if (!SameValues(expectedDocument, check)) failAt("semantic readback mismatch");
    }
    if (ok && stage(WriteStage::Replace, "replace", length)) {
        if (hooks && hooks->beforeReplace && !hooks->beforeReplace(error)) {
            if (error.empty()) error = "record 0: replace refused";
            ok = false;
        } else if (hooks && hooks->replace) {
            // The caller performs the real replacement (its own authorities held across it).
            if (!hooks->replace(tempPath, path, error)) {
                if (error.empty()) error = "record 0: protected replace refused";
                ok = false;
            }
        } else if (MoveFileExA(temp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            failAt("replace failed");
        }
    }
    if (!ok) DeleteFileA(temp);
    return ok;
}

} // namespace proj_codec
