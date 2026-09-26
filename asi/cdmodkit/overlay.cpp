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
    static bool g_streamlineFactoryExportsHooked = false;

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
    static std::mutex g_swapChainHookMutex;
    static std::mutex g_functionHookMutex;

    constexpr size_t kCapturedSwapChains = 16;
    constexpr size_t kMaximumCapturedPresentQueues = 16;
    struct CapturedD3D12Queue {
        IDXGISwapChain* appSwapChain = nullptr;
        IDXGISwapChain* nativeSwapChain = nullptr;
        IDXGISwapChain* hookSwapChain = nullptr;
        uintptr_t appIdentity = 0;
        uintptr_t nativeIdentity = 0;
        uintptr_t hookIdentity = 0;
        HWND outputWindow = nullptr;
        ID3D12CommandQueue* creationQueue = nullptr;
        std::array<ID3D12CommandQueue*, kMaximumCapturedPresentQueues> presentQueues{};
        UINT presentQueueCount = 0;
        bool presentQueueOverrideObserved = false;
    };
    static std::array<CapturedD3D12Queue, kCapturedSwapChains> g_capturedQueues{};
    static size_t g_nextCapturedQueue = 0;
    static std::mutex g_capturedQueueMutex;
    static std::mutex g_renderMutex;
    static std::atomic<int> g_resizeInProgress{0};
    enum class PresentRendererResizeState : uint8_t { Idle, ReleaseForRetry, RebindAfterSuccess };
    static std::atomic<PresentRendererResizeState> g_rendererResizeState{PresentRendererResizeState::Idle};
    static std::atomic<uint64_t> g_resizeCallbacks{0};
    static std::atomic<uint64_t> g_resizeNonblockingReleases{0};
    static std::atomic<void*> g_colorSpaceHintSwapChain{nullptr};
    static std::atomic<int> g_colorSpaceHint{(int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    static std::atomic<int> g_colorSpaceHintValid{0};
    static std::atomic<HWND> g_gameWindow{nullptr};
    static thread_local bool g_insidePresent = false;

    static ID3D12CommandQueue* g_queue = nullptr;
    static ID3D12Device* g_device = nullptr;
    static uintptr_t g_swapChainIdentity = 0;
    static std::atomic<IDXGISwapChain*> g_boundSwapChain{nullptr};
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
    static bool g_ready = false, g_failed = false, g_disabled = false;
    static std::atomic<long> g_presents{0};
    static bool g_wasOpen = false;

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

    static bool CreateRenderTargets(IDXGISwapChain3* sc) {
        DXGI_SWAP_CHAIN_DESC desc = {}; sc->GetDesc(&desc);
        g_bufferCount = desc.BufferCount; g_width = desc.BufferDesc.Width; g_height = desc.BufferDesc.Height; g_format = desc.BufferDesc.Format;
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

    static bool IsSupportedPresentColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
        return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
               colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
               colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
               colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
    }

    static bool RendererGpuIdleNoWait() {
        if (!g_ready || !g_fence) return true;
        UINT64 pending = 0;
        for (const Frame& frame : g_frames) pending = std::max(pending, frame.fence);
        return !pending || g_fence->GetCompletedValue() >= pending;
    }
    static void WaitIdle() {
        if (!g_fence || !g_queue) return;
        g_queue->Signal(g_fence, ++g_fenceValue);
        if (g_fence->GetCompletedValue() < g_fenceValue) { g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 2000); }
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
        if (restartBackend) ImGui_ImplDX12_Shutdown();
        if (!CreateRenderTargets(sc)) return false;
        if (restartBackend && !ImGui_ImplDX12_Init(g_device, (int)g_bufferCount, g_format, g_srvHeap,
                g_srvHeap->GetCPUDescriptorHandleForHeapStart(), g_srvHeap->GetGPUDescriptorHandleForHeapStart())) return false;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow) g_hwnd = desc.OutputWindow;
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
        // edit mode: every input belongs to the menu (the game keeps running but does not react); play mode / placement: everything to the game
        { const bool edit = editor::IsOpen() && !editor::PlayMode();
          const bool gizmo = editor::Placing() && editor::MouseMode();   // placement is mouse/gizmo driven
          core::g_uiWantsMouse = edit || gizmo; core::g_uiWantsKeyboard = edit;
          const ImGuiIO& io = ImGui::GetIO(); core::g_uiTextInput = io.WantTextInput; core::g_uiMouseOverUi = io.WantCaptureMouse; }

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
        ID3D12CommandList* lists[] = { g_cmdList };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_fence, ++g_fenceValue);
        f.fence = g_fenceValue;
        rt->Release();   // the swapchain keeps the buffer alive while the queue works; our reference must not outlive the frame
        Stage("done");
        g_drawCount++;
    }

    static void RenderGuarded(IDXGISwapChain3* sc) {
        CDK_GUARD_BEGIN DrawFrame(sc);
        CDK_GUARD_FAIL g_disabled = true; core::Log("[overlay] exception 0x%08x while drawing; overlay disabled", cdk::GuardCode());
        CDK_GUARD_END
    }

    template <typename T> static void ReleaseCom(T*& value) {
        if (value) { value->Release(); value = nullptr; }
    }

    using SlGetNativeInterfaceFn = int32_t (*)(void*, void**);
    template <typename T> static T* TryUnwrapStreamlineNativeInterface(T* incoming) {
        if (!incoming) return nullptr;
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

    static uintptr_t CanonicalComIdentityToken(IUnknown* incoming) {
        if (!incoming) return 0;
        IUnknown* identity = nullptr;
        if (FAILED(incoming->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity) return 0;
        const uintptr_t token = reinterpret_cast<uintptr_t>(identity);
        identity->Release();
        return token;
    }

    static bool SameCanonicalComIdentity(IUnknown* left, IUnknown* right) {
        const uintptr_t a = CanonicalComIdentityToken(left);
        const uintptr_t b = CanonicalComIdentityToken(right);
        return a && a == b;
    }

    static HWND GetSwapChainWindow(IDXGISwapChain* chain) {
        if (!chain) return nullptr;
        if (g_boundSwapChain.load(std::memory_order_acquire) == chain) {
            HWND cached = g_boundSwapChainWindow.load(std::memory_order_acquire);
            if (cached) return cached;
        }
        IDXGISwapChain1* chain1 = nullptr;
        HWND hwnd = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))) && chain1) {
            if (FAILED(chain1->GetHwnd(&hwnd))) hwnd = nullptr;
            chain1->Release();
        }
        if (!hwnd) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(chain->GetDesc(&desc))) hwnd = desc.OutputWindow;
        }
        if (g_boundSwapChain.load(std::memory_order_acquire) == chain)
            g_boundSwapChainWindow.store(hwnd, std::memory_order_release);
        return hwnd;
    }

    struct GameWindowCandidate { HWND hwnd = nullptr; uint64_t area = 0; };
    static bool IsGameWindowCandidate(HWND hwnd, bool requireVisible) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) != nullptr) return false;
        if (requireVisible && !IsWindowVisible(hwnd)) return false;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)) return false;
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
        if (latest) g_gameWindow.store(latest, std::memory_order_release);
        else {
            HWND cached = g_gameWindow.load(std::memory_order_acquire);
            if (!IsGameWindowCandidate(cached, false)) g_gameWindow.store(nullptr, std::memory_order_release);
        }
        return g_gameWindow.load(std::memory_order_acquire);
    }

    static bool QueueMatchesSwapChainDevice(IDXGISwapChain* chain, ID3D12CommandQueue* queue) {
        if (!chain || !queue) return false;
        ID3D12Device* chainDevice = nullptr;
        ID3D12Device* queueDevice = nullptr;
        const bool okChain = SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&chainDevice))) && chainDevice;
        const bool okQueue = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice;
        const bool same = okChain && okQueue && SameCanonicalComIdentity(chainDevice, queueDevice);
        ReleaseCom(queueDevice);
        ReleaseCom(chainDevice);
        return same;
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

    static bool InstallSingleHook(HookTarget& state, void* target, void* detour, void** original, const char* name) {
        if (!target) return false;
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
        std::lock_guard<std::mutex> lock(g_swapChainHookMutex);

        IDXGISwapChain1* chain1 = nullptr;
        IDXGISwapChain3* chain3 = nullptr;
        chain->QueryInterface(IID_PPV_ARGS(&chain1));
        chain->QueryInterface(IID_PPV_ARGS(&chain3));

        void* presentTarget = ComVtableSlot(chain, 8);
        void* resizeTarget = ComVtableSlot(chain, 13);
        void* present1Target = ComVtableSlot(chain1, 22);
        void* colorSpaceTarget = ComVtableSlot(chain3, 38);
        void* resize1Target = ComVtableSlot(chain3, 39);

        const HookTargetState presentState = GetHookTargetState(g_presentHookTarget, presentTarget);
        if (presentState == HookTargetState::Different) {
            core::Log("[overlay] swap_chain_present_target_mismatch=1; existing_module=%s; new_module=%s; alternate_swap_chain_hooked=0",
                      ModuleNameForAddress(g_presentHookTarget.target).c_str(), ModuleNameForAddress(presentTarget).c_str());
            ReleaseCom(chain3);
            ReleaseCom(chain1);
            return false;
        }
        if (presentState == HookTargetState::Matching) {
            const bool allTargetsMatch =
                GetHookTargetState(g_resizeHookTarget, resizeTarget) == HookTargetState::Matching &&
                (!present1Target || GetHookTargetState(g_present1HookTarget, present1Target) == HookTargetState::Matching) &&
                (!resize1Target || GetHookTargetState(g_resize1HookTarget, resize1Target) == HookTargetState::Matching) &&
                (!colorSpaceTarget || GetHookTargetState(g_colorSpaceHookTarget, colorSpaceTarget) == HookTargetState::Matching);
            ReleaseCom(chain3);
            ReleaseCom(chain1);
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
        ReleaseCom(chain3);
        ReleaseCom(chain1);
        return ok;
    }

    static ID3D12CommandQueue* AcquireCapturedQueue(const CapturedD3D12Queue& captured, IDXGISwapChain* chain) {
        ID3D12CommandQueue* queue = captured.creationQueue;
        if (captured.presentQueueOverrideObserved) {
            IDXGISwapChain3* chain3 = nullptr;
            if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) || !chain3) return nullptr;
            const UINT index = chain3->GetCurrentBackBufferIndex();
            chain3->Release();
            if (index >= captured.presentQueueCount || index >= captured.presentQueues.size()) return nullptr;
            queue = captured.presentQueues[index];
        }
        if (!queue || !QueueMatchesSwapChainDevice(chain, queue)) return nullptr;
        queue->AddRef();
        return queue;
    }

    static ID3D12CommandQueue* FindCapturedPresentQueue(IDXGISwapChain* chain) {
        if (!chain) return nullptr;
        const uintptr_t identity = CanonicalComIdentityToken(chain);
        if (!identity) return nullptr;
        std::unique_lock<std::mutex> lock(g_capturedQueueMutex, std::try_to_lock);
        if (!lock.owns_lock()) return nullptr;

        for (const CapturedD3D12Queue& captured : g_capturedQueues) {
            if (captured.appIdentity == identity || captured.nativeIdentity == identity || captured.hookIdentity == identity)
                return AcquireCapturedQueue(captured, chain);
        }

        const HWND hwnd = GetSwapChainWindow(chain);
        ID3D12CommandQueue* unique = nullptr;
        for (const CapturedD3D12Queue& captured : g_capturedQueues) {
            if (!hwnd || captured.outputWindow != hwnd) continue;
            ID3D12CommandQueue* candidate = AcquireCapturedQueue(captured, chain);
            if (!candidate) continue;
            if (unique) {
                candidate->Release();
                unique->Release();
                return nullptr;
            }
            unique = candidate;
        }
        return unique;
    }

    static void ReleaseCaptured(CapturedD3D12Queue& captured) {
        ReleaseCom(captured.creationQueue);
        for (ID3D12CommandQueue*& queue : captured.presentQueues) ReleaseCom(queue);
        captured = {};
    }

    static bool CapturePresentSwapChain(IUnknown* device, IDXGISwapChain* appChain, HWND outputWindow) {
        if (!device || !appChain) return false;

        ID3D12CommandQueue* queue = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return false;
        ID3D12CommandQueue* nativeQueue = TryUnwrapStreamlineNativeInterface(queue);
        const bool nativeQueueAlias = nativeQueue != nullptr;
        if (nativeQueue) {
            queue->Release();
            queue = nativeQueue;
        }
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            queue->Release();
            return false;
        }

        IDXGISwapChain* nativeChain = TryUnwrapStreamlineNativeInterface(appChain);
        IDXGISwapChain* hookChain = nativeChain ? nativeChain : appChain;
        const bool nativeChainAlias = nativeChain && nativeChain != appChain;
        if (!QueueMatchesSwapChainDevice(hookChain, queue)) {
            core::Log("[overlay] captured swapchain and DIRECT queue belong to different D3D12 devices; ignored");
            queue->Release();
            ReleaseCom(nativeChain);
            return false;
        }
        if (!InstallRealSwapChainHooks(hookChain)) {
            queue->Release();
            ReleaseCom(nativeChain);
            return false;
        }

        const uintptr_t appIdentity = CanonicalComIdentityToken(appChain);
        const uintptr_t nativeIdentity = CanonicalComIdentityToken(nativeChain);
        const uintptr_t hookIdentity = CanonicalComIdentityToken(hookChain);

        {
            std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
            CapturedD3D12Queue* destination = nullptr;
            for (CapturedD3D12Queue& captured : g_capturedQueues) {
                if ((appIdentity && captured.appIdentity == appIdentity) ||
                    (nativeIdentity && captured.nativeIdentity == nativeIdentity) ||
                    (hookIdentity && captured.hookIdentity == hookIdentity)) {
                    destination = &captured;
                    break;
                }
                if (!destination && !captured.appSwapChain && !captured.nativeSwapChain && !captured.hookSwapChain)
                    destination = &captured;
            }
            if (!destination) destination = &g_capturedQueues[g_nextCapturedQueue++ % g_capturedQueues.size()];
            ReleaseCaptured(*destination);
            destination->appSwapChain = appChain;
            destination->nativeSwapChain = nativeChain;
            destination->hookSwapChain = hookChain;
            destination->appIdentity = appIdentity;
            destination->nativeIdentity = nativeIdentity;
            destination->hookIdentity = hookIdentity;
            destination->outputWindow = outputWindow;
            destination->creationQueue = queue;
        }

        core::Log("[overlay] captured game D3D12 swapchain (native chain=%d, native queue=%d, hwnd=%p)",
                  nativeChainAlias ? 1 : 0, nativeQueueAlias ? 1 : 0, (void*)outputWindow);
        ReleaseCom(nativeChain);
        return true;
    }

    static void CaptureResizePresentQueues(IDXGISwapChain3* chain, UINT count, IUnknown* const* presentQueues) {
        if (!chain || !presentQueues || !count) return;

        std::array<ID3D12CommandQueue*, kMaximumCapturedPresentQueues> verified{};
        bool valid = count <= verified.size();
        for (UINT i = 0; valid && i < count; ++i) {
            ID3D12CommandQueue* queue = nullptr;
            if (!presentQueues[i] || FAILED(presentQueues[i]->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
                valid = false;
                break;
            }
            ID3D12CommandQueue* native = TryUnwrapStreamlineNativeInterface(queue);
            if (native) {
                queue->Release();
                queue = native;
            }
            if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || !QueueMatchesSwapChainDevice(chain, queue)) {
                queue->Release();
                valid = false;
                break;
            }
            verified[i] = queue;
        }
        if (!valid) for (ID3D12CommandQueue*& queue : verified) ReleaseCom(queue);

        const uintptr_t identity = CanonicalComIdentityToken(chain);
        bool matched = false;
        {
            std::lock_guard<std::mutex> lock(g_capturedQueueMutex);
            for (CapturedD3D12Queue& captured : g_capturedQueues) {
                if (captured.appIdentity != identity && captured.nativeIdentity != identity && captured.hookIdentity != identity) continue;
                matched = true;
                for (ID3D12CommandQueue*& queue : captured.presentQueues) ReleaseCom(queue);
                captured.presentQueueCount = 0;
                captured.presentQueueOverrideObserved = true;
                if (valid) {
                    captured.presentQueueCount = count;
                    for (UINT i = 0; i < count; ++i) {
                        captured.presentQueues[i] = verified[i];
                        verified[i] = nullptr;
                    }
                }
                break;
            }
        }
        for (ID3D12CommandQueue*& queue : verified) ReleaseCom(queue);
        if (matched) {
            core::Log("[overlay] ResizeBuffers1 presentation queues %s (%u)", valid ? "validated" : "rejected", valid ? count : 0);
        }
    }

    static bool SelectPresentQueue(IDXGISwapChain* chain) {
        ID3D12CommandQueue* queue = FindCapturedPresentQueue(chain);
        if (!queue) {
            static std::atomic<unsigned> misses{0};
            const unsigned n = misses.fetch_add(1, std::memory_order_relaxed);
            if (n < 3) core::Log("[overlay] no validated D3D12 presentation queue for this swapchain; frame skipped");
            return false;
        }
        if (queue != g_queue) {
            if (g_ready && g_queue && !RendererGpuIdleNoWait()) {
                queue->Release();
                return false;
            }
            ReleaseCom(g_queue);
            g_queue = queue;
            core::Log("[overlay] presentation queue selected %p", (void*)g_queue);
        } else {
            queue->Release();
        }
        return true;
    }

    static void OnPresent(IDXGISwapChain* baseChain) {
        g_presents++;
        if (g_disabled || g_failed || !baseChain || g_resizeInProgress.load(std::memory_order_acquire) != 0) return;

        const HWND chainWindow = GetSwapChainWindow(baseChain);
        HWND gameWindow = g_gameWindow.load(std::memory_order_acquire);
        if (!gameWindow || !IsWindow(gameWindow))
            gameWindow = RefreshGameWindow(chainWindow);
        if (!gameWindow || chainWindow != gameWindow) return;

        std::unique_lock<std::mutex> renderLock(g_renderMutex, std::try_to_lock);
        if (!renderLock.owns_lock()) return;

        IDXGISwapChain3* sc = nullptr;
        if (FAILED(baseChain->QueryInterface(IID_PPV_ARGS(&sc))) || !sc) return;

        if (!g_ready) {
            if (!SelectPresentQueue(baseChain)) { sc->Release(); return; }
            if (Init(sc)) g_ready = true;
            else {
                g_failed = true;
                core::Log("[overlay] init failed; overlay disabled");
                sc->Release();
                return;
            }
        } else {
            const uintptr_t identity = CanonicalComIdentityToken(sc);
            const bool replaced = identity != g_swapChainIdentity;
            PresentRendererResizeState lifecycle = g_rendererResizeState.load(std::memory_order_acquire);
            if (lifecycle == PresentRendererResizeState::ReleaseForRetry) {
                sc->Release();
                return;
            }
            const bool rebind = replaced || lifecycle == PresentRendererResizeState::RebindAfterSuccess;
            if (rebind && !RendererGpuIdleNoWait()) {
                sc->Release();
                return;
            }
            if (rebind && !SelectPresentQueue(baseChain)) {
                sc->Release();
                return;
            }
            if (rebind && !RebindRenderer(sc, true)) {
                g_disabled = true;
                core::Log("[overlay] renderer rebind after swapchain change failed; overlay disabled");
                sc->Release();
                return;
            }
            if (rebind) {
                g_rendererResizeState.store(PresentRendererResizeState::Idle, std::memory_order_release);
                g_resizeNonblockingReleases.fetch_add(1, std::memory_order_relaxed);
            }
        }

        static bool s_insDown = false;
        bool down = (GetAsyncKeyState(core::g_keyToggle) & 0x8000) != 0;
        if (down && !s_insDown) editor::Toggle();
        s_insDown = down;

        static bool s_homeDown = false;
        bool home = (GetAsyncKeyState(core::g_keyMode) & 0x8000) != 0;
        if (home && !s_homeDown && editor::IsOpen()) {
            if (editor::PlayMode()) editor::TogglePlay();
            else editor::ToggleCameraMode();
        }
        s_homeDown = home;

        bool open = editor::IsOpen() || editor::Placing();
        if (open != g_wasOpen) {
            if (open) input::MenuOpened(); else input::MenuClosed();
            g_wasOpen = open;
        }
        core::g_menuOpen = open;
        if (!open) {
            core::g_uiWantsMouse = false;
            core::g_uiWantsKeyboard = false;
            core::g_uiTextInput = false;
            core::g_uiMouseOverUi = false;
            sc->Release();
            return;
        }

        RenderGuarded(sc);
        sc->Release();
    }

    static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
        const Present_t original = oPresent;
        if (g_insidePresent) return original ? original(sc, sync, flags) : E_FAIL;
        g_insidePresent = true;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        const HRESULT hr = original ? original(sc, sync, flags) : E_FAIL;
        g_insidePresent = false;
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
        const Present1_t original = oPresent1;
        if (g_insidePresent) return original ? original(sc, sync, flags, pp) : E_FAIL;
        g_insidePresent = true;
        if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
        const HRESULT hr = original ? original(sc, sync, flags, pp) : E_FAIL;
        g_insidePresent = false;
        return hr;
    }

    static bool BeginResize(IDXGISwapChain* chain) {
        if (!chain || g_presentHookReady.load(std::memory_order_acquire) == 0) return false;
        const HWND gameWindow = g_gameWindow.load(std::memory_order_acquire);
        if (!gameWindow || GetSwapChainWindow(chain) != gameWindow) return false;
        g_resizeInProgress.fetch_add(1, std::memory_order_acq_rel);
        g_resizeCallbacks.fetch_add(1, std::memory_order_relaxed);
        g_rendererResizeState.store(PresentRendererResizeState::ReleaseForRetry, std::memory_order_release);
        return true;
    }

    static void FinishResize(bool tracked) {
        if (tracked) g_resizeInProgress.fetch_sub(1, std::memory_order_acq_rel);
    }

    static bool IsRetryableResizeFailure(HRESULT hr) {
        return hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_WAS_STILL_DRAWING;
    }

    static bool TryReleaseRendererBeforeResize(IDXGISwapChain* chain) {
        std::unique_lock<std::mutex> lock(g_renderMutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        if (!g_ready || CanonicalComIdentityToken(chain) != g_swapChainIdentity) return true;
        // WB deliberately holds no persistent swap-chain backbuffer references.
        // Match CrimsonRoute's nonblocking contract: never wait in Resize; the
        // renderer is retired/rebound by the following Present once its fence is idle.
        return RendererGpuIdleNoWait();
    }

    static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        const bool tracked = BeginResize(sc);
        if (tracked) (void)TryReleaseRendererBeforeResize(sc);
        const HRESULT hr = oResizeBuffers ? oResizeBuffers(sc, n, w, h, fmt, flags) : E_FAIL;
        if (tracked) {
            g_rendererResizeState.store(
                SUCCEEDED(hr) ? PresentRendererResizeState::RebindAfterSuccess :
                (IsRetryableResizeFailure(hr) ? PresentRendererResizeState::ReleaseForRetry : PresentRendererResizeState::Idle),
                std::memory_order_release);
            core::Log("[overlay] ResizeBuffers result=0x%08x; state=%s", (unsigned)hr,
                SUCCEEDED(hr) ? "rebind_after_success" : (IsRetryableResizeFailure(hr) ? "release_for_retry" : "idle"));
        }
        FinishResize(tracked);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                                      const UINT* nodeMasks, IUnknown* const* presentQueues) {
        const bool tracked = BeginResize(sc);
        if (tracked) (void)TryReleaseRendererBeforeResize(sc);
        const HRESULT hr = oResizeBuffers1 ? oResizeBuffers1(sc, n, w, h, fmt, flags, nodeMasks, presentQueues) : E_FAIL;
        if (SUCCEEDED(hr) && presentQueues && n) CaptureResizePresentQueues(sc, n, presentQueues);
        if (tracked) {
            g_rendererResizeState.store(
                SUCCEEDED(hr) ? PresentRendererResizeState::RebindAfterSuccess :
                (IsRetryableResizeFailure(hr) ? PresentRendererResizeState::ReleaseForRetry : PresentRendererResizeState::Idle),
                std::memory_order_release);
            core::Log("[overlay] ResizeBuffers1 result=0x%08x; state=%s", (unsigned)hr,
                SUCCEEDED(hr) ? "rebind_after_success" : (IsRetryableResizeFailure(hr) ? "release_for_retry" : "idle"));
        }
        FinishResize(tracked);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE colorSpace) {
        const HRESULT hr = oSetColorSpace1 ? oSetColorSpace1(sc, colorSpace) : E_FAIL;
        if (sc) {
            if (SUCCEEDED(hr) && IsSupportedPresentColorSpace(colorSpace)) {
                g_colorSpaceHintSwapChain.store(sc, std::memory_order_release);
                g_colorSpaceHint.store((int)colorSpace, std::memory_order_release);
                g_colorSpaceHintValid.store(1, std::memory_order_release);
            } else if (FAILED(hr)) {
                g_colorSpaceHintValid.store(
                    g_colorSpaceHintSwapChain.load(std::memory_order_acquire) == sc ? 1 : 0,
                    std::memory_order_release);
            } else {
                g_colorSpaceHintValid.store(0, std::memory_order_release);
            }
            if (SUCCEEDED(hr) && g_boundSwapChain.load(std::memory_order_acquire) == sc)
                g_rendererResizeState.store(PresentRendererResizeState::RebindAfterSuccess, std::memory_order_release);
        }
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
        const HRESULT hr = oCreateSwapChain ? oCreateSwapChain(self, device, desc, pp) : E_FAIL;
        if (SUCCEEDED(hr) && pp && *pp) CapturePresentSwapChain(device, *pp, desc ? desc->OutputWindow : nullptr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        const HRESULT hr = oCreateSwapChainForHwnd ? oCreateSwapChainForHwnd(self, device, hwnd, desc, fs, out, pp) : E_FAIL;
        if (SUCCEEDED(hr) && pp && *pp) CapturePresentSwapChain(device, *pp, hwnd);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                                     const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* out, IDXGISwapChain1** pp) {
        const HRESULT hr = oCreateSwapChainForCoreWindow ? oCreateSwapChainForCoreWindow(self, device, window, desc, out, pp) : E_FAIL;
        if (SUCCEEDED(hr) && pp && *pp) CapturePresentSwapChain(device, *pp, nullptr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                      IDXGIOutput* out, IDXGISwapChain1** pp) {
        const HRESULT hr = oCreateSwapChainForComposition ? oCreateSwapChainForComposition(self, device, desc, out, pp) : E_FAIL;
        if (SUCCEEDED(hr) && pp && *pp) CapturePresentSwapChain(device, *pp, nullptr);
        return hr;
    }

    static int HookReturnedFactory(IUnknown* created, const char* provider) {
        if (!created) return 0;
        std::lock_guard<std::mutex> lock(g_factoryHookMutex);
        IDXGIFactory* factory = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        created->QueryInterface(IID_PPV_ARGS(&factory));
        created->QueryInterface(IID_PPV_ARGS(&factory2));
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
        ReleaseCom(factory2);
        ReleaseCom(factory);
        uint32_t mask = 0;
        for (int i = 0; i < 4; ++i) if (g_factoryHookTargets[i].installed) mask |= (1u << i);
        g_factoryMethodHookMask.store(mask, std::memory_order_release);
        if (installed) core::Log("[overlay] %s returned factory captured (%d methods ready, mask=0x%02x)", provider, installed, mask);
        return installed;
    }

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

    static bool HookExport(HookTarget& state, HMODULE module, const char* symbol, void* detour, void** original) {
        if (!module) return false;
        void* target = reinterpret_cast<void*>(GetProcAddress(module, symbol));
        if (!target) return false;
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
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
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
        HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
        if (sl) {
            if (HookExport(g_streamlineExportHookTargets[0], sl, "CreateDXGIFactory", (void*)&hkStreamlineCreateDXGIFactory, (void**)&oStreamlineCreateDXGIFactory)) streamlineExports++;
            if (HookExport(g_streamlineExportHookTargets[1], sl, "CreateDXGIFactory1", (void*)&hkStreamlineCreateDXGIFactory1, (void**)&oStreamlineCreateDXGIFactory1)) streamlineExports++;
            if (HookExport(g_streamlineExportHookTargets[2], sl, "CreateDXGIFactory2", (void*)&hkStreamlineCreateDXGIFactory2, (void**)&oStreamlineCreateDXGIFactory2)) streamlineExports++;
        }
        uint32_t streamlineMask = 0;
        for (int i = 0; i < 3; ++i) if (g_streamlineExportHookTargets[i].installed) streamlineMask |= (1u << i);
        g_streamlineExportHookMask.store(streamlineMask, std::memory_order_release);
        g_streamlineFactoryExportsHooked = streamlineMask != 0;

        IDXGIFactory2* probe = nullptr;
        HRESULT hr = E_FAIL;
        const char* provider = g_streamlineFactoryExportsHooked ? "Streamline" : "DXGI";
        if (g_streamlineFactoryExportsHooked) {
            if (oStreamlineCreateDXGIFactory1) hr = oStreamlineCreateDXGIFactory1(IID_PPV_ARGS(&probe));
            else if (oStreamlineCreateDXGIFactory2) hr = oStreamlineCreateDXGIFactory2(0, IID_PPV_ARGS(&probe));
            else if (oStreamlineCreateDXGIFactory) hr = oStreamlineCreateDXGIFactory(IID_PPV_ARGS(&probe));
        } else {
            hr = CreateDXGIFactory1(IID_PPV_ARGS(&probe));
        }
        g_factoryImportHookMask.store(dxgiMask | (streamlineMask ? 0x08u : 0u), std::memory_order_release);
        g_factoryProbeResult.store(hr, std::memory_order_release);
        if (SUCCEEDED(hr) && probe) {
            HookReturnedFactory(probe, provider);
            probe->Release();
        }

        core::Log("[overlay] CrimsonRoute DXGI capture ready: dxgi_export_mask=0x%02x streamline_export_mask=0x%02x factory_import_mask=0x%02x factory_method_mask=0x%02x probe_hr=0x%08x; waiting for game D3D12 swapchain",
                  dxgiMask, streamlineMask, g_factoryImportHookMask.load(std::memory_order_acquire),
                  g_factoryMethodHookMask.load(std::memory_order_acquire), (unsigned)hr);
    }
}
