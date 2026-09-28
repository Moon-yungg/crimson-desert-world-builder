// GroupPlacement host fixture (plan wb079-unified-77f68967, Task 11 / C4+C5).
//
// The production core TU is compiled by production_core_host.cpp (WB_UNIFIED_HOST_TEST) and the ACTUAL editor
// TU is included here once, so the group admission (core::AdmitGroupCopy), the registry, the queues, the C4
// copy envelopes and the editor's camera-aware grab (StartGrab/Place/Member/DropCarried/CancelCarried) and
// History are production code. The shipped codec (proj_codec.cpp) and geometry (wb_group_math.cpp) are linked
// as their own TUs; the real input.cpp/Win32 backend are linked (INPUT). The only substituted boundaries are
// the native engine calls behind host::Seam().
//
// Cases assert the Task 11 contract:
//   PARTITIONS             - source groups 42,7,42,0,7 become TWO independent nonzero maps, zero stays zero
//   TWO-COPIES             - two admitted copies own independent partitions and independent envelopes
//   PIVOT                  - the saved (off-origin) anchor is the placement pivot, not the member average
//   TILT                   - yaw + positive uniform scale survive, member pitch/roll are preserved, group tilt off
//   CAMERA-GRAB            - the copy is carried by the EXISTING camera-aware grab; Home/camera stays usable
//   PERSIST-UNDO           - drop/undo/redo/save/reload/re-export keep partitions, envelopes and pivot
//   SINGLE-SURVIVOR-DIFF-BOUNDS - one surviving imported member keeps the saved pivot/pose (receiver box wins never)
//   MISSING-PREFAB         - a receiver-unknown prefab is a NAMED preflight exclusion, the rest still place
//   CANCEL-LATE            - cancel reports the ACTUAL terminal rows (never the queue depth) and keeps one History
//   INVALID-ADMISSION      - an invalid document mutates nothing (scene, History, queues, allocations)
//
// Machine output: CHECK lines on stdout, one ASSERTIONS=<n> line, a per-case cases.json receipt and a trace at
// <fixtureDir>\group_placement_trace.json; the production log sink is <fixtureDir>\group_placement.log.
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
#include <limits>
#include <iterator>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "production_core_host.cpp"            // actual core once; native NPC packet boundary is set below
#include "../../asi/cdmodkit/editor.cpp"        // the actual editor TU (StartGrab/Place/Member, Drop/Cancel, History)
#include "../../asi/cdmodkit/proj_codec.h"       // the shipped disk-value codec (linked as its own TU)
#include "../../asi/cdmodkit/wb_group_math.h"    // the shipped saved-anchor geometry (linked as its own TU)
#include "production_host.h"                    // host::Seam / host::PumpGame / host::PumpServer / prefab cache seam

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

// Fixture-local migration adapters, not shipped seams or a second placement engine. The v0.95
// public mouse ownership and actual camera toggle replace the removed g_placing/Home scheme.
namespace editor { namespace host_seam {
void CancelCarriedNow() { ::editor::CancelCarried(); }
void DropCarriedNow() { ::editor::DropCarried(); }
bool Placing() { return ::editor::Placing(); }
bool SavedPivot() { return g_place.req && !g_place.haveCenter && g_place.prefabIdx == -1; }
Vec3 PlaceCenter() { return g_place.center; }
size_t CarriedCount() { return g_place.m.size(); }
int CarriedUid(size_t i) { return g_place.m.at(i).uid; }
core::PlaceRequestHandle CarriedRequest() { return g_place.req; }
void GrabUids(const std::vector<int>& uids, bool isNew, const std::string& name) { StartGrab(uids, isNew, name); }
void BeginCameraMode() {
    g_open = true; host::SetCameraAvailable(true); host::CaptureCamera({0, 10, -20}, 0, 0);
    if (!g_cameraMode) ToggleCameraMode();
}
bool CameraModeActive() { return g_cameraMode; }
} }

