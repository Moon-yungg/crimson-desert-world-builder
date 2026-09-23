// cdmodkit core API shared by the console, the overlay and the editor UI.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct Vec3 { float x, y, z; };
struct Rot { float yaw = 0, pitch = 0, roll = 0; };   // degrees; rotation = Ry(yaw) * Rx(pitch) * Rz(roll) (yaw about the up axis, then tilt, then roll)
struct PosInfo { Vec3 tiled, world; int tileX, tileZ; };
struct SpawnedObj { uintptr_t obj; std::string prefab; Vec3 pos; Rot rot; float scale; bool hidden; DWORD tick; Rot colRot; float colScale;
                    int uid; int group; int proj; bool gimmick = false; uintptr_t actor = 0; bool standin = false; };   // gimmick: spawned through the game's server path (obj = its server scene object, actor = its actor)   // uid: stable id for the editor (indices shift when entries are forgotten); group 0 = none;
                                                       // proj: which project the object belongs to (0 = placed by hand, not part of a saved project yet)

namespace core {
    extern uintptr_t g_base;
    extern bool      g_menuOpen;      // set by the editor UI
    extern bool      g_uiWantsMouse;  // cursor is over a World Builder window (ImGui WantCaptureMouse), updated every frame
    extern bool      g_uiWantsKeyboard; // a text field is active (ImGui WantTextInput)
    extern bool      g_placing;         // placement mode: arrow/numpad/Enter/Backspace go to World Builder, everything else to the game

    void Log(const char* fmt, ...);
    std::string ModDir();             // bin64\cdmodkit

    bool ReadBytes(uintptr_t a, void* out, size_t n);
    bool WriteBytes(uintptr_t a, const void* src, size_t n);
    const char* RttiName(uintptr_t obj);

    uintptr_t PlayerActor();
    bool ReadPos(uintptr_t actor, PosInfo* out);
    bool PlayerWorldPos(Vec3* out);
    bool PlayerPosInfo(PosInfo* out);
    bool SetPlayerPos(Vec3 world);             // writes the transform component (experimental: the game may correct it)
    bool CameraPose(Vec3* fwd, Vec3* pos);     // horizontal view direction (local +Z of the camera object) and world position of the active camera
    bool CameraBasis(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd);   // full camera frame from the camera object's quaternion (fwd = local +Z, sign applied by the editor)
    extern float g_fovDeg; extern bool g_camMirror; extern bool g_fovAuto;   // projection settings (settings.txt fov=, mirror=, fovauto=)
    extern int g_camLag;                                                     // settings.txt camlag=: frames the overlay camera trails the game camera (the game simulates ahead of the frame on screen)
    bool CameraFov(float* deg);                // live vertical field of view read from the camera object (PhotoCamera +0x1DC), false if implausible
    void ViewScan();                          // logs every copy of the camera basis in memory (view matrices) and nearby projection blocks
    // the view + projection the renderer really uses (found by a memory scan, read every frame); rank 0 = newest copy in flight
    bool RenderCamera(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd, float* m00, float* m11, int rank);
    void FindRenderCamera();                  // (re)starts the background scan; RenderCamera() does this itself when it has nothing
    int  RenderCameraBlocks();                // how many copies are currently tracked (0 = falling back to the camera object + fov setting)
    // research: the game's server gimmick spawns are captured in a ring; one of them can be issued again at 'at' (the next spawn the game makes triggers it)
    struct GimmickCapInfo { int id; uintptr_t caller; uint32_t k1, k2; Vec3 pos; unsigned long ageMs; char name[96]; char path[200]; };
    int  GimmickCaptureList(GimmickCapInfo* out, int max);   // newest first
    const char* GimmickCallerName(uintptr_t caller);        // "level", "item drop", "housing", "buff" ... or nullptr when unknown
    void ArmGimmickReplay(Vec3 at, int id);
    struct SpawnedInfo { uintptr_t so, actor; char prefab[200]; Vec3 pos; unsigned long ageMs; };
    int  SpawnedList(SpawnedInfo* out, int max);            // objects our replays created, newest first
    void RequestRemoveSpawned(uintptr_t actor);              // experiment: repeat the game's removal loop body for that actor on the server tick
    extern bool g_gimmickSpawn;                              // gimmick prefabs are spawned through the game (interactive) instead of as plain objects
    int  GimmickPending();                                   // objects waiting for a spawn template
    bool GimmickTemplateReady();
    void SetGimmickReplayPrefab(const char* path);   // the replay creates the server object from this prefab path instead of the captured one (empty = as captured)
    const char* GimmickReplayPrefab();
    bool GimmickReplayArmed();
    void FovTrace(int seconds);                // logs camera-side floats that change while zooming (to find the field of view)
    void CamTrace(int seconds);                // logs candidate camera-direction fields of the PlayerCameraComponent while the camera is rotated
    void RayTrace(int calls);                  // logs the next N ray casts of the game (reverse engineering aid)
    void ProbeGround(float above, float len);  // dev: logs three replayed casts from above the player
    // Ground queries: a sphere cast of the game (captured automatically from its own character probe) replayed downward.
    struct GroundHit { bool done = false, hit = false; float centerY = 0; float fraction = 0; Vec3 normal{}; };
    bool GroundProbeReady();                   // a cast template was captured (the character has to be in the world for a moment)
    int  GroundProbe(Vec3 start, float len);   // queues a cast from start straight down on the game thread; ticket (0 = not possible)
    bool GroundResult(int ticket, GroundHit* out);   // true once the ticket finished (poll every frame)
    extern float g_probeRadius;                // sphere radius of the template, calibrated at the player's feet (ground = centerY - radius)

