# World Builder – In-Game Prefab Editor & Modding SDK

**Place any of the game's 48,000+ prefabs anywhere in the world, live, without leaving the game. Torches, doors, chests and every other gimmick come alive: they are spawned through the game's own spawn path.**

Discord: [Crimson Desert Modding](https://discord.gg/HfkShRJZU) for help, builds, feature requests and everything else about modding this game.

World Builder is an in-game editor for Crimson Desert. Open it with a hotkey, browse every prefab the game ships (walls, ruins, furniture, props, gimmicks, nature), put them in front of your character, walk around them, move, turn, tilt and scale them with the keyboard or a Blender-style mouse gizmo, drop them onto the ground, and save the result as a project that loads again on the next start. It also exposes a small C API so other mods can spawn and move objects through it.

No game files are modified. Nothing is written into the game's own save data. Everything the mod places lives in its own project files.

## Quick start

1. Press **Insert** in the game world. The editor opens in edit mode: the mouse belongs to the menu, the world keeps running.
2. Type a word into the search box, pick a card or a row, press **PLACE** (or double-click).
3. The object appears in front of your character with the mouse gizmo active. Drag the arrows, rings, center point or scale handles to position it. Use the placement HUD to Drop, Cancel, level or snap it to the ground; placing/selecting something else or double-clicking into the open also finishes the placement.
4. Prefer a small panel? Click **dock**: the narrow side window can switch between Browser, Scene, NPCs, Project and Time & Weather, and stays open while you work.
5. **Home** switches between editing and transparent click-through player control; keyboard and mouse go fully to the game. Use the **free camera** button for flight, or enable automatic free camera on editor open in Settings. **Insert** hides the editor.

## Features

- **Prefab browser**: category tree, full-text search (words in any order), tag filters, a "meshes only" filter, favorites and your own named collections
- **List or cards**: the matches as a list or as tiles with the preview image; the dock shows one to four per row, sized to the window
- **Textured preview images** for every prefab, rendered on your machine with the game's own textures. The mod asks the game's resource loader for the files: no archive code, no keys, nothing extracted, nothing redistributed
- **Placement mode**: newly placed or grabbed objects are edited directly with the mouse gizmo; the placement HUD provides Drop, Cancel, level, snap and To ground actions
- **Mouse gizmo**: arrows move along the object's axes, the center dot slides over the ground, the green ring turns, the red and blue rings tilt, and the cubes scale
- **Full rotation**: yaw, pitch and roll through the gizmo and the Scene tab, with a level action to remove tilt
- **Snapping**: grid steps (0.1 to 2 m) and angle steps (5 to 90 degrees) apply to gizmo movement and rotation
- **Snap to ground**: "To ground" on the placement HUD or in the Scene tab drops objects onto the surface below them using the game's own physics probe
- **Click to select**: in edit mode a click on a placed object selects it, a double-click grabs it. Selected objects are outlined
- **Scene tab**: placed objects and World Builder-managed NPCs are mixed in the same list and card view, the same named groups, and the same selection model. World clicks and box selection can hit either type; Ctrl and Shift multi-select can mix them. Common move, group, focus, delete, notes and history actions work on the combined selection, while object-only transform tools stay object-only
- **Terrain brush**: raise, lower and flatten the ground in the world (radius 2 to 60 m). The ground and its collision change live, no reload: walk into the dip you just painted. Undo, redo, saved with the project, back on autoload. No game files are modified, the change is applied in memory as the game reads its height maps
- **Travel**: go to any coordinates or your own saved spots through the game's own fast travel
- **Time & Weather**: set the visual time of day, freeze the day/night lighting without pausing gameplay, restore native time progression, and control clear sky, rain, snow, clouds and wind. The controls are also available in the dock
- **NPC browser and spawning**: the NPC page stays focused on browsing and spawning: search, list or cards, count presets up to 500, line, matrix and circle formations, footprint feedback, normal AI or Hold, and drag-to-world spawning with ground probing. Spawned NPCs are managed directly beside objects in **Scene**
- **Line and circle tools**: N copies in a row or on a ring in front of you, grouped, handed to the placement mode
- **Projects**: objects and managed NPCs are equal project entities. Loading a project recreates both; unloading removes both from the live scene without deleting the file. The project page has explicit Load, Reload, Save, Unload and Autoload actions, plus an action to attach unassigned objects and NPCs to an existing project
- **Real-time project auto-save**: a manual master switch controls immediate saving. When enabled, every committed edit to an entity already belonging to a project is written back on the next editor frame. New unassigned entities are never attached silently
- **Backward-compatible files**: existing `.cdproj` object rows from older versions remain readable. Current files keep those rows compatible and add named groups, notes and managed NPC records as optional metadata records
- **Controls**: object placement uses the mouse gizmo by default; optional keyboard placement (off by default, keys configurable) can be switched on in Settings
- **Modding SDK**: `cdk_spawn`, `cdk_move`, `cdk_remove`, `cdk_player_pos` and friends, callable from any other ASI mod
- **Interactive objects**: every gimmick prefab (torches, lamps, doors, chests, campfires, levers - 16,591 of them) is spawned through the game's own spawn path and behaves like the real thing: light a torch, open a chest, knock a stand over. Select, drag, rotate, undo and save them like any other object. The game provides the spawn template by itself a few steps after loading
- **Update-tolerant**: game functions are located by signature and class name at startup. After a game patch the mod either works or disables itself cleanly and tells you why

## Controls

| Key | Action |
| --- | --- |
| Insert | show / hide the editor |
| Home | edit / player-control mode; the transparent editor does not capture keyboard or mouse |
| Placement HUD | Drop / Cancel / To ground / level / snapping |
| Mouse gizmo | move / rotate / tilt / scale the carried object or selection |
| Placing or selecting something else, or double-click into the open | finish the current placement |
| Scene tab | Ctrl+A select all, Ctrl+G group, Ctrl+Z / Ctrl+Y undo / redo, Ctrl+C / Ctrl+V copy / paste, Delete |

## Requirements

- Crimson Desert (Steam), tested with builds 1.0.0.2850 and 1.0.0.2944
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) installed as `bin64\winmm.dll` (DMM and Vortex users usually have it already)

