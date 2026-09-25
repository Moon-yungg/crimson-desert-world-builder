# World Builder (Crimson Desert map editor / mod kit)

An ASI plugin that adds an in-game world editor to Crimson Desert: prefab browser with rendered previews,
spawning, moving/rotating/scaling, groups, projects, plus the reverse-engineering scripts that produced the
knowledge behind it. Public name **World Builder**, technical names stay `cdmodkit.asi`, `bin64\cdmodkit\`, `cdk_*`.

## Layout

| Path | What |
| --- | --- |
| `asi/cdmodkit/` | the plugin (C++17, MSVC, static CRT) |
| `asi/cdmodkit/data/` | `prefabs.tsv` (48k prefab paths + tags), `settings.txt` defaults |
| `scripts/` | build helpers (`make_release.py`, `pack_index.py`) and RE tools (`xref.py`, `disasm.py`, `rtti_*.py`, `peek.py`, `parse_parc.py`) |
| `notes/FORMATS.md` | **read this first for anything about the game**: RVAs, struct offsets, the spawn recipe, file formats, pitfalls |
| `notes/rtti_*.txt` | RTTI dumps of the game (class name -> vtable rva) |
| `release/` | `README.md`, `NEXUS_PAGE.md`, `CHANGELOG_<ver>.txt`; built zips and folders are gitignored |
| `tools/` | gitignored third party: ImGui 1.91.5, MinHook, pycrimson |

The repo lives **outside** the game folder on purpose: Steam's "verify integrity of game files" deletes anything
in `D:\SteamLibrary\steamapps\common\Crimson Desert` that the game did not ship. Never keep sources or backups there.

## Build, deploy, release

```
asi\cdmodkit\build.bat                  # vcvars64 -> pack_index.py -> rc -> cl; output asi\cdmodkit\build\cdmodkit.asi
python scripts\make_release.py 0.79     # bumps the version strings, builds, checks the exports, writes release\WorldBuilder-v0.79.zip
```

- Deploy = copy `asi\cdmodkit\build\cdmodkit.asi` over `<game>\bin64\cdmodkit.asi`. **The game must be closed**, the
  DLL is locked while it runs. Relaunch with `steam://rungameid/3321460`.
