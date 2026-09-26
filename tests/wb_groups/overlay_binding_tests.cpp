// Host seam for WB_BINDING_FUNCTIONAL (Design B). The real production overlay.cpp is compiled here (included,
// the same convention ui_shell_host.cpp uses for cdmodkit.cpp); substituted are only OS protection/module-lifetime
// APIs and the external COM/graphics operations (in-memory PE import fixture, callable fake COM vtables, a draw
// sink and a drain hook at the GPU boundary). The production import walker, CAS publication, typed thunk dispatch,
// return observers, cookie lifecycle, generation selection and present/resize routing all run for real.
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
    void* BoundaryHelperImplModel(void* self);   // the model body the trampoline returns to

    std::vector<std::string> logs;
    void Capture(const std::string& line) { logs.push_back(line); }
    void ClearLogs() { logs.clear(); }
    std::vector<std::string> LinesWith(const std::string& token) {
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
    int stage = 0; int faultAt = 0;
    void StageGate() {
        ++stage;
        if (faultAt && stage == faultAt) RaiseException(0xC0000005, 0, 0, nullptr);
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
    HMODULE OsPinModule(HMODULE mod) {
        pinAttempts.push_back(mod);
        if (mod && mod == pinBlockModule && pinBlockCount > 0) { pinBlockCount--; BlockPinGate(); }
        if (pinFails.count(mod)) return nullptr;          // prerequisite failure path
        pins.push_back(mod);
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
        UINT getBufferCalls = 0;
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

    long addRefs = 0, releases = 0, destroyed = 0;
    long queueDescCalls = 0, chainDescCalls = 0, deviceNodeCalls = 0;

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
    HWND STDMETHODCALLTYPE ChainGetHwnd(IDXGISwapChain1* self);
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
        nt->OptionalHeader.SectionAlignment = 0x1000;
        nt->OptionalHeader.FileAlignment = 0x200;
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
    }
    void BuildImage() { BuildImageSized(kImageSize, kImageSize); }

    // ---- draw sink + drain hook (the only GPU boundary the host substitutes) ----
    struct DrawEvent { void* chain; void* queue; int generation; DWORD thread; };
    std::vector<DrawEvent> draws;
    std::mutex drawMutex;
    bool (*drainFn)() = nullptr;
    void DrawSink(IDXGISwapChain3* chain, ID3D12CommandQueue* queue, int generation) {
        std::lock_guard<std::mutex> l(drawMutex);
        draws.push_back({ chain, queue, generation, GetCurrentThreadId() });
    }
    size_t DrawCount() { std::lock_guard<std::mutex> l(drawMutex); return draws.size(); }
    DrawEvent LastDraw() { std::lock_guard<std::mutex> l(drawMutex); return draws.empty() ? DrawEvent{} : draws.back(); }

    struct RebindCall { void* chain; UINT buffers; UINT w; UINT h; DXGI_FORMAT format; };
    std::vector<RebindCall> rebindCalls;
    bool rebindFails = false;
    bool RebindSeam(IDXGISwapChain3* chain, UINT buffers, UINT w, UINT h, DXGI_FORMAT format) {
        rebindCalls.push_back({ chain, buffers, w, h, format });
        return !rebindFails;
    }
    bool editorOpen = false, editorPlaying = false, editorPlacing = false, editorCameraMode = false;
    long toggleCalls = 0;
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
}
namespace thumbgen {
int Generation() { return 0; }
std::vector<std::string> TakeRefreshed() { return {}; }
}
namespace input {
void Init(HWND) {}
void Shutdown() {}
void MenuOpened() {}
void MenuClosed() {}
void FeedMouse(ImGuiIO&) {}
}

namespace editor {
void Draw() {}
void Toggle() { fx::toggleCalls++; }   // the suite sets editorOpen directly: upstream's raw-key hotkey path cannot be pressed here
bool IsOpen() { return fx::editorOpen; }
void ApplyStyle(float) {}
bool PlayMode() { return fx::editorPlaying; }
void TogglePlay() { fx::toggleCalls++; }
void ToggleCameraMode() { fx::toggleCalls++; }   // upstream main's Home behaviour (inert in the host suite)
bool Placing() { return fx::editorPlacing; }
bool MouseMode() { return false; }
}

// MinHook boundary: input.cpp is linked for its window/key-state layer; this suite does not exercise its hook
// installation. The game-side creation boundary IS exercised: for the fixture's modeled helper the substitute
// patches a real 5-byte jump to the production detour and hands back the fixture model as the trampoline, exactly
// the contract MinHook provides ("call the original once through *orig").
#include "../../tools/minhook/include/MinHook.h"
MH_STATUS MH_CreateHook(void* target, void* detour, void** orig) {
    if (fx::boundaryTarget && target == fx::boundaryTarget) {
        unsigned char* p = static_cast<unsigned char*>(target);
        DWORD old = 0;
        if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old)) return MH_ERROR_MEMORY_ALLOC;
        // an absolute jump (mov rax, imm64 ; jmp rax): the modeled head's 36-byte prologue is verified by production
        // before this call, and a rel32 would be range-limited between the fixture image and the test module.
        const uint64_t target64 = reinterpret_cast<uint64_t>(detour);
        p[0] = 0x48; p[1] = 0xB8;
        std::memcpy(p + 2, &target64, 8);
        p[10] = 0xFF; p[11] = 0xE0;
        DWORD tmp = 0;
        VirtualProtect(p, 16, old, &tmp);
        if (orig) *orig = reinterpret_cast<void*>(&fx::BoundaryHelperImplModel);
        fx::boundaryHooked = true;
        return MH_OK;
    }
    (void)target; if (orig) *orig = nullptr; return MH_ERROR_FUNCTION_NOT_FOUND;
}
MH_STATUS MH_EnableHook(void* target) {
    if (fx::boundaryTarget && target == fx::boundaryTarget) return MH_OK;
    (void)target; return MH_ERROR_DISABLED;
}

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
        return outerHwndImpl(self, dev, hwnd, d, fs, out, pp);
    }
    // Set the nested ("inner") creation of the wrapper to go through this factory table with this queue, which is
    // how a wrapper reaches the native factory it wrapped: the inner call then passes through an observed table.
    void SetWrapperInner(Factory* innerFactory, Queue* innerQueue);
    bool PatchLockIsFree() {
        const bool free = overlay::g_patchMutex.try_lock();
        if (free) overlay::g_patchMutex.unlock();
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
        {   // inside the capture/observer path: the render boundary must be free
            const bool free = overlay::g_renderMutex.try_lock();
            renderFreeDuringPatch.store(free ? 1 : 0);
            if (free) overlay::g_renderMutex.unlock();
        }
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
    HWND STDMETHODCALLTYPE ChainGetHwnd(IDXGISwapChain1* self) { return reinterpret_cast<Chain*>(self)->hwnd; }
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
        return reinterpret_cast<overlay::FactoryCreateSwapChain_t>(inner->vt[10])(reinterpret_cast<IDXGIFactory*>(inner), dev, d, pp);
    }
    HRESULT STDMETHODCALLTYPE WrapperFactoryHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        Factory* f = reinterpret_cast<Factory*>(self);
        Factory* inner = f->innerFactory;
        if (!inner) { if (pp) *pp = nullptr; return E_FAIL; }
        return reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(inner->vt[15])(reinterpret_cast<IDXGIFactory2*>(inner), dev, hwnd, d, fs, out, pp);
    }

    // The model body the trampoline returns to: reads the game's own members, calls the wrapper factory's slot 15
    // exactly like the inspected build, and stores the canonical-IDXGISwapChain4 QI of the created wrapper swapchain
    // in this+0xa8 (the game keeps that reference).
    void* BoundaryHelperImplModel(void* self) {
        if (!self) return reinterpret_cast<void*>(static_cast<intptr_t>(E_FAIL));
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
        IDXGISwapChain1* out = nullptr;
        void** vt = nullptr;
        std::memcpy(&vt, factory, sizeof vt);                 // the object's vptr, then slot 15 (the inspected dispatch shape)
        if (!vt || !vt[15]) return reinterpret_cast<void*>(static_cast<intptr_t>(E_FAIL));
        const HRESULT hr = reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(vt[15])(
            reinterpret_cast<IDXGIFactory2*>(factory), reinterpret_cast<IUnknown*>(queue), hwnd, &d, nullptr, nullptr, &out);
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

    // ---- game-side call shims: the fixture declares these addresses to be inside the game image, so the
    //      production caller filter still runs its own module comparison. ----
    HRESULT GameCreateFactory0(REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::FactoryCreateFn>(*cellFactory0)(iid, pp); }
    HRESULT GameCreateFactory1(REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::FactoryCreateFn>(*cellFactory1)(iid, pp); }
    HRESULT GameCreateFactory2(UINT flags, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::FactoryCreate2Fn>(*cellFactory2)(flags, iid, pp); }
    FARPROC GameGetProcAddress(HMODULE mod, LPCSTR name) { CallerScope scope(mainModule); return reinterpret_cast<overlay::GetProcAddressFn>(*cellResolver)(mod, name); }
    HRESULT GameCallFnA(void* fn, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::FactoryCreateFn>(fn)(iid, pp); }
    HRESULT GameCallFnC(void* fn, UINT flags, REFIID iid, void** pp) { CallerScope scope(mainModule); return reinterpret_cast<overlay::FactoryCreate2Fn>(fn)(flags, iid, pp); }
    HRESULT GameCallHwnd(void** table, IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d, IDXGISwapChain1** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::FactoryCreateSwapChainForHwnd_t>(table[15])(self, dev, hwnd, d, nullptr, nullptr, pp);
    }
    HRESULT GameCallCreate(void** table, IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::FactoryCreateSwapChain_t>(table[10])(self, dev, d, pp);
    }
    HRESULT GameCallQi(void** table, IUnknown* self, REFIID iid, void** pp) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::FactoryQi_t>(table[0])(self, iid, pp);
    }
    HRESULT GamePresent(void** table, IDXGISwapChain* self, UINT sync, UINT flags) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::Present_t>(table[8])(self, sync, flags);
    }
    HRESULT GamePresent1(void** table, IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::Present1_t>(table[22])(self, sync, flags, p);
    }
    HRESULT GameResize(void** table, IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::ResizeBuffers_t>(table[13])(self, n, w, h, fmt, flags);
    }
    HRESULT GameResize1(void** table, IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues) {
        CallerScope scope(mainModule);
        return reinterpret_cast<overlay::ResizeBuffers1_t>(table[39])(self, n, w, h, fmt, flags, nodes, queues);
    }
    // Converts an escaped added-work fault into a failed assertion instead of a crashed suite (SEH-only catcher, no
    // unwinding objects in this wrapper; used by the fault schedules to run the same call on the pre-fix source).
    bool CallPresentCatchingFault(void** table, IDXGISwapChain* self, UINT sync, UINT flags) {
        __try { GamePresent(table, self, sync, flags); return false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------------------------------------
namespace {
int assertions = 0;
std::filesystem::path evidence;
void Check(bool ok, const std::string& what) { ++assertions; if (!ok) throw std::runtime_error(what); }
void CaseHeader(const char* name) { std::cout << "CASE: " << name << "\n"; }

HWND g_window = nullptr, g_window2 = nullptr;

DXGI_SWAP_CHAIN_DESC1 MakeDesc(UINT w, UINT h, UINT buffers) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferCount = buffers; d.SampleDesc.Count = 1;
    return d;
}
// The record structs gained a per-table terminal-log flag with this repair; the RED baseline (the pre-fix source)
// does not have it, so the reset is feature-detected instead of named directly.
template<typename T, typename = void> struct HasTerminalLogged : std::false_type {};
template<typename T> struct HasTerminalLogged<T, decltype((void)std::declval<T&>().terminalLogged, void())> : std::true_type {};
template<typename T> void ResetTerminalLogged(T& r) { if constexpr (HasTerminalLogged<T>::value) r.terminalLogged.store(0); }

void ResetProduction() {
    { std::lock_guard<std::mutex> l(overlay::g_receiptMutex); for (auto& r : overlay::g_receipts) r = overlay::Receipt{}; }   // a fresh fixture has no live bindings
    if (overlay::g_activeCookie) { overlay::g_activeCookie->Release(); overlay::g_activeCookie = nullptr; }
    overlay::g_queue = nullptr;                              // borrowed alias: the active lease owns the reference
    overlay::g_activeLease.reset();
    overlay::g_swapChain = nullptr; overlay::g_hwnd = nullptr;
    overlay::g_ready = false; overlay::g_failed = false; overlay::g_disabled = false; overlay::g_forceRebind = false;
    overlay::g_overlayStop.store(false);
    overlay::g_resizeCalls = 0;
    overlay::g_wasOpen = false; overlay::g_selectedHwnd = nullptr; overlay::g_activeGeneration = 0;
    overlay::g_activationCount.store(0); overlay::g_frameCount.store(0); overlay::g_presents.store(0);
    overlay::g_generationCounter.store(0);
    overlay::g_resolverSaved.store(nullptr);
    for (int i = 0; i < overlay::kMaxFnRecs; i++) {
        overlay::g_fnRecs[i].saved.store(nullptr); overlay::g_fnRecs[i].thunk.store(nullptr);
        overlay::g_fnRecs[i].origin.store(0); overlay::g_fnRecs[i].flavor.store(0);
        overlay::g_fnRecs[i].provider = nullptr; overlay::g_fnRecs[i].name = nullptr;
    }
    for (int i = 0; i < overlay::kMaxFactoryVt; i++) {
        overlay::FactoryVtRec& r = overlay::g_factoryVt[i];
        r.vtable.store(nullptr); r.savedQi.store(nullptr); r.savedCreate.store(nullptr); r.savedHwnd.store(nullptr);
        r.state.store(0); r.coverage.store(0); ResetTerminalLogged(r); r.ownedQi.store(0); r.ownedCreate.store(0); r.ownedHwnd.store(0); r.lost.store(0);
    }
    for (int i = 0; i < overlay::kMaxChainVt; i++) {
        overlay::ChainVtRec& r = overlay::g_chainVt[i];
        r.vtable.store(nullptr); r.savedQi.store(nullptr); r.savedPresent.store(nullptr); r.savedPresent1.store(nullptr);
        r.savedResize.store(nullptr); r.savedResize1.store(nullptr);
        r.state.store(0); r.coverage.store(0); ResetTerminalLogged(r); r.ownedQi.store(0); r.ownedPresent.store(0); r.ownedPresent1.store(0); r.ownedResize.store(0); r.ownedResize1.store(0);
        r.lost.store(0);
    }
    overlay::t_createDepth = 0; overlay::t_presentDepth = 0; overlay::t_observerDepth = 0;
}
// The renderer's own state leaves through this helper: metadata reference and the active lease (the borrowed
// g_queue alias is never released by anyone but the lease).
void DropActiveRenderer() {
    if (overlay::g_activeCookie) { overlay::g_activeCookie->Release(); overlay::g_activeCookie = nullptr; }
    overlay::g_queue = nullptr;
    overlay::g_activeLease.reset();
}
void ResetFixture() {
    fx::ClearLogs();
    fx::protectCalls.clear();
    fx::pins.clear();
    fx::addRefs = fx::releases = fx::destroyed = 0;
    fx::failProtectCell = nullptr; fx::failRestore = false; fx::conflictCell = nullptr; fx::conflictValue = nullptr;
    fx::pauseCell = nullptr;
    { std::lock_guard<std::mutex> l(fx::gateMutex); fx::paused = false; fx::resume = false; fx::gateTimeout = false; }
    fx::directCalls[0] = fx::directCalls[1] = fx::directCalls[2] = 0;
    fx::directHr[0] = fx::directHr[1] = fx::directHr[2] = S_OK;
    fx::directNullOutput[0] = fx::directNullOutput[1] = fx::directNullOutput[2] = false;
    fx::resolveCalls = 0; fx::slCreateCalls = 0; fx::slCreateFail = false; fx::slCreateHr = E_FAIL; fx::innerCreateCalls = 0;
    fx::resolverFails = false; fx::resolverForeign = false;
    fx::lastDirectFactory = nullptr; fx::lastSlFactory = nullptr; fx::lastReturnedChain = nullptr;
    fx::outerHwndImpl = &fx::FactoryHwnd;
    { std::lock_guard<std::mutex> l(fx::drawMutex); fx::draws.clear(); }
    fx::rebindCalls.clear(); fx::rebindFails = false; fx::chainPool.clear();
    fx::drainFn = nullptr;
    fx::drainWaitBoundMs = 10000;
    fx::ResetDrainState();
    fx::resizeWaitBoundMs = 10000;
    fx::ResetResizeGate();
    { std::lock_guard<std::mutex> l(fx::lookupMutex); fx::identityQueries = 0; }
    fx::pinBlockModule = nullptr; fx::pinBlockCount = 0; fx::ResetPinGate();
    fx::chain3QiBlockCount = 0; fx::ResetChain3QiGate(); fx::faultChain3Qi = 0; fx::faultIdentityQi = 0;
    fx::armAllocFailOnDesc = false; g_testFailNextAlloc.store(0, std::memory_order_release);
    fx::foreignBlobMode = -1; fx::payloadReads = 0;
    fx::stage = 0; fx::faultAt = 0;
    fx::editorOpen = false; fx::editorPlaying = false; fx::editorPlacing = false; fx::toggleCalls = 0;
    fx::unowned.clear();
    fx::moduleSpans.clear();
    fx::pinFails.clear(); fx::pinAttempts.clear();
    fx::moduleNames[fx::dxgiModule] = "dxgi.dll";        // the fixture's own code/tables stand for the dxgi provider
    fx::imageImportDirRva = 0; fx::imageImportDirSize = 0;
    fx::imageFirstThunkOverride = 0; fx::imageSizeOfHeaders = 0x400; fx::kernelModuleName = "KERNEL32.dll";
    fx::moduleNames[fx::slModule] = "sl.interposer.dll";
    fx::boundaryHooked = false; fx::boundaryHost = nullptr;
    fx::boundaryUnwindMode = 0; fx::boundaryHeadSigCorrupt = false;
    fx::patchFreeDuringRender.store(-1); fx::renderFreeDuringPatch.store(-1);
    fx::BuildImage();
    fx::BuildWorld();
    ResetProduction();
    overlay::g_os.virtualProtect = &fx::OsVirtualProtect;
    overlay::g_os.moduleFromAddress = &fx::OsModuleFromAddress;
    overlay::g_os.moduleFileName = &fx::OsModuleFileName;
    overlay::g_os.pinModule = &fx::OsPinModule;
    overlay::g_os.mainImageBase = &fx::OsMainImageBase;
    overlay::g_mainModule = nullptr;
    overlay::g_testFrameSink = &fx::DrawSink;
    overlay::g_testDrainFn = &fx::DrainHook;
    overlay::g_testRebindFn = &fx::RebindSeam;
}
void Install() { overlay::Install(); }
size_t Draws() { return fx::DrawCount(); }
overlay::BindingCookie* CookieOf(fx::Chain* c) {
    if (!c) return nullptr;
    return overlay::CookieOfChain(reinterpret_cast<void*>(c));   // the production live-receipt lookup, owned metadata reference
}
void* AssociatedQueue(fx::Chain* c) {
    overlay::BindingCookie* k = CookieOf(c);
    if (!k) return nullptr;
    const std::shared_ptr<const overlay::QueueLease>& lease = k->renderLease ? k->renderLease : k->creationLease;
    void* q = lease ? reinterpret_cast<void*>(lease->queue) : nullptr;
    k->Release();
    return q;
}
bool QueueUnsupported(fx::Chain* c) {
    overlay::BindingCookie* k = CookieOf(c);
    if (!k) return false;
    const bool u = k->renderQueueUnsupported;
    k->Release();
    return u;
}
// Releases a paused publication/drain gate and joins the worker on every path: an assertion failure must not
// leave a joinable thread behind (std::terminate would hide the failure). A production lock the test holds is
// released before the join, on every assertion path, so a worker that needs that lock can finish.
struct WorkerGuard {
    std::thread* t = nullptr;
    std::unique_lock<std::mutex>* held = nullptr;
    ~WorkerGuard() {
        if (held && held->owns_lock()) held->unlock();
        if (t && t->joinable()) {
            fx::ReleasePause();
            fx::ReleaseDrain();
            fx::ReleaseResizeOriginal();
            fx::ReleasePinGate();
            fx::ReleaseChain3Qi();
            t->join();
        }
    }
};
std::string OneLine(const std::string& token) {
    auto v = fx::LinesWith(token);
    Check(v.size() == 1, "exactly one line for token " + token + " (got " + std::to_string(v.size()) + ")");
    return v[0];
}
bool HasToken(const std::string& token) { return fx::CountWith(token) > 0; }
// The standard "SL resolves its factory and the game creates its swapchain" path for a case.
FARPROC ResolveSlFactory2() { return fx::GameGetProcAddress(fx::slModule, "CreateDXGIFactory2"); }
IDXGIFactory4* CreateSlFactory(FARPROC thunk) {
    IDXGIFactory4* f4 = nullptr;
    fx::GameCallFnC(reinterpret_cast<void*>(thunk), 0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f4));
    return f4;
}
IDXGISwapChain1* CreateOuterChain(HWND hwnd, UINT w, UINT h, UINT buffers) {
    DXGI_SWAP_CHAIN_DESC1 desc = MakeDesc(w, h, buffers);
    IDXGISwapChain1* pp = nullptr;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), hwnd, &desc, &pp);
    return pp;
}
void DetachCookie(fx::Chain* c) {   // the chain drops its private data (DXGI object destruction)
    if (c->cookie) { c->cookie->Release(); c->cookie = nullptr; }
    c->hasCookie = false;
}
}

