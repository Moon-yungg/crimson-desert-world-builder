// Play mode: start the game straight into an isolated world at a requested position - terrain, sky, weather, the player
// character, the camera and World Builder's own objects stay, the world's own content around the player is not created.
//
// Request file bin64\cdmodkit\playmode.json (format: notes/FORMATS.md "Play mode"). It is read once at attach and renamed to
// playmode.last.json right away, so a crash or a forced quit never traps the user in play mode: the next start is normal.
// Without the file nothing in here is installed (no hook, no IAT patch) and the game behaves exactly as before.
//
// Pieces (all resolved at runtime, nothing hardcoded):
// - auto continue: presses "continue" (E) on the title screen from inside the process while the game window is in front,
//   until the load starts.
// - destination: the game's own fast travel (travel.cpp) right after the first load, one more loading screen. The first load
//   itself cannot be redirected yet (the "directLoad" experiment below places the player but the game resets it).
// - isolation: (1) the server's actor creation (ServerField slot 17, "field create", the one point every desc list passes)
//   drops descs whose spawn reason (ICreateServerActorDesc +0xA, enum names registered by the game) is a world spawn: level
//   actors, NPC schedules, auto spawns, faction spawns, ambient animals. (2) The world's level files (sector levels and named
//   location levels) are loaded from an empty level of the game's own, so their buildings, props and proxy meshes - with their
//   collision - never exist. Terrain, sky, sea and the global trigger levels stay. Our own spawns are never filtered.
// - save guard: the game's file imports (IAT of CrimsonDesert.exe only) refuse every mutating access below the save folder,
//   and the server's save requests / timers do not run.
// - our scene: the requested objects (or a World Builder project) are spawned through the normal spawn path once the
//   player stands at the destination.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace core {

// ---- tiny JSON reader (the request is nested: objects, arrays) --------------------------------------------------------
namespace {
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s; std::vector<JVal> a; std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const { if (t != Obj) return nullptr; for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
    double num(double def) const { return t == Num ? n : t == Bool ? (b ? 1 : 0) : def; }
    bool truthy(bool def) const { return t == Bool ? b : t == Num ? n != 0 : def; }
};
struct JParser {
    const std::string& s; size_t p = 0; bool ok = true;
    explicit JParser(const std::string& str) : s(str) {}
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }
    bool str(std::string& out) {
        if (p >= s.size() || s[p] != '"') return false; ++p; out.clear();
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) return false;
            char e = s[p++];
            if (e == 'n') out += '\n'; else if (e == 't') out += '\t'; else if (e == 'r') out += '\r'; else if (e == 'b') out += '\b'; else if (e == 'f') out += '\f';
            else if (e == 'u') { if (p + 4 > s.size()) return false; unsigned u = (unsigned)strtoul(s.substr(p, 4).c_str(), nullptr, 16); p += 4;
                if (u < 0x80) out += (char)u; else if (u < 0x800) { out += (char)(0xC0 | (u >> 6)); out += (char)(0x80 | (u & 63)); }
                else { out += (char)(0xE0 | (u >> 12)); out += (char)(0x80 | ((u >> 6) & 63)); out += (char)(0x80 | (u & 63)); } }
            else out += e;
        }
        return false;
    }
    bool val(JVal& v, int depth) {
        if (depth > 32) return false;
        ws(); if (p >= s.size()) return false;
        const char c = s[p];
        if (c == '{') { ++p; v.t = JVal::Obj; ws(); if (p < s.size() && s[p] == '}') { ++p; return true; }
            for (;;) { ws(); std::string k; if (!str(k)) return false; ws(); if (p >= s.size() || s[p++] != ':') return false;
                JVal x; if (!val(x, depth + 1)) return false; v.o.emplace_back(std::move(k), std::move(x)); ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; } if (p < s.size() && s[p] == '}') { ++p; return true; } return false; } }
        if (c == '[') { ++p; v.t = JVal::Arr; ws(); if (p < s.size() && s[p] == ']') { ++p; return true; }
            for (;;) { JVal x; if (!val(x, depth + 1)) return false; v.a.push_back(std::move(x)); ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; } if (p < s.size() && s[p] == ']') { ++p; return true; } return false; } }
        if (c == '"') { v.t = JVal::Str; return str(v.s); }
        if (s.compare(p, 4, "true") == 0) { p += 4; v.t = JVal::Bool; v.b = true; return true; }
        if (s.compare(p, 5, "false") == 0) { p += 5; v.t = JVal::Bool; v.b = false; return true; }
        if (s.compare(p, 4, "null") == 0) { p += 4; v.t = JVal::Null; return true; }
        char* end = nullptr; const double d = strtod(s.c_str() + p, &end);
        if (!end || end == s.c_str() + p) return false;
        p = (size_t)(end - s.c_str()); v.t = JVal::Num; v.n = d; return std::isfinite(d);
    }
};
static bool ParseJson(const std::string& text, JVal& out) {
    size_t start = (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) ? 3 : 0;   // UTF-8 BOM
    const std::string body = text.substr(start);
    JParser jp(body); if (!jp.val(out, 0)) return false; jp.ws(); return jp.p == body.size();
}
static bool Vec3Of(const JVal* v, Vec3& out) {   // [x,y,z] or {"x","y","z"}
    if (!v) return false;
    if (v->t == JVal::Arr && v->a.size() >= 3) { out = { (float)v->a[0].num(0), (float)v->a[1].num(0), (float)v->a[2].num(0) }; return true; }
    if (v->t == JVal::Obj && v->get("x") && v->get("y") && v->get("z")) { out = { (float)v->get("x")->num(0), (float)v->get("y")->num(0), (float)v->get("z")->num(0) }; return true; }
    return false;
}
}   // namespace

// ---- the request -------------------------------------------------------------------------------------------------------
struct PmObject { std::string prefab; Vec3 pos{}; Rot rot{}; float scale = 1.0f; };
struct PmRequest {
    bool enabled = false;
    bool hasSpawn = false; Vec3 spawn{}; float yaw = 0;
    bool isolate = true;             // master switch for the gates below
    bool isolateActors = true;       // server actors with a world spawn reason (NPCs, level actors, animals, faction spawns)
    bool isolateObjects = true;      // client scene objects (createSceneObjectFrom): census only, the world creates none there
    float radius = 0;                // > 0: sector levels farther than this from the spawn point stay (0 = everywhere)
    bool autoContinue = true;
    bool directLoad = false;         // experiment: redirect the first load on the server (see InstallDirectLoad); off = fast travel after it
    bool blockSave = true;
    std::vector<PmObject> objects;
    std::string project;
    std::vector<std::string> keep;   // extra prefab path prefixes that are never filtered
    int isolateLevels = 2;           // 0 off, 1 sector levels, 2 sector levels + named location levels (towns, shops, quests, phases)
    std::vector<std::string> keepLevels;   // level path substrings that always load
    std::set<int> blockReasons;      // spawn reasons the actor gate drops
};
static PmRequest g_req;
static std::atomic<bool> g_active{ false };       // a request was consumed this session
static std::atomic<bool> g_isolating{ false };    // gates are filtering right now
static std::atomic<int> g_phase{ 0 };
enum { PhOff = 0, PhTitle, PhLoading, PhInWorld, PhTravel, PhArrived, PhReady, PhExited };
static const char* PhaseName(int p) {
    switch (p) { case PhOff: return "off"; case PhTitle: return "title"; case PhLoading: return "loading"; case PhInWorld: return "in_world";
        case PhTravel: return "travelling"; case PhArrived: return "arrived"; case PhReady: return "ready"; case PhExited: return "exited"; }
    return "?";
}
static std::mutex g_mx;
static std::string g_status = "off", g_requestError;
static std::atomic<bool> g_directFirstLoad{ false };    // the first load was redirected (directLoad experiment, or a stage reload of the game's own)
static std::atomic<bool> g_rootLevelSeen{ false };      // the load asked for the world root level
static std::atomic<long> g_continuePresses{ 0 };
static Vec3 g_loadedAt{}; static bool g_haveLoadedAt = false;   // where the save put the player (exit by travel goes back here)
static std::atomic<long> g_savesBlocked{ 0 }, g_actorsBlocked{ 0 }, g_actorsAllowed{ 0 }, g_objectsBlocked{ 0 }, g_objectsAllowed{ 0 };
static std::vector<int> g_sceneUids; static bool g_sceneSpawned = false;

