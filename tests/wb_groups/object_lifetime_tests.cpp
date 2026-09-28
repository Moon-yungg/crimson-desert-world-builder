// ObjectLifetime host fixture (plan wb079-unified-77f68967, Task 7 / C3).
//
// The production core TU is compiled by production_core_host.cpp (WB_UNIFIED_HOST_TEST) and the ACTUAL
// editor TU is included here once, so Undo/Redo, the History stacks, the registry, the spawn/retry
// queues and the gimmick fallback are production code. Substituted are only the native engine
// boundaries behind host::Seam(): creation, removal, transforms, actor removal, the server spawn/replay
// attempts and thread readiness. The oracle below owns those handles, counts every creation and removal
// and can re-enter the production caller from inside an engine callback (the "event" seam), which is how
// stale completions, reversed callbacks and index-shift races are produced deterministically.
//
// Cases assert the C3 contract: the logical UID survives hide/restore/forget, every restore materializes
// a fresh physical generation, the newest record-owned project/group survives undo/redo, a completion
// queued for an older incarnation is disposed instead of attaching, and no engine/server work is queued
// while the registry lock is held (a probe thread try-locks the production lock during every callback).
//
// Machine output: CHECK lines on stdout, one ASSERTIONS=<n> line, a per-case cases.json receipt and the
// object/generation/action trace through the production log sink (host::OpenLog) at
// <fixtureDir>\object_lifetime.log.
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

#include "../../asi/cdmodkit/editor.cpp"   // the actual editor TU (History, Undo/Redo, DeleteSel, GroupSel)
#include "production_host.h"               // host::Seam / host::PumpGame / host::PumpServer / lock probe

// editor.cpp's dialogs and shell integration (no dialog is opened in a host run)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace oracle {
struct Obj { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1; uintptr_t actor = 0; bool live = true; };

std::map<uintptr_t, Obj> objects;
std::vector<uintptr_t> createdOrder;
std::set<uintptr_t> disposed;          // scene handles remove() disposed
std::set<uintptr_t> released;          // handles the fixture released with Forget (the game keeps them)
std::set<uintptr_t> actorsLive;
std::set<uintptr_t> actorsRemoved;
std::set<uintptr_t> releasedActors;
uintptr_t next = 0x10000;
int creates = 0, removes = 0, moveInPlaceCalls = 0, liveMoveCalls = 0;
int directCalls = 0, replayCalls = 0, replayRefusals = 0, actorRemoveCalls = 0;
bool templateReady = true, refuseDirect = false, refuseCreate = false;
bool lockFreeDuringEngineCalls = true;
std::function<void()> duringCreate, duringRemove, duringMoveInPlace, duringDirect, duringReplay, duringActorRemove;
uintptr_t lastDirectSo = 0, lastDirectActor = 0, lastReplaySo = 0, lastReplayActor = 0;

void Reset() {
    objects.clear(); createdOrder.clear(); disposed.clear(); released.clear();
    actorsLive.clear(); actorsRemoved.clear(); releasedActors.clear();
    next = 0x10000; creates = removes = moveInPlaceCalls = liveMoveCalls = 0;
    directCalls = replayCalls = replayRefusals = actorRemoveCalls = 0;
    templateReady = true; refuseDirect = false; refuseCreate = false; lockFreeDuringEngineCalls = true;
    duringCreate = duringRemove = duringMoveInPlace = duringDirect = duringReplay = duringActorRemove = {};
    lastDirectSo = lastDirectActor = lastReplaySo = lastReplayActor = 0;
}
bool Live(uintptr_t h) { auto it = objects.find(h); return it != objects.end() && it->second.live; }
bool DisposedHandle(uintptr_t h) { return disposed.count(h) != 0; }
uintptr_t LastCreated() { return createdOrder.empty() ? 0 : createdOrder.back(); }
std::string Describe(uintptr_t h) {
    auto it = objects.find(h);
    char b[192];
    if (it == objects.end()) { std::snprintf(b, sizeof b, "handle=0x%llx unknown", (unsigned long long)h); return b; }
    std::snprintf(b, sizeof b, "handle=0x%llx live=%d actor=0x%llx pos=(%.1f %.1f %.1f)", (unsigned long long)h,
                  it->second.live ? 1 : 0, (unsigned long long)it->second.actor, it->second.pos.x, it->second.pos.y, it->second.pos.z);
    return b;
}
}   // namespace oracle

// ---- lock-discipline probe: a second thread try-locks the production registry lock on request ----
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
    // Called from inside an engine callback on the main thread: the lock must be acquirable right now.
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
int g_assertions = 0, g_failures = 0, g_lockBaseline = 0;
std::string g_case = "startup";
std::vector<SpawnedObj> g_list;
LockProbe g_probe;
std::string g_fixtureDir;
struct CaseRec { std::string id; int assertions = 0, failures = 0; std::string observed; };
std::vector<CaseRec> g_cases;

