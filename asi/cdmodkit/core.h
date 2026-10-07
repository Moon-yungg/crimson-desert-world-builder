// cdmodkit core API shared by the console, the overlay and the editor UI.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include "proj_codec.h"   // the C4 disk-value document a group copy is admitted from (values only, no handles)

namespace core { struct PlaceRequest; }   // C5: caller-owned placement request (defined below)

struct Vec3 { float x, y, z; };
struct Rot { float yaw = 0, pitch = 0, roll = 0; };   // degrees; rotation = Ry(yaw) * Rx(pitch) * Rz(roll) (yaw about the up axis, then tilt, then roll)
struct PosInfo { Vec3 tiled, world; int tileX, tileZ; };
struct SpawnedObj { uintptr_t obj; std::string prefab; Vec3 pos; Rot rot; float scale; bool hidden; DWORD tick; Rot colRot; float colScale;
                    int uid; int group; int proj; bool gimmick = false; uintptr_t actor = 0; bool standin = false; uint64_t gen = 0; uint64_t poseGen = 0;
                    std::weak_ptr<core::PlaceRequest> placeReq; int placeRow = -1; std::string note; };   // C5: the caller-owned request that owns this record's row (empty for editor-internal spawns). The record carries it, so a retry, a stand-in or a reversed completion always resolves its OWN row - never a basename or a global last-result lookup.   // gimmick: spawned through the game's server path (obj = its server scene object, actor = its actor)
                                                       // proj: which project the object belongs to (0 = placed by hand, not part of a saved project yet)
                                                       // gen: lifetime token of the record's current physical incarnation (C3). It is renewed when the physical object is
                                                       // replaced (restore, re-create) or invalidated (hide/forget) so that engine work queued for an older incarnation can
                                                       // never attach to a newer one; the record keeps its uid, project, group and pose across all of them.
struct ManagedNpc { int uid = 0; uint32_t key = 0; Vec3 pos{}; int type = 1; uint32_t extra = 0; uintptr_t actor = 0; uint32_t actorId = 0;
                    uintptr_t transform = 0; DWORD spawnRequestTick = 0; bool bindTimeoutLogged = false; bool editMoving = false; Vec3 liveMoveTarget{}; Vec3 editMoveStart{}; bool liveMovePending = false;   // runtime-only live binding/edit state; not serialized
                    bool aiEnabled = true; bool aiApplied = true; int behavior = 0; bool hidden = false; bool spawnPending = false; DWORD tick = 0;
                    int group = 0; int proj = 0; std::string label, note; uint64_t gen = 0; };