static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(g_mx); if (g_status != s) Log("[playmode] %s", s.c_str()); g_status = s; }
bool PlayModeActive() { return g_active.load(); }
bool PlayModeIsolating() { return g_isolating.load(); }

static std::string ReadFileText(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return {};
    std::string s; char buf[8192]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (1u << 22)) s.append(buf, n);
    fclose(f); return s;
}

// Spawn reasons (ICreateServerActorDesc +0xA). The names are the game's own enum registration (41 values): 0 Level,
// 1 Sequencer, 2..11 AutoSpawn_* (Bird, SidWalk, Wagon, TerrainRegion, SpawningPool_Near/Socket/Point, MeshGroup,
// FactionPatrol, FactionPosition), 12 NPCSchedule, 13 Summon, 14 SummonMercenary, 15 SummonGimmick, 16 CombinationGimmick,
// 17..20 Equip/Installation/GuideEffect/PortalGimmick, 21 DropItem, 22 DiscardItemFromInventory, 23 Housing, 24 Craft,
// 25 ThrowEquip, 26/27 DropFromDeadBody(Vehicle), 28 Transmutation, 29 Inspect, 30 GlobalGameActor, 31 SceneCollectSpawn
// (the level's gimmicks), 32 Summon_LinkedVehicle, 33 Cheat, 34..37 Editor_*, 38 AutoSpawn_SpawningPool_ActionPoint,
// 39 DailyRoutine. The default gate drops what the world places by itself; the player's equipment, items, summons, our
// cheat-request NPCs and our gimmicks (spawned with reason 31, but flagged as ours) pass. Sequencer (1) is dropped too: the
// ambient animals (sequencer/.../animallife/cd_seq_spawn_*) come that way.
static const char* kReasonNames[] = { "Level", "Sequencer", "AutoSpawn_Bird", "AutoSpawn_SidWalk", "AutoSpawn_Wagon", "AutoSpawn_TerrainRegion",
    "AutoSpawn_SpawningPool_Near", "AutoSpawn_SpawningPool_Socket", "AutoSpawn_SpawningPool_Point", "AutoSpawn_MeshGroup", "AutoSpawn_FactionPatrol",
    "AutoSpawn_FactionPosition", "NPCSchedule", "Summon", "SummonMercenary", "SummonGimmick", "CombinationGimmick", "EquipDockingGimmick",
    "InstallationGimmick", "GuideEffectGimmick", "PortalGimmick", "DropItem", "DiscardItemFromInventory", "Housing", "Craft", "ThrowEquip",
    "DropFromDeadBody", "DropFromDeadBodyVehicle", "Transmutation", "Inspect", "GlobalGameActor", "SceneCollectSpawn", "Summon_LinkedVehicle",
    "Cheat", "Editor_GimmickEventExecutable", "Editor_EquipDockingGimmickEventExecutable", "Editor_AttackDebugAttackTarget",
    "Editor_AttackDebugAttacker", "AutoSpawn_SpawningPool_ActionPoint", "DailyRoutine" };
static const int kReasonCount = (int)(sizeof kReasonNames / sizeof kReasonNames[0]);
static const char* ReasonName(int r) { return r >= 0 && r < kReasonCount ? kReasonNames[r] : "?"; }
static int ReasonByName(const std::string& n) { for (int i = 0; i < kReasonCount; i++) if (_stricmp(kReasonNames[i], n.c_str()) == 0) return i; return -1; }
static void DefaultBlockReasons(std::set<int>& r) {
    r = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 31, 38, 39 };
}

static bool ParseRequest(const std::string& text, PmRequest& r, std::string& err) {
    JVal j; if (!ParseJson(text, j) || j.t != JVal::Obj) { err = "not a JSON object"; return false; }
    r.enabled = j.get("enabled") ? j.get("enabled")->truthy(false) : true;
    if (const JVal* sp = j.get("spawn")) {
        Vec3 v{}; if (!Vec3Of(sp, v)) { err = "spawn needs x, y, z"; return false; }
        r.spawn = v; r.hasSpawn = true; if (sp->t == JVal::Obj && sp->get("yaw")) r.yaw = (float)sp->get("yaw")->num(0);
        if (fabsf(v.x) > 200000 || fabsf(v.z) > 200000 || fabsf(v.y) > 20000) { err = "spawn position out of range"; return false; }
    }
    if (const JVal* v = j.get("isolate")) r.isolate = v->truthy(true);
    if (const JVal* v = j.get("isolateActors")) r.isolateActors = v->truthy(true);
    if (const JVal* v = j.get("isolateObjects")) r.isolateObjects = v->truthy(true);
    if (const JVal* v = j.get("radius")) r.radius = (float)std::max(0.0, v->num(0));
    if (const JVal* v = j.get("autoContinue")) r.autoContinue = v->truthy(true);
    if (const JVal* v = j.get("directLoad")) r.directLoad = v->truthy(false);
    if (const JVal* v = j.get("blockSave")) r.blockSave = v->truthy(true);
    if (const JVal* v = j.get("project")) { if (v->t != JVal::Str) { err = "project must be a string"; return false; } r.project = v->s; }
    if (const JVal* v = j.get("keep")) if (v->t == JVal::Arr) for (auto& k : v->a) if (k.t == JVal::Str && !k.s.empty()) r.keep.push_back(k.s);
    if (const JVal* v = j.get("keepLevels")) if (v->t == JVal::Arr) for (auto& k : v->a) if (k.t == JVal::Str && !k.s.empty()) r.keepLevels.push_back(k.s);
    if (const JVal* v = j.get("isolateLevels")) r.isolateLevels = v->t == JVal::Str ? (v->s == "sector" ? 1 : v->s == "all" ? 2 : 0) : (v->truthy(true) ? 2 : 0);
    DefaultBlockReasons(r.blockReasons);
    if (const JVal* v = j.get("blockReasons")) if (v->t == JVal::Arr) {
        r.blockReasons.clear();
        for (auto& k : v->a) { int id = k.t == JVal::Num ? (int)k.n : k.t == JVal::Str ? ReasonByName(k.s) : -1; if (id >= 0 && id < 256) r.blockReasons.insert(id); }
    }
    if (const JVal* v = j.get("objects")) {
        if (v->t != JVal::Arr) { err = "objects must be an array"; return false; }
        for (size_t i = 0; i < v->a.size(); i++) {
            const JVal& o = v->a[i]; PmObject po;
            const JVal* pf = o.get("prefab"); if (!pf || pf->t != JVal::Str || pf->s.empty()) { err = "objects[" + std::to_string(i) + "].prefab missing"; return false; }
            po.prefab = pf->s;
            if (!Vec3Of(o.get("pos"), po.pos)) { err = "objects[" + std::to_string(i) + "].pos needs [x,y,z]"; return false; }
            Vec3 rr{}; if (Vec3Of(o.get("rot"), rr)) po.rot = Rot{ rr.x, rr.y, rr.z };   // degrees: yaw, pitch, roll (World Builder order)
            else if (const JVal* y = o.get("yaw")) po.rot.yaw = (float)y->num(0);
            if (const JVal* sc = o.get("scale")) { Vec3 s3{}; po.scale = Vec3Of(sc, s3) ? s3.x : (float)sc->num(1); }   // uniform: the first component of [sx,sy,sz]
            if (!(po.scale > 0.001f && po.scale < 1000.0f)) po.scale = 1.0f;
            r.objects.push_back(std::move(po));
        }
    }
    return true;
}

