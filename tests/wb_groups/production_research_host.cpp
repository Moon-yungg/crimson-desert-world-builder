// Native debugger/renderer boundaries unavailable in the host. Terrain/project/travel state stays real.
#include "../../asi/cdmodkit/core_internal.h"
#include "../../asi/cdmodkit/overlay.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
namespace core {
bool StartWatch(const uintptr_t[4]) { return false; }
void RefreshWatch() {}
void StopWatch() {}
void DumpWatch(const char*, uint64_t, uint64_t) {}
bool WatchWrites(const uintptr_t[4], int, const char*) { return false; }
bool WatchAccessSync(const uintptr_t[4], int, const char*) { return false; }
}
namespace overlay {
void* D3DDevice() { return nullptr; }
void* D3DQueue() { return nullptr; }
}
