# Third-party notices

World Builder is MIT licensed (see `LICENSE`). It builds on and borrows from the following projects.
The full license texts are in the linked repositories; the copyright lines below are reproduced as their
licenses require.

## Compiled into cdmodkit.asi

- **MinHook** (BSD-2-Clause) - Copyright (C) 2009-2017 Tsuda Kageyu - https://github.com/TsudaKageyu/minhook
  Expected at `tools/minhook` when building.
- **Dear ImGui 1.91.5** (MIT) - Copyright (c) 2014-2024 Omar Cornut - https://github.com/ocornut/imgui
  Expected at `tools/imgui` when building.
- **stb_image.h / stb_image_write.h** (MIT or public domain, dual licensed) - Sean Barrett and contributors -
  https://github.com/nothings/stb. Vendored in `asi/cdmodkit/`, license text at the end of each header.

## Adapted code and format knowledge

- **Master Looter** (MIT) - Copyright (c) 2026 Seth. The input layer (window subclass, virtual cursor driven by raw
  mouse deltas) in `asi/cdmodkit/input.cpp` and the D3D12 overlay approach in `overlay.cpp` (no throwaway device)
  are adapted from it.
- **Trinity** (MIT) - Copyright (c) 2026 XeTrinityz. Parts of the input handling in `input.cpp` follow it.
- **pycrimson** (MIT) - Copyright (c) 2026-present LukeFZ - https://github.com/LukeFZ. The reflection serializer
  format used by `thumbgen.cpp` and `scripts/parse_parc.py` was ported from it; the offline scripts import it directly.
- **CDMW** (MIT) - Copyright (c) 2026 Ratrider. The `.pam` static mesh layouts in `thumbgen.cpp` and
  `scripts/render_thumbs.py` were ported from its mesh parser.
- **CrimsonRoute** - the player transform layout and the overlay's DXGI/Streamline interception and swap-chain lifecycle were adapted from the adjacent
  CrimsonRoute implementation. CrimsonRoute-specific map/route rendering, D3D11/HDR paths, diagnostics, and runtime-management code are not bundled here.
- **CrimsonWeather** by **Nostyxx** - https://github.com/Nostyxx/CrimsonWeather. Its public reverse-engineering
  work was used as a technical reference for the optional visual time-of-day and weather bridge in
  `asi/cdmodkit/environment.cpp` (environment/time layout, visual-time freeze concept, and composed weather-table
  fields). World Builder's implementation is independently written; CrimsonWeather source is not bundled here.

## Required at runtime, not bundled

- **Ultimate ASI Loader** (MIT) by ThirteenAG - https://github.com/ThirteenAG/Ultimate-ASI-Loader

## Game data

No game assets are included. `asi/cdmodkit/data/prefabs.tsv`, `notes/gimmickinfo.tsv`, `notes/errnames.txt` and
`notes/rtti_static.txt` list file names, identifiers and class names read from the installed game; preview images are
rendered on the player's own machine from their own installation. Crimson Desert is a trademark of Pearl Abyss.
This project is not affiliated with or endorsed by Pearl Abyss.
