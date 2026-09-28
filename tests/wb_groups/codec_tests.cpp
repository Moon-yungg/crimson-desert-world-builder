// Codec suite: the production proj_codec TU is the unit under test. The fixture exercises the disk grammar
// (legacy defaults, full modern metadata roundtrip, malformed rejection with record ordinals, engine
// narrowing) and the transactional write primitive (short write, flush, close, readback and replace
// failures all leave the previous file byte-identical). There is no scene/registry/engine surface here:
// the codec is pure disk values, so every assertion below is a value comparison, never a prose match.
#include "../../asi/cdmodkit/proj_codec.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace proj_codec;

static int assertions = 0;
static std::vector<std::string> failureLog;

struct CaseRec { std::string id; int assertions = 0; int failures = 0; };
static std::vector<CaseRec> cases;
static std::string currentId;
static int caseStart = 0;
static size_t caseFailStart = 0;

static void Check(bool ok, const char* what) {
    ++assertions;
    if (!ok) {
        failureLog.push_back(currentId + ": " + what);
        std::fprintf(stderr, "FAIL [%s] %s\n", currentId.c_str(), what);
    }
}
static void BeginCase(const char* id) { currentId = id; caseStart = assertions; caseFailStart = failureLog.size(); }
static void EndCase() { cases.push_back({currentId, assertions - caseStart, (int)(failureLog.size() - caseFailStart)}); }

static const std::string kHeader = "# cdproj v3 kind=project\n"; // retained WB input dialect
static const std::string kV097Header = "# cdmodkit project v4: object rows remain v3-compatible; optional note hex, named groups, managed NPCs and terrain strokes are comment records\n";
static const std::string kGroupHeader = "# cdproj v3 kind=group\n";
static const std::string kLegacy9Header = "# cdmodkit project v3: prefab|x|y|z|yawDeg|scale|group|pitchDeg|rollDeg (absolute world coordinates)\n";
static const std::string kLegacy7Header = "# cdmodkit project v2: prefab|x|y|z|yawDeg|scale|group\n";
static const std::string kRow = "/object/airship.prefab|0.00049|2|-3|37|1.7|42|19|-8\n";
static const std::string kDocMeta = "# wb-document anchor=0,0,0 min=-1,-1,-1 max=1,1,1 quality=measured\n";
static const std::string kEnvelope1 = "# wb-envelope id=1 anchor=0,0,0 min=-1,-1,-1 max=1,1,1 quality=measured\n";
static const std::string kEnvelope2 = "# wb-envelope id=2 anchor=6,0,0 min=5,0,-1 max=7,2,1 quality=approx\n";

static Record Sentinel() {
    Record r; r.prefab = "sentinel"; r.pos = {9, 9, 9}; r.group = 12345; r.yaw = -1; r.scale = 3; r.pitch = -2; r.roll = -3; return r;
}

static void Bad(const std::string& data, const std::string& path, Kind kind, const std::string& ordinal, const char* what) {
    Document d; d.records.push_back(Sentinel()); d.records[0].note = "object-sentinel";
    NpcRecord npc; npc.key = 17; npc.group = 12345; npc.label = "npc-sentinel"; npc.note = "npc-note";
    d.npcs.push_back(npc); d.groupNames[12345] = "group-sentinel";
    d.terrain.push_back({1, 2, 3, 4, 5, .5, 6, 7, 8});
    std::string err;
    const bool accepted = Parse(data, path, kind, d, err);
    Check(!accepted, what);
    Check(err.rfind("record " + ordinal + ":", 0) == 0, "the rejection names the data-record ordinal");
    Check(d.records.size() == 1 && d.records[0].prefab == "sentinel" && d.records[0].group == 12345,
          "a failed parse leaves the caller's document untouched");
    Check(d.records.size() == 1 && d.records[0].note == "object-sentinel" && d.npcs.size() == 1 &&
          d.npcs[0].key == 17 && d.npcs[0].label == "npc-sentinel" && d.npcs[0].note == "npc-note" &&
          d.groupNames.size() == 1 && d.groupNames.at(12345) == "group-sentinel" &&
          d.terrain.size() == 1 && d.terrain[0].y == 8,
          "failed parse preserves every appended v0.97 value family");
}

static Document FullGroupDocument() {
    Document out; out.kind = Kind::Group; out.hasBounds = true;
    out.bounds = {{0, 0, 0}, {-2, 0, -2}, {2, 4, 2}, false};
    out.envelopes = {{1, {{0, 0, 0}, {-2, 0, -2}, {2, 4, 2}, false}},
                     {2, {{6, 0, 0}, {5, 0, -1}, {7, 2, 1}, true}}};
    for (int i = 0; i < 5; ++i) {
        Record r; r.prefab = "/object/a.prefab"; r.pos.x = 0.00049; r.pos.y = i;
        r.group = (i == 3 ? 0 : i == 1 || i == 4 ? 7 : 42); r.envelope = i == 1 || i == 4 ? 2 : 1;
        r.yaw = 37; r.pitch = 19; r.roll = -8; out.records.push_back(r);
    }
    return out;
}

