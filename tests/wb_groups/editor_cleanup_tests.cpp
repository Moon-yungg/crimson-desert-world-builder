// Task15 (wb079-unified-77f68967): dead Travel UI removal + developer console reachability.
//
// The fixture drives (a) the ACTUAL production editor (every reachable tab, both shells) and (b) the ACTUAL
// production console dispatcher: host::ConsoleDispatch forwards to core::ConsoleDispatch in cdmodkit.cpp, the
// very function ConsoleThread feeds line by line -- there is no second, copied test dispatcher. Only OS/engine
// boundaries are substituted (host::Seam engine calls and host::Services for the diag/thumbnail services that
// do not link in a host run). No sleeps, no polling: every wait is the production queue/physics release.
//
// The editor TU is instrumented at its file-open boundary (fopen/fopen_s) BEFORE it is included, so the
// teleports.txt sentinel check is a real trace, not a source grep.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace fio {
struct Attempt { std::string path, mode, caller; };
std::vector<Attempt>& Opens();
FILE* TracedFopen(const char* path, const char* mode);
errno_t TracedFopenS(FILE** out, const char* path, const char* mode);
}
#define fopen fio::TracedFopen
#define fopen_s fio::TracedFopenS
#include "../../asi/cdmodkit/editor.cpp"
#undef fopen
#undef fopen_s
#include "production_host.h"
#include <imgui_internal.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace fio {
std::vector<Attempt>& Opens() { static std::vector<Attempt> v; return v; }
FILE* TracedFopen(const char* path, const char* mode) {
    Opens().push_back({ path ? path : "", mode ? mode : "", "editor.cpp" });
    return std::fopen(path, mode);
}
errno_t TracedFopenS(FILE** out, const char* path, const char* mode) {
    Opens().push_back({ path ? path : "", mode ? mode : "", "editor.cpp" });
    return ::fopen_s(out, path, mode);
}
int TeleportsAttempts() {
    int n = 0; for (const auto& a : Opens()) if (a.path.find("teleports") != std::string::npos) ++n;
    return n;
}
}

namespace {
struct Item { ImGuiID id = 0; ImRect box, clip; std::string label; bool disabled = false; std::vector<ImGuiID> stack;
    ImGuiItemStatusFlags status = 0; ImGuiWindow* window = nullptr; };
std::map<ImGuiID, Item> items;
int assertions = 0, failures = 0, quitSeen = 0;
std::string root, current, dir;
std::vector<std::string> cases, dispatch;
bool full = false; float screenW = 1280, screenH = 900;

// engine-boundary substitutes (observation of the real production calls)
int creates = 0, removes = 0, moves = 0, teleports = 0, casts = 0, actorsRemoved = 0;
uintptr_t nextHandle = 100;
Vec3 lastTeleport{};
struct CastCall { Vec3 start; float len; };
std::vector<CastCall> castCalls;
struct Obj { Vec3 pos; Rot rot; float scale; bool live = true; };
std::map<uintptr_t, Obj> objects;
// diag/thumbnail service substitutes
int viewScanCalls = 0, findCameraCalls = 0, camWatchCalls = 0, fovTraceCalls = 0, camTraceCalls = 0, ioTraceCalls = 0, thumbsCalls = 0;
int camWatchSeconds = -1, camWatchMode = -1, fovTraceSeconds = -1, camTraceSeconds = -1;
bool ioTraceLast = false, thumbsLast = false;

const char* kGood = "/object/good.prefab";

std::string J(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else o += c; }
    return o;
}
void Check(bool ok, const char* what) {
    ++assertions; if (!ok) ++failures;
    printf("CHECK %s %s/%s\n", ok ? "PASS" : "FAIL", current.c_str(), what);
    core::Log("CHECK %s %s/%s", ok ? "PASS" : "FAIL", current.c_str(), what);
}
void Require(bool ok, const char* what) { Check(ok, what); if (!ok) throw std::runtime_error(what); }
bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }
void Write(const std::string& path, const std::string& bytes) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) throw std::runtime_error("file open failed: " + path);
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(); ok = fclose(f) == 0 && ok;
    if (!ok) throw std::runtime_error("file write failed: " + path);
}
std::string Read(const std::string& path) { std::ifstream f(path, std::ios::binary); return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() }; }
std::string Sha256Hex(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) throw std::runtime_error("sha256 provider");
    DWORD objSize = 0, cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objSize, sizeof objSize, &cb, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("sha256 object length"); }
    std::vector<unsigned char> obj(objSize);
    if (BCryptCreateHash(alg, &hash, obj.data(), objSize, nullptr, 0, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("sha256 hash object"); }
    bool ok = BCryptHashData(hash, (PUCHAR)bytes.data(), (ULONG)bytes.size(), 0) == 0;
    unsigned char digest[32]{};
    ok = BCryptFinishHash(hash, digest, sizeof digest, 0) == 0 && ok;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) throw std::runtime_error("sha256 compute");
    char buf[65]; for (int i = 0; i < 32; ++i) sprintf_s(buf + i * 2, 3, "%02x", digest[i]);
    return std::string(buf, 64);
}
void Array(const char* name, const std::vector<std::string>& rows) {
    std::string text = "[\n"; for (size_t i = 0; i < rows.size(); ++i) text += rows[i] + (i + 1 == rows.size() ? "\n" : ",\n");
    Write(root + "\\" + name, text + "]\n");
}
std::string LogPath() { return root + "\\editor_cleanup.log"; }
bool LogHas(const std::string& marker) { return Read(LogPath()).find(marker) != std::string::npos; }
std::string World();   // canonical machine state digest (defined below Cmd)