// ---- seam oracle -----------------------------------------------------------------------------------------
namespace oracle {
struct Obj { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1; uintptr_t actor = 0; bool live = true; };

std::map<uintptr_t, Obj> objects;
std::set<uintptr_t> disposed;
uintptr_t next = 0x10000;
int createCalls = 0, creates = 0, removes = 0, moveInPlaceCalls = 0, liveMoveCalls = 0;
bool lockFreeDuringEngineCalls = true;

void Reset() {
    objects.clear(); disposed.clear(); next = 0x10000;
    createCalls = creates = removes = moveInPlaceCalls = liveMoveCalls = 0;
    lockFreeDuringEngineCalls = true;
}
uintptr_t MakeObj(const std::string& prefab, Vec3 pos, Rot rot, float scale) {
    const uintptr_t h = next++;
    objects.emplace(h, Obj{ prefab, pos, rot, scale, 0, true });
    return h;
}
bool Live(uintptr_t h) { auto it = objects.find(h); return it != objects.end() && it->second.live; }
bool DisposedHandle(uintptr_t h) { return disposed.count(h) != 0; }
int LiveCount() { int n = 0; for (auto& kv : objects) if (kv.second.live) ++n; return n; }
std::string Describe(uintptr_t h) {
    auto it = objects.find(h);
    char b[200];
    if (it == objects.end()) { std::snprintf(b, sizeof b, "handle=0x%llx unknown", (unsigned long long)h); return b; }
    std::snprintf(b, sizeof b, "handle=0x%llx live=%d prefab=%s", (unsigned long long)h, it->second.live ? 1 : 0, it->second.prefab.c_str());
    return b;
}
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
        if (!cv_.wait_for(l, std::chrono::seconds(5), [this] { return done_; })) return false;
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
std::string g_trace;
struct CaseRec { std::string id; int assertions = 0, failures = 0; };
std::vector<CaseRec> g_cases;

void Check(const char* label, bool ok, const std::string& observed = "") {
    ++g_assertions;
    if (!g_cases.empty()) { ++g_cases.back().assertions; if (!ok) ++g_cases.back().failures; }
    if (!ok) ++g_failures;
    std::printf("CHECK %s %s%s%s\n", ok ? "PASS" : "FAIL", label, observed.empty() ? "" : " | ", observed.c_str());
    core::Log("[group_placement] %s %s %s%s%s", g_case.c_str(), ok ? "PASS" : "FAIL", label,
              observed.empty() ? "" : " | ", observed.c_str());
}
void Require(bool ok, const char* label, const std::string& observed = "") {
    Check(label, ok, observed);
    if (!ok) throw std::runtime_error(std::string(label) + (observed.empty() ? "" : (" | " + observed)));
}
void BeginCase(const char* id) {
    g_cases.push_back(CaseRec{ id, 0, 0 });
    g_case = id;
    std::printf("CASE %s\n", id);
    core::Log("[group_placement] CASE %s", id);
}

// ---- engine seam -----------------------------------------------------------------------------------------
void InstallSeam() {
    host::Engine& e = host::Seam();
    e.ready = true;
    e.createGeneric = [](const std::string& prefab, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::createCalls;
        ++oracle::creates;
        return oracle::MakeObj(prefab, pos, rot, scale);
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
    e.removeActor = [](uintptr_t) -> bool { return false; };
    e.directGimmick = [](const std::string&, Vec3, Rot, float, uintptr_t*, uintptr_t*) -> bool { return false; };
    e.replay = [](int, const std::string&, Vec3, Rot, float, uintptr_t*, uintptr_t*) -> bool { return false; };
    e.templateReady = [] { return false; };
}

// ---- pumping ---------------------------------------------------------------------------------------------
void PumpGame_() {
    int guard = 0;
    while (host::PumpGame()) { if (++guard > 4096) throw std::runtime_error("the production game queue did not settle"); }
}
void PumpAll() { PumpGame_(); host::PumpServer(); PumpGame_(); }

// ---- the receiver's prefab cache (admission preflight / box hints) ---------------------------------------
std::vector<core::PrefabInfo> PrefabRow(const std::string& path, float sx, float sy, float sz, float cx, float cy, float cz) {
    core::PrefabInfo p; p.path = path; p.hasCenter = true; p.sx = sx; p.sy = sy; p.sz = sz; p.cx = cx; p.cy = cy; p.cz = cz;
    return { p };
}
void InstallIndex(bool withB = true, const std::string& b = "/object/group_b.prefab") {
    std::vector<core::PrefabInfo> idx = PrefabRow("/object/group_a.prefab", 1.5f, 2.0f, 1.0f, 0.1f, 1.0f, -0.2f);
    if (withB) { const auto more = PrefabRow(b, 2.5f, 3.0f, 2.0f, 0.0f, 1.5f, 0.0f); idx.insert(idx.end(), more.begin(), more.end()); }
    host::SetPrefabIndex(idx);
}

// ---- fixture directory -----------------------------------------------------------------------------------
std::string ProjectDir() { return g_fixtureDir + "\\projects"; }
std::string ProjPath(const std::string& name) { return ProjectDir() + "\\" + name + ".cdproj"; }
bool ReadFileText(const std::string& path, std::string& out) {
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
    if (!f) return false;
    char b[4096]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}
void CleanFixtureDir() {
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((ProjectDir() + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { DeleteFileA((ProjectDir() + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(ProjectDir().c_str());
    CreateDirectoryA(ProjectDir().c_str(), nullptr);
}

// ---- documents -------------------------------------------------------------------------------------------
// The required partition fixture 42,7,42,0,7 as real group-file text (the codec is the reader under test; the
// author stays free to write any group ids). The saved anchor is off-origin for the AABB (bottom centre, never
// the member average) and there are two per-copy envelopes (record 5 carries its own).
std::string BoundsLine(const char* anchor, const char* min, const char* max) {
    return "anchor=" + std::string(anchor) + " min=" + std::string(min) + " max=" + std::string(max) + " quality=approx";
}
std::string RecipeText() {
    const int groups[5] = { 42, 7, 42, 0, 7 };
    std::string s = "# cdproj v3 kind=group\n";
    s += "# wb-document " + BoundsLine("10,1,20", "-10,1,-20", "30,50,60") + "\n";
    s += "# wb-envelope id=1 " + BoundsLine("10,1,20", "-10,1,-20", "30,50,60") + "\n";
    s += "# wb-envelope id=2 " + BoundsLine("5,3,9", "0,3,0", "10,20,18") + "\n";
    for (int i = 0; i < 5; ++i) {
        char row[256];
        std::snprintf(row, sizeof row, "%s|%g|%g|%g|%g|%g|%d|%g|%g\n",
                      (i % 2) ? "/object/group_b.prefab" : "/object/group_a.prefab",
                      12.0 + i * 3.0, 4.0 + i, 25.0 - i * 2.0, 13.0 + i, 1.25, groups[i % 5], 19.0 + i, -23.0 - i);
        s += "# wb-member record=" + std::to_string(i + 1) + " envelope=" + (i == 4 ? "2" : "1") + "\n";
        s += row;
    }
    return s;
}
// One imported member whose saved bounds are far larger than (and unrelated to) the receiver's box hint.
std::string SingleRecipeText() {
    std::string s = "# cdproj v3 kind=group\n";
    s += "# wb-document " + BoundsLine("10,1,20", "0,1,10", "20,21,30") + "\n";
    s += "# wb-envelope id=1 " + BoundsLine("10,1,20", "0,1,10", "20,21,30") + "\n";
    s += "# wb-member record=1 envelope=1\n";
    s += "/object/group_a.prefab|12|4|25|30|1.5|42|11|-7\n";
    return s;
}
bool ParseGroupText(const std::string& text, proj_codec::Document& out, std::string& error) {
    return proj_codec::Parse(text, "group_placement.cdgroup", proj_codec::Kind::Group, out, error);
}
bool ParseDocumentFile(const std::string& path, proj_codec::Document& doc, std::string& error) {
    std::string bytes;
    if (!ReadFileText(path, bytes)) { error = "cannot read " + path; return false; }
    return proj_codec::Parse(bytes, path, proj_codec::Kind::Project, doc, error);
}

// ---- registry snapshot helpers ---------------------------------------------------------------------------
void Snapshot() { if (g_list.capacity() < 256) g_list.reserve(256); g_list = core::Spawned(); }
const SpawnedObj* Rec(int uid) { for (auto& o : g_list) if (o.uid == uid) return &o; return nullptr; }
int VisibleCount() { int n = 0; for (const SpawnedObj& o : g_list) if (!o.hidden) ++n; return n; }
std::string UidStr(int uid) {
    Snapshot();
    const SpawnedObj* o = Rec(uid);
    char b[320];
    if (!o) { std::snprintf(b, sizeof b, "uid=%d missing nextUid=%d", uid, host::NextUid()); return b; }
    std::snprintf(b, sizeof b, "uid=%d obj=0x%llx hidden=%d proj=%d group=%d pos=(%.3f %.3f %.3f) yaw=%.3f pitch=%.3f roll=%.3f scale=%.3f",
                  uid, (unsigned long long)o->obj, o->hidden ? 1 : 0, o->proj, o->group, o->pos.x, o->pos.y, o->pos.z,
                  o->rot.yaw, o->rot.pitch, o->rot.roll, o->scale);
    return b;
}

// ---- world state (scene + History + selection + queues + allocations + engine handles) -------------------
struct WorldState {
    std::vector<std::string> records;
    std::vector<std::string> history;
    std::set<uintptr_t> live;
    int nextUid = 0, pending = 0, undo = 0, redo = 0, selection = 0, creates = 0, removes = 0, lockViolations = 0;
};
WorldState CaptureWorld() {
    WorldState w;
    w.nextUid = host::NextUid(); w.pending = core::PendingSpawns();
    w.undo = (int)editor::host_seam::UndoSize(); w.redo = (int)editor::host_seam::RedoSize();
    w.selection = editor::host_seam::SelectionSize();
    w.creates = oracle::creates; w.removes = oracle::removes; w.lockViolations = host::LockViolations();
    Snapshot();
    char b[384];
    for (const SpawnedObj& o : g_list) {
        std::snprintf(b, sizeof b, "uid=%d %s %.3f %.3f %.3f yaw=%.3f pitch=%.3f roll=%.3f scale=%.3f grp=%d proj=%d hidden=%d live=%d",
                      o.uid, o.prefab.c_str(), o.pos.x, o.pos.y, o.pos.z, o.rot.yaw, o.rot.pitch, o.rot.roll, o.scale, o.group, o.proj,
                      o.hidden ? 1 : 0, oracle::Live(o.obj) ? 1 : 0);
        w.records.push_back(b);
    }
    for (const auto& kv : oracle::objects) if (kv.second.live) w.live.insert(kv.first);
    for (int redo = 0; redo < 2; ++redo) {
        const size_t n = redo ? editor::host_seam::RedoSize() : editor::host_seam::UndoSize();
        for (size_t e = 0; e < n; ++e) {
            editor::host_seam::ActView v;
            char line[160];
            if (!editor::host_seam::HistoryView(redo != 0, e, 0, &v)) { w.history.push_back((redo ? std::string("redo[") : std::string("undo[")) + std::to_string(e) + "]=missing"); continue; }
            std::snprintf(line, sizeof line, "%s[%zu] kind=%d uid=%d group=%d group1=%d proj=%d", redo ? "redo" : "undo", e, v.kind, v.uid, v.group, v.group1, v.proj);
            w.history.push_back(line);
        }
    }
    return w;
}
bool SameWorld(const WorldState& a, const WorldState& b) {
    return a.records == b.records && a.history == b.history && a.live == b.live && a.nextUid == b.nextUid &&
           a.pending == b.pending && a.undo == b.undo && a.redo == b.redo && a.selection == b.selection &&
           a.creates == b.creates && a.removes == b.removes && a.lockViolations == b.lockViolations;
}
std::string WorldDiff(const WorldState& a, const WorldState& b) {
    if (a.records != b.records) return "records differ";
    if (a.history != b.history) return "history differs";
    if (a.live != b.live) return "live handles differ";
    if (a.nextUid != b.nextUid) return "nextUid " + std::to_string(a.nextUid) + "->" + std::to_string(b.nextUid);
    if (a.pending != b.pending) return "pending " + std::to_string(a.pending) + "->" + std::to_string(b.pending);
    if (a.undo != b.undo || a.redo != b.redo) return "history sizes";
    if (a.selection != b.selection) return "selection " + std::to_string(a.selection) + "->" + std::to_string(b.selection);
    if (a.creates != b.creates || a.removes != b.removes) return "engine calls " + std::to_string(a.creates) + "/" + std::to_string(a.removes) + "->" + std::to_string(b.creates) + "/" + std::to_string(b.removes);
    if (a.lockViolations != b.lockViolations) return "queue-under-lock violations";
    return "identical";
}
void CheckWorldUnchanged(const char* label, const WorldState& before) {
    const WorldState now = CaptureWorld();
    Check(label, SameWorld(before, now), WorldDiff(before, now));
}

// ---- value helpers ---------------------------------------------------------------------------------------
bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
bool PointNear(const Vec3& a, const proj_codec::Point& b, double eps = 1e-4) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}
std::string PointStr(const proj_codec::Point& p) {
    char b[160]; std::snprintf(b, sizeof b, "(%.6g %.6g %.6g)", p.x, p.y, p.z); return b;
}
std::string VecStr(const Vec3& p) {
    char b[160]; std::snprintf(b, sizeof b, "(%.6g %.6g %.6g)", p.x, p.y, p.z); return b;
}
// The shipped saved-anchor geometry is the oracle for the expected destination values.
bool ExpectedRecords(const proj_codec::Document& doc, Vec3 target, double yaw, double factor, std::vector<proj_codec::Record>& out) {
    return wb_group_math::AnchorTransform(doc.records, doc.bounds, { target.x, target.y, target.z }, yaw, factor, out);
}
std::string GroupMap(const std::vector<int>& uids, const std::vector<proj_codec::Record>& src) {
    Snapshot();
    std::string s;
    for (size_t i = 0; i < uids.size(); ++i) {
        const SpawnedObj* o = Rec(uids[i]);
        s += "[" + std::to_string(i) + " src=" + std::to_string(src.size() > i ? src[i].group : -1) + " -> " + std::to_string(o ? o->group : -1) + "]";
    }
    return s;
}
core::PlaceRequestView View(const core::PlaceRequestHandle& req) { return core::PlaceRequestState(req); }
const core::PlaceRow* Row(const core::PlaceRequestView& v, int rowId) {
    for (const auto& r : v.rows) if (r.rowId == rowId) return &r;
    return nullptr;
}
std::string VStr(const core::PlaceRequestView& v) {
    char b[256];
    std::snprintf(b, sizeof b, "requested=%d attached=%d excluded=%d failed=%d canceled=%d pending=%d settled=%d cleanupPending=%d",
                  v.requested, v.attached, v.excluded, v.failed, v.canceled, v.pending, v.settled ? 1 : 0, v.cleanupPending ? 1 : 0);
    return b;
}
std::string RStr(const core::PlaceRow& r) {
    char b[320];
    std::snprintf(b, sizeof b, "row=%d prefab=%s state=%d lane=%d uid=%d obs=%d removed=%d cleanup=%d reason=%s",
                  r.rowId, r.prefab.c_str(), r.state, r.lane, r.uid, r.attachObservations, r.removedAfterAttach ? 1 : 0, r.cleanupPending ? 1 : 0, r.reason.c_str());
    return b;
}
void CheckEquation(const char* label, const core::PlaceRequestView& v) {
    Check(label, v.requested == v.attached + v.excluded + v.failed + v.canceled + v.pending, VStr(v));
}
void Trace(const std::string& name, const std::string& json) {
    g_trace += "{\"case\":" + name + "," + json + "}\n";
}

// ---- case helpers ----------------------------------------------------------------------------------------
void ResetWorld() {
    editor::host_seam::CancelCarriedNow();
    editor::host_seam::ResetPlacement();
    core::DeleteAllSpawned();
    PumpGame_();
    host::PumpServer();
    PumpGame_();
    oracle::Reset();
    editor::host_seam::ResetHistory();
    editor::host_seam::ClearSelection();
    CleanFixtureDir();
    host::ResetAutoloadDone();
    g_probe.CheckFree();
}
void CheckNoQueueUnderLock(const char* label) { Check(label, host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations())); }

// =========================================================================================================
// happy cases
// =========================================================================================================
// PARTITIONS: the required fixture 42,7,42,0,7 becomes exactly two independent nonzero maps; zero stays zero.
static void CasePartitions() {
    BeginCase("partitions");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "partitions-document-valid", error);
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(doc, { 100, 5, 200 }, 0.0, 1.0, uids, pivot);
    Require(rep.valid, "partitions-admission-valid", rep.error);
    Check("partitions-all-admitted", rep.admitted == 5 && rep.excluded == 0 && uids.size() == 5, "admitted=" + std::to_string(rep.admitted));
    PumpAll();
    Snapshot();
    Check("partitions-pivot-is-target", Near(pivot.x, 100) && Near(pivot.y, 5) && Near(pivot.z, 200), VecStr(pivot));
    const int g0 = Rec(uids[0]) ? Rec(uids[0])->group : -1;
    const int g1 = Rec(uids[1]) ? Rec(uids[1])->group : -1;
    const int g2 = Rec(uids[2]) ? Rec(uids[2])->group : -1;
    const int g3 = Rec(uids[3]) ? Rec(uids[3])->group : -1;
    const int g4 = Rec(uids[4]) ? Rec(uids[4])->group : -1;
    Check("partitions-two-independent-nonzero-maps", g0 > 0 && g1 > 0 && g0 != g1, GroupMap(uids, doc.records));
    Check("partitions-same-source-same-map", g0 == g2 && g1 == g4, GroupMap(uids, doc.records));
    Check("partitions-zero-stays-zero", g3 == 0, GroupMap(uids, doc.records));
    std::vector<proj_codec::Record> expected;
    Require(ExpectedRecords(doc, { 100, 5, 200 }, 0.0, 1.0, expected), "partitions-expected-transform", "");
    bool poses = true;
    for (size_t i = 0; i < uids.size(); ++i) {
        const SpawnedObj* o = Rec(uids[i]);
        poses &= o && PointNear(o->pos, expected[i].pos) && Near(o->rot.yaw, expected[i].yaw) && Near(o->scale, expected[i].scale);
    }
    Check("partitions-poses-are-saved-anchor-transform", poses, "expected " + PointStr(expected[0].pos));
    const core::PlaceRequestView v = View(rep.request);
    Check("partitions-rows-attached", v.attached == 5 && v.pending == 0 && v.excluded == 0 && v.failed == 0 && v.settled, VStr(v));
    CheckEquation("partitions-equation", v);
    Check("partitions-never-pending-spawns-count", core::PendingSpawns() == 0 && v.attached == 5, "pendingSpawns=" + std::to_string(core::PendingSpawns()));
    Trace("\"partitions\"", "\"admitted\":" + std::to_string(rep.admitted) + ",\"groups\":\"" + GroupMap(uids, doc.records) + "\"");
    CheckNoQueueUnderLock("partitions-no-queue-under-lock");
}

// TWO-COPIES: two admitted copies own independent partitions and independent per-copy envelopes.
static void CaseTwoCopies() {
    BeginCase("two-copies");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "two-copies-document-valid", error);
    std::vector<int> a, b; Vec3 pa{}, pb{};
    const core::GroupAdmissionReport ra = core::AdmitGroupCopy(doc, { 100, 5, 200 }, 0.0, 1.0, a, pa);
    const core::GroupAdmissionReport rb = core::AdmitGroupCopy(doc, { -50, 7, 60 }, 0.0, 1.0, b, pb);
    Require(ra.valid && rb.valid, "two-copies-admissions-valid", ra.error + rb.error);
    PumpAll();
    Snapshot();
    std::set<int> ga, gb;
    for (size_t i = 0; i < a.size(); ++i) { const SpawnedObj* o = Rec(a[i]); if (o && o->group) ga.insert(o->group); }
    for (size_t i = 0; i < b.size(); ++i) { const SpawnedObj* o = Rec(b[i]); if (o && o->group) gb.insert(o->group); }
    Check("two-copies-each-two-partitions", ga.size() == 2 && gb.size() == 2, "copyA=" + std::to_string(ga.size()) + " copyB=" + std::to_string(gb.size()));
    std::vector<int> shared;
    std::set_intersection(ga.begin(), ga.end(), gb.begin(), gb.end(), std::back_inserter(shared));
    Check("two-copies-partitions-independent", shared.empty(), "shared=" + std::to_string(shared.size()));
    Check("two-copies-each-zero-survives", (Rec(a[3]) ? Rec(a[3])->group : -1) == 0 && (Rec(b[3]) ? Rec(b[3])->group : -1) == 0, "");
    // The saved document proves the per-copy envelopes are independent: each copy owns its two envelopes, so the
    // authoritative value builder writes FOUR envelope records, each with the copy's translated saved bounds.
    Require(core::SaveProject("t11-two-copies"), "two-copies-save", core::ProjectError());
    proj_codec::Document saved; std::string err2;
    Require(ParseDocumentFile(ProjPath("t11-two-copies"), saved, err2), "two-copies-readback", err2);
    Check("two-copies-envelope-count", saved.envelopes.size() == 4 && saved.records.size() == 10, "envelopes=" + std::to_string(saved.envelopes.size()));
    bool envelopesOk = saved.envelopes.size() == 4;
    if (envelopesOk) {
        const proj_codec::Point env1 = doc.envelopes[0].bounds.anchor;
        const proj_codec::Point env2 = doc.envelopes[1].bounds.anchor;
        const proj_codec::Point shiftA{ 100 - doc.bounds.anchor.x, 5 - doc.bounds.anchor.y, 200 - doc.bounds.anchor.z };
        const proj_codec::Point shiftB{ -50 - doc.bounds.anchor.x, 7 - doc.bounds.anchor.y, 60 - doc.bounds.anchor.z };
        const proj_codec::Point expected[4] = {
            { env1.x + shiftA.x, env1.y + shiftA.y, env1.z + shiftA.z },
            { env2.x + shiftA.x, env2.y + shiftA.y, env2.z + shiftA.z },
            { env1.x + shiftB.x, env1.y + shiftB.y, env1.z + shiftB.z },
            { env2.x + shiftB.x, env2.y + shiftB.y, env2.z + shiftB.z },
        };
        std::vector<bool> matched(4, false);
        for (const auto& e : saved.envelopes) {
            bool hit = false;
            for (int i = 0; i < 4; ++i)
                if (!matched[i] && Near(e.bounds.anchor.x, expected[i].x) && Near(e.bounds.anchor.y, expected[i].y) && Near(e.bounds.anchor.z, expected[i].z)) { matched[i] = true; hit = true; break; }
            envelopesOk &= hit;
        }
    }
    Check("two-copies-envelopes-per-copy-translated", envelopesOk, "envelopes=" + std::to_string(saved.envelopes.size()));
    // the saved records reference the envelope IDs: each copy owns its own two, so the two id sets are disjoint
    std::set<int> envA, envB;
    for (size_t i = 0; i < saved.records.size(); ++i) (i < 5 ? envA : envB).insert(saved.records[i].envelope);
    std::vector<int> envOverlap;
    std::set_intersection(envA.begin(), envA.end(), envB.begin(), envB.end(), std::back_inserter(envOverlap));
    Check("two-copies-envelope-ids-independent", envA.size() == 2 && envB.size() == 2 && envOverlap.empty(),
          "envA=" + std::to_string(envA.size()) + " envB=" + std::to_string(envB.size()));
    // partition structure in the file: each admitted copy remapped its two source partitions to fresh ids, so
    // the two copies together own FOUR distinct nonzero maps and both zero rows stay zero.
    std::set<int> fileA, fileB;
    int zeros = 0;
    for (size_t i = 0; i < saved.records.size(); ++i) {
        const int g = saved.records[i].group;
        if (!g) { ++zeros; continue; }
        (i < 5 ? fileA : fileB).insert(g);
    }
    std::vector<int> overlap;
    std::set_intersection(fileA.begin(), fileA.end(), fileB.begin(), fileB.end(), std::back_inserter(overlap));
    Check("two-copies-file-partitions", fileA.size() == 2 && fileB.size() == 2 && zeros == 2 && overlap.empty(),
          "A=" + std::to_string(fileA.size()) + " B=" + std::to_string(fileB.size()) + " zeros=" + std::to_string(zeros));
    Trace("\"two-copies\"", "\"copyA\":" + std::to_string(ga.size()) + ",\"copyB\":" + std::to_string(gb.size()) + ",\"envelopes\":" + std::to_string(saved.envelopes.size()));
    CheckNoQueueUnderLock("two-copies-no-queue-under-lock");
}

// PIVOT: the saved off-origin anchor is the placement pivot (not the member average), and the existing grab
// carries the copy with that pivot while the camera mode stays usable.
static void CasePivot() {
    BeginCase("pivot");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "pivot-document-valid", error);
    editor::host_seam::BeginCameraMode();
    Require(editor::host_seam::PlaceGroupCopy(doc, { 100, 5, 200 }, 0.0f, 1.0f, "pivot"), "pivot-grab-started", "place=0");
    PumpAll();
    Snapshot();
    Check("pivot-grab-active", editor::host_seam::Placing(), "");
    Check("pivot-saved-pivot-flag", editor::host_seam::SavedPivot(), "");
    const Vec3 center = editor::host_seam::PlaceCenter();
    Check("pivot-center-is-saved-anchor", Near(center.x, 100) && Near(center.y, 5) && Near(center.z, 200), VecStr(center));
    proj_codec::Point avg{ 0, 0, 0 };
    for (const auto& r : doc.records) { avg.x += r.pos.x; avg.y += r.pos.y; avg.z += r.pos.z; }
    avg.x = 100 + (avg.x / 5 - doc.bounds.anchor.x);
    avg.y = 5 + (avg.y / 5 - doc.bounds.anchor.y);
    avg.z = 200 + (avg.z / 5 - doc.bounds.anchor.z);
    Check("pivot-not-member-average", !Near(avg.x, 100) || !Near(avg.y, 5) || !Near(avg.z, 200), "memberAverage=" + PointStr(avg));
    std::vector<proj_codec::Record> expected;
    Require(ExpectedRecords(doc, { 100, 5, 200 }, 0.0, 1.0, expected), "pivot-expected-transform", "");
    bool poses = true;
    for (size_t i = 0; i < expected.size(); ++i) {
        const SpawnedObj* o = Rec(editor::host_seam::CarriedUid(i));
        poses &= o && PointNear(o->pos, expected[i].pos);
    }
    Check("pivot-off-origin-poses", poses, "");
    // The carried set is the existing Place/Member engine: 5 members, all admitted records, no second engine.
    Check("pivot-carried-count", editor::host_seam::CarriedCount() == 5, "count=" + std::to_string(editor::host_seam::CarriedCount()));
    editor::host_seam::DropCarriedNow();
    PumpAll();
    Check("pivot-drop-one-history-entry", editor::host_seam::UndoSize() == 1, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    CheckNoQueueUnderLock("pivot-no-queue-under-lock");
}

// TILT: yaw + positive uniform scale survive, member pitch/roll are preserved and no group tilt is applied.
static void CaseTilt() {
    BeginCase("tilt");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "tilt-document-valid", error);
    const double yaw = 37.5, factor = 1.6;
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(doc, { -20, 3, 40 }, yaw, factor, uids, pivot);
    Require(rep.valid, "tilt-admission-valid", rep.error);
    PumpAll();
    Snapshot();
    std::vector<proj_codec::Record> expected;
    Require(ExpectedRecords(doc, { -20, 3, 40 }, yaw, factor, expected), "tilt-expected-transform", "");
    bool tilt = true, yawOk = true, scaleOk = true, posOk = true;
    for (size_t i = 0; i < uids.size(); ++i) {
        const SpawnedObj* o = Rec(uids[i]);
        if (!o) { tilt = yawOk = scaleOk = posOk = false; continue; }
        tilt &= Near(o->rot.pitch, doc.records[i].pitch) && Near(o->rot.roll, doc.records[i].roll);
        yawOk &= Near(o->rot.yaw, doc.records[i].yaw + yaw);
        scaleOk &= Near(o->scale, doc.records[i].scale * factor);
        posOk &= PointNear(o->pos, expected[i].pos);
    }
    Check("tilt-member-pitch-roll-preserved", tilt, "");
    Check("tilt-yaw-delta-applied", yawOk, "");
    Check("tilt-uniform-scale-applied", scaleOk, "");
    Check("tilt-group-tilt-disabled-poses", posOk, "expected " + PointStr(expected[0].pos));
    Trace("\"tilt\"", "\"yaw\":" + std::to_string(yaw) + ",\"factor\":" + std::to_string(factor));
    CheckNoQueueUnderLock("tilt-no-queue-under-lock");
}

// CAMERA-GRAB: an admitted copy is carried by the EXISTING camera-aware grab; Home/camera remains usable and
// the drop produces exactly one History entry with the admitted values.
static void CaseCameraGrab() {
    BeginCase("camera-grab");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "camera-grab-document-valid", error);
    editor::host_seam::BeginCameraMode();
    Check("camera-grab-camera-mode-live", editor::host_seam::CameraModeActive(), "");
    Require(editor::host_seam::PlaceGroupCopy(doc, { 30, 2, 30 }, 0.0f, 1.0f, "camera"), "camera-grab-started", "");
    Check("camera-grab-keeps-camera-mode", editor::host_seam::CameraModeActive(), "");
    Check("camera-grab-placing-flag", editor::host_seam::Placing() && editor::MouseMode(), "");
    Check("camera-grab-existing-engine", editor::host_seam::CarriedCount() == 5 && editor::host_seam::SavedPivot(), "");
    PumpAll();
    {
        const core::PlaceRequestView v = View(editor::host_seam::CarriedRequest());
        Check("camera-grab-drop-rows-attached", v.attached == 5 && v.pending == 0 && v.settled, VStr(v));
    }
    editor::host_seam::DropCarriedNow();
    PumpAll();
    {
        const core::PlaceRequestView v = View(editor::host_seam::CarriedRequest());
        Check("camera-grab-drop-used-terminal-rows", v.attached == 5 && v.pending == 0, VStr(v));
    }
    Check("camera-grab-one-history-entry", editor::host_seam::UndoSize() == 1 && editor::host_seam::RedoSize() == 0, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    editor::host_seam::ActView act;
    Check("camera-grab-history-is-spawn", editor::host_seam::HistoryView(false, 0, 0, &act) && act.kind == 0 /*Act::Spawn*/, "kind=" + std::to_string(act.kind));
    Check("camera-grab-camera-still-usable", editor::host_seam::CameraModeActive(), "");
    Check("camera-grab-placing-cleared", !editor::host_seam::Placing(), "");
    // ordinary single-object grab is unchanged: no saved pivot, the receiver's bbox center is used
    editor::host_seam::CancelCarriedNow(); editor::host_seam::ResetPlacement();
    const int uid = core::SpawnAt("/object/group_a.prefab", { 200, 10, 200 }, Rot{ 0, 0, 0 }, 1.0f);
    Require(uid != 0, "camera-grab-single-spawn", "");
    PumpAll();
    editor::host_seam::GrabUids({ uid }, true, "single");
    Check("camera-grab-single-ordinary", editor::host_seam::Placing() && !editor::host_seam::SavedPivot() && editor::host_seam::CarriedCount() == 1, "");
    editor::host_seam::CancelCarriedNow();
    CheckNoQueueUnderLock("camera-grab-no-queue-under-lock");
}

// PERSIST-UNDO: drop -> one History entry -> undo/redo -> save/reload/re-export keep partitions, envelopes, pivot.
static void CasePersistUndo() {
    BeginCase("persist-undo");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "persist-document-valid", error);
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(doc, { 100, 5, 200 }, 0.0, 1.0, uids, pivot);
    Require(rep.valid, "persist-admission-valid", rep.error);
    PumpAll();
    editor::host_seam::GrabUids(uids, true, "persist");
    Check("persist-carried", editor::host_seam::Placing() && editor::host_seam::CarriedCount() == 5, "");
    Check("persist-saved-pivot-not-used", !editor::host_seam::SavedPivot(), "");   // plain GrabUids path keeps the old center rule
    editor::host_seam::DropCarriedNow();
    PumpAll();
    Require(editor::host_seam::UndoSize() == 1, "persist-one-history-entry", "undo=" + std::to_string(editor::host_seam::UndoSize()));
    Snapshot();
    std::vector<int> groups;
    for (int uid : uids) groups.push_back(Rec(uid) ? Rec(uid)->group : -1);
    const int gA = groups[0], gB = groups[1];
    Check("persist-two-partitions", gA > 0 && gB > 0 && gA != gB && groups[3] == 0, GroupMap(uids, doc.records));
    Require(editor::host_seam::UndoOne(), "persist-undo", "");
    PumpAll();
    Snapshot();
    bool hidden = true;
    for (int uid : uids) hidden &= Rec(uid) && Rec(uid)->hidden;
    Check("persist-undo-hides", hidden, "");
    Require(editor::host_seam::RedoOne(), "persist-redo", "");
    PumpAll();
    Snapshot();
    bool restored = true;
    for (size_t i = 0; i < uids.size(); ++i) {
        const SpawnedObj* o = Rec(uids[i]);
        restored &= o && !o->hidden && o->group == groups[i];
    }
    Check("persist-redo-restores-identity-and-groups", restored, "");
    Require(core::SaveProject("t11-persist"), "persist-save", core::ProjectError());
    proj_codec::Document saved; std::string err2;
    Require(ParseDocumentFile(ProjPath("t11-persist"), saved, err2), "persist-readback", err2);
    std::set<int> fileGroups; int zeros = 0;
    for (const auto& r : saved.records) { if (r.group) fileGroups.insert(r.group); else ++zeros; }
    Check("persist-file-partitions", fileGroups.size() == 2 && zeros == 1 && saved.envelopes.size() == 2, "groups=" + std::to_string(fileGroups.size()));
    const proj_codec::Point delta{ 100 - doc.bounds.anchor.x, 5 - doc.bounds.anchor.y, 200 - doc.bounds.anchor.z };
    const proj_codec::Bounds& env1 = saved.envelopes[0].bounds;
    Check("persist-envelope-translated-saved-bounds",
          Near(env1.anchor.x, doc.bounds.anchor.x + delta.x) && Near(env1.anchor.y, doc.bounds.anchor.y + delta.y) && Near(env1.anchor.z, doc.bounds.anchor.z + delta.z),
          PointStr(env1.anchor));
    // reload (real save -> real parse -> real scene) and re-export
    core::DeleteAllSpawned();
    PumpAll();
    Require(core::LoadProject("t11-persist", false), "persist-reload", core::ProjectError());
    PumpAll();
    Snapshot();
    Check("persist-reload-count", VisibleCount() == 5, "visible=" + std::to_string(VisibleCount()));
    std::set<int> reloadGroups; int reloadZeros = 0;
    for (const SpawnedObj& o : g_list) { if (o.hidden) continue; if (o.group) reloadGroups.insert(o.group); else ++reloadZeros; }
    Check("persist-reload-partitions", reloadGroups.size() == 2 && reloadZeros == 1, "groups=" + std::to_string(reloadGroups.size()));
    Require(core::SaveProject("t11-persist-reexport"), "persist-reexport", core::ProjectError());
    proj_codec::Document again; std::string err3;
    Require(ParseDocumentFile(ProjPath("t11-persist-reexport"), again, err3), "persist-reexport-readback", err3);
    std::set<int> againGroups; int againZeros = 0;
    for (const auto& r : again.records) { if (r.group) againGroups.insert(r.group); else ++againZeros; }
    Check("persist-reexport-partitions", againGroups.size() == 2 && againZeros == 1 && again.envelopes.size() == 2, "groups=" + std::to_string(againGroups.size()));
    const proj_codec::Bounds& env2 = again.envelopes[0].bounds;
    Check("persist-reexport-envelope-preserved",
          Near(env2.anchor.x, env1.anchor.x) && Near(env2.anchor.y, env1.anchor.y) && Near(env2.anchor.z, env1.anchor.z) &&
          Near(env2.min.x, env1.min.x) && Near(env2.max.z, env1.max.z), PointStr(env2.anchor));
    Trace("\"persist-undo\"", "\"fileGroups\":" + std::to_string(fileGroups.size()) + ",\"envelopes\":" + std::to_string(saved.envelopes.size()));
    CheckNoQueueUnderLock("persist-no-queue-under-lock");
}

// SINGLE-SURVIVOR-DIFFERENT-BOUNDS: one imported member keeps the saved pivot/pose; the receiver's different
// box measurement never recenters it and never overrides the saved envelope.
static void CaseSingleSurvivor() {
    BeginCase("single-survivor");
    ResetWorld();
    // receiver box hint differs from the saved bounds on purpose
    InstallIndex(false);
    const auto weird = PrefabRow("/object/group_a.prefab", 10.0f, 10.0f, 10.0f, 5.0f, 5.0f, 5.0f);
    host::SetPrefabIndex(weird);
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(SingleRecipeText(), doc, error), "single-document-valid", error);
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(doc, { 300, 12, -40 }, 0.0, 1.0, uids, pivot);
    Require(rep.valid && rep.admitted == 1, "single-admission-valid", rep.error);
    PumpAll();
    Snapshot();
    std::vector<proj_codec::Record> expected;
    Require(ExpectedRecords(doc, { 300, 12, -40 }, 0.0, 1.0, expected), "single-expected-transform", "");
    const SpawnedObj* o = uids.empty() ? nullptr : Rec(uids[0]);
    Check("single-pivot-is-saved-anchor", Near(pivot.x, 300) && Near(pivot.y, 12) && Near(pivot.z, -40), VecStr(pivot));
    Check("single-keeps-saved-pose-not-receiver", o && PointNear(o->pos, expected[0].pos), o ? VecStr(o->pos) : "missing");
    // Not recentered onto the receiver box: the receiver-derived AABB center would be ~(pos + (5,5,5)).
    const bool receiverCentered = o && Near(o->pos.x, expected[0].pos.x + 5.0) && Near(o->pos.z, expected[0].pos.z + 5.0);
    Check("single-not-receiver-centered", !receiverCentered, o ? VecStr(o->pos) : "missing");
    Require(core::SaveProject("t11-single"), "single-save", core::ProjectError());
    proj_codec::Document saved; std::string err2;
    Require(ParseDocumentFile(ProjPath("t11-single"), saved, err2), "single-readback", err2);
    Require(saved.envelopes.size() == 1 && saved.records.size() == 1, "single-saved-shape", "envelopes=" + std::to_string(saved.envelopes.size()));
    const proj_codec::Point delta{ 300 - doc.bounds.anchor.x, 12 - doc.bounds.anchor.y, -40 - doc.bounds.anchor.z };
    const proj_codec::Bounds& e = saved.envelopes[0].bounds;
    Check("single-saved-envelope-translated-bounds",
          Near(e.anchor.x, doc.bounds.anchor.x + delta.x) && Near(e.min.x, doc.bounds.min.x + delta.x) && Near(e.min.y, doc.bounds.min.y + delta.y) &&
          Near(e.max.x, doc.bounds.max.x + delta.x) && Near(e.max.y, doc.bounds.max.y + delta.y) && Near(e.max.z, doc.bounds.max.z + delta.z),
          PointStr(e.anchor));
    Check("single-envelope-not-receiver-box", !Near(e.max.x - e.min.x, 10.0), "savedWidth=" + std::to_string(e.max.x - e.min.x));
    Trace("\"single-survivor\"", "\"pivot\":" + VecStr(pivot) + ",\"envelopeAnchor\":" + PointStr(e.anchor));
    CheckNoQueueUnderLock("single-no-queue-under-lock");
}