void Check(const char* label, bool ok, const std::string& observed = "") {
    ++g_assertions;
    if (!g_cases.empty()) { ++g_cases.back().assertions; if (!ok) ++g_cases.back().failures; }
    if (!ok) ++g_failures;
    std::printf("CHECK %s %s%s%s\n", ok ? "PASS" : "FAIL", label, observed.empty() ? "" : " | ", observed.c_str());
    core::Log("[object_lifetime] %s %s %s%s%s", g_case.c_str(), ok ? "PASS" : "FAIL", label,
              observed.empty() ? "" : " | ", observed.c_str());
}
void Require(bool ok, const char* label, const std::string& observed = "") {
    Check(label, ok, observed);
    if (!ok) throw std::runtime_error(std::string(label) + (observed.empty() ? "" : (" | " + observed)));
}
void ResetWorld();   // defined below; BeginCase starts every case from a clean registry and oracle
void BeginCase(const char* id) {
    g_case = id;
    ResetWorld();
    g_cases.push_back(CaseRec{});
    g_cases.back().id = id;
    g_lockBaseline = host::LockViolations();
    std::printf("CASE: %s\n", id);
    core::Log("[object_lifetime] CASE %s", id);
}

// ---- registry snapshot helpers ---------------------------------------------------------------------------
// Snapshot() copies the registry into a buffer with reserved capacity, so a `const SpawnedObj*` taken from it
// stays valid across later snapshots inside the same assertion (the copy assignment reuses the capacity).
void Snapshot() { if (g_list.capacity() < 256) g_list.reserve(256); g_list = core::Spawned(); }
const SpawnedObj* Rec(int uid) { for (auto& o : g_list) if (o.uid == uid) return &o; return nullptr; }
int VisibleCount() { int n = 0; for (const SpawnedObj& o : g_list) if (!o.hidden) ++n; return n; }
std::string Hex(uintptr_t v) { char b[32]; std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v); return b; }
std::string UidGen(int uid) {
    Snapshot();
    const SpawnedObj* o = Rec(uid);
    char b[256];
    if (!o) { std::snprintf(b, sizeof b, "uid=%d missing nextUid=%d", uid, host::NextUid()); return b; }
    std::snprintf(b, sizeof b, "uid=%d gen=%llu obj=0x%llx hidden=%d proj=%d group=%d standin=%d gimmick=%d nextUid=%d",
                  uid, (unsigned long long)o->gen, (unsigned long long)o->obj, o->hidden ? 1 : 0, o->proj, o->group,
                  o->standin ? 1 : 0, o->gimmick ? 1 : 0, host::NextUid());
    return b;
}

// ---- engine oracle invariants -----------------------------------------------------------------------------
struct Inv {
    int live = 0, attached = 0, orphans = 0, hiddenWithHandle = 0, liveActors = 0, orphanActors = 0;
};
Inv ComputeInv() {
    Inv v;
    std::set<uintptr_t> attached, attachedActors;
    Snapshot();
    for (const SpawnedObj& o : g_list) {
        if (o.hidden) { if (o.obj) ++v.hiddenWithHandle; }
        else if (o.obj) { attached.insert(o.obj); if (o.actor) attachedActors.insert(o.actor); }
    }
    for (const auto& kv : oracle::objects) {
        if (!kv.second.live) continue;
        ++v.live;
        if (attached.count(kv.first)) ++v.attached;
        else if (!oracle::released.count(kv.first)) ++v.orphans;
    }
    for (uintptr_t a : oracle::actorsLive) {
        ++v.liveActors;
        if (!attachedActors.count(a) && !oracle::releasedActors.count(a)) ++v.orphanActors;
    }
    return v;
}
std::string InvStr(const Inv& v) {
    char b[256];
    std::snprintf(b, sizeof b, "live=%d attached=%d orphans=%d hidden_with_handle=%d live_actors=%d orphan_actors=%d",
                  v.live, v.attached, v.orphans, v.hiddenWithHandle, v.liveActors, v.orphanActors);
    return b;
}
void CheckInvariants(const char* label) {
    const Inv v = ComputeInv();
    Check(label, v.orphans == 0 && v.orphanActors == 0 && v.hiddenWithHandle == 0, InvStr(v));
}
void CheckLockDiscipline(const char* label) {
    Check((std::string(label) + "-engine-lock-free").c_str(), oracle::lockFreeDuringEngineCalls,
          "engine callbacks saw the registry lock free: " + std::string(oracle::lockFreeDuringEngineCalls ? "yes" : "no"));
    Check((std::string(label) + "-no-queue-under-lock").c_str(), host::LockViolations() == g_lockBaseline,
          "queue pushes under the registry lock in this case: " + std::to_string(host::LockViolations() - g_lockBaseline));
}

