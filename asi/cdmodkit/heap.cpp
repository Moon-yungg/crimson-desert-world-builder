// Private heap for the plugin's C++ allocations (STL containers, thumbnail worker, texture decoding).
// Keeps the worker's allocation churn off the process default heap the game allocates from, so the two never
// contend for the same heap lock.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <new>

static HANDLE PluginHeap() {
    static HANDLE h = HeapCreate(0, 1 << 20, 0);   // growable, serialized
    return h ? h : GetProcessHeap();
}
void* CdHeapAlloc(size_t n) { return HeapAlloc(PluginHeap(), 0, n ? n : 1); }
void* CdHeapRealloc(void* p, size_t n) { return p ? HeapReAlloc(PluginHeap(), 0, p, n ? n : 1) : CdHeapAlloc(n); }
void  CdHeapFree(void* p) { if (p) HeapFree(PluginHeap(), 0, p); }

void* operator new(size_t n) { void* p = CdHeapAlloc(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n) { void* p = CdHeapAlloc(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return CdHeapAlloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return CdHeapAlloc(n); }
void operator delete(void* p) noexcept { CdHeapFree(p); }
void operator delete[](void* p) noexcept { CdHeapFree(p); }
void operator delete(void* p, size_t) noexcept { CdHeapFree(p); }
void operator delete[](void* p, size_t) noexcept { CdHeapFree(p); }
