// Reverse-engineering aids, split out of cdmodkit.cpp: reachable only from the console or the Settings tab, never
// running during normal use. Nothing here is needed to spawn, move or save. The functional physics-probe code
// (CaptureTemplate / RunGroundCast) deliberately stays in cdmodkit.cpp next to the cast hooks it belongs to.
//   viewscan  - find the renderer's view matrix in memory
//   fovtrace  - which camera field changes while zooming
//   camtrace  - which camera field follows the view direction
//   traceio   - log the game's call chain when it reads from a .paz pack (hooks kernel32!ReadFile, opt-in only)
#include "core_internal.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include "MinHook.h"

namespace core {

static bool InImage(uintptr_t p) { return p >= g_base && p < g_base + 0x17000000; }

static DWORD WINAPI FovTraceThread(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg;
    std::vector<std::pair<std::string, uintptr_t>> cams; ExpandCameraManager(cams);
    if (cams.empty()) { Log("fovtrace: no camera objects"); return 0; }
    if (cams.size() > 24) cams.resize(24);
    const unsigned N = 0x800;
    Log("fovtrace: sampling %zu objects for %d s. Zoom the camera in and out (mouse wheel), do not move.", cams.size(), seconds);
    std::vector<std::vector<std::vector<float>>> all(cams.size());
    for (int t = 0; t < seconds * 4; t++) { for (size_t ci = 0; ci < cams.size(); ci++) { std::vector<float> f(N / 4); if (ReadBytes(cams[ci].second, f.data(), N)) all[ci].push_back(f); } Sleep(250); }
    for (size_t ci = 0; ci < cams.size(); ci++) {
        auto& samples = all[ci]; if (samples.size() < 4) continue;
        for (unsigned i = 0; i < N / 4; i++) {
            bool ok = true; float mn = 1e9f, mx = -1e9f;
            for (auto& s : samples) { float v = s[i]; if (!std::isfinite(v) || v < 0.2f || v > 180.0f) { ok = false; break; } mn = (std::min)(mn, v); mx = (std::max)(mx, v); }
            if (!ok || mx - mn < 0.05f * mx) continue;
            const bool degrees = mn >= 15.0f && mx <= 150.0f, radians = mn >= 0.25f && mx <= 2.7f;
            if (degrees || radians) Log("fovtrace: %s +0x%X: %.3f .. %.3f (%s) first %.3f last %.3f", cams[ci].first.c_str(), i * 4, mn, mx, degrees ? "deg?" : "rad?", samples.front()[i], samples.back()[i]);
        }
    }
    Log("fovtrace: done");
    return 0;
}
// ---- diagnosis: where does the renderer keep its view matrix? Scans the process memory for the camera basis vectors (as read from
// the camera scene object) laid out as a 3x3 / 3x4 / 4x4 matrix, in rows or columns, any sign. Every hit is logged with the 16
// floats and the 16 after it; a perspective block nearby is decoded to fov / aspect. Hold the camera still while it runs.
static bool NearV(float a, float b) { return fabsf(a - b) < 0.0025f; }
static int MatchVec(const float* f, int stride, const Vec3& v) {   // +1 = same sign, -1 = negated, 0 = no
    if (NearV(f[0], v.x) && NearV(f[stride], v.y) && NearV(f[2 * stride], v.z)) return 1;
    if (NearV(f[0], -v.x) && NearV(f[stride], -v.y) && NearV(f[2 * stride], -v.z)) return -1;
    return 0;
}
static void LogFloats(const char* tag, const float* f, int n) {
    char line[640]; int k = snprintf(line, sizeof line, "%s", tag);
    for (int i = 0; i < n && k < (int)sizeof line - 20; i++) k += snprintf(line + k, sizeof line - k, " %.4g", f[i]);
    Log("%s", line);
}
static DWORD WINAPI ViewScanThread(LPVOID) {
    Vec3 pos, right, up, fwd; if (!CameraBasis(&pos, &right, &up, &fwd)) { Log("[viewscan] no camera basis"); return 0; }
    const Vec3 local = { pos.x - (int)(pos.x * 0.001) * 1000.0f, pos.y, pos.z - (int)(pos.z * 0.001) * 1000.0f };
    Log("[viewscan] camera pos world (%.3f %.3f %.3f) tile-local (%.3f %.3f %.3f) right (%.4f %.4f %.4f) up (%.4f %.4f %.4f) fwd (%.4f %.4f %.4f); scanning, hold still",
        pos.x, pos.y, pos.z, local.x, local.y, local.z, right.x, right.y, right.z, up.x, up.y, up.z, fwd.x, fwd.y, fwd.z);
    const DWORD t0 = GetTickCount(); int hits = 0; size_t scanned = 0; std::vector<uintptr_t> blocks;
    std::vector<uint8_t> buf(1 << 20);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x10000;
    while (addr < 0x7FFFFFFF0000ULL && hits < 60 && GetTickCount() - t0 < 60000) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize; addr = base + size;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 || (mbi.Protect & PAGE_GUARD)) continue;
        if (size > (1ull << 31)) continue;
        for (uintptr_t off = 0; off < size; off += buf.size() - 256) {
            const size_t n = std::min<size_t>(buf.size(), size - off); if (n < 64) break;
            if (!ReadBytes(base + off, buf.data(), n)) break; scanned += n;
            const float* f = (const float*)buf.data(); const size_t nf = n / 4;
            for (size_t i = 0; i + 16 <= nf && hits < 60; i++) {
                if (!(NearV(f[i], right.x) || NearV(f[i], -right.x))) continue;   // cheap first test on right.x (row and column layouts both start with it)
                int layout = 0; const char* how = "";
                if (MatchVec(f + i, 1, right) && MatchVec(f + i + 4, 1, up) && MatchVec(f + i + 8, 1, fwd)) { layout = 1; how = "rows stride 4"; }
                else if (MatchVec(f + i, 1, right) && MatchVec(f + i + 3, 1, up) && MatchVec(f + i + 6, 1, fwd)) { layout = 2; how = "rows stride 3"; }
                else if (MatchVec(f + i, 4, right) && MatchVec(f + i + 1, 4, up) && MatchVec(f + i + 2, 4, fwd)) { layout = 3; how = "columns stride 4"; }
                if (!layout) continue;
                hits++;
                const uintptr_t at = base + off + i * 4;
                // translation candidates: rows stride 4 -> f[i+12..14]; columns -> f[i+3], f[i+7], f[i+11]; stride 3 -> f[i+9..11]
                float t[3] = { 0, 0, 0 };
                if (layout == 1 || layout == 3) { t[0] = f[i + 12]; t[1] = f[i + 13]; t[2] = f[i + 14]; } else { t[0] = f[i + 9]; t[1] = f[i + 10]; t[2] = f[i + 11]; }   // 4x4 in either convention keeps the translation in the 4th row
                const Vec3 mw = { -(right.x * pos.x + right.y * pos.y + right.z * pos.z), -(up.x * pos.x + up.y * pos.y + up.z * pos.z), -(fwd.x * pos.x + fwd.y * pos.y + fwd.z * pos.z) };
                const Vec3 ml = { -(right.x * local.x + right.y * local.y + right.z * local.z), -(up.x * local.x + up.y * local.y + up.z * local.z), -(fwd.x * local.x + fwd.y * local.y + fwd.z * local.z) };
                auto near3 = [&](const Vec3& v) { return fabsf(t[0] - v.x) < 0.05f && fabsf(t[1] - v.y) < 0.05f && fabsf(t[2] - v.z) < 0.05f; };
                auto near3n = [&](const Vec3& v) { return fabsf(t[0] + v.x) < 0.05f && fabsf(t[1] + v.y) < 0.05f && fabsf(t[2] + v.z) < 0.05f; };
                const char* what = near3(pos) ? "translation = world pos" : near3(local) ? "translation = tile-local pos" : near3(mw) || near3n(mw) ? "translation = -R*world pos (VIEW MATRIX)" : near3(ml) || near3n(ml) ? "translation = -R*local pos (VIEW MATRIX, tile-local)" : "translation unknown";
                Log("[viewscan] hit %d at %p (%s): %s; t = (%.3f %.3f %.3f)", hits, (void*)at, how, what, t[0], t[1], t[2]);
                LogFloats("[viewscan]   m:", f + i, 16);
                if (i + 32 <= nf) LogFloats("[viewscan]   +64:", f + i + 16, 16);
                if (i >= 16) LogFloats("[viewscan]   -64:", f + i - 16, 16);
                // perspective block within +-256 bytes: m00, m11 nonzero, off-diagonals zero, a +-1 in [2][3] or [3][2]
                bool persp = false;
                for (long d = -64; d <= 64; d++) {
                    const long j = (long)i + d; if (j < 0 || j + 16 > (long)nf) continue; const float* m = f + j;
                    if (m[0] == 0 || m[5] == 0 || m[1] != 0 || m[4] != 0 || m[2] != 0 || m[8] != 0 || m[9] != 0 || m[12] != 0 || m[13] != 0) continue;
                    if (!(fabsf(fabsf(m[11]) - 1.0f) < 1e-4f || fabsf(fabsf(m[14]) - 1.0f) < 1e-4f)) continue;
                    if (!std::isfinite(m[0]) || !std::isfinite(m[5]) || fabsf(m[0]) > 100 || fabsf(m[5]) > 100) continue;
                    Log("[viewscan]   perspective at %+ld floats: m00 %.5f m11 %.5f m22 %.5f m23 %.5f m32 %.5f m33 %.5f -> vertical fov %.2f deg, aspect %.3f",
                        d, m[0], m[5], m[10], m[11], m[14], m[15], 2.0f * atanf(1.0f / fabsf(m[5])) * 57.2958f, m[5] / m[0]);
                    persp = true; break;
                }
                if ((layout == 1 || layout == 3) && fabsf(t[0]) + fabsf(t[1]) + fabsf(t[2]) > 1.0f && blocks.size() < 8) blocks.push_back(at | (layout == 3 ? 1ull : 0ull));   // 4x4 with a translation: world matrix (rows) or view matrix (columns, bit 0 set)
            }
        }
    }
    Log("[viewscan] done: %d hits, %.0f MB scanned in %lu ms", hits, scanned / 1048576.0, GetTickCount() - t0);
    if (blocks.empty()) return 0;
    // timing: who is ahead, the camera scene object or the renderer's matrices? Sample both for 5 s while the user pans.
    Log("[viewscan] tracking %zu matrices for 5 s: pan the camera slowly and steadily now", blocks.size());
    auto yawOf = [](const Vec3& f) { return atan2f(f.x, f.z) * 57.2958f; };
    for (int k = 0; k < 1000; k++) {
        Vec3 np, nr, nu, nf; const bool okn = CameraBasis(&np, &nr, &nu, &nf);
        if (k % 8 == 0) {
            char line[700]; int n = snprintf(line, sizeof line, "[viewtrack] %4d ms node yaw %8.3f pitch %7.3f pos (%.2f %.2f %.2f)", k * 5, okn ? yawOf(nf) : 0.0f, okn ? asinf(-nf.y) * 57.2958f : 0.0f, np.x, np.y, np.z);
            for (uintptr_t bb : blocks) {
                const bool view = (bb & 1) != 0; const uintptr_t bl = bb & ~1ull;
                float m[16]; if (!ReadBytes(bl, m, sizeof m)) continue;
                Vec3 f, eye;
                if (view) { const Vec3 r = { m[0], m[4], m[8] }, u = { m[1], m[5], m[9] }; f = { m[2], m[6], m[10] };
                            eye = { -(m[12] * r.x + m[13] * u.x + m[14] * f.x), -(m[12] * r.y + m[13] * u.y + m[14] * f.y), -(m[12] * r.z + m[13] * u.z + m[14] * f.z) }; }
                else { f = { m[8], m[9], m[10] }; eye = { m[12], m[13], m[14] }; }
                n += snprintf(line + n, sizeof line - n, " | %s %llx yaw %8.3f pitch %7.3f eye (%.2f %.2f %.2f)", view ? "view" : "world", (unsigned long long)(bl & 0xFFFFFFF), yawOf(f), asinf(-f.y) * 57.2958f, eye.x, eye.y, eye.z);
                if (n > (int)sizeof line - 90) break;
            }
            Log("%s", line);
        }
        Sleep(5);
    }
    Log("[viewscan] tracking done");
    return 0;
}
// ---- render camera ---------------------------------------------------------------------------------------------
// The renderer keeps a per-frame block [view matrix (columns = basis, 4th row = -R*eye)] [...] [projection] in a constant
// buffer copy on the heap. The camera scene object only gives the pose (and no field of view, which the game changes with
// the situation: 50 degrees outdoors, 40 in town were measured). So the block is located once by scanning for the camera
// basis of the moment (like ViewScan, silently) and then read every frame: eye, basis and the exact focal scales.
// Several copies exist (frames in flight); RenderCamera ranks them by similarity to the scene object's pose and returns
// the one at rank g_camLag (0 = the newest). When every copy stops validating the block was freed: a rescan is queued.
struct RcBlock { uintptr_t view, proj; };
static std::mutex g_rcMutex; static std::vector<RcBlock> g_rcBlocks; static DWORD g_rcScanAt = 0; static volatile LONG g_rcScanning = 0; static int g_rcState = 0;   // 0 never, 1 found, 2 lost
static bool PerspOk(const float* p) {
    return p[0] != 0 && p[5] != 0 && p[1] == 0 && p[4] == 0 && p[2] == 0 && p[8] == 0 && p[9] == 0 && p[12] == 0 && p[13] == 0 && fabsf(fabsf(p[11]) - 1.0f) < 1e-4f
        && std::isfinite(p[0]) && std::isfinite(p[5]) && fabsf(p[5]) > 0.3f && fabsf(p[5]) < 20.0f && fabsf(p[0]) > 0.1f && fabsf(p[0]) < 20.0f;
}
static DWORD WINAPI RenderCamScanThread(LPVOID) {
    Vec3 pos, right, up, fwd; std::vector<RcBlock> found;
    if (CameraBasis(&pos, &right, &up, &fwd)) {
        const DWORD t0 = GetTickCount(); std::vector<uint8_t> buf(1 << 20);
        MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0x10000;
        while (addr < 0x7FFFFFFF0000ULL && found.size() < 8 && GetTickCount() - t0 < 30000) {
            if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) break;
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize; addr = base + size;
            if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 || (mbi.Protect & PAGE_GUARD)) continue;
            if (size > (1ull << 31)) continue;
            for (uintptr_t off = 0; off < size; off += buf.size() - 512) {
                const size_t n = std::min<size_t>(buf.size(), size - off); if (n < 320) break;
                if (!ReadBytes(base + off, buf.data(), n)) break;
                const float* f = (const float*)buf.data(); const size_t nf = n / 4;
                for (size_t i = 0; i + 80 <= nf && found.size() < 8; i++) {
                    if (!NearV(f[i], right.x)) continue;
                    if (!(MatchVec(f + i, 4, right) == 1 && MatchVec(f + i + 1, 4, up) == 1 && MatchVec(f + i + 2, 4, fwd) == 1)) continue;
                    if (fabsf(f[i + 12]) + fabsf(f[i + 13]) + fabsf(f[i + 14]) < 1.0f) continue;   // the rotation-only copy
                    for (size_t j = i + 16; j + 16 <= nf && j <= i + 64; j += 16) if (PerspOk(f + j)) { found.push_back({ base + off + i * 4, base + off + j * 4 }); break; }
                    i += 15;
                }
            }
        }
    }
    { std::lock_guard<std::mutex> l(g_rcMutex); g_rcBlocks = found; }
    if (!found.empty()) { float p[16] = { 0 }; ReadBytes(found[0].proj, p, sizeof p); Log("[rendercam] %zu view/projection blocks found (first at %p, fov %.2f deg, aspect %.3f)", found.size(), (void*)found[0].view, 2.0f * atanf(1.0f / fabsf(p[5])) * 57.2958f, p[5] / p[0]); g_rcState = 1; }
    else if (g_rcState != 2) { Log("[rendercam] no block found this time (camera moving?), retrying later"); }
    g_rcScanAt = GetTickCount(); InterlockedExchange(&g_rcScanning, 0);
    return 0;
}
void FindRenderCamera() { if (InterlockedCompareExchange(&g_rcScanning, 1, 0) != 0) return; CreateThread(nullptr, 0, RenderCamScanThread, nullptr, 0, nullptr); }
int RenderCameraBlocks() { std::lock_guard<std::mutex> l(g_rcMutex); return (int)g_rcBlocks.size(); }
static bool WriteRenderView(uintptr_t at, const float* m) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & writable)) return false;
    SIZE_T wrote = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)at, m, sizeof(float) * 16, &wrote) && wrote == sizeof(float) * 16;
}
bool CameraControlRenderOverride() {
    static volatile LONG s_logged = 0;
    Vec3 pos{}, r{}, u{}, f{};
    if (!CameraControlBasis(&pos, &r, &u, &f)) { InterlockedExchange(&s_logged, 0); return false; }
    std::vector<RcBlock> blocks; { std::lock_guard<std::mutex> l(g_rcMutex); blocks = g_rcBlocks; }
    if (blocks.empty()) {
        if (GetTickCount() - g_rcScanAt > 1000 && !g_rcScanning) FindRenderCamera();
        return false;
    }
    auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    float m[16] = {
        r.x, u.x, f.x, 0.0f,
        r.y, u.y, f.y, 0.0f,
        r.z, u.z, f.z, 0.0f,
        -dot(pos, r), -dot(pos, u), -dot(pos, f), 1.0f
    };
    int wrote = 0;
    for (const RcBlock& b : blocks) {
        float old[16], p[16];
        if (!ReadBytes(b.view, old, sizeof old) || !ReadBytes(b.proj, p, sizeof p) || !PerspOk(p)) continue;
        if (WriteRenderView(b.view, m)) wrote++;
    }
    if (wrote && InterlockedCompareExchange(&s_logged, 1, 0) == 0)
        Log("[rendercam] camera control overriding %d tracked view block(s)", wrote);
    return wrote != 0;
}
bool RenderCamera(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd, float* m00, float* m11, int rank) {
    std::vector<RcBlock> blocks; { std::lock_guard<std::mutex> l(g_rcMutex); blocks = g_rcBlocks; }
    Vec3 np, nr, nu, nf;
    const bool haveNode = CameraControlActive() ? CameraControlBasis(&np, &nr, &nu, &nf) : CameraBasis(&np, &nr, &nu, &nf);
    struct Cand { Vec3 pos, r, u, f; float m00, m11, sim; }; std::vector<Cand> c;
    for (const RcBlock& b : blocks) {
        float m[16], p[16]; if (!ReadBytes(b.view, m, sizeof m) || !ReadBytes(b.proj, p, sizeof p)) continue;
        const Vec3 r = { m[0], m[4], m[8] }, u = { m[1], m[5], m[9] }, f = { m[2], m[6], m[10] };
        auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        if (fabsf(dot(r, r) - 1) > 0.02f || fabsf(dot(u, u) - 1) > 0.02f || fabsf(dot(f, f) - 1) > 0.02f || fabsf(dot(r, u)) > 0.02f || fabsf(dot(r, f)) > 0.02f || fabsf(dot(u, f)) > 0.02f) continue;
        if (!PerspOk(p)) continue;
        Cand k; k.r = r; k.u = u; k.f = f; k.m00 = fabsf(p[0]); k.m11 = fabsf(p[5]);
        k.pos = { -(m[12] * r.x + m[13] * u.x + m[14] * f.x), -(m[12] * r.y + m[13] * u.y + m[14] * f.y), -(m[12] * r.z + m[13] * u.z + m[14] * f.z) };
        if (!std::isfinite(k.pos.x) || fabsf(k.pos.x) > 1e6f) continue;
        // liveness: the renderer's eye and forward must match the camera scene object (verified to agree within centimetres);
        // a copy that drifted away is a stale buffer, not the camera of this frame
        if (haveNode) {
            const float dpos = fabsf(k.pos.x - np.x) + fabsf(k.pos.y - np.y) + fabsf(k.pos.z - np.z);
            if (dot(f, nf) < 0.9994f || dot(r, nr) < 0.9994f || dpos > 0.6f) continue;   // ~2 degrees, 0.6 m
        }
        k.sim = haveNode ? dot(f, nf) + dot(r, nr) - 0.001f * (fabsf(k.pos.x - np.x) + fabsf(k.pos.y - np.y) + fabsf(k.pos.z - np.z)) : 0.0f;
        c.push_back(k);
    }
    static int s_disagree = 0;
    if (c.empty()) {
        if (!blocks.empty() && ++s_disagree > 180 && g_rcState == 1) { Log("[rendercam] blocks stale for 3 s, rescanning"); g_rcState = 2; { std::lock_guard<std::mutex> l(g_rcMutex); g_rcBlocks.clear(); } s_disagree = 0; }
        if (blocks.empty() && GetTickCount() - g_rcScanAt > 10000 && !g_rcScanning) FindRenderCamera();
        return false;
    }
    s_disagree = 0;
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.sim > b.sim; });
    const int ri = rank < 0 ? 0 : rank >= (int)c.size() ? (int)c.size() - 1 : rank; const Cand& k = c[ri];
    if (pos) *pos = k.pos; if (right) *right = k.r; if (up) *up = k.u; if (fwd) *fwd = k.f; if (m00) *m00 = k.m00; if (m11) *m11 = k.m11;
    return true;
}
void ViewScan() { CreateThread(nullptr, 0, ViewScanThread, nullptr, 0, nullptr); }
void FovTrace(int seconds) { CreateThread(nullptr, 0, FovTraceThread, (LPVOID)(intptr_t)seconds, 0, nullptr); }
static DWORD WINAPI CamTraceThread(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg;
    std::vector<std::pair<std::string, uintptr_t>> cams; ExpandCameraManager(cams);
    if (cams.empty()) { Log("camtrace: no camera objects found near the world root / player"); return 0; }
    for (auto& c : cams) Log("camtrace: candidate %s at %p", c.first.c_str(), (void*)c.second);
    if (cams.size() > 24) cams.resize(24);
    const unsigned N = 0x800;
    Log("camtrace: sampling %zu objects every 250 ms for %d s. Rotate the camera slowly (do not move).", cams.size(), seconds);
    std::vector<std::vector<std::vector<float>>> all(cams.size());
    for (int t = 0; t < seconds * 4; t++) { for (size_t ci = 0; ci < cams.size(); ci++) { std::vector<float> f(N / 4); if (ReadBytes(cams[ci].second, f.data(), N)) all[ci].push_back(f); } Sleep(250); }
    for (size_t ci = 0; ci < cams.size(); ci++) {
    auto& samples = all[ci]; uintptr_t cam = cams[ci].second;
    if (samples.size() < 4) { Log("camtrace: %s: no samples", cams[ci].first.c_str()); continue; }
    Log("camtrace: results for %s (%p)", cams[ci].first.c_str(), (void*)cam);
    // unit-length triples that change over time = direction vectors; scalars in [-7,7] that change = angles
    for (unsigned i = 0; i + 2 < N / 4; i++) {
        bool unit = true, moved = false; float mn = 1e9f, mx = -1e9f;
        for (auto& s : samples) { float l = sqrtf(s[i] * s[i] + s[i + 1] * s[i + 1] + s[i + 2] * s[i + 2]); if (!std::isfinite(l) || fabsf(l - 1.0f) > 0.02f) { unit = false; break; } mn = (std::min)(mn, s[i]); mx = (std::max)(mx, s[i]); }
        if (unit && mx - mn > 0.1f) Log("camtrace: unit vector at +0x%X: first (%.3f %.3f %.3f) last (%.3f %.3f %.3f)", i * 4, samples.front()[i], samples.front()[i + 1], samples.front()[i + 2], samples.back()[i], samples.back()[i + 1], samples.back()[i + 2]);
        (void)moved;
    }
    for (unsigned i = 0; i < N / 4; i++) {
        bool ok = true; float mn = 1e9f, mx = -1e9f;
        for (auto& s : samples) { if (!std::isfinite(s[i]) || fabsf(s[i]) > 7.0f) { ok = false; break; } mn = (std::min)(mn, s[i]); mx = (std::max)(mx, s[i]); }
        if (ok && mx - mn > 0.5f && mx - mn < 7.0f) Log("camtrace: angle-like float at +0x%X: %.3f .. %.3f (first %.3f last %.3f)", i * 4, mn, mx, samples.front()[i], samples.back()[i]);
    }
    }
    Log("camtrace: done");
    return 0;
}
void CamTrace(int seconds) { CreateThread(nullptr, 0, CamTraceThread, (LPVOID)(intptr_t)seconds, 0, nullptr); }

