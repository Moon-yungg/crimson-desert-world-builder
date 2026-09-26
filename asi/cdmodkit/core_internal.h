// Declarations shared between cdmodkit.cpp (core) and diag.cpp (reverse-engineering aids).
// Not part of the public API: cdmodkit_api.h and core.h are what other code should use.
#pragma once
#include "core.h"
#include <utility>

namespace core {
    // Internal hook wrapper used by optional feature modules. It keeps the same
    // near-image trampoline reservation/retry behavior as the core hooks.
    bool InstallInternalHook(void* target, void* detour, void** original, const char* name);

    // Optional environment bridge (time-of-day + weather). Resolution failures
    // only disable these controls and never make the rest of World Builder fail.
    void EnvironmentInstall();
    void EnvironmentTick();

    // camera objects reachable from the camera manager, with their RTTI names; used by the fov / camera traces
    void ExpandCameraManager(std::vector<std::pair<std::string, uintptr_t>>& out);

    // the renderer camera through its own object (cdmodkit.cpp); false when unresolved or the block does not validate
    uintptr_t CameraSceneObject();    // the camera manager's scene object (the pose the game's camera logic produces), 0 if unknown
    uintptr_t NativeCameraObject();   // the renderer camera object itself (0 when unresolved or its type does not match)

    // pack I/O tracing (diag.cpp): installed once at startup, switched by the console command "traceio on|off"
    void InstallIoTrace();
    void SetIoTrace(bool on);
}
