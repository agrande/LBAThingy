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
   for interiors (MIT licensed). A checkout at `../lba2remake` or `../refs/lba2remake` next to the
   project is auto-detected.
4. Pick layers and press **Import**. Re-importing updates assets in place and only replaces the
   actors the importer generated inside levels; anything you add yourself is kept.

Headless (batch) import:

```
UnrealEditor-Cmd.exe LBAThingy.uproject -run=LBA2Import [-GameDir="..."] [-RemakeDir="..."]
    [-Layers=Islands,Interiors,Characters,Scenes,Audio] [-Islands=CITABAU,DESERT] [-Grids=0,3] [-Entities=0,1]
    [-Scenes=0,43] [-DemoScenes] [-NoRemake] [-Root=/Game/LBA2]
```

World scale lives in **Project Settings > Plugins > LBA2** (default 0.125 cm per LBA unit: a brick is
64x32x64 cm, Twinsen about 155 cm).

Importer settings come in two parts, both shown in the panel:

- **Editor Preferences > Plugins > LBA2 Importer** (per user, not shared): where the game and the lba2remake
  checkout are on this machine, and the filters (islands, grids, entities, scenes) to import a subset while
  iterating.
- **Project Settings > Plugins > LBA2 Importer** (`Config/DefaultLBA2Importer.ini`, shared with the project and
  kept across imports): how things are imported, i.e. destination, look, interiors (back faces, kit pieces,
  remake models), demo scenes, language, and the **Enclosure** table for the coming enclosed interiors:
  per layout library whether rooms get closed, the room height and door height in cells (32 cm) and the
  layouts that frame doorways (jambs, thresholds), with per-grid overrides. The library defaults come from a
  survey of every grid (`Saved/LookDev/tools/survey_grids.py`): the most common height of the back walls the
  original drew in full, the median height of its doorways, the layouts found at its doors. Caves and outdoor
  grids are left open. Options that used to be per-user are picked up from your old settings until the project
  settings are saved.

The Interiors layer analyses each grid's rooms and doorways (`FLBARoomAnalysis`) and logs a line per grid
(`Grid 0: 5 spaces, 5 doorways (3 exits, ...)`; `-LogCmds="LogLBA2 Verbose"` lists every doorway). A doorway is
an opening in a wall line (walls of at least 3 cells, so the walls cut down for the camera count) between two
rooms, or out of the grid: the original closes exits with invisible collision just past their door zone.
Openings count as doorways when they have a lintel, a jamb or threshold layout, or a door (cube-change zone)
of one of the grid's scenes; 228 of the 322 interior door zones sit at one (most others are stairs, ladders
or open ground).

Grids whose library (or grid override) has the enclosure on also get `SM_Grid_NNN_Enclosure`
(`FLBAEnclosureCells`), placed as an `Enclosure` actor tagged `LBAEnclosure`. Per room, the footprint is
what the outside cannot reach when it floods in from beyond the grid through columns with nothing at or
above the room's lowest floor; its edge (but its floors, which stay open) is raised to the ceiling, the
ceiling sits the room height above the highest floor nearby, the wall above doorways is closed from their
lintel (or the door height where the camera-side walls were cut), and exits stay open up to the top of
their door. Walls inside the room the original cut down (their collision goes on above what is drawn,
unlike railings and furniture) are raised too, and so are the stubs of the camera-side walls, flush. The
frames the original put on its cut walls (the **CutFrames** layouts of each library, seeded from
`Saved/LookDev/tools/survey_frames.py`) come off where the enclosure takes their place, into
`SM_Grid_NNN_Frames` (an `EnclosureFrames` actor, hidden until the look switch shows it with the Original
look); bricks without any art count as empty. Every face looks into a room, so a camera outside sees
through while the walls and ceiling still cast shadows (two-sided shadows and distance fields); the camera
ignores its collision, Twinsen doesn't. For now the enclosure shows in both looks; its walls continue the
art of the wall below (or along it, or the other side of a wall panel), else a darker flat color.
`-LBA2EnclosureDebug` on an import colors it by part (walls red, backings gray, ceilings green, risers
blue, above doorways yellow, partitions and stubs orange), `-LBA2EnclosureDebugArt` shows the walls that
found no art in magenta, and the first dumps its cells to
`Saved/LookDev/enclosure/cells/`; `Saved/LookDev/tools/raycast_leaks.py` checks a grid for holes by
casting rays from its rooms against the exported meshes (`export_enclosure_obj.py`).

