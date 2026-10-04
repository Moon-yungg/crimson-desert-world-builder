// Declarations shared between cdmodkit.cpp (core) and diag.cpp (reverse-engineering aids).
// Not part of the public API: cdmodkit_api.h and core.h are what other code should use.
#pragma once
#include "core.h"
#include <utility>

namespace core {
    // Internal hook wrapper used by optional feature modules. It keeps the same
    // near-image trampoline reservation/retry behavior as the core hooks.
    bool InstallInternalHook(void* target, void* detour, void** original, const char* name);

    // Optional environment bridge (time-of-day + weather). Resolution failures
    // only disable these controls and never make the rest of World Builder fail.
    void EnvironmentInstall();
    void EnvironmentTick();

    // camera objects reachable from the camera manager, with their RTTI names; used by the fov / camera traces
    void ExpandCameraManager(std::vector<std::pair<std::string, uintptr_t>>& out);

    // the renderer camera through its own object (cdmodkit.cpp); false when unresolved or the block does not validate
    bool WatchWrites(const uintptr_t addr[4], int seconds, const char* tag);
    bool WatchAccessSync(const uintptr_t addr[4], int seconds, const char* tag);   // read/write watch armed before returning (the caller's own next access is caught)
    bool StartWatch(const uintptr_t addr[4]); void RefreshWatch(); void StopWatch(); void DumpWatch(const char* tag, uint64_t from, uint64_t to);   // continuous write watch   // diag.cpp: hardware write breakpoints, writers logged
    uintptr_t CameraSceneObject();    // the camera manager's scene object (the pose the game's camera logic produces), 0 if unknown
    uintptr_t NativeCameraObject();   // the renderer camera object itself (0 when unresolved or its type does not match)

    // pack I/O tracing (diag.cpp): installed once at startup, switched by the console command "traceio on|off"
    void InstallIoTrace();
    void SetIoTrace(bool on);

    // helpers for optional modules (cdmodkit.cpp)
    uintptr_t SigScanUnique(const char* pat); uintptr_t VtableByName(const char* mangled); std::string PathObjText(void* path);
    void TerrainInstall(); void TravelInstall();
    // live terrain (terrain_live.cpp)
    void TerrainLiveInstall(); bool TerrainLiveAvailable(); bool TerrainLiveHasTexture(int tx, int tz);
    void TerrainLiveNoteRead(int tx, int tz, const uint8_t* data, uint32_t len);
    bool TerrainLiveUpload(int tx, int tz, const uint8_t* chain, size_t len);
    void TerrainPhysInstall(); int TerrainPhysSync(int tx, int tz, const float* prev, const float* next);   // terrain_physics.cpp
    // play mode (playmode.cpp): an isolated start requested by bin64\cdmodkit\playmode.json; inert without the file
    void PlayModeLoad();                 // at attach: read + consume the request
    void PlayModeInstall();              // after ResolveGame: the hooks a consumed request needs
    void PlayModeAbandon(const char* why);   // ResolveGame failed: a consumed request is dropped, saves stay allowed
    void PlayModeTick();                 // game thread pump
    bool PlayModeActive(); bool PlayModeIsolating();
    bool PlayModeOnCreate(uintptr_t retRva, const std::string& prefab, const float* xf, uint8_t f1, uint8_t f2, uint8_t f3, bool ours);   // true = do not create
    bool PlayModeOnResLoad(const std::string& path);   // true = an isolated level (loaded as PlayModeEmptyLevel())
    std::string PlayModeSwapPath(const std::string& path);   // the game path to load instead ("" = as asked): isolated level / pack override
    void PlayModeAfterResLoad(const std::string& path, uintptr_t handler);   // local-file overrides, scene level scan
    bool PlayModeWorkerRead(uintptr_t handler, uint8_t* buf, uint32_t cap, uint32_t off, uint32_t len, bool after, bool* ok);   // load worker vslot 5
    const char* PlayModeEmptyLevel();
    void PlayModeOnStream(const std::string& path);    // the texture / mesh streamer's requests (terrain.cpp hook)
    bool PlayModeStageOverride(float* tf);   // a stage reload of the game's own during the first load: redirected
    bool PlayModeLoadStarted();
    bool PlayModeExit(const std::string& mode);   // "quit" or "travel"
    std::string PlayModeStatusJson(bool census);
    void PlayModeOurSpawn(bool on);      // marks this thread's server work as World Builder's own (never filtered)
    // helpers for play mode (cdmodkit.cpp)
    bool OurServerSpawnOnThisThread();   // a World Builder gimmick spawn runs on this thread right now
    bool ServerFieldTicking();           // the in-process server field ticks (a game is loading or running)
    uint32_t GameHashOf(const char* s);  // the game's string hash (error codes, reason names); 0 when unresolved
    uintptr_t StaticObjectWithVtable(uintptr_t vt, int* count);   // first object in the image's writable data whose vtable is vt
}
