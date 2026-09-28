// PlacementResults host fixture (plan wb079-unified-77f68967, Task 10 / C5).
//
// The production core TU is compiled by production_core_host.cpp (WB_UNIFIED_HOST_TEST) and this fixture
// drives the REAL production spawn/retry/settlement paths through host::Seam(): SpawnAt -> the game-thread
// DoSpawn lane, the interactive server lane (template-free direct builder / captured-template replay), the
// client stand-in during a retry, the accepted plain-object fallback after the retry budget, and the
// production cancel semantics (HideUid) behind PlaceRequestCancel. Nothing here re-implements the registry,
// the queues, the retry budget or the request bookkeeping: only the native engine boundaries behind
// host::Seam() are substituted, and the oracle owns the handles those boundaries create/remove.
//
// The C5 contract asserted per case:
//   requested == attached + excluded + failed + canceled + pending   (at every observation)
//   every row settles exactly ONCE from a genuine final attachment, a final engine failure or a cancel
//   an intermediate stand-in during a retry is Pending, never a terminal attachment
//   two concurrent same-basename requests own separate rows; a late/reversed completion cannot steal or
//   resurrect another request's row; rejected handles are disposed on the thread that created them
//   PendingSpawns() is a queue diagnostic only - never a placed count
//
// Machine output: CHECK lines on stdout, one ASSERTIONS=<n> line, a per-case cases.json receipt, per-request
// rows/dispositions in placement_results_trace.json and the production log sink at
// <fixtureDir>\placement_results.log.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "production_host.h"   // host::Seam / host::PumpGame / host::PumpServer / lock probe

namespace core {
#ifdef WB_UNIFIED_HOST_TEST
// D1 replay-revalidation interleave seam, defined in the production TU (cdmodkit.cpp).
// Declared here (not in core.h) so no shipped header changes for a host-only probe.
extern std::function<void()> g_replayRevalidateProbe;
#endif
}

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

// ---- seam oracle -----------------------------------------------------------------------------------------
namespace oracle {
struct Obj { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1; uintptr_t actor = 0; bool live = true; };

std::map<uintptr_t, Obj> objects;
std::vector<uintptr_t> createdOrder;      // scene objects created, in creation order
std::set<uintptr_t> disposed;             // handles remove() disposed
std::set<uintptr_t> actorsLive, actorsRemoved;
uintptr_t next = 0x10000;
int createCalls = 0, creates = 0, removes = 0, moveInPlaceCalls = 0, liveMoveCalls = 0;
int directCalls = 0, directRefusals = 0, replayCalls = 0, replayRefusals = 0, actorRemoveCalls = 0;
bool templateReady = false, refuseDirect = false;
int refuseCreateFrom = 0;                  // 1-based create call ordinal that fails and every later one (0 = never)
bool lockFreeDuringEngineCalls = true;
std::function<void()> duringCreate, duringDirect, duringReplay;

void Reset() {
    objects.clear(); createdOrder.clear(); disposed.clear(); actorsLive.clear(); actorsRemoved.clear();
    next = 0x10000; createCalls = creates = removes = moveInPlaceCalls = liveMoveCalls = 0;
    directCalls = directRefusals = replayCalls = replayRefusals = actorRemoveCalls = 0;
    templateReady = false; refuseCreateFrom = 0; refuseDirect = false; lockFreeDuringEngineCalls = true;
    duringCreate = duringDirect = duringReplay = {};
}
uintptr_t MakeActor() { const uintptr_t a = next++; actorsLive.insert(a); return a; }
uintptr_t MakeObj(const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t actor) {
    const uintptr_t h = next++;
    objects.emplace(h, Obj{ prefab, pos, rot, scale, actor, true });
    createdOrder.push_back(h);
    return h;
}
bool Live(uintptr_t h) { auto it = objects.find(h); return it != objects.end() && it->second.live; }
bool DisposedHandle(uintptr_t h) { return disposed.count(h) != 0; }
std::string Describe(uintptr_t h) {
    auto it = objects.find(h);
    char b[200];
    if (it == objects.end()) { std::snprintf(b, sizeof b, "handle=0x%llx unknown", (unsigned long long)h); return b; }
    std::snprintf(b, sizeof b, "handle=0x%llx live=%d prefab=%s pos=(%.1f %.1f %.1f)", (unsigned long long)h,
                  it->second.live ? 1 : 0, it->second.prefab.c_str(), it->second.pos.x, it->second.pos.y, it->second.pos.z);
    return b;
}
int LiveCount() { int n = 0; for (auto& kv : objects) if (kv.second.live) ++n; return n; }
}   // namespace oracle

// ---- lock-discipline probe: a second thread try-locks the production registry lock on request ------------
class LockProbe {
public:
    void Start() {
        worker_ = std::thread([this] {
            for (;;) {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return request_ || quit_; });
                if (quit_) return;
                request_ = false;
                free_ = host::RegistryLockFree();   // never called from the lock owner: this is the probe thread
                done_ = true;
                l.unlock();
                cv_.notify_all();
            }
        });
    }
    bool CheckFree() {
        std::unique_lock<std::mutex> l(m_);
        request_ = true; done_ = false;
        cv_.notify_all();
        if (!cv_.wait_for(l, std::chrono::seconds(5), [this] { return done_; })) return false;   // bounded handshake
        return free_;
    }
    void Stop() {
        { std::lock_guard<std::mutex> l(m_); quit_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
private:
    std::mutex m_; std::condition_variable cv_;
    bool request_ = false, done_ = false, free_ = false, quit_ = false;
    std::thread worker_;
};

// ---- fixture state ---------------------------------------------------------------------------------------
int g_assertions = 0, g_failures = 0;
std::string g_case = "startup";
std::vector<SpawnedObj> g_list;
LockProbe g_probe;
std::string g_fixtureDir;
struct CaseRec { std::string id; int assertions = 0, failures = 0; };
std::vector<CaseRec> g_cases;
std::string g_trace;   // machine trace of per-request rows / engine dispositions

void Check(const char* label, bool ok, const std::string& observed = "") {
    ++g_assertions;
    if (!g_cases.empty()) { ++g_cases.back().assertions; if (!ok) ++g_cases.back().failures; }
    if (!ok) ++g_failures;
    std::printf("CHECK %s %s%s%s\n", ok ? "PASS" : "FAIL", label, observed.empty() ? "" : " | ", observed.c_str());
    core::Log("[placement_results] %s %s %s%s%s", g_case.c_str(), ok ? "PASS" : "FAIL", label,
              observed.empty() ? "" : " | ", observed.c_str());
}
void Require(bool ok, const char* label, const std::string& observed = "") {
    Check(label, ok, observed);
    if (!ok) throw std::runtime_error(std::string(label) + (observed.empty() ? "" : (" | " + observed)));
}
void Trace(const std::string& line) {
    g_trace += line;
    g_trace += "\n";
    core::Log("[placement_results] trace %s", line.c_str());
    std::printf("TRACE %s\n", line.c_str());
}
void BeginCase(const char* id) {
    g_case = id;
    g_cases.push_back(CaseRec{ id, 0, 0 });
    g_list.clear();
    core::Log("[placement_results] ==== case %s ====", id);
    std::printf("CASE %s\n", id);
}

// ---- deterministic event gate for the C5 cleanup-ordering case (bounded condition wait, no sleeps) ---------
class Gate {
public:
    void Arrive() { std::unique_lock<std::mutex> l(m_); entered_ = true; cv_.notify_all(); if (!cv_.wait_for(l, std::chrono::seconds(10), [this] { return released_; })) throw std::runtime_error("gate timeout"); }
    bool Entered() { std::unique_lock<std::mutex> l(m_); return cv_.wait_for(l, std::chrono::seconds(10), [this] { return entered_; }); }
    void Release() { { std::lock_guard<std::mutex> l(m_); released_ = true; } cv_.notify_all(); }
private:
    std::mutex m_; std::condition_variable cv_; bool entered_ = false, released_ = false;
};
Gate* g_moveGate = nullptr;

// ---- engine seam -----------------------------------------------------------------------------------------
void InstallSeam() {
    host::Engine& e = host::Seam();
    e.ready = true;
    e.createGeneric = [](const std::string& prefab, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::createCalls;
        if (oracle::duringCreate) oracle::duringCreate();
        if (oracle::refuseCreateFrom && oracle::createCalls >= oracle::refuseCreateFrom) return 0;   // deterministic final engine failure
        ++oracle::creates;
        return oracle::MakeObj(prefab, pos, rot, scale, 0);
    };
    e.remove = [](uintptr_t h) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::removes;
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.live = false;
        oracle::disposed.insert(h);
        return true;
    };
    e.moveInPlace = [](uintptr_t h, Vec3 pos, Rot rot, float scale) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::moveInPlaceCalls;
        if (g_moveGate) g_moveGate->Arrive();
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.pos = pos; it->second.rot = rot; it->second.scale = scale;
        return true;
    };
    e.liveMove = [](uintptr_t h, Vec3 pos, Rot rot, float scale) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::liveMoveCalls;
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.pos = pos; it->second.rot = rot; it->second.scale = scale;
        return true;
    };
    e.removeActor = [](uintptr_t a) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::actorRemoveCalls;
        if (a && oracle::actorsRemoved.insert(a).second) return true;
        return false;
    };
    e.directGimmick = [](const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::directCalls;
        if (oracle::duringDirect) oracle::duringDirect();
        if (oracle::refuseDirect) { ++oracle::directRefusals; return false; }
        const uintptr_t a = oracle::MakeActor();
        *so = oracle::MakeObj(prefab, pos, rot, scale, a);
        *actor = a;
        return true;
    };
    e.replay = [](int tmpl, const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor) -> bool {
        (void)tmpl;
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::replayCalls;
        if (oracle::duringReplay) oracle::duringReplay();
        if (oracle::replayRefusals > 0) { --oracle::replayRefusals; return false; }
        const uintptr_t a = oracle::MakeActor();
        *so = oracle::MakeObj(prefab, pos, rot, scale, a);
        *actor = a;
        return true;
    };
    e.templateReady = [] { return oracle::templateReady; };
}

// ---- pumping ---------------------------------------------------------------------------------------------
void PumpGame_() {
    int guard = 0;
    while (host::PumpGame()) { if (++guard > 4096) throw std::runtime_error("the production game queue did not settle"); }
}
void PumpServerOnce() { host::PumpServer(); }
// Bounded, sleep-free settling: alternate the game and server queues a fixed number of rounds.
void PumpRounds(int rounds = 8) {
    for (int r = 0; r < rounds; ++r) { PumpGame_(); host::PumpServer(); PumpGame_(); }
}
void Snapshot() { g_list = core::Spawned(); }
const SpawnedObj* Rec(int uid) {
    for (const SpawnedObj& o : g_list) if (o.uid == uid) return &o;
    return nullptr;
}