    void RunOnGameThread(std::function<void()> f);
    bool GameThreadReady();
    long PumpTicks();
    long CreateCalls();
    bool HooksReady();
    bool BuildOk();                   // code bytes at all used RVAs match the supported game build
    const char* BuildMessage();
    const char* GameVersion();        // file version of CrimsonDesert.exe ("" if unreadable); informational only, nothing depends on it

    // Spawning (queued to the game thread). The registry keeps every object we created.
    int  SpawnAt(const std::string& prefab, Vec3 world, Rot rot = {}, float scale = 1.0f, int group = 0, int proj = 0);   // returns the uid (0 = not queued)
    std::vector<SpawnedObj> Spawned();
    int  IndexOfUid(int uid);                   // -1 when the object was forgotten
    void SetGroup(int uid, int group); int NewGroupId();
    struct MoveReq { int uid; Vec3 pos; Rot rot; float scale; };
    bool MoveMany(const std::vector<MoveReq>& reqs, bool final);   // all moves in one game-thread job; false = dropped (live, queue lagging)
    bool HideUid(int uid); void ForgetUid(int uid);
    // SceneObject::setWorldTransform on the game thread. final=false uses flags (0,0) (update without re-insert, for dragging),
    // final=true uses (0,1) like the game's placement code (re-inserts the render instance).
    bool MoveSpawned(size_t idx, Vec3 pos, Rot rot, float scale, bool final = true);
    bool HideSpawned(size_t idx);               // scale ~0 and far below the world
    void ForgetSpawned(size_t idx);
    extern bool g_recreateOnMove;               // false: disable/setTransform/enable in place; true: remove + re-create
    extern int  g_liveMode;                     // live-drag method, see DoLiveMove
    extern int  g_keyToggle, g_keyMode;         // configurable hotkeys (virtual key codes), settings.txt in the mod folder
    extern bool g_showConsole;                  // settings.txt console=0 hides the console window (takes effect on the next start)
    extern bool g_httpEnabled;                  // settings.txt http_api=1 runs the loopback HTTP API (off by default, switched live in the Settings tab)
    extern int  g_httpPort;                     // settings.txt http_port= (1..65535)
    int KeyCount(); const char* KeyNameAt(int i); int KeyVkAt(int i); const char* KeyName(int vk); void SaveSettings();
    // placement keys (virtual key codes, settings.txt key_move_fwd= ...); every action can be bound to any key, numpad is only the default
    enum PlaceKey { PK_FWD, PK_BACK, PK_LEFT, PK_RIGHT, PK_UP, PK_DOWN, PK_ROT_L, PK_ROT_R, PK_SCALE_UP, PK_SCALE_DOWN, PK_FETCH, PK_SNAP, PK_MOUSE, PK_LEVEL, PK_GROUND, PK_DROP, PK_CANCEL, PK_FAST, PK_COUNT };
    extern int g_placeKeys[PK_COUNT];
    const char* PlaceKeyId(int i); const char* PlaceKeyLabel(int i);
    void ApplyPlaceKeys();                      // hands the bound keys to the input layer (swallowed while placing)
    void SetTrace(bool on); bool Trace();       // log the game's own setWorldTransform/setEnable calls (reverse engineering aid)
    bool GameReadAvailable();                   // the game's resource loader can be used (instance captured, functions resolved)
    bool GameReadFile(const std::string& packPath, std::vector<uint8_t>& out);   // read a pack file through the game's loader