// ---- the production dispatcher through the host seam (one line per call, exactly like ConsoleThread) ----
bool Cmd(const std::string& line) {
    const std::string before = World();
    const bool quit = host::ConsoleDispatch(line);
    if (quit) ++quitSeen;
    dispatch.push_back("{\"case\":\"" + J(current) + "\",\"command\":\"" + J(line) + "\",\"quit\":" + (quit ? "true" : "false") +
                       ",\"mutated\":" + (World() != before ? "true" : "false") + "}");
    return quit;
}
std::string World() {
    std::ostringstream s; s << std::hexfloat;
    s << "uid=" << host::NextUid() << ";pending=" << core::PendingSpawns() << ";creates=" << creates << ";removes=" << removes
      << ";moves=" << moves << ";teleports=" << teleports << ";casts=" << casts << ";actors=" << actorsRemoved
      << ";live=" << core::g_liveMode << ";recreate=" << core::g_recreateOnMove << ";gimmicks=" << core::g_gimmickSpawn
      << ";livedrag=" << core::g_liveDrag << ";trace=" << core::Trace() << ";replayprefab=" << core::GimmickReplayPrefab();
    for (const auto& o : core::Spawned()) s << "|" << o.uid << "," << o.obj << "," << o.gen << "," << o.poseGen << "," << o.hidden << "," << o.proj << "," << o.group
      << "," << o.pos.x << "," << o.pos.y << "," << o.pos.z << "," << o.rot.yaw << "," << o.rot.pitch << "," << o.rot.roll << "," << o.scale
      << "," << o.colRot.yaw << "," << o.colRot.pitch << "," << o.colRot.roll << "," << o.colScale << "," << o.tick << "," << o.gimmick << "," << o.actor
      << "," << o.standin << "," << o.placeReq.lock().get() << "," << o.placeRow << "," << o.prefab.size() << ":" << o.prefab;
    s << ";undo=" << editor::g_undo.size() << ";redo=" << editor::g_redo.size();
    for (int uid : editor::g_sel) s << "S" << uid;
    return s.str();
}
SpawnedObj Rec(int uid) { for (const auto& o : core::Spawned()) if (o.uid == uid) return o; throw std::runtime_error("missing uid"); }
void Drain() { for (int i = 0; host::PumpGame(); ++i) if (i > 4096) throw std::runtime_error("CORE queue did not drain"); host::PumpServer(); }

std::vector<int> ServiceCounters() {
    return { viewScanCalls, findCameraCalls, camWatchCalls, fovTraceCalls, camTraceCalls, ioTraceCalls, thumbsCalls };
}
std::string HistoryState() {
    std::ostringstream s; s << std::hexfloat << editor::g_historySerial << "," << editor::g_historyBranch;
    for (const auto* stack : { &editor::g_undo, &editor::g_redo }) {
        s << "|stack=" << stack->size();
        for (const auto& e : *stack) {
            s << "|entry=" << e.serial << "," << e.branch << "," << e.acts.size();
            for (const auto& a : e.acts) s << "|act=" << a.kind << "," << a.uid << "," << a.prefab.size() << ":" << a.prefab
              << "," << a.pos0.x << "," << a.pos0.y << "," << a.pos0.z << "," << a.pos1.x << "," << a.pos1.y << "," << a.pos1.z
              << "," << a.rot0.yaw << "," << a.rot0.pitch << "," << a.rot0.roll << "," << a.rot1.yaw << "," << a.rot1.pitch << "," << a.rot1.roll
              << "," << a.sc0 << "," << a.sc1 << "," << a.group << "," << a.group1 << "," << a.proj;
        }
    }
    return s.str();
}