// ---- save guard: the exe's own file imports refuse writes below the save folder ------------------------------------------
static std::wstring g_saveRootLower;   // "...\pearl abyss\cd\save" in lower case
static bool IsSavePath(const wchar_t* p) {
    if (!p) return false;
    std::wstring s(p); std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (!g_saveRootLower.empty() && s.find(g_saveRootLower) != std::wstring::npos) return true;
    return s.find(L"\\pearl abyss\\cd\\save") != std::wstring::npos;
}
static bool IsSavePathA(const char* p) {
    if (!p) return false; wchar_t w[1024]; if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, 1024)) return false; return IsSavePath(w);
}
static bool WriteAccess(DWORD access, DWORD disposition) {
    const DWORD writeBits = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    return (access & writeBits) != 0 || (disposition != OPEN_EXISTING && disposition != 0);
}
static bool GuardOn() { return g_active.load() && g_req.blockSave; }
static void NoteBlocked(const char* op, const wchar_t* path) {
    const long n = ++g_savesBlocked;
    if (n <= 200) { char p[600]; WideCharToMultiByte(CP_UTF8, 0, path ? path : L"", -1, p, sizeof p, nullptr, nullptr); Log("[playmode] save guard: %s refused: %s", op, p); }
}
typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFile2Fn)(LPCWSTR, DWORD, DWORD, DWORD, void*);
typedef BOOL(WINAPI* DeleteFileWFn)(LPCWSTR);
typedef BOOL(WINAPI* MoveFileExWFn)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI* CopyFileWFn)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL(WINAPI* DirWFn)(LPCWSTR);
typedef BOOL(WINAPI* CreateDirWFn)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL(WINAPI* SetAttrWFn)(LPCWSTR, DWORD);
typedef BOOL(WINAPI* SetAttrAFn)(LPCSTR, DWORD);
static CreateFileWFn o_CreateFileW = nullptr; static CreateFileAFn o_CreateFileA = nullptr; static CreateFile2Fn o_CreateFile2 = nullptr;
static DeleteFileWFn o_DeleteFileW = nullptr; static MoveFileExWFn o_MoveFileExW = nullptr; static CopyFileWFn o_CopyFileW = nullptr;
static DirWFn o_RemoveDirectoryW = nullptr; static CreateDirWFn o_CreateDirectoryW = nullptr; static SetAttrWFn o_SetFileAttributesW = nullptr; static SetAttrAFn o_SetFileAttributesA = nullptr;
static HANDLE WINAPI G_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    if (GuardOn() && WriteAccess(a, d) && IsSavePath(p)) { NoteBlocked("CreateFileW", p); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return o_CreateFileW(p, a, s, sa, d, f, t);
}
static HANDLE WINAPI G_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    if (GuardOn() && WriteAccess(a, d) && IsSavePathA(p)) { NoteBlocked("CreateFileA", L"(ansi path)"); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return o_CreateFileA(p, a, s, sa, d, f, t);
}
static HANDLE WINAPI G_CreateFile2(LPCWSTR p, DWORD a, DWORD s, DWORD d, void* x) {
    if (GuardOn() && WriteAccess(a, d) && IsSavePath(p)) { NoteBlocked("CreateFile2", p); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return o_CreateFile2(p, a, s, d, x);
}
static BOOL WINAPI G_DeleteFileW(LPCWSTR p) { if (GuardOn() && IsSavePath(p)) { NoteBlocked("DeleteFileW", p); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_DeleteFileW(p); }
static BOOL WINAPI G_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) { if (GuardOn() && (IsSavePath(a) || IsSavePath(b))) { NoteBlocked("MoveFileExW", a); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_MoveFileExW(a, b, f); }
static BOOL WINAPI G_CopyFileW(LPCWSTR a, LPCWSTR b, BOOL f) { if (GuardOn() && IsSavePath(b)) { NoteBlocked("CopyFileW", b); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_CopyFileW(a, b, f); }
static BOOL WINAPI G_RemoveDirectoryW(LPCWSTR p) { if (GuardOn() && IsSavePath(p)) { NoteBlocked("RemoveDirectoryW", p); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_RemoveDirectoryW(p); }
static BOOL WINAPI G_CreateDirectoryW(LPCWSTR p, LPSECURITY_ATTRIBUTES sa) { if (GuardOn() && IsSavePath(p)) { NoteBlocked("CreateDirectoryW", p); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_CreateDirectoryW(p, sa); }
static BOOL WINAPI G_SetFileAttributesW(LPCWSTR p, DWORD a) { if (GuardOn() && IsSavePath(p)) { NoteBlocked("SetFileAttributesW", p); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_SetFileAttributesW(p, a); }
static BOOL WINAPI G_SetFileAttributesA(LPCSTR p, DWORD a) { if (GuardOn() && IsSavePathA(p)) { NoteBlocked("SetFileAttributesA", L"(ansi path)"); SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return o_SetFileAttributesA(p, a); }

// Patches every import slot of the exe whose name matches (by name, through OriginalFirstThunk; several import descriptors
// may name the same function). Only the game's own calls are affected, other modules keep the real functions.
static int PatchImport(const char* name, void* detour, void** orig) {
    const uintptr_t base = g_base; auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk); auto* slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, name) != 0) continue;
            void* cur = (void*)slots->u1.Function; if (cur == detour) continue;
            if (!*orig) *orig = cur;
            DWORD old = 0; if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) continue;
            slots->u1.Function = (ULONGLONG)detour; VirtualProtect(&slots->u1.Function, 8, old, &old); patched++;
        }
    }
    return patched;
}
static void InstallSaveGuard() {
    PWSTR la = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)) && la) {
        std::wstring s = std::wstring(la) + L"\\Pearl Abyss\\CD\\save"; CoTaskMemFree(la);
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); }); g_saveRootLower = s;
    }
    int n = 0;
    n += PatchImport("CreateFileW", (void*)G_CreateFileW, (void**)&o_CreateFileW);
    n += PatchImport("CreateFileA", (void*)G_CreateFileA, (void**)&o_CreateFileA);
    n += PatchImport("CreateFile2", (void*)G_CreateFile2, (void**)&o_CreateFile2);
    n += PatchImport("DeleteFileW", (void*)G_DeleteFileW, (void**)&o_DeleteFileW);
    n += PatchImport("MoveFileExW", (void*)G_MoveFileExW, (void**)&o_MoveFileExW);
    n += PatchImport("CopyFileW", (void*)G_CopyFileW, (void**)&o_CopyFileW);
    n += PatchImport("RemoveDirectoryW", (void*)G_RemoveDirectoryW, (void**)&o_RemoveDirectoryW);
    n += PatchImport("CreateDirectoryW", (void*)G_CreateDirectoryW, (void**)&o_CreateDirectoryW);
    n += PatchImport("SetFileAttributesW", (void*)G_SetFileAttributesW, (void**)&o_SetFileAttributesW);
    n += PatchImport("SetFileAttributesA", (void*)G_SetFileAttributesA, (void**)&o_SetFileAttributesA);
    char root[600] = {}; WideCharToMultiByte(CP_UTF8, 0, g_saveRootLower.c_str(), -1, root, sizeof root, nullptr, nullptr);
    Log("[playmode] save guard: %d import slots patched, save folder %s", n, root);
    if (!o_CreateFileW && !o_CreateFile2) Log("[playmode] save guard WARNING: no file-open import found, saving is NOT blocked");
}

