# Local HTTP API

World Builder can run a local HTTP server. It is **off by default**: tick **HTTP API for programs on this PC** in the editor's **Settings** tab. The server starts at once, and the choice is remembered in `bin64/cdmodkit/settings.txt` (`http_api=1`) until the box is unticked again, which stops it at once. The default address is `http://127.0.0.1:8765`; the port can be changed next to the checkbox (or with `http_port=`) and a running server moves to the new port immediately. If the port is taken by another program, the Settings tab says so and offers a retry. The server listens only on IPv4 loopback.

Requests and responses use UTF-8 JSON. Write requests take a flat JSON object with `Content-Type: application/json` and a `Content-Length` header. Responses have an `error` field on failure. The server handles one connection at a time and closes each connection after its response. Request bodies are limited to 64 KiB.

## Read operations

| Request | Result |
| --- | --- |
| `GET /api/status` | API version, game readiness/build status, queued game-thread jobs |
| `GET /api/player` | Player world coordinates `{x,y,z}`; 503 if unavailable |
| `GET /api/camera` | Camera position `{x,y,z}`, its raw `axis` (sign ambiguous) and `view` `{x,z}`: horizontal unit direction from the camera to the player, i.e. where the screen looks. 503 if unavailable |
| `GET /api/prefabs?q=lamp&offset=0&limit=100` | Search prefab path, display name and tags; paged results. Omit `q` for the whole catalog. |
| `GET /api/objects?offset=0&limit=100` | Paged World Builder scene objects with stable `uid`, prefab, position, rotation, scale, hidden state, group and project |
| `GET /api/objects/{uid}` | One scene object |
| `GET /api/projects` | Saved project names |
| `GET /api/autoload` | Names loaded automatically next game start |
| `GET /api/playmode` | Play mode state: `active`, `isolating`, `phase` (`title`, `loading`, `in_world`, `travelling`, `arrived`, `ready`, `exited`, `off`), `status`, `spawn`, `loadedAt`, `counts` (refused saves / save requests, refused actors, isolated levels, ...) |
| `GET /api/playmode/census` | The same plus what the game created this session: actor spawn reasons, field-create callers, createSceneObjectFrom callers, resource loads and streamer reads by type |

Coordinates are absolute world coordinates. A prefab search returns catalog records, while scene object reads return instances created by World Builder. It does not enumerate all native game objects.

Both list endpoints return `total`, `offset`, `limit`, `nextOffset` and `items`. The default page size is 100; each request may ask for 1–500 entries. **500 is a page size limit, not a catalog limit**: request `offset=0`, then use each non-null `nextOffset` until it becomes `null` to read all 48,000+ prefabs or all scene objects.

## Write operations

