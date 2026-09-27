// Live terrain editing: brush strokes written straight into the GPU texture a tile already uses, no reload.
//
// Every terrain tile ends up as its own 512x512 R16_TYPELESS texture with 10 mips, filled by CopyTextureRegion from an upload
// buffer while it streams in; the game never transitions it (it rests in COMMON). To know which texture belongs to which tile,
// terrain.cpp hands over a signature (a hash over four rows of mip 0) of every tile read it saw complete, and the hook below
// matches the same rows in the upload buffer of each copy into such a texture. A live update then builds the tile's full mip
// chain on the CPU (original file + strokes, through the same function as the stream patch) and copies it into the texture
// with a command list of our own on the game's queue: COMMON -> COPY_DEST -> COMMON around the copies.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "overlay.h"
#include "guard.h"
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace core {

static const uint32_t kSigRow = 200, kSigRows = 4, kPitch = 1024;   // mip 0: 512 samples x 2 bytes per row, tightly packed
static std::mutex g_lmx;
static std::map<uint64_t, std::pair<int, int>> g_sig;                // signature of a completed read -> tile
static std::map<std::pair<int, int>, ID3D12Resource*> g_tex;         // tile -> its texture (one reference held by us)
typedef void(STDMETHODCALLTYPE* CopyTexFn)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
static CopyTexFn g_origCopyTex = nullptr; static bool g_liveOk = false;

static uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

// terrain.cpp: a tile read completed (after any patch); data = file bytes from offset 128 (mip 0 first)
void TerrainLiveNoteRead(int tx, int tz, const uint8_t* data, uint32_t len) {
    if (!g_liveOk || len < (kSigRow + kSigRows) * kPitch) return;
    uint8_t rows[kSigRows * kPitch];
    if (!ReadBytes((uintptr_t)(data + kSigRow * kPitch), rows, sizeof rows)) return;
    const uint64_t h = Fnv(rows, sizeof rows);
    std::lock_guard<std::mutex> l(g_lmx); if (g_sig.size() > 512) g_sig.clear(); g_sig[h] = { tx, tz };
}

static void STDMETHODCALLTYPE HookCopyTex(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (dst && src && dst->pResource && src->pResource && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst->SubresourceIndex == 0 &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && !x && !y && !box) {
        const D3D12_RESOURCE_DESC d = dst->pResource->GetDesc();
        if (d.Width == 512 && d.Height == 512 && d.MipLevels == 10 && d.Format == DXGI_FORMAT_R16_TYPELESS && src->PlacedFootprint.Footprint.RowPitch == kPitch) {
            void* p = nullptr; uint64_t h = 0; bool have = false;
            if (SUCCEEDED(src->pResource->Map(0, nullptr, &p)) && p) {
                uint8_t rows[kSigRows * kPitch];
                if (ReadBytes((uintptr_t)p + (uintptr_t)src->PlacedFootprint.Offset + kSigRow * kPitch, rows, sizeof rows)) { h = Fnv(rows, sizeof rows); have = true; }
                D3D12_RANGE none{ 0, 0 }; src->pResource->Unmap(0, &none);
            }
            if (have) {
                std::lock_guard<std::mutex> l(g_lmx); auto it = g_sig.find(h);
                if (it != g_sig.end()) {
                    ID3D12Resource*& slot = g_tex[it->second];
                    if (slot != dst->pResource) { dst->pResource->AddRef(); if (slot) slot->Release(); slot = dst->pResource;
                        Log("[terrain] tile %d,%d uses texture %p", it->second.first, it->second.second, (void*)dst->pResource); }
                }
            }
        }
    }
    g_origCopyTex(cl, dst, x, y, z, src, box);
}

void TerrainLiveInstall() {
    ID3D12Device* dev = (ID3D12Device*)overlay::D3DDevice(); if (!dev) return;
    static bool s_done = false; if (s_done) return; s_done = true;
    ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) || FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)))) { if (al) al->Release(); Log("[terrain] live: no command list"); return; }
    void** vt = *(void***)cl;   // ID3D12GraphicsCommandList slot 16 = CopyTextureRegion (the driver's implementation, shared by every list)
    g_liveOk = InstallInternalHook(vt[16], (void*)HookCopyTex, (void**)&g_origCopyTex, "terrain live (CopyTextureRegion)");
    cl->Close(); cl->Release(); al->Release();
    Log("[terrain] live editing %s", g_liveOk ? "ready" : "unavailable");
}
bool TerrainLiveAvailable() { return g_liveOk; }
bool TerrainLiveHasTexture(int tx, int tz) { std::lock_guard<std::mutex> l(g_lmx); return g_tex.count({ tx, tz }) != 0; }

// Copies a full mip chain (file bytes from offset 128, 10 mips tightly packed) into the tile's texture and waits for the GPU.
bool TerrainLiveUpload(int tx, int tz, const uint8_t* chain, size_t len) {
    ID3D12Device* dev = (ID3D12Device*)overlay::D3DDevice(); ID3D12CommandQueue* q = (ID3D12CommandQueue*)overlay::D3DQueue();
    if (!dev || !q) return false;
    ID3D12Resource* tex = nullptr;
    { std::lock_guard<std::mutex> l(g_lmx); auto it = g_tex.find({ tx, tz }); if (it == g_tex.end()) return false; tex = it->second; tex->AddRef(); }
    const D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[10]; UINT rows[10]; UINT64 rowBytes[10], total = 0;
    dev->GetCopyableFootprints(&d, 0, 10, 0, fp, rows, rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; ID3D12Fence* fence = nullptr; bool ok = false;
    do {
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) break;
        uint8_t* m = nullptr; if (FAILED(up->Map(0, nullptr, (void**)&m)) || !m) break;
        size_t srcOff = 0; bool fits = true;
        for (int mip = 0; mip < 10; mip++) {
            const size_t rb = (size_t)rowBytes[mip];   // dim * 2
            for (UINT r = 0; r < rows[mip]; r++) { if (srcOff + rb > len) { fits = false; break; } memcpy(m + fp[mip].Offset + (size_t)r * fp[mip].Footprint.RowPitch, chain + srcOff, rb); srcOff += rb; }
            if (!fits) break;
        }
        up->Unmap(0, nullptr); if (!fits) break;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) || FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)))) break;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; cl->ResourceBarrier(1, &b);
        for (UINT mip = 0; mip < 10; mip++) {
            D3D12_TEXTURE_COPY_LOCATION dl{}; dl.pResource = tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION sl{}; sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl.PlacedFootprint = fp[mip];
            g_origCopyTex(cl, &dl, 0, 0, 0, &sl, nullptr);   // past our own hook
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; cl->ResourceBarrier(1, &b);
        if (FAILED(cl->Close())) break;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) break;
        ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, 1);
        for (int i = 0; i < 400 && fence->GetCompletedValue() < 1; i++) Sleep(5);   // wait up to 2 s before the buffers go away
        ok = fence->GetCompletedValue() >= 1;
    } while (false);
    if (!ok) Log("[terrain] live upload of tile %d,%d failed", tx, tz);
    if (fence) fence->Release(); if (cl) cl->Release(); if (al) al->Release();
    if (up) { if (ok) up->Release(); /* not finished on the GPU: leak the buffer rather than free it under the GPU */ }
    tex->Release();
    return ok;
}

}   // namespace core