// =========================================================================================================
// adversarial cases
// =========================================================================================================
// MISSING-PREFAB: a valid row whose prefab the receiver does not know is a NAMED preflight exclusion; the rest
// still place, and the row receipt reports Excluded with that reason.
static void CaseMissingPrefab() {
    BeginCase("missing-prefab");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "missing-document-valid", error);
    doc.records[3].prefab = "/object/absent.prefab";
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(doc, { 70, 4, 70 }, 0.0, 1.0, uids, pivot);
    Require(rep.valid, "missing-admission-valid", rep.error);
    PumpAll();
    Snapshot();
    Check("missing-named-exclusion", rep.excluded == 1 && rep.admitted == 4 && rep.excludedPrefabs.size() == 1 && rep.excludedPrefabs[0] == "/object/absent.prefab",
          "admitted=" + std::to_string(rep.admitted) + " excluded=" + std::to_string(rep.excluded));
    const core::PlaceRequestView v = View(rep.request);
    const core::PlaceRow* excluded = Row(v, 3);
    Check("missing-row-excluded-by-name", excluded && excluded->state == core::PlaceExcluded && excluded->reason == "missing prefab", excluded ? RStr(*excluded) : "missing");
    Check("missing-others-attached", v.attached == 4 && v.pending == 0, VStr(v));
    bool absentRegistered = false;
    for (const SpawnedObj& o : g_list) if (o.prefab == "/object/absent.prefab") absentRegistered = true;
    Check("missing-never-spawned", !absentRegistered && VisibleCount() == 4, "visible=" + std::to_string(VisibleCount()));
    CheckEquation("missing-equation", v);
    Trace("\"missing-prefab\"", "\"excluded\":\"" + rep.excludedPrefabs[0] + "\"");
    CheckNoQueueUnderLock("missing-no-queue-under-lock");
}

