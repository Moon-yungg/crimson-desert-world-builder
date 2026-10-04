// Reverse-engineering aids, split out of cdmodkit.cpp: reachable only from the console or the Settings tab, never
// running during normal use. Nothing here is needed to spawn, move or save. The functional physics-probe code
// (CaptureTemplate / RunGroundCast) deliberately stays in cdmodkit.cpp next to the cast hooks it belongs to.
//   fovtrace  - which camera field changes while zooming
//   camtrace  - which camera field follows the view direction
//   traceio   - log the game's call chain when it reads from a .paz pack (hooks ntdll!NtReadFile, opt-in only)
#include "core_internal.h"
#include "guard.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "MinHook.h"

namespace core {

static bool InImage(uintptr_t p) { return g_base && p >= g_base && p < ImageEnd(); }

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
// Enabled by the file bin64\cdmodkit\traceio.flag (hook installed at start) and the console command "traceio on".
// Hooks ntdll!NtReadFile (every file API ends there). Only disk files are looked at (named pipes stall on name queries,
// which upset NvMessageBus.dll with the old ReadFile hook). With bin64\cdmodkit\ioranges.txt ("0015\49.paz offset size label"
// per line, re-read by "traceio on") only reads overlapping those ranges are logged, otherwise the first 60 pack reads.
static bool g_traceIo = false; static volatile LONG g_ioLogged = 0;
struct IoRange { std::string paz; unsigned long long off, size; std::string label; };
static std::vector<IoRange> g_ioRanges; static SRWLOCK g_ioLock = SRWLOCK_INIT; static std::map<HANDLE, int> g_ioHandles;   // handle -> 0 other, 1 pack
static thread_local bool t_inIoHook = false;
static volatile LONG g_ioStreamDelta = 0;   // research: u16 shift for streamed terrain height data (0 = off)
void IoStreamDelta(int d) { g_ioStreamDelta = d; Log("[io] stream height patch %d", d); }
static uint32_t IoShiftSamples(uint8_t* buf, uint32_t n, int dlt) {
    uint32_t k = 0;
    CDK_GUARD_BEGIN
        for (uint32_t o = 0; o + 2 <= n; o += 2) { uint16_t* v = (uint16_t*)(buf + o); int x = (int)*v - dlt; *v = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); k++; }
    CDK_GUARD_FAIL
    CDK_GUARD_END
    return k;
}
static void LoadIoRanges() {
    std::vector<IoRange> v; FILE* f = fopen((ModDir() + "\\ioranges.txt").c_str(), "r");
    if (f) { char line[512]; while (fgets(line, sizeof line, f)) { char paz[64], lab[256] = ""; unsigned long long o, n;
        if (sscanf(line, "%63s %llu %llu %255s", paz, &o, &n, lab) >= 3) v.push_back({ paz, o, n, lab }); } fclose(f); }
    AcquireSRWLockExclusive(&g_ioLock); g_ioRanges.swap(v); g_ioHandles.clear(); ReleaseSRWLockExclusive(&g_ioLock);
    Log("[io] %zu watched ranges", g_ioRanges.size());
}
void SetIoTrace(bool on) { if (on) LoadIoRanges(); g_traceIo = on; g_ioLogged = 0; Log("[io] trace %s", on ? "on" : "off"); }
typedef LONG(NTAPI* NtReadFileFn)(HANDLE, HANDLE, void*, void*, void*, void*, ULONG, PLARGE_INTEGER, PULONG);
static NtReadFileFn g_origNtReadFile = nullptr;
static LONG NTAPI HookNtReadFile(HANDLE h, HANDLE ev, void* apc, void* apcCtx, void* iosb, void* buf, ULONG n, PLARGE_INTEGER off, PULONG key) {
    if (!g_traceIo || t_inIoHook || g_ioLogged >= 3000) return g_origNtReadFile(h, ev, apc, apcCtx, iosb, buf, n, off, key);
    t_inIoHook = true;
    int kind = -1; AcquireSRWLockShared(&g_ioLock); auto it = g_ioHandles.find(h); if (it != g_ioHandles.end()) kind = it->second; ReleaseSRWLockShared(&g_ioLock);
    char name[MAX_PATH] = { 0 };
    if (kind != 0) {   // first sight of this handle (or a pack): resolve its name once
        kind = 0;
        if (GetFileType(h) == FILE_TYPE_DISK && GetFinalPathNameByHandleA(h, name, MAX_PATH, 0)) { size_t l = strlen(name); if (l > 4 && _stricmp(name + l - 4, ".paz") == 0) kind = 1; }
        AcquireSRWLockExclusive(&g_ioLock); g_ioHandles[h] = kind; ReleaseSRWLockExclusive(&g_ioLock);
    }
    unsigned long long pos = ~0ull;
    if (kind == 1) {
        if (off && off->QuadPart >= 0) pos = (unsigned long long)off->QuadPart;
        else { LARGE_INTEGER z{}, cur{}; if (SetFilePointerEx(h, z, &cur, FILE_CURRENT)) pos = (unsigned long long)cur.QuadPart; }
    }
    const LONG r = g_origNtReadFile(h, ev, apc, apcCtx, iosb, buf, n, off, key);
    if (kind == 1) {
        if (!name[0]) GetFinalPathNameByHandleA(h, name, MAX_PATH, 0);
        const char* fn = strrchr(name, '\\'); const char* dir = fn ? fn - 1 : name; while (dir > name && *dir != '\\') dir--; if (*dir == '\\') dir++;
        std::string label; bool want = false, heightData = false;
        AcquireSRWLockShared(&g_ioLock);
        if (g_ioRanges.empty()) want = g_ioLogged < 60;
        else for (const auto& rg : g_ioRanges) if (_stricmp(rg.paz.c_str(), dir) == 0 && pos != ~0ull && pos < rg.off + rg.size && pos + n > rg.off) {
            want = true; char b[320]; snprintf(b, sizeof b, " %s+%lld", rg.label.c_str(), (long long)(pos - rg.off)); label += b;
            if (pos == rg.off + 128 && n == rg.size - 128 && rg.label.find("height16f/") != std::string::npos) heightData = true; }
        ReleaseSRWLockShared(&g_ioLock);
        if (heightData && g_ioStreamDelta && buf) {   // research: wait for the async read, then shift the L16 samples (races the consumer)
            volatile LONG* st = (volatile LONG*)iosb; int waited = 0;
            if (r == 0x103 && st) while (*st == 0x103 && waited < 2000) { Sleep(1); waited++; }
            const uint32_t k = IoShiftSamples((uint8_t*)buf, n, g_ioStreamDelta);
            Log("[io] stream height patch: %lu B at %p, %u samples shifted by %d after %d ms (status now %lx)", n, buf, k, -(int)g_ioStreamDelta, waited, st ? (unsigned long)*st : 0ul);
        }
        if (want) {
            InterlockedIncrement(&g_ioLogged);
            void* frames[28]; USHORT k = RtlCaptureStackBackTrace(1, 28, frames, nullptr);
            std::string s; for (USHORT i = 0; i < k; i++) { uintptr_t a = (uintptr_t)frames[i]; if (InImage(a)) { char b[32]; snprintf(b, sizeof b, " %llx", (unsigned long long)(a - g_base)); s += b; } }
            Log("[io] read %lu B %s pos=%llu buf=%p status=%lx async=%d thread=%lu%s stack:%s", n, dir, pos, buf, (unsigned long)r, ev || apc ? 1 : 0, GetCurrentThreadId(), label.c_str(), s.c_str());
        }
    }
    t_inIoHook = false;
    return r;
}
void InstallIoTrace() {
    g_traceIo = GetFileAttributesA((ModDir() + "\\traceio.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!g_traceIo) return;   // only installed for an explicit trace run
    LoadIoRanges();
    HMODULE nt = GetModuleHandleA("ntdll.dll"); if (!nt) return;
    void* t = (void*)GetProcAddress(nt, "NtReadFile"); if (!t) return;
    if (MH_CreateHook(t, (void*)HookNtReadFile, (void**)&g_origNtReadFile) == MH_OK && MH_EnableHook(t) == MH_OK) Log("[io] NtReadFile hooked (trace %s)", g_traceIo ? "on" : "off");
}

// ---- camwatch: which code writes the renderer camera's pose (free-fly camera research) ----
// Hardware write breakpoints (DR0..DR3) on the camera object's fields, set on every thread of the process except our own;
// a vectored handler counts each writing instruction (the RIP after the write) with its call chain. Removed after the time.
struct CwSite { volatile LONG64 rip; volatile LONG hits; volatile LONG slot; uintptr_t chain[8]; volatile LONG64 firstTick, lastTick; };
static CwSite g_cwSites[48]; static volatile LONG g_cwActive = 0; static uintptr_t g_cwAddr[4] = {};
static PVOID g_cwVeh = nullptr; static bool g_cwRw = false;   // g_cwRw: break on reads too (WatchAccessSync)
static LONG CALLBACK CamWatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_cwActive) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord; const DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = 0; while (slot < 4 && !(dr6 & (1ull << slot))) slot++;
    const LONG64 rip = (LONG64)c->Rip;
    for (auto& s : g_cwSites) {
        if (s.rip == rip && s.slot == slot) { InterlockedIncrement(&s.hits); s.lastTick = (LONG64)GetTickCount64(); break; }
        if (s.rip == 0 && InterlockedCompareExchange64(&s.rip, rip, 0) == 0) {
            s.slot = slot; s.hits = 1; s.firstTick = s.lastTick = (LONG64)GetTickCount64();
            CONTEXT u = *c;   // call chain via the unwind tables
            for (int i = 0; i < 8 && u.Rip; i++) {
                s.chain[i] = (uintptr_t)u.Rip;
                DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(u.Rip, &ib, nullptr);
                if (!rf) break;
                void* hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, rf, &u, &hd, &ef, nullptr);
            }
            break;
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void CwSetAll(bool on, bool quiet = false) {   // debug registers on every other thread of this process
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof te }; const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId(); int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID); if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = on ? g_cwAddr[0] : 0; c.Dr1 = on ? g_cwAddr[1] : 0; c.Dr2 = on ? g_cwAddr[2] : 0; c.Dr3 = on ? g_cwAddr[3] : 0;
                DWORD64 dr7 = 0;
                if (on) for (int i = 0; i < 4; i++) if (g_cwAddr[i]) dr7 |= (1ull << (i * 2)) | ((g_cwRw ? 3ull : 1ull) << (16 + i * 4)) | (3ull << (18 + i * 4));   // local enable, break on write (or read/write), length 4 (LEN 11)
                c.Dr7 = dr7; c.Dr6 = 0;
                if (SetThreadContext(t, &c)) n++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    if (!quiet) Log("[camwatch] debug registers %s on %d threads", on ? "set" : "cleared", n);
}
static DWORD WINAPI CamWatchThread(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg % 100, mode = (int)(intptr_t)arg / 100; const uintptr_t rcam = NativeCameraObject();
    // mode 0: the renderer camera (+0xC8 position x, +0xD0 z, +0xD8 / +0xE0); mode 1: the camera scene object's TiledTransform
    // (+0x1B0 rotation x, +0x1BC position x, +0x1C4 position z, +0x1CC tile), the pose the renderer camera copies every frame
    const uintptr_t so = CameraSceneObject(), cam = mode == 1 ? so : rcam;
    if (!cam) { Log("[camwatch] %s not resolved", mode == 1 ? "camera scene object" : "renderer camera"); InterlockedExchange(&g_cwActive, 0); return 0; }
    if (rcam) { uintptr_t link = 0; ReadBytes(rcam + 0x2A0, &link, 8); Log("[camwatch] renderer camera %p links %p (-0x28 = %p), camera scene object %p (%s)", (void*)rcam, (void*)link, (void*)(link ? link - 0x28 : 0), (void*)so, so && RttiName(so) ? RttiName(so) : "?"); }
    for (auto& s : g_cwSites) { s.rip = 0; s.hits = 0; s.slot = 0; memset(s.chain, 0, sizeof s.chain); }
    if (mode == 1) { g_cwAddr[0] = cam + 0x1B0; g_cwAddr[1] = cam + 0x1BC; g_cwAddr[2] = cam + 0x1C4; g_cwAddr[3] = cam + 0x1CC; }
    else { g_cwAddr[0] = cam + 0xC8; g_cwAddr[1] = cam + 0xD0; g_cwAddr[2] = cam + 0xD8; g_cwAddr[3] = cam + 0xE0; }
    { float v[8] = {}; ReadBytes(cam + 0xC0, v, sizeof v); Log("[camwatch] camera %p: +0xC0.. %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f", (void*)cam, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); }
    if (!g_cwVeh) g_cwVeh = AddVectoredExceptionHandler(1, CamWatchVeh);
    CwSetAll(true);
    Sleep(seconds * 1000);
    CwSetAll(false);
    InterlockedExchange(&g_cwActive, 0);
    int n = 0;
    for (const auto& s : g_cwSites) {
        if (!s.rip) continue; n++;
        char line[400]; int k = 0;
        for (int i = 0; i < 8 && s.chain[i]; i++) k += snprintf(line + k, sizeof line - k, InImage(s.chain[i]) ? " %llx" : " ?%llx", (unsigned long long)(InImage(s.chain[i]) ? s.chain[i] - g_base : s.chain[i]));
        Log("[camwatch] DR%ld (+0x%llx) written before rva 0x%llx, %ld hits, chain:%s", s.slot, (unsigned long long)(g_cwAddr[s.slot] - cam),
            (unsigned long long)(InImage((uintptr_t)s.rip) ? (uintptr_t)s.rip - g_base : (uintptr_t)s.rip), s.hits, line);
    }
    Log("[camwatch] done: %d writing sites", n);
    return 0;
}
// Generic: hardware write breakpoints on up to four 4-byte fields for 'seconds', every writing instruction logged once with
// its hit count and call chain (same machinery as camwatch; only one watch at a time).
struct WwArgs { uintptr_t addr[4]; int seconds; char tag[32]; };
static DWORD WINAPI WatchWritesThread(LPVOID p) {
    WwArgs a = *(WwArgs*)p; delete (WwArgs*)p;
    for (auto& s : g_cwSites) { s.rip = 0; s.hits = 0; s.slot = 0; memset(s.chain, 0, sizeof s.chain); }
    for (int i = 0; i < 4; i++) g_cwAddr[i] = a.addr[i];
    if (!g_cwVeh) g_cwVeh = AddVectoredExceptionHandler(1, CamWatchVeh);
    CwSetAll(true); Sleep(a.seconds * 1000); CwSetAll(false);
    InterlockedExchange(&g_cwActive, 0);
    int n = 0;
    for (const auto& s : g_cwSites) {
        if (!s.rip) continue; n++;
        char line[400]; int k = 0;
        for (int i = 0; i < 8 && s.chain[i]; i++) k += snprintf(line + k, sizeof line - k, InImage(s.chain[i]) ? " %llx" : " ?%llx", (unsigned long long)(InImage(s.chain[i]) ? s.chain[i] - g_base : s.chain[i]));
        Log("[%s] DR%ld (%p) written before rva 0x%llx, %ld hits, chain:%s", a.tag, s.slot, (void*)g_cwAddr[s.slot],
            (unsigned long long)(InImage((uintptr_t)s.rip) ? (uintptr_t)s.rip - g_base : (uintptr_t)s.rip), s.hits, line);
    }
    Log("[%s] write watch done: %d writing sites", a.tag, n);
    return 0;
}
// Continuous variant for the fall watcher: breakpoints stay armed (re-applied to new threads by RefreshWatch), every site keeps
// its first / last write time, DumpWatch logs the sites written inside a time window.
bool StartWatch(const uintptr_t addr[4]) {
    if (InterlockedCompareExchange(&g_cwActive, 1, 0) != 0) return false;
    for (auto& s : g_cwSites) { s.rip = 0; s.hits = 0; s.slot = 0; s.firstTick = s.lastTick = 0; memset(s.chain, 0, sizeof s.chain); }
    for (int i = 0; i < 4; i++) g_cwAddr[i] = addr[i];
    if (!g_cwVeh) g_cwVeh = AddVectoredExceptionHandler(1, CamWatchVeh);
    CwSetAll(true); return true;
}
void RefreshWatch() { if (g_cwActive) CwSetAll(true, true); }
void StopWatch() { if (!g_cwActive) return; CwSetAll(false); InterlockedExchange(&g_cwActive, 0); }
void DumpWatch(const char* tag, uint64_t from, uint64_t to) {
    int n = 0;
    for (const auto& s : g_cwSites) {
        if (!s.rip || (uint64_t)s.lastTick < from || (uint64_t)s.firstTick > to) continue; n++;
        char line[400]; int k = 0;
        for (int i = 0; i < 8 && s.chain[i]; i++) k += snprintf(line + k, sizeof line - k, InImage(s.chain[i]) ? " %llx" : " ?%llx", (unsigned long long)(InImage(s.chain[i]) ? s.chain[i] - g_base : s.chain[i]));
        Log("[%s]   DR%ld written before rva 0x%llx, %ld hits total, first %lld ms / last %lld ms before, chain:%s", tag, s.slot,
            (unsigned long long)(InImage((uintptr_t)s.rip) ? (uintptr_t)s.rip - g_base : (uintptr_t)s.rip), s.hits, (long long)(to - s.firstTick), (long long)(to - s.lastTick), line);
    }
    Log("[%s]   %d writing sites in the window", tag, n);
}
// Read/write watch armed synchronously: a helper thread sets the debug registers on every other thread - including the caller,
// which waits until they are set - so the very next access from the caller's own code is caught (who consumes a buffer).
struct WaArgs { uintptr_t addr[4]; int seconds; char tag[32]; HANDLE armed; };
static DWORD WINAPI WatchAccessThread(LPVOID p) {
    WaArgs a = *(WaArgs*)p; delete (WaArgs*)p;
    for (auto& s : g_cwSites) { s.rip = 0; s.hits = 0; s.slot = 0; memset(s.chain, 0, sizeof s.chain); }
    for (int i = 0; i < 4; i++) g_cwAddr[i] = a.addr[i];
    if (!g_cwVeh) g_cwVeh = AddVectoredExceptionHandler(1, CamWatchVeh);
    g_cwRw = true; CwSetAll(true, true); SetEvent(a.armed);
    Sleep(a.seconds * 1000); CwSetAll(false, true); g_cwRw = false;
    InterlockedExchange(&g_cwActive, 0);
    int n = 0;
    for (const auto& s : g_cwSites) {
        if (!s.rip) continue; n++;
        char line[400]; int k = 0;
        for (int i = 0; i < 8 && s.chain[i]; i++) k += snprintf(line + k, sizeof line - k, InImage(s.chain[i]) ? " %llx" : " ?%llx", (unsigned long long)(InImage(s.chain[i]) ? s.chain[i] - g_base : s.chain[i]));
        Log("[%s] DR%ld (%p) accessed before rva 0x%llx, %ld hits, chain:%s", a.tag, s.slot, (void*)g_cwAddr[s.slot],
            (unsigned long long)(InImage((uintptr_t)s.rip) ? (uintptr_t)s.rip - g_base : (uintptr_t)s.rip), s.hits, line);
    }
    Log("[%s] access watch done: %d sites", a.tag, n);
    return 0;
}
bool WatchAccessSync(const uintptr_t addr[4], int seconds, const char* tag) {
    if (InterlockedCompareExchange(&g_cwActive, 1, 0) != 0) return false;
    WaArgs* a = new WaArgs{}; for (int i = 0; i < 4; i++) a->addr[i] = addr[i]; a->seconds = seconds < 1 ? 1 : seconds > 30 ? 30 : seconds;
    strncpy_s(a->tag, tag ? tag : "access", _TRUNCATE); a->armed = CreateEventA(nullptr, TRUE, FALSE, nullptr); HANDLE ev = a->armed;
    HANDLE t = CreateThread(nullptr, 0, WatchAccessThread, a, 0, nullptr);
    if (t) { WaitForSingleObject(ev, 2000); CloseHandle(t); }
    CloseHandle(ev); return t != nullptr;
}
bool WatchWrites(const uintptr_t addr[4], int seconds, const char* tag) {
    if (InterlockedCompareExchange(&g_cwActive, 1, 0) != 0) return false;
    WwArgs* a = new WwArgs{}; for (int i = 0; i < 4; i++) a->addr[i] = addr[i]; a->seconds = seconds < 1 ? 1 : seconds > 60 ? 60 : seconds;
    strncpy_s(a->tag, tag ? tag : "watch", _TRUNCATE);
    CreateThread(nullptr, 0, WatchWritesThread, a, 0, nullptr);
    return true;
}
void CamWatch(int seconds, int mode) {
    if (InterlockedCompareExchange(&g_cwActive, 1, 0) != 0) { Log("[camwatch] already running"); return; }
    CreateThread(nullptr, 0, CamWatchThread, (LPVOID)(intptr_t)((seconds < 1 ? 1 : seconds > 30 ? 30 : seconds) + 100 * (mode == 1 ? 1 : 0)), 0, nullptr);
}
}   // namespace core