// ---------------------------------------------------------------------------------------------------------
// Case groups (functional-correction-design.md section 8)
// ---------------------------------------------------------------------------------------------------------

void CaseMissedPath() {
    CaseHeader("C1 missed path: resolver-returned SL factory tapped, outer chain captured, production Present routes the draw");
    ResetFixture();
    Install();
    Check(*fx::cellFactory2 != reinterpret_cast<void*>(&fx::ExportCreateFactory2), "C1: the main module's CreateDXGIFactory2 import cell is tapped");
    Check(*fx::cellFactory1 != reinterpret_cast<void*>(&fx::ExportCreateFactory1), "C1: CreateDXGIFactory1 is tapped");
    Check(*fx::cellResolver != reinterpret_cast<void*>(&fx::NativeGetProcAddress), "C1: the GetProcAddress import cell is tapped");
    const std::string inst = OneLine("WB_BINDING_FUNCTIONAL install pid");
    Check(fx::Field(inst, "factory_create") == "owned" && fx::Field(inst, "factory_create1") == "owned" && fx::Field(inst, "factory_create2") == "owned", "C1: the installer reports all three factory taps as owned");
    Check(fx::Field(inst, "resolver") == "owned", "C1: the installer reports the resolver tap as owned");
    FARPROC thunk = ResolveSlFactory2();
    Check(thunk && thunk != reinterpret_cast<FARPROC>(&fx::SlCreateFactory2), "C1: the resolver tap returned a factory-return thunk for SL's export");
    Check(HasToken("resolve api CreateDXGIFactory2"), "C1: the resolver substitution is logged");
    IDXGIFactory4* f4 = CreateSlFactory(thunk);
    Check(f4 == reinterpret_cast<IDXGIFactory4*>(fx::world.slFactory), "C1: the thunk returned SL's final factory unchanged");
    Check(fx::slCreateCalls == 1, "C1: the downstream SL export ran exactly once");
    Check(fx::directCalls[2] == 0, "C1: no DXGI/dummy factory creation happened at all");
    Check(fx::world.dxgiFactory[2]->vt[15] == reinterpret_cast<void*>(&fx::FactoryHwnd), "C1: the unused direct factory's table stayed untouched");
    const std::string fr = OneLine("factory_return api IDXGIFactory4");
    Check(fx::Field(fr, "slots") == "ready", "C1: the observed SL factory table reported ready");
    Check(fx::world.slFactory->vt[15] != reinterpret_cast<void*>(&fx::SlFactoryHwnd), "C1: the SL factory's vt15 now holds the production thunk");
    Check(fx::world.slFactory->vt[0] != reinterpret_cast<void*>(&fx::FactoryQi), "C1: the SL factory's QI slot is interposed");
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "C1: the creation returned the final SL chain with its original HRESULT");
    Check(HasToken("create entry Hwnd outer 1"), "C1: the outermost creation was recognized as the outer game call");
    const std::string pair = OneLine("pair generation 1");
    Check(fx::Field(pair, "ready") == "1", "C1: the pairing reported ready");
    Check(fx::Field(pair, "chain") == fx::PtrText(fx::world.slChain), "C1: the pair names the returned chain");
    Check(fx::Field(pair, "queue") == fx::PtrText(fx::world.slQueue), "C1: the pair names the queue from the same creation call");
    Check(fx::Field(pair, "caller_module") == "crimsondesert.exe", "C1: the creation caller's module provenance is the game image");
    Check(fx::world.slChain->hasCookie, "C1: the binding cookie was attached to the returned chain");
    Check(fx::world.slChain->vt[8] != reinterpret_cast<void*>(&fx::ChainPresent), "C1: the returned chain's Present slot is interposed");
    Check(fx::world.slChain->vt[13] != reinterpret_cast<void*>(&fx::ChainResize), "C1: the returned chain's ResizeBuffers slot is interposed");
    Check(fx::world.slChain->vt[39] != reinterpret_cast<void*>(&fx::ChainResize1), "C1: ResizeBuffers1 is interposed");
    Check(overlay::g_queue == nullptr, "C1: no renderer state was written from the creation thread");
    Check(overlay::g_activationCount.load() == 0, "C1: no activation before an eligible Present");
    fx::editorOpen = true;
    HRESULT hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(hr == S_OK && fx::world.slChain->presents == 1, "C1: the game's Present was delegated exactly once");
    Check(Draws() == 1, "C1: exactly one first draw through the production activation path");
    Check(fx::LastDraw().chain == fx::world.slChain && fx::LastDraw().queue == fx::world.slQueue, "C1: the draw used the paired chain/queue");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C1: the renderer queue came from the cookie, not from an arbitrary creation");
    Check(overlay::g_activationCount.load() == 1, "C1: one activation");
    Check(HasToken("outer_present 1"), "C1: the activation is logged as an outer present");
}

void CaseWrapperNesting() {
    CaseHeader("C2 wrapper nesting: only the outer pair commits; a later downstream behavior change is followed without re-patching");
    ResetFixture();
    Install();
    IDXGIFactory* native = nullptr;
    fx::GameCreateFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&native));   // the native factory the wrapper wrapped
    fx::SetWrapperInner(fx::world.dxgiFactory[2], fx::world.dxgiQueue);                      // the nested creation goes through that observed table, with another queue
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "C2: the outer chain was returned");
    Check(HasToken("create entry Hwnd outer 0"), "C2: the nested creation was seen through an observed table and classified as not outer");
    Check(HasToken("create entry Hwnd outer 1"), "C2: the outer creation was classified as the outer game call");
    Check(!fx::world.dxgiChain->hasCookie, "C2: the nested inner chain got no cookie");
    Check(fx::world.slChain->hasCookie, "C2: the outer chain got the cookie");
    const std::string pair = OneLine("pair generation 1");
    Check(fx::Field(pair, "queue") == fx::PtrText(fx::world.slQueue), "C2: the committed queue is the outer call's queue, not the inner one");
    Check(fx::Field(pair, "chain") == fx::PtrText(fx::world.slChain), "C2: the committed chain is the outer chain");
    {
        bool innerInPair = false;
        for (const auto& l : fx::LinesWith("pair generation")) if (l.find(fx::PtrText(fx::world.dxgiQueue)) != std::string::npos) innerInPair = true;
        Check(!innerInPair, "C2: the inner queue never appears in a pairing event");
    }
    void** cell = &fx::world.slFactory->vt[15];
    void* cellBefore = *cell;
    fx::outerHwndImpl = &fx::AltFactoryHwnd;
    IDXGISwapChain1* pp2 = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(*cell == cellBefore, "C2: our cell is still ours after the downstream behavior changed (no re-patch)");
    Check(pp2 == reinterpret_cast<IDXGISwapChain1*>(fx::lastReturnedChain) && pp2 != pp, "C2: the call followed the changed downstream behavior and returned the new chain");
    Check(fx::lastReturnedChain->hasCookie, "C2: the newly returned chain was captured");
    Check(fx::world.slChain->hasCookie, "C2: the earlier chain keeps its own cookie");
    const std::string pair2 = OneLine("pair generation 2");
    Check(fx::Field(pair2, "chain") == fx::PtrText(fx::lastReturnedChain), "C2: the second pair names the new chain");
    {
        size_t slTable = 0;
        for (const auto& l : fx::LinesWith("factory_return api IDXGIFactory4")) if (l.find(fx::PtrText(fx::world.slFactory->vt)) != std::string::npos) slTable++;
        Check(slTable == 1, "C2: the SL factory table was observed once, not re-patched per creation");
    }
}

void CaseProviderDispatch() {
    CaseHeader("C3 provider dispatch: direct imports, all three names, wrong symbols/modules/ordinals/null pass through unchanged");
    ResetFixture();
    Install();
    IDXGIFactory* f0 = nullptr;
    HRESULT hr = fx::GameCreateFactory0(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f0));
    Check(hr == S_OK && f0 == reinterpret_cast<IDXGIFactory*>(fx::world.dxgiFactory[0]), "C3: the direct CreateDXGIFactory thunk returned the DXGI factory");
    Check(HasToken("factory_return api IDXGIFactory iid IDXGIFactory "), "C3: the direct import return was observed");
    f0 = nullptr;
    hr = fx::GameCreateFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f0));
    Check(hr == S_OK && f0 == reinterpret_cast<IDXGIFactory*>(fx::world.dxgiFactory[1]), "C3: CreateDXGIFactory1 has its own dispatch record and object");
    fx::directHr[2] = E_ACCESSDENIED;
    IDXGIFactory* f2 = nullptr;
    hr = fx::GameCreateFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f2));
    Check(hr == E_ACCESSDENIED && f2 == nullptr, "C3: a failing direct creation preserves its exact HRESULT and output");
    Check(!HasToken("factory_observed iid IDXGIFactory4"), "C3: a failed creation is never observed");
    FARPROC slThunk = fx::GameGetProcAddress(fx::slModule, "CreateDXGIFactory1");
    Check(slThunk != reinterpret_cast<FARPROC>(&fx::SlCreateFactory1), "C3: the SL CreateDXGIFactory1 result is a thunk");
    IDXGIFactory1* g1 = nullptr;
    hr = fx::GameCallFnA(reinterpret_cast<void*>(slThunk), __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&g1));
    Check(hr == S_OK && g1 == reinterpret_cast<IDXGIFactory1*>(fx::world.slFactory), "C3: the SL resolver path for CreateDXGIFactory1 produced SL's object");
    SetLastError(0x1111);
    FARPROC wrong = fx::GameGetProcAddress(fx::slModule, "Sleep");
    Check(wrong == nullptr && GetLastError() == 0x33u, "C3: an unknown symbol passes the resolver's null result and its last error through");
    FARPROC foreign = fx::GameGetProcAddress(fx::foreignModule, "CreateDXGIFactory2");
    Check(foreign == reinterpret_cast<FARPROC>(&fx::ForeignExport), "C3: a foreign provider module is never substituted");
    fx::resolverFails = true;
    SetLastError(0x2222);
    FARPROC none = fx::GameGetProcAddress(fx::slModule, "CreateDXGIFactory2");
    Check(none == nullptr && GetLastError() == 0x2A2Au, "C3: a null resolver result passes through with the resolver's own last error");
    fx::resolverFails = false;
    SetLastError(0x3333);
    FARPROC ord = fx::GameGetProcAddress(fx::slModule, MAKEINTRESOURCEA(7));
    Check(ord == nullptr && GetLastError() == 0x5151u, "C3: an ordinal request is delegated, never dereferenced as a string");
    Check(!HasToken("resolve api Sleep"), "C3: no substitution is logged for a wrong symbol");
    Check(fx::resolveCalls >= 5, "C3: the saved resolver ran for every request");
}

void CaseInterfaceAliases() {
    CaseHeader("C4 aliases: recognized QI results get their own record; higher slots stay byte-for-byte intact");
    ResetFixture();
    Install();
    fx::world.slFactory->AddAlias(__uuidof(IDXGIFactory2), fx::world.aliasFactory);
    fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3), fx::world.aliasChain);
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    void* aliasVt16 = fx::world.aliasFactory->vt[16];
    void* aliasVt0 = fx::world.aliasFactory->vt[0];
    IDXGIFactory2* alias = nullptr;
    HRESULT hr = fx::GameCallQi(fx::world.slFactory->vt, reinterpret_cast<IUnknown*>(fx::world.slFactory), __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&alias));
    Check(hr == S_OK && alias == reinterpret_cast<IDXGIFactory2*>(fx::world.aliasFactory), "C4: QI returned the wrapper's alias interface unchanged");
    Check(fx::world.aliasFactory->vt[0] != aliasVt0, "C4: the alias interface's own prefix QI slot was interposed");
    Check(fx::world.aliasFactory->vt[15] != reinterpret_cast<void*>(&fx::SlFactoryHwnd), "C4: the alias interface's creation slot was interposed");
    Check(fx::world.aliasFactory->vt[16] == aliasVt16, "C4: above the guaranteed prefix nothing changed (byte-for-byte)");
    Check(fx::world.slFactory->identity == fx::world.aliasFactory->identity, "C4: the alias shares the canonical IUnknown identity although the interface pointer differs");
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "C4: the creation still returned the outer chain");
    void* aliasChain40 = fx::world.aliasChain->vt[40];
    Check(fx::world.aliasChain->vt[39] != reinterpret_cast<void*>(&fx::ChainResize1), "C4: the chain3 alias result was observed and covered through slot 39");
    Check(fx::world.aliasChain->vt[40] == aliasChain40, "C4: chain methods above the guaranteed prefix are untouched");
    Check(HasToken("chain_table table") && HasToken("resize1 1"), "C4: the chain3 alias record declares its ResizeBuffers1 coverage");
}

void CaseMultipleConcurrent() {
    CaseHeader("C5 multiple/concurrent: idempotent observation, distinct delegates, publication barrier, no early rendering");
    ResetFixture();
    Install();
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    void** factorySlot0 = &fx::world.slFactory->vt[0];
    Check(fx::ProtectWritesFor(factorySlot0) == 1, "C5: the factory cell was written exactly once");
    CreateSlFactory(thunk);
    Check(fx::ProtectWritesFor(factorySlot0) == 1, "C5: observing the same table twice does not patch it again");
    Check(fx::CountWith("factory_return api IDXGIFactory4") == 1, "C5: one factory_return event per observed table");
    Check(fx::world.dxgiFactory[0]->vt[15] == reinterpret_cast<void*>(&fx::FactoryHwnd) && fx::world.dxgiFactory[1]->vt[15] == reinterpret_cast<void*>(&fx::FactoryHwnd), "C5: unrelated factory tables stay untouched");
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    std::atomic<bool> creatorDone{ false };
    IDXGISwapChain1* pA = nullptr;
    std::thread creator([&] {
        pA = CreateOuterChain(g_window, 2560, 1440, 3);
        creatorDone.store(true);
    });
    WorkerGuard guardA{ &creator };
    creator.join();
    Check(creatorDone.load() && pA == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "C5: the creator thread completed its creation");
    Check(overlay::g_queue == nullptr && overlay::g_activationCount.load() == 0, "C5: creation never writes rendering globals");
    // publication barrier: pause inside the first cell transaction of a fresh table, then present concurrently
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());                            // observe the factory table first
    fx::pauseCell = &fx::world.slChain->vt[0];
    IDXGISwapChain1* pB = nullptr;
    std::thread creator2([&] { pB = CreateOuterChain(g_window, 2560, 1440, 3); });
    WorkerGuard guardB{ &creator2 };
    Check(fx::WaitPaused(15000), "C5: the creator reached the publication barrier within the bound");
    Check(overlay::g_activationCount.load() == 0, "C5: no activation while the table is being published");
    fx::editorOpen = true;
    HRESULT phr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(phr == S_OK && Draws() == 0, "C5: a concurrent callback delegates safely and cannot draw early");
    fx::ReleasePause();
    creator2.join();
    Check(!fx::gateTimeout, "C5: the publication barrier was released inside its own bound");
    Check(pB == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "C5: the creator finished after the barrier was released");
    phr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(phr == S_OK && Draws() == 1, "C5: after publication the same Present draws exactly once");
    // distinct tables get distinct delegates and their own cells
    ResetFixture();
    Install();
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    void* savedSl = nullptr; HMODULE providerSl = nullptr;
    for (int i = 0; i < overlay::kMaxFnRecs; i++) {
        if (overlay::g_fnRecs[i].saved.load() == reinterpret_cast<void*>(&fx::SlCreateFactory2)) { savedSl = overlay::g_fnRecs[i].saved.load(); providerSl = overlay::g_fnRecs[i].provider; }
    }
    Check(savedSl == reinterpret_cast<void*>(&fx::SlCreateFactory2), "C5: a dispatch record holds SL's exact factory function");
    Check(providerSl == fx::slModule, "C5: the dispatch record names the provider module");
    Check(std::find(fx::pins.begin(), fx::pins.end(), fx::slModule) != fx::pins.end(), "C5: the SL provider module was pinned for process lifetime");
    Check(std::find(fx::pins.begin(), fx::pins.end(), fx::dxgiModule) != fx::pins.end(), "C5: the direct provider module was pinned for process lifetime");
}