// CANCEL-LATE: cancel after the rows genuinely attached reports the ACTUAL terminal rows (never the queue
// depth), removes the attached members, and adds no History entry.
static void CaseCancelLate() {
    BeginCase("cancel-late");
    ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(RecipeText(), doc, error), "cancel-document-valid", error);
    editor::host_seam::BeginCameraMode();
    Require(editor::host_seam::PlaceGroupCopy(doc, { 60, 6, 60 }, 0.0f, 1.0f, "cancel"), "cancel-grab-started", "");
    PumpAll();
    const core::PlaceRequestHandle req = editor::host_seam::CarriedRequest();
    Require(req != nullptr, "cancel-request-owned", "");
    {
        const core::PlaceRequestView v = View(req);
        Check("cancel-attached-before-cancel", v.attached == 5 && v.pending == 0, VStr(v));
        // the receipt reports the real terminal attachments while the queue is empty: it is never the queue depth
        Check("cancel-receipt-not-queue-depth", core::PendingSpawns() == 0 && v.attached == 5, "pendingSpawns=" + std::to_string(core::PendingSpawns()) + ", attached=" + std::to_string(v.attached));
    }
    const int liveBefore = oracle::LiveCount();
    editor::host_seam::CancelCarriedNow();
    {
        const core::PlaceRequestView v = View(req);
        Check("cancel-keeps-attached-receipts", v.attached == 5 && v.canceled == 0 && v.pending == 0, VStr(v));
        bool flagged = true;
        for (const auto& r : v.rows) flagged &= r.removedAfterAttach;
        Check("cancel-flags-removed-after-attach", flagged, VStr(v));
        Check("cancel-cleanup-pending-until-dispatch", v.cleanupPending && !v.settled, VStr(v));
        Check("cancel-history-unchanged", editor::host_seam::UndoSize() == 0 && editor::host_seam::RedoSize() == 0, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    }
    PumpAll();
    PumpAll();
    {
        const core::PlaceRequestView v = View(req);
        Check("cancel-cleanup-settled", v.settled && !v.cleanupPending && v.attached == 5, VStr(v));
        Check("cancel-engine-members-removed", oracle::LiveCount() == 0 && liveBefore == 5, "live=" + std::to_string(oracle::LiveCount()));
        Snapshot();
        Check("cancel-records-hidden-forgotten", VisibleCount() == 0, "visible=" + std::to_string(VisibleCount()));
        CheckEquation("cancel-equation", v);
    }
    Trace("\"cancel-late\"", "\"liveBefore\":" + std::to_string(liveBefore));
    CheckNoQueueUnderLock("cancel-no-queue-under-lock");
}