// Second line of the save guard: the server's save requests and timers (TrocTr* classes, execute = vtable slot 2) do not
// run while play mode is active, so the game does not even try (and shows no "save failed"). Some of them are thunks into
// the protected part of the exe; hooking the slot target still works. Each is optional: the file guard stays the authority.
typedef void* (__fastcall* SaveExecFn)(void*, void*, void*, void*);
static SaveExecFn g_origSaveExec[5] = {};
static const char* kSaveReqs[5] = { ".?AVTrocTrSaveAutoReq@pa@@", ".?AVTrocTrSaveToFileCurrentPlayerReq@pa@@", ".?AVTrocTrSaveGameDataAutoTimer@pa@@",
    ".?AVTrocTrGamePlaySaveDataRepeatTimer@pa@@", ".?AVTrocTrFlushPendingSaveDataOnceTimer@pa@@" };
static std::atomic<long> g_saveReqsSkipped{ 0 };
template<int K> static void* __fastcall HookSaveExec(void* a, void* b, void* c, void* d) {
    if (GuardOn()) {
        const long n = ++g_saveReqsSkipped;
        if (n <= 50) Log("[playmode] save guard: %s skipped", kSaveReqs[K] + 4);
        return b;   // nothing is written: the request's result slot keeps what the dispatcher put there (0 = handled)
    }
    return g_origSaveExec[K](a, b, c, d);
}
static void InstallSaveRequestGate() {
    void* det[5] = { (void*)&HookSaveExec<0>, (void*)&HookSaveExec<1>, (void*)&HookSaveExec<2>, (void*)&HookSaveExec<3>, (void*)&HookSaveExec<4> };
    int ok = 0;
    for (int k = 0; k < 5; k++) {
        const uintptr_t vt = VtableByName(kSaveReqs[k]); uintptr_t f = 0;
        if (!vt || !ReadBytes(vt + 16, &f, 8) || !f) { Log("[playmode] save guard: %s not found", kSaveReqs[k]); continue; }
        if (InstallInternalHook((void*)f, det[k], (void**)&g_origSaveExec[k], kSaveReqs[k] + 4)) ok++;
    }
    Log("[playmode] save guard: %d of 5 save requests gated", ok);
}

// ---- request load (at attach, before any game code ran) ------------------------------------------------------------------
void PlayModeLoad() {
    const std::string path = ModDir() + "\\playmode.json", last = ModDir() + "\\playmode.last.json";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::string text = ReadFileText(path);
    // consumed first: whatever happens during this session, the next start is a normal one
    if (!MoveFileExA(path.c_str(), last.c_str(), MOVEFILE_REPLACE_EXISTING)) { Log("[playmode] request %s could not be consumed (error %lu); ignored so it cannot trap the next start", path.c_str(), GetLastError()); DeleteFileA(path.c_str()); return; }
    PmRequest r; std::string err;
    if (!ParseRequest(text, r, err)) { g_requestError = err; Log("[playmode] request rejected: %s (renamed to playmode.last.json)", err.c_str()); return; }
    if (!r.enabled) { Log("[playmode] request has enabled=false: normal start"); return; }
    g_req = r; g_active = true; g_phase = PhTitle;
    std::string reasons; for (int x : r.blockReasons) { if (!reasons.empty()) reasons += ","; reasons += ReasonName(x); }
    Log("[playmode] REQUEST consumed: spawn %s (%.1f %.1f %.1f) yaw %.0f, isolate %d (actors %d, objects %d, radius %.0f), %zu objects, project \"%s\", auto continue %d, block save %d; blocked reasons: %s",
        r.hasSpawn ? "at" : "none", r.spawn.x, r.spawn.y, r.spawn.z, r.yaw, r.isolate, r.isolateActors, r.isolateObjects, r.radius, r.objects.size(), r.project.c_str(), r.autoContinue, r.blockSave, reasons.c_str());
    if (r.blockSave) InstallSaveGuard();
    if (r.isolate) g_isolating = true;   // from the first load on: nothing of the world near the save position is created either
    SetStatus("waiting for the title screen");
}

