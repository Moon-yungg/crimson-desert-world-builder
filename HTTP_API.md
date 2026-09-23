# Local HTTP API

World Builder can run a local HTTP server. It is **off by default**: tick **HTTP API for programs on this PC** in the editor's **Settings** tab. The server starts at once, and the choice is remembered in `bin64/cdmodkit/settings.txt` (`http_api=1`) until the box is unticked again, which stops it at once. The default address is `http://127.0.0.1:8765`; the port can be changed next to the checkbox (or with `http_port=`) and a running server moves to the new port immediately. If the port is taken by another program, the Settings tab says so and offers a retry. The server listens only on IPv4 loopback.

Requests and responses use UTF-8 JSON. Write requests take a flat JSON object with `Content-Type: application/json` and a `Content-Length` header. Responses have an `error` field on failure. The server handles one connection at a time and closes each connection after its response. Request bodies are limited to 64 KiB.

## Read operations

| Request | Result |
| --- | --- |
| `GET /api/status` | API version, game readiness/build status, queued game-thread jobs |
| `GET /api/player` | Player world coordinates `{x,y,z}`; 503 if unavailable |
| `GET /api/prefabs?q=lamp&offset=0&limit=100` | Search prefab path, display name and tags; paged results. Omit `q` for the whole catalog. |
| `GET /api/objects?offset=0&limit=100` | Paged World Builder scene objects with stable `uid`, prefab, position, rotation, scale, hidden state, group and project |
| `GET /api/objects/{uid}` | One scene object |
| `GET /api/projects` | Saved project names |
| `GET /api/autoload` | Names loaded automatically next game start |

Coordinates are absolute world coordinates. A prefab search returns catalog records, while scene object reads return instances created by World Builder. It does not enumerate all native game objects.

Both list endpoints return `total`, `offset`, `limit`, `nextOffset` and `items`. The default page size is 100; each request may ask for 1–500 entries. **500 is a page size limit, not a catalog limit**: request `offset=0`, then use each non-null `nextOffset` until it becomes `null` to read all 48,000+ prefabs or all scene objects.

## Write operations

| Request | JSON body | Result |
| --- | --- | --- |
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
| `POST /api/log` | `{"text":"hello"}` | Write a line to `cdmodkit.log` |

Spawns, moves and removals run on the next game simulation tick. These requests return HTTP 202 when queued; `GET /api/status` reports the remaining job count. The ASI must be installed and running in Crimson Desert, and the game must be in a state where its simulation tick runs. `GET /api/status` remains available while the game is still loading.

The HTTP `uid` is stable while the instance remains in the scene. The older C API uses scene-list indices, which may shift after an object is forgotten.

Example in PowerShell:

```powershell
$base = 'http://127.0.0.1:8765'
Invoke-RestMethod "$base/api/prefabs?q=lamp&limit=5"
$item = Invoke-RestMethod "$base/api/objects" -Method Post -ContentType 'application/json' -Body '{"prefab":"/object/cd_gimmick/breakable/gimmick_breakable_sack_01.prefab","x":0,"y":0,"z":0}'
Invoke-RestMethod "$base/api/objects/$($item.uid)" -Method Patch -ContentType 'application/json' -Body '{"y":2}'
```
