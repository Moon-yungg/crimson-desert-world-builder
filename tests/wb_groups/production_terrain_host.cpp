// Real terrain registry, persistence and two-trip Apply body. Only OS/native dependencies are substituted.
// Preinclude headers before macros. Joining at the thread-lifetime boundary makes worker teardown observable;
// Apply is invoked by a fixture worker, so the driver can pump the actual game queue at each exact wait event.
#include "production_host.h"
#include "../../asi/cdmodkit/core_internal.h"
#include "../../asi/cdmodkit/guard.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
namespace host {
namespace {
std::mutex terrainMutex;
std::condition_variable terrainChanged;
unsigned enteredStep = 0, resumedStep = 0;
DWORD terrainTick = 10000;
Vec3 terrainPlayer{};
}
void TerrainWait(DWORD) {
    std::unique_lock<std::mutex> lock(terrainMutex);
    const unsigned step = ++enteredStep;
    terrainChanged.notify_all();
    if (!terrainChanged.wait_for(lock, std::chrono::seconds(10), [step] { return resumedStep >= step; }))
        throw std::runtime_error("terrain native wait was not released by the fixture");
}
DWORD TerrainClock() { std::lock_guard<std::mutex> lock(terrainMutex); return terrainTick; }
bool TerrainPlayerPosInfo(PosInfo* out) {
    std::lock_guard<std::mutex> lock(terrainMutex);
    *out = {}; out->world = terrainPlayer; return true;
}
bool TerrainReadAvailable() { return true; }
bool TerrainReadFile(const std::string& path, std::vector<uint8_t>& out, bool* notFound = nullptr) {
    const bool table = path.find("heighttable/sector_") != std::string::npos;
    if (notFound) *notFound = !table;
    if (!table) return false; // real native texture/GPU service is unavailable, not an empty terrain registry
    const std::string xml = "<sector _heightRange=\"100\" _heightOffset=\"0\"/>";
    out.assign(xml.begin(), xml.end()); return true;
}
}
#define Sleep host::TerrainWait
#define GetTickCount host::TerrainClock
#define PlayerPosInfo host::TerrainPlayerPosInfo
#define GameReadAvailable host::TerrainReadAvailable
#define GameReadFile host::TerrainReadFile
#define detach join
#include "../../asi/cdmodkit/terrain.cpp"
#undef detach
#undef GameReadFile
#undef GameReadAvailable
#undef PlayerPosInfo
#undef GetTickCount
#undef Sleep
namespace host {
void SetTerrainAvailable(bool available) { core::g_ok = available; }
void ResetTerrainClock(Vec3 player) {
    std::lock_guard<std::mutex> lock(terrainMutex);
    enteredStep = resumedStep = 0; terrainTick = 10000; terrainPlayer = player;
}
bool WaitTerrainStep(unsigned step) {
    std::unique_lock<std::mutex> lock(terrainMutex);
    return terrainChanged.wait_for(lock, std::chrono::seconds(10), [step] { return enteredStep >= step; });
}
void ResumeTerrainStep(unsigned step, DWORD elapsedMs) {
    { std::lock_guard<std::mutex> lock(terrainMutex); terrainTick += elapsedMs; resumedStep = step; }
    terrainChanged.notify_all();
}
void SetTerrainPlayer(Vec3 player) { std::lock_guard<std::mutex> lock(terrainMutex); terrainPlayer = player; }
}