// INVALID-ADMISSION: each rejected document mutates nothing (scene, History, selection, queues, allocations).
static void CaseInvalidAdmission() {
    BeginCase("invalid-admission");
    ResetWorld();
    proj_codec::Document good; std::string error;
    Require(ParseGroupText(RecipeText(), good, error), "invalid-base-document", error);
    struct Sample { const char* id; proj_codec::Document doc; Vec3 target; double yaw, factor; };
    std::vector<Sample> samples;
    { proj_codec::Document d = good; d.records[2].pos.x = std::numeric_limits<double>::quiet_NaN(); samples.push_back({ "NaN position", d, { 1, 1, 1 }, 0, 1 }); }
    { proj_codec::Document d = good; samples.push_back({ "nonpositive scale", d, { 1, 1, 1 }, 0, 0 }); }
    { proj_codec::Document d = good; d.hasBounds = false; d.envelopes.clear(); samples.push_back({ "missing bounds", d, { 1, 1, 1 }, 0, 1 }); }
    { proj_codec::Document d = good; d.records.clear(); samples.push_back({ "no records", d, { 1, 1, 1 }, 0, 1 }); }
    { proj_codec::Document d = good; samples.push_back({ "NaN target", d, { std::numeric_limits<float>::quiet_NaN(), 0, 0 }, 0, 1 }); }
    for (const Sample& s : samples) {
        const WorldState before = CaptureWorld();
        std::vector<int> uids; Vec3 pivot{};
        const core::GroupAdmissionReport rep = core::AdmitGroupCopy(s.doc, s.target, s.yaw, s.factor, uids, pivot);
        Check((std::string("invalid-rejected-") + s.id).c_str(), !rep.valid && !rep.error.empty() && uids.empty(), "error=" + rep.error);
        CheckWorldUnchanged((std::string("invalid-no-mutation-") + s.id).c_str(), before);
        Check((std::string("invalid-not-placing-") + s.id).c_str(), !editor::host_seam::Placing(), "");
    }
    CheckNoQueueUnderLock("invalid-no-queue-under-lock");
}