void CaseCreationFilters() {
    CaseHeader("C6 filters: failures and unsupported configurations delegate unchanged with a reason");
    ResetFixture();
    Install();
    FARPROC thunk = ResolveSlFactory2();
    fx::slCreateFail = true;
    IDXGIFactory4* failOut = reinterpret_cast<IDXGIFactory4*>(static_cast<uintptr_t>(0x1));
    HRESULT hr = fx::GameCallFnC(reinterpret_cast<void*>(thunk), 0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&failOut));
    Check(hr == E_FAIL && failOut == reinterpret_cast<IDXGIFactory4*>(static_cast<uintptr_t>(0x1)), "C6: a failed creation keeps its HRESULT and its output value untouched");
    Check(!HasToken("factory_observed"), "C6: a failed creation is not observed");
    fx::slCreateFail = false;
    CreateSlFactory(thunk);
    fx::world.slFactory->hwndNullOutput = true;
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == nullptr && !HasToken("pair generation"), "C6: a successful null output binds nothing");
    fx::world.slFactory->hwndNullOutput = false;
    fx::world.slFactory->hwndHr = E_FAIL;
    pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(HasToken("create entry Hwnd outer 1"), "C6: a failing creation still delegates through the tapped slot");
    fx::world.slFactory->hwndHr = S_OK;
    fx::Device* notAQueue = fx::NewDevice(1);
    DXGI_SWAP_CHAIN_DESC1 desc = MakeDesc(2560, 1440, 3);
    pp = nullptr;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(notAQueue), g_window, &desc, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason no_d3d12_queue"), "C6: a non-D3D12 device argument never binds");
    fx::world.slQueue->desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason queue_not_direct"), "C6: a non-DIRECT queue never binds");
    fx::world.slQueue->desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    fx::world.slQueue->desc.NodeMask = 3;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason queue_node_unsupported"), "C6: an unsupported creation-node mask on the queue never binds");
    fx::world.slQueue->desc.NodeMask = 0;
    fx::Chain* otherDevChain = fx::NewChain(g_window, fx::world.slDev2, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = otherDevChain;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    Check(!otherDevChain->hasCookie && HasToken("reason identity_failed"), "C6: a queue/chain device identity mismatch never binds");
    {
        bool differing = false;
        for (const auto& l : fx::LinesWith("reason identity_failed")) if (l.find("cdev_id") != std::string::npos && l.find("qdev_id") != std::string::npos) differing = true;
        Check(differing, "C6: the mismatch is reported with both device identities");
    }
    fx::world.slFactory->chain = fx::world.slChain;
    fx::world.slChain->descHr = E_FAIL;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason desc_failed"), "C6: a failed description query never binds");
    fx::world.slChain->descHr = S_OK;
    fx::world.slChain->w = 64; fx::world.slChain->h = 64;          // the resolved chain is tiny even if more was requested
    DXGI_SWAP_CHAIN_DESC1 tinyReq = MakeDesc(0, 0, 3);
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &tinyReq, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason desc_dimensions"), "C6: a tiny resolved description never binds");
    fx::world.slChain->w = 2560; fx::world.slChain->h = 1440;
    fx::world.slChain->cookieHr = E_FAIL;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    Check(!fx::world.slChain->hasCookie && HasToken("reason cookie_rejected"), "C6: a refused private-data contract fails open");
    Check(fx::LinesWith("pair generation").empty(), "C6: a rejected cookie never produces a pairing event");
    fx::world.slChain->cookieHr = S_OK;
    DXGI_SWAP_CHAIN_DESC1 zero = MakeDesc(0, 0, 3);
    pp = nullptr;
    fx::GameCallHwnd(fx::world.slFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.slFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &zero, &pp);
    Check(fx::world.slChain->hasCookie, "C6: zero requested dimensions with a large resolved description still bind");
    const std::string pair = OneLine("pair generation");
    Check(fx::Field(pair, "desc") == "2560x1440", "C6: the pair reports the resolved description, not the request");
}

void CaseSlotOwnership() {
    CaseHeader("C7 ownership: protection/CAS failures roll back only our cells and never fabricate readiness");
    ResetFixture();
    Install();
    FARPROC thunk = ResolveSlFactory2();
    void* foreign = reinterpret_cast<void*>(&fx::ForeignExport);
    fx::world.slFactory->vt[15] = reinterpret_cast<void*>(&fx::SlFactoryHwnd);
    fx::conflictCell = &fx::world.slFactory->vt[15];
    fx::conflictValue = foreign;
    CreateSlFactory(thunk);
    Check(fx::world.slFactory->vt[15] == foreign, "C7: a CAS conflict leaves the foreign pointer untouched");
    Check(HasToken("slots cas_conflict"), "C7: the conflict is reported by the record outcome");
    Check(fx::world.slFactory->vt[0] == reinterpret_cast<void*>(&fx::FactoryQi), "C7: the partially installed factory cell was rolled back");
    fx::conflictCell = nullptr; fx::conflictValue = nullptr;
    // protection failure on a chain cell: the whole chain record stays delegation-only
    ResetFixture();
    Install();
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    fx::failProtectCell = &fx::world.slChain->vt[13];
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slChain->vt[13] == reinterpret_cast<void*>(&fx::ChainResize), "C7: the failed cell keeps its original");
    Check(fx::world.slChain->vt[8] == reinterpret_cast<void*>(&fx::ChainPresent), "C7: the rolled-back Present cell is the original again");
    {
        const std::string p2 = OneLine("pair generation");                 // an unready cookie is harmless, never ready
        Check(fx::Field(p2, "ready") == "0", "C7: a chain whose coverage failed is never paired ready");
        bool chainReady = false;
        for (const auto& l : fx::LinesWith("chain_table table")) if (l.find(fx::PtrText(fx::world.slChain->vt)) != std::string::npos && l.find("slots ready") != std::string::npos) chainReady = true;
        Check(!chainReady, "C7: the chain record never reports ready after a protection failure");
    }
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 0, "C7: an unready cookie never draws");
    Check(HasToken("slots protection_failed"), "C7: the protection failure is reported by the record outcome");
    Check(HasToken("slots protection_failed"), "C7: the chain record reports the failed publication");
    fx::failProtectCell = nullptr;
    // a cell that keeps our thunk but cannot be protection-restored: never reported ready, delegation still works
    ResetFixture();
    Install();
    fx::failRestore = true;
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    Check(HasToken("restore_failed"), "C7: the restoration failure is logged");
    Check(HasToken("slots protection_restore_failed"), "C7: a restore failure never reports a ready record");
    fx::failRestore = false;
    // capacity exhaustion is a declared fail-open limit
    ResetFixture();
    Install();
    for (int i = 0; i < overlay::kMaxChainVt; i++) {
        overlay::g_chainVt[i].vtable.store(reinterpret_cast<void**>(&overlay::g_chainVt[i]));
        overlay::g_chainVt[i].state.store(1, std::memory_order_release);   // every record occupied: a fresh reservation must fail
    }
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slChain->vt[8] == reinterpret_cast<void*>(&fx::ChainPresent), "C7: capacity exhaustion leaves the chain table untouched");
    Check(HasToken("slots capacity_exhausted"), "C7: capacity exhaustion is logged");
    {
        const std::string p3 = OneLine("pair generation");
        Check(fx::Field(p3, "ready") == "0", "C7: a chain without a record is never paired ready");
    }
    // another writer replaces our cell: ownership is lost, never restored
    ResetFixture();
    Install();
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slChain->hasCookie, "C7: the chain was captured before the foreign write");
    fx::world.slChain->vt[8] = foreign;
    void* qiOut = nullptr;
    fx::GameCallQi(fx::world.slChain->vt, reinterpret_cast<IUnknown*>(fx::world.slChain), __uuidof(IDXGISwapChain1), &qiOut);   // an observation of a known chain IID checks ownership
    Check(fx::world.slChain->vt[8] == foreign, "C7: the foreign write is never undone");
    Check(HasToken("reason ownership_lost"), "C7: the lost ownership is reported once");
    fx::editorOpen = true;
    HRESULT phr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(phr == static_cast<HRESULT>(0x5A5A) && Draws() == 0, "C7: the foreign Present entry is called and the overlay draws nothing");
    if (qiOut) reinterpret_cast<IUnknown*>(qiOut)->Release();
}

void CasePresentSelection() {
    CaseHeader("C8 selection: nesting draws once, TEST never draws, other HWNDs and stale generations never take over");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    HRESULT hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, DXGI_PRESENT_TEST);
    Check(hr == S_OK && fx::world.slChain->presents == 1 && Draws() == 0, "C8: a TEST present delegates exactly once and never draws");
    fx::world.slChain->present1CallsPresent = true;
    hr = fx::GamePresent1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), 1, 0, nullptr);
    Check(hr == S_OK && fx::world.slChain->presents1 == 1 && fx::world.slChain->presents == 2, "C8: Present1 and its nested Present both participated");
    Check(Draws() == 1, "C8: Present1 -> Present nesting drew exactly once");
    IDXGIFactory* native = nullptr;
    fx::GameCreateFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&native));   // observe the native factory the wrapper wraps
    fx::SetWrapperInner(fx::world.dxgiFactory[2], fx::world.dxgiQueue);                      // the inner creation passes through its observed table
    CreateOuterChain(g_window, 2560, 1440, 3);                                               // the wrapper performs its nested creation now
    fx::world.slChain->wrapperInner = fx::world.dxgiChain;                                   // the wrapper presents the inner chain first
    Check(fx::world.slChain->wrapperInner->vt[8] != reinterpret_cast<void*>(&fx::ChainPresent), "C8: the nested wrapper's Present slot is interposed as well");
    Check(!fx::world.slChain->wrapperInner->hasCookie, "C8: the nested wrapper chain owns no binding");
    hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 2 && fx::world.slChain->wrapperInner->presents == 1, "C8: the nested wrapper presentation delegated but did not draw again");
    // another HWND never replaces the selected one
    fx::world.slFactory->chain = fx::NewChain(g_window2, fx::world.slDev, 1920, 1080, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::Chain* other = fx::world.slFactory->chain;
    CreateOuterChain(g_window2, 2560, 1440, 3);
    Check(other->hasCookie, "C8: the other window's chain got its own cookie");
    hr = fx::GamePresent(other->vt, reinterpret_cast<IDXGISwapChain*>(other), 1, 0);
    Check(hr == S_OK && Draws() == 2 && HasToken("reason unselected_hwnd"), "C8: an unselected HWND never draws");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C8: an unselected chain never repins the renderer queue");
    hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(hr == S_OK, "C8: the selected chain still presents");
    Check(HasToken("reason stale_generation") || Draws() >= 2, "C8: the older generation cannot steal the activation back");
    // an unregistered object sharing the table can never bind
    fx::Chain* unregistered = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    unregistered->vt = fx::world.slChain->vt;
    const size_t before = Draws();
    hr = fx::GamePresent(unregistered->vt, reinterpret_cast<IDXGISwapChain*>(unregistered), 1, 0);
    Check(hr == S_OK && Draws() == before && unregistered->presents == 1, "C8: a shared table without the cookie delegates and never draws");
}

void CaseCookieAccounting() {
    CaseHeader("C9 cookie: balanced references, no chain held after the callback, queue released at retirement");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    const LONG queueRefsBefore = fx::world.slQueue->refs;
    const LONG chainRefsBefore = fx::world.slChain->refs;
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slChain->hasCookie, "C9: cookie attached");
    Check(fx::world.slQueue->refs == queueRefsBefore + 1, "C9: exactly the cookie's own queue reference is retained");
    Check(fx::world.slChain->refs == chainRefsBefore + 1, "C9: only the returned-chain reference from the creation call is retained");
    IUnknown* selfQi = nullptr;
    HRESULT hr = fx::world.slChain->cookie->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&selfQi));
    Check(SUCCEEDED(hr) && selfQi == fx::world.slChain->cookie, "C9: the cookie's IUnknown QI is its own identity");
    if (selfQi) selfQi->Release();
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C9: first draw happened");
    Check(fx::world.slQueue->refs == queueRefsBefore + 1, "C9: the active renderer borrows the cookie's queue lease; adoption adds no COM reference");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C9: the renderer queue is the cookie's queue");
    // failed setup leaves no reference behind
    ResetFixture();
    Install();
    fx::world.slChain->cookieHr = E_FAIL;
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    const LONG qBefore = fx::world.slQueue->refs;
    const LONG cBefore = fx::world.slChain->refs;
    IDXGISwapChain1* rejected = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slQueue->refs == qBefore, "C9: a rejected cookie leaves no retained queue reference");
    Check(fx::world.slChain->refs == cBefore + 1, "C9: only the caller's own chain reference exists after a rejected cookie");
    if (rejected) rejected->Release();
    Check(fx::world.slChain->refs == cBefore, "C9: the created chain reference is the caller's, nothing is retained by the overlay");
    // retirement: the attachment is DXGI's only entry; the metadata lives on while the attachment does
    ResetFixture();
    Install();
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C9: draw before retirement");
    IUnknown* local = fx::world.slChain->cookie;   // the attachment DXGI owns
    local->AddRef();
    const LONG qRefsActive = fx::world.slQueue->refs;
    DetachCookie(fx::world.slChain);
    Check(fx::world.slQueue->refs == qRefsActive, "C9: the receipt keeps the metadata alive while the attachment is still held");
    hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(hr == S_OK && Draws() == 1 && HasToken("reason attachment_gone"), "C9: a removed private entry stops routing at qualification (no payload was ever read)");
    local->Release();                              // the attachment dies: its receipt is unlinked before the metadata reference drops
    Check(fx::world.slQueue->refs == qRefsActive, "C9: the renderer's own reference keeps the cookie metadata alive after the last attachment user");
    Check(fx::CountWith("cookie retired") == 0, "C9: no retirement while the renderer still holds the cookie");
    DropActiveRenderer();                                          // the renderer drops its metadata reference (rebind/teardown)
    Check(fx::world.slQueue->refs == qRefsActive - 1, "C9: the cookie released its creation queue exactly once at retirement");
    Check(fx::CountWith("cookie retired") == 1, "C9: retirement happens exactly once");
    if (overlay::g_queue) { overlay::g_queue->Release(); overlay::g_queue = nullptr; }   // the renderer's active queue reference
    hr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(hr == S_OK && Draws() == 1 && fx::world.slChain->presents > 0, "C9: after retirement the patched slot still delegates (never touching a dead chain)");
    // wrapper-forwarded private data with a different outer identity never draws
    ResetFixture();
    Install();
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    IDXGIFactory* native2 = nullptr;
    fx::GameCreateFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&native2));   // observe the native factory the wrapper wrapped
    fx::SetWrapperInner(fx::world.dxgiFactory[2], fx::world.dxgiQueue);
    CreateOuterChain(g_window, 2560, 1440, 3);                                               // the nested inner creation passes through the observed table
    fx::Chain* inner = fx::world.dxgiChain;
    inner->forwardCookieTo = fx::world.slChain;                                              // the wrapper forwards private data to the outer object
    Check(inner->vt[8] != reinterpret_cast<void*>(&fx::ChainPresent), "C9: the inner chain's own table is patched (observed through an observed table)");
    Check(!inner->hasCookie, "C9: the inner chain has no cookie of its own");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C9: the real chain drew");
    hr = fx::GamePresent(inner->vt, reinterpret_cast<IDXGISwapChain*>(inner), 1, 0);
    Check(hr == S_OK && Draws() == 1 && (HasToken("reason no_cookie") || HasToken("reason identity_mismatch")), "C9: a forwarded private entry on a different outer identity is never routed and never draws");
}

void CaseResizeGeneration() {
    CaseHeader("C10 resize: HRESULT/arguments preserved, explicit single-DIRECT-queue association only, drain failures disable drawing");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C10: active generation before the resize");
    HRESULT hr = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Check(hr == S_OK && fx::world.slChain->resizes == 1 && fx::world.slChain->lastWidth == 1920 && fx::world.slChain->lastHeight == 1080, "C10: ResizeBuffers kept its HRESULT and arguments");
    Check(overlay::g_forceRebind, "C10: a successful resize requests a render-target rebind");
    fx::world.slChain->resizeHr = E_FAIL;
    hr = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Check(hr == E_FAIL && HasToken("resize failed hr 0x80004005"), "C10: a failing resize keeps its HRESULT and keeps the old binding");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C10: a failed resize does not change the queue association");
    fx::world.slChain->resizeHr = S_OK;
    IUnknown* queues3[3] = { reinterpret_cast<IUnknown*>(fx::world.slQueue), reinterpret_cast<IUnknown*>(fx::world.slQueue), reinterpret_cast<IUnknown*>(fx::world.slQueue) };    UINT nodes3[3] = { 1, 1, 1 };
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 3, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, nodes3, queues3);
    Check(hr == S_OK && fx::world.slChain->resizes1 == 1, "C10: ResizeBuffers1 kept its HRESULT");
    Check(!overlay::g_disabled && !QueueUnsupported(fx::world.slChain), "C10: a uniform single-queue association is accepted");
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    fx::world.slQueue2->dev = fx::world.slDev;
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one);
    Check(hr == S_OK && AssociatedQueue(fx::world.slChain) == fx::world.slQueue2, "C10: an explicit same-device queue replaces the association");
    Check(HasToken("resize1 commit"), "C10: the association change is logged");
    IUnknown* mixed[2] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2), reinterpret_cast<IUnknown*>(fx::world.slQueue) };
    UINT node2[2] = { 1, 1 };
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 2, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node2, mixed);
    Check(hr == S_OK && QueueUnsupported(fx::world.slChain) && HasToken("reason queue_association_mixed"), "C10: mixed queues disable drawing instead of guessing");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C10: an unsupported association disables drawing for this cookie");
    // unsupported node mask and a null queue array
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    one[0] = reinterpret_cast<IUnknown*>(fx::world.slQueue);
    UINT badNodes[1] = { 3 };
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, badNodes, one);
    Check(hr == S_OK && HasToken("reason node_unsupported"), "C10: an unsupported creation-node mask is reported, not guessed");
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 0, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, nullptr, nullptr);
    Check(hr == S_OK && HasToken("reason queue_association_unspecified"), "C10: a null queue array marks the cookie unsupported");
    // a drain timeout retains the association and disables drawing
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::drainFn = &fx::TimeoutDrainHook;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    hr = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Check(hr == S_OK && overlay::g_overlayStop.load() && HasToken("reason gpu_drain_failed"), "C10: a failed drain stops participation");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C10: a failed drain retains the existing queue association and resources");
    const size_t frames = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == frames, "C10: no drawing after a drain failure");
}