namespace core {
    constexpr int kResourcePrefabs = 101;
    constexpr int kResourceErrNames = 102;
    constexpr int kResourceLocales = 103;
    extern uintptr_t g_base;
    // end of the game's image from its PE header (SizeOfImage); was a fixed 0x17000000, which a patch that grows the exe
    // outruns and then valid vtables and functions failed InImage
    inline uintptr_t ImageEnd() {
        static uintptr_t end = 0;
        if (!end && g_base) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            end = g_base + nt->OptionalHeader.SizeOfImage;
        }
        return end;
    }
    extern bool      g_menuOpen;      // set by the editor UI
    extern bool      g_uiWantsMouse;  // cursor is over a World Builder window (ImGui WantCaptureMouse), updated every frame
    extern bool      g_uiWantsKeyboard; // a text field is active (ImGui WantTextInput)
    extern bool      g_placing;       // an editor object/group is currently carried

    void Log(const char* fmt, ...);
    std::string ModDir();             // bin64\cdmodkit
    bool EmbeddedResource(int resourceId, const uint8_t** data, size_t* size);   // RCDATA linked into cdmodkit.asi

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
    bool CameraFov(float* deg);                // live vertical field of view read from the camera object (PhotoCamera +0x1DC), false if implausible
    // the view + projection the renderer really uses through the renderer's camera object
    bool RenderCamera(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd, float* m00, float* m11);
    bool ReadMem(uintptr_t addr, void* out, size_t n); bool WriteMem(uintptr_t addr, const void* in, size_t n);   // research: guarded raw access
    // research: hardware watch (mode 0 write, 1 read/write, 2 execute) with a JSON report; a guarded call of code at addr with
    // six integer arguments (1 done, 0 faulted, 2 still queued on the game thread, -1 not code); scratch memory for calls
    bool ResearchWatch(const uintptr_t addr[4], int seconds, int mode); std::string ResearchWatchReport(bool* running);
    int ResearchCall(uintptr_t addr, const uint64_t args[6], bool gameThread, int timeoutMs, uint64_t* result, unsigned long* code);
    uintptr_t ResearchAlloc(size_t bytes); bool ResearchFree(uintptr_t addr);
    void ResearchPeek(uintptr_t addr, int bytes, bool u16);   // research: log raw memory
    void ResearchFind(const std::vector<uint8_t>& pat, int maxHits);   // research: byte pattern search (logged)
    void SetFallWatch(bool on, uintptr_t addr = 0, bool breakpoints = true, uintptr_t probe = 0, unsigned probeLen = 0);   // research: log falls of the player, the code that moves it (addr: explicit field) and what changed in a probed object
    void ResearchFindPos();
    void GroundTrace(int seconds);
    struct DebugPt { Vec3 p; uint32_t col; };   // research overlay: world points drawn by the editor
    std::vector<DebugPt> DebugPoints(); size_t DebugPointCount(); int LoadDebugPoints(bool clear);   // file bin64\cdmodkit\debugpoints.txt: "x y z rrggbb" per line
    void GeoTraceInstall(uintptr_t vt, int slots); void GeoTraceArm(); void FnTraceInstall(uintptr_t rva); void IoTraceSet(const std::string& filter); void IoHeightDelta(int d); void IoStreamDelta(int d);
    // Terrain editing (terrain.cpp): brush strokes applied to the terrain height textures as the game streams them in;
    // collision is captured from the rendered terrain and follows. Tiles already loaded change after TerrainApply.
    enum { TerrainRaise = 0, TerrainFlatten = 1 };
    struct TerrainStroke {           // world metres; raise: amount (+ up / - down) at the centre, cosine falloff to r
        int mode; float x, z, r;     // flatten: pulls toward the height at (ax, az) by strength (0..1) at the centre
        float amount, strength, ax, az;
        float y;                     // ground height when painted (display only)
        int proj;                    // project the stroke belongs to (0 = painted since the last save), like SpawnedObj::proj
        bool tileScoped = false;     // a stroke crossing a tile boundary is stored once for each affected tile
        int tileX = 0, tileZ = 0;    // only the named tile receives this copy; legacy project rows are expanded on load
    };
    bool TerrainAvailable(); std::string TerrainStatus();
    void TerrainAddStroke(const TerrainStroke& s); bool TerrainUndo(); void TerrainClear();
    std::vector<TerrainStroke> TerrainStrokes();
    bool TerrainRemoveTile(int tx, int tz, int project, std::vector<TerrainStroke>& removed, std::vector<size_t>& positions);
    void TerrainRestoreStrokes(const std::vector<TerrainStroke>& strokes, const std::vector<size_t>& positions);
    void TerrainSetProject(int from, int to);                                     // strokes saved into a project become its members
    void TerrainReplaceProject(int proj, const std::vector<TerrainStroke>& strokes); // a project's strokes as loaded from its file
    bool TerrainRemoveProject(int proj, std::vector<TerrainStroke>& removed, std::vector<size_t>& positions);
    bool TerrainNeedsApply(); void TerrainMarkApplied();
    bool TerrainApply(Vec3 back);        // fast travel 5 km away and back to 'back': the edited tiles stream again (async)
    std::string TerrainApplyState();     // "" when idle
    int TerrainPreviewGen();             // changes whenever the preview heights change
    bool TerrainTilePreview(int tx, int tz, int project, int dim, std::vector<float>* delta); // downsampled edited-minus-original heights
    bool TerrainPreviewGrid(float x0, float z0, int nx, int nz, std::vector<float>* orig, std::vector<float>* edit);   // 2 m texel grid, NaN = not loaded
    // Travel (travel.cpp): the game's own fast travel to any position (loading screen; the world streams at the destination).
    bool TravelAvailable(); bool TravelPrepared(); void TravelPrepare(); std::string TravelStatus();
    bool TravelTo(Vec3 pos, float yawDeg);   // false while the travel system is still being found (TravelPrepare runs then)
    void MarkProjectDirty(int proj);
    void GpuTrace(int seconds);   // research (gpu_research.cpp): log the game's copies into 16-bit textures for a while
    // research (terrain_research.cpp, rvas given at runtime)
    int TerrainEditDisc(float x, float z, float radius, float metres); void TerrainEditClear(); void TerrainSyncTrace();
    void TerrainJobTrace(uintptr_t rva); void TerrainLoadTrace(int slot); void TerrainTexTrace(uintptr_t rva); void TeleTraceInstall(uintptr_t rva); void ReloadStageTrace(uintptr_t rva); void RsSendTrace(uintptr_t rva); void ClientReloadTrace(uintptr_t rva); void ClientReloadReplay(float x, float y, float z); void ReloadStageReplay(float x, float y, float z); bool ResearchWatchWrites(const uintptr_t addr[4], int seconds); void TerrainTileTaskTrace(uintptr_t rva); void TerrainRetTrace(uintptr_t rva); void TerrainTexReload(int tx, int tz, uintptr_t mgr, uintptr_t rva); void TerrainReloadCall(uintptr_t obj, int slot, const std::string& name);
    void ResearchVtScan(const std::string& mangled, int maxHits, int dumpBytes);   // research: live objects of an RTTI class (logged)
    void CamWatch(int seconds, int mode = 0);  // research: logs which code writes the camera pose (hardware write breakpoints); mode 0 renderer camera, 1 camera scene object
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
    struct GroundHit { bool done = false, hit = false; float centerY = 0; float fraction = 0; Vec3 normal{}; Vec3 center{}; float radius = 0; };   // center: sphere centre at the hit
    bool GroundProbeReady();                   // a cast template was captured (the character has to be in the world for a moment)
    int  GroundProbe(Vec3 start, float len);   // queues a cast from start straight down on the game thread; ticket (0 = not possible)
    int  RayProbe(Vec3 start, Vec3 dir, float len);   // the same cast along any direction (result: GroundResult, center = hit sphere centre)
    // Raw tickets have explicit terminal outcomes. Only Pending is retryable. Terminal results are consumed;
    // Unknown means never admitted, already consumed, or evicted. Invalidated/Unknown must DISCARD a brush/drop
    // action (no paint, spawn or old-position fallback). Hit/Miss alone carry a physical cast result in out.
    enum class GroundProbeStatus { Pending, Hit, Miss, Invalidated, Unknown };
    GroundProbeStatus GroundResultState(int ticket, GroundHit* out);
    bool GroundResult(int ticket, GroundHit* out); // legacy: true only for Hit/Miss; raw UI consumers must use GroundResultState
    bool GroundGrid(float x0, float z0, int nx, int nz, float step, float top, float len, std::vector<float>* out);   // research: collision heights on a grid (NAN = none)   // true once the ticket finished (poll every frame)
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
    bool SpawnNpc(uint32_t characterKey, Vec3 world, int type = 1, uint32_t extra = 0);   // NPC / creature via the game's spawn-character request (queued to the server tick)
    int  SpawnManagedNpc(uint32_t characterKey, Vec3 world, int type = 1, uint32_t extra = 0, bool aiEnabled = true, int behavior = 0,
                         int group = 0, int proj = 0, const std::string& label = {}, const std::string& note = {});
    std::vector<ManagedNpc> ManagedNpcs();
    bool ManagedNpcLivePosition(const ManagedNpc& npc, Vec3* out);   // live actor position; false until the spawned actor is bound
    bool MoveManagedNpc(int uid, Vec3 world);           // committed move; live TransformSync when bound, respawn fallback otherwise
    bool BeginManagedNpcMove(int uid);                  // temporarily pauses runtime AI without changing the saved desired state
    bool MoveManagedNpcLive(int uid, Vec3 world);       // live drag update; call between Begin/End
    bool CommitManagedNpcMove(int uid, Vec3 world);     // commit position/history boundary but keep the edit move active and AI paused
    bool EndManagedNpcMove(int uid, Vec3 world);        // commits the position and restores the desired AI state
    bool HideManagedNpc(int uid);                       // remove the live actor, keep the registry entry for undo/project state
    bool RestoreManagedNpc(int uid);                    // respawn a hidden managed NPC
    void ForgetManagedNpc(int uid);
    bool SetManagedNpcControl(int uid, bool enabled, int behavior); // atomically set desired AI state + behavior preset
    bool SetManagedNpcAi(int uid, bool enabled);        // exact desired state; uses the game's native control-ownership toggle
    bool SetManagedNpcBehavior(int uid, int behavior);  // 0 normal/autonomous, 1 hold position (AI paused)
    void SetManagedNpcGroup(int uid, int group);
    void SetManagedNpcNote(int uid, const std::string& note);
    void SetManagedNpcLabel(int uid, const std::string& label);
    int  NpcState();                                  // 0 = not available in this game build, 1 = walk a few steps first (player actor unknown), 2 = ready
    bool NpcAiControlAvailable();                      // native NPC AI termination control resolved
