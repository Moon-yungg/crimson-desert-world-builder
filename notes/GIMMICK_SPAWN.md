# Gimmick actors: how the game spawns interactive objects (research, build 1.0.0.2949)

Goal: spawn torches / doors that really work (game interaction). Status: not reached. What is known:

## Layers
- A visible prefab is a SceneObject (client: `SceneObjectClient`, server: `SceneObjectServer`, both carry a 16-byte UUID).
- An interactive object is a **gimmick actor** (client/server pair) with a `GimmickInfoKey` (uint16, `gimmickinfo.staticinfo`)
  and a state chart in `gamedata/binarygimmickchart__/bin/<name>.binarygimmick` (states Wait / GimmickOn / Lock / Deactive / Clear,
  handlers like GimmickEventHandlerData_LightSwitch, _PrefabSwitch, _PlayAnimation, _SummonGimmick, ...).
- The actor is created **server side** and bound to a `SceneObjectServer` by UUID; the client gets the actor replicated and its
  `ClientGimmickActorComponent::sceneObjectCreateAndAddToLevel()` (function 0x8ce350, step function 0x8cfae0, calls
  createSceneObjectFrom from rva 0x8cfe11) creates the visible client object. Torches come as `_on` prefab plus a separate
  flame-effect prefab; doors as frame prefab plus `_door` leaf prefab (leaf pivot = hinge, bottom).

## Server spawn path (all reasons)
- `prepare` = function at rva 0x278f420 (signature in cdmodkit.cpp `ResolveGimmickSpawn`), 42 callers, each with a spawn reason
  string hashed with the game's lookup3 variant (rva 0x1364810): "housing" (0x2b651f0, call at 0x2b65e9b), "drop" (0x2902d50),
  "discardfrominventory" (0x2b40370), "inspect" (0x2b572a0), "buff" (summon processors), "docking", "catchspawn", "spawnbyprojectile",
  "AutoSpawnOwnerData", ... The item drop caller is 0x2b4cff0 (call at 0x2b4d733), level streaming 0x2af5180 (call at 0x2af537e).
- Arguments: rcx = param (a region of the caller's frame: `CreateServerActorDesc_InstantGimmick` object 0xC8 bytes before it,
  param+0x18 -> `FieldGimmickSaveData` on the heap), rdx = out, r8 = `ServerField`, r9 = owner, stack: s5 context, s6 context,
  s7 flags, s8 transform block (scale3 quat4 pos3 world at +0..+0x28, identity words at +0x30/+0x38: housing = item key 100xxxx and
  gimmick key), s9 static.
