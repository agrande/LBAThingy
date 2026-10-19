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
| F / gamepad Y | Ride the Dinofly (see below) |
| M / gamepad Start | Island menu (see below) |

`ALBACharacter` loads its behaviour entities from **Project Settings > Plugins > LBA2 > Hero** and can be
subclassed in Blueprint. `SetMoveInput` and `DoBehaviourAction` let AI or scripts drive it.
`ULBAAnimInstance` plays LBA animations natively (crossfades, LBA loop frames, root motion) without
an Animation Blueprint.

### Riding the Dinofly (prototype)

F gets Twinsen on the Dinofly, on the ground: a Dinofly waiting nearby is mounted where it stands, otherwise
one comes to Twinsen's spot (body 0 of `DA_Dinofly` has Twinsen on its back). On the ground (or the sea) it
walks slowly (get off to go faster); climbing takes off, with a crouch and a push-off, the legs folding up
as it rises. Touching the ground from anywhere lands at once: the legs come down during the last meters of a
descent and the landing absorbs the touchdown. Legs and wings are animated separately there (a layer of
`ULBAAnimInstance` on some bone branches): the wings keep beating at a natural pace while the legs fold or
unfold as fast as needed. `ALBADinofly` flies with `ULBAFlightMovementComponent`, in two
modes:

- **Hover** (default), a simplified helicopter: hands-off it holds its position and altitude; it flies
  forward or backward and turns, with some inertia. Thrusters: each tap adds a burst of forward speed on top
  of the cruise (up to three times the cruise speed), fading unless you tap again. Its body stays level,
  with a small cosmetic lean and bank.
- **Glide** (Tab): a small aerodynamic model (lift and drag from speed and angle of attack, banked turns,
  flapping thrust) where speed and altitude trade; harder to fly.

The camera stays behind the Dinofly, swinging after it with some lag (`CameraFollowSpeed`). It rises and
looks down at the Dinofly and the ground when descending or high above the ground (`HighAltitude`,
`MaxCameraDrop`), and its field of view widens with speed, accelerations (each thrust), climbing or
descending, and height. Looking around with the mouse or right stick (vertical inverted on the stick) eases
back behind after a moment. It never
goes under water: it floats on the sea (and skims off it when gliding in fast).

| Key | Hover | Glide |
|---|---|---|
| W/S, up/down, left stick | Forward / backward (half speed); walk on the ground | Nose down / up (`lba.Dinofly.InvertPitch 1` swaps); walk on the ground |
| A/D, left/right, left stick | Turn | Bank; turn on the ground |
| Space, gamepad A or right trigger (hold) | Climb (take off) | Flap (take off) |
| Left Ctrl, C, gamepad B or left trigger (hold) | Descend (land) | Brake |
| Left Shift, gamepad X or right bumper (tap, repeat) | Thrusters (in the air only) | |
| Mouse, right stick | Look around (eases back behind) | Look around (eases back behind) |
| Tab, gamepad View | Switch to glide | Switch to hover |
| F / gamepad Y | Get off (once landed on the ground, not at sea); the Dinofly waits there | |

`-LBA2Shots=dinofly` runs a scripted ride (mount, walk, takeoff, moves, turns, thrusters, landing at sea, paddling, landing on the ground, dismount) that logs the
flight state and screenshots each step.

### Island menu

