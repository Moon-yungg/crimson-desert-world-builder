// production_host.h - CORE host seam for the production core (plan wb079-unified-77f68967, Tasks 7+).
//
// production_core_host.cpp includes asi/cdmodkit/cdmodkit.cpp exactly once with WB_UNIFIED_HOST_TEST
// defined; the guards in that TU replace ONLY the native engine/service boundaries (creation, removal,
// transforms, actor removal, gimmick server spawn/replay, thread readiness) with calls through
// host::Seam(). A fixture installs its oracle behind those boundaries BEFORE its first core call.
//
// DllMain / Attach / InitThread / hook installation / image scanning are never executed here, and no
// production lifecycle, queue, registry or History logic is re-implemented by a fixture: the queues
// (g_queue, g_serverJobs, g_gimmickQueue), the pump dispatch, the registry and the retry/stand-in
// fallback run as production code and are pumped by host::PumpGame / host::PumpServer.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include "../../asi/cdmodkit/core.h"
#include "../../asi/cdmodkit/thumbgen.h"

namespace host {

// One function per substituted native boundary. Empty std::function = "boundary unavailable"
// (creation returns 0, removal returns false, the gimmick paths report a refusal). Every call is
// made from production code on the thread the boundary belongs to; the registry lock is not held.
struct Engine {
    bool ready = false;                                                              // GameThreadReady()
    bool probeReady = false;
    std::function<bool(Vec3* out)> playerWorldPos;                                 // OS/engine boundary: the game-memory player transform read
    std::function<bool(Vec3, float, core::GroundHit*)> groundCast;
    std::function<bool(Vec3)> teleport;
    std::function<uintptr_t(const std::string& prefab, Vec3 pos, Rot rot, float scale)> createGeneric;   // 0 = refused
    std::function<bool(uintptr_t handle)> remove;                                    // setEnable(0); false = not a live handle
    std::function<bool(uintptr_t handle, Vec3 pos, Rot rot, float scale)> moveInPlace;   // final move without re-create
    std::function<bool(uintptr_t handle, Vec3 pos, Rot rot, float scale)> liveMove;      // visual-only drag update
    std::function<bool(uintptr_t actor)> removeActor;                                // server-side actor removal
    std::function<bool(const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor)> directGimmick;
    std::function<bool(int templateId, const std::string& prefab, Vec3 pos, Rot rot, float scale, uintptr_t* so, uintptr_t* actor)> replay;
    std::function<bool()> templateReady;                                             // a captured spawn template exists
    int nextTemplateId = 1;                                                          // ids handed to the production retry bookkeeping
};

Engine& Seam();

// ---- host-side controls and observations (implemented in production_core_host.cpp) ----
void OpenLog(const std::string& path);     // opens the production log sink; the run log the runner requires
bool PumpGame();                           // dispatch ONE game-thread job on this thread (production PumpJobs); false = empty
void PumpServer();                         // run the pending server jobs and one gimmick queue step (production code)
int  NextUid();                            // the production uid allocator's next value (identity assertions)
struct AllocatorState { int object, npc, group; size_t projects; };
AllocatorState Allocators();               // read-only validation-before-mutation observation
unsigned NpcPositionOffset();               // actual native TransformSync ABI, no copied movement implementation
bool SetDirectGimmick(bool on);            // enable/disable the template-free server path + ServerField presence
void NoteWorkQueued(bool registryLockHeld); // production code reports whether a queue push happened under the registry lock
int  LockViolations();                     // how many pushes were made under that lock (host lock-discipline assertion)
bool RegistryLockFree();                   // try-acquire the production registry lock (call from a probe thread, never the lock owner)
size_t RegistrySize();                     // records currently registered (including hidden tombstones)

// ---- Task 9 file/autoload seams (host builds only; the shipped ASI compiles none of this) ----
void SetModDir(const std::string& dir);    // point the production mod dir at the fixture directory (real files on disk)
enum class SaveFault { None = 0, WriteAbort = 1, ShortWrite = 2, FlushAbort = 3, ReplaceAbort = 4, CloseAbort = 5 };
void SetSaveFault(SaveFault fault);        // one-shot fault honored by the production transactional writes (save/import/export)
void SetBeforeReplace(std::function<void()> callback);   // runs after readback, before the replace (C6 approval seam, save/import/export)
void AutoloadRun();                        // the production autoload action (file order; a group file is rejected there)
void ResetAutoloadDone();                  // clears the per-session autoload marker for deterministic case isolation

// ---- Task 11 admission seam (host builds only) ----
void SetPrefabIndex(const std::vector<core::PrefabInfo>& index);   // the receiver's prefab cache the group admission preflights against

// ---- Task 12 export seam (host builds only) ----
// Observation seam inside the PROTECTED region of the export replacement: it runs after the last authoritative
// comparison and before the rename, while the selection publication and the registry are held. A fixture uses
// it to prove - from a second thread - that neither authority can change between the check and the rename.
// It is not a fault seam and not a substitute writer: the production code performs the real rename right after.
void SetExportReplaceProbe(std::function<void()> callback);
bool ExportContextLockFree();   // try-locks the selection-publication guard (call from a probe thread, never the guard owner)

// Task 13: manually select queued production work; no substitute movement/History policy.
bool PumpGameAt(size_t index);
void PumpPhysics(bool reverse = false);
bool GroundLocksFree(); // second-thread observation of operation/registry/probe/CORE/server locks
size_t GroundLeaseCount();
size_t GroundDeferredCount();
size_t GroundTicketCount();
uint64_t GroundEpoch();
unsigned GroundWorldWriters();
void SetGroundRadius(float radius); // controlled calibration; no native player-memory read
bool RawGroundDirection(int ticket, Vec3* direction); // observes production admission, not the cast substitute
void SetSpawnQuietAge(DWORD worldAge, DWORD gameSpawnAge);

// Actual travel.cpp with only the native stage entry/manager memory supplied by the fixture.
using TravelStage = std::function<void(uint32_t, uint32_t, uint32_t, const float*)>;
void SetTravelStage(TravelStage stage);
void NativeTravel(const float* transform); // calls the production game-initiated HookStage

// Actual terrain.cpp; native file/player/clock/wait boundaries are deterministic. Detached workers
// are joined in the test TU so teardown cannot outlive their owning fixture. No terrain policy is copied.
void SetTerrainAvailable(bool available);
void ResetTerrainClock(Vec3 player);
bool WaitTerrainStep(unsigned step); // exact worker wait entry, bounded event wait
void ResumeTerrainStep(unsigned step, DWORD elapsedMs);
void SetTerrainPlayer(Vec3 player);

// ---- Task 15 console seam (host builds only) ----
// The production console dispatcher in cdmodkit.cpp (the exact function ConsoleThread feeds line by line);
// true = the console loop would end. No second dispatcher exists for tests.
bool ConsoleDispatch(const std::string& cmd);
// Observation boundary for the research services whose real bodies live in diag.cpp/thumbgen.cpp and are
// unavailable in a host run: the substitute records the call; it never replaces the production dispatch.
struct ConsoleServices {
    std::function<void()> viewScan;                    // core::ViewScan substitute
    std::function<void()> findCamera;                  // core::FindRenderCamera substitute
    std::function<void(int, int)> camWatch;            // core::CamWatch substitute (seconds, mode)
    std::function<void(int)> fovTrace;                 // core::FovTrace substitute (seconds)
    std::function<void(int)> camTrace;                 // core::CamTrace substitute (seconds)
    std::function<void(bool)> ioTrace;                 // core::SetIoTrace substitute
    std::function<void(bool)> thumbsBackground;        // thumbgen::SetBackground substitute
};
ConsoleServices& Services();

// Task16: native thumbnail service observations and camera-memory fixture setup.
// No editor action or page dispatcher is substituted.
struct UiServices {
    bool thumbnailsReady = false;
    std::shared_ptr<const std::vector<thumbgen::CharInfo>> characters;
    std::function<void(const std::string&)> refreshPreview;
};
UiServices& Ui();
void SetCameraAvailable(bool available);
void CaptureCamera(Vec3 pos, float yaw, float pitch);

} // namespace host