// ---- production seam installation -------------------------------------------------------------------------
void InstallSeam() {
    host::Engine& e = host::Seam();
    e.ready = true;
    e.createGeneric = [](const std::string& prefab, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::creates;
        if (oracle::refuseCreate) return 0;
        const uintptr_t h = oracle::next++;
        oracle::objects.emplace(h, oracle::Obj{prefab, pos, rot, scale, 0, true});
        oracle::createdOrder.push_back(h);
        if (oracle::duringCreate) { std::function<void()> f = std::move(oracle::duringCreate); oracle::duringCreate = {}; f(); }
        return h;
    };
    e.remove = [](uintptr_t h) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::removes;
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.live = false;
        oracle::disposed.insert(h);
        if (oracle::duringRemove) { std::function<void()> f = std::move(oracle::duringRemove); oracle::duringRemove = {}; f(); }
        return true;
    };
    e.moveInPlace = [](uintptr_t h, Vec3 pos, Rot rot, float scale) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::moveInPlaceCalls;
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.pos = pos; it->second.rot = rot; it->second.scale = scale;
        if (oracle::duringMoveInPlace) { std::function<void()> f = std::move(oracle::duringMoveInPlace); oracle::duringMoveInPlace = {}; f(); }
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
    e.removeActor = [](uintptr_t actor) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::actorRemoveCalls;
        if (!oracle::actorsLive.erase(actor)) return false;
        oracle::actorsRemoved.insert(actor);
        if (oracle::duringActorRemove) { std::function<void()> f = std::move(oracle::duringActorRemove); oracle::duringActorRemove = {}; f(); }
        return true;
    };
    e.directGimmick = [](const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::directCalls;
        if (oracle::refuseDirect) return false;
        const uintptr_t h = oracle::next++;
        oracle::objects.emplace(h, oracle::Obj{prefab, pos, rot, scale, 0, true});
        oracle::createdOrder.push_back(h);
        const uintptr_t a = oracle::next++;
        oracle::actorsLive.insert(a);
        oracle::objects[h].actor = a;
        oracle::lastDirectSo = h; oracle::lastDirectActor = a;
        if (oracle::duringDirect) { std::function<void()> f = std::move(oracle::duringDirect); oracle::duringDirect = {}; f(); }
        *so = h; *actor = a;
        return true;
    };
    e.replay = [](int, const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::replayCalls;
        if (oracle::replayRefusals > 0) {
            --oracle::replayRefusals;
            if (oracle::duringReplay) { std::function<void()> f = std::move(oracle::duringReplay); oracle::duringReplay = {}; f(); }
            return false;
        }
        const uintptr_t h = oracle::next++;
        oracle::objects.emplace(h, oracle::Obj{prefab, pos, rot, scale, 0, true});
        oracle::createdOrder.push_back(h);
        const uintptr_t a = oracle::next++;
        oracle::actorsLive.insert(a);
        oracle::objects[h].actor = a;
        oracle::lastReplaySo = h; oracle::lastReplayActor = a;
        if (oracle::duringReplay) { std::function<void()> f = std::move(oracle::duringReplay); oracle::duringReplay = {}; f(); }
        *so = h; *actor = a;
        return true;
    };
    e.templateReady = [] { return oracle::templateReady; };
}

// ---- pumping ----------------------------------------------------------------------------------------------
void PumpGame_() {
    int guard = 0;
    while (host::PumpGame()) { if (++guard > 1024) throw std::runtime_error("the production game queue did not settle"); }
}
void PumpAll() {
    PumpGame_();
    host::PumpServer();     // one production ServerField tick: server jobs + one gimmick queue step
    PumpGame_();
}
void PumpServerTick() {
    host::PumpServer();
    PumpGame_();
}
void ResetWorld() {
    editor::host_seam::ResetHistory();
    core::DeleteAllSpawned();
    PumpGame_();
    for (int i = 0; i < 8; i++) PumpServerTick();   // drain any retry entries whose records are gone
    oracle::Reset();
    g_probe.CheckFree();                            // keep the probe warm (and the lock acquirable)
}

// ---- case helpers -----------------------------------------------------------------------------------------
int Spawn(const std::string& prefab, Vec3 at, Rot rot = {}, float scale = 1.0f, int group = 0, int proj = 0) {
    const int uid = editor::host_seam::SpawnRecorded(prefab, at, rot, scale, group, proj);
    Require(uid > 0, "spawn-accepted", "uid=" + std::to_string(uid));
    return uid;
}
void DeleteViaEditor(int uid) {
    editor::host_seam::SelectUid(uid);
    editor::host_seam::DeleteSelection();
}
void ForgetViaEditor(int uid) {
    editor::host_seam::SelectUid(uid);
    editor::host_seam::ForgetSelection();
}
bool MoveFinal(int uid, Vec3 pos, Rot rot, float scale) {
    return core::MoveMany(std::vector<core::MoveReq>{ core::MoveReq{ uid, pos, rot, scale } }, true);
}
int HistoryKind(bool redo, size_t entry, size_t act) {
    editor::host_seam::ActView v;
    if (!editor::host_seam::HistoryView(redo, entry, act, &v)) return -1;
    return v.kind;
}
int HistoryUid(bool redo, size_t entry, size_t act) {
    editor::host_seam::ActView v;
    if (!editor::host_seam::HistoryView(redo, entry, act, &v)) return -1;
    return v.uid;
}