// ---- actor gate: ServerField slot 17 (field create) -----------------------------------------------------------------------
// int* create(ServerField* field, int* result, {desc** data; u32 count}* list, void*). The game itself filters this list
// (when a global switch is set it copies the passing descs into a local vector and, if none is left, stores an error code in
// *result and returns), so callers already handle a shortened list and an empty one. The gate does the same before the call.
typedef int* (__fastcall* FieldCreateFn)(void* field, int* result, void* list, void* x);
static FieldCreateFn g_origFieldCreate = nullptr;
struct DescList { void** data; uint32_t count; uint32_t cap; };
static std::mutex g_censusMx;
struct ActorCensus { long seen = 0, blocked = 0; std::string cls; };
static std::map<int, ActorCensus> g_actorCensus;               // by reason
static std::map<uintptr_t, long> g_fieldCallers;               // caller rva -> calls
static uint32_t g_blockCode = 0;
static thread_local bool t_ourSpawn = false;
void PlayModeOurSpawn(bool on) { t_ourSpawn = on; }
static bool DescReason(void* desc, int* reason, const char** cls) {
    uint8_t r = 0; if (!desc || !ReadBytes((uintptr_t)desc + 0xA, &r, 1)) return false;
    *reason = r; *cls = RttiName((uintptr_t)desc); return true;
}
static int* __fastcall HookFieldCreate(void* field, int* result, void* list, void* x) {
    DescList* dl = (DescList*)list; uint32_t n = 0; void** data = nullptr;
    if (dl) { ReadBytes((uintptr_t)&dl->count, &n, 4); ReadBytes((uintptr_t)&dl->data, &data, 8); }
    const bool ours = t_ourSpawn || OurServerSpawnOnThisThread();
    const bool gate = g_isolating.load() && g_req.isolateActors && !ours && n > 0 && n < 4096 && data;
    { const uintptr_t ret = (uintptr_t)_ReturnAddress(); std::lock_guard<std::mutex> l(g_censusMx); g_fieldCallers[ret - g_base]++;
      for (uint32_t i = 0; i < n && i < 4096 && data; i++) { void* d = nullptr; if (!ReadBytes((uintptr_t)(data + i), &d, 8)) break; int r = -1; const char* c = nullptr; if (!DescReason(d, &r, &c)) continue;
          auto& e = g_actorCensus[r]; e.seen++; if (e.cls.empty() && c) e.cls = c; } }
    if (!gate) return g_origFieldCreate(field, result, list, x);
    std::vector<void*> keep; keep.reserve(n); int dropped = 0;
    for (uint32_t i = 0; i < n; i++) {
        void* d = nullptr; ReadBytes((uintptr_t)(data + i), &d, 8); int r = -1; const char* c = nullptr;
        if (DescReason(d, &r, &c) && g_req.blockReasons.count(r)) { dropped++; std::lock_guard<std::mutex> l(g_censusMx); g_actorCensus[r].blocked++; continue; }
        keep.push_back(d);
    }
    if (!dropped) { g_actorsAllowed += (long)n; return g_origFieldCreate(field, result, list, x); }
    g_actorsBlocked += dropped; g_actorsAllowed += (long)keep.size();
    if (keep.empty()) { if (result) *result = (int)g_blockCode; return result; }
    DescList mine{ keep.data(), (uint32_t)keep.size(), (uint32_t)keep.size() };
    return g_origFieldCreate(field, result, &mine, x);
}
static void InstallActorGate() {
    const uintptr_t vt = VtableByName(".?AVServerField@pa@@"); uintptr_t f = 0;
    if (!vt || !ReadBytes(vt + 17 * 8, &f, 8)) { Log("[playmode] actor gate off: ServerField vtable not found"); return; }
    // the slot must be the list create: prologue + "rbx = r9, r13 = result, r12 = field" and the 10-entry local vector
    static const uint8_t pro[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10 };
    uint8_t got[sizeof pro] = {}; if (!ReadBytes(f, got, sizeof got) || memcmp(got, pro, sizeof pro) != 0) { Log("[playmode] actor gate off: ServerField slot 17 (rva 0x%llx) does not look like the field create", (unsigned long long)(f - g_base)); return; }
    g_blockCode = GameHashOf("eErrNoActorSpawnFail"); if (!g_blockCode) g_blockCode = 0x7FFFFFF1;
    if (InstallInternalHook((void*)f, (void*)HookFieldCreate, (void**)&g_origFieldCreate, "field create (play mode actor gate)"))
        Log("[playmode] actor gate on ServerField slot 17 rva 0x%llx, refusal code 0x%08x", (unsigned long long)(f - g_base), g_blockCode);
}

// ---- scene object gate / census (createSceneObjectFrom hook in cdmodkit.cpp) -------------------------------------------------
struct CreateCensus { long n = 0, blocked = 0; int flags = -1; std::vector<std::string> samples; };
static std::map<uintptr_t, CreateCensus> g_createCensus;   // caller rva
// Which client scene objects belong to the world (decided from the census of a normal load, see notes/FORMATS.md).
static bool PlayModeObjectBlocked(uintptr_t retRva, const std::string& prefab, const float* xf) {
    (void)retRva; (void)prefab; (void)xf;
    return false;
}
static void PlayModeObjectGateInstall() {}
// ---- level gate: the world's level files (ResourceLoader::load in cdmodkit.cpp) ------------------------------------------------
// Static world content (buildings, props, cliffs, trees) is not created through createSceneObjectFrom: it comes in with the
// level files, leveldata/bin__/rootlevel/sectorlevel/sector_X_Z[_sub_A_B|_indoor..].palevel (256 m sectors) and the named levels
// next to rootlevel.palevel (towns, quests, phases, roads, triggers). An isolated level is loaded from an empty level file of the
// game's own (788 bytes: a SceneLevelDataReflect with _useProxyLOD false and no objects or child levels) - a level that
// does not exist keeps the loading screen up forever, and an empty level with default settings leaves the level's
// low-detail proxy mesh (proxylod/<level>.pam + .hkx) standing. rootlevel.palevel itself (the world root) always loads.
static std::map<std::string, std::pair<long, std::vector<std::string>>> g_resCensus;   // extension -> loads, samples
static std::set<std::string> g_levelSeen;                                             // every level path asked for (logged once)
static std::atomic<long> g_levelsBlocked{ 0 }, g_levelsAllowed{ 0 };
static bool SectorOf(const std::string& path, int* x, int* z) {
    const size_t k = path.rfind("/sector_"); if (k == std::string::npos) return false;
    return sscanf_s(path.c_str() + k + 8, "%d_%d", x, z) == 2;
}
static bool LevelIsolated(const std::string& path) {
    if (g_req.isolateLevels == 0) return false;
    if (path.size() < 8 || path.compare(path.size() - 8, 8, ".palevel") != 0) return false;
    if (path.find("leveldata/") == std::string::npos || path.find("/rootlevel/") == std::string::npos) return false;
    const size_t slash = path.rfind('/'); const std::string file = path.substr(slash + 1);
    if (file == "rootlevel.palevel") return false;
    for (auto& k : g_req.keepLevels) if (path.find(k) != std::string::npos) return false;
    const bool sector = path.find("/sectorlevel/") != std::string::npos;
    if (!sector && g_req.isolateLevels < 2) return false;   // 1 = sector levels only, 2 = named location levels as well
    if (!sector) {   // the root's global children (loaded first, before any location): triggers, regions, game data, roads, sea mask
        static const char* kGlobal[] = { "gameplaytrigger", "regioninfolevel", "trigger_", "content_level", "world_use_terrain", "dev_", "edit_dev_level",
            "gamedatalevel", "gamephase", "levelactionpoint", "levelsequencerspawn", "roadlevel", "seamask", "fx_sector_" };
        for (const char* g : kGlobal) if (file.compare(0, strlen(g), g) == 0) return false;
    }
    int x = 0, z = 0;
    if (sector && g_req.radius > 0 && SectorOf(path, &x, &z)) {   // a sector farther than radius from the spawn stays (sectors are 256 m: sector_-12_-3 spans x -3089..-2932, z -778..-493)
        const float x0 = x * 256.0f, z0 = z * 256.0f;
        const float dx = std::max(std::max(x0 - g_req.spawn.x, 0.0f), g_req.spawn.x - (x0 + 256.0f));
        const float dz = std::max(std::max(z0 - g_req.spawn.z, 0.0f), g_req.spawn.z - (z0 + 256.0f));
        if (sqrtf(dx * dx + dz * dz) > g_req.radius) return false;
    }
    return true;
}
// The substitute: 788 bytes, SceneLevelDataReflect {_forceMinLoadingRange true, _useProxyLOD false}, no objects, no child levels.
// It is checked on the title screen (PlayModeCheckEmptyLevel): a game patch that drops it turns the level gate off instead of
// substituting a file that does not exist (which would keep the loading screen up forever).
static const char kEmptyLevel[] = "leveldata/bin__/rootlevel/calphade_after_area_03_indoor_1_0.palevel";
static std::atomic<bool> g_emptyLevelOk{ false };
const char* PlayModeEmptyLevel() { return kEmptyLevel; }
static void PlayModeCheckEmptyLevel() {
    if (!g_req.isolate || g_req.isolateLevels == 0) return;
    std::vector<uint8_t> data; bool notFound = false;
    const bool ok = GameReadAvailable() && GameReadFile(kEmptyLevel, data, &notFound) && data.size() >= 16 && data.size() < 4096 && memcmp(data.data(), "PARC", 4) == 0;
    g_emptyLevelOk = ok;
    if (ok) Log("[playmode] level gate: empty level %s (%zu bytes) is there", kEmptyLevel, data.size());
    else Log("[playmode] level gate OFF: the empty level %s could not be read (%s); buildings and props stay", kEmptyLevel, notFound ? "not in the packs" : "loader not ready");
}
bool PlayModeOnResLoad(const std::string& path) {
    if (!g_active.load() || path.empty()) return false;
    const size_t dot = path.rfind('.'); const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    const bool block = g_isolating.load() && g_emptyLevelOk.load() && LevelIsolated(path);
    if (ext == ".palevel" && path.size() >= 17 && path.compare(path.size() - 17, 17, "rootlevel.palevel") == 0) g_rootLevelSeen = true;
    bool first = false;
    { std::lock_guard<std::mutex> l(g_censusMx); auto& e = g_resCensus[ext]; e.first++; if (e.second.size() < 8) e.second.push_back(path);
      if (ext == ".palevel" && g_levelSeen.size() < 20000) first = g_levelSeen.insert(path).second; }
    if (ext == ".palevel") { if (block) g_levelsBlocked++; else g_levelsAllowed++; }
    if (ext != ".palevel" && (path.find("leveldata/") != std::string::npos || path.find("proxylod") != std::string::npos)) {
        static std::set<std::string> s_seen; static std::mutex s_mx; bool f = false;
        { std::lock_guard<std::mutex> l(s_mx); if (s_seen.size() < 400) f = s_seen.insert(path).second; }
        if (f) Log("[playmode] world file %s", path.c_str());
    }
    if (first) Log("[playmode] level %s: %s", path.c_str(), block ? "ISOLATED" : "loaded");
    return block;
}
static std::map<std::string, long> g_streamCensus;   // streamer reads by top folder / extension
void PlayModeOnStream(const std::string& path) {
    if (!g_active.load() || path.empty()) return;
    const size_t dot = path.rfind('.'); const size_t sl = path.find('/', 1);
    const std::string key = (sl == std::string::npos ? path : path.substr(0, sl)) + " " + (dot == std::string::npos ? "" : path.substr(dot));
    bool first = false;
    { std::lock_guard<std::mutex> l(g_censusMx); g_streamCensus[key]++;
      static std::set<std::string> s_seen; if (path.find("leveldata") != std::string::npos && path.find("/terrain/") == std::string::npos && s_seen.size() < 300) first = s_seen.insert(path).second; }
    if (first) Log("[playmode] streamed %s", path.c_str());
}
static bool Kept(const std::string& prefab) {
    for (auto& k : g_req.keep) if (prefab.compare(0, k.size(), k) == 0) return true;
    return false;
}
bool PlayModeOnCreate(uintptr_t retRva, const std::string& prefab, const float* xf, uint8_t f1, uint8_t f2, uint8_t f3, bool ours) {
    if (!g_active.load()) return false;
    bool block = false;
    if (!ours && g_isolating.load() && g_req.isolateObjects && !Kept(prefab)) block = PlayModeObjectBlocked(retRva, prefab, xf);
    std::lock_guard<std::mutex> l(g_censusMx);
    auto& c = g_createCensus[retRva]; c.n++; if (block) c.blocked++; c.flags = f1 | (f2 << 1) | (f3 << 2);
    if (c.samples.size() < 6 && std::find(c.samples.begin(), c.samples.end(), prefab) == c.samples.end()) c.samples.push_back(prefab);
    if (block) g_objectsBlocked++; else if (!ours) g_objectsAllowed++;
    return block;
}