static int CountOf(const std::string& text, const std::string& needle) {
    int n = 0; size_t p = 0;
    while ((p = text.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
    return n;
}
static bool WriteFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static int FileCount(const std::string& dir) {
    int n = 0;
    for (auto it = std::filesystem::directory_iterator(dir); it != std::filesystem::directory_iterator(); ++it) ++n;
    return n;
}
static void SaveInput(const std::string& dir, const char* name, const std::string& content, const char* what) {
    if (dir.empty()) { Check(false, "fixture directory argument missing"); return; }
    std::filesystem::create_directories(dir);
    const std::filesystem::path p = std::filesystem::path(dir) / name;
    Check(WriteFile(p.string(), content) && std::filesystem::exists(p), what);
}

static void RunCases(const std::string& dir) {
    std::string err;

    // -------------------------------------------------------------------------------------------------
    // Legacy nine/seven/four-field input keeps working, with per-row optional-field defaults.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-LEGACY");
    {
        Document d;
        const std::string legacy9 = kLegacy9Header + kRow;
        SaveInput(dir, "legacy-v086.cdproj", legacy9, "legacy nine-field input written");
        Check(Parse(legacy9, "old.cdproj", Kind::Project, d, err), "legacy v3 header and nine-field row parse");
        Check(d.legacy && d.kind == Kind::Project && !d.hasBounds && d.envelopes.empty() && d.records.size() == 1,
              "a legacy document carries no copy metadata");
        Check(!d.records.empty(), "the legacy row produced a record");
        if (!d.records.empty()) {
            const Record& r = d.records[0];
            Check(r.prefab == "/object/airship.prefab" && r.pos.x == 0.00049 && r.pos.y == 2 && r.pos.z == -3 &&
                  r.yaw == 37 && r.scale == 1.7 && r.group == 42 && r.pitch == 19 && r.roll == -8,
                  "all nine fields survive the legacy grammar exactly");
            Check(r.envelope == 0 && r.state == Record::State::Placeable, "a legacy row has no envelope and is placeable");
        }
    }
    {
        Document d;
        Check(Parse("/object/a.prefab|1|2|3\n", "old.cdproj", Kind::Project, d, err), "an unheadered four-field row parses");
        Check(!d.records.empty(), "the four-field row produced a record");
        if (!d.records.empty()) {
            const Record& r = d.records[0];
            Check(d.legacy && r.yaw == 0 && r.pitch == 0 && r.roll == 0 && r.scale == 1 && r.group == 0 && r.envelope == 0,
                  "omitted legacy fields take their defaults (yaw/pitch/roll=0, scale=1, group=0)");
        }
        Check(d.records.size() == 1 && !d.hasBounds, "the four-field row is one record without metadata");
    }
    {
        Document d;
        Check(Parse(kLegacy7Header + "/object/a.prefab|1|2|3|4|1|7\n", "old.cdproj", Kind::Project, d, err),
              "a v2 seven-field header document parses");
        Check(d.records.size() == 1 && d.records[0].group == 7 && d.records[0].yaw == 4 && d.records[0].pitch == 0 &&
              d.records[0].roll == 0 && d.records[0].scale == 1, "the v2 group is kept and tilt defaults apply");
    }
    {
        Document d;
        const std::string mixed = "/object/a.prefab|1|2|3\n"
                                  "/object/b.prefab|4|5|6|7|2|9\n"
                                  "/object/c.prefab|8|9|10|11|1|3|12|13\n";
        Check(Parse(mixed, "old.cdproj", Kind::Project, d, err), "an unheadered legacy document mixes four/seven/nine-field rows");
        Check(d.records.size() == 3, "every mixed-arity row is a record");
        Check(d.records[0].scale == 1 && d.records[0].yaw == 0 && d.records[0].group == 0 &&
              d.records[1].pitch == 0 && d.records[1].roll == 0 && d.records[1].scale == 2 && d.records[1].group == 9 &&
              d.records[2].scale == 1 && d.records[2].pitch == 12 && d.records[2].roll == 13,
              "each row takes only its own omitted defaults");
    }
    {
        Document d;
        const std::string crlf = kLegacy9Header + "/object/a.prefab|1|2|3|4|1|7|8|9\r\n";
        Check(Parse(crlf, "old.cdproj", Kind::Project, d, err) && d.records.size() == 1 &&
              d.records[0].pitch == 8 && d.records[0].roll == 9, "CRLF line endings parse like LF");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Full modern metadata: document bounds, per-copy envelopes, member relations, byte-stable output.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-ROUNDTRIP");
    {
        Document full = FullGroupDocument();
        std::string bytes;
        Check(Serialize(full, bytes, err), "a full-metadata group serializes");
        SaveInput(dir, "full-group.cdgroup", bytes, "serialized group written as fixture input");
        Check(CountOf(bytes, "# wb-document ") == 1 && CountOf(bytes, "# wb-envelope ") == 2 &&
              CountOf(bytes, "# wb-member record=") == 5,
              "document, per-copy and per-record metadata are all emitted");
        Document parsed;
        Check(Parse(bytes, "Groups/test.cdgroup", Kind::Group, parsed, err), "the serialized group re-parses");
        Check(parsed.kind == Kind::Group && !parsed.legacy && parsed.hasBounds && parsed.records.size() == 5,
              "the re-parsed document is a modern five-record group");
        Check(!parsed.records.empty() && parsed.records.size() == 5, "every member record survived the roundtrip");
        if (parsed.records.size() == 5) {
        Check(parsed.bounds.anchor.x == 0 && parsed.bounds.anchor.y == 0 && parsed.bounds.anchor.z == 0 &&
              parsed.bounds.min.x == -2 && parsed.bounds.min.z == -2 && parsed.bounds.max.y == 4 &&
              !parsed.bounds.approximate,
              "the document anchor/min/max/quality round-trip exactly");
        Check(parsed.envelopes.size() == 2 && parsed.envelopes[0].id == 1 && parsed.envelopes[1].id == 2 &&
              parsed.envelopes[1].bounds.anchor.x == 6 && parsed.envelopes[1].bounds.min.y == 0 &&
              parsed.envelopes[1].bounds.max.z == 1 && parsed.envelopes[1].bounds.approximate,
              "each copy envelope keeps its own anchor/min/max/approximate");
        Check(parsed.records[0].pos.x == 0.00049 && parsed.records[0].yaw == 37 && parsed.records[0].pitch == 19 &&
              parsed.records[0].roll == -8 && parsed.records[0].scale == 1,
              "pose values survive the double-valued grammar exactly");
        Check(parsed.records[0].group == 1 && parsed.records[1].group == 2 && parsed.records[2].group == 1 &&
              parsed.records[3].group == 0 && parsed.records[4].group == 2,
              "partitions canonicalize by first appearance and group zero stays zero");
        Check(parsed.records[0].envelope == 1 && parsed.records[1].envelope == 2 && parsed.records[2].envelope == 1 &&
              parsed.records[3].envelope == 1 && parsed.records[4].envelope == 2,
              "member-envelope relations survive the roundtrip");
        }
        std::string again;
        Check(Serialize(parsed, again, err) && again == bytes, "write-read-write is byte-stable");
        Document project = full; project.kind = Kind::Project;
        std::string pbytes;
        Check(Serialize(project, pbytes, err) && pbytes.rfind(kV097Header, 0) == 0,
              "the project flavor writes its own kind header");
        Document pback;
        Check(Parse(pbytes, "x.cdproj", Kind::Project, pback, err) && pback.kind == Kind::Project &&
              pback.records.size() == 5 && pback.envelopes.size() == 2,
              "a project with copy metadata round-trips through v4 rows");
        Check(pback.records[1].group == 2 && pback.records[3].group == 0 && pback.bounds.max.y == 4,
              "project partitions and document bounds are identical to the group flavor");
        Document reordered = full;
        for (auto& r : reordered.records) r.envelope = (r.envelope == 1 ? 9 : 3);
        reordered.envelopes[0].id = 9; reordered.envelopes[1].id = 3;
        std::string encoded;
        Check(Serialize(reordered, encoded, err) && encoded == bytes,
              "envelope ids canonicalize independent of the source numbering");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // A malformed final row is rejected at its own ordinal; a bad final record is never ignored.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-BAD-LAST-ROW");
    {
        Document full = FullGroupDocument();
        std::string bytes;
        Check(Serialize(full, bytes, err), "base group for the malformed-last-row case");
        {
            std::string corrupt = bytes;
            const size_t rowAt = corrupt.rfind("/object/a.prefab|");
            const size_t barAt = rowAt == std::string::npos ? std::string::npos : corrupt.find('|', rowAt);
            Check(rowAt != std::string::npos && barAt != std::string::npos, "the last row is present to corrupt");
            if (rowAt != std::string::npos && barAt != std::string::npos) {
                corrupt.insert(barAt + 1, "bad");
                SaveInput(dir, "malformed-last.cdgroup", corrupt, "malformed input written as fixture input");
                Bad(corrupt, "x.cdgroup", Kind::Group, "5", "a malformed final row is rejected");
            }
        }
        {
            std::string trunc = bytes;
            const std::string member5 = "# wb-member record=5 envelope=2\n";
            const size_t last = trunc.rfind(member5);
            Check(last != std::string::npos, "the last member line is present to drop");
            if (last != std::string::npos) {
                trunc.erase(last, member5.size());
                Bad(trunc, "x.cdgroup", Kind::Group, "0", "a row without its final member record is rejected");
            }
        }
        {
            std::string extra = bytes;
            extra += "/object/extra.prefab|1|2\n";
            Bad(extra, "x.cdgroup", Kind::Group, "6", "an appended legacy-arity final row in a modern document is rejected");
        }
        {
            Document d;
            const std::string trailing = bytes + "# trailing ordinary comment\n\n";
            Check(Parse(trailing, "x.cdgroup", Kind::Group, d, err) && d.records.size() == 5,
                  "a trailing ordinary comment is not a record and does not change the record count");
        }
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Header family, kind and extension rules.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-HEADER");
    {
        Document d;
        Bad("# cdproj v4 kind=project\n" + kDocMeta + kRow, "x.cdproj", Kind::Project, "0", "an unsupported header version is rejected");
        Bad(kLegacy9Header + kRow + "# cdproj v3 kind=project\n", "x.cdproj", Kind::Project, "0", "a modern header after records is rejected");
        Bad(kLegacy9Header + kLegacy9Header + kRow, "x.cdproj", Kind::Project, "0", "a duplicate legacy header is rejected");
        Bad(kLegacy9Header + kRow + kLegacy7Header, "x.cdproj", Kind::Project, "0", "a conflicting legacy header family is rejected");
        Bad(kHeader + kDocMeta + "# wb-future kind=widget\n" + kRow, "x.cdproj", Kind::Project, "0", "an unknown reserved metadata family is rejected");
        Bad(kHeader + "# wb-document nonsense\n" + kRow, "x.cdproj", Kind::Project, "0", "malformed document metadata is rejected");
        Bad(kLegacy9Header + kDocMeta + kRow, "x.cdproj", Kind::Project, "0", "modern metadata inside a legacy document is rejected");
        Bad(kHeader + kRow, "x.cdgroup", Kind::Project, "0", "a project kind with a group extension is rejected");
        Bad("# cdproj v3 kind=group\n" + kRow, "x.cdproj", Kind::Project, "0", "a group header in a project file is rejected");
        Bad(kHeader + kRow, "x.cdproj", Kind::Group, "0", "a project header for a group kind is rejected");
        Bad(kGroupHeader, "x.cdgroup", Kind::Group, "0", "an empty group document is rejected");
        Check(Parse(kHeader, "empty.cdproj", Kind::Project, d, err) && d.records.empty() && !d.hasBounds && !d.legacy,
              "an empty modern project document is valid");
        Bad("# cdproj v3 kind=project extra\n" + kDocMeta + kRow, "x.cdproj", Kind::Project, "0", "trailing header text is rejected");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Envelope and member relationship rules.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-ENVELOPE");
    {
        Document full = FullGroupDocument();
        std::string bytes;
        Check(Serialize(full, bytes, err), "base group for the envelope cases");
        {
            std::string c = bytes;
            const std::string member1 = "# wb-member record=1 envelope=1";
            const size_t p = c.find(member1);
            Check(p != std::string::npos, "the first member line exists");
            if (p != std::string::npos) {
                c.replace(p, member1.size(), "# wb-member record=1 envelope=3");
                Bad(c, "x.cdgroup", Kind::Group, "0", "a member referencing an unknown envelope is rejected");
            }
        }
        {
            std::string c = bytes;
            const std::string member2 = "# wb-member record=2 envelope=2\n";
            const size_t p = c.find(member2);
            Check(p != std::string::npos, "the second member line exists");
            if (p != std::string::npos) {
                c.erase(p, member2.size());
                Bad(c, "x.cdgroup", Kind::Group, "0", "a row without its member record is rejected");
            }
        }
        {
            std::string c = bytes;
            const std::string member2 = "# wb-member record=2 envelope=2";
            const size_t p = c.find(member2);
            Check(p != std::string::npos, "the second member line exists for the ordinal case");
            if (p != std::string::npos) {
                c.replace(p, member2.size(), "# wb-member record=3 envelope=2");
                Bad(c, "x.cdgroup", Kind::Group, "0", "a member record ordinal out of order is rejected");
            }
        }
        {
            std::string c = bytes;
            const std::string env1 = "# wb-envelope id=1 ";
            const std::string env2 = "# wb-envelope id=2 ";
            const size_t p1 = c.find(env1);
            const size_t p2 = c.find(env2);
            Check(p1 != std::string::npos && p2 != std::string::npos && p1 < p2, "both envelope lines exist");
            if (p1 != std::string::npos && p2 != std::string::npos && p1 < p2) {
                c.replace(p1, 18, env2); c.replace(p2, 18, env1);
                Bad(c, "x.cdgroup", Kind::Group, "0", "envelopes out of first-appearance order are rejected");
            }
        }
        {
            std::string c = kGroupHeader + kDocMeta + kEnvelope1 + kEnvelope2 +
                "# wb-envelope id=3 anchor=0,0,0 min=-1,-1,-1 max=1,1,1 quality=measured\n" +
                "# wb-member record=1 envelope=1\n" + kRow +
                "# wb-member record=2 envelope=2\n" + kRow;
            Bad(c, "x.cdgroup", Kind::Group, "0", "an unreferenced envelope is rejected");
        }
        {
            const std::string c = kGroupHeader + kEnvelope1 + kDocMeta + kRow;
            Bad(c, "x.cdgroup", Kind::Group, "0", "an envelope declared before the document metadata is rejected");
        }
        {
            std::string c = bytes;
            const size_t env = c.find("# wb-envelope");
            const size_t p = env == std::string::npos ? std::string::npos : c.find("quality=measured", env);
            Check(p != std::string::npos, "an envelope quality field exists");
            if (p != std::string::npos) {
                c.replace(p, 16, "quality=untrusted");
                Bad(c, "x.cdgroup", Kind::Group, "0", "an unknown bounds quality is rejected");
            }
        }
        {
            const std::string c = kGroupHeader + "# wb-document anchor=0,0,0 min=2,0,0 max=1,1,1 quality=measured\n" + kRow;
            Bad(c, "x.cdgroup", Kind::Group, "0", "an anchor outside min/max is rejected");
        }
        {
            const std::string c = kLegacy9Header + kRow + "# wb-member record=1 envelope=1\n";
            Bad(c, "x.cdproj", Kind::Project, "0", "member metadata inside a legacy document is rejected");
        }
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Duplicate records: the same member, envelope or header must not be accepted twice.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-DUPLICATE");
    {
        Document full = FullGroupDocument();
        std::string bytes;
        Check(Serialize(full, bytes, err), "base group for the duplicate cases");
        {
            std::string c = bytes;
            const std::string member1 = "# wb-member record=1 envelope=1\n";
            const size_t p = c.find(member1);
            Check(p != std::string::npos, "the first member line exists for duplication");
            if (p != std::string::npos) {
                c.insert(p, member1);
                Bad(c, "x.cdgroup", Kind::Group, "0", "a duplicate member record is rejected");
            }
        }
        {
            std::string c = bytes;
            const size_t p = c.find("# wb-envelope id=1 ");
            Check(p != std::string::npos, "the first envelope line exists for duplication");
            if (p != std::string::npos) {
                c.insert(p, kEnvelope1);
                Bad(c, "x.cdgroup", Kind::Group, "0", "a duplicate envelope id is rejected");
            }
        }
        {
            std::string c = bytes;
            const size_t p = c.find("# wb-envelope id=2 ");
            Check(p != std::string::npos, "the second envelope line exists for duplication");
            if (p != std::string::npos) {
                c.insert(p, kEnvelope1);
                Bad(c, "x.cdgroup", Kind::Group, "0", "two envelopes with the same id are rejected");
            }
        }
        {
            std::string c = bytes;
            const size_t p = c.find("# wb-document ");
            Check(p != std::string::npos, "the document metadata line exists for duplication");
            if (p != std::string::npos) {
                c.insert(p, kGroupHeader);
                Bad(c, "x.cdgroup", Kind::Group, "0", "a duplicate modern header is rejected");
            }
        }
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Value grammar: non-finite/invalid numbers, scale and partition ranges, arity.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-NANINF");
    {
        const std::string members = kEnvelope1 + "# wb-member record=1 envelope=1\n";
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|nan|2|3|0|1|0|0|0\n", "x.cdproj", Kind::Project, "1", "a NaN coordinate is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|inf|3|0|1|0|0|0\n", "x.cdproj", Kind::Project, "1", "an infinite coordinate is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|1e999|0|1|0|0|0\n", "x.cdproj", Kind::Project, "1", "an overflowing coordinate is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|nan|1|0|0|0\n", "x.cdproj", Kind::Project, "1", "a NaN yaw is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|inf|0|0|0\n", "x.cdproj", Kind::Project, "1", "an infinite scale is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|0|-inf|0\n", "x.cdproj", Kind::Project, "1", "an infinite pitch is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|0|0|1e999\n", "x.cdproj", Kind::Project, "1", "an overflowing roll is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|12oops|2|3|0|1|0|0|0\n", "x.cdproj", Kind::Project, "1", "a malformed number is rejected");
        Bad(kLegacy9Header + kRow + "/object/a.prefab|1|2|bad|0|1|0|0|0\n", "x.cdproj", Kind::Project, "2", "a malformed final legacy row is rejected");
    }
    EndCase();

    BeginCase("CODEC-SCALE");
    {
        const std::string members = kEnvelope1 + "# wb-member record=1 envelope=1\n";
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|0|0|0|0\n", "x.cdproj", Kind::Project, "1", "a zero scale is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|-1.5|0|0|0\n", "x.cdproj", Kind::Project, "1", "a negative scale is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|-1|0|0\n", "x.cdproj", Kind::Project, "1", "a negative partition is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|2147483648|0|0\n", "x.cdproj", Kind::Project, "1", "an overflowing partition is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|0|0\n", "x.cdproj", Kind::Project, "1", "an eight-field modern row is rejected");
        Bad(kHeader + kDocMeta + members + "/object/a.prefab|1|2|3|0|1|0|0|0|4\n", "x.cdproj", Kind::Project, "1", "a ten-field modern row is rejected");
        Bad(kLegacy9Header + kRow + "/object/a.prefab|1|2|3|0|1|0|0\n", "x.cdproj", Kind::Project, "2", "a short row after a nine-field header is rejected");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Caller-built documents: full validation before any write, with record ordinals.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-VALIDATE");
    {
        Document good = FullGroupDocument();
        Check(Validate(good, err), "a full-metadata group passes validation");
        const size_t before = good.records.size();
        {
            Document d = good; d.records[0].prefab = "#hidden-style";
            Check(!Validate(d, err) && err.rfind("record 1:", 0) == 0, "an invalid prefab is rejected at its ordinal");
        }
        {
            Document d = good; d.records[2].pos.y = std::numeric_limits<double>::quiet_NaN();
            Check(!Validate(d, err) && err.rfind("record 3:", 0) == 0, "a non-finite pose is rejected at its ordinal");
        }
        {
            Document d = good; d.records[4].group = -4;
            Check(!Validate(d, err) && err.rfind("record 5:", 0) == 0, "a negative partition is rejected at its ordinal");
        }
        {
            Document d = good; d.records[0].envelope = 77;
            Check(!Validate(d, err) && err.rfind("record 1:", 0) == 0, "a missing envelope reference is rejected at its ordinal");
        }
        {
            Document d = good; d.records[0].scale = std::numeric_limits<double>::infinity();
            std::string text = "keep";
            Check(!Serialize(d, text, err) && text == "keep" && err.rfind("record 1:", 0) == 0,
                  "serialization rejects an overflowing row without touching the output");
        }
        {
            Document d = good;
            Record extra; extra.prefab = "/object/extra.prefab"; extra.envelope = 3;
            d.records.push_back(extra);
            d.envelopes.push_back({3, good.bounds});
            Check(Validate(d, err), "an envelope referenced by a later record is valid");
        }
        {
            Document d = good; d.envelopes.push_back({3, good.bounds});
            Check(Validate(d, err), "unused envelopes are allowed for the writer (they are dropped)");
        }
        {
            Document d = good; d.records.clear();
            Check(!Validate(d, err) && err.rfind("record 0:", 0) == 0, "a group without records is rejected");
        }
        {
            Document d = good; d.records[0].envelope = 0;
            Check(!Validate(d, err) && err.rfind("record 1:", 0) == 0, "a modern row without an envelope is rejected");
        }
        {
            Document d; d.kind = Kind::Project; d.records.push_back(Sentinel());
            d.envelopes.push_back({1, good.bounds}); // v4 permits ordinary rows, never a partial WB metadata set
            Check(!Validate(d, err) && err.rfind("record 0:", 0) == 0, "WB envelopes without document bounds are rejected");
            std::string text = "keep";
            Check(!Serialize(d, text, err) && text == "keep", "serialization of incomplete WB metadata fails transactionally");
        }
        Check(good.records.size() == before, "validation never mutates the document");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Engine boundary: full-document narrowing before any scene mutation.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-NARROW");
    {
        auto base = []() {
            Document d; d.kind = Kind::Project; d.hasBounds = true;
            d.bounds = {{0, 0, 0}, {-1, -1, -1}, {1, 1, 1}, false};
            d.envelopes = {{1, {{0, 0, 0}, {-1, -1, -1}, {1, 1, 1}, false}}};
            Record r; r.prefab = "/object/a.prefab"; r.envelope = 1; r.group = 7;
            r.pos = {32767500.0, 5.0, -32768500.0}; r.yaw = 37.5; r.pitch = -19.25; r.roll = 8.0; r.scale = 1.5;
            d.records.push_back(r);
            Record excluded = r; excluded.state = Record::State::Hidden;
            d.records.push_back(excluded);
            return d;
        };
        {
            Document d = base();
            std::vector<EngineRow> rows;
            std::string nerr;
            Check(NarrowForEngine(d, rows, nerr), "an in-range document narrows for the engine");
            Check(rows.size() == d.records.size(), "every record keeps its ordinal in the narrowed rows");
            if (rows.size() == 2) {
                Check(rows[0].placeable && rows[0].x == (float)32767500.0 && rows[0].y == 5.0f &&
                      rows[0].z == (float)(-32768500.0), "positions narrow to the engine floats");
                Check(rows[0].tileX == 32767 && rows[0].tileZ == -32768,
                      "the tile pair matches the engine derivation at the int16 boundary");
                Check(rows[0].yaw == 37.5f && rows[0].pitch == -19.25f && rows[0].roll == 8.0f && rows[0].scale == 1.5f,
                      "yaw/pitch/roll/scale narrow to floats");
                Check(rows[0].group == 7, "the partition survives narrowing unchanged");
                Check(!rows[1].placeable, "an excluded legacy row yields no engine values");
            }
        }
        {
            Document d = base(); d.records[0].pos.y = 33000000.0;
            std::vector<EngineRow> rows;
            std::string nerr;
            Check(NarrowForEngine(d, rows, nerr) && rows.size() == 2 && rows[0].y == (float)33000000.0,
                  "the tile bound applies to the tiled x/z positions only");
        }
        auto rejected = [&](double v, int field, const char* token, const char* what) {
            Document d = base();
            Record& r = d.records[0];
            switch (field) {
                case 0: r.pos.x = v; break;
                case 1: r.pos.y = v; break;
                case 2: r.pos.z = v; break;
                case 3: r.yaw = v; break;
                case 4: r.scale = v; break;
                default: r.pitch = v; break;
            }
            std::vector<EngineRow> rows;
            rows.push_back(EngineRow{});
            rows[0].x = -777.0f;
            std::string nerr;
            const bool ok = NarrowForEngine(d, rows, nerr);
            Check(!ok, what);
            Check(nerr.rfind("record 1:", 0) == 0 && nerr.find(token) != std::string::npos,
                  "the narrowing error names the actual engine boundary");
            Check(rows.size() == 1 && rows[0].x == -777.0f, "a failed narrowing leaves the output untouched");
        };
        rejected(1e39, 0, "float overflow", "a coordinate beyond the float range is rejected");
        rejected(1e39, 1, "float overflow", "a y coordinate beyond the float range is rejected");
        rejected(std::numeric_limits<double>::quiet_NaN(), 2, "float overflow", "a NaN coordinate is rejected before the engine sees it");
        rejected(1e39, 3, "float overflow", "a yaw beyond the float range is rejected");
        rejected(1e39, 4, "float overflow", "a scale beyond the float range is rejected");
        rejected(1e-47, 4, "scale underflow", "a scale that underflows to zero float is rejected");
        rejected(-1.5, 4, "scale underflow", "a negative scale is rejected at the engine boundary");
        rejected(32768500.0, 0, "tile overflow", "an x position beyond the int16 tile range is rejected");
        rejected(-32769500.0, 2, "tile overflow", "a z position below the int16 tile range is rejected");
        rejected(std::numeric_limits<double>::quiet_NaN(), 5, "float overflow", "a NaN pitch is rejected before the engine sees it");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Transactional write: temporary file, flush, readback and replace; the old file always survives.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-WRITE");
    {
        const std::string wdir = dir.empty() ? std::string() : (dir + "/codec-write");
        if (!wdir.empty()) std::filesystem::create_directories(wdir);
        Check(!wdir.empty() && std::filesystem::exists(wdir), "the write workspace exists");
        if (wdir.empty() || !std::filesystem::exists(wdir)) {
            Check(false, "the transactional write cases need a fixture workspace directory");
        } else {
        const std::string dest = wdir + "/group.cdgroup";
        const std::string oldBytes = "OLD FILE BYTES\n";
        Document full = FullGroupDocument();
        std::string bytes;
        Check(Serialize(full, bytes, err), "serialize the document for the write cases");
        Check(WriteFile(dest, oldBytes), "the previous version is in place");

        {
            std::string werr;
            Check(WriteTransactional(dest, Kind::Group, bytes, nullptr, werr), "a valid document replaces the destination");
            Check(ReadFile(dest) == bytes, "the destination holds the serialized document byte-for-byte");
            Check(FileCount(wdir) == 1, "no temporary file survives a successful replace");
        }
        {
            Document other = full; other.records[0].pos.y = 42;
            std::string otherBytes;
            Check(Serialize(other, otherBytes, err) && otherBytes != bytes, "a second valid document differs");
            std::string werr;
            Check(WriteTransactional(dest, Kind::Group, otherBytes, nullptr, werr) && ReadFile(dest) == otherBytes,
                  "an existing destination is replaced by the new document");
            Check(FileCount(wdir) == 1, "the second replace leaves no temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the short-write case");
            WriteHooks hooks;
            hooks.fault = [](WriteStage s, const std::string&, size_t& length, std::string&) {
                if (s == WriteStage::Write) length /= 2;
                return true;
            };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &hooks, werr), "a short write fails the transaction");
            Check(ReadFile(dest) == oldBytes, "a short write leaves the previous bytes untouched");
            Check(FileCount(wdir) == 1, "a short write removes only the owned temporary file");
            Check(!werr.empty() && werr.rfind("record 0:", 0) == 0, "the short-write failure carries a codec error");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the flush case");
            WriteHooks hooks;
            hooks.fault = [](WriteStage s, const std::string&, size_t&, std::string& e) {
                if (s == WriteStage::Flush) { e = "record 0: injected flush failure"; return false; }
                return true;
            };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &hooks, werr), "a flush failure aborts the transaction");
            Check(werr == "record 0: injected flush failure", "the injected flush failure is reported verbatim");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a flush failure preserves the previous bytes and removes the temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the close case");
            WriteHooks hooks;
            hooks.fault = [](WriteStage s, const std::string&, size_t&, std::string& e) {
                if (s == WriteStage::Close) { e = "record 0: injected close failure"; return false; }
                return true;
            };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &hooks, werr), "a close failure aborts the transaction");
            Check(werr == "record 0: injected close failure", "the injected close failure is reported verbatim");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a close failure preserves the previous bytes and removes the temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the readback case");
            WriteHooks hooks;
            hooks.fault = [](WriteStage s, const std::string& temp, size_t&, std::string&) {
                if (s == WriteStage::Readback) {
                    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
                    f << "corrupt-readback";
                }
                return true;
            };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &hooks, werr), "a corrupted readback aborts the transaction");
            Check(!werr.empty(), "a corrupted readback reports an error");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a corrupted readback preserves the previous bytes and removes the temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the replace case");
            WriteHooks hooks;
            hooks.fault = [](WriteStage s, const std::string&, size_t&, std::string& e) {
                if (s == WriteStage::Replace) { e = "record 0: injected replace failure"; return false; }
                return true;
            };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &hooks, werr), "a failed replace aborts the transaction");
            Check(werr == "record 0: injected replace failure", "the injected replace failure is reported verbatim");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a failed replace preserves the previous bytes and removes the temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the guard case");
            WriteHooks guard;
            guard.beforeReplace = [](std::string& e) { e = "record 0: injected guard refusal"; return false; };
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, bytes, &guard, werr), "a refused final guard aborts before replacement");
            Check(werr == "record 0: injected guard refusal", "the guard refusal is reported verbatim");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a refused guard preserves the previous bytes and removes the temporary file");
        }
        {
            Check(WriteFile(dest, oldBytes), "reset the destination before the pre-parse case");
            std::string werr;
            Check(!WriteTransactional(dest, Kind::Group, "not a document\n", nullptr, werr), "unparseable text is refused before any file is created");
            Check(ReadFile(dest) == oldBytes && FileCount(wdir) == 1, "a refused document creates no temporary file");
            Check(!WriteTransactional(wdir + "/group.cdproj", Kind::Group, bytes, nullptr, werr),
                  "a wrong extension for the kind is refused");
            Check(!WriteTransactional(wdir + "/missing-dir/group.cdgroup", Kind::Group, bytes, nullptr, werr),
                  "a missing destination directory is refused");
            Check(FileCount(wdir) == 1, "the refused destinations create no files in the workspace");
        }
        {
            std::string werr;
            Check(WriteTransactional(dest, Kind::Group, bytes, nullptr, werr) && ReadFile(dest) == bytes,
                  "a successful write still works after every injected failure");
            Check(FileCount(wdir) == 1, "the recovery write leaves no temporary file");
        }
        Check(true, "the transactional write cases ran");
        }
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Parse failure versus valid-but-excluded record: a legacy state wrapper is a successful parse.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-EXCLUSION");
    {
        Document d;
        const std::string wrapped = kLegacy9Header + kRow +
            "# ordinary comment\n"
            "# hidden /object/h.prefab|1|2|3|0|1|7|0|0\n"
            "\n"
            "# missing /object/m.prefab|4|5|6|0|1|7|0|0\n";
        Check(Parse(wrapped, "x.cdproj", Kind::Project, d, err), "valid hidden/missing wrappers parse successfully");
        Check(d.records.size() == 3 && d.records[0].state == Record::State::Placeable &&
              d.records[1].state == Record::State::Hidden && d.records[2].state == Record::State::Missing,
              "each legacy state is recorded on its own record");
        Check(d.records[1].prefab == "/object/h.prefab" && d.records[1].group == 7 &&
              d.records[2].prefab == "/object/m.prefab" && d.records[2].scale == 1,
              "an excluded legacy row keeps its values and defaults");
        Bad(kLegacy9Header + kRow + "# hidden\n", "x.cdproj", Kind::Project, "2", "a malformed state wrapper is a parse failure");
        Bad(kLegacy9Header + kRow + "# missing /object/a.prefab|1|2|bad|0|1|0|0\n", "x.cdproj", Kind::Project, "2",
            "a malformed excluded row is a parse failure");
        Bad(kHeader + kDocMeta + kEnvelope1 + "# wb-member record=1 envelope=1\n# hidden /object/a.prefab|1|2|3|0|1|0|0|0\n",
            "x.cdproj", Kind::Project, "1", "a legacy state wrapper inside a modern document is a parse failure");
        {
            Document full = FullGroupDocument();
            std::string baseline;
            Check(Serialize(full, baseline, err), "placeable-only baseline for the exclusion writer case");
            Record hidden = full.records[0]; hidden.state = Record::State::Hidden;
            Record missing = full.records[1]; missing.state = Record::State::Missing;
            full.records.insert(full.records.begin(), hidden);
            full.records.push_back(missing);
            full.envelopes.push_back({3, full.bounds});
            Record extra = full.records[0]; extra.state = Record::State::Hidden; extra.envelope = 3;
            full.records.push_back(extra);
            std::string emitted;
            Check(Serialize(full, emitted, err) && emitted == baseline,
                  "state wrappers are omitted and placeable ordinals are retained");
            Check(CountOf(emitted, "# hidden") == 0 && CountOf(emitted, "# wb-envelope ") == 2,
                  "an envelope used only by excluded records is not persisted");
        }
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Legacy and modern shapes in one session: comments never consume ordinals, arity is per row.
    // -------------------------------------------------------------------------------------------------
    BeginCase("CODEC-MIXED");
    {
        Document d;
        const std::string mixed = "/object/a.prefab|1|2|3\n"
                                  "# a legacy file for the archive\n"
                                  "# /object/commented.prefab|9|9|9\n"
                                  "\n"
                                  "/object/b.prefab|4|5|6|7|2|9\n"
                                  "/object/c.prefab|8|9|10|11|1|3|12|13\n";
        Check(Parse(mixed, "old.cdproj", Kind::Project, d, err) && d.records.size() == 3,
              "comments and blank lines do not become records in a mixed legacy document");
        if (d.records.size() == 3) {
            Check(d.records[0].prefab == "/object/a.prefab" && d.records[2].pitch == 12,
                  "legacy ordinals follow only real rows");
        }
        {
            const std::string c = kHeader + "# note before metadata\n" + kDocMeta + kEnvelope1 +
                "# note after metadata\n" + "# wb-member record=1 envelope=1\n" + kRow + "# trailing note\n";
            Document modern;
            Check(Parse(c, "x.cdproj", Kind::Project, modern, err) && modern.records.size() == 1 &&
                  modern.records[0].envelope == 1, "ordinary comments in a modern document shift no ordinals");
            std::string again;
            Check(Serialize(modern, again, err) && CountOf(again, "# note") == 0,
                  "ordinary comments are not persisted by the writer");
        }
        {
            const std::string c = kHeader + kDocMeta + kEnvelope1 + "# wb-member record=1 envelope=1\n" +
                "/object/legacy.prefab|1|2|3\n" + "# wb-member record=2 envelope=1\n/object/b.prefab|4|5|6|0|1|0|0|0\n";
            Bad(c, "x.cdproj", Kind::Project, "1", "a legacy four-field row inside a modern document is rejected");
        }
        {
            const std::string mixedDoc = kGroupHeader + kDocMeta + kEnvelope1 + kEnvelope2 +
                "# wb-member record=1 envelope=1\n/object/a.prefab|1|2|3|0|1|0|0|0\n"
                "# wb-member record=2 envelope=2\n/object/b.prefab|4|5|6|0|1|0|7|8\n";
            Document two;
            Check(Parse(mixedDoc, "x.cdgroup", Kind::Group, two, err) && two.records.size() == 2 &&
                  two.records[1].envelope == 2 && two.envelopes[1].bounds.approximate,
                  "a hand-written modern group with two copies parses");
        }
        {
            Document full = FullGroupDocument();
            std::string bytes;
            Check(Serialize(full, bytes, err), "base group for the mixed modern case");
            Bad(bytes + "# wb-note future=1\n", "x.cdgroup", Kind::Group, "0",
                "an unknown reserved metadata line after the records is rejected");
        }
    }
    EndCase();
}

#include "codec_v097_cases.h"

static void WriteEvidence(const std::string& dir) {
    if (dir.empty()) return;
    std::filesystem::create_directories(dir);
    std::ofstream log(std::filesystem::path(dir) / "codec.log", std::ios::binary | std::ios::trunc);
    for (const auto& c : cases) {
        log << "case " << c.id << ": " << (c.failures == 0 ? "PASS" : "FAIL")
            << " assertions=" << c.assertions << " failures=" << c.failures << "\n";
    }
    for (const auto& f : failureLog) log << "FAILURE " << f << "\n";
    std::string json = "{\"kind\":\"wb079-codec-cases\",\"schemaVersion\":1,\"assertions\":" + std::to_string(assertions) +
        ",\"failures\":" + std::to_string((int)failureLog.size()) + ",\"cases\":[";
    for (size_t i = 0; i < cases.size(); ++i) {
        if (i) json += ",";
        json += "{\"id\":\"" + cases[i].id + "\",\"assertions\":" + std::to_string(cases[i].assertions) +
            ",\"failures\":" + std::to_string(cases[i].failures) + ",\"status\":\"" +
            (cases[i].failures == 0 ? "PASS" : "FAIL") + "\"}";
    }
    json += "]}";
    std::ofstream f(std::filesystem::path(dir) / "codec.cases.json", std::ios::binary | std::ios::trunc);
    f << json;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "";
    try {
        RunCases(dir);
        RunV097Cases(dir);
    } catch (const std::exception& e) {
        Check(false, "unhandled fixture exception");
        std::fprintf(stderr, "EXCEPTION %s\n", e.what());
    }
    WriteEvidence(dir);
    std::printf("ASSERTIONS=%d\n", assertions);
    if (!failureLog.empty()) { std::printf("FAILURES=%d\n", (int)failureLog.size()); return 1; }
    return 0;
}
