// D3D12 overlay. The DXGI/Streamline capture and swapchain-lifecycle layer is
// adapted from CrimsonRoute: intercept the factories actually used by the game,
// unwrap native Streamline interfaces, validate the DIRECT queue/device/window,
// then hook the real Present/Resize path. World Builder keeps its own ImGui,
// editor, input and thumbnail renderer on top of that capture layer.
#include "core.h"
#include "input.h"
#include "overlay.h"
#include "guard.h"
#include "thumbgen.h"
#include "icons.h"
#include "i18n.h"
#include "overlay_discovery.h"
#include <memory>
void* CdHeapAlloc(size_t n); void* CdHeapRealloc(void* p, size_t n); void CdHeapFree(void* p);   // heap.cpp
#define STBI_MALLOC(sz)        CdHeapAlloc(sz)
#define STBI_REALLOC(p, newsz) CdHeapRealloc(p, newsz)
#define STBI_FREE(p)           CdHeapFree(p)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <map>
#include <set>
#include <deque>
#include <mutex>
#include <chrono>
#include <string>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <vector>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace editor { void Draw(); void Toggle(); bool IsOpen(); void ApplyStyle(float scale); bool PlayMode(); void TogglePlay(); void ToggleCameraMode(); bool Placing(); bool MouseMode(); }

namespace overlay {
    typedef HRESULT (WINAPI* CreateDXGIFactory_t)(REFIID, void**);
    typedef HRESULT (WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
    typedef HRESULT (WINAPI* CreateDXGIFactory2_t)(UINT, REFIID, void**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChainForCoreWindow_t)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChainForComposition_t)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    typedef HRESULT (STDMETHODCALLTYPE* SetColorSpace1_t)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

    static CreateDXGIFactory_t oCreateDXGIFactory = nullptr;
    static CreateDXGIFactory1_t oCreateDXGIFactory1 = nullptr;
    static CreateDXGIFactory2_t oCreateDXGIFactory2 = nullptr;
    static CreateDXGIFactory_t oStreamlineCreateDXGIFactory = nullptr;
    static CreateDXGIFactory1_t oStreamlineCreateDXGIFactory1 = nullptr;
    static CreateDXGIFactory2_t oStreamlineCreateDXGIFactory2 = nullptr;
    static FactoryCreateSwapChain_t oCreateSwapChain = nullptr;
    static FactoryCreateSwapChainForHwnd_t oCreateSwapChainForHwnd = nullptr;
    static FactoryCreateSwapChainForCoreWindow_t oCreateSwapChainForCoreWindow = nullptr;
    static FactoryCreateSwapChainForComposition_t oCreateSwapChainForComposition = nullptr;
    static Present_t oPresent = nullptr;
    static Present1_t oPresent1 = nullptr;
    static ResizeBuffers_t oResizeBuffers = nullptr;
    static ResizeBuffers1_t oResizeBuffers1 = nullptr;
    static SetColorSpace1_t oSetColorSpace1 = nullptr;
    // One set of factory method hooks only, first wins (InstallSingleHook logs the other provider as a mismatch): hooking
    // Streamline's proxy methods on top of the real ones captured the chain twice and the game died with ACCESS_DENIED.
    static std::atomic<bool> g_streamlineFactoryExportsHooked{false};

    struct HookTarget { void* target = nullptr; bool installed = false; };
    static HookTarget g_dxgiExportHookTargets[3];
    static HookTarget g_streamlineExportHookTargets[3];
    static HookTarget g_factoryHookTargets[4];
    static HookTarget g_presentHookTarget;
    static HookTarget g_present1HookTarget;
    static HookTarget g_resizeHookTarget;
    static HookTarget g_resize1HookTarget;
    static HookTarget g_colorSpaceHookTarget;
    static std::atomic<uint32_t> g_dxgiExportHookMask{0};
    static std::atomic<uint32_t> g_streamlineExportHookMask{0};
    static std::atomic<uint32_t> g_factoryImportHookMask{0};
    static std::atomic<uint32_t> g_factoryMethodHookMask{0};
    static std::atomic<uint32_t> g_swapChainMethodHookMask{0};
    static std::atomic<long> g_factoryProbeResult{E_NOINTERFACE};
    static std::atomic<int> g_presentHookReady{0};
    static std::mutex g_factoryHookMutex;
    static std::mutex g_functionHookMutex;
    static std::set<void*> g_seenFactoryVtables; // protected by g_factoryHookMutex

    // Only the outer creation owns its result. A verified game helper suppresses
    // incidental SL/ReShade creations until the final game-stored chain is known.
    static thread_local unsigned t_createDepth = 0, t_observerDepth = 0, t_helperDepth = 0;
    struct DepthScope {
        unsigned& depth;
        explicit DepthScope(unsigned& value) : depth(value) { ++depth; }
        ~DepthScope() { --depth; }
        DepthScope(const DepthScope&) = delete; DepthScope& operator=(const DepthScope&) = delete;
    };
    template<class T> struct ComOwner {
        T* value = nullptr;
        ~ComOwner() { if (value) value->Release(); }
        ComOwner() = default;
        ComOwner(const ComOwner&) = delete; ComOwner& operator=(const ComOwner&) = delete;
    };
#ifdef WB_OVERLAY_BINDING_TEST
    // Only external GPU work and module lookup are substitutable; policy stays production.
    static void (*g_testFrameSink)(IDXGISwapChain3*, ID3D12CommandQueue*, uint64_t) = nullptr;
    static bool (*g_testDrainFn)() = nullptr;
    static bool (*g_testRebindFn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT) = nullptr;
    static void* (*g_testNativeInterface)(void*) = nullptr;
    static bool (*g_testPinTarget)(void*) = nullptr;
    static void (*g_testResizePhase)(int, uint64_t) = nullptr; // event-only ordering seam: begin / commit / closed
    static void (*g_testAfterPresentPrepare)() = nullptr; // event-only: render lock held, no menu publication yet
#endif
    static std::atomic<uint64_t> g_frameAttempts{0}, g_frameAdmitted{0}, g_frameSkipped{0}, g_frameConsecutiveSkips{0};

    constexpr size_t kCapturedSwapChains = 16;
    constexpr size_t kMaximumCapturedPresentQueues = 16;
    struct QueueLease {
        ID3D12CommandQueue* queue;
        explicit QueueLease(ID3D12CommandQueue* q) : queue(q) { queue->AddRef(); }
        ~QueueLease() { queue->Release(); }
        QueueLease(const QueueLease&) = delete; QueueLease& operator=(const QueueLease&) = delete;
    };
    struct CapturedD3D12Queue {
        // Tokens only, never retained/dereferenced after a callback. DXGI owns the
        // attachment below; destroying it retires this generation without a chain cycle.
        uintptr_t appIdentity = 0, nativeIdentity = 0, hookIdentity = 0;
        uint64_t generation = 0;
        HWND outputWindow = nullptr;
        std::atomic<bool> live{false}, ready{false}, unsupported{false};
        std::shared_ptr<QueueLease> creationQueue;
        std::array<std::shared_ptr<QueueLease>, kMaximumCapturedPresentQueues> presentQueues{};
        UINT presentQueueCount = 0;
        uint64_t revision = 0; // queue association; all mutable fields below live under the capture mutex
        unsigned resizeCalls = 0;
        bool resizeAmbiguous = false;
        // Required public aliases are read-only dependencies: never patch or repair a foreign cell.
        void** tables[2]{};
        std::array<void*, 5> methods[2]{};
        unsigned methodMask[2]{};
        unsigned tableCount = 0;
    };
    static std::array<std::shared_ptr<CapturedD3D12Queue>, kCapturedSwapChains> g_capturedQueues{};
    static std::mutex g_capturedQueueMutex;
    static std::atomic<uint64_t> g_captureGeneration{0};
    static uint64_t g_boundGeneration = 0, g_boundRevision = 0; // render lock
    static std::shared_ptr<QueueLease> g_activeQueue; // backs the renderer's borrowed g_queue
    static std::shared_ptr<CapturedD3D12Queue> g_activeCapture;
    // timed: ResizeBuffers waits for a frame in flight on another thread (frame generation) but must never hang the game
    static std::timed_mutex g_renderMutex;
    // Serializes short externally visible commits with terminal disable, not whole
    // frames: never held over preparation, UI building, buffer acquisition or GPU waits.
    // Recursive only for same-thread fault/reentrant disable from a guarded commit.
    static std::recursive_mutex g_participationMutex;
    static std::atomic<int> g_resizeInProgress{0};
    enum class PresentRendererResizeState : uint8_t { Idle, ReleaseForRetry, RebindAfterSuccess };
    static std::atomic<PresentRendererResizeState> g_rendererResizeState{PresentRendererResizeState::Idle};
    static std::atomic<int> g_boundColorSpace{(int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    static std::atomic<HWND> g_gameWindow{nullptr};
    static std::atomic<bool> g_gameWindowFromSwapChain{false};   // g_gameWindow came from a swapchain's own HWND, not the size heuristic
    static thread_local bool g_insidePresent = false;

    static ID3D12CommandQueue* g_queue = nullptr;
    static ID3D12Device* g_device = nullptr;
    void* D3DDevice() { return g_device; }
    void* D3DQueue() { return g_queue; }
    static uintptr_t g_swapChainIdentity = 0;
    static std::atomic<IDXGISwapChain*> g_boundSwapChain{nullptr};   // identity-only (pointer compare), never dereferenced, no reference held
    static std::atomic<HWND> g_boundSwapChainWindow{nullptr};
    static HWND g_hwnd = nullptr;
    static UINT g_bufferCount = 0, g_width = 0, g_height = 0;
    static DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
    static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
    static ID3D12DescriptorHeap* g_srvHeap = nullptr;
    static ID3D12GraphicsCommandList* g_cmdList = nullptr;
    static ID3D12Fence* g_fence = nullptr;
    static HANDLE g_fenceEvent = nullptr;
    static UINT64 g_fenceValue = 0;
    // No reference to a back buffer is kept between frames: DXGI cannot destroy or resize a swapchain while someone holds one,
    // and the game does destroy and recreate its swapchain (DLSS / DLAA switch, display mode). The buffer is fetched, drawn
    // to and released again inside DrawFrame; the RTV descriptor is rewritten each frame (cheap).
    struct Frame { ID3D12CommandAllocator* alloc = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}; UINT64 fence = 0; };
    static std::vector<Frame> g_frames;
    static std::atomic<bool> g_ready{false}, g_failed{false}, g_disabled{false};   // read by every Present thread (frame generation presents from its own)
    static std::atomic<long> g_presents{0};
    static std::atomic<bool> g_wasOpen{false};

    static uintptr_t CanonicalComIdentityToken(IUnknown* incoming);
    static bool SameCanonicalComIdentity(IUnknown* left, IUnknown* right);

    static bool IsReadableRange(const void* address, size_t size) {
        if (!address || size == 0) return false;
        uintptr_t current = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = current + size;
        if (end < current) return false;
        while (current < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if ((mbi.Protect & readable) == 0) return false;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= current) return false;
            current = std::min(end, regionEnd);
        }
        return true;
    }