#ifdef WB_UNIFIED_HOST_TEST
    // Native boundaries only; registry, generation, desired AI state, queues and persistence remain production.
    struct ManagedNpcNativeTest {
        std::function<int()> state;
        std::function<bool(uint32_t, Vec3, int, uint32_t, uintptr_t*, uint32_t*)> spawn;
        std::function<uintptr_t(uintptr_t)> transform;
        std::function<bool(uint32_t)> toggleAi;
    };
    extern ManagedNpcNativeTest g_managedNpcNativeTest;
#endif
    std::vector<SpawnedObj> Spawned();
    int  IndexOfUid(int uid);                   // -1 when the object was forgotten
    void SetGroup(int uid, int group); int NewGroupId();
    std::string GroupName(int group); void SetGroupName(int group, const std::string& name);
    void SetObjectNote(int uid, const std::string& note);
    struct MoveReq { int uid; Vec3 pos; Rot rot; float scale; };
    // C7: one authority from probe through actual final movement. Handles own immutable terminal receipts;
    // editor serial/branch reservation is separate from History insertion. No engine call holds the local
    // operation mutex (lock order operation -> registry); Applying owns leases until all members finish.
    enum GroundState { GroundProbing, GroundReady, GroundQueued, GroundApplying, GroundSettled,
                       GroundReconciled, GroundCanceled, GroundInvalidated, GroundFailed };
    struct GroundPlacement { uint64_t generation = 0, transform = 0; std::vector<int> members; };
    struct GroundMemberResult {
        SpawnedObj before, after;
        MoveReq requested{};
        bool terminal = false, accepted = false;
        std::string reason;
    };
    struct GroundView {
        uint64_t id = 0, serial = 0, branch = 0, epoch = 0;
        GroundState state = GroundProbing;
        int probe = 0; // most recent ticket identity; retained in the immutable terminal receipt
        GroundPlacement carried;
        std::vector<GroundMemberResult> members;
        std::string reason;
        bool terminal() const { return state >= GroundSettled; }
    };
    struct GroundOp;
    using GroundHandle = std::shared_ptr<GroundOp>;
    GroundHandle BeginGround(const std::vector<int>& uids, uint64_t serial, uint64_t branch,
                             const GroundPlacement& carried = {});
    GroundView GroundStateOf(const GroundHandle& op);
    bool GroundValidate(const GroundHandle& op);
    bool GroundBounds(const GroundHandle& op, proj_codec::Bounds& bounds);
    void GroundCancel(const GroundHandle& op, const char* reason = "canceled");
    // One virtual frame per Draw, independent of wall clock. 120 readiness/ticket; 240 total unapplied.
    void GroundFrame();
    int GroundTicket(const GroundHandle& op, Vec3 start, float length);
    bool GroundPoll(const GroundHandle& op, GroundHit* hit);
    bool GroundApply(const GroundHandle& op, const std::vector<MoveReq>& moves);
    // Atomically cancel unapplied members and observe the WHOLE batch. Applying is never called canceled.
    std::vector<GroundView> GroundBarrier(const std::vector<GroundHandle>& batch, bool cancelUnapplied);
    bool GroundReconcile(const std::vector<GroundHandle>& batch);
    uint64_t GroundNotice();                  // terminal publication event sequence, not UI delivery authority
    bool PublishGroundPlacement(const GroundPlacement& placement);
    // Validates the carried context AND actual member identities/poses under the admission mutex, then
    // performs a state-only editor update. The callback must not call core/engine functions.
    bool GroundUpdateCarried(const GroundHandle& op, const GroundPlacement& next, std::function<void()> update);
    void InvalidateGroundWorld();             // world/template replacement (teleport uses the same epoch)
    // Game-thread native world transition. Invalidates unapplied work immediately; Applying keeps its leases.
    // If a lease is live, copies of the callback wait behind it and resume on the GAME thread. The same world
    // authority rejects new probes/Apply until the callback finishes (also on failure); no mutex crosses native code.
    void RunGroundWorldChange(std::function<void()> change);
    bool MoveMany(const std::vector<MoveReq>& reqs, bool final);   // all moves in one game-thread job; false = dropped (live, queue lagging)
    bool HideUid(int uid); void ForgetUid(int uid);
    // SceneObject::setWorldTransform on the game thread. final=false uses flags (0,0) (update without re-insert, for dragging),
    // final=true uses (0,1) like the game's placement code (re-inserts the render instance).
    bool MoveSpawned(size_t idx, Vec3 pos, Rot rot, float scale, bool final = true);
    bool HideSpawned(size_t idx);               // scale ~0 and far below the world
    void ForgetSpawned(size_t idx);
    // C3: bring a hidden logical record back to life with a fresh physical generation. The uid and every
    // record-owned value (project, group, pose, prefab) are kept exactly as the record carries them - never an
    // older History snapshot, so a later AssignProject/SaveProject adoption survives undo/redo. false when the
    // record was explicitly forgotten, is already materialized, or the game thread is not ready yet.
    bool RestoreUid(int uid);
    extern bool g_recreateOnMove;               // false: disable/setTransform/enable in place; true: remove + re-create
    extern bool g_liveDrag;                     // live drag in the details pane (editor details checkbox + console "livedrag on/off")
    extern int  g_liveMode;                     // live-drag method, see DoLiveMove
    extern bool g_uiTextInput, g_uiMouseOverUi;  // a text field is active / the cursor is over a World Builder window (finer than g_uiWants*: edit mode claims all input)
    extern int  g_keyToggle, g_keyMode;         // configurable hotkeys (virtual key codes), settings.txt in the mod folder
    extern bool g_keyboardPlacement;             // optional legacy keyboard placement controls; off by default
    extern bool g_projectAutoSave;               // saves dirty loaded projects as soon as an edit gesture/command is committed
    extern bool g_autoFreeCamOnOpen;             // start free camera automatically when the editor opens
    extern bool g_showSelectionDetails;          // Browser selected-item information panel
    enum PlaceKey { PK_FWD, PK_BACK, PK_LEFT, PK_RIGHT, PK_UP, PK_DOWN, PK_ROT_L, PK_ROT_R, PK_SCALE_UP, PK_SCALE_DOWN, PK_FETCH, PK_SNAP, PK_MOUSE, PK_LEVEL, PK_GROUND, PK_DROP, PK_CANCEL, PK_FAST, PK_COUNT };
    extern int g_placeKeys[PK_COUNT];
    const char* PlaceKeyId(int i); const char* PlaceKeyLabel(int i); void ApplyPlaceKeys();
    extern float g_fcSpeed, g_fcSens;           // free camera: m/s and degrees per mouse count
    bool FreeCamAvailable();                    // the camera pose function and the renderer camera were found
    bool FreeCamActive();
    void SetFreeCam(bool on);                   // the editor's camera mode (key_mode) switches it; also POST /api/freecam
    bool FreeCamPose(Vec3* pos, Vec3* fwd);     // current free camera position and view direction (false while off)
    bool FreeCamBasis(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd);   // full frame of the free camera (false while off)
    void FreeCamFocus(Vec3 target, float radius); // move/look at a target; queued until the free camera finishes initializing
    void FreeCamViewPreset(int preset);          // 1 level, 2 straight down, 3 straight up; position is unchanged
    void FreeCamDolly(float meters);            // move along the view (mouse wheel)
    void FreeCamTurn(float dyaw, float dpitch);  // degrees, as the mouse would (tests without a mouse)
    bool FreeCamSetPosition(Vec3 pos);           // exact free-camera world position; false until the free camera initialized
    bool FreeCamMove(float forward, float right, float up); // relative movement in the current camera frame
    extern volatile bool g_fcHoldMove;          // editor: no key movement now (context menu open, a field being edited)
    extern bool g_showConsole;                  // settings.txt console=0 hides the console window (takes effect on the next start)

    // Environment controls. These are optional runtime-resolved features: when a
    // game patch moves the relevant code, TimeControlAvailable/WeatherControlAvailable
    // return false and the editor disables only that part of the UI.
    bool TimeControlAvailable();
    bool TimeHour(float* hour);                 // current visual time of day, 0..24
    float TimeTargetHour();                     // hour requested by World Builder while frozen / pending, else the current hour
    bool TimeFrozen();                          // freezes visual time / lighting, not gameplay
    void SetTimeHour(float hour);               // jump visual time; native progression continues unless frozen
    void SetTimeFrozen(bool frozen);
    void ResetTimeControl();                    // restore the game's native time limits / progression

    bool WeatherControlAvailable();
    bool WeatherRainAvailable();                // each control is on only when every field it writes was derived from game code
    bool WeatherCloudAvailable();
    bool WeatherWindAvailable();
    bool WeatherSnowEffectsAvailable();          // snow table field and particle bridge both derived
    bool WeatherClearSky();
    void SetWeatherClearSky(bool enabled);
    bool WeatherRainOverride(float* value);     // value 0..1; return true when override is active
    bool WeatherSnowOverride(float* value);     // value 0..1; particle bridge must also be available
    bool WeatherCloudOverride(float* value);    // direct cloud amount 0..3
    bool WeatherWindOverride(float* multiplier);// native wind multiplier 0..3
    void SetWeatherRainOverride(bool enabled, float value);
    void SetWeatherSnowOverride(bool enabled, float value);
    void SetWeatherCloudOverride(bool enabled, float value);
    void SetWeatherWindOverride(bool enabled, float multiplier);
    void ResetWeatherControl();                 // return all weather fields to native behavior

    extern bool g_httpEnabled;                  // settings.txt http_api=1 runs the loopback HTTP API (off by default, switched live in the Settings tab)
    extern int  g_httpPort;                     // settings.txt http_port= (1..65535)
    int KeyCount(); const char* KeyNameAt(int i); int KeyVkAt(int i); const char* KeyName(int vk); void SaveSettings();
    void SetTrace(bool on); bool Trace();       // log the game's own setWorldTransform/setEnable calls (reverse engineering aid)
    bool GameReadAvailable();                   // the game's resource loader can be used (instance captured, functions resolved)
    bool GameReadFile(const std::string& packPath, std::vector<uint8_t>& out, bool* notFound = nullptr);   // read a pack file through the game's loader; notFound: no entry (false = the read itself failed)
    // length > 0: only [offset, offset+length) of the entry as stored (partial textures keep their LZ4 blocks); storedTotal = its full stored size
    bool GameReadFileRange(const std::string& packPath, std::vector<uint8_t>& out, uint32_t offset, uint32_t length, uint32_t* storedTotal = nullptr, bool* notFound = nullptr);

    // The level ("scene") the player is in (scene_info.cpp): the game's level name table + the .palevel loads seen
    struct SceneLevel { std::string name, path; bool sector = false; double loadedAgo = -1; };   // loadedAgo: s since its last load, -1 = not seen
    struct SceneInfo {
        bool ok = false; std::string error;
        SceneLevel current;              // the smallest level around the player that was loaded (else the smallest)
        std::string area, areaPath;      // the place around the player: the smallest named level that is no layer, without its quadtree cell ("" if none)
        std::string sectorPath;          // its 256 m sector level
        int sectorX = 0, sectorZ = 0;    // 256 m sector of the position
        std::vector<SceneLevel> levels;  // every level whose box holds the position, smallest first
    };
    SceneInfo CurrentScene(const Vec3& pos);
    void SceneInfoOnResLoad(const std::string& path);   // the resource loader hook: every requested path

    // Prefab index (embedded RCDATA, LZ4-compressed at build time): logical path, display name, category tree, tags
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
    int  PreviewCommit();          // registers the preview object as spawned (no new spawn needed); returns uid (0 = none)
    bool PreviewActive();
    bool PreviewPending();         // a PreviewSet job is still queued (commit only once it ran)

    // Projects: bin64\cdmodkit\projects\<name>.cdproj. Legacy v1-v3 object rows stay readable; current files also persist
    // managed NPC entities, notes and named groups in backward-compatible comment records. Objects and NPCs are equal project
    // entities: both carry a project id, both load/autoload with the project and both are included in dirty/save counts.
    enum SaveScope { SaveWholeScene = 0, SaveProjectAndNew = 1, SaveNewOnly = 2, SaveProjectOnly = 3 };
    int  ProjectId(const std::string& name);       // id for a project name, creating one on first use (0 for an empty name)
    std::string ProjectNameOf(int id);             // "" for 0 / unknown
    std::string EditingProject();                 // persisted editing target; independent of the autoload list
    bool IsProjectLoaded(const std::string& name);
    bool SetEditingProject(const std::string& name); // loads the chosen project before making it editable; empty closes editing
    int  EnsureEditingProject();                  // creates and loads a numbered Untitled project on first new edit
    int  ProjectObjectCount(int id);               // visible objects + visible managed NPCs + terrain strokes (upstream scene-count semantics); 0 = new/unassigned
    bool ProjectDirty(int id);                     // an entity/metadata item changed since the last load/save
    void AssignProject(int uid, int proj);
    // Save is validate-first and transactional: the whole document (values, copy envelopes, bounds) is built
    // from the live records and re-parsed before the destination is replaced through a same-directory temporary
    // file. A failure leaves the previous bytes, the membership and the dirty stars untouched. Adoption (the
    // written records becoming members) is skipped for any record whose project changed while the file was being
    // written, so a newer adoption is never overwritten by the captured snapshot; that project then stays dirty.
    bool SaveProject(const std::string& name, int scope = SaveWholeScene);   // saved objects become members of that project
    // Load validates the full document - parse, metadata, float and tiled-int16 narrowing - BEFORE anything
    // changes: no DeleteAllSpawned, no ProjectId allocation, no queue/selection/History mutation happens for a
    // document that is rejected. Group files are never loaded here. Spawns are queued one per game tick; rows
    // the legacy state wrapper marked hidden/missing are reported as named exclusions (see ProjectLoadReportFor).
    bool PreflightProject(const std::string& name); // validate-only, before the editor History barrier
    // Synchronous outcome: false (including busy) performs no scene mutation and schedules no deferred load.
    // true means validated registry/terrain admission finished and its report is available; native spawns may
    // still be queued or later refused. Inspect report counts, never infer native attachment success from this bool.
    bool LoadProject(const std::string& name, bool clearFirst);
    // C4/C6: the last successful admission of that name reports queued objects and named exclusions.
    // Failure publishes no NEW receipt; check the bool first (an older successful receipt may still exist).
    // ProjectError names validation/busy failure; valid exclusions are not malformed parse success.
    struct ProjectLoadReport { bool valid = false; int requested = 0; int queued = 0; int excluded = 0; std::vector<std::string> excludedPrefabs;
                               int requestedNpcs = 0, queuedNpcs = 0, excludedNpcs = 0, terrainStrokes = 0; std::vector<uint32_t> excludedNpcKeys; };
    ProjectLoadReport ProjectLoadReportFor(const std::string& name);
    std::string ProjectError();                    // last save/load failure ("" = none); names the data-record ordinal
    // validate-first copy of a shared .cdproj into bin64\cdmodkit\projects: malformed bytes are refused and
    // nothing appears on disk (the previous file of that name, if any, is untouched)
    bool ImportProjectFile(const std::string& path);
    bool ImportGroupFile(const std::string& path);
    bool UnloadProject(int id); // false = busy/empty/invalid, no deferred unload; true = logical removal done, native cleanup may remain queued
    bool ReloadProject(const std::string& name); // same synchronous outcome as Load; ONE snapshot validated before any old entities are removed
    bool ProjectMutationPending(int id); // live AUTOSAVE/readiness gate only; false is NOT a success/completion receipt
    std::vector<std::string> ListProjects();
    // Clear's synchronous success authority for the editor: false = busy, no scene mutation or deferred action;
    // true = all logical objects/NPCs/terrain removed (including an already-empty scene). Native cleanup may be queued.
    // Only true authorizes the caller's selection/History cleanup. ProjectMutationPending is never an outcome.
    bool ClearScene();
    void DeleteAllSpawned(); // compatibility only: calls ClearScene and logs refusal; NEVER defers. UI must use the bool.
    // Autoload: bin64\cdmodkit\autoload.txt, one project name per line (without .cdproj), '#' at the line start = comment.
    // Several projects can be active at once; they are all loaded, in file order, once the player is in the world.
    // File lifecycle never unloads objects, clears History, or implicitly switches autoload OFF.
    void AutoloadFrame(); // called from the game-thread pump, including while the editor window is closed
    enum class FileReason {
        None, InvalidName, InvalidAction, NotFound, StaleTarget, ConfirmationMismatch, GuardMissing,
        SelectionChanged, VisibleReference, HiddenReference, UndoReference, RedoReference, DirtyProject,
        PendingOperation, InFlightExport, InFlightPlace, AutoloadEnabled, AutoloadReadFailed,
        Collision, UnsafePath, ReadFailed, WriteFailed, MoveFailed, DeleteFailed, VerifyFailed
    };
    struct FileResult {
        FileReason reason = FileReason::None;
        DWORD systemError = 0;
        bool ok() const { return reason == FileReason::None; }
    };
    const char* FileReasonCode(FileReason reason); // stable machine code; UI localizes separately
    bool FileNameEqual(const std::string& a, const std::string& b); // Windows ordinal casefold, also for the editor's reference guard
    struct SavedFile { proj_codec::Kind kind = proj_codec::Kind::Project; bool archived = false; std::string filename, path; };
    struct FileSelection; // immutable file identity + exact bytes captured when the action/modal opens
    using FileSelectionHandle = std::shared_ptr<const FileSelection>;
    FileResult ListSavedFiles(proj_codec::Kind kind, bool archived, std::vector<SavedFile>& files);
    // Caller-owned library cache. Refresh is read-only and publishes only on success; a failure preserves
    // the previous snapshot. All four locations are listed, then ownership is counted in ONE registry pass.
    // visible/hidden count OBJECTS only; NPCs and terrain have separate components. Pending is a subset of visible, not an added entity.
    // Windows-equivalent project names share counts; groups have none. These are display values, NOT an
    // archive/delete authority: SelectSavedFile and the live execution guard are still required.
    struct ProjectOwnership { size_t visible = 0, hidden = 0; bool dirty = false; size_t terrain = 0;
                              size_t visibleNpcs = 0, hiddenNpcs = 0, pendingObjects = 0, pendingNpcs = 0; };
    struct SavedLibraryEntry { SavedFile file; ProjectOwnership ownership; };
    struct SavedLibraryTotals { size_t projects = 0, groups = 0; };
    struct SavedLibrarySnapshot {
        std::vector<SavedLibraryEntry> entries;
        SavedLibraryTotals active, archived;
        ProjectOwnership unassigned;
    };
    enum class SavedLibraryKind { All, Projects, Groups };
    enum class SavedLibraryLocation { Active, Archived, All };
    // Order: ordinal case-insensitive full filename, exact ordinal case, kind, active before archived,
    // exact path. No filesystem enumeration order or allocated ProjectId participates in the tie-break.
    FileResult RefreshSavedLibrary(SavedLibrarySnapshot& snapshot);
    // Case-insensitive ordinal substring search of the full filename; empty search matches all names.
    // Returns indices into this snapshot in its sorted order. Filtering performs no file or scene reads.
    std::vector<size_t> FilterSavedLibrary(const SavedLibrarySnapshot& snapshot, SavedLibraryKind kind,
                                          SavedLibraryLocation location, const std::string& search);
