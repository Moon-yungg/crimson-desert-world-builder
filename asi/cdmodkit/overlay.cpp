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

namespace editor { void Draw(); void Toggle(); bool IsOpen(); void ApplyStyle(float scale); bool PlayMode(); void TogglePlay(); bool Placing(); bool MouseMode(); }

namespace overlay {
    typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT (WINAPI* Present_t)(IDXGISwapChain3*, UINT, UINT);
    typedef HRESULT (WINAPI* Present1_t)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT (WINAPI* ResizeBuffers_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    static FactoryCreateSwapChainForHwnd_t oCreateSwapChainForHwnd = nullptr;
    static Present_t oPresent = nullptr;
    static Present1_t oPresent1 = nullptr;
    static ResizeBuffers_t oResizeBuffers = nullptr;

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
    struct Frame { ID3D12CommandAllocator* alloc = nullptr; ID3D12Resource* rt = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}; UINT64 fence = 0; };
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

    static void ReleaseRenderTargets() {
        for (auto& f : g_frames) { if (f.rt) { f.rt->Release(); f.rt = nullptr; } }
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
        for (UINT i = 0; i < g_bufferCount; i++) {
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].rt)))) return false;
            D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = g_format; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            g_device->CreateRenderTargetView(g_frames[i].rt, &rd, h);
            g_frames[i].rtv = h; h.ptr += inc;
        }
        return true;
    }
    static void WaitIdle() {
        if (!g_fence || !g_queue) return;
        g_queue->Signal(g_fence, ++g_fenceValue);
        if (g_fence->GetCompletedValue() < g_fenceValue) { g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 2000); }
    }

    static bool Init(IDXGISwapChain3* sc) {
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { core::Log("[overlay] GetDevice failed"); return false; }
        DXGI_SWAP_CHAIN_DESC desc = {}; sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 16;
        if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC sh = {}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = kSrvSlots; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap)))) return false;
        if (!CreateRenderTargets(sc)) { core::Log("[overlay] render targets failed"); return false; }
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_cmdList)))) return false;
        g_cmdList->Close();
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigErrorRecoveryEnableTooltip = false; io.ConfigErrorRecoveryEnableAssert = false;
        float scale = g_height / 1080.0f; if (scale < 0.75f) scale = 0.75f;
        editor::ApplyStyle(scale);
        i18n::Initialize(core::ModDir());
        const char* fonts[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\calibri.ttf" };
        bool haveFont = false;
        ImFont* textFont = nullptr;
        const ImWchar* ranges = (const ImWchar*)i18n::GlyphRanges();
        for (const char* fp : fonts) if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) { textFont = io.Fonts->AddFontFromFileTTF(fp, 17.0f * scale, nullptr, ranges); core::Log("[overlay] font %s", fp); haveFont = true; break; }
        if (!haveFont) textFont = io.Fonts->AddFontDefault();
        i18n::MergeSystemFonts(io.Fonts, 17.0f * scale);
        // icons are drawn by the plugin into the atlas (icons.cpp); no icon font is needed
        icons::Register(io.Fonts, textFont, 17.0f * scale);
        io.Fonts->Build();
        icons::Paint(io.Fonts);
        i18n::ReleaseMergedFontData(io.Fonts);
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
          core::g_uiWantsMouse = edit || gizmo; core::g_uiWantsKeyboard = edit; }

        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (idx >= g_frames.size() || !g_frames[idx].rt) return;
        Frame& f = g_frames[idx];
        if (f.fence && g_fence->GetCompletedValue() < f.fence) { g_fence->SetEventOnCompletion(f.fence, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 1000); }
        f.alloc->Reset();
        g_cmdList->Reset(f.alloc, nullptr);
        RecordUploads();
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = f.rt;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cmdList->ResourceBarrier(1, &b);
        g_cmdList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &b);
        if (FAILED(g_cmdList->Close())) { Stage("close failed"); return; }
        Stage("execute");
        ID3D12CommandList* lists[] = { g_cmdList };
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_fence, ++g_fenceValue);
        f.fence = g_fenceValue;
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
        static bool s_homeDown = false;   // Home switches between edit mode (menu takes all input) and play mode (game takes all input)
        bool home = (GetAsyncKeyState(core::g_keyMode) & 0x8000) != 0;
        if (home && !s_homeDown && editor::IsOpen()) editor::TogglePlay();
        s_homeDown = home;
        bool open = editor::IsOpen() || editor::Placing();
        if (open != g_wasOpen) { if (open) input::MenuOpened(); else input::MenuClosed(); g_wasOpen = open; }
        core::g_menuOpen = open;
        if (!open) { core::g_uiWantsMouse = false; core::g_uiWantsKeyboard = false; return; }
        RenderGuarded(sc);
    }

    static HRESULT WINAPI hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) { OnPresent(sc); return oPresent(sc, sync, flags); }
    static HRESULT WINAPI hkPresent1(IDXGISwapChain3* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) { OnPresent(sc); return oPresent1(sc, sync, flags, pp); }
    static HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        if (g_ready && sc == g_swapChain) { WaitIdle(); ReleaseRenderTargets(); }
        HRESULT hr = oResizeBuffers(sc, n, w, h, fmt, flags);
        if (g_ready && sc == g_swapChain) {
            if (!CreateRenderTargets(sc)) { g_disabled = true; core::Log("[overlay] rebuild after resize failed; overlay disabled"); }
            else core::Log("[overlay] resized to %ux%u", g_width, g_height);
        }
        return hr;
    }

    static void PinQueue(IUnknown* queueUnk) {
        ID3D12CommandQueue* q = nullptr;
        if (queueUnk && SUCCEEDED(queueUnk->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            if (q != g_queue) { if (g_queue) g_queue->Release(); g_queue = q; core::Log("[overlay] present queue pinned %p", (void*)q); }
            else q->Release();
        } else core::Log("[overlay] swapchain device is not a D3D12 command queue");
    }
    static void HookFrom(IDXGISwapChain1* chain, IUnknown* queueUnk) {
        if (!chain) return;
        PinQueue(queueUnk);           // every real creation re-pins: the game replaced its chain once already at startup
        if (g_targetsHooked) return;
        g_targetsHooked = true;
        void** vt = *reinterpret_cast<void***>(chain);
        void* present = vt[8]; void* resize = vt[13]; void* present1 = vt[22];
        if (MH_CreateHook(present, (void*)&hkPresent, (void**)&oPresent) == MH_OK && MH_EnableHook(present) == MH_OK) core::Log("[overlay] Present hooked at %p", present);
        else core::Log("[overlay] Present hook failed");
        if (MH_CreateHook(present1, (void*)&hkPresent1, (void**)&oPresent1) == MH_OK && MH_EnableHook(present1) == MH_OK) core::Log("[overlay] Present1 hooked");
        if (MH_CreateHook(resize, (void*)&hkResizeBuffers, (void**)&oResizeBuffers) == MH_OK && MH_EnableHook(resize) == MH_OK) core::Log("[overlay] ResizeBuffers hooked");
    }

    static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** pp) {
        HRESULT hr = oCreateSwapChainForHwnd(self, device, hwnd, desc, fs, out, pp);
        if (SUCCEEDED(hr) && pp && *pp && desc && desc->Width > 64 && desc->Height > 64) {
            core::Log("[overlay] game created swapchain %ux%u fmt %d buffers %u hwnd %p", desc->Width, desc->Height, (int)desc->Format, desc->BufferCount, (void*)hwnd);
            HookFrom(*pp, device);
        }
        return hr;
    }

    void Install() {
        IDXGIFactory2* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory) {
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) { core::Log("[overlay] no DXGI factory; overlay disabled"); return; }
        }
        void** vt = *reinterpret_cast<void***>(factory);
        void* fn = vt[15];
        if (MH_CreateHook(fn, (void*)&hkCreateSwapChainForHwnd, (void**)&oCreateSwapChainForHwnd) == MH_OK && MH_EnableHook(fn) == MH_OK)
            core::Log("[overlay] CreateSwapChainForHwnd detoured (%p); waiting for the game's swapchain", fn);
        else core::Log("[overlay] could not detour CreateSwapChainForHwnd");
        factory->Release();
    }
}
