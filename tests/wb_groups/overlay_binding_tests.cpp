// Host seam for the reconciled v0.95 overlay. Real production capture, generation,
// helper precedence and resize policy run against the retained COM/PE event model.
// MinHook is modeled only in the deterministic variant; the native variant uses
// real DXGI/WARP and MinHook. No fixture path writes a foreign vtable.
#define NOMINMAX
#define WB_OVERLAY_BINDING_TEST
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <future>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <new>
#include "../../tools/imgui/imgui.h"
#include "../../asi/cdmodkit/core.h"
#include "../../asi/cdmodkit/input.h"
#include "../../asi/cdmodkit/thumbgen.h"
#include "../../asi/cdmodkit/guard.h"

// =========================================================================================================
// Deterministic allocation-failure seam (test only). When armed, exactly the next allocation throws
// std::bad_alloc, which is the allocation the resize lease construction performs after a successful queue
// validation. Every global new/delete form is replaced with the C allocator so no deallocation is mismatched.
// =========================================================================================================
static std::atomic<int> g_testFailNextAlloc{0};
void* operator new(size_t n) {
    if (g_testFailNextAlloc.load(std::memory_order_acquire) > 0 && g_testFailNextAlloc.fetch_sub(1, std::memory_order_acq_rel) > 0) throw std::bad_alloc();
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// =========================================================================================================
// Fixture: module identity, in-memory PE64 import image, callable fake COM objects.
// =========================================================================================================
namespace fx {
    // Canonical IDXGISwapChain4 IID {3D585D5A-BD4A-489E-B1F4-3DBCB6452FFB}: the game QIs the created chain with it.
    const GUID kIidSwapChain4Canonical = { 0x3D585D5A, 0xBD4A, 0x489E, { 0xB1, 0xF4, 0x3D, 0xBC, 0xB6, 0x45, 0x2F, 0xFB } };
    // The former (non-canonical) tail the contract carried: kept only so the fixture can prove it is rejected.
    const GUID kIidSwapChain4Former = { 0x3D585D5A, 0xBD4A, 0x489E, { 0xB1, 0xF4, 0x3B, 0xCE, 0x6F, 0xC5, 0xCC, 0x4D } };
    const GUID kWbCookieGuidProbe = { 0x7F2B1C94, 0x3A6D, 0x4BE1, { 0x9C, 0x5A, 0x11, 0x6E, 0xD0, 0x44, 0x2A, 0x87 } };   // same value as the production cookie GUID
    const int kSlots = 48;
    const size_t kImageSize = 0x20000;
    const uintptr_t kRvaImport = 0x1000;
    const uintptr_t kRvaNames = 0x1100;
    const uintptr_t kRvaHintName = 0x1200;
    const uintptr_t kRvaInt = 0x1400;
    const uintptr_t kRvaIat = 0x1600;
    const uintptr_t kRvaKernelName = 0x1800;   // descriptor 1's provider name (a real image may use an API-set name)
    // Game-side creation boundary model (v3): the modeled helper lives in the image's executable section, references
    // the anchor string, carries the head signature, contains the call-site signature, and has one .pdata entry.
    const uintptr_t kRvaBoundaryString = 0x2000;
    const uintptr_t kRvaBoundaryFn = 0x2100;
    const uintptr_t kRvaBoundaryPdata = 0x2200;
    const uintptr_t kRvaBoundaryUnwind = 0x2300;
    const char kBoundaryString[] = "SwapChain::CreateSwapChainForHwnd failed: %d";
    void* boundaryTarget = nullptr;              // the resolved modeled helper head (image + kRvaBoundaryFn)
    bool boundaryHooked = false;                 // the fixture hook installer patched the head
    void* boundaryHost = nullptr;                // the modeled game object for the current case
    int boundaryUnwindMode = 0;                  // .pdata shape: 0 plain, 1 chain-info fragment, 2 EHANDLER tail, 3 cyclic chain
    bool boundaryHeadSigCorrupt = false;         // corrupt a byte after the 16-byte sanity prefix
    // MinHook failure injection (the status values are MinHook.h enumerants; MH_OK == 0). A failed CreateHook never
    // patches; a failed EnableHook leaves the target unpatched, exactly like the real library.
    int boundaryHookCreateStatus = 0, boundaryHookEnableStatus = 0;   // the helper target
    int legacyHookCreateStatus = 0, legacyHookEnableStatus = 0;       // the discovered DXGI function
    unsigned char boundarySavedBytes[16] = {};                        // head bytes restored when enabling the helper hook fails
    // The modeled DXGI function: the legacy adapter discovers this address from a dummy factory's slot 15, and every
    // table entry holding it dispatches through the armed detour exactly as a MinHook patch would.
    struct SystemHwndHook { bool active = false; void* detour = nullptr; void* original = nullptr; };
    SystemHwndHook systemHwndHook;
    HRESULT STDMETHODCALLTYPE SystemHwndBody(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    void* BoundaryHelperImplModel(void* self);   // the model body the trampoline returns to

    std::mutex logMutex;
    std::vector<std::string> logs, allLogs;
    void Capture(const std::string& line) { std::lock_guard<std::mutex> l(logMutex); logs.push_back(line); allLogs.push_back(line); }
    void ClearLogs() { std::lock_guard<std::mutex> l(logMutex); logs.clear(); }
    std::vector<std::string> LinesWith(const std::string& token) {
        std::lock_guard<std::mutex> lock(logMutex);
        std::vector<std::string> out;
        for (const auto& l : logs) if (l.find(token) != std::string::npos) out.push_back(l);
        return out;
    }
    size_t CountWith(const std::string& token) { return LinesWith(token).size(); }
    std::string Field(const std::string& line, const std::string& key) {
        const std::string needle = key + " ";
        size_t p = line.find(needle);
        if (p == std::string::npos) return {};
        size_t s = p + needle.size(), e = line.find(' ', s);
        return line.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }
    std::string PtrText(const void* p) { char b[32] = {}; std::snprintf(b, sizeof b, "%p", p); return b; }

    // ---- module identity (the seam substitutes the mapping, never the production filter) ----
    unsigned char* image = nullptr;
    HMODULE mainModule = nullptr;
    HMODULE dxgiModule = nullptr;
    HMODULE slModule = (HMODULE)(uintptr_t)0x12340000ULL;
    HMODULE reshadeModule = (HMODULE)(uintptr_t)0x12350000ULL;
    HMODULE foreignModule = (HMODULE)(uintptr_t)0x12360000ULL;
    std::map<HMODULE, std::string> moduleNames;
    std::map<uintptr_t, HMODULE> owners;
    // Synthetic module address ranges: a wrapper's heap vtable belongs to the wrapper module for identity purposes.
    struct ModuleSpan { uintptr_t lo; uintptr_t hi; HMODULE mod; };
    std::vector<ModuleSpan> moduleSpans;
    std::set<uintptr_t> unowned;
    std::vector<HMODULE> pins;
    std::vector<HMODULE> pinAttempts;
    std::atomic<int> patchFreeDuringRender{-1}, renderFreeDuringPatch{-1};
    std::atomic<int> comUnderPatchLock{0};               // set when a fixture COM method runs with the patch mutex held
    bool PatchLockIsFree();                              // defined after the production include
    std::set<HMODULE> pinFails;                          // modules whose pin must fail (prerequisite negative case)
    HMODULE pinBlockModule = nullptr; unsigned pinBlockCount = 0;   // pause the next N pin attempts (ordering only)
    std::mutex pinMutex; std::condition_variable pinCv; bool pinEntered = false, pinReleaseFlag = false, pinGateTimeout = false;
    bool BlockPinGate() {
        std::unique_lock<std::mutex> l(pinMutex);
        pinEntered = true;
        pinCv.notify_all();
        const bool released = pinCv.wait_for(l, std::chrono::seconds(10), [] { return pinReleaseFlag; });
        if (!released) pinGateTimeout = true;
        return released;
    }
    bool WaitPinEntered(unsigned boundMs) {
        std::unique_lock<std::mutex> l(pinMutex);
        const bool entered = pinCv.wait_for(l, std::chrono::milliseconds(boundMs), [] { return pinEntered; });
        if (!entered) pinGateTimeout = true;
        return entered;
    }
    void ReleasePinGate() { std::lock_guard<std::mutex> l(pinMutex); pinReleaseFlag = true; pinCv.notify_all(); }
    void ResetPinGate() { std::lock_guard<std::mutex> l(pinMutex); pinEntered = false; pinReleaseFlag = false; pinGateTimeout = false; }
    // Private-entry fixture: an entry that is NOT an interface. size 0 = zero-sized, 1 = pointer-sized blob,
    // 2 = oversized blob; payloadReads proves production never took the payload path.
    int foreignBlobMode = -1;
    long payloadReads = 0;
    // Staged fault injection: each capture stage bumps `stage`, and the stage selected by `faultAt` raises.
    std::atomic<int> stage{0}, faultAt{0}, faultsRaised{0};
    void StageGate() {
        ++stage;
        if (faultAt && stage == faultAt) { ++faultsRaised; RaiseException(0xC0000005, 0, 0, nullptr); }
    }

    void CopyToken(char* out, size_t cap, const char* src) {
        if (!out || !cap) return;
        size_t i = 0;
        if (src) for (; src[i] && i + 1 < cap; i++) out[i] = src[i];
        out[i] = 0;
    }
    bool InImage(uintptr_t a) { return image && a >= reinterpret_cast<uintptr_t>(image) && a < reinterpret_cast<uintptr_t>(image) + kImageSize; }
    // The fixture declares the module of its own call sites (each shim wraps its call in a CallerScope);
    // production still performs the module comparison that decides eligibility.
    thread_local HMODULE t_declaredCallerModule = nullptr;
    struct CallerScope {
        HMODULE prev;
        explicit CallerScope(HMODULE m) : prev(t_declaredCallerModule) { t_declaredCallerModule = m; }
        ~CallerScope() { t_declaredCallerModule = prev; }
        CallerScope(const CallerScope&) = delete; CallerScope& operator=(const CallerScope&) = delete;
    };
    HMODULE OsModuleFromAddress(const void* addr) {
        if (!addr) return nullptr;
        const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        if (unowned.count(a)) return nullptr;
        auto it = owners.find(a);                // the exact provider identity of a known export wins
        if (it != owners.end()) return it->second;
        for (const auto& s : moduleSpans) if (a >= s.lo && a < s.hi) return s.mod;   // a synthetic (foreign) module range
        if (t_declaredCallerModule) return t_declaredCallerModule;
        if (InImage(a)) return mainModule;
        return dxgiModule;                       // fixture code and tables stand for one provider module
    }
    bool OsModuleFileName(HMODULE mod, char* out, size_t cap) {
        auto it = moduleNames.find(mod);
        if (it == moduleNames.end()) return false;
        CopyToken(out, cap, it->second.c_str());
        return true;
    }
    std::mutex pinRecordMutex;
    HMODULE OsPinModule(HMODULE mod) {
        { std::lock_guard<std::mutex> l(pinRecordMutex); pinAttempts.push_back(mod); }
        if (mod && mod == pinBlockModule && pinBlockCount > 0) { pinBlockCount--; BlockPinGate(); }
        if (pinFails.count(mod)) return nullptr;          // prerequisite failure path
        { std::lock_guard<std::mutex> l(pinRecordMutex); pins.push_back(mod); }
        return mod;
    }
    uintptr_t OsMainImageBase() { return reinterpret_cast<uintptr_t>(image); }

    // ---- protection seam: records transactions, injects protection/CAS faults, hosts the publication barrier ----
    struct ProtectCall { uintptr_t cell; DWORD prot; };
    std::vector<ProtectCall> protectCalls;
    size_t ProtectWritesFor(const void* cell) {
        size_t n = 0;
        for (const auto& c : protectCalls) if (c.cell == reinterpret_cast<uintptr_t>(cell) && c.prot == PAGE_READWRITE) ++n;
        return n;
    }
    void* failProtectCell = nullptr;
    bool failRestore = false;
    void* conflictCell = nullptr;
    void* conflictValue = nullptr;
    void* pauseCell = nullptr;
    bool paused = false, resume = false, gateTimeout = false;
    std::mutex gateMutex;
    std::condition_variable gateCv;

    BOOL WINAPI OsVirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) {
        (void)n;
        if (old) *old = PAGE_READONLY;
        if (prot != PAGE_READWRITE && failRestore) return FALSE;
        if (prot == PAGE_READWRITE && p == failProtectCell) return FALSE;
        protectCalls.push_back({ reinterpret_cast<uintptr_t>(p), prot });
        if (prot == PAGE_READWRITE && p == conflictCell && conflictValue) {
            *reinterpret_cast<void**>(p) = conflictValue;    // a foreign writer got there before our CAS
            conflictCell = nullptr;
        }
        if (prot == PAGE_READWRITE && p == pauseCell) {
            std::unique_lock<std::mutex> l(gateMutex);
            paused = true;
            gateCv.notify_all();
            if (!gateCv.wait_for(l, std::chrono::seconds(20), [] { return resume; })) {       // a missing release is a bounded failure, never a hang
                gateTimeout = true;
                paused = false;
                return FALSE;                                                                 // the protection fails, so no cell is published from a stalled gate
            }
            paused = false;
        }
        return TRUE;
    }
    bool WaitPaused(unsigned boundMs) {
        std::unique_lock<std::mutex> l(gateMutex);
        return gateCv.wait_for(l, std::chrono::milliseconds(boundMs), [] { return paused; });
    }
    void ReleasePause() {
        std::lock_guard<std::mutex> l(gateMutex);
        resume = true;
        gateCv.notify_all();
    }
    // Chain3-QI ordering gate: pauses the next N chain3 QIs (the present qualification path) so a schedule can
    // interleave an attachment removal exactly between a taken metadata reference and the render lock.
    int chain3QiBlockCount = 0;
    std::mutex chain3QiMutex; std::condition_variable chain3QiCv;
    bool chain3QiEntered = false, chain3QiReleaseFlag = false, chain3QiTimeout = false;
    bool BlockChain3Qi() {
        std::unique_lock<std::mutex> l(chain3QiMutex);
        chain3QiEntered = true;
        chain3QiCv.notify_all();
        const bool released = chain3QiCv.wait_for(l, std::chrono::seconds(10), [] { return chain3QiReleaseFlag; });
        if (!released) chain3QiTimeout = true;
        return released;
    }
    bool WaitChain3QiEntered(unsigned boundMs) {
        std::unique_lock<std::mutex> l(chain3QiMutex);
        const bool entered = chain3QiCv.wait_for(l, std::chrono::milliseconds(boundMs), [] { return chain3QiEntered; });
        if (!entered) chain3QiTimeout = true;
        return entered;
    }
    void ReleaseChain3Qi() { std::lock_guard<std::mutex> l(chain3QiMutex); chain3QiReleaseFlag = true; chain3QiCv.notify_all(); }
    void ResetChain3QiGate() { std::lock_guard<std::mutex> l(chain3QiMutex); chain3QiEntered = false; chain3QiReleaseFlag = false; chain3QiTimeout = false; }
    // Fault injection on the present/resize qualification: the QI writes its output and reference first, then raises,
    // which is exactly the "output written, then fault" case the guarded slot frames must survive.
    int faultChain3Qi = 0;
    int faultIdentityQi = 0;
    // Allocation-failure ordering: the next fixture ChainGetDesc arms the global allocation seam, whose next
    // allocation is the lease construction that follows a successful validation.
    bool armAllocFailOnDesc = false;
    void ArmNextAllocFailure() { g_testFailNextAlloc.store(1, std::memory_order_release); }
    bool FaultDrainHook();

    // ---- fake COM objects: real callable vtables, fixture-owned lifetime ----
    HRESULT STDMETHODCALLTYPE ObjQiRaw(IUnknown* self, REFIID iid, void** pp);
    ULONG STDMETHODCALLTYPE ObjAddRefRaw(IUnknown* self);
    ULONG STDMETHODCALLTYPE ObjReleaseRaw(IUnknown* self);
    HRESULT STDMETHODCALLTYPE ObjQi(IUnknown* self, REFIID iid, void** pp);
    ULONG STDMETHODCALLTYPE ObjAddRef(IUnknown* self);
    ULONG STDMETHODCALLTYPE ObjRelease(IUnknown* self);
    struct Obj {
        void** vt = nullptr;
        LONG refs = 1;
        void* identity = nullptr;
        bool identityFail = false;                 // the canonical IUnknown QI returns E_NOINTERFACE (null chain id fixture)
        struct AliasEntry { GUID iid; void* ptr; };
        AliasEntry aliases[8] = {};
        int aliasCount = 0;
        void AddAlias(REFIID iid, void* p) {   // replaces an existing entry for the same IID (a test can repoint an alias)
            for (int i = 0; i < aliasCount; i++) if (aliases[i].iid == iid) { aliases[i].ptr = p; return; }
            if (aliasCount < 8) { aliases[aliasCount].iid = iid; aliases[aliasCount].ptr = p; aliasCount++; }
        }
        ULONG AddRef() { return ObjAddRef(reinterpret_cast<IUnknown*>(this)); }
        ULONG Release() { return ObjRelease(reinterpret_cast<IUnknown*>(this)); }
        HRESULT QueryInterface(REFIID iid, void** pp) { return ObjQi(reinterpret_cast<IUnknown*>(this), iid, pp); }
    };
    struct Device : Obj { UINT nodes = 1; };
    struct Queue : Obj {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        Device* dev = nullptr;
    };
    bool chain3QiFails = false;                          // forces the chain3 QI to fail (capture qualification negative case)
    struct Chain : Obj {
        HWND hwnd = nullptr;
        UINT w = 0, h = 0, buffers = 3;
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        Device* dev = nullptr;
        Chain* forwardCookieTo = nullptr;          // wrapper forwards private data to another object
        long presents = 0, presents1 = 0, resizes = 0, resizes1 = 0;
        bool resizeCallsNested = false;                  // the original resize re-enters the patched slot (nested lifecycle)
        int resizeBlockCount = 0;                        // pause the next N resize originals on the gate (ordering only)
        Chain* nestedResizeChain = nullptr;              // the original synchronously resizes this other chain (A.original -> B.resize)
        HRESULT presentHr = S_OK, resizeHr = S_OK, resize1Hr = S_OK, descHr = S_OK, cookieHr = S_OK, deviceHr = S_OK;
        IUnknown* cookie = nullptr; bool hasCookie = false; GUID cookieGuid = {};
        bool cookieIsForeign = false;
        bool present1CallsPresent = false;
        Chain* wrapperInner = nullptr;
        UINT lastResizeCount = 0;
        UINT lastWidth = 0, lastHeight = 0; DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN; UINT lastFlags = 0;
        UINT lastNodes[8] = {}; IUnknown* lastQueues[8] = {};
        UINT getBufferCalls = 0, currentIndex = 0;
    };
    struct Factory : Obj {
        Chain* chain = nullptr;
        Factory* innerFactory = nullptr;
        Chain* innerChain = nullptr;
        Queue* outerQueue = nullptr;
        Queue* innerQueue = nullptr;
        HRESULT hwndHr = S_OK;
        bool hwndNullOutput = false;
        long hwndCalls = 0, createCalls = 0;
    };

    std::atomic<long> addRefs{0}, releases{0}, destroyed{0};
    std::atomic<long> queueDescCalls{0}, chainDescCalls{0}, deviceNodeCalls{0};

    template<int N> static intptr_t StubSlot() { return static_cast<intptr_t>(N * 31 + 7); }
    template<int N> struct FillStub { static void Fill(void** vt) { FillStub<N - 1>::Fill(vt); vt[N] = reinterpret_cast<void*>(&StubSlot<N>); } };
    template<> struct FillStub<0> { static void Fill(void** vt) { vt[0] = reinterpret_cast<void*>(&StubSlot<0>); } };
    void** NewVt() {
        void** vt = static_cast<void**>(calloc(kSlots, sizeof(void*)));
        if (!vt) std::abort();
        FillStub<kSlots - 1>::Fill(vt);
        return vt;
    }
    Obj* NewIdent() {
        Obj* o = new Obj();
        o->vt = NewVt();
        o->vt[0] = reinterpret_cast<void*>(&ObjQiRaw);
        o->vt[1] = reinterpret_cast<void*>(&ObjAddRefRaw);
        o->vt[2] = reinterpret_cast<void*>(&ObjReleaseRaw);
        o->identity = o;
        return o;
    }

    // ---- specific slots ----
    HRESULT STDMETHODCALLTYPE FactoryQi(IUnknown* self, REFIID iid, void** pp);
    HRESULT STDMETHODCALLTYPE FactoryCreate(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp);
    HRESULT STDMETHODCALLTYPE FactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    HRESULT STDMETHODCALLTYPE InnerFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    HRESULT STDMETHODCALLTYPE AltFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    HRESULT STDMETHODCALLTYPE SlFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    HRESULT STDMETHODCALLTYPE ChainQi(IUnknown* self, REFIID iid, void** pp);
    HRESULT STDMETHODCALLTYPE ChainGetDevice(IDXGISwapChain* self, REFIID iid, void** pp);
    HRESULT STDMETHODCALLTYPE ChainPresent(IDXGISwapChain* self, UINT sync, UINT flags);
    HRESULT STDMETHODCALLTYPE ChainPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p);
    HRESULT STDMETHODCALLTYPE ChainResize(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);
    HRESULT STDMETHODCALLTYPE ChainResize1(IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues);
    HRESULT STDMETHODCALLTYPE ChainGetDesc(IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* out);
    HRESULT STDMETHODCALLTYPE ChainGetHwnd(IDXGISwapChain1* self, HWND* out);
    UINT STDMETHODCALLTYPE ChainIndex(IDXGISwapChain3* self);
    HRESULT STDMETHODCALLTYPE ChainColor(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
    HRESULT STDMETHODCALLTYPE ChainGetBuffer(IDXGISwapChain* self, UINT idx, REFIID iid, void** pp);
    HRESULT STDMETHODCALLTYPE ChainSetPrivateDataInterface(IDXGIObject* self, REFGUID g, const IUnknown* p);
    HRESULT STDMETHODCALLTYPE ChainGetPrivateData(IDXGIObject* self, REFGUID g, UINT* size, void* data);
    HRESULT STDMETHODCALLTYPE QueueGetDevice(ID3D12DeviceChild* self, REFIID iid, void** pp);
    void STDMETHODCALLTYPE QueueGetDescShim(ID3D12CommandQueue* self, D3D12_COMMAND_QUEUE_DESC* out);
    UINT STDMETHODCALLTYPE DeviceGetNodeCount(ID3D12Device* self);

    // Exact SDK vtable indices for the methods the fixture must answer. A method that returns a struct by value
    // (ID3D12CommandQueue::GetDesc) must match the member-function ABI the compiler emits at the call site:
    // this in RCX, the hidden return buffer in RDX -> a void shim (self, out) is ABI-identical.
    void ChainVt(void** vt) {
        vt[0] = reinterpret_cast<void*>(&ChainQi);
        vt[1] = reinterpret_cast<void*>(&ObjAddRef);
        vt[2] = reinterpret_cast<void*>(&ObjRelease);
        vt[4] = reinterpret_cast<void*>(&ChainSetPrivateDataInterface);
        vt[5] = reinterpret_cast<void*>(&ChainGetPrivateData);
        vt[7] = reinterpret_cast<void*>(&ChainGetDevice);
        vt[8] = reinterpret_cast<void*>(&ChainPresent);
        vt[9] = reinterpret_cast<void*>(&ChainGetBuffer);
        vt[12] = reinterpret_cast<void*>(&ChainGetDesc);
        vt[13] = reinterpret_cast<void*>(&ChainResize);
        vt[20] = reinterpret_cast<void*>(&ChainGetHwnd);
        vt[22] = reinterpret_cast<void*>(&ChainPresent1);
        vt[36] = reinterpret_cast<void*>(&ChainIndex);
        vt[38] = reinterpret_cast<void*>(&ChainColor);
        vt[39] = reinterpret_cast<void*>(&ChainResize1);
    }
    void FactoryVt(void** vt, void* hwndImpl) {
        vt[0] = reinterpret_cast<void*>(&FactoryQi);
        vt[1] = reinterpret_cast<void*>(&ObjAddRef);
        vt[2] = reinterpret_cast<void*>(&ObjRelease);
        vt[10] = reinterpret_cast<void*>(&FactoryCreate);
        vt[15] = hwndImpl ? hwndImpl : reinterpret_cast<void*>(&FactoryHwnd);
    }
    void QueueVt(void** vt) {
        vt[0] = reinterpret_cast<void*>(&ObjQi);
        vt[1] = reinterpret_cast<void*>(&ObjAddRef);
        vt[2] = reinterpret_cast<void*>(&ObjRelease);
        vt[7] = reinterpret_cast<void*>(&QueueGetDevice);
        vt[18] = reinterpret_cast<void*>(&QueueGetDescShim);
    }
    void DeviceVt(void** vt) {
        vt[0] = reinterpret_cast<void*>(&ObjQi);
        vt[1] = reinterpret_cast<void*>(&ObjAddRef);
        vt[2] = reinterpret_cast<void*>(&ObjRelease);
        vt[7] = reinterpret_cast<void*>(&DeviceGetNodeCount);   // ID3D12Device : ID3D12Object (GetNodeCount = slot 7, not via ID3D12DeviceChild)
    }
    Device* NewDevice(UINT nodes) {
        Device* d = new Device();
        d->vt = NewVt(); DeviceVt(d->vt);
        d->identity = NewIdent();
        d->nodes = nodes;
        return d;
    }
    Queue* NewQueue(Device* dev, D3D12_COMMAND_LIST_TYPE type, UINT nodeMask) {
        Queue* q = new Queue();
        q->vt = NewVt(); QueueVt(q->vt);
        q->identity = NewIdent();
        q->dev = dev;
        q->desc.Type = type; q->desc.NodeMask = nodeMask; q->desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL; q->desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        q->AddAlias(__uuidof(ID3D12CommandQueue), q);
        return q;
    }
    std::vector<Chain*> chainPool;                       // freed chains are recycled so address reuse is deterministic
    void FreeChainStorage(Chain* c) { if (c) chainPool.push_back(c); }   // the fixture destroys its own storage explicitly
    Chain* NewChain(HWND hwnd, Device* dev, UINT w, UINT h, UINT buffers, DXGI_FORMAT fmt) {
        Chain* c = nullptr;
        if (!chainPool.empty()) { c = chainPool.back(); chainPool.pop_back(); *c = Chain(); }   // reuse the exact dead storage
        else c = new Chain();
        c->vt = NewVt(); ChainVt(c->vt);
        c->identity = NewIdent();
        c->hwnd = hwnd; c->dev = dev; c->w = w; c->h = h; c->buffers = buffers; c->fmt = fmt;
        c->AddAlias(IID_IUnknown, c->identity);
        c->AddAlias(__uuidof(IDXGIObject), c);
        c->AddAlias(__uuidof(IDXGISwapChain), c);
        c->AddAlias(__uuidof(IDXGISwapChain1), c);
        c->AddAlias(__uuidof(IDXGISwapChain2), c);
        c->AddAlias(__uuidof(IDXGISwapChain3), c);
        c->AddAlias(kIidSwapChain4Canonical, c);      // the canonical IID the game QIs the created chain with
        return c;
    }
    Factory* NewFactory(Chain* chain, Queue* outerQueue) {
        Factory* f = new Factory();
        f->vt = NewVt(); FactoryVt(f->vt, nullptr);
        f->identity = NewIdent();
        f->chain = chain; f->outerQueue = outerQueue;
        f->AddAlias(IID_IUnknown, f->identity);
        f->AddAlias(__uuidof(IDXGIFactory), f);
        f->AddAlias(__uuidof(IDXGIFactory1), f);
        f->AddAlias(__uuidof(IDXGIFactory2), f);
        f->AddAlias(__uuidof(IDXGIFactory3), f);
        f->AddAlias(__uuidof(IDXGIFactory4), f);
        return f;
    }
    Factory* NewAliasFactory(Factory* parent, void* hwndImpl) {
        Factory* f = NewFactory(parent->chain, parent->outerQueue);
        FactoryVt(f->vt, hwndImpl);
        f->identity = parent->identity;
        return f;
    }

    // ---- exports the game can reach (direct imports and the resolver) ----
    long directCalls[3] = {};
    long resolveCalls = 0;
    long slCreateCalls = 0;
    long innerCreateCalls = 0;
    HRESULT directHr[3] = { S_OK, S_OK, S_OK };
    bool directNullOutput[3] = { false, false, false };
    bool slCreateFail = false;
    HRESULT slCreateHr = E_FAIL;
    Factory* lastDirectFactory = nullptr;
    Factory* lastSlFactory = nullptr;
    Chain* lastReturnedChain = nullptr;
    bool resolverFails = false, resolverForeign = false;
    FARPROC WINAPI NativeGetProcAddress(HMODULE mod, LPCSTR name);
    HRESULT WINAPI ExportCreateFactory(REFIID iid, void** pp);
    HRESULT WINAPI ExportCreateFactory1(REFIID iid, void** pp);
    HRESULT WINAPI ExportCreateFactory2(UINT flags, REFIID iid, void** pp);
    HRESULT WINAPI SlCreateFactory(REFIID iid, void** pp);
    HRESULT WINAPI SlCreateFactory1(REFIID iid, void** pp);
    HRESULT WINAPI SlCreateFactory2(UINT flags, REFIID iid, void** pp);
    intptr_t WINAPI ForeignExport();
    typedef HRESULT (STDMETHODCALLTYPE* HwndFn)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    HwndFn outerHwndImpl = nullptr;              // repointable downstream behavior (later provider redirection)

    // ---- in-memory PE64 image ----
    void** cellFactory0 = nullptr, ** cellFactory1 = nullptr, ** cellFactory2 = nullptr, ** cellResolver = nullptr;
    size_t g_allocSize = 0;
    uint32_t imageImportDirRva = 0, imageImportDirSize = 0;   // nonzero overrides for negative image cases
    uint32_t imageFirstThunkOverride = 0;                     // nonzero overrides descriptor 0's IAT RVA (alignment negative case)
    uint32_t imageSizeOfHeaders = 0x400;                      // the mapped header window; a header-bound case sets it smaller
    uint32_t imageSectionAlignment = 0x1000, imageFileAlignment = 0x200;   // binding-layout overrides (the ImageBounds split case)
    std::string kernelModuleName = "KERNEL32.dll";             // descriptor 1's provider name (identity negative case)
    // allocSize is what is mapped, claimedSize what the PE header announces: a mismatch models an unmapped range.
    void BuildImageSized(size_t allocSize, size_t claimedSize) {
        if (image) { VirtualFree(image, 0, MEM_RELEASE); image = nullptr; }
        image = static_cast<unsigned char*>(VirtualAlloc(nullptr, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));   // the modeled game-side helper executes from here
        if (!image) throw std::runtime_error("fixture image allocation failed");
        g_allocSize = allocSize;
        std::memset(image, 0, (std::min)(allocSize, static_cast<size_t>(0x20000)));
        mainModule = reinterpret_cast<HMODULE>(image);
        moduleNames[mainModule] = "crimsondesert.exe";
        const uintptr_t base = reinterpret_cast<uintptr_t>(image);
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        IMAGE_NT_HEADERS64* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(claimedSize);
        nt->OptionalHeader.SizeOfHeaders = imageSizeOfHeaders;
        nt->OptionalHeader.SectionAlignment = imageSectionAlignment;
        nt->OptionalHeader.FileAlignment = imageFileAlignment;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = imageImportDirRva ? imageImportDirRva : static_cast<DWORD>(kRvaImport);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = imageImportDirSize ? imageImportDirSize : sizeof(IMAGE_IMPORT_DESCRIPTOR) * 4;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        std::memcpy(sec->Name, ".rdata", 6);
        sec->VirtualAddress = 0x1000;
        sec->Misc.VirtualSize = 0x8000;
        sec->SizeOfRawData = 0x8000;
        sec->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;   // a real image has an executable section holding the imports

        // The descriptor table follows the declared import-directory RVA (the shipped image's is not 4-aligned); a
        // declared RVA outside the mapped fixture keeps the table where it was so the negative cases stay in bounds.
        const uintptr_t dirTableRva = (imageImportDirRva && imageImportDirRva + sizeof(IMAGE_IMPORT_DESCRIPTOR) * 4 <= kImageSize) ? imageImportDirRva : kRvaImport;
        IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dirTableRva);
        desc[0].Name = static_cast<DWORD>(kRvaNames);
        desc[0].OriginalFirstThunk = static_cast<DWORD>(kRvaInt);
        desc[0].FirstThunk = imageFirstThunkOverride ? imageFirstThunkOverride : static_cast<DWORD>(kRvaIat);
        desc[1].Name = static_cast<DWORD>(kRvaKernelName);
        desc[1].OriginalFirstThunk = static_cast<DWORD>(kRvaInt + 0x40);
        desc[1].FirstThunk = static_cast<DWORD>(kRvaIat + 0x40);
        desc[2].Name = static_cast<DWORD>(kRvaNames + 0x40);
        desc[2].OriginalFirstThunk = static_cast<DWORD>(kRvaInt + 0x80);
        desc[2].FirstThunk = static_cast<DWORD>(kRvaIat + 0x80);
        std::memcpy(image + kRvaNames, "dxgi.dll", 9);
        CopyToken(reinterpret_cast<char*>(image + kRvaKernelName), 0x40, kernelModuleName.c_str());
        std::memcpy(image + kRvaNames + 0x40, "user32.dll", 11);
        struct Named { uintptr_t off; const char* name; };
        const Named named[] = {
            { kRvaHintName + 0x00, "CreateDXGIFactory" },
            { kRvaHintName + 0x20, "CreateDXGIFactory1" },
            { kRvaHintName + 0x40, "CreateDXGIFactory2" },
            { kRvaHintName + 0x60, "GetProcAddress" },
            { kRvaHintName + 0x80, "Sleep" },
            { kRvaHintName + 0xA0, "MessageBoxW" },
        };
        for (const auto& n : named) {
            image[n.off] = 0; image[n.off + 1] = 0;
            std::memcpy(image + n.off + 2, n.name, std::strlen(n.name) + 1);
        }
        uint64_t* ints = reinterpret_cast<uint64_t*>(base + kRvaInt);
        void** iat = reinterpret_cast<void**>(base + kRvaIat);
        ints[0] = kRvaHintName + 0x00; ints[1] = kRvaHintName + 0x20; ints[2] = kRvaHintName + 0x40;
        iat[0] = reinterpret_cast<void*>(&ExportCreateFactory);
        iat[1] = reinterpret_cast<void*>(&ExportCreateFactory1);
        iat[2] = reinterpret_cast<void*>(&ExportCreateFactory2);
        uint64_t* intsK = reinterpret_cast<uint64_t*>(base + kRvaInt + 0x40);
        void** iatK = reinterpret_cast<void**>(base + kRvaIat + 0x40);
        intsK[0] = kRvaHintName + 0x60; intsK[1] = kRvaHintName + 0x80;
        iatK[0] = reinterpret_cast<void*>(&NativeGetProcAddress);
        iatK[1] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&StubSlot<1>));
        uint64_t* intsU = reinterpret_cast<uint64_t*>(base + kRvaInt + 0x80);
        void** iatU = reinterpret_cast<void**>(base + kRvaIat + 0x80);
        intsU[0] = kRvaHintName + 0xA0;
        iatU[0] = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&StubSlot<2>));

        // ---- game-side creation boundary model (v3) -----------------------------------------------------------------
        std::memcpy(image + kRvaBoundaryString, kBoundaryString, sizeof(kBoundaryString));
        {
            static const unsigned char head[36] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                                    0x48, 0x8D, 0x6C, 0x24, 0xE0, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x4C, 0x8B, 0x79, 0x40 };
            static const unsigned char site[32] = { 0x49, 0x8B, 0x9F, 0x40, 0x09, 0x00, 0x00, 0x48, 0x8B, 0x03, 0x4C, 0x8B, 0x60, 0x78, 0x48, 0x8B,
                                                    0x4D, 0x78, 0x48, 0x85, 0xC9, 0x74, 0x0D, 0x48, 0x8B, 0x01, 0xC5, 0xF8, 0x77, 0x00, 0x00, 0x00 };
            std::memcpy(image + kRvaBoundaryFn, head, sizeof head);
            unsigned char lea[7] = { 0x48, 0x8D, 0x05, 0, 0, 0, 0 };
            const int32_t disp = static_cast<int32_t>(kRvaBoundaryString) - static_cast<int32_t>(kRvaBoundaryFn + sizeof head + sizeof lea);
            std::memcpy(lea + 3, &disp, 4);
            std::memcpy(image + kRvaBoundaryFn + sizeof head, lea, sizeof lea);
            std::memcpy(image + kRvaBoundaryFn + sizeof head + sizeof lea, site, sizeof site);
            const uintptr_t jmpOff = kRvaBoundaryFn + sizeof head + sizeof lea + sizeof site;
            unsigned char jmp[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };   // mov rax, &model ; jmp rax
            const uint64_t body = reinterpret_cast<uint64_t>(&BoundaryHelperImplModel);
            std::memcpy(jmp + 2, &body, 8);
            std::memcpy(image + jmpOff, jmp, sizeof jmp);
            if (boundaryHeadSigCorrupt) image[kRvaBoundaryFn + 20] = 0xFF;   // bytes 17..35 of the head signature mismatch
            RUNTIME_FUNCTION* rf = reinterpret_cast<RUNTIME_FUNCTION*>(base + kRvaBoundaryPdata);
            const DWORD fnBegin = static_cast<DWORD>(kRvaBoundaryFn);
            const DWORD fnEnd = static_cast<DWORD>(jmpOff + sizeof jmp);   // 0x2157
            unsigned char* unw0 = image + kRvaBoundaryUnwind;             // primary: flags 0
            unw0[0] = 0x01; unw0[1] = 0; unw0[2] = 0; unw0[3] = 0;
            unsigned char* unw1 = image + kRvaBoundaryUnwind + 0x10;      // fragment tail: UNW_FLAG_CHAININFO -> primary
            unw1[0] = 0x01 | (0x4 << 3); unw1[1] = 0; unw1[2] = 0; unw1[3] = 0;
            unsigned char* unw2 = image + kRvaBoundaryUnwind + 0x20;      // handler tail: must never be parsed as chain info
            unw2[0] = 0x01 | (0x1 << 3); unw2[1] = 0; unw2[2] = 0; unw2[3] = 0;
            unsigned char* unw3 = image + kRvaBoundaryUnwind + 0x30;      // cyclic chain tail
            unw3[0] = 0x01 | (0x4 << 3); unw3[1] = 0; unw3[2] = 0; unw3[3] = 0;
            size_t pdataCount = 1;
            rf[0].BeginAddress = fnBegin; rf[0].EndAddress = fnEnd; rf[0].UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind);
            if (boundaryUnwindMode == 1) {                                // fragment [0x2120,0x2130) containing the lea chains to the primary
                rf[1].BeginAddress = static_cast<DWORD>(kRvaBoundaryFn + 0x20);
                rf[1].EndAddress = static_cast<DWORD>(kRvaBoundaryFn + 0x30);
                rf[1].UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind + 0x10);
                RUNTIME_FUNCTION chain = {}; chain.BeginAddress = fnBegin; chain.EndAddress = fnEnd; chain.UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind);
                std::memcpy(unw1 + 4, &chain, sizeof chain);
                pdataCount = 2;
            } else if (boundaryUnwindMode == 2) {                          // EHANDLER payload that looks like a RUNTIME_FUNCTION
                RUNTIME_FUNCTION bogus = {}; bogus.BeginAddress = 0x1234; bogus.EndAddress = 0x1240; bogus.UnwindInfoAddress = 0x0088;
                std::memcpy(unw2 + 4, &bogus, sizeof bogus);
                rf[0].UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind + 0x20);
            } else if (boundaryUnwindMode == 4) {                          // 17 links: the 16-level budget must decline
                const uintptr_t uwBase = kRvaBoundaryUnwind + 0x40;
                rf[0].BeginAddress = static_cast<DWORD>(kRvaBoundaryFn + 0x20);   // fragment containing the lea/site
                rf[0].EndAddress = static_cast<DWORD>(kRvaBoundaryFn + 0x30);
                rf[0].UnwindInfoAddress = static_cast<DWORD>(uwBase);
                for (int i = 0; i < 18; i++) {   // 17 chained levels + one terminal record: the budget allows 16 follows
                    unsigned char* uw = image + uwBase + (size_t)i * 0x10;
                    const bool chained = (i < 17);
                    uw[0] = static_cast<unsigned char>(0x01 | (chained ? (0x4 << 3) : 0));
                    uw[1] = 0; uw[2] = 0; uw[3] = 0;
                    if (chained) {
                        RUNTIME_FUNCTION nx = {};
                        if (i >= 15) { nx.BeginAddress = static_cast<DWORD>(kRvaBoundaryFn); nx.EndAddress = static_cast<DWORD>(kRvaBoundaryFn + 0x60); }
                        else { nx.BeginAddress = 0x5000u + static_cast<DWORD>(i); nx.EndAddress = 0x5001u + static_cast<DWORD>(i); }
                        nx.UnwindInfoAddress = static_cast<DWORD>(uwBase + (size_t)(i + 1) * 0x10);
                        std::memcpy(uw + 4, &nx, sizeof nx);
                    }
                }
            } else if (boundaryUnwindMode == 3) {                          // chain pointing at itself: must fail closed
                RUNTIME_FUNCTION self = {}; self.BeginAddress = fnBegin; self.EndAddress = fnEnd; self.UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind + 0x30);
                std::memcpy(unw3 + 4, &self, sizeof self);
                rf[0].UnwindInfoAddress = static_cast<DWORD>(kRvaBoundaryUnwind + 0x30);
            }
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = static_cast<DWORD>(kRvaBoundaryPdata);
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = static_cast<DWORD>(pdataCount * sizeof(RUNTIME_FUNCTION));
            boundaryTarget = image + kRvaBoundaryFn;
            boundaryHooked = false;
        }

        cellFactory0 = iat + 0;
        cellFactory1 = iat + 1;
        cellFactory2 = iat + 2;
        cellResolver = iatK + 0;
        owners.clear();
        owners[reinterpret_cast<uintptr_t>(&ExportCreateFactory)] = dxgiModule;
        owners[reinterpret_cast<uintptr_t>(&ExportCreateFactory1)] = dxgiModule;
        owners[reinterpret_cast<uintptr_t>(&ExportCreateFactory2)] = dxgiModule;
        owners[reinterpret_cast<uintptr_t>(&NativeGetProcAddress)] = dxgiModule;
        owners[reinterpret_cast<uintptr_t>(&SlCreateFactory)] = slModule;
        owners[reinterpret_cast<uintptr_t>(&SlCreateFactory1)] = slModule;
        owners[reinterpret_cast<uintptr_t>(&SlCreateFactory2)] = slModule;
        owners[reinterpret_cast<uintptr_t>(&SlFactoryHwnd)] = slModule;
        owners[reinterpret_cast<uintptr_t>(&AltFactoryHwnd)] = slModule;
        owners[reinterpret_cast<uintptr_t>(&ForeignExport)] = foreignModule;
        // Exact method ownership must win over the synthetic caller provenance.
        for (void* target : {reinterpret_cast<void*>(&FactoryHwnd), reinterpret_cast<void*>(&FactoryCreate),
            reinterpret_cast<void*>(&ChainPresent), reinterpret_cast<void*>(&ChainPresent1),
            reinterpret_cast<void*>(&ChainResize), reinterpret_cast<void*>(&ChainResize1), reinterpret_cast<void*>(&ChainColor)})
            owners[reinterpret_cast<uintptr_t>(target)] = dxgiModule;
    }
    void BuildImage() { BuildImageSized(kImageSize, kImageSize); }

    // ---- draw sink + drain hook (the only GPU boundary the host substitutes) ----
    struct DrawEvent {
        void* chain; void* queue; uint64_t generation; DWORD thread;
        input::OwnershipPolicy policy;
        bool menuOpen, wantsMouse, wantsKeyboard, textInput, mouseOverUi;
    };
    std::vector<DrawEvent> draws;
    std::mutex drawMutex;
    bool (*drainFn)() = nullptr;
    void DrawSink(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, uint64_t generation);
    size_t DrawCount() { std::lock_guard<std::mutex> l(drawMutex); return draws.size(); }
    DrawEvent LastDraw() { std::lock_guard<std::mutex> l(drawMutex); return draws.empty() ? DrawEvent{} : draws.back(); }

    struct RebindCall { void* chain; UINT buffers; UINT w; UINT h; DXGI_FORMAT format; };
    std::vector<RebindCall> rebindCalls;
    bool rebindFails = false;
    bool RebindSeam(IDXGISwapChain3* chain, UINT buffers, UINT w, UINT h, DXGI_FORMAT format) {
        rebindCalls.push_back({ chain, buffers, w, h, format });
        return !rebindFails;
    }
    bool editorOpen = false, editorPlaying = false, editorPlacing = false, editorCameraMode = false, editorMouseMode = false;
    bool inputInitialized = false;
    std::atomic<unsigned> menuOpenedCalls{0}, menuClosedCalls{0};
    struct OwnershipProbe { input::OwnershipPolicy policy; unsigned calls = 0, openCalls = 0; };
    std::mutex ownershipMutex;
    OwnershipProbe ownershipProbe;
    OwnershipProbe OwnershipSnapshot() { std::lock_guard<std::mutex> lock(ownershipMutex); return ownershipProbe; }
    void ResetOwnershipProbe() { std::lock_guard<std::mutex> lock(ownershipMutex); ownershipProbe = {}; }
    void DrawSink(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, uint64_t generation) {
        const auto ownership = OwnershipSnapshot();
        std::lock_guard<std::mutex> l(drawMutex);
        draws.push_back({ chain, queue, generation, GetCurrentThreadId(), ownership.policy,
                          core::g_menuOpen, core::g_uiWantsMouse, core::g_uiWantsKeyboard,
                          core::g_uiTextInput, core::g_uiMouseOverUi });
    }
    size_t debugPointCount = 0;
    bool applyToggleTransition = false; // selected cases model the editor's open/close boundary
    int togglePollsObserved = 0;        // calls already sampled when the first editor side effect begins
    bool sharedHotkey = false;          // one immediate second query when both configured actions use one VK
    thread_local int inputScopeDepth = 0;
    long toggleCalls = 0;
    long cameraModeCalls = 0, togglePlayCalls = 0;   // upstream's preserved Home branch: which action the frame selected
    // ---- D1 admission model of the fixture's input stub (see namespace input below) ----
    // The real input layer is deliberately not linked into this suite, so the added frame's optional admission is
    // modelled here: the fixture arms deterministic refusals (the modelled foreign scope owner) and pending taps.
    // These knobs prove the OVERLAY-side contract only (skip = no poll/consume/render, admitted frame consumes a
    // pending tap once); the real latch/admission proof lives in the InputHotkeys/InputOwnership suites.
    std::atomic<int> frameScopeTryCalls{0}, frameScopeAdmitted{0}, frameScopeDeclined{0};
    int inputDenyNext = 0;                           // deterministic admission refusals armed by a case
    int hotkeyPressedCalls = 0;                      // configured-key polls reaching the stub
    int pendingToggleTaps = 0, pendingModeTaps = 0;   // pending event-first taps (one consume per pending action)
}