#ifdef WB_UNIFIED_HOST_TEST
    // Observes Spawned, ProjectObjectCount and library refresh, under the registry lock. Tests reset only
    // while quiescent, so a per-row fallback to either older enumeration API cannot escape the work bound.
    struct SceneEnumerationStats { size_t calls = 0, records = 0; };
    extern SceneEnumerationStats g_sceneEnumerationStats;
#endif
    FileResult SelectSavedFile(const SavedFile& file, FileSelectionHandle& selection);
    SavedFile SelectedFile(const FileSelectionHandle& selection);
    enum class FileAction { Archive, Restore, Purge, Delete };
    // Required execution-time editor authority, NOT a cached boolean from the rendered row. Called on the
    // editor thread immediately before the protected core check/mutation. It must compare the CURRENT selected
    // kind/archive/filename/path and inspect BOTH History stacks (act.proj AND current record ownership),
    // deferred intents, pending reports and carried Place for this file. Return SelectionChanged / UndoReference /
    // RedoReference / PendingOperation / InFlightPlace / InFlightExport, or None. No editor mutation or pumping
    // inside the callback. An absent authority fails closed. Core independently guards registry/dirty/autoload
    // and in-flight core file work. Task 13 owns wiring this authority to its UI state.
    using FileExecutionGuard = std::function<FileReason(const SavedFile&)>;
    FileResult ExecuteFileAction(const FileSelectionHandle& selection, FileAction action,
                                 const std::string& typedFilename, const FileExecutionGuard& guard);