// ---- auto continue -------------------------------------------------------------------------------------------------------
static HWND GameWindow() {
    struct Ctx { DWORD pid; HWND h; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL { auto* c = (Ctx*)lp; DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
        wchar_t t[64] = {}; GetWindowTextW(h, t, 64); if (wcscmp(t, L"Crimson Desert") == 0) { c->h = h; return FALSE; } return TRUE; }, (LPARAM)&ctx);
    return ctx.h;
}
static void PressScan(WORD scan) {   // the game reads keys by scan code (raw input), so a scan-code SendInput is what reaches it
    INPUT in[2] = {}; in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = scan; in[0].ki.dwFlags = KEYEVENTF_SCANCODE; in[1].ki.wScan = scan; in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT)); Sleep(120); SendInput(1, &in[1], sizeof(INPUT));
}
// Research aid for the first minutes of a play mode session: where the player is (the pump does not run during loads).
static void WatchThread() {
    const DWORD t0 = GetTickCount(); Vec3 last{ 1e9f, 0, 0 };
    while (GetTickCount() - t0 < 150000 && g_phase.load() != PhExited) {
        Sleep(1000); Vec3 p{};
        if (!PlayerWorldPos(&p)) continue;
        if (fabsf(p.x - last.x) + fabsf(p.y - last.y) + fabsf(p.z - last.z) < 0.3f) continue;
        last = p; Log("[playmode] player at (%.2f %.2f %.2f), phase %s", p.x, p.y, p.z, PhaseName(g_phase.load()));
    }
}
static void ContinueThread() {
    // the title screen comes ~15 s after the overlay is ready and ignores keys while it fades in: first press after 18 s, then
    // every 6 s until the load starts (the server field ticks / the player exists). E only, never anything else.
    // The game thread pump (movement tick) does not run on the title screen, so this thread also moves the phase on.
    const DWORD t0 = GetTickCount(); DWORD seen = 0; bool waitedFocus = false, checked = false;
    while (g_phase.load() == PhTitle && GetTickCount() - t0 < 600000) {
        Sleep(500);
        if (!checked && (GameReadAvailable() || PlayModeLoadStarted() || GetTickCount() - t0 > 120000)) { checked = true; PlayModeCheckEmptyLevel(); }
        if (PlayModeLoadStarted()) { g_phase = PhLoading; SetStatus("loading"); std::thread(WatchThread).detach(); break; }
        if (!g_req.autoContinue) continue;
        {
            HWND h = GameWindow(); if (!h) continue;
            if (!seen) seen = GetTickCount();
            if (GetTickCount() - seen < 25000) continue;   // window up -> title screen faded in
            if (GetForegroundWindow() != h) { if (!waitedFocus) { waitedFocus = true; SetStatus("title screen: waiting for the game window to be in front to press continue"); } continue; }
            static DWORD s_last = 0; if (s_last && GetTickCount() - s_last < 6000) continue; s_last = GetTickCount();
            PressScan(0x12); const long k = ++g_continuePresses; Log("[playmode] continue pressed (E, #%ld)", k); SetStatus("continue pressed, waiting for the load");
        }
    }
}
// The load has started once the game asks for the world root level (seconds after continue; the title screen loads no level)
// or the server field ticks. Continue is not pressed any more from then on.
bool PlayModeLoadStarted() { return g_rootLevelSeen.load() || ServerFieldTicking(); }

