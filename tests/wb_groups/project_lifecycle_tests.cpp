// ProjectLifecycle host fixture (plan wb079-unified-77f68967, Task 9 / C4+C6).
//
// The production core TU is compiled by production_core_host.cpp (WB_UNIFIED_HOST_TEST) and the ACTUAL editor
// TU is included here once, so LoadProject/SaveProject/DeleteAllSpawned, the registry, the queues, the project
// membership, the dirty stars and the editor History are production code. The codec (proj_codec.cpp) and the
// group math (wb_group_math.cpp) are the shipped TUs, linked as sources; the files the suite loads and saves are
// real files under the fixture directory (host::SetModDir), and the only substituted boundaries are the engine
// calls behind host::Seam() plus the one-shot file-write fault seam (host::SetSaveFault/SetBeforeReplace).
//
// Cases assert the C4/C6 contract:
//   ROUNDTRIP              - record-owned partition/envelope/pivot metadata survives load -> save -> reload -> re-export
//   ADOPTION               - the full SaveProject adoption chain survives undo/redo, and a newer adoption made
//                            while the file is being written is never overwritten by the captured snapshot
//   SAVE-SCOPES            - the four SaveScope modes, the scoped dirty stars and untouched other-project files
//   LEGACY                 - legacy rows/defaults stay valid, and a valid missing-prefab row is a NAMED exclusion
//   REPLACE-LAST-ROW       - a malformed final row rejects the document; scene/History/selection/queues/allocations/file unchanged
//   NARROWING              - float overflow, tile overflow and float-scale underflow are rejected before any mutation
//   WRITE-FAIL             - a failed transactional write preserves the old bytes, membership, dirty stars and world
//   GROUP-AUTOLOAD         - .cdgroup is never Load/autoload; autoload order is file order; a manual load suppresses it
//   POST-ADMISSION-PARTIAL - an engine refusal after admission is an honest partial, not a rollback and not a
//                            mislabeled missing-prefab exclusion
//
// Machine output: CHECK lines on stdout, one ASSERTIONS=<n> line, a per-case cases.json receipt and a lifecycle
// trace through the production log sink (host::OpenLog) at <fixtureDir>\project_lifecycle.log.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <filesystem>