// ---- ImGui item observation (real geometry and machine status flags; no simulated UI) ----
void Frame() {
    items.clear(); ImGui::GetIO().DisplaySize = { screenW, screenH }; ImGui::NewFrame();
    editor::Draw();
    ImGui::Render();
}
bool HasContaining(const std::string& part) { for (const auto& x : items) if (x.second.label.find(part) != std::string::npos) return true; return false; }
Item FindItem(const std::string& label) {
    for (const auto& x : items) if (x.second.label == label) return x.second;
    throw std::runtime_error("control missing: " + label);
}
void Click(const std::string& label) {
    Frame();
    Item item = FindItem(label); auto& io = ImGui::GetIO();
    Require(item.box.GetCenter().y >= 0 && item.box.GetCenter().y < screenH, "control-in-viewport");
    io.AddMousePosEvent(item.box.GetCenter().x, item.box.GetCenter().y); Frame();
    io.AddMouseButtonEvent(0, true); Frame(); io.AddMouseButtonEvent(0, false); Frame(); Frame();
}

Item ReachCheckbox(const std::string& label) {
    Frame(); Item item = FindItem(label);
    Require(item.window && !item.disabled && (item.status & ImGuiItemStatusFlags_Checkable), "checkbox-enabled");
    // Scroll the actual child window to the checkbox square, not a guessed screen coordinate or a timed wait.
    ImRect square(item.box.Min, ImVec2(item.box.Min.x + item.box.GetHeight(), item.box.Max.y));
    if (!item.clip.Contains(square)) {
        ImGui::ScrollToRect(item.window, square, ImGuiScrollFlags_KeepVisibleEdgeY); Frame();
        item = FindItem(label);
        square = ImRect(item.box.Min, ImVec2(item.box.Min.x + item.box.GetHeight(), item.box.Max.y));
    }
    Require(item.clip.Contains(square) && square.Min.x >= 0 && square.Min.y >= 0 &&
        square.Max.x <= screenW && square.Max.y <= screenH, "checkbox-in-viewport");
    return item;
}
void ClickCheckbox(const std::string& label) {
    const Item item = ReachCheckbox(label); auto& io = ImGui::GetIO();
    io.AddMousePosEvent(item.box.Min.x + item.box.GetHeight() * 0.5f, item.box.GetCenter().y); Frame();
    // Navigation/scroll processing can move a wrapped control on that frame. Click its current square.
    const auto target = FindItem(label);
    io.AddMousePosEvent(target.box.Min.x + target.box.GetHeight() * 0.5f, target.box.GetCenter().y);
    io.AddMouseButtonEvent(0, true); Frame();
    std::printf("CHECKBOX id=%u active=%u mouse=(%g,%g) box=(%g,%g,%g,%g) hovered=%s\n", target.id, GImGui->ActiveId, io.MousePos.x, io.MousePos.y, target.box.Min.x, target.box.Min.y, target.box.Max.x, target.box.Max.y, GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "none");
    Check(GImGui->ActiveId == target.id, "checkbox-press-owns-actual-control");
    io.AddMouseButtonEvent(0, false); Frame(); Frame();
}
bool Checked(const std::string& label) { return (FindItem(label).status & ImGuiItemStatusFlags_Checked) != 0; }
int SavedConsoleSetting() {
    Require(core::ModDir() == dir, "settings-isolated-mod-dir");
    std::ifstream f(dir + "\\settings.txt");
    Require(f.is_open(), "settings-production-file-written");
    std::string line; int value = -1, fields = 0;
    while (std::getline(f, line)) if (line.rfind("console=", 0) == 0) {
        std::istringstream field(line.substr(8));
        Require(bool(field >> value) && (field >> std::ws).eof(), "settings-console-field-numeric");
        ++fields;
    }
    Require(f.eof() && fields == 1, "settings-single-console-field");
    return value;
}

void InstallEngine() {
    auto& e = host::Seam();
    e.ready = true; e.probeReady = true;
    e.playerWorldPos = [](Vec3* out) { *out = { 100.0f, 10.0f, -20.0f }; return true; };
    e.teleport = [](Vec3 p) { ++teleports; lastTeleport = p; return true; };
    e.createGeneric = [](const std::string&, Vec3 pos, Rot rot, float scale) { ++creates; uintptr_t h = nextHandle++; objects[h] = { pos, rot, scale, true }; return h; };
    e.remove = [](uintptr_t h) { ++removes; auto it = objects.find(h); if (it == objects.end() || !it->second.live) return false; it->second.live = false; return true; };
    e.moveInPlace = [](uintptr_t h, Vec3 p, Rot r, float s) { ++moves; auto it = objects.find(h); if (it == objects.end() || !it->second.live) return false; it->second.pos = p; it->second.rot = r; it->second.scale = s; return true; };
    e.liveMove = e.moveInPlace;
    e.groundCast = [](Vec3 start, float len, core::GroundHit* hit) { ++casts; castCalls.push_back({ start, len }); hit->done = hit->hit = true; hit->centerY = 0.0f; hit->fraction = 0.0f; return true; };
    e.removeActor = [](uintptr_t) { ++actorsRemoved; return true; };
    e.templateReady = [] { return false; };
    auto& s = host::Services();
    s.viewScan = [] { ++viewScanCalls; };
    s.findCamera = [] { ++findCameraCalls; };
    s.camWatch = [](int seconds, int mode) { ++camWatchCalls; camWatchSeconds = seconds; camWatchMode = mode; };
    s.fovTrace = [](int seconds) { ++fovTraceCalls; fovTraceSeconds = seconds; };
    s.camTrace = [](int seconds) { ++camTraceCalls; camTraceSeconds = seconds; };
    s.ioTrace = [](bool on) { ++ioTraceCalls; ioTraceLast = on; };
    s.thumbsBackground = [](bool on) { ++thumbsCalls; thumbsLast = on; };
    std::vector<core::PrefabInfo> index;
    core::PrefabInfo p; p.path = kGood; p.name = "good"; p.hasCenter = true; p.meshes = 1; p.sx = p.sy = p.sz = 2; index.push_back(p);
    host::SetPrefabIndex(index);
}