    static void* ComVtableSlot(void* instance, size_t index) {
        if (!instance || !IsReadableRange(instance, sizeof(void*))) return nullptr;
        void** vtable = *reinterpret_cast<void***>(instance);
        if (!vtable || !IsReadableRange(vtable + index, sizeof(void*))) return nullptr;
        return vtable[index];
    }

    // ---- thumbnail textures (slot 0 of the SRV heap is the ImGui font) ----
    constexpr UINT kSrvSlots = 1024;
    constexpr size_t kMaxThumbs = 700;
    struct Tex { ID3D12Resource* res = nullptr; ID3D12Resource* upload = nullptr; UINT slot = 0; int w = 0, h = 0; DWORD lastUse = 0; bool failed = false; bool uploaded = false; int gen = 0; };
    static std::map<std::string, Tex> g_texs;
    static std::vector<UINT> g_freeSlots;
    static UINT g_nextSlot = 1;
    static std::vector<std::pair<ID3D12Resource*, UINT64>> g_retire;   // resources to release once the fence passes
    static std::vector<std::string> g_pendingUploads;                     // decoded this frame, copy recorded in DrawFrame
    static int g_loadsThisFrame = 0;

    static UINT AllocSlot() { if (!g_freeSlots.empty()) { UINT s = g_freeSlots.back(); g_freeSlots.pop_back(); return s; } return g_nextSlot < kSrvSlots ? g_nextSlot++ : 0; }
    static D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT slot) { auto h = g_srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)slot * g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h; }
    static D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT slot) { auto h = g_srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)slot * g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h; }

    // decode queue: files are decoded on a worker thread; the render thread only picks up finished pixels
    struct Decoded { std::string file; int w = 0, h = 0; unsigned char* px = nullptr; };
    static std::mutex g_decMutex; static std::deque<std::string> g_decQueue; static std::deque<Decoded> g_decDone; static std::set<std::string> g_decInFlight; static HANDLE g_decThread = nullptr;
    static DWORD WINAPI DecodeThread(LPVOID) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::string file;
            { std::lock_guard<std::mutex> l(g_decMutex); if (!g_decQueue.empty()) { file = g_decQueue.front(); g_decQueue.pop_front(); } }
            if (file.empty()) { Sleep(8); continue; }
            Decoded d; d.file = file; int comp = 0; d.px = stbi_load(file.c_str(), &d.w, &d.h, &comp, 4);
            std::lock_guard<std::mutex> l(g_decMutex); g_decDone.push_back(d);
        }
    }
    static void QueueDecode(const std::string& file) {
        std::lock_guard<std::mutex> l(g_decMutex);
        if (g_decInFlight.count(file)) return;
        g_decInFlight.insert(file); g_decQueue.push_back(file);
        if (!g_decThread) g_decThread = CreateThread(nullptr, 0, DecodeThread, nullptr, 0, nullptr);
    }
    // The pixels stay owned by the caller (DrawFrame frees them exactly once); every failure path here releases what it created.
    static bool CreateFromPixels(unsigned char* px, int w, int h, Tex& t) {
        if (!px) return false;
        auto fail = [&t]() { if (t.upload) { t.upload->Release(); t.upload = nullptr; } if (t.res) { t.res->Release(); t.res = nullptr; } t.slot = 0; return false; };
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.res)))) return fail();
        const UINT pitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&t.upload)))) return fail();
        void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
        if (FAILED(t.upload->Map(0, &none, &mapped))) return fail();
        for (int y = 0; y < h; y++) memcpy((uint8_t*)mapped + (size_t)y * pitch, px + (size_t)y * w * 4, (size_t)w * 4);
        t.upload->Unmap(0, nullptr);
        t.slot = AllocSlot(); if (!t.slot) return fail();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        g_device->CreateShaderResourceView(t.res, &sv, CpuHandle(t.slot));
        t.w = w; t.h = h; return true;
    }
    // records the pending copies into the (already reset) command list
    static void RecordUploads() {
        for (auto& file : g_pendingUploads) {
            auto it = g_texs.find(file); if (it == g_texs.end() || !it->second.upload) continue;
            Tex& t = it->second;
            const UINT pitch = (t.w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = t.res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = t.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)t.w, (UINT)t.h, 1, pitch };
            g_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = t.res; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_cmdList->ResourceBarrier(1, &b);
            g_retire.push_back({ t.upload, g_fenceValue + 1 }); t.upload = nullptr; t.uploaded = true;
        }
        g_pendingUploads.clear();
    }
    static void RetireResources() {
        UINT64 done = g_fence ? g_fence->GetCompletedValue() : 0;
        for (size_t i = 0; i < g_retire.size(); ) { if (g_retire[i].second <= done) { g_retire[i].first->Release(); g_retire.erase(g_retire.begin() + i); } else i++; }
    }
    static void EvictIfNeeded() {
        if (g_texs.size() <= kMaxThumbs) return;
        // entries for images that did not exist (yet) hold no slot and were never evicted: after scrolling past ~600 tiles
        // without a preview the loop below could not get under the limit and dropped every real texture in every frame
        for (auto it = g_texs.begin(); it != g_texs.end();) { if (it->second.failed && !it->second.res) it = g_texs.erase(it); else ++it; }
        if (g_texs.size() <= kMaxThumbs) return;
        std::vector<std::pair<DWORD, std::string>> byAge; for (auto& kv : g_texs) if (kv.second.uploaded) byAge.push_back({ kv.second.lastUse, kv.first });
        std::sort(byAge.begin(), byAge.end());
        for (size_t i = 0; i < byAge.size() && g_texs.size() > kMaxThumbs - 100; i++) {
            Tex& t = g_texs[byAge[i].second];
            if (t.res) g_retire.push_back({ t.res, g_fenceValue + 1 });
            g_freeSlots.push_back(t.slot); g_texs.erase(byAge[i].second);
        }
    }
    ImTextureID Thumb(const std::string& file, int* w, int* h) {
        if (!g_ready || !g_device) return 0;
        auto it = g_texs.find(file);
        if (it != g_texs.end() && it->second.failed && it->second.gen != thumbgen::Generation()) { g_texs.erase(it); it = g_texs.end(); }   // the file may exist now
        if (it == g_texs.end()) { QueueDecode(file); return 0; }   // decoded in the background, picked up in BeginFrame
        Tex& t = it->second; t.lastUse = GetTickCount();
        if (t.failed || !t.uploaded) return 0;
        if (w) *w = t.w; if (h) *h = t.h;
        return (ImTextureID)GpuHandle(t.slot).ptr;
    }

    static bool PublishFrameOwnership(bool textInput, bool mouseOverUi);
    static bool SubmitFrame(Frame* frame, IDXGISwapChain3* chain);
    static void NotifyRebindMenu(bool opened);

    static bool CreateRenderTargets(IDXGISwapChain3* sc) {
        DXGI_SWAP_CHAIN_DESC desc = {}; sc->GetDesc(&desc);
        g_bufferCount = desc.BufferCount; g_width = desc.BufferDesc.Width; g_height = desc.BufferDesc.Height; g_format = desc.BufferDesc.Format;
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testRebindFn) return g_testRebindFn(sc, g_bufferCount, g_width, g_height, g_format);
#endif
        if (g_frames.size() != g_bufferCount) {
            for (auto& f : g_frames) { if (f.alloc) f.alloc->Release(); }
            g_frames.clear(); g_frames.resize(g_bufferCount);
            for (auto& f : g_frames) if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
        }
        const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_bufferCount; i++) { g_frames[i].rtv = h; h.ptr += inc; }   // the views are written per frame
        return true;
    }

    // bounded: returns whether our queue really drained (a hung GPU must not hang the game's Present or ResizeBuffers)
    static bool WaitIdle(DWORD timeoutMs = 2000) {
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testDrainFn) return g_testDrainFn();
#endif
        if (!g_fence || !g_queue) return true;
        const UINT64 target = ++g_fenceValue;
        if (FAILED(g_queue->Signal(g_fence, target))) return false;
        if (g_fence->GetCompletedValue() < target) {
            if (!g_fenceEvent || FAILED(g_fence->SetEventOnCompletion(target, g_fenceEvent)) ||
                WaitForSingleObject(g_fenceEvent, timeoutMs) != WAIT_OBJECT_0) return false;
        }
        return g_fence->GetCompletedValue() >= target;
    }

    // text font + system fonts for other scripts + the plugin's icons; again after in-game names brought new characters
    static float g_fontScale = 1.0f;
    static void BuildFonts(bool first) {
        ImGuiIO& io = ImGui::GetIO();
        if (!first) io.Fonts->Clear();
        const char* fonts[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\calibri.ttf" };
        ImFont* textFont = nullptr; const ImWchar* ranges = (const ImWchar*)i18n::GlyphRanges();
        for (const char* fp : fonts) if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) { textFont = io.Fonts->AddFontFromFileTTF(fp, 17.0f * g_fontScale, nullptr, ranges); if (first) core::Log("[overlay] font %s", fp); break; }
        if (!textFont) textFont = io.Fonts->AddFontDefault();
        if (first) core::Log("[overlay] init: system fonts");
        i18n::MergeSystemFonts(io.Fonts, 17.0f * g_fontScale);
        // icons are drawn by the plugin into the atlas (icons.cpp); no icon font is needed
        icons::Register(io.Fonts, textFont, 17.0f * g_fontScale);
        if (first) core::Log("[overlay] init: atlas build");
        io.Fonts->Build();
        icons::Paint(io.Fonts);
        i18n::ReleaseMergedFontData(io.Fonts);
    }
    static bool Init(IDXGISwapChain3* sc) {   // each step is logged: a crash report then shows how far the first frame got
        core::Log("[overlay] init: device");
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { core::Log("[overlay] GetDevice failed"); return false; }
        DXGI_SWAP_CHAIN_DESC desc = {}; sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testFrameSink) {
            if (!CreateRenderTargets(sc)) return false;
            g_swapChainIdentity = CanonicalComIdentityToken(sc);
            g_boundSwapChain.store(sc);
            g_boundSwapChainWindow.store(g_hwnd);
            input::Init(g_hwnd);
            return true;
        }
