# LBAThingy

An Unreal Engine 5.8 playground for *Little Big Adventure 2*. The original game's islands, interiors,
characters, scenes and sounds are converted into native Unreal assets from your own copy of the game, then
played in Unreal: Twinsen with the original or modern controls, the original palette look rebuilt as a
post-process (or an enhanced take on it), Unreal water and skies, Citadel's storm, a rideable Dinofly, the
ferry and cars, a travel menu, and a prototype globe mode that puts the islands on a round Twinsun.

**No game data is included in this repository.** Everything from the game is generated on your machine by the
importer and lands in `Content/LBA2/`, which is not versioned.

## Getting started

1. You need Unreal Engine 5.8 and an installed copy of *Little Big Adventure 2* (Steam or GOG). Optionally:
   a checkout of [lba2remake](https://github.com/LBALab/lba2remake) for its community 3D models, and *Little
   Big Adventure 1* (GOG) for its towns on the globe.
2. Open `LBAThingy.uproject` and let it build the C++ modules. Until the import has run, the default maps
   (under `Content/LBA2/`) are missing; that is expected.
3. Open **Tools > LBA2 Importer**, check the game folder (Steam and GOG installs are detected), and press
   **Import**.

Everything else (the importer, settings, controls, look, globe mode, tools and tests) is documented in the
plugin's [README](Plugins/LBA2Importer/README.md).

## Layout

- `Plugins/LBA2Importer`: the importer (editor module), the gameplay and rendering code (runtime module),
  shaders, Python and PowerShell tools, golden tests.
- `Source/LBAThingy`, `Content/`: the game module and content from Unreal's Third Person template.
- `Content/LBA2_Modded`: a couple of old hand-made experiments on imported levels (they need the import).

## License

The code in this repository is released under the [MIT License](LICENSE), except:

- `Plugins/LBA2Importer/Shaders/Private/LBADither3D.ush` and
  `Plugins/LBA2Importer/Source/LBA2Editor/Private/Import/LBADither3DTextures.cpp` are ports of [Dither3D](https://github.com/runevision/Dither3D) by Rune Skovbo Johansen and stay under the
  Mozilla Public License 2.0 (`Plugins/LBA2Importer/Resources/ThirdParty/Dither3D/LICENSE.md`).
- `Plugins/LBA2Importer/Resources/CommunityMetadata` and the travel menu's location icons come from
  [lba2remake](https://github.com/LBALab/lba2remake) under its MIT License (`LICENSE-lba2remake.txt`).
- Content and code from Unreal Engine templates are Epic Games' and covered by the Unreal Engine EULA.

## Disclaimer

This is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or connected to
[2.21], Adeline Software International or any other rights holder of *Little Big Adventure*. *Little Big
Adventure*, *Twinsen* and related names are trademarks of their respective owners. You need your own legally
obtained copy of the game to use the importer; no game assets are distributed here.