void CaseConcurrencyGates() {
    CaseHeader("C11 concurrency: exact drain gate before the generation switch, bounded waits");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    fx::ResetDrainState();
    fx::drainFn = &fx::BlockingDrainHook;
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "C11: the first generation is active");
    fx::Chain* older = fx::world.slChain;
    fx::world.slFactory->chain = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::Chain* newer = fx::world.slFactory->chain;
    Check(newer != older, "C11: a second chain was created");
    std::atomic<bool> presented{ false };
    std::thread presentThread([&] {
        fx::GamePresent(newer->vt, reinterpret_cast<IDXGISwapChain*>(newer), 1, 0);
        presented.store(true);
    });
    WorkerGuard guardP{ &presentThread };
    Check(fx::WaitDrainEntered(15000), "C11: the newer generation reached the drain gate before switching");
    Check(!presented.load(), "C11: the generation switch waits for the drain");
    fx::ReleaseDrain();
    presentThread.join();
    Check(!fx::drainTimeout, "C11: the drain gate completed inside its own bound");
    Check(presented.load() && Draws() == 2, "C11: after the drain the newer generation drew once");
    Check(fx::LastDraw().chain == newer, "C11: the last draw used the newer chain");
    Check(HasToken("activate generation 2"), "C11: the second activation is recorded");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "C11: the queue association is the cookie's own");
}

void CaseDrainGateTimeout() {
    CaseHeader("R6-DRAIN: a drain gate that is never released fails the drain inside its bound and disables drawing");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    const size_t frames = Draws();
    Check(frames == 1, "R6-DRAIN: the generation is active before the drain");
    fx::ResetDrainState();
    fx::drainFn = &fx::BlockingDrainHook;
    fx::drainWaitBoundMs = 100;                                  // the release is deliberately never given: a short bounded wait, never a hang
    const auto start = std::chrono::steady_clock::now();
    const HRESULT hr = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(hr == S_OK && elapsedMs < 2000, "R6-DRAIN: the resize delegates and its bounded drain wait finishes");
    Check(hr == S_OK && overlay::g_overlayStop.load() && HasToken("reason gpu_drain_failed"), "R6-DRAIN: the timed-out drain is reported as a failure and stops participation");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "R6-DRAIN: the failed drain retains the existing queue association");
    Check(Draws() == frames, "R6-DRAIN: no drawing after the timed-out drain");
    Check(fx::drainTimeout, "R6-DRAIN: the unreleased gate is recorded as a timeout, never a silent success");
    fx::drainFn = nullptr;
}

void CaseDeadObject() {
    CaseHeader("C12 dead object: registry callbacks stay callable, no dead chain dereference, single cleanup");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    IUnknown* local = fx::world.slChain->cookie;
    local->AddRef();
    const LONG queueRefs = fx::world.slQueue->refs;
    DetachCookie(fx::world.slChain);
    fx::Chain* dead = fx::world.slChain;
    void* presentCell = dead->vt[8];
    const long presentsBefore = dead->presents;
    HRESULT hr = fx::GamePresent(dead->vt, reinterpret_cast<IDXGISwapChain*>(dead), 1, 0);
    Check(hr == S_OK && Draws() == 1 && dead->presents == presentsBefore + 1, "C12: a detached binding delegates through the same thunk without drawing");
    Check(dead->vt[8] == presentCell, "C12: the thunk cell is unchanged by detachment");
    void* qiOut = nullptr;
    hr = fx::GameCallQi(dead->vt, reinterpret_cast<IUnknown*>(dead), IID_IUnknown, &qiOut);
    Check(SUCCEEDED(hr) && qiOut != nullptr, "C12: the interposed QI slot still delegates after detachment");
    if (qiOut) reinterpret_cast<IUnknown*>(qiOut)->Release();
    local->Release();                              // the attachment dies here: the receipt is unlinked before the metadata reference drops
    Check(fx::CountWith("cookie retired") == 0, "C12: the renderer's own reference still holds the metadata");
    DropActiveRenderer();                                          // the renderer drops its metadata reference
    Check(fx::world.slQueue->refs == queueRefs - 1, "C12: cleanup (queue release) happened once after the last reference");
    Check(fx::CountWith("cookie retired") == 1, "C12: exactly one retirement");
    overlay::g_queue = nullptr;                                    // borrowed alias only: the active lease owns the reference
    overlay::g_activeLease.reset();
    hr = fx::GamePresent(dead->vt, reinterpret_cast<IDXGISwapChain*>(dead), 1, 0);
    Check(hr == S_OK && Draws() == 1 && dead->presents == presentsBefore + 2, "C12: after cleanup the callback still delegates and never touches a dead chain");
}

// ---- Increment A additions: real image bounds, pin prerequisites, resolver error, lock exclusion,
//      factory capture ownership, existing private entry -------------------------------------------------
void CaseRealImageBounds() {
    CaseHeader("A-PE: real game image dimensions accepted; unmapped/oversized claims and out-of-image import directories rejected");
    ResetFixture();
    fx::BuildImageSized(0x173ab000, 0x173ab000);                 // the shipped image's SizeOfImage (read-only offline inspection)
    Install();
    const std::string inst = OneLine("WB_BINDING_FUNCTIONAL install pid");
    Check(fx::Field(inst, "factory_create2") == "owned", "A-PE: the real-size image (0x173ab000) is accepted and its factory import tapped");
    Check(!HasToken("image_unavailable") && !HasToken("image_reject"), "A-PE: no image rejection for the real header dimensions");
    Check(*fx::cellResolver != reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-PE: the resolver cell is tapped for the real-size image");
    // a header claim beyond the mapped range is rejected
    ResetFixture();
    fx::BuildImageSized(0x40000, 0x80000);
    Install();
    Check(HasToken("image_unavailable") || HasToken("image_range_unmapped"), "A-PE: an image claim beyond the mapped range is rejected");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: no tap is published for an unmapped image");
    // an import directory outside the image is rejected
    ResetFixture();
    fx::imageImportDirRva = 0x30000;                             // beyond SizeOfImage
    fx::BuildImage();                                            // rebuild with the override applied
    Install();
    Check(HasToken("image_unavailable") || HasToken("image_reject import_dir"), "A-PE: an import directory outside the image is rejected");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: no tap is published for a bad import directory");
    fx::imageImportDirRva = 0;
    // a header claiming fewer bytes than the directory needs is rejected
    ResetFixture();
    fx::imageImportDirSize = 0x10;                               // smaller than one descriptor
    fx::BuildImage();
    Install();
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: a truncated import directory publishes no tap");
    fx::imageImportDirSize = 0;
    // the walk is bounded by the declared directory length, not a fixed scan limit
    ResetFixture();
    fx::imageImportDirSize = sizeof(IMAGE_IMPORT_DESCRIPTOR);    // exactly one descriptor is declared
    fx::BuildImage();
    Install();
    Check(*fx::cellFactory2 != reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: the declared descriptor's factory imports are tapped");
    Check(*fx::cellResolver == reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-PE: the walk never runs past the declared directory length");
    const std::string instBounded = OneLine("WB_BINDING_FUNCTIONAL install pid");
    Check(fx::Field(instBounded, "resolver") == "absent", "A-PE: the undeclared descriptor contributes no resolver tap");
    fx::imageImportDirSize = 0;
    // a header window too small for the section table is rejected
    ResetFixture();
    fx::imageSizeOfHeaders = 0x100;                              // below the section table offset
    fx::BuildImage();
    Install();
    Check(HasToken("image_unavailable") || HasToken("image_reject"), "A-PE: a header window too small for the section table is rejected");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: a short header window publishes no tap");
    fx::imageSizeOfHeaders = 0x400;
    // a misaligned import address table is never treated as pointer cells
    ResetFixture();
    fx::imageFirstThunkOverride = static_cast<uint32_t>(fx::kRvaIat + 1);
    fx::BuildImage();
    Install();
    Check(HasToken("reason iat_misaligned"), "A-PE: a misaligned IAT is reported and skipped");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PE: a misaligned IAT publishes no tap");
    fx::imageFirstThunkOverride = 0;
}

void CasePinPrerequisites() {
    CaseHeader("A-PIN: module pins are prerequisites; a failed self/main/target pin declines publication");
    ResetFixture();
    fx::pinFails.insert(fx::mainModule);
    Install();
    Check(HasToken("install reason pin_prerequisite_failed"), "A-PIN: a failed main-module pin is a prerequisite failure");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PIN: no tap is published when the image cannot be pinned");
    Check(!HasToken("tap factory2"), "A-PIN: no tap event is emitted without the pin prerequisite");
    ResetFixture();
    fx::pinFails.insert(fx::dxgiModule);                         // the direct provider cannot be pinned
    Install();
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-PIN: a provider pin failure declines the cell (the game keeps its pointer)");
    Check(*fx::cellResolver == reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-PIN: a provider pin failure also declines the resolver import tap");
    Check(HasToken("reason pin_failed"), "A-PIN: the declined target pin is reported");
    ResetFixture();
    fx::pinFails.insert(fx::slModule);                           // the resolver's target module cannot be pinned
    Install();
    FARPROC t = fx::GameGetProcAddress(fx::slModule, "CreateDXGIFactory2");
    Check(t == reinterpret_cast<FARPROC>(&fx::SlCreateFactory2), "A-PIN: a resolver target whose module cannot be pinned is delegated, never substituted");
    Check(HasToken("reason pin_failed"), "A-PIN: the resolver pin failure is reported");
    ResetFixture();
    Install();
    Check(std::find(fx::pinAttempts.begin(), fx::pinAttempts.end(), fx::mainModule) != fx::pinAttempts.end(), "A-PIN: the main module pin is attempted");
    Check(std::find(fx::pinAttempts.begin(), fx::pinAttempts.end(), fx::dxgiModule) != fx::pinAttempts.end(), "A-PIN: the exact target owner is pinned");
    // exact import-provider identity: a name that merely contains the loader name is not the loader module
    ResetFixture();
    fx::kernelModuleName = "notkernel32.dll";
    fx::BuildImage();
    Install();
    Check(*fx::cellResolver == reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-PIN: a DLL name that only contains kernel32 is never treated as the loader module");
    Check(HasToken("reason resolver_module_identity"), "A-PIN: the declined resolver import identity is reported");
    // the documented libraryloader API set is a legitimate loader provider for the observed import
    ResetFixture();
    fx::kernelModuleName = "api-ms-win-core-libraryloader-l1-2-0.dll";
    fx::BuildImage();
    Install();
    Check(*fx::cellResolver != reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-PIN: the documented libraryloader API set is accepted as GetProcAddress's provider");
    fx::kernelModuleName = "KERNEL32.dll";
}

void CaseResolverErrorPreservation() {
    CaseHeader("A-ERR: the resolver's own last error survives our postprocessing on success and on pass-through");
    ResetFixture();
    Install();
    SetLastError(0x1234);
    FARPROC thunk = fx::GameGetProcAddress(fx::slModule, "CreateDXGIFactory2");
    Check(thunk && thunk != reinterpret_cast<FARPROC>(&fx::SlCreateFactory2), "A-ERR: the successful substitution still returns a thunk");
    Check(GetLastError() == 0x1234, "A-ERR: a successful substitution preserves the caller/resolver last error across postprocessing");
    SetLastError(0x2345);
    FARPROC foreign = fx::GameGetProcAddress(fx::foreignModule, "CreateDXGIFactory2");
    Check(foreign == reinterpret_cast<FARPROC>(&fx::ForeignExport) && GetLastError() == 0x2345, "A-ERR: a foreign-module pass-through preserves the resolver's last error");
}

void CaseLockExclusion() {
    CaseHeader("A-LOCK: observer transaction and render boundary never hold each other's lock; a paused publisher cannot block a present");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    FARPROC thunk = ResolveSlFactory2();
    CreateSlFactory(thunk);
    fx::patchFreeDuringRender.store(-1);
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::renderFreeDuringPatch.load() == 1, "A-LOCK: the render boundary is free while the capture/observer path runs");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "A-LOCK: the render boundary completed a frame");
    Check(fx::patchFreeDuringRender.load() == 1, "A-LOCK: the patch mutex is free while the render boundary calls into foreign code");
    Check(fx::comUnderPatchLock.load() == 0, "A-LOCK: no fixture COM method is ever entered while the patch mutex is held");
    // a creator paused inside the patch transaction must not block a present (no lock coupling, bounded)
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::pauseCell = &fx::world.slChain->vt[0];
    IDXGISwapChain1* pp = nullptr;
    std::thread creator([&] { pp = CreateOuterChain(g_window, 2560, 1440, 3); });
    WorkerGuard guard{ &creator };
    Check(fx::WaitPaused(15000), "A-LOCK: the publisher reached the transaction barrier within the bound");
    fx::editorOpen = true;
    const auto start = std::chrono::steady_clock::now();
    const HRESULT phr = fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(phr == S_OK && elapsedMs < 2000, "A-LOCK: a present completes within a bounded time while a publisher holds the patch mutex");
    Check(Draws() == 0, "A-LOCK: the present delegated without drawing while the table was still being published");
    fx::ReleasePause();
    creator.join();
    Check(!fx::gateTimeout, "A-LOCK: the transaction barrier was released inside its own bound");
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "A-LOCK: the publisher completed after the barrier was released");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "A-LOCK: after publication the present draws once");
    fx::pauseCell = nullptr;
}

void CaseFactoryCaptureOwnership() {
    CaseHeader("A-CAP: a factory capture requires ownership and coverage of the record at capture time");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    Check(fx::world.slFactory->vt[15] != reinterpret_cast<void*>(&fx::SlFactoryHwnd), "A-CAP: the factory record was published");
    fx::world.slFactory->vt[10] = reinterpret_cast<void*>(&fx::ForeignExport);   // a foreign write on a required cell
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "A-CAP: the creation still returns the final chain");
    Check(!fx::world.slChain->hasCookie, "A-CAP: no cookie is attached once the record lost a required cell");
    Check(HasToken("reason record_not_owned") || HasToken("reason ownership_lost"), "A-CAP: the lost ownership is reported at capture");
    Check(fx::world.slFactory->vt[10] == reinterpret_cast<void*>(&fx::ForeignExport), "A-CAP: the foreign cell is never restored");
}

void CaseExistingPrivateEntry() {
    CaseHeader("A-PRIV: an existing private entry under our GUID is never overwritten");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Obj* foreign = fx::NewDevice(1);                        // any interface object standing in for a foreign entry
    fx::SetForeignEntry(fx::world.slChain, reinterpret_cast<IUnknown*>(foreign));
    Check(fx::world.slChain->hasCookie && fx::world.slChain->cookieIsForeign, "A-PRIV: the fixture installed a foreign entry under the production GUID");
    IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "A-PRIV: the creation still returns the chain to the game");
    Check(HasToken("reason private_entry_exists"), "A-PRIV: the capture is declined because an entry already exists");
    Check(fx::world.slChain->cookie == reinterpret_cast<IUnknown*>(foreign), "A-PRIV: the existing entry is untouched");
    Check(!HasToken("pair generation"), "A-PRIV: no pairing event is published for a declined capture");
}


void CaseResizeQueueCommit() {
    CaseHeader("B-QUEUE: a resize association commits to the drawing queue, before and after the first Present");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    // association committed BEFORE the first Present (no activation yet)
    fx::world.slChain->buffers = 3;
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    fx::world.slQueue2->dev = fx::world.slDev;
    HRESULT hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one);
    Check(hr == S_OK && AssociatedQueue(fx::world.slChain) == fx::world.slQueue2, "B-QUEUE: the association is recorded before any Present");
    Check(HasToken("resize1 commit"), "B-QUEUE: the commit is logged");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "B-QUEUE: the first Present draws");
    Check(fx::LastDraw().queue == fx::world.slQueue2, "B-QUEUE: the first draw already uses the committed queue, not the creation queue");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue2), "B-QUEUE: the active strong queue is the committed one");
    // a later association on the SAME cookie/generation must re-adopt and move the drawing queue
    fx::world.slQueue->dev = fx::world.slDev;
    IUnknown* back[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue) };
    hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, back);
    Check(hr == S_OK && AssociatedQueue(fx::world.slChain) == fx::world.slQueue, "B-QUEUE: the same cookie can change its association again");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 2 && fx::LastDraw().queue == fx::world.slQueue, "B-QUEUE: the next draw uses the re-committed queue (same cookie, same generation)");
    Check(overlay::g_queue == reinterpret_cast<ID3D12CommandQueue*>(fx::world.slQueue), "B-QUEUE: the active strong queue followed the commit");
    // the association revision is what makes the re-adoption observable
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie && cookie->associationRevision.load() >= 2, "B-QUEUE: each commit bumps the association revision");
    if (cookie) cookie->Release();
}

