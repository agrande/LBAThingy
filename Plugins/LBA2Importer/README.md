# LBA2 Importer for Unreal Engine 5.8

Imports *Little Big Adventure 2* game data from your own installed copy of the game (Steam or GOG)
into native Unreal assets. No game data ships with this plugin; everything is converted on your
machine from the original HQR/ILE/OBL files.

## Usage

1. Enable the plugin (it lives in `Plugins/LBA2Importer`) and open **Tools > LBA2 Importer**.
2. Set **Game Directory** to your LBA2 install (Steam/GOG installs are auto-detected).
3. Optionally set **Remake Directory** to a checkout of
   [lba2remake](https://github.com/LBALab/lba2remake) to use the community's 3D replacement models
   for interiors (MIT licensed).
4. Pick layers and press **Import**. Re-importing updates assets in place and only replaces the
   actors the importer generated inside levels; anything you add yourself is kept.

Headless (batch) import:

```
UnrealEditor-Cmd.exe LBAThingy.uproject -run=LBA2Import [-GameDir="..."] [-RemakeDir="..."]
    [-Layers=Islands,Interiors,Characters,Audio] [-Islands=CITABAU,DESERT] [-Grids=0,3] [-Entities=0,1]
    [-NoRemake] [-Root=/Game/LBA2]
```

World scale lives in **Project Settings > Plugins > LBA2** (default 0.125 cm per LBA unit: a brick is
64x32x64 cm, Twinsen about 155 cm).

## What gets imported

| Layer | Source | Unreal result |
|---|---|---|
| Islands | `*.ILE`, `*.OBL` | Per island: ground texture, decor static meshes (kit pieces), one terrain static mesh per section, level `L_<ISLAND>` with all decor placed, sea, sky/fog preset, player start |
| Interiors | `LBA_BKG.HQR` | Brick atlas per library, one static mesh per library layout (kit pieces with convex collision), one mesh per grid with exact slope collision, level `L_Grid_NNN_<Scene>` |
| Remake models | lba2remake checkout | GLB replacement models as static meshes under `Remake/`, placed in interiors instead of the matching bricks |
| Characters | `RESS.HQR` (entities), `BODY.HQR`, `ANIM.HQR` | One skeleton per character family, skeletal meshes per body, animation sequences with root motion and notifies (footsteps, hits, sounds, throws), `ULBAEntityAsset` per entity, `L_CharacterGallery` |
| Audio | `SAMPLES.HQR` | Sound waves (converted to 16-bit PCM) |

## Playing as Twinsen

Generated levels use `LBAGameMode` (toggle with *Play As Twinsen* in the importer settings), so
pressing Play spawns `ALBACharacter`. Any other map can do the same with a GameMode override.

| Key | Authentic mode (default) |
|---|---|
| Up/W, Down/S | Walk forward / backward (movement comes from the animation's root motion) |
| Left/A, Right/D | Turn in place (turn animations), or steer while walking |
| Space | Behaviour action: Normal search, Athletic jump (running jump when moving), Aggressive punch/kick, Discreet crouch |
| F5-F8 or 1-4 | Normal, Athletic, Aggressive, Discreet |
| Mouse | Orbit the camera (it recenters behind Twinsen when he walks) |
| Tab | Toggle the experimental Modern mode (camera-relative movement, in-place animations) |

`ALBACharacter` loads its behaviour entities from **Project Settings > Plugins > LBA2 > Hero** and can be
subclassed in Blueprint. `SetMoveInput` and `DoBehaviourAction` let AI or scripts drive it.
`ULBAAnimInstance` plays LBA animations natively (crossfades, LBA loop frames, root motion) without
an Animation Blueprint.

## Conventions

- LBA space (right-handed, Y up) maps to Unreal as `(x, z, y) * scale`; LBA yaw `beta` becomes `-beta`.
- Characters face +Y like Unreal mannequins, so put them in an `ACharacter` with the usual -90 yaw.
- Vertex color carries the palette albedo; vertex alpha carries the original baked shading. The
  `BakedShading` parameter on the `M_LBA_*` masters blends between modern lighting (0) and the
  original look (1).
- Community names (scene, entity, actor and zone names) come from lba2remake's metadata, bundled in
  `Resources/CommunityMetadata` with its MIT license.

## Roadmap

- [x] L0 Foundation: HQR/LZ reader, palette, coordinate mapping, settings panel, commandlet
- [x] L1 Islands: terrain, decor, levels, environment presets
- [x] L2 Interiors: brick projection, kit pieces, collision, smart backfaces, remake 3D models
- [x] L3 Characters: skeletons, skinned meshes, animations with root motion, notifies, entity assets
- [x] L3b Playable Twinsen, authentic controls (root motion, behaviours, actions)
- [ ] L3c Modern controls for Unreal (motion matching or blendspaces: to be discussed)
- [ ] L4 Scenes: actors, zones (typed trigger volumes), track points, hero start, ambience, per-scene levels
- [ ] L5 Scripts: life/move scripts (on hold; interpreter recommended over Blueprint generation)
- [ ] L6 Text (String Tables), voices, music, sprites, inventory objects, holomap, screens
- [ ] Procedural wall/pattern builder using interior kit pieces
- [ ] Light-only remake GLBs as point lights