void Reset() {
    if (!editor::g_pendingGround.empty()) { core::GroundBarrier(editor::g_pendingGround, true); editor::g_pendingGround.clear(); }
    editor::g_groundLastResults.clear(); editor::g_deferredEditor = {};
    full = false; screenW = 1280; screenH = 900;
    editor::ClearSceneAction(false); Drain(); editor::PumpSnapJobs();
    editor::host_seam::ResetPlacement(); editor::host_seam::ResetHistory();
    objects.clear(); castCalls.clear(); nextHandle = 100;
    core::g_liveMode = 2; core::g_recreateOnMove = true; core::g_gimmickSpawn = true; core::g_liveDrag = true;
    core::SetTrace(false); core::SetGimmickReplayPrefab("");
    host::Seam().ready = true; host::Seam().probeReady = true;
    editor::g_projectPlacements.clear(); editor::g_groundLastResults.clear(); editor::g_projectReadValid = false;
    editor::g_open = true; editor::g_playMode = false; editor::g_compact = false;
    editor::g_primary = 0; editor::g_sel.clear(); editor::g_selPrefab = -1;
    dir = root + "\\" + current; CreateDirectoryA(dir.c_str(), nullptr); CreateDirectoryA((dir + "\\projects").c_str(), nullptr);
    host::SetModDir(dir);
    ImGui::GetIO().AddMousePosEvent(-100, -100); Frame(); Frame();
}