#endif
        D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 16;
        if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC sh = {}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = kSrvSlots; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap)))) return false;
        core::Log("[overlay] init: heaps + render targets");
        if (!CreateRenderTargets(sc)) { core::Log("[overlay] render targets failed"); return false; }
        core::Log("[overlay] init: command list + fence");
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_cmdList)))) return false;
        g_cmdList->Close();
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        core::Log("[overlay] init: imgui context");
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigErrorRecoveryEnableTooltip = false; io.ConfigErrorRecoveryEnableAssert = false;
        float scale = g_height / 1080.0f; if (scale < 0.75f) scale = 0.75f;
        core::Log("[overlay] init: style");
        editor::ApplyStyle(scale);
        core::Log("[overlay] init: locales");
        i18n::Initialize();
        core::Log("[overlay] init: fonts (language %s)", i18n::Preference());
        g_fontScale = scale; BuildFonts(true);
        core::Log("[overlay] init: backends (atlas %dx%d)", io.Fonts->TexWidth, io.Fonts->TexHeight);
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_Init(g_device, (int)g_bufferCount, g_format, g_srvHeap, g_srvHeap->GetCPUDescriptorHandleForHeapStart(), g_srvHeap->GetGPUDescriptorHandleForHeapStart());
        input::Init(g_hwnd);
        g_swapChainIdentity = CanonicalComIdentityToken(sc);
        g_boundSwapChain.store(sc, std::memory_order_release);
        g_boundSwapChainWindow.store(g_hwnd, std::memory_order_release);
        g_boundColorSpace.store((int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, std::memory_order_release);
        core::Log("[overlay] ready: %ux%u, %u buffers, format %d, hwnd %p", g_width, g_height, g_bufferCount, (int)g_format, (void*)g_hwnd);
        return true;
    }

    static bool RebindRenderer(IDXGISwapChain3* sc, bool restartBackend) {
        if (!sc || !g_device || !g_queue) return false;
        ID3D12Device* dev = nullptr;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
        const bool sameDevice = SameCanonicalComIdentity(dev, g_device);
        dev->Release();
        if (!sameDevice) {
            core::Log("[overlay] replacement swapchain belongs to a different D3D12 device");
            return false;
        }
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testFrameSink) restartBackend = false;
#endif
        if (restartBackend) ImGui_ImplDX12_Shutdown();
        if (!CreateRenderTargets(sc)) return false;
        if (restartBackend && !ImGui_ImplDX12_Init(g_device, (int)g_bufferCount, g_format, g_srvHeap,
                g_srvHeap->GetCPUDescriptorHandleForHeapStart(), g_srvHeap->GetGPUDescriptorHandleForHeapStart())) return false;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow && desc.OutputWindow != g_hwnd) {
            // the game recreated its window: the Win32 backend and the input subclass must follow, otherwise the menu draws
            // but mouse and keys still go to (or are swallowed by) the old window
            core::Log("[overlay] swapchain window changed %p -> %p; re-attaching ImGui Win32 backend and input", (void*)g_hwnd, (void*)desc.OutputWindow);
            NotifyRebindMenu(false);
            input::Shutdown();
            ImGui_ImplWin32_Shutdown();
            g_hwnd = desc.OutputWindow;
            ImGui_ImplWin32_Init(g_hwnd);
            input::Init(g_hwnd);
            NotifyRebindMenu(true);
        }
        g_swapChainIdentity = CanonicalComIdentityToken(sc);
        g_boundSwapChain.store(sc, std::memory_order_release);
        g_boundSwapChainWindow.store(g_hwnd, std::memory_order_release);
        core::Log("[overlay] renderer rebound to swapchain %p (%ux%u, %u buffers, format %d)",
                  (void*)sc, g_width, g_height, g_bufferCount, (int)g_format);
        return true;
    }

    static int g_drawCount = 0;
    static void Stage(const char* s) { if (g_drawCount < 2) core::Log("[overlay] frame %d: %s", g_drawCount, s); }
    static void DrawFrame(IDXGISwapChain3* sc) {
        g_loadsThisFrame = 0; RetireResources(); EvictIfNeeded();
        for (;;) {   // finished decodes -> GPU textures, a few per frame
            Decoded d; { std::lock_guard<std::mutex> l(g_decMutex); if (g_decDone.empty() || g_loadsThisFrame >= 4) break; d = g_decDone.front(); g_decDone.pop_front(); g_decInFlight.erase(d.file); }
            g_loadsThisFrame++;
            Tex t; t.lastUse = GetTickCount(); t.gen = thumbgen::Generation();
            if (!CreateFromPixels(d.px, d.w, d.h, t)) t.failed = true; else g_pendingUploads.push_back(d.file);
            if (d.px) stbi_image_free(d.px);
            if (g_texs.find(d.file) == g_texs.end()) g_texs.emplace(d.file, t); else if (t.res) { g_retire.push_back({ t.res, g_fenceValue + 1 }); if (t.upload) g_retire.push_back({ t.upload, g_fenceValue + 1 }); g_freeSlots.push_back(t.slot); }
        }
        for (const auto& file : thumbgen::TakeRefreshed()) {   // re-rendered image: drop the cached texture so the next Thumb() reloads it
            auto it = g_texs.find(file); if (it == g_texs.end()) continue;
            if (it->second.uploaded && it->second.res) { g_retire.push_back({ it->second.res, g_fenceValue + 1 }); g_freeSlots.push_back(it->second.slot); g_texs.erase(it); }
        }
        if (i18n::TakeGlyphsDirty()) {   // in-game names brought characters the atlas lacks (they showed as "?"): rebuild it once
            WaitIdle(); ImGui_ImplDX12_InvalidateDeviceObjects();
            i18n::RebuildGlyphRanges(); BuildFonts(false);   // NewFrame below recreates the font texture and the pipeline
            core::Log("[overlay] font atlas rebuilt for new characters (%dx%d)", ImGui::GetIO().Fonts->TexWidth, ImGui::GetIO().Fonts->TexHeight);
        }
        Stage("newframe dx12");
        ImGui_ImplDX12_NewFrame();
        Stage("newframe win32");
        ImGui_ImplWin32_NewFrame();
        input::FeedMouse(ImGui::GetIO());
        Stage("imgui newframe");
        ImGui::NewFrame();
        editor::Draw();
        Stage("imgui render");
        ImGui::Render();
        const ImGuiIO& io = ImGui::GetIO();
        if (!PublishFrameOwnership(io.WantTextInput, io.WantCaptureMouse)) return;

        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (idx >= g_frames.size()) return;
        Frame& f = g_frames[idx];
        ID3D12Resource* rt = nullptr;
        if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&rt))) || !rt) { Stage("no back buffer"); return; }
        { D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = g_format; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; g_device->CreateRenderTargetView(rt, &rd, f.rtv); }
        if (f.fence && g_fence->GetCompletedValue() < f.fence) { g_fence->SetEventOnCompletion(f.fence, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 1000); }
        f.alloc->Reset();
        g_cmdList->Reset(f.alloc, nullptr);
        RecordUploads();
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = rt;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cmdList->ResourceBarrier(1, &b);
        g_cmdList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &b);
        if (FAILED(g_cmdList->Close())) { Stage("close failed"); rt->Release(); return; }
        Stage("execute");
        const bool submitted = SubmitFrame(&f, sc);
        rt->Release();   // also release when terminal disable refused the recorded work
        if (submitted) Stage("done");
    }

    // Every path that switches the overlay off hands the input back: with the menu open the subclassed window proc swallows
    // mouse and keys, and with no more frames nothing would ever close the menu again.
    static void DisableOverlay() {
        std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
        const bool first = !g_disabled.exchange(true);
        const bool wasOpen = g_wasOpen.exchange(false);
        core::g_menuOpen = false;
        core::g_uiWantsMouse = false; core::g_uiWantsKeyboard = false;
        core::g_uiTextInput = false; core::g_uiMouseOverUi = false;
        if (first) core::SetFreeCam(false);
        if (wasOpen) input::MenuClosed();
        input::PublishOwnership(input::OwnershipPolicy{});
    }

    // Locks live outside the guarded leaves: an added-work fault cannot strand
    // publication ownership. Disable never waits for g_renderMutex here.
    static bool PublishFrameOwnershipGuarded(bool textInput, bool mouseOverUi) {
        CDK_GUARD_BEGIN
            // Research points may draw with the editor closed; drawing alone never acquires input.
            const bool open = core::g_menuOpen;
            const bool edit = open && editor::IsOpen() && !editor::PlayMode();
            const bool gizmo = open && editor::Placing() && editor::MouseMode();
            textInput = edit && textInput; mouseOverUi = edit && mouseOverUi;
            core::g_uiWantsMouse = edit || gizmo; core::g_uiWantsKeyboard = edit;
            core::g_uiTextInput = textInput; core::g_uiMouseOverUi = mouseOverUi;
            input::OwnershipPolicy policy;
            policy.menuOpen = open; policy.mouseToUi = core::g_uiWantsMouse; policy.keysToUi = core::g_uiWantsKeyboard;
            policy.placing = editor::Placing(); policy.textInput = textInput; policy.mouseOverUi = mouseOverUi;
            policy.play = editor::PlayMode(); input::PublishOwnership(policy);
            return true;
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] ownership_publish_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool PublishFrameOwnership(bool textInput, bool mouseOverUi) {
        std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
        if (g_disabled) return false;
        return PublishFrameOwnershipGuarded(textInput, mouseOverUi);
    }
    static bool SubmitFrameGuarded(Frame* frame, IDXGISwapChain3* chain) {
        CDK_GUARD_BEGIN
#ifdef WB_OVERLAY_BINDING_TEST
            if (g_testFrameSink) { g_testFrameSink(chain, g_queue, g_boundGeneration); return true; }
#endif
            (void)chain;
            ID3D12CommandList* lists[] = { g_cmdList };
            g_queue->ExecuteCommandLists(1, lists);
            g_queue->Signal(g_fence, ++g_fenceValue);
            frame->fence = g_fenceValue;
            ++g_drawCount;
            return true;
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] submit_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool SubmitFrame(Frame* frame, IDXGISwapChain3* chain) {
        std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
        if (g_disabled) return false;
        return SubmitFrameGuarded(frame, chain); // submission only; all GPU waits/recording occur outside this fence
    }
    static void NotifyRebindMenuGuarded(bool opened) {
        CDK_GUARD_BEGIN if (opened) input::MenuOpened(); else input::MenuClosed();
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] rebind_menu_fault=0x%08x", cdk::GuardCode());
        CDK_GUARD_END
    }
    static void NotifyRebindMenu(bool opened) {
        std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
        if (!g_disabled && g_wasOpen) NotifyRebindMenuGuarded(opened);
    }

    static void RenderGuarded(IDXGISwapChain3* sc) {
        CDK_GUARD_BEGIN DrawFrame(sc);
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] exception 0x%08x while drawing; overlay disabled", cdk::GuardCode());
        CDK_GUARD_END
    }

    template <typename T> static void ReleaseCom(T*& value) {
        if (value) { value->Release(); value = nullptr; }
    }

    using SlGetNativeInterfaceFn = int32_t (*)(void*, void**);
    template <typename T> static T* TryUnwrapStreamlineNativeInterface(T* incoming) {
        if (!incoming) return nullptr;
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testNativeInterface) return static_cast<T*>(g_testNativeInterface(incoming));
#endif
        HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
        if (!sl) return nullptr;
        FARPROC exported = GetProcAddress(sl, "slGetNativeInterface");
        SlGetNativeInterfaceFn getNative = nullptr;
        static_assert(sizeof(getNative) == sizeof(exported), "function pointer size mismatch");
        memcpy(&getNative, &exported, sizeof(getNative));
        if (!getNative) return nullptr;
        void* native = nullptr;
        if (getNative(incoming, &native) != 0 || !native) return nullptr;
        return static_cast<T*>(native);
    }

    // Output slots belong to the caller, outside the fault catcher. Even a COM
    // implementation that writes an owned reference and then faults is balanced.
    static HRESULT SafeQuery(IUnknown* object, REFIID iid, void** out) {
        CDK_GUARD_BEGIN return object->QueryInterface(iid, out);
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] query_fault=0x%08x", cdk::GuardCode()); return E_FAIL;
        CDK_GUARD_END
    }
    template<class T> static HRESULT SafeDevice(T* object, ID3D12Device** out) {
        CDK_GUARD_BEGIN return object->GetDevice(IID_PPV_ARGS(out));
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] device_fault=0x%08x", cdk::GuardCode()); return E_FAIL;
        CDK_GUARD_END
    }
    static HRESULT SafeDesc(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC* desc) {
        CDK_GUARD_BEGIN return chain->GetDesc(desc);
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] desc_fault=0x%08x", cdk::GuardCode()); return E_FAIL;
        CDK_GUARD_END
    }
    static uintptr_t CanonicalComIdentityToken(IUnknown* incoming) {
        if (!incoming) return 0;
        ComOwner<IUnknown> identity;
        if (FAILED(SafeQuery(incoming, IID_IUnknown, reinterpret_cast<void**>(&identity.value))) || !identity.value) return 0;
        return reinterpret_cast<uintptr_t>(identity.value);
    }

    static bool SameCanonicalComIdentity(IUnknown* left, IUnknown* right) {
        const uintptr_t a = CanonicalComIdentityToken(left);
        const uintptr_t b = CanonicalComIdentityToken(right);
        return a && a == b;
    }

    struct GameWindowCandidate { HWND hwnd = nullptr; uint64_t area = 0; };
    static bool IsGameWindowCandidate(HWND hwnd, bool requireVisible) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) != nullptr || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
        if (requireVisible && !IsWindowVisible(hwnd)) return false;
        // WS_EX_LAYERED is not rejected: borderless-fullscreen game windows can carry it, and a real game window must pass
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return false;
        // TransmogOverlay is another Crimson Desert mod's in-process overlay window with its own swapchain; it must never be
        // taken for the game window (neither by the size fallback nor as a captured swapchain window)
        wchar_t cls[128] = {};
        if (GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) > 0 &&
            (wcscmp(cls, L"ConsoleWindowClass") == 0 || wcscmp(cls, L"TransmogOverlay") == 0)) return false;
        RECT rc = {};
        return GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top;
    }

    static BOOL CALLBACK FindGameWindowCallback(HWND hwnd, LPARAM param) {
        if (!IsGameWindowCandidate(hwnd, true)) return TRUE;
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        const uint64_t area = static_cast<uint64_t>(rc.right - rc.left) * static_cast<uint64_t>(rc.bottom - rc.top);
        auto* best = reinterpret_cast<GameWindowCandidate*>(param);
        if (area > best->area) { best->hwnd = hwnd; best->area = area; }
        return TRUE;
    }

    static HWND FindGameWindow() {
        GameWindowCandidate best{};
        EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&best));
        return best.hwnd;
    }

    static HWND RefreshGameWindow(HWND hint = nullptr) {
        HWND latest = FindGameWindow();
        if (!latest && IsGameWindowCandidate(hint, false)) latest = hint;
        if (latest) { g_gameWindow.store(latest, std::memory_order_release); g_gameWindowFromSwapChain.store(false, std::memory_order_release); }
        else {
            HWND cached = g_gameWindow.load(std::memory_order_acquire);
            if (!IsGameWindowCandidate(cached, false)) g_gameWindow.store(nullptr, std::memory_order_release);
        }
        return g_gameWindow.load(std::memory_order_acquire);
    }

    // The window a swapchain was created for is the best evidence of the game window; the largest-visible-window heuristic
    // is only the fallback (it can pick a launcher or another mod's window, and then the overlay stayed off silently).
    static bool AcceptPresentWindow(HWND chainWindow, HWND createdFor) {
        if (!chainWindow) return false;
        HWND gameWindow = g_gameWindow.load(std::memory_order_acquire);
        const bool gameValid = gameWindow && IsWindow(gameWindow);
        // a window taken from an earlier swapchain is kept while it is visible, so two swapchains cannot flip it every frame
        if (createdFor == chainWindow && IsGameWindowCandidate(createdFor, false) &&
            (!gameValid || !g_gameWindowFromSwapChain.load(std::memory_order_acquire) ||
             (!IsWindowVisible(gameWindow) && (!g_activeCapture || !g_activeCapture->live.load())))) {
            if (gameWindow != createdFor) core::Log("[overlay] game window %p taken from its swapchain", (void*)createdFor);
            g_gameWindow.store(createdFor, std::memory_order_release);
            g_gameWindowFromSwapChain.store(true, std::memory_order_release);
            return true;
        }
        if (!gameValid) gameWindow = RefreshGameWindow(chainWindow);
        if (gameWindow && chainWindow == gameWindow) return true;
        static std::atomic<HWND> s_lastIgnored{nullptr};
        static std::atomic<unsigned> s_ignoredLogs{0};
        if (s_lastIgnored.exchange(chainWindow, std::memory_order_relaxed) != chainWindow && s_ignoredLogs.fetch_add(1, std::memory_order_relaxed) < 8)
            core::Log("[overlay] swapchain for window %p ignored by the game-window filter (game window %p)", (void*)chainWindow, (void*)gameWindow);
        return false;
    }

    static bool QueueMatchesSwapChainDevice(IDXGISwapChain* chain, ID3D12CommandQueue* queue) {
        if (!chain || !queue) return false;
        ComOwner<ID3D12Device> chainDevice, queueDevice;
        const bool okChain = SUCCEEDED(SafeDevice(chain, &chainDevice.value)) && chainDevice.value;
        const bool okQueue = SUCCEEDED(SafeDevice(queue, &queueDevice.value)) && queueDevice.value;
        return okChain && okQueue && SameCanonicalComIdentity(chainDevice.value, queueDevice.value);
    }

    struct HookRequest {
        HookTarget* state;
        void* target;
        void* detour;
        void** original;
        const char* name;
    };

    enum class HookTargetState { NotInstalled, Matching, Different };
    static HookTargetState GetHookTargetState(const HookTarget& state, const void* target) {
        std::lock_guard<std::mutex> hookLock(g_functionHookMutex);
        if (!state.installed) return HookTargetState::NotInstalled;
        return state.target == target ? HookTargetState::Matching : HookTargetState::Different;
    }

    static std::string ModuleNameForAddress(void* address) {
        if (!address) return "?";
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(address), &module) ||
            !GetModuleFileNameA(module, path, MAX_PATH)) return "?";
        const char* slash = strrchr(path, '\\');
        return slash ? slash + 1 : path;
    }

    static bool PinHookTarget(void* target) {
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testPinTarget) return g_testPinTarget(target);
#endif
        HMODULE pinned = nullptr;
        return target && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCSTR>(target), &pinned) && pinned;
    }
    static bool InstallSingleHook(HookTarget& state, void* target, void* detour, void** original, const char* name) {
        if (!target || target == detour || !PinHookTarget(target)) return false;
        std::lock_guard<std::mutex> hookLock(g_functionHookMutex);
        if (state.installed) {
            if (state.target == target) return true;
            core::Log("[overlay] factory_method_target_mismatch=1; method=%s; existing_module=%s; new_module=%s; alternate_factory_hooked=0",
                      name, ModuleNameForAddress(state.target).c_str(), ModuleNameForAddress(target).c_str());
            return false;
        }
        MH_STATUS create = MH_CreateHook(target, detour, original);
        if (create != MH_OK) {
            core::Log("[overlay] %s hook create failed: %d", name, (int)create);
            return false;
        }
        MH_STATUS enable = MH_EnableHook(target);
        if (enable != MH_OK) {
            MH_RemoveHook(target);
            if (original) *original = nullptr;
            core::Log("[overlay] %s hook enable failed: %d", name, (int)enable);
            return false;
        }
        state.target = target;
        state.installed = true;
        core::Log("[overlay] %s hooked at %p", name, target);
        return true;
    }

    static bool InstallHookGroup(const std::vector<HookRequest>& requests) {
        // Module APIs can re-enter a provider: never call them under the hook lock.
        for (const HookRequest& r : requests) if (!r.target || r.target == r.detour || !PinHookTarget(r.target)) return false;
        std::lock_guard<std::mutex> hookLock(g_functionHookMutex);
        for (const HookRequest& r : requests) {
            if (!r.target) return false;
            if (r.state->installed && r.state->target != r.target) {
                core::Log("[overlay] swap_chain_method_target_mismatch=1; method=%s; existing_module=%s; new_module=%s; alternate_swap_chain_hooked=0",
                          r.name, ModuleNameForAddress(r.state->target).c_str(), ModuleNameForAddress(r.target).c_str());
                return false;
            }
        }

        std::vector<const HookRequest*> created;
        std::vector<const HookRequest*> enabled;
        for (const HookRequest& r : requests) {
            if (r.state->installed) continue;
            MH_STATUS st = MH_CreateHook(r.target, r.detour, r.original);
            if (st != MH_OK) {
                core::Log("[overlay] %s hook create failed: %d", r.name, (int)st);
                for (auto it = created.rbegin(); it != created.rend(); ++it) {
                    MH_RemoveHook((*it)->target);
                    if ((*it)->original) *(*it)->original = nullptr;
                }
                return false;
            }
            created.push_back(&r);
        }
        for (const HookRequest* r : created) {
            MH_STATUS st = MH_EnableHook(r->target);
            if (st != MH_OK) {
                core::Log("[overlay] %s hook enable failed: %d", r->name, (int)st);
                for (const HookRequest* e : enabled) MH_DisableHook(e->target);
                for (auto it = created.rbegin(); it != created.rend(); ++it) {
                    MH_RemoveHook((*it)->target);
                    if ((*it)->original) *(*it)->original = nullptr;
                }
                return false;
            }
            enabled.push_back(r);
        }
        for (const HookRequest* r : created) {
            r->state->target = r->target;
            r->state->installed = true;
            core::Log("[overlay] %s hooked at %p", r->name, r->target);
        }
        return true;
    }

    static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags);
    static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp);
    static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);
    static HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                                      const UINT* nodeMasks, IUnknown* const* presentQueues);
    static HRESULT STDMETHODCALLTYPE hkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE colorSpace);

    static bool InstallRealSwapChainHooks(IDXGISwapChain* chain) {
        if (!chain || !IsReadableRange(chain, sizeof(void*))) return false;
        ComOwner<IDXGISwapChain1> view1;
        ComOwner<IDXGISwapChain3> view3;
        if (FAILED(SafeQuery(chain, IID_PPV_ARGS(&view1.value))) || !view1.value ||
            FAILED(SafeQuery(chain, IID_PPV_ARGS(&view3.value))) || !view3.value) return false;
        IDXGISwapChain1* chain1 = view1.value;
        IDXGISwapChain3* chain3 = view3.value;

        void* presentTarget = ComVtableSlot(chain, 8);
        void* resizeTarget = ComVtableSlot(chain, 13);
        void* present1Target = ComVtableSlot(chain1, 22);
        void* colorSpaceTarget = ComVtableSlot(chain3, 38);
        void* resize1Target = ComVtableSlot(chain3, 39);

        if (!presentTarget || !resizeTarget || !present1Target || !colorSpaceTarget || !resize1Target) return false;
        const HookTargetState presentState = GetHookTargetState(g_presentHookTarget, presentTarget);
        if (presentState == HookTargetState::Different) {
            core::Log("[overlay] swap_chain_present_target_mismatch=1; existing_module=%s; new_module=%s; alternate_swap_chain_hooked=0",
                      ModuleNameForAddress(g_presentHookTarget.target).c_str(), ModuleNameForAddress(presentTarget).c_str());
            return false;
        }
        if (presentState == HookTargetState::Matching) {
            const bool allTargetsMatch =
                GetHookTargetState(g_resizeHookTarget, resizeTarget) == HookTargetState::Matching &&
                (!present1Target || GetHookTargetState(g_present1HookTarget, present1Target) == HookTargetState::Matching) &&
                (!resize1Target || GetHookTargetState(g_resize1HookTarget, resize1Target) == HookTargetState::Matching) &&
                (!colorSpaceTarget || GetHookTargetState(g_colorSpaceHookTarget, colorSpaceTarget) == HookTargetState::Matching);
            if (!allTargetsMatch) core::Log("[overlay] swap_chain_method_target_mismatch=1; alternate_swap_chain_hooked=0");
            return allTargetsMatch;
        }

        std::vector<HookRequest> requests;
        requests.push_back({ &g_presentHookTarget, presentTarget, (void*)&hkPresent, (void**)&oPresent, "Present" });
        requests.push_back({ &g_resizeHookTarget, resizeTarget, (void*)&hkResizeBuffers, (void**)&oResizeBuffers, "ResizeBuffers" });
        if (present1Target) requests.push_back({ &g_present1HookTarget, present1Target, (void*)&hkPresent1, (void**)&oPresent1, "Present1" });
        if (colorSpaceTarget) requests.push_back({ &g_colorSpaceHookTarget, colorSpaceTarget, (void*)&hkSetColorSpace1, (void**)&oSetColorSpace1, "SetColorSpace1" });
        if (resize1Target) requests.push_back({ &g_resize1HookTarget, resize1Target, (void*)&hkResizeBuffers1, (void**)&oResizeBuffers1, "ResizeBuffers1" });

        const bool ok = InstallHookGroup(requests);
        if (ok) {
            uint32_t mask = 0x01u | 0x04u;
            if (present1Target) mask |= 0x02u;
            if (resize1Target) mask |= 0x08u;
            if (colorSpaceTarget) mask |= 0x10u;
            g_swapChainMethodHookMask.store(mask, std::memory_order_release);
            g_presentHookReady.store(1, std::memory_order_release);
        }
        return ok;
    }

    static const GUID kCaptureGuid = {0x7f2b1c94,0x3a6d,0x4be1,{0x9c,0x5a,0x11,0x6e,0xd0,0x44,0x2a,0x87}};
    static constexpr unsigned kChainSlots[] = {8, 13, 22, 38, 39};

    static void RetireCaptured(const std::shared_ptr<CapturedD3D12Queue>& capture) {
        capture->live.store(false, std::memory_order_release);
        std::shared_ptr<CapturedD3D12Queue> displaced;
        {
            std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
            for (auto& slot : g_capturedQueues) if (slot == capture) { displaced = std::move(slot); break; }
        } // the last queue reference must never call foreign Release under the table lock
    }
    class CaptureAttachment final : public IUnknown {
        std::atomic<ULONG> refs{1};
        std::shared_ptr<CapturedD3D12Queue> capture;
    public:
        explicit CaptureAttachment(const std::shared_ptr<CapturedD3D12Queue>& c) : capture(c) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (iid != IID_IUnknown) return E_NOINTERFACE;
            *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1) + 1; }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG n = refs.fetch_sub(1) - 1;
            if (!n) { RetireCaptured(capture); delete this; }
            return n;
        }
    };
    static bool MatchesCapture(const CapturedD3D12Queue& c, uintptr_t id) {
        return id && (c.appIdentity == id || c.nativeIdentity == id || c.hookIdentity == id);
    }
    static std::shared_ptr<CapturedD3D12Queue> LookupCaptured(IDXGISwapChain* chain) {
        const uintptr_t id = CanonicalComIdentityToken(chain);
        std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
        for (const auto& c : g_capturedQueues)
            if (c && c->live.load(std::memory_order_acquire) && c->ready.load(std::memory_order_acquire) && MatchesCapture(*c, id)) return c;
        return {}; // an unrelated chain sharing HWND/device/vtable is never evidence of identity
    }
    static bool CaptureDependenciesReady(const CapturedD3D12Queue& c) {
        for (unsigned t = 0; t < c.tableCount; ++t) {
            for (unsigned i = 0; i < 5; ++i) {
                if (!(c.methodMask[t] & (1u << i))) continue;
                void* target = nullptr;
                if (!core::ReadBytes(reinterpret_cast<uintptr_t>(c.tables[t] + kChainSlots[i]), &target, sizeof target) || target != c.methods[t][i]) return false;
            }
        }
        return true;
    }
    static HRESULT CapturePrivateEntry(IDXGISwapChain* chain, CaptureAttachment* attachment) {
        CDK_GUARD_BEGIN return chain->SetPrivateDataInterface(kCaptureGuid, attachment);
        CDK_GUARD_FAIL core::Log("[overlay] capture_attach_fault=0x%08x", cdk::GuardCode()); return E_FAIL;
        CDK_GUARD_END
    }
    static bool PrivateEntryAbsent(IDXGISwapChain* chain) {
        UINT size = 0; // existence only: a foreign blob is never read, released or replaced
        CDK_GUARD_BEGIN return chain->GetPrivateData(kCaptureGuid, &size, nullptr) == DXGI_ERROR_NOT_FOUND;
        CDK_GUARD_FAIL core::Log("[overlay] capture_entry_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool PrivateEntryPresent(IDXGISwapChain* chain) {
        UINT size = 0;
        CDK_GUARD_BEGIN return chain->GetPrivateData(kCaptureGuid, &size, nullptr) == S_OK;
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] present_entry_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool QueueDescriptionSupported(ID3D12CommandQueue* queue) {
        CDK_GUARD_BEGIN
            const auto desc = queue->GetDesc();
            return desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && desc.NodeMask <= 1;
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] queue_desc_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool SingleNodeDevice(ID3D12Device* device) {
        CDK_GUARD_BEGIN return device->GetNodeCount() == 1;
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] node_count_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static bool DirectQueue(ID3D12CommandQueue* queue) {
        if (!QueueDescriptionSupported(queue)) return false;
        ComOwner<ID3D12Device> device;
        return SUCCEEDED(SafeDevice(queue, &device.value)) && device.value && SingleNodeDevice(device.value);
    }
    static bool CapturePresentSwapChain(IUnknown* device, IDXGISwapChain* appChain, HWND outputWindow) {
        if (!device || !appChain || g_disabled) return false; // fast decline; publication is fenced below too
        DepthScope observer(t_observerDepth);
        ComOwner<ID3D12CommandQueue> queue, nativeQueue;
        ComOwner<IDXGISwapChain> nativeChain;
        ComOwner<IDXGISwapChain3> view;
        if (FAILED(SafeQuery(device, IID_PPV_ARGS(&queue.value))) || !queue.value) return false;
        nativeQueue.value = TryUnwrapStreamlineNativeInterface(queue.value);
        ID3D12CommandQueue* chosenQueue = nativeQueue.value ? nativeQueue.value : queue.value;
        nativeChain.value = TryUnwrapStreamlineNativeInterface(appChain);
        IDXGISwapChain* hookChain = nativeChain.value ? nativeChain.value : appChain;
        if (!DirectQueue(chosenQueue) || !QueueMatchesSwapChainDevice(hookChain, chosenQueue)) return false;
        if (FAILED(SafeQuery(hookChain, IID_PPV_ARGS(&view.value))) || !view.value) return false;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(SafeDesc(appChain, &desc))) return false;
        if (desc.OutputWindow) outputWindow = desc.OutputWindow;
        if (!IsGameWindowCandidate(outputWindow, false) || desc.BufferDesc.Width <= 64 || desc.BufferDesc.Height <= 64 || !desc.BufferCount) return false;
        if (!PrivateEntryAbsent(appChain)) return false;
        const uintptr_t appId = CanonicalComIdentityToken(appChain), hookId = CanonicalComIdentityToken(hookChain);
        if (!appId || !hookId) return false;
        if (!InstallRealSwapChainHooks(hookChain)) return false;
        auto c = std::make_shared<CapturedD3D12Queue>();
        c->appIdentity = appId; c->hookIdentity = hookId; c->nativeIdentity = nativeChain.value ? hookId : 0;
        c->outputWindow = outputWindow;
        c->creationQueue = std::make_shared<QueueLease>(chosenQueue);
        c->tables[0] = *reinterpret_cast<void***>(appChain);
        c->tables[1] = *reinterpret_cast<void***>(view.value);
        c->tableCount = c->tables[0] == c->tables[1] ? 1u : 2u;
        c->methodMask[0] = c->tableCount == 1 ? 31u : 3u; // appChain guarantees only the base prefix
        c->methodMask[1] = 31u; // QueryInterface proved the chain3 prefix
        for (unsigned t = 0; t < c->tableCount; ++t) {
            for (unsigned i = 0; i < 5; ++i) {
                if (!(c->methodMask[t] & (1u << i))) continue;
                if (!core::ReadBytes(reinterpret_cast<uintptr_t>(c->tables[t] + kChainSlots[i]), &c->methods[t][i], sizeof(void*)) || !c->methods[t][i]) return false;
            }
        }
        ComOwner<CaptureAttachment> attachment;
        attachment.value = new CaptureAttachment(c);
        {
            std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
            if (g_disabled) return false;
            std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
            size_t freeSlot = g_capturedQueues.size();
            for (size_t i = 0; i < g_capturedQueues.size(); ++i) {
                if (!g_capturedQueues[i]) freeSlot = i;
                else if (MatchesCapture(*g_capturedQueues[i], appId) || MatchesCapture(*g_capturedQueues[i], hookId)) return false;
            }
            if (freeSlot == g_capturedQueues.size()) return false; // bounded, fail closed: never evict a live association
            c->generation = g_captureGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            c->live.store(true, std::memory_order_release);
            g_capturedQueues[freeSlot] = c; // reserved but not ready: no early Present while attachment is in flight
        }
        if (FAILED(CapturePrivateEntry(appChain, attachment.value))) { RetireCaptured(c); return false; }
        bool published = false;
        {
            std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
            if (!g_disabled) { c->ready.store(true, std::memory_order_release); published = true; }
        }
        if (!published) { RetireCaptured(c); return false; }
        g_boundSwapChainWindow.store(nullptr, std::memory_order_release); // a reused address must obtain its new HWND
        core::Log("[overlay] capture generation=%llu chain=%p hook=%p queue=%p hwnd=%p", c->generation, appChain, hookChain, chosenQueue, outputWindow);
        return true;
    }

    struct CapturedInfo {
        std::shared_ptr<CapturedD3D12Queue> capture;
        std::shared_ptr<QueueLease> queue;
        uint64_t revision = 0;
    };
    static bool CaptureInfo(IDXGISwapChain3* chain, const std::shared_ptr<CapturedD3D12Queue>& c, CapturedInfo& out) {
        const UINT index = chain->GetCurrentBackBufferIndex();
        if (!CaptureDependenciesReady(*c)) { c->unsupported.store(true); return false; }
        std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
        if (!c->live.load() || !c->ready.load() || c->unsupported.load() || c->resizeCalls) return false;
        out.capture = c; out.revision = c->revision;
        out.queue = c->presentQueueCount ? (index < c->presentQueueCount ? c->presentQueues[index] : nullptr) : c->creationQueue;
        return out.queue != nullptr;
    }
    static bool SelectPresentQueue(const CapturedInfo& info) {
        if (!info.queue) return false;
        if (info.queue->queue != g_queue) {
            // One fence spans all queues. Never let a new queue overtake work on the old one.
            if (g_queue && !WaitIdle(500)) { DisableOverlay(); return false; }
            g_activeQueue = info.queue;
            g_queue = info.queue->queue;
        }
        return true;
    }

    static bool PreparePresentBinding(IDXGISwapChain3* sc, CapturedInfo* info) {
        auto& c = info->capture;
        if (g_disabled || g_failed || g_resizeInProgress.load() || !c->live.load() || c->unsupported.load()) return false;
        if (c->generation < g_boundGeneration) return false; // an older live chain cannot steal a newer generation
        if (!AcceptPresentWindow(c->outputWindow, c->outputWindow)) return false;
        if (!CaptureInfo(sc, c, *info)) return false; // recheck association and attachment under the render lock
        if (!g_ready) {
            if (!SelectPresentQueue(*info)) return false;
            if (!Init(sc)) { g_failed = true; DisableOverlay(); return false; }
            g_ready = true;
        } else {
            const bool replaced = CanonicalComIdentityToken(sc) != g_swapChainIdentity || c->generation != g_boundGeneration;
            const auto lifecycle = g_rendererResizeState.load();
            bool retryChanged = false;
            if (lifecycle == PresentRendererResizeState::ReleaseForRetry) {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (FAILED(SafeDesc(sc, &desc))) return false;
                retryChanged = desc.BufferCount != g_bufferCount || desc.BufferDesc.Width != g_width ||
                    desc.BufferDesc.Height != g_height || desc.BufferDesc.Format != g_format;
                if (!retryChanged) g_rendererResizeState.store(PresentRendererResizeState::Idle);
            }
            const bool rebind = replaced || retryChanged || info->revision != g_boundRevision || lifecycle == PresentRendererResizeState::RebindAfterSuccess;
            if (rebind && !WaitIdle(500)) { DisableOverlay(); return false; }
            if (!SelectPresentQueue(*info)) return false; // per-buffer queues are selected on EVERY frame
            if (rebind && !RebindRenderer(sc, true)) { DisableOverlay(); return false; }
        }
        if (!c->live.load() || c->unsupported.load()) return false; // a drain may have allowed retirement
        g_activeCapture = c;
        g_boundGeneration = c->generation; g_boundRevision = info->revision;
        g_rendererResizeState.store(PresentRendererResizeState::Idle);
        return true;
    }
    static bool PreparePresentGuarded(IDXGISwapChain3* sc, CapturedInfo* info) {
        CDK_GUARD_BEGIN return PreparePresentBinding(sc, info);
        CDK_GUARD_FAIL DisableOverlay(); core::Log("[overlay] present_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static void OnPresent(IDXGISwapChain* baseChain) {
        g_frameAttempts.fetch_add(1);
        input::FrameScope inputFrame(std::try_to_lock);
        if (!inputFrame.OwnsLock()) { g_frameSkipped.fetch_add(1); g_frameConsecutiveSkips.fetch_add(1); return; }
        g_frameAdmitted.fetch_add(1); g_frameConsecutiveSkips.store(0); g_presents++;
        // Admission is optional; each input API owns its short router section.
        // Do not keep that section over foreign COM/backend/close dispatches.
        inputFrame.Release();
        if (g_disabled || g_failed || !baseChain || t_observerDepth || g_resizeInProgress.load()) return;
        DepthScope observer(t_observerDepth);
        CapturedInfo info;
        info.capture = LookupCaptured(baseChain);
        if (!info.capture) return;
        if (CanonicalComIdentityToken(baseChain) == info.capture->appIdentity && !PrivateEntryPresent(baseChain)) return;
        ComOwner<IDXGISwapChain3> sc;
        if (FAILED(SafeQuery(baseChain, IID_PPV_ARGS(&sc.value))) || !sc.value) return;
        std::unique_lock<std::timed_mutex> renderLock(g_renderMutex, std::try_to_lock);
        if (!renderLock.owns_lock() || g_resizeInProgress.load() || !info.capture->live.load()) return;
        // v0.95 initializes the context/backend before MenuOpened or any hotkey,
        // including the first closed-menu frame. Unselected chains consume no input.
        if (!PreparePresentGuarded(sc.value, &info)) return;
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testAfterPresentPrepare) g_testAfterPresentPrepare();
#endif
        {
            std::lock_guard<std::recursive_mutex> publication(g_participationMutex);
            if (g_disabled) return;
            static bool toggleDown = false, modeDown = false;
            // Sample both latches before a toggle/camera transition can clear them.
            const bool toggle = input::HotkeyPressed(core::g_keyToggle, toggleDown);
            const bool mode = input::HotkeyPressed(core::g_keyMode, modeDown);
            if (toggle) editor::Toggle();
            if (mode && editor::IsOpen()) {
                if (editor::PlayMode()) editor::TogglePlay(); else editor::ToggleCameraMode();
            }
            const bool open = editor::IsOpen() || editor::Placing();
            const bool changed = open != g_wasOpen.exchange(open);
            core::g_menuOpen = open;
            if (changed) { if (open) input::MenuOpened(); else input::MenuClosed(); }
            if (!open) {
                core::g_uiWantsMouse = false; core::g_uiWantsKeyboard = false;
                core::g_uiTextInput = false; core::g_uiMouseOverUi = false;
                input::PublishOwnership(input::OwnershipPolicy{});
                if (!core::DebugPointCount()) return; // current main draws research points with the editor closed too
            }
        } // no publication lock over UI building, command recording or GPU waits
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testFrameSink) {
            if (PublishFrameOwnership(false, false)) SubmitFrame(nullptr, sc.value);
            return;
        }
