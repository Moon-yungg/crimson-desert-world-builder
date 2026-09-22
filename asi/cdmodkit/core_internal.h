// Declarations shared between cdmodkit.cpp (core) and diag.cpp (reverse-engineering aids).
// Not part of the public API: cdmodkit_api.h and core.h are what other code should use.
#pragma once
#include "core.h"
#include <utility>

namespace core {
    // camera objects reachable from the camera manager, with their RTTI names; used by the fov / camera traces
    void ExpandCameraManager(std::vector<std::pair<std::string, uintptr_t>>& out);

    // pack I/O tracing (diag.cpp): installed once at startup, switched by the console command "traceio on|off"
    void InstallIoTrace();
    void SetIoTrace(bool on);
}