// A completed/canceled receipt must outlive the carried slot and its removed records.
static void CaseReceiptRetention() {
    BeginCase("receipt-retention"); ResetWorld();
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(SingleRecipeText(), doc, error), "retention-document", error);
    Require(editor::PlaceGroupCopy(doc, {10, 1, 20}, 0, 1, "same-name"), "retention-place");
    std::weak_ptr<core::PlaceRequest> first = editor::g_place.req;
    editor::CancelCarried(); PumpAll(); editor::host_seam::ResetPlacement();
    Check("retention-canceled-receipt-outlives-place-and-record", !first.expired());
    if (auto receipt = first.lock()) {
        const auto v = View(receipt);
        Check("retention-still-own-canceled-row", v.canceled == 1 && v.settled && !v.cleanupPending);
    }
}

// The upstream NPC packet builder and server queue run unchanged. Only the game's execute
// function/address and captured player session are fixture memory (no game/installed data).
static int npcCalls = 0;
static std::vector<uint32_t> npcKeys;
static std::vector<Vec3> npcPositions;
static void* __fastcall NpcExecute(void*, int* result, void* packet) {
    uint8_t* bytes = nullptr; memcpy(&bytes, (uint8_t*)packet + 0x18, sizeof bytes);
    uint32_t key = 0; memcpy(&key, bytes + 5, sizeof key); npcKeys.push_back(key); ++npcCalls;
    Vec3 position{}; memcpy(&position, bytes + 13, sizeof position); npcPositions.push_back(position);
    *result = 0; return nullptr;
}
static void CaseNpcBudget() {
    BeginCase("npc-server-budget"); ResetWorld();
    uintptr_t vtable = 0;
    uintptr_t session = (uintptr_t)&vtable; // ReadPtr requires a canonical, non-low pointer value
    core::g_npcExecute = (void*)&NpcExecute; core::g_npcHandler = 1;
    core::g_serverSession = (uintptr_t)&session; core::g_sessionVt = session;
    npcCalls = 0; npcKeys.clear(); const auto before = CaptureWorld();
    for (uint32_t i = 0; i < 17; ++i) Require(core::SpawnNpc(1000 + i, {1, 2, 3}), "npc-admitted");
    Check("npc-not-executed-on-ui-thread", npcCalls == 0);
    host::PumpServer(); Check("npc-first-tick-eight", npcCalls == 8);
    host::PumpServer(); Check("npc-second-tick-eight", npcCalls == 16);
    host::PumpServer(); Check("npc-third-tick-one", npcCalls == 17);
    bool ordered = npcKeys.size() == 17;
    for (size_t i = 0; i < npcKeys.size(); ++i) ordered &= npcKeys[i] == 1000 + i;
    Check("npc-packet-order", ordered); CheckWorldUnchanged("npc-not-scene-or-history", before);
    core::g_npcExecute = nullptr; core::g_npcHandler = 0; core::g_serverSession = core::g_sessionVt = 0;
}

