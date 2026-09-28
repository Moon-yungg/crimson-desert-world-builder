// AnchorTransform suite: the production wb_group_math TU is the unit under test. The fixture exercises the
// C4 geometry in isolation - eight-corner AABB under Ry*Rx*Rz, the saved-anchor rule (a receiver never
// recentres a single imported survivor), uniform yaw/scale about the saved anchor with member tilt kept,
// and the transactional contract (a failed call leaves the caller's vector untouched). An independent
// eight-corner reference is built here from explicit rotation matrices, so a production regression in the
// rotation order or corner enumeration cannot pass by agreeing with itself.
#include "../../asi/cdmodkit/wb_group_math.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using proj_codec::Bounds;
using proj_codec::Point;
using proj_codec::Record;
using wb_group_math::Member;

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

static bool Near(double a, double b) {
    return std::abs(a - b) <= 1e-9 * (std::max)(1.0, (std::max)(std::abs(a), std::abs(b)));
}
static bool SameBounds(const Bounds& a, const Bounds& b) {
    return Near(a.anchor.x, b.anchor.x) && Near(a.anchor.y, b.anchor.y) && Near(a.anchor.z, b.anchor.z) &&
           Near(a.min.x, b.min.x) && Near(a.min.y, b.min.y) && Near(a.min.z, b.min.z) &&
           Near(a.max.x, b.max.x) && Near(a.max.y, b.max.y) && Near(a.max.z, b.max.z) &&
           a.approximate == b.approximate;
}
static Record Sentinel() {
    Record r; r.prefab = "sentinel"; r.pos = {9, 9, 9}; r.yaw = -1; r.scale = 3; r.pitch = -2; r.roll = -3; r.note = "sentinel-note"; return r;
}
static bool Unchanged(const std::vector<Record>& out) {
    return out.size() == 1 && out[0].prefab == "sentinel" && out[0].pos.x == 9 && out[0].yaw == -1 && out[0].scale == 3 && out[0].note == "sentinel-note";
}
static std::vector<Record> Sentinels(size_t n) {
    std::vector<Record> v;
    for (size_t i = 0; i < n; ++i) v.push_back(Sentinel());
    return v;
}
static bool AllSentinels(const std::vector<Record>& out, size_t n) {
    if (out.size() != n) return false;
    for (const Record& r : out) {
        if (r.prefab != "sentinel" || r.pos.x != 9 || r.pos.y != 9 || r.pos.z != 9 ||
            r.yaw != -1 || r.pitch != -2 || r.roll != -3 || r.scale != 3 || r.note != "sentinel-note") return false;
    }
    return true;
}
static Bounds SentinelBounds() {
    Bounds b; b.anchor = {9, 9, 9}; b.min = {8, 8, 8}; b.max = {10, 10, 10}; b.approximate = true; return b;
}

// Independent Y-only reference for the group transform: the explicit Ry matrix, not the production
// step-wise rotation. Tilt preservation is asserted separately, so the group rotation must be yaw-only.
static Point RefYaw(Point v, double deltaYaw) {
    const double d = std::acos(-1.0) / 180.0;
    const double cy = std::cos(deltaYaw * d), sy = std::sin(deltaYaw * d);
    return {cy * v.x + sy * v.z, v.y, -sy * v.x + cy * v.z};
}

// Independent reference: the full rotation matrix is composed explicitly as Ry * Rx * Rz, then applied to
// each of the eight box corners. This is a different formulation from the production step-wise rotation.
static Point RefCorner(const Member& m, int k) {
    const double d = std::acos(-1.0) / 180.0;
    const double cy = std::cos(m.record.yaw * d), sy = std::sin(m.record.yaw * d);
    const double cx = std::cos(m.record.pitch * d), sx = std::sin(m.record.pitch * d);
    const double cz = std::cos(m.record.roll * d), sz = std::sin(m.record.roll * d);
    const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    double a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a[i][j] = rx[i][0] * rz[0][j] + rx[i][1] * rz[1][j] + rx[i][2] * rz[2][j];
    }
    double full[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) full[i][j] = ry[i][0] * a[0][j] + ry[i][1] * a[1][j] + ry[i][2] * a[2][j];
    }
    const Point size = m.box.known ? m.box.size : Point{2, 2, 2};
    const Point center = m.box.known ? m.box.center : Point{0, 0, 0};
    const double lx = center.x + ((k & 1) ? size.x : -size.x) * .5;
    const double ly = center.y + ((k & 2) ? size.y : -size.y) * .5;
    const double lz = center.z + ((k & 4) ? size.z : -size.z) * .5;
    const double x = full[0][0] * lx + full[0][1] * ly + full[0][2] * lz;
    const double y = full[1][0] * lx + full[1][1] * ly + full[1][2] * lz;
    const double z = full[2][0] * lx + full[2][1] * ly + full[2][2] * lz;
    return {m.record.pos.x + m.record.scale * x, m.record.pos.y + m.record.scale * y, m.record.pos.z + m.record.scale * z};
}
static Bounds RefBounds(const std::vector<Member>& members) {
    const double inf = std::numeric_limits<double>::infinity();
    Bounds b; b.min = {inf, inf, inf}; b.max = {-inf, -inf, -inf};
    for (const Member& m : members) {
        for (int k = 0; k < 8; ++k) {
            const Point p = RefCorner(m, k);
            b.min.x = (std::min)(b.min.x, p.x); b.max.x = (std::max)(b.max.x, p.x);
            b.min.y = (std::min)(b.min.y, p.y); b.max.y = (std::max)(b.max.y, p.y);
            b.min.z = (std::min)(b.min.z, p.z); b.max.z = (std::max)(b.max.z, p.z);
        }
    }
    b.anchor = {(b.min.x + b.max.x) * .5, b.min.y, (b.min.z + b.max.z) * .5};
    return b;
}

