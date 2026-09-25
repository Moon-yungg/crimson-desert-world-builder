// D3D12 overlay: detour the factory's CreateSwapChainForHwnd (learned from a dummy factory of the same class),
// read Present/ResizeBuffers from the game's own swapchain when it is created, pin its command queue,
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
    typedef HRESULT (STDMETHODCALLTYPE* FactoryQi_t)(IUnknown*, REFIID, void**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    typedef HRESULT (WINAPI* FactoryCreateFn)(REFIID, void**);                 // CreateDXGIFactory / CreateDXGIFactory1
    typedef HRESULT (WINAPI* FactoryCreate2Fn)(UINT, REFIID, void**);          // CreateDXGIFactory2
    typedef FARPROC (WINAPI* GetProcAddressFn)(HMODULE, LPCSTR);

    static FactoryCreateSwapChainForHwnd_t oCreateSwapChainForHwnd = nullptr;
    static Present_t oPresent = nullptr;
    static Present1_t oPresent1 = nullptr;
    static ResizeBuffers_t oResizeBuffers = nullptr;

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
    static bool g_forceRebind = false;                     // generation adoption / resize: rebind even when the pointer is equal
    static std::atomic<bool> g_overlayStop{false};         // sticky: a guarded-work failure, no future WB frame is submitted

    static bool Init(IDXGISwapChain3* sc);                 // the renderer below
    static bool CreateRenderTargets(IDXGISwapChain3* sc);
    static bool EnsureRenderBinding(IDXGISwapChain3* sc);   // rebind policy (defined with DrawFrame)
    static void RenderGuarded(IDXGISwapChain3* sc);
    static void OverlayFrame(IDXGISwapChain3* sc);
    static bool RendererReady(IDXGISwapChain3* sc);   // renderer entry points (defined with the binding)
    static void RendererDraw(IDXGISwapChain3* sc);
#ifdef WB_OVERLAY_BINDING_TEST
    static void (*g_testFrameSink)(IDXGISwapChain3*, ID3D12CommandQueue*, int) = nullptr;   // host seam: the GPU boundary only
    static bool (*g_testDrainFn)() = nullptr;                                              // host seam: the drain result, not its policy
    static bool (*g_testRebindFn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT) = nullptr;   // host seam: allocator rebuild only
#endif

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
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testRebindFn) return g_testRebindFn(sc, g_bufferCount, g_width, g_height, g_format);   // host seam: the GPU allocator work only
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
    static bool WaitIdle() {                              // checked: a timeout keeps in-flight resources and disables drawing
        if (!g_fence || !g_queue) return true;
        g_queue->Signal(g_fence, ++g_fenceValue);
        if (g_fence->GetCompletedValue() < g_fenceValue) {
            g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
            if (WaitForSingleObject(g_fenceEvent, 2000) != WAIT_OBJECT_0) return false;
        }
        return true;
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
        if (!EnsureRenderBinding(sc)) return;   // device check, drain, allocator rebuild (generation-aware)
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
          const bool gizmo = editor::Placing() && editor::MouseMode();   // Numpad 5 while placing: the mouse drives the gizmo instead of the camera
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

    // The renderer entry point the binding machinery calls (upstream's input/editor handling, our readiness gate).
    static void OverlayFrame(IDXGISwapChain3* sc) {
        g_presents++;
        if (!RendererReady(sc)) return;
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
        RendererDraw(sc);
    }


    // =====================================================================================================
    // WB_BINDING_FUNCTIONAL
    // =====================================================================================================
    static const char kMarker[] = "WB_BINDING_FUNCTIONAL";
    static const int kMaxFnRecs = 16;        // declared fail-open capacities, never an unbounded code allocator
    static const int kMaxFactoryVt = 64;
    static const int kMaxChainVt = 64;
    static const DWORD kDrainTimeoutMs = 2000;

    // A wrapper may hand out an interface whose IID we cannot recognize; recognizing more of them only widens the
    // supported prefix, it never changes what is touched. Values from the public DXGI headers (dxgi1_4.h has 1..4).
    static const GUID kIidFactory5 = { 0x7632E1F5, 0xEE65, 0x4DCA, { 0x87, 0xFD, 0x84, 0xCD, 0x75, 0xF8, 0x83, 0x8D } };
    static const GUID kIidFactory6 = { 0xC1B6694F, 0xFF09, 0x44A9, { 0xB0, 0x3C, 0x77, 0x90, 0x0A, 0x0A, 0x1D, 0x17 } };
    static const GUID kIidFactory7 = { 0xA4966EED, 0x76DB, 0x44DA, { 0x84, 0xC1, 0xEE, 0x9A, 0x7A, 0xFB, 0x20, 0xA8 } };
    // Canonical IDXGISwapChain4 IID {3D585D5A-BD4A-489E-B1F4-3DBCB6452FFB} (Windows SDK dxgi1_4.h; the value the game
    // itself QIs the created swapchain with). The previous tail {B1F4 3BCE 6FC5 CC4D} could only ever return E_NOINTERFACE.
    static const GUID kIidSwapChain4 = { 0x3D585D5A, 0xBD4A, 0x489E, { 0xB1, 0xF4, 0x3D, 0xBC, 0xB6, 0x45, 0x2F, 0xFB } };
    static const GUID kWbCookieGuid = { 0x7F2B1C94, 0x3A6D, 0x4BE1, { 0x9C, 0x5A, 0x11, 0x6E, 0xD0, 0x44, 0x2A, 0x87 } };   // binding metadata
    static const GUID kWbCookieIid  = { 0x2A63D3B8, 0x9D21, 0x44C7, { 0x83, 0x3E, 0x6B, 0xF1, 0x2F, 0x55, 0x0C, 0x19 } };   // validates the retrieved cookie

    // ---- OS seam: only protection / module lifetime / the main image base are substitutable (host tests) ----
    struct BindingOs {
        BOOL (WINAPI* virtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);
        HMODULE (*moduleFromAddress)(const void*);
        bool (*moduleFileName)(HMODULE, char*, size_t);
        HMODULE (*pinModule)(HMODULE);
        uintptr_t (*mainImageBase)();
    };
    static BOOL WINAPI OsVirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) { return VirtualProtect(p, n, prot, old); }
    static HMODULE OsModuleFromAddress(const void* addr) {
        HMODULE mod = nullptr;
        if (!addr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &mod)) return nullptr;
        return mod;
    }
    static bool OsModuleFileName(HMODULE mod, char* out, size_t cap) {   // lowercase basename; the identity that matters for "dxgi.dll"
        if (!mod || !out || cap < 2) return false;
        char full[MAX_PATH] = {};
        if (!GetModuleFileNameA(mod, full, MAX_PATH)) return false;
        size_t last = 0; for (size_t i = 0; full[i]; i++) if (full[i] == '\\' || full[i] == '/') last = i + 1;
        size_t n = 0; for (; full[last + n] && n + 1 < cap; n++) { char c = full[last + n]; out[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
        out[n] = 0; return n != 0;
    }
    static HMODULE OsPinModule(HMODULE mod) {
        HMODULE m = mod;
        if (!mod) return nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  reinterpret_cast<LPCSTR>(mod), &m) ? mod : nullptr;
    }
    static uintptr_t OsMainImageBase() { return core::g_base; }
    static BindingOs g_os = { &OsVirtualProtect, &OsModuleFromAddress, &OsModuleFileName, &OsPinModule, &OsMainImageBase };
    static HMODULE g_mainModule = nullptr;
    static bool PinTarget(void* addr);                   // exact-target module pin (defined with the install helpers)

    // ---- guarded reads / small helpers ------------------------------------------------------------------
    static bool SafeRead(const void* p, void* out, size_t n) { return p && out && core::ReadBytes((uintptr_t)p, out, n); }
    static bool StrEq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }
    static bool StrEqI(const char* a, const char* b) {   // case-insensitive exact comparison (module identities)
        if (!a || !b) return false;
        for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a'); if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a'); if (x != y) return false; }
        return *a == 0 && *b == 0;
    }
    static bool StartsWithI(const char* s, const char* prefix) {   // case-insensitive prefix comparison (documented API-set families)
        if (!s || !prefix) return false;
        for (; *prefix; s++, prefix++) {
            if (!*s) return false;
            char x = *s, y = *prefix;
            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    }
    static void CopyToken(char* out, size_t cap, const char* src) {
        if (!out || !cap) return;
        size_t i = 0;
        if (src) for (; src[i] && i + 1 < cap; i++) out[i] = src[i];
        out[i] = 0;
    }
    static const char* IidName(REFIID iid) {
        if (iid == __uuidof(IDXGIFactory)) return "IDXGIFactory";
        if (iid == __uuidof(IDXGIFactory1)) return "IDXGIFactory1";
        if (iid == __uuidof(IDXGIFactory2)) return "IDXGIFactory2";
        if (iid == __uuidof(IDXGIFactory3)) return "IDXGIFactory3";
        if (iid == __uuidof(IDXGIFactory4)) return "IDXGIFactory4";
        if (iid == kIidFactory5) return "IDXGIFactory5";
        if (iid == kIidFactory6) return "IDXGIFactory6";
        if (iid == kIidFactory7) return "IDXGIFactory7";
        if (iid == __uuidof(IDXGISwapChain)) return "IDXGISwapChain";
        if (iid == __uuidof(IDXGISwapChain1)) return "IDXGISwapChain1";
        if (iid == __uuidof(IDXGISwapChain2)) return "IDXGISwapChain2";
        if (iid == __uuidof(IDXGISwapChain3)) return "IDXGISwapChain3";
        if (iid == kIidSwapChain4) return "IDXGISwapChain4";
        if (iid == IID_IUnknown) return "IUnknown";
        if (iid == __uuidof(ID3D12CommandQueue)) return "ID3D12CommandQueue";
        if (iid == __uuidof(ID3D12Device)) return "ID3D12Device";
        return "unknown";
    }
    // Only a declared IID guarantees the prefix, so the slots touched follow the requested/retrieved IID.
    static bool FactoryIidSupported(REFIID iid, bool* hasHwnd) {
        *hasHwnd = false;
        if (iid == __uuidof(IDXGIFactory) || iid == __uuidof(IDXGIFactory1)) return true;          // slots 0..10 (+11)
        if (iid == __uuidof(IDXGIFactory2) || iid == __uuidof(IDXGIFactory3) || iid == __uuidof(IDXGIFactory4) ||
            iid == kIidFactory5 || iid == kIidFactory6 || iid == kIidFactory7) { *hasHwnd = true; return true; }
        return false;
    }
    static bool ChainIidSupported(REFIID iid, bool* hasPresent1, bool* hasResize1) {
        *hasPresent1 = false; *hasResize1 = false;
        if (iid == __uuidof(IDXGISwapChain)) return true;
        if (iid == __uuidof(IDXGISwapChain1) || iid == __uuidof(IDXGISwapChain2) || iid == __uuidof(IDXGISwapChain3) || iid == kIidSwapChain4) {
            *hasPresent1 = true; *hasResize1 = (iid == __uuidof(IDXGISwapChain3) || iid == kIidSwapChain4);
            return true;
        }
        return false;
    }

    // ---- TLS: observer guard, presentation depth, creation depth ---------------------------------------
    static thread_local int t_observerDepth = 0;
    static thread_local int t_presentDepth = 0;
    static thread_local int t_createDepth = 0;
    static bool ObserverGuardActive() { return t_observerDepth > 0; }
    struct ObserverScope { ObserverScope() { ++t_observerDepth; } ~ObserverScope() { --t_observerDepth; } ObserverScope(const ObserverScope&) = delete; ObserverScope& operator=(const ObserverScope&) = delete; };
    struct PresentDepthScope {
        bool outer;
        explicit PresentDepthScope(bool enter) : outer(enter) { if (outer) ++t_presentDepth; }   // spans overlay work and the original call
        ~PresentDepthScope() { if (outer) --t_presentDepth; }
        PresentDepthScope(const PresentDepthScope&) = delete; PresentDepthScope& operator=(const PresentDepthScope&) = delete;
    };
    struct CreationDepthScope {
        int depth;
        CreationDepthScope() : depth(++t_createDepth) {}
        ~CreationDepthScope() { --t_createDepth; }
        CreationDepthScope(const CreationDepthScope&) = delete; CreationDepthScope& operator=(const CreationDepthScope&) = delete;
    };
    static void DelegateReason(const char* what, const char* reason, const void* a = nullptr, const void* b = nullptr) {
        core::Log("[overlay] %s delegate %s reason %s %p %p", kMarker, what, reason, a, b);
    }
    static void CallerModuleToken(void* caller, char* out, size_t cap) {   // provenance of a call site, never an assumption
        if (out && cap) out[0] = 0;
        if (!g_os.moduleFileName) return;
        HMODULE mod = caller && g_os.moduleFromAddress ? g_os.moduleFromAddress(caller) : nullptr;
        if (!mod) { CopyToken(out, cap, "<unresolved>"); return; }
        char name[64] = {};
        if (!g_os.moduleFileName(mod, name, sizeof name)) { CopyToken(out, cap, "<unresolved>"); return; }
        CopyToken(out, cap, name);
    }
    static bool CallerIsMainImage(void* caller) {
        if (!caller || !g_mainModule) return false;
        HMODULE mod = g_os.moduleFromAddress ? g_os.moduleFromAddress(caller) : nullptr;
        return mod && mod == g_mainModule;
    }

    // =====================================================================================================
    // Typed factory-function dispatch records: one immutable original per target, published before the thunk
    // that delegates through it is reachable. Never a single mutable global across distinct providers.
    // =====================================================================================================
    struct FnRec {
        std::atomic<void*> saved{nullptr};
        std::atomic<void*> thunk{nullptr};
        std::atomic<int> origin{0};      // 0 empty, 1 direct import cell, 2 resolver return
        std::atomic<int> flavor{0};      // 1 = HRESULT(REFIID, void**), 2 = HRESULT(UINT, REFIID, void**)
        HMODULE provider = nullptr;      // pinned owner module of `saved`
        const char* name = nullptr;      // static token
    };
    static FnRec g_fnRecs[kMaxFnRecs];
    static std::atomic<void*> g_resolverSaved{nullptr};
    static std::mutex g_patchMutex;      // covers record field publication, VirtualProtect/CAS/restore and table status only

    static void ObserveFactoryGuarded(REFIID iid, void* returned, void* caller);
    struct CaptureSlots;
    // Which boundary handed the chain over. ONLY the verified game-side helper invocation and the factory
    // thunks supply it; it is never inferred from return addresses, module names or any general predicate.
    enum class CaptureOrigin { Factory, GameBoundary };
    static void CaptureCreatedChainGuarded(CaptureSlots* s, IUnknown* gameQueue, IUnknown* returned, REFIID returnedIid, HWND hwnd, void* caller, CaptureOrigin origin);
    static void ObserveFactoryInterfaceGuarded(REFIID iid, void* iface);
    static void ObserveChainInterfaceGuarded(REFIID iid, void* iface);
    static void PresentBoundChain(IDXGISwapChain* self, UINT flags, void* caller);
    class BindingCookie;
    struct ResizeTxn;
    struct ResizeValidation;
    static void ResizeTxnMarkFaulted(ResizeTxn* tx, const char* reason, const void* chain, unsigned code, bool validationUnspecified);
    static void ResizeTxnFaultCloseTerminal(ResizeTxn* tx);
    static void ResizeValidationIterationClose(ResizeValidation* v);
    static void ResizeValidateGuarded(ResizeValidation* v, void* self, BindingCookie* cookie, UINT bufferCount, const UINT* nodes, IUnknown* const* queues);
    static void ResizeBeginTxn(ResizeTxn* tx, void* self, bool buffers1);
    static void ResizePrepareTxn(ResizeTxn* tx, void* self, HRESULT hr, UINT bufferCount, const UINT* nodes, IUnknown* const* queues);
    static void ResizeFinishTxn(ResizeTxn* tx, bool abandoned);
    static bool WaitRenderIdle();
    static void AnalyzeResizeBuffers1(ResizeValidation* v, void* self, BindingCookie* cookie, UINT bufferCount, const UINT* nodes, IUnknown* const* queues);

    // The caller identity is taken inside the typed thunk itself, before any helper runs.
    template<int N> static HRESULT FnReturn(REFIID iid, void** pp, void* caller) {
        FactoryCreateFn fn = reinterpret_cast<FactoryCreateFn>(g_fnRecs[N].saved.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;                                   // unreachable: a thunk is published after its original
        HRESULT hr = fn(iid, pp);                                 // exactly one downstream call, original arguments
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) ObserveFactoryGuarded(iid, *pp, caller);
        return hr;                                                // the game's HRESULT/output are never altered
    }
    template<int N> static HRESULT FnReturnC(UINT flags, REFIID iid, void** pp, void* caller) {
        FactoryCreate2Fn fn = reinterpret_cast<FactoryCreate2Fn>(g_fnRecs[N].saved.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        HRESULT hr = fn(flags, iid, pp);
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) ObserveFactoryGuarded(iid, *pp, caller);
        return hr;
    }
    template<int N> static HRESULT WINAPI FnThunkA(REFIID iid, void** pp) { return FnReturn<N>(iid, pp, _ReturnAddress()); }
    template<int N> static HRESULT WINAPI FnThunkC(UINT flags, REFIID iid, void** pp) { return FnReturnC<N>(flags, iid, pp, _ReturnAddress()); }
    static void* g_fnThunkA[kMaxFnRecs] = {};
    static void* g_fnThunkC[kMaxFnRecs] = {};
    template<int N> struct FillFn { static void Fill() { FillFn<N - 1>::Fill(); g_fnThunkA[N] = reinterpret_cast<void*>(&FnThunkA<N>); g_fnThunkC[N] = reinterpret_cast<void*>(&FnThunkC<N>); } };
    template<> struct FillFn<0> { static void Fill() { g_fnThunkA[0] = reinterpret_cast<void*>(&FnThunkA<0>); g_fnThunkC[0] = reinterpret_cast<void*>(&FnThunkC<0>); } };

    // Reserves (or finds) the record for one exact downstream function pointer; the caller holds no lock.
    static int AcquireFnRec(void* saved, int flavor, int origin, HMODULE provider, const char* name) {
        std::lock_guard<std::mutex> l(g_patchMutex);
        for (int i = 0; i < kMaxFnRecs; i++) {
            if (g_fnRecs[i].origin.load(std::memory_order_relaxed) != 0 && g_fnRecs[i].saved.load(std::memory_order_relaxed) == saved && g_fnRecs[i].flavor.load(std::memory_order_relaxed) == flavor) return i;
        }
        for (int i = 0; i < kMaxFnRecs; i++) {
            int expected = 0;
            if (!g_fnRecs[i].origin.compare_exchange_strong(expected, origin, std::memory_order_acq_rel)) continue;
            g_fnRecs[i].provider = provider; g_fnRecs[i].name = name;
            g_fnRecs[i].flavor.store(flavor, std::memory_order_release);
            g_fnRecs[i].saved.store(saved, std::memory_order_release);   // the original is callable before the thunk is
            g_fnRecs[i].thunk.store(flavor == 2 ? g_fnThunkC[i] : g_fnThunkA[i], std::memory_order_release);
            return i;
        }
        return -1;      // declared capacity exhausted: fail open, the game keeps its own pointer
    }

    // GetProcAddress return tap: the exact saved resolver runs first (result, LastError and ordinals preserved),
    // and only a non-null result for the three exact names from dxgi.dll / sl.interposer.dll is substituted.
    static bool ModuleIsDxgiOrInterposer(HMODULE mod) {
        char name[64] = {};
        if (!g_os.moduleFileName || !g_os.moduleFileName(mod, name, sizeof name)) return false;
        return StrEq(name, "dxgi.dll") || StrEq(name, "sl.interposer.dll");
    }
    static FARPROC WINAPI HkGetProcAddress(HMODULE mod, LPCSTR name) {
        GetProcAddressFn fn = reinterpret_cast<GetProcAddressFn>(g_resolverSaved.load(std::memory_order_acquire));
        if (!fn) { SetLastError(ERROR_PROC_NOT_FOUND); return nullptr; }
        FARPROC resolved = fn(mod, name);
        const DWORD savedError = GetLastError();                    // the resolver's own error, taken right after its call
        if (!resolved || !name || HIWORD(reinterpret_cast<uintptr_t>(name)) == 0) { SetLastError(savedError); return resolved; }
        const bool is2 = StrEq(name, "CreateDXGIFactory2");
        if (!is2 && !StrEq(name, "CreateDXGIFactory") && !StrEq(name, "CreateDXGIFactory1")) { SetLastError(savedError); return resolved; }
        if (!ModuleIsDxgiOrInterposer(mod)) { SetLastError(savedError); return resolved; }
        HMODULE provider = g_os.moduleFromAddress ? g_os.moduleFromAddress(reinterpret_cast<const void*>(resolved)) : nullptr;
        if (!provider) { core::Log("[overlay] %s delegate resolve api %s reason nonmodule_pointer %p", kMarker, name, reinterpret_cast<const void*>(resolved)); SetLastError(savedError); return resolved; }
        if (!PinTarget(reinterpret_cast<void*>(resolved))) { core::Log("[overlay] %s delegate resolve api %s reason pin_failed %p", kMarker, name, reinterpret_cast<const void*>(resolved)); SetLastError(savedError); return resolved; }
        char providerName[64] = {};
        if (g_os.moduleFileName) g_os.moduleFileName(provider, providerName, sizeof providerName);
        if (g_os.pinModule) g_os.pinModule(provider);                    // the resolved target is retained for process lifetime
        const int flavor = is2 ? 2 : 1;
        const int rec = AcquireFnRec(reinterpret_cast<void*>(resolved), flavor, 2, provider, is2 ? "CreateDXGIFactory2" : name);
        if (rec < 0) { core::Log("[overlay] %s delegate resolve api %s reason capacity_exhausted", kMarker, name); SetLastError(savedError); return resolved; }
        void* thunk = flavor == 2 ? g_fnThunkC[rec] : g_fnThunkA[rec];
        core::Log("[overlay] %s resolve api %s provider %s dispatch %d module %p", kMarker, name, providerName, rec, reinterpret_cast<void*>(mod));
        SetLastError(savedError);
        return reinterpret_cast<FARPROC>(thunk);   // a stable typed thunk bound to this exact returned pointer
    }

    // =====================================================================================================
    // Observed public vtable records with CAS-owned cells
    // =====================================================================================================
    struct FactoryVtRec {
        std::atomic<void**> vtable{nullptr};
        std::atomic<int> terminalLogged{0};        // the terminal state was reported once for this table
        std::atomic<void*> savedQi{nullptr}, savedCreate{nullptr}, savedHwnd{nullptr};
        std::atomic<int> state{0};        // 0 empty, 1 preparing, 2 ready, 3 failed/unsupported
        std::atomic<unsigned> coverage{0};// the published coverage word: which of this record's slots are owned
        std::atomic<int> ownedQi{0}, ownedCreate{0}, ownedHwnd{0};
        std::atomic<int> lost{0};         // ownership found gone: delegation-only from then on
    };
    struct ChainVtRec {
        std::atomic<void**> vtable{nullptr};
        std::atomic<int> terminalLogged{0};        // the terminal state was reported once for this table
        std::atomic<void*> savedQi{nullptr}, savedPresent{nullptr}, savedPresent1{nullptr}, savedResize{nullptr}, savedResize1{nullptr};
        std::atomic<int> state{0};
        std::atomic<unsigned> coverage{0};
        std::atomic<int> ownedQi{0}, ownedPresent{0}, ownedPresent1{0}, ownedResize{0}, ownedResize1{0};
        std::atomic<int> lost{0};
    };
    static FactoryVtRec g_factoryVt[kMaxFactoryVt];
    static ChainVtRec g_chainVt[kMaxChainVt];

    struct FactoryThunks { void* qi; void* create; void* hwnd; };
    struct ChainThunks { void* qi; void* present; void* present1; void* resize; void* resize1; };
    static FactoryThunks g_factoryThunks[kMaxFactoryVt];
    static ChainThunks g_chainThunks[kMaxChainVt];

    template<int N> static HRESULT STDMETHODCALLTYPE FactoryQiThunk(IUnknown* self, REFIID iid, void** pp) {
        FactoryQi_t fn = reinterpret_cast<FactoryQi_t>(g_factoryVt[N].savedQi.load(std::memory_order_acquire));
        if (!fn) return E_NOINTERFACE;                       // unreachable: published before the cell changed
        HRESULT hr = fn(self, iid, pp);
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) ObserveFactoryInterfaceGuarded(iid, *pp);   // the pointer itself is returned unchanged
        return hr;
    }
    template<int N> static HRESULT STDMETHODCALLTYPE FactoryCreateThunk(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
        FactoryCreateSwapChain_t fn = reinterpret_cast<FactoryCreateSwapChain_t>(g_factoryVt[N].savedCreate.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        void* caller = _ReturnAddress();
        const bool owned = !ObserverGuardActive() && RecordReadyFrom("factory_capture", N, true, kFactoryBaseMask);   // ownership + coverage re-checked at capture
        const bool eligible = owned && t_createDepth == 0 && CallerIsMainImage(caller);
        CreationDepthScope scope;                            // nested creations (SL/ReShade inner chains) delegate, never capture
        HRESULT hr = fn(self, device, desc, pp);
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) {
            if (eligible) { ObserverScope guard; CaptureSlots slots; CaptureCreatedChainGuarded(&slots, device, *pp, __uuidof(IDXGISwapChain), desc ? desc->OutputWindow : nullptr, caller, CaptureOrigin::Factory); }
            ObserveChainInterfaceGuarded(__uuidof(IDXGISwapChain), *pp);
        }
        return hr;
    }
    template<int N> static HRESULT STDMETHODCALLTYPE FactoryHwndThunk(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        FactoryCreateSwapChainForHwnd_t fn = reinterpret_cast<FactoryCreateSwapChainForHwnd_t>(g_factoryVt[N].savedHwnd.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        void* caller = _ReturnAddress();
        const bool owned = !ObserverGuardActive() && RecordReadyFrom("factory_capture", N, true, kFactoryBaseMask | kFbHwnd);   // ownership + coverage re-checked at capture
        if (!owned) DelegateReason("factory_capture", "record_not_owned", self);
        const bool eligible = owned && t_createDepth == 0 && CallerIsMainImage(caller);
        core::Log("[overlay] %s create entry Hwnd outer %d factory %p device %p hwnd %p width %u height %u buffers %u", kMarker,
                  eligible ? 1 : 0, (void*)self, (void*)device, (void*)hwnd, desc ? desc->Width : 0u, desc ? desc->Height : 0u, desc ? desc->BufferCount : 0u);
        CreationDepthScope scope;
        HRESULT hr = fn(self, device, hwnd, desc, fs, out, pp);
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) {
            if (eligible) { ObserverScope guard; CaptureSlots slots; CaptureCreatedChainGuarded(&slots, device, *pp, __uuidof(IDXGISwapChain1), hwnd, caller, CaptureOrigin::Factory); }
            ObserveChainInterfaceGuarded(__uuidof(IDXGISwapChain1), *pp);   // the returned chain's own table; drawing still needs a ready cookie
        }
        return hr;
    }
    template<int N> static HRESULT STDMETHODCALLTYPE ChainQiThunk(IUnknown* self, REFIID iid, void** pp) {
        FactoryQi_t fn = reinterpret_cast<FactoryQi_t>(g_chainVt[N].savedQi.load(std::memory_order_acquire));
        if (!fn) return E_NOINTERFACE;
        HRESULT hr = fn(self, iid, pp);
        if (SUCCEEDED(hr) && pp && *pp && !ObserverGuardActive()) ObserveChainInterfaceGuarded(iid, *pp);
        return hr;
    }
    template<int N> static HRESULT STDMETHODCALLTYPE ChainPresentThunk(IDXGISwapChain* self, UINT sync, UINT flags) {
        Present_t fn = reinterpret_cast<Present_t>(g_chainVt[N].savedPresent.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        void* caller = _ReturnAddress();
        const bool outer = (t_presentDepth == 0);
        PresentDepthScope scope(outer);                      // spans overlay work and the original: nesting cannot draw twice
        if (outer) PresentBoundChain(self, flags, caller);
        return fn(self, sync, flags);
    }
    template<int N> static HRESULT STDMETHODCALLTYPE ChainPresent1Thunk(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
        Present1_t fn = reinterpret_cast<Present1_t>(g_chainVt[N].savedPresent1.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        void* caller = _ReturnAddress();
        const bool outer = (t_presentDepth == 0);
        PresentDepthScope scope(outer);
        if (outer) PresentBoundChain(reinterpret_cast<IDXGISwapChain*>(self), flags, caller);
        return fn(self, sync, flags, pp);
    }
    // Guarded added-work leaves and the resize thunks live with the transaction (defined after ResizeTxn): the
    // render lock is owned only by the non-guarded callers (ResizeBeginTxn/ResizeFinishTxn), exactly like the
    // Present locked wrapper, so a caught structured fault cannot bypass the unlock.
    template<int N> static HRESULT STDMETHODCALLTYPE ChainResizeThunk(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        ResizeBuffers_t fn = reinterpret_cast<ResizeBuffers_t>(g_chainVt[N].savedResize.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        ResizeTxn tx;
        ResizeBeginTxn(&tx, self, false);                    // qualification leaf, then the lock scope outside every catcher
        HRESULT hr = fn(self, n, w, h, fmt, flags);          // original arguments, original HRESULT, outside every overlay mutex
        ResizePrepareTxn(&tx, self, hr, 0, nullptr, nullptr);
        if (tx.faulted) ResizeTxnFaultCloseTerminal(&tx);    // terminal: never retried by a blocking Finish or the destructor
        else ResizeFinishTxn(&tx, false);
        return hr;
    }
    template<int N> static HRESULT STDMETHODCALLTYPE ChainResize1Thunk(IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* nodes, IUnknown* const* queues) {
        ResizeBuffers1_t fn = reinterpret_cast<ResizeBuffers1_t>(g_chainVt[N].savedResize1.load(std::memory_order_acquire));
        if (!fn) return E_FAIL;
        ResizeTxn tx;
        ResizeBeginTxn(&tx, reinterpret_cast<void*>(self), true);
        HRESULT hr = fn(self, n, w, h, fmt, flags, nodes, queues);   // original arguments, original HRESULT, outside every overlay mutex
        ResizePrepareTxn(&tx, reinterpret_cast<void*>(self), hr, n, nodes, queues);
        if (tx.faulted) ResizeTxnFaultCloseTerminal(&tx);
        else ResizeFinishTxn(&tx, false);
        return hr;
    }
    template<int N> struct FillFactory { static void Fill() { FillFactory<N - 1>::Fill(); g_factoryThunks[N].qi = reinterpret_cast<void*>(&FactoryQiThunk<N>); g_factoryThunks[N].create = reinterpret_cast<void*>(&FactoryCreateThunk<N>); g_factoryThunks[N].hwnd = reinterpret_cast<void*>(&FactoryHwndThunk<N>); } };
    template<> struct FillFactory<0> { static void Fill() { g_factoryThunks[0].qi = reinterpret_cast<void*>(&FactoryQiThunk<0>); g_factoryThunks[0].create = reinterpret_cast<void*>(&FactoryCreateThunk<0>); g_factoryThunks[0].hwnd = reinterpret_cast<void*>(&FactoryHwndThunk<0>); } };
    template<int N> struct FillChain { static void Fill() { FillChain<N - 1>::Fill(); g_chainThunks[N].qi = reinterpret_cast<void*>(&ChainQiThunk<N>); g_chainThunks[N].present = reinterpret_cast<void*>(&ChainPresentThunk<N>); g_chainThunks[N].present1 = reinterpret_cast<void*>(&ChainPresent1Thunk<N>); g_chainThunks[N].resize = reinterpret_cast<void*>(&ChainResizeThunk<N>); g_chainThunks[N].resize1 = reinterpret_cast<void*>(&ChainResize1Thunk<N>); } };
    template<> struct FillChain<0> { static void Fill() { g_chainThunks[0].qi = reinterpret_cast<void*>(&ChainQiThunk<0>); g_chainThunks[0].present = reinterpret_cast<void*>(&ChainPresentThunk<0>); g_chainThunks[0].present1 = reinterpret_cast<void*>(&ChainPresent1Thunk<0>); g_chainThunks[0].resize = reinterpret_cast<void*>(&ChainResizeThunk<0>); g_chainThunks[0].resize1 = reinterpret_cast<void*>(&ChainResize1Thunk<0>); } };
    static void PublishThunkTables() {   // static addresses only; no executable memory is generated
        FillFn<kMaxFnRecs - 1>::Fill();
        FillFactory<kMaxFactoryVt - 1>::Fill();
        FillChain<kMaxChainVt - 1>::Fill();
    }

    // One cell transaction: protect, CAS(expected -> our thunk), restore protection. The write outcome and the
    // protection outcome are separate facts: a CAS conflict is never pointer ownership, and a CAS success whose
    // protection restore failed still owns the cell (and must still be rolled back exactly).
    struct TapResult { bool madeWritable = false; bool wrotePointer = false; bool restoredProtection = false; void* observedPointer = nullptr; DWORD originalProtection = 0; };
    static TapResult TapCellEx(void** cell, void* expected, void* thunk) {
        TapResult r;
        if (!cell || !g_os.virtualProtect) return r;
        DWORD old = 0;
        if (!g_os.virtualProtect(cell, sizeof(void*), PAGE_READWRITE, &old)) return r;
        r.madeWritable = true; r.originalProtection = old;
        void* prev = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(cell), thunk, expected);
        if (prev == expected) r.wrotePointer = true; else r.observedPointer = prev;
        DWORD tmp = 0;
        r.restoredProtection = g_os.virtualProtect(cell, sizeof(void*), old, &tmp) != FALSE;
        return r;
    }
    // Rollback outcomes are two separate facts: whether the exact pointer was ours to undo, and whether the
    // protection captured BEFORE the original write was restored. Only the first clears ownership; a failed
    // restore is recorded without turning a foreign cell into an owned one.
    struct RollbackOutcome { bool madeWritable = false; bool pointerRestored = false; bool protectionRestored = false; };
    static RollbackOutcome RollbackWrite(void** cell, void* thunk, void* saved, DWORD originalProtection) {
        RollbackOutcome r;
        if (!cell || !g_os.virtualProtect) return r;
        DWORD old = 0;
        if (!g_os.virtualProtect(cell, sizeof(void*), PAGE_READWRITE, &old)) return r;
        r.madeWritable = true;
        void* prev = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(cell), saved, thunk);
        r.pointerRestored = (prev == thunk);
        DWORD tmp = 0;
        r.protectionRestored = g_os.virtualProtect(cell, sizeof(void*), originalProtection, &tmp) != FALSE;
        return r;
    }
    // ---- registry publication: one status+coverage word per record, one CAS-validated transaction ----------
    // Coverage bits: one per touched slot. A slot outside the observed IID's declared prefix has no bit here,
    // so it is never inspected, pinned or tapped. The two registries reuse the same bit values.
    enum { kFbQi = 1u, kFbCreate = 2u, kFbHwnd = 4u };
    enum { kCbQi = 1u, kCbPresent = 2u, kCbResize = 4u, kCbPresent1 = 8u, kCbResize1 = 16u };
    static const unsigned kFactoryBaseMask = kFbQi | kFbCreate;                // slots 0,10
    static const unsigned kChainBaseMask = kCbQi | kCbPresent | kCbResize;     // slots 0,8,13
    struct SlotDef { int slot; unsigned bit; };
    static const SlotDef kFactorySlotDefs[3] = { { 0, kFbQi }, { 10, kFbCreate }, { 15, kFbHwnd } };
    static const SlotDef kChainSlotDefs[5] = { { 0, kCbQi }, { 8, kCbPresent }, { 13, kCbResize }, { 22, kCbPresent1 }, { 39, kCbResize1 } };
    static unsigned FactoryMaskForIid(REFIID iid) { bool hw = false; return FactoryIidSupported(iid, &hw) ? (kFactoryBaseMask | (hw ? kFbHwnd : 0u)) : 0u; }
    static unsigned ChainMaskForIid(REFIID iid) { bool p1 = false, r1 = false; if (!ChainIidSupported(iid, &p1, &r1)) return 0u; return kChainBaseMask | (p1 ? kCbPresent1 : 0u) | (r1 ? kCbResize1 : 0u); }

    // A borrowed view of one record; the publication engine works on the view so both registries share one
    // protocol without another abstraction layer over the two concrete record types.
    struct VtSlotRef { int slot = 0; unsigned bit = 0; std::atomic<void*>* saved = nullptr; std::atomic<int>* owned = nullptr; void* thunk = nullptr; };
    struct VtRef { std::atomic<void**>* vtable = nullptr; std::atomic<int>* state = nullptr; std::atomic<unsigned>* coverage = nullptr; std::atomic<int>* lost = nullptr; VtSlotRef slots[5]; int slotCount = 0; };
    static int RecCount(bool f) { return f ? kMaxFactoryVt : kMaxChainVt; }
    static std::atomic<int>& RecState(bool f, int i) { return f ? g_factoryVt[i].state : g_chainVt[i].state; }
    static std::atomic<unsigned>& RecCoverage(bool f, int i) { return f ? g_factoryVt[i].coverage : g_chainVt[i].coverage; }
    static std::atomic<int>& RecLost(bool f, int i) { return f ? g_factoryVt[i].lost : g_chainVt[i].lost; }
    static std::atomic<void**>& RecVtable(bool f, int i) { return f ? g_factoryVt[i].vtable : g_chainVt[i].vtable; }
    static std::atomic<void*>* RecSaved(bool f, int i, int p) {
        if (f) { FactoryVtRec& r = g_factoryVt[i]; return p == 0 ? &r.savedQi : p == 1 ? &r.savedCreate : &r.savedHwnd; }
        ChainVtRec& r = g_chainVt[i]; return p == 0 ? &r.savedQi : p == 1 ? &r.savedPresent : p == 2 ? &r.savedResize : p == 3 ? &r.savedPresent1 : &r.savedResize1;
    }
    static std::atomic<int>* RecOwned(bool f, int i, int p) {
        if (f) { FactoryVtRec& r = g_factoryVt[i]; return p == 0 ? &r.ownedQi : p == 1 ? &r.ownedCreate : &r.ownedHwnd; }
        ChainVtRec& r = g_chainVt[i]; return p == 0 ? &r.ownedQi : p == 1 ? &r.ownedPresent : p == 2 ? &r.ownedResize : p == 3 ? &r.ownedPresent1 : &r.ownedResize1;
    }
    static void* RecThunk(bool f, int i, int p) {
        if (f) { FactoryThunks& t = g_factoryThunks[i]; return p == 0 ? t.qi : p == 1 ? t.create : t.hwnd; }
        ChainThunks& t = g_chainThunks[i]; return p == 0 ? t.qi : p == 1 ? t.present : p == 2 ? t.resize : p == 3 ? t.present1 : t.resize1;
    }
    static void ViewRecord(bool f, int i, VtRef* v) {
        v->vtable = &RecVtable(f, i); v->state = &RecState(f, i); v->coverage = &RecCoverage(f, i); v->lost = &RecLost(f, i);
        v->slotCount = f ? 3 : 5;
        const SlotDef* defs = f ? kFactorySlotDefs : kChainSlotDefs;
        for (int p = 0; p < v->slotCount; p++) { v->slots[p].slot = defs[p].slot; v->slots[p].bit = defs[p].bit; v->slots[p].saved = RecSaved(f, i, p); v->slots[p].owned = RecOwned(f, i, p); v->slots[p].thunk = RecThunk(f, i, p); }
    }
    static bool IsOurThunk(bool f, void* fn) {   // never saved as a downstream original, in any record
        if (!fn) return false;
        if (f) {
            for (int i = 0; i < kMaxFactoryVt; i++) if (fn == g_factoryThunks[i].qi || fn == g_factoryThunks[i].create || fn == g_factoryThunks[i].hwnd) return true;
        } else {
            for (int j = 0; j < kMaxChainVt; j++) if (fn == g_chainThunks[j].qi || fn == g_chainThunks[j].present || fn == g_chainThunks[j].resize || fn == g_chainThunks[j].present1 || fn == g_chainThunks[j].resize1) return true;
        }
        return false;
    }
    // Factory/chain records also remember that their terminal state was reported, so a repeatedly observed foreign
    // table logs once instead of once per game call (the 1 Hz extension loop in the live log).
    static std::atomic<int>& RecTerminalLogged(bool f, int i) { return f ? g_factoryVt[i].terminalLogged : g_chainVt[i].terminalLogged; }
    static void ObserveTerminalOnce(bool f, int i, const char* what, void** vt) {   // one line per terminal table
        if (i < 0) return;                                                         // no record: nothing to gate on, nothing to index
        if (RecTerminalLogged(f, i).exchange(1, std::memory_order_acq_rel) != 0) return;
        core::Log("[overlay] %s delegate %s table %p reason extension_unsupported terminal 1 no_retap 1", kMarker, what, (void*)vt);
    }
    static int FindRecord(bool f, void** vt) {   // reserved or published records only
        for (int i = 0; i < RecCount(f); i++) if (RecVtable(f, i).load(std::memory_order_acquire) == vt && RecState(f, i).load(std::memory_order_acquire) != 0) return i;
        return -1;
    }
    static int FindChainVt(void** vt) { return FindRecord(false, vt); }
    static int ReserveRecord(bool f, void** vt) {   // patch lock held: Empty -> Preparing, table published before any cell work
        for (int i = 0; i < RecCount(f); i++) {
            int expected = 0;
            if (RecState(f, i).compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) { RecVtable(f, i).store(vt, std::memory_order_release); return i; }
        }
        return -1;
    }
    static bool CellsOurs(bool f, int i) {
        void** vt = RecVtable(f, i).load(std::memory_order_acquire);
        if (!vt) return false;
        VtRef v; ViewRecord(f, i, &v);
        for (int p = 0; p < v.slotCount; p++) {
            if (!v.slots[p].owned->load(std::memory_order_acquire)) continue;
            void* fn = nullptr;
            if (!SafeRead(&vt[v.slots[p].slot], &fn, sizeof fn) || fn != v.slots[p].thunk) return false;
        }
        return true;
    }
    // Ready only while the published word covers the requirement and every required owned cell still holds our
    // typed thunk. Preparing is not ownership loss; loss of a published cell is terminal, never repaired.
    static bool RecordReadyFrom(const char* what, int idx, bool factory, unsigned requiredMask) {
        if (idx < 0) return false;
        if (RecState(factory, idx).load(std::memory_order_acquire) != 2) return false;
        if ((RecCoverage(factory, idx).load(std::memory_order_acquire) & requiredMask) != requiredMask) return false;
        if (RecLost(factory, idx).load(std::memory_order_acquire) != 0) return false;
        if (CellsOurs(factory, idx)) return true;
        RecState(factory, idx).store(3, std::memory_order_release);
        if (RecLost(factory, idx).exchange(1, std::memory_order_acq_rel) == 0) core::Log("[overlay] %s delegate %s reason ownership_lost table %p", kMarker, what, (void*)RecVtable(factory, idx).load(std::memory_order_relaxed));
        return false;
    }
    static bool ChainRecordFor(void* chainSelf, int* idxOut) {   // the record of the object actually presented
        *idxOut = -1;
        void* vt = nullptr;
        if (!SafeRead(chainSelf, &vt, sizeof vt) || !vt) return false;
        *idxOut = FindChainVt(reinterpret_cast<void**>(vt));
        return *idxOut >= 0;
    }

    // ---- one install/extend protocol for both registries ------------------------------------------------
    // Preparation (exact cell read, owner identity, BOTH owners pinned) happens OUTSIDE the registry lock; the
    // cell CAS inside the lock revalidates the exact value; one status+coverage word is published atomically.
    // A failure of any required slot is terminal for the record (delegation-only); a shorter Ready record never
    // authorizes a longer prefix requirement.
    enum TapOutcome { TapReady = 0, TapCapacity, TapPrepHard, TapProtect, TapRestore, TapConflict, TapTerminal };
    static const char* TapToken(TapOutcome o) {
        switch (o) {
        case TapCapacity: return "capacity_exhausted";
        case TapPrepHard: return "vtable_unsupported";
        case TapProtect: return "protection_failed";
        case TapRestore: return "protection_restore_failed";
        case TapTerminal: return "terminal";
        default: return "cas_conflict";
        }
    }
    static TapOutcome EnsureCoverage(bool factory, void** vt, unsigned required, VtRef* out, bool* extendedOut, int* idxOut, bool* grewOut) {
        *extendedOut = false;
        if (grewOut) *grewOut = false;
        if (idxOut) *idxOut = -1;
        const SlotDef* defs = factory ? kFactorySlotDefs : kChainSlotDefs;
        const int defCount = factory ? 3 : 5;
        for (int attempt = 0; attempt < 8; attempt++) {
            int idx = -1, state = 0; unsigned coverage = 0;
            {
                std::lock_guard<std::mutex> l(g_patchMutex);
                idx = FindRecord(factory, vt);
                if (idx >= 0) { state = RecState(factory, idx).load(std::memory_order_acquire); coverage = RecCoverage(factory, idx).load(std::memory_order_acquire); }
            }
            if (idx >= 0 && state == 3) { *extendedOut = true; if (idxOut) *idxOut = idx; return TapTerminal; }           // terminal Failed: no repair loop, no re-tap
            if (idx >= 0 && state == 2 && (coverage & required) == required) { ViewRecord(factory, idx, out); *extendedOut = true; if (idxOut) *idxOut = idx; return TapReady; }
            const bool extending = idx >= 0;
            *extendedOut = extending;
            const unsigned missing = required & ~coverage;
            // 1. prepare outside every state lock: read the exact cell, validate identity, pin both owners
            struct Prep { int pos; void* fn; };
            Prep prep[5] = {}; int prepCount = 0; bool prepOk = true;
            HMODULE tableOwner = g_os.moduleFromAddress ? g_os.moduleFromAddress(reinterpret_cast<const void*>(&vt[0])) : nullptr;
            if (!tableOwner || !g_os.pinModule || g_os.pinModule(tableOwner) != tableOwner) prepOk = false;
            for (int p = 0; p < defCount && prepOk; p++) {
                if (!(missing & defs[p].bit)) continue;
                void* fn = nullptr;
                if (!SafeRead(&vt[defs[p].slot], &fn, sizeof fn) || !fn || IsOurThunk(factory, fn)) { prepOk = false; break; }
                HMODULE cellMod = g_os.moduleFromAddress ? g_os.moduleFromAddress(reinterpret_cast<const void*>(&vt[defs[p].slot])) : nullptr;
                HMODULE fnMod = g_os.moduleFromAddress ? g_os.moduleFromAddress(reinterpret_cast<const void*>(fn)) : nullptr;
                if (!cellMod || !fnMod) { prepOk = false; break; }
                if (!g_os.pinModule || g_os.pinModule(cellMod) != cellMod || g_os.pinModule(fnMod) != fnMod) { prepOk = false; break; }
                prep[prepCount].pos = p; prep[prepCount].fn = fn; prepCount++;
            }
            bool retry = false;
            bool publishFailed = false; TapOutcome failOutcome = TapConflict;
            int rollbackCells = 0, rollbackPointerFailed = 0, rollbackProtectionFailed = 0; bool rollbackCoherent = true;
            {
                std::lock_guard<std::mutex> l(g_patchMutex);
                const int again = FindRecord(factory, vt);
                if (again >= 0) {
                    const int st2 = RecState(factory, again).load(std::memory_order_acquire);
                    if (st2 == 3) { *extendedOut = true; if (idxOut) *idxOut = again; return TapTerminal; }
                    if (st2 == 2 && (RecCoverage(factory, again).load(std::memory_order_acquire) & required) == required) { ViewRecord(factory, again, out); *extendedOut = true; if (idxOut) *idxOut = again; return TapReady; }
                    if (st2 == 1) {
                        // Preparing here can only be an abandoned window (a live transaction holds this lock): never re-tap it.
                        retry = true;
                        if (attempt >= 7) { RecState(factory, again).store(3, std::memory_order_release); RecLost(factory, again).store(1, std::memory_order_release); if (idxOut) *idxOut = again; return TapPrepHard; }
                    } else if (again != idx || RecCoverage(factory, again).load(std::memory_order_acquire) != coverage) { retry = true; }   // another install moved the snapshot: recompute missing-only
                    else idx = again;
                } else {
                    const int reserved = ReserveRecord(factory, vt);           // Empty -> Preparing, before any cell work
                    if (reserved < 0) return TapCapacity;
                    idx = reserved;
                }
                if (!retry) {
                    if (!prepOk) { RecState(factory, idx).store(3, std::memory_order_release); RecLost(factory, idx).store(1, std::memory_order_release); *extendedOut = extending; if (idxOut) *idxOut = idx; return TapPrepHard; }
                    RecState(factory, idx).store(1, std::memory_order_release);    // Preparing is published before any cell changes
                    VtRef v; ViewRecord(factory, idx, &v);
                    for (int i = 0; i < prepCount; i++) v.slots[prep[i].pos].saved->store(prep[i].fn, std::memory_order_release);   // originals before their thunks are reachable
                    struct Wr { int pos; void* fn; DWORD prot; };
                    Wr wr[5] = {}; int wn = 0; bool allWrote = true, allRestored = true, protectFail = false, restoreFail = false, conflictFail = false;
                    for (int i = 0; i < prepCount; i++) {
                        const TapResult tr = TapCellEx(&vt[defs[prep[i].pos].slot], prep[i].fn, v.slots[prep[i].pos].thunk);
                        if (!tr.madeWritable) protectFail = true;
                        else if (!tr.wrotePointer) conflictFail = true;
                        if (!tr.restoredProtection) restoreFail = true;
                        if (tr.wrotePointer) { v.slots[prep[i].pos].owned->store(1, std::memory_order_release); wr[wn].pos = prep[i].pos; wr[wn].fn = prep[i].fn; wr[wn].prot = tr.originalProtection; wn++; }
                        else allWrote = false;
                        if (!tr.restoredProtection) allRestored = false;
                    }
                    if (allWrote && allRestored) {
                        if (grewOut && (RecCoverage(factory, idx).load(std::memory_order_acquire) & required) != required) *grewOut = true;   // real extension
                        RecCoverage(factory, idx).fetch_or(required, std::memory_order_release);
                        RecState(factory, idx).store(2, std::memory_order_release);
                        ViewRecord(factory, idx, out); *extendedOut = extending; if (idxOut) *idxOut = idx; return TapReady;
                    }
                    for (int i = wn - 1; i >= 0; i--) {                            // rollback only this transaction's exact writes
                        const RollbackOutcome rb = RollbackWrite(&vt[defs[wr[i].pos].slot], v.slots[wr[i].pos].thunk, wr[i].fn, wr[i].prot);
                        if (rb.pointerRestored) v.slots[wr[i].pos].owned->store(0, std::memory_order_release);
                        else rollbackPointerFailed++;
                        if (!rb.protectionRestored) rollbackProtectionFailed++;
                        rollbackCells++;
                    }
                    // Coherent status/coverage check (still two atomics, one snapshot): publish the terminal Failed
                    // state only while this record is the Preparing reservation for this table and its coverage did
                    // not move under us. Escaped originals stay saved and callable whatever the outcome.
                    const int stateNow = RecState(factory, idx).load(std::memory_order_acquire);
                    const unsigned coverageNow = RecCoverage(factory, idx).load(std::memory_order_acquire);
                    rollbackCoherent = (RecVtable(factory, idx).load(std::memory_order_acquire) == vt && stateNow == 1 && coverageNow == coverage);
                    if (rollbackCoherent) {
                        RecState(factory, idx).store(3, std::memory_order_release);    // required coverage was not published: delegation-only
                        RecLost(factory, idx).store(1, std::memory_order_release);
                    }
                    failOutcome = protectFail ? TapProtect : restoreFail ? TapRestore : (conflictFail || !allWrote) ? TapConflict : TapRestore;
                    publishFailed = true;
                }
            }
            if (publishFailed) {                               // both rollback outcomes are logged after unlocking
                core::Log("[overlay] %s delegate %s table %p reason %s rollback_cells %d pointer_failed %d protection_failed %d coherent %d", kMarker,
                          factory ? "factory_return" : "chain_table", (void*)vt, TapToken(failOutcome), rollbackCells, rollbackPointerFailed, rollbackProtectionFailed, rollbackCoherent ? 1 : 0);
                *extendedOut = extending;
                if (idxOut) *idxOut = idx;
                return failOutcome;
            }
            if (retry) continue;
        }
        return TapConflict;   // the snapshot kept changing under foreign writers: nothing was published
    }
    // Installs the supported prefix of one observed factory table. Never copies or replaces the object's vptr.
    static void InstallFactoryCells(void** vt, REFIID iid, const char* api) {
        const unsigned required = FactoryMaskForIid(iid);
        if (!required) { core::Log("[overlay] %s delegate factory_return api %s iid %s reason unsupported_iid %p", kMarker, api, IidName(iid), (void*)vt); return; }
        VtRef ref; bool extended = false; int idx = -1;
        const TapOutcome outcome = EnsureCoverage(true, vt, required, &ref, &extended, &idx, nullptr);
        if (outcome == TapReady) {
            if (extended && !RecordReadyFrom("factory_return", idx, true, required)) return;   // ownership loss is reported once there
            if (!extended) core::Log("[overlay] %s factory_return api %s iid %s factory %p table %p vt15 %p slots ready", kMarker, api, IidName(iid), (void*)vt, (void*)vt, (required & kFbHwnd) ? vt[15] : nullptr);
            return;                                   // a repeated observation of a covered table is not a new event
        }
        if (extended) {
            if (outcome == TapTerminal) { ObserveTerminalOnce(true, idx, "factory_return", vt); return; }   // once per terminal table
            core::Log("[overlay] %s delegate factory_return table %p reason extension_%s", kMarker, (void*)vt, outcome == TapPrepHard ? "unsupported" : "incomplete");
            return;
        }
        core::Log("[overlay] %s factory_return api %s iid %s factory %p table %p vt15 %p slots %s", kMarker, api, IidName(iid), (void*)vt, (void*)vt, nullptr, TapToken(outcome));
    }

    // Installs the supported prefix of one observed chain table (QI, resize coverage, then presentation); a
    // longer alias of the same table extends the same record through the same transaction.
    static void InstallChainCells(void** vt, REFIID iid) {
        const unsigned required = ChainMaskForIid(iid);
        if (!required) { core::Log("[overlay] %s delegate chain_table iid %s reason unsupported_iid %p", kMarker, IidName(iid), (void*)vt); return; }
        VtRef ref; bool extended = false; int idx = -1; bool grew = false;
        const TapOutcome outcome = EnsureCoverage(false, vt, required, &ref, &extended, &idx, &grew);
        if (outcome == TapReady) {
            if (extended && !RecordReadyFrom("chain_table", idx, false, required)) return;    // ownership loss is reported once there
            // A covered re-observation (no new coverage) is silent: only a real extension reports "extended". The
            // requested-mask semantics are unchanged.
            if (!extended) core::Log("[overlay] %s chain_table table %p iid %s present1 %d resize1 %d slots ready", kMarker, (void*)vt, IidName(iid), (required & kCbPresent1) ? 1 : 0, (required & kCbResize1) ? 1 : 0);
            else if (grew) core::Log("[overlay] %s chain_table table %p extended present1 %d resize1 %d", kMarker, (void*)vt, (required & kCbPresent1) ? 1 : 0, (required & kCbResize1) ? 1 : 0);
            return;
        }
        if (extended) {
            if (outcome == TapTerminal) { ObserveTerminalOnce(false, idx, "chain_table", vt); return; }      // once per terminal table
            core::Log("[overlay] %s delegate chain_table table %p reason %s", kMarker, (void*)vt, outcome == TapPrepHard ? "extension_unsupported" : "extension_incomplete");
            return;
        }
        core::Log("[overlay] %s chain_table table %p iid %s present1 %d resize1 %d slots %s", kMarker, (void*)vt, IidName(iid), (required & kCbPresent1) ? 1 : 0, (required & kCbResize1) ? 1 : 0, TapToken(outcome));
    }

    static void ObserveFactory(REFIID iid, void* returned, void* caller) {
        if (!returned) return;
        bool hasHwnd = false;
        if (!FactoryIidSupported(iid, &hasHwnd)) { DelegateReason("factory", "unsupported_iid", returned); return; }
        void* vt = nullptr;
        if (!SafeRead(returned, &vt, sizeof vt) || !vt) { DelegateReason("factory", "unreadable_vtable", returned); return; }
        char mod[64] = {}; CallerModuleToken(caller, mod, sizeof mod);
        core::Log("[overlay] %s factory_observed iid %s factory %p table %p caller_module %s", kMarker, IidName(iid), returned, vt, mod);
        InstallFactoryCells(reinterpret_cast<void**>(vt), iid, IidName(iid));
    }
    static void ObserveFactoryInterface(REFIID iid, void* iface) {
        if (!iface) return;
        bool hasHwnd = false;
        if (!FactoryIidSupported(iid, &hasHwnd)) return;     // unknown/private IIDs are delegated, never cast
        void* vt = nullptr;
        if (!SafeRead(iface, &vt, sizeof vt) || !vt) return;
        InstallFactoryCells(reinterpret_cast<void**>(vt), iid, IidName(iid));
    }
    static void ObserveChainInterface(REFIID iid, void* iface) {
        if (!iface) return;
        bool hasP1 = false, hasR1 = false;
        if (!ChainIidSupported(iid, &hasP1, &hasR1)) return;
        void* vt = nullptr;
        if (!SafeRead(iface, &vt, sizeof vt) || !vt) { DelegateReason("chain", "unreadable_vtable", iface); return; }
        InstallChainCells(reinterpret_cast<void**>(vt), iid);
    }
    static void ObserveFactoryGuarded(REFIID iid, void* returned, void* caller) {
        CDK_GUARD_BEGIN ObserveFactory(iid, returned, caller);
        CDK_GUARD_FAIL core::Log("[overlay] %s delegate factory reason observe_fault 0x%08x %p", kMarker, (unsigned)cdk::GuardCode(), returned);
        CDK_GUARD_END
    }
    static void ObserveFactoryInterfaceGuarded(REFIID iid, void* iface) {
        CDK_GUARD_BEGIN ObserveFactoryInterface(iid, iface);
        CDK_GUARD_FAIL core::Log("[overlay] %s delegate factory_qi reason observe_fault 0x%08x %p", kMarker, (unsigned)cdk::GuardCode(), iface);
        CDK_GUARD_END
    }
    static void ObserveChainInterfaceGuarded(REFIID iid, void* iface) {
        CDK_GUARD_BEGIN ObserveChainInterface(iid, iface);
        CDK_GUARD_FAIL core::Log("[overlay] %s delegate chain reason observe_fault 0x%08x %p", kMarker, (unsigned)cdk::GuardCode(), iface);
        CDK_GUARD_END
    }

    // =====================================================================================================
    // Binding cookie: lifetime metadata on the game's own chain (no COM proxy, no chain/factory/backbuffer cycle)
    // =====================================================================================================
    enum CookieState { CookieUnready = 0, CookieReady = 1, CookieRetired = 2 };
    // One owned queue reference plus the immutable identity that validated it. Cookies and the active renderer
    // share leases; copying one under a lock is an internal ownership increment, never a COM call.
    struct QueueLease {
        ID3D12CommandQueue* queue = nullptr;
        void* identity = nullptr;
        QueueLease(ID3D12CommandQueue* q, void* id) : queue(q), identity(id) { if (queue) queue->AddRef(); }
        ~QueueLease() { if (queue) queue->Release(); }
        QueueLease(const QueueLease&) = delete;
        QueueLease& operator=(const QueueLease&) = delete;
    };
    static std::atomic<int> g_generationCounter{0};

    class BindingCookie : public IUnknown {
    public:
        std::atomic<ULONG> refs{1};
        std::atomic<int> state{CookieUnready};
        std::atomic<bool> attachmentLive{false};         // sticky: true only while a live receipt publishes this metadata
        std::atomic<int> generation{0};
        std::atomic<bool> resizing{false};               // visible to the present path while a resize is in flight
        std::atomic<int> associationRevision{0};         // bumped on every committed queue association change
        int depIdx[2] = { -1, -1 };                      // records this cookie was captured against
        unsigned depMask[2] = { 0, 0 };                  // and the coverage each of them must keep publishing
        int depCount = 0;
        HWND hwnd = nullptr;
        std::shared_ptr<const QueueLease> creationLease;  // immutable observation: owns the queue reference
        std::shared_ptr<const QueueLease> renderLease;    // committed association, guarded by g_renderMutex; empty = creation
        unsigned resizeCalls = 0;                         // per-cookie calls inside the global barrier (render mutex)
        bool resizeAmbiguous = false;                     // sticky: overlapping same-cookie mutations are unsupported
        bool renderQueueUnsupported = false;              // guarded by g_renderMutex
        void* queueIdentity = nullptr;                    // canonical IUnknown values: compared, never dereferenced
        void* chainIdentity = nullptr;
        void* deviceIdentity = nullptr;
        UINT width = 0, height = 0, buffers = 0; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
            if (!pp) return E_POINTER;
            *pp = nullptr;
            if (iid == IID_IUnknown || iid == kWbCookieIid) { *pp = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_acq_rel) + 1; }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG n = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (n == 0) Retire();
            return n;
        }
    private:
        void Retire() {
            state.store(CookieRetired, std::memory_order_release);
            attachmentLive.store(false, std::memory_order_release);
            renderLease.reset();                          // lease destructors release each queue reference exactly once
            creationLease.reset();
            core::Log("[overlay] %s cookie retired generation %d hwnd %p", kMarker, generation.load(std::memory_order_relaxed), (void*)hwnd);
            delete this;
        }
    };

    static std::atomic<int> g_renderMutexHolders{0};   // diagnostics only: no lock decision below depends on it
    struct RenderMutexScope {
        std::mutex& m; bool owned;
        explicit RenderMutexScope(std::mutex& mm) : m(mm), owned(mm.try_lock()) {}
        ~RenderMutexScope() { if (owned) { m.unlock(); } }
        RenderMutexScope(const RenderMutexScope&) = delete; RenderMutexScope& operator=(const RenderMutexScope&) = delete;
        bool owns() const { return owned; }
    };
    static std::mutex g_renderMutex;                     // render state serialization (never held across an original call)
    // Patch and render locks are never nested: the registry transaction holds the patch lock over fixed-record
    // reads/writes, safe cell reads and VirtualProtect/CAS/rollback only, and never over foreign calls, module
    // lookups, pins, logging or reference destruction. Qualification work runs before render locking.
    static std::mutex& RenderMutex() { return g_renderMutex; }
    static BindingCookie* g_activeCookie = nullptr;      // owned reference
    static int g_activeGeneration = 0;
    static int g_activeRevision = 0;
    static HWND g_selectedHwnd = nullptr;
    static std::atomic<int> g_activationCount{0};
    static std::atomic<int> g_frameCount{0};

    static bool CanonicalIdentity(IUnknown* obj, IUnknown** out) {   // only IUnknown identity is stable per object
        *out = nullptr;
        if (!obj) return false;
        return SUCCEEDED(obj->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(out))) && *out != nullptr;
    }
    // The owned identity reference is written straight into the caller's owned slot, so a structured fault after a
    // successful QI cannot leak it: the slot owner releases it on every path.
    static bool CanonicalIdentityInto(IUnknown* obj, IUnknown** slot) {
        if (!obj || !slot || *slot) return false;
        return SUCCEEDED(obj->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(slot))) && *slot != nullptr;
    }
    // ---- R5: live receipts keyed by the captured canonical outer identity ----------------------------------
    // The receipt list is an intrusive fixed-capacity table whose short mutex covers only link/unlink and
    // increments of OUR concrete metadata atomic refcount: never a virtual COM call, and never a payload read.
    struct Receipt { void* identity = nullptr; BindingCookie* meta = nullptr; bool live = false; };
    static std::mutex g_receiptMutex;
    static const int kMaxReceipts = kMaxChainVt;   // at least one live receipt per record the registry can publish
    static Receipt g_receipts[kMaxReceipts];
    // Publishes one live receipt and invalidates any older receipt registered for the same identity (a newly
    // observed creation never infers object continuity from address equality). The displaced metadata is marked
    // non-drawable (sticky) under the list mutex and its reference is released after unlocking. Returns false when
    // no live receipt could be published: the capture then fails closed instead of declaring readiness it cannot back.
    static bool PublishReceipt(void* identity, BindingCookie* meta) {
        if (!meta || !identity) return false;
        BindingCookie* displaced[kMaxReceipts]; int displacedCount = 0;
        bool published = false;
        {
            std::lock_guard<std::mutex> l(g_receiptMutex);
            int slot = -1;
            for (int i = 0; i < kMaxReceipts; i++) {                       // an older same-identity receipt is invalidated FIRST
                if (!g_receipts[i].live || g_receipts[i].identity != identity) continue;
                BindingCookie* m = g_receipts[i].meta;
                if (m) { m->attachmentLive.store(false, std::memory_order_release); displaced[displacedCount++] = m; }
                g_receipts[i] = Receipt{};
                slot = i;
            }
            if (slot < 0) for (int i = 0; i < kMaxReceipts; i++) if (!g_receipts[i].live) { slot = i; break; }
            if (slot >= 0) {
                meta->refs.fetch_add(1, std::memory_order_acq_rel);       // our concrete metadata count, no COM call
                g_receipts[slot].identity = identity; g_receipts[slot].meta = meta; g_receipts[slot].live = true;
                meta->attachmentLive.store(true, std::memory_order_release);
                published = true;
            }
        }
        for (int i = 0; i < displacedCount; i++) displaced[i]->Release();   // outside the mutex: may drop the last reference
        return published;
    }
    // Lookup acquires an owned metadata reference from a LIVE receipt; never reads the private-data payload.
    static BindingCookie* FindReceipt(void* identity) {
        if (!identity) return nullptr;
        std::lock_guard<std::mutex> l(g_receiptMutex);
        for (int i = 0; i < kMaxReceipts; i++) if (g_receipts[i].live && g_receipts[i].identity == identity) {
            BindingCookie* meta = g_receipts[i].meta;
            if (meta) meta->refs.fetch_add(1, std::memory_order_acq_rel);
            return meta;
        }
        return nullptr;
    }
    // Unlink returns the receipt's metadata reference for the caller to drop AFTER unlocking. Eligibility ends here
    // and the metadata is marked non-drawable (sticky) under the same mutex: a caller that already took a metadata
    // reference before the unlink is rejected by the render-locked recheck.
    static BindingCookie* UnlinkReceipt(BindingCookie* meta) {
        if (!meta) return nullptr;
        std::lock_guard<std::mutex> l(g_receiptMutex);
        for (int i = 0; i < kMaxReceipts; i++) if (g_receipts[i].live && g_receipts[i].meta == meta) {
            meta->attachmentLive.store(false, std::memory_order_release);
            BindingCookie* m = g_receipts[i].meta;
            g_receipts[i] = Receipt{};
            return m;
        }
        return nullptr;
    }
    // One cleanup release, guarded as a small leaf: a broken COM Release is recorded without skipping the others.
    static void ReleaseGuarded(IUnknown* p) {
        if (!p) return;
        CDK_GUARD_BEGIN p->Release();
        CDK_GUARD_FAIL core::Log("[overlay] %s cleanup release fault 0x%08x %p", kMarker, (unsigned)cdk::GuardCode(), (void*)p);
        CDK_GUARD_END
    }
    // The only object DXGI owns under our GUID. It holds one metadata reference and is never retained by the
    // renderer or a callback; its destruction ends the metadata's eligibility (receipt unlinked under the list
    // mutex) and only then drops that reference.
    class CookieAttachment : public IUnknown {
    public:
        explicit CookieAttachment(BindingCookie* metadata) : meta(metadata) { if (meta) meta->refs.fetch_add(1, std::memory_order_acq_rel); }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
            if (!pp) return E_POINTER;
            *pp = nullptr;
            if (iid == IID_IUnknown) { *pp = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
            return E_NOINTERFACE;                     // never pretends to be the renderer metadata
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_acq_rel) + 1; }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG n = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (n == 0) {
                BindingCookie* m = meta;
                meta = nullptr;
                BindingCookie* receiptRef = UnlinkReceipt(m);   // eligibility ends under the receipt mutex
                if (receiptRef) receiptRef->Release();          // dropped after unlocking
                if (m) m->Release();                            // the attachment's own metadata reference
                delete this;
            }
            return n;
        }
        std::atomic<ULONG> refs{1};
        BindingCookie* meta = nullptr;
    };
    // Every owned reference one capture acquires, released exactly once by the caller's frame (never by the
    // guarded leaf, and never under a patch/render lock).
    struct CaptureSlots {
        ID3D12CommandQueue* queue = nullptr;
        IDXGISwapChain3* chain = nullptr;
        ID3D12Device* qdev = nullptr, *cdev = nullptr;
        IUnknown* qdevId = nullptr; IUnknown* cdevId = nullptr; IUnknown* chainId = nullptr; IUnknown* queueId = nullptr;
        BindingCookie* cookie = nullptr;
        CookieAttachment* attachment = nullptr;
        ~CaptureSlots() {
            if (attachment) ReleaseGuarded(static_cast<IUnknown*>(attachment));
            if (cookie) ReleaseGuarded(static_cast<IUnknown*>(cookie));
            if (chain) ReleaseGuarded(reinterpret_cast<IUnknown*>(chain));
            if (queue) ReleaseGuarded(reinterpret_cast<IUnknown*>(queue));
            if (qdev) ReleaseGuarded(reinterpret_cast<IUnknown*>(qdev));
            if (cdev) ReleaseGuarded(reinterpret_cast<IUnknown*>(cdev));
            if (qdevId) ReleaseGuarded(qdevId);
            if (cdevId) ReleaseGuarded(cdevId);
            if (chainId) ReleaseGuarded(chainId);
            if (queueId) ReleaseGuarded(queueId);
        }
    };
    // Resolves the live caller's canonical identity into the caller-owned slot and returns the metadata reference
    // from a live receipt. The slot keeps the identity reference until the slot owner releases it, so a caught
    // fault after the QI cannot leak it; an already filled slot is reused (one identity per object).
    static BindingCookie* CookieOfChainInto(void* chainSelf, IUnknown** identitySlot) {
        if (!chainSelf || !identitySlot) return nullptr;
        ObserverScope guard;                              // the identity QI is our own qualification work
        if (!*identitySlot && (!CanonicalIdentityInto(reinterpret_cast<IUnknown*>(chainSelf), identitySlot) || !*identitySlot)) return nullptr;
        return FindReceipt(*identitySlot);                // a live receipt, never a payload probe
    }
    static BindingCookie* CookieOfChain(void* chainSelf) {   // convenience lookup for callers without an owned slot frame
        IUnknown* identity = nullptr;
        BindingCookie* cookie = CookieOfChainInto(chainSelf, &identity);
        if (identity) ReleaseGuarded(identity);
        return cookie;
    }
    static bool ProcessWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;     // a real top-level game window, not a child window
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;   // and not an owned popup/dialog
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid == GetCurrentProcessId();
    }

    static bool WaitRenderIdle() {   // drain only our own GPU work; a failed wait never frees in-flight resources
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testDrainFn) return g_testDrainFn();
#endif
        if (!g_fence || !g_queue) return true;
        const UINT64 target = ++g_fenceValue;
        if (FAILED(g_queue->Signal(g_fence, target))) return false;                 // no signal, no claimed drain
        if (g_fence->GetCompletedValue() < target) {
            if (!g_fenceEvent) return false;
            if (FAILED(g_fence->SetEventOnCompletion(target, g_fenceEvent))) return false;
            if (WaitForSingleObject(g_fenceEvent, kDrainTimeoutMs) != WAIT_OBJECT_0) return false;
        }
        return g_fence->GetCompletedValue() >= target;                              // the captured value, not any old signal
    }

    // One completed outer game creation -> one queue/chain pair, observed and paired from the same call.
    static void CaptureCreatedChainBody(CaptureSlots* s, IUnknown* gameQueue, IUnknown* returned, REFIID returnedIid, HWND hwnd, void* caller, CaptureOrigin origin) {
        if (!returned) return;
        char mod[64] = {}; CallerModuleToken(caller, mod, sizeof mod);
        if (gameQueue && SUCCEEDED(gameQueue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&s->queue))) && s->queue) {
            const D3D12_COMMAND_QUEUE_DESC qd = s->queue->GetDesc();
            if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { core::Log("[overlay] %s create queue %p type %d", kMarker, (void*)s->queue, (int)qd.Type); DelegateReason("create", "queue_not_direct", returned); return; }
            if (qd.NodeMask != 0 && qd.NodeMask != 1) { DelegateReason("create", "queue_node_unsupported", returned); return; }
        } else { DelegateReason("create", "no_d3d12_queue", returned); return; }
        if (FAILED(returned->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s->chain))) || !s->chain) { DelegateReason("create", "no_chain3", returned); return; }
        IDXGISwapChain* outerChain = reinterpret_cast<IDXGISwapChain*>(returned);   // the public interface the game presents
        bool ok = SUCCEEDED(s->queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&s->qdev))) && s->qdev &&
                  SUCCEEDED(s->chain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&s->cdev))) && s->cdev;
        if (ok) ok = CanonicalIdentity(s->qdev, &s->qdevId) && s->qdevId && CanonicalIdentity(s->cdev, &s->cdevId) && s->cdevId;
        const bool deviceMatch = ok && s->qdevId == s->cdevId;               // the raw wrapper identity comparison
        if (ok) ok = CanonicalIdentity(returned, &s->chainId) && s->chainId && CanonicalIdentity(s->queue, &s->queueId) && s->queueId;   // returned interface and queue outer identities
        UINT nodeCount = 0;
        if (ok) nodeCount = s->qdev->GetNodeCount();
        // Approved amendment: a device-wrapping interposer exposes one device through different wrapper objects for the queue
        // and the chain, so the raw identity comparison is required only on the factory route. On the game-boundary route the
        // game itself paired this queue with this chain in the verified call. Every other check stays mandatory verbatim.
        const bool deviceAccepted = ok && nodeCount == 1 && (origin == CaptureOrigin::GameBoundary || deviceMatch);
        if (!deviceAccepted) {
            core::Log("[overlay] %s delegate create reason %s origin %s qdev %p qdev_id %p cdev %p cdev_id %p chain_id %p queue_id %p nodes %u device_match %d boundary_attested %d", kMarker,
                      !ok ? "identity_failed" : (nodeCount != 1 ? "device_not_single_node" : "identity_failed"),
                      origin == CaptureOrigin::GameBoundary ? "game_boundary" : "factory",
                      (void*)s->qdev, (void*)s->qdevId, (void*)s->cdev, (void*)s->cdevId, (void*)s->chainId, (void*)s->queueId, nodeCount,
                      deviceMatch ? 1 : 0, origin == CaptureOrigin::GameBoundary ? 1 : 0);
            return;
        }
        DXGI_SWAP_CHAIN_DESC sd = {};
        HWND chainHwnd = hwnd;
        const bool descOk = SUCCEEDED(outerChain->GetDesc(&sd));
        if (descOk && sd.OutputWindow) chainHwnd = sd.OutputWindow;
        const bool hwndOk = ProcessWindow(chainHwnd);
        const UINT width = descOk ? sd.BufferDesc.Width : 0, height = descOk ? sd.BufferDesc.Height : 0;
        if (!descOk || width <= 64 || height <= 64 || !hwndOk || sd.BufferCount == 0) {
            DelegateReason("create", !descOk ? "desc_failed" : (!hwndOk ? "hwnd_invalid" : "desc_dimensions"), returned);
            return;
        }
        // Existence-only query: no payload buffer, no assumed type. Any entry under our GUID (or any error other
        // than NOT_FOUND) declines the capture, so a foreign blob is never read, released or overwritten.
        UINT existenceSize = 0;
        const HRESULT existenceHr = reinterpret_cast<IDXGIObject*>(returned)->GetPrivateData(kWbCookieGuid, &existenceSize, nullptr);
        if (existenceHr != DXGI_ERROR_NOT_FOUND) {
            core::Log("[overlay] %s delegate create reason private_entry_exists chain %p hr 0x%08x", kMarker, returned, (unsigned)existenceHr);
            return;
        }
        s->cookie = new (std::nothrow) BindingCookie();
        if (!s->cookie) { DelegateReason("create", "cookie_alloc_failed", returned); return; }
        s->attachment = new (std::nothrow) CookieAttachment(s->cookie);
        if (!s->attachment) { DelegateReason("create", "cookie_alloc_failed", returned); return; }
        s->cookie->generation.store(g_generationCounter.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release);
        s->cookie->hwnd = chainHwnd;
        s->cookie->creationLease.reset(new QueueLease(s->queue, s->queueId));   // the lease owns the cookie's queue reference
        s->cookie->queueIdentity = s->queueId; s->cookie->chainIdentity = s->chainId; s->cookie->deviceIdentity = s->qdevId;   // raw values, no extra references
        s->cookie->width = width; s->cookie->height = height; s->cookie->buffers = sd.BufferCount; s->cookie->format = sd.BufferDesc.Format;
        // DXGI alone retains the attachment after this call returns; the capture keeps no attachment reference of its own.
        const HRESULT setHr = reinterpret_cast<IDXGIObject*>(returned)->SetPrivateDataInterface(kWbCookieGuid, static_cast<IUnknown*>(s->attachment));
        const int gen = s->cookie->generation.load(std::memory_order_relaxed);
        bool slotsReady = false;
        if (SUCCEEDED(setHr)) {
            ObserveChainInterface(returnedIid, returned);                        // the exact returned public interface
            ObserveChainInterface(__uuidof(IDXGISwapChain3), s->chain);          // supported public alias
            const unsigned reqReturned = ChainMaskForIid(returnedIid);
            const unsigned reqChain3 = ChainMaskForIid(__uuidof(IDXGISwapChain3));
            const int ridx = FindChainVt(*reinterpret_cast<void***>(returned));
            const int cidx = FindChainVt(*reinterpret_cast<void***>(s->chain));
            if (ridx == cidx) {                                                  // one table: require the union of both observed prefixes
                slotsReady = ridx >= 0 && RecordReadyFrom("create", ridx, false, reqReturned | reqChain3);
                if (slotsReady) { s->cookie->depIdx[0] = ridx; s->cookie->depMask[0] = reqReturned | reqChain3; s->cookie->depCount = 1; }
            } else {
                const bool rOk = ridx >= 0 && RecordReadyFrom("create", ridx, false, reqReturned);
                const bool cOk = cidx >= 0 && RecordReadyFrom("create", cidx, false, reqChain3);
                slotsReady = rOk && cOk;
                if (slotsReady) { s->cookie->depIdx[0] = ridx; s->cookie->depMask[0] = reqReturned; s->cookie->depIdx[1] = cidx; s->cookie->depMask[1] = reqChain3; s->cookie->depCount = 2; }
            }
            const char* createFail = nullptr;
            if (slotsReady) {
                if (PublishReceipt(s->chainId, s->cookie)) s->cookie->state.store(CookieReady, std::memory_order_release);   // ready only after identity, attachment, coverage and a published live receipt
                else { slotsReady = false; createFail = "receipt_unavailable"; }                    // fail closed: no readiness without a lookupable receipt
            } else createFail = "slots_unavailable";
            if (createFail) core::Log("[overlay] %s delegate create reason %s chain %p generation %d", kMarker, createFail, returned, gen);
            core::Log("[overlay] %s pair generation %d chain %p identity %p queue %p device_match %d boundary_attested %d hwnd %p ready %d desc %ux%u buffers %u caller_module %s", kMarker,
                      gen, returned, (void*)s->chainId, s->queue, deviceMatch ? 1 : 0, origin == CaptureOrigin::GameBoundary ? 1 : 0,
                      (void*)chainHwnd, slotsReady ? 1 : 0, width, height, sd.BufferCount, mod);
        } else core::Log("[overlay] %s delegate create reason cookie_rejected chain %p hr 0x%08x", kMarker, returned, (unsigned)setHr);
        // Every reference acquired above is released exactly once by the caller's slot frame; the receipt and the
        // attachment hold their own metadata references, so an unready or rejected cookie is harmless.
    }
    static void CaptureCreatedChainGuarded(CaptureSlots* s, IUnknown* gameQueue, IUnknown* returned, REFIID returnedIid, HWND hwnd, void* caller, CaptureOrigin origin) {
        CDK_GUARD_BEGIN CaptureCreatedChainBody(s, gameQueue, returned, returnedIid, hwnd, caller, origin);
        CDK_GUARD_FAIL core::Log("[overlay] %s delegate create reason capture_fault 0x%08x %p", kMarker, (unsigned)cdk::GuardCode(), returned);
        CDK_GUARD_END
    }

    // =====================================================================================================
    // Present / resize routing. Renderer state is only ever written here, under the render mutex.
    // =====================================================================================================
    static std::shared_ptr<const QueueLease> g_activeLease;   // owns the reference behind the borrowed g_queue alias
    static unsigned g_resizeCalls = 0;                        // one-viewport barrier; written under g_renderMutex only
    // One guarded release leaf for a shared queue lease: a broken COM release is recorded without skipping the rest.
    static void ReleaseLeaseGuarded(std::shared_ptr<const QueueLease>* lease) {
        if (!lease || !*lease) return;
        CDK_GUARD_BEGIN lease->reset();
        CDK_GUARD_FAIL core::Log("[overlay] %s cleanup release fault lease", kMarker);
        CDK_GUARD_END
    }
    // Owners displaced from render-locked state: destructors (queue/metadata release) run after unlocking.
    struct DisplacedOwners {
        BindingCookie* cookie = nullptr;
        std::shared_ptr<const QueueLease> lease;
        ~DisplacedOwners() { ReleaseLeaseGuarded(&lease); if (cookie) ReleaseGuarded(static_cast<IUnknown*>(cookie)); }
    };
    // Adopts the cookie's committed association: the lease is copied (no COM work) and g_queue becomes the borrowed
    // alias the active lease backs. The displaced owners leave the lock inside `displaced`.
    static void AdoptCookie(BindingCookie* cookie, DisplacedOwners* displaced) {   // render mutex held
        std::shared_ptr<const QueueLease> lease = cookie->renderLease ? cookie->renderLease : cookie->creationLease;
        if (!lease) return;
        if (displaced) displaced->lease = g_activeLease; else g_activeLease.reset();
        g_activeLease = lease;
        g_queue = lease->queue;                           // borrowed: the lease owns the reference
        cookie->AddRef();
        if (displaced) displaced->cookie = g_activeCookie; else if (g_activeCookie) g_activeCookie->Release();
        g_activeCookie = cookie;
        g_activeGeneration = cookie->generation.load(std::memory_order_relaxed);
        g_activeRevision = cookie->associationRevision.load(std::memory_order_relaxed);
        g_forceRebind = true;                             // generation-aware: an equal address must rebind too
        g_activationCount.fetch_add(1, std::memory_order_relaxed);
        core::Log("[overlay] %s activate generation %d outer_present 1 queue %p hwnd %p", kMarker, g_activeGeneration, (void*)lease->queue, (void*)cookie->hwnd);
    }

    // The record a cookie was captured against and the coverage it must keep publishing (0 = no dependency).
    static unsigned CookieRequires(BindingCookie* cookie, int idx) {
        for (int i = 0; i < cookie->depCount; i++) if (cookie->depIdx[i] == idx) return cookie->depMask[i];
        return 0;
    }
    // EVERY captured dependency must still be a published Ready record whose required owned cells still hold our
    // typed thunks, plus the actual entry. A returned-interface Present is refused when the other alias's requirement
    // (e.g. its ResizeBuffers1 coverage) was lost, even though that alias is not the presenting table. No patch lock
    // is taken on this path (it may run under the render lock).
    static bool AllDependenciesReady(const char* what, BindingCookie* cookie, int entryIdx, unsigned entryMask) {
        if (entryIdx < 0 || entryMask == 0 || !RecordReadyFrom(what, entryIdx, false, entryMask)) return false;
        for (int i = 0; i < cookie->depCount; i++) {
            if (cookie->depIdx[i] < 0 || cookie->depMask[i] == 0) continue;
            if (!RecordReadyFrom(what, cookie->depIdx[i], false, cookie->depMask[i])) return false;
        }
        return true;
    }
    // Present added-work frame: every owned reference the qualification acquires is written straight into these slots
    // by guarded leaves and released exactly once after unlocking. The frame outlives every leaf, so a caught fault
    // cannot leak an acquired reference.
    struct PresentSlots {
        IUnknown* identity = nullptr;                 // canonical identity acquired for the receipt lookup
        BindingCookie* cookie = nullptr;              // owned metadata reference taken from the live receipt
        IDXGISwapChain3* chain = nullptr;             // owned chain3 reference taken for the draw
        bool eligible = false;
        ~PresentSlots() {
            if (chain) ReleaseGuarded(reinterpret_cast<IUnknown*>(chain));
            if (cookie) ReleaseGuarded(static_cast<IUnknown*>(cookie));
            if (identity) ReleaseGuarded(identity);
        }
    };
    static void PresentQualifyBody(PresentSlots* s, void* chainSelf, void* caller, bool observedAtEntry) {
        if (!chainSelf || !CallerIsMainImage(caller) || observedAtEntry) return;
        int idx = -1;
        if (!ChainRecordFor(chainSelf, &idx)) { DelegateReason("present", "unknown_table", chainSelf); return; }
        BindingCookie* cookie = CookieOfChainInto(chainSelf, &s->identity);   // the identity reference lives in the frame
        if (!cookie) { DelegateReason("present", "no_cookie", chainSelf); return; }
        s->cookie = cookie;                                                   // the frame owns it from here on
        const unsigned required = CookieRequires(cookie, idx);                // no dependency for this record: this cookie is not this object's
        if (!required) { DelegateReason("present", "identity_mismatch", chainSelf); return; }
        // The object must still carry our GUID entry: no payload is read, but a removed/overwritten entry stops routing.
        UINT liveEntry = 0;
        if (reinterpret_cast<IDXGIObject*>(chainSelf)->GetPrivateData(kWbCookieGuid, &liveEntry, nullptr) != S_OK) {
            DelegateReason("present", "attachment_gone", chainSelf);
            return;
        }
        if (!AllDependenciesReady("present", cookie, idx, required)) { DelegateReason("present", "requirements_lost", chainSelf); return; }
        if (cookie->state.load(std::memory_order_acquire) != CookieReady) { DelegateReason("present", "cookie_not_ready", chainSelf); return; }
        if (!cookie->attachmentLive.load(std::memory_order_acquire)) { DelegateReason("present", "attachment_retired", chainSelf); return; }
        s->eligible = true;
    }
    static void PresentQualifyGuarded(PresentSlots* s, void* chainSelf, void* caller, bool observedAtEntry) {
        CDK_GUARD_BEGIN PresentQualifyBody(s, chainSelf, caller, observedAtEntry);
        CDK_GUARD_FAIL g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate present reason qualify_fault 0x%08x %p stop 1", kMarker, (unsigned)cdk::GuardCode(), chainSelf);
        CDK_GUARD_END
    }
    static void PresentChainQiGuarded(PresentSlots* s, void* self) {   // runs under the caller's ObserverScope
        CDK_GUARD_BEGIN if (FAILED(reinterpret_cast<IUnknown*>(self)->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s->chain))) || !s->chain) DelegateReason("present", "no_chain3", self);
        CDK_GUARD_FAIL g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate present reason chain_qualify_fault 0x%08x %p stop 1", kMarker, (unsigned)cdk::GuardCode(), self);
        CDK_GUARD_END
    }
    // Every gate is rechecked here on the state that is actually locked: a qualification made before a Begin must
    // not authorize an activation after it, and a qualification whose attachment retired in between is refused.
    static bool PresentActivateLocked(BindingCookie* cookie, void* self, DisplacedOwners* displaced) {
        if (g_overlayStop.load(std::memory_order_acquire) || g_disabled || g_failed) return false;
        if (g_resizeCalls != 0) { DelegateReason("present", "resize_barrier", self); return false; }
        if (cookie->resizing.load(std::memory_order_acquire) || cookie->resizeAmbiguous) { DelegateReason("present", "cookie_ineligible", self); return false; }
        if (!g_selectedHwnd) g_selectedHwnd = cookie->hwnd;                // one viewport: latched on an eligible Present
        if (cookie->hwnd != g_selectedHwnd || cookie->renderQueueUnsupported) { DelegateReason("present", "unselected_hwnd", self); return false; }
        const int gen = cookie->generation.load(std::memory_order_relaxed);
        const int assoc = cookie->associationRevision.load(std::memory_order_relaxed);
        if (gen < g_activeGeneration) { DelegateReason("present", "stale_generation", self); return false; }   // an older generation never re-activates
        // The qualification may be arbitrarily old by now: metadata readiness and the attachment that authorized it
        // are rechecked immediately before activation, on the state that is actually locked.
        if (cookie->state.load(std::memory_order_acquire) != CookieReady) { DelegateReason("present", "cookie_not_ready", self); return false; }
        if (!cookie->attachmentLive.load(std::memory_order_acquire)) { DelegateReason("present", "attachment_retired", self); return false; }
        int idx = -1;
        if (!ChainRecordFor(self, &idx)) return false;
        const unsigned required = CookieRequires(cookie, idx);
        if (!required || !AllDependenciesReady("present", cookie, idx, required)) return false;   // every captured dependency + this entry, rechecked under the lock
        if (g_activeCookie != cookie || g_activeGeneration != gen || g_activeRevision != assoc) {   // a new queue association re-adopts
            if ((g_activeCookie != nullptr || g_activeLease != nullptr) && !WaitRenderIdle()) {
                g_disabled = true;
                core::Log("[overlay] %s delegate present reason gpu_drain_failed chain %p", kMarker, self);
                return false;
            }
            AdoptCookie(cookie, displaced);
        }
        return true;
    }
    static void PresentLockedBody(PresentSlots* s, void* self, DisplacedOwners* displaced) {
        if (!PresentActivateLocked(s->cookie, self, displaced)) return;
        ObserverScope guard;                              // the locked renderer's own incidental QIs never start observation
        g_frameCount.fetch_add(1, std::memory_order_relaxed);
        OverlayFrame(s->chain);
    }
    static void PresentLockedGuarded(PresentSlots* s, void* self, DisplacedOwners* displaced) {
        CDK_GUARD_BEGIN PresentLockedBody(s, self, displaced);
        CDK_GUARD_FAIL g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate present reason locked_fault 0x%08x %p stop 1", kMarker, (unsigned)cdk::GuardCode(), self);
        CDK_GUARD_END
    }
    static void PresentBoundChainBody(IDXGISwapChain* self, UINT flags, void* caller) {
        if (!self || (flags & DXGI_PRESENT_TEST)) return;                 // TEST presents never draw
        const bool observedAtEntry = ObserverGuardActive();               // a nested WB probe present is never an outer game present
        ObserverScope observer;                                           // declared before the cleanup frame: its releases stay under observation
        PresentSlots slots;
        PresentQualifyGuarded(&slots, self, caller, observedAtEntry);
        if (!slots.eligible) return;
        PresentChainQiGuarded(&slots, self);
        if (!slots.chain) return;
        DisplacedOwners displaced;
        {
            RenderMutexScope lk(RenderMutex());
            if (!lk.owns()) DelegateReason("present", "render_busy", self);
            else PresentLockedGuarded(&slots, self, &displaced);
        }   // every foreign reference and displaced owner dies here, after unlocking
    }
    // Added work only: the original Present is delegated outside this catcher, exactly once.
    static void PresentBoundChain(IDXGISwapChain* self, UINT flags, void* caller) {
        CDK_GUARD_BEGIN PresentBoundChainBody(self, flags, caller);
        CDK_GUARD_FAIL g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate present reason added_work_fault 0x%08x %p stop 1", kMarker, (unsigned)cdk::GuardCode(), self);
        CDK_GUARD_END
    }

    // One stack-owned transaction per resize call (never a TLS owner slot, never a lifecycle mutex). Begin raises
    // the global barrier, drains the existing frame and returns; the original is forwarded once, unchanged, outside
    // every overlay mutex; Finish re-locks (blocking: completion is not optional), commits or reports, and closes
    // its own ticket exactly once. An abandoned entered transaction closes as Unknown, never as success.
    struct ResizeTxn {
        BindingCookie* cookie = nullptr;                  // owned reference, retained until the ticket is accounted
        IUnknown* identityKeep = nullptr;                 // owned canonical identity reference (slot closed on every path)
        void* identity = nullptr; bool identityKnown = false;
        bool entered = false, finished = false;
        bool faulted = false;                             // an added-work fault was caught: only the terminal close runs
        bool ticketClosed = false;                        // the terminal close accounted the counters exactly once
        bool buffers1 = false;
        bool resultKnown = false;
        HRESULT result = E_FAIL;                          // Unknown until the original returned
        std::shared_ptr<const QueueLease> validated;      // prepared replacement association (successful call only)
        bool validationUnspecified = false;
        const char* reason = nullptr;
        ~ResizeTxn();
    };
    // Every reference the array validation acquires is written into this caller-owned frame as soon as it exists:
    // a caught fault leaves the release to the frame owner's destructor (guarded single release, outside locks).
    struct ResizeValidation {
        ID3D12Device* chainDev = nullptr;                  // per-validation slots
        ID3D12Device* queueDev = nullptr;
        IUnknown* queueDevId = nullptr;
        ID3D12CommandQueue* candidate = nullptr;
        IUnknown* candidateId = nullptr;
        ID3D12CommandQueue* first = nullptr;               // the proposed association (owned until transferred)
        IUnknown* firstQueueId = nullptr;
        bool unsupported = false;
        const char* reason = nullptr;
        ~ResizeValidation() {
            if (firstQueueId) ReleaseGuarded(firstQueueId);
            if (first) ReleaseGuarded(reinterpret_cast<IUnknown*>(first));
            if (chainDev) ReleaseGuarded(reinterpret_cast<IUnknown*>(chainDev));
            ResizeValidationIterationClose(this);
        }
    };
    // One guarded single release per transaction slot, outside every lock, under ObserverScope (declared first).
    static void ResizeTxnCloseSlots(ResizeTxn* tx) {
        ObserverScope guard;
        ReleaseLeaseGuarded(&tx->validated);
        if (tx->identityKeep) { ReleaseGuarded(tx->identityKeep); tx->identityKeep = nullptr; tx->identity = nullptr; tx->identityKnown = false; }
        if (tx->cookie) { ReleaseGuarded(static_cast<IUnknown*>(tx->cookie)); tx->cookie = nullptr; }
    }
    // A caught added-work fault is represented explicitly: the sticky stop is set here and the transaction is
    // closed exactly once by ResizeTxnFaultCloseTerminal. The cookie slot stays owned until its counters are
    // accounted, so neither the normal continuation nor the destructor retries an unsafe blocking close.
    static void ResizeTxnMarkFaulted(ResizeTxn* tx, const char* reason, const void* chain, unsigned code, bool validationUnspecified) {
        tx->faulted = true;
        if (validationUnspecified) tx->validationUnspecified = true;
        g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate resize reason %s 0x%08x %p stop 1", kMarker, reason, code, chain);
    }
    // Accounts one raised ticket under the render mutex; the cookie slot must still be owned by the transaction.
    static void ResizeTxnAccountTicket(ResizeTxn* tx, bool unsupported) {
        if (tx->cookie) {
            if (unsupported) tx->cookie->renderQueueUnsupported = true;             // a faulted association is never exposed
            --tx->cookie->resizeCalls;
            tx->cookie->resizing.store(tx->cookie->resizeCalls != 0, std::memory_order_release);
        }
        --g_resizeCalls;
        tx->finished = true;
    }
    // Terminal close of a faulted transaction: exactly once, after this thread has released every render-lock scope,
    // never from inside a catcher and never while this thread owns the render mutex, so the blocking acquisition
    // cannot self-deadlock and no later Finish is attempted.
    static void ResizeTxnFaultCloseTerminal(ResizeTxn* tx) {
        if (!tx->faulted || tx->ticketClosed) return;
        {
            std::unique_lock<std::mutex> l(g_renderMutex);
            if (tx->entered && !tx->finished) ResizeTxnAccountTicket(tx, true);
            tx->ticketClosed = true;
        }
        core::Log("[overlay] %s delegate resize reason ticket_closed_after_fault entered %d finished %d stop 1", kMarker, tx->entered ? 1 : 0, tx->finished ? 1 : 0);
        ResizeTxnCloseSlots(tx);
    }
    // Qualification leaf: owned cookie + live outer identity into the transaction's slots; no state lock, no render lock.
    static void ResizeBeginQualify(ResizeTxn* tx, void* self, bool buffers1) {
        ObserverScope guard;
        if (CanonicalIdentityInto(reinterpret_cast<IUnknown*>(self), &tx->identityKeep) && tx->identityKeep) {
            tx->identity = tx->identityKeep; tx->identityKnown = true;   // kept alive for the transaction, released outside locks
        }
        int idx = -1;
        if (ChainRecordFor(self, &idx)) {
            BindingCookie* cookie = CookieOfChainInto(self, &tx->identityKeep);   // the same canonical identity, no second QI
            if (cookie) {
                tx->cookie = cookie;                                             // owned slot: closed on every exit path
                const unsigned required = CookieRequires(cookie, idx);
                if (!required || !RecordReadyFrom("resize", idx, false, required | (buffers1 ? kCbResize1 : 0))) {
                    tx->cookie = nullptr;                                        // not eligible: no ticket for this transaction
                    ReleaseGuarded(static_cast<IUnknown*>(cookie));
                }
            }
        }
    }
    static void ResizeBeginQualifyGuarded(ResizeTxn* tx, void* self, bool buffers1) {
        CDK_GUARD_BEGIN ResizeBeginQualify(tx, self, buffers1);
        CDK_GUARD_FAIL ResizeTxnMarkFaulted(tx, "resize_begin_fault", self, (unsigned)cdk::GuardCode(), false);
        CDK_GUARD_END
    }
    // Locked begin body: raise/observe the barrier under render state. The leaf itself owns no lock.
    static void ResizeBeginLockedBody(ResizeTxn* tx, void* self) {
        ++g_resizeCalls;                                   // every hooked resize participates (unselected chains included)
        tx->entered = true;
        if (tx->cookie) {
            if (tx->cookie->resizeCalls != 0) tx->cookie->resizeAmbiguous = true;   // same-cookie overlap: unsupported and sticky
            ++tx->cookie->resizeCalls;
            tx->cookie->resizing.store(true, std::memory_order_release);            // observation derived from the counter
        }
        if (g_resizeCalls == 1) {                          // first Begin: no WB frame runs, so the existing GPU work is drained here
            if (!WaitRenderIdle()) {
                g_overlayStop.store(true, std::memory_order_release);               // unsupported, never a claimed drain
                core::Log("[overlay] %s delegate resize reason gpu_drain_failed chain %p stop 1 resources_kept 1", kMarker, self);
            }
        }
        if (tx->buffers1) core::Log("[overlay] %s resize1 begin chain %p", kMarker, self);
    }
    static void ResizeBeginLockedGuarded(ResizeTxn* tx, void* self) {
        CDK_GUARD_BEGIN ResizeBeginLockedBody(tx, self);
        CDK_GUARD_FAIL ResizeTxnMarkFaulted(tx, "resize_begin_locked_fault", self, (unsigned)cdk::GuardCode(), false);
        CDK_GUARD_END
    }
    // Begin: the render lock is owned by THIS function, outside every catcher (the Present locked-wrapper pattern),
    // so a caught structured fault can never leave the mutex owned or skip its release.
    static void ResizeBeginTxn(ResizeTxn* tx, void* self, bool buffers1) {
        tx->buffers1 = buffers1;
        ResizeBeginQualifyGuarded(tx, self, buffers1);
        {
            std::unique_lock<std::mutex> l(g_renderMutex);   // blocking: the barrier is not optional
            ResizeBeginLockedGuarded(tx, self);
        }
    }
    // Prepare: the validation frame belongs to this function, outside the guarded leaves, so a caught fault or an
    // allocation failure still releases every acquired reference (the frame releases it after publication or failure).
    static void ResizePrepareTxn(ResizeTxn* tx, void* self, HRESULT hr, UINT bufferCount, const UINT* nodes, IUnknown* const* queues) {
        tx->result = hr; tx->resultKnown = true;
        if (FAILED(hr) || !tx->cookie) return;             // a failure reads no queue array and queries no replacement association
        if (!tx->buffers1) return;
        ObserverScope guard;
        ResizeValidation v;                                // caller-owned frame: created before the guarded leaf
        ResizeValidateGuarded(&v, self, tx->cookie, bufferCount, nodes, queues);
        tx->reason = v.reason;
        if (v.unsupported || !v.first) { tx->validationUnspecified = true; return; }   // the frame releases every acquired reference
        try {
            // The frame still owns the qualification's queue reference here; make_shared takes its own reference and
            // the frame releases that reference when this function returns. v.first is never moved into a raw local.
            tx->validated = std::make_shared<const QueueLease>(v.first, v.firstQueueId);
        } catch (...) {
            ResizeTxnMarkFaulted(tx, "lease_alloc_failed", self, 0, true);             // fail closed: no association is exposed
        }
    }
    // Locked finish body: commit/report under render state (the lock is owned by ResizeFinishTxn, outside this leaf).
    static void ResizeFinishLockedBody(ResizeTxn* tx, bool abandoned, DisplacedOwners* displaced) {   // render mutex held
        if (abandoned) tx->resultKnown = false;            // an abandoned transaction closes as Unknown, never as success
        BindingCookie* cookie = tx->cookie;
        if (cookie) {
            if (!tx->resultKnown || cookie->resizeAmbiguous) {
                cookie->renderQueueUnsupported = true;                       // never expose an association after an unknown/overlapping mutation
                core::Log("[overlay] %s delegate resize reason %s chain %p", kMarker,
                          !tx->resultKnown ? "resize_result_unknown" : "resize_overlap_ambiguous", (void*)cookie->hwnd);
            } else if (SUCCEEDED(tx->result)) {
                if (tx->buffers1 && (!tx->validated || tx->validationUnspecified)) {
                    cookie->renderQueueUnsupported = true;
                    core::Log("[overlay] %s delegate resize1 reason %s chain %p", kMarker, tx->reason ? tx->reason : "queue_association_unspecified", (void*)cookie->hwnd);
                } else {
                    if (tx->buffers1) { displaced->lease = cookie->renderLease; cookie->renderLease = tx->validated; }   // transfer under the lock
                    cookie->associationRevision.fetch_add(1, std::memory_order_release);   // every successful resize invalidates the binding revision
                    if (g_activeCookie == cookie) g_forceRebind = true;                    // the next eligible Present rebuilds
                    core::Log("[overlay] %s resize1 commit chain %p queue %p revision %d", kMarker, (void*)cookie->hwnd,
                              (void*)(tx->validated ? tx->validated->queue : nullptr), cookie->associationRevision.load(std::memory_order_relaxed));
                }
            } else {
                core::Log("[overlay] %s resize failed hr 0x%08x chain %p resources_kept 1", kMarker, (unsigned)tx->result, (void*)cookie->hwnd);   // the old binding and dirty state stay
            }
            --cookie->resizeCalls;
            cookie->resizing.store(cookie->resizeCalls != 0, std::memory_order_release);
        } else {
            // unresolved lookup: only a successful/unknown call whose identity is the active one (or unestablished) stops participation
            const bool risky = !tx->resultKnown || SUCCEEDED(tx->result);
            if (risky && (!tx->identityKnown || (g_activeCookie && g_activeCookie->chainIdentity == tx->identity))) {
                g_overlayStop.store(true, std::memory_order_release);
                core::Log("[overlay] %s delegate resize reason unresolved_cookie stop 1", kMarker);
            }
        }
        --g_resizeCalls;
        tx->finished = true;
    }
    static void ResizeFinishLockedGuarded(ResizeTxn* tx, bool abandoned, DisplacedOwners* displaced) {
        CDK_GUARD_BEGIN ResizeFinishLockedBody(tx, abandoned, displaced);
        CDK_GUARD_FAIL ResizeTxnMarkFaulted(tx, "resize_finish_fault", nullptr, (unsigned)cdk::GuardCode(), true);
        CDK_GUARD_END
    }
    static void ResizeFinishTxn(ResizeTxn* tx, bool abandoned) {
        if (!tx->entered || tx->finished || tx->faulted) return;   // a faulted transaction is closed only terminally
        DisplacedOwners displaced;
        {
            std::unique_lock<std::mutex> l(g_renderMutex);   // blocking: a commit is never dropped on contention
            ResizeFinishLockedGuarded(tx, abandoned, &displaced);
        }
        if (tx->faulted) return;                             // the terminal close retains and accounts the cookie slot
        ResizeTxnCloseSlots(tx);
    }
    ResizeTxn::~ResizeTxn() {
        if (faulted) ResizeTxnFaultCloseTerminal(this);      // idempotent terminal close; never re-enters a blocking Finish
        else if (entered && !finished) ResizeFinishTxn(this, true);   // abandoned: closes as Unknown, never as success
        ResizeTxnCloseSlots(this);                           // any slot a faulted/unentered transaction still owns
    }
    static void ResizeValidationIterationClose(ResizeValidation* v) {   // the per-iteration slots, guarded single release
        if (v->candidateId) { ReleaseGuarded(v->candidateId); v->candidateId = nullptr; }
        if (v->candidate) { ReleaseGuarded(reinterpret_cast<IUnknown*>(v->candidate)); v->candidate = nullptr; }
        if (v->queueDevId) { ReleaseGuarded(v->queueDevId); v->queueDevId = nullptr; }
        if (v->queueDev) { ReleaseGuarded(reinterpret_cast<IUnknown*>(v->queueDev)); v->queueDev = nullptr; }
    }
    static void ResizeValidateGuarded(ResizeValidation* v, void* self, BindingCookie* cookie, UINT bufferCount, const UINT* nodes, IUnknown* const* queues) {
        CDK_GUARD_BEGIN AnalyzeResizeBuffers1(v, self, cookie, bufferCount, nodes, queues);
        CDK_GUARD_FAIL v->unsupported = true; v->reason = "validation_fault"; g_overlayStop.store(true, std::memory_order_release);
        core::Log("[overlay] %s delegate resize reason validation_fault 0x%08x stop 1", kMarker, (unsigned)cdk::GuardCode());
        CDK_GUARD_END
    }
    // The single-DIRECT-queue/single-node renderer only accepts an explicit, uniform queue association. A null
    // array is not read as "kept": the cookie is marked unsupported instead of guessing a queue.
    static void AnalyzeResizeBuffers1(ResizeValidation* v, void* self, BindingCookie* cookie, UINT bufferCount, const UINT* nodes, IUnknown* const* queues) {
        if (!queues) { v->unsupported = true; v->reason = "queue_association_unspecified"; return; }
        DXGI_SWAP_CHAIN_DESC sd = {};
        IDXGISwapChain* chain = static_cast<IDXGISwapChain*>(self);
        if (FAILED(chain->GetDesc(&sd)) || sd.BufferCount == 0) { v->unsupported = true; v->reason = "queue_association_unspecified"; return; }
        const UINT effective = bufferCount ? bufferCount : sd.BufferCount;   // the call's count; zero means "keep the existing count"
        if (effective == 0 || effective > 16) { v->unsupported = true; v->reason = "queue_association_unspecified"; return; }
        if (FAILED(chain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&v->chainDev))) || !v->chainDev) { v->unsupported = true; v->reason = "device_mismatch"; return; }
        if (v->chainDev->GetNodeCount() != 1) { v->unsupported = true; v->reason = "device_not_single_node"; return; }
        for (UINT i = 0; i < effective && !v->unsupported; i++) {
            if (nodes && nodes[i] != 1) { v->unsupported = true; v->reason = "node_unsupported"; break; }
            if (!queues[i]) { v->unsupported = true; v->reason = "queue_association_unspecified"; break; }
            if (FAILED(queues[i]->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&v->candidate))) || !v->candidate) { v->unsupported = true; v->reason = "queue_association_mixed"; break; }
            const D3D12_COMMAND_QUEUE_DESC qdesc = v->candidate->GetDesc();
            if (qdesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { core::Log("[overlay] %s resize1 queue %p type %d", kMarker, (void*)v->candidate, (int)qdesc.Type); v->unsupported = true; v->reason = "queue_not_direct"; break; }
            const bool devOk = SUCCEEDED(v->candidate->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&v->queueDev))) && v->queueDev && CanonicalIdentityInto(v->queueDev, &v->queueDevId) && v->queueDevId;
            if (!devOk || (cookie->deviceIdentity && v->queueDevId != cookie->deviceIdentity)) { v->unsupported = true; v->reason = "device_mismatch"; break; }
            if (!CanonicalIdentityInto(v->candidate, &v->candidateId) || !v->candidateId) { v->unsupported = true; v->reason = "device_mismatch"; break; }
            if (!v->first) { v->first = v->candidate; v->firstQueueId = v->candidateId; v->candidate = nullptr; v->candidateId = nullptr; }
            else if (v->candidateId != v->firstQueueId) { v->unsupported = true; v->reason = "queue_association_mixed"; break; }   // the same queue object, not merely the same device
            ResizeValidationIterationClose(v);
        }
        ResizeValidationIterationClose(v);   // after the loop too: a break must not leave this iteration's references owned
    }

    // =====================================================================================================
    // Main-module import walker: named imports only, bounded, validated before any access.
    // =====================================================================================================
    struct ImageInfo { uintptr_t base = 0; size_t size = 0; };
    // The bound is the mapped image range, not a fixed ceiling: a real main module (the shipped game image is
    // 0x173ab000 = 389 722 112 bytes) is accepted, while an unreadable or unmapped range is not.
    static bool ImageMapped(ImageInfo img) {
        if (!img.base || !img.size) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(img.base), &mbi, sizeof mbi) != sizeof mbi) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xff;
        if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) return false;
        if (mbi.AllocationBase != reinterpret_cast<void*>(img.base)) return false;   // the image is its own allocation, not a run of adjacent readable regions
        size_t checked = 0;
        uintptr_t at = img.base;
        while (checked < img.size) {                                          // every page of the claim must be committed in that same allocation
            MEMORY_BASIC_INFORMATION part = {};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &part, sizeof part) != sizeof part) return false;
            if (part.State != MEM_COMMIT) return false;
            if (part.AllocationBase != reinterpret_cast<void*>(img.base)) return false;   // a different allocation ends the image
            const DWORD p2 = part.Protect & 0xff;
            if (p2 == PAGE_NOACCESS || p2 == PAGE_EXECUTE) return false;
            if (!part.RegionSize) return false;
            checked += part.RegionSize;
            at += part.RegionSize;
        }
        return checked >= img.size;
    }
    static bool ImageBounds(ImageInfo* img) {
        const uintptr_t base = g_os.mainImageBase ? g_os.mainImageBase() : 0;
        if (!base) return false;
        IMAGE_DOS_HEADER dos = {};
        if (!core::ReadBytes(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) { core::Log("[overlay] %s delegate install reason image_reject dos", kMarker); return false; }
        if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000) { core::Log("[overlay] %s delegate install reason image_reject lfanew %ld", kMarker, (long)dos.e_lfanew); return false; }
        IMAGE_NT_HEADERS64 nt = {};
        if (!core::ReadBytes(base + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof nt)) { core::Log("[overlay] %s delegate install reason image_reject nt_read", kMarker); return false; }
        if (nt.Signature != IMAGE_NT_SIGNATURE) { core::Log("[overlay] %s delegate install reason image_reject signature 0x%08x", kMarker, (unsigned)nt.Signature); return false; }
        if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { core::Log("[overlay] %s delegate install reason image_reject machine 0x%04x", kMarker, (unsigned)nt.FileHeader.Machine); return false; }
        if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { core::Log("[overlay] %s delegate install reason image_reject magic 0x%04x", kMarker, (unsigned)nt.OptionalHeader.Magic); return false; }
        if (nt.OptionalHeader.SizeOfImage < 0x1000) { core::Log("[overlay] %s delegate install reason image_reject size 0x%x", kMarker, (unsigned)nt.OptionalHeader.SizeOfImage); return false; }
        if (nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || (nt.FileHeader.SizeOfOptionalHeader % 8) != 0) { core::Log("[overlay] %s delegate install reason image_reject optional_header %u", kMarker, (unsigned)nt.FileHeader.SizeOfOptionalHeader); return false; }
        const uintptr_t headerEnd = static_cast<uintptr_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
        if (nt.OptionalHeader.SizeOfHeaders < headerEnd || nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) { core::Log("[overlay] %s delegate install reason image_reject headers 0x%x end 0x%llx", kMarker, (unsigned)nt.OptionalHeader.SizeOfHeaders, (unsigned long long)headerEnd); return false; }
        if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) { core::Log("[overlay] %s delegate install reason image_reject rva_count %u", kMarker, (unsigned)nt.OptionalHeader.NumberOfRvaAndSizes); return false; }
        const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        // The PE format imposes no alignment on the import-directory RVA (the shipped image's is 0x170d322e, mod 4 == 2)
        // and every read below goes through core::ReadBytes, which is a byte copy: bounds are enforced, alignment is not.
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
            dir.VirtualAddress > nt.OptionalHeader.SizeOfImage ||
            dir.Size > nt.OptionalHeader.SizeOfImage - dir.VirtualAddress) { core::Log("[overlay] %s delegate install reason image_reject import_dir 0x%x/0x%x size 0x%x", kMarker, (unsigned)dir.VirtualAddress, (unsigned)dir.Size, (unsigned)nt.OptionalHeader.SizeOfImage); return false; }
        if (nt.OptionalHeader.SectionAlignment != 0x1000 || nt.OptionalHeader.FileAlignment != 0x200) { core::Log("[overlay] %s delegate install reason image_reject align 0x%x/0x%x", kMarker, (unsigned)nt.OptionalHeader.SectionAlignment, (unsigned)nt.OptionalHeader.FileAlignment); return false; }
        // Section headers are read from the mapped image (never from a local header copy), bounds-checked per entry.
        if (nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96) { core::Log("[overlay] %s delegate install reason image_reject sections %u", kMarker, (unsigned)nt.FileHeader.NumberOfSections); return false; }
        const uintptr_t secBase = static_cast<uintptr_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        const size_t secSpan = static_cast<size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        // The section table's own offset is read with byte copies as well: only its bounds matter, not its alignment.
        if (secBase > nt.OptionalHeader.SizeOfHeaders || secSpan > nt.OptionalHeader.SizeOfHeaders - secBase) { core::Log("[overlay] %s delegate install reason image_reject section_array 0x%llx span %llu headers 0x%x", kMarker, (unsigned long long)secBase, (unsigned long long)secSpan, (unsigned)nt.OptionalHeader.SizeOfHeaders); return false; }
        bool haveText = false, haveContaining = false;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!core::ReadBytes(base + secBase + static_cast<uintptr_t>(i) * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) { core::Log("[overlay] %s delegate install reason image_reject section_read %u", kMarker, i); return false; }
            const uintptr_t lo = sc.VirtualAddress;
            const uintptr_t hi = static_cast<uintptr_t>(sc.VirtualAddress) + sc.Misc.VirtualSize;
            if (hi > nt.OptionalHeader.SizeOfImage) { core::Log("[overlay] %s delegate install reason image_reject section_range 0x%x+0x%x image 0x%x", kMarker, (unsigned)sc.VirtualAddress, (unsigned)sc.Misc.VirtualSize, (unsigned)nt.OptionalHeader.SizeOfImage); return false; }
            if ((sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) haveText = true;
            if (dir.VirtualAddress >= lo && dir.VirtualAddress < hi) haveContaining = true;
        }
        if (!haveText || !haveContaining) { core::Log("[overlay] %s delegate install reason image_reject text %d containing %d", kMarker, haveText ? 1 : 0, haveContaining ? 1 : 0); return false; }
        img->base = base; img->size = nt.OptionalHeader.SizeOfImage;
        if (!ImageMapped(*img)) { core::Log("[overlay] %s delegate install reason image_range_unmapped base %p size 0x%llx", kMarker, (void*)base, (unsigned long long)img->size); return false; }
        return true;
    }
    static bool ImageRange(const ImageInfo& img, uintptr_t off, size_t n) {   // RVA + length inside the mapped image, no unsigned wrap
        return off <= img.size && n <= img.size - off;
    }
    static bool ImageRead(const ImageInfo& img, uintptr_t off, void* out, size_t n) {
        if (!ImageRange(img, off, n)) return false;                  // bounds before access
        return core::ReadBytes(img.base + off, out, n);
    }
    static bool ImageCString(const ImageInfo& img, uintptr_t off, char* out, size_t cap) {
        if (!out || cap < 2 || off >= img.size) return false;
        for (size_t i = 0; i + 1 < cap; i++) {
            char c = 0;
            if (!ImageRead(img, off + i, &c, 1)) return false;
            out[i] = c;
            if (!c) return i != 0;
        }
        out[cap - 1] = 0;
        return false;                                       // no terminator inside the bound: not a name we accept
    }
    struct TapStats { int factoryCreate = 0, factoryCreate1 = 0, factoryCreate2 = 0, resolver = 0, descriptors = 0, scanned = 0, conflicts = 0, protectFail = 0, restoreFail = 0, unresolved = 0; };
    static bool TapNamedCell(void** cell, void* savedExpected, int flavor, int origin, HMODULE provider, const char* name, TapStats* st, const char* what) {
        if (!savedExpected) { st->unresolved++; core::Log("[overlay] %s delegate tap %s reason unresolved_import %p", kMarker, what, (void*)cell); return false; }
        if (!PinTarget(savedExpected)) { st->unresolved++; core::Log("[overlay] %s delegate tap %s reason pin_failed target %p", kMarker, what, savedExpected); return false; }
        const int idx = AcquireFnRec(savedExpected, flavor, origin, provider, name);
        if (idx < 0) { core::Log("[overlay] %s delegate tap %s reason capacity_exhausted", kMarker, what); return false; }
        void* thunk = flavor == 2 ? g_fnThunkC[idx] : g_fnThunkA[idx];
        const TapResult tr = TapCellEx(cell, savedExpected, thunk);
        if (tr.wrotePointer && tr.restoredProtection) { core::Log("[overlay] %s tap %s cell %p saved %p thunk %p dispatch %d provider %p result owned", kMarker, what, (void*)cell, savedExpected, thunk, idx, (void*)provider); return true; }
        if (tr.wrotePointer) { st->restoreFail++; core::Log("[overlay] %s delegate tap %s reason protection_restore_failed cell %p", kMarker, what, (void*)cell); return false; }
        if (tr.madeWritable) { st->conflicts++; core::Log("[overlay] %s delegate tap %s reason cas_conflict cell %p expected %p found %p", kMarker, what, (void*)cell, savedExpected, tr.observedPointer); return false; }
        st->protectFail++; core::Log("[overlay] %s delegate tap %s reason protection_failed cell %p", kMarker, what, (void*)cell);
        return false;
    }
    // Exact import-provider identity for KERNEL32's loader exports. The observed game image imports GetProcAddress
    // from KERNEL32.dll; a PE may legitimately import it through the documented api-ms-win-core-libraryloader
    // API set instead, so that documented family is accepted in addition to the exact observed module name.
    static bool KernelLoaderModule(const char* module) {
        if (StrEqI(module, "kernel32.dll")) return true;
        return StartsWithI(module, "api-ms-win-core-libraryloader-");
    }
    static void InstallImportTaps() {
        ImageInfo img = {};
        if (!ImageBounds(&img)) { core::Log("[overlay] %s install reason image_unavailable pid %lu", kMarker, (unsigned long)GetCurrentProcessId()); return; }
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) { core::Log("[overlay] %s install reason image_reject dos pid %lu", kMarker, (unsigned long)GetCurrentProcessId()); return; }
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) { core::Log("[overlay] %s install reason image_reject nt pid %lu", kMarker, (unsigned long)GetCurrentProcessId()); return; }
        const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) { core::Log("[overlay] %s install reason no_import_directory pid %lu", kMarker, (unsigned long)GetCurrentProcessId()); return; }
        TapStats st;
        const uintptr_t dirOff = dir.VirtualAddress;
        const int descriptorCount = static_cast<int>(dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR));   // the declared directory length bounds the walk
        for (int d = 0; d < descriptorCount; d++) {
            const uintptr_t off = dirOff + static_cast<uintptr_t>(d) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            IMAGE_IMPORT_DESCRIPTOR desc = {};
            if (!ImageRead(img, off, &desc, sizeof desc)) break;
            if (!desc.Name && !desc.FirstThunk) break;                       // the table ends with an all-zero descriptor
            st.descriptors++;
            char dllName[64] = {};
            const bool named = ImageCString(img, desc.Name, dllName, sizeof dllName);
            if (!named) core::Log("[overlay] %s delegate tap reason module_name_unreadable descriptor %d", kMarker, d);
            if (!desc.FirstThunk) continue;
            if ((desc.FirstThunk % sizeof(void*)) != 0) { core::Log("[overlay] %s delegate tap reason iat_misaligned rva 0x%x descriptor %d", kMarker, (unsigned)desc.FirstThunk, d); continue; }
            const bool namedTable = desc.OriginalFirstThunk != 0 && (desc.OriginalFirstThunk % sizeof(uint64_t)) == 0;
            if (desc.OriginalFirstThunk && !namedTable) core::Log("[overlay] %s delegate tap reason int_misaligned rva 0x%x descriptor %d", kMarker, (unsigned)desc.OriginalFirstThunk, d);
            HMODULE provider = nullptr;
            for (int t = 0; t < 4096; t++) {
                const uintptr_t cellOff = static_cast<uintptr_t>(desc.FirstThunk) + static_cast<uintptr_t>(t) * sizeof(void*);
                if (!ImageRange(img, cellOff, sizeof(void*))) break;
                void* saved = nullptr;
                if (!ImageRead(img, cellOff, &saved, sizeof saved)) break;
                if (!saved) break;
                st.scanned++;
                if (!namedTable) continue;                                    // without a name table there is nothing named to match
                const uintptr_t intOff = static_cast<uintptr_t>(desc.OriginalFirstThunk) + static_cast<uintptr_t>(t) * sizeof(uint64_t);
                if (!ImageRange(img, intOff, sizeof(uint64_t))) break;
                uint64_t first = 0;
                if (!ImageRead(img, intOff, &first, sizeof first)) break;
                if (!first) break;
                if (first & IMAGE_ORDINAL_FLAG64) continue;                  // ordinal entries are never treated as strings
                if (first > img.size || img.size - static_cast<size_t>(first) < 3) { st.unresolved++; continue; }   // the hint word plus a name must be inside the image
                char importName[64] = {};
                if (!ImageCString(img, static_cast<uintptr_t>(first) + 2, importName, sizeof importName)) { st.unresolved++; continue; }
                void** cell = reinterpret_cast<void**>(img.base + cellOff);
                if ((reinterpret_cast<uintptr_t>(cell) % alignof(void*)) != 0) {   // never a CAS on an unaligned cell address
                    core::Log("[overlay] %s delegate tap reason unaligned_cell cell %p descriptor %d", kMarker, (void*)cell, d);
                    continue;
                }
                if (!provider && g_os.moduleFromAddress) provider = g_os.moduleFromAddress(reinterpret_cast<const void*>(saved));
                if (StrEq(importName, "CreateDXGIFactory2") || StrEq(importName, "CreateDXGIFactory1") || StrEq(importName, "CreateDXGIFactory")) {
                    char ownerName[64] = {};
                    HMODULE ownerMod = g_os.moduleFromAddress ? g_os.moduleFromAddress(reinterpret_cast<const void*>(saved)) : nullptr;
                    if (g_os.moduleFileName && ownerMod) g_os.moduleFileName(ownerMod, ownerName, sizeof ownerName);
                    if (!StrEqI(dllName, "dxgi.dll")) { st.unresolved++; core::Log("[overlay] %s delegate tap %s reason dll_identity module %s cell %p", kMarker, importName, dllName, (void*)cell); }
                    else if (!StrEqI(dllName, ownerName)) { st.unresolved++; core::Log("[overlay] %s delegate tap %s reason provider_mismatch import %s owner %s cell %p", kMarker, importName, dllName, ownerName, (void*)cell); }
                    else if (StrEq(importName, "CreateDXGIFactory2")) { if (TapNamedCell(cell, saved, 2, 1, provider, "CreateDXGIFactory2", &st, "factory2")) st.factoryCreate2++; }
                    else if (StrEq(importName, "CreateDXGIFactory1")) { if (TapNamedCell(cell, saved, 1, 1, provider, "CreateDXGIFactory1", &st, "factory1")) st.factoryCreate1++; }
                    else { if (TapNamedCell(cell, saved, 1, 1, provider, "CreateDXGIFactory", &st, "factory0")) st.factoryCreate++; }
                }
                else if (StrEq(importName, "GetProcAddress") && named) {
                    if (!KernelLoaderModule(dllName)) { st.unresolved++; core::Log("[overlay] %s delegate tap %s reason resolver_module_identity module %s cell %p", kMarker, importName, dllName, (void*)cell); continue; }
                    void* expected = g_resolverSaved.load(std::memory_order_acquire);
                    if (!expected) {
                        if (!PinTarget(saved)) { st.unresolved++; core::Log("[overlay] %s delegate tap resolver reason pin_failed target %p", kMarker, saved); continue; }
                        std::lock_guard<std::mutex> l(g_patchMutex);
                        if (g_resolverSaved.load(std::memory_order_acquire) == nullptr) g_resolverSaved.store(saved, std::memory_order_release);
                        expected = g_resolverSaved.load(std::memory_order_acquire);
                    }
                    if (expected == saved) {
                        const TapResult tr = TapCellEx(cell, saved, reinterpret_cast<void*>(&HkGetProcAddress));
                        if (tr.wrotePointer && tr.restoredProtection) { st.resolver++; core::Log("[overlay] %s tap resolver cell %p saved %p result owned provider %p", kMarker, (void*)cell, saved, (void*)provider); }
                        else if (tr.wrotePointer) core::Log("[overlay] %s delegate tap resolver reason protection_restore_failed cell %p", kMarker, (void*)cell);
                        else if (tr.madeWritable) core::Log("[overlay] %s delegate tap resolver reason cas_conflict cell %p found %p", kMarker, (void*)cell, tr.observedPointer);
                        else core::Log("[overlay] %s delegate tap resolver reason protection_failed cell %p", kMarker, (void*)cell);
                    } else core::Log("[overlay] %s delegate tap resolver reason distinct_resolver cell %p", kMarker, (void*)cell);
                }
            }
        }
        core::Log("[overlay] %s install pid %lu method game_import_taps factory_create %s factory_create1 %s factory_create2 %s resolver %s descriptors %d imports_scanned %d conflicts %d protect_fail %d restore_fail %d unresolved %d",
                  kMarker, (unsigned long)GetCurrentProcessId(), st.factoryCreate ? "owned" : "absent", st.factoryCreate1 ? "owned" : "absent",
                  st.factoryCreate2 ? "owned" : "absent", st.resolver ? "owned" : "absent", st.descriptors, st.scanned, st.conflicts, st.protectFail, st.restoreFail, st.unresolved);
    }

    static bool g_selfPinned = false;
    static bool PinSelf() {                              // the ASI itself: no safe hot unload exists in this plugin
        HMODULE self = nullptr;
        const bool ok = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                           reinterpret_cast<LPCSTR>(&Install), &self) && self != nullptr;
        if (!ok) core::Log("[overlay] %s delegate install reason pin_failed module self", kMarker);
        else g_selfPinned = true;
        return ok;
    }
    static bool PinMainModule() {                        // the image whose imports are tapped, for process lifetime
        if (!g_mainModule || !g_os.pinModule) return false;
        const bool ok = g_os.pinModule(g_mainModule) == g_mainModule;
        if (!ok) core::Log("[overlay] %s delegate install reason pin_failed module main %p", kMarker, (void*)g_mainModule);
        return ok;
    }
    // Pin the exact owner of a retained target (table cell, delegated function or resolved export). Returning false
    // is a prerequisite failure, not a warning: callers decline that record instead of publishing it.
    static bool PinTarget(void* addr) {
        if (!addr || !g_os.moduleFromAddress || !g_os.pinModule) return false;
        HMODULE mod = g_os.moduleFromAddress(addr);
        if (!mod) return false;
        return g_os.pinModule(mod) == mod;
    }

    // =====================================================================================================
    // Game-side creation boundary (v3). The game dispatches IDXGIFactory2::CreateSwapChainForHwnd (slot 15) from
    // exactly one place in its own code - a vtable-resident SwapChain helper. Head-detouring that helper makes the
    // game the caller by construction: no foreign (ReShade/SL wrapper) factory vtable cell is ever written, and the
    // wrapper factory, the game's queue, the HWND, the description and the returned wrapper swapchain are all read
    // from the game's own members. Resolution uses the repo conventions (referenced-string anchor -> PE unwind
    // function start, with the head signature as a cross-check); every byte is verified before the detour is
    // enabled, and any mismatch leaves this route uninstalled (fail closed, no hardcoded RVA).
    // =====================================================================================================
    static const char kGameBoundaryString[] = "SwapChain::CreateSwapChainForHwnd failed: %d";
    static const char kGameBoundaryHeadSig[] =
        "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B F9 4C 8B 79 40";
    static const char kGameBoundarySiteSig[] =
        "49 8B 9F 40 09 00 00 48 8B 03 4C 8B 60 78 48 8B 4D 78 48 85 C9 74 0D 48 8B 01 C5 F8 77";
    static const unsigned char kGameBoundaryHead16[16] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    struct GameBoundary {
        void* target = nullptr;          // resolved helper head inside the main image
        void* original = nullptr;        // trampoline the detour delegates through
        bool resolved = false;
        bool installed = false;
        std::atomic<long> calls{0}, captures{0};
    };
    static GameBoundary g_gameBoundary;
    static thread_local int t_gameBoundaryDepth = 0;
    typedef intptr_t(STDMETHODCALLTYPE* GameSwapChainInit_t)(void* self);
    struct GameBoundaryInputs { IUnknown* queue = nullptr; HWND hwnd = nullptr; };
    static void GameBoundaryReadInputs(void* self, GameBoundaryInputs* in);   // reads the game's own members (below)
    static IUnknown* GameBoundaryReadChain(void* self);
    static intptr_t STDMETHODCALLTYPE GameSwapChainInitDetour(void* self) {
        const bool outer = (t_gameBoundaryDepth == 0);
        if (!outer) return g_gameBoundary.original ? reinterpret_cast<GameSwapChainInit_t>(g_gameBoundary.original)(self) : 0;
        ++t_gameBoundaryDepth;
        GameBoundaryInputs in;
        if (self) GameBoundaryReadInputs(self, &in);
        const intptr_t hr = g_gameBoundary.original ? reinterpret_cast<GameSwapChainInit_t>(g_gameBoundary.original)(self) : 0;   // the original helper, exactly once
        g_gameBoundary.calls.fetch_add(1, std::memory_order_relaxed);
        if (self && SUCCEEDED(static_cast<HRESULT>(hr))) {
            IUnknown* chain = GameBoundaryReadChain(self);   // the wrapper swapchain the game stored (this+0xa8), after the call
            if (chain) {
                ObserverScope guard;                          // our own qualification QIs never start observation
                CaptureSlots slots;
                core::Log("[overlay] %s game_boundary capture chain %p queue %p hwnd %p", kMarker, (void*)chain, (void*)in.queue, (void*)in.hwnd);
                CaptureCreatedChainGuarded(&slots, in.queue, chain, kIidSwapChain4, in.hwnd, _ReturnAddress(), CaptureOrigin::GameBoundary);
                // One line that names whether the hand-over completed, with the queue that was actually paired: the
                // specific decline reason (no_d3d12_queue / identity_failed / private_entry_exists / slots_unavailable /
                // receipt_unavailable / cookie_rejected / capture_fault) is logged immediately above by the body.
                core::Log("[overlay] %s game_boundary capture outcome %s chain %p queue %p cookie_state %d", kMarker,
                          (slots.cookie && slots.cookie->state.load(std::memory_order_acquire) == CookieReady) ? "bound" : "declined",
                          (void*)chain, (void*)in.queue, slots.cookie ? slots.cookie->state.load(std::memory_order_acquire) : -1);
                g_gameBoundary.captures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        --t_gameBoundaryDepth;
        return hr;
    }
    static uintptr_t GameBoundaryScan(const ImageInfo& img, bool executable, const unsigned char* val, const unsigned char* mask, int nlen, int* count, bool* complete);
    static uintptr_t GameBoundaryFuncStart(const ImageInfo& img, uintptr_t rva);
    static uintptr_t GameBoundaryFuncReferencingString(const ImageInfo& img, const char* s, bool* complete);
    static bool InstallGameBoundaryDetour(void* target, void* detour, void** original);
    // Pattern-string -> value/mask bytes for GameBoundaryScan (wildcards match any byte).
    static int GameBoundaryParsePattern(const char* pat, unsigned char* val, unsigned char* mask) {
        int n = 0;
        for (const char* s = pat; *s && n < 64;) {
            while (*s == ' ') s++;
            if (!*s) break;
            if (*s == '?') { val[n] = 0; mask[n] = 0; n++; while (*s == '?') s++; }
            else { val[n] = (unsigned char)strtoul(s, const_cast<char**>(&s), 16); mask[n] = 0xFF; n++; }
        }
        return n;
    }
    static void InstallGameBoundary() {
        ImageInfo img = {};
        if (!ImageBounds(&img)) { core::Log("[overlay] %s delegate game_boundary reason game_boundary_unavailable", kMarker); return; }
        unsigned char hv[64] = {}, hm[64] = {}, sv[64] = {}, sm[64] = {};
        const int hn = GameBoundaryParsePattern(kGameBoundaryHeadSig, hv, hm);
        const int sn = GameBoundaryParsePattern(kGameBoundarySiteSig, sv, sm);
        int headHits = 0, siteHits = 0; bool anchorComplete = false, headComplete = false, siteComplete = false;
        const uintptr_t head = GameBoundaryFuncReferencingString(img, kGameBoundaryString, &anchorComplete);
        const uintptr_t sigHead = GameBoundaryScan(img, true, hv, hm, hn, &headHits, &headComplete);
        const uintptr_t sigSite = GameBoundaryScan(img, true, sv, sm, sn, &siteHits, &siteComplete);
        if (!anchorComplete || !headComplete || !siteComplete) {
            core::Log("[overlay] %s delegate game_boundary reason game_boundary_scan_incomplete anchor %d head %d site %d", kMarker, anchorComplete ? 1 : 0, headComplete ? 1 : 0, siteComplete ? 1 : 0);
            return;                                                           // a partial scan can never prove a unique anchor
        }
        if (!head && headHits == 1) core::Log("[overlay] %s game_boundary reason string_anchor_absent signature_head 1", kMarker);
        const uintptr_t target = head ? head : (headHits == 1 ? sigHead : 0);
        if (!target || headHits != 1 || sigHead != target || siteHits != 1 ||
            !sigSite || GameBoundaryFuncStart(img, sigSite) != target) {
            core::Log("[overlay] %s delegate game_boundary reason game_boundary_resolve_failed head %p sig_head %p/%d site %p/%d", kMarker, (void*)target, (void*)sigHead, headHits, (void*)sigSite, siteHits);
            return;                                                           // fail closed: every required anchor must agree (no partial proof)
        }
        unsigned char pro[16] = {};
        if (!ImageRead(img, target, pro, sizeof pro) || memcmp(pro, kGameBoundaryHead16, sizeof pro) != 0) {
            core::Log("[overlay] %s delegate game_boundary reason game_boundary_prologue_mismatch head %p", kMarker, (void*)target);
            return;
        }
        if (!PinTarget(reinterpret_cast<void*>(img.base + target))) { core::Log("[overlay] %s delegate game_boundary reason game_boundary_pin_failed head %p", kMarker, (void*)target); return; }
        g_gameBoundary.target = reinterpret_cast<void*>(img.base + target);
        g_gameBoundary.resolved = true;
        if (!InstallGameBoundaryDetour(g_gameBoundary.target, reinterpret_cast<void*>(&GameSwapChainInitDetour), &g_gameBoundary.original) || !g_gameBoundary.original) {
            core::Log("[overlay] %s delegate game_boundary reason game_boundary_hook_failed head %p", kMarker, g_gameBoundary.target);
            g_gameBoundary.original = nullptr;
            return;
        }
        g_gameBoundary.installed = true;
        core::Log("[overlay] %s game_boundary installed head %p site %p calls 0 captures 0", kMarker, g_gameBoundary.target, (void*)(img.base + sigSite));
    }

    // One bounded, chunked scan over the sections that carry the requested access. The needle is a byte pattern with
    // optional wildcards; every read goes through ImageRead/ImageRange, so a malformed image cannot make us read out
    // of the mapping, and the scan is O(image) with 64 KiB copies rather than per-byte guarded reads.
    static uintptr_t GameBoundaryScan(const ImageInfo& img, bool executable, const unsigned char* val, const unsigned char* mask, int nlen, int* count, bool* complete) {
        *count = 0; uintptr_t first = 0;
        if (complete) *complete = false;   // only set true after every planned read succeeded
        if (!nlen || nlen > 64) return 0;
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
        const uintptr_t secBase = (uintptr_t)dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        std::vector<unsigned char> buf(0x10000);
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!ImageRead(img, secBase + (uintptr_t)i * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) return first;   // incomplete
            const bool want = executable ? ((sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) : ((sc.Characteristics & IMAGE_SCN_MEM_READ) != 0);
            if (!want || !sc.Misc.VirtualSize || sc.VirtualAddress >= img.size) continue;
            const uintptr_t span = (sc.Misc.VirtualSize > img.size - sc.VirtualAddress) ? (img.size - sc.VirtualAddress) : sc.Misc.VirtualSize;
            for (uintptr_t base = 0; base + (uintptr_t)nlen <= span; ) {
                const size_t want64 = (size_t)((span - base) < buf.size() ? (span - base) : buf.size());
                if (!ImageRead(img, sc.VirtualAddress + base, buf.data(), want64)) return first;   // incomplete: never claim uniqueness
                if (want64 >= (size_t)nlen) {
                    for (size_t k = 0; k + (size_t)nlen <= want64; k++) {
                        bool ok = true;
                        for (int j = 0; j < nlen; j++) if ((buf[k + (size_t)j] & mask[j]) != (val[j] & mask[j])) { ok = false; break; }
                        if (!ok) continue;
                        (*count)++;
                        if (!first) first = sc.VirtualAddress + base + k;
                    }
                }
                if (want64 < buf.size()) break;
                base += want64 - (uintptr_t)(nlen - 1);   // overlap so a needle across the chunk boundary is found
            }
        }
        if (complete) *complete = true;   // every planned read succeeded
        return first;
    }
    // Exact-byte scan (the referenced string): mask all ones.
    static uintptr_t GameBoundaryScanExact(const ImageInfo& img, bool executable, const unsigned char* bytes, int nlen, int* count, bool* complete) {
        unsigned char mask[64];
        for (int i = 0; i < nlen && i < 64; i++) mask[i] = 0xFF;
        return GameBoundaryScan(img, executable, bytes, mask, nlen, count, complete);
    }
    // Function start for an RVA inside a function, via the PE exception directory (chain info followed, bounded).
    static uintptr_t GameBoundaryFuncStart(const ImageInfo& img, uintptr_t rva) {
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos)) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt)) return 0;
        const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) return 0;
        const size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
        size_t lo = 0, hi = n;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            RUNTIME_FUNCTION rf = {};
            if (!ImageRead(img, dir.VirtualAddress + (uintptr_t)mid * sizeof(RUNTIME_FUNCTION), &rf, sizeof rf)) return 0;
            if (rf.BeginAddress <= rva) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return 0;
        RUNTIME_FUNCTION cur = {};
        if (!ImageRead(img, dir.VirtualAddress + (uintptr_t)(lo - 1) * sizeof(RUNTIME_FUNCTION), &cur, sizeof cur)) return 0;
        if (!(cur.BeginAddress <= rva && rva < cur.EndAddress)) return 0;
        uintptr_t seen[16] = {}; int seenCount = 0;
        int level = 0;
        for (;;) {
            unsigned char ui[4] = {};
            if (!ImageRead(img, cur.UnwindInfoAddress, ui, sizeof ui)) return 0;   // unreadable unwind info: fail closed
            const unsigned flags = ui[0] >> 3;
            // winnt.h: UNW_FLAG_EHANDLER 0x1, UNW_FLAG_UHANDLER 0x2, UNW_FLAG_CHAININFO 0x4. Only chain info carries a
            // linked RUNTIME_FUNCTION right after the unwind codes; a handler payload is never parsed as one.
            const unsigned kUnwFlagChainInfo = 0x4;
            if (!(flags & kUnwFlagChainInfo)) break;                              // a read, non-chained record is the head
            if (level >= 16) return 0;                                            // depth budget exhausted with another link pending: fail closed
            RUNTIME_FUNCTION nxt = {};
            const uintptr_t off = cur.UnwindInfoAddress + 4 + ((uintptr_t)(ui[2] + 1) & ~(uintptr_t)1) * 2;
            if (!ImageRead(img, off, &nxt, sizeof nxt)) return 0;                 // truncated chain: fail closed
            if (nxt.BeginAddress >= nxt.EndAddress || nxt.EndAddress > img.size) return 0;   // invalid linked entry
            for (int j = 0; j < seenCount; j++) if (seen[j] == nxt.BeginAddress) return 0;   // cyclic chain: fail closed
            if (seenCount < 16) seen[seenCount++] = cur.BeginAddress;
            cur = nxt;
            ++level;
        }
        return cur.BeginAddress;
    }
    // Finds `lea r64,[rip+disp32]` instructions referencing the string and returns the containing function head.
    static uintptr_t GameBoundaryFuncReferencingString(const ImageInfo& img, const char* s, bool* complete) {
        if (complete) *complete = false;
        const int len = (int)strlen(s) + 1;
        std::vector<unsigned char> needle((size_t)len);
        for (int i = 0; i < len; i++) needle[(size_t)i] = (unsigned char)s[i];
        int strHits = 0; bool strComplete = false;
        const uintptr_t str = GameBoundaryScanExact(img, false, needle.data(), len, &strHits, &strComplete);
        if (!strComplete) return 0;                                           // a partial scan cannot prove uniqueness
        if (!str || strHits != 1) { if (complete) *complete = true; return 0; }   // a completed scan without a unique anchor is an absence, not a read error
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos)) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt)) return 0;
        const uintptr_t secBase = (uintptr_t)dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        std::vector<unsigned char> buf(0x10000);
        uintptr_t found = 0; int refs = 0;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!ImageRead(img, secBase + (uintptr_t)i * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) return 0;   // unreadable section header: the reference scan is incomplete
            if (!(sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sc.Misc.VirtualSize || sc.VirtualAddress >= img.size) continue;
            const uintptr_t span = (sc.Misc.VirtualSize > img.size - sc.VirtualAddress) ? (img.size - sc.VirtualAddress) : sc.Misc.VirtualSize;
            for (uintptr_t base = 0; base + 7 <= span; ) {
                const size_t want64 = (size_t)((span - base) < buf.size() ? (span - base) : buf.size());
                if (!ImageRead(img, sc.VirtualAddress + base, buf.data(), want64)) return 0;   // incomplete reference scan
                for (size_t k = 0; k + 7 <= want64; k++) {
                    if (buf[k] != 0x48 && buf[k] != 0x4C) continue;
                    if (buf[k + 1] != 0x8D || (buf[k + 2] & 0xC7) != 0x05) continue;   // lea reg, [rip+disp32]
                    int32_t disp = 0;
                    memcpy(&disp, &buf[k + 3], 4);
                    const uintptr_t at = sc.VirtualAddress + base + k;
                    if (at + 7 + (intptr_t)disp == str) { refs++; if (!found) found = GameBoundaryFuncStart(img, at); }
                }
                if (want64 < buf.size()) break;
                base += want64 - 6;
            }
        }
        if (complete) *complete = true;
        return (refs == 1) ? found : 0;                                       // exactly one reference, or the anchor is rejected
    }
    // MinHook is the repo's hooking convention (cdmodkit.cpp initialises it on the same init worker before Install).
    static bool InstallGameBoundaryDetour(void* target, void* detour, void** original) {
        if (MH_CreateHook(target, detour, original) != MH_OK) return false;
        if (MH_EnableHook(target) != MH_OK) { *original = nullptr; return false; }
        return true;
    }
    // Boundary inputs read from the game's own member layout (offsets are observations for build 1.0.0.2944, verified
    // offline; the values themselves are what the game passes, so no vtable cell has to be owned to obtain them).
    static void GameBoundaryReadInputs(void* self, GameBoundaryInputs* in) {
        in->queue = nullptr; in->hwnd = nullptr;
        void* qctx = nullptr;
        if (SafeRead(reinterpret_cast<char*>(self) + 0x38, &qctx, sizeof qctx) && qctx) {
            void* queue = nullptr;
            if (SafeRead(reinterpret_cast<char*>(qctx) + 0x3e8, &queue, sizeof queue) && queue) in->queue = reinterpret_cast<IUnknown*>(queue);
        }
        HWND hwnd = nullptr;
        if (SafeRead(reinterpret_cast<char*>(self) + 0x8, &hwnd, sizeof hwnd)) in->hwnd = hwnd;
    }
    static IUnknown* GameBoundaryReadChain(void* self) {   // this+0xa8: the wrapper swapchain the game stored, borrowed
        void* chain = nullptr;
        if (!self || !SafeRead(reinterpret_cast<char*>(self) + 0xa8, &chain, sizeof chain) || !chain) return nullptr;
        return reinterpret_cast<IUnknown*>(chain);
    }

    // =====================================================================================================
    // Install: on the existing init worker, after MinHook init, before the game creates its swapchain.
    // =====================================================================================================
    void Install() {
        g_mainModule = g_os.mainImageBase ? reinterpret_cast<HMODULE>(g_os.mainImageBase()) : nullptr;
        PublishThunkTables();
        if (!PinSelf() || !PinMainModule()) { core::Log("[overlay] %s install reason pin_prerequisite_failed pid %lu", kMarker, (unsigned long)GetCurrentProcessId()); return; }
        if (!g_os.virtualProtect || !g_os.moduleFromAddress || !g_os.mainImageBase()) { core::Log("[overlay] %s install reason os_seam_unavailable", kMarker); return; }
        InstallImportTaps();
        InstallGameBoundary();
    }


    static bool RendererReady(IDXGISwapChain3* sc) {
        if (g_failed || g_disabled || g_overlayStop.load(std::memory_order_acquire) || !g_queue) return false;
        if (!g_ready) {
#ifdef WB_OVERLAY_BINDING_TEST
            if (g_testFrameSink) return true;                              // host tests do not execute real GPU init
#endif
            if (Init(sc)) g_ready = true;
            else { g_failed = true; core::Log("[overlay] init failed; overlay disabled"); return false; }
        }
        return true;
    }
    static void RendererDraw(IDXGISwapChain3* sc) {
#ifdef WB_OVERLAY_BINDING_TEST
        if (g_testFrameSink) {
            if (!EnsureRenderBinding(sc)) return;                                            // the rebind policy runs for real
            g_testFrameSink(sc, g_queue, g_activeGeneration);
            return;
        }
#endif
        RenderGuarded(sc);
    }
    static bool EnsureRenderBinding(IDXGISwapChain3* sc) {
        if (!(g_forceRebind || sc != g_swapChain)) return true;
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {   // an uninitialized renderer adopts the chain's device
            if (g_device && dev != g_device) { core::Log("[overlay] swapchain belongs to a different device; overlay disabled"); dev->Release(); g_disabled = true; return false; }
            dev->Release();
        }
        if (!WaitIdle()) { g_disabled = true; core::Log("[overlay] %s delegate rebind reason gpu_drain_failed chain %p", kMarker, (void*)sc); return false; }
        ReleaseRenderTargets();
        if (!CreateRenderTargets(sc)) { g_disabled = true; core::Log("[overlay] rebind failed; overlay disabled"); return false; }
        g_swapChain = sc;
        g_forceRebind = false;
        core::Log("[overlay] rebound to new swapchain %p (%ux%u)", (void*)sc, g_width, g_height);
        return true;
    }
}