static void CaseInteractive() {
    BeginCase("interactive-server-lane"); ResetWorld();
    const std::string prefab = "/object/cd_gimmick/campfire.prefab";
    host::SetPrefabIndex(PrefabRow(prefab, 2, 2, 2, 0, 1, 0)); host::SetDirectGimmick(true);
    int directs = 0;
    host::Seam().directGimmick = [&](const std::string& p, Vec3 at, Rot rot, float sc, uintptr_t* so, uintptr_t* actor) {
        ++directs; *so = oracle::MakeObj(p, at, rot, sc); *actor = 0x7788;
        oracle::objects.at(*so).actor = *actor; return true;
    };
    host::Seam().removeActor = [](uintptr_t a) {
        for (auto& entry : oracle::objects) if (entry.second.actor == a && entry.second.live) {
            entry.second.live = false; oracle::disposed.insert(entry.first); return true;
        }
        return false;
    };
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(SingleRecipeText(), doc, error), "interactive-document", error); doc.records[0].prefab = prefab;
    Require(editor::PlaceGroupCopy(doc, {10, 1, 20}, 0, 1, "campfire"), "interactive-place");
    const auto req = editor::g_place.req;
    PumpGame_(); Check("interactive-not-generic", oracle::creates == 0 && directs == 0 && View(req).pending == 1);
    host::PumpServer(); auto v = View(req);
    Check("interactive-game-builder-lane", directs == 1 && v.attached == 1 && v.rows[0].lane == core::PlaceLaneDirect);
    editor::CancelCarried(); v = View(req);
    Check("interactive-cancel-waits-both-lanes", v.cleanupPending && !v.settled && v.rows[0].removedAfterAttach);
    PumpAll(); v = View(req);
    Check("interactive-cleaned-not-survivor", v.settled && v.attached == 1 && oracle::LiveCount() == 0);
    InstallSeam(); host::SetDirectGimmick(false);
}