    // Prefab index (bin64\cdmodkit\prefabs.tsv, fallback prefabs.txt): logical path, display name, category tree, tags
    struct PrefabInfo { std::string path, name, tags, mesh; int cat = 0; int meshes = 0, children = 0;
                        float sx = 0, sy = 0, sz = 0;                 // bounding box in m (0 = unknown)
                        float cx = 0, cy = 0, cz = 0; bool hasCenter = false; };   // bounding box center relative to the prefab pivot
    std::string ThumbFile(const std::string& prefabPath);   // bin64\cdmodkit\thumbs\<fnv64>.png
    std::string GameDir();                                  // game root (parent of bin64), holds meta\0.papgt and the pack groups
    void SetPrefabSize(const std::string& path, const float* sizeAndCenter);     // 6 floats (w h d cx cy cz), called by the thumbnail worker
    struct CatNode { std::string name; int parent; std::vector<int> children; std::vector<int> prefabs; int total; };
    const std::vector<PrefabInfo>& PrefabIndex();
    const std::vector<CatNode>& Categories();          // node 0 = root
    const std::vector<std::pair<std::string, int>>& TagCounts();
    bool IsFavorite(int prefabIdx); void ToggleFavorite(int prefabIdx);
    const std::vector<int>& Favorites();
    const std::vector<std::string>& Prefabs();         // legacy: plain path list (same order as PrefabIndex)

    // Live preview: one temporary object that follows the browser selection; Commit turns it into a permanent object.
    void PreviewSet(const std::string& prefab, Vec3 world, float yawDeg, float scale, bool recreate = false);   // recreate: always remove + spawn
    void PreviewClear();
    bool PreviewCommit();          // registers the preview object as spawned (no new spawn needed); false if none
    bool PreviewActive();
    bool PreviewPending();         // a PreviewSet job is still queued (commit only once it ran)

    // Projects: bin64\cdmodkit\projects\<name>.cdproj, one object per line "prefab|x|y|z|yaw|scale|group|pitch|roll" (v3; older files have fewer fields)
    // Project membership: every object knows which project it came from, so a project can be overwritten with exactly its
    // own objects while other loaded projects stay untouched. Ids are per session and name the .cdproj files; 0 = new object.
    enum SaveScope { SaveWholeScene = 0, SaveProjectAndNew = 1, SaveNewOnly = 2, SaveProjectOnly = 3 };
    int  ProjectId(const std::string& name);       // id for a project name, creating one on first use (0 for an empty name)
    std::string ProjectNameOf(int id);             // "" for 0 / unknown
    int  ProjectObjectCount(int id);               // visible objects currently belonging to that project (0 = the new ones)
    bool ProjectDirty(int id);                     // an object of it was moved, deleted or regrouped since the last load / save
    void AssignProject(int uid, int proj);
    bool SaveProject(const std::string& name, int scope = SaveWholeScene);   // saved objects become members of that project
    bool LoadProject(const std::string& name, bool clearFirst);   // spawns are queued one per game tick
    std::vector<std::string> ListProjects();
    void DeleteAllSpawned();
    // Autoload: bin64\cdmodkit\autoload.txt, one project name per line (without .cdproj), '#' at the line start = comment.
    // Several projects can be active at once; they are all loaded, in file order, once the player is in the world.
    void SetAutoload(const std::string& name, bool on);   // adds or removes one project; the file is deleted when none are left
    std::vector<std::string> Autoload();
    int  PendingSpawns();
}

namespace overlay { void Install(); }