void CaseRebindPolicy() {
    CaseHeader("B-REBIND: the rebind drains, keeps resources on a failed drain, and rebuilds for the changed description");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "B-REBIND: first frame");
    // a successful resize changes the resolved description; the next frame must rebuild for the NEW count/size
    fx::world.slChain->buffers = 4;
    fx::world.slChain->w = 1920;
    fx::world.slChain->h = 1080;
    fx::world.slChain->fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    fx::rebindCalls.clear();
    fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    Check(overlay::g_forceRebind, "B-REBIND: a successful resize requests a rebuild");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(fx::rebindCalls.size() == 1, "B-REBIND: exactly one rebuild ran on the next frame");
    if (!fx::rebindCalls.empty()) {
        const fx::RebindCall& rc = fx::rebindCalls.back();
        Check(rc.buffers == 4 && rc.w == 1920 && rc.h == 1080 && rc.format == DXGI_FORMAT_B8G8R8A8_UNORM, "B-REBIND: the rebuild used the changed description (buffers/width/height/format)");
    }
    Check(!overlay::g_forceRebind, "B-REBIND: a successful rebuild clears the pending flag");
    Check(overlay::g_bufferCount == 4 && overlay::g_width == 1920 && overlay::g_format == DXGI_FORMAT_B8G8R8A8_UNORM, "B-REBIND: the renderer state follows the new description");
    // a drain timeout keeps the old binding/resources and disables drawing (no allocator rebuild)
    fx::world.slChain->buffers = 6;
    fx::world.slChain->w = 1280;
    fx::world.slChain->h = 720;
    fx::rebindCalls.clear();
    fx::drainFn = &fx::TimeoutDrainHook;
    fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    const UINT buffersBefore = overlay::g_bufferCount;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(fx::rebindCalls.empty(), "B-REBIND: a failed drain never rebuilds the allocators");
    Check(overlay::g_bufferCount == buffersBefore, "B-REBIND: the old renderer state is retained on a failed drain");
    Check(HasToken("reason gpu_drain_failed"), "B-REBIND: the failed drain is reported");
    // a FAILED resize must not leave a pending rebuild behind
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    fx::world.slChain->resizeHr = E_FAIL;
    fx::rebindCalls.clear();
    fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Check(!overlay::g_forceRebind, "B-REBIND: a failed resize does not leave a pending rebuild");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(fx::rebindCalls.empty(), "B-REBIND: no rebuild is triggered by a failed resize");
}

void CaseDeadStorageReuse() {
    CaseHeader("B-DEAD: freed chain storage is reused deterministically and never resurrects a binding");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "B-DEAD: the first chain drew");
    fx::Chain* dead = fx::world.slChain;
    IUnknown* local = dead->cookie;
    local->AddRef();
    DetachCookie(dead);                                     // DXGI destroys the private data
    DropActiveRenderer();                                   // the renderer drops its metadata reference and active lease
    local->Release();                                       // last user: the metadata retires here
    Check(fx::CountWith("cookie retired") == 1, "B-DEAD: the cookie retired exactly once");
    Check(overlay::g_selectedHwnd == g_window, "B-DEAD: the selected window survives the retirement");
    const void* deadAddr = dead;
    fx::FreeChainStorage(dead);                             // explicit fixture destruction, deterministic reuse
    fx::Chain* reused = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(reused == reinterpret_cast<fx::Chain*>(const_cast<void*>(deadAddr)), "B-DEAD: the recycled storage has the same address as the dead chain");
    Check(!reused->hasCookie, "B-DEAD: recycled storage carries no binding from the dead object");
    fx::world.slFactory->chain = reused;
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(reused->hasCookie, "B-DEAD: the recycled storage obtains its own fresh binding");
    const size_t before = Draws();
    fx::GamePresent(reused->vt, reinterpret_cast<IDXGISwapChain*>(reused), 1, 0);
    Check(Draws() == before + 1, "B-DEAD: the recycled storage draws as a new object");
    Check(fx::CountWith("cookie retired") == 1, "B-DEAD: no extra retirement happened for the recycled storage");
}


void CaseExtensionAtomicity() {
    CaseHeader("C-EXT: extension is atomic under the lock, never saves our own thunk, and degrades on failure");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::world.slChain->AddAlias(overlay::kIidSwapChain4, fx::world.slChain);   // a longer public alias of the same table
    CreateOuterChain(g_window, 2560, 1440, 3);
    void* qiOut = nullptr;
    fx::GameCallQi(fx::world.slChain->vt, reinterpret_cast<IUnknown*>(fx::world.slChain), overlay::kIidSwapChain4, &qiOut);
    if (qiOut) reinterpret_cast<IUnknown*>(qiOut)->Release();
    Check(HasToken("extended present1 0 resize1 1") || HasToken("extended present1 1 resize1 1"), "C-EXT: the longer alias extended the record");
    for (int i = 0; i < overlay::kMaxChainVt; i++) {
        if (overlay::g_chainVt[i].state.load() != 2) continue;
        Check(overlay::g_chainVt[i].savedPresent.load() != overlay::g_chainThunks[i].present, "C-EXT: no record saved our own Present thunk as the downstream original");
        Check(overlay::g_chainVt[i].savedQi.load() != overlay::g_chainThunks[i].qi, "C-EXT: no record saved our own QI thunk as the downstream original");
        Check(overlay::g_chainVt[i].savedResize1.load() != overlay::g_chainThunks[i].resize1, "C-EXT: no record saved our own ResizeBuffers1 thunk as the downstream original");
    }
    // a failed required extension degrades the record instead of leaving it ready; the shorter-prefix record is
    // produced through the production observation entry point, so the extension path itself runs for real
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::world.slChain->vt[0] = reinterpret_cast<void*>(&fx::ChainQi);
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), fx::world.slChain);      // shorter prefix only
    int recIdx = -1;
    for (int i = 0; i < overlay::kMaxChainVt; i++) if (overlay::g_chainVt[i].vtable.load() == fx::world.slChain->vt) recIdx = i;
    Check(recIdx >= 0 && overlay::g_chainVt[recIdx].state.load() == 2, "C-EXT: the shorter-prefix record was published");
    Check((overlay::g_chainVt[recIdx].coverage.load() & 16u) == 0, "C-EXT: the shorter prefix does not claim ResizeBuffers1 coverage");
    fx::failProtectCell = &fx::world.slChain->vt[39];
    overlay::ObserveChainInterface(overlay::kIidSwapChain4, fx::world.slChain);        // a longer alias requires slot 39
    Check(HasToken("reason extension_incomplete"), "C-EXT: a failed required extension is reported");
    Check(overlay::g_chainVt[recIdx].lost.load() == 1, "C-EXT: an incomplete extension degrades the record instead of leaving it ready");
    fx::failProtectCell = nullptr;
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 0, "C-EXT: a degraded record never draws");
    Check(fx::world.slChain->presents == 1, "C-EXT: the degraded record still delegates the game's present");
}

void CaseCapacityLimits() {
    CaseHeader("C-CAP: the declared table capacities are real limits with fail-open delegation");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    int observed = 0;
    for (int i = 0; i < overlay::kMaxChainVt + 2; i++) {
        fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        fx::world.slFactory->chain = c;
        CreateOuterChain(g_window, 2560, 1440, 3);
        overlay::BindingCookie* k = CookieOf(c);                     // "bound" means the record is ready, not merely attached
        if (k) { if (k->state.load(std::memory_order_acquire) == overlay::CookieReady) observed++; k->Release(); }
    }
    int ready = 0, declined = 0;
    for (int i = 0; i < overlay::kMaxChainVt; i++) { const int st = overlay::g_chainVt[i].state.load(); if (st == 2) ready++; else if (st == 3) declined++; }
    core::Log("[fx] C-CAP bound %d ready %d declined %d", observed, ready, declined);
    Check(observed == overlay::kMaxChainVt, "C-CAP: exactly the declared chain-record capacity binds (others delegate)");
    Check(HasToken("slots capacity_exhausted"), "C-CAP: the exhaustion is reported");
    Check(overlay::g_chainVt[overlay::kMaxChainVt - 1].state.load() == 2, "C-CAP: the last declared record is used");
}

void CaseAliasRouting() {
    CaseHeader("C-ALIAS: creation and presentation through a supported alias table are captured and routed");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::world.slFactory->AddAlias(__uuidof(IDXGIFactory2), fx::world.aliasFactory);   // the alias factory table
    fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3), fx::world.aliasChain);     // the alias chain table
    IDXGIFactory2* alias = nullptr;
    fx::GameCallQi(fx::world.slFactory->vt, reinterpret_cast<IUnknown*>(fx::world.slFactory), __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&alias));
    Check(alias == reinterpret_cast<IDXGIFactory2*>(fx::world.aliasFactory), "C-ALIAS: the alias factory is returned unchanged");
    Check(fx::world.aliasFactory->vt[15] != reinterpret_cast<void*>(&fx::SlFactoryHwnd), "C-ALIAS: the alias factory's creation slot is interposed");
    DXGI_SWAP_CHAIN_DESC1 desc = MakeDesc(2560, 1440, 3);
    IDXGISwapChain1* pp = nullptr;
    fx::GameCallHwnd(fx::world.aliasFactory->vt, reinterpret_cast<IDXGIFactory2*>(fx::world.aliasFactory), reinterpret_cast<IUnknown*>(fx::world.slQueue), g_window, &desc, &pp);
    core::Log("[fx] C-ALIAS pp %p slChain %p aliasChain %p hasCookie %d aliasCookie %d", (void*)pp, (void*)fx::world.slChain, (void*)fx::world.aliasChain,
              fx::world.slChain->hasCookie ? 1 : 0, fx::world.aliasChain->hasCookie ? 1 : 0);
    Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain) && fx::world.slChain->hasCookie, "C-ALIAS: creation through the alias table is captured");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    {   // the routed frame belongs to the presented object's canonical identity (its chain3 view may be a subobject)
        fx::Obj* drawn = reinterpret_cast<fx::Obj*>(fx::LastDraw().chain);
        Check(Draws() == 1 && drawn && drawn->identity == fx::world.slChain->identity, "C-ALIAS: presentation through the returned interface is routed once");
    }
    const size_t aliasDraws = Draws();
    fx::GamePresent(fx::world.aliasChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.aliasChain), 1, 0);
    Check(Draws() == aliasDraws && HasToken("reason attachment_gone"), "C-ALIAS: a subobject that does not carry the live attachment never draws");
}

void CaseHwndAndChainEdges() {
    CaseHeader("C-EDGE: chain3 QI failure and non-top-level windows never bind");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::chain3QiFails = true;
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(!fx::world.slChain->hasCookie && HasToken("reason no_chain3"), "C-EDGE: a failed chain3 qualification never binds");
    fx::chain3QiFails = false;
    // a child window is not a game window
    HWND child = CreateWindowExA(0, "wbHostWindow", "child", WS_CHILD | WS_VISIBLE, 0, 0, 64, 64, g_window, nullptr, GetModuleHandleA(nullptr), nullptr);
    Check(child != nullptr, "C-EDGE: the child window exists");
    fx::world.slChain->hwnd = child;
    fx::world.slChain->hasCookie = false;
    CreateOuterChain(child, 2560, 1440, 3);
    Check(!fx::world.slChain->hasCookie && HasToken("reason hwnd_invalid"), "C-EDGE: a child window never binds");
    // an owned popup is not a game window either
    HWND popup = CreateWindowExA(WS_EX_TOOLWINDOW, "wbHostWindow", "popup", WS_POPUP, 0, 0, 64, 64, g_window, nullptr, GetModuleHandleA(nullptr), nullptr);
    Check(popup != nullptr, "C-EDGE: the owned popup exists");
    fx::world.slChain->hwnd = popup;
    CreateOuterChain(popup, 2560, 1440, 3);
    Check(!fx::world.slChain->hasCookie && HasToken("reason hwnd_invalid"), "C-EDGE: an owned popup never binds");
    DestroyWindow(popup); DestroyWindow(child);
    fx::world.slChain->hwnd = g_window;
}

void CaseResizeOverlap() {
    CaseHeader("C-OVERLAP: A.original -> B.resize on another chain commits independently; same-cookie nesting is sticky delegation");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::Chain* b = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = b;
    CreateOuterChain(g_window, 2560, 1440, 3);                 // chain B and its own cookie
    fx::world.slFactory->chain = fx::world.slChain;
    overlay::BindingCookie* cookieA = CookieOf(fx::world.slChain);
    overlay::BindingCookie* cookieB = CookieOf(b);
    Check(cookieA && cookieB && cookieA != cookieB, "C-OVERLAP: two distinct cookies for the two chains");
    // A fails, B succeeds: B's committed revision must survive A's failure
    fx::world.slChain->resizeHr = E_FAIL;
    b->resizeHr = S_OK;
    fx::world.slChain->nestedResizeChain = b;
    const HRESULT hrA = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    fx::world.slChain->nestedResizeChain = nullptr;
    Check(hrA == E_FAIL && fx::world.slChain->resizes == 1 && b->resizes == 1, "C-OVERLAP: both originals ran exactly once with their own HRESULTs");
    Check(overlay::g_resizeCalls == 0, "C-OVERLAP: both nested transactions closed and the barrier reached zero");
    Check(cookieB->associationRevision.load() >= 1 && !cookieB->renderQueueUnsupported, "C-OVERLAP: the inner success kept its committed revision");
    Check(cookieA->associationRevision.load() == 0, "C-OVERLAP: the outer failure kept its previous association");
    Check(cookieA->resizeCalls == 0 && cookieB->resizeCalls == 0 && !cookieA->resizing.load() && !cookieB->resizing.load(), "C-OVERLAP: both tickets closed");
    Check(HasToken("resize failed hr 0x80004005"), "C-OVERLAP: the failing outer call is reported with its HRESULT");
    // the same cookie through two live tickets (the fixture's own original re-enters the patched slot): sticky
    fx::world.slChain->resizeHr = S_OK;
    fx::world.slChain->resizeCallsNested = true;
    fx::world.slQueue2->dev = fx::world.slDev;
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    const void* before = AssociatedQueue(fx::world.slChain);
    const HRESULT hrN = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one);
    fx::world.slChain->resizeCallsNested = false;
    Check(hrN == S_OK && fx::world.slChain->resizes1 == 2, "C-OVERLAP: the same-cookie pair delegated both originals");
    Check(cookieA->resizeAmbiguous, "C-OVERLAP: same-cookie nesting is detected as an unsupported overlap");
    Check(AssociatedQueue(fx::world.slChain) == before, "C-OVERLAP: the overlapping nested call exposed no new association");
    Check(cookieA->resizeCalls == 0 && !cookieA->resizing.load() && overlay::g_resizeCalls == 0, "C-OVERLAP: both nested tickets closed");
    cookieA->Release();
    cookieB->Release();
}

void CaseResizeSameCookieOverlap() {
    CaseHeader("R1-OVERLAP: same-cookie cross-thread overlap is sticky delegation; neither call exposes an association");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-OVERLAP: the generation is active");
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie != nullptr, "R1-OVERLAP: the cookie is resolvable");
    const void* queueBefore = AssociatedQueue(fx::world.slChain);
    // T1 pauses inside its original (holding no overlay lock there); T2 then begins the same cookie
    fx::world.slChain->resizeBlockCount = 1;
    fx::world.slChain->resizeHr = E_FAIL;
    fx::world.slChain->resize1Hr = S_OK;
    std::atomic<bool> aDone{ false };
    std::atomic<HRESULT> aHr{ E_PENDING };
    std::thread a([&] { aHr.store(fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0)); aDone.store(true); });
    WorkerGuard guardA{ &a };
    Check(fx::WaitResizeEntered(15000), "R1-OVERLAP: T1 reached its original inside the bound");
    fx::world.slQueue2->dev = fx::world.slDev;
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    const HRESULT hrB = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one);
    Check(hrB == S_OK && fx::world.slChain->resizes1 == 1, "R1-OVERLAP: T2 delegated the original once and kept its HRESULT");
    fx::ReleaseResizeOriginal();
    a.join();
    Check(!fx::resizeGateTimeout, "R1-OVERLAP: T1's original completed inside its own bound");
    Check(aDone.load() && fx::world.slChain->resizes == 1, "R1-OVERLAP: T1 ran its original exactly once");
    Check(cookie->resizeAmbiguous && cookie->renderQueueUnsupported, "R1-OVERLAP: the same-cookie overlap is sticky delegation for this cookie");
    Check(cookie->resizeCalls == 0 && !cookie->resizing.load(), "R1-OVERLAP: both tickets closed and the resizing observation cleared");
    Check(overlay::g_resizeCalls == 0, "R1-OVERLAP: the global barrier reached zero");
    Check(AssociatedQueue(fx::world.slChain) == queueBefore, "R1-OVERLAP: neither overlapping call exposed a new association");
    Check(HasToken("resize_overlap_ambiguous"), "R1-OVERLAP: the unsupported overlap is reported");
    Check(aHr.load() == E_FAIL, "R1-OVERLAP: the failing original kept its exact HRESULT");
    const size_t frames = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == frames, "R1-OVERLAP: no frame is drawn on the superseded association after either return");
    cookie->Release();
}

void CaseResizeBlockedFinish() {
    CaseHeader("R1-FINISH: a contended Finish waits, then commits and closes; it never drops the commit or leaves resizing set");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-FINISH: the generation is active");
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    const void* before = AssociatedQueue(fx::world.slChain);
    fx::world.slQueue2->dev = fx::world.slDev;
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    std::unique_lock<std::mutex> hold(overlay::g_renderMutex);   // the test holds the render state T3's Finish needs
    fx::world.slChain->resizeBlockCount = 1;
    std::atomic<bool> done{ false };
    HRESULT hr = E_PENDING;
    std::thread t3([&] { hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one); done.store(true); });
    WorkerGuard guard3{ &t3, &hold };   // the held test lock is released before the join on every assertion path
    Check(!done.load(), "R1-FINISH: the resize cannot pass held render state");
    hold.unlock();
    Check(fx::WaitResizeEntered(15000), "R1-FINISH: T3 reached its original inside the bound");
    hold.lock();                                              // from here T3's Finish has to wait for render state
    fx::ReleaseResizeOriginal();
    Check(overlay::g_resizeCalls == 1, "R1-FINISH: the barrier is still raised while the commit waits for render state");
    Check(cookie->resizing.load(), "R1-FINISH: the cookie is still observed as resizing before the commit lands");
    Check(AssociatedQueue(fx::world.slChain) == before, "R1-FINISH: no association is exposed before Finish ran");
    hold.unlock();
    t3.join();
    Check(!fx::resizeGateTimeout, "R1-FINISH: the gate was released inside its own bound");
    Check(done.load() && hr == S_OK, "R1-FINISH: the resize returned the game's HRESULT");
    Check(fx::world.slChain->resizes1 == 1, "R1-FINISH: the original ran exactly once");
    Check(AssociatedQueue(fx::world.slChain) == fx::world.slQueue2, "R1-FINISH: the waiting Finish still committed the association");
    Check(overlay::g_resizeCalls == 0 && cookie->resizeCalls == 0 && !cookie->resizing.load(), "R1-FINISH: the barrier and the cookie ticket closed");
    Check(cookie->associationRevision.load() >= 1, "R1-FINISH: the successful commit published a new revision");
    Check(!cookie->renderQueueUnsupported, "R1-FINISH: a clean commit keeps the cookie drawable");
    cookie->Release();
}