- After prepare the caller: reason hash -> param+0x3F8 and save+0x220; `desc->vtable[4](desc, &int)` (commit); then
  `ServerField->vtable[17](field, &int, &{&desc,1}, 0)` (create). Result int: 0 = ok, else a hashed error name
  (the embedded error-name table + the game's hash decode it; the mod logs the name).
- FieldGimmickSaveData layout (reflection setters): +0x28 fieldGimmickSaveDataKey, +0x30 fieldSaveDataReason, +0x4C
  levelOriginSceneObjectUuid (16), +0x60 item, +0x1C0 gimmickInfoKey (u16), +0x1CC transform, +0x1F4 originSpawnTransform,
  +0x220 spawnReason hash.
- ServerField vtable (rtti_static: .?AVServerField@pa@@ 0x5b292b8): slot 17 = 0x2af9a70 (create from desc list). It drops a
  desc when the gimmick is flagged unique and an actor with the key exists (eErrNoDuplicateUniqueGimmick), then creates the
  actor and finally (function 0x282e630) looks up the actor's scene object by UUID; missing -> eErrNoInvalidSceneObjectUUID.
- The actor creation core for level objects is 0x2827ce0 (wrapper 0x28279d0, signature in `ResolveActorCore`): a1 =
  `ServerSyncSceneObjectManager`, the info object is a `SceneObjectServer` whose UUID sits at +0x1D8.

## Replay experiments (cdmodkit.cpp, Log tab "gimmick spawn research")
- Copying the caller's frame window and re-issuing prepare + commit + create works up to the last step for housing and level
  captures; both then fail with eErrNoInvalidSceneObjectUUID: the copied spawn still refers to the scene object of the original
  (housing: the preview object the client had created and sent with the request; level: the level's own object).
- Missing piece: creating a fresh `SceneObjectServer` (ServerSyncSceneObjectManager) from the prefab with a new UUID and putting
  that UUID into the spawn description, then letting the client replicate the actor. Not attempted.
- Housing objects are additionally bound to the inventory item (item key 1000511 = the lamp): the second identity word.

## Replay works (2026-09-22, build 1.0.0.2949)
- The item-drop capture (caller 0x2b4d733) replays into a second, working object. Two things were missing:
  1. The scene object UUID of the spawn is the first 8 bytes of stack arg 5 (two words: an id and a per-session constant,
     e.g. `5039bd3f 0000134e`). The prepare copies it into the `SceneObjectServer` (+0x1D8); the create refuses a UUID the
     ServerSyncSceneObjectManager already knows. The replay writes a fresh id word (`FreshUuid`, 0x7E000000 + n).
  2. The error is raised by 0x2827ce0 (the inner create; the drop / housing field create calls it directly from 0x2a999de, the
     level path through the wrapper 0x28279d0): 6th argument -> array entry -> pointee (a `SceneObjectServer`) whose +0x1D8
     UUID must be non-zero and unknown. Hooked as `HookActorInner` (trace only).
- The sync key (u16, param+8) is per item type (6579 shield, 6687 lantern), not per instance; replacing it is unnecessary.
- After the field create the drop caller only releases references, so prepare + register + reason + commit + create is the
  whole sequence. The client replicates the object by itself (ClientSyncSceneObjectManager looks the UUID up, then finds it).
- The capture window must be clamped to the mapped stack region (VirtualQuery): the drop frame lies near the stack top.
- A replayed drop shows a different name / look than the original: the item instance data is not duplicated (the register
  step 0x240b020 binds the original's item). Irrelevant for gimmicks that carry no item (lamps, doors).
- Replays bypass the prepare hook (they call the original directly), so they never appear in the capture list.

## Any gimmick, anywhere, on demand, removable (2026-09-23)
- The prepare's 4th argument ("owner", r9) is the prefab path (C string) the server scene object is built from (0x278f488 ->
  0x129cb80 strlen/string/create). A replay with another path spawns that gimmick: `notes/gimmickinfo.tsv` (13,941 keys ->
  name -> prefab, parsed from gamedata/binarystaticinfo__/bin/gimmickinfo.staticinfobody with the rebuilt notes/packctx.pkl).
  Verified: a housing lamp capture replayed as gimmick_lamp_standtorch_03_on.prefab gives a working, interactive torch.
- Level streaming captures (caller 0x2af537e, made by walking) work as the template too: clear the origin UUID in stack arg 6,
  fresh spawn UUID, keep the level's own reason ("housing" made the create look a housing item up and crash at 0x389570).
  MakeCopy must not write the position blindly to s5+0x44: in the level frame s5 = desc-0x10 and that is the desc's self
  pointer (crash in 0x2799185 with float garbage); the captured position is replaced wherever its exact bytes occur.
- On demand: ServerField vtable slot 9 runs ~18x/s on the server thread; armed replays and removal requests run from there.
- Actor: the field's actor factory 0x2a82b80 allocates 0x100 bytes and calls the ServerActor base ctor 0x28f2380 (hooked:
  `this` on the spawn thread during a replay = our actor, class ServerNormalInGameActor; it refers to the scene object at
  actor+0xB0 one pointer deep).
- Removal: an item pickup queues the actor; the server tick loop 0x27803e0 (container+0x1B0 list of {word, actor}) does: lock
  actor+0x18 (vtable[1]/[2] of that sub-object), actor+0x98 = reason global (rva 0x6cdb94c), actor+0x9C = 0, actor+0x5C = state
  word (actor+0x5E), actor->vtable[16](actor), unlock, actor->vtable[34](actor, &result). Repeating that for our actor removes the
  spawned torch (RemoveSpawnedActor; ends in ServerSyncSceneObjectManager slot 2/3 like the pickup).
- Open: rotation (replace the captured quaternion like the position), project persistence + editor objects, whether the game
  saves the spawned field gimmicks itself (would duplicate on reload).

## Practical alternative (World Builder's own interaction)
- 81 lamps exist as `_on` / `_off` prefab pairs (lamp/off/...); a torch toggle is a prefab swap.
- Door leaves are separate prefabs with the pivot on the hinge; opening is a yaw rotation about the pivot.