// ---------------------------------------------------------------------------------------------------------
// core / editor / thumbgen symbols the included production TU calls.
// ---------------------------------------------------------------------------------------------------------
void* CdHeapAlloc(size_t n) { return malloc(n ? n : 1); }
void* CdHeapRealloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void CdHeapFree(void* p) { free(p); }

namespace cdk { thread_local FaultInfo t_fault; }

namespace core {
void Log(const char* fmt, ...) {
    char buf[2048];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    fx::Capture(std::string(buf));
}
bool ReadBytes(uintptr_t a, void* out, size_t n) {         // in-memory PE fixture: plain bounded copy, no SEH needed
    if (!a || !out || !n) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), n);
    return true;
}
std::string ModDir() { return "C:\\omo-host-no-mod-dir"; }
uintptr_t g_base = 0;
bool g_menuOpen = false, g_uiWantsMouse = false, g_uiWantsKeyboard = false, g_placing = false;
bool g_uiTextInput = false, g_uiMouseOverUi = false;   // upstream main's finer input-capture flags (used by DrawFrame)
int g_keyToggle = VK_INSERT, g_keyMode = VK_HOME;
bool GameReadAvailable() { return false; }
void SetFreeCam(bool on) { fx::editorCameraMode=on; }
size_t DebugPointCount() { return fx::debugPointCount; }
// The host executable carries no ASI RCDATA; fonts/GPU init are outside this suite.
bool EmbeddedResource(int, const uint8_t** data, size_t* size) { *data=nullptr; *size=0; return false; }
}
namespace thumbgen {
int Generation() { return 0; }
std::vector<std::string> TakeRefreshed() { return {}; }
}
namespace input {
void Init(HWND) { fx::inputInitialized=true; }
void Shutdown() {}
void MenuOpened() { if(!fx::inputInitialized || fx::inputScopeDepth) throw std::runtime_error("menu open before init or under router scope"); ++fx::menuOpenedCalls; }
void MenuClosed() { if(fx::inputScopeDepth) throw std::runtime_error("menu close dispatch under router scope"); ++fx::menuClosedCalls; }
void FeedMouse(ImGuiIO&) {}
// Paused-candidate input API adaptation (Task 1). The real input.cpp is deliberately NOT linked into this
// fixture, so these are deterministic STUBS - they keep the real overlay frame compiling and running, and they
// are NOT input-behaviour proof (InputHotkeys/InputOwnership own real input proof; the plan says the same).
//   FrameScope()               : the real scope enters the recursive router section; the stub always admits.
//   FrameScope(std::try_to_lock_t): deterministic admission driven by fx::inputDenyNext (the modelled foreign
//                                scope owner). A case arms the refusals, so the skipped-frame path IS produced here
//                                and every D1 case below depends on it; the counters are the case's own evidence.
//   Release()                  : gives up this scope's own acquisition; OwnsLock() then reports false, exactly
//                                like the real contract (a released scope owns nothing).
//   HotkeyPressed()            : consumes the fixture's pending taps and shares one immediate query for a
//                                same-VK binding. Toggle observes the poll count before its side effect, proving
//                                overlay sampling order; real event delivery/latch correctness belongs to InputHotkeys.
//   AdmissionStats()           : zeroed counters; diagnostics/evidence only, never read by routing decisions.
FrameScope::FrameScope() : owns(true) { ++fx::inputScopeDepth; }
FrameScope::FrameScope(std::try_to_lock_t) : owns(fx::inputDenyNext <= 0) {
    ++fx::frameScopeTryCalls;
    if (owns) { ++fx::frameScopeAdmitted; ++fx::inputScopeDepth; } else { --fx::inputDenyNext; ++fx::frameScopeDeclined; }
}
FrameScope::~FrameScope() { Release(); }
void FrameScope::Release() noexcept { if(owns) --fx::inputScopeDepth; owns = false; }
bool HotkeyPressed(int vk, bool& wasDown) {
    ++fx::hotkeyPressedCalls;
    const bool shared = fx::sharedHotkey && vk == core::g_keyToggle && vk == core::g_keyMode;
    fx::sharedHotkey = false;
    int* pending = vk == core::g_keyToggle ? &fx::pendingToggleTaps : (vk == core::g_keyMode ? &fx::pendingModeTaps : nullptr);
    wasDown = pending && *pending > 0;
    if (shared) return true;
    if (wasDown) {
        --*pending;
        fx::sharedHotkey = core::g_keyToggle == core::g_keyMode;
        return true;
    }
    return false;
}
InputAdmission AdmissionStats() { return InputAdmission{}; }
// Record the actual policy requests made by production overlay code. This is an
// observation boundary, not a second router; real delivery remains InputOwnership's proof.
void PublishOwnership(const OwnershipPolicy& policy) {
    std::lock_guard<std::mutex> lock(fx::ownershipMutex);
    fx::ownershipProbe.policy = policy;
    ++fx::ownershipProbe.calls;
    if (policy.menuOpen) ++fx::ownershipProbe.openCalls;
}
}