// ---- request helpers -------------------------------------------------------------------------------------
std::string VStr(const core::PlaceRequestView& v) {
    char b[256];
    std::snprintf(b, sizeof b, "requested=%d attached=%d excluded=%d failed=%d canceled=%d pending=%d settled=%d cancelRequested=%d cleanupPending=%d",
                  v.requested, v.attached, v.excluded, v.failed, v.canceled, v.pending, v.settled ? 1 : 0, v.requestCanceled ? 1 : 0, v.cleanupPending ? 1 : 0);
    return b;
}
const core::PlaceRow* Row(const core::PlaceRequestView& v, int rowId) {
    for (const core::PlaceRow& r : v.rows) if (r.rowId == rowId) return &r;
    return nullptr;
}
const char* StateName(int s) {
    switch (s) {
    case core::PlacePending: return "Pending";
    case core::PlaceAttached: return "Attached";
    case core::PlaceExcluded: return "Excluded";
    case core::PlaceFailed: return "Failed";
    case core::PlaceCanceled: return "Canceled";
    }
    return "?";
}
const char* LaneName(int l) {
    switch (l) {
    case core::PlaceLaneGeneric: return "Generic";
    case core::PlaceLaneDirect: return "Direct";
    case core::PlaceLaneReplay: return "Replay";
    case core::PlaceLanePlain: return "Plain";
    case core::PlaceLaneStandin: return "Standin";
    }
    return "None";
}
std::string RStr(const core::PlaceRow& r) {
    char b[256];
    std::snprintf(b, sizeof b, "row=%d prefab=\"%s\" state=%s lane=%s uid=%d obs=%d removedAfterAttach=%d cleanupPending=%d reason=\"%s\"",
                  r.rowId, r.prefab.c_str(), StateName(r.state), LaneName(r.lane), r.uid, r.attachObservations,
                  r.removedAfterAttach ? 1 : 0, r.cleanupPending ? 1 : 0, r.reason.c_str());
    return b;
}
// The C5 outcome equation, checked at every observation (both machine-state and UI-projection forms).
void CheckEquation(const char* label, const core::PlaceRequestView& v) {
    const bool eq = v.requested == v.attached + v.excluded + v.failed + v.canceled + v.pending;
    Check(label, eq, VStr(v));
    const bool proj = v.displayedPlaced() + v.displayedExcluded() + v.displayedPending() == v.requested;
    Check("projection-equation", proj, VStr(v));
}
void TraceRequest(const char* tag, const core::PlaceRequestHandle& req) {
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    Trace(std::string(tag) + " " + VStr(v));
    for (const core::PlaceRow& r : v.rows) Trace(std::string(tag) + " " + RStr(r));
}
// A request must be fully settled before it is presented: no pending row and no outstanding cancel cleanup.
void CheckSettled(const char* label, const core::PlaceRequestHandle& req) {
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    Check(label, v.settled && v.pending == 0 && !v.cleanupPending, VStr(v));
}
void CheckNoQueueUnderLock(const char* label) { Check(label, host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations())); }

// =========================================================================================================
// happy: generic lane + interactive lane + a preflight exclusion in ONE request
// =========================================================================================================
static void CaseMixedLanes() {
    BeginCase("mixed-lanes");
    host::SetDirectGimmick(true);          // the interactive lane uses the template-free server builder
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/plain_a.prefab", "/object/cd_gimmick/lamp_a.prefab", "" });
    const int u0 = core::SubmitPlaceRow(req, 0, { 1, 0, 1 });
    const int u1 = core::SubmitPlaceRow(req, 1, { 2, 0, 2 });
    const int u2 = core::SubmitPlaceRow(req, 2, { 3, 0, 3 });
    Check("rows-admitted", u0 != 0 && u1 != 0 && u2 == 0, "u0=" + std::to_string(u0) + " u1=" + std::to_string(u1) + " u2=" + std::to_string(u2));
    PumpRounds(4);
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("mixed-lanes", req);
    const core::PlaceRow* r0 = Row(v, 0);
    const core::PlaceRow* r1 = Row(v, 1);
    const core::PlaceRow* r2 = Row(v, 2);
    Check("generic-row-attached", r0 && r0->state == core::PlaceAttached && r0->lane == core::PlaceLaneGeneric && r0->uid == u0, r0 ? RStr(*r0) : "missing");
    Check("interactive-row-attached-direct", r1 && r1->state == core::PlaceAttached && r1->lane == core::PlaceLaneDirect && r1->uid == u1, r1 ? RStr(*r1) : "missing");
    Check("empty-prefab-excluded-preflight", r2 && r2->state == core::PlaceExcluded && r2->lane == core::PlaceLaneNone && !r2->reason.empty(), r2 ? RStr(*r2) : "missing");
    Check("counts", v.requested == 3 && v.attached == 2 && v.excluded == 1 && v.failed == 0 && v.canceled == 0 && v.pending == 0, VStr(v));
    CheckEquation("mixed-lanes-equation", v);
    CheckSettled("mixed-lanes-settled", req);
    Check("engine-attachments-are-real", oracle::LiveCount() == 2, "live=" + std::to_string(oracle::LiveCount()));
    CheckNoQueueUnderLock("mixed-lanes-no-queue-under-lock");
}

// happy: the captured-template replay lane attaches the row
static void CaseReplayLane() {
    BeginCase("replay-lane");
    host::SetDirectGimmick(false);         // the interactive lane must fall through to the replay
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_r.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 4, 0, 4 });
    PumpRounds(4);
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("replay-lane", req);
    const core::PlaceRow* r = Row(v, 0);
    Check("replay-row-attached", r && r->state == core::PlaceAttached && r->lane == core::PlaceLaneReplay && r->uid == uid, r ? RStr(*r) : "missing");
    Check("replay-attempted", oracle::replayCalls >= 1, "replay=" + std::to_string(oracle::replayCalls));
    Check("counts", v.requested == 1 && v.attached == 1 && v.pending == 0, VStr(v));
    CheckEquation("replay-lane-equation", v);
    CheckSettled("replay-lane-settled", req);
    CheckNoQueueUnderLock("replay-lane-no-queue-under-lock");
}

// happy: one refused replay -> production stand-in (still Pending) -> retry attaches the SAME row
static void CaseRetrySameRow() {
    BeginCase("retry-same-row");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 1;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_s.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 5, 0, 5 });
    PumpServerOnce();                       // replay refused: the production retry queues the client stand-in
    PumpGame_();
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        const core::PlaceRow* r = Row(v, 0);
        Check("standin-is-not-terminal", r && r->state == core::PlacePending && r->lane == core::PlaceLaneNone && r->attachObservations == 1 && v.pending == 1, r ? RStr(*r) : "missing");
        Check("standin-row-equation", v.requested == 1 && v.pending == 1, VStr(v));
        Check("standin-record-visible", Rec(uid) && !Rec(uid)->hidden && Rec(uid)->standin && Rec(uid)->obj != 0, Rec(uid) ? "obj=" + oracle::Describe(Rec(uid)->obj) : "no record");
        TraceRequest("retry-same-row-after-standin", req);
    }
    const uintptr_t standin = Rec(uid) ? Rec(uid)->obj : 0;
    PumpRounds(4);                          // retry with a fresh template: attaches the server object
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("retry-same-row-settled", req);
    const core::PlaceRow* r = Row(v, 0);
    Check("retry-row-attached-once", r && r->state == core::PlaceAttached && r->lane == core::PlaceLaneReplay && r->uid == uid && r->attachObservations == 2, r ? RStr(*r) : "missing");
    Check("retry-does-not-double-count", v.requested == 1 && v.attached == 1 && v.pending == 0, VStr(v));
    Check("standin-disposed", standin != 0 && oracle::DisposedHandle(standin), oracle::Describe(standin));
    Check("uid-stable", Rec(uid) && Rec(uid)->uid == uid && !Rec(uid)->hidden && !Rec(uid)->standin, Rec(uid) ? "hidden=" + std::to_string(Rec(uid)->hidden) : "no record");
    CheckEquation("retry-same-row-equation", v);
    CheckSettled("retry-same-row-settled-flag", req);
    CheckNoQueueUnderLock("retry-same-row-no-queue-under-lock");
}

// B1: a refused retry-backed stand-in must NOT hide the record or strand the row Pending.
// Schedule: one replay refusal -> production stand-in whose FIRST create is refused ->
// the row stays Pending on a visible, non-stand-in record with no object -> the next
// server step retries the replay and attaches the SAME uid on the Replay lane.
// Permanent refusal (replay always refused, every create refused) must still settle
// terminally through the refused plain fallback instead of stranding Pending.
static void CaseRefusedStandinRetry() {
    BeginCase("refused-standin-retry");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 1;            // exactly one refused replay, then the retry succeeds
    oracle::refuseCreateFrom = 1;          // the FIRST stand-in create is refused ...
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b1.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 50, 0, 50 });
    Require(uid != 0, "b1-row-admitted", "");
    PumpServerOnce();                       // replay refused: the production retry queues the client stand-in
    PumpGame_();                            // the stand-in create is refused
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("refused-standin-retry-after-refusal", req);
        const core::PlaceRow* r = Row(v, 0);
        Check("refused-standin-stays-pending", r && r->state == core::PlacePending && r->lane == core::PlaceLaneNone && r->attachObservations == 0, r ? RStr(*r) : "missing");
        Check("refused-standin-record-visible", Rec(uid) && !Rec(uid)->hidden && !Rec(uid)->standin && Rec(uid)->obj == 0, Rec(uid) ? ("hidden=" + std::to_string(Rec(uid)->hidden) + " standin=" + std::to_string(Rec(uid)->standin)) : "no record");
        Check("refused-standin-equation", v.requested == 1 && v.pending == 1 && v.failed == 0, VStr(v));
    }
    oracle::refuseCreateFrom = 0;           // ... only the first stand-in was refused
    PumpRounds(4);                          // the retry replays with a fresh template and attaches
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("refused-standin-retry-settled", req);
        const core::PlaceRow* r = Row(v, 0);
        Check("retry-attached-same-uid-replay", r && r->state == core::PlaceAttached && r->lane == core::PlaceLaneReplay && r->uid == uid && r->attachObservations == 1, r ? RStr(*r) : "missing");
        Check("retry-counted-once", v.requested == 1 && v.attached == 1 && v.pending == 0, VStr(v));
        Check("retry-record-visible", Rec(uid) && !Rec(uid)->hidden && !Rec(uid)->standin && Rec(uid)->obj != 0, Rec(uid) ? "obj=" + oracle::Describe(Rec(uid)->obj) : "no record");
        CheckEquation("refused-standin-retry-equation", v);
        CheckSettled("refused-standin-retry-settled-flag", req);
    }
    // Permanent refusal: every replay refused and every create refused. The retry budget
    // runs out and the accepted plain fallback is refused too: one terminal Failed, never Pending.
    oracle::Reset();
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 100;
    oracle::refuseCreateFrom = 1;
    const core::PlaceRequestHandle reqP = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b1p.prefab" });
    const int uidP = core::SubmitPlaceRow(reqP, 0, { 51, 0, 51 });
    Require(uidP != 0, "b1-permanent-row-admitted", "");
    PumpRounds(8);
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(reqP);
        TraceRequest("refused-standin-permanent", reqP);
        const core::PlaceRow* r = Row(v, 0);
        Check("permanent-refusal-is-failed", r && r->state == core::PlaceFailed && !r->reason.empty(), r ? RStr(*r) : "missing");
        Check("permanent-refusal-counted-once", v.requested == 1 && v.attached == 0 && v.failed == 1 && v.pending == 0, VStr(v));
        Check("permanent-refusal-tombstone", Rec(uidP) && Rec(uidP)->hidden && Rec(uidP)->obj == 0, Rec(uidP) ? "hidden=" + std::to_string(Rec(uidP)->hidden) : "no record");
        CheckEquation("refused-standin-permanent-equation", v);
        CheckSettled("refused-standin-permanent-settled", reqP);
    }
    CheckNoQueueUnderLock("refused-standin-no-queue-under-lock");
}