#endif
        RenderGuarded(sc.value); // lock and owned references outlive the fault catcher
    }
    struct PresentScope {
        PresentScope() { g_insidePresent = true; }
        ~PresentScope() { g_insidePresent = false; }
    };
    static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
        if (g_insidePresent) return oPresent ? oPresent(sc, sync, flags) : E_FAIL;
        PresentScope scope;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        return oPresent ? oPresent(sc, sync, flags) : E_FAIL;
    }
    static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
        if (g_insidePresent) return oPresent1 ? oPresent1(sc, sync, flags, pp) : E_FAIL;
        PresentScope scope;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        return oPresent1 ? oPresent1(sc, sync, flags, pp) : E_FAIL;
    }

    static thread_local bool t_resizeLockHeld = false;
    struct ResizeTxn {
        std::shared_ptr<CapturedD3D12Queue> capture;
        std::unique_lock<std::timed_mutex> lock{g_renderMutex, std::defer_lock};
        bool entered = false, finished = false, faulted = false, previousLock = false;
        ~ResizeTxn() {
            if (entered && !finished) { DisableOverlay(); Close(); }
            t_resizeLockHeld = previousLock;
        }
        void Close() {
            if (finished || !entered) return;
            if (capture) {
                std::lock_guard<std::mutex> tableLock(g_capturedQueueMutex);
                --capture->resizeCalls;
                if (faulted) capture->unsupported.store(true);
            }
            if (lock.owns_lock()) lock.unlock();
            g_resizeInProgress.fetch_sub(1);
            finished = true;
        }
    };
    static bool DrainResizeGuarded() {
        CDK_GUARD_BEGIN return WaitIdle(500);
        CDK_GUARD_FAIL core::Log("[overlay] resize_drain_fault=0x%08x", cdk::GuardCode()); return false;
        CDK_GUARD_END
    }
    static void BeginResize(ResizeTxn& tx, IDXGISwapChain* chain) {
        tx.previousLock = t_resizeLockHeld;
        tx.capture = LookupCaptured(chain);
        g_resizeInProgress.fetch_add(1); tx.entered = true;
        if (tx.capture) {
            std::lock_guard<std::mutex> tableLock(g_capturedQueueMutex);
            if (tx.capture->resizeCalls++) { tx.capture->resizeAmbiguous = true; tx.capture->unsupported.store(true); }
        }
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testResizePhase) g_testResizePhase(1, tx.capture ? tx.capture->generation : 0);
#endif
        // v0.95: keep the render lock ACROSS the original. No GetBuffer reference
        // or WB command list may race DXGI resize. Reentrant calls use the outer lock.
        if (!t_resizeLockHeld && !tx.lock.try_lock_for(std::chrono::milliseconds(1000))) {
            tx.faulted = true; DisableOverlay(); core::Log("[overlay] resize_lock_timeout=1; resources_kept=1"); return;
        }
        t_resizeLockHeld = true;
        if (!tx.previousLock && !DrainResizeGuarded()) {
            tx.faulted = true; DisableOverlay(); core::Log("[overlay] resize_drain_failed=1; resources_kept=1");
        }
    }
    struct ResizeQueues {
        std::array<ComOwner<ID3D12CommandQueue>, kMaximumCapturedPresentQueues> raw, native;
        std::array<std::shared_ptr<QueueLease>, kMaximumCapturedPresentQueues> verified;
        UINT count = 0;
        bool valid = false, faulted = false;
    };
    static void ReadResizeQueues(ResizeQueues* result, IDXGISwapChain* chain, UINT count, const UINT* nodes, IUnknown* const* queues) {
        if (!queues) return; // plain/null resize clears any override, back to creation queue
        DXGI_SWAP_CHAIN_DESC desc{};
        if (!count && SUCCEEDED(SafeDesc(chain, &desc))) count = desc.BufferCount;
        if (!count || count > kMaximumCapturedPresentQueues) return;
        for (UINT i = 0; i < count; ++i) {
            if (nodes && nodes[i] != 1) return;
            if (!queues[i] || FAILED(SafeQuery(queues[i], IID_PPV_ARGS(&result->raw[i].value))) || !result->raw[i].value) return;
            result->native[i].value = TryUnwrapStreamlineNativeInterface(result->raw[i].value);
            auto* queue = result->native[i].value ? result->native[i].value : result->raw[i].value;
            if (!DirectQueue(queue) || !QueueMatchesSwapChainDevice(chain, queue)) return;
        }
        result->count = count; result->valid = true;
    }
    static void ReadResizeQueuesGuarded(ResizeQueues* result, IDXGISwapChain* chain, UINT count, const UINT* nodes, IUnknown* const* queues) {
        CDK_GUARD_BEGIN ReadResizeQueues(result, chain, count, nodes, queues);
        CDK_GUARD_FAIL result->faulted = true; DisableOverlay(); core::Log("[overlay] resize_validation_fault=0x%08x", cdk::GuardCode());
        CDK_GUARD_END
    }
    static void FinishResize(ResizeTxn& tx, IDXGISwapChain* chain, HRESULT hr, UINT n, const UINT* nodes, IUnknown* const* queues) {
        ResizeQueues result; // caller-owned slots survive guarded work, including output-then-fault QI
        if (SUCCEEDED(hr) && tx.capture && !tx.faulted && !g_disabled) {
            ReadResizeQueuesGuarded(&result, chain, n, nodes, queues);
            tx.faulted = result.faulted || g_disabled.load();
            try {
                if (result.valid && !tx.faulted) for (UINT i = 0; i < result.count; ++i)
                    result.verified[i] = std::make_shared<QueueLease>(result.native[i].value ? result.native[i].value : result.raw[i].value);
            } catch (const std::bad_alloc&) {
                tx.faulted = true; DisableOverlay(); core::Log("[overlay] resize_allocation_failed=1; resources_kept=1");
            }
#ifdef WB_OVERLAY_BINDING_TEST
            if (g_testResizePhase) g_testResizePhase(2, tx.capture->generation);
#endif
            {
                std::lock_guard<std::mutex> tableLock(g_capturedQueueMutex);
                // Commit to the SAME live creation, never a new object at a reused address.
                if (tx.capture->live.load() && !tx.capture->resizeAmbiguous && !tx.faulted) {
                    tx.capture->presentQueues.swap(result.verified); // displaced COM owners die outside the table lock
                    tx.capture->presentQueueCount = result.valid ? result.count : 0;
                    ++tx.capture->revision;
                }
            }
        }
        if (!tx.faulted && tx.capture && tx.capture == g_activeCapture && tx.capture->live.load()) {
            g_rendererResizeState.store(SUCCEEDED(hr) ? PresentRendererResizeState::RebindAfterSuccess :
                (hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_WAS_STILL_DRAWING ? PresentRendererResizeState::ReleaseForRetry : PresentRendererResizeState::Idle));
        }
        core::Log("[overlay] resize_result=0x%08x; generation=%llu; fault=%d", (unsigned)hr, tx.capture ? tx.capture->generation : 0, tx.faulted ? 1 : 0);
        tx.Close();
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testResizePhase) g_testResizePhase(3, tx.capture ? tx.capture->generation : 0);
#endif
    }
    static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        ResizeTxn tx;
        BeginResize(tx, sc);
        const HRESULT hr = oResizeBuffers ? oResizeBuffers(sc, n, w, h, fmt, flags) : E_FAIL;
        FinishResize(tx, sc, hr, 0, nullptr, nullptr);
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                                      const UINT* nodeMasks, IUnknown* const* presentQueues) {
        ResizeTxn tx;
        BeginResize(tx, sc);
        const HRESULT hr = oResizeBuffers1 ? oResizeBuffers1(sc, n, w, h, fmt, flags, nodeMasks, presentQueues) : E_FAIL;
        FinishResize(tx, sc, hr, n, nodeMasks, presentQueues);
        return hr;
    }

    // ImGui keeps drawing plain sRGB values into whatever the back buffer is; tone mapping the menu for an HDR color space
    // is out of scope. The color space does not change the buffers either, so only an actual change triggers a (cheap to
    // skip, costly to repeat) backend rebind, and a game re-setting the same value every frame or after each resize costs nothing.
    static HRESULT STDMETHODCALLTYPE hkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE colorSpace) {
        const HRESULT hr = oSetColorSpace1 ? oSetColorSpace1(sc, colorSpace) : E_FAIL;
        if (sc && SUCCEEDED(hr) && g_boundSwapChain.load(std::memory_order_acquire) == sc &&
            g_boundColorSpace.exchange((int)colorSpace, std::memory_order_acq_rel) != (int)colorSpace)
            g_rendererResizeState.store(PresentRendererResizeState::RebindAfterSuccess, std::memory_order_release);
        return hr;
    }

    static void CaptureAfterCreation(IUnknown* queue, IDXGISwapChain* chain, HWND hwnd) {
        try { CapturePresentSwapChain(queue, chain, hwnd); }
        catch (const std::bad_alloc&) { core::Log("[overlay] capture_allocation_failed=1; delegated=1"); }
    }
    static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
        const bool outer = !t_createDepth && !t_helperDepth && !t_observerDepth;
        DepthScope creation(t_createDepth);
        const HRESULT hr = oCreateSwapChain ? oCreateSwapChain(self, device, desc, pp) : E_FAIL;
        if (outer && SUCCEEDED(hr) && pp && *pp) CaptureAfterCreation(device, *pp, desc ? desc->OutputWindow : nullptr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        const bool outer = !t_createDepth && !t_helperDepth && !t_observerDepth;
        DepthScope creation(t_createDepth);
        const HRESULT hr = oCreateSwapChainForHwnd ? oCreateSwapChainForHwnd(self, device, hwnd, desc, fs, out, pp) : E_FAIL;
        if (outer && SUCCEEDED(hr) && pp && *pp) CaptureAfterCreation(device, *pp, hwnd);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                                     const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* out, IDXGISwapChain1** pp) {
        const bool outer = !t_createDepth && !t_helperDepth && !t_observerDepth;
        DepthScope creation(t_createDepth);
        const HRESULT hr = oCreateSwapChainForCoreWindow ? oCreateSwapChainForCoreWindow(self, device, window, desc, out, pp) : E_FAIL;
        if (outer && SUCCEEDED(hr) && pp && *pp) CaptureAfterCreation(device, *pp, nullptr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                      IDXGIOutput* out, IDXGISwapChain1** pp) {
        const bool outer = !t_createDepth && !t_helperDepth && !t_observerDepth;
        DepthScope creation(t_createDepth);
        const HRESULT hr = oCreateSwapChainForComposition ? oCreateSwapChainForComposition(self, device, desc, out, pp) : E_FAIL;
        if (outer && SUCCEEDED(hr) && pp && *pp) CaptureAfterCreation(device, *pp, nullptr);
        return hr;
    }

    static int HookReturnedFactory(IUnknown* created, const char* provider) {
        if (!created) return 0;
        // Reserve once per vtable. Foreign QI/pin calls run outside the registry lock.
        void* vtable = IsReadableRange(created, sizeof(void*)) ? *reinterpret_cast<void**>(created) : nullptr;
        if (!vtable) return 0;
        {
            std::lock_guard<std::mutex> lock(g_factoryHookMutex);
            if (!g_seenFactoryVtables.insert(vtable).second) return 0;
        }
        DepthScope observer(t_observerDepth);
        ComOwner<IDXGIFactory> base;
        ComOwner<IDXGIFactory2> view;
        SafeQuery(created, IID_PPV_ARGS(&base.value));
        SafeQuery(created, IID_PPV_ARGS(&view.value));
        IDXGIFactory* factory = base.value;
        IDXGIFactory2* factory2 = view.value;
        int installed = 0;
        if (factory) {
            void* target = ComVtableSlot(factory, 10);
            if (target && InstallSingleHook(g_factoryHookTargets[0], target, (void*)&hkCreateSwapChain,
                                        (void**)&oCreateSwapChain, "CreateSwapChain")) installed++;
        }
        if (factory2) {
            void* targetHwnd = ComVtableSlot(factory2, 15);
            void* targetCore = ComVtableSlot(factory2, 16);
            void* targetComposition = ComVtableSlot(factory2, 24);
            if (targetHwnd && InstallSingleHook(g_factoryHookTargets[1], targetHwnd, (void*)&hkCreateSwapChainForHwnd,
                                        (void**)&oCreateSwapChainForHwnd, "CreateSwapChainForHwnd")) installed++;
            if (targetCore && InstallSingleHook(g_factoryHookTargets[2], targetCore, (void*)&hkCreateSwapChainForCoreWindow,
                                        (void**)&oCreateSwapChainForCoreWindow, "CreateSwapChainForCoreWindow")) installed++;
            if (targetComposition && InstallSingleHook(g_factoryHookTargets[3], targetComposition, (void*)&hkCreateSwapChainForComposition,
                                        (void**)&oCreateSwapChainForComposition, "CreateSwapChainForComposition")) installed++;
        }
        uint32_t mask = 0;
        { std::lock_guard<std::mutex> lock(g_functionHookMutex);
          for (int i = 0; i < 4; ++i) if (g_factoryHookTargets[i].installed) mask |= (1u << i); }
        g_factoryMethodHookMask.store(mask, std::memory_order_release);
        if (installed) core::Log("[overlay] %s returned factory captured (%d methods ready, mask=0x%02x)", provider, installed, mask);
        return installed;
    }

    // With Streamline's exports hooked, factories from the real dxgi.dll are left alone (the contributor's tested behaviour);
    // the install-time probe below has already hooked the real factory methods, which the proxy forwards into, so a game
    // creating its swapchain through plain dxgi is captured as well.
    static HRESULT WINAPI hkCreateDXGIFactory(REFIID iid, void** out) {
        const HRESULT hr = oCreateDXGIFactory ? oCreateDXGIFactory(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkCreateDXGIFactory1(REFIID iid, void** out) {
        const HRESULT hr = oCreateDXGIFactory1 ? oCreateDXGIFactory1(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkCreateDXGIFactory2(UINT flags, REFIID iid, void** out) {
        const HRESULT hr = oCreateDXGIFactory2 ? oCreateDXGIFactory2(flags, iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory(REFIID iid, void** out) {
        const HRESULT hr = oStreamlineCreateDXGIFactory ? oStreamlineCreateDXGIFactory(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory1(REFIID iid, void** out) {
        const HRESULT hr = oStreamlineCreateDXGIFactory1 ? oStreamlineCreateDXGIFactory1(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory2(UINT flags, REFIID iid, void** out) {
        const HRESULT hr = oStreamlineCreateDXGIFactory2 ? oStreamlineCreateDXGIFactory2(flags, iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }

    // Candidate observer route, reconciled with the v0.95 owner: a verified game
    // helper supplies only the FINAL chain/queue. It installs no factory/chain cells
    // and creates no second renderer or queue table. Failed resolution leaves the
    // independent real-DXGI capture fully usable.
    static const char kGameBoundaryString[] = "SwapChain::CreateSwapChainForHwnd failed: %d";
    static const char kGameBoundaryHeadSig[] =
        "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B F9 4C 8B 79 40";
    static const char kGameBoundarySiteSig[] =
        "49 8B 9F 40 09 00 00 48 8B 03 4C 8B 60 78 48 8B 4D 78 48 85 C9 74 0D 48 8B 01 C5 F8 77";
    static HookTarget g_gameBoundary;
    using GameSwapChainInit_t = intptr_t (STDMETHODCALLTYPE*)(void*);
    static GameSwapChainInit_t oGameSwapChainInit = nullptr;
    static std::atomic<uint64_t> g_helperCalls{0};
    static intptr_t STDMETHODCALLTYPE GameSwapChainInitDetour(void* self) {
        if (t_helperDepth) return oGameSwapChainInit ? oGameSwapChainInit(self) : E_FAIL;
        DepthScope helper(t_helperDepth);
        void* context = nullptr; IUnknown* queue = nullptr; HWND hwnd = nullptr;
        const uintptr_t base = reinterpret_cast<uintptr_t>(self);
        if (base && core::ReadBytes(base + 0x38, &context, sizeof context) && context)
            core::ReadBytes(reinterpret_cast<uintptr_t>(context) + 0x3e8, &queue, sizeof queue);
        if (base) core::ReadBytes(base + 8, &hwnd, sizeof hwnd);
        ComOwner<ID3D12CommandQueue> queueKeep;
        if (queue && FAILED(SafeQuery(queue, IID_PPV_ARGS(&queueKeep.value)))) queue = nullptr;
        // The game may replace its queue member inside the helper. Keep the input
        // alive through the original, without holding any capture/render mutex.
        const intptr_t hr = oGameSwapChainInit ? oGameSwapChainInit(self) : E_FAIL;
        g_helperCalls.fetch_add(1);
        IDXGISwapChain* chain = nullptr;
        if (base && SUCCEEDED(static_cast<HRESULT>(hr)) && core::ReadBytes(base + 0xa8, &chain, sizeof chain) && chain)
            CaptureAfterCreation(queue ? queueKeep.value : nullptr, chain, hwnd);
        return hr;
    }
    static void InstallGameBoundary() {
        if (g_gameBoundary.installed) return;
        discovery::ImageInfo image{};
        if (!discovery::ImageBounds(core::g_base, &image)) { core::Log("[overlay] game_boundary_unavailable=1"); return; }
        unsigned char hv[64]{}, hm[64]{}, sv[64]{}, sm[64]{};
        const int hn = discovery::GameBoundaryParsePattern(kGameBoundaryHeadSig, hv, hm);
        const int sn = discovery::GameBoundaryParsePattern(kGameBoundarySiteSig, sv, sm);
        int headHits = 0, siteHits = 0;
        bool anchorComplete = false, headComplete = false, siteComplete = false;
        const uintptr_t anchor = discovery::GameBoundaryFuncReferencingString(image, kGameBoundaryString, &anchorComplete);
        const uintptr_t head = discovery::GameBoundaryScan(image, true, hv, hm, hn, &headHits, &headComplete);
        const uintptr_t site = discovery::GameBoundaryScan(image, true, sv, sm, sn, &siteHits, &siteComplete);
        if (!anchorComplete || !headComplete || !siteComplete || headHits != 1 || siteHits != 1 || !head ||
            (anchor && anchor != head) || discovery::GameBoundaryFuncStart(image, site) != head) {
            core::Log("[overlay] game_boundary_resolve_failed=1"); return;
        }
        if (!InstallSingleHook(g_gameBoundary, reinterpret_cast<void*>(image.base + head), reinterpret_cast<void*>(&GameSwapChainInitDetour),
            reinterpret_cast<void**>(&oGameSwapChainInit), "GameSwapChainInit")) core::Log("[overlay] game_boundary_hook_failed=1");
    }

    static HMODULE (WINAPI* g_moduleHandle)(LPCWSTR) = &GetModuleHandleW;
    static FARPROC (WINAPI* g_exportAddress)(HMODULE, LPCSTR) = &GetProcAddress;
    static bool HookExport(HookTarget& state, HMODULE module, const char* symbol, void* detour, void** original) {
        if (!module) return false;
        void* target = reinterpret_cast<void*>(g_exportAddress(module, symbol));
        if (!target || !PinHookTarget(target)) return false;
        std::lock_guard<std::mutex> hookLock(g_functionHookMutex);
        if (state.installed) return state.target == target;
        MH_STATUS create = MH_CreateHook(target, detour, original);
        if (create != MH_OK) return false;
        MH_STATUS enable = MH_EnableHook(target);
        if (enable != MH_OK) {
            MH_RemoveHook(target);
            if (original) *original = nullptr;
            return false;
        }
        state.target = target;
        state.installed = true;
        return true;
    }

    void Install() {
        HMODULE dxgi = g_moduleHandle(L"dxgi.dll");
        if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi) {
            core::Log("[overlay] dxgi.dll unavailable; overlay disabled");
            return;
        }

        int dxgiExports = 0;
        if (HookExport(g_dxgiExportHookTargets[0], dxgi, "CreateDXGIFactory", (void*)&hkCreateDXGIFactory, (void**)&oCreateDXGIFactory)) dxgiExports++;
        if (HookExport(g_dxgiExportHookTargets[1], dxgi, "CreateDXGIFactory1", (void*)&hkCreateDXGIFactory1, (void**)&oCreateDXGIFactory1)) dxgiExports++;
        if (HookExport(g_dxgiExportHookTargets[2], dxgi, "CreateDXGIFactory2", (void*)&hkCreateDXGIFactory2, (void**)&oCreateDXGIFactory2)) dxgiExports++;
        uint32_t dxgiMask = 0;
        for (int i = 0; i < 3; ++i) if (g_dxgiExportHookTargets[i].installed) dxgiMask |= (1u << i);
        g_dxgiExportHookMask.store(dxgiMask, std::memory_order_release);

        int streamlineExports = 0;
        HMODULE sl = g_moduleHandle(L"sl.interposer.dll");
        if (sl) {
            if (HookExport(g_streamlineExportHookTargets[0], sl, "CreateDXGIFactory", (void*)&hkStreamlineCreateDXGIFactory, (void**)&oStreamlineCreateDXGIFactory)) streamlineExports++;
            if (HookExport(g_streamlineExportHookTargets[1], sl, "CreateDXGIFactory1", (void*)&hkStreamlineCreateDXGIFactory1, (void**)&oStreamlineCreateDXGIFactory1)) streamlineExports++;
            if (HookExport(g_streamlineExportHookTargets[2], sl, "CreateDXGIFactory2", (void*)&hkStreamlineCreateDXGIFactory2, (void**)&oStreamlineCreateDXGIFactory2)) streamlineExports++;
        }
        uint32_t streamlineMask = 0;
        for (int i = 0; i < 3; ++i) if (g_streamlineExportHookTargets[i].installed) streamlineMask |= (1u << i);
        g_streamlineExportHookMask.store(streamlineMask, std::memory_order_release);
        g_streamlineFactoryExportsHooked = streamlineMask != 0;

        // The probe goes to the real dxgi.dll only: Streamline forbids any call into sl.interposer before the game's slInit,
        // and this runs on the init thread, possibly before it. It hooks the real factory methods first (the state the
        // contributor's Streamline probe produced before slInit, where it still returned a real factory); the proxy's own
        // methods are then refused as a mismatch, so a chain is captured once, from the call the proxy forwards.
        IDXGIFactory2* probe = nullptr;
        HRESULT hr = E_FAIL;
        if (oCreateDXGIFactory1) hr = oCreateDXGIFactory1(IID_PPV_ARGS(&probe));   // the original: no detour in between
        else hr = CreateDXGIFactory1(IID_PPV_ARGS(&probe));
        g_factoryImportHookMask.store(dxgiMask | (streamlineMask ? 0x08u : 0u), std::memory_order_release);
        g_factoryProbeResult.store(hr, std::memory_order_release);
        if (SUCCEEDED(hr) && probe) {
            HookReturnedFactory(probe, "DXGI");
            probe->Release();
        }

        InstallGameBoundary();

        core::Log("[overlay] World Builder DXGI capture ready: dxgi_export_mask=0x%02x streamline_export_mask=0x%02x factory_import_mask=0x%02x factory_method_mask=0x%02x probe_hr=0x%08x; waiting for game D3D12 swapchain",
                  dxgiMask, streamlineMask, g_factoryImportHookMask.load(std::memory_order_acquire),
                  g_factoryMethodHookMask.load(std::memory_order_acquire), (unsigned)hr);
    }
}
