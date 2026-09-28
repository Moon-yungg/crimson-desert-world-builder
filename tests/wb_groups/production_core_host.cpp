// production_core_host.cpp - CORE host TU for the unified suite set (plan wb079-unified-77f68967, Task 7).
//
// This is the one place where the production core TU (asi/cdmodkit/cdmodkit.cpp) is compiled for a host
// suite: WB_UNIFIED_HOST_TEST makes the boundaries marked in that TU route through host::Seam(), and the
// oracle behind them belongs to the fixture. The registry, the queues, the pump dispatch, the spawn
// lifecycle, project membership and the editor History all stay production code; DllMain, Attach,
// InitThread, hook installation and image scanning are never executed here.
//
// The service boundaries CORE does not link (the thumbnail worker, the HTTP API, the overlay's texture
// cache and MinHook installation) are explicit host substitutes below: not copies of production logic,
// only the "unavailable" answer each of them would give without its native dependency.
#ifndef WB_UNIFIED_HOST_TEST
#define WB_UNIFIED_HOST_TEST
#endif
#include "../../asi/cdmodkit/cdmodkit.cpp"
#include "../../asi/cdmodkit/overlay.h"   // the overlay texture boundary (only its declaration is substituted below)

#include <mutex>

// ---- core services that live in TUs CORE does not link (diag.cpp) ----
// The substitute records the call for a fixture (host::Services); it is still the "unavailable" answer.
namespace core {
void InstallIoTrace() {}
void SetIoTrace(bool on) { if (host::Services().ioTrace) host::Services().ioTrace(on); }
void ViewScan() { if (host::Services().viewScan) host::Services().viewScan(); }
// RenderCamera and debug-point storage now live in the real current-main core TU above.
void FindRenderCamera() { if (host::Services().findCamera) host::Services().findCamera(); }
void CamWatch(int seconds, int mode) { if (host::Services().camWatch) host::Services().camWatch(seconds, mode); }
void FovTrace(int seconds) { if (host::Services().fovTrace) host::Services().fovTrace(seconds); }
void CamTrace(int seconds) { if (host::Services().camTrace) host::Services().camTrace(seconds); }
}

// ---- thumbnail worker (thumbgen.cpp): unavailable in a host run ----
namespace thumbgen {
namespace { bool s_background = false; }
void Start() {}
void Request(const std::string&) {}
void Refresh(const std::string& path) { if (host::Ui().refreshPreview) host::Ui().refreshPreview(path); }
bool Pending(const std::string&) { return false; }
bool Processed(const std::string&) { return false; }
bool Ready() { return host::Ui().thumbnailsReady; }
bool Idle() { return true; }
void SetBackground(bool on) { s_background = on; if (host::Services().thumbsBackground) host::Services().thumbsBackground(on); }
void SetQuality(int) {}
int Quality() { return 0; }
bool Background() { return s_background; }
int Done() { return 0; }
void WantNamesLanguage(const std::string&) {}
std::shared_ptr<const std::unordered_map<std::string, std::string>> GameNames() { return nullptr; }
uint32_t GimmickKey(const std::string&) { return 1; }   // the direct gimmick path only needs a nonzero key
std::shared_ptr<const std::vector<CharInfo>> Characters() { return host::Ui().characters; }
bool PassProgress(int*, int*) { return false; }
int Failed() { return 0; }
int Total() { return 0; }
int Generation() { return 0; }
std::vector<std::string> TakeRefreshed() { return {}; }
bool Lz4Decode(const unsigned char*, size_t, std::vector<unsigned char>&, size_t) { return false; }
const char* Error() { return "thumbnails are not available in a host run"; }
}

// ---- loopback HTTP API (http_api.cpp): never started in a host run ----
namespace httpapi {
bool Start(int) { return false; }
void Stop() {}
int ActivePort() { return 0; }
std::string LastError() { return "the HTTP API is not available in a host run"; }
}