// adversarial: cancel the whole request BEFORE anything attached; queued work must not resurrect rows
static void CaseCancelFirst() {
    BeginCase("cancel-first");
    host::SetDirectGimmick(false);
    oracle::templateReady = false;         // the interactive row would wait for a template (honest pending)
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_c.prefab", "/object/plain_c.prefab" });
    const int ug = core::SubmitPlaceRow(req, 0, { 6, 0, 6 });
    const int up = core::SubmitPlaceRow(req, 1, { 7, 0, 7 });
    Check("rows-admitted", ug != 0 && up != 0, "ug=" + std::to_string(ug) + " up=" + std::to_string(up));
    const int moved = core::PlaceRequestCancel(req);     // nothing attached yet
    Check("cancel-moved-both", moved == 2, "moved=" + std::to_string(moved));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("cancel-first", req);
        Check("no-attachments", v.attached == 0 && v.pending == 0 && v.canceled == 2, VStr(v));
        Check("cancel-flag", v.requestCanceled, VStr(v));
        CheckEquation("cancel-first-equation", v);
        CheckSettled("cancel-first-settled", req);
    }
    PumpRounds(6);                          // drain the queued generic job and one server step
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("cancel-first-after-pump", req);
    Check("queued-generic-never-created", oracle::creates == 0, "creates=" + std::to_string(oracle::creates));
    Check("queued-interactive-never-replayed", oracle::replayCalls == 0, "replay=" + std::to_string(oracle::replayCalls));
    Check("rows-still-canceled", v.canceled == 2 && v.attached == 0 && v.pending == 0, VStr(v));
    Check("records-hidden-not-materialized", Rec(ug) && Rec(ug)->hidden && Rec(ug)->obj == 0 && Rec(up) && Rec(up)->hidden && Rec(up)->obj == 0, "ug=" + std::to_string(Rec(ug) ? Rec(ug)->hidden : -1) + " up=" + std::to_string(Rec(up) ? Rec(up)->hidden : -1));
    CheckEquation("cancel-first-after-pump-equation", v);
    CheckNoQueueUnderLock("cancel-first-no-queue-under-lock");
}

// adversarial: two same-basename requests started concurrently cannot share a result
static void CaseStartSameName() {
    BeginCase("start-same-name");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    const core::PlaceRequestHandle reqA = core::BeginPlaceRequest({ "/object/a/lamp.prefab" });
    const core::PlaceRequestHandle reqB = core::BeginPlaceRequest({ "/object/b/lamp.prefab" });

    // The second request is submitted from a helper thread while the first request's create callback is on
    // the stack - a genuinely concurrent, same-basename submission. The events are subscribed before the
    // first submission and every wait is bounded (no sleep/poll).
    std::mutex m; std::condition_variable cv; bool go = false, done = false, fired = false, helperAdmitted = false; int uidB = 0;
    std::thread helper([&] {
        std::unique_lock<std::mutex> l(m);
        helperAdmitted = cv.wait_for(l, std::chrono::seconds(5), [&] { return go; });
        if (!helperAdmitted) return;
        l.unlock();
        uidB = core::SubmitPlaceRow(reqB, 0, { 21, 0, 21 });
        l.lock();
        done = true;
        l.unlock();
        cv.notify_all();
    });
    oracle::duringCreate = [&] {
        if (fired) return;
        fired = true;
        { std::lock_guard<std::mutex> l(m); go = true; }
        cv.notify_all();
        std::unique_lock<std::mutex> l(m);
        Check("concurrent-native-callback-handshake", cv.wait_for(l, std::chrono::seconds(5), [&] { return done; }));
    };
    const int uidA = core::SubmitPlaceRow(reqA, 0, { 20, 0, 20 });
    PumpRounds(2);                          // runs A's create callback, which lets B submit concurrently
    {
        std::lock_guard<std::mutex> l(m);
        Check("concurrent-submit-completed", done && uidB != 0, "done=" + std::to_string(done) + " uidB=" + std::to_string(uidB));
    }
    oracle::duringCreate = {};
    if (helper.joinable()) helper.join();
    Check("concurrent-helper-admission-handshake", helperAdmitted);
    PumpRounds(4);
    Snapshot();
    const core::PlaceRequestView a = core::PlaceRequestState(reqA);
    const core::PlaceRequestView b = core::PlaceRequestState(reqB);
    TraceRequest("start-same-name-A", reqA);
    TraceRequest("start-same-name-B", reqB);
    const core::PlaceRow* ra = Row(a, 0);
    const core::PlaceRow* rb = Row(b, 0);
    Check("distinct-rows", uidA != 0 && uidB != 0 && uidA != uidB, "uidA=" + std::to_string(uidA) + " uidB=" + std::to_string(uidB));
    Check("each-own-prefab", ra && ra->prefab == "/object/a/lamp.prefab" && rb && rb->prefab == "/object/b/lamp.prefab", (ra && rb) ? ra->prefab + " / " + rb->prefab : "missing");
    Check("each-own-terminal", ra && rb && ra->state == core::PlaceAttached && rb->state == core::PlaceAttached && ra->uid == uidA && rb->uid == uidB, (ra && rb) ? RStr(*ra) + " || " + RStr(*rb) : "missing");
    Check("each-counts-one", a.attached == 1 && a.pending == 0 && b.attached == 1 && b.pending == 0, VStr(a) + " || " + VStr(b));
    CheckEquation("start-same-name-equation-A", a);
    CheckEquation("start-same-name-equation-B", b);
    CheckSettled("start-same-name-A-settled", reqA);
    CheckSettled("start-same-name-B-settled", reqB);
    Check("two-real-objects", oracle::LiveCount() == 2, "live=" + std::to_string(oracle::LiveCount()));
    CheckNoQueueUnderLock("start-same-name-no-queue-under-lock");
}

// adversarial: the FIRST request's completion arrives AFTER the second one (reversed callbacks)
static void CaseLateFirst() {
    BeginCase("late-first");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    const core::PlaceRequestHandle reqA = core::BeginPlaceRequest({ "/object/a/lamp.prefab" });
    const core::PlaceRequestHandle reqB = core::BeginPlaceRequest({ "/object/b/lamp.prefab" });
    const int uidA = core::SubmitPlaceRow(reqA, 0, { 31, 0, 31 });
    const int uidB = core::SubmitPlaceRow(reqB, 0, { 32, 0, 32 });
    Check("rows-admitted", uidA != 0 && uidB != 0, "uidA=" + std::to_string(uidA) + " uidB=" + std::to_string(uidB));
    // While A's create callback is on the stack, finish B completely; A's own completion is therefore late.
    bool reentered = false;
    oracle::duringCreate = [&] {
        if (reentered) return;
        reentered = true;
        host::PumpGame();                 // runs B's queued DoSpawn to completion before A returns
    };
    host::PumpGame();                     // dispatch A's DoSpawn; B completes inside its callback
    oracle::duringCreate = {};
    PumpRounds(3);
    Snapshot();
    const core::PlaceRequestView a = core::PlaceRequestState(reqA);
    const core::PlaceRequestView b = core::PlaceRequestState(reqB);
    TraceRequest("late-first-A", reqA);
    TraceRequest("late-first-B", reqB);
    const core::PlaceRow* ra = Row(a, 0);
    const core::PlaceRow* rb = Row(b, 0);
    Check("second-completed-first", oracle::createdOrder.size() == 2 && Rec(uidB) && oracle::createdOrder[0] == Rec(uidB)->obj, "order=" + std::to_string(oracle::createdOrder.size()));
    Check("late-row-kept-its-own-handle", ra && ra->state == core::PlaceAttached && ra->uid == uidA && Rec(uidA) && Rec(uidA)->obj == oracle::createdOrder[1], ra ? RStr(*ra) : "missing");
    Check("first-row-not-stolen", rb && rb->state == core::PlaceAttached && rb->uid == uidB && Rec(uidB) && Rec(uidB)->obj == oracle::createdOrder[0], rb ? RStr(*rb) : "missing");
    Check("each-counts-one", a.attached == 1 && b.attached == 1 && a.pending == 0 && b.pending == 0, VStr(a) + " || " + VStr(b));
    CheckEquation("late-first-equation-A", a);
    CheckEquation("late-first-equation-B", b);
    CheckSettled("late-first-A-settled", reqA);
    CheckSettled("late-first-B-settled", reqB);
    CheckNoQueueUnderLock("late-first-no-queue-under-lock");
}

// adversarial: every stand-in attach during the retry plus the final fallback = ONE terminal settlement
static void CaseDoubleComplete() {
    BeginCase("double-complete");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 3;            // the production retry budget (kGimmickMaxTries)
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_d.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 8, 0, 8 });
    PumpRounds(8);                          // three refusals -> two stand-ins -> the accepted plain fallback
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("double-complete", req);
    const core::PlaceRow* r = Row(v, 0);
    Check("many-attachments-one-terminal", r && r->attachObservations >= 2 && r->state == core::PlaceAttached, r ? RStr(*r) : "missing");
    Check("fallback-records-its-own-lane", r && r->lane == core::PlaceLanePlain, r ? RStr(*r) : "missing");
    Check("attached-counted-once", v.requested == 1 && v.attached == 1 && v.pending == 0 && v.failed == 0, VStr(v));
    Check("record-visible-after-fallback", Rec(uid) && !Rec(uid)->hidden && !Rec(uid)->gimmick && Rec(uid)->obj != 0, Rec(uid) ? "obj=" + oracle::Describe(Rec(uid)->obj) : "no record");
    CheckEquation("double-complete-equation", v);
    CheckSettled("double-complete-settled", req);
    CheckNoQueueUnderLock("double-complete-no-queue-under-lock");
}