void CaseResizeBarrierHeldDraw() {
    CaseHeader("R1-BARRIER: a held frame delays the resize; under the raised barrier nothing activates or draws, and the revision lands only after Finish");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-BARRIER: the first generation is active");
    fx::Chain* b = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = b;
    CreateOuterChain(g_window, 2560, 1440, 3);                 // B is captured but not presented yet
    fx::world.slFactory->chain = fx::world.slChain;
    // T1 holds render state inside the frame sink's adoption drain
    fx::ResetDrainState();
    fx::drainFn = &fx::BlockingDrainHook;
    std::atomic<bool> t1Done{ false };
    std::thread t1([&] { fx::GamePresent(b->vt, reinterpret_cast<IDXGISwapChain*>(b), 1, 0); t1Done.store(true); });
    WorkerGuard guard1{ &t1 };
    Check(fx::WaitDrainEntered(15000), "R1-BARRIER: the frame sink reached the adoption drain");
    // T2 reaches its resize qualification but cannot run its original while T1 holds render state
    fx::world.slChain->resizeBlockCount = 1;
    const long lookupsBefore = fx::IdentityQueryCount();
    std::atomic<bool> t2Done{ false };
    std::thread t2([&] { fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0); t2Done.store(true); });
    WorkerGuard guard2{ &t2 };
    Check(fx::WaitIdentityQueries(lookupsBefore + 1, 15000), "R1-BARRIER: the resize reached its Begin qualification");
    Check(fx::world.slChain->resizes == 0 && !t2Done.load(), "R1-BARRIER: the resize never reached its original while the frame sink held render state");
    fx::ReleaseDrain();
    Check(fx::WaitResizeEntered(15000), "R1-BARRIER: the resize reached its original after the held frame drained");
    t1.join();                                                 // T1's exact completion, never a timing assumption
    Check(!fx::drainTimeout && t1Done.load(), "R1-BARRIER: the held frame completed inside its own bound");
    Check(overlay::g_resizeCalls == 1, "R1-BARRIER: the barrier is raised while the original runs");
    const int activationsBefore = overlay::g_activationCount.load();
    const size_t drawsBefore = Draws();
    fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = c;
    IDXGISwapChain1* pc = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pc == reinterpret_cast<IDXGISwapChain1*>(c) && c->hasCookie, "R1-BARRIER: a creation still publishes its cookie while the barrier is raised");
    fx::GamePresent(c->vt, reinterpret_cast<IDXGISwapChain*>(c), 1, 0);
    Check(overlay::g_activationCount.load() == activationsBefore && Draws() == drawsBefore, "R1-BARRIER: no generation activates or draws under a raised barrier");
    Check(HasToken("reason resize_barrier"), "R1-BARRIER: the refused activation is reported");
    fx::ReleaseResizeOriginal();
    t2.join();
    Check(!fx::resizeGateTimeout && t2Done.load(), "R1-BARRIER: the resize completed");
    Check(overlay::g_resizeCalls == 0, "R1-BARRIER: the barrier reached zero after Finish");
    const int activationsAfter = overlay::g_activationCount.load();
    fx::GamePresent(c->vt, reinterpret_cast<IDXGISwapChain*>(c), 1, 0);
    Check(overlay::g_activationCount.load() == activationsAfter + 1 && Draws() == drawsBefore + 1, "R1-BARRIER: after Finish the replacement generation may activate and draw");
    fx::drainFn = nullptr;
    fx::ResetDrainState();
}

void CasePinReentry() {
    CaseHeader("R2-PINREENTRY: a paused pin holds no lock; observation and render qualification complete; Preparing never marks lost");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R2-PINREENTRY: the first binding is active");
    // thread P publishes a second chain; its preparation pauses inside a module pin (which happens outside every lock)
    fx::Chain* b = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = b;
    fx::pinBlockModule = fx::mainModule;   // the fixture maps its heap-allocated tables to the main module for the pin
    fx::pinBlockCount = 1;
    std::atomic<bool> pDone{ false };
    IDXGISwapChain1* pb = nullptr;
    std::thread p([&] { pb = CreateOuterChain(g_window, 2560, 1440, 3); pDone.store(true); });
    WorkerGuard guardP{ &p };
    Check(fx::WaitPinEntered(15000), "R2-PINREENTRY: the publisher paused inside a pin call");
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws + 1, "R2-PINREENTRY: a present completes while the pin call is paused");
    fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = c;
    IDXGISwapChain1* pc = CreateOuterChain(g_window, 2560, 1440, 3);
    Check(pc == reinterpret_cast<IDXGISwapChain1*>(c) && c->hasCookie, "R2-PINREENTRY: a concurrent observation completes and binds");
    fx::world.slFactory->chain = b;
    fx::ReleasePinGate();
    p.join();
    Check(!fx::pinGateTimeout && pDone.load(), "R2-PINREENTRY: the paused pin completed inside its own bound");
    Check(pb == reinterpret_cast<IDXGISwapChain1*>(b) && b->hasCookie, "R2-PINREENTRY: the paused transaction still published its binding");
    Check(!HasToken("reason ownership_lost"), "R2-PINREENTRY: no observation marked another's record lost");
    // phase 2: publish is paused inside its first cell transaction; a longer-IID observer must wait for it
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* d = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = d;
    fx::pauseCell = &d->vt[0];
    IDXGISwapChain1* pd = nullptr;
    std::thread q([&] { pd = CreateOuterChain(g_window, 2560, 1440, 3); });
    WorkerGuard guardQ{ &q };
    Check(fx::WaitPaused(15000), "R2-PINREENTRY: the publisher paused inside its first cell transaction");
    fx::editorOpen = true;
    const size_t frames = Draws();
    fx::GamePresent(d->vt, reinterpret_cast<IDXGISwapChain*>(d), 1, 0);
    Check(Draws() == frames, "R2-PINREENTRY: a callback during Preparing delegates without drawing");
    std::atomic<bool> oDone{ false };
    std::thread o([&] { overlay::ObserveChainInterface(__uuidof(IDXGISwapChain3), d); oDone.store(true); });
    WorkerGuard guardO{ &o };
    fx::ReleasePause();
    o.join();
    q.join();
    Check(!fx::gateTimeout && oDone.load(), "R2-PINREENTRY: the longer-IID observer waited for the transaction and then completed");
    {
        int ridx = -1;
        for (int i = 0; i < overlay::kMaxChainVt; i++) if (overlay::g_chainVt[i].vtable.load() == d->vt) ridx = i;
        Check(ridx >= 0 && overlay::g_chainVt[ridx].state.load() == 2 && (overlay::g_chainVt[ridx].coverage.load() & 16u) != 0, "R2-PINREENTRY: the extended record ends Ready with the longer prefix");
        Check(overlay::g_chainVt[ridx].lost.load() == 0, "R2-PINREENTRY: the Preparing window never marked the record lost");
    }
    Check(pd == reinterpret_cast<IDXGISwapChain1*>(d) && d->hasCookie, "R2-PINREENTRY: the paused publisher completed its capture");
    fx::pauseCell = nullptr;
}

void CaseExtensionFailureMatrix() {
    CaseHeader("R3-EXTFAIL: required extension failures are terminal, only exact prefix cells are touched, foreign writes and protections survive");
    // exact prefix: a chain1 observation never touches the ResizeBuffers1 cell
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), c);
    Check(fx::ProtectWritesFor(&c->vt[22]) == 1 && fx::ProtectWritesFor(&c->vt[39]) == 0, "R3-EXTFAIL: only the observed prefix is ever inspected or pinned");
    // an unpinnable required slot makes the whole extension terminal
    fx::pinFails.insert(fx::dxgiModule);
    overlay::ObserveChainInterface(overlay::kIidSwapChain4, c);
    Check(HasToken("reason extension_unsupported"), "R3-EXTFAIL: an unpinnable required slot declines the extension");
    {
        int ridx = -1;
        for (int i = 0; i < overlay::kMaxChainVt; i++) if (overlay::g_chainVt[i].vtable.load() == c->vt) ridx = i;
        Check(ridx >= 0 && overlay::g_chainVt[ridx].state.load() == 3 && overlay::g_chainVt[ridx].lost.load() == 1, "R3-EXTFAIL: a failed required extension is terminal, never a shorter Ready alias");
        Check((overlay::g_chainVt[ridx].coverage.load() & 16u) == 0, "R3-EXTFAIL: the longer prefix is not claimed");
    }
    fx::pinFails.erase(fx::dxgiModule);
    // CAS success + restore failure: our exact write is rolled back, the foreign/original cell stays
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* r = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), r);
    fx::failRestore = true;
    overlay::ObserveChainInterface(overlay::kIidSwapChain4, r);
    Check(HasToken("reason extension_incomplete") && r->vt[39] == reinterpret_cast<void*>(&fx::ChainResize1), "R3-EXTFAIL: a restore failure rolls the exact write back to the original");
    Check(HasToken("rollback_cells 1 pointer_failed 0 protection_failed 1"), "R3-EXTFAIL: the rollback reports the pointer outcome and the protection outcome (logged after unlocking)");
    fx::failRestore = false;
    // CAS conflict + restore failure: the foreign cell is never treated as our write and is untouched
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* w = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), w);
    void* foreign = reinterpret_cast<void*>(&fx::ForeignExport);
    fx::conflictCell = &w->vt[39];
    fx::conflictValue = foreign;
    fx::failRestore = true;
    overlay::ObserveChainInterface(overlay::kIidSwapChain4, w);
    Check(w->vt[39] == foreign && HasToken("reason extension_incomplete"), "R3-EXTFAIL: a conflicted foreign cell is never rolled back or owned");
    Check(HasToken("rollback_cells 0 pointer_failed 0 protection_failed 0"), "R3-EXTFAIL: a conflicted cell enters no rollback write set");
    fx::conflictCell = nullptr; fx::conflictValue = nullptr; fx::failRestore = false;
}

void CasePrivateDataBlobsAndFaults() {
    CaseHeader("R5-PRIV: arbitrary private blobs are declined without a payload read; a fault after each acquisition still releases every reference");
    for (int mode = 0; mode < 3; mode++) {
        ResetFixture();
        Install();
        fx::world.slFactory->innerFactory = nullptr;
        fx::world.slFactory->innerQueue = nullptr;
        CreateSlFactory(ResolveSlFactory2());
        fx::foreignBlobMode = mode;
        const LONG chainRefsBefore = fx::world.slChain->refs;
        IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
        Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "R5-PRIV: the creation still returns the chain");
        Check(HasToken("reason private_entry_exists") && !fx::world.slChain->hasCookie, "R5-PRIV: an existing blob of any size declines the capture");
        Check(fx::world.slChain->refs == chainRefsBefore + 1, "R5-PRIV: only the caller's own reference exists after the decline");
        if (pp) pp->Release();
        Check(fx::payloadReads == 0, "R5-PRIV: the payload path is never taken (zero/pointer-sized/oversized blob)");
        Check(fx::world.slChain->refs == chainRefsBefore, "R5-PRIV: nothing was retained for a declined blob");
    }
    fx::foreignBlobMode = -1;
    for (int fault = 1; fault <= 6; fault++) {
        ResetFixture();
        Install();
        fx::world.slFactory->innerFactory = nullptr;
        fx::world.slFactory->innerQueue = nullptr;
        CreateSlFactory(ResolveSlFactory2());
        const LONG chainRefsBefore = fx::world.slChain->refs;
        const LONG queueRefsBefore = fx::world.slQueue->refs;
        fx::faultAt = fault;
        IDXGISwapChain1* pp = CreateOuterChain(g_window, 2560, 1440, 3);
        Check(pp == reinterpret_cast<IDXGISwapChain1*>(fx::world.slChain), "R5-PRIV: a faulted capture still returns the game's chain");
        Check(fx::world.slChain->refs == chainRefsBefore + 1, "R5-PRIV: the caller's reference is the only retained chain reference after the fault");
        Check(fx::world.slQueue->refs <= queueRefsBefore + 1, "R5-PRIV: at most the chain's own attachment may still hold the queue after the fault");
        if (pp) pp->Release();
        DetachCookie(fx::world.slChain);                         // the fixture drops DXGI's private data: the attachment dies
        Check(fx::world.slChain->refs == chainRefsBefore, "R5-PRIV: nothing was retained on the chain");
        Check(fx::world.slQueue->refs == queueRefsBefore, "R5-PRIV: no queue reference survives the faulted capture (released exactly once)");
        fx::faultAt = 0;
    }
}

void CaseAttachmentRetirementInterleave() {
    CaseHeader("R5-INTERLEAVE: a qualification that survives the attachment is rejected under the render lock");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R5-INTERLEAVE: the generation is active");
    // T1 takes its metadata reference and blocks in the chain3 qualification, i.e. after qualification and before the lock
    fx::chain3QiBlockCount = 1;
    std::atomic<bool> done{ false };
    std::thread t([&] { fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0); done.store(true); });
    WorkerGuard guard{ &t };
    Check(fx::WaitChain3QiEntered(15000), "R5-INTERLEAVE: the presenting caller reached its chain3 qualification");
    const size_t draws = Draws();
    DetachCookie(fx::world.slChain);                        // the DXGI attachment is removed while the qualification holds metadata
    fx::ReleaseChain3Qi();
    t.join();
    Check(!fx::chain3QiTimeout && done.load(), "R5-INTERLEAVE: the presenting caller completed inside its bound");
    Check(fx::world.slChain->presents == 2, "R5-INTERLEAVE: the interleaved call still delegated the game's Present once");
    Check(Draws() == draws, "R5-INTERLEAVE: the retired attachment never activates or draws under the lock");
    Check(HasToken("reason attachment_retired"), "R5-INTERLEAVE: the locked recheck reports the retired attachment");
}

void CaseReceiptCapacityFailClosed() {
    CaseHeader("R5-RECEIPT: a capture without a publishable receipt fails closed instead of declaring readiness");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* first = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    fx::world.slFactory->chain = first;
    Check(CreateOuterChain(g_window, 2560, 1440, 3) == reinterpret_cast<IDXGISwapChain1*>(first), "R5-RECEIPT: the first capture returns the chain");
    void** sharedVt = first->vt;                            // one record, one receipt per distinct identity
    for (int i = 1; i < overlay::kMaxChainVt; i++) {
        fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
        c->vt = sharedVt;
        fx::world.slFactory->chain = c;
        Check(CreateOuterChain(g_window, 2560, 1440, 3) == reinterpret_cast<IDXGISwapChain1*>(c), "R5-RECEIPT: every capture returns its own chain");
        overlay::BindingCookie* k = CookieOf(c);
        Check(k != nullptr && k->state.load(std::memory_order_acquire) == overlay::CookieReady, "R5-RECEIPT: a live receipt publishes readiness");
        if (k) k->Release();
    }
    fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    c->vt = sharedVt;
    fx::world.slFactory->chain = c;
    Check(CreateOuterChain(g_window, 2560, 1440, 3) == reinterpret_cast<IDXGISwapChain1*>(c), "R5-RECEIPT: the over-capacity creation still returns the game's chain");
    Check(HasToken("reason receipt_unavailable"), "R5-RECEIPT: a capture that cannot publish a receipt fails closed");
    const auto pairs = fx::LinesWith("pair generation");
    Check(!pairs.empty() && fx::Field(pairs.back(), "ready") == "0", "R5-RECEIPT: the failed capture does not log readiness");
    Check(CookieOf(c) == nullptr, "R5-RECEIPT: no live receipt is resolvable for the failed capture");
    fx::editorOpen = true;
    const size_t draws = Draws();
    fx::GamePresent(c->vt, reinterpret_cast<IDXGISwapChain*>(c), 1, 0);
    Check(Draws() == draws && c->presents == 1, "R5-RECEIPT: a capture without a receipt never draws and still delegates");
}

void CaseAliasDependencyLoss() {
    CaseHeader("R3-ALIASLOSS: losing the non-presenting alias requirement stops the presenting entry from drawing");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::world.slChain->AddAlias(__uuidof(IDXGISwapChain3), fx::world.aliasChain);   // chain3 resolves to a distinct table
    CreateOuterChain(g_window, 2560, 1440, 3);
    int entryIdx = -1, aliasIdx = -1;
    for (int i = 0; i < overlay::kMaxChainVt; i++) {
        if (overlay::g_chainVt[i].vtable.load() == fx::world.slChain->vt) entryIdx = i;
        if (overlay::g_chainVt[i].vtable.load() == fx::world.aliasChain->vt) aliasIdx = i;
    }
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie != nullptr && cookie->depCount == 2, "R3-ALIASLOSS: the capture recorded two table dependencies");
    Check(entryIdx >= 0 && aliasIdx >= 0 && entryIdx != aliasIdx, "R3-ALIASLOSS: two distinct records were published");
    fx::editorOpen = true;
    // lose the NON-presenting alias's ResizeBuffers1 requirement: its owned cell is no longer ours
    fx::world.aliasChain->vt[39] = reinterpret_cast<void*>(&fx::ForeignExport);
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws, "R3-ALIASLOSS: the presenting entry must not draw when the alias requirement was lost");
    Check(fx::world.slChain->presents == 1, "R3-ALIASLOSS: the game's Present still ran exactly once");
    Check(HasToken("reason ownership_lost"), "R3-ALIASLOSS: the lost alias record is reported terminally");
    Check(aliasIdx >= 0 && overlay::g_chainVt[aliasIdx].state.load() == 3, "R3-ALIASLOSS: the alias record is terminal, never Ready");
    if (cookie) cookie->Release();
}