// ---- destination ---------------------------------------------------------------------------------------------------------
// travel.cpp calls this for every stage reload the game itself starts. The first one while play mode waits for its first load
// is redirected to the requested spawn (scale, quat xyzw, pos).
bool PlayModeStageOverride(float* tf) {
    if (!g_active.load() || !g_req.hasSpawn || g_directFirstLoad.load()) return false;
    const int ph = g_phase.load(); if (ph != PhTitle && ph != PhLoading) return false;
    Log("[playmode] the game starts a stage reload to (%.1f %.1f %.1f) during its first load: redirected to (%.1f %.1f %.1f)", tf[7], tf[8], tf[9], g_req.spawn.x, g_req.spawn.y, g_req.spawn.z);
    const float h = g_req.yaw * 3.14159265f / 360.0f;
    tf[3] = 0; tf[4] = sinf(h); tf[5] = 0; tf[6] = cosf(h); tf[7] = g_req.spawn.x; tf[8] = g_req.spawn.y; tf[9] = g_req.spawn.z;
    g_directFirstLoad = true; return true;
}

// The first load of a save does not go through the client's stage reload. On the server, TrocTrGameLoadingStartReq::execute
// (vtable slot 2) applies a destination that a TrocTrReloadStageStartReq stored in the player's ServerTransformSyncActorComponent
// (+0x550, "pending reload"); without one it only reports an error code back. So the first GameLoadingStartReq of a play mode
// session runs a ReloadStageStartReq of our own first (same sender, payload u32 0, u32 1, u32 0, pos3, quat4, scale3, u8 1 -
// what the client's fast travel sends), and the load that was about to start lands at the spawn point.
typedef void* (__fastcall* ExecFn)(void* handler, int* result, void* packet);
static ExecFn g_origLoadingStart = nullptr; static void* g_reloadExec = nullptr; static uintptr_t g_reloadHandler = 0;
static std::atomic<long> g_loadingStarts{ 0 };
static bool CallReloadGuarded(void* pkt, int* res) {
    CDK_GUARD_BEGIN ((ExecFn)g_reloadExec)((void*)g_reloadHandler, res, pkt); return true;
    CDK_GUARD_FAIL return false;
    CDK_GUARD_END
}
static void* __fastcall HookLoadingStart(void* handler, int* result, void* packet) {
    const long n = ++g_loadingStarts;
    if (g_active.load() && g_req.hasSpawn && !g_directFirstLoad.load() && g_reloadExec && g_reloadHandler && packet) {
        const int ph = g_phase.load();
        if (ph == PhTitle || ph == PhLoading) {
            alignas(16) uint8_t pkt[0x40] = {}; ReadBytes((uintptr_t)packet, pkt, sizeof pkt);   // the incoming request as template: same sender / session fields
            uint8_t buf[5 + 53] = {}; const uint16_t plen = 53; memcpy(buf + 3, &plen, 2);
            const uint32_t a = 0, b = 1, c = 0; const float h = g_req.yaw * 3.14159265f / 360.0f;
            const float pos[3] = { g_req.spawn.x, g_req.spawn.y, g_req.spawn.z }, quat[4] = { 0.0f, sinf(h), 0.0f, cosf(h) }, scale[3] = { 1, 1, 1 };
            memcpy(buf + 5, &a, 4); memcpy(buf + 9, &b, 4); memcpy(buf + 13, &c, 4); memcpy(buf + 17, pos, 12); memcpy(buf + 29, quat, 16); memcpy(buf + 45, scale, 12); buf[57] = 1;
            const uint16_t total = sizeof buf; memcpy(pkt + 0x10, &total, 2); uint8_t* bp = buf; memcpy(pkt + 0x18, &bp, 8);
            int res = -1; const bool ran = CallReloadGuarded(pkt, &res);
            Log("[playmode] first load (GameLoadingStartReq #%ld): own ReloadStageStartReq to (%.1f %.1f %.1f): %s, result 0x%08x", n, pos[0], pos[1], pos[2], ran ? "executed" : "FAULTED", (uint32_t)res);
            if (ran && res == 0) g_directFirstLoad = true;
        }
    } else if (g_active.load()) Log("[playmode] GameLoadingStartReq #%ld", n);
    void* r = g_origLoadingStart(handler, result, packet);
    if (g_active.load()) Log("[playmode] GameLoadingStartReq #%ld result 0x%08x", n, result ? (uint32_t)*result : 0u);
    return r;
}
static void InstallDirectLoad() {
    if (!g_req.hasSpawn || !g_req.directLoad) return;
    const uintptr_t vtL = VtableByName(".?AVTrocTrGameLoadingStartReq@pa@@"), vtR = VtableByName(".?AVTrocTrReloadStageStartReq@pa@@");
    uintptr_t fL = 0, fR = 0; if (vtL) ReadBytes(vtL + 16, &fL, 8); if (vtR) ReadBytes(vtR + 16, &fR, 8);
    int nH = 0; const uintptr_t hR = vtR ? StaticObjectWithVtable(vtR, &nH) : 0;
    if (!fL || !fR || !hR) { Log("[playmode] direct first load off (loading start %p, reload exec %p, reload handler %p): the game's fast travel takes over after the first load", (void*)fL, (void*)fR, (void*)hR); return; }
    g_reloadExec = (void*)fR; g_reloadHandler = hR;
    if (InstallInternalHook((void*)fL, (void*)HookLoadingStart, (void**)&g_origLoadingStart, "GameLoadingStartReq (play mode first load)"))
        Log("[playmode] direct first load: GameLoadingStartReq rva 0x%llx, ReloadStageStartReq rva 0x%llx, handler rva 0x%llx (%d)", (unsigned long long)(fL - g_base), (unsigned long long)(fR - g_base), (unsigned long long)(hR - g_base), nH);
}

static float Dist2D(Vec3 a, Vec3 b) { const float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }
static void SpawnScene() {
    if (g_sceneSpawned) return; g_sceneSpawned = true;
    int ok = 0;
    for (const auto& o : g_req.objects) { const int uid = SpawnAt(o.prefab, o.pos, o.rot, o.scale); if (uid) { g_sceneUids.push_back(uid); ok++; } else Log("[playmode] object %s not queued", o.prefab.c_str()); }
    if (!g_req.project.empty()) { const bool loaded = LoadProject(g_req.project, false); Log("[playmode] project \"%s\": %s", g_req.project.c_str(), loaded ? "loading" : ProjectError().c_str()); }
    Log("[playmode] scene: %d of %zu objects queued%s", ok, g_req.objects.size(), g_req.project.empty() ? "" : ", project requested");
}

// ---- exit -----------------------------------------------------------------------------------------------------------------
static std::atomic<int> g_exitReq{ 0 };   // 1 quit, 2 travel back
bool PlayModeExit(const std::string& mode) {
    if (!g_active.load()) return false;
    g_exitReq = mode == "travel" ? 2 : 1; return true;
}
static void DoExit(int how) {
    if (how == 2) {
        g_isolating = false;
        const bool ok = g_haveLoadedAt && TravelTo(g_loadedAt, 0);
        Log("[playmode] exit by travel: isolation off, travel back to (%.1f %.1f %.1f): %s; saving stays blocked until the game is restarted", g_loadedAt.x, g_loadedAt.y, g_loadedAt.z, ok ? "started" : "not possible");
        g_phase = PhExited; SetStatus(ok ? "exited: isolation off, travelling back (saving stays blocked until restart)" : "exit by travel failed: travel system not ready");
        return;
    }
    Log("[playmode] exit: closing the game");
    SetStatus("exiting: closing the game"); g_phase = PhExited;
    if (HWND h = GameWindow()) PostMessageW(h, WM_CLOSE, 0, 0);
}