// ---- overlay texture cache and installation (overlay.cpp): no renderer in a host run ----
namespace overlay {
void Install() {}
ImTextureID Thumb(const std::string&, int*, int*) { return static_cast<ImTextureID>(0); }
}

// ---- MinHook (installation never runs in a host run) ----
extern "C" {
MH_STATUS WINAPI MH_Initialize(VOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_Uninitialize(VOID) { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_FUNCTION_NOT_FOUND; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_DISABLED; }
MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_ERROR_DISABLED; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_ERROR_NOT_CREATED; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "MinHook is not installed in a host run"; }
}

namespace host {
namespace {
struct State {
    Engine engine;
    std::mutex noteMutex;
    int lockViolations = 0;
};
State& S() { static State s; return s; }
}   // namespace

Engine& Seam() { return S().engine; }

void OpenLog(const std::string& path) {
    if (core::g_log) { std::fflush(core::g_log); std::fclose(core::g_log); }
    core::g_log = std::fopen(path.c_str(), "a");
}

bool PumpGame() {
    if (InterlockedCompareExchange(&core::g_queueCount, 0, 0) == 0) return false;
    core::PumpJobs();          // the production dispatch: one job, once
    return true;
}

void PumpServer() {             // the production ServerField slot 9 tick body
    core::ProcessServerJobs();
    core::ProcessGimmickQueue();
}

int NextUid() { return core::g_nextUid; }
AllocatorState Allocators() {
    std::lock_guard<std::mutex> lock(core::g_regMutex);
    return {core::g_nextUid, core::g_nextNpcUid, core::g_nextGroup, core::g_projNames.size()};
}
unsigned NpcPositionOffset() { return core::kOff_Tf_Pos; }

bool SetDirectGimmick(bool on) {
    core::kRva_GimmickSpawn_ = 1;                       // the gimmick route exists for the host
    core::kRva_GimmickFromSave = on ? 1 : 0;            // the template-free builder
    core::g_serverFieldObj = on ? 1 : 0;                // the ServerField whose tick would run it
    if (on) SetSpawnQuietAge(20001, 10001);              // retain main's real quiet-world guard without sleeping
    return on;
}

void NoteWorkQueued(bool registryLockHeld) {
    if (!registryLockHeld) return;
    std::lock_guard<std::mutex> l(S().noteMutex);
    ++S().lockViolations;
}

int LockViolations() {
    std::lock_guard<std::mutex> l(S().noteMutex);
    return S().lockViolations;
}

bool RegistryLockFree() {
    std::unique_lock<std::mutex> l(core::g_regMutex, std::try_to_lock);
    return l.owns_lock();
}

size_t RegistrySize() { return core::g_reg.size(); }

void SetModDir(const std::string& dir) { core::g_modDir = dir; }
void SetSaveFault(SaveFault fault) { core::g_saveFaultStage = static_cast<int>(fault); }
void SetBeforeReplace(std::function<void()> callback) { core::g_beforeReplaceCallback = std::move(callback); }
void AutoloadRun() { core::AutoloadProjects(); }
void ResetAutoloadDone() { core::g_autoDone = false; }
void SetPrefabIndex(const std::vector<core::PrefabInfo>& index) {
    core::g_index = index;                        // the receiver's cache the admission preflights against
    core::g_byPath.clear();
    core::g_cats.clear(); core::g_cats.push_back({ "all", -1, {}, {}, 0 });
    for (size_t i = 0; i < index.size(); ++i) {
        core::g_byPath[index[i].path] = (int)i;
        auto& pi = core::g_index[i]; pi.cat = core::CatFor(pi.path);
        core::g_cats[pi.cat].prefabs.push_back((int)i);
        for (int n = pi.cat; n >= 0; n = core::g_cats[n].parent) ++core::g_cats[n].total;
    }
}

void SetExportReplaceProbe(std::function<void()> callback) { core::g_exportReplaceProbe = std::move(callback); }
bool ExportContextLockFree() {
    std::unique_lock<std::mutex> l(core::g_exportCtxMutex, std::try_to_lock);
    return l.owns_lock();
}

bool PumpGameAt(size_t index) {
    { std::lock_guard<std::mutex> l(core::g_qMutex);
      if (index >= core::g_queue.size()) return false;
      std::rotate(core::g_queue.begin(), core::g_queue.begin() + index, core::g_queue.begin() + index + 1);
    }
    return PumpGame(); // exact production dispatch (including fault guard), queue lock released
}
void PumpPhysics(bool reverse) {
    { std::lock_guard<std::mutex> l(core::g_groundMutex);
      if (reverse) std::reverse(core::g_groundQueue.begin(), core::g_groundQueue.end());
    }
    core::ServiceGroundQueue(nullptr);
}
bool GroundLocksFree() {
    std::unique_lock<std::mutex> a(core::g_groundOpMutex, std::try_to_lock);
    std::unique_lock<std::mutex> b(core::g_regMutex, std::try_to_lock);
    std::unique_lock<std::mutex> c(core::g_groundMutex, std::try_to_lock);
    std::unique_lock<std::mutex> d(core::g_qMutex, std::try_to_lock);
    std::unique_lock<std::mutex> e(core::g_serverJobsMutex, std::try_to_lock);
    std::unique_lock<std::mutex> f(core::g_gimmickQueueMutex, std::try_to_lock);
    return a.owns_lock() && b.owns_lock() && c.owns_lock() && d.owns_lock() && e.owns_lock() && f.owns_lock();
}
size_t GroundLeaseCount() { std::lock_guard<std::mutex> l(core::g_groundOpMutex); return core::g_groundLeases.size(); }
size_t GroundDeferredCount() { std::lock_guard<std::mutex> l(core::g_groundOpMutex); return core::g_groundDeferred.size(); }
size_t GroundTicketCount() { std::lock_guard<std::mutex> l(core::g_groundMutex); return core::g_groundQueue.size() + core::g_groundResults.size(); }
uint64_t GroundEpoch() { std::lock_guard<std::mutex> l(core::g_groundOpMutex); return core::g_groundEpoch; }
unsigned GroundWorldWriters() { std::lock_guard<std::mutex> l(core::g_groundOpMutex); return core::g_groundWorldWriting; }
void SetGroundRadius(float radius) {
    std::lock_guard<std::mutex> l(core::g_tplMutex);
    core::g_probeRadius = radius; core::g_probeCalibrated = true;
}
bool RawGroundDirection(int ticket, Vec3* direction) {
    std::lock_guard<std::mutex> l(core::g_groundMutex);
    for (const auto& request : core::g_groundQueue) if (request.id == ticket) { *direction = request.dir; return true; }
    return false;
}
void SetSpawnQuietAge(DWORD worldAge, DWORD gameSpawnAge) {
    const DWORD now = GetTickCount();
    core::g_worldSince = now - worldAge; core::g_gameSpawnTick = now - gameSpawnAge;
    if (!core::g_worldSince) --core::g_worldSince;
}

UiServices& Ui() { static UiServices s; return s; }
void SetCameraAvailable(bool available) {
    // Resolved native entry / camera-global memory, not a substitute camera-mode policy.
    static uintptr_t camera = 0;
    core::g_natCamGlobal = available ? (uintptr_t)&camera : 0;
    core::g_origSetCamPose = available ? +[](void*, const float*, const float*, const float*, const float*, const float*, void*) -> void* { return nullptr; } : nullptr;
}
void CaptureCamera(Vec3 pos, float yaw, float pitch) {
    core::g_fcPos[0] = pos.x; core::g_fcPos[1] = pos.y; core::g_fcPos[2] = pos.z;
    core::g_fcYaw = yaw; core::g_fcPitch = pitch; core::g_fcInit = true;
}
ConsoleServices& Services() { static ConsoleServices s; return s; }
bool ConsoleDispatch(const std::string& cmd) { return core::ConsoleDispatch(cmd); }   // the production dispatcher, not a copy

}   // namespace host
