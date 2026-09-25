// D3D12 overlay: detour the DXGI factory swapchain creation methods (learned from a dummy factory of the same class),
// read Present/ResizeBuffers from the game's own swapchain when one of them is created, pin its command queue,
// and draw Dear ImGui into the back buffer on Present. Approach follows master-looter (MIT): no throwaway device.
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
#include <atomic>
#include <algorithm>

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
    typedef HRESULT (WINAPI* Present_t)(IDXGISwapChain3*, UINT, UINT);
    typedef HRESULT (WINAPI* Present1_t)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT (WINAPI* ResizeBuffers_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef HRESULT (WINAPI* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

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
    static void* g_factoryTargets[4] = {};
    static bool g_streamlineFactoryExportsHooked = false;

    static bool g_targetsHooked = false;
    static ID3D12CommandQueue* g_queue = nullptr;
    static ID3D12Device* g_device = nullptr;
    static IDXGISwapChain3* g_swapChain = nullptr;
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

    static void ReleaseRenderTargets() {}   // nothing is held between frames (see Frame)
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
        i18n::Initialize(core::ModDir());
        core::Log("[overlay] init: fonts (language %s)", i18n::Preference());
        g_fontScale = scale; BuildFonts(true);
        core::Log("[overlay] init: backends (atlas %dx%d)", io.Fonts->TexWidth, io.Fonts->TexHeight);
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_Init(g_device, (int)g_bufferCount, g_format, g_srvHeap, g_srvHeap->GetCPUDescriptorHandleForHeapStart(), g_srvHeap->GetGPUDescriptorHandleForHeapStart());
        input::Init(g_hwnd);
        g_swapChain = sc;
        core::Log("[overlay] ready: %ux%u, %u buffers, format %d, hwnd %p", g_width, g_height, g_bufferCount, (int)g_format, (void*)g_hwnd);
        return true;
    }

    static int g_drawCount = 0;
    static void Stage(const char* s) { if (g_drawCount < 2) core::Log("[overlay] frame %d: %s", g_drawCount, s); }
    static void DrawFrame(IDXGISwapChain3* sc) {
        if (sc != g_swapChain) {   // the game replaced its swapchain: rebind
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) { if (dev != g_device) { core::Log("[overlay] swapchain belongs to a different device; overlay disabled"); dev->Release(); g_disabled = true; return; } dev->Release(); }
            WaitIdle(); ReleaseRenderTargets();
            if (!CreateRenderTargets(sc)) { g_disabled = true; core::Log("[overlay] rebind failed; overlay disabled"); return; }
            g_swapChain = sc;
            core::Log("[overlay] rebound to new swapchain %p (%ux%u)", (void*)sc, g_width, g_height);
        }
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

    static void OnPresent(IDXGISwapChain3* sc) {
        g_presents++;
        if (g_disabled || g_failed || !g_queue) return;
        if (!g_ready) { if (Init(sc)) g_ready = true; else { g_failed = true; core::Log("[overlay] init failed; overlay disabled"); return; } }
        // toggle key handled here so it works without the console: Insert
        static bool s_insDown = false;
        bool down = (GetAsyncKeyState(core::g_keyToggle) & 0x8000) != 0;
        if (down && !s_insDown) editor::Toggle();
        s_insDown = down;
        static bool s_homeDown = false;   // Home enters/exits free camera; placement's existing play state still returns to edit first
        bool home = (GetAsyncKeyState(core::g_keyMode) & 0x8000) != 0;
        if (home && !s_homeDown && editor::IsOpen()) {
            if (editor::PlayMode()) editor::TogglePlay();
            else editor::ToggleCameraMode();
        }
        s_homeDown = home;
        bool open = editor::IsOpen() || editor::Placing();
        if (open != g_wasOpen) { if (open) input::MenuOpened(); else input::MenuClosed(); g_wasOpen = open; }
        core::g_menuOpen = open;
        if (!open) { core::g_uiWantsMouse = false; core::g_uiWantsKeyboard = false; core::g_uiTextInput = false; core::g_uiMouseOverUi = false; return; }
        RenderGuarded(sc);
    }

    static HRESULT WINAPI hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) { OnPresent(sc); return oPresent(sc, sync, flags); }
    static HRESULT WINAPI hkPresent1(IDXGISwapChain3* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) { OnPresent(sc); return oPresent1(sc, sync, flags, pp); }
    static HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        if (g_ready && sc == g_swapChain) WaitIdle();   // our last frame must be off the GPU; no buffer reference is held
        HRESULT hr = oResizeBuffers(sc, n, w, h, fmt, flags);
        if (g_ready && sc == g_swapChain) {
            if (FAILED(hr)) core::Log("[overlay] the game's ResizeBuffers failed: 0x%08x", (unsigned)hr);
            else if (!CreateRenderTargets(sc)) { g_disabled = true; core::Log("[overlay] rebuild after resize failed; overlay disabled"); }
            else core::Log("[overlay] resized to %ux%u", g_width, g_height);
        }
        return hr;
    }

    static HRESULT WINAPI hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                           const UINT* nodeMasks, IUnknown* const* presentQueues) {
        if (g_ready && sc == g_swapChain) WaitIdle();
        HRESULT hr = oResizeBuffers1(sc, n, w, h, fmt, flags, nodeMasks, presentQueues);
        if (g_ready && sc == g_swapChain) {
            if (FAILED(hr)) core::Log("[overlay] the game's ResizeBuffers1 failed: 0x%08x", (unsigned)hr);
            else if (!CreateRenderTargets(sc)) { g_disabled = true; core::Log("[overlay] rebuild after ResizeBuffers1 failed; overlay disabled"); }
            else core::Log("[overlay] ResizeBuffers1 to %ux%u", g_width, g_height);
        }
        return hr;
    }

    static bool QueueMatchesSwapChainDevice(IDXGISwapChain1* chain, ID3D12CommandQueue* q) {
        if (!chain || !q) return false;
        ID3D12Device* scDev = nullptr;
        ID3D12Device* qDev = nullptr;
        IUnknown* scId = nullptr;
        IUnknown* qId = nullptr;
        const bool haveSc = SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&scDev))) && scDev;
        const bool haveQ = SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qDev))) && qDev;
        if (haveSc) scDev->QueryInterface(IID_PPV_ARGS(&scId));
        if (haveQ) qDev->QueryInterface(IID_PPV_ARGS(&qId));
        const bool same = scId && qId && scId == qId;
        if (qId) qId->Release();
        if (scId) scId->Release();
        if (qDev) qDev->Release();
        if (scDev) scDev->Release();
        return same;
    }

    static bool PinQueue(IDXGISwapChain1* chain, IUnknown* queueUnk) {
        ID3D12CommandQueue* q = nullptr;
        if (queueUnk && SUCCEEDED(queueUnk->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                core::Log("[overlay] swapchain queue is not DIRECT; ignored");
                q->Release();
                return false;
            }
            if (!QueueMatchesSwapChainDevice(chain, q)) {
                core::Log("[overlay] swapchain and queue belong to different D3D12 devices; ignored");
                q->Release();
                return false;
            }
            if (q != g_queue) { if (g_queue) g_queue->Release(); g_queue = q; core::Log("[overlay] present queue pinned %p", (void*)q); }
            else q->Release();
            return true;
        }
        core::Log("[overlay] swapchain device is not a D3D12 command queue; ignored");
        return false;
    }
    static void HookFrom(IDXGISwapChain1* chain, IUnknown* queueUnk) {
        if (!chain) return;
        if (!PinQueue(chain, queueUnk)) return;   // ignore helper/overlay swapchains that are not backed by the game's D3D12 DIRECT queue
        // Every real creation re-pins: the game replaces its chain at startup and can do so again after display-mode changes.
        if (g_targetsHooked) return;
        void** vt = *reinterpret_cast<void***>(chain);
        void* present = vt[8]; void* resize = vt[13]; void* present1 = vt[22];
        if (MH_CreateHook(present, (void*)&hkPresent, (void**)&oPresent) == MH_OK && MH_EnableHook(present) == MH_OK) {
            g_targetsHooked = true;
            core::Log("[overlay] Present hooked at %p", present);
        } else {
            core::Log("[overlay] Present hook failed; a later swapchain may retry");
            return;
        }
        if (MH_CreateHook(present1, (void*)&hkPresent1, (void**)&oPresent1) == MH_OK && MH_EnableHook(present1) == MH_OK) core::Log("[overlay] Present1 hooked");
        if (MH_CreateHook(resize, (void*)&hkResizeBuffers, (void**)&oResizeBuffers) == MH_OK && MH_EnableHook(resize) == MH_OK) core::Log("[overlay] ResizeBuffers hooked");
        IDXGISwapChain3* chain3 = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3) {
            void* resize1 = (*reinterpret_cast<void***>(chain3))[39];
            if (MH_CreateHook(resize1, (void*)&hkResizeBuffers1, (void**)&oResizeBuffers1) == MH_OK && MH_EnableHook(resize1) == MH_OK)
                core::Log("[overlay] ResizeBuffers1 hooked");
            chain3->Release();
        }
    }

    static void ConsiderSwapChain(const char* api, IUnknown* chainUnk, IUnknown* device) {
        if (!chainUnk) return;
        IDXGISwapChain1* chain = nullptr;
        if (FAILED(chainUnk->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) {
            core::Log("[overlay] %s returned an object without IDXGISwapChain1; ignored", api);
            return;
        }
        DXGI_SWAP_CHAIN_DESC actual = {};
        if (SUCCEEDED(chain->GetDesc(&actual)))
            core::Log("[overlay] %s created swapchain %ux%u fmt %d buffers %u hwnd %p", api, actual.BufferDesc.Width, actual.BufferDesc.Height, (int)actual.BufferDesc.Format, actual.BufferCount, (void*)actual.OutputWindow);
        else
            core::Log("[overlay] %s created swapchain %p", api, (void*)chain);
        HookFrom(chain, device);
        chain->Release();
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
        HRESULT hr = oCreateSwapChain(self, device, desc, pp);
        if (SUCCEEDED(hr) && pp && *pp) ConsiderSwapChain("CreateSwapChain", *pp, device);
        else if (FAILED(hr)) core::Log("[overlay] CreateSwapChain failed: 0x%08x", (unsigned)hr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        HRESULT hr = oCreateSwapChainForHwnd(self, device, hwnd, desc, fs, out, pp);
        if (SUCCEEDED(hr) && pp && *pp) ConsiderSwapChain("CreateSwapChainForHwnd", *pp, device);
        else if (FAILED(hr)) core::Log("[overlay] the game's CreateSwapChainForHwnd failed: 0x%08x (a swapchain that is still referenced cannot be replaced)", (unsigned)hr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                    IDXGIOutput* out, IDXGISwapChain1** pp) {
        HRESULT hr = oCreateSwapChainForCoreWindow(self, device, window, desc, out, pp);
        if (SUCCEEDED(hr) && pp && *pp) ConsiderSwapChain("CreateSwapChainForCoreWindow", *pp, device);
        else if (FAILED(hr)) core::Log("[overlay] CreateSwapChainForCoreWindow failed: 0x%08x", (unsigned)hr);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                     IDXGIOutput* out, IDXGISwapChain1** pp) {
        HRESULT hr = oCreateSwapChainForComposition(self, device, desc, out, pp);
        if (SUCCEEDED(hr) && pp && *pp) ConsiderSwapChain("CreateSwapChainForComposition", *pp, device);
        else if (FAILED(hr)) core::Log("[overlay] CreateSwapChainForComposition failed: 0x%08x", (unsigned)hr);
        return hr;
    }

    // Follow CrimsonRoute's capture strategy: hook the factory exports the game
    // actually calls, then hook CreateSwapChain* on the factory object returned to
    // the game. This avoids assuming that a factory created by us has the same
    // implementation/vtable as a Streamline or other interposed game factory.
    static int HookReturnedFactory(IUnknown* created, const char* provider) {
        if (!created) return 0;
        IDXGIFactory* factory = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        created->QueryInterface(IID_PPV_ARGS(&factory));
        created->QueryInterface(IID_PPV_ARGS(&factory2));
        int installed = 0;
        auto hook = [&](int which, void* target, void* detour, void** original, const char* name) {
            if (!target) return;
            if (g_factoryTargets[which]) {
                if (g_factoryTargets[which] != target)
                    core::Log("[overlay] %s factory target differs from the already captured provider; ignored", name);
                return;
            }
            MH_STATUS create = MH_CreateHook(target, detour, original);
            MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
            if (create == MH_OK && enable == MH_OK) {
                g_factoryTargets[which] = target;
                installed++;
                core::Log("[overlay] %s %s hooked at %p", provider, name, target);
            } else {
                core::Log("[overlay] could not hook %s %s (create=%d enable=%d)", provider, name, (int)create, (int)enable);
            }
        };
        if (factory) {
            void** vt = *reinterpret_cast<void***>(factory);
            hook(0, vt[10], (void*)&hkCreateSwapChain, (void**)&oCreateSwapChain, "CreateSwapChain");
        }
        if (factory2) {
            void** vt = *reinterpret_cast<void***>(factory2);
            hook(1, vt[15], (void*)&hkCreateSwapChainForHwnd, (void**)&oCreateSwapChainForHwnd, "CreateSwapChainForHwnd");
            hook(2, vt[16], (void*)&hkCreateSwapChainForCoreWindow, (void**)&oCreateSwapChainForCoreWindow, "CreateSwapChainForCoreWindow");
            hook(3, vt[24], (void*)&hkCreateSwapChainForComposition, (void**)&oCreateSwapChainForComposition, "CreateSwapChainForComposition");
        }
        if (factory2) factory2->Release();
        if (factory) factory->Release();
        return installed;
    }

    static HRESULT WINAPI hkCreateDXGIFactory(REFIID iid, void** out) {
        HRESULT hr = oCreateDXGIFactory ? oCreateDXGIFactory(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkCreateDXGIFactory1(REFIID iid, void** out) {
        HRESULT hr = oCreateDXGIFactory1 ? oCreateDXGIFactory1(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkCreateDXGIFactory2(UINT flags, REFIID iid, void** out) {
        HRESULT hr = oCreateDXGIFactory2 ? oCreateDXGIFactory2(flags, iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out && !g_streamlineFactoryExportsHooked)
            HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "DXGI");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory(REFIID iid, void** out) {
        HRESULT hr = oStreamlineCreateDXGIFactory ? oStreamlineCreateDXGIFactory(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory1(REFIID iid, void** out) {
        HRESULT hr = oStreamlineCreateDXGIFactory1 ? oStreamlineCreateDXGIFactory1(iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }
    static HRESULT WINAPI hkStreamlineCreateDXGIFactory2(UINT flags, REFIID iid, void** out) {
        HRESULT hr = oStreamlineCreateDXGIFactory2 ? oStreamlineCreateDXGIFactory2(flags, iid, out) : E_FAIL;
        if (SUCCEEDED(hr) && out && *out) HookReturnedFactory(reinterpret_cast<IUnknown*>(*out), "Streamline");
        return hr;
    }

    void Install() {
        auto hookExport = [](HMODULE module, const char* symbol, void* detour, void** original) {
            if (!module) return false;
            void* target = reinterpret_cast<void*>(GetProcAddress(module, symbol));
            if (!target) return false;
            MH_STATUS create = MH_CreateHook(target, detour, original);
            MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
            return create == MH_OK && enable == MH_OK;
        };

        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
        int dxgiExports = 0;
        if (hookExport(dxgi, "CreateDXGIFactory", (void*)&hkCreateDXGIFactory, (void**)&oCreateDXGIFactory)) dxgiExports++;
        if (hookExport(dxgi, "CreateDXGIFactory1", (void*)&hkCreateDXGIFactory1, (void**)&oCreateDXGIFactory1)) dxgiExports++;
        if (hookExport(dxgi, "CreateDXGIFactory2", (void*)&hkCreateDXGIFactory2, (void**)&oCreateDXGIFactory2)) dxgiExports++;

        int streamlineExports = 0;
        if (HMODULE sl = GetModuleHandleW(L"sl.interposer.dll")) {
            if (hookExport(sl, "CreateDXGIFactory", (void*)&hkStreamlineCreateDXGIFactory, (void**)&oStreamlineCreateDXGIFactory)) streamlineExports++;
            if (hookExport(sl, "CreateDXGIFactory1", (void*)&hkStreamlineCreateDXGIFactory1, (void**)&oStreamlineCreateDXGIFactory1)) streamlineExports++;
            if (hookExport(sl, "CreateDXGIFactory2", (void*)&hkStreamlineCreateDXGIFactory2, (void**)&oStreamlineCreateDXGIFactory2)) streamlineExports++;
        }
        g_streamlineFactoryExportsHooked = streamlineExports != 0;

        // Same-provider probe as CrimsonRoute: install method hooks now as a
        // fallback if the game created its factory before our ASI initialized.
        IDXGIFactory2* probe = nullptr;
        HRESULT hr = E_FAIL;
        const char* provider = g_streamlineFactoryExportsHooked ? "Streamline" : "DXGI";
        if (g_streamlineFactoryExportsHooked) {
            if (oStreamlineCreateDXGIFactory1) hr = oStreamlineCreateDXGIFactory1(IID_PPV_ARGS(&probe));
            else if (oStreamlineCreateDXGIFactory2) hr = oStreamlineCreateDXGIFactory2(0, IID_PPV_ARGS(&probe));
            else if (oStreamlineCreateDXGIFactory) hr = oStreamlineCreateDXGIFactory(IID_PPV_ARGS(&probe));
        } else {
            if (oCreateDXGIFactory1) hr = oCreateDXGIFactory1(IID_PPV_ARGS(&probe));
            else if (oCreateDXGIFactory2) hr = oCreateDXGIFactory2(0, IID_PPV_ARGS(&probe));
            else if (oCreateDXGIFactory) hr = oCreateDXGIFactory(IID_PPV_ARGS(&probe));
        }
        if (SUCCEEDED(hr) && probe) { HookReturnedFactory(probe, provider); probe->Release(); }
        core::Log("[overlay] factory interception installed: DXGI %d/3, Streamline %d/3; waiting for the game's D3D12 swapchain", dxgiExports, streamlineExports);
    }
}