// adversarial: an actual final engine failure is Failed, never a fabricated success
static void CaseFailedAttach() {
    BeginCase("failed-attach");
    host::SetDirectGimmick(false);
    oracle::templateReady = false;
    oracle::refuseCreateFrom = 1;          // (a) a plain record whose create is refused
    const core::PlaceRequestHandle reqA = core::BeginPlaceRequest({ "/object/plain_f.prefab" });
    const int uidA = core::SubmitPlaceRow(reqA, 0, { 9, 0, 9 });
    PumpRounds(3);
    Snapshot();
    {
        const core::PlaceRequestView a = core::PlaceRequestState(reqA);
        TraceRequest("failed-attach-A", reqA);
        const core::PlaceRow* ra = Row(a, 0);
        Check("plain-create-refusal-is-failed", ra && ra->state == core::PlaceFailed && ra->lane == core::PlaceLaneNone && !ra->reason.empty(), ra ? RStr(*ra) : "missing");
        Check("plain-failed-counted-once", a.requested == 1 && a.attached == 0 && a.failed == 1 && a.pending == 0, VStr(a));
        Check("plain-failed-record-hidden", Rec(uidA) && Rec(uidA)->hidden && Rec(uidA)->obj == 0, Rec(uidA) ? "hidden=" + std::to_string(Rec(uidA)->hidden) : "no record");
        CheckEquation("failed-attach-equation-A", a);
        CheckSettled("failed-attach-A-settled", reqA);
    }
    // (b) an interactive record that exhausts the retry budget (three stand-ins) and whose accepted plain
    // fallback create is then refused: one real failure, not a fabricated success and not a second terminal.
    oracle::Reset();
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 3;
    oracle::refuseCreateFrom = 3;          // allow the two stand-in creates, refuse the accepted fallback (call 3)
    const core::PlaceRequestHandle reqB = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_f.prefab" });
    const int uidB = core::SubmitPlaceRow(reqB, 0, { 10, 0, 10 });
    PumpRounds(8);
    Snapshot();
    const core::PlaceRequestView b = core::PlaceRequestState(reqB);
    TraceRequest("failed-attach-B", reqB);
    const core::PlaceRow* rb = Row(b, 0);
    Check("fallback-refusal-is-failed", rb && rb->state == core::PlaceFailed && !rb->reason.empty() && rb->attachObservations >= 2, rb ? RStr(*rb) : "missing");
    Check("interactive-failed-counted-once", b.requested == 1 && b.attached == 0 && b.failed == 1 && b.pending == 0, VStr(b));
    Check("failed-record-is-a-hidden-tombstone", Rec(uidB) && Rec(uidB)->hidden && Rec(uidB)->obj == 0, Rec(uidB) ? "hidden=" + std::to_string(Rec(uidB)->hidden) : "no record");
    CheckEquation("failed-attach-equation-B", b);
    CheckSettled("failed-attach-B-settled", reqB);
    CheckNoQueueUnderLock("failed-attach-no-queue-under-lock");
}

// adversarial: cancel an attached row + a pending row; the Attached receipt is kept, cleanup is tracked,
// and the canceled row can never be resurrected by a later template/callback
static void CaseProjectionCancelCleanup() {
    BeginCase("projection-cancel-cleanup");
    host::SetDirectGimmick(false);
    oracle::templateReady = false;         // the interactive row stays pending
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/plain_g.prefab", "/object/cd_gimmick/lamp_g.prefab" });
    const int up = core::SubmitPlaceRow(req, 0, { 11, 0, 11 });
    const int ug = core::SubmitPlaceRow(req, 1, { 12, 0, 12 });
    PumpRounds(3);                          // generic row attaches; interactive row waits without a template
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("projection-cancel-cleanup-attached", req);
        Check("one-attached-one-pending", v.attached == 1 && v.pending == 1 && v.canceled == 0, VStr(v));
        CheckEquation("before-cancel-equation", v);
    }
    const uintptr_t attachedHandle = Rec(up) ? Rec(up)->obj : 0;
    Check("attached-handle-live", attachedHandle != 0 && oracle::Live(attachedHandle), oracle::Describe(attachedHandle));
    const int moved = core::PlaceRequestCancel(req);
    Check("cancel-counts-attached-and-pending", moved == 2, "moved=" + std::to_string(moved));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("projection-cancel-cleanup-canceled", req);
        const core::PlaceRow* rp = Row(v, 0);
        const core::PlaceRow* rg = Row(v, 1);
        Check("attached-receipt-kept-and-flagged", rp && rp->state == core::PlaceAttached && rp->removedAfterAttach && rp->lane == core::PlaceLaneGeneric, rp ? RStr(*rp) : "missing");
        Check("pending-row-canceled", rg && rg->state == core::PlaceCanceled && !rg->reason.empty(), rg ? RStr(*rg) : "missing");
        Check("counts", v.requested == 2 && v.attached == 1 && v.canceled == 1 && v.pending == 0 && v.failed == 0, VStr(v));
        Check("projection-totals", v.displayedPlaced() == 1 && v.displayedExcluded() == 1 && v.displayedPending() == 0, VStr(v));
        Check("cleanup-pending-until-dispatch", v.cleanupPending && !v.settled, VStr(v));
        CheckEquation("after-cancel-equation", v);
    }
    PumpRounds(4);                          // the queued removal and its cleanup marker dispatch
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("projection-cancel-cleanup-drained", req);
        Check("attached-member-removed", oracle::DisposedHandle(attachedHandle), oracle::Describe(attachedHandle));
        Check("cleanup-settled", v.settled && !v.cleanupPending && v.pending == 0, VStr(v));
        Check("receipt-unchanged-by-cleanup", v.attached == 1 && v.canceled == 1, VStr(v));
        CheckEquation("drained-equation", v);
    }
    // A later genuine completion for the canceled row must not resurrect it.
    const int createsBefore = oracle::createCalls, replaysBefore = oracle::replayCalls;
    oracle::templateReady = true;
    PumpRounds(6);
    Snapshot();
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    const core::PlaceRow* rg = Row(v, 1);
    TraceRequest("projection-cancel-cleanup-after-template", req);
    Check("no-new-attachment-for-canceled-row", rg && rg->state == core::PlaceCanceled && rg->attachObservations == 0, rg ? RStr(*rg) : "missing");
    Check("canceled-record-still-hidden", Rec(ug) == nullptr || Rec(ug)->hidden, "");
    Check("no-engine-resurrection", v.attached == 1 && v.canceled == 1, VStr(v));
    (void)createsBefore; (void)replaysBefore;
    CheckEquation("late-template-equation", v);
    CheckNoQueueUnderLock("projection-cancel-cleanup-no-queue-under-lock");
}

// adversarial: cancel one request, then start a SAME-BASENAME request; the second owns its own result
static void CaseCancelThenSameName() {
    BeginCase("cancel-then-same-name");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    const core::PlaceRequestHandle reqA = core::BeginPlaceRequest({ "/object/a/lamp.prefab" });
    const int uidA = core::SubmitPlaceRow(reqA, 0, { 41, 0, 41 });
    core::PlaceRequestCancel(reqA);        // canceled first, before any pump
    const core::PlaceRequestHandle reqB = core::BeginPlaceRequest({ "/object/a/lamp.prefab" });
    const int uidB = core::SubmitPlaceRow(reqB, 0, { 42, 0, 42 });
    Check("distinct-records", uidA != 0 && uidB != 0 && uidA != uidB, "uidA=" + std::to_string(uidA) + " uidB=" + std::to_string(uidB));
    PumpRounds(4);
    Snapshot();
    const core::PlaceRequestView a = core::PlaceRequestState(reqA);
    const core::PlaceRequestView b = core::PlaceRequestState(reqB);
    TraceRequest("cancel-then-same-name-A", reqA);
    TraceRequest("cancel-then-same-name-B", reqB);
    Check("canceled-first-never-attached", a.attached == 0 && a.canceled == 1 && a.pending == 0, VStr(a));
    Check("second-attached-own-row", b.attached == 1 && b.canceled == 0 && b.pending == 0 && Row(b, 0) && Row(b, 0)->uid == uidB, VStr(b));
    Check("second-owns-its-handle", Rec(uidB) && !Rec(uidB)->hidden && Rec(uidB)->obj != 0 && oracle::Live(Rec(uidB)->obj), Rec(uidB) ? "obj=" + oracle::Describe(Rec(uidB)->obj) : "no record");
    Check("canceled-record-hidden", Rec(uidA) && Rec(uidA)->hidden && Rec(uidA)->obj == 0, Rec(uidA) ? "hidden=" + std::to_string(Rec(uidA)->hidden) : "no record");
    CheckEquation("cancel-then-same-name-equation-A", a);
    CheckEquation("cancel-then-same-name-equation-B", b);
    CheckSettled("cancel-then-same-name-A-settled", reqA);
    CheckSettled("cancel-then-same-name-B-settled", reqB);
    CheckNoQueueUnderLock("cancel-then-same-name-no-queue-under-lock");
}

// adversarial: a dropped live update (queue lagging) neither queues engine work nor rewrites the receipt
static void CaseDroppedLiveUpdate() {
    BeginCase("dropped-live-update");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/plain_h.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 13, 0, 13 });
    PumpRounds(2);
    Snapshot();
    Require(Rec(uid) && Rec(uid)->obj != 0, "row-attached-before-drop", "");
    const int liveBefore = oracle::liveMoveCalls;
    // Fill the game queue so a live (final=false) move is dropped by the production lag guard.
    std::vector<core::PlaceRequestHandle> extra;
    for (int i = 0; i < 3; ++i) {
        extra.push_back(core::BeginPlaceRequest({ "/object/filler.prefab" }));
        core::SubmitPlaceRow(extra.back(), 0, { 100.0f + i, 0, 0 });
    }
    Check("queue-lagged", core::PendingSpawns() > 2, "pendingSpawns=" + std::to_string(core::PendingSpawns()));
    const int idx = core::IndexOfUid(uid);
    const bool accepted = core::MoveSpawned((size_t)idx, { 14, 0, 14 }, Rot{}, 1.0f, false);
    Check("dropped-live-update-reports-true", accepted, "");
    Check("dropped-live-update-queued-nothing", oracle::liveMoveCalls == liveBefore, "liveMoves=" + std::to_string(oracle::liveMoveCalls));
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    TraceRequest("dropped-live-update", req);
    Check("receipt-unchanged-by-dropped-update", v.attached == 1 && v.pending == 0 && v.settled, VStr(v));
    CheckEquation("dropped-live-update-equation", v);
    PumpRounds(6);
    CheckNoQueueUnderLock("dropped-live-update-no-queue-under-lock");
}