M or gamepad Start (on foot or riding) pauses the game and lists every island level found under
**Project Settings > Plugins > LBA2 > Island Menu > Islands Path** (`/Game/LBA2/Islands`), in story order;
picking one opens its level at its PlayerStart. Mouse, keyboard (arrows, Enter) and gamepad (d-pad, A) all
work; Esc, B, M or Start close it. Packaged and standalone games open it at startup (*Island Menu At
Startup*; Play In Editor and `-LBA2Shots` runs do not). Nothing references the island levels, so the
LBA2Editor module adds every level the menu lists to each cook (`ModifyCook` delegate). It lives in `ALBAPlayerController` (the controller
of `LBAGameMode`), `ULBAIslandMenu` (a UMG widget built in C++, `IslandMenuClass` takes a Blueprint
subclass) and `ULBAIslandTravelSubsystem` (`GetIslands`, `TravelToIsland`). Console: `lba.Travel DESERT`,
`lba.Travel` (lists the islands), `lba.IslandMenu`.

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
  waves (`<Root>/Rendering/Water/Waves_LBA_Calm|Storm`, kept on reimport). The ocean's coastline is the
  outline of the island's largest land mass, traced from the ground the importer keeps, a cell inside it:
  land at sea level (the desert's dry riverbeds) stays dry, and sea-level water enclosed by land keeps a
  channel to the sea. Unreal's water mesh only leaves out whole tiles (4 m here), and its material only
  turns see-through where the zone has no water, which the cartoon water would still paint, so the
  ocean uses a copy of the Water plugin's material chain that clips there (`Rendering/Water/M_LBA_Water`,
  `MI_LBA_Water`, `MI_LBA_Water_Ocean`; the plugin's own depth mask feeds the opacity mask). The terrain renders into the
  zone as the ground (`WaterTerrainComponent`), with a generated seabed (`SM_<Island>_Seabed`) sloping
  down from the shores: a shallow shelf off beaches, deep water right along quays and cliffs, in the
  shores' average color, so beaches get lighter shallows and the water's shore foam and waves see a
  real depth. The seabed has collision, and an invisible floor (`SeaFloor`) closes the open sea where the
  seabed stops, 7.5 m down. The palette shading recognizes water (single layer
  water shading model) and draws it as cartoon water (`Shaders/Private/LBAWaterCartoon.ush`): the
  island's original sea texture, at the original's scale but without visible repetition (hex-tiling:
  three randomly shifted, turned and mirrored copies blended texel by texel, *SeaCellSize*,
  *SeaVariation*, *SeaTransition*) nor shimmering in the distance (its mips, picked from the pixel's
  size on the water), drifting and bent by the waves, each texel
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
  fog tables. An island's preset can set its own fog instead: Citadel's storm is a dark grey soup from
  6 m past the camera, nearly the same at every height, hiding everything about 60 m away.
- **Rain** (Citadel's storm, both looks): a `Rain` Niagara actor running the plugin's
  `Effects/NS_LBA_Rain` (edit it with Niagara's editor): GPU drops kept in view around the camera
  (Niagara's view recycler, evenly up to 25 m, so moving doesn't thin them out), falling with the
  original's wind slant (RAIN.CPP) at 70% of its speed (real heavy rain). Streaks follow each drop's motion
  relative to the camera (flying into the rain stretches them towards you, exaggerated by *CameraMotion*,
  up to *MaxStretch*). User parameters, set per level on the actor: *DropCount* (preset Rain x 18000), *DropVelocity*, *DropSize* (width, length), *ViewDistance* (near,
  far), *DropMaterial*. The level's `MI_<Level>_RainDrops` (child of `M_LBA_RainDrop`) draws them after
  motion blur, so after the palette shading: palette colors near/far (*NearColor*, *FarColor*,
  *ColorDistance*), never thinner than *MinPixels* (fainter instead), fading along the trail
  (*TrailFade*) and in the fog (*FogDensity*, *FogStart*), *Opacity*. Splash rings on the ground and
  water near the camera are still a post-process after tonemapping, `M_LBA_Rain` (level instance
  `MI_<Level>_Rain`: *RainAmount*, *Opacity*, *SplashRate*, *SplashRadius*).
- **Lights**: islands are lit so that sunlit ground lands on the shade rows the original's baked
  terrain lighting uses (from the island's terrain data), with the sun at the original's angle, and
  ground in the shade keeps most of that (the original has no cast shadows). The storm is
  lit evenly by a uniform overcast sky. Characters (lighting channel 1) also get a key light along the
  sun, as bright on their lit side as the original's objects (lit by N.L alone), and a shadowless fill
  light from the other side.
- **Lamps**: lit decor is listed per island in the environment preset (`FLBADecorLight`: .OBL object,
  glass palette color, light, radius, halo). Citadel's lampposts are lit during the storm: their glass uses
  `M_LBA_Glow` (emissive, *GlowStrength*; casts no shadow) and a point light in the glass's color sits at
  its center, attached to the lamppost. The light casts no shadows (a soft pool around the lamppost)
  and lights the volumetric fog a little (a halo in the storm's fog). Citabau's stay off.

Customizing (material instances in `<Root>/Rendering`, children inherit what they don't override):

| Instance | Scope | Kept on reimport |
|---|---|---|
| `MI_LBA_Look_Enhanced`, `MI_LBA_Look_Original` | every level | yes, unless a newer importer changes their defaults |
| `MI_LBA_Enhanced_<Palette>` | levels using that palette (per island tweaks) | yes, same rule |
| `MI_LBA_Original_<Palette>`, level `MI_<Level>_<Look>Look` (fog, contrast pivot) | generated | no |
| level `MI_<Level>_Clouds` (child of Unreal's `m_SimpleVolumetricCloud_Inst`) | generated | no |
| level `MI_<Level>_Rain` (child of `M_LBA_Rain`), `MI_<Level>_RainDrops` (child of `M_LBA_RainDrop`) | generated | no |
| plugin `Effects/NS_LBA_Rain` (rain particles) | levels with rain | yes (plugin content, not generated) |
| island `MI_<Island>_Ocean` / `_OceanFar` (children of the clipped ocean material / the plugin's far mesh material) | generated | no |
| `Water/M_LBA_Water`, `MI_LBA_Water`, `MI_LBA_Water_Ocean` (copies of the Water plugin's ocean chain, clipped) | every water level | yes, unless a newer importer changes them |
| `Water/Waves_LBA_Calm`, `Water/Waves_LBA_Storm` (Gerstner wave assets) | every water level | yes, unless a newer importer changes their defaults |

Main parameters: *LightContrast*, *RowStep*, *Exposure*, *Contrast*, *Saturation*, *ShadowTint* /
*HighlightTint* and their amounts (Grade); *DotPattern*, *DotSpacing* (pixels), *DotContrast*,
*DitherAmount* (Dither); *Strength* (0 = Unreal's image), *KeepLuminance* (keep Unreal's brightness,
use only the ramps' hue), *LightTint* (Palette); *WaterStrength* (0 = Unreal's water), *WaterLightTint*,
*WaterHighlightRange* (Water); *SeaDrift* (cm/s), *SeaWobble*, *WaterSmoothing* (band calmness),
*FoamWidth*, *FoamSpeed*, *FoamBrightness*, *SparkleLight* (Water Cartoon). To try values live, `lba.Look.Set <Parameter> <Value>`
(or `R G B` for colors; no value removes the override; it reaches the rain splashes' parameters too). Other console variables:
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
- [x] Citadel's lampposts give light during the storm (glowing glass + soft point light with a halo)
- [x] Citadel's storm: dense dark fog, rain (Niagara drops) and splashes
- [ ] Look, next: Citadel storm (Niagara splashes like the original's spray, lightning, wet surfaces), cloud shadows, interior lights,
  sprites; shallows (a seabed sloping down from the coast), Zeelich's cloud sea
- [ ] L3c Modern controls for Unreal (motion matching or blendspaces: to be discussed)
- [x] Dinofly riding prototype: mount on the ground, walk, take off, hover (default) and glide flight, land, float, get off
- [x] Island menu (M / Start) to travel between islands; packaged builds start on it
- [ ] L4 Scenes: actors, zones (typed trigger volumes), track points, hero start, ambience, per-scene levels
- [ ] L5 Scripts: life/move scripts (on hold; interpreter recommended over Blueprint generation)
- [ ] L6 Text (String Tables), voices, music, sprites, inventory objects, holomap, screens
- [ ] Procedural wall/pattern builder using interior kit pieces
- [ ] Light-only remake GLBs as point lights
