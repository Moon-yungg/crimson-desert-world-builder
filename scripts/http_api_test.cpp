// Standalone HTTP contract test with a fake core; no game process is needed.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../asi/cdmodkit/core.h"
#include "../asi/cdmodkit/http_api.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace core {
static std::vector<SpawnedObj> objects;
static std::vector<PrefabInfo> prefabs{ { "/object/test_lamp.prefab", "test lamp", "light", "mesh" } };
void Log(const char*, ...) {}
bool HooksReady() { return true; }
bool GameThreadReady() { return true; }
bool BuildOk() { return true; }
const char* BuildMessage() { return "ok"; }
const char* GameVersion() { return "test"; }
int PendingSpawns() { return 0; }
bool PlayerWorldPos(Vec3* p) { *p = { 1, 2, 3 }; return true; }
const std::vector<PrefabInfo>& PrefabIndex() { return prefabs; }
std::vector<SpawnedObj> Spawned() { return objects; }
std::string ProjectNameOf(int) { return ""; }
int IndexOfUid(int uid) { for (size_t i = 0; i < objects.size(); ++i) if (objects[i].uid == uid) return (int)i; return -1; }
int SpawnAt(const std::string& prefab, Vec3 pos, Rot rot, float scale, int group, int proj) {
    int uid = (int)objects.size() + 1;
    objects.push_back({ 1, prefab, pos, rot, scale, false, 0, rot, scale, uid, group, proj });
    return uid;
}
bool MoveMany(const std::vector<MoveReq>& reqs, bool) {
    for (auto& r : reqs) { int i = IndexOfUid(r.uid); if (i < 0) return false; objects[i].pos = r.pos; objects[i].rot = r.rot; objects[i].scale = r.scale; }
    return true;
}
bool HideUid(int uid) { int i = IndexOfUid(uid); if (i < 0) return false; objects[i].hidden = true; return true; }
void ForgetUid(int uid) { int i = IndexOfUid(uid); if (i >= 0) objects.erase(objects.begin() + i); }
void SetGroup(int uid, int group) { int i = IndexOfUid(uid); if (i >= 0) objects[i].group = group; }
int NewGroupId() { return 1; }
int ProjectId(const std::string&) { return 1; }
void AssignProject(int uid, int proj) { int i = IndexOfUid(uid); if (i >= 0) objects[i].proj = proj; }
void DeleteAllSpawned() { objects.clear(); }
std::vector<std::string> ListProjects() { return {}; }
std::vector<std::string> Autoload() { return {}; }
FileResult SetAutoload(const std::string&, bool) { return {}; }
bool SaveProject(const std::string&, int) { return true; }
bool LoadProject(const std::string&, bool) { return true; }
// The upstream fixture predates the v0.97 core surface; these stubs only satisfy http_api.cpp's links for the
// routes the upstream test does not exercise (camera/freecam, time, travel, NPC, research, terrain). Each one
// keeps the production route's own unavailable/empty semantics; no upstream case is changed, skipped or added.
bool CameraPose(Vec3* fwd, Vec3* pos) { *fwd = { 0, 0, 1 }; *pos = { 0, 0, 0 }; return true; }
bool FreeCamAvailable() { return true; }
bool FreeCamActive() { return false; }
void SetFreeCam(bool) {}
bool FreeCamPose(Vec3*, Vec3*) { return false; }
void FreeCamFocus(Vec3, float) {}
void FreeCamViewPreset(int) {}
void FreeCamDolly(float) {}
void FreeCamTurn(float, float) {}
bool FreeCamSetPosition(Vec3) { return false; }
bool FreeCamMove(float, float, float) { return false; }
bool TimeControlAvailable() { return false; }
bool TimeHour(float*) { return false; }
bool TimeFrozen() { return false; }
void SetTimeHour(float) {}
void SetTimeFrozen(bool) {}
void ResetTimeControl() {}
bool ReadMem(uintptr_t, void*, size_t) { return false; }
bool WriteMem(uintptr_t, const void*, size_t) { return false; }
void ResearchPeek(uintptr_t, int, bool) {}
void ResearchFind(const std::vector<uint8_t>&, int) {}
void SetFallWatch(bool, uintptr_t, bool, uintptr_t, unsigned) {}
void ResearchFindPos() {}
void GroundTrace(int) {}
int LoadDebugPoints(bool) { return 0; }
void GeoTraceInstall(uintptr_t, int) {}
void GeoTraceArm() {}
void FnTraceInstall(uintptr_t) {}
void IoTraceSet(const std::string&) {}
void IoHeightDelta(int) {}
void IoStreamDelta(int) {}
void GpuTrace(int) {}
std::string TerrainStatus() { return "unavailable in the HTTP contract test"; }
std::vector<TerrainStroke> TerrainStrokes() { return {}; }
bool PlayerPosInfo(PosInfo*) { return false; }
bool TerrainApply(Vec3) { return false; }
int TerrainEditDisc(float, float, float, float) { return 0; }
void TerrainEditClear() {}
void TerrainSyncTrace() {}
void TerrainJobTrace(uintptr_t) {}
void TerrainLoadTrace(int) {}
void TerrainTexTrace(uintptr_t) {}
void TeleTraceInstall(uintptr_t) {}
void ReloadStageTrace(uintptr_t) {}
void RsSendTrace(uintptr_t) {}
void ClientReloadTrace(uintptr_t) {}
void ClientReloadReplay(float, float, float) {}
void ReloadStageReplay(float, float, float) {}
bool ResearchWatchWrites(const uintptr_t*, int) { return false; }
void TerrainTileTaskTrace(uintptr_t) {}
void TerrainRetTrace(uintptr_t) {}
void TerrainTexReload(int, int, uintptr_t, uintptr_t) {}
void TerrainReloadCall(uintptr_t, int, const std::string&) {}
void ResearchVtScan(const std::string&, int, int) {}
void CamWatch(int, int) {}
bool GroundGrid(float, float, int, int, float, float, float, std::vector<float>*) { return false; }
bool TravelAvailable() { return false; }
std::string TravelStatus() { return "unavailable in the HTTP contract test"; }
bool TravelTo(Vec3, float) { return false; }
bool SetPlayerPos(Vec3) { return false; }
int NpcState() { return 0; }
bool SpawnNpc(uint32_t, Vec3, int, uint32_t) { return false; }
}