// =========================================================================================================
// C5 cleanup ordering: a cancel whose HideUid is deferred behind an Applying lease must NOT let the
// caller-visible cleanup (settled / cleanupPending / removedAfterAttach) clear before the member's engine
// removal executed. The regression reads the production request state after every lane job; the engine removal
// seam count is the ground truth. Deterministic: an event gate holds the Applying lease (no sleeps/polling).
// =========================================================================================================
struct OrderStep { bool settled = false, cleanupPending = false, requestCanceled = false; int removals = 0; };
std::vector<OrderStep> g_order;
OrderStep ObserveOrder(const core::PlaceRequestHandle& req, int rowId) {
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    OrderStep s; s.settled = v.settled; s.cleanupPending = v.cleanupPending; s.requestCanceled = v.requestCanceled;
    for (const core::PlaceRow& r : v.rows) if (r.rowId == rowId && r.cleanupPending) s.cleanupPending = true;
    s.removals = oracle::removes;
    g_order.push_back(s);
    return s;
}
// The defect predicate: any observed state after the cancel was requested in which the caller-visible cleanup
// already settled while no engine removal had executed yet.
bool EarlyFinal() { for (const OrderStep& s : g_order) if (s.requestCanceled && s.settled && s.removals == 0) return true; return false; }
std::vector<core::MoveReq> LiftMoves(int uid) {
    for (const SpawnedObj& o : core::Spawned()) if (o.uid == uid) return { { uid, { o.pos.x, o.pos.y + 5.0f, o.pos.z }, o.rot, o.scale } };
    throw std::runtime_error("uid missing");
}

// The defect schedule (generic lane): attach, start a grounding op on the same record, gate its engine move so
// the op is Applying with the lease held, cancel through the public C5 API (the hide is deferred), then drain
// the lane one production job at a time. settled may only become true after removalCalls has advanced.
static void CaseDeferredCancelCleanup() {
    BeginCase("deferred-cancel-cleanup");
    g_order.clear();
    host::SetDirectGimmick(false);
    oracle::templateReady = false;
    core::g_recreateOnMove = false;        // the final move is the gated in-place leaf (no re-create)
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/plain_dc.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 22, 1, 22 });
    PumpRounds(2);
    Snapshot();
    Require(uid != 0 && Rec(uid) && Rec(uid)->obj != 0, "deferred-row-attached", "");
    { const core::PlaceRequestView v0 = core::PlaceRequestState(req); const core::PlaceRow* r0 = Row(v0, 0); Require(r0 && r0->state == core::PlaceAttached, "deferred-row-attached-state", r0 ? RStr(*r0) : "missing"); }
    const uintptr_t handle = Rec(uid)->obj;

    const core::GroundHandle op = core::BeginGround({ uid }, 501, 1);
    Require(core::GroundStateOf(op).state == core::GroundProbing, "deferred-op-probing", "");
    Require(core::GroundApply(op, LiftMoves(uid)), "deferred-op-apply-admitted", "");
    Require(core::GroundStateOf(op).state == core::GroundQueued, "deferred-op-queued", "");

    Gate gate; g_moveGate = &gate;
    std::thread worker([] { host::PumpGame(); });
    const bool entered = gate.Entered();
    Check("deferred-engine-entered", entered, "");
    if (entered) {
        Check("deferred-lease-held", core::GroundStateOf(op).state == core::GroundApplying && host::GroundLeaseCount() == 1,
              "state=" + std::to_string((int)core::GroundStateOf(op).state) + " leases=" + std::to_string(host::GroundLeaseCount()));
        Check("deferred-cancel-moved-one", core::PlaceRequestCancel(req) == 1, "");
        const OrderStep s = ObserveOrder(req, 0);
        Check("deferred-cancel-keeps-cleanup-pending", !s.settled && s.cleanupPending, "settled=" + std::to_string(s.settled) + " cleanup=" + std::to_string(s.cleanupPending));
        Check("deferred-hide-deferred-no-removal", oracle::removes == 0 && host::GroundDeferredCount() == 1,
              "removes=" + std::to_string(oracle::removes) + " deferred=" + std::to_string(host::GroundDeferredCount()));
    }
    gate.Release(); worker.join(); g_moveGate = nullptr;
    const OrderStep afterMove = ObserveOrder(req, 0);
    Check("deferred-removal-queued-not-executed", afterMove.removals == 0 && !afterMove.settled, "removes=" + std::to_string(afterMove.removals));

    int guard = 0;
    while (host::PumpGame()) { ObserveOrder(req, 0); if (++guard > 4096) throw std::runtime_error("lane did not drain"); }

    Check("deferred-no-early-final", !EarlyFinal(), "steps=" + std::to_string(g_order.size()));
    Check("deferred-removal-executed", oracle::removes == 1, "removes=" + std::to_string(oracle::removes));
    Check("deferred-handle-disposed", oracle::DisposedHandle(handle), oracle::Describe(handle));
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    const core::PlaceRow* r = Row(v, 0);
    Check("deferred-final-settled-after-removal", v.settled && !v.cleanupPending && v.pending == 0 && oracle::removes == 1, VStr(v));
    Check("deferred-receipt-kept", r && r->state == core::PlaceAttached && r->removedAfterAttach && r->lane == core::PlaceLaneGeneric, r ? RStr(*r) : "missing");
    CheckEquation("deferred-equation", v);
    Check("deferred-locks-clean", host::GroundLeaseCount() == 0 && host::GroundDeferredCount() == 0, "");
    CheckNoQueueUnderLock("deferred-no-queue-under-lock");
    core::g_recreateOnMove = true;         // restore for the remaining cases
}

// Control (no lease): the immediate HideUid path must also settle only after the removal executed.
static void CaseImmediateCancelCleanup() {
    BeginCase("immediate-cancel-cleanup");
    g_order.clear();
    host::SetDirectGimmick(false);
    oracle::templateReady = false;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/plain_ic.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 23, 1, 23 });
    PumpRounds(2);
    Snapshot();
    Require(uid != 0 && Rec(uid) && Rec(uid)->obj != 0, "immediate-row-attached", "");
    const uintptr_t handle = Rec(uid)->obj;
    const OrderStep attached = ObserveOrder(req, 0);
    Check("immediate-attached-settled", attached.settled && !attached.cleanupPending, "");
    Check("immediate-cancel-moved-one", core::PlaceRequestCancel(req) == 1, "");
    const OrderStep canceled = ObserveOrder(req, 0);
    Check("immediate-cancel-keeps-cleanup-pending", !canceled.settled && canceled.cleanupPending && canceled.removals == 0, "");
    PumpRounds(2);
    const OrderStep done = ObserveOrder(req, 0);
    Check("immediate-no-early-final", !EarlyFinal(), "");
    Check("immediate-removal-before-settle", done.removals == 1 && done.settled && !done.cleanupPending, "removes=" + std::to_string(done.removals));
    Check("immediate-handle-disposed", oracle::DisposedHandle(handle), oracle::Describe(handle));
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    CheckEquation("immediate-equation", v);
}

// Interactive lane: a cancel of a Pending row with a visible client stand-in must settle only after that
// stand-in's client removal ran. The old server-lane marker cleared the cleanup one lane turn early; pumping
// the server lane first is exactly the step that used to settle it before the game-lane removal.
static void CasePendingStandinCleanupOrder() {
    BeginCase("pending-standin-cleanup-order");
    g_order.clear();
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 1;            // one refused replay -> the production stand-in appears
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_pc.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 24, 1, 24 });
    PumpServerOnce();
    PumpGame_();
    Snapshot();
    Require(uid != 0 && Rec(uid) && Rec(uid)->standin && Rec(uid)->obj != 0, "standin-row-visible", "");
    { const core::PlaceRequestView v0 = core::PlaceRequestState(req); const core::PlaceRow* r0 = Row(v0, 0); Require(r0 && r0->state == core::PlacePending, "standin-row-still-pending", r0 ? RStr(*r0) : "missing"); }
    const uintptr_t standin = Rec(uid)->obj;
    Check("standin-cancel-moved-one", core::PlaceRequestCancel(req) == 1, "");
    const OrderStep canceled = ObserveOrder(req, 0);
    Check("standin-cancel-keeps-cleanup-pending", !canceled.settled && canceled.cleanupPending, "");
    PumpServerOnce();                       // the server lane must not clear the cleanup while the stand-in lives
    const OrderStep afterServer = ObserveOrder(req, 0);
    Check("standin-server-lane-does-not-early-settle", !afterServer.settled, "settled=" + std::to_string(afterServer.settled));
    Check("standin-no-early-final", !EarlyFinal(), "");
    PumpRounds(2);
    const OrderStep done = ObserveOrder(req, 0);
    Check("standin-disposed-before-settle", oracle::DisposedHandle(standin) && done.settled && !done.cleanupPending, oracle::Describe(standin));
    const core::PlaceRequestView v = core::PlaceRequestState(req);
    Check("standin-row-canceled-kept", Row(v, 0) && Row(v, 0)->state == core::PlaceCanceled, VStr(v));
    Snapshot();
    Check("standin-record-hidden", Rec(uid) && Rec(uid)->hidden, Rec(uid) ? "hidden=" + std::to_string(Rec(uid)->hidden) : "no record");
    CheckEquation("standin-equation", v);
    CheckNoQueueUnderLock("standin-no-queue-under-lock");
}

// Server-lane control: a cancel of an attached interactive (direct-lane) row whose visible member is the server
// actor must settle only after RemoveSpawnedActor ran on the server lane - the client lane must not settle it.
static void CaseServerLaneCleanupOrder() {
    BeginCase("server-lane-cleanup-order");
    g_order.clear();
    host::SetDirectGimmick(true);          // the interactive lane uses the template-free server builder
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_sv.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 25, 1, 25 });
    PumpRounds(3);
    Snapshot();
    Require(uid != 0 && Rec(uid) && Rec(uid)->obj != 0 && Rec(uid)->actor != 0, "server-row-attached-with-actor", Rec(uid) ? "actor=" + std::to_string((unsigned long long)Rec(uid)->actor) : "no record");
    const int actorsBefore = oracle::actorRemoveCalls;
    Check("server-cancel-moved-one", core::PlaceRequestCancel(req) == 1, "");
    const OrderStep canceled = ObserveOrder(req, 0);
    Check("server-cancel-keeps-cleanup-pending", !canceled.settled && canceled.cleanupPending, "");
    PumpGame_();                            // the client lane must not settle: the removal completion lives on the server lane
    const OrderStep afterGame = ObserveOrder(req, 0);
    Check("server-game-lane-does-not-early-settle", !afterGame.settled, "settled=" + std::to_string(afterGame.settled));
    Check("server-no-early-final", !EarlyFinal(), "");
    PumpServerOnce();
    const OrderStep done = ObserveOrder(req, 0);
    Check("server-actor-removed-before-settle", oracle::actorRemoveCalls == actorsBefore + 1 && done.settled && !done.cleanupPending,
          "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorsBefore));
    CheckEquation("server-equation", core::PlaceRequestState(req));
}

