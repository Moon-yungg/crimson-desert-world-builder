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

// Puppets (exports present since 2026-10-26; look them up with GetProcAddress, older builds lack them): an NPC of a
// characterinfo row spawned by the game's own request, never saved, AI terminated once bound, moved by the caller.
// Spawning needs the player to have walked a few steps after loading (0 until then).
typedef int      (*cdk_puppet_spawn_t)(uint32_t characterKey, float x, float y, float z);   // puppet id, 0 = not now
typedef int      (*cdk_puppet_state_t)(int id, float* xyz);    // 1 bound (xyz = live position), 0 spawning, -1 failed / gone
typedef int      (*cdk_puppet_move_t)(int id, float x, float y, float z, const float* quatXyzw);  // quat may be NULL; 1 ok
typedef int      (*cdk_puppet_remove_t)(int id);               // removes the actor; the id becomes invalid

#ifdef __cplusplus
}
#endif