// ---- pump (game thread) ----------------------------------------------------------------------------------------------------
void PlayModeTick() {
    if (!g_active.load()) return;
    static DWORD s_last = 0; const DWORD now = GetTickCount(); if (now - s_last < 250) return; s_last = now;
    if (const int e = g_exitReq.exchange(0)) { DoExit(e); return; }
    {   // hotkey Ctrl+Shift+End: leave play mode (quit) while the game window is in front
        static bool s_down = false; HWND h = GameWindow();
        const bool down = h && GetForegroundWindow() == h && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_END) & 0x8000);
        if (down && !s_down) { s_down = true; DoExit(1); return; } s_down = down;
    }
    const int ph = g_phase.load();
    if (ph == PhTitle) { if (PlayModeLoadStarted()) { g_phase = PhLoading; SetStatus("loading"); } return; }
    Vec3 p{}; const bool inWorld = PlayerWorldPos(&p) && (fabsf(p.x) > 1 || fabsf(p.z) > 1);
    static DWORD s_since = 0; if (!inWorld) { s_since = 0; return; } if (!s_since) s_since = now;
    const bool stable = now - s_since > 2500;
    if (ph == PhLoading && stable) {
        g_loadedAt = p; g_haveLoadedAt = true; g_phase = PhInWorld;
        Log("[playmode] in the world at (%.1f %.1f %.1f)%s", p.x, p.y, p.z, g_directFirstLoad ? " after the redirected first load" : "");
        if (!g_req.hasSpawn || Dist2D(p, g_req.spawn) < 40.0f) { g_phase = PhArrived; SetStatus(g_req.hasSpawn ? "arrived at the spawn point with the first load" : "in the world (no spawn point requested)"); }
        else { TravelPrepare(); SetStatus("in the world; travelling to the spawn point"); }
        return;
    }
    if (ph == PhInWorld) {
        static DWORD s_try = 0; if (now - s_try < 1000) return; s_try = now;
        if (TravelTo(g_req.spawn, g_req.yaw)) { g_phase = PhTravel; s_since = 0; SetStatus("travelling to the spawn point (one loading screen)"); }
        return;
    }
    if (ph == PhTravel && stable && Dist2D(p, g_req.spawn) < 40.0f) { g_phase = PhArrived; SetStatus("arrived at the spawn point"); return; }
    if (ph == PhArrived && now - s_since > 4000) { SpawnScene(); g_phase = PhReady; SetStatus(g_isolating.load() ? "ready: isolated world" : "ready (isolation off)"); return; }
}

// ---- status ----------------------------------------------------------------------------------------------------------------
static std::string J(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((unsigned char)c < 32) o += ' '; else o += c; } return o + "\""; }
static std::string F(double v) { char b[48]; snprintf(b, sizeof b, "%.3f", v); return b; }
std::string PlayModeStatusJson(bool census) {
    std::string st; { std::lock_guard<std::mutex> l(g_mx); st = g_status; }
    std::string o = "{\"active\":" + std::string(g_active ? "true" : "false") + ",\"isolating\":" + (g_isolating ? "true" : "false") +
        ",\"phase\":" + J(PhaseName(g_phase.load())) + ",\"status\":" + J(st) + ",\"requestError\":" + J(g_requestError) +
        ",\"directFirstLoad\":" + (g_directFirstLoad ? "true" : "false") + ",\"continuePresses\":" + std::to_string(g_continuePresses.load()) +
        ",\"spawn\":" + (g_req.hasSpawn ? "{\"x\":" + F(g_req.spawn.x) + ",\"y\":" + F(g_req.spawn.y) + ",\"z\":" + F(g_req.spawn.z) + ",\"yaw\":" + F(g_req.yaw) + "}" : "null") +
        ",\"loadedAt\":" + (g_haveLoadedAt ? "{\"x\":" + F(g_loadedAt.x) + ",\"y\":" + F(g_loadedAt.y) + ",\"z\":" + F(g_loadedAt.z) + "}" : "null") +
        ",\"counts\":{\"savesBlocked\":" + std::to_string(g_savesBlocked.load()) + ",\"saveRequestsSkipped\":" + std::to_string(g_saveReqsSkipped.load()) + ",\"actorsBlocked\":" + std::to_string(g_actorsBlocked.load()) +
        ",\"actorsAllowed\":" + std::to_string(g_actorsAllowed.load()) + ",\"objectsBlocked\":" + std::to_string(g_objectsBlocked.load()) +
        ",\"objectsAllowed\":" + std::to_string(g_objectsAllowed.load()) + ",\"createCalls\":" + std::to_string(CreateCalls()) +
        ",\"sceneObjects\":" + std::to_string(g_sceneUids.size()) + ",\"levelsBlocked\":" + std::to_string(g_levelsBlocked.load()) +
        ",\"levelsAllowed\":" + std::to_string(g_levelsAllowed.load()) + ",\"loadingStarts\":" + std::to_string(g_loadingStarts.load()) + "}";
    if (census) {
        std::lock_guard<std::mutex> l(g_censusMx);
        o += ",\"actorReasons\":["; bool first = true;
        for (auto& kv : g_actorCensus) { if (!first) o += ","; first = false; o += "{\"reason\":" + std::to_string(kv.first) + ",\"name\":" + J(ReasonName(kv.first)) + ",\"seen\":" + std::to_string(kv.second.seen) + ",\"blocked\":" + std::to_string(kv.second.blocked) + ",\"class\":" + J(kv.second.cls) + "}"; }
        o += "],\"fieldCallers\":["; first = true;
        for (auto& kv : g_fieldCallers) { if (!first) o += ","; first = false; char b[64]; snprintf(b, sizeof b, "{\"rva\":\"0x%llx\",\"calls\":%ld}", (unsigned long long)kv.first, kv.second); o += b; }
        o += "],\"createCallers\":["; first = true;
        for (auto& kv : g_createCensus) { if (!first) o += ","; first = false; char b[96]; snprintf(b, sizeof b, "{\"rva\":\"0x%llx\",\"calls\":%ld,\"blocked\":%ld,\"flags\":%d,\"samples\":[", (unsigned long long)kv.first, kv.second.n, kv.second.blocked, kv.second.flags); o += b;
            for (size_t i = 0; i < kv.second.samples.size(); i++) { if (i) o += ","; o += J(kv.second.samples[i]); } o += "]}"; }
        o += "],\"streamed\":["; first = true;
        for (auto& kv : g_streamCensus) { if (!first) o += ","; first = false; o += "{\"key\":" + J(kv.first) + ",\"reads\":" + std::to_string(kv.second) + "}"; }
        o += "],\"resources\":["; first = true;
        for (auto& kv : g_resCensus) { if (!first) o += ","; first = false; o += "{\"ext\":" + J(kv.first) + ",\"loads\":" + std::to_string(kv.second.first) + ",\"samples\":[";
            for (size_t i = 0; i < kv.second.second.size(); i++) { if (i) o += ","; o += J(kv.second.second[i]); } o += "]}"; }
        o += "]";
    }
    return o + "}";
}

// called once after ResolveGame (init thread): installs the hooks a consumed request needs; nothing for a normal start
void PlayModeInstall() {
    if (!g_active.load()) return;
    InstallActorGate();
    PlayModeObjectGateInstall();
    InstallDirectLoad();
    if (g_req.blockSave) InstallSaveRequestGate();
    std::thread(ContinueThread).detach();   // title screen: auto continue and the move to the loading phase
}

}   // namespace core