namespace editor {
void Draw() {}
void Toggle() {
    fx::toggleCalls++;
    fx::togglePollsObserved = fx::hotkeyPressedCalls;
    if (fx::applyToggleTransition) fx::editorOpen = !fx::editorOpen;
}
bool IsOpen() { return fx::editorOpen; }
void ApplyStyle(float) {}
bool PlayMode() { return fx::editorPlaying; }
void TogglePlay() { fx::toggleCalls++; fx::togglePlayCalls++; }
void ToggleCameraMode() { fx::toggleCalls++; fx::cameraModeCalls++; }   // upstream main's Home behaviour (driven by the Home case below)
bool Placing() { return fx::editorPlacing; }
bool MouseMode() { return fx::editorMouseMode; }
}

// MinHook boundary (host variant): input.cpp is substituted above; this suite does not exercise
// its window-hook installation. Both production targets run through the contract MinHook provides ("any call to the patched
// address reaches the detour; call the original exactly once through *orig"):
//   * the game-side creation boundary, where the fixture patches a real jump at the modeled helper head;
//   * the legacy adapter's discovered DXGI function, where the modeled system function dispatches through the armed
//     detour - the path a MinHook patch produces for every vtable entry that holds the function address.
// The native variant (WB_NATIVE_MINHOOK_SMOKE) links the real MinHook sources instead of this shim.
#include "../../tools/minhook/include/MinHook.h"
namespace fx {
    struct HookModel { void* original = nullptr; void* detour = nullptr; bool enabled = false; };
    std::mutex hookMutex;
    std::map<void*, HookModel> hooks;
    std::map<void*, unsigned> hookAttempts;
    void* failCreateTarget = nullptr;
    void* failEnableTarget = nullptr;
    void* failRemoveTarget = nullptr;
    unsigned hookRemoves = 0, hookDisables = 0;
    void* Dispatch(void* target) {
#ifndef WB_NATIVE_MINHOOK_SMOKE
        std::lock_guard<std::mutex> lock(hookMutex);
        auto it = hooks.find(target);
        if (it != hooks.end() && it->second.enabled) return it->second.detour;
#endif
        return target;
    }
    bool Hooked(void* target) {
        std::lock_guard<std::mutex> lock(hookMutex);
        auto it = hooks.find(target);
        return it != hooks.end() && it->second.enabled;
    }
    unsigned Attempts(void* target) {
        std::lock_guard<std::mutex> lock(hookMutex);
        return hookAttempts[target];
    }
    void ResetHooks() {
        std::lock_guard<std::mutex> lock(hookMutex);
        hooks.clear(); hookAttempts.clear(); hookRemoves = hookDisables = 0;
        failCreateTarget = failEnableTarget = failRemoveTarget = nullptr;
    }
}
#ifndef WB_NATIVE_MINHOOK_SMOKE
MH_STATUS MH_CreateHook(void* target, void* detour, void** orig) {
    std::lock_guard<std::mutex> lock(fx::hookMutex);
    ++fx::hookAttempts[target];
    if (orig) *orig = nullptr;
    if (target == fx::failCreateTarget) return MH_ERROR_MEMORY_ALLOC;
    if (target == fx::boundaryTarget && fx::boundaryHookCreateStatus != MH_OK) return static_cast<MH_STATUS>(fx::boundaryHookCreateStatus);
    if (target == reinterpret_cast<void*>(&fx::FactoryHwnd) && fx::legacyHookCreateStatus != MH_OK) return static_cast<MH_STATUS>(fx::legacyHookCreateStatus);
    if (fx::hooks.count(target)) return MH_ERROR_ALREADY_CREATED;
    void* original = target == fx::boundaryTarget ? reinterpret_cast<void*>(&fx::BoundaryHelperImplModel) : target;
    fx::hooks[target] = { original, detour, false };
    if (orig) *orig = original;
    return MH_OK;
}
MH_STATUS MH_EnableHook(void* target) {
    if (target == fx::pauseCell) {
        std::unique_lock<std::mutex> gate(fx::gateMutex);
        fx::paused = true; fx::gateCv.notify_all();
        if (!fx::gateCv.wait_for(gate, std::chrono::seconds(20), [] { return fx::resume; })) {
            fx::gateTimeout = true; fx::paused = false; return MH_ERROR_DISABLED;
        }
        fx::paused = false;
    }
    std::lock_guard<std::mutex> lock(fx::hookMutex);
    auto it = fx::hooks.find(target);
    if (it == fx::hooks.end()) return MH_ERROR_NOT_CREATED;
    if (target == fx::failEnableTarget) return MH_ERROR_DISABLED;
    if (target == fx::boundaryTarget && fx::boundaryHookEnableStatus != MH_OK) return static_cast<MH_STATUS>(fx::boundaryHookEnableStatus);
    if (target == reinterpret_cast<void*>(&fx::FactoryHwnd) && fx::legacyHookEnableStatus != MH_OK) return static_cast<MH_STATUS>(fx::legacyHookEnableStatus);
    if (target == fx::boundaryTarget) {
        auto* p = static_cast<unsigned char*>(target);
        std::memcpy(fx::boundarySavedBytes, p, 16);
        DWORD old = 0;
        if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old)) return MH_ERROR_MEMORY_ALLOC;
        const uint64_t address = reinterpret_cast<uint64_t>(it->second.detour);
        p[0] = 0x48; p[1] = 0xb8; std::memcpy(p + 2, &address, 8); p[10] = 0xff; p[11] = 0xe0;
        DWORD ignored = 0;
        if (!VirtualProtect(p, 16, old, &ignored)) return MH_ERROR_MEMORY_PROTECT;
        FlushInstructionCache(GetCurrentProcess(), p, 16);
        fx::boundaryHooked = true;
    }
    it->second.enabled = true;
    return MH_OK;
}
MH_STATUS MH_DisableHook(void* target) {
    std::lock_guard<std::mutex> lock(fx::hookMutex);
    auto it = fx::hooks.find(target);
    if (it == fx::hooks.end()) return MH_ERROR_NOT_CREATED;
    it->second.enabled = false; ++fx::hookDisables;
    return MH_OK;
}
MH_STATUS MH_RemoveHook(void* target) {
    std::lock_guard<std::mutex> lock(fx::hookMutex);
    if (target == fx::failRemoveTarget) return MH_ERROR_MEMORY_PROTECT;
    if (!fx::hooks.erase(target)) return MH_ERROR_NOT_CREATED;
    ++fx::hookRemoves;
    return MH_OK;
}
#endif


#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#include "../../asi/cdmodkit/overlay.cpp"
// =========================================================================================================
// Fixture implementations, production call shims, harness and cases.
// =========================================================================================================
namespace fx {
    // Production call shims (defined below): declared here because the fixture's COM methods drive production slots.
    HRESULT GameCallHwnd(void** table, IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, IDXGISwapChain1** pp);
    HRESULT GamePresent(void** table, IDXGISwapChain* self, UINT sync, UINT flags);
    HRESULT GameCallQi(void** table, IUnknown* self, REFIID iid, void** pp);
    HRESULT GamePresent1(void** table, IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p);
    HRESULT GameResize(void** table, IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);
    HRESULT GameResize1(void** table, IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues);
    // Ordering gates used by the resize schedules (defined with the drain fixture below; declared here because the
    // fixture's own resize originals drive them).
    bool BlockResizeOriginal();
    bool WaitResizeEntered(unsigned boundMs);
    void ReleaseResizeOriginal();
    void ResetResizeGate();
    void CountIdentityQuery();
    bool WaitIdentityQueries(long count, unsigned boundMs);
    long IdentityQueryCount();
    HRESULT GameCallCreate(void** table, IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp);
    HRESULT GameCreateFactory0(REFIID iid, void** pp);
    HRESULT GameCreateFactory1(REFIID iid, void** pp);
    HRESULT GameCreateFactory2(UINT flags, REFIID iid, void** pp);
    FARPROC GameGetProcAddress(HMODULE mod, LPCSTR name);