// B2: cancel inside the in-flight direct/replay native call must not publish Final before BOTH
// rejected handles are disposed. Schedule: submit one interactive row, subscribe duringDirect /
// duringReplay BEFORE the trigger, cancel inside the native call (before it returns its handles),
// assert inside and after the server step that the row is Canceled with cleanup pending and NOT
// settled, then drain one lane at a time: after the first lane's removal the request must STILL
// not be settled, and only after the second lane's removal is it settled with both handles disposed
// exactly once. Direct drains server-then-game; replay drains game-then-server (reverse order).
static void CaseInflightCancelDirect() {
    BeginCase("inflight-cancel-direct");
    host::SetDirectGimmick(true);          // the interactive lane uses the template-free server builder
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b2d.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 60, 0, 60 });
    Require(uid != 0, "b2d-row-admitted", "");
    bool sawInside = false;
    oracle::duringDirect = [&]() {
        const int moved = core::PlaceRequestCancel(req);   // cancel inside the in-flight native call
        Check("b2d-cancel-inside-moved-one", moved == 1, "moved=" + std::to_string(moved));
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-direct-inside", req);
        Check("b2d-cancel-inside-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0, VStr(v));
        Check("b2d-cancel-inside-not-settled", !v.settled && v.cleanupPending, VStr(v));
        sawInside = true;
    };
    PumpServerOnce();                       // the direct builder runs; the cancel fires inside it
    oracle::duringDirect = {};
    Check("b2d-cancel-fired-inside", sawInside, "");
    Require(!oracle::createdOrder.empty() && !oracle::actorsLive.empty(), "b2d-handles-returned", "");
    const uintptr_t so = oracle::createdOrder.back();
    const uintptr_t actor = *oracle::actorsLive.begin();
    Snapshot();
    Check("b2d-record-hidden-no-handle", Rec(uid) && Rec(uid)->hidden && Rec(uid)->obj == 0 && Rec(uid)->actor == 0, Rec(uid) ? ("hidden=" + std::to_string(Rec(uid)->hidden)) : "no record");
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-direct-after-step", req);
        Check("b2d-after-step-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0 && v.attached == 0, VStr(v));
        Check("b2d-after-step-not-settled", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-direct-equation", v);
    }
    const int actorRemovesBefore = oracle::actorRemoveCalls;
    const int removesBefore = oracle::removes;
    PumpServerOnce();                       // server lane only: the actor removal dispatches
    Check("b2d-actor-removed-on-server-lane", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::actorsRemoved.count(actor) == 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    Check("b2d-so-still-live", oracle::Live(so), oracle::Describe(so));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-direct-after-server", req);
        Check("b2d-server-only-not-final", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-direct-after-server-equation", v);
    }
    PumpGame_();                            // game lane: the scene-object removal dispatches
    Check("b2d-so-disposed-on-game-lane", oracle::removes == removesBefore + 1 && oracle::DisposedHandle(so), oracle::Describe(so));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-direct-final", req);
        Check("b2d-final-settled", v.settled && !v.cleanupPending && v.pending == 0, VStr(v));
        Check("b2d-handles-removed-once", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::removes == removesBefore + 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls) + " removes=" + std::to_string(oracle::removes));
        CheckEquation("inflight-cancel-direct-final-equation", v);
        CheckSettled("inflight-cancel-direct-settled-flag", req);
    }
    CheckNoQueueUnderLock("inflight-cancel-direct-no-queue-under-lock");
}

// B2 reverse order: the same in-flight cancel through the captured-template replay lane, drained
// game-then-server. After the game-lane removal the actor must still be live and the request still
// not settled; the server-lane removal settles it.
static void CaseInflightCancelReplay() {
    BeginCase("inflight-cancel-replay");
    host::SetDirectGimmick(false);         // the interactive lane must fall through to the replay
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b2r.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 61, 0, 61 });
    Require(uid != 0, "b2r-row-admitted", "");
    bool sawInside = false;
    oracle::duringReplay = [&]() {
        const int moved = core::PlaceRequestCancel(req);   // cancel inside the in-flight native call
        Check("b2r-cancel-inside-moved-one", moved == 1, "moved=" + std::to_string(moved));
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-replay-inside", req);
        Check("b2r-cancel-inside-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0, VStr(v));
        Check("b2r-cancel-inside-not-settled", !v.settled && v.cleanupPending, VStr(v));
        sawInside = true;
    };
    PumpServerOnce();                       // the replay runs; the cancel fires inside it
    oracle::duringReplay = {};
    Check("b2r-cancel-fired-inside", sawInside, "");
    Require(!oracle::createdOrder.empty() && !oracle::actorsLive.empty(), "b2r-handles-returned", "");
    const uintptr_t so = oracle::createdOrder.back();
    const uintptr_t actor = *oracle::actorsLive.begin();
    Snapshot();
    Check("b2r-record-hidden-no-handle", Rec(uid) && Rec(uid)->hidden && Rec(uid)->obj == 0 && Rec(uid)->actor == 0, Rec(uid) ? ("hidden=" + std::to_string(Rec(uid)->hidden)) : "no record");
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-replay-after-step", req);
        Check("b2r-after-step-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0 && v.attached == 0, VStr(v));
        Check("b2r-after-step-not-settled", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-replay-equation", v);
    }
    const int actorRemovesBefore = oracle::actorRemoveCalls;
    const int removesBefore = oracle::removes;
    PumpGame_();                            // game lane first: the scene-object removal dispatches
    Check("b2r-so-disposed-on-game-lane", oracle::removes == removesBefore + 1 && oracle::DisposedHandle(so), oracle::Describe(so));
    Check("b2r-actor-still-live", oracle::actorsRemoved.count(actor) == 0, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-replay-after-game", req);
        Check("b2r-game-only-not-final", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-replay-after-game-equation", v);
    }
    PumpServerOnce();                       // server lane: the actor removal dispatches
    Check("b2r-actor-removed-on-server-lane", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::actorsRemoved.count(actor) == 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-replay-final", req);
        Check("b2r-final-settled", v.settled && !v.cleanupPending && v.pending == 0, VStr(v));
        Check("b2r-handles-removed-once", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::removes == removesBefore + 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls) + " removes=" + std::to_string(oracle::removes));
        CheckEquation("inflight-cancel-replay-final-equation", v);
        CheckSettled("inflight-cancel-replay-settled-flag", req);
    }
    CheckNoQueueUnderLock("inflight-cancel-replay-no-queue-under-lock");
}

// B2-R1/R2 delta: admission-interrupt handshake. Production SubmitPlaceRow invokes
// core::g_admissionInterrupt after the record and its executable work are published but before
// the row UID is written; the submitting thread pauses there while the main thread pumps the
// server, so the in-flight native call runs with row.uid == 0. Bounded condition waits only;
// no sleeps, no queue-order luck. Every path after Start resumes and joins the worker.
struct AdmissionPause {
    std::mutex m; std::condition_variable cv;
    bool admitted = false, resume = false, armed = true, resumeReceived = false;
    std::thread worker;
    int submitUid = 0;
    void OnGate() {
        std::unique_lock<std::mutex> l(m);
        if (!armed) return;
        armed = false;
        admitted = true;
        cv.notify_all();
        resumeReceived = cv.wait_for(l, std::chrono::seconds(10), [this] { return resume; });
    }
    bool WaitAdmitted() {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::seconds(10), [this] { return admitted; });
    }
    void Resume() { { std::lock_guard<std::mutex> l(m); resume = true; } cv.notify_all(); }
    void Start(const core::PlaceRequestHandle& req, int rowId, Vec3 pos) {
        worker = std::thread([this, req, rowId, pos] { submitUid = core::SubmitPlaceRow(req, rowId, pos); });
    }
    void Join() { if (worker.joinable()) { worker.join(); Check("admission-resume-handshake", resumeReceived); } }
};

// B2-R1 direct: interrupt admission after enqueue but before UID publication, cancel inside the
// actual direct native boundary (row.uid == 0, so no hide can invalidate the record), let the
// native call return valid handles. The accepted completion must reject them through charged
// cleanup: no live handle after the drain, no early Final. Server-then-game drain.
static void CaseInflightCancelAdmissionDirect() {
    BeginCase("inflight-cancel-admission-direct");
    host::SetDirectGimmick(true);          // the interactive lane uses the template-free server builder
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b2ad.prefab" });
    AdmissionPause ap;
    core::g_admissionInterrupt = [&] { ap.OnGate(); };
    ap.Start(req, 0, { 62, 0, 62 });
    const bool admitted = ap.WaitAdmitted();
    Check("b2ad-admission-paused", admitted, "");
    if (!admitted) { ap.Resume(); ap.Join(); core::g_admissionInterrupt = nullptr; return; }
    bool sawInside = false;
    oracle::duringDirect = [&]() {
        const int moved = core::PlaceRequestCancel(req);   // row.uid == 0: no record work to hide
        Check("b2ad-cancel-inside-moved-zero", moved == 0, "moved=" + std::to_string(moved));
        const core::PlaceRequestView v0 = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-direct-inside", req);
        Check("b2ad-cancel-inside-canceled", v0.requested == 1 && v0.canceled == 1 && v0.pending == 0, VStr(v0));
        sawInside = true;
    };
    PumpServerOnce();                       // the direct builder runs with row.uid == 0
    oracle::duringDirect = {};
    Check("b2ad-cancel-fired-inside", sawInside, "");
    Check("b2ad-handles-returned", !oracle::createdOrder.empty() && !oracle::actorsLive.empty(),
          "objects=" + std::to_string(oracle::createdOrder.size()) + " actors=" + std::to_string(oracle::actorsLive.size()));
    const uintptr_t so = oracle::createdOrder.empty() ? 0 : oracle::createdOrder.back();
    const uintptr_t actor = oracle::actorsLive.empty() ? 0 : *oracle::actorsLive.begin();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-direct-after-step", req);
        Check("b2ad-after-step-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0 && v.attached == 0, VStr(v));
        Check("b2ad-after-step-not-settled", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-admission-direct-equation", v);
    }
    ap.Resume(); ap.Join();
    core::g_admissionInterrupt = nullptr;
    Check("b2ad-uid-published", ap.submitUid != 0, "uid=" + std::to_string(ap.submitUid));
    Snapshot();
    Check("b2ad-record-tombstone", Rec(ap.submitUid) && Rec(ap.submitUid)->hidden && Rec(ap.submitUid)->obj == 0 && Rec(ap.submitUid)->actor == 0,
          Rec(ap.submitUid) ? ("hidden=" + std::to_string(Rec(ap.submitUid)->hidden)) : "no record");
    const int actorRemovesBefore = oracle::actorRemoveCalls;
    const int removesBefore = oracle::removes;
    PumpServerOnce();                       // server lane only: the actor removal dispatches
    Check("b2ad-actor-removed-on-server-lane", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::actorsRemoved.count(actor) == 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    Check("b2ad-so-still-live", oracle::Live(so), oracle::Describe(so));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-direct-after-server", req);
        Check("b2ad-server-only-not-final", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-admission-direct-after-server-equation", v);
    }
    PumpGame_();                            // game lane: the scene-object removal dispatches
    Check("b2ad-so-disposed-on-game-lane", oracle::removes == removesBefore + 1 && oracle::DisposedHandle(so), oracle::Describe(so));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-direct-final", req);
        Check("b2ad-final-settled", v.settled && !v.cleanupPending && v.pending == 0, VStr(v));
        Check("b2ad-no-live-handle", !oracle::Live(so) && oracle::actorsRemoved.count(actor) == 1, oracle::Describe(so));
        Check("b2ad-handles-removed-once", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::removes == removesBefore + 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls) + " removes=" + std::to_string(oracle::removes));
        CheckEquation("inflight-cancel-admission-direct-final-equation", v);
        CheckSettled("inflight-cancel-admission-direct-settled-flag", req);
    }
    CheckNoQueueUnderLock("inflight-cancel-admission-direct-no-queue-under-lock");
}

