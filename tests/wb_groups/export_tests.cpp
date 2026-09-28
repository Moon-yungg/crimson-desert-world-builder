// Export host fixture (plan wb079-unified-77f68967, Task 12 / C4+C6 - protected export approval).
//
// The production core TU is compiled by production_core_host.cpp (WB_UNIFIED_HOST_TEST) and this fixture drives
// the REAL prepare/write path through it: the authoritative value builder (ValueDocumentLocked, the same one
// SaveProject uses), the published selection/project/placement context, the transactional writer
// (proj_codec::WriteTransactional) and the protected replacement that holds both authorities across the rename.
// The shipped codec (proj_codec.cpp) and geometry (wb_group_math.cpp) are linked as their own TUs; the only
// substituted boundaries are the native engine calls behind host::Seam(), the one-shot file faults
// (host::SetSaveFault), the deterministic pre-replace callback (host::SetBeforeReplace) and the observation
// probe inside the protected region (host::SetExportReplaceProbe). Nothing here re-implements the builder,
// the guard, the writer or the admission: the same production code paths run.
//
// Cases assert the Task 12 contract:
//   PREFLIGHT-WRITE      - preflight -> write -> the file holds exactly the approved values; registry/dirty untouched
//   ROUNDTRIP            - an admitted copy's records/envelopes survive prepare -> write -> parse -> re-export
//   LIVE-MOVE            - a final=false move after approval rejects at BOTH final boundaries; old bytes stay
//   SELECTION            - a changed published selection (and an unpublished selection) rejects
//   PROJECT-CONTEXT      - a changed destination/project name rejects at both boundaries
//   PLACEMENT-CONTEXT    - a changed placement (placing/carried) rejects at both boundaries
//   ENVELOPE             - a changed receiver box hint changes only the envelope; the stale approval rejects,
//                          the fresh one writes
//   EMPTY                - an empty selection is refused by preflight
//   ALL-EXCLUDED         - hidden + forgotten selections are named exclusions; nothing is written
//   NAN                  - a non-finite live value rejects at both boundaries
//   PATH-KIND            - invalid names, a kind tamper and a path tamper are refused; nothing escapes Groups\
//   OVERWRITE-APPROVAL   - an existing file needs explicit approval; a file appearing after preflight rejects
//   SHORT-WRITE/FLUSH/CLOSE/REPLACE - actual file faults fail the export, keep the old bytes, clean the temporary
//   PROTECTED-REPLACE    - the publication and the registry are BOTH held between the last comparison and the rename
//
// Machine output: CHECK lines on stdout, one ASSERTIONS=<n> line, a per-case cases.json receipt, per-case
// approval/current documents + old/new file hashes in export_trace.json and the production log sink at
// <fixtureDir>\export.log.
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
#include <limits>
#include <iterator>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "../../asi/cdmodkit/proj_codec.h"   // the shipped disk-value codec (linked as its own TU)
#include "production_host.h"                // host::Seam / host::PumpGame / host::PumpServer / file + export seams

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "bcrypt.lib")

// ---- seam oracle -----------------------------------------------------------------------------------------
namespace oracle {
struct Obj { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1; uintptr_t actor = 0; bool live = true; };

std::map<uintptr_t, Obj> objects;
std::vector<uintptr_t> createdOrder;
std::set<uintptr_t> disposed;
uintptr_t next = 0x10000;
int createCalls = 0, creates = 0, removes = 0, moveInPlaceCalls = 0, liveMoveCalls = 0;
bool lockFreeDuringEngineCalls = true;

void Reset() {
    objects.clear(); createdOrder.clear(); disposed.clear(); next = 0x10000;
    createCalls = creates = removes = moveInPlaceCalls = liveMoveCalls = 0;
    lockFreeDuringEngineCalls = true;
}
bool Live(uintptr_t h) { auto it = objects.find(h); return it != objects.end() && it->second.live; }
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
std::string g_trace = "[\n";
bool g_traceFirst = true;
struct CaseRec { std::string id; int assertions = 0, failures = 0; };
std::vector<CaseRec> g_cases;

void Check(const char* label, bool ok, const std::string& observed = "") {
    ++g_assertions;
    if (!g_cases.empty()) { ++g_cases.back().assertions; if (!ok) ++g_cases.back().failures; }
    if (!ok) ++g_failures;
    std::printf("CHECK %s %s%s%s\n", ok ? "PASS" : "FAIL", label, observed.empty() ? "" : " | ", observed.c_str());
    core::Log("[export] %s %s %s%s%s", g_case.c_str(), ok ? "PASS" : "FAIL", label,
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
    core::Log("[export] CASE %s", id);
}
void Trace(const std::string& fields) {
    if (!g_traceFirst) g_trace += ",\n";
    g_traceFirst = false;
    g_trace += " {" + std::string("\"case\":\"") + g_case + "\"," + fields + "}";
}
std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c); out += b; }
        else out += c;
    }
    return out + "\"";
}