- Data files live in `<game>\bin64\cdmodkit\` (`prefabs.tsv`, `settings.txt`, `thumbs\`, `projects\`, `cdmodkit.log`).
  `prefabs.tsv` is also linked into the .asi as an LZ4 resource and written out when the file is missing.
- The release zip ships **no game assets**: only `.asi`, `prefabs.tsv`, `settings.txt`, the SDK header and the docs.
  Thumbnails are rendered on the player's own machine from their own installed game files.
- One `CHANGELOG_<ver>.txt` per release, tag `v<ver>` on the release commit, commit message `v<ver>: <summary>`.
- `release/NEXUS_PAGE.md` contains hand edits by the user - patch it in place, never regenerate it wholesale.

## Architecture

- `cdmodkit.cpp` - core: logging, guarded memory reads, player/camera lookup, signature resolution, the
  createSceneObjectFrom hook, the game-thread pump, the spawn registry, projects/autoload, the console,
  plus reverse-engineering aids (trace hooks, `FovTrace`, `CamTrace`, ray/shape tracing).
- `environment.cpp` - optional visual time-of-day and weather bridge: runtime signature resolution, lighting-only time freeze,
  and composed weather-table overrides. Failures disable only the environment controls.
- `overlay.cpp` - D3D12: intercepts the game's returned DXGI factory (including Streamline when present), hooks Present/Present1/ResizeBuffers, draws ImGui into
  the back buffer, manages the thumbnail textures (decode thread -> upload -> SRV heap).
- `diag.cpp` - console-only reverse-engineering aids (`fovtrace`, `camtrace`, `traceio`) behind `core_internal.h`.
- `editor.cpp` - the whole UI: browser, scene tree, placement mode, line/circle tools, projects, travel, settings.
- `input.cpp` - window subclass, virtual cursor from raw mouse deltas, scan-code key state.
- `thumbgen.cpp` - background worker that reads the game's packs through the game's own loader and renders prefab previews.
- `heap.cpp` - private Win32 heap behind `operator new`/`delete`, so the plugin never contends with the game's heap lock.
- `icons.cpp` - icons drawn procedurally into the ImGui atlas (no icon font is bundled).

## Rules that are load-bearing

1. **Never hardcode a game RVA.** Everything is resolved at startup in `ResolveGame()` by byte signature, by a
   referenced string plus the PE unwind table (`FuncReferencingString`), or through RTTI class names. A game patch
   must leave the mod working or disabled with a clear message - never silently calling a wrong address.
   If resolution fails, `BuildOk()` is false, nothing is hooked, and the UI says so.
2. **Everything that touches game objects runs on the game thread** via `RunOnGameThread()`, which the movement-tick
   hook drains. Spawning or moving from the render thread crashes the game.
3. **Every read of game memory goes through `ReadBytes`/`ReadPtr`/`Deref`** (SEH-guarded). Pointer walks into the
   game's structures are expected to fail sometimes; that must never fault.
4. `setWorldTransform` takes a **44-byte TiledTransform** (scale3, quat4, pos3 in-tile, int16 tileX/tileZ, tile pair
   read at +0x28). Passing 40 bytes puts garbage in the tile and objects vanish. See `MakeTransform`.
5. The build uses **`/EHa`** on purpose: `__try/__except` in `GenerateGuarded`, `CallCastGuarded` and `RenderGuarded`
   then also catches C++ exceptions. Keep those wrappers as plain functions without C++ objects in their own frame.
6. **ImGui asserts must log, not abort** (`CdImguiAssert`, `ConfigErrorRecoveryEnableAssert = false`).
7. The overlay must be installed **before** the game creates its swapchain, and the command queue is re-pinned on
   every swapchain creation (the game creates a second one at startup).
8. A pointer the plugin owns is freed exactly once by its owner: decoded thumbnail pixels belong to `DrawFrame`,
   not to `CreateFromPixels`. Everything goes through the private heap, so a double free corrupts our own heap.
9. Every spawned object carries the id of the project it came from (`SpawnedObj::proj`, 0 = placed by hand). That is
   what lets one project be saved back without touching the others, so anything that re-creates an object (undo,
   redo, replace-on-move) has to carry the id along. `g_projDirty` marks a project whose objects changed since its
   load or save; `g_loading` keeps LoadProject's own spawns from marking it.

## Working in this repo

- Comments explain **why**, in English, dense one-liner style, no emoji. UI strings are English; `release/README.md`
  is German. Match the surrounding style instead of reformatting.
- Git Bash heredocs mangle backslashes: write C++ files and anything with Windows paths using the Write/Edit tools,
  not `cat <<EOF`. `python - <<'PY'` also eats `\x` escapes - prefer a real file.
- `python scripts\render_thumbs.py` needs `MSYS_NO_PATHCONV=1` in Git Bash, otherwise the filter argument is emptied.
- `pycrimson` on PyPI is an unrelated squatter. Use the clone in `tools/pycrimson` (editable, patched).
- `scripts/ingame_test.py` spawns a prefab list in the running game over the HTTP API and screenshots each one (`--launch`
  starts the game and continues the save). **Run it only when the user asks for an in-game test**, never on your own to verify
  a change: it takes over the user's PC (game window focus, keystrokes).
- There is no test suite; the user verifies in game. Pure logic (file parsers, name handling) can be checked by
  copying the function into a scratch `.cpp` and running it standalone - do that before asking for an in-game test.
- The exe build number is logged at attach (`game build 1.0.0.2944`). Ask for it in any bug report after a patch.
