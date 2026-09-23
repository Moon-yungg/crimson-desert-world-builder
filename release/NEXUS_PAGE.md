# World Builder – In-Game Prefab Editor & Modding SDK

**Place any of the game's 48,000+ prefabs anywhere in the world, live, without leaving the game. Torches, doors, chests and every other gimmick come alive: they are spawned through the game's own spawn path.**

Discord: [Crimson Desert Modding](https://discord.gg/HfkShRJZU) for help, builds, feature requests and everything else about modding this game.

World Builder is an in-game editor for Crimson Desert. Open it with a hotkey, browse every prefab the game ships (walls, ruins, furniture, props, gimmicks, nature), put them in front of your character, walk around them, move, turn, tilt and scale them with the keyboard or a Blender-style mouse gizmo, drop them onto the ground, and save the result as a project that loads again on the next start. It also exposes a small C API so other mods can spawn and move objects through it.

No game files are modified. Nothing is written into the game's own save data. Everything the mod places lives in its own project files.

## Quick start

1. Press **Insert** in the game world. The editor opens in edit mode: the mouse belongs to the menu, the world keeps running.
2. Type a word into the search box, pick a card or a row, press **PLACE** (or double-click).
3. The object appears in front of your character. Move it with the placement keys (numpad by default) or press Numpad 5 and drag the gizmo. Place the next one, click another object or double-click into the open and it stays where it is (**Enter** works too); **Backspace** cancels while you hold it.
4. Prefer a small panel? Click **dock**: a narrow side window with search, one to four columns of cards and PLACE. It stays open while you place.
5. **Home** switches between edit mode (menu takes the input) and play mode (game takes the input, window stays visible). Save your build in the Project tab.

## Features

- **Prefab browser**: category tree, full-text search (words in any order), tag filters, a "meshes only" filter, favorites and your own named collections
- **List or cards**: the matches as a list or as tiles with the preview image; the dock shows one to four per row, sized to the window
- **Textured preview images** for every prefab, rendered on your machine with the game's own textures. The mod asks the game's resource loader for the files: no archive code, no keys, nothing extracted, nothing redistributed
- **Placement mode**: the object stays where it is while you walk around it. Move away / closer, left / right, up / down, rotate about its center, scale, bring it back in front of you
- **Mouse gizmo**: Numpad 5 hands the mouse to World Builder. Arrows move along the object's axes, the center dot slides over the ground, the green ring turns, the red and blue rings tilt, the cubes scale
- **Full rotation**: yaw, pitch and roll, with sliders in the Scene tab and a "level" key
- **Snapping**: grid steps (0.1 to 2 m) and angle steps (5 to 90 degrees), one step per key press
- **Snap to ground**: one key drops the carried object onto the surface below it, "To ground" in the Scene tab does the same for the selection. Uses the game's own physics probe, so it lands on terrain, floors and other objects alike
- **Click to select**: in edit mode a click on a placed object selects it, a double-click grabs it. Selected objects are outlined
- **Scene tab**: every placed object with position and distance; multi-select (Ctrl / Shift + click), groups (Ctrl+G) as collapsible rows, undo / redo (Ctrl+Z / Ctrl+Y), copy / paste with orientation (Ctrl+C / Ctrl+V), duplicate, delete, To ground, Grab, Remove duplicates
- **Line and circle tools**: N copies in a row or on a ring in front of you, grouped, handed to the placement mode
- **Projects**: save and load whole builds (absolute world coordinates, groups and tilt included), import .cdproj files shared by others, optional autoload when the game starts (tick as many projects as you like; they are all placed into the same world)
- **One scene, several projects**: every object knows which project it came from. The scene has a tab per loaded project plus "new" for what you just placed, a star marks unsaved changes, and each project is written back into its own file - so you can build inside a loaded project without having to clear it first
- **Every key is yours**: the editor hotkeys and all placement keys can be changed in the Settings tab or settings.txt. No numpad? Bind WASD, the arrows or anything else
- **Modding SDK**: `cdk_spawn`, `cdk_move`, `cdk_remove`, `cdk_player_pos` and friends, callable from any other ASI mod
- **Interactive objects**: every gimmick prefab (torches, lamps, doors, chests, campfires, levers - 16,591 of them) is spawned through the game's own spawn path and behaves like the real thing: light a torch, open a chest, knock a stand over. Select, drag, rotate, undo and save them like any other object. The game provides the spawn template by itself a few steps after loading
- **Update-tolerant**: game functions are located by signature and class name at startup. After a game patch the mod either works or disables itself cleanly and tells you why

## Controls (defaults, all rebindable)

| Key | Action |
| --- | --- |
| Insert | show / hide the editor |
| Home | edit mode (menu takes the input) / play mode (game takes the input) |
| Numpad 8 / 2 | move the object away from you / closer |
| Numpad 4 / 6 | move it left / right |
| Numpad 9 / 3 | move it up / down |
| Numpad 7 / 1 | rotate it about its center |
| Numpad + / - | scale |
| Shift | fast |
| Numpad 0 | bring it back in front of you |
| Numpad . | snapping on / off |
| Numpad 5 | mouse gizmo on / off |
| Numpad * | level (remove the tilt) |
| Numpad / | snap to ground |
| Enter, placing or selecting something else, double-click into the open / Backspace | done / cancel or put back |
| Scene tab | Ctrl+A select all, Ctrl+G group, Ctrl+Z / Ctrl+Y undo / redo, Ctrl+C / Ctrl+V copy / paste, Delete |

## Requirements

- Crimson Desert (Steam), tested with builds 1.0.0.2850 and 1.0.0.2944
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) installed as `bin64\winmm.dll` (DMM and Vortex users usually have it already)

## Installation

1. Install Ultimate ASI Loader if you do not have it: download Ultimate-ASI-Loader_x64.zip, copy the included dinput8.dll into `<game>\bin64\` and rename it to winmm.dll.
2. Copy `cdmodkit.asi` and the `cdmodkit` folder from this archive into `<game>\bin64\`. Mod managers (DMM, Vortex) work too: the prefab list is built into the plugin and written out if the folder is missing.
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

## Support

If World Builder is useful to you and you want to say thanks, you can buy me a coffee: [ko-fi.com/daebak91](https://ko-fi.com/daebak91). Never expected, always appreciated.

## Credits and thanks

- **dofo7777** for extensive testing, ideas and videos. Many of the placement and input improvements exist because of that feedback.
- **Shin234** for Master Looter, whose input layer this mod's virtual cursor is adapted from, and for showing the way into this engine.
- **LiangWood** for tracking down the missing prefab list with DMM, **Alduin1991** for the mounted-coordinates report, and the author of CrimsonRoute for the transform snapshot notes.
- The whole Crimson Desert modding community: the pycrimson and CDMW projects for the archive and mesh format research, and everyone sharing findings in the open.

Third-party code: MinHook (BSD-2), Dear ImGui (MIT), stb_image / stb_image_write (public domain), input layer adapted from Master Looter and Trinity (MIT). No game assets are included.
