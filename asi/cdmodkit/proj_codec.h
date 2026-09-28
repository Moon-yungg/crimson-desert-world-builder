#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <functional>
#include <string>
#include <vector>

// Disk values only: no engine handles, session identities, group/uid allocation or
// registry access. Every entry point is transactional: on failure the caller's value
// (a Document, a text buffer, or the destination file) is left exactly as it was.
//
// Projects emit upstream v4 (optional tenth object field = note hex), with WB bounds/
// envelopes when present; legacy v1-v3 and WB v3 inputs remain readable. Groups stay
// object-only WB v3; optional # wb-note record=N hex=... preserves object notes.
// Shared project/group metadata: #group|id|utf8Hex. Project-only NPC metadata:
//   #npc|key|x|y|z|type|extra|ai|behavior|group[|labelHex[|noteHex]]
// WB v3 metadata grammar:
//   # cdproj v3 kind=project|group
//   # wb-document  anchor=x,y,z min=x,y,z max=x,y,z quality=measured|approx
//   # wb-envelope  id=N anchor=x,y,z min=x,y,z max=x,y,z quality=measured|approx  (sequential, per copy)
//   # wb-member    record=N envelope=N                                            (one per data record)
//   prefab|x|y|z|yawDeg|scale|group|pitchDeg|rollDeg
//   #terrain|mode|x|z|r|amount|strength|ax|az|y[|tileX|tileZ]   (projects only)
// Legacy rows stay valid input: four fields (prefab|x|y|z), seven fields
// (+yaw|scale|group) and the nine-field row; omitted optional fields take their
// defaults (yaw/pitch/roll=0, scale=1, group=0). Ordinary comments never consume
// record ordinals; the reserved header and # wb-* families are never ignored.
namespace proj_codec {

enum class Kind { Project, Group };

struct Point { double x = 0, y = 0, z = 0; };

struct Bounds { Point anchor, min, max; bool approximate = false; };

struct Envelope { int id = 0; Bounds bounds; };

struct Record {
    std::string prefab;
    Point pos;
    double yaw = 0, scale = 1, pitch = 0, roll = 0;
    int group = 0;
    enum class State { Placeable, Hidden, Missing };
    State state = State::Placeable; // legacy read-only exclusion; never emitted
    int envelope = 0;              // document-local id; zero when no object envelopes are present
    std::string note;
};

// Terrain is project data, not a placeable row or a group envelope. Legacy files may
// omit ax/az/y; their zero defaults match main's reader. Values must narrow to floats.
struct TerrainRecord {
    int mode = 0;
    double x = 0, z = 0, r = 0, amount = 0, strength = 0, ax = 0, az = 0, y = 0;
    bool tileScoped = false;
    int tileX = 0, tileZ = 0;
};

struct NpcRecord {
    uint32_t key = 0;
    Point pos;
    int type = 1;
    uint32_t extra = 0;
    bool aiEnabled = true;
    int behavior = 0, group = 0;
    std::string label, note;
};

struct Document {
    Kind kind = Kind::Project;
    bool legacy = false;
    bool hasBounds = false;
    Bounds bounds;
    std::vector<Envelope> envelopes;
    std::vector<Record> records;
    std::vector<TerrainRecord> terrain;
    std::vector<NpcRecord> npcs;     // separate ordinals: never consumes an object envelope/member
    std::map<int, std::string> groupNames; // one partition namespace shared by objects and NPCs
};

// The double-valued document narrowed at the engine boundary: float pose plus the
// int16 tile pair the engine transform derives from the destination position. One
// entry per record keeps data-record ordinals aligned with the caller's scene
// mapping; excluded legacy rows carry placeable=false and no engine values.
struct EngineRow {
    bool placeable = false;
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, roll = 0;
    float scale = 1;
    int group = 0;
    int tileX = 0, tileZ = 0;
};

// Structural validation of a parsed or caller-built document, shared with
// Serialize. Errors use data-record ordinals (header/metadata errors are record 0).
bool Validate(const Document& document, std::string& error);
// Exact semantic values including notes, NPCs, names and terrain; ignores input dialect (legacy).
// Partition/envelope ids are compared exactly. Serialize compares its canonically remapped snapshot.
bool SameValues(const Document& a, const Document& b);

// Parse is transactional: a failure leaves out untouched. A row the legacy state
// wrapper marks hidden/missing is a successful parse that carries its State (the
// caller decides the exclusion); a malformed row is a parse failure naming that
// data-record ordinal.
bool Parse(const std::string& text, const std::string& path, Kind expected, Document& out, std::string& error);

// Serialize validates the document, writes the canonical grammar and re-parses its
// own output before returning it; a failure leaves text untouched.
bool Serialize(const Document& document, std::string& text, std::string& error);

// Full-document validation and narrowing BEFORE any scene mutation: finite values,
// positive scale, nonnegative partitions, double-to-float representability and
// int16 tile representability. A failure leaves out untouched.
bool NarrowForEngine(const Document& document, std::vector<EngineRow>& out, std::string& error);

// Transactional write: a same-directory temporary file receives the full text, is
// flushed and committed, is read back and re-parsed byte-for-byte, and only then
// replaces the destination. Any failure (including an injected one) leaves the
// destination untouched and removes only the owned temporary file.
enum class WriteStage { Write, Flush, Close, Readback, Replace };
struct WriteHooks {
    // Test-only fault seam (production passes nullptr): called before each stage; a
    // false return aborts the transaction with error. `length` is the byte count for
    // the Write stage, so a hook can inject a short write.
    std::function<bool(WriteStage stage, const std::string& tempPath, size_t& length, std::string& error)> fault;
    // Final approval/selection guard, called after a successful readback and before
    // replacement. A false return aborts with error; the destination is untouched.
    std::function<bool(std::string& error)> beforeReplace;
    // Final replacement of the guarded transaction (empty = the codec's own rename).
    // The codec has written, flushed, closed and re-parsed the owned temporary and
    // calls this so the caller can perform the ACTUAL replacement while its own
    // authorities (selection publication, registry) are held across it, leaving no
    // window between the last validation and the rename. It must return false
    // without touching the destination when it refuses.
    std::function<bool(const std::string& tempPath, const std::string& destination, std::string& error)> replace;
};
bool WriteTransactional(const std::string& path, Kind kind, const std::string& text,
                        const WriteHooks* hooks, std::string& error);

} // namespace proj_codec