// ===================== happy: every listed command reaches its production branch =====================
void ConsoleCommands() {
    Reset();
    Cmd("pos"); Check(LogHas("pos: no player actor yet"), "pos-real-branch");
    Cmd("status"); Check(LogHas("status: creates="), "status-real-branch");

    const int uidBefore = host::NextUid(); const int createsBefore = creates;
    Cmd("spawn /object/good.prefab"); Drain();
    Require(core::Spawned().size() == 1, "spawn-registry-record");
    Check(host::NextUid() == uidBefore + 1, "spawn-uid-allocated");
    Check(creates == createsBefore + 1, "spawn-engine-created");
    const int uid = core::Spawned().front().uid;
    Check(Near(Rec(uid).pos.x, 102) && Near(Rec(uid).pos.y, 10) && Near(Rec(uid).pos.z, -20), "spawn-at-player-offset");

    Cmd("list"); Check(LogHas("  #0 "), "list-real-branch");

    const int engineBefore = creates + removes + moves;
    Cmd("move 0 0 5 0 45 2"); Drain();
    Check(Near(Rec(uid).pos.x, 0) && Near(Rec(uid).pos.y, 5) && Near(Rec(uid).pos.z, 0), "move-actual-pose");
    Check(creates + removes + moves > engineBefore, "move-engine-boundary-called");

    Cmd("save scene1");
    const std::string projFile = dir + "\\projects\\scene1.cdproj";
    const std::string saved = Read(projFile);
    proj_codec::Document savedDocument; std::string savedError;
    Check(proj_codec::Parse(saved, projFile, proj_codec::Kind::Project, savedDocument, savedError) && !savedDocument.records.empty(), "save-real-file");

    const int removesBefore = removes;
    Cmd("hide 0"); Drain();
    Check(Rec(uid).hidden, "hide-registry-hidden");
    Check(removes > removesBefore, "hide-engine-remove");

    Cmd("projects"); Check(LogHas("scene1"), "projects-real-branch");

    const size_t registryBefore = core::Spawned().size();
    Cmd("load scene1"); Drain();
    Check(core::Spawned().size() == registryBefore + 1, "load-spawned-record");
    Check(core::ProjectLoadReportFor("scene1").queued == 1, "load-real-report");
    { const SpawnedObj loaded = core::Spawned().back();
      Check(!loaded.hidden && Near(loaded.pos.x, 0) && Near(loaded.pos.y, 5) && Near(loaded.pos.z, 0), "load-actual-saved-pose"); }

    Cmd("flags 2 3 4"); Check(LogHas("spawn flags now 2,3,4"), "flags-real-branch");

    Cmd("livemode 3"); Check(core::g_liveMode == 3, "livemode-state");
    Cmd("trace on"); Check(core::Trace(), "trace-on-state");
    Cmd("trace off"); Check(!core::Trace(), "trace-off-state");
    Cmd("livedrag off"); Check(!core::g_liveDrag, "livedrag-off-state");
    Cmd("livedrag on"); Check(core::g_liveDrag, "livedrag-on-state");
    Cmd("recreate off"); Check(!core::g_recreateOnMove, "recreate-off-state");
    Cmd("recreate on"); Check(core::g_recreateOnMove, "recreate-on-state");
    Cmd("gimmicks off"); Check(!core::g_gimmickSpawn, "gimmicks-off-state");
    Cmd("gimmicks on"); Check(core::g_gimmickSpawn, "gimmicks-on-state");

    const int tpBefore = teleports;
    Cmd("tp 7 8 9");
    Check(teleports == tpBefore + 1 && Near(lastTeleport.x, 7) && Near(lastTeleport.y, 8) && Near(lastTeleport.z, 9), "tp-engine-boundary");

    // Seed both History stacks through production actions: clearing or rewriting an existing entry must fail too.
    Require(editor::host_seam::SpawnRecorded(kGood, { 1, 2, 3 }, Rot{ 25, 5, 10 }, 1.25f, 17, 0) > 0, "retired-history-first-spawn");
    Require(editor::host_seam::SpawnRecorded(kGood, { 4, 5, 6 }, Rot{ 50, 10, 20 }, 1.5f, 18, 0) > 0, "retired-history-second-spawn");
    Drain(); Require(editor::host_seam::UndoOne(), "retired-history-undo"); Drain();
    Require(editor::g_undo.size() == 1 && editor::g_redo.size() == 1, "retired-history-both-stacks-populated");
    for (const char* command : { "viewscan", "findcamera" }) {
        const std::string prefix = std::string(command) + "-retired-";
        Require(!host::PumpGame(), (prefix + "queue-empty-before").c_str());
        const std::string worldBefore = World(), historyBefore = HistoryState();
        const auto servicesBefore = ServiceCounters();
        Check(!Cmd(command), (prefix + "nonquit").c_str());
        Check(World() == worldBefore, (prefix + "world-unchanged").c_str());
        Check(ServiceCounters() == servicesBefore, (prefix + "all-services-unchanged").c_str());
        Check(HistoryState() == historyBefore, (prefix + "history-unchanged").c_str());
        Check(!host::PumpGame(), (prefix + "no-game-job").c_str());
        Check(World() == worldBefore && ServiceCounters() == servicesBefore && HistoryState() == historyBefore,
            (prefix + "no-deferred-mutation").c_str());
    }

    const int ctBefore = camTraceCalls; Cmd("camtrace"); Check(camTraceCalls == ctBefore + 1 && camTraceSeconds == 16, "camtrace-service");
    const int ftBefore = fovTraceCalls; Cmd("fovtrace"); Check(fovTraceCalls == ftBefore + 1 && fovTraceSeconds == 12, "fovtrace-service");
    const int cwBefore = camWatchCalls; Cmd("camwatch 5 1"); Check(camWatchCalls == cwBefore + 1 && camWatchSeconds == 5 && camWatchMode == 1, "camwatch-arguments");
    Cmd("camwatch"); Check(camWatchCalls == cwBefore + 2 && camWatchSeconds == 8 && camWatchMode == 0, "camwatch-defaults");

    Cmd("raytrace"); Check(LogHas("[ray] tracing the next 12"), "raytrace-real-branch");

    const int castsBefore = casts; const size_t castListBefore = castCalls.size();
    Cmd("probe"); Drain();
    // the production probe path casts once to calibrate the sphere (this substitute never calibrates: the hit sits
    // at y=0, 10 m below the player) and then replays exactly the three queued casts.
    Check(casts == castsBefore + 4, "probe-real-casts-calibration-plus-three");
    Require(castCalls.size() == castListBefore + 4, "probe-cast-boundary");
    Check(Near(castCalls[castListBefore].start.y, 13) && Near(castCalls[castListBefore].len, 10), "probe-calibration-cast");
    Check(Near(castCalls[castListBefore + 1].start.y, 13) && Near(castCalls[castListBefore + 1].len, 10), "probe-first-ticket-arguments");
    Check(Near(castCalls[castListBefore + 2].start.y, 16) && Near(castCalls[castListBefore + 2].len, 10), "probe-second-ticket-arguments");
    Check(Near(castCalls[castListBefore + 3].start.y, 16) && Near(castCalls[castListBefore + 3].len, 30), "probe-third-ticket-arguments");

    const int ioBefore = ioTraceCalls; Cmd("traceio on"); Check(ioTraceCalls == ioBefore + 1 && ioTraceLast, "traceio-service");
    const int thBefore = thumbsCalls; Cmd("thumbs on"); Check(thumbsCalls == thBefore + 1 && thumbsLast && thumbgen::Background(), "thumbs-state-on");
    Cmd("thumbs off"); Check(!thumbgen::Background(), "thumbs-state-off");

    const int npcBefore = actorsRemoved;
    Cmd("npc 42"); Check(LogHas("[npc] not available"), "npc-real-branch");
    Check(actorsRemoved == npcBefore, "npc-host-not-available-no-side-effect");

    Cmd("captures"); Check(LogHas("captures=0"), "captures-real-branch");
    Cmd("replays"); Check(LogHas("replays=0"), "replays-real-branch");
    Cmd("replayprefab /object/good.prefab"); Check(std::string(core::GimmickReplayPrefab()) == kGood, "replayprefab-state");
    Cmd("replayprefab"); Check(std::string(core::GimmickReplayPrefab()).empty(), "replayprefab-cleared");
    Cmd("replay 7 1 2 3"); Check(core::GimmickReplayArmed(), "replay-armed");
    Cmd("removereplay 1234abcd"); Check(LogHas("removal of actor"), "removereplay-real-branch");

    const std::string beforeHelp = World();
    Check(!Cmd("help"), "help-continues");
    Check(World() == beforeHelp && !LogHas("unknown command 'help'"), "help-real-branch-no-unknown");
    const std::string beforeQuit = World();
    Check(Cmd("quit"), "quit-ends-loop");
    Check(World() == beforeQuit, "quit-no-mutation");
    Check(LogHas("pos: no player actor yet") && LogHas("status: creates=") && LogHas("captures=0"), "dispatch-log-receipts");
}