static std::string Request(const std::string& method, const std::string& path, const std::string& body = "", const std::string& host = "127.0.0.1", const std::string& extra = "") {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); assert(s != INVALID_SOCKET);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(18765);
    assert(connect(s, (sockaddr*)&addr, sizeof addr) == 0);
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" + extra + "Content-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    assert(send(s, request.data(), (int)request.size(), 0) == (int)request.size());
    std::string reply; char buf[4096]; int n = 0;
    while ((n = recv(s, buf, sizeof buf, 0)) > 0) reply.append(buf, n);
    closesocket(s); return reply;
}
static int Count(const std::string& text, const std::string& token) {
    int count = 0; size_t p = 0;
    while ((p = text.find(token, p)) != std::string::npos) { ++count; p += token.size(); }
    return count;
}
int main() {
    WSADATA data{}; assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    assert(httpapi::Start(18765) && httpapi::ActivePort() == 18765);
    httpapi::Stop(); assert(httpapi::ActivePort() == 0);
    { SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(18765);
      assert(connect(s, (sockaddr*)&a, sizeof a) != 0); closesocket(s); }   // stopped = nothing listens any more
    assert(httpapi::Start(18765));                                          // and the port can be taken again right away
    assert(Request("GET", "/api/status").find("\"ready\":true") != std::string::npos);
    assert(Request("GET", "/api/prefabs?q=lamp").find("test_lamp.prefab") != std::string::npos);
    assert(Request("GET", "/api/prefabs?q=missing").find("\"total\":0") != std::string::npos);
    assert(Request("GET", "/api/status", "", "evil.example").find("400 Bad Request") != std::string::npos);
    assert(Request("POST", "/api/groups", "{}", "127.0.0.1", "Origin: https://evil.example\r\n").find("400 Bad Request") != std::string::npos);
    assert(Request("POST", "/api/projects/save", "{\"name\":\"../escape\"}").find("400 Bad Request") != std::string::npos);
    auto bad = Request("POST", "/api/objects", "{bad}"); assert(bad.find("400 Bad Request") != std::string::npos);
    auto spawn = Request("POST", "/api/objects", "{\"prefab\":\"/object/test_lamp.prefab\",\"x\":10,\"y\":20,\"z\":30}");
    assert(spawn.find("202 Accepted") != std::string::npos && spawn.find("\"uid\":1") != std::string::npos);
    assert(Request("PATCH", "/api/objects/1", "{\"y\":25}").find("202 Accepted") != std::string::npos);
    assert(Request("GET", "/api/objects/1").find("\"y\":25") != std::string::npos);
    assert(Request("POST", "/api/objects/1/hide", "{}").find("202 Accepted") != std::string::npos);
    assert(Request("GET", "/api/objects/1").find("\"hidden\":true") != std::string::npos);
    assert(Request("DELETE", "/api/objects/1").find("202 Accepted") != std::string::npos);
    assert(Request("GET", "/api/objects/1").find("404 Not Found") != std::string::npos);
    for (int i = 0; i < 1205; ++i) {
        core::prefabs.push_back({ "/object/item_" + std::to_string(i) + ".prefab", "item", "test", "mesh" });
        core::SpawnAt("/object/item.prefab", { (float)i, 0, 0 });
    }
    auto firstPrefabs = Request("GET", "/api/prefabs?offset=0&limit=500");
    auto lastPrefabs = Request("GET", "/api/prefabs?offset=1000&limit=500");
    assert(firstPrefabs.find("\"total\":1206") != std::string::npos && firstPrefabs.find("\"nextOffset\":500") != std::string::npos);
    assert(Count(firstPrefabs, "\"index\":") == 500 && Count(lastPrefabs, "\"index\":") == 206);
    assert(lastPrefabs.find("\"nextOffset\":null") != std::string::npos);
    auto firstObjects = Request("GET", "/api/objects?offset=0&limit=500");
    auto lastObjects = Request("GET", "/api/objects?offset=1000&limit=500");
    assert(firstObjects.find("\"total\":1205") != std::string::npos && firstObjects.find("\"nextOffset\":500") != std::string::npos);
    assert(Count(firstObjects, "\"uid\":") == 500 && Count(lastObjects, "\"uid\":") == 205);
    assert(lastObjects.find("\"nextOffset\":null") != std::string::npos);
    puts("HTTP API contract OK");
    return 0;
}