void CaseSelfThunkRejected() {
    CaseHeader("R3-SELFTHUNK: one of our own chain thunks in a missing slot is rejected, never saved as an original");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::Chain* c = fx::NewChain(g_window, fx::world.slDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), c);
    int recIdx = -1;
    for (int i = 0; i < overlay::kMaxChainVt; i++) if (overlay::g_chainVt[i].vtable.load() == c->vt) recIdx = i;
    Check(recIdx >= 0 && overlay::g_chainVt[recIdx].state.load() == 2, "R3-SELFTHUNK: the shorter-prefix record is Ready");
    void* ownThunk = overlay::g_chainThunks[overlay::kMaxChainVt - 1].resize1;   // our own thunk, not this record's saved original
    c->vt[39] = ownThunk;
    overlay::ObserveChainInterface(overlay::kIidSwapChain4, c);                  // the longer alias requires slot 39
    Check(HasToken("reason extension_unsupported"), "R3-SELFTHUNK: a chain thunk as a missing-slot candidate is rejected");
    Check(c->vt[39] == ownThunk, "R3-SELFTHUNK: the rejected cell is untouched");
    Check(overlay::g_chainVt[recIdx].state.load() == 3 && overlay::g_chainVt[recIdx].lost.load() == 1, "R3-SELFTHUNK: the record stays terminal, never a longer Ready alias");
    Check(overlay::g_chainVt[recIdx].savedResize1.load() != overlay::g_chainThunks[recIdx].resize1, "R3-SELFTHUNK: our own thunk is never saved as the downstream original");
}

void CasePresentQualifyFault() {
    CaseHeader("R5-PRESFAULT: a Present qualification fault is caught; the original still runs once and no frame is authorized");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    Check(fx::world.slChain->presents == 0 && Draws() == 0, "R5-PRESFAULT: baseline before the fault");
    fx::faultChain3Qi = 1;                                   // the next chain3 qualification raises after writing its output
    const bool escaped = fx::CallPresentCatchingFault(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(!escaped, "R5-PRESFAULT: no exception escapes before the original Present");
    Check(fx::world.slChain->presents == 1, "R5-PRESFAULT: the game's Present was delegated exactly once");
    Check(Draws() == 0, "R5-PRESFAULT: no frame is drawn after the qualification fault");
    Check(overlay::g_overlayStop.load(), "R5-PRESFAULT: the fault sets the sticky stop");
    Check(HasToken("reason chain_qualify_fault"), "R5-PRESFAULT: the caught qualification fault is reported");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(fx::world.slChain->presents == 2 && Draws() == 0, "R5-PRESFAULT: the sticky stop refuses every later frame");
}

void CaseResizeLookupFault() {
    CaseHeader("R1-RESFAULT: a qualification fault before entered sets the sticky stop, closes the ticket and leaks nothing");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-RESFAULT: the first generation is active");
    const long addRefsBefore = fx::addRefs, releasesBefore = fx::releases;
    fx::faultIdentityQi = 1;                                 // the next canonical identity QI writes its output and reference, then raises
    fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Check(fx::world.slChain->resizes == 1, "R1-RESFAULT: the original resize still ran exactly once");
    Check(overlay::g_overlayStop.load(), "R1-RESFAULT: the qualification fault sets the sticky stop before/after the original");
    Check(fx::addRefs - addRefsBefore == fx::releases - releasesBefore, "R1-RESFAULT: the faulted qualification released every reference it acquired");
    Check(overlay::g_resizeCalls == 0, "R1-RESFAULT: no ticket is left raised");
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws, "R1-RESFAULT: no further frame is authorized after the fault");
}

void CaseEnteredTicketFault() {
    CaseHeader("R1-TICKETFAULT: a fault inside the entered begin ticket is closed once, with the cookie retained until accounted");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-TICKETFAULT: the generation is active");
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie != nullptr, "R1-TICKETFAULT: the cookie is resolvable");
    const void* before = AssociatedQueue(fx::world.slChain);
    const long addRefsBefore = fx::addRefs, releasesBefore = fx::releases;
    fx::drainFn = &fx::FaultDrainHook;                 // the entered begin ticket faults at the GPU boundary (render state held)
    const HRESULT hr = fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    fx::drainFn = nullptr;
    Check(fx::world.slChain->resizes == 1, "R1-TICKETFAULT: the original resize ran exactly once");
    Check(hr == S_OK, "R1-TICKETFAULT: the original HRESULT is preserved");
    Check(overlay::g_overlayStop.load(), "R1-TICKETFAULT: the fault sets the sticky stop");
    Check(HasToken("ticket_closed_after_fault"), "R1-TICKETFAULT: the faulted ticket is closed terminally exactly once");
    Check(overlay::g_resizeCalls == 0, "R1-TICKETFAULT: the global barrier reached zero");
    Check(cookie->resizeCalls == 0 && !cookie->resizing.load(), "R1-TICKETFAULT: the retained cookie ticket was accounted");
    Check(cookie->renderQueueUnsupported, "R1-TICKETFAULT: the faulted association is refused, never committed");
    Check(AssociatedQueue(fx::world.slChain) == before, "R1-TICKETFAULT: no association is committed after the fault");
    Check(fx::addRefs - addRefsBefore == fx::releases - releasesBefore, "R1-TICKETFAULT: every reference the faulted transaction acquired was released");
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws, "R1-TICKETFAULT: no further frame is authorized");
    if (cookie) cookie->Release();
}

void CaseContendedTicketClose() {
    CaseHeader("R1-TICKETCONTEND: a contended terminal close waits, then accounts the retained cookie ticket (no later Finish)");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-TICKETCONTEND: the generation is active");
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie != nullptr, "R1-TICKETCONTEND: the cookie is resolvable");
    fx::drainFn = &fx::FaultDrainHook;                      // the entered begin ticket faults before its original
    fx::world.slChain->resizeBlockCount = 1;                // the original pauses, so the test takes render state before the close
    std::atomic<bool> done{ false };
    std::thread t([&] { fx::GameResize(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 0, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0); done.store(true); });
    WorkerGuard guard{ &t };
    Check(fx::WaitResizeEntered(15000), "R1-TICKETCONTEND: the faulted transaction reached its original");
    std::unique_lock<std::mutex> hold(overlay::g_renderMutex);   // declared after the guard: unlocked before its join
    fx::ReleaseResizeOriginal();
    Check(overlay::g_resizeCalls == 1, "R1-TICKETCONTEND: the contended close has not accounted the barrier yet");
    Check(cookie->resizeCalls == 1 && cookie->resizing.load(), "R1-TICKETCONTEND: the retained cookie ticket is still open while the close waits");
    hold.unlock();
    t.join();
    Check(done.load(), "R1-TICKETCONTEND: the resize call completed after the contention was released");
    Check(overlay::g_resizeCalls == 0, "R1-TICKETCONTEND: the waiting close accounted the global barrier");
    Check(cookie->resizeCalls == 0 && !cookie->resizing.load(), "R1-TICKETCONTEND: the waiting close accounted the retained cookie ticket");
    Check(cookie->renderQueueUnsupported && overlay::g_overlayStop.load(), "R1-TICKETCONTEND: the faulted association is refused");
    Check(HasToken("ticket_closed_after_fault"), "R1-TICKETCONTEND: the terminal close is reported once");
    Check(fx::world.slChain->resizes == 1, "R1-TICKETCONTEND: the original ran exactly once");
    fx::drainFn = nullptr;
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws, "R1-TICKETCONTEND: no further frame is authorized");
    if (cookie) cookie->Release();
}

void CaseLeaseAllocationFailure() {
    CaseHeader("R1-LEASEFAIL: an allocation failure after successful validation releases the acquired queue and commits nothing");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    CreateOuterChain(g_window, 2560, 1440, 3);
    fx::editorOpen = true;
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "R1-LEASEFAIL: the generation is active");
    overlay::BindingCookie* cookie = CookieOf(fx::world.slChain);
    Check(cookie != nullptr, "R1-LEASEFAIL: the cookie is resolvable");
    const void* before = AssociatedQueue(fx::world.slChain);
    fx::world.slQueue2->dev = fx::world.slDev;
    IUnknown* one[1] = { reinterpret_cast<IUnknown*>(fx::world.slQueue2) };
    UINT node1[1] = { 1 };
    const long addRefsBefore = fx::addRefs, releasesBefore = fx::releases;
    fx::armAllocFailOnDesc = true;                          // queue validation succeeds; the lease allocation fails
    bool threw = false;
    HRESULT hr = E_PENDING;
    try { hr = fx::GameResize1(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain3*>(fx::world.slChain), 1, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0, node1, one); }
    catch (...) { threw = true; }
    Check(!threw, "R1-LEASEFAIL: the allocation failure is contained inside the added-work guard");
    Check(hr == S_OK, "R1-LEASEFAIL: the original resize kept its HRESULT");
    Check(fx::world.slChain->resizes1 == 1, "R1-LEASEFAIL: the original ran exactly once");
    Check(fx::addRefs - addRefsBefore == fx::releases - releasesBefore, "R1-LEASEFAIL: the qualification's queue reference is released, not leaked");
    Check(AssociatedQueue(fx::world.slChain) == before, "R1-LEASEFAIL: no association is committed");
    Check(cookie->renderQueueUnsupported && overlay::g_overlayStop.load(), "R1-LEASEFAIL: the failed association is refused");
    Check(overlay::g_resizeCalls == 0 && cookie->resizeCalls == 0 && !cookie->resizing.load(), "R1-LEASEFAIL: the ticket closed");
    const size_t draws = Draws();
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == draws, "R1-LEASEFAIL: no further frame is authorized");
    if (cookie) cookie->Release();
}

void CaseRealLayoutImportDir() {
    CaseHeader("A-REALDIR: an unaligned real-layout import directory is accepted and tapped; bounds predicates still reject");
    Check((0x1002 % alignof(IMAGE_IMPORT_DESCRIPTOR)) == 2, "A-REALDIR: the modeled directory RVA is not 4-aligned");
    // the bounds predicates still reject: the same unaligned RVA outside the image
    ResetFixture();
    fx::imageImportDirRva = 0x1FFF2;                             // mod 4 == 2 but beyond SizeOfImage
    fx::BuildImage();
    Install();
    Check(HasToken("image_reject import_dir") && HasToken("image_unavailable"), "A-REALDIR: an unaligned directory outside the image is still rejected");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-REALDIR: an out-of-image directory publishes no tap");
    // and a truncated unaligned directory
    ResetFixture();
    fx::imageImportDirRva = 0x1002;
    fx::imageImportDirSize = 0x10;                               // smaller than one descriptor
    fx::BuildImage();
    Install();
    Check(HasToken("image_reject import_dir"), "A-REALDIR: a truncated unaligned directory is still rejected");
    Check(*fx::cellFactory2 == reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-REALDIR: a truncated directory publishes no tap");
    fx::imageImportDirRva = 0; fx::imageImportDirSize = 0;
    // the real layout, accepted last so its diagnostics remain the case evidence
    ResetFixture();
    fx::imageImportDirRva = 0x1002;                              // import dir RVA mod 4 == 2, the shipped image's shape
    fx::imageSizeOfHeaders = 0x400;                              // the real header window (secBase 0x188 + 12*40 fits)
    fx::BuildImage();                                            // the descriptor table moves to the unaligned RVA
    Install();
    const std::string inst = OneLine("WB_BINDING_FUNCTIONAL install pid");
    Check(fx::Field(inst, "factory_create") == "owned" && fx::Field(inst, "factory_create1") == "owned" && fx::Field(inst, "factory_create2") == "owned", "A-REALDIR: the unaligned import directory's factory cells are tapped");
    Check(fx::Field(inst, "resolver") == "owned", "A-REALDIR: the resolver cell in the unaligned table is tapped");
    Check(!HasToken("image_reject import_dir") && !HasToken("image_unavailable"), "A-REALDIR: the real-layout import directory is not rejected");
    Check(fx::Field(inst, "descriptors") == "3", "A-REALDIR: the walk read the relocated descriptor table");
    Check(*fx::cellFactory2 != reinterpret_cast<void*>(&fx::ExportCreateFactory2), "A-REALDIR: the factory import cell holds the production thunk");
    Check(*fx::cellResolver != reinterpret_cast<void*>(&fx::NativeGetProcAddress), "A-REALDIR: the resolver cell holds the production thunk");
    fx::editorOpen = true;
    IDXGIFactory4* f4 = CreateSlFactory(ResolveSlFactory2());
    Check(f4 == reinterpret_cast<IDXGIFactory4*>(fx::world.slFactory), "A-REALDIR: the resolver tap returns the final factory");
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(fx::world.slChain->hasCookie, "A-REALDIR: the binding is captured through the real-layout taps");
    fx::GamePresent(fx::world.slChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.slChain), 1, 0);
    Check(Draws() == 1, "A-REALDIR: the binding path draws through the tapped table");
}

void CaseForeignWrapperChurn() {
    CaseHeader("W-WRAPCHURN (diagnosis pin): a foreign wrapper table re-created per generation loses ownership after one publish");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());                        // the same-module table the fixture normally returns
    // model the live stack: the game's factory object carries a wrapper-module clone table and forwards creation calls
    // to an inner real factory
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable;
    // generation 1: the wrapped factory is observed and interposed (the live 16:04 first-return behaviour)
    IDXGIFactory4* gen1 = CreateSlFactory(ResolveSlFactory2());
    Check(gen1 == reinterpret_cast<IDXGIFactory4*>(fx::world.slFactory), "W-WRAPCHURN: the wrapped factory is returned unchanged");
    Check(HasToken("factory_return api IDXGIFactory4") && HasToken("slots ready"), "W-WRAPCHURN: the foreign wrapper table is interposed on the first return");
    Check(fx::ProtectWritesFor(&wrapperTable[0]) == 1 && fx::ProtectWritesFor(&wrapperTable[15]) == 1, "W-WRAPCHURN: the wrapper table's QI and creation cells were written once each");
    Check(wrapperTable[0] != reinterpret_cast<void*>(&fx::WrapperFactoryQi), "W-WRAPCHURN: the wrapper QI cell holds the production thunk after the first pass");
    // generation 2: a new wrapper object re-creates its table at the same address (short-lived proxy churn)
    fx::RewrapGeneration();
    fx::world.slFactory->identity = fx::NewIdent();
    IDXGIFactory4* gen2 = CreateSlFactory(ResolveSlFactory2());
    Check(gen2 == reinterpret_cast<IDXGIFactory4*>(fx::world.slFactory), "W-WRAPCHURN: the next wrapped factory is returned unchanged");
    Check(wrapperTable[0] == reinterpret_cast<void*>(&fx::WrapperFactoryQi), "W-WRAPCHURN: the fresh wrapper generation reverted the interposition");
    Check(HasToken("delegate factory_return reason ownership_lost table"), "W-WRAPCHURN: the first re-observation reports ownership loss (the live log shape)");
    // every later return stays terminal and never touches the foreign table again
    const size_t writesAfterLoss = fx::ProtectWritesFor(&wrapperTable[0]);
    for (int i = 0; i < 3; i++) CreateSlFactory(ResolveSlFactory2());
    Check(fx::CountWith("reason extension_unsupported") == 1, "W-WRAPCHURN: the terminal state is reported once per table, not once per return");
    Check(fx::ProtectWritesFor(&wrapperTable[0]) == writesAfterLoss && fx::ProtectWritesFor(&wrapperTable[15]) == 1, "W-WRAPCHURN: the terminal state short-circuits before any further VirtualProtect/CAS on the foreign table");
    Check(!fx::world.slChain->hasCookie, "W-WRAPCHURN: this stack captures no binding on the current source");
}

void CaseChain4IidConstant() {
    CaseHeader("G-C4IID: the canonical IDXGISwapChain4 IID constant resolves the chain the game QIs");
    ResetFixture();
    fx::Chain* c = fx::NewChain(g_window, fx::world.dxgiDev, 2560, 1440, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    IUnknown* got = nullptr;
    const HRESULT hr = c->QueryInterface(overlay::kIidSwapChain4, reinterpret_cast<void**>(&got));
    Check(SUCCEEDED(hr) && got == reinterpret_cast<IUnknown*>(c), "G-C4IID: our kIidSwapChain4 is the canonical IDXGISwapChain4 IID");
    if (got) got->Release();
    bool hasPresent1 = false, hasResize1 = false;
    Check(overlay::ChainIidSupported(overlay::kIidSwapChain4, &hasPresent1, &hasResize1) && hasPresent1 && hasResize1, "G-C4IID: the constant declares the ResizeBuffers1-capable prefix");
    IUnknown* wrong = nullptr;
    const HRESULT bad = c->QueryInterface(fx::kIidSwapChain4Former, reinterpret_cast<void**>(&wrong));
    Check(FAILED(bad) && wrong == nullptr, "G-C4IID: the former (non-canonical) tail is not an interface of the chain");
}

void CaseGameSideBoundary() {
    CaseHeader("G-BOUNDARY: the game-side creation boundary is resolved, hooked and routed into the existing capture machinery");
    ResetFixture();
    Install();
    Check(HasToken("game_boundary installed"), "G-BOUNDARY: the game-side helper was resolved and hooked");
    Check(fx::boundaryHooked && fx::boundaryTarget != nullptr, "G-BOUNDARY: the fixture installer patched the modeled helper head");
    // the factory the game holds: a wrapper-module clone table forwarding to an inner real factory
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable;
    void* host = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);                 // the game calls its own helper
        Check(static_cast<HRESULT>(reinterpret_cast<intptr_t>(fx::RunBoundaryHelper(host))) == S_OK, "G-BOUNDARY: the original helper ran exactly once through the detour and kept its HRESULT");
    }
    Check(fx::CountWith("game_boundary capture chain") == 1, "G-BOUNDARY: exactly one capture ran through the game's own call");
    Check(fx::world.dxgiChain->hasCookie, "G-BOUNDARY: the cookie attached to the wrapper swapchain the game stored");
    Check(HasToken("pair generation 1") && HasToken("ready 1"), "G-BOUNDARY: the capture paired the queue from the same call");
    const std::string pair = OneLine("pair generation 1");
    Check(fx::Field(pair, "queue") == fx::PtrText(fx::world.dxgiQueue), "G-BOUNDARY: the paired queue is the game's own queue argument");
    Check(AssociatedQueue(fx::world.dxgiChain) == fx::world.dxgiQueue, "G-BOUNDARY: the committed association is the game's queue");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.dxgiChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.dxgiChain), 1, 0);
    Check(Draws() == 1, "G-BOUNDARY: the binding draws through the swapchain the game presents");
    Check(fx::ProtectWritesFor(&wrapperTable[0]) == 0 && fx::ProtectWritesFor(&wrapperTable[10]) == 0 && fx::ProtectWritesFor(&wrapperTable[15]) == 0,
          "G-BOUNDARY: no foreign wrapper factory table cell was ever CASed by this route");
}