#ifdef WB_UNIFIED_HOST_TEST
    enum class FileMutationFault { None, Move, Delete };
    extern FileMutationFault g_fileMutationFault; // one-shot OS-boundary refusal, after all production guards
#endif
    FileResult SetAutoload(const std::string& name, bool on); // atomic, reopened verification; empty list deletes autoload.txt
    std::vector<std::string> Autoload();
    int  PendingSpawns();

    // ---- C5: per-request caller-owned terminal placement results ----------------------------------------
    // One placement REQUEST owns a fixed set of rows (original row id + prefab). The caller (editor UI) holds
    // the shared handle and reads its receipt; every callback resolves the row through the record it created
    // (SpawnedObj::placeReq/placeRow), never through a basename or a global "last result". A row moves from
    // Pending to exactly one terminal state - Attached (a real engine attachment on a FINAL lane), Excluded
    // (preflight missing/hidden/nonplaceable), Failed (actual final engine failure) or Canceled - so
    //     requested == attached + excluded + failed + canceled + pending
    // holds at every observation. An intermediate stand-in during a retry is NOT a terminal attachment and
    // stays Pending; the accepted plain-object fallback after the retry budget is spent records its own lane.
    // Hide/Undo after completion never rewrites a settled receipt. PendingSpawns() is a queue diagnostic only.
    enum PlaceRowState { PlacePending = 0, PlaceAttached = 1, PlaceExcluded = 2, PlaceFailed = 3, PlaceCanceled = 4 };
    enum PlaceLane {
        PlaceLaneNone    = 0,   // no engine attachment (excluded / failed / canceled)
        PlaceLaneGeneric = 1,   // the plain client-object lane (SpawnAt -> DoSpawn)
        PlaceLaneDirect  = 2,   // the template-free server builder
        PlaceLaneReplay  = 3,   // the captured-template server replay
        PlaceLanePlain   = 4,   // the accepted plain-object fallback after the interactive retry budget was spent
        PlaceLaneStandin = 5    // an intermediate client stand-in during a retry: observed, never a terminal attachment
    };
    struct PlaceRow {
        int rowId = 0;                    // fixed original row id inside the request
        std::string prefab;
        int state = PlacePending;         // PlaceRowState
        int lane = PlaceLaneNone;         // the lane that actually attached the row
        int uid = 0;                      // logical record uid once admitted (0 = never admitted)
        int attachObservations = 0;       // diagnostic: every real engine attach callback seen for this row (a stand-in counts; the terminal transition happens once)
        bool removedAfterAttach = false;  // a cancel removed this row's already attached member
        int cleanupObligations = 0;       // counted cleanup obligations: every queued engine removal that must precede finality holds one, released by its own lane completion after dispatch
        bool cleanupPending = false;      // derived mirror of (cleanupObligations > 0), kept for existing observers
        std::string reason;               // named exclusion/failure reason (empty for Attached)
    };
    struct PlaceRequestView {
        int requested = 0, attached = 0, excluded = 0, failed = 0, canceled = 0, pending = 0;
        bool settled = false;             // no pending rows and no cancel cleanup left
        bool requestCanceled = false;
        bool cleanupPending = false;
        std::vector<PlaceRow> rows;
        // the existing C5 UI projection (four totals); Failed and Canceled are displayed under excluded
        int displayedPlaced()   const { return attached; }
        int displayedExcluded() const { return excluded + failed + canceled; }
        int displayedPending()  const { return pending; }
    };
    // Opaque, caller-owned. The UI holds one handle and reads its own receipt; callbacks use the row identity
    // carried by the record, so two requests can share a basename without sharing a result.
    struct PlaceRequest {
        std::vector<PlaceRow> rows;
        bool canceled = false;
        int cleanupOutstanding = 0;       // queued engine removals that must precede finality: always the sum of the rows' cleanupObligations
        std::mutex m;                     // serializes row settlement; never held while the registry lock is taken
    };
    typedef std::shared_ptr<PlaceRequest> PlaceRequestHandle;
    // The requested count is fixed here (one row per prefab, row ids 0..n-1).
    PlaceRequestHandle BeginPlaceRequest(const std::vector<std::string>& prefabs);
    // Spawn one row through the production lanes and bind the record to this request+row. `group` is the
    // already-remapped session partition (0 = ungrouped). Returns the logical uid (0 when the row was
    // terminally excluded before any spawn).
    int  SubmitPlaceRow(const PlaceRequestHandle& req, int rowId, Vec3 world, Rot rot = Rot(), float scale = 1.0f, int group = 0);