// B2-R1 replay, reverse drain order: same admission interrupt through the captured-template
// replay boundary; game lane first, then server. After the game-lane removal the actor must
// still be live and the request still not settled.
static void CaseInflightCancelAdmissionReplay() {
    BeginCase("inflight-cancel-admission-replay");
    host::SetDirectGimmick(false);         // the interactive lane must fall through to the replay
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b2ar.prefab" });
    AdmissionPause ap;
    core::g_admissionInterrupt = [&] { ap.OnGate(); };
    ap.Start(req, 0, { 63, 0, 63 });
    const bool admitted = ap.WaitAdmitted();
    Check("b2ar-admission-paused", admitted, "");
    if (!admitted) { ap.Resume(); ap.Join(); core::g_admissionInterrupt = nullptr; return; }
    bool sawInside = false;
    oracle::duringReplay = [&]() {
        const int moved = core::PlaceRequestCancel(req);   // row.uid == 0: no record work to hide
        Check("b2ar-cancel-inside-moved-zero", moved == 0, "moved=" + std::to_string(moved));
        const core::PlaceRequestView v0 = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-replay-inside", req);
        Check("b2ar-cancel-inside-canceled", v0.requested == 1 && v0.canceled == 1 && v0.pending == 0, VStr(v0));
        sawInside = true;
    };
    PumpServerOnce();                       // the replay runs with row.uid == 0
    oracle::duringReplay = {};
    Check("b2ar-cancel-fired-inside", sawInside, "");
    Check("b2ar-handles-returned", !oracle::createdOrder.empty() && !oracle::actorsLive.empty(),
          "objects=" + std::to_string(oracle::createdOrder.size()) + " actors=" + std::to_string(oracle::actorsLive.size()));
    const uintptr_t so = oracle::createdOrder.empty() ? 0 : oracle::createdOrder.back();
    const uintptr_t actor = oracle::actorsLive.empty() ? 0 : *oracle::actorsLive.begin();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-replay-after-step", req);
        Check("b2ar-after-step-counts", v.requested == 1 && v.canceled == 1 && v.pending == 0 && v.attached == 0, VStr(v));
        Check("b2ar-after-step-not-settled", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-admission-replay-equation", v);
    }
    ap.Resume(); ap.Join();
    core::g_admissionInterrupt = nullptr;
    Check("b2ar-uid-published", ap.submitUid != 0, "uid=" + std::to_string(ap.submitUid));
    Snapshot();
    Check("b2ar-record-tombstone", Rec(ap.submitUid) && Rec(ap.submitUid)->hidden && Rec(ap.submitUid)->obj == 0 && Rec(ap.submitUid)->actor == 0,
          Rec(ap.submitUid) ? ("hidden=" + std::to_string(Rec(ap.submitUid)->hidden)) : "no record");
    const int actorRemovesBefore = oracle::actorRemoveCalls;
    const int removesBefore = oracle::removes;
    PumpGame_();                            // game lane first: the scene-object removal dispatches
    Check("b2ar-so-disposed-on-game-lane", oracle::removes == removesBefore + 1 && oracle::DisposedHandle(so), oracle::Describe(so));
    Check("b2ar-actor-still-live", oracle::actorsRemoved.count(actor) == 0, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-replay-after-game", req);
        Check("b2ar-game-only-not-final", !v.settled && v.cleanupPending, VStr(v));
        CheckEquation("inflight-cancel-admission-replay-after-game-equation", v);
    }
    PumpServerOnce();                       // server lane: the actor removal dispatches
    Check("b2ar-actor-removed-on-server-lane", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::actorsRemoved.count(actor) == 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("inflight-cancel-admission-replay-final", req);
        Check("b2ar-final-settled", v.settled && !v.cleanupPending && v.pending == 0, VStr(v));
        Check("b2ar-no-live-handle", !oracle::Live(so) && oracle::actorsRemoved.count(actor) == 1, oracle::Describe(so));
        Check("b2ar-handles-removed-once", oracle::actorRemoveCalls == actorRemovesBefore + 1 && oracle::removes == removesBefore + 1, "actorRemoves=" + std::to_string(oracle::actorRemoveCalls) + " removes=" + std::to_string(oracle::removes));
        CheckEquation("inflight-cancel-admission-replay-final-equation", v);
        CheckSettled("inflight-cancel-admission-replay-settled-flag", req);
    }
    CheckNoQueueUnderLock("inflight-cancel-admission-replay-no-queue-under-lock");
}

// B2-R2: a repeated PlaceRowCancel on an already cleaned Attached receipt is a true no-op.
// Attach an interactive row, cancel it, dispatch its cleanup, then snapshot and repeat: the
// repeat must return false, change no observable state, and queue no new engine removal.
static void CaseRepeatRowCancelNoop() {
    BeginCase("repeat-row-cancel-noop");
    host::SetDirectGimmick(true);
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_b2rc.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 64, 0, 64 });
    Require(uid != 0, "b2rc-row-admitted", "");
    PumpRounds(3);
    Snapshot();
    Require(uid != 0 && Rec(uid) && Rec(uid)->obj != 0 && Rec(uid)->actor != 0, "b2rc-row-attached-with-actor", "");
    { const core::PlaceRequestView v0 = core::PlaceRequestState(req); const core::PlaceRow* r0 = Row(v0, 0); Require(r0 && r0->state == core::PlaceAttached, "b2rc-row-attached-state", r0 ? RStr(*r0) : "missing"); }
    Check("b2rc-first-cancel-moved-one", core::PlaceRowCancel(req, 0), "");
    { const core::PlaceRequestView v1 = core::PlaceRequestState(req); Check("b2rc-first-cleanup-pending", !v1.settled && v1.cleanupPending, VStr(v1)); }
    PumpServerOnce();
    PumpGame_();
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("repeat-row-cancel-noop-cleaned", req);
        const core::PlaceRow* r = Row(v, 0);
        Require(r && r->state == core::PlaceAttached && r->removedAfterAttach, "b2rc-receipt-kept", r ? RStr(*r) : "missing");
        Require(v.settled && !v.cleanupPending, "b2rc-cleaned-settled", VStr(v));
    }
    const std::string snapBefore = VStr(core::PlaceRequestState(req));
    const int removesBefore = oracle::removes;
    const int actorRemovesBefore = oracle::actorRemoveCalls;
    Check("b2rc-repeat-cancel-noop", !core::PlaceRowCancel(req, 0), "repeat must be a no-op");
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("repeat-row-cancel-noop-after-repeat", req);
        const core::PlaceRow* r = Row(v, 0);
        Check("b2rc-repeat-state-identical", VStr(v) == snapBefore, VStr(v));
        Check("b2rc-repeat-receipt-kept", r && r->state == core::PlaceAttached && r->removedAfterAttach && r->lane == core::PlaceLaneDirect, r ? RStr(*r) : "missing");
        Check("b2rc-repeat-no-new-removal", oracle::removes == removesBefore && oracle::actorRemoveCalls == actorRemovesBefore, "removes=" + std::to_string(oracle::removes - removesBefore) + " actorRemoves=" + std::to_string(oracle::actorRemoveCalls - actorRemovesBefore));
        CheckEquation("repeat-row-cancel-noop-equation", v);
        CheckSettled("repeat-row-cancel-noop-settled-flag", req);
    }
    CheckNoQueueUnderLock("repeat-row-cancel-noop-no-queue-under-lock");
}

// D1 (WB080 Task 3): the replay revalidation added by B2 lock-hoisting must not touch a
// stand-in it no longer owns. The production probe below fires between the replay flight
// capture and its second registry lookup (no locks held, same thread, fully synchronous),
// so the interleaving is deterministic: no threads, no sleeps, no queue-order luck.
struct D1ProbeGuard {
    ~D1ProbeGuard() {
#ifdef WB_UNIFIED_HOST_TEST
        core::g_replayRevalidateProbe = nullptr;
#endif
    }
};