void CaseBoundaryUnwindChains() {
    CaseHeader("G-UNWIND: the .pdata walk follows real chain info, ignores handler tails, and fails closed on a cycle");
    ResetFixture();
    Install();
    Check(HasToken("game_boundary installed"), "G-UNWIND: the plain .pdata entry resolves and installs");
    ResetFixture();
    fx::boundaryUnwindMode = 1;                                  // fragment entry with UNW_FLAG_CHAININFO
    fx::BuildImage();
    Install();
    {
        const std::string inst = OneLine("game_boundary installed");
        Check(fx::Field(inst, "head") == fx::PtrText(fx::boundaryTarget), "G-UNWIND: a chain-info fragment resolves to the primary head");
    }
    ResetFixture();
    fx::boundaryUnwindMode = 2;                                  // EHANDLER tail: must not be parsed as chain info
    fx::BuildImage();
    Install();
    {
        const std::string inst = OneLine("game_boundary installed");
        Check(fx::Field(inst, "head") == fx::PtrText(fx::boundaryTarget), "G-UNWIND: an EHANDLER tail is never parsed as chain info");
    }
    ResetFixture();
    fx::boundaryUnwindMode = 4;                                  // 17 links: the 16-level budget is exhausted
    fx::BuildImage();
    Install();
    Check(HasToken("game_boundary_resolve_failed") && !HasToken("game_boundary installed"), "G-UNWIND: a chain exceeding the 16-level budget declines installation");
    ResetFixture();
    fx::boundaryUnwindMode = 3;                                  // cyclic chain: fail closed
    fx::BuildImage();
    Install();
    Check(HasToken("game_boundary_resolve_failed") && !HasToken("game_boundary installed"), "G-UNWIND: a cyclic chain declines installation");
}

void CaseBoundaryHeadSignatureRequired() {
    CaseHeader("G-HEADSIG: a head-signature mismatch after the sanity prefix fails closed even with a valid string anchor");
    ResetFixture();
    fx::boundaryHeadSigCorrupt = true;                           // bytes 17..35 differ; string/lea/.pdata/site unchanged
    fx::BuildImage();
    Install();
    Check(HasToken("game_boundary_resolve_failed") && !HasToken("game_boundary installed"), "G-HEADSIG: a missing full head signature declines installation");
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
    overlay::ImageInfo a = {}; a.base = reinterpret_cast<uintptr_t>(readable); a.size = 0x600;
    bool complete = false;
    const uintptr_t absent = overlay::GameBoundaryFuncReferencingString(a, "no such anchor string present", &complete);
    Check(absent == 0 && complete, "G-REFSCAN: a completed scan without the anchor reports absence, not incompleteness");
    complete = false;
    overlay::GameBoundaryFuncReferencingString(a, fx::kBoundaryString, &complete);
    Check(complete, "G-REFSCAN: a fully readable image yields a complete reference scan");
    unsigned char* partial = build(true, 0x240);
    overlay::ImageInfo b = {}; b.base = reinterpret_cast<uintptr_t>(partial); b.size = 0x240;
    bool partialComplete = true;
    const uintptr_t none = overlay::GameBoundaryFuncReferencingString(b, fx::kBoundaryString, &partialComplete);
    Check(!partialComplete && none == 0, "G-REFSCAN: a section-header read failure marks the reference scan incomplete and yields no head");
    bool patternComplete = true; int patternHits = 0;
    const unsigned char val[2] = { 0x48, 0x8D }, mask[2] = { 0xFF, 0xFF };
    overlay::GameBoundaryScan(b, true, val, mask, 2, &patternHits, &patternComplete);
    Check(!patternComplete, "G-REFSCAN: the byte-pattern scanner reports the same partial section walk as incomplete");
}

void CaseBoundaryQueueZeroThenReal() {
    CaseHeader("G-BOUNDARY-Q0: a boot capture with the queue member still zero retires cleanly; the next call with the real queue binds and activates");
    ResetFixture();
    Install();
    Check(HasToken("game_boundary installed"), "G-BOUNDARY-Q0: the game-side boundary is installed");
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable;
    // 1) the boot swapchain: the game's queue member is not set yet (this+0x38 -> +0x3e8 == 0)
    void* hostBoot = fx::BuildBoundaryHost(fx::world.slFactory, nullptr, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(hostBoot);
    }
    Check(HasToken("delegate create reason no_d3d12_queue"), "G-BOUNDARY-Q0: the queue-less capture declines with no_d3d12_queue");
    Check(HasToken("game_boundary capture outcome declined"), "G-BOUNDARY-Q0: the boundary reports the decline");
    Check(!fx::world.dxgiChain->hasCookie, "G-BOUNDARY-Q0: nothing is attached for the declined capture (retired cleanly)");
    // 2) the second swapchain (CLAUDE.md rule 7) with the real queue: the same chain object binds and activates
    void* hostReal = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(hostReal);
    }
    Check(HasToken("game_boundary capture outcome bound"), "G-BOUNDARY-Q0: the capture with the real queue completes the binding");
    Check(fx::world.dxgiChain->hasCookie, "G-BOUNDARY-Q0: the cookie is attached to the wrapper swapchain");
    Check(HasToken("pair generation 1") && HasToken("ready 1"), "G-BOUNDARY-Q0: the pair is published ready");
    fx::editorOpen = true;
    fx::GamePresent(fx::world.dxgiChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.dxgiChain), 1, 0);
    Check(Draws() == 1, "G-BOUNDARY-Q0: Present activates and the menu path draws");
}

void CaseIdentityAmendment() {
    CaseHeader("G-IDENT-AMEND: a game-boundary capture accepts a split device-wrapper identity; the factory route still declines it");
    // 1) GameBoundary: the interposer hands the chain a different device wrapper -> accepted, pairing completes, menu draws
    ResetFixture();
    Install();
    Check(HasToken("game_boundary installed"), "G-IDENT-AMEND: the boundary is installed");
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable;
    fx::world.dxgiChain->dev = fx::world.slDev2;                 // different device wrapper for the chain
    void* host = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(host);
    }
    Check(HasToken("game_boundary capture outcome bound"), "G-IDENT-AMEND: the boundary-attested capture completes the binding");
    Check(fx::world.dxgiChain->hasCookie, "G-IDENT-AMEND: the cookie is attached to the wrapper swapchain");
    {
        const std::string pair = OneLine("pair generation 1");
        Check(fx::Field(pair, "device_match") == "0" && fx::Field(pair, "boundary_attested") == "1" && fx::Field(pair, "ready") == "1",
              "G-IDENT-AMEND: the pair logs the real comparison and the boundary attestation");
    }
    fx::editorOpen = true;
    fx::GamePresent(fx::world.dxgiChain->vt, reinterpret_cast<IDXGISwapChain*>(fx::world.dxgiChain), 1, 0);
    Check(Draws() == 1, "G-IDENT-AMEND: Present activates and the menu path draws");
    // 2) Factory origin: the identical mismatch must still decline
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = nullptr;
    fx::world.slFactory->innerQueue = nullptr;
    CreateSlFactory(ResolveSlFactory2());
    fx::world.slChain->dev = fx::world.slDev2;                   // queue dev (slDev) != chain dev (slDev2)
    CreateOuterChain(g_window, 2560, 1440, 3);
    Check(HasToken("delegate create reason identity_failed origin factory"), "G-IDENT-AMEND: the factory route declines the same wrapper-identity split");
    Check(!fx::world.slChain->hasCookie, "G-IDENT-AMEND: the declined factory capture attaches nothing");
    // 3) a null canonical chain id declines on the boundary route too
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable2 = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable2;
    fx::world.dxgiChain->identityFail = true;                    // CanonicalIdentity(returned) fails -> null chain id
    void* host2 = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(host2);
    }
    Check(HasToken("delegate create reason identity_failed"), "G-IDENT-AMEND: a null canonical chain id declines the capture");
    Check(!fx::world.dxgiChain->hasCookie && HasToken("game_boundary capture outcome declined"), "G-IDENT-AMEND: nothing is attached for the null-id decline");
    // 4) a non-single-node device declines on the boundary route too
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable3 = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable3;
    fx::world.dxgiDev->nodes = 2;                                // the queue's device is not single-node
    void* host3 = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(host3);
    }
    Check(HasToken("delegate create reason device_not_single_node"), "G-IDENT-AMEND: a non-single-node device still declines");
    Check(!fx::world.dxgiChain->hasCookie, "G-IDENT-AMEND: no binding for the multi-node decline");
}

void CaseMask15Unauthorized() {
    CaseHeader("G-MASK15: the chain4 required mask stays 31; coverage 15 with an unusable slot 39 does not authorize a capture");
    ResetFixture();
    Install();
    fx::world.slFactory->innerFactory = fx::world.dxgiFactory[2];
    fx::world.slFactory->innerQueue = fx::world.dxgiQueue;
    void** wrapperTable = fx::BuildWrapperFactory(fx::reshadeModule);
    fx::world.slFactory->vt = wrapperTable;
    Check(overlay::ChainMaskForIid(overlay::kIidSwapChain4) == 31u, "G-MASK15: the returned-interface requirement is the full 31-bit prefix");
    overlay::ObserveChainInterface(__uuidof(IDXGISwapChain1), fx::world.dxgiChain);   // coverage 15 only
    fx::world.dxgiChain->vt[39] = nullptr;                       // the wrapper table cannot provide ResizeBuffers1
    void* host = fx::BuildBoundaryHost(fx::world.slFactory, fx::world.dxgiQueue, g_window, 2560, 1440, 0);
    {
        fx::CallerScope scope(fx::mainModule);
        fx::RunBoundaryHelper(host);
    }
    Check(HasToken("delegate create reason slots_unavailable"), "G-MASK15: a mask-15 record does not authorize the chain4 capture");
    Check(HasToken("game_boundary capture outcome declined") && HasToken("cookie_state 0"), "G-MASK15: the capture declines with the cookie unready");
    {
        const std::string pair = OneLine("pair generation 1");
        Check(fx::Field(pair, "ready") == "0", "G-MASK15: coverage 15 with an unusable slot 39 never publishes ready");
    }
}

void DumpLogs() {
    std::cout << "--- captured overlay diagnostics ---\n";
    for (const auto& l : fx::logs) std::cout << l << "\n";
}
void WriteEvidence() {
    if (evidence.empty()) return;
    std::ofstream f(evidence / "overlay_binding.log");
    for (const auto& l : fx::logs) f << l << "\n";
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows which case was running
    if (argc > 1) { evidence = argv[1]; std::error_code ec; std::filesystem::create_directories(evidence, ec); }
    fx::dxgiModule = GetModuleHandleA(nullptr);
    fx::moduleNames[fx::slModule] = "sl.interposer.dll";
    fx::moduleNames[fx::reshadeModule] = "reshade.asi";
    fx::moduleNames[fx::foreignModule] = "user32.dll";
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "wbHostWindow";
    if (!RegisterClassExA(&wc)) { std::cout << "window class registration failed\n"; return 2; }
    g_window = CreateWindowExA(0, "wbHostWindow", "wb1", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    g_window2 = CreateWindowExA(0, "wbHostWindow", "wb2", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window || !g_window2) { std::cout << "window creation failed\n"; return 2; }
    std::set<std::string> selected;
    char onlyBuf[256] = {}; size_t onlyLen = 0;
    if (getenv_s(&onlyLen, onlyBuf, sizeof onlyBuf, "WB_OB_CASES") == 0 && onlyLen > 0) {
        std::string list(onlyBuf), item;
        for (char c : list + ",") { if (c == ',') { if (!item.empty()) selected.insert(item); item.clear(); } else item.push_back(c); }
    }
    const bool all = selected.empty();
    auto want = [&](const char* name) { return all || selected.count(name) != 0; };
    try {
        if (want("C1")) CaseMissedPath();
        if (want("C2")) CaseWrapperNesting();
        if (want("C3")) CaseProviderDispatch();
        if (want("C4")) CaseInterfaceAliases();
        if (want("C5")) CaseMultipleConcurrent();
        if (want("C6")) CaseCreationFilters();
        if (want("C7")) CaseSlotOwnership();
        if (want("C8")) CasePresentSelection();
        if (want("C9")) CaseCookieAccounting();
        if (want("C10")) CaseResizeGeneration();
        if (want("C11")) CaseConcurrencyGates();
        if (want("C12")) CaseDeadObject();
        if (want("A-PE")) CaseRealImageBounds();
        if (want("A-REALDIR")) CaseRealLayoutImportDir();
        if (!all && want("W-WRAPCHURN")) CaseForeignWrapperChurn();   // opt-in diagnosis pin: expected to pass only after the wrap-route amendment
        if (want("G-C4IID")) CaseChain4IidConstant();
        if (want("G-BOUNDARY")) CaseGameSideBoundary();
        if (want("G-UNWIND")) CaseBoundaryUnwindChains();
        if (want("G-HEADSIG")) CaseBoundaryHeadSignatureRequired();
        if (want("G-REFSCAN")) CaseReferenceScanCompleteness();
        if (want("G-BOUNDARY-Q0")) CaseBoundaryQueueZeroThenReal();
        if (want("G-IDENT-AMEND")) CaseIdentityAmendment();
        if (want("G-MASK15")) CaseMask15Unauthorized();
        if (want("A-PIN")) CasePinPrerequisites();
        if (want("A-ERR")) CaseResolverErrorPreservation();
        if (want("A-LOCK")) CaseLockExclusion();
        if (want("A-CAP")) CaseFactoryCaptureOwnership();
        if (want("A-PRIV")) CaseExistingPrivateEntry();
        if (want("B-QUEUE")) CaseResizeQueueCommit();
        if (want("B-REBIND")) CaseRebindPolicy();
        if (want("B-DEAD")) CaseDeadStorageReuse();
        if (want("C-EXT")) CaseExtensionAtomicity();
        if (want("C-CAP")) CaseCapacityLimits();
        if (want("C-ALIAS")) CaseAliasRouting();
        if (want("C-EDGE")) CaseHwndAndChainEdges();
        if (want("C-OVERLAP")) CaseResizeOverlap();
        if (want("R1-OVERLAP")) CaseResizeSameCookieOverlap();
        if (want("R1-FINISH")) CaseResizeBlockedFinish();
        if (want("R1-BARRIER")) CaseResizeBarrierHeldDraw();
        if (want("R6-DRAIN")) CaseDrainGateTimeout();
        if (want("R2-PINREENTRY")) CasePinReentry();
        if (want("R3-EXTFAIL")) CaseExtensionFailureMatrix();
        if (want("R5-PRIV")) CasePrivateDataBlobsAndFaults();
        if (want("R5-INTERLEAVE")) CaseAttachmentRetirementInterleave();
        if (want("R5-RECEIPT")) CaseReceiptCapacityFailClosed();
        if (want("R3-ALIASLOSS")) CaseAliasDependencyLoss();
        if (want("R3-SELFTHUNK")) CaseSelfThunkRejected();
        if (want("R5-PRESFAULT")) CasePresentQualifyFault();
        if (want("R1-RESFAULT")) CaseResizeLookupFault();
        if (want("R1-TICKETFAULT")) CaseEnteredTicketFault();
        if (want("R1-TICKETCONTEND")) CaseContendedTicketClose();
        if (want("R1-LEASEFAIL")) CaseLeaseAllocationFailure();
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        std::cout << "LAST_FAULT code=0x" << std::hex << cdk::GuardCode() << " rip=0x" << cdk::t_fault.ctx.Rip << std::dec << "\n";
        struct Sym { const char* name; const void* addr; };
        const Sym syms[] = {
            { "ChainQi", (void*)&fx::ChainQi }, { "ChainGetDevice", (void*)&fx::ChainGetDevice }, { "ChainGetDesc", (void*)&fx::ChainGetDesc },
            { "ChainGetHwnd", (void*)&fx::ChainGetHwnd }, { "ChainSetPrivateDataInterface", (void*)&fx::ChainSetPrivateDataInterface },
            { "ChainGetPrivateData", (void*)&fx::ChainGetPrivateData }, { "QueueGetDevice", (void*)&fx::QueueGetDevice },
            { "QueueGetDesc", (void*)&fx::QueueGetDescShim }, { "DeviceGetNodeCount", (void*)&fx::DeviceGetNodeCount },
            { "ObjQi", (void*)&fx::ObjQi }, { "ObjAddRef", (void*)&fx::ObjAddRef }, { "ObjRelease", (void*)&fx::ObjRelease },
            { "ObjQiRaw", (void*)&fx::ObjQiRaw }, { "GameCallHwnd", (void*)&fx::GameCallHwnd }, { "GamePresent", (void*)&fx::GamePresent },
            { "SlFactoryHwnd", (void*)&fx::SlFactoryHwnd }, { "FactoryHwnd", (void*)&fx::FactoryHwnd }, { "InnerFactoryHwnd", (void*)&fx::InnerFactoryHwnd },
            { "ChainPresent", (void*)&fx::ChainPresent }, { "ChainResize", (void*)&fx::ChainResize }, { "ChainResize1", (void*)&fx::ChainResize1 },
        };
        const uintptr_t rip = (uintptr_t)cdk::t_fault.ctx.Rip;
        const uintptr_t modBase = (uintptr_t)GetModuleHandleA(nullptr);
        bool named = false;
        for (const auto& sy : syms) {
            const uintptr_t a = (uintptr_t)sy.addr;
            if (rip >= a && rip < a + 0x600) { std::cout << "  fault in " << sy.name << " +0x" << std::hex << (rip - a) << std::dec << "\n"; named = true; }
        }
        if (!named) std::cout << "  fault not inside a registered fixture slot (module+0x" << std::hex << (rip - modBase) << std::dec << ")\n";
        std::cout << "  thunks:";
        for (int i = 0; i < 6 && i < overlay::kMaxChainVt; i++) std::cout << " chain" << i << "=" << (void*)overlay::g_chainThunks[i].present << " qi=" << (void*)overlay::g_chainThunks[i].qi;
        for (int i = 0; i < 6 && i < overlay::kMaxFactoryVt; i++) std::cout << " factory" << i << "=" << (void*)overlay::g_factoryThunks[i].hwnd;
        for (int i = 0; i < 6 && i < overlay::kMaxFnRecs; i++) std::cout << " fn" << i << "=" << overlay::g_fnThunkA[i];
        std::cout << "\n";
        WriteEvidence();
        DumpLogs();
        return 1;
    }
    WriteEvidence();
    std::cout << "ASSERTIONS=" << assertions << "\n";
    return 0;
}