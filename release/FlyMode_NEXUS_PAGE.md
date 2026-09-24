# Fly Mode © dofo7777 and Zappenduster

**Press F7 and fly. A free camera for Crimson Desert: glide over Pywel, look at any castle, cliff or camp from wherever you like, and take the screenshot you always wanted.**

Fly Mode is a small, standalone plugin with no menu and nothing to set up. One key switches it on and off. Your character stays exactly where it is while the camera flies, and the moment you switch off, you are back behind your character as if nothing happened.

## Controls

| Key | Action |
| --- | --- |
| **F7** | Fly Mode on / off |
| **W / A / S / D** | Fly forward, left, back, right |
| **Mouse** | Look around |
| **E** or **Space** | Up |
| **Q** or **Ctrl** | Down |
| **Shift** (hold) | Faster |

The key, the flying speed, the Shift factor and the mouse sensitivity can be changed in `FlyMode.ini`, which appears next to the plugin on the first start.

## What makes it different

The camera is not just a render trick. The game sees the fly camera as its real camera, so the world is drawn, culled and detailed around your view: nothing disappears when you turn, the minimap turns with you, and sound follows the camera. While you fly, your character does not move, attack or aim, and a key you were holding when you switched on is released properly, so nobody runs off on their own.

## Installation

1. Install Ultimate ASI Loader if you do not have it: download Ultimate-ASI-Loader_x64.zip, copy the included dinput8.dll into `<game>\bin64\` and rename it to winmm.dll.
2. Copy `FlyMode.asi` into `<game>\bin64\`.
3. Start the game through Steam and press F7 in the world.

To uninstall, delete `FlyMode.asi` (and `FlyMode.ini` / `FlyMode.log` if you like). No game files are changed and nothing is written into your save.

## Good to know

- The world loads its details around your **character**, not the camera. Fly far away and distant areas get coarser; fly back and they sharpen again.
- **World Builder users:** World Builder already has the same camera in its camera mode (Home, with its own settings in the Settings tab). If World Builder is installed, Fly Mode stays inactive on purpose, so the two never get in each other's way.
- After a game update Fly Mode checks that it still finds everything it needs. If something moved, it stays switched off and says why in `FlyMode.log`, instead of guessing.

## Credits

Fly Mode is a joint work of three people who put their tools and knowledge together:

- **Zappenduster** found how to reach the game's real render camera directly (from his Crimson Desert Telemetry project). That discovery is the foundation the fly camera stands on.
- **dofo7777** and the uploader built the fly mode itself: finding where the game sets its camera every frame, moving it without breaking the picture, making the game's own camera follow so the world stays visible, and the controls.

Thank you to everyone in the Crimson Desert modding community for testing and feedback.