// =========================================================================================================
// Cases
// =========================================================================================================

// happy: generic create materializes on the game pump and attaches to the logical record
static void CaseCreateAttach() {
    BeginCase("create-attach");
    const int uid = Spawn("/object/box.prefab", {7, 8, 9}, { 30, 0, 0 }, 1.5f);
    Snapshot();
    const SpawnedObj* o = Rec(uid);
    Check("record-created-before-pump", o && !o->hidden && o->obj == 0, UidGen(uid));
    Check("history-spawn-entry", editor::host_seam::UndoSize() == 1 && HistoryKind(false, 0, 0) == 0 /*Act::Spawn*/, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    PumpAll();
    Snapshot(); o = Rec(uid);
    Check("record-materialized", o && !o->hidden && o->obj != 0, UidGen(uid));
    Check("handle-live-in-engine", o && oracle::Live(o->obj), o ? oracle::Describe(o->obj) : "");
    Check("engine-created-once", oracle::creates == 1, "creates=" + std::to_string(oracle::creates));
    Check("nothing-disposed", oracle::removes == 0, "removes=" + std::to_string(oracle::removes));
    Check("pose-applied", o && std::fabs(o->pos.x - 7.0f) < 0.001f && std::fabs(o->pos.y - 8.0f) < 0.001f && std::fabs(o->pos.z - 9.0f) < 0.001f, UidGen(uid));
    CheckInvariants("create-attach-invariants");
    CheckLockDiscipline("create-attach");
}

// happy + failure: hide/restore keeps the logical uid, restore materializes a fresh generation and handle,
// the newest record-owned metadata survives; a forgotten record is never resurrected
static void CaseHistoryRestoreSameUid() {
    BeginCase("restore-same-uid");
    const int a = Spawn("/object/a.prefab", { 1, 0, 0 });
    const int b = Spawn("/object/b.prefab", { 2, 0, 0 });
    PumpAll();
    Snapshot();
    const uintptr_t handleA0 = Rec(a)->obj; const uint64_t genA0 = Rec(a)->gen;
    Require(handleA0 != 0, "initial-handle-recorded", UidGen(a));
    DeleteViaEditor(a);
    Snapshot();
    Check("delete-hides-and-keeps-tombstone", Rec(a) && Rec(a)->hidden && Rec(a)->obj == 0, UidGen(a));
    const int nextUidBefore = host::NextUid();
    Require(editor::host_seam::UndoOne(), "undo-delete-admitted", "");
    Snapshot();
    const SpawnedObj* ra = Rec(a);
    Check("restore-keeps-logical-uid", ra && !ra->hidden && ra->uid == a, UidGen(a));
    Check("restore-allocates-no-uid", host::NextUid() == nextUidBefore, "nextUid=" + std::to_string(host::NextUid()) + " before=" + std::to_string(nextUidBefore));
    Check("history-reference-not-remapped", HistoryUid(true, 0, 0) == a, "redo-act-uid=" + std::to_string(HistoryUid(true, 0, 0)));
    PumpAll();
    Snapshot(); ra = Rec(a);
    Check("restore-materializes-handle", ra && ra->obj != 0 && ra->obj != handleA0, UidGen(a));
    Check("restore-fresh-generation", ra && ra->gen != 0 && ra->gen != genA0, UidGen(a));
    Check("restored-handle-live", ra && oracle::Live(ra->obj), ra ? oracle::Describe(ra->obj) : "");
    Check("old-handle-disposed-once", oracle::DisposedHandle(handleA0) || handleA0 == 0, oracle::Describe(handleA0));
    Check("restore-keeps-pose", ra && std::fabs(ra->pos.x - 1.0f) < 0.001f, UidGen(a));
    Check("two-visible", VisibleCount() == 2, "visible=" + std::to_string(VisibleCount()));
    // a second hide/restore cycle must again renew the generation (monotonic, never reused)
    const uint64_t genA1 = ra->gen;
    DeleteViaEditor(a);
    Require(editor::host_seam::UndoOne(), "undo-delete-second", "");
    PumpAll();
    Snapshot(); ra = Rec(a);
    Check("second-restore-fresh-generation", ra && ra->gen != 0 && ra->gen != genA1, UidGen(a));
    CheckInvariants("restore-same-uid-invariants");
    CheckLockDiscipline("restore-same-uid");
}

// failure: Forget is explicit removal and invalidates the pending restore work (an undo of the delete
// must not materialize a new physical object for a record that no longer exists)
static void CaseForgetInvalidatesRestore() {
    BeginCase("forget-invalidates-restore");
    const int uid = Spawn("/object/box.prefab", { 2, 0, 2 });
    PumpAll();
    Snapshot();
    const uintptr_t handle0 = Rec(uid)->obj;
    DeleteViaEditor(uid);
    Snapshot();
    Check("deleted-and-hidden", Rec(uid) && Rec(uid)->hidden, UidGen(uid));
    ForgetViaEditor(uid);
    Snapshot();
    Check("forget-removes-record", Rec(uid) == nullptr, "registry=" + std::to_string(host::RegistrySize()));
    const int createsBefore = oracle::creates;
    Require(editor::host_seam::UndoOne(), "undo-delete-of-forgotten-act", "");
    PumpAll();
    Snapshot();
    Check("forgotten-not-resurrected", Rec(uid) == nullptr, "registry=" + std::to_string(host::RegistrySize()));
    Check("no-physical-object-for-forgotten-record", oracle::creates == createsBefore && VisibleCount() == 0,
          "creates=" + std::to_string(oracle::creates) + " before=" + std::to_string(createsBefore) + " visible=" + std::to_string(VisibleCount()));
    Check("forgotten-record-leaves-no-live-handle", !oracle::Live(handle0), oracle::Describe(handle0));
    CheckInvariants("forget-invariants");
    CheckLockDiscipline("forget");
}

// failure/event: a create completion that arrives after a hide+restore of the same record is disposed,
// never attached to the restored incarnation
static void CaseStaleCreateAfterRestore() {
    BeginCase("stale-create-after-restore");
    const int uid = Spawn("/object/box.prefab", { 3, 1, 3 });
    oracle::duringCreate = [uid] {
        DeleteViaEditor(uid);                                  // hide while the engine create is in flight
        Require(editor::host_seam::UndoOne(), "restore-during-create", "");   // restore the same logical record
    };
    PumpAll();
    Snapshot();
    const uintptr_t stale = oracle::createdOrder.empty() ? 0 : oracle::createdOrder[0];
    const SpawnedObj* r = Rec(uid);
    Check("record-restored-not-replaced", r && !r->hidden && r->uid == uid, UidGen(uid));
    Check("next-uid-stable", host::NextUid() == uid + 1, "nextUid=" + std::to_string(host::NextUid()));
    Check("stale-completion-disposed", stale != 0 && !oracle::Live(stale) && oracle::DisposedHandle(stale), oracle::Describe(stale));
    Check("stale-not-attached", r && r->obj != stale && r->obj != 0 && oracle::Live(r->obj), UidGen(uid));
    Check("exactly-one-live-handle", ComputeInv().live == 1, InvStr(ComputeInv()));
    CheckInvariants("stale-create-invariants");
    CheckLockDiscipline("stale-create");
}

// failure/event: forgetting a record from inside the engine callback of a replacement must not make the
// replacement follow a shifted registry index (the physical object is disposed or attached to its own uid)
static void CaseReentrantForgetReplace() {
    BeginCase("reentrant-forget-replace");
    const int a = Spawn("/object/a.prefab", { 1, 0, 0 });
    const int b = Spawn("/object/b.prefab", { 2, 0, 0 });
    const int c = Spawn("/object/c.prefab", { 3, 0, 0 });
    PumpAll();
    Snapshot();
    const uintptr_t hc = Rec(c)->obj; const uintptr_t ha = Rec(a)->obj;
    oracle::released.insert(ha);                       // forgetting A releases its engine object to the game
    oracle::duringRemove = [a] { ForgetViaEditor(a); };// re-entrant forget during the replacement's removal
    const int nextUidBefore = host::NextUid();
    Require(MoveFinal(c, { 30, 2, 30 }, { 10, 0, 0 }, 2.0f), "final-move-queued", "");
    PumpAll();
    Snapshot();
    const SpawnedObj* rc = Rec(c);
    Check("record-c-kept", rc && !rc->hidden && rc->uid == c, UidGen(c));
    Check("record-c-replaced-handle", rc && rc->obj != 0 && rc->obj != hc && oracle::Live(rc->obj), UidGen(c));
    Check("record-c-new-pose", rc && std::fabs(rc->pos.x - 30.0f) < 0.001f && std::fabs(rc->scale - 2.0f) < 0.001f, UidGen(c));
    Check("old-c-handle-disposed", oracle::DisposedHandle(hc), oracle::Describe(hc));
    Check("forgotten-record-gone", Rec(a) == nullptr, "registry=" + std::to_string(host::RegistrySize()));
    Check("record-b-untouched", Rec(b) && Rec(b)->obj != 0 && oracle::Live(Rec(b)->obj), UidGen(b));
    Check("no-uid-allocated", host::NextUid() == nextUidBefore, "nextUid=" + std::to_string(host::NextUid()));
    CheckInvariants("reentrant-forget-invariants");
    CheckLockDiscipline("reentrant-forget");
}

// failure/event: a final move queued before a hide is dropped; no physical object may materialize for a
// hidden tombstone
static void CaseStaleFinalMoveAfterHide() {
    BeginCase("stale-final-move-after-hide");
    const int a = Spawn("/object/a.prefab", { 1, 0, 0 });
    const int b = Spawn("/object/b.prefab", { 2, 0, 0 });
    PumpAll();
    Snapshot();
    const uintptr_t ha = Rec(a)->obj;
    Require(MoveFinal(a, { 9, 9, 9 }, { 0, 0, 0 }, 1.0f), "stale-move-queued", "");
    DeleteViaEditor(a);                                  // hide before the queued job runs
    PumpAll();
    Snapshot();
    const SpawnedObj* ra = Rec(a);
    Check("record-still-hidden", ra && ra->hidden && ra->obj == 0, UidGen(a));
    Check("stale-move-created-nothing", oracle::creates == 2, "creates=" + std::to_string(oracle::creates));
    Check("hidden-handle-disposed", oracle::DisposedHandle(ha), oracle::Describe(ha));
    Check("other-record-intact", Rec(b) && oracle::Live(Rec(b)->obj), UidGen(b));
    Check("one-visible", VisibleCount() == 1, "visible=" + std::to_string(VisibleCount()));
    CheckInvariants("stale-final-move-invariants");
    CheckLockDiscipline("stale-final-move");
}

// happy: the template-free server spawn attaches to the record (uid, object and actor stay together)
static void CaseGimmickDirectAttach() {
    BeginCase("gimmick-direct-attach");
    host::SetDirectGimmick(true);
    const int uid = Spawn("/object/cd_gimmick/lamp.prefab", { 4, 0, 4 });
    Snapshot();
    Check("record-marked-gimmick", Rec(uid) && Rec(uid)->gimmick, UidGen(uid));
    PumpServerTick();
    PumpAll();
    Snapshot();
    const SpawnedObj* r = Rec(uid);
    Check("direct-attached", r && !r->hidden && r->obj == oracle::lastDirectSo && r->actor == oracle::lastDirectActor && r->obj != 0, UidGen(uid));
    Check("standin-cleared", r && !r->standin, UidGen(uid));
    Check("direct-called-once", oracle::directCalls == 1, "direct=" + std::to_string(oracle::directCalls));
    CheckInvariants("gimmick-direct-invariants");
    CheckLockDiscipline("gimmick-direct");
}

// failure/event: a direct server spawn that completes after the record was hidden disposes BOTH the scene
// object and the actor, and the queue push for the actor happens outside the registry lock
static void CaseGimmickDirectReject() {
    BeginCase("gimmick-direct-reject");
    host::SetDirectGimmick(true);
    const int uid = Spawn("/object/cd_gimmick/lamp.prefab", { 4, 0, 4 });
    oracle::duringDirect = [uid] { DeleteViaEditor(uid); };
    PumpServerTick();
    PumpAll();
    Snapshot();
    const SpawnedObj* r = Rec(uid);
    Check("record-hidden-during-spawn", r && r->hidden, UidGen(uid));
    Check("rejected-scene-object-disposed", oracle::lastDirectSo != 0 && oracle::DisposedHandle(oracle::lastDirectSo), oracle::Describe(oracle::lastDirectSo));
    Check("rejected-actor-disposed", oracle::lastDirectActor != 0 && oracle::actorsRemoved.count(oracle::lastDirectActor) == 1, "actor=" + Hex(oracle::lastDirectActor));
    Check("no-live-leftover", ComputeInv().live == 0 || ComputeInv().orphans == 0, InvStr(ComputeInv()));
    CheckInvariants("gimmick-direct-reject-invariants");
    CheckLockDiscipline("gimmick-direct-reject");
}

// happy: template replay refused once -> production stand-in -> retry attaches the server object and
// disposes the stand-in; the logical uid never changes
static void CaseGimmickStandinRetry() {
    BeginCase("gimmick-standin-retry");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 1;
    const int uid = Spawn("/object/cd_gimmick/lamp.prefab", { 5, 0, 5 });
    PumpServerTick();                                    // replay refused: production queues + creates the stand-in
    Snapshot();
    const SpawnedObj* r = Rec(uid);
    Check("standin-attached", r && !r->hidden && r->standin && r->obj != 0, UidGen(uid));
    const uintptr_t standin = r ? r->obj : 0;
    Check("replay-attempted-once", oracle::replayCalls == 1, "replay=" + std::to_string(oracle::replayCalls));
    PumpServerTick();                                    // retry with a fresh template: attaches, stand-in goes
    PumpAll();
    Snapshot(); r = Rec(uid);
    Check("retry-attached-server-object", r && !r->hidden && !r->standin && r->obj == oracle::lastReplaySo && r->actor == oracle::lastReplayActor, UidGen(uid));
    Check("standin-disposed", oracle::DisposedHandle(standin), oracle::Describe(standin));
    Check("uid-stable-through-retry", r && r->uid == uid, UidGen(uid));
    Check("next-uid-untouched", host::NextUid() == uid + 1, "nextUid=" + std::to_string(host::NextUid()));
    CheckInvariants("standin-retry-invariants");
    CheckLockDiscipline("standin-retry");
}

// failure/event: a replay that succeeds after the record was hidden is disposed (scene object and actor)
static void CaseGimmickReplayRejectAfterHide() {
    BeginCase("gimmick-replay-reject-after-hide");
    host::SetDirectGimmick(false);
    oracle::templateReady = true;
    oracle::replayRefusals = 1;
    const int uid = Spawn("/object/cd_gimmick/lamp.prefab", { 6, 0, 6 });
    PumpServerTick();
    Snapshot();
    Require(Rec(uid) && Rec(uid)->obj != 0 && Rec(uid)->standin, "standin-present", UidGen(uid));
    oracle::replayRefusals = 0;
    oracle::duringReplay = [uid] { DeleteViaEditor(uid); };
    PumpServerTick();
    PumpAll();
    Snapshot();
    const SpawnedObj* r = Rec(uid);
    Check("record-hidden-during-replay", r && r->hidden, UidGen(uid));
    Check("replay-scene-object-disposed", oracle::lastReplaySo != 0 && oracle::DisposedHandle(oracle::lastReplaySo), oracle::Describe(oracle::lastReplaySo));
    Check("replay-actor-disposed", oracle::lastReplayActor != 0 && oracle::actorsRemoved.count(oracle::lastReplayActor) == 1, "actor=" + Hex(oracle::lastReplayActor));
    CheckInvariants("replay-reject-invariants");
    CheckLockDiscipline("replay-reject");
}

// happy: newest project adoption (including one that happens while the record is hidden) survives
// undo/redo; the restore never re-applies the older History snapshot
static void CaseAdoptionUndoRedo() {
    BeginCase("adoption-undo-redo");
    const int p1 = core::ProjectId("task7-adopted");
    const int p2 = core::ProjectId("task7-later-adoption");
    Check("project-ids-created", p1 > 0 && p2 > 0 && p1 != p2, "p1=" + std::to_string(p1) + " p2=" + std::to_string(p2));
    const int uid = Spawn("/object/a.prefab", { 1, 0, 0 });
    PumpAll();
    core::AssignProject(uid, p1);
    Snapshot();
    Check("adopted", Rec(uid) && Rec(uid)->proj == p1, UidGen(uid));
    Require(editor::host_seam::UndoOne(), "undo-spawn-after-adoption", "");
    core::AssignProject(uid, p2);                        // later adoption while the record is hidden
    Require(editor::host_seam::RedoOne(), "redo-spawn", "");
    PumpAll();
    Snapshot();
    const SpawnedObj* r = Rec(uid);
    Check("uid-kept", r && r->uid == uid && !r->hidden, UidGen(uid));
    Check("newest-adoption-kept", r && !r->hidden && r->obj != 0 && r->proj == p2, UidGen(uid));
    Check("redo-makes-history-act-current", editor::host_seam::UndoSize() == 1 && HistoryUid(false, 0, 0) == uid, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    DeleteViaEditor(uid);
    Require(editor::host_seam::UndoOne(), "undo-delete-after-adoption", "");
    PumpAll();
    Snapshot();
    Check("delete-undo-keeps-adoption", Rec(uid) && Rec(uid)->proj == p2 && !Rec(uid)->hidden && Rec(uid)->obj != 0, UidGen(uid));
    CheckInvariants("adoption-invariants");
    CheckLockDiscipline("adoption");
}

// contract: the shared v0.97 Act/SetGroup History retains exactly the newest 1000 entries
static void CaseSetGroupAndLimit() {
    BeginCase("setgroup-and-limit");
    Check("history-limit-is-1000", editor::kHistoryLimit == 1000, "limit=" + std::to_string(editor::kHistoryLimit));
    const int a = Spawn("/object/a.prefab", { 1, 0, 0 });
    const int b = Spawn("/object/b.prefab", { 2, 0, 0 });
    PumpAll();
    editor::host_seam::SelectUid(a);
    editor::host_seam::SelectAdd(b);
    editor::GroupSel(true);
    Snapshot();
    const int g1 = Rec(a) ? Rec(a)->group : 0;
    Check("grouped-through-editor", g1 > 0 && Rec(b) && Rec(b)->group == g1, UidGen(a));
    Check("setgroup-act-recorded", editor::host_seam::UndoSize() == 3 && HistoryKind(false, 2, 0) == 3 /*Act::SetGroup*/, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    Require(editor::host_seam::UndoOne(), "undo-group", "");
    Snapshot();
    Check("group-undone", Rec(a) && Rec(a)->group == 0 && Rec(b) && Rec(b)->group == 0, UidGen(a));
    Require(editor::host_seam::RedoOne(), "redo-group", "");
    Snapshot();
    Check("group-redone", Rec(a) && Rec(a)->group == g1, UidGen(a));
    DeleteViaEditor(a);
    Require(editor::host_seam::UndoOne(), "undo-delete-grouped-member", "");
    PumpAll();
    Snapshot();
    Check("group-kept-through-restore", Rec(a) && Rec(a)->group == g1 && !Rec(a)->hidden, UidGen(a));
    Check("limit-entry-count-unchanged", editor::host_seam::UndoSize() == 3, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    editor::host_seam::SelectUid(a); editor::host_seam::SelectAdd(b);
    editor::GroupSel(true); const auto firstSerial = editor::g_undo.back().serial;
    for (int n = 0; n < 1000; ++n) editor::GroupSel((n & 1) != 0);
    Check("shared-history-evicts-only-oldest-at-1000", editor::g_undo.size() == 1000 && editor::g_undo.front().serial == firstSerial + 1 && editor::g_undo.back().serial == firstSerial + 1000, "undo=" + std::to_string(editor::g_undo.size()));
    Require(editor::host_seam::UndoOne() && editor::host_seam::RedoOne(), "capped-history-still-undoable", "");
    CheckInvariants("setgroup-invariants");
    CheckLockDiscipline("setgroup");
}

// contract: engine and server work is queued outside the registry lock, for generic and gimmick records
static void CaseLockDiscipline() {
    BeginCase("lock-discipline");
    const int a = Spawn("/object/a.prefab", { 1, 0, 0 });
    PumpAll();
    oracle::lockFreeDuringEngineCalls = true;
    DeleteViaEditor(a);
    PumpAll();
    Check("generic-hide-engine-lock-free", oracle::lockFreeDuringEngineCalls, "");
    Check("generic-hide-no-queue-under-lock", host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations()));
    host::SetDirectGimmick(true);
    const int g = Spawn("/object/cd_gimmick/lamp.prefab", { 4, 0, 4 });
    PumpServerTick();
    PumpAll();
    Snapshot();
    Require(Rec(g) && Rec(g)->actor != 0, "gimmick-attached-before-hide", UidGen(g));
    const uintptr_t actor = Rec(g)->actor;
    // The game tears the server scene object down with its actor (production only removes the actor here), so that
    // handle is released to the game, not left as a host-side orphan.
    oracle::released.insert(Rec(g)->obj);
    const int violationsBefore = host::LockViolations();
    DeleteViaEditor(g);
    PumpServerTick();
    PumpAll();
    Check("gimmick-hide-no-queue-under-lock", host::LockViolations() == violationsBefore, "before=" + std::to_string(violationsBefore) + " after=" + std::to_string(host::LockViolations()));
    Check("gimmick-hide-removed-actor", oracle::actorsRemoved.count(actor) == 1, "actor=" + Hex(actor));
    CheckInvariants("lock-discipline-invariants");
}

// ---- main -------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : ".";
    host::OpenLog(g_fixtureDir + "\\object_lifetime.log");
    core::Log("[object_lifetime] run start: fixtureDir=%s", g_fixtureDir.c_str());
    g_probe.Start();
    InstallSeam();
    core::g_recreateOnMove = true;               // production default: the final move re-creates
    host::SetDirectGimmick(false);

    struct Entry { const char* id; void (*fn)(); };
    const Entry entries[] = {
        { "create-attach", CaseCreateAttach },
        { "restore-same-uid", CaseHistoryRestoreSameUid },
        { "forget-invalidates-restore", CaseForgetInvalidatesRestore },
        { "stale-create-after-restore", CaseStaleCreateAfterRestore },
        { "reentrant-forget-replace", CaseReentrantForgetReplace },
        { "stale-final-move-after-hide", CaseStaleFinalMoveAfterHide },
        { "gimmick-direct-attach", CaseGimmickDirectAttach },
        { "gimmick-direct-reject", CaseGimmickDirectReject },
        { "gimmick-standin-retry", CaseGimmickStandinRetry },
        { "gimmick-replay-reject-after-hide", CaseGimmickReplayRejectAfterHide },
        { "adoption-undo-redo", CaseAdoptionUndoRedo },
        { "setgroup-and-limit", CaseSetGroupAndLimit },
        { "lock-discipline", CaseLockDiscipline },
    };

    for (const Entry& e : entries) {
        try {
            e.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("CASE-ABORT %s: %s\n", e.id, ex.what());
            core::Log("[object_lifetime] CASE-ABORT %s: %s", e.id, ex.what());
            if (!g_cases.empty() && g_cases.back().failures == 0) ++g_cases.back().failures;
        }
    }

    // cases.json: one machine receipt per case with the observed identity values
    {
        std::string json = "{\"suite\":\"ObjectLifetime\",\"status\":\"" + std::string(g_failures == 0 ? "PASS" : "FAIL") +
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
        core::Log("[object_lifetime] cases: %s", json.c_str());
    }
    std::printf("CASES=%zu\nASSERTIONS=%d\nFAILURES=%d\n", g_cases.size(), g_assertions, g_failures);
    core::Log("[object_lifetime] run end: cases=%zu assertions=%d failures=%d lockViolations=%d", g_cases.size(), g_assertions, g_failures, host::LockViolations());
    g_probe.Stop();
    return g_failures == 0 ? 0 : 1;
}