#ifdef WB_UNIFIED_HOST_TEST
    // B2-R1 deterministic admission-interrupt seam: SubmitPlaceRow invokes this after the record and its
    // executable work are published but before the row UID is written, so a test can deterministically cancel
    // inside the actual in-flight native boundary with row.uid == 0. Null by default: admission is
    // uninterrupted. Never set outside host tests.
    extern std::function<void()> g_admissionInterrupt;
#endif
    // Preflight (caller-side) exclusion: missing/hidden/nonplaceable rows never spawn.
    bool PlaceRowExclude(const PlaceRequestHandle& req, int rowId, const std::string& reason);
    bool PlaceRowCancel(const PlaceRequestHandle& req, int rowId);
    // Cancel invalidates every unapplied row and removes the already attached members through the existing
    // HideUid semantics, keeping their terminal Attached receipts. Returns how many rows it moved/cleaned.
    int  PlaceRequestCancel(const PlaceRequestHandle& req);
    PlaceRequestView PlaceRequestState(const PlaceRequestHandle& req);

    // ---- C4/C5: validated group admission (independent copies) ------------------------------------------
    // One parsed group document becomes ONE independent copy in the scene. Every destination value (record
    // pose, envelope bounds, pivot anchor) is computed by the saved-anchor geometry BEFORE any registry/queue/
    // History mutation, so a rejected admission mutates nothing. Partitions are remapped per copy: every
    // nonzero source group id gets a fresh nonzero session id (zero stays zero), so two admitted copies of the
    // same source never share a partition. The saved anchor is the pivot and every envelope is carried per copy
    // - never recomputed from the receiver's measurements, not even when a single member survives. Records are
    // spawned through the caller-owned C5 request, so each row settles from its actual engine outcome; `uids`
    // and `pivot` feed the existing camera-aware grab. A prefab the receiver does not know is a NAMED
    // exclusion, never a parse failure.
    struct GroupAdmissionReport {
        bool valid = false;                        // true only when a copy was admitted (even with named exclusions)
        int requested = 0, admitted = 0, excluded = 0;
        std::vector<std::string> excludedPrefabs;  // named exclusions (missing prefab / refused admission)
        std::string error;                         // named failure ("" = none)
        PlaceRequestHandle request;                // the caller-owned per-record C5 receipt
    };
    // Admit one validated group copy: `uids` receives the admitted logical records and `pivot` the saved
    // anchor's destination (the placement center the existing grab must use).
    GroupAdmissionReport AdmitGroupCopy(const proj_codec::Document& doc, Vec3 target, double deltaYaw, double factor,
                                        std::vector<int>& uids, Vec3& pivot);

    // File-backed Place: rechecks the selected file's identity/bytes, parses it, then uses AdmitGroupCopy.
    // Core tracks this file until the request settles INCLUDING cleanup. Carried-after-settlement is still
    // editor-owned and must be rejected by FileExecutionGuard. Previously placed copies never block the file.
    GroupAdmissionReport AdmitGroupFileCopy(const FileSelectionHandle& selection, Vec3 target, double deltaYaw,
                                            double factor, std::vector<int>& uids, Vec3& pivot);

    // ---- C6: protected group export ---------------------------------------------------------------------
    // One selection becomes one .cdgroup document through the SAME authoritative value builder SaveProject
    // uses (never a second builder and never cached bytes) and one transactional file write. The approval
    // binds what the write must still observe: the selected UIDs, the selection/project/placement context the
    // editor PUBLISHED, every inclusion/exclusion with its named reason, and the full values and envelopes.
    // There is no global revision counter: each final boundary re-derives the semantic values from the live
    // registry and compares them, and the protected replacement holds BOTH authorities - the selection
    // publication and the registry - so neither can change between the last comparison and the atomic rename.
    // A live final=false move, a changed selection/name/placement, a changed inclusion/exclusion or a changed
    // envelope therefore rejects a stale approval; a rejected write leaves the previous file bytes untouched
    // and removes only its own temporary.
    struct ExportContext {
        std::vector<int> selection;   // selected logical uids (order-insensitive; duplicates are tolerated)
        std::string name;             // the destination name this context exports under
        bool placing = false;         // a placement/grab is in progress
        std::vector<int> carried;     // the uids that placement carries (order-insensitive)
    };
    // The editor publishes its live context on every selection/context mutation (including the destination
    // name it would export under) and at the end of every frame that can mutate it, so the export guard always
    // reads a fresh authority. PrepareGroupExport requires the passed selection AND name to be that published
    // context. Publication touches nothing else and never takes the registry lock.
    void PublishExportContext(const ExportContext& context);
    ExportContext PublishedExportContext();

    // One named exclusion: the uid, the prefab and the reason the selected object is not part of the group.
    struct ExportExclusion { int uid = 0; std::string prefab; std::string reason; };
    // What the editor holds between preflight and confirmation. `document` is the approved value document
    // (kind=Group); WriteGroupExport re-derives everything from the live state and refuses an approval the
    // live state no longer matches, so a tampered or stale approval can never replace the destination.
    struct GroupExportApproval {
        bool valid = false;                        // prepared from authoritative values and a fresh publication
        std::string name;                          // destination group name (no extension)
        std::string path;                          // full destination path (<moddir>\groups\<name>.cdgroup)
        bool overwriteApproved = false;            // explicit approval to replace an existing file
        proj_codec::Document document;             // the approved semantic values/envelopes (kind=Group)
        std::vector<int> included;                 // included uids, registry order
        std::vector<int> projects;                 // parallel to included: each record's project id
        std::vector<ExportExclusion> excluded;     // named exclusions, registry order (forgotten uids appended)
        ExportContext context;                     // the published context this approval is bound to
        std::string error;                         // named failure ("" = none)
    };
    // PREFLIGHT: builds the document from the authoritative value builder, names every inclusion/exclusion and
    // rejects an empty selection, an all-excluded selection, an invalid name/path, an unpublished selection or
    // context, and an existing destination without explicit overwrite approval. Mutates nothing.
    bool PrepareGroupExport(const std::vector<int>& selection, const std::string& name, bool overwriteApproved,
                            GroupExportApproval& approval);
    // WRITE: validates the approval against the live registry and the published context BEFORE writing, writes
    // a same-directory temporary and re-parses it, then validates again immediately before the protected
    // replacement (both authorities held across the rename). Returns true only when the destination was
    // actually replaced; every failure leaves the previous bytes and removes only the owned temporary.
    bool WriteGroupExport(const GroupExportApproval& approval, std::string& error);
}

namespace overlay { void Install(); }