| Request | JSON body | Result |
| --- | --- | --- |
| `POST /api/camera` | Any of `on`, exact `x/y/z`, relative `forward/right/up`, `turnYaw/turnPitch`, `dolly`, `focusX/focusY/focusZ/focusRadius`, or `view` = `level` / `down` / `up` | Controls the free camera. Turn it on with `{"on":true}` first; movement/position requests return 409 until the free camera has initialized. `{"on":false}` restores the game camera. |
| `POST /api/objects` | `{"prefab":"/object/...prefab","x":1,"y":2,"z":3,"yaw":0,"scale":1}` | Queue a spawn; returns a stable `uid` |
| `PATCH /api/objects/{uid}` | Any of `x`, `y`, `z`, `yaw`, `pitch`, `roll`, `scale` | Move/rotate/scale; omitted values stay unchanged |
| `POST /api/objects/{uid}/hide` | `{}` | Hide the instance but keep its record |
| `DELETE /api/objects/{uid}` | No body | Hide and forget the instance |
| `POST /api/objects/{uid}/group` | `{"group":2}` | Set its group; group 0 means none |
| `POST /api/objects/{uid}/project` | `{"name":"Camp"}` | Assign it to a project |
| `POST /api/groups` | `{}` | Create and return a group ID |
| `POST /api/scene/clear` | `{}` | Clear all World Builder objects |
| `POST /api/projects/save` | `{"name":"Camp","scope":0}` | Save a project. Scope: 0 whole scene, 1 this project and new objects, 2 new objects only, 3 this project only. |
| `POST /api/projects/load` | `{"name":"Camp","clearFirst":false}` | Load a project; optionally clear the scene first |
| `POST /api/autoload` | `{"name":"Camp","enabled":true}` | Add or remove a project from autoload |
| `POST /api/npc` | `{"key":30191,"x":1,"y":2,"z":3,"type":1}` | Legacy one-shot NPC spawn through the game's own request. `key` is the characterinfo row key and `type` the spawn reason (default 1). Returns 202 when queued; 409 until the player has walked a few steps after loading; 503 if unsupported. This HTTP endpoint remains unmanaged (no editor UID/project persistence); NPCs created from the World Builder NPC/Scene UI use the separate managed-NPC registry with selection, AI/behavior control, movement, undo/redo and project saving. |
| `POST /api/playmode/exit` | `{"mode":"quit"}` or `{"mode":"travel"}` | Leave play mode: `quit` closes the game (the game's quit dialog is confirmed by holding Space), `travel` turns isolation off and travels back to where the save had put the player (NPCs return at once, levels already loaded empty stay empty until they stream out; saving stays blocked until the game is restarted). 409 when play mode is not active. Also `POST /api/playmode {"exit":...}`. |
| `POST /api/log` | `{"text":"hello"}` | Write a line to `cdmodkit.log` |

Spawns, moves and removals run on the next game simulation tick. These requests return HTTP 202 when queued; `GET /api/status` reports the remaining job count. The ASI must be installed and running in Crimson Desert, and the game must be in a state where its simulation tick runs. `GET /api/status` remains available while the game is still loading.

Object scale accepted by the editor and HTTP object endpoints is 0.05–20.0.

The HTTP `uid` is stable while the instance remains in the scene. The older C API uses scene-list indices, which may shift after an object is forgotten.

Example in PowerShell:

```powershell
$base = 'http://127.0.0.1:8765'
Invoke-RestMethod "$base/api/prefabs?q=lamp&limit=5"
$item = Invoke-RestMethod "$base/api/objects" -Method Post -ContentType 'application/json' -Body '{"prefab":"/object/cd_gimmick/breakable/gimmick_breakable_sack_01.prefab","x":0,"y":0,"z":0}'
Invoke-RestMethod "$base/api/objects/$($item.uid)" -Method Patch -ContentType 'application/json' -Body '{"y":2}'
```

## Play mode

`bin64/cdmodkit/playmode.json` asks for an isolated start: the plugin presses continue on the title screen itself (while the game window is in front), the game loads, the player is taken to `spawn` with the game's own fast travel, and only terrain, sky, the player and the requested scene remain: the world's NPCs, animals, level gimmicks, buildings and props are not created, and saving is blocked for the whole session. The file is renamed to `playmode.last.json` as soon as it is read, so the following start is a normal one. Minimal request:

```json
{ "enabled": true,
  "spawn": { "x": -11656.4, "y": 733.0, "z": -2178.2, "yaw": 0 },
  "objects": [ { "prefab": "/object/00_common/castle/cd_castle_mercenary_wall_09b_shield.prefab", "pos": [-11656.0, 731.0, -2166.0], "rot": [0, 0, 0], "scale": 1 } ] }
```

Version 2 (`"version": 2`) shows the editor's scene instead of an empty world: `"scene": {"levels": [game paths], "sectors": [[sx, sz], ...], "keepTerrain": true}` lists the levels that load normally (their child levels follow) and the 256 m sectors whose NPCs and level actors stay; everything else is isolated as before. `"overrides": {"<game path>": "<absolute path of a local file>" or "<another game path>"}` serves edited files in place of the game's (files the game loads through its resource loader: levels, prefabs, tables, string tables; not textures or meshes). `spawn.yaw` 0 faces +z, 90 faces +x. Vegetation outside the scene (the procedural forest) is not removed yet.

`"project": "Name"` loads a saved World Builder project instead of (or in addition to) `objects`. All keys and the details are in `notes/FORMATS.md` ("Play mode"). A launcher writes the file and starts the game with `steam://rungameid/3321460`; `GET /api/playmode` reports when the scene is `ready`.
