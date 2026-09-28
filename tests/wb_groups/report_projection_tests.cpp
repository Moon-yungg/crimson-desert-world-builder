// WB080 Task9: production report_projection + production CORE queues/receipts.
// Only native engine/service boundaries are substituted. No editor TU or History policy is mocked.
// Every async transition below is an explicit queue dispatch or a synchronous native-boundary event.
#include "production_host.h"
#include "../../asi/cdmodkit/report_projection.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
namespace rp = report_projection;
struct Case { std::string id; int assertions = 0, failures = 0; };
std::vector<Case> cases;
int assertions = 0, failures = 0;
std::string root;
std::vector<std::string> trace;
void Check(bool ok, const char* label) {
    ++assertions; ++cases.back().assertions;
    if (!ok) { ++failures; ++cases.back().failures; }
    std::printf("CHECK %s %s/%s\n", ok ? "PASS" : "FAIL", cases.back().id.c_str(), label);
    core::Log("CHECK %s %s/%s", ok ? "PASS" : "FAIL", cases.back().id.c_str(), label);
}
void Require(bool ok, const char* label) { Check(ok, label); if (!ok) throw std::runtime_error(label); }
std::string Json(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out + '"';
}
void Write(const char* name, const std::string& bytes) {
    FILE* f = std::fopen((root + "\\" + name).c_str(), "wb");
    if (!f) throw std::runtime_error("receipt open failed");
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) throw std::runtime_error("receipt write failed");
}
struct Object { Vec3 pos; Rot rot; float scale; bool live = true; };
std::map<uintptr_t, Object> objects;
uintptr_t nextHandle = 100;
std::set<uintptr_t> refusedMoves;
bool refuseNextCreate = false;
int engineCalls = 0;
std::function<void(uintptr_t)> moveObserver;
struct ObserveMove {
    explicit ObserveMove(std::function<void(uintptr_t)> f) { moveObserver = std::move(f); }
    ~ObserveMove() { moveObserver = {}; }
};
const char* prefab = "/object/report/same.prefab";
void Install() {
    auto& e = host::Seam(); e.ready = e.probeReady = true;
    e.createGeneric = [](const std::string& path, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        ++engineCalls;
        const bool refuse = refuseNextCreate || path == "/object/report/refused.prefab";
        refuseNextCreate = false;
        if (refuse) return 0;
        const uintptr_t h = nextHandle++; objects.emplace(h, Object{ pos, rot, scale, true }); return h;
    };
    e.remove = [](uintptr_t h) {
        ++engineCalls; auto it = objects.find(h);
        if (it == objects.end() || !it->second.live) return false;
        it->second.live = false; return true;
    };
    e.moveInPlace = [](uintptr_t h, Vec3 pos, Rot rot, float scale) {
        ++engineCalls; if (moveObserver) moveObserver(h);
        auto it = objects.find(h);
        if (it == objects.end() || !it->second.live || refusedMoves.count(h)) return false;
        it->second.pos = pos; it->second.rot = rot; it->second.scale = scale; return true;
    };
    e.liveMove = e.moveInPlace;
    e.groundCast = [](Vec3, float, core::GroundHit* hit) {
        ++engineCalls; hit->done = true; hit->hit = false; return true;
    };
    e.templateReady = [] { return false; };
    host::SetDirectGimmick(false);
    core::g_gimmickSpawn = false; core::g_recreateOnMove = false;
    core::PrefabInfo p; p.path = prefab; p.hasCenter = true; p.sx = p.sy = p.sz = 2;
    host::SetPrefabIndex({ p });
}
void Drain() {
    for (int n = 0; host::PumpGame(); ++n) if (n >= 4096) throw std::runtime_error("queue failed to drain");
}
void Reset() {
    moveObserver = {}; refusedMoves.clear(); refuseNextCreate = false;
    core::DeleteAllSpawned(); Drain(); objects.clear();
}
SpawnedObj Record(int uid) {
    for (const auto& r : core::Spawned()) if (r.uid == uid) return r;
    throw std::runtime_error("missing native record");
}
int Spawn(float x) {
    const int uid = core::SpawnAt(prefab, { x, 10, 0 });
    Require(uid != 0, "native-spawn-admitted"); Drain();
    Require(Record(uid).obj != 0, "native-spawn-attached"); return uid;
}
bool Contains(const std::vector<rp::Id>& ids, rp::Id id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
const rp::Placement& Find(const rp::Placements& views, rp::Id id) {
    for (const auto& v : views.reports) if (v.id == id) return v;
    throw std::runtime_error("placement identity lost");
}
const rp::Ground& Find(const rp::Grounds& views, rp::Id id) {
    for (const auto& v : views.reports) if (v.receipt.id == id) return v;
    throw std::runtime_error("ground identity lost");
}
void Counts(const rp::Placement& p, int a, int f, int e, int c, int pending, bool final) {
    const auto& v = p.receipt;
    Check(v.requested == a + f + e + c + pending, "requested");
    Check(v.attached == a && v.failed == f && v.excluded == e && v.canceled == c && v.pending == pending, "disjoint-counts");
    Check(v.requested == v.attached + p.displayedExcluded() + v.failed + v.canceled + v.pending, "displayed-partition");
    Check(p.displayedExcluded() == e, "excluded-never-double-counts-failed-canceled");
    Check(p.final() == final, "final-needs-terminal-and-cleanup");
    std::ostringstream out;
    out << "{\"case\":" << Json(cases.back().id) << ",\"kind\":\"placement\",\"id\":" << p.id
        << ",\"requested\":" << v.requested << ",\"attached\":" << v.attached << ",\"failed\":" << v.failed
        << ",\"excluded\":" << v.excluded << ",\"canceled\":" << v.canceled << ",\"pending\":" << v.pending
        << ",\"displayedExcluded\":" << p.displayedExcluded() << ",\"cleanup\":" << v.cleanupPending
        << ",\"final\":" << p.final() << ",\"carrying\":" << p.carrying << '}'; trace.push_back(out.str());
}
void Details(const rp::Placement& p, const core::PlaceRequestView& source) {
    Require(p.receipt.rows.size() == source.rows.size(), "all-original-rows-retained");
    for (size_t i = 0; i < source.rows.size(); ++i) {
        const auto& a = p.receipt.rows[i]; const auto& b = source.rows[i];
        Check(a.rowId == b.rowId && a.prefab == b.prefab && a.uid == b.uid && a.lane == b.lane && a.state == b.state &&
              a.attachObservations == b.attachObservations && a.reason == b.reason && a.removedAfterAttach == b.removedAfterAttach &&
              a.cleanupPending == b.cleanupPending && a.cleanupObligations == b.cleanupObligations, "exact-row-detail-copy");
    }
}
void Counts(const rp::Ground& g, size_t accepted, size_t notApplied, size_t pending, bool final) {
    Check(g.accepted == accepted && g.notApplied == notApplied && g.pending == pending, "ground-partition");
    Check(g.receipt.members.size() == g.accepted + g.notApplied + g.pending, "ground-members-conserved");
    Check(g.final() == final, "ground-final");
    std::ostringstream out;
    out << "{\"case\":" << Json(cases.back().id) << ",\"kind\":\"ground\",\"id\":" << g.receipt.id
        << ",\"serial\":" << g.receipt.serial << ",\"branch\":" << g.receipt.branch << ",\"epoch\":" << g.receipt.epoch
        << ",\"state\":" << g.receipt.state << ",\"accepted\":" << g.accepted << ",\"notApplied\":" << g.notApplied
        << ",\"pending\":" << g.pending << ",\"reason\":" << Json(g.receipt.reason) << ",\"members\":[";
    for (size_t i = 0; i < g.receipt.members.size(); ++i) {
        const auto& m = g.receipt.members[i]; if (i) out << ',';
        out << "{\"uid\":" << m.before.uid << ",\"terminal\":" << m.terminal << ",\"accepted\":" << m.accepted
            << ",\"beforeY\":" << m.before.pos.y << ",\"afterY\":" << m.after.pos.y << ",\"reason\":" << Json(m.reason) << '}';
    }
    out << "]}"; trace.push_back(out.str());
}
void MixedCleanup() {
    rp::PlacementArchive archive;
    auto req = core::BeginPlaceRequest({ prefab, "/object/report/refused.prefab", "/unknown/full/path.prefab", prefab, prefab });
    const auto id = archive.Admit(req, "mixed", true);
    Require(core::PlaceRowExclude(req, 2, "MISSING_PREFAB_09"), "preflight-exclusion");
    for (int row : { 0, 1, 3, 4 }) Require(core::SubmitPlaceRow(req, row, { float(row), 10, 0 }) != 0, "row-admission");
    Require(host::PumpGame() && host::PumpGame(), "two-native-completions");
    const auto before = archive.Snapshot(); const auto& p = Find(before, id);
    Counts(p, 1, 1, 1, 0, 2, false); Details(p, core::PlaceRequestState(req));
    Check(p.approximate && p.attention() && p.active(), "approximate-and-active-attention");
    const auto native = Record(p.receipt.rows[0].uid).obj;
    Require(core::PlaceRequestCancel(req) == 3, "cancel-attached-and-two-pending");
    const auto lag = archive.Snapshot(); const auto& canceled = Find(lag, id);
    Counts(canceled, 1, 1, 1, 2, 0, false); Details(canceled, core::PlaceRequestState(req));
    Check(canceled.receipt.cleanupPending && canceled.removedAfterAttachment == 1 && objects.at(native).live, "cancel-cleanup-not-yet-dispatched");
    Check(p.receipt.pending == 2 && !p.receipt.requestCanceled && !p.receipt.rows[0].removedAfterAttach, "previous-snapshot-immutable");
    Drain(); const auto done = archive.Snapshot();
    Counts(Find(done, id), 1, 1, 1, 2, 0, true);
    Check(!objects.at(native).live && Find(done, id).receipt.attached == 1, "historical-attached-not-live-census");
    Check(!Find(done, id).receipt.cleanupPending && Find(done, id).attention(), "terminal-warning-retained");
    Check(Find(done, id).request == req, "cancel-target-is-original-request");
}
void ReversedSameName() {
    rp::PlacementArchive archive;
    auto old = core::BeginPlaceRequest({ prefab }), newer = core::BeginPlaceRequest({ prefab });
    const auto oldId = archive.Admit(old, "same"), newId = archive.Admit(newer, "same");
    Require(oldId != newId, "same-name-distinct-ids");
    Require(core::SubmitPlaceRow(old, 0, { 1, 10, 0 }) != 0 && core::SubmitPlaceRow(newer, 0, { 2, 10, 0 }) != 0, "both-queued");
    Require(host::PumpGameAt(1), "newer-completes-first");
    auto views = archive.Snapshot(newer);
    Check(views.order.newest == newId && Contains(views.order.olderActive, oldId), "newest-admitted-with-old-pending-visible");
    Counts(Find(views, newId), 1, 0, 0, 0, 0, true);
    Check(Find(views, newId).carrying && Find(views, newId).active(), "final-can-still-be-carried");
    refuseNextCreate = true; Require(host::PumpGame(), "older-fails-last");
    views = archive.Snapshot(newer);
    Check(views.order.newest == newId && Contains(views.order.earlierCompleted, oldId) && views.order.olderActive.empty(), "late-callback-never-promotes-old-request");
    Check(views.order.earlierAttentionCount == 1 && Find(views, oldId).attention(), "older-failure-disclosure-count");
    Check(Find(views, oldId).receipt.failed == 1 && Find(views, newId).receipt.attached == 1, "no-same-name-result-theft");
    Check(archive.Admit(old, "renamed", true) == oldId && archive.Snapshot().order.newest == newId, "repeat-registration-cannot-reorder");
    Check(Find(archive.Snapshot(), oldId).name == "same", "admission-label-not-callback-label");
    auto last = core::BeginPlaceRequest({}); const auto lastId = archive.Admit(last, "third");
    views = archive.Snapshot(newer);
    Check(views.order.newest == lastId && Contains(views.order.olderActive, newId) && !Contains(views.order.earlierCompleted, newId), "older-final-carried-remains-visible");
    views = archive.Snapshot();
    Check(Contains(views.order.earlierCompleted, newId), "drop-only-changes-presentation-activity");
}
void AllExcluded() {
    rp::PlacementArchive archive;
    const auto empty = archive.Snapshot(); Check(empty.order.newest == 0 && empty.reports.empty(), "empty-section");
    auto req = core::BeginPlaceRequest({ "/missing/one.prefab", "/missing/two.prefab" });
    const auto id = archive.Admit(req, "all-excluded");
    Require(core::PlaceRowExclude(req, 0, "MISSING_A_09") && core::PlaceRowExclude(req, 1, "HIDDEN_B_09"), "exclude-all-with-reasons");
    const auto views = archive.Snapshot(); Counts(Find(views, id), 0, 0, 2, 0, 0, true);
    Details(Find(views, id), core::PlaceRequestState(req));
    Check(Find(views, id).attention() && !Find(views, id).active(), "all-excluded-final-not-success");
    Check(!host::PumpGame(), "no-excluded-spawn-work");
    std::weak_ptr<core::PlaceRequest> weak = req; req.reset();
    Check(!weak.expired() && archive.Snapshot().reports.size() == 1, "session-archive-retains-receipt-owner");
}
core::GroundHandle Begin(rp::GroundArchive& archive, const std::vector<int>& uids, uint64_t serial) {
    auto op = core::BeginGround(uids, serial, 9); archive.Observe(core::GroundStateOf(op)); return op;
}
bool Apply(const core::GroundHandle& op, float dy) {
    std::vector<core::MoveReq> moves;
    for (const auto& m : core::GroundStateOf(op).members) {
        const auto& b = m.before; moves.push_back({ b.uid, { b.pos.x, b.pos.y + dy, b.pos.z }, b.rot, b.scale });
    }
    return core::GroundApply(op, moves);
}
void GroundSiblings() {
    const int a = Spawn(0), b = Spawn(4), c = Spawn(20), d = Spawn(30);
    rp::GroundArchive archive;
    auto partial = Begin(archive, { a, b }, 101), noSurface = Begin(archive, { c }, 102), success = Begin(archive, { d }, 103);
    const auto initial = core::GroundStateOf(partial);
    const auto partialId = initial.id, noId = core::GroundStateOf(noSurface).id, successId = core::GroundStateOf(success).id;
    Check(archive.Snapshot().order.newest == successId && archive.Snapshot().order.olderActive.size() == 2, "all-admitted-siblings-visible");
    // Actual native no-hit result; the editor policy turns that result into the same core reason.
    Require(core::GroundTicket(noSurface, { 20, 20, 0 }, 40) != 0, "no-surface-probe-admitted");
    host::PumpPhysics(); Drain(); core::GroundHit hit;
    Require(core::GroundPoll(noSurface, &hit) && hit.done && !hit.hit, "native-no-hit-observed");
    core::GroundCancel(noSurface, "no-surface"); archive.Observe(core::GroundStateOf(noSurface));
    refusedMoves.insert(Record(b).obj);
    Require(Apply(partial, -3) && Apply(success, 0), "partial-and-zero-displacement-sibling-queued");
    archive.Observe(core::GroundStateOf(partial)); archive.Observe(core::GroundStateOf(success));
    Require(host::PumpGameAt(1), "newest-sibling-finishes-first"); archive.Observe(core::GroundStateOf(success));
    auto views = archive.Snapshot();
    Counts(Find(views, successId), 1, 0, 0, true);
    Check(Find(views, successId).receipt.members[0].before.pos.y == Find(views, successId).receipt.members[0].after.pos.y, "zero-displacement-is-accepted");
    Check(views.order.newest == successId && Contains(views.order.olderActive, partialId) && views.order.earlierAttentionCount == 1, "pending-sibling-and-no-surface-warning-visible");
    bool observedApplying = false;
    {
        ObserveMove observe([&](uintptr_t h) {
            if (h != Record(b).obj) return;
            observedApplying = true;
            const auto applying = core::GroundStateOf(partial); archive.Observe(applying);
            const auto during = archive.Snapshot();
            Check(applying.state == core::GroundApplying, "actual-applying-boundary");
            Counts(Find(during, partialId), 1, 0, 1, false);
        });
        Require(host::PumpGame(), "partial-native-movement-completes");
    }
    Require(observedApplying, "member-event-was-observed");
    const auto terminal = core::GroundBarrier({ partial, noSurface, success }, false);
    for (const auto& receipt : terminal) archive.Observe(receipt);
    views = archive.Snapshot();
    Counts(Find(views, partialId), 1, 1, 0, true); Counts(Find(views, noId), 0, 1, 0, true);
    const auto& p = Find(views, partialId); const auto& n = Find(views, noId);
    Check(p.receipt.reason == "partial" && p.receipt.members[1].reason == "move-refused" && p.receipt.members[0].reason == "applied", "exact-partial-member-machine-reasons");
    Check(n.receipt.reason == "no-surface" && n.receipt.members[0].reason == "no-surface" && n.receipt.probe != 0, "exact-no-surface-and-probe-retained");
    Check(p.receipt.serial == 101 && p.receipt.branch == 9 && p.receipt.epoch == initial.epoch && p.receipt.members[1].before.uid == b, "origin-and-member-identity-retained");
    Check(views.order.newest == successId && views.order.earlierAttentionCount == 2 && views.order.olderActive.empty(), "late-partial-cannot-replace-success-sibling");
    Require(core::GroundReconcile({ partial, noSurface, success }), "real-core-reconciliation");
    for (const auto& op : { partial, noSurface, success }) archive.Observe(core::GroundStateOf(op));
    archive.Observe(initial); // replay of an older observation must not regress an archived terminal result
    views = archive.Snapshot();
    Check(Find(views, partialId).receipt.state == core::GroundSettled && Find(views, noId).receipt.state == core::GroundFailed, "archive-keeps-original-terminal-state");
    const auto notice = core::GroundNotice(); const int calls = engineCalls;
    const auto livePartial = core::GroundStateOf(partial);
    views.reports.clear(); auto copied = terminal.front(); copied.members.clear(); copied.reason.clear();
    partial.reset(); noSurface.reset(); success.reset();
    views = archive.Snapshot();
    Check(views.reports.size() == 3 && Find(views, partialId).receipt.reason == "partial" && Find(views, noId).receipt.reason == "no-surface", "archive-independent-of-caller-values-and-handles");
    Check(engineCalls == calls && core::GroundNotice() == notice && livePartial.state == core::GroundReconciled && host::GroundLeaseCount() == 0, "projection-no-operational-side-effects");
    Check(Record(a).pos.y == 7 && Record(b).pos.y == 10 && Record(d).pos.y == 10, "native-outcomes-not-rewritten-by-projection");
}
void GroundCancelEpochAndOrder() {
    const int a = Spawn(0), b = Spawn(4);
    auto old = core::BeginGround({ a }, 501, 17), newer = core::BeginGround({ b }, 502, 17);
    const auto oldId = core::GroundStateOf(old).id, newId = core::GroundStateOf(newer).id;
    rp::GroundArchive archive;
    archive.Observe(core::GroundStateOf(newer)); archive.Observe(core::GroundStateOf(old));
    Check(archive.Snapshot().order.newest == newId && Contains(archive.Snapshot().order.olderActive, oldId), "ground-admission-order-not-first-observation-order");
    core::GroundCancel(newer); archive.Observe(core::GroundStateOf(newer));
    core::InvalidateGroundWorld(); archive.Observe(core::GroundStateOf(old));
    auto views = archive.Snapshot(); Counts(Find(views, oldId), 0, 1, 0, true); Counts(Find(views, newId), 0, 1, 0, true);
    Check(Find(views, newId).receipt.reason == "canceled" && Find(views, newId).receipt.members[0].reason == "canceled", "canceled-reason-preserved");
    Check(Find(views, oldId).receipt.reason == "epoch-invalidated" && Find(views, oldId).receipt.members[0].reason == "epoch-invalidated", "epoch-reason-preserved");
    Check(views.order.newest == newId && views.order.earlierAttentionCount == 1, "older-epoch-result-never-becomes-newest");
}
void Retention() {
    rp::PlacementArchive placements; rp::GroundArchive grounds;
    rp::Id firstPlace = 0, firstGround = 0, lastPlace = 0, lastGround = 0;
    constexpr int count = 257; // capacity challenge, NOT a production cap
    for (int i = 0; i < count; ++i) {
        auto req = core::BeginPlaceRequest({ prefab }); core::PlaceRowExclude(req, 0, "RETAIN_09");
        lastPlace = placements.Admit(req, "same");
        auto op = core::BeginGround({}, static_cast<uint64_t>(i + 1), 1);
        const auto view = core::GroundStateOf(op); grounds.Observe(view); lastGround = view.id;
        if (!i) { firstPlace = lastPlace; firstGround = lastGround; }
    }
    const auto p = placements.Snapshot(); const auto g = grounds.Snapshot();
    Check(p.reports.size() == count && g.reports.size() == count, "no-numeric-eviction");
    Check(p.order.newest == lastPlace && g.order.newest == lastGround, "retention-newest-ids");
    Check(p.order.earlierCompleted.size() == count - 1 && g.order.earlierCompleted.size() == count - 1 &&
          p.order.earlierAttentionCount == count - 1 && g.order.earlierAttentionCount == count - 1, "retention-disclosure-counts");
    Check(Find(p, firstPlace).receipt.rows[0].reason == "RETAIN_09" && Find(g, firstGround).receipt.reason == "refused", "oldest-reasons-still-present");
    Check(Find(g, firstGround).attention() && Find(g, firstGround).final() && Find(g, firstGround).accepted == 0, "empty-refused-operation-not-success");
}
void Run(const char* id, void (*body)()) {
    cases.push_back({ id });
    try { Reset(); body(); Drain(); }
    catch (const std::exception& e) { Check(false, e.what()); }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "fixture directory required\n"); return 2; }
    root = argv[1]; host::SetModDir(root); host::OpenLog(root + "\\report_projection.log"); Install();
    Run("five-mixed-cleanup-lag", MixedCleanup);
    Run("same-name-reverse-completion", ReversedSameName);
    Run("all-excluded", AllExcluded);
    Run("ground-partial-no-surface-sibling", GroundSiblings);
    Run("ground-cancel-epoch-order", GroundCancelEpochAndOrder);
    Run("session-retention", Retention);
    Check(host::LockViolations() == 0, "no-core-queue-push-under-registry-lock");
    try {
        Reset();
        std::ostringstream out; out << "{\"assertions\":" << assertions << ",\"failures\":" << failures << ",\"cases\":[";
        for (size_t i = 0; i < cases.size(); ++i) { const auto& c = cases[i]; if (i) out << ',';
            out << "{\"id\":" << Json(c.id) << ",\"assertions\":" << c.assertions << ",\"failures\":" << c.failures
                << ",\"status\":\"" << (c.failures ? "FAIL" : "PASS") << "\"}";
        }
        out << "]}\n"; Write("cases.json", out.str());
        out.str(""); out.clear(); out << "[";
        for (size_t i = 0; i < trace.size(); ++i) { if (i) out << ','; out << trace[i]; } out << "]\n";
        Write("projections.json", out.str());
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
    std::printf("FAILURES=%d\nASSERTIONS=%d\n", failures, assertions);
    return failures ? 1 : 0;
}
