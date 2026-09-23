# World Builder - in-game world editor for Crimson Desert

An ASI plugin that adds a live prefab editor to Crimson Desert: browse the 48,000+ prefabs the game ships, place them in
front of your character, move, rotate, scale and group them with hotkeys or a mouse gizmo, and save builds as projects
that load again at the next start. No game files are modified and nothing is written into the game's own save data.

- Downloads and user guide: the Nexus page (link in `release/NEXUS_PAGE.md`) or `release/WorldBuilder-v<version>.zip`
  built from this repository; the user guide is `release/README.md` (German).
- Discord: **Crimson Desert Modding** - https://discord.gg/HfkShRJZU
- Support the project: https://ko-fi.com/daebak91

An experimental branch of the work spawns objects through the game's own server spawn path, which makes them
interactive (a torch placed that way can be lit and put out); see `notes/GIMMICK_SPAWN.md` for the research behind it.

## Install (players)

1. Install **Ultimate ASI Loader** if you do not have it: download `Ultimate-ASI-Loader_x64.zip` from
   https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases and copy the contained `dinput8.dll` to
   `<game>\bin64\winmm.dll` (renamed). Mod managers for Crimson Desert usually install it already.
2. Copy `cdmodkit.asi` and the folder `cdmodkit\` from the release zip into `<game>\bin64\`.
3. Start the game through Steam. Press **Insert** in the world to open the editor, **Home** to switch between edit and
   play mode. The log is `bin64\cdmodkit\cdmodkit.log`.

## Interface language

Choose **Settings → Language** in the editor. The interface supports English, Simplified Chinese, Traditional Chinese,
German, French, Korean, Japanese, Spanish, Brazilian Portuguese, Russian, and Turkish, plus automatic system-language
detection. The language names in the selector follow the currently selected interface language. Keep `locales.tsv` in
`bin64\cdmodkit`; untranslated text falls back to English.

## Build from source

Requirements: Visual Studio 2026 Build Tools (MSVC, x64; `asi/cdmodkit/build.bat` calls `vcvars64.bat`, adjust the path
if yours differs), Python 3 with the `lz4` module (packs the prefab index into the binary), and two dependencies placed
under `tools/` (not part of the repository):

```
tools/minhook   https://github.com/TsudaKageyu/minhook        (BSD-2-Clause)
tools/imgui     https://github.com/ocornut/imgui   v1.91.5   (MIT)
```

Then:

```bash
asi\cdmodkit\build.bat
```

Without Visual Studio, `asi\cdmodkituild-mingw.bat` builds the same sources with GCC / MinGW-w64 (g++, gcc and windres on
PATH; the fault guards use the plugin's exception handler there instead of MSVC's `__try`).

The plugin lands in `asi/cdmodkit/build/cdmodkit.asi`. Copy it together with `asi/cdmodkit/data/prefabs.tsv`,
`settings.txt`, `errnames.txt`, and `locales.tsv` (into `bin64\cdmodkit\`) while the game is not running.
`python scripts/make_release.py <version>` bumps the version strings, builds, and assembles the release zip.

## Repository layout

| Path | What |
| --- | --- |
| `asi/cdmodkit/` | the plugin: `cdmodkit.cpp` (hooks, spawning, projects), `editor.cpp` (ImGui editor), `overlay.cpp` / `input.cpp` (D3D12 overlay, input), `thumbgen.cpp` (preview renderer), `diag.cpp` (reverse-engineering aids) |
| `asi/cdmodkit/cdmodkit_api.h` | the C API other ASI mods can call (`cdk_spawn`, `cdk_move`, `cdk_remove`, `cdk_player_pos`, ...) |
| `HTTP_API.md` | local HTTP API for prefab search, scene objects and project operations |
| `asi/cdmodkit/data/` | `prefabs.tsv` (prefab paths and tags), default `settings.txt`, `errnames.txt`, `locales.tsv` (UI translations) |
| `scripts/` | build helpers and the offline reverse-engineering tools (`xref.py`, `disasm.py`, `rtti_static.py`, `parse_parc.py`, ...); the pack-reading scripts need pycrimson, bier and CDMW under `tools/` |
| `notes/FORMATS.md` | how the game works from the plugin's point of view: signatures, struct offsets, the spawn recipe, file formats |
| `notes/GIMMICK_SPAWN.md` | the server spawn path research (interactive objects) |
| `notes/*.tsv`, `notes/rtti_static.txt` | identifier tables read from the game (gimmick keys, error names, class names) |
| `release/` | user guide, Nexus page text, changelogs |

Game functions are located by signature and RTTI class name at startup, never by fixed addresses; after a game update the
plugin either works or disables the affected feature and says so in the log. The research hooks (trace-gated, `[gimmick]`,
`[vt]`, `[core]` log lines) use build-specific signatures and are meant for development.

## Contributing

Issues and pull requests are welcome. For questions and builds the Discord server is the fastest way. Please keep in mind
that most of the interesting parts are reverse-engineered game internals; `notes/FORMATS.md` is the place to read first.

## License

MIT, see `LICENSE`. Third-party code and borrowed format knowledge are listed in `THIRD_PARTY_NOTICES.md`.
Crimson Desert is a trademark of Pearl Abyss; this project is not affiliated with or endorsed by Pearl Abyss.
