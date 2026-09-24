// Declarations shared between cdmodkit.cpp (core) and diag.cpp (reverse-engineering aids).
// Not part of the public API: cdmodkit_api.h and core.h are what other code should use.
#pragma once
#include "core.h"
#include <utility>

namespace core {
    // camera objects reachable from the camera manager, with their RTTI names; used by the fov / camera traces
    void ExpandCameraManager(std::vector<std::pair<std::string, uintptr_t>>& out);

    // the renderer camera through its own object (cdmodkit.cpp); false when unresolved or the block does not validate
    bool NativeRenderCamera(Vec3* pos, Vec3* right, Vec3* up, Vec3* fwd, float* m00, float* m11);

    // pack I/O tracing (diag.cpp): installed once at startup, switched by the console command "traceio on|off"
    void InstallIoTrace();
    void SetIoTrace(bool on);
}