// Deterministic native-boundary interleave: a Drop while the server builder owns its in-flight
// obligation must still record the logical spawn. cleanupPending alone is not a canceled row.
static void CaseDropDuringAttach() {
    BeginCase("drop-during-interactive-attach"); ResetWorld();
    const std::string prefab = "/object/cd_gimmick/bed.prefab";
    host::SetPrefabIndex(PrefabRow(prefab, 2, 2, 2, 0, 1, 0)); host::SetDirectGimmick(true);
    int directs = 0;
    host::Seam().directGimmick = [&](const std::string& p, Vec3 at, Rot rot, float sc, uintptr_t* so, uintptr_t* actor) {
        ++directs; *so = oracle::MakeObj(p, at, rot, sc); *actor = *so + 0x10000;
        oracle::objects.at(*so).actor = *actor;
        if (directs == 1) {
            const auto v = View(editor::g_place.req);
            Check("drop-flight-is-pending-not-canceled", v.pending == 1 && v.cleanupPending && !v.requestCanceled);
            editor::DropCarried();
        }
        return true;
    };
    host::Seam().removeActor = [](uintptr_t a) {
        for (auto& entry : oracle::objects) if (entry.second.actor == a && entry.second.live) {
            entry.second.live = false; oracle::disposed.insert(entry.first); return true;
        }
        return false;
    };
    proj_codec::Document doc; std::string error;
    Require(ParseGroupText(SingleRecipeText(), doc, error), "drop-flight-document", error); doc.records[0].prefab = prefab;
    Require(editor::PlaceGroupCopy(doc, {10, 1, 20}, 0, 1, "bed"), "drop-flight-place");
    const auto request = editor::g_place.req; const int uid = editor::g_place.m[0].uid;
    host::PumpServer(); PumpAll(); PumpAll();
    const auto v = View(request);
    Check("drop-flight-one-history-entry", editor::g_undo.size() == 1 && editor::g_undo.back().acts.size() == 1 && editor::g_undo.back().acts[0].uid == uid);
    Check("drop-flight-final-one-live", !editor::Placing() && v.attached == 1 && v.settled && oracle::LiveCount() == 1, VStr(v));
    editor::Undo(); PumpAll();
    Check("drop-flight-undo-removes-survivor", oracle::LiveCount() == 0);
    InstallSeam(); host::SetDirectGimmick(false);
}

#include "raw_drop_cases.h"

// =========================================================================================================
// main
// =========================================================================================================
int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : ".";
    host::OpenLog(g_fixtureDir + "\\group_placement.log");
    core::Log("[group_placement] run start: fixtureDir=%s", g_fixtureDir.c_str());
    ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    g_probe.Start();
    InstallSeam();
    host::SetModDir(g_fixtureDir);       // real files, inside the fixture directory only
    core::g_recreateOnMove = true;
    host::SetDirectGimmick(false);
    InstallIndex(true);
    CreateDirectoryA(ProjectDir().c_str(), nullptr);

    struct Entry { const char* id; void (*fn)(); };
    const Entry entries[] = {
        { "partitions", CasePartitions },
        { "two-copies", CaseTwoCopies },
        { "pivot", CasePivot },
        { "tilt", CaseTilt },
        { "camera-grab", CaseCameraGrab },
        { "persist-undo", CasePersistUndo },
        { "single-survivor", CaseSingleSurvivor },
        { "missing-prefab", CaseMissingPrefab },
        { "cancel-late", CaseCancelLate },
        { "invalid-admission", CaseInvalidAdmission },
        { "receipt-retention", CaseReceiptRetention },
        { "npc-server-budget", CaseNpcBudget },
        { "interactive-server-lane", CaseInteractive },
        { "drop-during-interactive-attach", CaseDropDuringAttach },
        { "main-raw-drop-invalidation", CaseRawDropInvalidation },
    };
    for (const Entry& e : entries) {
        oracle::Reset();
        host::SetDirectGimmick(false);
        InstallIndex(true);
        try {
            e.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("CASE-ABORT %s: %s\n", e.id, ex.what());
            core::Log("[group_placement] CASE-ABORT %s: %s", e.id, ex.what());
            if (!g_cases.empty() && g_cases.back().failures == 0) ++g_cases.back().failures;
        }
    }

    Check("engine-lock-free-throughout", oracle::lockFreeDuringEngineCalls, "lockFree=" + std::to_string(oracle::lockFreeDuringEngineCalls));
    Check("no-queue-push-under-registry-lock", host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations()));

    {
        std::string json = "{\"suite\":\"GroupPlacement\",\"status\":\"" + std::string(g_failures == 0 ? "PASS" : "FAIL") +
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
        core::Log("[group_placement] cases: %s", json.c_str());
    }
    {
        FILE* f = std::fopen((g_fixtureDir + "\\group_placement_trace.json").c_str(), "wb");
        if (f) { std::fwrite(g_trace.data(), 1, g_trace.size(), f); std::fclose(f); }
    }
    std::printf("CASES=%zu\nASSERTIONS=%d\nFAILURES=%d\n", g_cases.size(), g_assertions, g_failures);
    core::Log("[group_placement] run end: cases=%zu assertions=%d failures=%d lockViolations=%d", g_cases.size(), g_assertions, g_failures, host::LockViolations());
    editor::CancelCarried(); PumpAll(); core::DeleteAllSpawned(); PumpAll();
    g_probe.Stop(); ImGui::DestroyContext();
    return g_failures == 0 ? 0 : 1;
}
