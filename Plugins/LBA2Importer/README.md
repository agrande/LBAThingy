# LBA2 Importer for Unreal Engine 5.8

Imports *Little Big Adventure 2* game data from your own installed copy of the game (Steam or GOG)
into native Unreal assets. No game data ships with this plugin; everything is converted on your
machine from the original HQR/ILE/OBL files.

## Usage

1. Enable the plugin (it lives in `Plugins/LBA2Importer`; it enables Unreal's experimental Water
   plugin, used for the seas) and open **Tools > LBA2 Importer**.
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

## Look

Generated levels render with Unreal's lighting (Lumen GI, shadows) and then put LBA2's palette back
on top, through a `Look` post-process volume. It holds two looks, both instances of the same
post-process material: **Original**, close to the original's pixels, and **Enhanced**, a graded
take with deeper shadows, bolder dots and more color. *Look Mode* in the importer settings picks the
one levels start with; `lba.Look.Mode 0|1` switches live (values in between blend them).

- **Palette shading** (`M_LBA_PaletteShading`, `Shaders/Private/LBAPaletteShading.ush`). Every island
  palette ships a shade table that says, for each of its 256 colors, which color it becomes in each
  of 16 shade rows (row 12 is the unshaded color). Those rows mostly move inside a 16-color ramp,
  which is what gives the original its painted shadows and highlights. For each pixel, the palette
  color is found from the G-buffer base color, the light the surface receives (scene luminance /
  base color luminance) picks a shade row linearly, like the original's Gouraud intensity, and the
  light's color is kept as a tint. It runs before TSR, in linear HDR, which the volume makes equal
  to the displayed colors: manual exposure 1 and no tone curve.
- **World-space dither** between shade rows, in two patterns (`DotPattern`, or `lba.Look.Pattern 0|1`):
  - *Fractal dots*: Rune Skovbo Johansen's Surface-Stable Fractal Dithering.
    `Shaders/Private/LBADither3D.ush` and `Source/LBA2Editor/Private/Import/LBADither3DTextures.*`
    are ports of Dither3D and stay under the Mozilla Public License 2.0
    (`Resources/ThirdParty/Dither3D/LICENSE.md`).
  - *Halftone*: a 45 degree screen of round dots growing into a checkerboard, like printed comics
    (`Shaders/Private/LBAHalftone.ush`).

  The dots stick to surfaces as the camera moves and keep about the same size on screen. Characters
  are marked with a custom stencil (`LBA::MovingObjectStencil`, needs `r.CustomDepth=3`) and get smooth
  shading instead, since world dots would slide over them.
- **Grade** (enhanced look): a contrast curve on the received light that keeps the island's sunlit
  ground where the original has it (`ContrastPivot`, from the terrain data) and pushes shade down and
  sun-facing slopes up; shade rows mixed two apart (`RowStep`) with bigger dots, so the dither reads as
  a comic screen; shadows tinted with the island's sky color and highlights warm; exposure, contrast
  and saturation over the whole image.
- **Sky**: Unreal's sky atmosphere, tinted (*Sky Luminance Factor*) so its average matches the island's
  original sky (the cloud half of its sky/sea texture page), with Unreal's volumetric clouds (coverage
  and color per island, storm clouds over Citadel). The Emerald Moon has no atmosphere.
- **Sea**: on Twinsun (Citabau, Citadel, the desert) Unreal's water (Water plugin): a water zone over the
  island and an ocean filling it, out to the horizon, with Epic's ocean material tinted towards the
  original sea's color under the island's lights (`MI_<Island>_Ocean`), and a calm swell or storm
  waves (`<Root>/Rendering/Water/Waves_LBA_Calm|Storm`, kept on reimport). The terrain renders into the
  zone as the ground (`WaterTerrainComponent`). The palette shading recognizes water (single layer
  water shading model) and draws it as cartoon water (`Shaders/Private/LBAWaterCartoon.ush`): the
  island's original sea texture, at the original's scale but without visible repetition (cells with
  irregular borders, each showing it shifted, turned and mirrored: *SeaCellSize*, *SeaVariation*),
  drifting and bent by the waves, each texel
  shaded along its palette color's shade path in flat bands that follow the water's light averaged
  around it (deep water, reflections, distance; the sky's reflections roll off into the brightest
  rows), white foam lines hugging the coast and anything standing in the water (piers, hulls, Twinsen
  wading, found on screen) with more lines leaving it, and white sparkles where the sun glints. Real
  water is darker than the original's painted sea, so each level stores the light at which water
  shows the sea's colors (*WaterNeutralLight*, from a model of the water's scattering calibrated
  against the original; the storm sea is darker, its foam grey). Other worlds keep the original's
  textured plane (Zeelich's cloud sea and gas, the moon's ground).
- **Fog**: exponential height fog in the palette's fog color, with volumetric fog (light shafts),
  starting at the original's fog distance so everything the original drew stays crisp, and fading the
  rest of the island. Unreal fogs surfaces before any post-process runs, so the palette shading evaluates the
  same fog again (height fog equations, and the volumetric fog lit with the height fog's colors, which
  needs `r.Lumen.TranslucencyVolume.Enable=0`), takes it off, shades, and puts it back
  (`lba.Look.Debug 4` shows surfaces without fog). Its copy of the fog settings follows the level's fog
  actor during play. The original look replaces it with the original's linear distance fog, from each
  section's fog distances (scaled by *Fog Distance Scale*), applied after shading like the original's
  fog tables.