static void RunCases() {
    // -------------------------------------------------------------------------------------------------
    // Eight-corner bounds under Ry*Rx*Rz, saved anchor = AABB bottom centre.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-BOUNDS");
    {
        Member m;
        m.record.pos = {0, 0, 0};
        m.record.yaw = 45; m.record.pitch = 0; m.record.roll = 0; m.record.scale = 1;
        m.box.known = true; m.box.size = {2, 2, 2}; m.box.center = {0, 0, 0};
        Bounds got;
        Check(wb_group_math::ComputeBounds({m}, nullptr, got), "a rotated measured box computes bounds");
        const double root2 = std::sqrt(2.0);
        Check(Near(got.max.x, root2) && Near(got.min.x, -root2) && Near(got.max.y, 1) && Near(got.min.y, -1) &&
              Near(got.max.z, root2) && Near(got.min.z, -root2), "all eight corners contribute under a 45 degree yaw about Y");
        Check(Near(got.anchor.x, 0) && Near(got.anchor.y, got.min.y) && Near(got.anchor.z, 0),
              "the computed anchor is the AABB bottom centre below the pivot");
        Check(!got.approximate, "a fully measured box is not approximate");

        Member t;
        t.record.pos = {100, 5, -7};
        t.record.yaw = 90; t.record.pitch = 30; t.record.roll = 20; t.record.scale = 2;
        t.box.known = true; t.box.size = {1, 2, 4}; t.box.center = {0.5, -0.5, 0};
        const Bounds reference = RefBounds({t});
        Bounds tilted;
        Check(wb_group_math::ComputeBounds({t}, nullptr, tilted), "a tilted scaled box computes bounds");
        Check(SameBounds(reference, tilted),
              "production bounds equal the independent eight-corner reference under Ry*Rx*Rz");
        Check(Near(tilted.anchor.x, (tilted.min.x + tilted.max.x) * .5) && Near(tilted.anchor.y, tilted.min.y) &&
              Near(tilted.anchor.z, (tilted.min.z + tilted.max.z) * .5),
              "the anchor stays the bottom-centre of the computed box");

        const std::vector<Member> both = {m, t};
        Bounds aggregate;
        Check(wb_group_math::ComputeBounds(both, nullptr, aggregate) && SameBounds(RefBounds(both), aggregate),
              "a multi-member aggregate spans every member's eight corners");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Unknown boxes: pivot-centred 2 m cube, aggregate marked approximate, never recentred.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-UNKNOWN");
    {
        Member u;
        u.record.pos = {1000, 10, -2000};
        u.record.yaw = 0; u.record.pitch = 0; u.record.roll = 0; u.record.scale = 1;
        u.box.known = false;
        Bounds b;
        Check(wb_group_math::ComputeBounds({u}, nullptr, b), "an unknown box still yields bounds");
        Check(Near(b.min.x, 999) && Near(b.max.x, 1001) && Near(b.min.y, 9) && Near(b.max.y, 11) &&
              Near(b.min.z, -2001) && Near(b.max.z, -1999), "an unknown box uses a pivot-centred 2 m cube");
        Check(b.approximate, "an unknown box marks the aggregate approximate");
        Check(Near(b.anchor.x, 1000) && Near(b.anchor.z, -2000),
              "a single imported member keeps its own position (no receiver recentring)");
        Member bad = u;
        bad.box.known = true; bad.box.size = {2, -1, 2}; bad.box.center = {0, 0, 0};
        Bounds keep = b;
        Check(!wb_group_math::ComputeBounds({bad}, nullptr, keep) && SameBounds(keep, b),
              "a box with a non-positive extent is rejected without touching the output");
        Member inf = u;
        inf.box.known = true; inf.box.size = {2, 2, 2};
        inf.box.center = {std::numeric_limits<double>::infinity(), 0, 0};
        Check(!wb_group_math::ComputeBounds({inf}, nullptr, keep) && SameBounds(keep, b),
              "a box with a non-finite centre is rejected without touching the output");
        Member badMember = u;
        badMember.record.scale = 0;
        Check(!wb_group_math::ComputeBounds({badMember}, nullptr, keep) && SameBounds(keep, b),
              "an invalid member is rejected without touching the output");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Saved bounds are authoritative even when the receiver would measure something else.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-SAVED");
    {
        Member weird;
        weird.record.pos = {-500, 3, 900};
        weird.record.yaw = 33; weird.record.pitch = 0; weird.record.roll = 0; weird.record.scale = 1;
        weird.box.known = false;
        const Bounds saved = {{1000.5, 10, -2000.25}, {990, 0, -2010}, {1010, 20, -1990}, true};
        Bounds out;
        Check(wb_group_math::ComputeBounds({weird}, &saved, out), "saved bounds are accepted");
        Check(SameBounds(out, saved), "the saved anchor/min/max/approximate are returned unchanged");
        Bounds invalid = saved;
        invalid.anchor.x = 5000;
        Check(!wb_group_math::ComputeBounds({weird}, &invalid, out) && SameBounds(out, saved),
              "an invalid saved envelope is rejected without touching the output");
        Bounds noAnchor = saved;
        noAnchor.anchor = {990, 10, -2000.25};
        Check(wb_group_math::ComputeBounds({weird}, &noAnchor, out) && SameBounds(out, noAnchor),
              "a legal off-origin saved anchor is preserved exactly");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Rigid transform about the saved anchor: yaw adds, uniform scale multiplies, member tilt stays.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-ANCHOR");
    {
        Record atAnchor;
        atAnchor.prefab = "/object/a.prefab";
        atAnchor.pos = {1000.5, 10, -2000.25};
        atAnchor.yaw = 10; atAnchor.pitch = 30; atAnchor.roll = 20; atAnchor.scale = 1.5;
        atAnchor.note = "anchor|note"; atAnchor.group = 42; atAnchor.envelope = 7;
        Record neighbour = atAnchor;
        neighbour.pos = {1002.5, 10, -2000.25}; neighbour.note = "neighbour-note";
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        const Point target = {5, 6, 7};
        std::vector<Record> out;
        Check(wb_group_math::AnchorTransform({atAnchor, neighbour}, bounds, target, 90, 2, out),
              "the whole group transforms about the saved anchor");
        Check(out.size() == 2, "every member is transformed");
        Check(Near(out[0].pos.x, 5) && Near(out[0].pos.y, 6) && Near(out[0].pos.z, 7),
              "the member sitting at the saved anchor lands exactly on the target");
        Check(Near(out[0].yaw, 100) && Near(out[0].scale, 3),
              "yaw adds the group delta and scale multiplies by the uniform factor");
        Check(out[0].pitch == 30 && out[0].roll == 20, "member tilt is preserved by the rigid group transform");
        Check(Near(out[1].pos.x, 5) && Near(out[1].pos.y, 6) && Near(out[1].pos.z, 3),
              "a neighbouring member rotates about the group Y axis through the anchor");
        Check(out[1].pitch == 30 && out[1].roll == 20, "the neighbouring member keeps its tilt too");
        Check(out[0].note == "anchor|note" && out[1].note == "neighbour-note" && out[0].group == 42 &&
              out[1].group == 42 && out[0].envelope == 7 && out[1].envelope == 7,
              "rigid geometry preserves distinct v0.97 notes and partition/envelope identity");

        std::vector<Record> half;
        Check(wb_group_math::AnchorTransform({neighbour}, bounds, target, 0, 0.5, half) &&
              Near(half[0].pos.x, 6) && Near(half[0].scale, 0.75),
              "a fraction factor scales the anchor-relative offset");
        std::vector<Record> wrapped;
        Check(wb_group_math::AnchorTransform({atAnchor}, bounds, target, 360, 1, wrapped) &&
              Near(wrapped[0].yaw, 370) && wrapped[0].pitch == 30 && wrapped[0].roll == 20,
              "a full-turn delta adds only yaw and keeps the member tilt");
        std::vector<Record> back;
        Check(wb_group_math::AnchorTransform({atAnchor}, bounds, target, -90, 1, back) &&
              Near(back[0].yaw, -80) && back[0].pitch == 30 && back[0].roll == 20,
              "a negative yaw delta is applied without touching tilt");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Rejections are transactional: out is replaced only by a fully valid transform.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-INVALID");
    {
        Record atAnchor;
        atAnchor.prefab = "/object/a.prefab";
        atAnchor.pos = {1000.5, 10, -2000.25};
        atAnchor.yaw = 10; atAnchor.pitch = 30; atAnchor.roll = 20; atAnchor.scale = 1.5;
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        const Point target = {5, 6, 7};
        std::vector<Record> out;
        out.push_back(Sentinel());
        Check(!wb_group_math::AnchorTransform({}, bounds, target, 0, 1, out) && Unchanged(out),
              "an empty source is rejected without touching the output");
        Bounds badBounds = bounds;
        badBounds.anchor.y = -100;
        Check(!wb_group_math::AnchorTransform({atAnchor}, badBounds, target, 0, 1, out) && Unchanged(out),
              "an anchor outside min/max is rejected without touching the output");
        Bounds nonfinite = bounds;
        nonfinite.max.z = std::numeric_limits<double>::infinity();
        Check(!wb_group_math::AnchorTransform({atAnchor}, nonfinite, target, 0, 1, out) && Unchanged(out),
              "a non-finite envelope is rejected without touching the output");
        const Point nanTarget = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
        Check(!wb_group_math::AnchorTransform({atAnchor}, bounds, nanTarget, 0, 1, out) && Unchanged(out),
              "a non-finite target is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({atAnchor}, bounds, target, std::numeric_limits<double>::infinity(), 1, out) &&
              Unchanged(out), "a non-finite delta yaw is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({atAnchor}, bounds, target, 0, 0, out) && Unchanged(out),
              "a zero factor is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({atAnchor}, bounds, target, 0, -2, out) && Unchanged(out),
              "a negative factor is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({atAnchor}, bounds, target, 0, std::numeric_limits<double>::quiet_NaN(), out) &&
              Unchanged(out), "a non-finite factor is rejected without touching the output");
        Record zeroScale = atAnchor;
        zeroScale.scale = 0;
        Check(!wb_group_math::AnchorTransform({zeroScale}, bounds, target, 0, 1, out) && Unchanged(out),
              "a member with a non-positive scale is rejected without touching the output");
        Record infinite = atAnchor;
        infinite.pos.x = std::numeric_limits<double>::infinity();
        Check(!wb_group_math::AnchorTransform({infinite}, bounds, target, 0, 1, out) && Unchanged(out),
              "a member with a non-finite position is rejected without touching the output");
        Record huge = atAnchor;
        huge.pos.x = 1e308;
        Check(!wb_group_math::AnchorTransform({huge}, bounds, target, 0, 1e10, out) && Unchanged(out),
              "a transform that overflows the result is rejected without touching the output");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // A successful call replaces the caller's vector completely; a failed one keeps every entry.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-TRANSACTION");
    {
        Member member;
        member.record.prefab = "/object/a.prefab";
        member.record.pos = {1000.5, 10, -2000.25};
        member.record.yaw = 10; member.record.pitch = 30; member.record.roll = 20; member.record.scale = 1.5;
        member.box.known = false;
        const Bounds saved = {{0, 0, 0}, {-1, -1, -1}, {1, 1, 1}, false};
        Bounds keep = {{5, 5, 5}, {4, 4, 4}, {6, 6, 6}, true};
        Bounds boundsOut = keep;
        const std::vector<Member> none;
        Check(!wb_group_math::ComputeBounds(none, &saved, boundsOut) && SameBounds(boundsOut, keep),
              "an empty member list is rejected even with saved bounds (no empty success)");
        Bounds invalidSaved = saved;
        invalidSaved.min.y = 200;
        Check(!wb_group_math::ComputeBounds({member}, &invalidSaved, boundsOut) && SameBounds(boundsOut, keep),
              "a failed saved-envelope lookup keeps the caller's bounds untouched");

        const Record atAnchor = member.record;
        const Bounds source = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        std::vector<Record> replaced;
        replaced.push_back(Sentinel());
        replaced.push_back(Sentinel());
        Check(wb_group_math::AnchorTransform({atAnchor}, source, {1, 2, 3}, 0, 1, replaced) &&
              replaced.size() == 1 && replaced[0].prefab == "/object/a.prefab",
              "a successful transform replaces the whole vector");
        replaced.push_back(Sentinel());
        replaced.push_back(Sentinel());
        Check(!wb_group_math::AnchorTransform({}, source, {1, 2, 3}, 0, 1, replaced) && replaced.size() == 3 &&
              replaced[0].prefab == "/object/a.prefab" && replaced[1].prefab == "sentinel" && replaced[2].prefab == "sentinel",
              "a failed transform keeps every pre-existing entry");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // ANCHOR-OFFSET: the saved anchor is the pivot even though it is off-origin and is not the centre of
    // the receiver's measured AABB; no receiver-bound recentring happens anywhere.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-ANCHOR-OFFSET");
    {
        const Bounds saved = {{1000.5, 10, -2000.25}, {995, 0, -2005}, {1005, 20, -1995}, false};
        const Point receiverCentre{(saved.min.x + saved.max.x) * .5, (saved.min.y + saved.max.y) * .5,
                                   (saved.min.z + saved.max.z) * .5};
        Record a;
        a.prefab = "/object/a.prefab";
        a.pos = {1000.5, 10, -2000.25}; a.yaw = 10; a.pitch = 30; a.roll = 20; a.scale = 1.5;
        Record b = a;
        b.prefab = "/object/b.prefab";
        b.pos = {1002.5, 14, -2003.25};
        const Point target = {5, 6, 7};
        std::vector<Record> out;
        Check(wb_group_math::AnchorTransform({a, b}, saved, target, 90, 2, out) && out.size() == 2,
              "a group with an off-origin saved anchor transforms");
        Check(Near(out[0].pos.x, target.x) && Near(out[0].pos.y, target.y) && Near(out[0].pos.z, target.z),
              "the member sitting at the off-origin saved anchor lands exactly on the target");
        const Point centreOffset{a.pos.x - receiverCentre.x, a.pos.y - receiverCentre.y, a.pos.z - receiverCentre.z};
        const Point centrePivot = RefYaw(centreOffset, 90);
        Check(!Near(out[0].pos.x, target.x + 2 * centrePivot.x) && !Near(out[0].pos.z, target.z + 2 * centrePivot.z),
              "the receiver's AABB centre is not used as a pivot (that would displace the anchor member)");
        const Point offset{b.pos.x - saved.anchor.x, b.pos.y - saved.anchor.y, b.pos.z - saved.anchor.z};
        const Point ref = RefYaw(offset, 90);
        Check(Near(out[1].pos.x, target.x + 2 * ref.x) && Near(out[1].pos.y, target.y + 2 * ref.y) &&
              Near(out[1].pos.z, target.z + 2 * ref.z),
              "every member's saved-anchor offset rotates about Y and scales by the uniform factor");
        Check(Near(out[1].pos.x, -1) && Near(out[1].pos.y, 14) && Near(out[1].pos.z, 3),
              "the neighbour's closed-form result for yaw 90 and factor 2");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // TILT: member pitch/roll survive the yaw delta and the uniform scale exactly, and the group rotation
    // stays yaw-only (a member's tilt never leaks into its anchor-relative offset).
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-TILT");
    {
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        Record p;
        p.prefab = "/object/p.prefab";
        p.pos = {1002.5, 10, -2000.25}; p.yaw = 10; p.pitch = 30; p.roll = 20; p.scale = 1.5;
        Record n;
        n.prefab = "/object/n.prefab";
        n.pos = {998.5, 12, -1999.25}; n.yaw = -45; n.pitch = -12.5; n.roll = -170.25; n.scale = 0.25;
        const Point target = {5, 6, 7};
        std::vector<Record> grown;
        Check(wb_group_math::AnchorTransform({p, n}, bounds, target, 90, 2, grown) && grown.size() == 2,
              "the tilted group transforms under a yaw delta and a uniform scale");
        Check(grown[0].pitch == 30 && grown[0].roll == 20, "member 1 keeps pitch and roll exactly");
        Check(grown[1].pitch == -12.5 && grown[1].roll == -170.25,
              "member 2 keeps its negative pitch and roll exactly");
        Check(Near(grown[0].yaw, 100) && Near(grown[1].yaw, 45),
              "only yaw changes: the group delta adds to each member's own yaw");
        Check(Near(grown[0].scale, 3) && Near(grown[1].scale, 0.5),
              "the uniform factor multiplies every member's scale");
        std::vector<Record> shrunk;
        Check(wb_group_math::AnchorTransform({p, n}, bounds, target, -270, 0.5, shrunk) && shrunk.size() == 2,
              "a negative yaw delta with a fraction factor is accepted");
        Check(shrunk[0].pitch == 30 && shrunk[0].roll == 20 && shrunk[1].pitch == -12.5 && shrunk[1].roll == -170.25,
              "tilt survives a negative delta and a fraction factor as well");
        Record flat = p;
        flat.prefab = "/object/flat.prefab"; flat.pitch = 0; flat.roll = 0;
        std::vector<Record> tilted, level;
        Check(wb_group_math::AnchorTransform({p}, bounds, target, 90, 2, tilted) &&
              wb_group_math::AnchorTransform({flat}, bounds, target, 90, 2, level) &&
              Near(tilted[0].pos.x, level[0].pos.x) && Near(tilted[0].pos.y, level[0].pos.y) &&
              Near(tilted[0].pos.z, level[0].pos.z),
              "member tilt never leaks into the group offset (the group rotation is yaw-only)");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // SINGLE-SURVIVOR: one imported row that survived an import keeps its own saved position; the receiver
    // measurement is not used to recentre it and a saved envelope wins over that measurement.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-SINGLE-SURVIVOR");
    {
        Member survivor;
        survivor.record.prefab = "/object/survivor.prefab";
        survivor.record.pos = {1234.5, -7, 8765.25};
        survivor.record.yaw = 17; survivor.record.pitch = 5; survivor.record.roll = -9; survivor.record.scale = 1;
        survivor.box.known = false;
        Bounds measured;
        Check(wb_group_math::ComputeBounds({survivor}, nullptr, measured), "a lone imported row still measures");
        const Bounds ref = RefBounds({survivor});
        Check(Near(ref.min.x, measured.min.x) && Near(ref.min.y, measured.min.y) && Near(ref.min.z, measured.min.z) &&
              Near(ref.max.x, measured.max.x) && Near(ref.max.y, measured.max.y) && Near(ref.max.z, measured.max.z),
              "the lone survivor's measured box matches the independent eight-corner reference");
        Check(Near(measured.anchor.x, survivor.record.pos.x) && Near(measured.anchor.z, survivor.record.pos.z) &&
              Near(measured.anchor.y, measured.min.y) &&
              Near(measured.min.y + measured.max.y, 2 * survivor.record.pos.y),
              "the lone survivor's measured anchor stays centred on its own pivot (bottom centre, no recentring)");
        Check(!Near(measured.anchor.x, 0) && !Near(measured.anchor.z, 0),
              "the measured anchor is the survivor's own pivot, not a receiver-relative origin");
        const Bounds saved = {{0, 0, 0}, {-1, -1, -1}, {1, 1, 1}, true};
        Bounds boundsOut = SentinelBounds();
        Check(wb_group_math::ComputeBounds({survivor}, &saved, boundsOut) && SameBounds(boundsOut, saved),
              "a saved envelope wins over the survivor's receiver measurement");
        const Point target = {5, 6, 7};
        std::vector<Record> kept;
        Check(wb_group_math::AnchorTransform({survivor.record}, saved, target, 0, 1, kept) && kept.size() == 1,
              "the single survivor transforms about the saved anchor");
        Check(Near(kept[0].pos.x, target.x + survivor.record.pos.x - saved.anchor.x) &&
              Near(kept[0].pos.y, target.y + survivor.record.pos.y - saved.anchor.y) &&
              Near(kept[0].pos.z, target.z + survivor.record.pos.z - saved.anchor.z),
              "its saved offset is preserved: the survivor is not collapsed onto the target");
        Check(!Near(kept[0].pos.x, target.x) && !Near(kept[0].pos.z, target.z),
              "a receiver recentre onto its own anchor would have zeroed that offset");
        std::vector<Record> turned;
        Check(wb_group_math::AnchorTransform({survivor.record}, saved, target, 90, 1, turned) &&
              Near(turned[0].pos.x, 8770.25) && Near(turned[0].pos.y, -1) && Near(turned[0].pos.z, -1227.5),
              "the survivor's off-origin offset rotates about Y about the saved anchor");
        Check(turned[0].pitch == 5 && turned[0].roll == -9, "the survivor keeps its tilt through the group rotation");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // NONFINITE: every non-finite input is rejected and the caller's vector is left untouched.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-NONFINITE");
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        Record ok;
        ok.prefab = "/object/a.prefab";
        ok.pos = {1000.5, 10, -2000.25}; ok.yaw = 10; ok.pitch = 30; ok.roll = 20; ok.scale = 1.5;
        const Point target = {5, 6, 7};
        std::vector<Record> out = Sentinels(1);
        Member member;
        member.record = ok;
        member.box.known = false;
        Bounds keep = SentinelBounds();
        Bounds probe = keep;
        const Bounds savedInf = {{inf, 0, 0}, {-1, -1, -1}, {1, 1, 1}, false};
        Check(!wb_group_math::ComputeBounds({member}, &savedInf, probe) && SameBounds(probe, keep),
              "a non-finite saved envelope is rejected without touching the output");
        probe = keep;
        member.box.known = true; member.box.size = {2, 2, 2};
        member.box.center = {0, nan, 0};
        Check(!wb_group_math::ComputeBounds({member}, nullptr, probe) && SameBounds(probe, keep),
              "a non-finite box centre is rejected without touching the output");
        Bounds nanMin = bounds;
        nanMin.min.y = nan;
        Check(!wb_group_math::AnchorTransform({ok}, nanMin, target, 0, 1, out) && AllSentinels(out, 1),
              "a NaN envelope minimum is rejected without touching the output");
        Bounds infMax = bounds;
        infMax.max.z = -inf;
        Check(!wb_group_math::AnchorTransform({ok}, infMax, target, 0, 1, out) && AllSentinels(out, 1),
              "a non-finite envelope maximum is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, {nan, 6, 7}, 0, 1, out) && AllSentinels(out, 1),
              "a NaN target coordinate is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, {5, inf, 7}, 0, 1, out) && AllSentinels(out, 1),
              "a non-finite target coordinate is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, nan, 1, out) && AllSentinels(out, 1),
              "a NaN delta yaw is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, -inf, 1, out) && AllSentinels(out, 1),
              "a non-finite delta yaw is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, 0, inf, out) && AllSentinels(out, 1),
              "a non-finite factor is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, 0, nan, out) && AllSentinels(out, 1),
              "a NaN factor is rejected without touching the output");
        Record bad = ok;
        bad.yaw = nan;
        Check(!wb_group_math::AnchorTransform({bad}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a NaN yaw is rejected without touching the output");
        bad = ok; bad.pitch = inf;
        Check(!wb_group_math::AnchorTransform({bad}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a non-finite pitch is rejected without touching the output");
        bad = ok; bad.roll = nan;
        Check(!wb_group_math::AnchorTransform({bad}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a NaN roll is rejected without touching the output");
        bad = ok; bad.scale = nan;
        Check(!wb_group_math::AnchorTransform({bad}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a NaN scale is rejected without touching the output");
        bad = ok; bad.pos.z = nan;
        Check(!wb_group_math::AnchorTransform({bad}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a NaN position is rejected without touching the output");
        Record badSecond = ok;
        badSecond.prefab = "/object/second.prefab";
        badSecond.pos.y = inf;
        std::vector<Record> two = Sentinels(2);
        Check(!wb_group_math::AnchorTransform({ok, badSecond}, bounds, target, 0, 1, two) && AllSentinels(two, 2),
              "a non-finite row later in the list leaves every caller entry untouched (no partial transform)");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // INVALID-BOUNDS: an envelope whose anchor lies outside its own min/max, an inverted envelope, or an
    // invalid saved envelope is rejected and the caller's bounds/vector are left untouched.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-INVALID-BOUNDS");
    {
        const Bounds good = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        Record ok;
        ok.prefab = "/object/a.prefab";
        ok.pos = {1000.5, 10, -2000.25}; ok.yaw = 10; ok.pitch = 30; ok.roll = 20; ok.scale = 1.5;
        const Point target = {5, 6, 7};
        std::vector<Record> out = Sentinels(1);
        Bounds belowX = good;
        belowX.anchor.x = 998;
        Check(!wb_group_math::AnchorTransform({ok}, belowX, target, 0, 1, out) && AllSentinels(out, 1),
              "an anchor below min.x is rejected without touching the output");
        Bounds aboveY = good;
        aboveY.anchor.y = 13;
        Check(!wb_group_math::AnchorTransform({ok}, aboveY, target, 0, 1, out) && AllSentinels(out, 1),
              "an anchor above max.y is rejected without touching the output");
        Bounds belowZ = good;
        belowZ.anchor.z = -2010;
        Check(!wb_group_math::AnchorTransform({ok}, belowZ, target, 0, 1, out) && AllSentinels(out, 1),
              "an anchor below min.z is rejected without touching the output");
        Bounds inverted = good;
        inverted.min.x = 1005; inverted.max.x = 999;
        Check(!wb_group_math::AnchorTransform({ok}, inverted, target, 0, 1, out) && AllSentinels(out, 1),
              "an inverted envelope (min.x > max.x) is rejected without touching the output");
        Member member;
        member.record = ok;
        member.box.known = false;
        Bounds keep = SentinelBounds();
        Bounds probe = keep;
        Bounds savedBelow = {{0, -5, 0}, {-1, -1, -1}, {1, 1, 1}, false};
        Check(!wb_group_math::ComputeBounds({member}, &savedBelow, probe) && SameBounds(probe, keep),
              "a saved envelope with its anchor below min.y is rejected without touching the output");
        probe = keep;
        Bounds savedAbove = {{0, 0, 5}, {-1, -1, -1}, {1, 1, 1}, false};
        Check(!wb_group_math::ComputeBounds({member}, &savedAbove, probe) && SameBounds(probe, keep),
              "a saved envelope with its anchor above max.z is rejected without touching the output");
        probe = keep;
        Bounds savedInverted = {{0, 0, 0}, {1, -1, -1}, {-1, 1, 1}, false};
        Check(!wb_group_math::ComputeBounds({member}, &savedInverted, probe) && SameBounds(probe, keep),
              "a saved envelope with min.x > max.x is rejected without touching the output");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // NONPOSITIVE-SCALE: a zero or negative factor, a non-positive member scale, or a non-positive box
    // extent is rejected and the caller's vector/bounds are left untouched.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-NONPOSITIVE-SCALE");
    {
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        Record ok;
        ok.prefab = "/object/a.prefab";
        ok.pos = {1000.5, 10, -2000.25}; ok.yaw = 10; ok.pitch = 30; ok.roll = 20; ok.scale = 1.5;
        const Point target = {5, 6, 7};
        std::vector<Record> out = Sentinels(1);
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, 0, 0, out) && AllSentinels(out, 1),
              "a zero factor is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, 0, -0.5, out) && AllSentinels(out, 1),
              "a negative factor is rejected without touching the output");
        Check(!wb_group_math::AnchorTransform({ok}, bounds, target, 0, -std::numeric_limits<double>::infinity(), out) &&
              AllSentinels(out, 1), "a non-finite negative factor is rejected without touching the output");
        Record zero = ok;
        zero.scale = 0;
        Check(!wb_group_math::AnchorTransform({zero}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with scale 0 is rejected without touching the output");
        Record negative = ok;
        negative.scale = -1.5;
        Check(!wb_group_math::AnchorTransform({negative}, bounds, target, 0, 1, out) && AllSentinels(out, 1),
              "a member with a negative scale is rejected without touching the output");
        std::vector<Record> tiny;
        Check(wb_group_math::AnchorTransform({ok}, bounds, target, 0, 5e-324, tiny) && tiny.size() == 1 &&
              tiny[0].scale > 0 && std::isfinite(tiny[0].scale),
              "the smallest positive factor is accepted (only non-positive factors are rejected)");
        Member member;
        member.record = ok;
        member.box.known = true; member.box.center = {0, 0, 0};
        Bounds keep = SentinelBounds();
        Bounds probe = keep;
        member.box.size = {0, 2, 2};
        Check(!wb_group_math::ComputeBounds({member}, nullptr, probe) && SameBounds(probe, keep),
              "a zero box extent is rejected without touching the output");
        probe = keep;
        member.box.size = {2, 2, -3};
        Check(!wb_group_math::ComputeBounds({member}, nullptr, probe) && SameBounds(probe, keep),
              "a negative box extent is rejected without touching the output");
        probe = keep;
        member.box.size = {2, 2, 2}; member.box.center = {-1, 0, 0};
        member.record.scale = 0;
        Check(!wb_group_math::ComputeBounds({member}, nullptr, probe) && SameBounds(probe, keep),
              "a member with scale 0 is rejected by ComputeBounds without touching the output");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // OVERFLOW: a transform whose result is not finite is rejected, and a later overflowing row never
    // leaves a partially transformed copy in the caller's vector.
    // -------------------------------------------------------------------------------------------------
    BeginCase("MATH-OVERFLOW");
    {
        const Bounds bounds = {{1000.5, 10, -2000.25}, {999.5, 8, -2001.25}, {1001.5, 12, -1999.25}, false};
        Record ok;
        ok.prefab = "/object/a.prefab";
        ok.pos = {1000.5, 10, -2000.25}; ok.yaw = 10; ok.pitch = 30; ok.roll = 20; ok.scale = 1.5;
        const Point target = {5, 6, 7};
        std::vector<Record> out = Sentinels(1);
        Record hugePos = ok;
        hugePos.pos.x = 1e308;
        Check(!wb_group_math::AnchorTransform({hugePos}, bounds, target, 0, 1e10, out) && AllSentinels(out, 1),
              "a scale-up that overflows the member position is rejected without touching the output");
        Record hugeScale = ok;
        hugeScale.scale = 1e308;
        Check(!wb_group_math::AnchorTransform({hugeScale}, bounds, target, 0, 2, out) && AllSentinels(out, 1),
              "an overflow of the member scale is rejected without touching the output");
        Record far = ok;
        far.pos.x = 1e308;
        Check(!wb_group_math::AnchorTransform({far}, bounds, {1.7e308, 0, 0}, 0, 1, out) && AllSentinels(out, 1),
              "a target reached through a large saved offset overflow is rejected");
        Record first = ok;
        first.prefab = "/object/first.prefab";
        Record second = ok;
        second.prefab = "/object/second.prefab";
        second.pos.y = 1e308;
        std::vector<Record> two = Sentinels(2);
        Check(!wb_group_math::AnchorTransform({first, second}, bounds, target, 0, 1e10, two) && AllSentinels(two, 2),
              "a later overflowing row leaves every caller entry untouched (no partial replacement)");
    }
    EndCase();
}

static void WriteEvidence(const std::string& dir) {
    if (dir.empty()) return;
    std::filesystem::create_directories(dir);
    std::ofstream log(std::filesystem::path(dir) / "anchor_transform.log", std::ios::binary | std::ios::trunc);
    for (const auto& c : cases) {
        log << "case " << c.id << ": " << (c.failures == 0 ? "PASS" : "FAIL")
            << " assertions=" << c.assertions << " failures=" << c.failures << "\n";
    }
    for (const auto& f : failureLog) log << "FAILURE " << f << "\n";
    std::string json = "{\"kind\":\"wb079-anchor-transform-cases\",\"schemaVersion\":1,\"assertions\":" + std::to_string(assertions) +
        ",\"failures\":" + std::to_string((int)failureLog.size()) + ",\"cases\":[";
    for (size_t i = 0; i < cases.size(); ++i) {
        if (i) json += ",";
        json += "{\"id\":\"" + cases[i].id + "\",\"assertions\":" + std::to_string(cases[i].assertions) +
            ",\"failures\":" + std::to_string(cases[i].failures) + ",\"status\":\"" +
            (cases[i].failures == 0 ? "PASS" : "FAIL") + "\"}";
    }
    json += "]}";
    std::ofstream f(std::filesystem::path(dir) / "anchor_transform.cases.json", std::ios::binary | std::ios::trunc);
    f << json;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "";
    try {
        RunCases();
    } catch (const std::exception& e) {
        Check(false, "unhandled fixture exception");
        std::fprintf(stderr, "EXCEPTION %s\n", e.what());
    }
    WriteEvidence(dir);
    std::printf("ASSERTIONS=%d\n", assertions);
    if (!failureLog.empty()) { std::printf("FAILURES=%d\n", (int)failureLog.size()); return 1; }
    return 0;
}