## What gets imported

| Layer | Source | Unreal result |
|---|---|---|
| Islands | `*.ILE`, `*.OBL` | Per island: ground texture, decor static meshes (kit pieces), one terrain static mesh per section, level `L_<ISLAND>` with all decor placed, sea, sky/fog preset, player start |
| Interiors | `LBA_BKG.HQR` | Brick atlas per library, one static mesh per library layout (kit pieces with convex collision), one mesh per grid with exact slope collision, level `L_Grid_NNN_<Scene>` |
| Remake models | lba2remake checkout | GLB replacement models as static meshes under `Remake/`, placed in interiors instead of the matching bricks |
| Characters | `RESS.HQR` (entities), `BODY.HQR`, `ANIM.HQR` | One skeleton per character family, skeletal meshes per body, animation sequences with root motion and notifies (footsteps, hits, sounds, throws), `ULBAEntityAsset` per entity, `L_CharacterGallery` |
| Scenes | `SCENE.HQR`, `SPRITES.HQR`, `SPRIRAW.HQR`, `ANIM3DS.HQR` | Per scene: a scene asset and a level with its objects, sprites, zones, track points and hero start, streamed into its island or interior level (see [Scenes](#scenes)) |
| Audio | `SAMPLES.HQR` | Sound waves (converted to 16-bit PCM) |

## Playing as Twinsen

Generated levels use `LBAGameMode` (toggle with *Play As Twinsen* in the importer settings), so
pressing Play spawns `ALBACharacter`. Any other map can do the same with a GameMode override.

Twinsen has two control modes; Tab switches between them.

**Modern** (default) plays like Unreal's third-person template: a free camera, movement relative to
the camera, and Twinsen always running (the athletic run). A partly tilted stick slows him down into
the normal walk.

**Authentic** is the original's "tank" control: every move is an animation with root motion.

| Key | Modern mode (default) | Authentic mode |
|---|---|---|
| W/A/S/D, arrows, left stick | Run where you push, relative to the camera | Up/down walk forward/backward; left/right turn in place or steer while walking |
| Mouse, right stick | Free camera (vertical inverted on the stick) | Orbit the camera (it recenters behind Twinsen when he walks) |
| Space, gamepad A | Jump (a running jump when moving) | Behaviour action |
| E, gamepad X | Behaviour action: Normal search, Athletic jump, Aggressive punch/kick, Discreet crouch | Behaviour action |
| F5-F8 or 1-4 | Normal, Athletic, Aggressive, Discreet (idle and actions; the run stays) | Normal, Athletic, Aggressive, Discreet |
| Tab | Switch to authentic mode | Switch to modern mode |
| F / gamepad Y | Ride the Dinofly (see below) | Ride the Dinofly |
| M / gamepad Start | Island menu (see below) | Island menu |
| Walk into a ladder | Climb it (see [Scenes](#scenes)) | Climb it |

In modern mode the LBA animations play in place and are kept in step with the movement:

- **Starts and cycles.** From a stand, the run's own lead-in (the run start) plays at the pace of
  the actual acceleration. After that, the walk and run cycles are blended by speed, in step on the
  feet (their footstep notifies). One loop is played per stride, so the feet keep up with the ground.
- **Smooth keyframes.** The original's sparse keyframes are interpolated with monotone cubic curves
  instead of straight in-betweens (`lba.Modern.SmoothKeys 0` shows the original). The importer
  stores the key times on each sequence (`ULBAAnimKeyframes` metadata).
- **Procedural pose** (`lba.Modern.Procedural 0` disables it):
  - the body leans into turns and accelerations;
  - the feet stand on the ground with two-bone IK (body lowered for the lower foot, planted feet
    tilted to the slope);
  - the head turns towards where Twinsen is going, or, standing, towards where the camera looks.
- **Jumps.** Jumps take off at once. The athletic jump animations are stretched over the actual
  flight, then touch down and recover; a hard landing stumbles.

Tunables are under *LBA|Modern* on the character: speeds, jump, camera distance, lean, head turn,
and the leg/head bones. `-LBA2Shots=modern` runs a scripted test (starts, stops, walk, turns,
reversal, jumps; side views with bursts of shots and a per-frame log).

`ALBACharacter` loads its behaviour entities from **Project Settings > Plugins > LBA2 > Hero** and can be
subclassed in Blueprint. `SetMoveInput` and `DoBehaviourAction` let AI or scripts drive it.
`ULBAAnimInstance` plays LBA animations natively without an Animation Blueprint: crossfades, LBA loop
frames, root motion, a bone-branch layer, synced cycle blends, smoothed keys and the procedural pose.

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
descending, and height. Looking around with the mouse or right stick (vertical inverted on the stick) turns all the
way around, and the view stays where it was left. It never
goes under water: it floats on the sea (and skims off it when gliding in fast).

| Key | Hover | Glide |
|---|---|---|
| W/S, up/down, left stick | Forward / backward (half speed); walk on the ground | Nose down / up (`lba.Dinofly.InvertPitch 1` swaps); walk on the ground |
| A/D, left/right, left stick | Turn | Bank; turn on the ground |
| Space, gamepad A or right trigger (hold) | Climb (take off) | Flap (take off) |
| Left Ctrl, C, gamepad B or left trigger (hold) | Descend (land) | Brake |
| Left Shift, gamepad X or right bumper (tap, repeat) | Thrusters (in the air only) | |
| Mouse, right stick | Look around (all around; stays put) | Look around (all around; stays put) |
| Tab, gamepad View | Switch to glide | Switch to hover |
| F / gamepad Y | Get off (once landed on the ground, not at sea); the Dinofly waits there | |

`-LBA2Shots=dinofly` runs a scripted ride (mount, walk, takeoff, moves, turns, thrusters, landing at sea, paddling, landing on the ground, dismount) that logs the
flight state and screenshots each step.

**Wind.** The Dinofly flies through the air and the level's wind carries it (its velocity = its motion through the air +
a drift that follows the wind over about a second, air pockets at once; Dinofly > Flight > Wind). Hands-off it drifts;
holding a place means flying against the wind. Floating, the wind pushes it gently along the water; on the ground, not
at all. Citadel's wind is a small hurricane around the island (`ALBAStorm`, see [Storm wind](#storm-wind)).
`-LBA2Shots=wind` (on `L_CITADEL`) hovers hands-off at a few places (Lupin-Burg, Twinsen's house, the cliffs, the
dome, the wall, far out at sea), tries to fly away and to climb to the ceiling, and logs the drift.

**Airflow strokes.** A few short comic speed lines come off the Dinofly where the airflow breaks away from its body
(`ULBAAirflowComponent`, reusable on any flyer). They start at hotspots (wingtips, wings, tail tip, feet, Twinsen's head,
the Dinofly's head), only those on the body's sides and back for the way it goes through the air: the tail and the feet
in forward flight, Twinsen's head in a dive, the feet in a climb, never a part the body pushes ahead (wingtips shed
whichever way they swing, `MinShedding`). Each hotspot has how hard the flow breaks on it (`Accent`), how much air it
drags along (`Wake`), how much its strokes ripple (`Turbulence`) and how much its own motion steers them (`Follow`,
flapping wings). The body drags the air near it along, so a new stroke keeps up with it a while (`Wake`, fading over
`WakeTime`) and fades in as it falls behind (`FadeInDistance`). A stroke is led by its head, which runs back along the
path the body takes through the air (`Stream`), its tail following the same path: it grows, slides and shrinks into its
head, and shows at a glance whether the Dinofly climbs, dives, goes forward or turns. The path follows
the air: bent like the body's path (`Curvature`, capped at `MaxCurve` degrees), held back near the body by the air it
drags along, rippling (`Ripple`), and now and then a stroke does a little cartoon loop (`LoopChance`, `LoopSize`), turned
towards the camera (none when the flow runs towards or away from it). As long as the Dinofly moves at all, at least
`MinStrokes` (2) are on screen (started from hotspots in view); the faster it flies, the more (mostly as the square of
its air speed, like its energy), never more than `MaxStrokes` (6) on screen at once, and none when it stands still.
Their look blends two styles by air speed (`Slow`, `Fast`: width, opacity, length, life, loop chance and size): small,
faint, lingering and loopy when it barely moves, quickly fat, opaque and straight (`BoldnessBias` 3: a sixth of
`FullSpeed` is already 40% of the way). The air is pushed down by the lift (strokes tilt with the downwash) and carried by the level's wind
where the Dinofly is (`ULBAWindSubsystem`): flying into it gives more strokes blown back, drifting with it hardly any. They are white with a
dark ink edge, unlit after motion blur (`Effects/M_LBA_Airflow`), and stand out from the rain. Look and physics are
properties of the component (Dinofly > Airflow: color, width, stroke life and length, density, speeds, mass, wake,
curvature, loops...); the drawing is the plugin's `Effects/NS_LBA_Airflow` (one ribbon per stroke, closed-form path in
its Set Parameters expressions, rebuilt by `Tools/airflow_niagara.py` through the Unreal MCP server; path in Project
Settings > Plugins > LBA2 > Airflow Trail). `lba.Airflow 0` hides them,
`lba.Airflow.Opacity` and `lba.Airflow.Density` scale them, `lba.Airflow.Debug 1` logs air speeds, the hotspots and
their stroke rates (2 also draws them).

### Island menu

M or gamepad Start (on foot or riding) pauses the game and lists every island level found under
**Project Settings > Plugins > LBA2 > Island Menu > Islands Path** (`/Game/LBA2/Islands`), in story order;
picking one opens its level at its start (its first scene's hero start, see [Scenes](#scenes)). Mouse, keyboard (arrows, Enter) and gamepad (d-pad, A) all
work; Esc, B, M or Start close it. Packaged and standalone games open it at startup (*Island Menu At
Startup*; Play In Editor and `-LBA2Shots` runs do not). Nothing references the island levels, so the
LBA2Editor module adds every level the menu lists to each cook (`ModifyCook` delegate). It lives in `ALBAPlayerController` (the controller
of `LBAGameMode`), `ULBAIslandMenu` (a UMG widget built in C++, `IslandMenuClass` takes a Blueprint
subclass) and `ULBAIslandTravelSubsystem` (`GetIslands`, `TravelToIsland`). Console: `lba.Travel DESERT`,
`lba.Travel` (lists the islands), `lba.IslandMenu`.

## Scenes

A scene (a "cube" of the original) is what lives in a place: its objects (characters, animals, props, doors),
its zones and its track points. The Scenes layer imports all 193 story scenes (the demo copies, 193-221, with
*Import Demo Scenes* or `-DemoScenes`); run it after Islands, Interiors and Characters. Each scene becomes:

- a **scene asset** `Scenes/DA_Scene_NNN_<Name>` (`ULBASceneAsset`, a Primary Data Asset): ambience, hero start,
  every object with its original settings and its life and track script bytecode (kept for a script interpreter),
  zones and track points, in LBA units local to the scene, with `ToWorld()` to place them;
- a **level** `Scenes/L_Scene_NNN_<Name>` holding its actors, streamed into its scenery level as a sub-level
  (Levels window). The cubes of an island are all always loaded with the island level, so the whole island is
  alive at once and walking from cube to cube needs nothing. Citadel's scenes are in both the storm (`L_CITADEL`)
  and the sunny island (`L_CITABAU`). An interior scene is streamed into its grid's level. A second scene on an
  island cube already taken (the sea-travel scene, demo copies) is added unloaded (Blueprint streaming).

In a scene level (outliner folders under *Scene*):

| Original | Actor |
|---|---|
| Object with a 3D body | `ALBASceneActor` (an `ACharacter`): the entity's body and its start animation (in place: root motion waits for scripts), capsule fitted in the body's box and the exact box blocking pawns. Objects without a body never collide, like the original. Objects the original hides at start are hidden in game and, for now, don't block (the original's do). Stand-ins for gameplay handled differently here (Twinsen with Zoe or carried, the Dinofly, meca-penguins: Project Settings > Plugins > LBA2 > Hidden Scene Entities) are hidden in game, without collision. The meca-penguin slot (object 1) is skipped. |
| Sprite object (interiors: doors, signs, pickups) | `ALBASceneSprite`: a static mesh per sprite (`Sprites/SM_Sprite_NNN`, `SM_SpriteRaw_NNN`, `SM_SpriteAnim_NNN`, one atlas `T_Sprites`) with the sprite projected onto the visible faces of its box like the interior bricks: pixel exact from the original angle, solid from the others, the box as collision. Animated sprites (ANIM3DS) flip through their frames. |
| Zone | A volume per zone type, sized to its box, with the original settings as properties: `ALBACubeChangeVolume`, `ALBACameraZoneVolume`, `ALBAScenarioZoneVolume`, `ALBAGridZoneVolume`, `ALBAGiverZoneVolume`, `ALBAMessageZoneVolume`, `ALBALadderVolume`, `ALBAEscalatorVolume`, `ALBARailVolume`, and `ALBAHitVolume` (Unreal's `APainCausingVolume`). Cube changes between cubes of the same island are left out (the island is one level). |
| Track point | `ATargetPoint` `Point_NN` (tag `LBAPoint_NN`) |
| Hero start | `APlayerStart` tagged `LBAScene_NNN` |

What works in game:

- **Doors and exits** (cube change zones): Twinsen entering one travels to the target scene
  (`ULBASceneTravelSubsystem`). Like the original (`GereZoneChangeCube`), he keeps his offset in the zone, turned
  by the zone's quarter turns, around the arrival point, and lands on the floor below it. Doors in walls fire only
  when he walks into them. The target's level opens (or, already loaded, he is teleported) and `ALBAGameMode`
  spawns him there. Coming out of an interior on Citadel goes back to the island variant (storm or sunny) he left.
  Zones he spawns or arrives in do not fire.
- **Ladders**: walking into the wall in a ladder zone climbs it with the original clips (grab, climb, climb out at
  the top of the zone onto the ledge); walking off the ledge over a ladder climbs down it backwards. Up and down
  with the stick on the way, otherwise he keeps going like the original. Both control modes.
- **Starting**: a level starts at the hero start of its lowest-numbered scene (islands: Citadel at the Lupin-Burg
  landing zone, scene 42), or at its plain `PlayerStart` without scenes.
- `lba.Scene <number>` goes to a scene's hero start; `lba.Scene` lists the imported scenes.

Not yet: scripts (objects stand in their start state, so some show that the story would hide or move), camera
zones (data only: the modern camera ignores them), givers, messages, escalators, rails, ambient sounds and music.
Doors and `lba.Scene` find scenes under **Project Settings > Plugins > LBA2 > Scenes Path**, and the LBA2Editor
module cooks every scene asset and its levels.

`-LBA2Shots=scenes -LBA2Doors=49,0 -LBA2SceneEnd=43` takes the doors to those scenes in turn (walking into the
ones that need it), then `lba.Scene 43`, with a shot in each level; `-LBA2Shots=ladder -LBA2Ladder=44:7` climbs a
ladder zone (scene:zone) up and back down.

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
- <a id="storm-wind"></a>**Storm wind** (Citadel): an `ALBAStorm` actor (`Storm`) whose location is the eye of a small
  hurricane, at sea level. `FLBAStormWind` is the whole wind as one closed-form function of position and time (no
  textures or state, so it can be computed anywhere; `Shaders/Private/LBAWind.ush` is the same function for shaders):
  still at the center of the eye (over Lupin-Burg), rising steadily (r^1.5) through a breeze around Twinsen's house
  (2.6 m/s) to the eye wall (8 m/s at sea level, 12 m/s aloft), which hugs the cliffs and the dome
  (about 6 m/s there) and lies out at sea elsewhere, then dying off slowly (about 5 m/s far out). The wind swirls
  around the island and spirals in by 25 degrees from the wall outward, so the wall pushes flyers back toward the
  island. Gusts (travelling waves) vary its speed and direction, more away from the eye. From 35 m up to the
  ceiling at 60 m the air sinks, with air pockets and bumps: flying higher gets hard, never dangerous. The eye is an
  egg, one radius per half axis (*RadiusAhead* +X, *RadiusBehind*, *RadiusRight* +Y, *RadiusLeft*); the importer
  places it from the island preset (eye and radii in island cubes). `ULBAWindSubsystem::GetWindAt` gives the level's
  wind anywhere; `lba.Wind.Debug 1` draws it around the player (arrows colored by speed) with the eye wall at the
  player's height, 2 also logs it. Each frame the subsystem writes the wind into `MPC_LBA_Wind` (generated; the
  storm's *MaterialParameters*) for materials (`Shaders/Private/LBAWind.ush`), and the rain falls through it: its
  drops move with the wind at the camera (straight down in the eye, leaning more toward the wall).
- **Rain** (Citadel's storm, both looks): a `Rain` Niagara actor running the plugin's
  `Effects/NS_LBA_Rain` (edit it with Niagara's editor): GPU drops kept in view around the camera
  (Niagara's view recycler, evenly up to 25 m, so moving doesn't thin them out), falling with the
  original's fall at 70% of its speed (real heavy rain), blown by the storm's wind (with a storm; else the
  original's wind slant). A drop's head stays where the drop is and its streak trails back along its path
  through the world, whatever the camera does. Drops fade in
  and out over *LifeFade* of their life (Niagara recycles them in view). Density varies with the place:
  the island's `T_<Island>_RainDensity` (generated from the terrain: full rain over the sea and ground
  above 8 m, 25% below 3 m, smoothed over ~10 m) thins out drops and splashes (*RainDensityMap*,
  *RainDensityBounds* on both level instances). User parameters, set per level on the actor: *DropCount* (preset Rain x 18000), *DropVelocity* (every drop's, every frame: `Tools/rain_niagara_wind.py` adds that module), *DropSize* (width, length), *ViewDistance* (near,
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
  Lit windows and the lighthouse's lamp room are texels of the island's object texture page instead
  (`FLBAGlowTexels`: a rectangle of the page, glass only or not, the palette ramp the glow takes, window
  or lamp, light, radius, halo, objects excluded), so every object showing them glows, except the
  excluded ones (Citadel's telescope, whose lens is window glass). On Citadel the window glass takes the
  lamp glass's yellow ramp (lamplight behind it; the palette shading takes a pixel's hue from its base
  color) and keeps its shades. Polygons showing them use `MI_<ISLAND>_Obj_<region>_Glow` (child of
  `M_LBA_TexturedGlow`: the region's texture with the glowing texels recolored and marked in alpha,
  emissive *GlowStrength*), and each window gets a rect light of its size just in front of it, facing
  out, the lamp room a point light at its center. A polygon only glows when its texture region holds the
  whole rectangle (some objects use a neighbor texture that bleeds into a window's edge).
  Interior lamps are lit: in lba2remake models, a bulb (a node named *...bulb...*, or a `keepMat_light...`
  material) glows with `M_LBA_Glow` and a lampshade (`LampTexture...` material) with its texture's
  `MI_Remake_<hash>_Glow` (*GlowStrength* 0.6: fabric lit from inside), neither casting shadows, and a warm point
  light (the remake's own lamplight color, or the bulb's emissive color) sits at the bulb's center
  (`FLBAEnvironmentPreset::RemakeLamps`: a soft pool of light in which furniture casts shadows).

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
- [x] Interior lamps (lba2remake models) glow and give light
- [x] Importer settings split: per-user (paths, filters) and project (shared import rules, enclosure table per library and grid)
- [x] Room and doorway analysis of interior grids (spaces, doorways and exits, door heights and doorway layouts per library)
- [ ] Enclosed interiors (Enhanced look): walls raised to the room height, doorways kept, ceilings; hidden from the
  iso camera but still shading; see-through dithered walls between Twinsen and the camera; Original look stays open
- [x] Citadel's storm wind: a small hurricane around the island (calm eye, wall, gusts, turbulent ceiling) that carries the Dinofly and blows the rain
- [ ] Look, next: Citadel storm (Niagara splashes like the original's spray, lightning, wet surfaces), cloud shadows, more interior lights (candles, original bricks),
  sprites; shallows (a seabed sloping down from the coast), Zeelich's cloud sea
- [x] L3c Modern controls (third-person template style; procedural layer over the LBA clips rather than motion matching, too few clips)
- [x] Dinofly riding prototype: mount on the ground, walk, take off, hover (default) and glide flight, land, float, get off
- [x] Island menu (M / Start) to travel between islands; packaged builds start on it
- [x] L4 Scenes: per-scene levels streamed into islands and interiors, objects, sprites, typed zone volumes,
  track points, hero starts; doors and ladders work in game
- [ ] Scenes, next: ambient sounds, givers and messages, escalators, camera zones (as hints for the modern camera)
- [ ] L5 Scripts: life/move scripts (on hold; interpreter recommended over Blueprint generation)
- [ ] L6 Text (String Tables), voices, music, sprites, inventory objects, holomap, screens
- [ ] Procedural wall/pattern builder using interior kit pieces
- [ ] Light-only remake GLBs as point lights
