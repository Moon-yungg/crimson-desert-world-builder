// cdmodkit public C API for other mods (ASI plugins, DLLs, scripting hosts).
//
// Usage from another mod:
//   HMODULE h = GetModuleHandleA("cdmodkit.asi");   // loaded by the ASI loader before you, or LoadLibraryA it
//   auto spawn = (cdk_spawn_t)GetProcAddress(h, "cdk_spawn");
//   int id = spawn("/object/cd_gimmick/breakable/gimmick_breakable_sack_01.prefab", x, y, z, 0.0f, 1.0f);
//
// All calls are thread-safe; work that must run on the game thread is queued and executed on the next game tick.
// Coordinates are WORLD coordinates (tile index * 1000 + tiled position). Object ids are indices into the scene list
// and stay valid until cdk_forget/cdk_clear.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CDK_API_VERSION 1

typedef uint32_t (*cdk_version_t)(void);                       // CDK_API_VERSION of the loaded cdmodkit
typedef int      (*cdk_ready_t)(void);                          // 1 when hooks are installed and the game thread pump runs
typedef int      (*cdk_player_pos_t)(float* xyz);               // world position of the player; 0 if not in the world
typedef int      (*cdk_spawn_t)(const char* prefab, float x, float y, float z, float yawDeg, float scale);  // returns object id or -1
typedef int      (*cdk_move_t)(int id, float x, float y, float z, float yawDeg, float scale);              // 1 ok
typedef int      (*cdk_remove_t)(int id);                       // hides/removes the object (setEnable 0), keeps the id
typedef int      (*cdk_count_t)(void);                          // number of objects in the scene list
typedef int      (*cdk_get_t)(int id, char* prefabOut, int prefabCap, float* xyz, float* yawDeg, float* scale, int* hidden);  // 1 ok
typedef void     (*cdk_log_t)(const char* text);                // writes to cdmodkit.log / console

#ifdef __cplusplus
}
#endif