// ===================== happy: the dead Travel surface is gone and teleports.txt is never touched =====================
void TravelNoAccess() {
    Reset();
    const std::string sentinel = "sentinel-user-file|1.25|2.50|-3.75\n";
    const std::string sentinelPath = dir + "\\teleports.txt";
    Write(sentinelPath, sentinel);
    const std::string shaBefore = Sha256Hex(Read(sentinelPath));
    fio::Opens().clear();
    editor::g_collsLoaded = false;   // force the production collections open inside the traced boundary

    full = true; editor::g_open = true; editor::g_playMode = false; editor::g_compact = false;
    Click(ICON_MAGNIFYING_GLASS " Browser");
    Click(ICON_LOCATION_DOT " NPCs");
    Click(ICON_CUBE " Scene");
    Click(ICON_CLOCK_ROTATE_LEFT " History");
    Click(ICON_FLOPPY_DISK " Project");
    Click(ICON_LIST " Settings");
    Click(ICON_LIST " Log");
    Click(ICON_COPY " dock"); Frame();
    full = false;

    Cmd("tp 1 2 3"); Cmd("probe"); Cmd("spawn /object/good.prefab"); Drain();
    Cmd("save travel"); Cmd("load travel"); Drain();

    Check(fio::TeleportsAttempts() == 0, "no-teleports-open-attempt");
    Check(!fio::Opens().empty(), "file-open-trace-records-production-opens");
    Check(Read(sentinelPath) == sentinel && Sha256Hex(Read(sentinelPath)) == shaBefore, "sentinel-user-file-unchanged");
    Check(core::ModDir() == dir, "isolated-fixture-dir-never-game-folder");
    Check(!HasContaining("Travel"), "no-travel-control");
    Check(!LogHas("teleports.txt"), "no-teleports-log-reference");

    std::ostringstream s;
    s << "{\"case\":\"TRAVEL-NO-ACCESS\",\"boundary\":\"production editor fopen/fopen_s (instrumented before include)\""
      << ",\"totalOpenAttempts\":" << fio::Opens().size() << ",\"teleportsOpenAttempts\":" << fio::TeleportsAttempts()
      << ",\"sentinelPath\":\"" << J(sentinelPath) << "\",\"sentinelBytes\":" << sentinel.size()
      << ",\"sentinelSha256Before\":\"" << shaBefore << "\",\"sentinelSha256After\":\"" << Sha256Hex(Read(sentinelPath)) << "\""
      << ",\"openAttempts\":[";
    for (size_t i = 0; i < fio::Opens().size(); ++i) {
        if (i) s << ",";
        s << "{\"path\":\"" << J(fio::Opens()[i].path) << "\",\"mode\":\"" << J(fio::Opens()[i].mode) << "\",\"caller\":\"" << fio::Opens()[i].caller << "\"}";
    }
    s << "]}";
    Write(root + "\\file-access-trace.json", s.str());
}