#include "../../asi/cdmodkit/editor.cpp"   // the actual editor TU (History, project table action, Undo/Redo)
#include "../../asi/cdmodkit/proj_codec.h" // the shipped disk-value codec (linked as its own TU)
#include "production_host.h"               // host::Seam / host::PumpGame / host::PumpServer / file + autoload seams

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace oracle {
struct Obj { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1; uintptr_t actor = 0; bool live = true; };

std::map<uintptr_t, Obj> objects;
std::vector<uintptr_t> createdOrder;
std::set<uintptr_t> disposed;
std::set<uintptr_t> released;
uintptr_t next = 0x10000;
int creates = 0, removes = 0, createCalls = 0;
int refuseCreateCall = -1;             // 1-based call ordinal refused by the engine, -1 = never
bool lockFreeDuringEngineCalls = true;

void Reset() {
    objects.clear(); createdOrder.clear(); disposed.clear(); released.clear();
    next = 0x10000; creates = removes = createCalls = 0; refuseCreateCall = -1;
    lockFreeDuringEngineCalls = true;
}
bool Live(uintptr_t h) { auto it = objects.find(h); return it != objects.end() && it->second.live; }
bool DisposedHandle(uintptr_t h) { return disposed.count(h) != 0; }
std::string Describe(uintptr_t h) {
    auto it = objects.find(h);
    char b[192];
    if (it == objects.end()) { std::snprintf(b, sizeof b, "handle=0x%llx unknown", (unsigned long long)h); return b; }
    std::snprintf(b, sizeof b, "handle=0x%llx live=%d pos=(%.3f %.3f %.3f)", (unsigned long long)h,
                  it->second.live ? 1 : 0, it->second.pos.x, it->second.pos.y, it->second.pos.z);
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
struct CaseRec { std::string id; int assertions = 0, failures = 0; };
std::vector<CaseRec> g_cases;

void Check(const char* label, bool ok, const std::string& observed = "") {
    ++g_assertions;
    if (!g_cases.empty()) { ++g_cases.back().assertions; if (!ok) ++g_cases.back().failures; }
    if (!ok) ++g_failures;
    std::printf("CHECK %s %s%s%s\n", ok ? "PASS" : "FAIL", label, observed.empty() ? "" : " | ", observed.c_str());
    core::Log("[project_lifecycle] %s %s %s%s%s", g_case.c_str(), ok ? "PASS" : "FAIL", label,
              observed.empty() ? "" : " | ", observed.c_str());
}
void Require(bool ok, const char* label, const std::string& observed = "") {
    Check(label, ok, observed);
    if (!ok) throw std::runtime_error(std::string(label) + (observed.empty() ? "" : (" | " + observed)));
}

// ---- engine seam -----------------------------------------------------------------------------------------
void InstallSeam() {
    host::Engine& e = host::Seam();
    e.ready = true;
    e.createGeneric = [](const std::string& prefab, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::createCalls;
        if (oracle::createCalls == oracle::refuseCreateCall) return 0;   // deterministic engine refusal
        ++oracle::creates;
        const uintptr_t h = oracle::next++;
        oracle::objects.emplace(h, oracle::Obj{prefab, pos, rot, scale, 0, true});
        oracle::createdOrder.push_back(h);
        return h;
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
        auto it = oracle::objects.find(h);
        if (it == oracle::objects.end() || !it->second.live) return false;
        it->second.pos = pos; it->second.rot = rot; it->second.scale = scale;
        return true;
    };
    e.liveMove = [](uintptr_t h, Vec3 pos, Rot rot, float scale) -> bool {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
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
void PumpAll() {
    PumpGame_();
    host::PumpServer();
    PumpGame_();
}

// ---- real-file helpers ------------------------------------------------------------------------------------
// The fixture owns the mod directory (host::SetModDir): projects live under <fixtureDir>\projects, autoload.txt
// directly in <fixtureDir>. Every file this suite touches is one it created.
std::string ProjectDir() { return g_fixtureDir + "\\projects"; }
std::string ProjPath(const std::string& name) { return ProjectDir() + "\\" + name + ".cdproj"; }
std::string GroupPath(const std::string& name) { return ProjectDir() + "\\" + name + ".cdgroup"; }
std::string AutoloadPath() { return g_fixtureDir + "\\autoload.txt"; }
std::string OutsidePath(const std::string& name) { return g_fixtureDir + "\\" + name; }

bool WriteTextFile(const std::string& path, const std::string& text) {
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
    if (!f) return false;
    const bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}
bool ReadFileText(const std::string& path, std::string& out) {
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
    if (!f) return false;
    char b[4096]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}
bool FileExists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Sha256Bytes(const std::string& data) {
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        DWORD cb = 0, cbHash = 0;
        if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&cbHash), sizeof cbHash, &cb, 0) == 0 &&
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0) {
            std::vector<unsigned char> digest(cbHash);
            if (BCryptFinishHash(hash, digest.data(), cbHash, 0) == 0) {
                char b[3];
                for (unsigned char c : digest) { std::snprintf(b, sizeof b, "%02x", c); out += b; }
            }
        }
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}
std::string Sha256File(const std::string& path) {
    std::string bytes; if (!ReadFileText(path, bytes)) return std::string("<unreadable:").append(path).append(">");
    return Sha256Bytes(bytes);
}
std::string FileText(const std::string& path) { std::string b; ReadFileText(path, b); return b; }

int TempLeftovers() {
    int n = 0; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((ProjectDir() + "\\wbv*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { ++n; } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
void CleanFixtureDir() {
    // Only these task-created fixture paths are owned; never recurse over the evidence or mod root.
    std::filesystem::remove_all(AutoloadPath());
    std::filesystem::remove_all(ProjectDir());
    std::filesystem::remove_all(g_fixtureDir + "\\Groups");
    std::filesystem::create_directory(ProjectDir());
    // import sources written outside the projects folder
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((g_fixtureDir + "\\t9-import-*.cdproj").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { DeleteFileA((g_fixtureDir + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

// ---- document text builders (explicit grammar text; the codec is the reader under test) -------------------
std::string Row(const std::string& prefab, double x, double y, double z, double yaw = 0, double scale = 1, int group = 0, double pitch = 0, double roll = 0) {
    char b[512];
    std::snprintf(b, sizeof b, "%s|%.17g|%.17g|%.17g|%.17g|%.17g|%d|%.17g|%.17g\n", prefab.c_str(), x, y, z, yaw, scale, group, pitch, roll);
    return b;
}
std::string BoundsTail(const std::string& anchor, const std::string& min, const std::string& max, const char* quality) {
    return "anchor=" + anchor + " min=" + min + " max=" + max + " quality=" + quality + "\n";
}
// one modern project document with a single envelope and N rows
std::string OneEnvelopeDoc(const std::vector<std::string>& rows) {
    std::string s = "# cdproj v3 kind=project\n# wb-document " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "approx") +
                    "# wb-envelope id=1 " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "approx");
    for (size_t i = 0; i < rows.size(); ++i) s += "# wb-member record=" + std::to_string(i + 1) + " envelope=1\n" + rows[i];
    return s;
}

// ---- registry snapshot helpers ---------------------------------------------------------------------------
void Snapshot() { if (g_list.capacity() < 256) g_list.reserve(256); g_list = core::Spawned(); }
const SpawnedObj* Rec(int uid) { for (auto& o : g_list) if (o.uid == uid) return &o; return nullptr; }
int VisibleCount() { int n = 0; for (const SpawnedObj& o : g_list) if (!o.hidden) ++n; return n; }
std::string UidGen(int uid) {
    // Owned local snapshot: must NOT touch g_list. Callers pass borrowed pointers into g_list
    // (e.g. hiddenRow, r1..r5) in the same Check() call, and C++17 argument evaluation order is
    // indeterminately sequenced, so a Snapshot() that reassigns g_list here could invalidate the
    // sibling argument's pointer before it is read (use-after-free / UB under ASan).
    const std::vector<SpawnedObj> snap = core::Spawned();
    const SpawnedObj* o = nullptr;
    for (const auto& s : snap) if (s.uid == uid) { o = &s; break; }
    char b[320];
    if (!o) { std::snprintf(b, sizeof b, "uid=%d missing nextUid=%d", uid, host::NextUid()); return b; }
    std::snprintf(b, sizeof b, "uid=%d gen=%llu obj=0x%llx hidden=%d proj=%d group=%d nextUid=%d",
                  uid, (unsigned long long)o->gen, (unsigned long long)o->obj, o->hidden ? 1 : 0, o->proj, o->group, host::NextUid());
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
    w.undo = static_cast<int>(editor::host_seam::UndoSize()); w.redo = static_cast<int>(editor::host_seam::RedoSize());
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

// ---- project/report helpers ------------------------------------------------------------------------------
std::string ProjError() { return core::ProjectError(); }
core::ProjectLoadReport Rep(const std::string& name) { return core::ProjectLoadReportFor(name); }
std::string RepStr(const core::ProjectLoadReport& r) {
    char b[384];
    std::snprintf(b, sizeof b, "valid=%d requested=%d queued=%d excluded=%d excludedPrefabs=%zu", r.valid ? 1 : 0, r.requested, r.queued, r.excluded, r.excludedPrefabs.size());
    std::string s = b;
    for (const auto& p : r.excludedPrefabs) s += " [" + p + "]";
    return s;
}
bool ParseProjectFile(const std::string& path, proj_codec::Document& doc, std::string& error) {
    std::string bytes;
    if (!ReadFileText(path, bytes)) { error = "cannot read " + path; return false; }
    return proj_codec::Parse(bytes, path, proj_codec::Kind::Project, doc, error);
}
int RowCount(const std::string& path) {
    proj_codec::Document doc; std::string error;
    if (!ParseProjectFile(path, doc, error)) return -1;
    return static_cast<int>(doc.records.size());
}
bool PointEq(const proj_codec::Point& p, double x, double y, double z) {
    return std::fabs(p.x - x) < 1e-9 && std::fabs(p.y - y) < 1e-9 && std::fabs(p.z - z) < 1e-9;
}
std::string PointStr(const proj_codec::Point& p) {
    char b[160];
    std::snprintf(b, sizeof b, "(%.6g %.6g %.6g)", p.x, p.y, p.z);
    return b;
}

// ---- case helpers ----------------------------------------------------------------------------------------
int Spawn(const std::string& prefab, Vec3 at, Rot rot = {}, float scale = 1.0f, int group = 0, int proj = 0) {
    const int uid = editor::host_seam::SpawnRecorded(prefab, at, rot, scale, group, proj);
    Require(uid > 0, "spawn-accepted", "uid=" + std::to_string(uid));
    return uid;
}
int SpawnCore(const std::string& prefab, Vec3 at, Rot rot = {}, float scale = 1.0f, int group = 0, int proj = 0) {
    const int uid = core::SpawnAt(prefab, at, rot, scale, group, proj);
    Require(uid > 0, "spawn-core-accepted", "uid=" + std::to_string(uid));
    return uid;
}
void DeleteViaEditor(int uid) {
    editor::host_seam::SelectUid(uid);
    editor::host_seam::DeleteSelection();
}
bool MoveFinal(int uid, Vec3 pos, Rot rot, float scale) {
    return core::MoveMany(std::vector<core::MoveReq>{ core::MoveReq{ uid, pos, rot, scale } }, true);
}

void ResetWorld() {
    core::g_fileMutationFault = core::FileMutationFault::None;
    editor::host_seam::ResetPlacement();
    host::SetSaveFault(host::SaveFault::None);
    host::SetBeforeReplace({});
    editor::host_seam::ResetHistory();
    core::DeleteAllSpawned();
    PumpGame_();
    host::PumpServer();
    PumpGame_();
    oracle::Reset();
    CleanFixtureDir();
    host::ResetAutoloadDone();
    g_probe.CheckFree();
}
void BeginCase(const char* id) {
    g_case = id;
    ResetWorld();
    g_cases.push_back(CaseRec{});
    g_cases.back().id = id;
    g_lockBaseline = host::LockViolations();
    std::printf("CASE: %s\n", id);
    core::Log("[project_lifecycle] CASE %s", id);
}
void CheckNoQueueUnderLock(const char* label) {
    Check(label, host::LockViolations() == g_lockBaseline,
          "queue pushes under the registry lock: " + std::to_string(host::LockViolations() - g_lockBaseline));
}

// =========================================================================================================
// Cases
// =========================================================================================================

// happy: record-owned partition/envelope/pivot metadata survives load -> save -> reload -> re-export
static void CaseRoundtrip() {
    BeginCase("roundtrip-metadata");
    const std::string doc =
        "# cdproj v3 kind=project\n"
        "# wb-document anchor=-1,0,-1 min=-2,0,-2 max=0,3,0 quality=measured\n"
        "# wb-envelope id=1 anchor=-1,0,-1 min=-2,0,-2 max=0,3,0 quality=measured\n"
        "# wb-envelope id=2 anchor=5,0,4.5 min=4,0,4 max=6,2,5 quality=measured\n"
        "# wb-member record=1 envelope=1\n" + Row("/object/a.prefab", 1, 0, 1, 90, 2, 42) +
        "# wb-member record=2 envelope=1\n" + Row("/object/b.prefab", -1, 2, 1, 0, 1, 42) +
        "# wb-member record=3 envelope=1\n" + Row("/object/a.prefab", -1, 1, -1, 0, 1, 7) +
        "# wb-member record=4 envelope=2\n" + Row("/object/c.prefab", 5, 0, 5, 30, 1.5, 0) +
        "# wb-member record=5 envelope=2\n" + Row("/object/c.prefab", 5, 1, 4, 0, 1, 7);
    Require(WriteTextFile(ProjPath("t9-roundtrip"), doc), "fixture-wrote-roundtrip-document", "");
    const std::string hash0 = Sha256File(ProjPath("t9-roundtrip"));
    Require(editor::host_seam::LoadProjectAction("t9-roundtrip", false), "load-accepted", "error=" + ProjError());
    const core::ProjectLoadReport rep = Rep("t9-roundtrip");
    Check("report-requested", rep.valid && rep.requested == 5, RepStr(rep));
    Check("report-all-queued-no-exclusions", rep.queued == 5 && rep.excluded == 0 && rep.excludedPrefabs.empty(), RepStr(rep));
    Check("queues-changed-only-after-validation", core::PendingSpawns() == 5, "pending=" + std::to_string(core::PendingSpawns()));
    Check("additive-load-keeps-history", editor::host_seam::UndoSize() == 0 && editor::host_seam::RedoSize() == 0, "");
    Check("load-did-not-write-the-file", Sha256File(ProjPath("t9-roundtrip")) == hash0, "");
    PumpAll();
    Snapshot();
    Check("five-visible", VisibleCount() == 5 && g_list.size() == 5, "visible=" + std::to_string(VisibleCount()));
    const SpawnedObj* r1 = g_list.size() > 0 ? &g_list[0] : nullptr;
    const SpawnedObj* r2 = g_list.size() > 1 ? &g_list[1] : nullptr;
    const SpawnedObj* r3 = g_list.size() > 2 ? &g_list[2] : nullptr;
    const SpawnedObj* r4 = g_list.size() > 3 ? &g_list[3] : nullptr;
    const SpawnedObj* r5 = g_list.size() > 4 ? &g_list[4] : nullptr;
    Check("rows-kept-values", r1 && r1->prefab == "/object/a.prefab" && std::fabs(r1->pos.x - 1) < 1e-6f && std::fabs(r1->rot.yaw - 90) < 1e-6f && std::fabs(r1->scale - 2) < 1e-6f, r1 ? UidGen(r1->uid) : "missing");
    Check("partition-shared-per-file-group", r1 && r2 && r3 && r1->group == r2->group && r1->group != r3->group && r1->group > 0 && r3->group > 0, r1 && r2 && r3 ? ("g42=" + std::to_string(r1->group) + " g7=" + std::to_string(r3->group)) : "missing");
    Check("partition-zero-stays-zero", r4 && r4->group == 0, r4 ? UidGen(r4->uid) : "missing");
    Check("partition-second-copy-keeps-its-own-group", r5 && r5->group == r3->group && r5->group != r1->group, r5 ? UidGen(r5->uid) : "missing");
    CheckNoQueueUnderLock("roundtrip-no-queue-under-lock");
    Check("engine-callbacks-saw-lock-free", oracle::lockFreeDuringEngineCalls, "");

    // save (re-export of the loaded values) and inspect the real file through the codec
    Require(core::SaveProject("t9-roundtrip-copy", core::SaveWholeScene), "resave-accepted", "error=" + ProjError());
    proj_codec::Document out; std::string error;
    Require(ParseProjectFile(ProjPath("t9-roundtrip-copy"), out, error), "resave-parses", error);
    Check("resave-modern-document", out.kind == proj_codec::Kind::Project && !out.legacy && out.records.size() == 5, "records=" + std::to_string(out.records.size()));
    Check("resave-envelope-count", out.hasBounds && out.envelopes.size() == 2, "envelopes=" + std::to_string(out.envelopes.size()));
    if (out.envelopes.size() == 2) {
        Check("envelope-1-pivot-preserved", PointEq(out.envelopes[0].bounds.anchor, -1, 0, -1) && PointEq(out.envelopes[0].bounds.min, -2, 0, -2) && PointEq(out.envelopes[0].bounds.max, 0, 3, 0),
              "envelope1=" + PointStr(out.envelopes[0].bounds.anchor));
        Check("envelope-1-quality-measured", !out.envelopes[0].bounds.approximate, "");
        Check("envelope-2-pivot-preserved", PointEq(out.envelopes[1].bounds.anchor, 5, 0, 4.5) && PointEq(out.envelopes[1].bounds.min, 4, 0, 4) && PointEq(out.envelopes[1].bounds.max, 6, 2, 5),
              "envelope2=" + PointStr(out.envelopes[1].bounds.anchor));
        Check("envelope-2-quality-measured", !out.envelopes[1].bounds.approximate, "");
    }
    Check("document-union-bounds", out.hasBounds && PointEq(out.bounds.min, -2, 0, -2) && PointEq(out.bounds.max, 6, 3, 5) && PointEq(out.bounds.anchor, 2, 0, 1.5),
          "doc=" + PointStr(out.bounds.anchor));
    Check("member-envelope-mapping-preserved", out.records.size() == 5 && out.records[0].envelope == 1 && out.records[1].envelope == 1 && out.records[2].envelope == 1 &&
          out.records[3].envelope == 2 && out.records[4].envelope == 2, "envelopes=" + std::to_string(out.records.size()));
    Check("partition-canonical-renumber", out.records.size() == 5 && out.records[0].group == 1 && out.records[1].group == 1 && out.records[2].group == 2 &&
          out.records[3].group == 0 && out.records[4].group == 2, "");
    Check("records-carry-all-values", out.records.size() == 5 && out.records[1].prefab == "/object/b.prefab" && out.records[1].pos.y == 2 && out.records[3].yaw == 30 && out.records[3].scale == 1.5, "");
    Check("off-origin-pivot-not-recentered", out.envelopes.size() == 2 && PointEq(out.envelopes[0].bounds.anchor, -1, 0, -1), "anchor=" + (out.envelopes.empty() ? std::string("none") : PointStr(out.envelopes[0].bounds.anchor)));

    // reload the saved document and re-export: the values must be preserved exactly (byte-for-byte, since
    // Serialize is canonical)
    core::DeleteAllSpawned();
    PumpAll();
    Require(editor::host_seam::LoadProjectAction("t9-roundtrip-copy", false), "reload-accepted", "error=" + ProjError());
    PumpAll();
    const core::ProjectLoadReport rep2 = Rep("t9-roundtrip-copy");
    Check("reload-report-all-queued", rep2.valid && rep2.requested == 5 && rep2.queued == 5 && rep2.excluded == 0, RepStr(rep2));
    Require(core::SaveProject("t9-roundtrip-reexport", core::SaveWholeScene), "reexport-accepted", "error=" + ProjError());
    Check("reexport-byte-identical", FileText(ProjPath("t9-roundtrip-copy")) == FileText(ProjPath("t9-roundtrip-reexport")),
          "copy=" + std::to_string(FileText(ProjPath("t9-roundtrip-copy")).size()) + "B reexport=" + std::to_string(FileText(ProjPath("t9-roundtrip-reexport")).size()) + "B");
    proj_codec::Document again; std::string error2;
    Require(ParseProjectFile(ProjPath("t9-roundtrip-reexport"), again, error2), "reexport-parses", error2);
    Check("reexport-pivots-still-exact", again.envelopes.size() == 2 && PointEq(again.envelopes[0].bounds.anchor, -1, 0, -1) && PointEq(again.envelopes[1].bounds.anchor, 5, 0, 4.5), "");
}

// happy/adversarial: the full SaveProject adoption chain from Task 7, including a newer adoption that happens
// while the file is being written
static void CaseAdoption() {
    BeginCase("adoption-save-undo-redo");
    const int pA = core::ProjectId("t9-adopt-a");
    const int pB = core::ProjectId("t9-adopt-b");
    Check("project-ids-created", pA > 0 && pB > 0 && pA != pB, "pA=" + std::to_string(pA) + " pB=" + std::to_string(pB));
    const int uid = Spawn("/object/box.prefab", { 3, 0, 3 }, { 45, 0, 0 }, 1.25f);
    PumpAll();
    Snapshot();
    Check("spawn-materialized", Rec(uid) && !Rec(uid)->hidden && Rec(uid)->obj != 0, UidGen(uid));
    Check("history-has-spawn", editor::host_seam::UndoSize() == 1, "undo=" + std::to_string(editor::host_seam::UndoSize()));

    // (1) first save adopts the record into t9-adopt-a
    Require(core::SaveProject("t9-adopt-a", core::SaveWholeScene), "save-a-accepted", "error=" + ProjError());
    Snapshot();
    Check("adopted-into-a", Rec(uid) && Rec(uid)->proj == pA, UidGen(uid));
    Check("a-clean-after-save", !core::ProjectDirty(pA), "");
    Check("file-a-has-the-row", RowCount(ProjPath("t9-adopt-a")) == 1, "rows=" + std::to_string(RowCount(ProjPath("t9-adopt-a"))));

    // (2) undo the spawn, adopt into b while hidden, redo: the record keeps the NEWEST adoption
    Require(editor::host_seam::UndoOne(), "undo-spawn", "");
    core::AssignProject(uid, pB);
    Require(editor::host_seam::RedoOne(), "redo-spawn", "");
    PumpAll();
    Snapshot();
    Check("newest-adoption-survives-redo", Rec(uid) && !Rec(uid)->hidden && Rec(uid)->obj != 0 && Rec(uid)->proj == pB, UidGen(uid));
    {
        editor::host_seam::ActView act;
        const bool haveAct = editor::host_seam::HistoryView(false, 0, 0, &act);
        Check("act-keeps-its-old-snapshot", haveAct && act.uid == uid && act.proj == 0, "act.uid=" + std::to_string(act.uid) + " act.proj=" + std::to_string(act.proj));
    }

    // (3) save b while a NEWER adoption happens inside the write: the captured proj must not overwrite it
    host::SetBeforeReplace([uid, pA] { core::AssignProject(uid, pA); });
    Require(core::SaveProject("t9-adopt-b", core::SaveProjectAndNew), "save-b-with-interleaved-adoption", "error=" + ProjError());
    Snapshot();
    Check("interleaved-newest-adoption-kept", Rec(uid) && Rec(uid)->proj == pA, UidGen(uid));
    Check("stale-capture-did-not-clobber", Rec(uid) && Rec(uid)->proj != pB, UidGen(uid));
    Check("b-file-still-written", RowCount(ProjPath("t9-adopt-b")) == 1, "rows=" + std::to_string(RowCount(ProjPath("t9-adopt-b"))));
    Check("stale-project-stays-dirty", core::ProjectDirty(pB), "dirty(b)=" + std::string(core::ProjectDirty(pB) ? "1" : "0"));

    // (4) undo/redo once more, then reload the persisted row and re-export it unchanged
    Require(editor::host_seam::UndoOne(), "undo-spawn-again", "");
    Require(editor::host_seam::RedoOne(), "redo-spawn-again", "");
    PumpAll();
    Snapshot();
    Check("adoption-survives-second-cycle", Rec(uid) && Rec(uid)->proj == pA && !Rec(uid)->hidden && Rec(uid)->obj != 0, UidGen(uid));
    core::DeleteAllSpawned();
    PumpAll();
    Require(editor::host_seam::LoadProjectAction("t9-adopt-b", false), "reload-b", "error=" + ProjError());
    PumpAll();
    Snapshot();
    Check("reload-restores-the-persisted-row", VisibleCount() == 1 && g_list.size() == 1 && g_list[0].prefab == "/object/box.prefab" &&
          std::fabs(g_list[0].pos.x - 3) < 1e-6f && std::fabs(g_list[0].rot.yaw - 45) < 1e-6f && std::fabs(g_list[0].scale - 1.25f) < 1e-6f,
          g_list.empty() ? "empty" : UidGen(g_list[0].uid));
    Require(core::SaveProject("t9-adopt-b-reexport", core::SaveWholeScene), "reexport-accepted", "error=" + ProjError());
    Check("reexport-matches-saved-b", FileText(ProjPath("t9-adopt-b")) == FileText(ProjPath("t9-adopt-b-reexport")), "");
    Check("reexport-clears-b-star", !core::ProjectDirty(pB), "");
    CheckNoQueueUnderLock("adoption-no-queue-under-lock");
}

// happy: the four SaveScope modes, the scoped dirty stars and untouched other-project files
static void CaseSaveScopes() {
    BeginCase("save-scopes");
    const int pA = core::ProjectId("t9-scope-a");
    const int pB = core::ProjectId("t9-scope-b");
    const int a1 = SpawnCore("/object/a1.prefab", { 1, 0, 0 }, {}, 1, 0, pA);
    const int a2 = SpawnCore("/object/a2.prefab", { 2, 0, 0 }, {}, 1, 0, pA);
    const int b1 = SpawnCore("/object/b1.prefab", { 3, 0, 0 }, {}, 1, 0, pB);
    const int n1 = SpawnCore("/object/new1.prefab", { 4, 0, 0 }, {}, 1, 0, 0);
    PumpAll();
    Snapshot();
    Check("four-records", VisibleCount() == 4 && g_list.size() == 4, "visible=" + std::to_string(VisibleCount()));
    Require(MoveFinal(a1, { 1, 0, 1 }, {}, 1), "move-a1", "");
    Require(MoveFinal(b1, { 3, 0, 1 }, {}, 1), "move-b1", "");
    PumpAll();
    Check("a-and-b-dirty", core::ProjectDirty(pA) && core::ProjectDirty(pB), "");

    // SaveProjectOnly: exactly the project's own records; only its star is cleared
    Require(core::SaveProject("t9-scope-b", core::SaveProjectOnly), "save-b-only", "error=" + ProjError());
    const std::string hashB = Sha256File(ProjPath("t9-scope-b"));
    Check("b-file-one-row", RowCount(ProjPath("t9-scope-b")) == 1, "rows=" + std::to_string(RowCount(ProjPath("t9-scope-b"))));
    Check("b-clean-a-still-dirty", !core::ProjectDirty(pB) && core::ProjectDirty(pA), "");
    Snapshot();
    Check("membership-kept", Rec(a1) && Rec(a1)->proj == pA && Rec(a2) && Rec(a2)->proj == pA && Rec(b1) && Rec(b1)->proj == pB && Rec(n1) && Rec(n1)->proj == 0, "");

    // SaveProjectAndNew: the project plus everything new; other project files untouched
    Require(core::SaveProject("t9-scope-a", core::SaveProjectAndNew), "save-a-and-new", "error=" + ProjError());
    Check("a-file-three-rows", RowCount(ProjPath("t9-scope-a")) == 3, "rows=" + std::to_string(RowCount(ProjPath("t9-scope-a"))));
    Check("b-file-bytes-untouched", Sha256File(ProjPath("t9-scope-b")) == hashB, "");
    Check("a-clean-after-its-save", !core::ProjectDirty(pA), "");
    Snapshot();
    Check("new-adopted-into-a", Rec(n1) && Rec(n1)->proj == pA, UidGen(n1));
    Check("b-membership-untouched", Rec(b1) && Rec(b1)->proj == pB, UidGen(b1));

    // SaveNewOnly: only records still outside every project; scoped star semantics stay the legacy ones
    Require(MoveFinal(a2, { 2, 0, 2 }, {}, 1), "re-dirty-a", "");
    Require(MoveFinal(b1, { 3, 0, 2 }, {}, 1), "re-dirty-b", "");
    PumpAll();
    Check("a-and-b-dirty-again", core::ProjectDirty(pA) && core::ProjectDirty(pB), "");
    Require(core::SaveProject("t9-scope-a", core::SaveNewOnly), "save-new-only", "error=" + ProjError());
    Check("new-only-file-empty", RowCount(ProjPath("t9-scope-a")) == 0, "rows=" + std::to_string(RowCount(ProjPath("t9-scope-a"))));
    Check("new-only-clears-own-star", !core::ProjectDirty(pA), "");
    Check("new-only-keeps-other-star", core::ProjectDirty(pB), "");

    // SaveProjectOnly again: every record that is a member by now (a1, a2 and the new one adopted above),
    // b's file still byte-identical
    Require(core::SaveProject("t9-scope-a", core::SaveProjectOnly), "save-a-only", "error=" + ProjError());
    Check("a-file-three-rows-after-reonly", RowCount(ProjPath("t9-scope-a")) == 3, "rows=" + std::to_string(RowCount(ProjPath("t9-scope-a"))));
    Check("b-file-still-untouched", Sha256File(ProjPath("t9-scope-b")) == hashB, "");

    // SaveWholeScene: everything, every star cleared, existing project files not rewritten
    Require(core::SaveProject("t9-scope-all", core::SaveWholeScene), "save-all", "error=" + ProjError());
    const int pAll = core::ProjectId("t9-scope-all");
    Check("all-file-four-rows", RowCount(ProjPath("t9-scope-all")) == 4, "rows=" + std::to_string(RowCount(ProjPath("t9-scope-all"))));
    Snapshot();
    Check("whole-scene-adopts-everything", Rec(b1) && Rec(b1)->proj == pAll && Rec(a1) && Rec(a1)->proj == pAll, "");
    Check("all-stars-cleared", !core::ProjectDirty(pA) && !core::ProjectDirty(pB) && !core::ProjectDirty(pAll), "");
    Check("scope-files-not-rewritten", Sha256File(ProjPath("t9-scope-b")) == hashB, "");
    CheckNoQueueUnderLock("save-scopes-no-queue-under-lock");
}

// happy: legacy rows/defaults stay valid; a valid missing-prefab row is a NAMED exclusion
static void CaseLegacy() {
    BeginCase("legacy-defaults-and-missing");
    const std::string doc =
        "/object/legacy4.prefab|10|1|10\n"
        "/object/legacy7.prefab|11|1|11|90|2|3\n"
        "/object/legacy9.prefab|12|1|12|45|1.5|3|10|5\n"
        "# hidden /object/hidden.prefab|0|0|0|0|1|5\n"
        "# missing /object/gone.prefab|2|0|2|0|1|5\n";
    Require(WriteTextFile(ProjPath("t9-legacy"), doc), "fixture-wrote-legacy-document", "");
    const std::string hash0 = Sha256File(ProjPath("t9-legacy"));
    Require(editor::host_seam::LoadProjectAction("t9-legacy", false), "legacy-load-accepted", "error=" + ProjError());
    const core::ProjectLoadReport rep = Rep("t9-legacy");
    Check("legacy-report-counts", rep.valid && rep.requested == 5 && rep.queued == 3 && rep.excluded == 2, RepStr(rep));
    Check("missing-prefab-named-exclusion", rep.excludedPrefabs.size() == 2 && rep.excludedPrefabs[0] == "/object/hidden.prefab" && rep.excludedPrefabs[1] == "/object/gone.prefab", RepStr(rep));
    Check("legacy-file-untouched", Sha256File(ProjPath("t9-legacy")) == hash0, "");
    PumpAll();
    Snapshot();
    Check("legacy-three-visible", VisibleCount() == 3 && g_list.size() == 3, "visible=" + std::to_string(VisibleCount()));
    const SpawnedObj* r4 = g_list.size() > 0 ? &g_list[0] : nullptr;
    const SpawnedObj* r7 = g_list.size() > 1 ? &g_list[1] : nullptr;
    const SpawnedObj* r9 = g_list.size() > 2 ? &g_list[2] : nullptr;
    Check("legacy-defaults-applied", r4 && std::fabs(r4->pos.x - 10) < 1e-6f && r4->rot.yaw == 0 && r4->rot.pitch == 0 && r4->rot.roll == 0 && r4->scale == 1 && r4->group == 0,
          r4 ? UidGen(r4->uid) : "missing");
    Check("legacy-seven-fields", r7 && std::fabs(r7->rot.yaw - 90) < 1e-6f && std::fabs(r7->scale - 2) < 1e-6f && r7->rot.pitch == 0 && r7->group > 0, r7 ? UidGen(r7->uid) : "missing");
    Check("legacy-nine-keeps-tilt", r9 && std::fabs(r9->rot.yaw - 45) < 1e-6f && std::fabs(r9->rot.pitch - 10) < 1e-6f && std::fabs(r9->rot.roll - 5) < 1e-6f,
          r9 ? UidGen(r9->uid) : "missing");
    Check("excluded-rows-allocated-nothing", r4 && r7 && r9 && r7->uid == r4->uid + 1 && r9->uid == r4->uid + 2, "uids=" + std::to_string(r4 ? r4->uid : -1) + "," + std::to_string(r7 ? r7->uid : -1) + "," + std::to_string(r9 ? r9->uid : -1));

    // re-save: legacy values become a modern document with per-record approximate envelopes (no saved bounds existed)
    Require(core::SaveProject("t9-legacy-resave", core::SaveWholeScene), "legacy-resave-accepted", "error=" + ProjError());
    proj_codec::Document out; std::string error;
    Require(ParseProjectFile(ProjPath("t9-legacy-resave"), out, error), "legacy-resave-parses", error);
    Check("legacy-resave-modern", !out.legacy && out.hasBounds && out.records.size() == 3, "legacy=" + std::to_string(out.legacy ? 1 : 0) + " records=" + std::to_string(out.records.size()));
    Check("legacy-resave-envelope-per-record", out.envelopes.size() == 3, "envelopes=" + std::to_string(out.envelopes.size()));
    Check("legacy-resave-approximate", out.bounds.approximate && !out.envelopes.empty() && out.envelopes[0].bounds.approximate, "");
    // The anchors are the AABB bottom centres of the unknown 2 m cubes: the unscaled record sits at its
    // position minus half a metre, the scale-2 record a full 2 m below (yaw does not change the y extent), and
    // the tilted record's rotated box keeps the pivot's x/z while its bottom drops below the pivot.
    Check("legacy-resave-unknown-box-anchors", out.envelopes.size() == 3 && PointEq(out.envelopes[0].bounds.anchor, 10, 0, 10) &&
          std::fabs(out.envelopes[1].bounds.anchor.x - 11) < 1e-4 && std::fabs(out.envelopes[1].bounds.anchor.y + 1) < 1e-4 && std::fabs(out.envelopes[1].bounds.anchor.z - 11) < 1e-4 &&
          std::fabs(out.envelopes[2].bounds.anchor.x - 12) < 1e-4 && std::fabs(out.envelopes[2].bounds.anchor.z - 12) < 1e-4 && out.envelopes[2].bounds.anchor.y < 1.0,
          out.envelopes.empty() ? "none" : PointStr(out.envelopes[0].bounds.anchor) + " " + (out.envelopes.size() > 2 ? PointStr(out.envelopes[1].bounds.anchor) + " " + PointStr(out.envelopes[2].bounds.anchor) : std::string("-")));
    Check("legacy-group-canonicalized", out.records.size() == 3 && out.records[0].group == 0 && out.records[1].group == 1 && out.records[2].group == 1, "");

    // reload the resave: the same values come back from the modern document
    core::DeleteAllSpawned();
    PumpAll();
    Require(editor::host_seam::LoadProjectAction("t9-legacy-resave", false), "legacy-resave-reload", "error=" + ProjError());
    PumpAll();
    Snapshot();
    Check("legacy-roundtrip-values", VisibleCount() == 3 && g_list.size() == 3 && std::fabs(g_list[2].rot.pitch - 10) < 1e-6f && std::fabs(g_list[2].rot.roll - 5) < 1e-6f &&
          g_list[0].group == 0 && g_list[1].group == g_list[2].group && g_list[1].group > 0, "");
    CheckNoQueueUnderLock("legacy-no-queue-under-lock");
}

// adversarial: a malformed final row rejects the whole document; nothing changes and nothing is imported
static void CaseReplaceLastRow() {
    BeginCase("replace-last-row");
    const int a = Spawn("/object/keep-a.prefab", { 1, 0, 0 });
    const int b = Spawn("/object/keep-b.prefab", { 2, 0, 0 });
    PumpAll();
    DeleteViaEditor(b);
    editor::host_seam::SelectUid(a);
    Check("history-populated", editor::host_seam::UndoSize() == 3 && editor::host_seam::RedoSize() == 0, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    Check("selection-set", editor::host_seam::SelectionSize() == 1 && editor::host_seam::IsSelected(a), "");

    // malformed documents: a bad final row (arity / NaN) must not load silently
    const std::string header =
        "# cdproj v3 kind=project\n# wb-document " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "approx") +
        "# wb-envelope id=1 " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "approx") +
        "# wb-member record=1 envelope=1\n" + Row("/object/one.prefab", 0, 0, 0) +
        "# wb-member record=2 envelope=1\n" + Row("/object/two.prefab", 0, 0, 0);
    const std::string cases[2] = {
        header + "/object/broken.prefab|1|2\n",                          // arity 2: malformed data, not a missing prefab
        header + "/object/broken.prefab|nan|0|0|0|1|0|0|0\n",            // NaN: malformed data
    };
    const char* names[2] = { "t9-replace-arity", "t9-replace-nan" };
    const char* ordinals[2] = { "record 3", "record 3" };
    for (int i = 0; i < 2; ++i) {
        const std::string path = ProjPath(names[i]);
        Require(WriteTextFile(path, cases[i]), "fixture-wrote-malformed-document", "");
        const std::string hash = Sha256File(path);
        const int probeBase = core::ProjectId(std::string(names[i]) + "-idbase");
        const WorldState before = CaptureWorld();
        const bool ok = editor::host_seam::LoadProjectAction(names[i], true);
        Check((std::string("replace-rejected-") + names[i]).c_str(), !ok, "returned=" + std::to_string(ok));
        const std::string error = ProjError();
        Check((std::string("error-names-ordinal-") + names[i]).c_str(), error.find(ordinals[i]) != std::string::npos, "error=" + error);
        Check((std::string("no-receipt-published-") + names[i]).c_str(), !Rep(names[i]).valid, RepStr(Rep(names[i])));
        Check((std::string("file-bytes-unchanged-") + names[i]).c_str(), Sha256File(path) == hash, "");
        CheckWorldUnchanged((std::string("world-unchanged-") + names[i]).c_str(), before);
        Check((std::string("project-id-not-allocated-") + names[i]).c_str(), core::ProjectId(std::string(names[i]) + "-idprobe") == probeBase + 1,
              "base=" + std::to_string(probeBase) + " probe=" + std::to_string(core::ProjectId(std::string(names[i]) + "-idprobe")));
    }

    // the same malformed bytes are refused by the validate-first import (nothing appears on disk),
    // while a valid document imports and parses
    const WorldState beforeImport = CaptureWorld();
    Require(WriteTextFile(OutsidePath("t9-import-bad.cdproj"), cases[0]), "fixture-wrote-import-source", "");
    Check("import-rejects-malformed", !core::ImportProjectFile(OutsidePath("t9-import-bad.cdproj")), "error=" + ProjError());
    Check("import-created-nothing", !FileExists(ProjPath("t9-import-bad")), "");
    Require(WriteTextFile(OutsidePath("t9-import-ok.cdproj"), OneEnvelopeDoc({ Row("/object/imported.prefab", 7, 0, 7) })), "fixture-wrote-valid-import-source", "");
    Check("import-accepts-valid", core::ImportProjectFile(OutsidePath("t9-import-ok.cdproj")), "error=" + ProjError());
    Check("import-parses-at-destination", RowCount(ProjPath("t9-import-ok")) == 1, "rows=" + std::to_string(RowCount(ProjPath("t9-import-ok"))));
    CheckWorldUnchanged("replace-last-row-import-world", beforeImport);
}

// adversarial: float overflow / tile overflow / scale underflow are rejected before ANY mutation
static void CaseNarrowing() {
    BeginCase("narrowing-rejected");
    const int a = Spawn("/object/keep.prefab", { 1, 0, 0 });
    PumpAll();
    editor::host_seam::SelectUid(a);
    const WorldState before = CaptureWorld();
    struct Narrow { const char* name; std::string row; const char* ordinal; const char* label; };
    const Narrow rows[3] = {
        { "t9-narrow-float", Row("/object/overflow.prefab", 1e300, 0, 0), "record 2", "engine float overflow" },
        { "t9-narrow-tile",  Row("/object/overflow.prefab", 5e9, 0, 0), "record 2", "engine tile overflow" },
        { "t9-narrow-scale", Row("/object/underflow.prefab", 0, 0, 0, 0, 1e-300), "record 2", "engine scale underflow" },
    };
    for (const Narrow& n : rows) {
        const std::string path = ProjPath(n.name);
        const std::string document = OneEnvelopeDoc({ Row("/object/ok.prefab", 0, 0, 0), n.row });
        Require(WriteTextFile(path, document), "fixture-wrote-narrowing-document", "");
        const std::string hash = Sha256File(path);
        const int probeBase = core::ProjectId(std::string(n.name) + "-idbase");
        const bool ok = editor::host_seam::LoadProjectAction(n.name, true);
        Check((std::string("narrowing-rejected-") + n.name).c_str(), !ok, "returned=" + std::to_string(ok));
        const std::string error = ProjError();
        Check((std::string("narrowing-error-") + n.name).c_str(), error.find(n.ordinal) != std::string::npos && error.find(n.label) != std::string::npos, "error=" + error);
        Check((std::string("narrowing-file-unchanged-") + n.name).c_str(), Sha256File(path) == hash, "");
        Check((std::string("narrowing-id-not-allocated-") + n.name).c_str(), core::ProjectId(std::string(n.name) + "-idprobe") == probeBase + 1, "");
        CheckWorldUnchanged((std::string("narrowing-world-unchanged-") + n.name).c_str(), before);
    }
    // a failed clearFirst load also never clears the scene: the seeded record is still there
    Snapshot();
    Check("seed-record-survives", Rec(a) && !Rec(a)->hidden && oracle::Live(Rec(a)->obj), UidGen(a));
    CheckNoQueueUnderLock("narrowing-no-queue-under-lock");
}

// adversarial: a failed transactional write preserves bytes, membership, dirty stars and the world
static void CaseWriteFail() {
    BeginCase("write-fail");
    const int pA = core::ProjectId("t9-wfail");
    const int a1 = SpawnCore("/object/w1.prefab", { 1, 0, 0 }, {}, 1, 0, pA);
    const int a2 = SpawnCore("/object/w2.prefab", { 2, 0, 0 }, {}, 1, 0, pA);
    PumpAll();
    Require(core::SaveProject("t9-wfail", core::SaveProjectOnly), "seed-save", "error=" + ProjError());
    const std::string bytes0 = FileText(ProjPath("t9-wfail"));
    Check("seed-clean", !core::ProjectDirty(pA), "");
    Require(MoveFinal(a1, { 5, 0, 5 }, {}, 2.0f), "move-after-seed", "");
    PumpAll();
    Check("dirty-before-failed-save", core::ProjectDirty(pA), "");
    const WorldState before = CaptureWorld();
    const std::vector<std::pair<host::SaveFault, const char*>> faults = {
        { host::SaveFault::WriteAbort, "write-abort" },
        { host::SaveFault::ShortWrite, "short-write" },
        { host::SaveFault::FlushAbort, "flush-abort" },
        { host::SaveFault::ReplaceAbort, "replace-abort" },
    };
    for (const auto& fault : faults) {
        host::SetSaveFault(fault.first);
        const bool ok = core::SaveProject("t9-wfail", core::SaveProjectAndNew);
        Check((std::string("fault-") + fault.second + "-rejected").c_str(), !ok, "returned=" + std::to_string(ok));
        Check((std::string("fault-") + fault.second + "-error-named").c_str(), !ProjError().empty(), "error=" + ProjError());
        Check((std::string("fault-") + fault.second + "-bytes-preserved").c_str(), FileText(ProjPath("t9-wfail")) == bytes0, "");
        Check((std::string("fault-") + fault.second + "-no-temp-left").c_str(), TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
        CheckWorldUnchanged((std::string("fault-") + fault.second + "-world-unchanged").c_str(), before);
        Snapshot();
        Check((std::string("fault-") + fault.second + "-membership-preserved").c_str(), Rec(a1) && Rec(a1)->proj == pA && Rec(a2) && Rec(a2)->proj == pA && !Rec(a1)->hidden, UidGen(a1));
        Check((std::string("fault-") + fault.second + "-dirty-preserved").c_str(), core::ProjectDirty(pA), "");
    }
    // recovery: the same call succeeds once the fault is gone
    host::SetSaveFault(host::SaveFault::None);
    Require(core::SaveProject("t9-wfail", core::SaveProjectAndNew), "recovery-save", "error=" + ProjError());
    Check("recovery-clean", !core::ProjectDirty(pA), "");
    Snapshot();
    Check("recovery-adopted", Rec(a1) && Rec(a1)->proj == pA, UidGen(a1));
    const std::string bytes1 = FileText(ProjPath("t9-wfail"));
    Check("recovery-wrote-new-bytes", bytes1 != bytes0 && RowCount(ProjPath("t9-wfail")) == 2, "rows=" + std::to_string(RowCount(ProjPath("t9-wfail"))));
    // a failed write of a CLEAN project must not invent a dirty star
    host::SetSaveFault(host::SaveFault::ShortWrite);
    Check("clean-project-failed-write-rejected", !core::SaveProject("t9-wfail", core::SaveProjectAndNew), "");
    Check("failed-write-keeps-clean-star-off", !core::ProjectDirty(pA), "");
    Check("failed-write-keeps-recovered-bytes", FileText(ProjPath("t9-wfail")) == bytes1, "");
    host::SetSaveFault(host::SaveFault::None);
    CheckNoQueueUnderLock("write-fail-no-queue-under-lock");
}

// adversarial: .cdgroup is never loaded or autoloaded; autoload order is file order; a manual load suppresses it
static void CaseGroupAutoload() {
    BeginCase("group-autoload-prohibited");
    const int keep = Spawn("/object/keep.prefab", { 1, 0, 0 });
    PumpAll();
    editor::host_seam::SelectUid(keep);
    const std::string groupDoc = "# cdproj v3 kind=group\n# wb-document " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "measured") +
                                 "# wb-envelope id=1 " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "measured") +
                                 "# wb-member record=1 envelope=1\n" + Row("/object/g1.prefab", 0, 0, 0);
    Require(WriteTextFile(GroupPath("t9-grouponly"), groupDoc), "fixture-wrote-group-file", "");
    const std::string groupHash = Sha256File(GroupPath("t9-grouponly"));
    const WorldState before = CaptureWorld();

    // (a) neither the bare name nor the explicit group file name loads; nothing is cleared, spawned or allocated
    Check("group-bare-name-rejected", !editor::host_seam::LoadProjectAction("t9-grouponly", true), "error=" + ProjError());
    CheckWorldUnchanged("group-bare-world-unchanged", before);
    const int probeBase = core::ProjectId("t9-ga-probe-base");
    Check("group-file-name-rejected", !editor::host_seam::LoadProjectAction("t9-grouponly.cdgroup", true), "error=" + ProjError());
    Check("group-file-name-rejected-named", !ProjError().empty() && editor::g_projectStatus == ProjError(), "error=" + ProjError());
    CheckWorldUnchanged("group-file-world-unchanged", before);
    Check("group-load-allocated-no-project", core::ProjectId("t9-ga-probe") == probeBase + 1, "base=" + std::to_string(probeBase));
    Check("group-file-bytes-untouched", Sha256File(GroupPath("t9-grouponly")) == groupHash, "");
    Check("no-cdproj-created-for-group", !FileExists(ProjPath("t9-grouponly")), "");

    // (b) SetAutoload refuses a group name outright
    core::SetAutoload("t9-grouponly.cdgroup", true);
    {
        const std::vector<std::string> list = core::Autoload();
        Check("setautoload-refuses-group-name", std::find(list.begin(), list.end(), "t9-grouponly.cdgroup") == list.end(), "autoload=" + std::to_string(list.size()));
    }

    // (c) autoload order is file order; a hand-edited .cdgroup line is attempted and rejected, never loaded
    Require(WriteTextFile(ProjPath("t9-auto-b"), OneEnvelopeDoc({ Row("/object/autob.prefab", 20, 0, 0) })), "fixture-wrote-auto-b", "");
    Require(WriteTextFile(ProjPath("t9-auto-a"), OneEnvelopeDoc({ Row("/object/autoa.prefab", 21, 0, 0) })), "fixture-wrote-auto-a", "");
    Require(WriteTextFile(AutoloadPath(), "t9-auto-b\nt9-grouponly.cdgroup\nt9-auto-a\n"), "fixture-wrote-autoload-list", "");
    core::DeleteAllSpawned();
    PumpAll();
    host::ResetAutoloadDone();
    host::AutoloadRun();
    PumpAll();
    Snapshot();
    Check("autoload-loaded-in-file-order", g_list.size() == 2 && g_list[0].prefab == "/object/autob.prefab" && g_list[1].prefab == "/object/autoa.prefab" &&
          g_list[0].uid < g_list[1].uid, g_list.size() == 2 ? UidGen(g_list[0].uid) + " " + UidGen(g_list[1].uid) : "records=" + std::to_string(g_list.size()));
    Check("autoload-never-loaded-the-group", g_list.size() == 2 && std::none_of(g_list.begin(), g_list.end(), [](const SpawnedObj& o) { return o.prefab == "/object/g1.prefab"; }), "");
    Check("autoload-file-untouched", FileText(AutoloadPath()) == std::string("t9-auto-b\nt9-grouponly.cdgroup\nt9-auto-a\n"), "");
    Check("group-file-untouched-by-autoload", Sha256File(GroupPath("t9-grouponly")) == groupHash, "");

    // (d) a manual validated load settles the session: the same autoload list loads nothing more
    host::ResetAutoloadDone();
    Require(editor::host_seam::LoadProjectAction("t9-auto-a", false), "manual-load-accepted", "error=" + ProjError());
    PumpAll();
    Snapshot();
    const size_t afterManual = g_list.size();
    Check("manual-load-added-its-row", afterManual == 3, "records=" + std::to_string(afterManual));
    host::AutoloadRun();
    PumpAll();
    Snapshot();
    Check("manual-load-suppresses-autoload", g_list.size() == afterManual, "records=" + std::to_string(g_list.size()));
    CheckNoQueueUnderLock("group-autoload-no-queue-under-lock");
}

// adversarial: an engine refusal AFTER admission is an honest partial failure, not a rollback and not a
// mislabeled missing-prefab exclusion
static void CasePostAdmissionPartial() {
    BeginCase("post-admission-partial");
    const int old1 = Spawn("/object/old.prefab", { 9, 0, 9 });
    PumpAll();
    Snapshot();
    Require(Rec(old1) && Rec(old1)->obj != 0 && oracle::Live(Rec(old1)->obj), "old-record-live", UidGen(old1));
    const uintptr_t oldHandle = Rec(old1)->obj;
    editor::host_seam::SelectUid(old1);
    Require(WriteTextFile(ProjPath("t9-partial"), OneEnvelopeDoc({
        Row("/object/p1.prefab", 1, 0, 1), Row("/object/p2.prefab", 2, 0, 2), Row("/object/p3.prefab", 3, 0, 3) })), "fixture-wrote-partial-document", "");
    const std::string hash0 = Sha256File(ProjPath("t9-partial"));

    // the engine refuses the SECOND create of the replacement; admission of every row is already done
    oracle::refuseCreateCall = oracle::createCalls + 2;
    const bool ok = editor::host_seam::LoadProjectAction("t9-partial", true);
    Check("replace-admitted", ok, "error=" + ProjError());
    // the replace queues the old object's removal plus one spawn per admitted row
    Check("queues-removal-and-admissions", core::PendingSpawns() == 4, "pending=" + std::to_string(core::PendingSpawns()));
    Check("history-cleared-by-successful-replace", editor::host_seam::UndoSize() == 0 && editor::host_seam::RedoSize() == 0, "undo=" + std::to_string(editor::host_seam::UndoSize()));
    Check("selection-cleared-by-successful-replace", editor::host_seam::SelectionSize() == 0, "");
    PumpAll();
    Snapshot();
    Check("old-handle-disposed", oracle::DisposedHandle(oldHandle), oracle::Describe(oldHandle));
    Check("old-scene-not-rolled-back", Rec(old1) == nullptr, "registry=" + std::to_string(g_list.size()));
    Check("two-attached-one-refused", VisibleCount() == 2 && g_list.size() == 3, "visible=" + std::to_string(VisibleCount()) + " records=" + std::to_string(g_list.size()));
    const SpawnedObj* hiddenRow = nullptr;
    int attachedRows = 0;
    for (const SpawnedObj& o : g_list) {
        if (o.hidden) { hiddenRow = &o; continue; }
        if (oracle::Live(o.obj)) ++attachedRows;
    }
    Check("refused-row-is-hidden-tombstone", hiddenRow && hiddenRow->prefab == "/object/p2.prefab" && hiddenRow->obj == 0,
          hiddenRow ? UidGen(hiddenRow->uid) : "no hidden row");
    Check("attached-rows-live", attachedRows == 2, "attached=" + std::to_string(attachedRows));
    Check("engine-failure-not-labeled-exclusion", Rep("t9-partial").excluded == 0 && Rep("t9-partial").excludedPrefabs.empty(), RepStr(Rep("t9-partial")));
    const core::ProjectLoadReport rep = Rep("t9-partial");
    Check("report-honest-about-admission", rep.valid && rep.requested == 3 && rep.queued == 3, RepStr(rep));
    Check("document-file-untouched", Sha256File(ProjPath("t9-partial")) == hash0, "");
    CheckNoQueueUnderLock("post-admission-no-queue-under-lock");
}

// Task 12 exercises the public core surface and the actual editor authority. This adapter lives only in
// the fixture because Task 13 owns the UI wiring. It reads the real stacks, never synthetic allow/deny flags.
using FK = proj_codec::Kind;
using FA = core::FileAction;
using FR = core::FileReason;
static core::SavedFile g_currentFile;
static std::vector<std::string> g_fileProofs, g_refusalProofs;
static std::string J(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '\\' || c == '"') out += '\\'; if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else out += c; }
    return out + "\"";
}
static core::SavedFile Entry(FK kind, bool archived, const std::string& filename) {
    return { kind, archived, filename, g_fixtureDir + (kind == FK::Project ? "\\projects" : "\\Groups") + (archived ? "\\.archive\\" : "\\") + filename };
}
static core::FileSelectionHandle Select(const core::SavedFile& entry) {
    core::FileSelectionHandle selected;
    const auto result = core::SelectSavedFile(entry, selected);
    Require(result.ok() && selected, "select-file", entry.filename + ": " + core::FileReasonCode(result.reason));
    g_currentFile = entry;
    return selected;
}
static bool Listed(const core::SavedFile& file) {
    std::vector<core::SavedFile> list;
    if (!core::ListSavedFiles(file.kind, file.archived, list).ok()) return false;
    return std::any_of(list.begin(), list.end(), [&](const core::SavedFile& f) { return f.filename == file.filename && f.path == file.path; });
}
static FR EditorFileGuard(const core::SavedFile& file) {
    if (file.kind != g_currentFile.kind || file.archived != g_currentFile.archived || file.filename != g_currentFile.filename || file.path != g_currentFile.path) return FR::SelectionChanged;
    const std::string stem = file.filename.substr(0, file.filename.size() - (file.kind == FK::Project ? 7 : 8));
    if (file.kind == FK::Group) {
        if (editor::g_place.active && core::FileNameEqual(editor::g_place.name, stem)) return FR::InFlightPlace;
        return FR::None;
    }
    const auto records = core::Spawned();
    const auto refers = [&](int pid) { return pid > 0 && core::FileNameEqual(core::ProjectNameOf(pid), stem); };
    for (int redo = 0; redo < 2; ++redo) {
        const auto& stack = redo ? editor::g_redo : editor::g_undo;
        for (const auto& entry : stack) for (const auto& act : entry.acts) {
            if (refers(act.proj)) return redo ? FR::RedoReference : FR::UndoReference;
            for (const auto& record : records) if (record.uid == act.uid && refers(record.proj)) return redo ? FR::RedoReference : FR::UndoReference;
        }
    }
    if (editor::g_deferredEditor.action) return FR::PendingOperation;
    for (const auto& op : editor::g_pendingGround) for (const auto& member : core::GroundStateOf(op).members) if (refers(member.before.proj)) return FR::PendingOperation;
    return FR::None;
}
static core::FileResult Act(const core::FileSelectionHandle& selection, FA action, const std::string& typed = "") {
    return core::ExecuteFileAction(selection, action, typed, EditorFileGuard);
}
static void Refusal(const char* id, const core::FileSelectionHandle& selection, FA action, FR expected, const std::string& typed = "") {
    const auto file = core::SelectedFile(selection);
    const std::string before = FileText(file.path), hash = Sha256Bytes(before);
    const WorldState world = CaptureWorld();
    const auto result = Act(selection, action, typed.empty() ? file.filename : typed);
    const bool unchanged = FileExists(file.path) && FileText(file.path) == before;
    const bool listed = Listed(file);
    Check(id, result.reason == expected && !result.ok(), std::string("reason=") + core::FileReasonCode(result.reason));
    Check((std::string(id) + "-bytes").c_str(), unchanged, hash);
    Check((std::string(id) + "-discoverable").c_str(), listed, file.filename);
    CheckWorldUnchanged((std::string(id) + "-world").c_str(), world);
    g_refusalProofs.push_back("{\"id\":" + J(id) + ",\"filename\":" + J(file.filename) + ",\"action\":" + std::to_string((int)action) +
        ",\"expected\":" + J(core::FileReasonCode(expected)) + ",\"actual\":" + J(core::FileReasonCode(result.reason)) + ",\"win32\":" + std::to_string(result.systemError) +
        ",\"beforeSha256\":" + J(hash) + ",\"afterSha256\":" + J(Sha256File(file.path)) + ",\"discoverable\":" + (listed ? "true" : "false") + ",\"worldUnchanged\":" + (SameWorld(world, CaptureWorld()) ? "true" : "false") + "}");
}
static void MakeFile(const core::SavedFile& entry, const std::string& bytes) {
    std::filesystem::create_directories(std::filesystem::path(entry.path).parent_path());
    Require(WriteTextFile(entry.path, bytes), "lifecycle-fixture", entry.filename);
}
static void CaseFileRoundtrip() {
    BeginCase("file-archive-restore-delete");
    // Binary/unparseable bytes are allowed: file maintenance preserves files, never reserializes documents.
    const std::string bytes("# exact CRLF\r\nraw\0payload\r\n", 26);
    for (FK kind : { FK::Project, FK::Group }) {
        const std::string filename = kind == FK::Project ? "Exact Name.CDPROJ" : "Exact Name.CDGROUP";
        const auto active = Entry(kind, false, filename), archive = Entry(kind, true, filename);
        MakeFile(active, bytes);
        if (kind == FK::Project) core::ProjectId("Exact Name"); // allocated id + zero records is not itself a reference
        const auto selected = Select(active);
        const auto world = CaptureWorld();
        Require(Act(selected, FA::Archive).ok(), "archive-success", "");
        Check("archive-only-in-archive-list", !FileExists(active.path) && !Listed(active) && Listed(archive), "");
        const std::string archivedHash = Sha256File(archive.path);
        Check("archive-byte-identical", FileText(archive.path) == bytes, archivedHash);
        if (kind == FK::Project) { const auto list = core::ListProjects(); Check("legacy-active-list-excludes-archive", std::find(list.begin(), list.end(), "Exact Name") == list.end(), ""); }
        const auto a = Select(archive);
        const auto collision = Entry(kind, false, kind == FK::Project ? "exact name.cdproj" : "exact name.cdgroup");
        MakeFile(collision, "collision must survive");
        Refusal("restore-casefold-collision", a, FA::Restore, FR::Collision);
        Check("collision-destination-original", FileText(collision.path) == "collision must survive", "");
        Require(DeleteFileA(collision.path.c_str()) != 0, "remove-owned-collision", "");
        Require(Act(a, FA::Restore).ok(), "restore-success", "");
        Check("restored-exact-filename-and-bytes", Listed(active) && !Listed(archive) && FileText(active.path) == bytes, "");
        g_fileProofs.push_back("{\"filename\":" + J(filename) + ",\"originalSha256\":" + J(Sha256Bytes(bytes)) + ",\"archivedSha256\":" + J(archivedHash) + ",\"restoredSha256\":" + J(Sha256File(active.path)) + "}");
        auto again = Select(active);
        Refusal("direct-delete-exact-confirmation", again, FA::Delete, FR::ConfirmationMismatch, "wrong.cdproj");
        Refusal("wrong-active-action", again, FA::Purge, FR::InvalidAction);
        const auto missingGuard = core::ExecuteFileAction(again, FA::Delete, filename, {});
        Check("missing-authority-fails-closed", missingGuard.reason == FR::GuardMissing && FileText(active.path) == bytes, "");
        Require(Act(again, FA::Delete, filename).ok(), "direct-delete-success", "");
        Check("delete-only-selected-file", !FileExists(active.path) && !Listed(active), "");
        MakeFile(archive, bytes); again = Select(archive);
        Require(Act(again, FA::Purge, filename).ok(), "purge-success", "");
        Check("purge-only-selected-file", !FileExists(archive.path) && !Listed(archive), "");
        CheckWorldUnchanged("file-actions-never-change-world", world);
    }
}
static void CaseFileFailures() {
    BeginCase("file-failure-and-stale-target");
    const auto active = Entry(FK::Project, false, "t12-failure.cdproj");
    const auto archived = Entry(FK::Project, true, active.filename);
    MakeFile(active, "original"); auto s = Select(active);
    core::g_fileMutationFault = core::FileMutationFault::Move;
    Refusal("move-boundary-refusal", s, FA::Archive, FR::MoveFailed);
    Check("failed-move-no-destination", !FileExists(archived.path), "");
    MakeFile(Entry(FK::Project, true, "T12-FAILURE.CDPROJ"), "existing archive");
    Refusal("archive-casefold-collision", s, FA::Archive, FR::Collision);
    Require(DeleteFileA(archived.path.c_str()) != 0, "remove-collision", "");
    core::g_fileMutationFault = core::FileMutationFault::Delete;
    Refusal("delete-boundary-refusal", s, FA::Delete, FR::DeleteFailed);
    HANDLE locked = CreateFileA(active.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "locked-source", "");
    Refusal("locked-source-refusal", s, FA::Archive, FR::ReadFailed);
    FILETIME originalWrite{};
    Require(GetFileTime(locked, nullptr, nullptr, &originalWrite) != 0, "capture-write-time", "");
    Require(CloseHandle(locked) != 0, "unlock-source", "");
    MakeFile(active, "replaced"); // same byte length and same identity; restore mtime so ONLY byte comparison can refuse
    locked = CreateFileA(active.path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "open-write-time", "");
    Require(SetFileTime(locked, nullptr, nullptr, &originalWrite) != 0, "restore-write-time", "");
    Require(CloseHandle(locked) != 0, "close-write-time", "");
    Refusal("changed-bytes-stale-target", s, FA::Delete, FR::StaleTarget);
    s = Select(active);
    const auto other = Entry(FK::Project, false, "t12-other.cdproj"); MakeFile(other, "other"); g_currentFile = other;
    Refusal("changed-row-selection", s, FA::Delete, FR::SelectionChanged);
    g_currentFile = active;
    Require(DeleteFileA(active.path.c_str()) != 0, "remove-for-stale-identity", ""); MakeFile(active, "replaced");
    Refusal("same-bytes-new-file-identity", s, FA::Delete, FR::StaleTarget);
    s = Select(active);
    Require(SetFileAttributesA(active.path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "readonly-fixture", "");
    Refusal("os-delete-refusal", s, FA::Delete, FR::DeleteFailed);
    Require(SetFileAttributesA(active.path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "readonly-cleared", "");
    auto malformed = active; malformed.filename = "..\\escape.cdproj";
    core::FileSelectionHandle rejected;
    Check("path-traversal-rejected", core::SelectSavedFile(malformed, rejected).reason == FR::InvalidName && !rejected, "");
    malformed = active; malformed.path = other.path;
    Check("path-name-mismatch-rejected", core::SelectSavedFile(malformed, rejected).reason == FR::StaleTarget, "");
    Require(Act(s, FA::Archive).ok(), "failure-recovery-archive", "");
    s = Select(archived); core::g_fileMutationFault = core::FileMutationFault::Move;
    Refusal("restore-move-refusal", s, FA::Restore, FR::MoveFailed);
    core::g_fileMutationFault = core::FileMutationFault::Delete;
    Refusal("purge-boundary-refusal", s, FA::Purge, FR::DeleteFailed);
}
static void CaseFileReferences() {
    BeginCase("file-project-reference-matrix");
    const auto file = Entry(FK::Project, false, "t12-refs.cdproj");
    MakeFile(file, Row("/object/refs.prefab", 0, 0, 0)); auto selected = Select(file);
    const int pid = core::ProjectId("T12-REFS");
    auto allActions = [&](const char* name, FR reason) {
        Refusal((std::string(name) + "-archive").c_str(), selected, FA::Archive, reason);
        Refusal((std::string(name) + "-delete").c_str(), selected, FA::Delete, reason);
        const auto archive = Entry(FK::Project, true, file.filename); MakeFile(archive, "archived original");
        const auto a = Select(archive); Refusal((std::string(name) + "-purge").c_str(), a, FA::Purge, reason);
        Require(DeleteFileA(archive.path.c_str()) != 0, "remove-owned-archive", ""); g_currentFile = file;
    };
    int uid = SpawnCore("/object/refs.prefab", {}, {}, 1, 0, pid);
    allActions("pending-record", FR::PendingOperation); PumpAll();
    allActions("visible-record", FR::VisibleReference);
    core::HideUid(uid); PumpAll(); allActions("hidden-record", FR::HiddenReference);
    core::ForgetUid(uid); allActions("dirty-only", FR::DirtyProject);
    Require(core::SaveProject("t12-refs", core::SaveProjectOnly), "clear-dirty-by-save", ""); selected = Select(file);
    uid = Spawn("/object/refs.prefab", {}, {}, 1, 0, pid); PumpAll();
    allActions("undo-reference", FR::UndoReference);
    Require(editor::host_seam::UndoOne(), "history-to-redo", ""); PumpAll();
    allActions("redo-reference", FR::RedoReference);
    // Spawn History intentionally keeps proj=0 (Task 3); after Forget it is a no-op, not a project reference.
    // A REAL Delete act captures proj, so retain that snapshot in Redo before forgetting its logical record.
    Require(editor::host_seam::RedoOne(), "restore-for-delete-history", ""); PumpAll();
    editor::host_seam::ResetHistory(); DeleteViaEditor(uid);
    Require(editor::host_seam::UndoOne(), "delete-act-in-redo", ""); PumpAll();
    editor::host_seam::ActView act;
    Require(editor::host_seam::HistoryView(true, 0, 0, &act) && act.proj == pid, "redo-retains-project-snapshot", "");
    core::ForgetUid(uid); allActions("redo-after-forget", FR::RedoReference);
    editor::host_seam::ResetHistory();
    Require(core::SaveProject("t12-refs", core::SaveProjectOnly), "clear-after-history", ""); selected = Select(file);
    Require(core::SetAutoload("T12-REFS.cdproj", true).ok(), "enable-ref", "");
    const std::string enabled = FileText(AutoloadPath()); allActions("persisted-autoload", FR::AutoloadEnabled);
    Check("refusal-never-silently-disables", FileText(AutoloadPath()) == enabled, "");
    Require(core::SetAutoload("t12-refs", false).ok(), "explicit-autoload-off", "");
    Check("last-autoload-deleted-and-reopened", !FileExists(AutoloadPath()) && core::Autoload().empty(), "");
    const auto archivedResult = Act(selected, FA::Archive);
    Require(archivedResult.ok(), "unreferenced-project-archive", std::string(core::FileReasonCode(archivedResult.reason)) + ":" + std::to_string(archivedResult.systemError));
}
static void CaseUnicodeProjectAlias() {
    BeginCase("windows-unicode-project-alias");
    auto ansi = [](const wchar_t* text) { const int n = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr); std::string s((size_t)n, '\0'); WideCharToMultiByte(CP_ACP, 0, text, -1, &s[0], n, nullptr, nullptr); s.pop_back(); return s; };
    const std::string upper = ansi(L"t12-\u00c4"), lower = ansi(L"t12-\u00e4");
    const int emptyId = core::ProjectId(upper), referencedId = core::ProjectId(lower);
    Require(emptyId != referencedId && upper != lower, "legacy-ids-distinct-for-windows-alias", "");
    const auto file = Entry(FK::Project, false, upper + ".cdproj"); MakeFile(file, "unicode original");
    auto selected = Select(file);
    const int uid = SpawnCore("/object/unicode.prefab", {}, {}, 1, 0, referencedId); PumpAll();
    core::HideUid(uid); PumpAll();
    Refusal("windows-alias-hidden-reference", selected, FA::Archive, FR::HiddenReference);
    Refusal("windows-alias-hidden-delete", selected, FA::Delete, FR::HiddenReference);
    core::ForgetUid(uid);
    Refusal("windows-alias-dirty-reference", selected, FA::Delete, FR::DirtyProject);
}
static void CaseFileInflight() {
    BeginCase("file-inflight-save-export-place");
    const auto project = Entry(FK::Project, false, "t12-saving.cdproj");
    MakeFile(project, "original"); const auto p = Select(project);
    host::SetBeforeReplace([&] { Refusal("inflight-save", p, FA::Delete, FR::PendingOperation); });
    Require(core::SaveProject("t12-saving", core::SaveNewOnly), "save-completes", "");
    core::PrefabInfo prefab{}; prefab.path = "/object/g1.prefab"; host::SetPrefabIndex({ prefab });
    const int uid = SpawnCore(prefab.path, {}); PumpAll();
    const auto group = Entry(FK::Group, false, "t12-group.cdgroup");
    MakeFile(group, "old export bytes"); const auto g = Select(group);
    core::PublishExportContext({ {uid}, "t12-group", false, {} });
    core::GroupExportApproval approval;
    Require(core::PrepareGroupExport({ uid }, "t12-group", true, approval), "export-prepared", approval.error);
    host::SetBeforeReplace([&] { Refusal("inflight-export", g, FA::Archive, FR::InFlightExport); });
    std::string error;
    Require(core::WriteGroupExport(approval, error), "export-completes", error);
    const auto placedFile = Select(group);
    std::vector<int> uids; Vec3 pivot{};
    auto report = core::AdmitGroupFileCopy(placedFile, { 2, 0, 2 }, 0, 1, uids, pivot);
    Require(report.valid && report.request, "file-place-admitted", report.error);
    Refusal("inflight-place-before-pump", placedFile, FA::Archive, FR::InFlightPlace);
    PumpAll();
    Check("place-actual-attachment", core::PlaceRequestState(report.request).attached == 1, "");
    core::PlaceRequestCancel(report.request);
    Refusal("inflight-place-cleanup", placedFile, FA::Delete, FR::InFlightPlace);
    PumpAll(); Check("place-cleanup-settled", core::PlaceRequestState(report.request).settled, "");
    // Editor carrying is distinct from core finality. This invokes the REAL Place workflow and authority.
    proj_codec::Document doc;
    Require(proj_codec::Parse(FileText(group.path), group.path, FK::Group, doc, error), "group-parse", error);
    Require(editor::PlaceGroupCopy(doc, { 3, 0, 3 }, 0, 1, "t12-group"), "editor-carried-group", ""); PumpAll();
    Refusal("carried-after-settlement", placedFile, FA::Archive, FR::InFlightPlace);
    editor::DropCarried(); PumpAll();
    const auto world = CaptureWorld();
    Require(Act(placedFile, FA::Archive).ok(), "group-copy-survives-archive", "");
    CheckWorldUnchanged("group-archive-keeps-scene-and-history", world);
    const auto a = Select(Entry(FK::Group, true, group.filename));
    Require(Act(a, FA::Purge, group.filename).ok(), "group-copy-survives-purge", "");
    CheckWorldUnchanged("group-purge-keeps-scene-and-history", world);
}

static void CaseAutoloadAtomic() {
    BeginCase("autoload-atomic-refusal");
    Require(WriteTextFile(ProjPath("t12-off"), Row("/object/a.prefab", 0, 0, 0)), "off-fixture", "");
    const std::string original = "# retained comment\nt12-off\nother\n";
    Require(WriteTextFile(AutoloadPath(), original), "off-list-fixture", "");
    host::SetSaveFault(host::SaveFault::ReplaceAbort);
    core::SetAutoload("t12-off", false);
    Check("autoload-failed-replace-preserves-original", FileText(AutoloadPath()) == original, "");
    host::SetSaveFault(host::SaveFault::None);
    for (auto fault : { host::SaveFault::WriteAbort, host::SaveFault::ShortWrite, host::SaveFault::FlushAbort, host::SaveFault::CloseAbort, host::SaveFault::ReplaceAbort }) {
        host::SetSaveFault(fault);
        const auto r = core::SetAutoload("T12-OFF.cdproj", false);
        Check("autoload-fault-reported", !r.ok(), core::FileReasonCode(r.reason));
        Check("autoload-fault-original-exact", FileText(AutoloadPath()) == original, "");
        Check("autoload-fault-reopened-list", core::Autoload() == std::vector<std::string>{ "t12-off", "other" }, "");
    }
    const std::string aliases = "# retained comment\r\nt12-off\r\nOther.cdproj\r\nT12-OFF.CDPROJ\r\n# tail\r\nThird\r\n";
    Require(WriteTextFile(AutoloadPath(), aliases), "aliases-fixture", "");
    Require(core::SetAutoload("t12-off", false).ok(), "autoload-off-persisted", "");
    Check("off-removes-only-chosen-aliases", FileText(AutoloadPath()) == "# retained comment\r\nOther.cdproj\r\n# tail\r\nThird\r\n", "");
    Check("off-reopened-ordered-list", core::Autoload() == std::vector<std::string>{ "Other", "Third" }, "");
    g_fileProofs.push_back("{\"filename\":\"autoload.txt\",\"beforeSha256\":" + J(Sha256Bytes(aliases)) + ",\"afterSha256\":" + J(Sha256File(AutoloadPath())) + ",\"reopened\":[\"Other\",\"Third\"]}");
    Check("autoload-enable-missing-rejected", core::SetAutoload("not-on-disk", true).reason == FR::NotFound, "");
    Check("autoload-enable-group-rejected", core::SetAutoload("name.cdgroup", true).reason == FR::InvalidName, "");
    const auto file = Entry(FK::Project, false, "t12-off.cdproj"); const auto selection = Select(file);
    HANDLE lock = CreateFileA(AutoloadPath().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "autoload-locked", "");
    Refusal("unreadable-autoload-not-off", selection, FA::Archive, FR::AutoloadReadFailed);
    Check("unreadable-autoload-off-refused", core::SetAutoload("Other", false).reason == FR::AutoloadReadFailed, "");
    Require(CloseHandle(lock) != 0, "autoload-unlocked", "");
    host::SetBeforeReplace([&] { Require(WriteTextFile(AutoloadPath(), "external-edit\n"), "concurrent-list-edit", ""); });
    Check("autoload-stale-write-refused", core::SetAutoload("Other", false).reason == FR::StaleTarget, "");
    Check("autoload-keeps-newer-external-bytes", FileText(AutoloadPath()) == "external-edit\n", "");
    Require(SetFileAttributesA(AutoloadPath().c_str(), FILE_ATTRIBUTE_READONLY) != 0, "last-autoload-readonly", "");
    Check("last-autoload-off-os-refusal", core::SetAutoload("external-edit", false).reason == FR::WriteFailed, "");
    Check("last-autoload-failure-original", FileText(AutoloadPath()) == "external-edit\n" && core::Autoload() == std::vector<std::string>{ "external-edit" }, "");
    Require(SetFileAttributesA(AutoloadPath().c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "last-autoload-readonly-cleared", "");
    WIN32_FIND_DATAA fd{}; HANDLE tmp = FindFirstFileA((g_fixtureDir + "\\wba*.tmp").c_str(), &fd);
    Check("autoload-no-owned-temp-left", tmp == INVALID_HANDLE_VALUE, ""); if (tmp != INVALID_HANDLE_VALUE) FindClose(tmp);
}

// B2 repair regression: the Windows library (ValidFileName via FileNameEqual) and ImportProjectFile (_stricmp)
// accept .CDPROJ/.CDGROUP case-insensitively, so the codec's Parse/WriteTransactional extension-kind gates must
// accept the same bytes at the same paths. Real valid bytes, exact on-disk casing identity, no path rewriting.
static std::string ExactDirEntry(const std::string& dir, const std::string& name) {
    // Case-sensitive compare against the actual directory entry: proves the on-disk casing.
    WIN32_FIND_DATAA fd{}; HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    std::string found;
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name == fd.cFileName) found = fd.cFileName; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return found;
}
static void CaseMixedCaseExtensions() {
    BeginCase("mixed-case-extension-lifecycle");
    const std::string projectBytes = OneEnvelopeDoc({ Row("/object/house.prefab", 3, 0, 3) });
    const std::string groupBytes = "# cdproj v3 kind=group\n# wb-document " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "measured") +
                                   "# wb-envelope id=1 " + BoundsTail("0,0,0", "-1,-1,-1", "1,1,1", "measured") +
                                   "# wb-member record=1 envelope=1\n" + Row("/object/kit.prefab", 0, 0, 0);
    const std::string source = OutsidePath("House.CDPROJ"), plainSource = OutsidePath("plain.cdproj");
    Require(WriteTextFile(source, projectBytes) &&
            WriteTextFile(plainSource, OneEnvelopeDoc({ Row("/object/plain.prefab", 1, 0, 1) })), "mixedcase-sources-written", "");

    // Wrong extensions stay rejected: the gate becomes case-insensitive, never kind-blind.
    proj_codec::Document wrong; std::string werr;
    Check("mixedcase-wrong-extension-rejected",
          !proj_codec::Parse(projectBytes, OutsidePath("House.cdproj.bak"), proj_codec::Kind::Project, wrong, werr) &&
          werr == "record 0: wrong extension for kind", werr);
    Check("mixedcase-wrong-kind-extension-rejected",
          !proj_codec::WriteTransactional(ProjectDir() + "\\wrong-kind.cdproj", proj_codec::Kind::Group, groupBytes, nullptr, werr) &&
          werr == "record 0: wrong extension for kind" && !FileExists(ProjectDir() + "\\wrong-kind.cdproj"), werr);

    // Import a real House.CDPROJ: traverses Parse at the mixed-case source path and WriteTransactional at the
    // mixed-case destination inside the projects directory. The call is evaluated before ProjError() so the
    // observed diagnostic is deterministic (C++ argument evaluation order is otherwise unspecified).
    const bool importedHouse = core::ImportProjectFile(source);
    Require(importedHouse, "mixedcase-import-house-cdproj", "error=" + ProjError());
    const std::string importedPath = ProjectDir() + "\\House.CDPROJ";
    const std::string houseOnDisk = ExactDirEntry(ProjectDir(), "House.CDPROJ");
    Check("mixedcase-import-keeps-exact-casing", houseOnDisk == "House.CDPROJ", houseOnDisk);
    Check("mixedcase-import-bytes-identical", FileText(importedPath) == projectBytes, Sha256File(importedPath));
    std::vector<core::SavedFile> listed;
    Require(core::ListSavedFiles(proj_codec::Kind::Project, false, listed).ok(), "mixedcase-list-projects-ok", "");
    Check("mixedcase-import-listed-exact-identity",
          std::any_of(listed.begin(), listed.end(), [&](const core::SavedFile& f) { return f.filename == "House.CDPROJ" && f.path == importedPath; }), "");

    // Lower-case behavior is unchanged: same flow, lowercase name, lowercase identity on disk.
    const bool importedPlain = core::ImportProjectFile(plainSource);
    Require(importedPlain, "mixedcase-lowercase-import-unchanged", "error=" + ProjError());
    const std::string plainOnDisk = ExactDirEntry(ProjectDir(), "plain.cdproj");
    Check("mixedcase-lowercase-keeps-exact-casing", plainOnDisk == "plain.cdproj", plainOnDisk);

    // Direct codec write at a mixed-case destination pins WriteTransactional's own gate independently of import.
    const std::string directPath = ProjectDir() + "\\Writer.CDPROJ";
    Check("mixedcase-write-transactional-mixed-case",
          proj_codec::WriteTransactional(directPath, proj_codec::Kind::Project, projectBytes, nullptr, werr) && FileText(directPath) == projectBytes, werr);

    // The real editor Read consumer on the imported mixed-case project.
    const editor::ProjectFile imported{ "House", proj_codec::Kind::Project, false, importedPath };
    const bool readProject = editor::DispatchProjectAction(imported, editor::ProjectAction::Read);
    Require(readProject, "mixedcase-editor-read-project", editor::g_projectStatus);
    Check("mixedcase-editor-read-exact",
          editor::g_projectReadValid && editor::g_projectReadPath == importedPath && editor::g_projectRead.records.size() == 1 &&
          editor::g_projectRead.records[0].prefab == "/object/house.prefab", editor::g_projectReadPath);

    // A valid Kit.CDGROUP: listed exactly, read by the editor, and placed through the real editor workflow
    // (DispatchProjectAction Place -> ReadProjectFile -> PlaceGroupCopy -> AdmitGroupFileCopy Parse at the path).
    const auto kit = Entry(proj_codec::Kind::Group, false, "Kit.CDGROUP");
    MakeFile(kit, groupBytes);
    std::vector<core::SavedFile> groups;
    Require(core::ListSavedFiles(proj_codec::Kind::Group, false, groups).ok(), "mixedcase-list-groups-ok", "");
    Check("mixedcase-group-listed-exact-identity",
          std::any_of(groups.begin(), groups.end(), [&](const core::SavedFile& f) { return f.filename == "Kit.CDGROUP" && f.path == kit.path; }), "");
    const editor::ProjectFile kitFile{ "Kit", proj_codec::Kind::Group, false, kit.path };
    const bool readGroup = editor::DispatchProjectAction(kitFile, editor::ProjectAction::Read);
    Require(readGroup, "mixedcase-editor-read-group", editor::g_projectStatus);
    Check("mixedcase-editor-read-group-exact",
          editor::g_projectReadValid && editor::g_projectReadPath == kit.path && editor::g_projectRead.kind == proj_codec::Kind::Group, editor::g_projectReadPath);
    core::PrefabInfo prefab{}; prefab.path = "/object/kit.prefab"; host::SetPrefabIndex({ prefab });
    const bool placedGroup = editor::DispatchProjectAction(kitFile, editor::ProjectAction::Place);
    Require(placedGroup, "mixedcase-editor-place-group", editor::g_projectStatus);
    Check("mixedcase-place-carried", editor::g_place.active && editor::g_place.req != nullptr, editor::g_projectStatus);
    const auto request = editor::g_place.req;
    PumpAll();
    Check("mixedcase-place-attached", core::PlaceRequestState(request).attached == 1, "");
    editor::CancelProjectPlacement(request);
    PumpAll();
    Check("mixedcase-place-settled", core::PlaceRequestState(request).settled, "");

    Require(DeleteFileA(source.c_str()) != 0 && DeleteFileA(plainSource.c_str()) != 0, "mixedcase-sources-cleaned", "");
}

// Task11 core-only library surface: the same production API later consumed by the full/Dock table.
// Opaque file bytes deliberately are NOT valid documents; browsing must not parse or rewrite them.
using LK = core::SavedLibraryKind;
using LL = core::SavedLibraryLocation;
static std::string LibraryStem(int i) {
    char text[32]; std::snprintf(text, sizeof text, i % 2 ? "lib-%04d" : "LIB-%04d", i); return text;
}
static std::vector<std::string> LibraryPaths(const core::SavedLibrarySnapshot& s, const std::vector<size_t>& rows) {
    std::vector<std::string> paths;
    for (size_t i : rows) { Require(i < s.entries.size(), "library-filter-index-valid"); paths.push_back(s.entries[i].file.path); }
    return paths;
}
static std::string LibraryValue(const core::SavedLibrarySnapshot& s) {
    std::string value = std::to_string(s.active.projects) + ":" + std::to_string(s.active.groups) + ":" +
        std::to_string(s.archived.projects) + ":" + std::to_string(s.archived.groups);
    auto counts = [&](const core::ProjectOwnership& c) { value += ":" + std::to_string(c.visible) + ":" + std::to_string(c.hidden) + ":" + std::to_string(c.dirty); };
    counts(s.unassigned);
    for (const auto& entry : s.entries) {
        value += "\n" + std::to_string((int)entry.file.kind) + ":" + std::to_string(entry.file.archived) + ":" + entry.file.filename + ":" + entry.file.path;
        counts(entry.ownership);
    }
    return value;
}
static bool OwnershipIs(const core::ProjectOwnership& counts, size_t visible, size_t hidden, bool dirty) {
    return counts.visible == visible && counts.hidden == hidden && counts.dirty == dirty;
}
static std::string LibraryDiskStamp(const std::vector<std::string>& paths) {
    // Hash every actual byte, exact requested name, length, attributes and last-write time before/after.
    // No access-time assertion: Windows is allowed to update it on a read.
    std::string stamp;
    for (const auto& path : paths) {
        WIN32_FILE_ATTRIBUTE_DATA data{}; std::string bytes;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) || !ReadFileText(path, bytes))
            Require(false, "library-byte-read", path);
        stamp += path + ":" + Sha256Bytes(bytes) + ":" + std::to_string(bytes.size()) + ":" +
            std::to_string(data.dwFileAttributes) + ":" + std::to_string(data.ftLastWriteTime.dwHighDateTime) + ":" +
            std::to_string(data.ftLastWriteTime.dwLowDateTime) + "\n";
    }
    return Sha256Bytes(stamp);
}
static void CaseLibraryScale() {
    BeginCase("library-scale-snapshot");
    const int perLocation = 1024;
    std::vector<std::string> diskPaths;
    for (bool archived : { false, true }) for (FK kind : { FK::Project, FK::Group }) {
        const auto dir = std::filesystem::path(Entry(kind, archived, "unused").path).parent_path();
        std::filesystem::create_directories(dir);
        // Reverse creation order, mixed case and exact ties across active/archive. The expected order below
        // comes from fixture names, not a second copy of the production comparator.
        for (int i = perLocation - 1; i >= 0; --i) {
            const auto file = Entry(kind, archived, LibraryStem(i) + (kind == FK::Project ? ".cdproj" : ".cdgroup"));
            const std::string bytes = std::string("opaque\0\xff\r\n", 10) + std::to_string(i);
            if (!WriteTextFile(file.path, bytes)) Require(false, "library-scale-write", file.path);
            diskPaths.push_back(file.path);
        }
    }
    auto ansi = [](const wchar_t* text) { const int n = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr); std::string s((size_t)n, '\0'); WideCharToMultiByte(CP_ACP, 0, text, -1, &s[0], n, nullptr, nullptr); s.pop_back(); return s; };
    const std::string upper = ansi(L"Alias-\u00c4"), lower = ansi(L"Alias-\u00e4");
    for (bool archived : { false, true }) {
        for (FK kind : { FK::Project, FK::Group }) {
            const auto file = Entry(kind, archived, std::string(archived ? "tie" : "Tie") + (kind == FK::Project ? ".cdproj" : ".cdgroup"));
            MakeFile(file, "case tie"); diskPaths.push_back(file.path);
        }
        const auto file = Entry(FK::Project, archived, upper + ".CDPROJ");
        MakeFile(file, "unicode alias"); diskPaths.push_back(file.path);
    }
    for (const char* name : { ".cdproj", "wrong-kind.cdgroup", "trailing.cdproj.bak" }) {
        const auto file = Entry(FK::Project, false, name); MakeFile(file, "ignored original"); diskPaths.push_back(file.path);
    }
    std::filesystem::create_directory(Entry(FK::Project, false, "directory.cdproj").path);
    const auto readonly = Entry(FK::Group, true, "LIB-0000.cdgroup");
    Require(SetFileAttributesA(readonly.path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "library-readonly-file");
    const auto originalStamp = LibraryDiskStamp(diskPaths);

    const int p0 = core::ProjectId("lib-0000"), p1 = core::ProjectId("LIB-0001"), dirtyOnly = core::ProjectId("LIB-0002");
    const int emptyAlias = core::ProjectId(upper), usedAlias = core::ProjectId(lower);
    Require(emptyAlias != usedAlias && core::FileNameEqual(upper, lower), "library-alias-distinct-legacy-ids");
    Spawn("/object/library.prefab", {}, {}, 1, 0, p0); // real History must also survive browsing
    const int hidden0 = SpawnCore("/object/library.prefab", {}, {}, 1, 0, p0);
    const int hidden1 = SpawnCore("/object/library.prefab", {}, {}, 1, 0, p1);
    const int hidden2 = SpawnCore("/object/library.prefab", {}, {}, 1, 0, p1);
    SpawnCore("/object/library.prefab", {}, {}, 1, 0, usedAlias);
    const int hiddenAlias = SpawnCore("/object/library.prefab", {}, {}, 1, 0, usedAlias);
    SpawnCore("/object/library.prefab", {});
    const int hiddenNew = SpawnCore("/object/library.prefab", {});
    SpawnCore("/object/library.prefab", {}, {}, 1, 0, core::ProjectId("library-no-saved-file"));
    const int forgotten = SpawnCore("/object/library.prefab", {}, {}, 1, 0, dirtyOnly);
    PumpAll();
    for (int uid : { hidden0, hidden1, hidden2, hiddenAlias, hiddenNew, forgotten }) core::HideUid(uid);
    PumpAll(); core::ForgetUid(forgotten);
    const int pending = SpawnCore("/object/library.prefab", {}, {}, 1, 0, p0); // intentionally NOT pumped
    const auto world = CaptureWorld(); const auto records = host::RegistrySize();
    Require(records == 10 && core::PendingSpawns() == 1, "library-real-scene-arranged");
    const int idMarker = core::ProjectId("library-id-marker-before");

    // Deny all content opens of a valid listed file: enumeration must still succeed without reading it.
    const auto lockedFile = Entry(FK::Project, false, "LIB-0000.cdproj");
    std::unique_ptr<void, decltype(&CloseHandle)> locked(CreateFileA(lockedFile.path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr), &CloseHandle);
    Require(locked.get() != INVALID_HANDLE_VALUE, "library-content-locked");
    core::g_sceneEnumerationStats = {};
    core::SavedLibrarySnapshot snapshot;
    const auto refreshed = core::RefreshSavedLibrary(snapshot);
    Require(refreshed.ok(), "library-refresh", core::FileReasonCode(refreshed.reason));
    const auto firstWork = core::g_sceneEnumerationStats;
    Check("library-one-registry-pass", firstWork.calls == 1 && firstWork.records == records,
          "calls=" + std::to_string(firstWork.calls) + " records=" + std::to_string(firstWork.records));
    Check("library-four-location-totals", snapshot.active.projects == 1026 && snapshot.archived.projects == 1026 &&
          snapshot.active.groups == 1025 && snapshot.archived.groups == 1025 && snapshot.entries.size() == 4102);
    std::vector<std::string> expected, actual;
    for (bool archived : { false, true }) expected.push_back(Entry(FK::Project, archived, upper + ".CDPROJ").path);
    for (int i = 0; i < perLocation; ++i) for (FK kind : { FK::Group, FK::Project }) for (bool archived : { false, true })
        expected.push_back(Entry(kind, archived, LibraryStem(i) + (kind == FK::Project ? ".cdproj" : ".cdgroup")).path);
    for (FK kind : { FK::Group, FK::Project }) for (bool archived : { false, true })
        expected.push_back(Entry(kind, archived, std::string(archived ? "tie" : "Tie") + (kind == FK::Project ? ".cdproj" : ".cdgroup")).path);
    bool countsMatch = true;
    for (const auto& row : snapshot.entries) {
        actual.push_back(row.file.path);
        if (row.file.kind == FK::Group) countsMatch &= OwnershipIs(row.ownership, 0, 0, false);
        else if (row.file.filename == "LIB-0000.cdproj") countsMatch &= OwnershipIs(row.ownership, 2, 1, true);
        else if (row.file.filename == "lib-0001.cdproj") countsMatch &= OwnershipIs(row.ownership, 0, 2, true);
        else if (row.file.filename == "LIB-0002.cdproj") countsMatch &= OwnershipIs(row.ownership, 0, 0, true);
        else if (row.file.filename == upper + ".CDPROJ") countsMatch &= OwnershipIs(row.ownership, 1, 1, true);
        else countsMatch &= OwnershipIs(row.ownership, 0, 0, false);
    }
    Check("library-exact-order-case-ties-and-no-malformed-rows", actual == expected);
    Check("library-ownership-includes-hidden-pending-dirty-and-all-aliases", countsMatch && OwnershipIs(snapshot.unassigned, 1, 1, false));
    core::g_sceneEnumerationStats = {};
    for (LK kind : { LK::All, LK::Projects, LK::Groups }) for (LL location : { LL::Active, LL::Archived, LL::All }) {
        const size_t one = kind == LK::All ? 2051 : kind == LK::Projects ? 1026 : 1025;
        Check("library-empty-search-kind-location-totals", core::FilterSavedLibrary(snapshot, kind, location, "").size() == one * (location == LL::All ? 2 : 1));
    }
    Check("library-unmatched-search-empty", core::FilterSavedLibrary(snapshot, LK::All, LL::All, "not-in-library").empty());
    Check("library-long-search-empty", core::FilterSavedLibrary(snapshot, LK::All, LL::All, std::string(256, 'x')).empty());
    Check("library-extension-search", core::FilterSavedLibrary(snapshot, LK::All, LL::All, ".CDPROJ").size() == 2052);
    Check("library-unicode-case-search", core::FilterSavedLibrary(snapshot, LK::All, LL::All, lower).size() == 2);
    std::vector<std::string> searchExpected;
    for (int i = 0; i < 10; ++i) searchExpected.push_back(Entry(FK::Group, true, LibraryStem(i) + ".cdgroup").path);
    Check("library-search-filter-exact-identities", LibraryPaths(snapshot, core::FilterSavedLibrary(snapshot, LK::Groups, LL::Archived, "LiB-000")) == searchExpected);
    const auto filterWork = core::g_sceneEnumerationStats;
    Check("library-filter-never-enumerates-scene", filterWork.calls == 0 && filterWork.records == 0);
    locked.reset();
    Check("library-first-refresh-and-filter-byte-safe", LibraryDiskStamp(diskPaths) == originalStamp);
    CheckWorldUnchanged("library-browse-preserves-scene-history-queue", world);
    Check("library-refresh-does-not-allocate-project-ids", core::ProjectId("library-id-marker-after") == idMarker + 1);

    const auto selectedFile = Entry(FK::Project, false, "Tie.cdproj");
    const auto selection = Select(selectedFile);
    const auto originalValue = LibraryValue(snapshot);
    const auto added = Entry(FK::Project, false, "000-first.cdproj"); MakeFile(added, "new independent file"); diskPaths.push_back(added.path);
    core::AssignProject(pending, p1); // a new refresh must see new ownership; old caller-owned snapshots stay stable
    SpawnCore("/object/library.prefab", {}, {}, 1, 0, emptyAlias); // both Windows aliases now own records
    const auto changedWorld = CaptureWorld();
    const auto secondBeforeStamp = LibraryDiskStamp(diskPaths);
    auto next = snapshot;
    core::g_sceneEnumerationStats = {};
    Require(core::RefreshSavedLibrary(next).ok(), "library-second-refresh");
    const auto secondWork = core::g_sceneEnumerationStats;
    Check("library-second-one-registry-pass", secondWork.calls == 1 && secondWork.records == 11);
    Check("library-no-hard-cap-and-new-row-sorts-first", next.entries.size() == 4103 && next.active.projects == 1027 && next.entries.front().file.path == added.path);
    Check("library-old-snapshot-immutable", LibraryValue(snapshot) == originalValue);
    const auto chosenRows = core::FilterSavedLibrary(next, LK::Projects, LL::Active, "Tie.cdproj");
    Require(chosenRows.size() == 1, "library-selected-row-still-unique");
    Check("library-selection-retains-exact-identity-after-index-shift", next.entries[chosenRows[0]].file.path == core::SelectedFile(selection).path &&
          next.entries[chosenRows[0]].file.filename == core::SelectedFile(selection).filename);
    bool changedCounts = true;
    for (const auto& row : next.entries) if (row.file.kind == FK::Project) {
        if (row.file.filename == "LIB-0000.cdproj") changedCounts &= OwnershipIs(row.ownership, 1, 1, true);
        if (row.file.filename == "lib-0001.cdproj") changedCounts &= OwnershipIs(row.ownership, 1, 2, true);
        if (row.file.filename == upper + ".CDPROJ") changedCounts &= OwnershipIs(row.ownership, 2, 1, true);
    }
    Check("library-refresh-recounts-adoption-and-merges-both-aliases", changedCounts);
    const auto finalStamp = LibraryDiskStamp(diskPaths);
    Check("library-second-refresh-byte-and-mtime-safe", finalStamp == secondBeforeStamp);
    CheckWorldUnchanged("library-second-refresh-world-unchanged", changedWorld);
    size_t actualFiles = 0;
    for (const auto& root : { ProjectDir(), g_fixtureDir + "\\Groups" })
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (entry.is_regular_file()) ++actualFiles;
    Check("library-no-files-created-by-browsing", actualFiles == diskPaths.size());
    Require(WriteTextFile(g_fixtureDir + "\\library-scale.json", "{\"activeProjects\":" + std::to_string(next.active.projects) +
        ",\"activeGroups\":" + std::to_string(next.active.groups) + ",\"archivedProjects\":" + std::to_string(next.archived.projects) +
        ",\"archivedGroups\":" + std::to_string(next.archived.groups) + ",\"rows\":" + std::to_string(next.entries.size()) +
        ",\"filesByteChecked\":" + std::to_string(diskPaths.size()) + ",\"firstScenePasses\":" + std::to_string(firstWork.calls) +
        ",\"firstRecordsVisited\":" + std::to_string(firstWork.records) + ",\"secondScenePasses\":" + std::to_string(secondWork.calls) +
        ",\"secondRecordsVisited\":" + std::to_string(secondWork.records) + ",\"filterScenePasses\":" + std::to_string(filterWork.calls) +
        ",\"beforeSha256\":" + J(secondBeforeStamp) + ",\"afterSha256\":" + J(finalStamp) + "}"), "library-scale-receipt");
    Require(SetFileAttributesA(readonly.path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "library-readonly-cleanup");
}
static void CaseLibraryEmptyAndFailure() {
    BeginCase("library-empty-and-refresh-failure");
    const auto file = Entry(FK::Project, false, "Keep.cdproj"); MakeFile(file, "unchanged");
    core::SavedLibrarySnapshot snapshot;
    Require(core::RefreshSavedLibrary(snapshot).ok() && snapshot.entries.size() == 1, "library-single-row");
    const auto before = LibraryValue(snapshot);
    Require(WriteTextFile(g_fixtureDir + "\\Groups", "not a directory"), "library-bad-location-arranged");
    core::g_sceneEnumerationStats = {};
    const auto result = core::RefreshSavedLibrary(snapshot);
    Check("library-path-failure-reported-not-empty-success", result.reason == FR::UnsafePath);
    Check("library-failed-refresh-retains-complete-snapshot", LibraryValue(snapshot) == before);
    Check("library-failed-refresh-does-not-scan-scene", core::g_sceneEnumerationStats.calls == 0);
    // The real filesystem is now invalid: this query can succeed only from the captured snapshot.
    Check("library-filter-has-no-filesystem-dependency", LibraryPaths(snapshot, core::FilterSavedLibrary(snapshot, LK::Projects, LL::Active, "KEEP")) == std::vector<std::string>{ file.path });
    core::FileSelectionHandle invalid;
    Check("library-malformed-filename-rejected", core::SelectSavedFile(Entry(FK::Project, false, ".cdproj"), invalid).reason == FR::InvalidName && !invalid);
    Check("library-traversal-filename-rejected", core::SelectSavedFile(Entry(FK::Project, false, "..\\Keep.cdproj"), invalid).reason == FR::InvalidName && !invalid);
    Require(DeleteFileA((g_fixtureDir + "\\Groups").c_str()) != 0, "library-remove-owned-bad-location");
    Check("library-failure-left-source-original", FileText(file.path) == "unchanged");
    Require(DeleteFileA(file.path.c_str()) != 0, "library-remove-owned-last-file");
    core::g_sceneEnumerationStats = {};
    Require(core::RefreshSavedLibrary(snapshot).ok(), "library-empty-refresh");
    Check("library-empty-clears-previous-rows-and-totals", snapshot.entries.empty() && snapshot.active.projects == 0 && snapshot.active.groups == 0 && snapshot.archived.projects == 0 && snapshot.archived.groups == 0);
    Check("library-empty-filter-is-empty", core::FilterSavedLibrary(snapshot, LK::All, LL::All, "").empty());
    Check("library-empty-refresh-one-zero-record-pass", core::g_sceneEnumerationStats.calls == 1 && core::g_sceneEnumerationStats.records == 0);
    Check("library-missing-directories-not-created", !FileExists(g_fixtureDir + "\\Groups") && !FileExists(ProjectDir() + "\\.archive"));
}

#include "terrain_project_cases.h"

// =========================================================================================================
// main
// =========================================================================================================
int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : ".";
    host::OpenLog(g_fixtureDir + "\\project_lifecycle.log");
    core::Log("[project_lifecycle] run start: fixtureDir=%s", g_fixtureDir.c_str());
    g_probe.Start();
    InstallSeam();
    host::SetModDir(g_fixtureDir);       // real files, inside the fixture directory only
    core::g_recreateOnMove = true;
    host::SetDirectGimmick(false);
    CreateDirectoryA(ProjectDir().c_str(), nullptr);

    struct Entry { const char* id; void (*fn)(); };
    const Entry entries[] = {
        { "roundtrip-metadata", CaseRoundtrip },
        { "adoption-save-undo-redo", CaseAdoption },
        { "save-scopes", CaseSaveScopes },
        { "legacy-defaults-and-missing", CaseLegacy },
        { "replace-last-row", CaseReplaceLastRow },
        { "narrowing-rejected", CaseNarrowing },
        { "write-fail", CaseWriteFail },
        { "group-autoload-prohibited", CaseGroupAutoload },
        { "post-admission-partial", CasePostAdmissionPartial },
        { "autoload-atomic-refusal", CaseAutoloadAtomic },
        { "mixed-case-extension-lifecycle", CaseMixedCaseExtensions },
        { "file-archive-restore-delete", CaseFileRoundtrip },
        { "file-failure-and-stale-target", CaseFileFailures },
        { "file-project-reference-matrix", CaseFileReferences },
        { "file-inflight-save-export-place", CaseFileInflight },
        { "windows-unicode-project-alias", CaseUnicodeProjectAlias },
        { "library-scale-snapshot", CaseLibraryScale },
        { "library-empty-and-refresh-failure", CaseLibraryEmptyAndFailure },
        { "main-terrain-project-save-scopes", CaseTerrainSaveScopes },
        { "main-terrain-only-file-guard", CaseTerrainFileGuard },
    };
    for (const Entry& e : entries) {
        try {
            e.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("CASE-ABORT %s: %s\n", e.id, ex.what());
            core::Log("[project_lifecycle] CASE-ABORT %s: %s", e.id, ex.what());
            if (!g_cases.empty() && g_cases.back().failures == 0) ++g_cases.back().failures;
        }
    }

    try {
        ResetWorld();
        Check("owned-fixtures-cleaned", std::filesystem::is_empty(ProjectDir()) && !FileExists(g_fixtureDir + "\\Groups") && !FileExists(AutoloadPath()), "");
        auto receipt = [](const std::vector<std::string>& rows) { std::string s = "["; for (size_t i = 0; i < rows.size(); ++i) { if (i) s += ','; s += rows[i]; } return s + "]"; };
        Require(WriteTextFile(g_fixtureDir + "\\file-hashes.json", receipt(g_fileProofs)), "hash-receipt-written", "");
        Require(WriteTextFile(g_fixtureDir + "\\refusal-matrix.json", receipt(g_refusalProofs)), "refusal-receipt-written", "");
    } catch (const std::exception& ex) { Check("fixture-cleanup-and-receipts", false, ex.what()); }
    {
        std::string json = "{\"suite\":\"ProjectLifecycle\",\"status\":\"" + std::string(g_failures == 0 ? "PASS" : "FAIL") +
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
        core::Log("[project_lifecycle] cases: %s", json.c_str());
    }
    std::printf("CASES=%zu\nASSERTIONS=%d\nFAILURES=%d\n", g_cases.size(), g_assertions, g_failures);
    core::Log("[project_lifecycle] run end: cases=%zu assertions=%d failures=%d lockViolations=%d", g_cases.size(), g_assertions, g_failures, host::LockViolations());
    g_probe.Stop();
    return g_failures == 0 ? 0 : 1;
}
