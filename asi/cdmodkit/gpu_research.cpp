// Reverse-engineering aid for live terrain editing: where do the streamed terrain heights go on the GPU? Hooks the native
// ID3D12GraphicsCommandList copy functions (addresses taken from a command list of the game's own device) and, while a trace
// runs, logs copies into 16-bit single-channel resources (the height format) or 512-wide textures. Only installed on request
// (/api/research/gputrace {"seconds":20}).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "overlay.h"
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <mutex>
#include <set>
#include <utility>

namespace core {

typedef void(STDMETHODCALLTYPE* CopyTexFn)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void(STDMETHODCALLTYPE* CopyBufFn)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
typedef void(STDMETHODCALLTYPE* CopyResFn)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
static CopyTexFn g_origCopyTex = nullptr; static CopyBufFn g_origCopyBuf = nullptr; static CopyResFn g_origCopyRes = nullptr;
static std::atomic<DWORD> g_gpuUntil{ 0 }; static std::atomic<int> g_gpuLines{ 0 };

static std::mutex g_seenMx; static std::set<std::pair<void*, void*>> g_seen;   // each (dst, src) pair is logged once
static bool FirstTime(void* a, void* b) { std::lock_guard<std::mutex> l(g_seenMx); return g_seen.insert({ a, b }).second; }
static bool Tracing() { const DWORD u = g_gpuUntil; return u && GetTickCount() < u && g_gpuLines < 3000; }
static bool Interesting(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16_UNORM || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16_TYPELESS || f == DXGI_FORMAT_R16_SNORM || f == DXGI_FORMAT_R16_UINT; }
static std::string Desc(ID3D12Resource* r) {
    if (!r) return "null";
    const D3D12_RESOURCE_DESC d = r->GetDesc(); char b[200];
    snprintf(b, sizeof b, "%p dim %d %llux%u x%u mips %u fmt %d flags %x", (void*)r, (int)d.Dimension, (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, (int)d.Format, (unsigned)d.Flags);
    return b;
}

static void STDMETHODCALLTYPE HookCopyTex(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (Tracing() && dst && dst->pResource && src) {
        const D3D12_RESOURCE_DESC dd = dst->pResource->GetDesc();
        DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN; UINT sw = 0, sh = 0;
        if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) { sf = src->PlacedFootprint.Footprint.Format; sw = src->PlacedFootprint.Footprint.Width; sh = src->PlacedFootprint.Footprint.Height; }
        else if (src->pResource) { const D3D12_RESOURCE_DESC sd = src->pResource->GetDesc(); sf = sd.Format; sw = (UINT)sd.Width; sh = sd.Height; }
        if ((Interesting(dd.Format) || Interesting(sf)) && FirstTime(dst->pResource, src->pResource)) {
            g_gpuLines++;
            char sbuf[200] = "";
            if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) snprintf(sbuf, sizeof sbuf, "footprint off %llu fmt %d %ux%u pitch %u in %s", (unsigned long long)src->PlacedFootprint.Offset, (int)sf, sw, sh, src->PlacedFootprint.Footprint.RowPitch, Desc(src->pResource).c_str());
            else snprintf(sbuf, sizeof sbuf, "subres %u of %s", src->SubresourceIndex, Desc(src->pResource).c_str());
            Log("[gpu] CopyTextureRegion cl %p dst subres %u at %u,%u,%u of %s <- %s box %s", (void*)cl, dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst->SubresourceIndex : 9999, x, y, z,
                Desc(dst->pResource).c_str(), sbuf, box ? "yes" : "no");
        }
    }
    g_origCopyTex(cl, dst, x, y, z, src, box);
}
static void STDMETHODCALLTYPE HookCopyBuf(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 doff, ID3D12Resource* src, UINT64 soff, UINT64 n) {
    if (Tracing() && dst && (n == 699050 || n == 524288 || n == 0xAAAA || n == 0xAAA || (n >= 0x80000 && n <= 0xB0000))) {
        g_gpuLines++; Log("[gpu] CopyBufferRegion cl %p %llu bytes %s +%llu <- %s +%llu", (void*)cl, (unsigned long long)n, Desc(dst).c_str(), (unsigned long long)doff, Desc(src).c_str(), (unsigned long long)soff);
    }
    g_origCopyBuf(cl, dst, doff, src, soff, n);
}
static void STDMETHODCALLTYPE HookCopyRes(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (Tracing() && dst) { const D3D12_RESOURCE_DESC dd = dst->GetDesc();
        if (Interesting(dd.Format) && FirstTime(dst, src)) { g_gpuLines++; Log("[gpu] CopyResource cl %p %s <- %s", (void*)cl, Desc(dst).c_str(), Desc(src).c_str()); } }
    g_origCopyRes(cl, dst, src);
}

typedef void(STDMETHODCALLTYPE* BarrierFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
static BarrierFn g_origBarrier = nullptr;
static void STDMETHODCALLTYPE HookBarrier(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (Tracing() && b) for (UINT i = 0; i < n; i++) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b[i].Transition.pResource) continue;
        const D3D12_RESOURCE_DESC d = b[i].Transition.pResource->GetDesc();
        if (d.Width != 512 || d.Height != 512 || d.MipLevels != 10 || d.Format != DXGI_FORMAT_R16_TYPELESS) continue;
        if (!FirstTime(b[i].Transition.pResource, (void*)(uintptr_t)((b[i].Transition.StateBefore << 16) ^ b[i].Transition.StateAfter))) continue;
        g_gpuLines++; Log("[gpu] barrier cl %p height texture %p subres %u: 0x%x -> 0x%x", (void*)cl, (void*)b[i].Transition.pResource, b[i].Transition.Subresource, (unsigned)b[i].Transition.StateBefore, (unsigned)b[i].Transition.StateAfter);
    }
    g_origBarrier(cl, n, b);
}
void GpuTrace(int seconds) {
    static bool s_hooked = false;
    if (!s_hooked) {
        ID3D12Device* dev = (ID3D12Device*)overlay::D3DDevice(); if (!dev) { Log("[gpu] no device yet"); return; }
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) || FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)))) { Log("[gpu] command list failed"); if (al) al->Release(); return; }
        void** vt = *(void***)cl;
        const bool a = InstallInternalHook(vt[16], (void*)HookCopyTex, (void**)&g_origCopyTex, "CopyTextureRegion (research)");
        const bool b = InstallInternalHook(vt[15], (void*)HookCopyBuf, (void**)&g_origCopyBuf, "CopyBufferRegion (research)");
        const bool c = InstallInternalHook(vt[17], (void*)HookCopyRes, (void**)&g_origCopyRes, "CopyResource (research)");
        InstallInternalHook(vt[26], (void*)HookBarrier, (void**)&g_origBarrier, "ResourceBarrier (research)");
        cl->Close(); cl->Release(); al->Release();
        Log("[gpu] copy hooks: texture %d buffer %d resource %d (CopyTextureRegion at %p)", a, b, c, vt[16]);
        s_hooked = a || b || c; if (!s_hooked) return;
    }
    { std::lock_guard<std::mutex> l(g_seenMx); g_seen.clear(); }
    g_gpuLines = 0; g_gpuUntil = GetTickCount() + (DWORD)(seconds < 1 ? 1 : seconds > 120 ? 120 : seconds) * 1000;
    Log("[gpu] tracing copies for %d s", seconds);
}

}   // namespace core