// ---- pack I/O tracing (reverse engineering aid): logs the game's call chain when it reads from a .paz archive.
// Enabled by the file bin64\cdmodkit\traceio.flag or the console command "traceio on".
static bool g_traceIo = false; static volatile LONG g_ioLogged = 0;
void SetIoTrace(bool on) { g_traceIo = on; g_ioLogged = 0; Log("[io] trace %s", on ? "on" : "off"); }
static decltype(&ReadFile) g_origReadFile = nullptr;
static BOOL WINAPI HookReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
    const BOOL r = g_origReadFile(h, buf, n, read, ov);
    if (g_traceIo && g_ioLogged < 60) {
        char name[MAX_PATH] = { 0 };
        if (GetFinalPathNameByHandleA(h, name, MAX_PATH, 0)) {
            size_t l = strlen(name);
            if (l > 4 && _stricmp(name + l - 4, ".paz") == 0) {
                InterlockedIncrement(&g_ioLogged);
                void* frames[28]; USHORT k = RtlCaptureStackBackTrace(1, 28, frames, nullptr);
                std::string s; for (USHORT i = 0; i < k; i++) { uintptr_t a = (uintptr_t)frames[i]; if (InImage(a)) { char b[32]; snprintf(b, sizeof b, " %llx", (unsigned long long)(a - g_base)); s += b; } }
                const char* fn = strrchr(name, '\\'); const char* dir = fn ? fn - 1 : name; while (dir > name && *dir != '\\') dir--;
                Log("[io] ReadFile %lu B %s off=%lu thread=%lu stack:%s", n, dir + 1, ov ? ov->Offset : 0, GetCurrentThreadId(), s.c_str());
            }
        }
    }
    return r;
}
void InstallIoTrace() {
    g_traceIo = GetFileAttributesA((ModDir() + "\\traceio.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!g_traceIo) return;   // the ReadFile hook upsets NvMessageBus.dll; only installed for an explicit trace run
    HMODULE k32 = GetModuleHandleA("kernel32.dll"); if (!k32) return;
    void* t = (void*)GetProcAddress(k32, "ReadFile"); if (!t) return;
    if (MH_CreateHook(t, (void*)HookReadFile, (void**)&g_origReadFile) == MH_OK && MH_EnableHook(t) == MH_OK) Log("[io] ReadFile hooked (trace %s)", g_traceIo ? "on" : "off");
}
}   // namespace core