    HRESULT STDMETHODCALLTYPE ObjQiRaw(IUnknown* self, REFIID iid, void** pp) {
        if (!pp) return E_POINTER;
        if (iid == IID_IUnknown) { *pp = self; ++addRefs; InterlockedIncrement(&reinterpret_cast<Obj*>(self)->refs); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE ObjAddRefRaw(IUnknown* self) { ++addRefs; return static_cast<ULONG>(InterlockedIncrement(&reinterpret_cast<Obj*>(self)->refs)); }
    ULONG STDMETHODCALLTYPE ObjReleaseRaw(IUnknown* self) {
        ++releases;
        const LONG n = InterlockedDecrement(&reinterpret_cast<Obj*>(self)->refs);
        if (n == 0) ++destroyed;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE ObjQi(IUnknown* self, REFIID iid, void** pp) {
        Obj* o = reinterpret_cast<Obj*>(self);
        if (!pp) return E_POINTER;
        *pp = nullptr;
        if (iid == IID_IUnknown && o->identityFail) return E_NOINTERFACE;   // null canonical id (mandatory-check fixture)
        if (iid == IID_IUnknown) {
            CountIdentityQuery();
            *pp = o->identity;
            ObjAddRefRaw(reinterpret_cast<IUnknown*>(o->identity));
            if (faultIdentityQi > 0) { faultIdentityQi--; RaiseException(0xC0000005, 0, 0, nullptr); }
            return S_OK;
        }
        for (int i = 0; i < o->aliasCount; i++) {
            if (iid == o->aliases[i].iid) { *pp = o->aliases[i].ptr; ObjAddRef(reinterpret_cast<IUnknown*>(o->aliases[i].ptr)); return S_OK; }
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE ObjAddRef(IUnknown* self) { ++addRefs; return static_cast<ULONG>(InterlockedIncrement(&reinterpret_cast<Obj*>(self)->refs)); }
    ULONG STDMETHODCALLTYPE ObjRelease(IUnknown* self) {
        ++releases;
        const LONG n = InterlockedDecrement(&reinterpret_cast<Obj*>(self)->refs);
        if (n == 0) ++destroyed;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE FactoryQi(IUnknown* self, REFIID iid, void** pp) { return ObjQi(self, iid, pp); }
    HRESULT STDMETHODCALLTYPE FactoryCreate(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
        (void)dev;
        Factory* f = reinterpret_cast<Factory*>(self);
        f->createCalls++;
        if (desc) desc->OutputWindow = f->chain ? f->chain->hwnd : nullptr;
        if (pp) { *pp = f->chain ? reinterpret_cast<IDXGISwapChain*>(f->chain) : nullptr; if (f->chain) f->chain->AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InnerFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        (void)dev; (void)fs; (void)out;
        Factory* f = reinterpret_cast<Factory*>(self);
        f->hwndCalls++;
        innerCreateCalls++;
        if (f->chain) { f->chain->hwnd = hwnd; if (d && d->Width > 64) f->chain->w = d->Width; if (d && d->Height > 64) f->chain->h = d->Height; }
        if (pp) { *pp = f->chain ? reinterpret_cast<IDXGISwapChain1*>(f->chain) : nullptr; if (f->chain) f->chain->AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        return SystemHwndBody(self, dev, hwnd, d, fs, out, pp);
    }
    // The modeled system implementation: what the DXGI function itself does.
    HRESULT STDMETHODCALLTYPE SystemHwndBody(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        (void)dev; (void)fs; (void)out;
        Factory* f = reinterpret_cast<Factory*>(self);
        f->hwndCalls++;
        if (f->hwndHr != S_OK) { if (pp) *pp = reinterpret_cast<IDXGISwapChain1*>(static_cast<uintptr_t>(0x1)); return f->hwndHr; }
        if (f->hwndNullOutput) { if (pp) *pp = nullptr; return S_OK; }
        if (f->chain) {
            f->chain->hwnd = hwnd;
            if (d && d->Width > 64) f->chain->w = d->Width;
            if (d && d->Height > 64) f->chain->h = d->Height;
            if (d && d->Format != DXGI_FORMAT_UNKNOWN) f->chain->fmt = d->Format;
            if (d && d->BufferCount) f->chain->buffers = d->BufferCount;
        }
        lastReturnedChain = f->chain;
        if (pp) { *pp = f->chain ? reinterpret_cast<IDXGISwapChain1*>(f->chain) : nullptr; if (f->chain) f->chain->AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AltFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        (void)dev; (void)fs; (void)out; (void)d;
        Factory* f = reinterpret_cast<Factory*>(self);
        f->hwndCalls++;
        Chain* c = NewChain(hwnd, f->outerQueue ? f->outerQueue->dev : nullptr, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        f->chain = c;
        lastReturnedChain = c;
        if (pp) { *pp = reinterpret_cast<IDXGISwapChain1*>(c); c->AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SlFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        Factory* f = reinterpret_cast<Factory*>(self);
        if (f->innerFactory && f->innerQueue) {                        // wrapper work: a nested creation with another queue
            DXGI_SWAP_CHAIN_DESC1 innerDesc = d ? *d : DXGI_SWAP_CHAIN_DESC1{};
            IDXGISwapChain1* inner = nullptr;
            GameCallHwnd(f->innerFactory->vt, reinterpret_cast<IDXGIFactory2*>(f->innerFactory), reinterpret_cast<IUnknown*>(f->innerQueue), hwnd, &innerDesc, &inner);
            if (inner) inner->Release();
        }
        return reinterpret_cast<HwndFn>(fx::Dispatch(reinterpret_cast<void*>(outerHwndImpl)))(self, dev, hwnd, d, fs, out, pp);
    }
    // Set the nested ("inner") creation of the wrapper to go through this factory table with this queue, which is
    // how a wrapper reaches the native factory it wrapped: the inner call then passes through an observed table.
    void SetWrapperInner(Factory* innerFactory, Queue* innerQueue);
    bool PatchLockIsFree() {
        const bool free = overlay::g_functionHookMutex.try_lock();
        if (free) overlay::g_functionHookMutex.unlock();
        else comUnderPatchLock.store(1);
        return free;
    }
    HRESULT STDMETHODCALLTYPE ChainQi(IUnknown* self, REFIID iid, void** pp) {
        PatchLockIsFree();
        if (chain3QiFails && (iid == __uuidof(IDXGISwapChain3) || iid == __uuidof(IDXGISwapChain2))) { if (pp) *pp = nullptr; return E_NOINTERFACE; }
        const HRESULT hr = ObjQi(self, iid, pp);
        if (hr == S_OK && iid == __uuidof(IDXGISwapChain3)) {
            if (chain3QiBlockCount > 0) { chain3QiBlockCount--; BlockChain3Qi(); }   // ordering gate: qualification done, render lock not yet taken
            if (faultChain3Qi > 0) { faultChain3Qi--; RaiseException(0xC0000005, 0, 0, nullptr); }   // output + reference already written
        }
        StageGate();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ChainGetDevice(IDXGISwapChain* self, REFIID iid, void** pp) {
        Chain* c = reinterpret_cast<Chain*>(self);
        if (c->deviceHr != S_OK) return c->deviceHr;
        if (iid == __uuidof(ID3D12Device)) { if (pp) { *pp = c->dev; c->dev->AddRef(); } StageGate(); return S_OK; }
        if (pp) *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ChainPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
        Chain* c = reinterpret_cast<Chain*>(self);
        c->presents++;
        if (c->wrapperInner) return GamePresent(c->wrapperInner->vt, reinterpret_cast<IDXGISwapChain*>(c->wrapperInner), sync, flags);
        return c->presentHr;
    }
    HRESULT STDMETHODCALLTYPE ChainPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
        (void)p;
        Chain* c = reinterpret_cast<Chain*>(self);
        c->presents1++;
        if (c->present1CallsPresent) return GamePresent(c->vt, reinterpret_cast<IDXGISwapChain*>(c), sync, flags);
        return c->presentHr;
    }
    HRESULT STDMETHODCALLTYPE ChainResize(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        Chain* c = reinterpret_cast<Chain*>(self);
        c->resizes++; c->lastResizeCount = n; c->lastWidth = w; c->lastHeight = h; c->lastFormat = fmt; c->lastFlags = flags;
        if (c->resizeBlockCount > 0) { c->resizeBlockCount--; BlockResizeOriginal(); }
        if (c->nestedResizeChain) {                      // A.original -> B.resize on another chain: two independent transactions
            Chain* inner = c->nestedResizeChain;
            c->nestedResizeChain = nullptr;
            GameResize(inner->vt, reinterpret_cast<IDXGISwapChain*>(inner), 0, w, h, fmt, flags);
            c->nestedResizeChain = inner;
        }
        return c->resizeHr;
    }
    HRESULT STDMETHODCALLTYPE ChainResize1(IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues) {
        Chain* c = reinterpret_cast<Chain*>(self);
        c->resizes1++;
        if (c->resizeBlockCount > 0) { c->resizeBlockCount--; BlockResizeOriginal(); }
        if (c->resizeCallsNested) {                      // the original re-enters the patched slot: a nested transaction on the same cookie
            c->resizeCallsNested = false;
            GameResize1(c->vt, reinterpret_cast<IDXGISwapChain3*>(c), n, w, h, fmt, flags, nodes, queues);
            c->resizeCallsNested = true;
            return c->resize1Hr;
        } c->lastResizeCount = n; c->lastWidth = w; c->lastHeight = h; c->lastFormat = fmt; c->lastFlags = flags;
        if (FAILED(c->resize1Hr)) return c->resize1Hr; // failing original does not inspect replacement array
        const UINT keep = (std::min)(n, 8u);                                         // bound by the call's own entry count
        for (UINT i = 0; i < 8; i++) {
            c->lastNodes[i] = (nodes && i < keep) ? nodes[i] : 0;
            c->lastQueues[i] = (queues && i < keep) ? queues[i] : nullptr;
        }
        return c->resize1Hr;
    }
    HRESULT STDMETHODCALLTYPE ChainGetDesc(IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* out) {
        Chain* c = reinterpret_cast<Chain*>(self);
        chainDescCalls++;
        PatchLockIsFree();
        if (armAllocFailOnDesc) { armAllocFailOnDesc = false; ArmNextAllocFailure(); }
        if (c->descHr != S_OK) return c->descHr;
        if (!out) return E_POINTER;
        *out = DXGI_SWAP_CHAIN_DESC{};
        out->BufferCount = c->buffers;
        out->BufferDesc.Width = c->w;
        out->BufferDesc.Height = c->h;
        out->BufferDesc.Format = c->fmt;
        out->OutputWindow = c->hwnd;
        out->SampleDesc.Count = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ChainGetHwnd(IDXGISwapChain1* self, HWND* out) { if (!out) return E_POINTER; *out = reinterpret_cast<Chain*>(self)->hwnd; return S_OK; }
    UINT STDMETHODCALLTYPE ChainIndex(IDXGISwapChain3* self) { return reinterpret_cast<Chain*>(self)->currentIndex; }
    HRESULT STDMETHODCALLTYPE ChainColor(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE) { return S_OK; }
    HRESULT STDMETHODCALLTYPE ChainGetBuffer(IDXGISwapChain* self, UINT idx, REFIID iid, void** pp) {
        (void)idx; (void)iid;
        reinterpret_cast<Chain*>(self)->getBufferCalls++;
        if (pp) *pp = nullptr;
        return E_FAIL;
    }
    void SetForeignEntry(Chain* c, IUnknown* foreign) {
        c->cookie = foreign;
        if (foreign) ObjAddRef(foreign);
        c->cookieGuid = kWbCookieGuidProbe;
        c->hasCookie = true;
        c->cookieIsForeign = true;
    }
    HRESULT STDMETHODCALLTYPE ChainSetPrivateDataInterface(IDXGIObject* self, REFGUID g, const IUnknown* p) {
        Chain* c = reinterpret_cast<Chain*>(self);
        PatchLockIsFree();
        if (c->cookieHr != S_OK) return c->cookieHr;
        if (c->hasCookie && c->cookie == p) return S_OK;
        if (c->hasCookie) c->cookie->Release();
        c->cookie = const_cast<IUnknown*>(p);
        c->cookieGuid = g;
        c->hasCookie = c->cookie != nullptr;
        if (c->cookie) c->cookie->AddRef();
        StageGate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ChainGetPrivateData(IDXGIObject* self, REFGUID g, UINT* size, void* data) {
        Chain* c = reinterpret_cast<Chain*>(self);
        Chain* src = c->forwardCookieTo ? c->forwardCookieTo : c;
        if (foreignBlobMode >= 0) {                      // a foreign non-interface entry exists under our GUID
            const UINT need = foreignBlobMode == 0 ? 0u : (foreignBlobMode == 1 ? (UINT)sizeof(IUnknown*) : 4096u);
            if (data) payloadReads++;
            if (!data) { if (size) *size = need; return S_OK; }
            if (!size || *size < need) { if (size) *size = need; return DXGI_ERROR_MORE_DATA; }
            return S_OK;
        }
        if (!src->hasCookie || !IsEqualGUID(src->cookieGuid, g)) return DXGI_ERROR_NOT_FOUND;   // no entry under this GUID
        const UINT need = sizeof(IUnknown*);
        if (!data) { if (size) *size = need; return S_OK; }        // existence/size probe: no payload is read
        if (!size || *size < need) { if (size) *size = need; return DXGI_ERROR_MORE_DATA; }
        *reinterpret_cast<IUnknown**>(data) = src->cookie;
        src->cookie->AddRef();
        *size = need;
        return S_OK;
    }
    void STDMETHODCALLTYPE QueueGetDescShim(ID3D12CommandQueue* self, D3D12_COMMAND_QUEUE_DESC* out) {
        queueDescCalls++;
        if (self) {                                          // inside the render boundary
            const bool free = PatchLockIsFree();
            patchFreeDuringRender.store(free ? 1 : 0);
        }
        if (out) *out = reinterpret_cast<Queue*>(self)->desc;
    }
    HRESULT STDMETHODCALLTYPE QueueGetDevice(ID3D12DeviceChild* self, REFIID iid, void** pp) {
        Queue* q = reinterpret_cast<Queue*>(self);
        if (iid == __uuidof(ID3D12Device)) { if (pp) { *pp = q->dev; q->dev->AddRef(); } StageGate(); return S_OK; }
        if (pp) *pp = nullptr;
        return E_NOINTERFACE;
    }
    UINT STDMETHODCALLTYPE DeviceGetNodeCount(ID3D12Device* self) { deviceNodeCalls++; return reinterpret_cast<Device*>(self)->nodes; }

    // ---- foreign-module wrapper factory model (the live stack hands the game a ReShade-wrapped factory) --------
    // The wrapper's vtable is a heap clone owned by the wrapper module; rewriting its entries models a new wrapper
    // generation at the same reused table address, which is what wipes an interposition.
    void** wrapperClone = nullptr;
    std::vector<uintptr_t> wrapperCloneSpan;
    HRESULT STDMETHODCALLTYPE WrapperFactoryQi(IUnknown* self, REFIID iid, void** pp);
    HRESULT STDMETHODCALLTYPE WrapperFactoryCreate(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp);
    HRESULT STDMETHODCALLTYPE WrapperFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp);
    void** BuildWrapperFactory(HMODULE owner) {
        if (!wrapperClone) {
            wrapperClone = static_cast<void**>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!wrapperClone) throw std::runtime_error("wrapper table allocation failed");
        }
        bool spanned = false;
        for (const auto& s : moduleSpans) if (s.lo == reinterpret_cast<uintptr_t>(wrapperClone)) spanned = true;
        if (!spanned) moduleSpans.push_back({ reinterpret_cast<uintptr_t>(wrapperClone), reinterpret_cast<uintptr_t>(wrapperClone) + 0x1000, owner });
        for (int i = 0; i < kSlots; i++) wrapperClone[i] = reinterpret_cast<void*>(&StubSlot<33>);
        wrapperClone[0] = reinterpret_cast<void*>(&WrapperFactoryQi);
        wrapperClone[1] = reinterpret_cast<void*>(&ObjAddRef);
        wrapperClone[2] = reinterpret_cast<void*>(&ObjRelease);
        wrapperClone[10] = reinterpret_cast<void*>(&WrapperFactoryCreate);
        wrapperClone[15] = reinterpret_cast<void*>(&WrapperFactoryHwnd);
        owners[reinterpret_cast<uintptr_t>(&WrapperFactoryQi)] = owner;
        owners[reinterpret_cast<uintptr_t>(&WrapperFactoryCreate)] = owner;
        owners[reinterpret_cast<uintptr_t>(&WrapperFactoryHwnd)] = owner;
        return wrapperClone;
    }
    void RewrapGeneration() {   // a new wrapper generation rewrites the same table memory, reverting any interposition
        wrapperClone[0] = reinterpret_cast<void*>(&WrapperFactoryQi);
        wrapperClone[10] = reinterpret_cast<void*>(&WrapperFactoryCreate);
        wrapperClone[15] = reinterpret_cast<void*>(&WrapperFactoryHwnd);
    }
    HRESULT STDMETHODCALLTYPE WrapperFactoryQi(IUnknown* self, REFIID iid, void** pp) { return ObjQi(self, iid, pp); }
    HRESULT STDMETHODCALLTYPE WrapperFactoryCreate(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp) {
        Factory* f = reinterpret_cast<Factory*>(self);
        Factory* inner = f->innerFactory;
        if (!inner) { if (pp) *pp = nullptr; return E_FAIL; }
        return reinterpret_cast<overlay::FactoryCreateSwapChain_t>(fx::Dispatch(inner->vt[10]))(reinterpret_cast<IDXGIFactory*>(inner), dev, d, pp);
    }
    HRESULT STDMETHODCALLTYPE WrapperFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        Factory* f = reinterpret_cast<Factory*>(self);
        Factory* inner = f->innerFactory;
        if (!inner) { if (pp) *pp = nullptr; return E_FAIL; }
        return reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(fx::Dispatch(inner->vt[15]))(reinterpret_cast<IDXGIFactory2*>(inner), dev, hwnd, d, fs, out, pp);
    }

    // Precedence model: the modeled helper may perform earlier (inner) creations in the same invocation - through an
    // observed table (the existing factory route) and through a pre-existing table (the legacy route) - before its
    // final call. The final call uses boundaryFinalFactory when set, otherwise the host's own factory member.
    Factory* boundaryInnerFactory = nullptr; Queue* boundaryInnerQueue = nullptr;
    Factory* boundaryRawFactory = nullptr; Queue* boundaryRawQueue = nullptr;
    Factory* boundaryFinalFactory = nullptr; Queue* boundaryFinalQueue = nullptr;
    // The model body the trampoline returns to: reads the game's own members, calls the wrapper factory's slot 15
    // exactly like the inspected build, and stores the canonical-IDXGISwapChain4 QI of the created wrapper swapchain
    // in this+0xa8 (the game keeps that reference).
    void* BoundaryHelperImplModel(void* self) {
        if (!self) return reinterpret_cast<void*>(static_cast<intptr_t>(E_FAIL));
        CallerScope callerScope(mainModule);                 // the modeled helper lives in the game image: its calls are game calls
        unsigned char* host = static_cast<unsigned char*>(self);
        void* ctx = nullptr; std::memcpy(&ctx, host + 0x40, sizeof ctx);
        void* qctx = nullptr; std::memcpy(&qctx, host + 0x38, sizeof qctx);
        void* factory = nullptr; if (ctx) std::memcpy(&factory, static_cast<unsigned char*>(ctx) + 0x940, sizeof factory);
        void* queue = nullptr; if (qctx) std::memcpy(&queue, static_cast<unsigned char*>(qctx) + 0x3e8, sizeof queue);
        HWND hwnd = nullptr; std::memcpy(&hwnd, host + 0x8, sizeof hwnd);
        UINT w = 0, h = 0, flags = 0;
        std::memcpy(&w, host + 0x18, 4); std::memcpy(&h, host + 0x1c, 4); std::memcpy(&flags, host + 0xa0, 4);
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; if (ctx) std::memcpy(&fmt, static_cast<unsigned char*>(ctx) + 0x9fc, 4);
        if (!factory) return reinterpret_cast<void*>(static_cast<intptr_t>(E_FAIL));
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = w; d.Height = h; d.Format = fmt; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 3; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.Flags = flags;
        if (boundaryInnerFactory) {                           // the game's earlier creation through a table it already uses
            IDXGISwapChain1* inner = nullptr;
            void** ivt = nullptr;
            std::memcpy(&ivt, boundaryInnerFactory, sizeof ivt);
            if (ivt && ivt[15]) reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(fx::Dispatch(ivt[15]))(
                reinterpret_cast<IDXGIFactory2*>(boundaryInnerFactory), reinterpret_cast<IUnknown*>(boundaryInnerQueue ? boundaryInnerQueue : queue), hwnd, &d, nullptr, nullptr, &inner);
            if (inner) inner->Release();                      // the game keeps no reference to the earlier chain
        }
        if (boundaryRawFactory) {                             // and one through a pre-existing table (never observed)
            IDXGISwapChain1* inner = nullptr;
            void** ivt = nullptr;
            std::memcpy(&ivt, boundaryRawFactory, sizeof ivt);
            if (ivt && ivt[15]) reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(fx::Dispatch(ivt[15]))(
                reinterpret_cast<IDXGIFactory2*>(boundaryRawFactory), reinterpret_cast<IUnknown*>(boundaryRawQueue ? boundaryRawQueue : queue), hwnd, &d, nullptr, nullptr, &inner);
            if (inner) inner->Release();
        }
        void* finalFactory = boundaryFinalFactory ? static_cast<void*>(boundaryFinalFactory) : factory;
        IUnknown* finalQueue = reinterpret_cast<IUnknown*>(boundaryFinalFactory && boundaryFinalQueue ? boundaryFinalQueue : queue);
        IDXGISwapChain1* out = nullptr;
        void** vt = nullptr;
        std::memcpy(&vt, finalFactory, sizeof vt);            // the object's vptr, then slot 15 (the inspected dispatch shape)
        if (!vt || !vt[15]) return reinterpret_cast<void*>(static_cast<intptr_t>(E_FAIL));
        const HRESULT hr = reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(fx::Dispatch(vt[15]))(
            reinterpret_cast<IDXGIFactory2*>(finalFactory), finalQueue, hwnd, &d, nullptr, nullptr, &out);
        if (FAILED(hr) || !out) { if (out) out->Release(); return reinterpret_cast<void*>(static_cast<intptr_t>(hr)); }
        IUnknown* c4 = nullptr;   // IDXGISwapChain4 is declared only from dxgi1_5.h on; the QI result is stored as IUnknown
        const HRESULT qi = out->QueryInterface(kIidSwapChain4Canonical, reinterpret_cast<void**>(&c4));
        out->Release();
        if (SUCCEEDED(qi) && c4) std::memcpy(host + 0xa8, &c4, sizeof c4);
        return reinterpret_cast<void*>(static_cast<intptr_t>(SUCCEEDED(qi) && c4 ? S_OK : E_NOINTERFACE));
    }
    // Allocates the modeled game object with the inspected member offsets (this+0x38 -> queue ctx, this+0x40 -> DXGI
    // ctx with the wrapper factory at +0x940 and the format at +0x9fc, this+8 hwnd, this+0x18/0x1c size, this+0xa0 flags).
    void* BuildBoundaryHost(void* wrapperFactory, void* queue, HWND hwnd, UINT w, UINT h, UINT flags) {
        unsigned char* host = static_cast<unsigned char*>(calloc(1, 0x1000));
        unsigned char* qctx = static_cast<unsigned char*>(calloc(1, 0x800));
        unsigned char* dctx = static_cast<unsigned char*>(calloc(1, 0x1000));
        if (!host || !qctx || !dctx) std::abort();
        *reinterpret_cast<void**>(host + 0x38) = qctx;
        *reinterpret_cast<void**>(qctx + 0x3e8) = queue;
        *reinterpret_cast<void**>(host + 0x40) = dctx;
        *reinterpret_cast<void**>(dctx + 0x940) = wrapperFactory;
        *reinterpret_cast<DXGI_FORMAT*>(dctx + 0x9fc) = DXGI_FORMAT_R8G8B8A8_UNORM;
        *reinterpret_cast<HWND*>(host + 0x8) = hwnd;
        *reinterpret_cast<UINT*>(host + 0x18) = w;
        *reinterpret_cast<UINT*>(host + 0x1c) = h;
        *reinterpret_cast<UINT*>(host + 0xa0) = flags;
        boundaryHost = host;
        return host;
    }
    void* RunBoundaryHelper(void* host) {   // the game calls its own helper through the (patched) head
        if (!boundaryTarget) return nullptr;
        void* r = reinterpret_cast<void* (*)(void*)>(boundaryTarget)(host);
        return r;
    }

    struct World {
        Device* dxgiDev = nullptr; Queue* dxgiQueue = nullptr; Chain* dxgiChain = nullptr; Factory* dxgiFactory[3] = {};
        Factory* probeFactory = nullptr;   // the dummy factory the legacy discovery seam returns (never a game factory)
        Device* slDev = nullptr; Queue* slQueue = nullptr; Chain* slChain = nullptr;
        Device* slDev2 = nullptr; Queue* slQueue2 = nullptr;
        Device* slInnerDev = nullptr; Queue* slInnerQueue = nullptr; Chain* slInnerChain = nullptr; Factory* slInnerFactory = nullptr;
        Factory* slFactory = nullptr;
        Factory* aliasFactory = nullptr; Chain* aliasChain = nullptr;
    };
    World world;
    void SetWrapperInner(Factory* innerFactory, Queue* innerQueue) {
        world.slFactory->innerFactory = innerFactory;
        world.slFactory->innerQueue = innerQueue;
    }
    void BuildWorld() {
        world = World();
        world.dxgiDev = NewDevice(1);
        world.dxgiQueue = NewQueue(world.dxgiDev, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
        world.dxgiChain = NewChain(nullptr, world.dxgiDev, 1280, 720, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
        for (int i = 0; i < 3; i++) world.dxgiFactory[i] = NewFactory(world.dxgiChain, world.dxgiQueue);
        world.probeFactory = NewFactory(world.dxgiChain, world.dxgiQueue);   // a factory of the same class, reserved for discovery
        world.slDev = NewDevice(1);
        world.slQueue = NewQueue(world.slDev, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
        world.slDev2 = NewDevice(1);
        world.slQueue2 = NewQueue(world.slDev2, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
        world.slInnerDev = NewDevice(1);
        world.slInnerQueue = NewQueue(world.slInnerDev, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
        world.slInnerChain = NewChain(nullptr, world.slInnerDev, 1920, 1080, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        world.slInnerFactory = NewFactory(world.slInnerChain, world.slInnerQueue);
        world.slInnerFactory->vt[15] = reinterpret_cast<void*>(&InnerFactoryHwnd);
        world.slChain = NewChain(nullptr, world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        world.slFactory = NewFactory(world.slChain, world.slQueue);
        world.slFactory->vt[15] = reinterpret_cast<void*>(&SlFactoryHwnd);
        world.slFactory->innerFactory = world.slInnerFactory;
        world.slFactory->innerChain = world.slInnerChain;
        world.slFactory->innerQueue = world.slInnerQueue;
        world.aliasChain = NewChain(nullptr, world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        world.aliasChain->identity = world.slChain->identity;   // an interface alias is the same DXGI object (same canonical IUnknown)
        world.aliasFactory = NewAliasFactory(world.slFactory, reinterpret_cast<void*>(&SlFactoryHwnd));
        outerHwndImpl = &FactoryHwnd;
    }
    // Failure leaves the caller's output untouched, like the real exports do.
    HRESULT WINAPI ExportCreateFactory(REFIID iid, void** pp) {
        (void)iid;
        directCalls[0]++;
        if (directHr[0] != S_OK) return directHr[0];
        if (directNullOutput[0]) { if (pp) *pp = nullptr; return S_OK; }
        if (pp) { *pp = world.dxgiFactory[0]; world.dxgiFactory[0]->AddRef(); }
        lastDirectFactory = world.dxgiFactory[0];
        return S_OK;
    }
    HRESULT WINAPI ExportCreateFactory1(REFIID iid, void** pp) {
        (void)iid;
        directCalls[1]++;
        if (directHr[1] != S_OK) return directHr[1];
        if (pp) { *pp = world.dxgiFactory[1]; world.dxgiFactory[1]->AddRef(); }
        lastDirectFactory = world.dxgiFactory[1];
        return S_OK;
    }
    HRESULT WINAPI ExportCreateFactory2(UINT flags, REFIID iid, void** pp) {
        (void)flags; (void)iid;
        directCalls[2]++;
        if (directHr[2] != S_OK) return directHr[2];
        if (pp) { *pp = world.dxgiFactory[2]; world.dxgiFactory[2]->AddRef(); }
        lastDirectFactory = world.dxgiFactory[2];
        return S_OK;
    }
    HRESULT WINAPI SlCreateFactory(REFIID iid, void** pp) {
        (void)iid;
        slCreateCalls++;
        if (slCreateFail) { if (pp) *pp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1)); return slCreateHr; }
        if (pp) { *pp = world.slFactory; world.slFactory->AddRef(); }
        lastSlFactory = world.slFactory;
        return S_OK;
    }
    HRESULT WINAPI SlCreateFactory1(REFIID iid, void** pp) { return SlCreateFactory(iid, pp); }
    HRESULT WINAPI SlCreateFactory2(UINT flags, REFIID iid, void** pp) { (void)flags; return SlCreateFactory(iid, pp); }
    intptr_t WINAPI ForeignExport() { return 0x5A5A; }
    // The dummy factory the legacy adapter discovers its target through: stands for the system CreateDXGIFactory
    // export. Production creates it and releases it inside an ObserverScope; a failure here declines only that adapter.
    long dummyFactoryCalls = 0;
    HRESULT dummyFactoryHr = S_OK;
    HRESULT WINAPI OsCreateDummyFactory(REFIID iid, void** pp) {
        (void)iid;
        dummyFactoryCalls++;
        if (pp) *pp = nullptr;
        if (dummyFactoryHr != S_OK) return dummyFactoryHr;
        if (!world.probeFactory) return E_FAIL;
        if (pp) { *pp = world.probeFactory; world.probeFactory->AddRef(); }
        return S_OK;
    }
    FARPROC WINAPI NativeGetProcAddress(HMODULE mod, LPCSTR name) {
        resolveCalls++;
        if (!name) { SetLastError(0x9u); return nullptr; }
        if (HIWORD(reinterpret_cast<uintptr_t>(name)) == 0) { SetLastError(0x5151u); return nullptr; }
        if (resolverFails) { SetLastError(0x2A2Au); return nullptr; }
        if (mod == slModule) {
            if (std::strcmp(name, "CreateDXGIFactory2") == 0) return reinterpret_cast<FARPROC>(&SlCreateFactory2);
            if (std::strcmp(name, "CreateDXGIFactory1") == 0) return reinterpret_cast<FARPROC>(&SlCreateFactory1);
            if (std::strcmp(name, "CreateDXGIFactory") == 0) return reinterpret_cast<FARPROC>(&SlCreateFactory);
        }
        if (mod == dxgiModule) {
            if (std::strcmp(name, "CreateDXGIFactory2") == 0) return reinterpret_cast<FARPROC>(&ExportCreateFactory2);
            if (std::strcmp(name, "CreateDXGIFactory1") == 0) return reinterpret_cast<FARPROC>(&ExportCreateFactory1);
            if (std::strcmp(name, "CreateDXGIFactory") == 0) return reinterpret_cast<FARPROC>(&ExportCreateFactory);
        }
        if (mod == foreignModule || resolverForeign) return reinterpret_cast<FARPROC>(&ForeignExport);
        SetLastError(0x33u);
        return nullptr;
    }
    bool DrainHook() { return drainFn ? drainFn() : true; }
    bool TimeoutDrainHook() { return false; }
    bool FaultDrainHook() { RaiseException(0xC0000005, 0, 0, nullptr); return false; }   // entered-ticket fault at the GPU boundary
    std::mutex drainMutex; std::condition_variable drainCv; bool drainEntered = false, drainReleaseFlag = false, drainTimeout = false;
    unsigned drainWaitBoundMs = 10000;                       // the drain gate's own bound; a timeout case deliberately shortens it
    // Resize-original gate: pauses the fixture's downstream resize body so a schedule can be ordered exactly (which
    // thread holds which state at which moment), never to simulate timing luck.
    std::mutex resizeMutex; std::condition_variable resizeCv;
    bool resizeEntered = false, resizeReleaseFlag = false, resizeGateTimeout = false;
    unsigned resizeWaitBoundMs = 10000;
    bool BlockResizeOriginal() {
        std::unique_lock<std::mutex> l(resizeMutex);
        resizeEntered = true;
        resizeCv.notify_all();
        const bool released = resizeCv.wait_for(l, std::chrono::milliseconds(resizeWaitBoundMs), [] { return resizeReleaseFlag; });
        if (!released) resizeGateTimeout = true;
        return released;
    }
    bool WaitResizeEntered(unsigned boundMs) {
        std::unique_lock<std::mutex> l(resizeMutex);
        const bool entered = resizeCv.wait_for(l, std::chrono::milliseconds(boundMs), [] { return resizeEntered; });
        if (!entered) resizeGateTimeout = true;
        return entered;
    }
    void ReleaseResizeOriginal() { std::lock_guard<std::mutex> l(resizeMutex); resizeReleaseFlag = true; resizeCv.notify_all(); }
    void ResetResizeGate() { std::lock_guard<std::mutex> l(resizeMutex); resizeEntered = false; resizeReleaseFlag = false; resizeGateTimeout = false; }
    // Identity-qualification gate: production-visible evidence that a resize reached its Begin (the canonical
    // identity query runs there before the render mutex is taken).
    std::mutex lookupMutex; std::condition_variable lookupCv; long identityQueries = 0;
    void CountIdentityQuery() { std::lock_guard<std::mutex> l(lookupMutex); ++identityQueries; lookupCv.notify_all(); }
    bool WaitIdentityQueries(long count, unsigned boundMs) {
        std::unique_lock<std::mutex> l(lookupMutex);
        return lookupCv.wait_for(l, std::chrono::milliseconds(boundMs), [&] { return identityQueries >= count; });
    }
    long IdentityQueryCount() { std::lock_guard<std::mutex> l(lookupMutex); return identityQueries; }
    bool BlockingDrainHook() {
        std::unique_lock<std::mutex> l(drainMutex);
        drainEntered = true;
        drainCv.notify_all();
        const bool released = drainCv.wait_for(l, std::chrono::milliseconds(drainWaitBoundMs), [] { return drainReleaseFlag; });
        if (!released) drainTimeout = true;                  // an unreleased gate is a bounded failure, reported here ...
        return released;                                     // ... and returned to production, never a silent success
    }
    bool WaitDrainEntered(unsigned boundMs) {
        std::unique_lock<std::mutex> l(drainMutex);
        const bool entered = drainCv.wait_for(l, std::chrono::milliseconds(boundMs), [] { return drainEntered; });
        if (!entered) drainTimeout = true;                       // reported, so a missing signal fails instead of passing
        return entered;
    }
    void ReleaseDrain() { std::lock_guard<std::mutex> l(drainMutex); drainReleaseFlag = true; drainCv.notify_all(); }
    void ResetDrainState() { std::lock_guard<std::mutex> l(drainMutex); drainEntered = false; drainReleaseFlag = false; drainTimeout = false; }

    // ---- call shims: model function-head dispatch without changing the COM table;
    //      caller provenance below is fixture module/pin bookkeeping, not a production caller filter. ----
    HRESULT GameCreateFactory0(REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::CreateDXGIFactory_t>(fx::Dispatch(*cellFactory0))(iid, pp); }
    HRESULT GameCreateFactory1(REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::CreateDXGIFactory_t>(fx::Dispatch(*cellFactory1))(iid, pp); }
    HRESULT GameCreateFactory2(UINT flags, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::CreateDXGIFactory2_t>(fx::Dispatch(*cellFactory2))(flags, iid, pp); }
    FARPROC GameGetProcAddress(HMODULE mod, LPCSTR name) { CallerScope scope(mainModule); return reinterpret_cast<decltype(&GetProcAddress)>(*cellResolver)(mod, name); }
    HRESULT GameCallFnA(void* fn, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::CreateDXGIFactory_t>(fx::Dispatch(fn))(iid, pp); }
    HRESULT GameCallFnC(void* fn, UINT flags, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::CreateDXGIFactory2_t>(fx::Dispatch(fn))(flags, iid, pp); }
    HRESULT GameCallHwnd(void** table, IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, IDXGISwapChain1** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(fx::Dispatch(table[15]))(self, dev, hwnd, d, nullptr, nullptr, pp);
    }
    HRESULT GameCallCreate(void** table, IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::FactoryCreateSwapChain_t>(fx::Dispatch(table[10]))(self, dev, d, pp);
    }
    HRESULT GameCallQi(void** table, IUnknown* self, REFIID iid, void** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<decltype(&ObjQi)>(table[0])(self, iid, pp);
    }
    HRESULT GamePresent(void** table, IDXGISwapChain* self, UINT sync, UINT flags) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::Present_t>(fx::Dispatch(table[8]))(self, sync, flags);
    }
    HRESULT GamePresent1(void** table, IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::Present1_t>(fx::Dispatch(table[22]))(self, sync, flags, p);
    }
    HRESULT GameResize(void** table, IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::ResizeBuffers_t>(fx::Dispatch(table[13]))(self, n, w, h, fmt, flags);
    }
    HRESULT GameResize1(void** table, IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::ResizeBuffers1_t>(fx::Dispatch(table[39]))(self, n, w, h, fmt, flags, nodes, queues);
    }
    // Converts an escaped added-work fault into a failed assertion instead of a crashed suite (SEH-only catcher, no
    // unwinding objects in this wrapper; used by the fault schedules to run the same call on the pre-fix source).
    bool CallPresentCatchingFault(void** table, IDXGISwapChain* self, UINT sync, UINT flags) {
        __try { GamePresent(table, self, sync, flags); return false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
}

// Harness migrated to the v0.95 capture owner. No legacy renderer is compiled.
namespace {
int assertions = 0;
std::filesystem::path evidence;
HWND g_window = nullptr, g_window2 = nullptr;
std::map<void*, void*> nativeAliases;
std::vector<std::pair<std::string,int>> completedCases;
void Check(bool ok, const std::string& reason) { ++assertions; if (!ok) throw std::runtime_error(reason); }
void CaseHeader(const char* name) { std::cout << "CASE: " << name << '\n'; }
size_t Draws() { return fx::DrawCount(); }
DXGI_SWAP_CHAIN_DESC1 MakeDesc(UINT w = 1280, UINT h = 720, UINT n = 3) {
    DXGI_SWAP_CHAIN_DESC1 d{}; d.Width=w; d.Height=h; d.BufferCount=n; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
    d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; return d;
}
HMODULE WINAPI ModuleHandle(LPCWSTR name) {
    if (wcscmp(name,L"dxgi.dll")==0) return fx::dxgiModule;
    if (wcscmp(name,L"sl.interposer.dll")==0) return fx::slModule;
    return GetModuleHandleW(name);
}
bool PinTarget(void* p) { const HMODULE m=fx::OsModuleFromAddress(p); return m && fx::OsPinModule(m)==m; }
void* NativeInterface(void* p) {
    const auto it=nativeAliases.find(p);
    if (it==nativeAliases.end()) return nullptr;
    reinterpret_cast<IUnknown*>(it->second)->AddRef(); return it->second;
}
void DropActiveRenderer() {
    overlay::g_queue=nullptr; overlay::g_activeQueue.reset(); overlay::g_activeCapture.reset();
    if (overlay::g_device) { overlay::g_device->Release(); overlay::g_device=nullptr; }
    overlay::g_ready=false;
}
void ResetProduction() {
    overlay::g_testResizePhase=nullptr;
    overlay::g_testAfterPresentPrepare=nullptr;
    DropActiveRenderer();
    for (auto& c:overlay::g_capturedQueues) { if(c) c->live=false; c.reset(); }
    overlay::g_seenFactoryVtables.clear();
    for(auto& h:overlay::g_dxgiExportHookTargets) h={};
    for(auto& h:overlay::g_streamlineExportHookTargets) h={};
    for(auto& h:overlay::g_factoryHookTargets) h={};
    overlay::g_presentHookTarget={}; overlay::g_present1HookTarget={}; overlay::g_resizeHookTarget={};
    overlay::g_resize1HookTarget={}; overlay::g_colorSpaceHookTarget={}; overlay::g_gameBoundary={};
    overlay::oCreateDXGIFactory=nullptr; overlay::oCreateDXGIFactory1=nullptr; overlay::oCreateDXGIFactory2=nullptr;
    overlay::oStreamlineCreateDXGIFactory=nullptr; overlay::oStreamlineCreateDXGIFactory1=nullptr; overlay::oStreamlineCreateDXGIFactory2=nullptr;
    overlay::oCreateSwapChain=nullptr; overlay::oCreateSwapChainForHwnd=nullptr;
    overlay::oCreateSwapChainForCoreWindow=nullptr; overlay::oCreateSwapChainForComposition=nullptr;
    overlay::oPresent=nullptr; overlay::oPresent1=nullptr; overlay::oResizeBuffers=nullptr; overlay::oResizeBuffers1=nullptr; overlay::oSetColorSpace1=nullptr;
    overlay::oGameSwapChainInit=nullptr;
    overlay::g_captureGeneration=0; overlay::g_boundGeneration=0; overlay::g_boundRevision=0; overlay::g_helperCalls=0;
    overlay::g_boundSwapChain=nullptr; overlay::g_boundSwapChainWindow=nullptr; overlay::g_hwnd=nullptr;
    overlay::g_gameWindow=nullptr; overlay::g_gameWindowFromSwapChain=false;
    overlay::g_failed=false; overlay::g_disabled=false; overlay::g_wasOpen=false; overlay::g_insidePresent=false;
    overlay::g_resizeInProgress=0; overlay::t_resizeLockHeld=false;
    overlay::g_rendererResizeState=overlay::PresentRendererResizeState::Idle;
    overlay::g_presentHookReady=0; overlay::g_factoryMethodHookMask=0; overlay::g_swapChainMethodHookMask=0;
    overlay::g_dxgiExportHookMask=0; overlay::g_streamlineExportHookMask=0; overlay::g_streamlineFactoryExportsHooked=false;
    overlay::t_createDepth=overlay::t_helperDepth=overlay::t_observerDepth=0;
    overlay::g_frameAttempts=0; overlay::g_frameAdmitted=0; overlay::g_frameSkipped=0; overlay::g_frameConsecutiveSkips=0; overlay::g_presents=0;
    core::g_menuOpen=core::g_uiWantsMouse=core::g_uiWantsKeyboard=core::g_uiTextInput=core::g_uiMouseOverUi=false;
}
void ResetFixture() {
    ResetProduction(); fx::ResetHooks(); fx::ClearLogs(); nativeAliases.clear();
    fx::ResetOwnershipProbe(); fx::menuOpenedCalls=0; fx::menuClosedCalls=0;
    fx::protectCalls.clear(); fx::pins.clear(); fx::pinAttempts.clear(); fx::pinFails.clear();
    fx::addRefs=0; fx::releases=0; fx::destroyed=0; fx::stage=0; fx::faultAt=0;
    fx::unowned.clear(); fx::owners.clear(); fx::moduleSpans.clear();
    fx::pinBlockModule=nullptr; fx::pinBlockCount=0; fx::ResetPinGate();
    fx::chain3QiBlockCount=0; fx::ResetChain3QiGate(); fx::faultChain3Qi=fx::faultIdentityQi=0; fx::chain3QiFails=false;
    fx::pauseCell=nullptr; fx::failProtectCell=nullptr; fx::failRestore=false; fx::conflictCell=nullptr; fx::conflictValue=nullptr;
    { std::lock_guard<std::mutex> l(fx::gateMutex); fx::paused=fx::resume=fx::gateTimeout=false; }
    fx::ResetResizeGate(); fx::resizeWaitBoundMs=10000; fx::ResetDrainState(); fx::drainWaitBoundMs=10000; fx::drainFn=nullptr;
    { std::lock_guard<std::mutex> l(fx::lookupMutex); fx::identityQueries=0; }
    fx::foreignBlobMode=-1; fx::payloadReads=0; fx::armAllocFailOnDesc=false; g_testFailNextAlloc=0;
    fx::directCalls[0]=fx::directCalls[1]=fx::directCalls[2]=0;
    for(int i=0;i<3;++i) {fx::directHr[i]=S_OK; fx::directNullOutput[i]=false;}
    fx::slCreateCalls=fx::resolveCalls=fx::innerCreateCalls=0; fx::slCreateFail=false;
    fx::resolverFails=fx::resolverForeign=false; fx::lastReturnedChain=nullptr;
    fx::editorOpen=fx::editorPlaying=fx::editorPlacing=fx::editorCameraMode=fx::editorMouseMode=fx::inputInitialized=false; fx::toggleCalls=fx::cameraModeCalls=fx::togglePlayCalls=0;
    fx::frameScopeTryCalls=fx::frameScopeAdmitted=fx::frameScopeDeclined=0;
    fx::inputDenyNext=fx::hotkeyPressedCalls=fx::pendingToggleTaps=fx::pendingModeTaps=fx::togglePollsObserved=0;
    fx::debugPointCount=0; fx::applyToggleTransition=fx::sharedHotkey=false;
    core::g_keyToggle=VK_INSERT; core::g_keyMode=VK_HOME;
    { std::lock_guard<std::mutex> l(fx::drawMutex); fx::draws.clear(); }
    fx::rebindCalls.clear(); fx::rebindFails=false; fx::chainPool.clear();
    fx::boundaryHookCreateStatus=fx::boundaryHookEnableStatus=fx::legacyHookCreateStatus=fx::legacyHookEnableStatus=0;
    fx::boundaryUnwindMode=0; fx::boundaryHeadSigCorrupt=false;
    fx::boundaryInnerFactory=fx::boundaryRawFactory=fx::boundaryFinalFactory=nullptr;
    fx::boundaryInnerQueue=fx::boundaryRawQueue=fx::boundaryFinalQueue=nullptr;
    fx::imageImportDirRva=fx::imageImportDirSize=fx::imageFirstThunkOverride=0;
    fx::imageSizeOfHeaders=0x400; fx::imageSectionAlignment=0x1000; fx::imageFileAlignment=0x200; fx::kernelModuleName="KERNEL32.dll";
    fx::moduleNames[fx::dxgiModule]="dxgi.dll"; fx::moduleNames[fx::slModule]="sl.interposer.dll";
    fx::moduleNames[fx::reshadeModule]="reshade.asi"; fx::moduleNames[fx::foreignModule]="user32.dll";
    fx::BuildImage(); fx::BuildWorld(); core::g_base=reinterpret_cast<uintptr_t>(fx::image);
    overlay::g_moduleHandle=&ModuleHandle; overlay::g_exportAddress=&fx::NativeGetProcAddress;
    overlay::g_testNativeInterface=&NativeInterface; overlay::g_testPinTarget=&PinTarget;
    overlay::g_testFrameSink=&fx::DrawSink; overlay::g_testDrainFn=&fx::DrainHook; overlay::g_testRebindFn=&fx::RebindSeam;
}
void Install() { core::g_base=reinterpret_cast<uintptr_t>(fx::image); overlay::Install(); }
using Capture = std::shared_ptr<overlay::CapturedD3D12Queue>;
Capture CookieOf(fx::Chain* chain) { return overlay::LookupCaptured(reinterpret_cast<IDXGISwapChain*>(chain)); }
void* AssociatedQueue(fx::Chain* chain, UINT index=0) {
    auto c=CookieOf(chain); if(!c) return nullptr;
    std::lock_guard<std::mutex> l(overlay::g_capturedQueueMutex);
    const auto q=c->presentQueueCount ? (index<c->presentQueueCount ? c->presentQueues[index] : nullptr) : c->creationQueue;
    return q ? q->queue : nullptr;
}
void DetachCookie(fx::Chain* c) { if(c->cookie) {c->cookie->Release(); c->cookie=nullptr;} c->hasCookie=false; }
IDXGISwapChain1* CreateRawChain(fx::Factory* f, fx::Queue* q, HWND hwnd, UINT w=1280, UINT h=720, UINT n=3) {
    auto desc=MakeDesc(w,h,n); IDXGISwapChain1* out=nullptr;
    fx::GameCallHwnd(f->vt,reinterpret_cast<IDXGIFactory2*>(f),reinterpret_cast<IUnknown*>(q),hwnd,&desc,&out);
    return out;
}
IDXGISwapChain1* CreateOuterChain(HWND hwnd=g_window, UINT w=1280, UINT h=720, UINT n=3) {
    (void)n; // the inherited modeled game helper always requests three buffers
    void* host=fx::BuildBoundaryHost(fx::world.slFactory,fx::world.slQueue,hwnd,w,h,0);
    const auto hr=reinterpret_cast<intptr_t>(fx::RunBoundaryHelper(host));
    if(FAILED(static_cast<HRESULT>(hr))) return nullptr;
    IDXGISwapChain1* out=nullptr; std::memcpy(&out,static_cast<char*>(host)+0xa8,sizeof out); return out;
}
void Bound() {
    ResetFixture(); Install();
    Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)==reinterpret_cast<IDXGISwapChain1*>(fx::world.dxgiChain),"bind result");
    Check(CookieOf(fx::world.dxgiChain)!=nullptr,"live capture"); fx::editorOpen=true;
}
HRESULT Present(fx::Chain* c=nullptr, UINT flags=0) { if(!c)c=fx::world.dxgiChain; return fx::GamePresent(c->vt,reinterpret_cast<IDXGISwapChain*>(c),1,flags); }
HRESULT Resize(fx::Chain* c=nullptr) { if(!c)c=fx::world.dxgiChain; return fx::GameResize(c->vt,reinterpret_cast<IDXGISwapChain*>(c),0,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM,0); }
HRESULT Resize1(fx::Chain* c, UINT n, IUnknown* const* qs, const UINT* nodes=nullptr) { return fx::GameResize1(c->vt,reinterpret_cast<IDXGISwapChain3*>(c),n,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM,0,nodes,qs); }
void CheckNoTableWrites() { Check(fx::protectCalls.empty(),"no foreign VirtualProtect/CAS"); }
void CheckClosed() { Check(overlay::g_resizeInProgress==0,"resize barrier closed"); Check(!overlay::t_resizeLockHeld,"resize TLS restored"); }
std::mutex phaseMutex;
std::condition_variable phaseCv;
unsigned phases[4]{};
int blockedPhase=0;
bool phaseReleased=false, phaseTimeout=false;
void PhaseReset(int block=0) { std::lock_guard<std::mutex> l(phaseMutex); for(auto& n:phases)n=0; blockedPhase=block; phaseReleased=phaseTimeout=false; }
void ReleasePhase() { std::lock_guard<std::mutex> l(phaseMutex); phaseReleased=true; phaseCv.notify_all(); }
bool WaitPhase(int phase,unsigned count) { std::unique_lock<std::mutex> l(phaseMutex); return phaseCv.wait_for(l,std::chrono::seconds(15),[&]{return phases[phase]>=count;}); }
void ResizePhase(int phase,uint64_t) {
    std::unique_lock<std::mutex> l(phaseMutex); ++phases[phase]; phaseCv.notify_all();
    if(phase==blockedPhase && !phaseCv.wait_for(l,std::chrono::seconds(15),[]{return phaseReleased;}))phaseTimeout=true;
}
std::mutex preparedMutex;
std::condition_variable preparedCv;
bool preparedEntered=false, preparedReleased=false, preparedTimeout=false;
void ResetPreparedGate() { std::lock_guard<std::mutex> lock(preparedMutex); preparedEntered=preparedReleased=preparedTimeout=false; }
void ReleasePreparedGate() { std::lock_guard<std::mutex> lock(preparedMutex); preparedReleased=true; preparedCv.notify_all(); }
bool WaitPreparedGate() { std::unique_lock<std::mutex> lock(preparedMutex); return preparedCv.wait_for(lock,std::chrono::seconds(15),[]{return preparedEntered;}); }
void HoldPreparedFrame() {
    std::unique_lock<std::mutex> lock(preparedMutex);
    preparedEntered=true; preparedCv.notify_all();
    if(!preparedCv.wait_for(lock,std::chrono::seconds(15),[]{return preparedReleased;})) preparedTimeout=true;
}
struct WorkerGuard {
    std::thread* t;
    std::unique_lock<std::timed_mutex>* held=nullptr;
    ~WorkerGuard() {
        if(held && held->owns_lock()) held->unlock();
        ReleasePreparedGate(); ReleasePhase(); fx::ReleasePause(); fx::ReleasePinGate(); fx::ReleaseDrain(); fx::ReleaseResizeOriginal(); fx::ReleaseChain3Qi();
        if(t && t->joinable())t->join();
    }
};
void SaveLogs(const std::filesystem::path& path) { std::ofstream out(path); for(const auto& line:fx::allLogs)out<<line<<'\n'; }
}
void CaseMissedPath() {
    ResetFixture(); Install();
    Check(overlay::g_dxgiExportHookMask==7 && overlay::g_streamlineExportHookMask==7,"both provider export sets");
    Check(fx::slCreateCalls==0 && fx::directCalls[1]==1,"probe real DXGI only, never SL before init");
    Check(*fx::cellFactory2==reinterpret_cast<void*>(&fx::ExportCreateFactory2),"main import pointer unchanged");
    auto proc=fx::GameGetProcAddress(fx::slModule,"CreateDXGIFactory2");
    IDXGIFactory4* f=nullptr;
    Check(fx::GameCallFnC(reinterpret_cast<void*>(proc),0,__uuidof(IDXGIFactory4),reinterpret_cast<void**>(&f))==S_OK,"SL export delegated");
    Check(f==reinterpret_cast<IDXGIFactory4*>(fx::world.slFactory) && fx::slCreateCalls==1,"same final factory, once");
    Check(fx::world.slFactory->vt[15]==reinterpret_cast<void*>(&fx::SlFactoryHwnd),"SL table unchanged");
    Check(overlay::g_factoryHookTargets[1].target==reinterpret_cast<void*>(&fx::FactoryHwnd),"first native method wins");
    Check(CreateOuterChain()==reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain),"helper returns final wrapper chain");
    Check(CookieOf(fx::world.slChain)!=nullptr && !CookieOf(fx::world.slInnerChain),"only final chain captured");
    Check(overlay::g_queue==nullptr && overlay::g_boundGeneration==0,"creation never activates renderer");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK,"present forwarded");
    Check(Draws()==1 && fx::LastDraw().chain==fx::world.slChain && fx::LastDraw().queue==fx::world.slQueue,"one final pair drawn");
    CheckNoTableWrites();
}
void CaseWrapperNesting() {
    ResetFixture(); Install(); fx::SetWrapperInner(fx::world.dxgiFactory[2],fx::world.dxgiQueue);
    auto* first=CreateOuterChain();
    Check(first && CookieOf(fx::world.slChain) && !CookieOf(fx::world.dxgiChain),"nested native creation suppressed");
    Check(overlay::g_captureGeneration==1 && fx::world.dxgiFactory[2]->hwndCalls==1,"one owner, inner original once");
    auto* table=fx::world.slFactory->vt; const void* before=table[15];
    fx::outerHwndImpl=&fx::AltFactoryHwnd;
    auto* second=CreateOuterChain();
    Check(second && second!=first && CookieOf(fx::world.slFactory->chain),"changed downstream result captured");
    Check(table[15]==before && overlay::g_captureGeneration==2,"no repatch and exactly one new generation");
    Check(CookieOf(fx::world.slChain)!=nullptr,"old live creation retained independently"); CheckNoTableWrites();
}
void CaseProviderDispatch() {
    ResetFixture(); Install();
    for(int i=0;i<3;++i) {
        IDXGIFactory* f=nullptr; const long before=fx::directCalls[i]; HRESULT hr=E_FAIL;
        if(i==0)hr=fx::GameCreateFactory0(__uuidof(IDXGIFactory),reinterpret_cast<void**>(&f));
        if(i==1)hr=fx::GameCreateFactory1(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(&f));
        if(i==2)hr=fx::GameCreateFactory2(7,__uuidof(IDXGIFactory4),reinterpret_cast<void**>(&f));
        Check(hr==S_OK && f==reinterpret_cast<IDXGIFactory*>(fx::world.dxgiFactory[i]),"provider-specific result");
        Check(fx::directCalls[i]==before+1,"one original per export");
    }
    fx::directHr[2]=E_ACCESSDENIED; void* sentinel=reinterpret_cast<void*>(1);
    Check(fx::GameCreateFactory2(0,__uuidof(IDXGIFactory),&sentinel)==E_ACCESSDENIED && sentinel==reinterpret_cast<void*>(1),"failed result untouched");
    Check(!fx::GameGetProcAddress(fx::slModule,"Sleep") && GetLastError()==0x33,"unknown symbol untouched");
    Check(!fx::GameGetProcAddress(fx::slModule,MAKEINTRESOURCEA(7)) && GetLastError()==0x5151,"ordinal untouched");
    Check(fx::GameGetProcAddress(fx::foreignModule,"CreateDXGIFactory2")==reinterpret_cast<FARPROC>(&fx::ForeignExport),"foreign resolver result untouched");
    fx::resolverFails=true; Check(!fx::GameGetProcAddress(fx::slModule,"CreateDXGIFactory2") && GetLastError()==0x2a2a,"null result preserves error");
}
void CaseInterfaceAliases() {
    ResetFixture(); Install();
    fx::world.slFactory->AddAlias(__uuidof(IDXGIFactory2),fx::world.aliasFactory);
    auto snapshot=std::vector<void*>(fx::world.aliasFactory->vt,fx::world.aliasFactory->vt+fx::kSlots);
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.slFactory),"test");
    Check(std::equal(snapshot.begin(),snapshot.end(),fx::world.aliasFactory->vt),"alias prefix and higher slots untouched");
    Check(overlay::g_factoryHookTargets[1].target==reinterpret_cast<void*>(&fx::FactoryHwnd),"alias cannot replace chosen provider");
    fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3),fx::world.aliasChain);
    Check(CreateOuterChain()!=nullptr,"alias creation delegates");
    auto c=CookieOf(fx::world.slChain); Check(c && c->tableCount==2,"both public dependencies recorded");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK && Draws()==1,"returned interface draws chain3 view");
    Check(fx::LastDraw().chain==fx::world.aliasChain,"qualified alias used");
    Check(Present(fx::world.aliasChain)==S_OK && Draws()==1,"unattached alias never claims a frame"); CheckNoTableWrites();
}
void CaseMultipleConcurrent() {
    ResetFixture(); Install(); const auto count=fx::Attempts(reinterpret_cast<void*>(&fx::FactoryHwnd));
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.probeFactory),"test");
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.probeFactory),"test");
    Check(fx::Attempts(reinterpret_cast<void*>(&fx::FactoryHwnd))==count,"shared target installed once");
    IDXGISwapChain1* out=nullptr; std::thread worker([&]{out=CreateOuterChain();}); WorkerGuard guard{&worker}; worker.join();
    Check(out && overlay::g_queue==nullptr,"threaded capture never renders");
    ResetFixture(); Install(); fx::pauseCell=reinterpret_cast<void*>(&fx::ChainPresent);
    std::thread publisher([&]{CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);}); WorkerGuard pub{&publisher};
    Check(fx::WaitPaused(15000),"publication gate entered"); fx::editorOpen=true;
    Check(Present()==S_OK && Draws()==0 && overlay::g_boundGeneration==0,"no early callback authorization");
    fx::ReleasePause(); publisher.join(); Check(!fx::gateTimeout,"publication completed");
    Check(Present()==S_OK && Draws()==1,"first frame after full publication"); CheckNoTableWrites();
}
void CaseCreationFilters() {
    ResetFixture(); Install();
    auto* f=fx::world.probeFactory; auto* q=fx::world.dxgiQueue; auto* c=fx::world.dxgiChain;
    f->hwndHr=E_FAIL; auto* failed=CreateRawChain(f,q,g_window); Check(failed==reinterpret_cast<IDXGISwapChain1*>(1) && !CookieOf(c),"failed creation never qualified");
    f->hwndHr=S_OK; f->hwndNullOutput=true; Check(!CreateRawChain(f,q,g_window) && !CookieOf(c),"null output never captured"); f->hwndNullOutput=false;
    auto request=MakeDesc(); IDXGISwapChain1* nonQueueResult=nullptr;
    Check(fx::GameCallHwnd(f->vt,reinterpret_cast<IDXGIFactory2*>(f),reinterpret_cast<IUnknown*>(fx::world.dxgiDev),g_window,&request,&nonQueueResult)==S_OK,"non-queue original still delegates");
    Check(!CookieOf(c) && nonQueueResult==reinterpret_cast<IDXGISwapChain1*>(c),"non-D3D12-queue argument never binds");
    q->desc.Type=D3D12_COMMAND_LIST_TYPE_COPY; CreateRawChain(f,q,g_window); Check(!CookieOf(c),"non DIRECT declined"); q->desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    c->dev=fx::world.slDev; CreateRawChain(f,q,g_window); Check(!CookieOf(c),"device mismatch declined"); c->dev=fx::world.dxgiDev;
    c->identityFail=true; CreateRawChain(f,q,g_window); Check(!c->hasCookie,"unknown canonical identity declined"); c->identityFail=false;
    c->descHr=E_FAIL; CreateRawChain(f,q,g_window); Check(!CookieOf(c),"unreadable description declined"); c->descHr=S_OK;
    c->w=c->h=64; CreateRawChain(f,q,g_window,0,0); Check(!CookieOf(c),"tiny resolved chain declined");
    c->w=1280; c->h=720; c->cookieHr=E_FAIL; const LONG refs=q->refs;
    CreateRawChain(f,q,g_window); Check(!CookieOf(c) && q->refs==refs,"rejected attachment retains no queue");
    c->cookieHr=S_OK; CreateRawChain(f,q,g_window,0,0); Check(CookieOf(c)!=nullptr,"zero request uses valid resolved size"); CheckNoTableWrites();
}
void CaseSlotOwnership() {
    ResetFixture(); Install(); fx::failCreateTarget=reinterpret_cast<void*>(&fx::ChainResize);
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(!CookieOf(fx::world.dxgiChain) && !overlay::g_presentHookTarget.installed,"partial create rolled back");
    Check(!fx::Hooked(reinterpret_cast<void*>(&fx::ChainPresent)) && fx::hookRemoves>0,"owned hooks removed");
    ResetFixture(); Install(); fx::failEnableTarget=reinterpret_cast<void*>(&fx::ChainResize);
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(!CookieOf(fx::world.dxgiChain) && fx::hookDisables>0 && fx::hookRemoves>0,"partial enable rolled back");
    Check(!fx::Hooked(reinterpret_cast<void*>(&fx::ChainPresent)),"no partial present remains"); CheckNoTableWrites();
    Bound(); auto c=CookieOf(fx::world.dxgiChain);
    fx::world.dxgiChain->vt[39]=reinterpret_cast<void*>(&fx::ForeignExport);
    Check(Present()==S_OK && Draws()==0 && c->unsupported,"foreign dependency loss stops participation");
    Check(fx::world.dxgiChain->vt[39]==reinterpret_cast<void*>(&fx::ForeignExport),"never repair foreign slot");
}
void CasePresentSelection() {
    Bound(); Check(Present(nullptr,DXGI_PRESENT_TEST)==S_OK && Draws()==0,"TEST delegates without draw");
    fx::world.dxgiChain->present1CallsPresent=true;
    Check(fx::GamePresent1(fx::world.dxgiChain->vt,reinterpret_cast<IDXGISwapChain1*>(fx::world.dxgiChain),2,0,nullptr)==S_OK,"nested present result");
    Check(Draws()==1 && fx::world.dxgiChain->presents==2 && fx::world.dxgiChain->presents1==1,"nested originals once, one overlay draw");
    auto* other=fx::NewChain(g_window2,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.probeFactory->chain=other; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window2);
    Check(Present(other)==S_OK && Draws()==1,"other HWND cannot take over");
    auto* newer=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.probeFactory->chain=newer; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(Present(newer)==S_OK && Draws()==2,"new same-window generation activates");
    Check(Present()==S_OK && Draws()==2,"older generation cannot steal back");
    auto* unknown=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); unknown->vt=newer->vt;
    Check(Present(unknown)==S_OK && Draws()==2,"same table/HWND/device is not a captured identity");
    // Inherited C8 cross-object wrapper branch (not the same-chain Present1 case):
    // A owns the binding; its original calls a distinct, unbound B through the same hooked method.
    Bound();
    auto* outer=fx::world.dxgiChain; auto outerCapture=CookieOf(outer);
    auto* inner=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(inner!=outer && inner->identity!=outer->identity && inner->vt[8]==outer->vt[8],"C8 cross-object: distinct identities share the hooked method");
    Check(!CookieOf(inner) && !inner->hasCookie,"C8 cross-object: inner has no capture");
    outer->wrapperInner=inner; outer->presentHr=E_FAIL; inner->presentHr=DXGI_STATUS_OCCLUDED;
    Check(Present(outer)==DXGI_STATUS_OCCLUDED,"C8 cross-object: exact inner HRESULT propagates through outer");
    Check(outer->presents==1 && inner->presents==1,"C8 cross-object: both originals execute exactly once");
    Check(Draws()==1 && fx::LastDraw().chain==outer,"C8 cross-object: exactly one draw belongs to A");
    Check(fx::LastDraw().queue==fx::world.dxgiQueue && overlay::g_queue==reinterpret_cast<ID3D12CommandQueue*>(fx::world.dxgiQueue),"C8 cross-object: only A's queue is selected");
    Check(overlay::g_activeCapture==outerCapture && overlay::g_boundGeneration==outerCapture->generation,"C8 cross-object: B cannot adopt renderer ownership");
    Check(overlay::g_frameAttempts==1 && fx::frameScopeTryCalls==1 && fx::hotkeyPressedCalls==2 && fx::menuOpenedCalls==1,"C8 cross-object: inner delegates without input/frame admission");
    Check(!CookieOf(inner) && !inner->hasCookie && overlay::g_captureGeneration==1,"C8 cross-object: delegation creates no B binding");
    outer->wrapperInner=nullptr;
}
void CaseCookieAccounting() {
    Bound(); auto* c=fx::world.dxgiChain; auto* q=fx::world.dxgiQueue;
    const LONG qrefs=q->refs, crefs=c->refs; Check(Present()==S_OK && Draws()==1,"first frame");
    Check(q->refs==qrefs && c->refs==crefs,"active lease adds no queue or chain cycle");
    IUnknown* keep=c->cookie; keep->AddRef(); DetachCookie(c);
    Check(Present()==S_OK && Draws()==1,"removed private entry declines even with retained attachment");
    keep->Release(); Check(!CookieOf(c),"attachment retirement removes receipt");
    Check(q->refs==qrefs,"active work keeps queue lease"); DropActiveRenderer();
    Check(q->refs==qrefs-1,"queue released exactly once after active owner");
    Bound(); const LONG before=fx::world.dxgiQueue->refs;
    auto* wrong=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); wrong->forwardCookieTo=fx::world.dxgiChain;
    Check(Present(wrong)==S_OK && Draws()==0,"forwarded private data never authorizes different identity");
    Check(fx::world.dxgiQueue->refs==before,"wrong chain retains no queue");
}
void CaseResizeGeneration() {
    Bound(); Check(Present()==S_OK && Draws()==1,"initial frame"); auto c=CookieOf(fx::world.dxgiChain);
    Check(Resize()==S_OK && c->revision==1,"plain success commits revision");
    fx::world.slQueue2->dev=fx::world.dxgiDev;
    IUnknown* queues[3]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue),reinterpret_cast<IUnknown*>(fx::world.slQueue2),reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)};
    Check(Resize1(fx::world.dxgiChain,3,queues)==S_OK && c->presentQueueCount==3,"per-buffer DIRECT queues accepted");
    fx::world.dxgiChain->currentIndex=1; Check(Present()==S_OK && fx::LastDraw().queue==fx::world.slQueue2,"current buffer's queue selected");
    fx::world.dxgiChain->resize1Hr=DXGI_ERROR_INVALID_CALL; const auto revision=c->revision;
    Check(Resize1(fx::world.dxgiChain,3,reinterpret_cast<IUnknown* const*>(1))==DXGI_ERROR_INVALID_CALL,"failed resize never reads array");
    Check(c->revision==revision && AssociatedQueue(fx::world.dxgiChain,1)==fx::world.slQueue2,"failure retains association");
    fx::world.dxgiChain->resize1Hr=S_OK;
    Check(Resize1(fx::world.dxgiChain,0,nullptr)==S_OK && c->presentQueueCount==0,"null override falls back to creation queue");
    Check(Present()==S_OK && fx::LastDraw().queue==fx::world.dxgiQueue,"fallback recovers drawing");
    queues[0]=reinterpret_cast<IUnknown*>(fx::world.slQueue); // different device
    Check(Resize1(fx::world.dxgiChain,1,queues)==S_OK && c->presentQueueCount==0,"invalid override never installs foreign device queue");
    queues[0]=reinterpret_cast<IUnknown*>(fx::world.slQueue2); UINT badNodes[]={3};
    Check(Resize1(fx::world.dxgiChain,1,queues,badNodes)==S_OK && c->presentQueueCount==0,"invalid node mask never publishes a queue override");
    Check(Present()==S_OK && fx::LastDraw().queue==fx::world.dxgiQueue,"invalid node array recovers on creation queue");
    CheckClosed();
}
void CaseConcurrencyGates() {
    Bound(); Present(); auto* newer=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.probeFactory->chain=newer; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    fx::drainFn=&fx::BlockingDrainHook; std::atomic<bool> done{false};
    std::thread t([&]{Present(newer);done=true;}); WorkerGuard guard{&t};
    Check(fx::WaitDrainEntered(15000),"generation adoption drain entered");
    Check(!done && overlay::g_boundGeneration==1,"no activation before drain");
    fx::ReleaseDrain(); t.join(); Check(!fx::drainTimeout && done,"bounded adoption completes");
    Check(Draws()==2 && fx::LastDraw().chain==newer && overlay::g_boundGeneration==2,"one new frame after exact drain signal");
}
void CaseDrainGateTimeout() {
    Bound(); Present(); auto* queue=overlay::g_queue; fx::drainFn=&fx::BlockingDrainHook; fx::drainWaitBoundMs=100; fx::editorCameraMode=true;
    Check(Resize()==S_OK && fx::drainTimeout,"unreleased bounded GPU gate fails but delegates");
    Check(overlay::g_disabled && overlay::g_queue==queue,"timed out drain retains resources");
    Check(Present()==S_OK && Draws()==1,"no later drawing after failed drain");
    Check(!core::g_menuOpen && !core::g_uiWantsMouse && !core::g_uiWantsKeyboard && !fx::editorCameraMode,"disable hands input and camera back"); CheckClosed();
    // B1: the real render-lock timeout must complete while an admitted frame is
    // held after preparation, then remain authoritative when that frame resumes.
    Bound(); Present();
    input::OwnershipPolicy prior; prior.menuOpen=prior.mouseToUi=prior.keysToUi=true;
    core::g_uiWantsMouse=core::g_uiWantsKeyboard=true; input::PublishOwnership(prior);
    fx::editorCameraMode=true;
    const size_t drawsBefore=Draws();
    const unsigned opensBefore=fx::menuOpenedCalls, closesBefore=fx::menuClosedCalls;
    const int hotkeysBefore=fx::hotkeyPressedCalls;
    const auto generationBefore=overlay::g_captureGeneration.load();
    auto* queueBefore=overlay::g_queue;
    ResetPreparedGate(); overlay::g_testAfterPresentPrepare=&HoldPreparedFrame;
    HRESULT frameResult=E_PENDING;
    std::thread frame([&]{frameResult=Present();}); WorkerGuard frameGuard{&frame};
    Check(WaitPreparedGate(),"B1: admitted frame held after preparation before input publication");
    std::promise<HRESULT> resized; auto resizeDone=resized.get_future();
    IUnknown* sameQueue[]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)};
    std::thread resizing([&]{resized.set_value(Resize1(fx::world.dxgiChain,1,sameQueue));}); WorkerGuard resizeGuard{&resizing};
    Check(resizeDone.wait_for(std::chrono::seconds(5))==std::future_status::ready,"B1: resize timeout returns without waiting for the held frame");
    Check(resizeDone.get()==S_OK,"B1: timeout path preserves original ResizeBuffers1 result"); resizing.join();
    const auto stopped=fx::OwnershipSnapshot();
    Check(overlay::g_disabled && !core::g_menuOpen && !overlay::g_wasOpen && !stopped.policy.menuOpen,"B1: disable completes with closed menu and published ownership");
    Check(!core::g_uiWantsMouse && !core::g_uiWantsKeyboard && !fx::editorCameraMode && fx::menuClosedCalls==closesBefore+1,"B1: disable releases input and camera while frame remains held");
    ReleasePreparedGate(); frame.join(); overlay::g_testAfterPresentPrepare=nullptr;
    Check(!preparedTimeout && frameResult==S_OK && fx::world.dxgiChain->presents==2 && fx::world.dxgiChain->resizes1==1,"B1: both originals finish once after exact gate release");
    const auto resumed=fx::OwnershipSnapshot();
    Check(!core::g_menuOpen && !overlay::g_wasOpen && fx::menuOpenedCalls==opensBefore,"B1: resumed frame cannot reopen a terminally disabled menu");
    Check(!core::g_uiWantsMouse && !core::g_uiWantsKeyboard && !resumed.policy.menuOpen && !resumed.policy.mouseToUi && !resumed.policy.keysToUi && resumed.openCalls==stopped.openCalls,"B1: resumed frame publishes no renewed input ownership");
    Check(Draws()==drawsBefore && fx::hotkeyPressedCalls==hotkeysBefore,"B1: resumed frame performs no draw or hotkey consumption");
    Check(!overlay::PublishFrameOwnership(true,true),"B1: a late DrawFrame ownership commit is refused");
    Check(!overlay::SubmitFrame(nullptr,reinterpret_cast<IDXGISwapChain3*>(fx::world.dxgiChain)),"B1: a late recorded frame cannot submit after disable");
    Check(fx::OwnershipSnapshot().calls==resumed.calls && Draws()==drawsBefore,"B1: refused late commits have no input or GPU boundary effect");
    auto* later=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=later;
    Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)==reinterpret_cast<IDXGISwapChain1*>(later),"B1: post-disable creation still delegates unchanged");
    Check(!CookieOf(later) && !later->hasCookie && overlay::g_captureGeneration==generationBefore,"B1: terminal disable admits no new capture");
    Check(Present(later)==S_OK && Draws()==drawsBefore && overlay::g_queue==queueBefore,"B1: later presents cannot draw or adopt a queue"); CheckClosed();
}
void CaseDeadObject() {
    Bound(); Present(); auto* c=fx::world.dxgiChain; auto saved=std::vector<void*>(c->vt,c->vt+fx::kSlots);
    DetachCookie(c); Check(!CookieOf(c),"dead attachment unlinks capture");
    Check(Present()==S_OK && Draws()==1,"dead binding only delegates"); DropActiveRenderer();
    Check(Present()==S_OK && Draws()==1,"no resurrection after renderer release");
    Check(std::equal(saved.begin(),saved.end(),c->vt),"lifetime never writes foreign table");
}
void CaseRealImageBounds() {
    ResetFixture(); fx::BuildImageSized(0x173ab000,0x173ab000); Install();
    Check(overlay::g_gameBoundary.installed,"real-size mapped PE accepted"); Check(overlay::g_factoryMethodHookMask==15,"native DXGI independent");
    ResetFixture(); fx::BuildImageSized(0x40000,0x80000); Install(); Check(!overlay::g_gameBoundary.installed,"unmapped claim declined");
    ResetFixture(); fx::imageImportDirRva=0x30000; fx::BuildImage(); Install(); Check(!overlay::g_gameBoundary.installed,"out-of-range directory declined");
    ResetFixture(); fx::imageImportDirSize=0x10; fx::BuildImage(); Install(); Check(!overlay::g_gameBoundary.installed,"truncated import descriptor declined");
    ResetFixture(); fx::imageSizeOfHeaders=0x100; fx::BuildImage(); Install(); Check(!overlay::g_gameBoundary.installed,"short headers declined");
    Check(overlay::g_factoryMethodHookMask==15,"image rejection never disables native capture"); CheckNoTableWrites();
}
void CasePinPrerequisites() {
    ResetFixture(); fx::pinFails.insert(fx::dxgiModule); Install();
    Check(overlay::g_dxgiExportHookMask==0 && overlay::g_factoryMethodHookMask==0,"unpinnable code never hooked");
    Check(!overlay::g_presentHookTarget.installed,"no invented present target");
    ResetFixture(); fx::pinFails.insert(fx::slModule); Install();
    Check(overlay::g_streamlineExportHookMask==0 && overlay::g_dxgiExportHookMask==7,"SL failure does not overwrite native originals");
    ResetFixture(); fx::pinFails.insert(fx::mainModule); Install(); Check(!overlay::g_gameBoundary.installed && overlay::g_factoryMethodHookMask==15,"helper pin prerequisite independent");
    CheckNoTableWrites();
}
void CaseResolverErrorPreservation() {
    ResetFixture(); Install();
    SetLastError(0x1234); auto f=fx::GameGetProcAddress(fx::slModule,"CreateDXGIFactory2");
    Check(f==reinterpret_cast<FARPROC>(&fx::SlCreateFactory2) && GetLastError()==0x1234,"resolver pointer/error untouched by export hook route");
    SetLastError(0x2345); auto foreign=fx::GameGetProcAddress(fx::foreignModule,"CreateDXGIFactory2");
    Check(foreign==reinterpret_cast<FARPROC>(&fx::ForeignExport) && GetLastError()==0x2345,"foreign resolver unchanged");
    void* out=nullptr; Check(fx::GameCallFnC(reinterpret_cast<void*>(f),17,__uuidof(IDXGIFactory4),&out)==S_OK,"resolved export executes through its own original");
    Check(out==fx::world.slFactory && fx::slCreateCalls==1,"provider dispatch not a shared mutable original");
}
void CaseLockExclusion() {
    ResetFixture(); Install(); fx::pauseCell=reinterpret_cast<void*>(&fx::ChainPresent);
    std::thread publisher([&]{CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);}); WorkerGuard guard{&publisher};
    Check(fx::WaitPaused(15000),"publisher reached hook transaction"); fx::editorOpen=true;
    Check(Present()==S_OK && Draws()==0,"callback delegates without waiting for publication lock");
    fx::ReleasePause(); publisher.join(); Check(!fx::gateTimeout,"publisher exact release");
    Check(Present()==S_OK && Draws()==1,"full publication authorizes one frame"); CheckNoTableWrites();
}
void CaseFactoryCaptureOwnership() {
    ResetFixture(); Install();
    auto saved=overlay::oCreateSwapChainForHwnd;
    fx::world.slFactory->vt[15]=reinterpret_cast<void*>(&fx::AltFactoryHwnd);
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.slFactory),"replacement");
    Check(overlay::oCreateSwapChainForHwnd==saved && !fx::Hooked(reinterpret_cast<void*>(&fx::AltFactoryHwnd)),"second provider never replaces original");
    Check(fx::world.slFactory->vt[15]==reinterpret_cast<void*>(&fx::AltFactoryHwnd),"foreign update is never repaired");
    CreateRawChain(fx::world.slFactory,fx::world.slQueue,g_window);
    Check(!CookieOf(fx::world.slFactory->chain),"unselected provider call cannot claim native capture"); CheckNoTableWrites();
}
void CaseExistingPrivateEntry() {
    ResetFixture(); Install(); auto* foreign=fx::NewDevice(1); auto* c=fx::world.dxgiChain;
    fx::SetForeignEntry(c,reinterpret_cast<IUnknown*>(foreign)); const LONG refs=foreign->refs;
    Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)!=nullptr,"foreign private entry never changes creation result");
    Check(!CookieOf(c) && c->cookie==reinterpret_cast<IUnknown*>(foreign) && foreign->refs==refs,"foreign entry never read/replaced/released");
    Check(fx::payloadReads==0,"existence query only");
}
void CaseResizeQueueCommit() {
    Bound(); auto c=CookieOf(fx::world.dxgiChain); fx::world.slQueue2->dev=fx::world.dxgiDev;
    IUnknown* q[]={reinterpret_cast<IUnknown*>(fx::world.slQueue2)};
    Check(Resize1(fx::world.dxgiChain,1,q)==S_OK && c->revision==1,"association before first frame");
    Check(Present()==S_OK && fx::LastDraw().queue==fx::world.slQueue2,"first frame uses committed queue");
    q[0]=reinterpret_cast<IUnknown*>(fx::world.dxgiQueue);
    Check(Resize1(fx::world.dxgiChain,1,q)==S_OK && c->revision==2,"same generation new revision");
    Check(Present()==S_OK && fx::LastDraw().queue==fx::world.dxgiQueue && Draws()==2,"same generation re-adopts committed queue"); CheckClosed();
}
void CaseRebindPolicy() {
    Bound(); Present(); auto* c=fx::world.dxgiChain;
    c->buffers=4;c->w=1920;c->h=1080;c->fmt=DXGI_FORMAT_B8G8R8A8_UNORM; fx::rebindCalls.clear();
    Check(Resize()==S_OK && Present()==S_OK,"resize/rebind delegates");
    Check(fx::rebindCalls.size()==1 && overlay::g_bufferCount==4 && overlay::g_width==1920 && overlay::g_format==c->fmt,"exactly one rebuild, resolved description");
    fx::rebindCalls.clear(); c->resizeHr=DXGI_ERROR_INVALID_CALL;
    Check(Resize()==DXGI_ERROR_INVALID_CALL && Present()==S_OK && fx::rebindCalls.empty(),"failed unchanged resize resumes without rebuild");
    fx::drainFn=&fx::TimeoutDrainHook; c->resizeHr=S_OK;c->buffers=6;
    Check(Resize()==S_OK && overlay::g_disabled,"failed drain closes participation");
    Check(Present()==S_OK && fx::rebindCalls.empty() && overlay::g_bufferCount==4,"no allocator rebuild after failed drain"); CheckClosed();
}
void CaseDeadStorageReuse() {
    Bound(); Present(); auto* dead=fx::world.dxgiChain; const auto generation=overlay::g_boundGeneration;
    void* oldIdentity=dead->identity;
    DetachCookie(dead); DropActiveRenderer(); fx::FreeChainStorage(dead);
    auto* reused=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    reused->identity=oldIdentity; // strongest reuse: interface AND canonical IUnknown token are identical
    Check(reused==dead && !CookieOf(reused),"deterministic reused address starts unbound");
    fx::world.probeFactory->chain=reused; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(CookieOf(reused)->generation>generation,"reused address gets fresh generation");
    Check(Present(reused)==S_OK && overlay::g_boundGeneration>generation && Draws()==2,"reused address rebinds and draws once");
}
void CaseExtensionAtomicity() {
    ResetFixture(); Install(); auto* c=fx::world.dxgiChain;
    c->vt[39]=nullptr; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(!CookieOf(c) && !overlay::g_presentHookTarget.installed,"incomplete chain3 prefix cannot publish readiness");
    Check(c->vt[8]==reinterpret_cast<void*>(&fx::ChainPresent) && c->vt[39]==nullptr,"no prefix cell patched");
    c->vt[39]=reinterpret_cast<void*>(&fx::ChainResize1); CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(CookieOf(c)!=nullptr,"complete fresh capture uses full resize coverage");
    auto saved=overlay::oPresent; Check(saved!=&overlay::hkPresent && overlay::oResizeBuffers1!=&overlay::hkResizeBuffers1,"never saves own detour"); CheckNoTableWrites();
}
void CaseCapacityLimits() {
    ResetFixture(); Install(); std::vector<fx::Chain*> chains;
    for(size_t i=0;i<overlay::kCapturedSwapChains+2;++i) {
        auto* c=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); chains.push_back(c); fx::world.probeFactory->chain=c;
        Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)==reinterpret_cast<IDXGISwapChain1*>(c),"capacity never changes creation result");
        Check(static_cast<bool>(CookieOf(c))==(i<overlay::kCapturedSwapChains),"exact live capture capacity");
    }
    Check(CookieOf(chains.front())!=nullptr,"capacity never evicts live queue");
    fx::editorOpen=true; Check(Present(chains.back())==S_OK && Draws()==0,"over-capacity chain delegates without drawing"); CheckNoTableWrites();
}
void CaseAliasRouting() {
    ResetFixture(); Install(); fx::world.slFactory->AddAlias(__uuidof(IDXGIFactory2),fx::world.aliasFactory);
    auto* alias=fx::world.aliasFactory; void* out=nullptr;
    Check(fx::GameCallQi(fx::world.slFactory->vt,reinterpret_cast<IUnknown*>(fx::world.slFactory),__uuidof(IDXGIFactory2),&out)==S_OK && out==alias,"factory QI result preserved");
    alias->vt[15]=reinterpret_cast<void*>(&fx::FactoryHwnd); alias->chain=fx::world.slChain;
    fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3),fx::world.aliasChain);
    Check(CreateRawChain(alias,fx::world.slQueue,g_window)==reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain),"same chosen method through alias captured");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK && Draws()==1 && fx::LastDraw().chain==fx::world.aliasChain,"canonical chain3 alias drawn");
    Check(Present(fx::world.aliasChain)==S_OK && Draws()==1,"alias without attachment does not acquire independent ownership"); CheckNoTableWrites();
}
void CaseHwndAndChainEdges() {
    ResetFixture(); Install(); fx::chain3QiFails=true;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); Check(!CookieOf(fx::world.dxgiChain),"missing chain3 declined"); fx::chain3QiFails=false;
    HWND child=CreateWindowExA(0,"wbHostWindow","child",WS_CHILD,0,0,100,100,g_window,nullptr,GetModuleHandleA(nullptr),nullptr);
    Check(child!=nullptr,"child exists"); CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,child); Check(!CookieOf(fx::world.dxgiChain),"child declined");
    HWND popup=CreateWindowExA(WS_EX_TOOLWINDOW,"wbHostWindow","owned",WS_POPUP,0,0,100,100,g_window,nullptr,GetModuleHandleA(nullptr),nullptr);
    Check(popup!=nullptr,"owned popup exists"); CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,popup); Check(!CookieOf(fx::world.dxgiChain),"owned popup declined");
    DestroyWindow(child); DestroyWindow(popup); CheckNoTableWrites();
}
void CaseResizeOverlap() {
    Bound(); auto a=CookieOf(fx::world.dxgiChain);
    auto* b=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=b;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); auto cb=CookieOf(b);
    fx::world.dxgiChain->resizeHr=E_FAIL; fx::world.dxgiChain->nestedResizeChain=b;
    Check(Resize()==E_FAIL && b->resizes==1 && fx::world.dxgiChain->resizes==1,"different-chain nested originals once");
    Check(a->revision==0 && cb->revision==1,"inner success survives outer failure"); CheckClosed();
    fx::world.dxgiChain->nestedResizeChain=nullptr; fx::world.dxgiChain->resizeCallsNested=true;
    IUnknown* q[]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)};
    Check(Resize1(fx::world.dxgiChain,1,q)==S_OK && fx::world.dxgiChain->resizes1==2,"same-chain nested originals both forwarded");
    Check(a->resizeAmbiguous && a->unsupported && a->revision==0 && a->resizeCalls==0,"same-chain ambiguity never publishes guessed queue"); CheckClosed();
}
void CaseResizeSameCookieOverlap() {
    Bound(); Present(); auto c=CookieOf(fx::world.dxgiChain); PhaseReset(); overlay::g_testResizePhase=&ResizePhase;
    fx::world.dxgiChain->resizeHr=E_FAIL; fx::world.dxgiChain->resizeBlockCount=1;
    HRESULT ha=E_PENDING,hb=E_PENDING;
    std::thread a([&]{ha=Resize();}); WorkerGuard ga{&a}; Check(fx::WaitResizeEntered(15000),"T1 inside original holding render lock");
    IUnknown* q[]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)};
    std::thread b([&]{hb=Resize1(fx::world.dxgiChain,1,q);}); WorkerGuard gb{&b};
    Check(WaitPhase(1,2),"T2 ticket raised before release of T1");
    fx::ReleaseResizeOriginal(); a.join(); b.join();
    Check(ha==E_FAIL && hb==S_OK && fx::world.dxgiChain->resizes==1 && fx::world.dxgiChain->resizes1==1,"overlapping originals/results preserved");
    Check(c->resizeAmbiguous && c->unsupported && c->revision==0 && c->resizeCalls==0,"overlap publishes no association");
    Check(Present()==S_OK && Draws()==1,"ambiguous generation cannot draw"); CheckClosed();
}
void CaseResizeBlockedFinish() {
    Bound(); Present(); auto c=CookieOf(fx::world.dxgiChain); PhaseReset(2); overlay::g_testResizePhase=&ResizePhase;
    IUnknown* q[]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)}; HRESULT result=E_PENDING;
    std::thread t([&]{result=Resize1(fx::world.dxgiChain,1,q);}); WorkerGuard g{&t};
    Check(WaitPhase(2,1),"original returned and validation reached commit gate");
    std::unique_lock<std::mutex> hold(overlay::g_capturedQueueMutex);
    ReleasePhase(); Check(c->revision==0 && c->resizeCalls==1 && overlay::g_resizeInProgress==1,"blocked commit keeps ticket and old association");
    hold.unlock(); t.join();
    Check(result==S_OK && c->revision==1 && c->resizeCalls==0,"contended commit is not dropped");
    Check(!phaseTimeout && fx::world.dxgiChain->resizes1==1,"event completed and original exactly once"); CheckClosed();
    // A delayed completion belongs to its captured generation, even if another
    // creation reuses the exact interface AND canonical identity in the meantime.
    Bound(); Present(); auto old=CookieOf(fx::world.dxgiChain);
    fx::world.slQueue2->dev=fx::world.dxgiDev;
    IUnknown* replacementQueues[]={reinterpret_cast<IUnknown*>(fx::world.slQueue2)};
    fx::world.dxgiChain->resizeBlockCount=1;
    std::thread pending([&]{Resize1(fx::world.dxgiChain,1,replacementQueues);}); WorkerGuard pendingGuard{&pending};
    Check(fx::WaitResizeEntered(15000),"old generation completion held inside original");
    DetachCookie(fx::world.dxgiChain);
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    auto fresh=CookieOf(fx::world.dxgiChain);
    Check(fresh && fresh!=old && fresh->generation>old->generation,"same addresses publish a distinct live generation");
    fx::ReleaseResizeOriginal(); pending.join();
    Check(!old->live && old->resizeCalls==0 && fresh->revision==0,"stale completion accounts only its own retired ticket");
    Check(AssociatedQueue(fx::world.dxgiChain)==fx::world.dxgiQueue,"stale completion cannot install its queue on replacement");
    Check(Present()==S_OK && fx::LastDraw().queue==fx::world.dxgiQueue && overlay::g_boundGeneration==fresh->generation,"replacement rebind uses its own creation queue"); CheckClosed();
}
void CaseResizeBarrierHeldDraw() {
    Bound(); Present(); auto* b=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.probeFactory->chain=b; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    fx::drainFn=&fx::BlockingDrainHook;
    std::thread t1([&]{Present(b);}); WorkerGuard g1{&t1}; Check(fx::WaitDrainEntered(15000),"frame owns render state at drain");
    PhaseReset(); overlay::g_testResizePhase=&ResizePhase; fx::world.dxgiChain->resizeBlockCount=1;
    std::thread t2([&]{Resize();}); WorkerGuard g2{&t2}; Check(WaitPhase(1,1),"resize ticket entered while frame owns lock");
    Check(fx::world.dxgiChain->resizes==0,"original cannot race in-flight frame");
    fx::ReleaseDrain(); Check(fx::WaitResizeEntered(15000),"original entered after frame drain"); t1.join();
    auto* newer=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.probeFactory->chain=newer; CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(CookieOf(newer)!=nullptr,"creation can publish under resize barrier"); const size_t count=Draws(); const auto generation=overlay::g_boundGeneration;
    Check(Present(newer)==S_OK && Draws()==count && overlay::g_boundGeneration==generation,"barrier prevents activation/draw");
    fx::ReleaseResizeOriginal(); t2.join(); CheckClosed();
    Check(Present(newer)==S_OK && Draws()==count+1 && fx::LastDraw().chain==newer,"after close new creation activates");
}
void CasePinReentry() {
    Bound(); Present(); auto* fresh=fx::NewFactory(fx::world.dxgiChain,fx::world.dxgiQueue);
    fx::pinBlockModule=fx::dxgiModule; fx::pinBlockCount=1;
    std::thread p([&]{overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fresh),"pin");}); WorkerGuard gp{&p};
    Check(fx::WaitPinEntered(15000),"provider pin paused outside locks");
    Check(Present()==S_OK && Draws()==2,"present completes during paused pin");
    auto* next=fx::NewFactory(fx::world.dxgiChain,fx::world.dxgiQueue);
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(next),"concurrent"); CheckNoTableWrites();
    fx::ReleasePinGate(); p.join(); Check(!fx::pinGateTimeout,"pin completed on exact signal");
    ResetFixture(); Install(); fx::pauseCell=reinterpret_cast<void*>(&fx::ChainPresent);
    std::thread first([&]{CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);}); WorkerGuard gfirst{&first};
    Check(fx::WaitPaused(15000),"publication paused"); fx::editorOpen=true; Check(Present()==S_OK && Draws()==0,"preparing is not ready");
    fx::chain3QiBlockCount=1;
    bool observed=false; std::thread second([&]{observed=overlay::InstallRealSwapChainHooks(reinterpret_cast<IDXGISwapChain*>(fx::world.dxgiChain));}); WorkerGuard gsecond{&second};
    Check(fx::WaitChain3QiEntered(15000),"longer public observer entered before publication released");
    fx::ReleaseChain3Qi(); fx::ReleasePause(); first.join(); second.join();
    Check(!fx::chain3QiTimeout && !fx::gateTimeout,"both publication observers completed exact event gates");
    Check(observed && overlay::g_swapChainMethodHookMask==31,"concurrent complete-prefix observation idempotent");
    Check(fx::Attempts(reinterpret_cast<void*>(&fx::ChainPresent))==1,"one method hook despite concurrent observers");
}
void CaseExtensionFailureMatrix() {
    for(int failure=0;failure<3;++failure) {
        ResetFixture(); Install();
        if(failure==0)fx::pinFails.insert(fx::dxgiModule);
        if(failure==1)fx::failEnableTarget=reinterpret_cast<void*>(&fx::ChainResize1);
        if(failure==2) {void* saved=nullptr; Check(MH_CreateHook(reinterpret_cast<void*>(&fx::ChainResize1),reinterpret_cast<void*>(&fx::ForeignExport),&saved)==MH_OK,"foreign hook fixture");}
        CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
        Check(!CookieOf(fx::world.dxgiChain) && !overlay::g_presentHookTarget.installed,"pin/enable/conflict failure never publishes partial readiness");
        Check(fx::world.dxgiChain->vt[39]==reinterpret_cast<void*>(&fx::ChainResize1),"required foreign slot untouched");
        fx::editorOpen=true; Check(Present()==S_OK && Draws()==0,"failure delegates without drawing"); CheckNoTableWrites();
    }
}
void CasePrivateDataBlobsAndFaults() {
    for(int mode=0;mode<3;++mode) {
        ResetFixture(); Install(); fx::foreignBlobMode=mode; const LONG refs=fx::world.dxgiQueue->refs;
        CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
        Check(!CookieOf(fx::world.dxgiChain) && fx::payloadReads==0,"zero/pointer/oversized blob never interpreted as interface");
        Check(fx::world.dxgiQueue->refs==refs,"declined blob balances queue");
    }
    for(int fault=1;fault<=9;++fault) { // includes both hook-qualification aliases and output-then-fault attachment
        ResetFixture(); Install(); fx::faultsRaised=0; fx::faultAt=fault; const LONG qr=fx::world.dxgiQueue->refs, cr=fx::world.dxgiChain->refs;
        auto* out=CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
        Check(out==reinterpret_cast<IDXGISwapChain1*>(fx::world.dxgiChain),"fault cannot change original output");
        Check(fx::faultsRaised==1,"the scheduled acquisition fault actually executed");
        fx::faultAt=0; if(out)out->Release(); DetachCookie(fx::world.dxgiChain);
        Check(fx::world.dxgiQueue->refs==qr && fx::world.dxgiChain->refs==cr,"output-then-fault releases all temporary references");
        Check(!CookieOf(fx::world.dxgiChain),"faulted/retired receipt cannot authorize drawing");
    }
}
void CaseAttachmentRetirementInterleave() {
    Bound(); Present(); fx::chain3QiBlockCount=1;
    std::thread t([&]{Present();}); WorkerGuard g{&t}; Check(fx::WaitChain3QiEntered(15000),"qualification took metadata before retirement");
    DetachCookie(fx::world.dxgiChain); fx::ReleaseChain3Qi(); t.join();
    Check(!fx::chain3QiTimeout && fx::world.dxgiChain->presents==2,"retired interleaving still delegates once");
    Check(Draws()==1 && !CookieOf(fx::world.dxgiChain),"render-locked recheck rejects retired attachment");
}
void CaseReceiptCapacityFailClosed() {
    ResetFixture(); Install(); auto* shared=fx::world.dxgiChain->vt;
    for(size_t i=0;i<overlay::kCapturedSwapChains;++i) {
        auto* c=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); c->vt=shared; fx::world.probeFactory->chain=c;
        CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); Check(CookieOf(c)!=nullptr,"same table distinct identity gets own receipt");
    }
    auto* over=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); over->vt=shared; fx::world.probeFactory->chain=over;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); fx::editorOpen=true;
    Check(!CookieOf(over) && !over->hasCookie,"no publishable receipt means no attachment/readiness");
    Check(Present(over)==S_OK && Draws()==0,"shared table does not bypass exhausted receipt capacity");
}
void CaseAliasDependencyLoss() {
    ResetFixture(); Install(); fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3),fx::world.aliasChain); CreateOuterChain();
    auto c=CookieOf(fx::world.slChain); Check(c && c->tableCount==2,"two alias dependencies");
    fx::world.aliasChain->vt[39]=reinterpret_cast<void*>(&fx::ForeignExport); fx::editorOpen=true;
    Check(Present(fx::world.slChain)==S_OK && Draws()==0 && c->unsupported,"nonpresenting alias loss stops frame");
    Check(fx::world.aliasChain->vt[39]==reinterpret_cast<void*>(&fx::ForeignExport),"never restore lost alias cell");
}
void CaseSelfThunkRejected() {
    ResetFixture(); Install(); fx::world.dxgiChain->vt[39]=reinterpret_cast<void*>(&overlay::hkResizeBuffers1);
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(!CookieOf(fx::world.dxgiChain) && !overlay::g_presentHookTarget.installed,"own detour cannot be downstream original");
    Check(overlay::oResizeBuffers1!=&overlay::hkResizeBuffers1,"no recursive original saved");
    Check(fx::world.dxgiChain->vt[39]==reinterpret_cast<void*>(&overlay::hkResizeBuffers1),"rejected cell unchanged");
}
void CasePresentQualifyFault() {
    Bound(); fx::faultChain3Qi=1; const long ar=fx::addRefs, rr=fx::releases;
    Check(!fx::CallPresentCatchingFault(fx::world.dxgiChain->vt,reinterpret_cast<IDXGISwapChain*>(fx::world.dxgiChain),1,0),"present fault contained");
    Check(fx::world.dxgiChain->presents==1 && Draws()==0 && overlay::g_disabled,"original once and sticky disable");
    Check(fx::addRefs-ar==fx::releases-rr,"faulted QI reference balanced");
    Check(Present()==S_OK && Draws()==0,"later frames remain refused");
}
void CaseResizeLookupFault() {
    Bound(); Present(); const long ar=fx::addRefs,rr=fx::releases; fx::faultIdentityQi=1;
    Check(Resize()==S_OK && fx::world.dxgiChain->resizes==1,"lookup fault still delegates resize");
    Check(overlay::g_disabled && fx::addRefs-ar==fx::releases-rr,"lookup fault disables and balances references");
    Check(Present()==S_OK && Draws()==1,"fault never authorizes next frame"); CheckClosed();
}
void CaseEnteredTicketFault() {
    Bound(); Present(); auto c=CookieOf(fx::world.dxgiChain); const auto revision=c->revision; auto* q=overlay::g_queue;
    fx::drainFn=&fx::FaultDrainHook;
    Check(Resize()==S_OK && fx::world.dxgiChain->resizes==1,"entered drain fault still forwards original");
    Check(c->unsupported && c->resizeCalls==0 && c->revision==revision && overlay::g_queue==q,"faulted ticket closes without releasing active GPU resources");
    Check(overlay::g_disabled && Present()==S_OK && Draws()==1,"no frame after entered fault"); CheckClosed();
}
void CaseContendedTicketClose() {
    Bound(); Present(); auto c=CookieOf(fx::world.dxgiChain); fx::drainFn=&fx::FaultDrainHook; fx::world.dxgiChain->resizeBlockCount=1;
    std::thread t([&]{Resize();}); WorkerGuard g{&t}; Check(fx::WaitResizeEntered(15000),"faulted ticket inside original");
    std::unique_lock<std::mutex> hold(overlay::g_capturedQueueMutex);
    fx::ReleaseResizeOriginal(); Check(c->resizeCalls==1 && overlay::g_resizeInProgress==1,"contended terminal close retains its ticket");
    hold.unlock(); t.join();
    Check(c->resizeCalls==0 && c->unsupported && overlay::g_disabled,"waiting close accounts retained ticket exactly once");
    Check(fx::world.dxgiChain->resizes==1 && Present()==S_OK && Draws()==1,"original once, no later frame"); CheckClosed();
}
void CaseLeaseAllocationFailure() {
    Bound(); Present(); auto c=CookieOf(fx::world.dxgiChain); fx::world.dxgiChain->buffers=1;
    IUnknown* q[]={reinterpret_cast<IUnknown*>(fx::world.dxgiQueue)}; const long ar=fx::addRefs,rr=fx::releases;
    fx::armAllocFailOnDesc=true;
    Check(Resize1(fx::world.dxgiChain,0,q)==S_OK,"allocation failure contained after validated zero-count resize");
    Check(overlay::g_disabled && c->unsupported && c->revision==0,"failed allocation commits no association");
    Check(fx::addRefs-ar==fx::releases-rr,"validated queue references balanced after bad_alloc");
    Check(Present()==S_OK && Draws()==1,"allocation fault never authorizes frame"); CheckClosed();
}
void CaseRealLayoutImportDir() {
    ResetFixture(); fx::imageImportDirRva=0x1fff2; fx::BuildImage(); Install(); Check(!overlay::g_gameBoundary.installed,"unaligned out-of-bounds directory declined");
    ResetFixture(); fx::imageImportDirRva=0x1002; fx::imageImportDirSize=0x10; fx::BuildImage(); Install(); Check(!overlay::g_gameBoundary.installed,"unaligned truncated directory declined");
    ResetFixture(); fx::imageImportDirRva=0x1002; fx::BuildImage(); Install();
    Check(overlay::g_gameBoundary.installed && overlay::g_factoryMethodHookMask==15,"bounded unaligned real-layout image accepted");
    Check(*fx::cellFactory2==reinterpret_cast<void*>(&fx::ExportCreateFactory2) && *fx::cellResolver==reinterpret_cast<void*>(&fx::NativeGetProcAddress),"read-only image discovery does not write imports");
    Check(CreateOuterChain()!=nullptr && CookieOf(fx::world.slChain),"real-layout helper captures final chain");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK && Draws()==1,"real-layout route draws");
}
void CaseForeignWrapperChurn() {
    ResetFixture(); Install(); fx::world.slFactory->innerFactory=fx::world.dxgiFactory[2];
    auto* table=fx::BuildWrapperFactory(fx::reshadeModule); fx::world.slFactory->vt=table;
    const auto bytes=std::vector<void*>(table,table+fx::kSlots);
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.slFactory),"foreign");
    Check(std::equal(bytes.begin(),bytes.end(),table),"first observation leaves all foreign cells unchanged");
    fx::RewrapGeneration(); fx::world.slFactory->identity=fx::NewIdent();
    for(int i=0;i<3;++i) overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.slFactory),"foreign");
    Check(std::equal(bytes.begin(),bytes.end(),table),"reused foreign table never repaired or retapped");
    for(int i=0;i<2;++i) {
        auto* c=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.dxgiFactory[2]->chain=c;
        void* host=fx::BuildBoundaryHost(fx::world.slFactory,fx::world.dxgiQueue,g_window,1280,720,0);
        Check(static_cast<HRESULT>(reinterpret_cast<intptr_t>(fx::RunBoundaryHelper(host)))==S_OK && CookieOf(c),"helper binds through unchanged foreign wrapper");
        fx::editorOpen=true; Check(Present(c)==S_OK && fx::LastDraw().chain==c,"new wrapper generation draws");
    }
    Check(overlay::g_captureGeneration==2,"one final capture per wrapper generation"); CheckNoTableWrites();
}
void CaseChain4IidConstant() {
    ResetFixture(); Install(); IUnknown* out=nullptr;
    Check(fx::world.slChain->QueryInterface(fx::kIidSwapChain4Canonical,reinterpret_cast<void**>(&out))==S_OK && out==reinterpret_cast<IUnknown*>(fx::world.slChain),"game's canonical chain4 IID resolves");
    if(out)out->Release(); out=nullptr;
    Check(fx::world.slChain->QueryInterface(fx::kIidSwapChain4Former,reinterpret_cast<void**>(&out))==E_NOINTERFACE && !out,"former IID rejected");
    Check(CreateOuterChain()!=nullptr && CookieOf(fx::world.slChain),"game-stored chain4 binds through proven chain3 prefix"); CheckNoTableWrites();
}
void CaseGameSideBoundary() {
    ResetFixture(); Install(); Check(overlay::g_gameBoundary.installed && fx::boundaryHooked,"verified helper installed");
    fx::world.slFactory->innerFactory=fx::world.dxgiFactory[2]; fx::world.slFactory->vt=fx::BuildWrapperFactory(fx::reshadeModule);
    void* host=fx::BuildBoundaryHost(fx::world.slFactory,fx::world.dxgiQueue,g_window,1280,720,0);
    Check(static_cast<HRESULT>(reinterpret_cast<intptr_t>(fx::RunBoundaryHelper(host)))==S_OK,"helper original result unchanged");
    Check(overlay::g_helperCalls==1 && overlay::g_captureGeneration==1,"one verified invocation one final capture");
    Check(AssociatedQueue(fx::world.dxgiChain)==fx::world.dxgiQueue,"queue belongs to same helper invocation");
    fx::editorOpen=true; Check(Present()==S_OK && Draws()==1,"game-stored chain draws"); CheckNoTableWrites();
}
void CaseBoundaryUnwindChains() {
    for(int mode=0;mode<=4;++mode) {
        ResetFixture(); fx::boundaryUnwindMode=mode; fx::BuildImage(); Install();
        Check(overlay::g_gameBoundary.installed==(mode<=2),"plain/chaininfo/handler accepted; cycle and depth-budget fail closed");
        Check(overlay::g_factoryMethodHookMask==15,"helper resolve does not gate native methods");
    }
}
void CaseBoundaryHeadSignatureRequired() {
    ResetFixture(); fx::boundaryHeadSigCorrupt=true; fx::BuildImage(); Install();
    Check(!overlay::g_gameBoundary.installed && !fx::boundaryHooked,"head mismatch beyond sanity prefix refuses helper");
    Check(overlay::g_factoryMethodHookMask==15,"independent native capture retained");
}
void CaseReferenceScanCompleteness() {
    CaseHeader("G-REFSCAN: a failed section-header read marks the reference scan incomplete; a completed absence does not");
    ResetFixture();
    auto build = [](bool failHeaderRead, size_t size) -> unsigned char* {
        unsigned char* img = static_cast<unsigned char*>(calloc(1, 0x1000));
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img);
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        IMAGE_NT_HEADERS64* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = failHeaderRead ? 8 : 2;    // 8 with size 0x240: the 5th header read is out of bounds
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(size);
        nt->OptionalHeader.SizeOfHeaders = 0x400;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);         // secBase = 0x188
        std::memcpy(sec[0].Name, ".code", 6);
        sec[0].VirtualAddress = 0x80; sec[0].Misc.VirtualSize = 0x100;
        sec[0].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x180;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = sizeof(RUNTIME_FUNCTION);
        RUNTIME_FUNCTION* rf = reinterpret_cast<RUNTIME_FUNCTION*>(img + 0x180);
        rf->BeginAddress = 0x80; rf->EndAddress = 0x180; rf->UnwindInfoAddress = 0x1C0;
        img[0x1C0] = 0x01; img[0x1C1] = 0; img[0x1C2] = 0; img[0x1C3] = 0;
        std::memcpy(img + 0xA0, fx::kBoundaryString, sizeof(fx::kBoundaryString));
        unsigned char* lea = img + 0xB0;                             // lea rax,[rip+disp] -> the anchor at 0xA0
        lea[0] = 0x48; lea[1] = 0x8D; lea[2] = 0x05;
        const int32_t disp = static_cast<int32_t>(0xA0) - static_cast<int32_t>(0xB0 + 7);
        std::memcpy(lea + 3, &disp, 4);
        return img;
    };
    unsigned char* readable = build(false, 0x600);
    overlay::discovery::ImageInfo a = {}; a.base = reinterpret_cast<uintptr_t>(readable); a.size = 0x600;
    bool complete = false;
    const uintptr_t absent = overlay::discovery::GameBoundaryFuncReferencingString(a, "no such anchor string present", &complete);
    Check(absent == 0 && complete, "G-REFSCAN: a completed scan without the anchor reports absence, not incompleteness");
    complete = false;
    overlay::discovery::GameBoundaryFuncReferencingString(a, fx::kBoundaryString, &complete);
    Check(complete, "G-REFSCAN: a fully readable image yields a complete reference scan");
    unsigned char* partial = build(true, 0x240);
    overlay::discovery::ImageInfo b = {}; b.base = reinterpret_cast<uintptr_t>(partial); b.size = 0x240;
    bool partialComplete = true;
    const uintptr_t none = overlay::discovery::GameBoundaryFuncReferencingString(b, fx::kBoundaryString, &partialComplete);
    Check(!partialComplete && none == 0, "G-REFSCAN: a section-header read failure marks the reference scan incomplete and yields no head");
    bool patternComplete = true; int patternHits = 0;
    const unsigned char val[2] = { 0x48, 0x8D }, mask[2] = { 0xFF, 0xFF };
    overlay::discovery::GameBoundaryScan(b, true, val, mask, 2, &patternHits, &patternComplete);
    Check(!patternComplete, "G-REFSCAN: the byte-pattern scanner reports the same partial section walk as incomplete");
}