// D1 wrong-generation: a production live drag admitted between the two replay registry
// lookups renews the generation and (after draining the production game queue) plants a
// current-generation stand-in with a live handle. The stale replay must exit without
// retiring that stand-in: no replay attempt, no removal queued for it, the record keeps
// (standin, obj), the flight obligation is released, and the later cancel disposes it.
static void CaseStaleReplayWrongGenKeepsStandin() {
    BeginCase("stale-replay-wrong-gen-keeps-standin");
    host::SetDirectGimmick(false);         // the interactive lane must fall through to the replay
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_d1g.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 70, 0, 70 });
    Require(uid != 0, "d1g-row-admitted", "");
    D1ProbeGuard pg;
    bool probeFired = false;
    core::g_replayRevalidateProbe = [&]() {
        probeFired = true;
        // Production-linked: a live drag renews the generation (the record is not a
        // stand-in yet) and queues the client stand-in; draining the production game
        // queue (bounded, same thread) plants it with a live handle.
        const bool moved = core::MoveMany({ { uid, { 71, 0, 70 }, Rot(), 1.0f } }, false);
        Check("d1g-probe-live-drag-admitted", moved, "");
        int guard = 0;
        while (host::PumpGame()) { if (++guard > 4096) throw std::runtime_error("d1 probe game queue did not settle"); }
    };
    const int replayBefore = oracle::replayCalls;
    const int removesBefore = oracle::removes;
    PumpServerOnce();                       // flight captured; the probe runs; the revalidation sees a new generation
    Require(probeFired, "d1g-probe-fired-between-lookups", "");
    Snapshot();
    // The probe drain performs exactly one production create (the stand-in); the stale
    // path under test runs after it, so the handle is captured from the oracle, not the record.
    Check("d1g-probe-created-one-standin", oracle::createdOrder.size() == 1, "creates=" + std::to_string(oracle::createdOrder.size()));
    const uintptr_t standin = oracle::createdOrder.empty() ? 0 : oracle::createdOrder.back();
    Check("d1g-no-replay-on-stale", oracle::replayCalls == replayBefore, "replay=" + std::to_string(oracle::replayCalls - replayBefore));
    TraceRequest("stale-replay-wrong-gen-keeps-standin-after-step", req);
    Require(standin != 0, "d1g-current-standin-planted", standin ? oracle::Describe(standin) : "no stand-in");
    Check("d1g-current-standin-untouched", Rec(uid) && Rec(uid)->standin && Rec(uid)->obj == standin && !Rec(uid)->hidden,
          Rec(uid) ? ("standin=" + std::to_string(Rec(uid)->standin) + " " + oracle::Describe(Rec(uid)->obj)) : "no record");
    Check("d1g-standin-still-live", oracle::Live(standin), oracle::Describe(standin));
    Check("d1g-no-removal-queued-for-standin", oracle::removes == removesBefore, "removes=" + std::to_string(oracle::removes - removesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        const core::PlaceRow* r = Row(v, 0);
        Check("d1g-row-still-pending", r && r->state == core::PlacePending, r ? RStr(*r) : "missing");
        Check("d1g-flight-released", !v.cleanupPending, VStr(v));
        CheckEquation("stale-replay-wrong-gen-keeps-standin-equation", v);
    }
    Check("d1g-cancel-moved-one", core::PlaceRequestCancel(req) == 1, "");
    PumpGame_();
    PumpServerOnce();
    Snapshot();
    Check("d1g-standin-disposed-on-cancel", oracle::DisposedHandle(standin), oracle::Describe(standin));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        TraceRequest("stale-replay-wrong-gen-keeps-standin-final", req);
        Check("d1g-final-canceled-settled", v.settled && !v.cleanupPending && v.canceled == 1 && v.pending == 0, VStr(v));
        Check("d1g-no-live-handle", !oracle::Live(standin), oracle::Describe(standin));
        CheckEquation("stale-replay-wrong-gen-keeps-standin-final-equation", v);
        CheckSettled("stale-replay-wrong-gen-keeps-standin-settled-flag", req);
    }
    CheckNoQueueUnderLock("stale-replay-wrong-gen-keeps-standin-no-queue-under-lock");
}

// D1 missing record: a production ForgetUid between the two replay registry lookups erases
// the record. The revalidation must exit stale without indexing the erased record, without
// attempting the replay and with the flight obligation released. (Pre-fix this path indexes
// g_reg with -1: undefined behavior; the same stale-path assertions capture the outcome.)
static void CaseStaleReplayMissingRecord() {
    BeginCase("stale-replay-missing-record");
    host::SetDirectGimmick(false);         // the interactive lane must fall through to the replay
    oracle::templateReady = true;
    const core::PlaceRequestHandle req = core::BeginPlaceRequest({ "/object/cd_gimmick/lamp_d1m.prefab" });
    const int uid = core::SubmitPlaceRow(req, 0, { 72, 0, 72 });
    Require(uid != 0, "d1m-row-admitted", "");
    D1ProbeGuard pg;
    bool probeFired = false;
    core::g_replayRevalidateProbe = [&]() { probeFired = true; core::ForgetUid(uid); };
    const int replayBefore = oracle::replayCalls;
    const int removesBefore = oracle::removes;
    PumpServerOnce();                       // flight captured; the probe forgets the uid; the revalidation finds no record
    Require(probeFired, "d1m-probe-fired-between-lookups", "");
    Snapshot();
    Check("d1m-no-replay-on-stale", oracle::replayCalls == replayBefore, "replay=" + std::to_string(oracle::replayCalls - replayBefore));
    TraceRequest("stale-replay-missing-record-after-step", req);
    Check("d1m-record-gone", core::IndexOfUid(uid) < 0 && Rec(uid) == nullptr,
          "index=" + std::to_string(core::IndexOfUid(uid)));
    Check("d1m-no-removal-queued", oracle::removes == removesBefore, "removes=" + std::to_string(oracle::removes - removesBefore));
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        const core::PlaceRow* r = Row(v, 0);
        Check("d1m-row-canceled", r && r->state == core::PlaceCanceled, r ? RStr(*r) : "missing");
        Check("d1m-flight-released", !v.cleanupPending, VStr(v));
        Check("d1m-settled", v.settled && v.pending == 0, VStr(v));
        CheckEquation("stale-replay-missing-record-equation", v);
        CheckSettled("stale-replay-missing-record-settled-flag", req);
    }
    PumpRounds(2);                          // drain anything the stale path left behind (nothing expected)
    Snapshot();
    {
        const core::PlaceRequestView v = core::PlaceRequestState(req);
        Check("d1m-still-settled-after-drain", v.settled && !v.cleanupPending && v.canceled == 1 && v.pending == 0, VStr(v));
        CheckEquation("stale-replay-missing-record-drained-equation", v);
    }
    CheckNoQueueUnderLock("stale-replay-missing-record-no-queue-under-lock");
}

#include "current_main_cases.h"
#include "v097_ground_cases.h"
#include "v097_project_cases.h"

// =========================================================================================================
// main
// =========================================================================================================
void DumpCases() {
    std::string json = "{\"suite\":\"PlacementResults\",\"status\":\"" + std::string(g_failures == 0 ? "PASS" : "FAIL") +
                       "\",\"assertions\":" + std::to_string(g_assertions) + ",\"failures\":" + std::to_string(g_failures) + ",\"cases\":[";
    for (size_t i = 0; i < g_cases.size(); ++i) {
        const CaseRec& c = g_cases[i];
        if (i) json += ",";
        json += "{\"id\":\"" + c.id + "\",\"status\":\"" + std::string(c.failures == 0 ? "PASS" : "FAIL") +
                "\",\"assertions\":" + std::to_string(c.assertions) + ",\"failures\":" + std::to_string(c.failures) + "}";
    }
    json += "]}";
    FILE* f = std::fopen((g_fixtureDir + "\\cases.json").c_str(), "wb");
    if (f) { std::fwrite(json.data(), 1, json.size(), f); std::fclose(f); }
    core::Log("[placement_results] cases: %s", json.c_str());
}

int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : ".";
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // D1: unbuffered stdout so a stale-path abort cannot lose earlier CHECK lines
    host::OpenLog(g_fixtureDir + "\\placement_results.log");
    core::Log("[placement_results] run start: fixtureDir=%s", g_fixtureDir.c_str());
    g_probe.Start();
    InstallSeam();
    core::g_recreateOnMove = true;
    host::SetDirectGimmick(false);

    struct Entry { const char* id; void (*fn)(); };
    const Entry entries[] = {
        { "mixed-lanes", CaseMixedLanes },
        { "replay-lane", CaseReplayLane },
        { "retry-same-row", CaseRetrySameRow },
        { "refused-standin-retry", CaseRefusedStandinRetry },
        { "cancel-first", CaseCancelFirst },
        { "start-same-name", CaseStartSameName },
        { "late-first", CaseLateFirst },
        { "double-complete", CaseDoubleComplete },
        { "failed-attach", CaseFailedAttach },
        { "projection-cancel-cleanup", CaseProjectionCancelCleanup },
        { "cancel-then-same-name", CaseCancelThenSameName },
        { "dropped-live-update", CaseDroppedLiveUpdate },
        { "deferred-cancel-cleanup", CaseDeferredCancelCleanup },
        { "immediate-cancel-cleanup", CaseImmediateCancelCleanup },
        { "pending-standin-cleanup-order", CasePendingStandinCleanupOrder },
        { "server-lane-cleanup-order", CaseServerLaneCleanupOrder },
        { "inflight-cancel-direct", CaseInflightCancelDirect },
        { "inflight-cancel-replay", CaseInflightCancelReplay },
        { "inflight-cancel-admission-direct", CaseInflightCancelAdmissionDirect },
        { "inflight-cancel-admission-replay", CaseInflightCancelAdmissionReplay },
        { "repeat-row-cancel-noop", CaseRepeatRowCancelNoop },
        { "stale-replay-wrong-gen-keeps-standin", CaseStaleReplayWrongGenKeepsStandin },
        { "stale-replay-missing-record", CaseStaleReplayMissingRecord },
        { "main-raw-ticket-invalidation", current_main::RawInvalidation },
        { "main-travel-native-apply-lease", current_main::TravelLease },
        { "main-travel-native-failure", current_main::TravelFault },
        { "main-terrain-apply-two-trips", current_main::TerrainTwoTrips },
        { "V097-GROUND-FIVE-CASTS", v097_ground::FiveCasts },
        { "V097-GROUND-NEIGHBOR-INVALIDATION", v097_ground::NeighborInvalidation },
        { "V097-MIXED-SAVE-SCOPES", v097_project::Scopes },
        { "V097-MIXED-TRANSACTIONS", v097_project::Transactions },
        { "V097-MIXED-VALIDATION-RELOAD", v097_project::ValidationReload },
        { "V097-NPC-NATIVE-LIFETIME", v097_project::NpcLifecycle },
        { "V097-PROJECT-BUSY-OUTCOME", v097_project::BusyOutcomes },
    };
    for (const Entry& e : entries) {
        oracle::Reset();
        host::SetDirectGimmick(false);
        try {
            e.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("CASE-ABORT %s: %s\n", e.id, ex.what());
            core::Log("[placement_results] CASE-ABORT %s: %s", e.id, ex.what());
            if (!g_cases.empty() && g_cases.back().failures == 0) ++g_cases.back().failures;
        }
        DumpCases();   // D1: incremental receipt so a stale-path abort cannot lose earlier cases
    }

    Check("engine-lock-free-throughout", oracle::lockFreeDuringEngineCalls, "lockFree=" + std::to_string(oracle::lockFreeDuringEngineCalls));
    Check("no-queue-push-under-registry-lock", host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations()));

    DumpCases();
    {
        FILE* f = std::fopen((g_fixtureDir + "\\placement_results_trace.json").c_str(), "wb");
        if (f) { std::fwrite(g_trace.data(), 1, g_trace.size(), f); std::fclose(f); }
    }
    std::printf("CASES=%zu\nASSERTIONS=%d\nFAILURES=%d\n", g_cases.size(), g_assertions, g_failures);
    core::Log("[placement_results] run end: cases=%zu assertions=%d failures=%d lockViolations=%d", g_cases.size(), g_assertions, g_failures, host::LockViolations());
    g_probe.Stop();
    return g_failures == 0 ? 0 : 1;
}