// ===================== countercase: malformed input never mutates =====================
void Invalid() {
    Reset();
    const std::string before = World();
    for (const char* bad : { "tp 1 2", "move 1 2 3", "spawn   ", "replay abc", "removereplay zz", "hide x" }) {
        Check(!Cmd(bad), "invalid-not-quit");
        Check(World() == before, "invalid-no-scene-history-mutation");
    }
    Check(LogHas("usage: tp x y z") && LogHas("usage: move <idx>") && LogHas("usage: spawn <prefab>"), "invalid-usage-branches");
    Cmd("spawn /object/good.prefab"); Drain();
    Check(core::Spawned().size() == 1, "valid-command-after-invalid");
}

// ===================== countercase: unknown lines are inert =====================
void Unknown() {
    Reset();
    const std::string before = World();
    Check(!Cmd("frobnicate"), "unknown-not-quit");
    Check(!Cmd(""), "empty-line-not-quit");
    Check(!Cmd("trace maybe"), "unknown-trace-argument-not-quit");
    Check(World() == before, "unknown-no-scene-history-mutation");
    Check(LogHas("unknown command 'frobnicate'"), "unknown-real-branch");
    Check(!LogHas("unknown command ''"), "empty-line-silent");
    Cmd("spawn /object/good.prefab"); Drain();
    Check(core::Spawned().size() == 1, "valid-command-after-unknown");
}

// ===================== countercase: a rejected load changes nothing =====================
void BadLoad() {
    Reset();
    const std::string validRow = std::string(kGood) + "|0|5|0\n";
    Write(dir + "\\projects\\scene1.cdproj", validRow);
    Write(dir + "\\projects\\bad.cdproj", validRow + std::string(kGood) + "|bad|1|0\n");
    const std::string badBytes = Read(dir + "\\projects\\bad.cdproj");
    const std::string before = World();

    Cmd("load bad");
    Check(World() == before, "bad-load-no-scene-history-mutation");
    Check(core::PendingSpawns() == 0, "bad-load-no-queue");
    Check(Read(dir + "\\projects\\bad.cdproj") == badBytes, "bad-load-file-untouched");
    Check(!core::ProjectLoadReportFor("bad").valid, "bad-load-no-success-receipt");

    Cmd("load missing");
    Check(World() == before, "missing-load-no-scene-history-mutation");

    Cmd("load scene1"); Drain();
    Check(core::Spawned().size() == 1, "valid-load-after-bad");
    Check(core::ProjectLoadReportFor("scene1").queued == 1, "valid-load-receipt");
}

// ===================== countercase: without the pump nothing mutates =====================
void GameNotReady() {
    Reset();
    Write(dir + "\\projects\\scene1.cdproj", std::string(kGood) + "|0|5|0\n");
    host::Seam().ready = false; host::Seam().probeReady = false;
    const std::string before = World(); const int castsBefore = casts;
    Check(!Cmd("spawn /object/good.prefab"), "not-ready-spawn-not-quit");
    Check(!Cmd("move 0 1 2 3"), "not-ready-move-not-quit");
    Cmd("probe");
    Cmd("load scene1");
    Drain();
    Check(World() == before, "not-ready-no-scene-history-mutation");
    Check(casts == castsBefore, "not-ready-no-probe-cast");
    Check(LogHas("spawn: game thread pump not active yet"), "not-ready-spawn-branch");
    Check(LogHas("[probe] no template yet"), "not-ready-probe-branch");
    Check(core::ProjectLoadReportFor("scene1").queued == 0, "not-ready-load-no-queue");
    host::Seam().ready = true; host::Seam().probeReady = true;
    Cmd("spawn /object/good.prefab"); Drain();
    Check(core::Spawned().size() == 1, "ready-spawn-after-not-ready");
}

// ===================== C7: the console move keeps the pre-mutation reconciliation =====================
void C7Admission() {
    Reset();
    Cmd("spawn /object/good.prefab"); Drain();
    Require(core::Spawned().size() == 1, "c7-target-spawned");
    const int uid = core::Spawned().front().uid;
    auto ops = editor::BeginGrounding({ uid }, true, {});
    Require(ops.size() == 1, "c7-unapplied-ground-reserved");
    Check(core::GroundStateOf(ops[0]).state == core::GroundProbing, "c7-probing-before-console-move");
    Cmd("move 0 3 4 5"); Drain();
    Check(core::GroundStateOf(ops[0]).reason == "epoch-invalidated", "c7-console-move-invalidates-unapplied-ground");
    Check(Near(Rec(uid).pos.x, 3) && Near(Rec(uid).pos.y, 4) && Near(Rec(uid).pos.z, 5), "c7-move-still-admitted");
    const int tpBefore = teleports; Cmd("tp 1 1 1");
    Check(teleports == tpBefore + 1, "c7-teleport-after-invalidated-probe");
}