void CaseBoundaryQueueZeroThenReal() {
    ResetFixture(); Install(); fx::world.slFactory->innerFactory=fx::world.dxgiFactory[2]; fx::world.slFactory->vt=fx::BuildWrapperFactory(fx::reshadeModule);
    void* boot=fx::BuildBoundaryHost(fx::world.slFactory,nullptr,g_window,1280,720,0); fx::RunBoundaryHelper(boot);
    Check(!CookieOf(fx::world.dxgiChain) && overlay::g_captureGeneration==0,"queue-less boot captures nothing");
    void* real=fx::BuildBoundaryHost(fx::world.slFactory,fx::world.dxgiQueue,g_window,1280,720,0); fx::RunBoundaryHelper(real);
    Check(CookieOf(fx::world.dxgiChain) && overlay::g_captureGeneration==1,"next valid helper invocation captures once");
    fx::editorOpen=true; Check(Present()==S_OK && Draws()==1,"valid second swapchain activates");
}
void CaseIdentityAmendment() {
    ResetFixture(); Install(); fx::world.slChain->dev=fx::world.slDev2;
    nativeAliases[fx::world.slQueue]=fx::world.dxgiQueue; nativeAliases[fx::world.slChain]=fx::world.dxgiChain;
    CreateOuterChain(); auto c=CookieOf(fx::world.slChain);
    Check(c && c->appIdentity!=c->nativeIdentity && AssociatedQueue(fx::world.dxgiChain)==fx::world.dxgiQueue,"split wrappers accepted only through proven native pair");
    fx::editorOpen=true; Check(Present()==S_OK && Draws()==1 && fx::LastDraw().queue==fx::world.dxgiQueue,"native frame uses native queue/device");
    ResetFixture(); Install(); fx::world.slChain->dev=fx::world.slDev2; CreateOuterChain();
    Check(!CookieOf(fx::world.slChain),"helper attestation never bypasses v0.95 device matching");
    fx::world.slChain->dev=fx::world.slDev; fx::world.slChain->identityFail=true; CreateOuterChain();
    Check(!fx::world.slChain->hasCookie,"missing canonical identity still declined");
    fx::world.slChain->identityFail=false; fx::world.slDev->nodes=2; CreateOuterChain();
    Check(!CookieOf(fx::world.slChain),"unsupported multi-node device still declined");
    fx::world.slDev->nodes=1; fx::world.slQueue->desc.NodeMask=3; CreateOuterChain();
    Check(!CookieOf(fx::world.slChain),"unsupported queue node mask still declined");
    CheckNoTableWrites();
}
void CaseMask15Unauthorized() {
    ResetFixture(); Install(); fx::world.slChain->vt[39]=nullptr;
    CreateOuterChain(); Check(!CookieOf(fx::world.slChain) && overlay::g_swapChainMethodHookMask!=31,"missing ResizeBuffers1 cannot publish full hook coverage");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK && Draws()==0,"incomplete public prefix never draws");
}
void CaseQ3FallbackResolve() {
    ResetFixture(); fx::boundaryHeadSigCorrupt=true; fx::BuildImage(); Install();
    Check(!overlay::g_gameBoundary.installed && overlay::g_factoryMethodHookMask==15,"bad helper cannot disable native DXGI");
    Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)!=nullptr && CookieOf(fx::world.dxgiChain),"native path independently captures");
    fx::editorOpen=true; Check(Present()==S_OK && Draws()==1,"fallback draws");
}
void CaseQ3FallbackInstall() {
    for(bool enable:{false,true}) {
        ResetFixture(); if(enable)fx::boundaryHookEnableStatus=MH_ERROR_DISABLED; else fx::boundaryHookCreateStatus=MH_ERROR_MEMORY_ALLOC;
        Install(); Check(!overlay::g_gameBoundary.installed && !fx::boundaryHooked && !overlay::oGameSwapChainInit,"helper create/enable failure leaves no armed route");
        Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)!=nullptr && CookieOf(fx::world.dxgiChain),"native capture unaffected");
        fx::editorOpen=true; Check(Present()==S_OK && Draws()==1,"independent route draws after either hook failure");
    }
}
void CaseQ3NeverCalled() {
    Bound(); Check(overlay::g_gameBoundary.installed && overlay::g_helperCalls==0,"helper installed but never invoked");
    Check(Present()==S_OK && Draws()==1,"unused helper cannot disable native capture");
}
void CaseQ3PreexistingFactory() {
    Bound(); auto* table=fx::world.probeFactory->vt; const void* original=table[15];
    Check(overlay::g_captureGeneration==1,"pre-existing factory captured");
    overlay::HookReturnedFactory(reinterpret_cast<IUnknown*>(fx::world.probeFactory),"late");
    auto* next=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=next;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(CookieOf(next) && overlay::g_captureGeneration==2 && CookieOf(fx::world.dxgiChain),"late observation still has one owner per creation");
    Check(table[15]==original && fx::Attempts(reinterpret_cast<void*>(&fx::FactoryHwnd))==1,"late observation never repatches provider");
}
void CaseQ3DuplicateObservation() {
    Bound(); auto c=CookieOf(fx::world.dxgiChain); auto* attachment=fx::world.dxgiChain->cookie;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(overlay::g_captureGeneration==1 && CookieOf(fx::world.dxgiChain)==c && fx::world.dxgiChain->cookie==attachment,"duplicate living object does not create another owner");
    Check(Present()==S_OK && Draws()==1,"existing generation still draws once");
}
void CaseQ3ResizeRecreate() {
    Bound(); Present(); Check(Resize()==S_OK && Present()==S_OK && Draws()==2,"native resize rebinds");
    auto* old=fx::world.dxgiChain; auto* next=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=next;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window);
    Check(Present(next)==S_OK && Draws()==3 && overlay::g_boundGeneration==2,"recreated generation activates once");
    Check(Present(old)==S_OK && Draws()==3,"old live generation cannot re-activate"); CheckClosed();
}
void CaseQ3NestedPresent() {
    Bound(); fx::world.dxgiChain->present1CallsPresent=true;
    Check(fx::GamePresent1(fx::world.dxgiChain->vt,reinterpret_cast<IDXGISwapChain1*>(fx::world.dxgiChain),1,0,nullptr)==S_OK,"nested Present1 result");
    Check(fx::world.dxgiChain->presents==1 && fx::world.dxgiChain->presents1==1 && Draws()==1,"both originals once, one WB frame");
}
void CaseQ3IndependentFail() {
    ResetFixture(); fx::legacyHookCreateStatus=MH_ERROR_ALREADY_CREATED; Install();
    Check(!overlay::g_factoryHookTargets[1].installed && !overlay::oCreateSwapChainForHwnd && overlay::g_gameBoundary.installed,"native conflict never invents an original or disables helper");
    Check(CreateOuterChain()!=nullptr && CookieOf(fx::world.slChain),"helper can capture final chain independently");
    auto* c=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=c;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); Check(!CookieOf(c),"unarmed method never captures raw call");
    fx::editorOpen=true; Check(Present(fx::world.slChain)==S_OK && Draws()==1,"helper binding draws");
}
void CaseQ3BothFail() {
    ResetFixture(); fx::legacyHookCreateStatus=MH_ERROR_MEMORY_ALLOC; fx::boundaryHookCreateStatus=MH_ERROR_MEMORY_ALLOC; Install();
    Check(!overlay::g_factoryHookTargets[1].installed && !overlay::g_gameBoundary.installed,"both routes honestly declined");
    Check(CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window)!=nullptr,"original still returns chain");
    fx::editorOpen=true; Check(Present()==S_OK && fx::world.dxgiChain->presents==1 && Draws()==0,"zero renders, original flow");
    Check(!CookieOf(fx::world.dxgiChain) && overlay::g_captureGeneration==0,"no readiness fabricated");
}
void CaseQ3Precedence() {
    for(bool accept:{false,true}) {
        ResetFixture(); Install(); fx::world.slFactory->innerFactory=nullptr; fx::world.slFactory->innerQueue=nullptr;
        auto* inner=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); fx::world.probeFactory->chain=inner;
        auto* final=fx::NewChain(g_window,fx::world.dxgiDev,1280,720,3,DXGI_FORMAT_R8G8B8A8_UNORM); final->cookieHr=accept?S_OK:E_FAIL;
        auto* ff=fx::NewFactory(final,fx::world.dxgiQueue);
        fx::boundaryInnerFactory=fx::world.slFactory; fx::boundaryInnerQueue=fx::world.slQueue;
        fx::boundaryRawFactory=fx::world.probeFactory; fx::boundaryRawQueue=fx::world.dxgiQueue; fx::boundaryFinalFactory=ff;
        void* host=fx::BuildBoundaryHost(fx::world.slFactory,fx::world.dxgiQueue,g_window,1280,720,0);
        Check(static_cast<HRESULT>(reinterpret_cast<intptr_t>(fx::RunBoundaryHelper(host)))==S_OK,"helper original success preserved");
        Check(!CookieOf(fx::world.slChain) && !CookieOf(inner),"neither observed nor raw inner creation promoted");
        Check(static_cast<bool>(CookieOf(final))==accept,"only final stored result can bind");
        Check(fx::world.slFactory->hwndCalls==1 && fx::world.probeFactory->hwndCalls==1 && ff->hwndCalls==1,"every downstream original exactly once");
        fx::editorOpen=true; Check(Present(final)==S_OK && Draws()==(accept?1u:0u),"final rejection never falls back to inner chain");
    }
    ResetFixture(); Install(); fx::world.dxgiChain->dev=fx::world.slDev2;
    CreateRawChain(fx::world.probeFactory,fx::world.dxgiQueue,g_window); Check(!CookieOf(fx::world.dxgiChain),"strict device identity remains mandatory on native route");
}
void CaseD1Starve() {
    Bound(); fx::inputDenyNext=64;
    for(int i=0;i<64;++i)Check(Present()==S_OK,"denied frame delegates original");
    Check(fx::world.dxgiChain->presents==64 && Draws()==0 && overlay::g_presents==0,"64 originals, zero admitted WB frames");
    Check(overlay::g_frameAttempts==64 && overlay::g_frameSkipped==64 && overlay::g_frameAdmitted==0 && overlay::g_frameConsecutiveSkips==64,"all denied attempts counted");
    Check(fx::hotkeyPressedCalls==0 && fx::world.dxgiChain->getBufferCalls==0,"denied frames consume no key or GPU work");
    fx::inputDenyNext=1; overlay::OnPresent(reinterpret_cast<IDXGISwapChain*>(0x10));
    Check(overlay::g_frameSkipped==65,"optional admission precedes even chain dereference");
    Check(Present()==S_OK && Draws()==1 && overlay::g_frameAdmitted==1 && overlay::g_frameConsecutiveSkips==0,"first free frame completes followup");
}
void CaseD1PendingTap() {
    Bound(); fx::pendingToggleTaps=1; fx::inputDenyNext=3;
    for(int i=0;i<3;++i)Check(Present()==S_OK,"skipped tap schedule delegates");
    Check(fx::pendingToggleTaps==1 && fx::toggleCalls==0 && fx::hotkeyPressedCalls==0 && Draws()==0,"tap survives denied frames");
    Check(Present()==S_OK && fx::toggleCalls==1 && fx::pendingToggleTaps==0 && fx::hotkeyPressedCalls==2 && Draws()==1,"first admission consumes exactly once");
    Check(Present()==S_OK && fx::toggleCalls==1 && fx::hotkeyPressedCalls==4,"next frame no duplicate action");
}
void CaseD1HomeBranch() {
    Bound(); fx::pendingModeTaps=1; Check(Present()==S_OK && fx::cameraModeCalls==1 && fx::togglePlayCalls==0,"edit enters camera");
    fx::editorPlaying=true; fx::pendingModeTaps=1; Check(Present()==S_OK && fx::togglePlayCalls==1 && fx::cameraModeCalls==1,"play returns to edit");
    fx::editorOpen=false; fx::editorPlaying=false; fx::pendingModeTaps=1;
    Check(Present()==S_OK && fx::cameraModeCalls==1 && fx::togglePlayCalls==1 && fx::pendingModeTaps==0,"closed editor ignores consumed mode action");
    Check(!core::g_menuOpen && fx::hotkeyPressedCalls==6,"closed frame releases ownership");
}
void CaseD1SameVk() {
    Bound(); fx::editorOpen=false; fx::applyToggleTransition=true;
    core::g_keyToggle=core::g_keyMode=VK_F11; fx::pendingToggleTaps=1;
    Check(Present()==S_OK,"same-VK frame delegates original");
    Check(fx::toggleCalls==2 && fx::cameraModeCalls==1 && fx::togglePlayCalls==0 && fx::togglePollsObserved==2,
          "same tap samples both actions before the first editor transition and selects camera");
    Check(fx::editorOpen && core::g_menuOpen && Draws()==1 && fx::hotkeyPressedCalls==2,
          "same-VK actions produce one open frame and two configured polls");
    Check(fx::pendingToggleTaps==0 && !fx::sharedHotkey,"same-VK tap is consumed without residual sharing");
    Check(Present()==S_OK && fx::toggleCalls==2 && fx::cameraModeCalls==1 && Draws()==2,
          "same-VK actions do not repeat on the next frame");
}
void CaseD1Simultaneous() {
    Bound(); fx::editorOpen=false; fx::applyToggleTransition=true;
    core::g_keyToggle=VK_F11; core::g_keyMode=VK_F2;
    fx::pendingToggleTaps=fx::pendingModeTaps=1;
    Check(Present()==S_OK,"distinct simultaneous-key frame delegates original");
    Check(fx::toggleCalls==2 && fx::cameraModeCalls==1 && fx::togglePlayCalls==0 && fx::togglePollsObserved==2,
          "both distinct taps are sampled before the first editor transition");
    Check(fx::editorOpen && core::g_menuOpen && Draws()==1 && fx::hotkeyPressedCalls==2,
          "distinct simultaneous actions produce one open frame and two configured polls");
    Check(fx::pendingToggleTaps==0 && fx::pendingModeTaps==0,"both distinct taps are consumed");
    Check(Present()==S_OK && fx::toggleCalls==2 && fx::cameraModeCalls==1 && Draws()==2,
          "distinct simultaneous actions do not repeat on the next frame");
}
void CaseClosedResearchFrame() {
    Bound();
    Check(Present()==S_OK && Draws()==1,"open-editor positive control submits");
    const auto open=fx::LastDraw();
    Check(open.menuOpen && open.wantsMouse && open.wantsKeyboard && open.policy.menuOpen,
          "the submission observer sees the open editor's input ownership");
    const unsigned opens=fx::menuOpenedCalls.load(), closes=fx::menuClosedCalls.load();
    fx::editorOpen=false;
    Check(core::DebugPointCount()==0 && Present()==S_OK && Draws()==1,
          "closed editor with zero research points delegates without submission");
    Check(!core::g_menuOpen && !core::g_uiWantsMouse && !core::g_uiWantsKeyboard &&
          !core::g_uiTextInput && !core::g_uiMouseOverUi && !overlay::g_wasOpen,
          "closing clears every core UI ownership flag");
    fx::debugPointCount=3;
    core::g_uiTextInput=core::g_uiMouseOverUi=true;
    Check(Present()==S_OK && Draws()==2,"closed editor with research points submits a frame");
    const auto research=fx::LastDraw();
    Check(!research.menuOpen && !research.wantsMouse && !research.wantsKeyboard &&
          !research.textInput && !research.mouseOverUi,
          "every core UI ownership flag is false at research-frame submission");
    const auto& policy=research.policy;
    Check(!policy.menuOpen && !policy.mouseToUi && !policy.keysToUi && !policy.placing &&
          !policy.textInput && !policy.mouseOverUi && !policy.play,
          "research-frame submission observes an entirely closed ownership policy");
    Check(fx::menuOpenedCalls==opens && fx::menuClosedCalls==closes+1,
          "research rendering does not reopen the menu or repeat its close notification");
    Check(overlay::PublishFrameOwnership(true,true),"closed frame accepts a stale backend capture observation");
    const auto stale=fx::OwnershipSnapshot().policy;
    Check(!core::g_uiTextInput && !core::g_uiMouseOverUi && !stale.textInput && !stale.mouseOverUi &&
          !stale.menuOpen && !stale.mouseToUi && !stale.keysToUi,
          "stale backend capture requests cannot acquire closed-editor input");
    fx::debugPointCount=0;
    Check(Present()==S_OK && Draws()==2 && fx::world.dxgiChain->presents==4,
          "removing the points stops submission while every original Present still runs");
    CheckNoTableWrites();
}
void CaseV097EditPlayCapture() {
    Bound(); fx::editorPlaying=true;
    Check(Present()==S_OK && Draws()==1,"visible play frame still submits");
    Check(overlay::PublishFrameOwnership(true,true),"play backend capture observation reaches production policy");
    auto policy=fx::OwnershipSnapshot().policy;
    Check(core::g_menuOpen && policy.menuOpen && policy.play && !policy.mouseToUi && !policy.keysToUi,
          "visible play retains menu state without claiming ordinary input");
    Check(!core::g_uiTextInput && !core::g_uiMouseOverUi && !policy.textInput && !policy.mouseOverUi,
          "play never claims backend text or hover capture");
    fx::editorPlacing=true;fx::editorMouseMode=true;
    Check(Present()==S_OK && Draws()==2 && overlay::PublishFrameOwnership(true,true),"play carried-gizmo frame submits");
    policy=fx::OwnershipSnapshot().policy;
    Check(policy.placing && policy.mouseToUi && !policy.keysToUi && !policy.textInput && !policy.mouseOverUi,
          "carried mouse ownership remains independent of edit-only capture");
    fx::editorMouseMode=false;
    Check(overlay::PublishFrameOwnership(true,true) && !fx::OwnershipSnapshot().policy.mouseToUi,
          "non-mouse carried mode does not acquire gizmo mouse input in play");
    fx::editorPlaying=false;
    Check(overlay::PublishFrameOwnership(true,true),"edit capture publishes");policy=fx::OwnershipSnapshot().policy;
    Check(policy.mouseToUi && policy.keysToUi && policy.textInput && policy.mouseOverUi && core::g_uiTextInput && core::g_uiMouseOverUi,
          "edit mode retains positive text hover and keyboard capture");
}
void CaseBoundsSplit() {
    ResetFixture(); overlay::discovery::ImageInfo core{},bind{};
    Check(overlay::discovery::ImageCoreBounds(reinterpret_cast<uintptr_t>(fx::image),&core) && overlay::discovery::ImageBounds(reinterpret_cast<uintptr_t>(fx::image),&bind),"both consumers accept original image");
    Check(core.base==bind.base && core.size==bind.size,"same bounded image");
    fx::imageSectionAlignment=0x2000;fx::imageFileAlignment=0x100;fx::BuildImage();
    Check(overlay::discovery::ImageCoreBounds(reinterpret_cast<uintptr_t>(fx::image),&core) && !overlay::discovery::ImageBounds(reinterpret_cast<uintptr_t>(fx::image),&bind),"core access independent of binding alignment");
    Install(); Check(!overlay::g_gameBoundary.installed && overlay::g_factoryMethodHookMask==15,"helper fails closed, real DXGI independently armed");
    ResetFixture();fx::imageImportDirRva=0xc000;fx::BuildImage();
    Check(overlay::discovery::ImageCoreBounds(reinterpret_cast<uintptr_t>(fx::image),&core) && !overlay::discovery::ImageBounds(reinterpret_cast<uintptr_t>(fx::image),&bind),"containing-section eligibility still binding-specific");
}
void CaseQ3ForwardOnce() {
    ResetFixture(); Install(); auto* f=fx::world.probeFactory; f->hwndHr=DXGI_ERROR_INVALID_CALL;
    auto d=MakeDesc(1920,1080,2); IDXGISwapChain1* out=nullptr;
    Check(fx::GameCallHwnd(f->vt,reinterpret_cast<IDXGIFactory2*>(f),reinterpret_cast<IUnknown*>(fx::world.dxgiQueue),g_window,&d,&out)==DXGI_ERROR_INVALID_CALL,"failure HRESULT unchanged");
    Check(f->hwndCalls==1 && !CookieOf(fx::world.dxgiChain),"failed original once, zero capture"); f->hwndHr=S_OK;
    Check(CreateRawChain(f,fx::world.dxgiQueue,g_window,2560,1440,3)!=nullptr && f->hwndCalls==2,"success original once");
    Check(fx::world.dxgiChain->w==2560 && fx::world.dxgiChain->h==1440 && fx::world.dxgiChain->buffers==3,"original arguments unchanged");
    Check(overlay::g_captureGeneration==1 && AssociatedQueue(fx::world.dxgiChain)==fx::world.dxgiQueue,"same-call queue and chain");
    // v0.95 intentionally permits the SL/ReShade forwarding caller. It still must
    // never adopt an unvalidated queue or re-capture a live object on that call.
    { fx::CallerScope foreign(fx::foreignModule); CreateRawChain(f,fx::world.slQueue,g_window); }
    Check(f->hwndCalls==3 && overlay::g_captureGeneration==1,"foreign forwarding original once, no wrong-device capture");
    Check(AssociatedQueue(fx::world.dxgiChain)==fx::world.dxgiQueue,"foreign call never changes selected association");
}
struct CaseEntry { const char* name; void (*run)(); };
static const CaseEntry kCases[] = {
    {"C1", &CaseMissedPath},
    {"C2", &CaseWrapperNesting},
    {"C3", &CaseProviderDispatch},
    {"C4", &CaseInterfaceAliases},
    {"C5", &CaseMultipleConcurrent},
    {"C6", &CaseCreationFilters},
    {"C7", &CaseSlotOwnership},
    {"C8", &CasePresentSelection},
    {"C9", &CaseCookieAccounting},
    {"C10", &CaseResizeGeneration},
    {"C11", &CaseConcurrencyGates},
    {"C12", &CaseDeadObject},
    {"A-PE", &CaseRealImageBounds},
    {"A-REALDIR", &CaseRealLayoutImportDir},
    {"W-WRAPCHURN", &CaseForeignWrapperChurn},
    {"G-C4IID", &CaseChain4IidConstant},
    {"G-BOUNDARY", &CaseGameSideBoundary},
    {"G-UNWIND", &CaseBoundaryUnwindChains},
    {"G-HEADSIG", &CaseBoundaryHeadSignatureRequired},
    {"G-REFSCAN", &CaseReferenceScanCompleteness},
    {"G-BOUNDARY-Q0", &CaseBoundaryQueueZeroThenReal},
    {"G-IDENT-AMEND", &CaseIdentityAmendment},
    {"G-MASK15", &CaseMask15Unauthorized},
    {"Q3-FALLBACK-RESOLVE", &CaseQ3FallbackResolve},
    {"Q3-FALLBACK-INSTALL", &CaseQ3FallbackInstall},
    {"Q3-NEVER-CALLED", &CaseQ3NeverCalled},
    {"Q3-PREEXISTING", &CaseQ3PreexistingFactory},
    {"Q3-DUPLICATE", &CaseQ3DuplicateObservation},
    {"Q3-RESIZE-RECREATE", &CaseQ3ResizeRecreate},
    {"Q3-NESTED-PRESENT", &CaseQ3NestedPresent},
    {"Q3-INDEPENDENT-FAIL", &CaseQ3IndependentFail},
    {"Q3-BOTH-FAIL", &CaseQ3BothFail},
    {"Q3-PRECEDENCE", &CaseQ3Precedence},
    {"Q3-FORWARD-ONCE", &CaseQ3ForwardOnce},
    {"D1-STARVE", &CaseD1Starve},
    {"D1-PENDING-TAP", &CaseD1PendingTap},
    {"D1-HOME", &CaseD1HomeBranch},
    {"D1-SAME-VK", &CaseD1SameVk},
    {"D1-SIMULTANEOUS", &CaseD1Simultaneous},
    {"MAIN-RESEARCH-CLOSED", &CaseClosedResearchFrame},
    {"V097-EDIT-PLAY-CAPTURE", &CaseV097EditPlayCapture},
    {"A-BOUNDSPLIT", &CaseBoundsSplit},
    {"A-PIN", &CasePinPrerequisites},
    {"A-ERR", &CaseResolverErrorPreservation},
    {"A-LOCK", &CaseLockExclusion},
    {"A-CAP", &CaseFactoryCaptureOwnership},
    {"A-PRIV", &CaseExistingPrivateEntry},
    {"B-QUEUE", &CaseResizeQueueCommit},
    {"B-REBIND", &CaseRebindPolicy},
    {"B-DEAD", &CaseDeadStorageReuse},
    {"C-EXT", &CaseExtensionAtomicity},
    {"C-CAP", &CaseCapacityLimits},
    {"C-ALIAS", &CaseAliasRouting},
    {"C-EDGE", &CaseHwndAndChainEdges},
    {"C-OVERLAP", &CaseResizeOverlap},
    {"R1-OVERLAP", &CaseResizeSameCookieOverlap},
    {"R1-FINISH", &CaseResizeBlockedFinish},
    {"R1-BARRIER", &CaseResizeBarrierHeldDraw},
    {"R6-DRAIN", &CaseDrainGateTimeout},
    {"R2-PINREENTRY", &CasePinReentry},
    {"R3-EXTFAIL", &CaseExtensionFailureMatrix},
    {"R5-PRIV", &CasePrivateDataBlobsAndFaults},
    {"R5-INTERLEAVE", &CaseAttachmentRetirementInterleave},
    {"R5-RECEIPT", &CaseReceiptCapacityFailClosed},
    {"R3-ALIASLOSS", &CaseAliasDependencyLoss},
    {"R3-SELFTHUNK", &CaseSelfThunkRejected},
    {"R5-PRESFAULT", &CasePresentQualifyFault},
    {"R1-RESFAULT", &CaseResizeLookupFault},
    {"R1-TICKETFAULT", &CaseEnteredTicketFault},
    {"R1-TICKETCONTEND", &CaseContendedTicketClose},
    {"R1-LEASEFAIL", &CaseLeaseAllocationFailure},
};
static std::string JsonEscape(const std::string& text) {
    std::string out; for(char c:text) { if(c=='"'||c=='\\')out+='\\'; if(c=='\n'||c=='\r')out+=' '; else out+=c; } return out;
}
#ifdef WB_NATIVE_MINHOOK_SMOKE
static int RunNativeSmoke(const char* resultPath,const char* logPath,const char* runId) {
    std::string status="PASS",error; bool nativeModule=false,realHook=false,unchanged=false;
    try {
        WNDCLASSEXA wc{};wc.cbSize=sizeof wc;wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName="wbNativeSmokeWindow";
        Check(RegisterClassExA(&wc)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"native class");
        HWND hwnd=CreateWindowExA(0,wc.lpszClassName,"overlay host",WS_POPUP,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
        Check(hwnd!=nullptr,"native hidden window");
        IDXGIFactory2* factory=nullptr; Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory,"preexisting DXGI factory");
        IDXGIFactory4* factory4=nullptr; Check(SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) && factory4,"factory4 alias");
        IDXGIAdapter* warp=nullptr; Check(SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp,"WARP adapter");
        ID3D12Device* device=nullptr; Check(SUCCEEDED(D3D12CreateDevice(warp,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))) && device,"WARP device");
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue=nullptr;ID3D12CommandQueue* queue2=nullptr;
        Check(SUCCEEDED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue))) && queue,"first DIRECT queue");
        Check(SUCCEEDED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue2))) && queue2,"second DIRECT queue");
        void** factoryVt=*reinterpret_cast<void***>(factory); const std::vector<void*> factoryBytes(factoryVt,factoryVt+25);
        Check(MH_Initialize()==MH_OK,"real MinHook initialized"); ResetProduction();
        core::g_base=reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        overlay::g_testFrameSink=&fx::DrawSink;overlay::g_testDrainFn=&fx::DrainHook;overlay::g_testRebindFn=&fx::RebindSeam;
        overlay::Install();
        Check(overlay::g_factoryMethodHookMask==15 && overlay::g_factoryHookTargets[1].installed,"v0.95 real-DXGI probe hooks all four methods");
        nativeModule=_stricmp(overlay::ModuleNameForAddress(overlay::g_factoryHookTargets[1].target).c_str(),"dxgi.dll")==0;
        Check(nativeModule,"chosen method belongs to real dxgi.dll");
        realHook=MH_Initialize()==MH_ERROR_ALREADY_INITIALIZED && overlay::oCreateSwapChainForHwnd!=nullptr;
        Check(realHook,"real MinHook trampoline");
        Check(overlay::g_moduleHandle==&GetModuleHandleW && overlay::g_exportAddress==&GetProcAddress && !overlay::g_testPinTarget && !overlay::g_testNativeInterface,"native OS/unwrap boundaries not substituted");
        overlay::HookReturnedFactory(factory,"second observation");
        Check(std::equal(factoryBytes.begin(),factoryBytes.end(),factoryVt),"native factory vtable byte-identical");
        auto desc=MakeDesc(640,480,2); IDXGISwapChain1* chain=nullptr;
        Check(SUCCEEDED(factory->CreateSwapChainForHwnd(queue,hwnd,&desc,nullptr,nullptr,&chain)) && chain,"real creation through preexisting factory");
        IDXGISwapChain3* view=nullptr; Check(SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&view))) && view,"real chain3 view");
        void** chainVt=*reinterpret_cast<void***>(view); const std::vector<void*> chainBytes(chainVt,chainVt+40);
        auto capture=overlay::LookupCaptured(chain); Check(capture && capture->generation==1 && capture->creationQueue->queue==queue,"real capture pair exactly once");
        UINT entry=0; Check(chain->GetPrivateData(overlay::kCaptureGuid,&entry,nullptr)==S_OK,"real lifetime attachment");
        fx::editorOpen=true; Check(SUCCEEDED(chain->Present(1,0)) && Draws()==1 && fx::LastDraw().queue==queue,"real Present routes one frame");
        Check(SUCCEEDED(chain->ResizeBuffers(2,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0)),"real ResizeBuffers delegates");
        Check(SUCCEEDED(chain->Present(1,0)) && Draws()==2 && overlay::g_width==800,"real resize rebind");
        UINT nodes[2]={1,1}; IUnknown* queues[2]={queue,queue2};
        Check(SUCCEEDED(view->ResizeBuffers1(2,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0,nodes,queues)),"real per-buffer queue resize");
        Check(capture->presentQueueCount==2,"both real queues retained by buffer");
        const UINT index=view->GetCurrentBackBufferIndex();
        Check(SUCCEEDED(chain->Present(1,0)) && Draws()==3 && fx::LastDraw().queue==queues[index],"native frame uses current-buffer queue");
        const auto revision=capture->revision;
        Check(FAILED(chain->ResizeBuffers(1,800,600,DXGI_FORMAT_R8G8B8A8_UNORM,0)) && capture->revision==revision,"real invalid resize retains association");
        Check(SUCCEEDED(chain->Present(1,0)) && Draws()==4,"real failed-resize recovery");
        unchanged=std::equal(chainBytes.begin(),chainBytes.end(),chainVt) && std::equal(factoryBytes.begin(),factoryBytes.end(),factoryVt);
        Check(unchanged,"all native factory/chain vtable cells unchanged");
        view->Release();view=nullptr; Check(chain->Release()==0,"capture retains no swapchain reference");chain=nullptr;
        Check(!capture->live,"DXGI destruction retires attachment"); capture.reset();
        IDXGISwapChain1* replacement=nullptr;
        Check(SUCCEEDED(factory->CreateSwapChainForHwnd(queue2,hwnd,&desc,nullptr,nullptr,&replacement)) && replacement,"real display-mode recreation");
        Check(SUCCEEDED(replacement->Present(1,0)) && Draws()==5 && overlay::g_boundGeneration==2 && fx::LastDraw().queue==queue2,"fresh generation repins creation queue");
        Check(overlay::g_captureGeneration==2 && overlay::g_helperCalls==0,"two native creations, no helper/double capture");
        CheckClosed(); Check(replacement->Release()==0,"replacement owns no chain cycle"); DropActiveRenderer();
        Check(MH_DisableHook(MH_ALL_HOOKS)==MH_OK,"native hooks disabled");
        std::set<void*> targets;
        for(const auto& hook:overlay::g_dxgiExportHookTargets)if(hook.installed)targets.insert(hook.target);
        for(const auto& hook:overlay::g_streamlineExportHookTargets)if(hook.installed)targets.insert(hook.target);
        for(const auto& hook:overlay::g_factoryHookTargets)if(hook.installed)targets.insert(hook.target);
        for(const auto* hook:{&overlay::g_presentHookTarget,&overlay::g_present1HookTarget,&overlay::g_resizeHookTarget,&overlay::g_resize1HookTarget,&overlay::g_colorSpaceHookTarget,&overlay::g_gameBoundary})if(hook->installed)targets.insert(hook->target);
        for(void* target:targets)Check(MH_RemoveHook(target)==MH_OK,"each owned native target removed");
        Check(MH_Uninitialize()==MH_OK,"native MinHook teardown");
        queue2->Release();queue->Release();device->Release();warp->Release();factory4->Release();factory->Release();
        Check(DestroyWindow(hwnd)!=0,"native window destroyed");
    } catch(const std::exception& ex) {status="FAIL";error=ex.what();}
    if(logPath)SaveLogs(std::filesystem::path(logPath));
    if(resultPath) {
        std::ofstream out(resultPath);
        out<<"{\"suite\":\"OverlayBinding\",\"mode\":\"NativeMinHookSmoke\",\"runId\":\""<<JsonEscape(runId)<<"\",\"status\":\""<<status<<"\",\"assertions\":"<<assertions
           <<",\"caseCount\":1,\"error\":\""<<JsonEscape(error)<<"\",\"legacy\":{\"route\":\"v0.95 real-DXGI method hook\"},\"nativeDxgi\":"<<(nativeModule?"true":"false")<<",\"realMinHook\":"<<(realHook?"true":"false")
           <<",\"vtableBytesUnchanged\":"<<(unchanged?"true":"false")<<",\"hostObservedOnly\":true,\"liveClaim\":false,\"realRegions\":[\"DXGI export and method hooks\",\"WARP device and two DIRECT queues\",\"swapchain capture and attachment retirement\",\"Present, ResizeBuffers, ResizeBuffers1 and failed-resize recovery\"],\"substitutedGpuRegions\":[\"frame sink\",\"WB queue drain\",\"render target allocator rebuild\"]}\n";
    }
    if(status!="PASS")std::cout<<"FAIL: "<<error<<'\n';
    std::cout<<"CASES=1\nASSERTIONS="<<assertions<<'\n';return status=="PASS"?0:1;
}
#endif
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
#ifdef WB_NATIVE_MINHOOK_SMOKE
    const char* result=nullptr;const char* log=nullptr;const char* id="native";
    for(int i=1;i<argc;++i) { if(std::strcmp(argv[i],"--result")==0 && i+1<argc)result=argv[++i]; else if(std::strcmp(argv[i],"--log")==0 && i+1<argc)log=argv[++i]; else if(std::strcmp(argv[i],"--run-id")==0 && i+1<argc)id=argv[++i]; }
    return RunNativeSmoke(result,log,id);