// ---- engine seam -----------------------------------------------------------------------------------------
void InstallSeam() {
    host::Engine& e = host::Seam();
    e.ready = true;
    e.createGeneric = [](const std::string& prefab, Vec3 pos, Rot rot, float scale) -> uintptr_t {
        oracle::lockFreeDuringEngineCalls &= g_probe.CheckFree();
        ++oracle::createCalls; ++oracle::creates;
        const uintptr_t h = oracle::next++;
        oracle::objects.emplace(h, oracle::Obj{ prefab, pos, rot, scale, 0, true });
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

void PumpGame_() {
    int guard = 0;
    while (host::PumpGame()) { if (++guard > 4096) throw std::runtime_error("the production game queue did not settle"); }
}
void PumpAll() { PumpGame_(); host::PumpServer(); PumpGame_(); }

// ---- the receiver's prefab cache (inclusion preflight / box hints) ---------------------------------------
std::vector<core::PrefabInfo> PrefabRow(const std::string& path, float sx, float sy, float sz, float cx, float cy, float cz) {
    core::PrefabInfo p; p.path = path; p.hasCenter = true; p.sx = sx; p.sy = sy; p.sz = sz; p.cx = cx; p.cy = cy; p.cz = cz;
    return { p };
}
std::vector<core::PrefabInfo> PrefabIndexRows() {
    std::vector<core::PrefabInfo> idx = PrefabRow("/object/export_a.prefab", 1.5f, 2.0f, 1.0f, 0.1f, 1.0f, -0.2f);
    const auto b = PrefabRow("/object/export_b.prefab", 2.5f, 3.0f, 2.0f, 0.0f, 1.5f, 0.0f);
    idx.insert(idx.end(), b.begin(), b.end());
    return idx;
}
void InstallIndex() { host::SetPrefabIndex(PrefabIndexRows()); }

// ---- fixture directory (real files, inside the fixture directory only) -----------------------------------
std::string GroupDir() { return g_fixtureDir + "\\Groups"; }
std::string GroupPath(const std::string& name) { return GroupDir() + "\\" + name + ".cdgroup"; }
bool FileExists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool WriteTextFile(const std::string& path, const std::string& text) {
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
    if (!f) return false;
    const bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}
bool ReadFileText(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
    if (!f) return false;
    char b[4096]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}
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
    std::string bytes;
    if (!ReadFileText(path, bytes)) return std::string("<unreadable:").append(path).append(">");
    return Sha256Bytes(bytes);
}
int TempLeftovers() {
    int n = 0; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((GroupDir() + "\\wbv*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { ++n; } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
int GroupFileCount() {
    int n = 0; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((GroupDir() + "\\*.cdgroup").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { ++n; } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
void CleanFixtureDir() {
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((GroupDir() + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { DeleteFileA((GroupDir() + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CreateDirectoryA(GroupDir().c_str(), nullptr);
}
// A valid previous .cdgroup at the destination: every failure case asserts these bytes stay untouched.
std::string SentinelText(const char* marker) {
    std::string s = "# cdproj v3 kind=group\n";
    s += "# wb-document anchor=1,1,1 min=0,0,0 max=2,2,2 quality=measured\n";
    s += "# wb-envelope id=1 anchor=1,1,1 min=0,0,0 max=2,2,2 quality=measured\n";
    s += "# wb-member record=1 envelope=1\n";
    s += std::string("/object/export_old_") + marker + ".prefab|1|1|1|0|1|0|0|0\n";
    return s;
}
bool WriteSentinel(const std::string& name, const char* marker, std::string* hashOut = nullptr) {
    const bool ok = WriteTextFile(GroupPath(name), SentinelText(marker));
    if (ok && hashOut) *hashOut = Sha256File(GroupPath(name));
    return ok;
}

// ---- documents -------------------------------------------------------------------------------------------
bool ParseGroupText(const std::string& text, proj_codec::Document& out, std::string& error) {
    return proj_codec::Parse(text, "export_fixture.cdgroup", proj_codec::Kind::Group, out, error);
}
bool ParseGroupFile(const std::string& path, proj_codec::Document& out, std::string& error) {
    std::string bytes;
    if (!ReadFileText(path, bytes)) { error = "cannot read " + path; return false; }
    return ParseGroupText(bytes, out, error);
}
// Canonical serialization of a value document: the machine snapshot of an approval/rebuild, deliberately not a
// comparison (SameDocument in the production guard compares values; group ids are canonicalized on serialization).
std::string DocText(const proj_codec::Document& doc) {
    std::string text, error;
    if (!proj_codec::Serialize(doc, text, error)) return std::string("<invalid:") + error + ">";
    return text;
}
std::string DataRows(const std::string& text) {
    std::string out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        if (!line.empty() && line[0] != '#') out += line + "\n";
        p = e + 1;
    }
    return out;
}
std::string EnvelopeLines(const std::string& text) {
    std::string out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        if (line.rfind("# wb-envelope ", 0) == 0) out += line + "\n";
        p = e + 1;
    }
    return out;
}
// The admitted copy of the roundtrip case: two members in ONE saved envelope, saved anchor off the member average.
std::string RecipeText() {
    std::string s = "# cdproj v3 kind=group\n";
    s += "# wb-document anchor=10,1,20 min=0,1,10 max=20,21,30 quality=measured\n";
    s += "# wb-envelope id=1 anchor=10,1,20 min=0,1,10 max=20,21,30 quality=measured\n";
    s += "# wb-member record=1 envelope=1\n";
    s += "/object/export_a.prefab|12|4|25|30|1.5|42|11|-7\n";
    s += "# wb-member record=2 envelope=1\n";
    s += "/object/export_b.prefab|15|4|23|43|1.5|7|-11|7\n";
    return s;
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
    std::snprintf(b, sizeof b, "uid=%d obj=0x%llx hidden=%d proj=%d group=%d pos=(%.3f %.3f %.3f) grp=%d",
                  uid, (unsigned long long)o->obj, o->hidden ? 1 : 0, o->proj, o->group, o->pos.x, o->pos.y, o->pos.z, o->group);
    return b;
}
int Spawn(const std::string& prefab, Vec3 pos, int group = 0) {
    const int uid = core::SpawnAt(prefab, pos, Rot{}, 1.0f, group, 0);
    PumpAll();
    Snapshot();
    return uid;
}

// ---- the export surface under test -----------------------------------------------------------------------
void Publish(const std::vector<int>& selection, const std::string& name, bool placing = false, const std::vector<int>& carried = {}) {
    core::ExportContext ctx;
    ctx.selection = selection;
    ctx.name = name;
    ctx.placing = placing;
    ctx.carried = carried;
    core::PublishExportContext(ctx);
}
bool Prepare(const std::vector<int>& selection, const std::string& name, bool overwrite, core::GroupExportApproval& approval) {
    return core::PrepareGroupExport(selection, name, overwrite, approval);
}
bool Write(const core::GroupExportApproval& approval, std::string& error) { return core::WriteGroupExport(approval, error); }
bool Has(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }
std::string ReasonFor(const core::GroupExportApproval& approval, int uid) {
    for (const auto& x : approval.excluded) if (x.uid == uid) return x.reason;
    return std::string("<none>");
}

// The fixture publishes a fresh selection, prepares and writes; a failure returns false and records the error.
bool RoundtripWrite(const std::vector<int>& selection, const std::string& name, bool overwrite, std::string& error) {
    Publish(selection, name);
    core::GroupExportApproval approval;
    if (!Prepare(selection, name, overwrite, approval)) { error = approval.error; return false; }
    return Write(approval, error);
}

// =========================================================================================================
// cases
// =========================================================================================================

// happy: preflight -> write -> the file is exactly the approved document; registry, dirty stars and the
// publication stay untouched (an export is not a membership change)
static void CasePreflightWrite() {
    BeginCase("preflight-write");
    const std::string name = "t12-basic";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0 && Rec(a) && Rec(b), "fixture-spawned", UidStr(a) + " " + UidStr(b));
    const bool dirtyBefore = core::ProjectDirty(0);
    const int countBefore = core::ProjectObjectCount(0);
    const std::string registryBefore = UidStr(a) + " " + UidStr(b);

    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, false, approval), "preflight-valid", approval.error);
    Check("preflight-kind-group", approval.document.kind == proj_codec::Kind::Group, "");
    Check("preflight-included-uids", approval.included == std::vector<int>({ a, b }), "included=" + std::to_string(approval.included.size()));
    Check("preflight-excluded-none", approval.excluded.empty(), "excluded=" + std::to_string(approval.excluded.size()));
    Check("preflight-path", approval.path == GroupPath(name), approval.path);
    Check("preflight-no-file-yet", !FileExists(GroupPath(name)), "path=" + GroupPath(name));
    Check("preflight-envelope-per-record", approval.document.envelopes.size() == approval.document.records.size(), "envelopes=" + std::to_string(approval.document.envelopes.size()));

    std::string error;
    Require(Write(approval, error), "write-succeeded", error);
    Check("written-file-exists", FileExists(GroupPath(name)), GroupPath(name));
    proj_codec::Document parsed; std::string perr;
    Check("written-file-parses", ParseGroupFile(GroupPath(name), parsed, perr), perr);
    Check("written-file-is-approved-document", DocText(parsed) == DocText(approval.document), "");
    Check("written-file-records", parsed.records.size() == 2 && parsed.records[0].prefab == "/object/export_a.prefab", "records=" + std::to_string(parsed.records.size()));
    Check("no-temporary-left", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Check("registry-untouched", (UidStr(a) + " " + UidStr(b)) == registryBefore, UidStr(a) + " " + UidStr(b));
    Check("dirty-stars-untouched", core::ProjectDirty(0) == dirtyBefore && core::ProjectObjectCount(0) == countBefore, "dirty=" + std::to_string(core::ProjectDirty(0)));
    Check("publication-untouched", core::PublishedExportContext().name == name && core::PublishedExportContext().selection.size() == 2, "");
    Trace("\"boundary\":\"happy\",\"approval\":" + Quote(DocText(approval.document)) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"error\":" + Quote(error));
}

// happy: the written file is a real roundtrip - an admitted copy's records/envelopes come back out of the
// writer, and a re-export of the same live records reproduces the file
static void CaseRoundtrip() {
    BeginCase("roundtrip");
    const std::string name = "t12-round";
    proj_codec::Document recipe; std::string error;
    Require(ParseGroupText(RecipeText(), recipe, error), "fixture-recipe-parses", error);
    std::vector<int> uids; Vec3 pivot{};
    const core::GroupAdmissionReport rep = core::AdmitGroupCopy(recipe, { 100, 5, 200 }, 0.0, 1.0, uids, pivot);
    Require(rep.valid && uids.size() == 2, "fixture-copy-admitted", rep.error);
    PumpAll();
    Snapshot();

    Publish(uids, name);
    core::GroupExportApproval approval;
    Require(Prepare(uids, name, false, approval), "roundtrip-preflight", approval.error);
    std::string error2;
    Require(Write(approval, error2), "roundtrip-write", error2);
    const std::string firstFileHash = Sha256File(GroupPath(name));
    proj_codec::Document parsed; std::string perr;
    Require(ParseGroupFile(GroupPath(name), parsed, perr), "roundtrip-parses", perr);
    Check("roundtrip-kind", parsed.kind == proj_codec::Kind::Group && !parsed.legacy, "");
    Check("roundtrip-records", parsed.records.size() == 2, "records=" + std::to_string(parsed.records.size()));
    Check("roundtrip-envelope-count", parsed.envelopes.size() == 1, "envelopes=" + std::to_string(parsed.envelopes.size()));
    Check("roundtrip-canonical-equality", DocText(parsed) == DocText(approval.document), "");
    Check("roundtrip-saved-anchor-is-pivot", std::fabs(parsed.envelopes[0].bounds.anchor.x - pivot.x) < 1e-6 &&
          std::fabs(parsed.envelopes[0].bounds.anchor.y - pivot.y) < 1e-6 && std::fabs(parsed.envelopes[0].bounds.anchor.z - pivot.z) < 1e-6,
          "anchor=" + std::to_string(parsed.envelopes[0].bounds.anchor.x) + "," + std::to_string(parsed.envelopes[0].bounds.anchor.y));
    Check("roundtrip-member-offsets-kept", parsed.records.size() == 2 && std::fabs((parsed.records[0].pos.x - pivot.x) - (recipe.records[0].pos.x - recipe.bounds.anchor.x)) < 1e-6,
          "dx=" + std::to_string(parsed.records[0].pos.x - pivot.x));
    // a re-export of the same live records reproduces the file byte-for-byte (writer determinism from live state)
    core::GroupExportApproval again;
    Publish(uids, name);
    Require(Prepare(uids, name, true, again), "re-export-preflight", again.error);
    Check("re-export-document-equal", DocText(again.document) == DocText(approval.document), "");
    std::string reExportError;
    Require(Write(again, reExportError), "re-export-write", reExportError);
    Check("re-export-file-stable", Sha256File(GroupPath(name)) == firstFileHash, firstFileHash + " -> " + Sha256File(GroupPath(name)));
    proj_codec::Document reparsed; std::string rerr;
    Require(ParseGroupFile(GroupPath(name), reparsed, rerr), "re-export-parses", rerr);
    Check("re-export-text-equal", DocText(reparsed) == DocText(again.document), "");
    Trace("\"boundary\":\"roundtrip\",\"approval\":" + Quote(DocText(approval.document)) + ",\"pivot\":" + Quote(std::to_string(pivot.x) + "," + std::to_string(pivot.y) + "," + std::to_string(pivot.z)));
}

// failure: a live final=false move after approval rejects at BOTH final boundaries; the previous file stays
static void CaseLiveMove() {
    BeginCase("live-move");
    const std::string name = "t12-live";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", UidStr(a) + " " + UidStr(b));
    std::string oldHash;
    Require(WriteSentinel(name, "live", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight-before-move", approval.error);

    // boundary 1: between preflight and write
    Require(core::MoveMany({ { b, { 9, 2, 9 }, Rot{}, 1.0f } }, false), "fixture-live-move-accepted", UidStr(b));
    PumpAll();
    Snapshot();
    std::string error;
    Check("write-rejected-after-live-move", !Write(approval, error), error);
    Check("live-move-error-names-values", Has(error, "values or envelopes changed"), error);
    Check("live-move-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("live-move-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"error\":" + Quote(error));

    // boundary 2: inside the write, after the temporary was written and read back, before the replace
    Require(core::MoveMany({ { b, { 5, 1, 7 }, Rot{}, 1.0f } }, false), "fixture-move-back-accepted", UidStr(b));
    PumpAll();
    Publish({ a, b }, name);
    core::GroupExportApproval approval2;
    Require(Prepare({ a, b }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool preReplaceRan = false;
    host::SetBeforeReplace([&] {
        preReplaceRan = true;
        core::MoveMany({ { b, { 11, 2, 11 }, Rot{}, 1.0f } }, false);
        PumpAll();
    });
    std::string error2;
    Check("write-rejected-at-final-boundary", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", preReplaceRan, "");
    Check("final-boundary-error-names-values", Has(error2, "values or envelopes changed"), error2);
    Check("final-boundary-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("final-boundary-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"final\",\"approval\":" + Quote(DocText(approval2.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"error\":" + Quote(error2));
}

// failure: the published selection changed after approval (and an unpublished selection cannot be prepared)
static void CaseSelection() {
    BeginCase("selection");
    const std::string name = "t12-sel";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    const int c = Spawn("/object/export_a.prefab", { 8, 1, 9 });
    Require(a > 0 && b > 0 && c > 0, "fixture-spawned", UidStr(a) + " " + UidStr(b) + " " + UidStr(c));
    std::string oldHash;
    Require(WriteSentinel(name, "sel", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight-before-selection-change", approval.error);

    // an unpublished selection cannot be reviewed at all
    core::GroupExportApproval unpublished;
    Check("unpublished-selection-refused", !Prepare({ a }, name, true, unpublished) && Has(unpublished.error, "selection was not published"), unpublished.error);
    Check("unpublished-superset-refused", !Prepare({ a, b, c }, name, true, unpublished) && Has(unpublished.error, "selection was not published"), unpublished.error);

    // boundary 1: the editor published a changed selection
    Publish({ a, b, c }, name);
    std::string error;
    Check("write-rejected-after-selection-change", !Write(approval, error), error);
    Check("selection-error-names-context", Has(error, "selection or placement context changed"), error);
    Check("selection-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("selection-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval.document)) + ",\"current\":" + Quote("selection=" + std::to_string(core::PublishedExportContext().selection.size())) + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error));

    // boundary 2: the publication changes inside the write, before the protected replace
    Publish({ a, b }, name);
    core::GroupExportApproval approval2;
    Require(Prepare({ a, b }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool ran = false;
    host::SetBeforeReplace([&] { ran = true; Publish({ a, b, c }, name); });
    std::string error2;
    Check("final-boundary-selection-change-rejected", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", ran, "");
    Check("selection-final-error-names-context", Has(error2, "selection or placement context changed"), error2);
    Check("selection-final-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("selection-final-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"final\",\"approval\":" + Quote(DocText(approval2.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"error\":" + Quote(error2));
}

// failure: the destination/project name changed after approval
static void CaseProjectContext() {
    BeginCase("project-context");
    const std::string name = "t12-ctx-a";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, "ctx", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);

    Publish({ a, b }, "t12-ctx-b");
    std::string error;
    Check("write-rejected-after-project-change", !Write(approval, error), error);
    Check("project-error-names-context", Has(error, "selection or placement context changed"), error);
    Check("project-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval.document)) + ",\"current\":" + Quote(core::PublishedExportContext().name) + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error));

    Publish({ a, b }, name);
    core::GroupExportApproval approval2;
    Require(Prepare({ a, b }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool ran = false;
    host::SetBeforeReplace([&] { ran = true; Publish({ a, b }, "t12-ctx-c"); });
    std::string error2;
    Check("final-boundary-project-change-rejected", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", ran, "");
    Check("project-final-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("project-final-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));

    // the project membership of an included record is part of the approved context too
    Publish({ a, b }, name);
    core::GroupExportApproval approval3;
    Require(Prepare({ a, b }, name, true, approval3), "preflight-before-project-membership-change", approval3.error);
    core::AssignProject(a, 7);
    std::string error3;
    Check("write-rejected-after-project-membership-change", !Write(approval3, error3) && Has(error3, "included objects changed"), error3);
    Check("project-membership-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval3.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error3));
}

// failure: a placement/grab context change (placing flag and carried set) after approval
static void CasePlacementContext() {
    BeginCase("placement-context");
    const std::string name = "t12-place";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, "place", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name, false, {});
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);

    // boundary 1: a grab started after the approval
    Publish({ a, b }, name, true, { a });
    std::string error;
    Check("write-rejected-after-placement-change", !Write(approval, error), error);
    Check("placement-error-names-context", Has(error, "selection or placement context changed"), error);
    Check("placement-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval.document)) + ",\"current\":" + Quote("placing=1 carried=1") + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error));

    // boundary 2: the carried set changes inside the write
    Publish({ a, b }, name, false, {});
    core::GroupExportApproval approval2;
    Require(Prepare({ a, b }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool ran = false;
    host::SetBeforeReplace([&] { ran = true; Publish({ a, b }, name, true, { b }); });
    std::string error2;
    Check("final-boundary-placement-change-rejected", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", ran, "");
    Check("placement-final-error-names-context", Has(error2, "selection or placement context changed"), error2);
    Check("placement-final-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("placement-final-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
}

// failure: a changed receiver box hint changes only the envelope; the stale approval rejects, the fresh one writes
static void CaseEnvelope() {
    BeginCase("envelope");
    const std::string name = "t12-env";
    const std::string prefab = "/object/export_a.prefab";
    const int a = Spawn(prefab, { 2, 1, 3 });
    Require(a > 0 && Rec(a), "fixture-spawned", UidStr(a));
    std::string oldHash;
    Require(WriteSentinel(name, "env", &oldHash), "sentinel-written", "");
    const float oldSize[6] = { 1.5f, 2.0f, 1.0f, 0.1f, 1.0f, -0.2f };
    const float newSize[6] = { 4.5f, 6.0f, 3.0f, 0.5f, 2.0f, -0.6f };
    core::SetPrefabSize(prefab, oldSize);
    Publish({ a }, name);
    core::GroupExportApproval stale;
    Require(Prepare({ a }, name, true, stale), "preflight-with-old-hint", stale.error);

    core::SetPrefabSize(prefab, newSize);   // the thumbnail worker's measurement arrives while the dialog is open
    Publish({ a }, name);
    core::GroupExportApproval fresh;
    Require(Prepare({ a }, name, true, fresh), "preflight-with-new-hint", fresh.error);
    Check("envelope-records-unchanged", DataRows(DocText(stale.document)) == DataRows(DocText(fresh.document)), "");
    Check("envelope-changed", EnvelopeLines(DocText(stale.document)) != EnvelopeLines(DocText(fresh.document)), "");
    Check("envelope-bounds-differ", !(stale.document.bounds.max.x == fresh.document.bounds.max.x && stale.document.bounds.min.x == fresh.document.bounds.min.x), "");
    std::string error;
    Check("stale-envelope-approval-rejected", !Write(stale, error), error);
    Check("envelope-error-names-values", Has(error, "values or envelopes changed"), error);
    Check("envelope-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(stale.document)) + ",\"current\":" + Quote(DocText(fresh.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error));

    // boundary 2: the hint changes inside the write (restore the old hint, approve it, mutate during the write)
    core::SetPrefabSize(prefab, oldSize);
    Publish({ a }, name);
    core::GroupExportApproval approval2;
    Require(Prepare({ a }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool ran = false;
    host::SetBeforeReplace([&] { ran = true; core::SetPrefabSize(prefab, newSize); });
    std::string error2;
    Check("final-boundary-envelope-change-rejected", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", ran, "");
    Check("envelope-final-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("envelope-final-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));

    // the fresh approval (same live values as the rebuild) writes and replaces the sentinel
    std::string error3;
    Check("fresh-envelope-approval-writes", Write(fresh, error3), error3);
    Check("envelope-file-replaced", Sha256File(GroupPath(name)) != oldHash, Sha256File(GroupPath(name)));
    proj_codec::Document parsed; std::string perr;
    Check("envelope-file-is-new-document", ParseGroupFile(GroupPath(name), parsed, perr) && DocText(parsed) == DocText(fresh.document), perr);
    Trace("\"boundary\":\"final-write\",\"approval\":" + Quote(DocText(fresh.document)) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))));
}

// failure: empty selection and an all-excluded selection are refused by preflight, nothing is written
static void CaseEmpty() {
    BeginCase("empty");
    const std::string name = "t12-empty";
    Publish({}, name);
    core::GroupExportApproval approval;
    Check("empty-selection-refused", !Prepare({}, name, false, approval) && Has(approval.error, "no selected objects"), approval.error);
    Check("empty-nothing-written", GroupFileCount() == 0 && TempLeftovers() == 0, "files=" + std::to_string(GroupFileCount()));
    Check("empty-not-valid", !approval.valid, "");
    Trace("\"boundary\":\"preflight\",\"error\":" + Quote(approval.error));
}

static void CaseAllExcluded() {
    BeginCase("all-excluded");
    const std::string name = "t12-excluded";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    Require(core::HideUid(a), "fixture-hid-one", UidStr(a));
    core::ForgetUid(b);
    PumpAll();
    Snapshot();
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Check("all-excluded-refused", !Prepare({ a, b }, name, false, approval) && Has(approval.error, "no placeable selected objects"), approval.error);
    Check("all-excluded-named-hidden", approval.excluded.size() == 2 && ReasonFor(approval, a) == "hidden", ReasonFor(approval, a));
    Check("all-excluded-named-forgotten", approval.excluded.size() == 2 && ReasonFor(approval, b) == "forgotten selection", ReasonFor(approval, b));
    Check("all-excluded-nothing-written", GroupFileCount() == 0 && TempLeftovers() == 0, "files=" + std::to_string(GroupFileCount()));
    std::string reasons;
    for (const auto& x : approval.excluded) reasons += (reasons.empty() ? "" : ",") + x.reason;
    Trace("\"boundary\":\"preflight\",\"excluded\":" + Quote(reasons) + ",\"error\":" + Quote(approval.error));
}

// failure: a non-finite live value rejects at both boundaries
static void CaseNaN() {
    BeginCase("nan");
    const std::string name = "t12-nan";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, "nan", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    Require(core::MoveMany({ { b, { nan, 1.0f, 7.0f }, Rot{}, 1.0f } }, false), "fixture-nan-move-accepted", UidStr(b));
    PumpAll();
    std::string error;
    Check("write-rejected-after-nan", !Write(approval, error), error);
    Check("nan-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("nan-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"before-write\",\"approval\":" + Quote(DocText(approval.document)) + ",\"oldHash\":" + Quote(oldHash) + ",\"error\":" + Quote(error));

    Require(core::MoveMany({ { b, { 5, 1, 7 }, Rot{}, 1.0f } }, false), "fixture-move-back-accepted", "");
    PumpAll();
    Publish({ a, b }, name);
    core::GroupExportApproval approval2;
    Require(Prepare({ a, b }, name, true, approval2), "preflight-before-final-boundary", approval2.error);
    bool ran = false;
    host::SetBeforeReplace([&] { ran = true; core::MoveMany({ { b, { nan, 1.0f, 7.0f }, Rot{}, 1.0f } }, false); });
    std::string error2;
    Check("final-boundary-nan-rejected", !Write(approval2, error2), error2);
    Check("final-boundary-callback-ran", ran, "");
    Check("nan-final-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("nan-final-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
}

// failure: invalid names, a kind tamper and a path tamper are refused; nothing escapes the group folder
static void CasePathKind() {
    BeginCase("path-kind");
    const std::string name = "t12-path";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, "path", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    const char* invalid[] = { "", ".", "..", "a\\b", "..\\escape", "sub/dir", "bad:name", "ques?tion", "trail.", "name.cdgroup", "name.cdproj", ".cdgroup" };
    for (const char* candidate : invalid) {
        core::GroupExportApproval bad;
        const bool prepared = Prepare({ a, b }, candidate, false, bad);
        Check("invalid-name-refused", !prepared && !bad.valid && !bad.error.empty(), std::string(candidate) + " -> " + bad.error);
    }
    Check("no-escape-from-groups", !FileExists(g_fixtureDir + "\\escape.cdgroup") && !FileExists(g_fixtureDir + "\\a\\b.cdgroup"), "");
    Check("groups-folder-untouched-by-names", GroupFileCount() == 1 && Sha256File(GroupPath(name)) == oldHash, "files=" + std::to_string(GroupFileCount()));

    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);
    {
        core::GroupExportApproval tampered = approval;
        tampered.document.kind = proj_codec::Kind::Project;
        std::string error;
        Check("kind-tamper-refused", !Write(tampered, error) && Has(error, "not a group document"), error);
    }
    {
        core::GroupExportApproval tampered = approval;
        tampered.path = GroupDir() + "\\other.cdgroup";
        std::string error;
        Check("path-tamper-refused", !Write(tampered, error) && Has(error, "path does not match"), error);
    }
    {
        core::GroupExportApproval tampered = approval;
        tampered.document.records.clear();
        std::string error;
        Check("empty-document-tamper-refused", !Write(tampered, error), error);
    }
    Check("path-old-file-identical", Sha256File(GroupPath(name)) == oldHash, Sha256File(GroupPath(name)));
    Check("path-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"preflight+write\",\"oldHash\":" + Quote(oldHash));
}

// failure: an existing destination needs explicit overwrite approval; a file appearing after preflight rejects
static void CaseOverwrite() {
    BeginCase("overwrite-approval");
    const std::string name = "t12-overwrite";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    Require(a > 0, "fixture-spawned", UidStr(a));
    std::string oldHash;
    Require(WriteSentinel(name, "ow", &oldHash), "sentinel-written", "");
    Publish({ a }, name);
    core::GroupExportApproval refused;
    Check("existing-file-needs-approval", !Prepare({ a }, name, false, refused) && Has(refused.error, "explicit overwrite approval required"), refused.error);
    Check("overwrite-old-file-identical", Sha256File(GroupPath(name)) == oldHash, "");

    core::GroupExportApproval approved;
    Require(Prepare({ a }, name, true, approved), "approved-overwrite-preflight", approved.error);
    std::string error;
    Require(Write(approved, error), "approved-overwrite-write", error);
    Check("overwrite-replaced-file", Sha256File(GroupPath(name)) != oldHash, Sha256File(GroupPath(name)));
    proj_codec::Document parsed; std::string perr;
    Check("overwrite-file-is-approved-document", ParseGroupFile(GroupPath(name), parsed, perr) && DocText(parsed) == DocText(approved.document), perr);

    // a file that appears after preflight: the write refuses without explicit approval and keeps those bytes
    const std::string name2 = "t12-appear";
    Publish({ a }, name2);
    core::GroupExportApproval appeared;
    Require(Prepare({ a }, name2, false, appeared), "preflight-before-file-appears", appeared.error);
    std::string appearedHash;
    Require(WriteSentinel(name2, "appeared", &appearedHash), "external-file-appeared", "");
    std::string error2;
    Check("file-appeared-rejected", !Write(appeared, error2) && Has(error2, "explicit overwrite approval required"), error2);
    Check("appeared-file-unchanged", Sha256File(GroupPath(name2)) == appearedHash, Sha256File(GroupPath(name2)));
    Check("overwrite-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"preflight+write\",\"oldHash\":" + Quote(oldHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))));
}

// one file-fault case per stage: the export fails, the previous bytes stay and only the owned temporary is removed
static void CaseFileFault(const char* id, host::SaveFault fault, const char* label) {
    BeginCase(id);
    const std::string name = std::string("t12-fault-") + id;
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, id, &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);
    host::SetSaveFault(fault);
    std::string error;
    Check(label, !Write(approval, error), error);
    const std::string refusedHash = Sha256File(GroupPath(name));
    Check("fault-old-file-identical", refusedHash == oldHash, refusedHash);
    Check("fault-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    host::SetSaveFault(host::SaveFault::None);
    // no false success receipt: the same approval still writes once the fault is gone
    std::string error2;
    Require(Write(approval, error2), "write-succeeds-after-the-fault", error2);
    Check("fault-recovered-file", Sha256File(GroupPath(name)) != oldHash, "");
    Trace("\"boundary\":\"" + std::string(id) + "\",\"oldHash\":" + Quote(oldHash) + ",\"refusedHash\":" + Quote(refusedHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"error\":" + Quote(error));
}

// the protected replacement: both authorities are held between the last comparison and the rename
static void CaseProtectedReplace() {
    BeginCase("protected-replace");
    const std::string name = "t12-protected";
    const int a = Spawn("/object/export_a.prefab", { 2, 1, 3 });
    const int b = Spawn("/object/export_b.prefab", { 5, 1, 7 });
    Require(a > 0 && b > 0, "fixture-spawned", "");
    std::string oldHash;
    Require(WriteSentinel(name, "protected", &oldHash), "sentinel-written", "");
    Publish({ a, b }, name);
    core::GroupExportApproval approval;
    Require(Prepare({ a, b }, name, true, approval), "preflight", approval.error);
    bool ran = false, ctxFree = true, regFree = true;
    host::SetExportReplaceProbe([&] {
        ran = true;
        // the probe thread try-locks both authorities while this thread holds them, so each must fail
        std::thread ctxProbe([&] { ctxFree = host::ExportContextLockFree(); });
        ctxProbe.join();
        std::thread regProbe([&] { regFree = host::RegistryLockFree(); });
        regProbe.join();
    });
    std::string error;
    Require(Write(approval, error), "protected-write", error);
    Check("probe-ran-inside-protected-region", ran, "");
    Check("publication-held-through-replace", !ctxFree, "");
    Check("registry-held-through-replace", !regFree, "");
    Check("protected-file-replaced", Sha256File(GroupPath(name)) != oldHash, "");
    proj_codec::Document parsed; std::string perr;
    Check("protected-file-is-approved-document", ParseGroupFile(GroupPath(name), parsed, perr) && DocText(parsed) == DocText(approval.document), perr);
    Check("protected-no-temporary", TempLeftovers() == 0, "leftovers=" + std::to_string(TempLeftovers()));
    Trace("\"boundary\":\"protected-replace\",\"oldHash\":" + Quote(oldHash) + ",\"newHash\":" + Quote(Sha256File(GroupPath(name))) + ",\"publicationHeld\":true,\"registryHeld\":true");
}

// =========================================================================================================
// main
// =========================================================================================================
int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : ".";
    host::OpenLog(g_fixtureDir + "\\export.log");
    core::Log("[export] run start: fixtureDir=%s", g_fixtureDir.c_str());
    g_probe.Start();
    InstallSeam();
    host::SetModDir(g_fixtureDir);       // real files, inside the fixture directory only
    core::g_recreateOnMove = true;
    host::SetDirectGimmick(false);
    InstallIndex();
    CleanFixtureDir();

    struct Entry { const char* id; void (*fn)(); };
    const Entry entries[] = {
        { "preflight-write", CasePreflightWrite },
        { "roundtrip", CaseRoundtrip },
        { "live-move", CaseLiveMove },
        { "selection", CaseSelection },
        { "project-context", CaseProjectContext },
        { "placement-context", CasePlacementContext },
        { "envelope", CaseEnvelope },
        { "empty", CaseEmpty },
        { "all-excluded", CaseAllExcluded },
        { "nan", CaseNaN },
        { "path-kind", CasePathKind },
        { "overwrite-approval", CaseOverwrite },
        { "short-write", [] { CaseFileFault("short-write", host::SaveFault::ShortWrite, "short-write-fails-the-export"); } },
        { "flush", [] { CaseFileFault("flush", host::SaveFault::FlushAbort, "flush-failure-fails-the-export"); } },
        { "close", [] { CaseFileFault("close", host::SaveFault::CloseAbort, "close-failure-fails-the-export"); } },
        { "replace", [] { CaseFileFault("replace", host::SaveFault::ReplaceAbort, "replace-failure-fails-the-export"); } },
        { "protected-replace", CaseProtectedReplace },
    };
    for (const Entry& e : entries) {
        oracle::Reset();
        host::SetSaveFault(host::SaveFault::None);
        host::SetBeforeReplace({});
        host::SetExportReplaceProbe({});
        InstallIndex();
        core::DeleteAllSpawned();
        PumpAll();
        CleanFixtureDir();
        try {
            e.fn();
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("CASE-ABORT %s: %s\n", e.id, ex.what());
            core::Log("[export] CASE-ABORT %s: %s", e.id, ex.what());
            if (!g_cases.empty() && g_cases.back().failures == 0) ++g_cases.back().failures;
        }
    }

    Check("engine-lock-free-throughout", oracle::lockFreeDuringEngineCalls, "lockFree=" + std::to_string(oracle::lockFreeDuringEngineCalls));
    Check("no-queue-push-under-registry-lock", host::LockViolations() == 0, "violations=" + std::to_string(host::LockViolations()));

    {
        std::string json = "{\"suite\":\"Export\",\"status\":\"" + std::string(g_failures == 0 ? "PASS" : "FAIL") +
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
        core::Log("[export] cases: %s", json.c_str());
    }
    {
        g_trace += "\n]\n";
        FILE* f = std::fopen((g_fixtureDir + "\\export_trace.json").c_str(), "wb");
        if (f) { std::fwrite(g_trace.data(), 1, g_trace.size(), f); std::fclose(f); }
    }
    std::printf("CASES=%zu\nASSERTIONS=%d\nFAILURES=%d\n", g_cases.size(), g_assertions, g_failures);
    core::Log("[export] run end: cases=%zu assertions=%d failures=%d lockViolations=%d", g_cases.size(), g_assertions, g_failures, host::LockViolations());
    g_probe.Stop();
    return g_failures == 0 ? 0 : 1;
}