// ===================== v0.95: real trace controls and persisted next-start console opt-in =====================
void TraceWidget() {
    struct RestoreConsole { bool value = core::g_showConsole; ~RestoreConsole() { core::g_showConsole = value; } } restoreConsole;
    Reset();
    full = true; editor::g_open = true; editor::g_playMode = false; editor::g_compact = false;
    // Translated labels locate production items; assertions use state, flags and parsed settings, not prose.
    const std::string trace = i18n::T("trace game calls (writes to cdmodkit.log; turn on, move an object in the housing editor, turn off)");
    Click(i18n::TStable(ICON_LIST " Log")); ReachCheckbox(trace);
    Check(!core::Trace() && !Checked(trace), "trace-initial-off");
    ClickCheckbox(trace);
    Check(core::Trace() && Checked(trace), "trace-mouse-on");
    ClickCheckbox(trace);
    Check(!core::Trace() && !Checked(trace), "trace-mouse-off");
    Check(!Cmd("trace on"), "trace-command-on-nonquit"); Frame();
    Check(core::Trace() && Checked(trace), "trace-command-on-widget-checked");
    Check(!Cmd("trace off"), "trace-command-off-nonquit"); Frame();
    Check(!core::Trace() && !Checked(trace), "trace-command-off-widget-unchecked");

    const HWND consoleWindow = GetConsoleWindow();
    core::g_showConsole = false;
    Require(core::ModDir() == dir, "console-settings-scratch-dir");
    Require(GetFileAttributesA((dir + "\\settings.txt").c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "console-settings-start-without-file");
    const std::string console = i18n::T("console window (log output; applies on the next start)");
    Click(i18n::TStable(ICON_LIST " Settings")); ReachCheckbox(console);
    Check(!core::g_showConsole && !Checked(console), "console-initial-off");
    ClickCheckbox(console);
    Check(core::g_showConsole && Checked(console), "console-mouse-on");
    Check(SavedConsoleSetting() == 1, "console-production-saved-one");
    ClickCheckbox(console);
    Check(!core::g_showConsole && !Checked(console), "console-mouse-off");
    Check(SavedConsoleSetting() == 0, "console-production-saved-zero");
    Check(GetConsoleWindow() == consoleWindow, "console-next-start-no-window-launch");
    full = false;
}

void Run(const char* name, const std::function<void()>& fn) {
    current = name; int before = assertions, failed = failures;
    try { fn(); } catch (const std::exception& e) { Check(false, e.what()); }
    std::ostringstream s;
    s << "{\"id\":\"" << current << "\",\"assertions\":" << (assertions - before) << ",\"failures\":" << (failures - failed)
      << ",\"status\":\"" << ((failures - failed) == 0 ? "PASS" : "FAIL") << "\"}";
    cases.push_back(s.str());
}
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (id) { auto& item = items[id]; item.id = id; item.box = bb; item.disabled = (ctx->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
        item.window = ctx->CurrentWindow; item.clip = ctx->CurrentWindow->ClipRect;
        if (ctx->CurrentWindow) item.stack.assign(ctx->CurrentWindow->IDStack.begin(), ctx->CurrentWindow->IDStack.end()); }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags status) { items[id].label = label; items[id].status = status; }
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) { auto it = items.find(id); return it == items.end() ? nullptr : it->second.label.c_str(); }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    root = argv[1]; host::SetModDir(root); host::OpenLog(root + "\\editor_cleanup.log");
    ImGui::CreateContext(); GImGui->TestEngineHookItems = true;
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DeltaTime = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false; // each scripted frame owns its complete pointer/button sample
    auto* font = io.Fonts->AddFontDefault(); icons::Register(io.Fonts, font, 13); io.Fonts->Build(); icons::Paint(io.Fonts);
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    editor::ApplyStyle(1); InstallEngine();

    Run("CONSOLE", ConsoleCommands);
    Run("TRAVEL-NO-ACCESS", TravelNoAccess);
    Run("INVALID", Invalid);
    Run("UNKNOWN", Unknown);
    Run("BAD-LOAD", BadLoad);
    Run("GAME-NOT-READY", GameNotReady);
    Run("C7-ADMISSION", C7Admission);
    Run("TRACE-WIDGET", TraceWidget);

    Array("cases.json", cases);
    Array("console-trace.json", dispatch);
    printf("ASSERTIONS=%d\nFAILURES=%d\n", assertions, failures);
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