#else
    if(argc>1) {evidence=argv[1];std::filesystem::create_directories(evidence);}
    fx::dxgiModule=GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};wc.cbSize=sizeof wc;wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName="wbHostWindow";
    if(!RegisterClassExA(&wc))return 2;
    g_window=CreateWindowExA(0,wc.lpszClassName,"wb1",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,wc.hInstance,nullptr);
    g_window2=CreateWindowExA(0,wc.lpszClassName,"wb2",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,wc.hInstance,nullptr);
    if(!g_window||!g_window2)return 2;
    char selectedBuf[1024]{};size_t selectedLen=0;getenv_s(&selectedLen,selectedBuf,sizeof selectedBuf,"WB_OB_CASES");
    const std::string selected=","+std::string(selectedBuf)+",";
    std::string failed,error;
    try {
        for(const auto& c:kCases) {
            if(selectedLen>1 && selected.find(","+std::string(c.name)+",")==std::string::npos)continue;
            failed=c.name;CaseHeader(c.name);const int before=assertions;c.run();
            completedCases.push_back({c.name,assertions-before});std::cout<<"CASE_PASS="<<c.name<<" CHECKS="<<(assertions-before)<<'\n';
        }
        Check(!completedCases.empty(),"at least one registered case executed"); failed.clear();
    } catch(const std::exception& ex) {error=ex.what();std::cout<<"FAIL: "<<failed<<": "<<error<<'\n';}
    if(!evidence.empty()) {
        SaveLogs(evidence/"overlay_binding.log");
        std::ofstream out(evidence/"overlay_binding.cases.json");
        out<<"{\"status\":\""<<(error.empty()?"PASS":"FAIL")<<"\",\"assertions\":"<<assertions<<",\"caseCount\":"<<completedCases.size()<<",\"failedCase\":\""<<failed<<"\",\"error\":\""<<JsonEscape(error)<<"\",\"cases\":[";
        for(size_t i=0;i<completedCases.size();++i){if(i)out<<',';out<<"{\"name\":\""<<completedCases[i].first<<"\",\"assertions\":"<<completedCases[i].second<<'}';}out<<"]}\n";
    }
    DestroyWindow(g_window2);DestroyWindow(g_window);
    if(!error.empty())return 1;
    std::cout<<"CASES="<<completedCases.size()<<"\nASSERTIONS="<<assertions<<'\n';return 0;
#endif
}