- **Lights**: islands are lit so that sunlit ground lands on the shade rows the original's baked
  terrain lighting uses (from the island's terrain data), with the sun at the original's angle, and
  ground in the shade keeps about two thirds of that (the original has no cast shadows). The storm is
  lit evenly by a uniform overcast sky.

Customizing (material instances in `<Root>/Rendering`, children inherit what they don't override):

| Instance | Scope | Kept on reimport |
|---|---|---|
| `MI_LBA_Look_Enhanced`, `MI_LBA_Look_Original` | every level | yes, unless a newer importer changes their defaults |
| `MI_LBA_Enhanced_<Palette>` | levels using that palette (per island tweaks) | yes, same rule |
| `MI_LBA_Original_<Palette>`, level `MI_<Level>_<Look>Look` (fog, contrast pivot) | generated | no |
| level `MI_<Level>_Clouds` (child of Unreal's `m_SimpleVolumetricCloud_Inst`) | generated | no |
| island `MI_<Island>_Ocean` / `_OceanFar` (children of the Water plugin's ocean materials) | generated | no |
| `Water/Waves_LBA_Calm`, `Water/Waves_LBA_Storm` (Gerstner wave assets) | every water level | yes, unless a newer importer changes their defaults |

Main parameters: *LightContrast*, *RowStep*, *Exposure*, *Contrast*, *Saturation*, *ShadowTint* /
*HighlightTint* and their amounts (Grade); *DotPattern*, *DotSpacing* (pixels), *DotContrast*,
*DitherAmount* (Dither); *Strength* (0 = Unreal's image), *KeepLuminance* (keep Unreal's brightness,
use only the ramps' hue), *LightTint* (Palette); *WaterStrength* (0 = Unreal's water), *WaterLightTint*,
*WaterHighlightRange* (Water); *SeaDrift* (cm/s), *SeaWobble*, *WaterSmoothing* (band calmness),
*FoamWidth*, *FoamSpeed*, *FoamBrightness*, *SparkleLight* (Water Cartoon). To try values live, `lba.Look.Set <Parameter> <Value>`
(or `R G B` for colors; no value removes the override). Other console variables:
`lba.Look.Palette 0|1`, `lba.Look.Dither 0|1`, `lba.Look.Debug 1|2|3|4` (base color, matched palette
color, shade row / light, surfaces without fog).

## Conventions

- LBA space (right-handed, Y up) maps to Unreal as `(x, z, y) * scale`; LBA yaw `beta` becomes `-beta`.
- Characters face +Y like Unreal mannequins, so put them in an `ACharacter` with the usual -90 yaw.
- Vertex color carries the palette albedo (stored sRGB by Unreal's mesh build, decoded in the
  materials); vertex alpha carries the original baked shade row / 24. The `BakedShading` parameter
  on the `M_LBA_*` masters multiplies it back in (off by default: Unreal lights the scene and the
  palette shading needs the exact palette color as base color).
- Islands use their own palette (RESS.HQR XPL entries); interiors and characters use the Citadel
  (Citabau) palette, like the original.
- Generated master materials carry a version stamp (a comment node): a newer importer rebuilds
  them in place, otherwise your edits are kept.
- Community names (scene, entity, actor and zone names) come from lba2remake's metadata, bundled in
  `Resources/CommunityMetadata` with its MIT license.

## Roadmap

- [x] L0 Foundation: HQR/LZ reader, palette, coordinate mapping, settings panel, commandlet
- [x] L1 Islands: terrain, decor, levels, environment presets
- [x] L2 Interiors: brick projection, kit pieces, collision, smart backfaces, remake 3D models
- [x] L3 Characters: skeletons, skinned meshes, animations with root motion, notifies, entity assets
- [x] L3b Playable Twinsen, authentic controls (root motion, behaviours, actions)
- [x] Original look: island palettes, palette shading post-process, world-space dither, original fog,
  sky and sea, calibrated lights
- [x] Enhanced look: graded palette shading, bolder fractal or halftone dots, live look switch
- [x] Unreal sky atmosphere and volumetric clouds per island mood, height and volumetric fog taken
  into account by the palette shading, sun and sky light calibrated against them
- [x] Sea with Unreal's Water plugin on Twinsun: ocean, calm or storm waves, cartoon water with the
  original sea textures, foam lines and sparkles
- [ ] Look, next: Citadel storm (lamppost lights, rain, lightning), cloud shadows, interior lights,
  sprites; shallows (a seabed sloping down from the coast), Zeelich's cloud sea
- [ ] L3c Modern controls for Unreal (motion matching or blendspaces: to be discussed)
- [ ] L4 Scenes: actors, zones (typed trigger volumes), track points, hero start, ambience, per-scene levels
- [ ] L5 Scripts: life/move scripts (on hold; interpreter recommended over Blueprint generation)
- [ ] L6 Text (String Tables), voices, music, sprites, inventory objects, holomap, screens
- [ ] Procedural wall/pattern builder using interior kit pieces
- [ ] Light-only remake GLBs as point lights