## Installation

1. Install Ultimate ASI Loader if you do not have it: download Ultimate-ASI-Loader_x64.zip, copy the included dinput8.dll into `<game>\bin64\` and rename it to winmm.dll.
2. Copy `cdmodkit.asi` from this archive into `<game>\bin64\`. The `cdmodkit` runtime folder and `settings.txt` are created automatically on first start.
3. Start the game through Steam. The log is written to `bin64\cdmodkit\cdmodkit.log` (set `console=1` in settings.txt if you want a console window).

On the first start the mod renders preview images for all prefabs in the background (low priority, about 20 minutes). The prefab you select is always rendered first.

## Known limitations

- Gimmick prefabs (everything under /object/cd_gimmick/) are real, interactive game objects. All other prefabs are visual with collision only.
- Interactive objects need a spawn template the game provides by itself: right after loading a save, walk a few meters before they appear (the Scene tab says so while they wait).
- Objects exist for the running session only. Load a project (or use autoload) after restarting.

## For mod authors

`sdk\cdmodkit_api.h` documents the exported functions. Get the module with `GetModuleHandleA("cdmodkit.asi")` and resolve the functions with `GetProcAddress`. Everything is queued to the game thread for you.

## Questions, requests, problems

Join the [Crimson Desert Modding](https://discord.gg/HfkShRJZU) Discord: questions, feature requests and bug reports are all welcome there, the comments here work too.

When something goes wrong, everything is logged in `bin64\cdmodkit\cdmodkit.log`. Post the last 30 lines, especially lines containing "fault", "imgui assert" or "RESOLVE FAILED".

## Credits and thanks

- **dofo7777** for extensive testing, ideas and videos. Many of the placement and input improvements exist because of that feedback.
- **Shin234** for Master Looter, whose input layer this mod's virtual cursor is adapted from, and for showing the way into this engine.
- **LiangWood** for tracking down the missing prefab list with DMM, **Alduin1991** for the mounted-coordinates report, and the author of CrimsonRoute for the transform snapshot notes.
- **Nostyxx** for [CrimsonWeather](https://github.com/Nostyxx/CrimsonWeather): thanks for finding the offsets and signatures of the game's time and weather data that World Builder's time-of-day and weather controls build on (no code taken), and the author of **CrimsonRoute** for allowing us to use its overlay capture code.
- The whole Crimson Desert modding community: the pycrimson and CDMW projects for the archive and mesh format research, and everyone sharing findings in the open.

Third-party code: MinHook (BSD-2), Dear ImGui (MIT), stb_image / stb_image_write (public domain), input layer adapted from Master Looter and Trinity (MIT). No game assets are included.
