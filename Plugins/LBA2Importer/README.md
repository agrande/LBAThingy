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
    [-Layers=Islands,Interiors,Characters,Scenes,Audio,Tests] [-Islands=CITABAU,DESERT] [-Grids=0,3] [-Entities=0,1]
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
  remake models), demo scenes, language, and the **Enclosure** table of the enclosed interiors: per layout
  library whether rooms get closed, the room height and door height in cells (32 cm), its cut-wall frames
  (**CutFrames**), ceiling color or material and wallpaper, with per-grid overrides. The library defaults come
  from a survey of every grid (`Tools/survey_grids.py`, `Tools/survey_frames.py`): the most common height of the
  back walls the original drew in full, the median height of its doorways, the caps and rims on its cut walls.
  Caves and outdoor grids are left open: libraries of caverns, the underworld and the Nest, and a reviewed list
  of grids that are open air in the original (rooftops, the Dome's starfield, the control tower's grounds), each
  with a note. Options that used to be per-user are picked up from your old settings
  until the project settings are saved.

### Enclosed interiors: the plan

Grids whose library (or grid override) has the enclosure on are closed from a **plan** of the grid
(`FLBARoomPlan`, prototyped rule for rule in `Tools/room_plan.py`, which draws it top-down). The plan is made of
architectural concepts, each found by one explicit rule, applied in this order:

1. **Levels**: the storeys, floor heights with a connected floor of at least 16 cells, 4 of them with floor
   all around (one-cell strips are ledges and wall tops).
2. **R1 Stairs**: floors at no level's height that climb one cell at a time between floors of two levels.
3. **Columns** of each level: floor, stairs, furniture (standing on the level's floor: its first drawn layout
   starts at the floor or above; not another storey's floor, a storey being a level that holds rooms, so the plan
   is made again without the roomless ones, such as machine tops), wall, or nothing. **Walls** rise at least 3 cells over the floor (through
   window gaps) and are *cut* (collision above what is drawn, or topped by a cut-wall frame), *invisible*,
   *masonry* (drawn with the library's wall bricks, learned from all its grids), *exterior* (tall with the
   outside behind) or an exit's *jamb*, and go on (a wall next to them, or the outside). Furniture against a
   wall (lower than the drawn wall behind it) is furniture; a wall column in no straight run of 3 is a post.
4. **R2 Gap**: the floor between two walls in line is an opening in that wall (a door, a bay, an arch).
   **R3 Partition**: a wall of at least 4 columns of its own that ends inside the floor goes on as a line across
   it to the next wall.
5. **R4 Rectangle**: a room is the largest rectangle of floor and furniture off those lines, at least 2 columns
   across, with 12 floor cells and a wall (or a step to a split level) along one of its sides, not standing on walls (more than half of its
   floor on the walls of a lower storey is a roof or a hull's top), on its storey (mostly on the level's connected
   floor, not on a ledge or a rim at the same height) and enclosed (more than half of its border open is open
   ground); then the next largest, and so on. An L-shaped floor is two rooms sharing an open side.
6. **Connectors**: the columns no room took, joined through the flights they lead onto, named by what they join:
   **stairs** (rooms of two levels), **passage** (two rooms: a door or an arch), **exit** (one room and a
   scene-change zone), **alcove** (one room), pocket (none).
7. **Spaces**: rooms joined by open sides, or by a step to a split level (at most 4 cells up or down, no wall
   between: a sunken lounge, a dais), under one ceiling (their highest room's level + the room height). **R5 Double
   height**: a space overlooked by a higher storey's floor edge, more than 4 cells up (a gallery, a mezzanine),
   rises to that storey's ceiling. Closer storeys are split levels (above).

Every import logs each enclosed grid's plan (rooms with what lies past each side, spaces, connectors) and dumps
it to `Saved/LookDev/enclosure/cells/gridNNN.txt` with its air ("C x z room floor top" per room column, "K ..."
per connector column) and its walls. `Tools/room_plan.py <grid ...>` prints and draws the same plan (in
`Saved/LookDev/plans`), so a rule can be tried and checked on any grid before it is ported.

### Enclosed interiors: the saved plan, its views and golden tests

What the importer understood of each grid is saved as **`FLBAGridPlan`** (runtime module, `LBAGridPlan.h`), the
foundation for tools that edit rooms later: the levels (what each column is there and the rule that made a wall a
wall, the wall runs, gaps and partition lines), the rooms (level, rectangle, ceiling, space, and for each of their
four sides the runs of what lies past it), spaces, connectors, flights, the air the enclosure closes column by
column, and for caves the hollow (floor, spring and roof per column, supports). Every grid gets one, open-air
grids too (marked not enclosed). It is saved three ways by each import of the interiors:

- **`DA_Grid_NNN_Plan`** (`ULBAGridPlanAsset`, `Interiors/Grids/Plans`), every field visible in the editor.
- A **GridPlan** actor in the grid's level (editor only) draws it over the geometry: room floors and their air up to
  the ceiling, each side colored by what lies past it (wall red, open side green, connector its kind's color, step
  orange, open gray), connectors (stairs yellow, passage cyan, exit magenta, alcove blue), wall columns by rule (cut
  orange, jamb yellow, invisible pale blue, masonry brown, exterior gray), labels. Show it with **Show > Developer >
  LBA2 Grid Plan**; with the actor selected, click a room or connector to select it and see its data in Details.
- **Text** in the plugin's `Tests/Plans/gridNNN.txt`, kept in git: the rooms with their sides, spaces, connectors and
  flights, then top-down maps of each level (X across, Z down: which room or connector holds each column, what each
  column is, the cells of air over it). After an import, `git diff Tests/Plans` shows what a change did to the plans.

**Tools > LBA2 Plan Browser** (console `lba.PlanBrowser [grid]`) lists every grid's plan and draws it top-down, one
level at a time, with the same colors; hovering a column tells what it is at every level, clicking a room or
connector shows its data, **Show in Level** opens the grid's level with its overlay on and frames the selection.

The golden tests **`LBA2.Enclosure.Plans.<level>`** (`Automation RunTests LBA2.Enclosure.Plans`, needs the game's
data) make each grid's plan afresh with the current code and settings and compare it with its `Tests/Plans` file:
a difference is a change in how a grid is understood. The fresh plan goes to `Saved/LookDev/plans_actual` to diff;
if the change is meant, reimport the interiors and commit the files.

### Enclosed interiors: what holds the roof

One idea covers the layouts that are not houses: **a roof spans between what holds it, and rises with the distance
from it** (`FLBAVault`). Each point of a vault is its spring height plus a cove, a quarter ellipse **VaultRadius**
columns across and **VaultRise** cells high, of its distance to the nearest support, then level. So a corridor
narrower than twice the radius gets a pointed arch, a rectangle a cloister vault, a wide hall a coved, level crown,
and a pillar standing in it brings the roof down onto it. A flat ceiling is the vault of radius 0: houses. What holds
the roof is the **Structure** of an interior (library setting, per-grid overrides):

- **Rooms** (houses, offices; the plan above). With a vault radius, each space's ceiling becomes a vault springing
  from its walls at its ceiling height (with the connectors that reach it), in the stone the walls are crowned with:
  stone halls (the School of Magic's nave, sewer chambers).
- **Hollow** (`FLBAHollow`): caves, a temple carved in a mountain, a cistern. A cave has no walls in the builder's
  sense: its walkable floor is a hollow in the rock, and the rock rises from the hollow's edge and closes over it.
  The **hollow** is the columns of the big walkable floors (any height, stepping one cell at a time) and of any
  walkable floor at an exit's zone (the way out is never walled off), closed over gaps up to 6 columns across (a chasm
  a bridge crosses, a pit, a ridge between galleries). Its roof **springs** RoomHeight cells over the highest floor
  within **SpringReach** columns, smoothed over 4 (short reach: the roof follows the ground; long: one level, a
  cistern). A column without floor whose drawn top comes within 2 cells of the roof **holds it** (a pillar, a rock wall
  between galleries); rims, railings, lamps and invisible walls stand under it. Walls rise on its edge from what the
  original drew there (a rim, a rampart; nothing: from the grid's bottom, closing the abyss) up to the roof, dark past
  exits; one bounce light lights the roof from its spring up. Buildings inside a cavern (Tralu's house, the temple's
  galleries) keep their open tops under the cavern's roof. A vertical shaft (the Temple of Bu's well) has no ground,
  so no hollow: it stays open.

**Finish**: surfaces the original never drew wear the stone it crowned their supports with: the row drawn most on
top of the walls or rims around (`FCrownFinish`), its shadowed bricks left out, rendered into a texture that tiles
(`T_Grid_NNN_Finish`, one cell of wall per brick face) and laid by position, so a roof keeps the art's size however
steep it gets; roofs (`T_Grid_NNN_Roof`) are pulled 40% toward the library's ceiling color, as flat ceilings are.
**VaultNoise** makes rock rough. `-LBA2EnclosureTiles` writes the tiles to `Saved/LookDev/enclosure/tiles`.

Reviewed so far: Tralu's cave (2, 19, 20), the Temple of Bu (10, 11, 12, 75), the sewers (17 a cistern; 18, 34 vaulted
rooms) and the School of Magic (27, 28 vaulted rooms). Their libraries (1, 3) default to hollows but stay off until a
grid is reviewed. `Saved/LookDev/enclosure/cells/gridNNN.txt` holds a hollow's summary and its "C x z 0 floor roof"
columns. The concepts are checked on tiny made-up grids by automation tests (`Automation RunTests LBA2.Enclosure`).

### Enclosed interiors: the geometry

The enclosure (`SM_Grid_NNN_Enclosure`, an `Enclosure` actor tagged `LBAEnclosure`) closes the plan's **air**:
each room's, connector's and flight's column from its floor up to its space's ceiling, the underside of a higher
storey's floor (a gallery), a lintel, or, in a gap of a wall's line, the door height. Each air cell closes on
each of its sides on a wall of its level (raising the walls the original cut down), on air that stops lower (a
doorway's header, a lower ceiling), on a solid cell or above what rises from its floor where no one's air is (a
raised floor no room stands on, a frame, a nook), and in the dark past an exit (and right behind an exit's
doorway, away from its room, where the original's invisible plug stands: no wallpaper there); never on nothing from its floor up (drops and
the void stay open), nor on a slope (a flight's step is walked on: no face, no collision across it; over a slope, the
walls beside it close from the step's own cell). The tops of what it closes on are capped under the ceiling. On the sides the
original drew (the air's -X and -Z) its opaque art stays, except its cornice and the frieze under it (the top rows of
the wallpaper, or of any wall panel as tall) where the wall now goes on above them;
art that is not opaque is backed by the wallpaper and drawn again in front of it. Ceilings are
`SM_Grid_NNN_Ceiling` (`EnclosureCeiling`, no shadow, so the overhead light enters; each space gets a soft bounce
light from its floor, **RoomBounceLight**), in the library's **CeilingColor** or **CeilingMaterial**. Wall faces
sit half a centimeter in front of the walls and overhang their sides as much (no cracks at corners); the grid's
faces they cover move to the frames mesh. A camera outside a room sees through it, while walls still cast
shadows (two-sided shadows and distance fields); the camera ignores the enclosure's collision, Twinsen doesn't.
For now it shows in both looks.

**Wallpaper** (a first painting rule; more will come as explicit rules too): back walls take their space's, the
camera-side walls (never drawn inside) the building's, unless the space has a finish of its own (its back walls all
one wallpaper the building's walls don't use, like Zoé's nursery being papered anew): then all its walls. A
wallpaper is a height profile: at each height over the floor, the wall layout row drawn most there on back walls
(wall panels only, no frames), so a stacked wall keeps its dado, rail, body, frieze and cornice; a profile shorter
than its layout (back walls the original cut down to their dado) goes on with the layout's rows; laid from the
floor, the cornice under the ceiling with the frieze under it, the body repeated where the room is taller
(**WallpaperLayout** forces one layout per library). The frames the original put on its cut
walls (**CutFrames**, and the topmost piece under a wall's invisible collision) come off as whole instances where
the enclosure covers them, with the remake models replacing them, into `SM_Grid_NNN_Frames` (an
`EnclosureFrames` actor, hidden until the look switch shows it with the Original look). The library's **wall
styles** (`DA_LibNN_WallStyles`, `ULBAWallStyleLibrary`; **IgnoredWallLayouts** leaves layouts out) are still
found in all its grids, as the vocabulary of a future wall builder.

`-LBA2EnclosureDebug` colors the enclosure by part (back walls red, camera-side walls orange, steps and headers
yellow, sides of solid cells purple, past exits gray, ceilings green), `-LBA2EnclosureDebugArt` its walls by art
(wallpaper cyan, its foot green, flat magenta).

The interior tools in `Tools/` (Python 3 with numpy and Pillow; they read the game from `LBA2_GAME`, else the GOG
install, and write under `Saved/LookDev`): `room_plan.py <grid ...>` (the plan, above); `survey_grids.py`,
`survey_frames.py`, `survey_walls.py` and `render_layouts.py` survey walls, doorways, frames and layouts across
grids; `sides.ps1 -Name <run> -Grids <grids> [-Unlit]` shoots every side and corner of each room from the last
import's plan (`side_poses.py`, `shots.ps1`, `sheet.py`), one sheet per grid in `Saved/LookDev/sides/<run>`;
`raycast_leaks.py <grid>` casts rays from the rooms against the exported meshes (`export_enclosure_obj.py`, grids
in `LBA2_GRIDS`; remake models are not exported, so rays through them count as leaks). The regression set:
9, 7, 35, 22, 113, 114, 14, 82, 48 (simple houses across libraries), 38 (gallery), 29 (courtyard), 23, then
Twinsen's house (0).

## What gets imported

| Layer | Source | Unreal result |
|---|---|---|
| Islands | `*.ILE`, `*.OBL` | Per island: ground texture, decor static meshes (kit pieces), one terrain static mesh per section, level `L_<ISLAND>` with all decor placed, sea, sky/fog preset, player start |
| Interiors | `LBA_BKG.HQR` | Brick atlas per library (each brick's three faces apart, grown by a pixel of their own colors, and solid faces' transparent edge pixels filled, so filtering and mips show no seams between rows, as lba2remake does), one static mesh per library layout (kit pieces with convex collision), one mesh per grid with exact slope collision, level `L_Grid_NNN_<Scene>` |
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
| E, gamepad X | Use what is in front (a door); otherwise the behaviour action: Normal search, Athletic jump, Aggressive punch/kick, Discreet crouch | The same |
| Left mouse button, gamepad B | Punch/kick (the aggressive action) in any behaviour, turning to the closest enemy near where you aim; press again to chain | Punch/kick in any behaviour |
| F5-F8 or 1-4 | Normal, Athletic, Aggressive, Discreet (idle and actions; the run stays) | Normal, Athletic, Aggressive, Discreet |
| Tab | Switch to authentic mode | Switch to modern mode |
| F / gamepad Y | Ride the Dinofly (see below) | Ride the Dinofly |
| M / gamepad Start | Travel menu (see below) | Travel menu |
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

**Life points.** Twinsen has the original's 50 life points (`ULBAHitPointsComponent`, no HUD: hit stars fly out of
his head, one per 5 points lost, and he plays his hit reaction). What hurts him:

- falls past a hard landing (1100 cm/s, a 6 m drop), 0.082 points per cm/s faster: 7 m costs about 6, 12 m about 36
  (he lands on his head), 15 m kills;
- hit zones (`ALBAHitVolume`, the original's type 8 zones, and the feature map's red floors), through Unreal's damage:
  stars only, no hit reaction (it would hold him in place in the zone, hit after hit);
- enemies whose `TouchDamage` is set (the feature map's; the scenes' enemies still don't fight back), touching them
  throws him back.

After a hit nothing hurts for *Hurt Invulnerability* (0.8 s); he regains 1 point a second after 4 s without one (0
restores the original's no regeneration). At zero he falls on his head and the death screen offers Retry: the
level again, or in a test level its station closest to where he died. Tunables under *LBA|Pain* on the character.

`ALBACharacter` loads its behaviour entities from **Project Settings > Plugins > LBA2 > Hero** and can be
subclassed in Blueprint. `SetMoveInput` and `DoBehaviourAction` let AI or scripts drive it.
`ULBAAnimInstance` plays LBA animations natively without an Animation Blueprint: crossfades, LBA loop
frames, root motion, a bone-branch layer, synced cycle blends, smoothed keys and the procedural pose.

### Riding the Dinofly (prototype)

F gets Twinsen on the Dinofly, on the ground: a Dinofly waiting nearby is mounted where it stands, otherwise
one comes to Twinsen's spot (body 0 of `DA_Dinofly` has Twinsen on its back). On the ground (or the sea) it
walks slowly (get off to go faster); climbing takes off, with a crouch and a push-off, the legs folding up
as it rises (each press of climb is a wing beat). Touching the ground from anywhere lands at once: the legs come down during the last meters of a
descent and the landing absorbs the touchdown. Legs and wings are animated separately there (a layer of
`ULBAAnimInstance` on some bone branches): the wings keep beating at a natural pace while the legs fold or
unfold as fast as needed. `ALBADinofly` flies with `ULBAFlightMovementComponent`, in two
modes:

- **Hover** (default): steered like a simplified helicopter (forward or backward and turn, with some inertia),
  held up like a bird: pump A to climb, hold X to dive, tap and hold B to brake (see
  [Dinofly flight: choices and tradeoffs](#dinofly-flight-choices-and-tradeoffs)). Its body leans the way the flight's
  forces lean it, and its wings move procedurally with what it does.
- **Glide** (Tab): a small aerodynamic model (lift and drag from speed and angle of attack, banked turns,
  flapping thrust) where speed and altitude trade; harder to fly.

The camera stays behind the Dinofly, swinging after it with some lag (`CameraFollowSpeed`). It rises and
looks down at the Dinofly and the ground when descending or high above the ground (`HighAltitude`,
`MaxCameraDrop`), and its field of view widens with speed, accelerations, climbing or
descending, and height. Looking around with the mouse or right stick (vertical inverted on the stick) turns all the
way around, and the view stays where it was left. It never
goes under water: it floats on the sea (and skims off it when gliding in fast).

| Key | Hover | Glide |
|---|---|---|
| W/S, up/down, left stick | Forward / backward (half speed); walk on the ground | Nose down / up (`lba.Dinofly.InvertPitch 1` swaps); walk on the ground |
| A/D, left/right, left stick | Turn | Bank; turn on the ground |
| Space, gamepad A or right trigger (press, pump) | One wing beat (take off) | Flap, held (take off) |
| Left Ctrl, C, gamepad B or left trigger | Brake: taps and holds kill the speed (tires after about 5 s) | Brake |
| Left Shift, gamepad X or right bumper (hold) | Dive, the longer the steeper (in the air only); release to pull out | |
| Mouse, right stick | Look around (all around; stays put) | Look around (all around; stays put) |
| Tab, gamepad View | Switch to glide | Switch to hover |
| F / gamepad Y | Get off (once landed on the ground, not at sea); the Dinofly waits there | |

`-LBA2Shots=dinofly` runs a scripted ride (mount, walk, takeoff, pumping at three paces, a hands-off stall caught by the
brake, cruising and a banked turn, a short and a long dive, braking the speed away, a landing tapped out in Morse, a hit
standing, braking until the wings are spent, a hit in flight, a hands-off fall that hurts, dismount, then a death and the
retry screen) that logs the flight state (air density, beats, stall, brake and its endurance, dive, life) and screenshots
each step.

#### Dinofly flight: choices and tradeoffs

Three buttons, three rhythms:

- **Pump A at different rates: different rates of climb.** A beat is a punch that fades quickly, then a long tail, with
  a lot of inertia. One beat every 2.5 s holds the altitude, one per second gains some, one every 0.5 s climbs faster:
  more effort for quickly decreasing returns (a beat soon after another is weaker), so frantic pumping is barely better
  than a steady pace, and each player finds their own sweet spot. The wings beat once per press, twice as fast as they
  used to.
- **Hold X for different durations: a more or less aggressive dive.** The path steepens from 20 to 80 degrees over the
  1.5 s it is held, the wings folding tighter; releasing pulls out with the speed gained (holding A meanwhile zooms up).
- **Play Morse on B to stick a smooth landing.** Braking kills the speed, the fall and the forward speed; a tap brakes a
  moment, a hold longer. Taps and holds keep the wings working, so tapping all the way down lands softly. The wings
  tire: about 5 s of braking spends them (they droop, the brake stops) until they recover (10 s, faster standing).

Between beats the Dinofly glides down slowly. But a few seconds without a beat or the brake (3 s), unless it flies fast
enough to carry its weight, the wings give up: it **stalls** and falls faster and faster, nose first, up to 20 m/s. A fall
left alone is deadly: from more than about 9 m it hurts, two from 20 m kill it, one from 40 m does. A beat (two from a
full stall) or the brake catches it. Staying up is a choice made all the time, between options with different costs:

- **Flying forward.** Speed gives lift, much more in the thick air near the ground: low and forward, speed gives
  altitude for free (cruising climbs by itself up to about 21 m) and never stalls. But low and fast is where hitting
  things hurts.
- **Thin air.** The air thins out quickly with altitude (half as dense every 30 m): up high, beats, speed and the brake
  lift less, the glide sinks and stalls fall faster, speeding up is harder. Cruising up there stalls slowly unless you
  pump. That sets the practical ceiling (about 41 m pumping three times a second).

**How it shows.** No display: the body and the wings tell what the Dinofly does. The body leans the way the flight's
forces lean it, following like a spring (a little late, swinging slightly past): at speed its nose follows its path (down
in a dive or a stall, a bit up in a climb), slower it tips with the accelerations (nose down speeding up, up braking),
sideways accelerations bank it (turns are coordinated), and the nose rises on the power strokes and when the wings flare.
The wings are moved procedurally over the spread-wings pose of the flapping cycle (animation 218 held at 0.12 s;
`FLBAProceduralPose::BoneRotations` turn the shoulder, elbow and tip bones), every change eased:

| State | Wings |
|---|---|
| Gliding | Spread, breathing slowly |
| A beat | One full stroke, the power stroke first |
| Stalling, falling fast | Pushed up into a V, fluttering |
| Braking low (under 4 m) | Big beats reaching forward, flared, legs down: landing |
| Braking (4 to 15 m) | Fast hovering beats |
| Braking high (over 15 m) | Flared wide like a parachute, slow strong beats |
| Spent | Drooping, weak flaps |
| Diving | Swept back, the longer the tighter, buffeted; spread and flared pulling out |
| Turning | The tail steers |

**Pain.** Hitting anything faster than its pain speed (the part of the velocity going into the surface) hurts the
Dinofly: landing on the ground, hitting walls, trees, rocks, or the water (which hurts a bit less). Like the original,
there is no life display: white line-drawn stars fly out of it (the original's hit star, RESS.HQR POF 0; one per
5 life points lost, up to 5), and it flinches (animation 5: standing after a hard landing, the head and neck in
flight). Standing on the ground it slowly recovers. At zero life it dies: it drops limp and lies on its side (animation
335, the rider thrown off), and after a moment "You died!" offers to retry: the level starts over
(`ULBADeathScreen`). Hits are accepted only by actors with a `ULBAHitPointsComponent` (life, stars, hurt and death
events); actors without one don't take hits.

**The key values** (everything that shapes the experience) are in Project Settings > Game > LBA2 Dinofly
(`ULBADinoflySettings`, saved in `Config/DefaultGame.ini`, applied at once, even while playing). Its read-only
*Derived* summary shows what they give, simulated with the flight model itself: steady climb rates per pumping pace and
altitude, ceilings, falls, the brake, damage examples.

| Setting | Default | Intended effect |
|---|---|---|
| Hold Beat Interval | 2.5 s | THE pumping value: one beat every 2.5 s holds the altitude at the reference altitude; each beat's strength follows from it |
| Beat Recovery Time | 0.7 s | Diminishing returns: a beat this soon after the previous one has 63% of its strength |
| Beat Punch, Punch Time, Tail Time | 60%, 0.12 s, 0.9 s | Shape of a beat: the punch, then the long tail |
| Held Beat Interval | 0 (off) | Holding A beats by itself at this pace |
| Beat Frequency | 3.25 strokes/s | Speed of a beat's wing stroke |
| Glide Sink Speed | 2.2 m/s | Sinking between beats at the reference altitude (faster higher) |
| Vertical Inertia, Catch Rate | 0.5/s, 1.5/s | How slowly the vertical speed settles back to the glide (long carry after a beat), and how fast the spread wings catch a fall |
| Stall Grace Time, Stall Time | 3 s, 1.5 s | How long without a beat or the brake before the wings give up, and how quickly the stall sets in |
| Stall Acceleration, Fall Terminal Speed | 7 m/s², 20 m/s | How hard and how fast a stall falls |
| Beat Stall Recovery | 0.5 | Share of a stall one beat recovers |
| Brake Vertical Rate, Brake Deceleration | 2.5/s, 9 m/s² | How fast braking kills the fall (or rise) and the forward speed |
| Brake Tap Time | 0.15 s | The shortest brake (a tap) |
| Brake Endurance, Recovery Time, Resume Fraction | 5 s, 10 s, 30% | Seconds of braking before the wings are spent, time to recover, and how much must come back before it works again |
| Brake Style Heights | 4 m, 15 m | Which wing style braking uses (landing, hovering, parachute) |
| Dive Angles, Dive Ramp Time | 20 to 80 degrees, 1.5 s | How the dive steepens the longer X is held |
| Reference Altitude, Air Halving Height | 10 m, 30 m | Where the values above apply; how quickly the air thins out |
| Cruise Speed, Cruise Lift, Max Speed Lift | 8 m/s, 1.3, 4 | Forward flight, and the share of the weight its speed carries (x density x speed squared; above 1 it climbs) |
| Align Speeds, Acceleration Tilt, Climb Nose Follow | 3 to 10 m/s, 0.6, 0.6 | Attitude: from which speed the nose follows the path, how much accelerations tip the body, how much the nose follows a climb |
| Glide / Brake / Beat Nose Up | 4, 20, 8 degrees | The nose above the path |
| Pitch Limits, Max Bank | -80 to 50, 60 degrees | |
| Attitude Frequency, Damping | 0.9 Hz, 0.6 | The body's spring: how quickly it follows, how much it swings past |
| Max Life | 100 | |
| Ground / Obstacle / Water Pain Speed | 6.5, 3, 9 m/s | Speed into the surface that starts to hurt |
| Damage Per Speed, Water Damage Scale | 0.11 per cm/s, 0.7 | How fast it dies (flying is dangerous): a wall at cruise speed costs 55 (two kill it), at twice the cruise 143, a 20 m drop 63 (two kill it), a 40 m drop 114 |
| Life Regen Per Second, Damage Per Star, Retry Delay | 0.5, 5, 2.5 s | Standing on the ground heals slowly (a 20 m drop takes two minutes) |

With the defaults, pumping without moving forward gives (m/s; the last column cruises forward without pumping, averaged
over 20 to 40 s: up high it ends up stalling):

| Altitude | every 2.5 s | 1 s | 0.5 s | 0.3 s | cruising |
|---|---|---|---|---|---|
| sea level | +0.8 | +3.5 | +5.3 | +6.3 | +1.3 |
| 10 m (reference) | 0 | +2.1 | +3.6 | +4.4 | +0.7 |
| 25 m | -1.1 | +0.4 | +1.5 | +2.0 | stalls |
| 40 m | -2.0 | -1.0 | -0.2 | +0.2 | stalls |

Pumping once a second holds up to 29 m, three times a second up to 41 m. Stopping pumping while hovering, the drop starts
to hurt from 8.6 m; from 20 m it hits at 12 m/s (63 damage: two such falls kill it), from 40 m at 17 m/s (114: dead). The
settings' *Derived* summary counts how many of each kill a healthy Dinofly. Braking takes a fall at the
terminal speed below the pain speed in 0.4 s. `lba.Dinofly.Debug 1` shows life, altitude, air density, vertical speed,
beats, stall, brake and its endurance, and the dive as text above the Dinofly; `lba.Dinofly.Hurt <damage>` hurts it
(1000 kills it).

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

### Travel menu

M or gamepad Start (on foot or riding) pauses the game and opens the travel menu, laid out like lba2remake's
teleport menu: planets along the top (Twinsun, Emerald Moon, Zeelish Surface, Undergas), the selected planet's
islands on the left, and the selected island's scenes on the right: its areas (exterior scenes), each with the
interiors it opens on, nested (Twinsen's House Area > Twinsen's House > Cellar). The first row of an island is the
island level itself. Picking an island row opens its level at its start (its first scene's hero start, see
[Scenes](#scenes)); picking a scene goes to that scene's hero start. Where the player is shows in gold (planet,
island, scene) and starts selected. Mouse, keyboard (arrows, Enter, Q / E for planets) and gamepad (d-pad, A,
LB / RB) all work; Esc, B, M or Start close it. Packaged and standalone games open it at startup (*Island Menu At
Startup*; Play In Editor and `-LBA2Shots` runs do not).

- Islands are the levels found under **Project Settings > Plugins > LBA2 > Island Menu > Islands Path**
  (`/Game/LBA2/Islands`); their names, planets and order are a table in `LBAIslandTravelSubsystem.cpp`. Island
  variants (Citadel's storm and sunny levels, Celebration before and after the statue) are rows of their own with
  the same scenes: a scene picked under one opens in that variant, and an interior picked there leads back out to it.
- Scenes are the imported scene assets; the menu reads where each one goes from asset registry tags, without
  loading them: `ULBASceneAsset::Island` (the scene header's island, also for interiors), `MenuParent`, `MenuOrder`
  and `MenuName`. The Scenes layer sets those from lba2remake's location tree, exported to
  `Resources/CommunityMetadata/lba2_locations.json` by `Tools/export_locations.py` (demo copies left out, like the
  remake; scene 94, which the remake lists as Otringal's landing zone, is a Citadel cube in SCENE.HQR and 138 is
  Otringal's). Story scenes the tree misses go under the first listed scene with a door to them; the others (94, the
  sea travel) stay out. Reimport the Scenes layer after changing the tree.
- Icons are lba2remake's location icons (MIT), rendered from its SVGs by `Tools/export_locations.py` and imported
  into the plugin's `Content/UI` by `Tools/import_location_icons.py`; `ULBAIslandMenu`'s *Style* properties take
  others.

Nothing references the island levels, scenes or icons, so the LBA2Editor module adds them to each cook
(`ModifyCook` delegate). It lives in `ALBAPlayerController` (the controller of `LBAGameMode`), `ULBAIslandMenu` (a
UMG widget built in C++, `IslandMenuClass` takes a Blueprint subclass), `ULBAIslandTravelSubsystem` (`GetIslands`,
`TravelToIsland`) and `ULBASceneTravelSubsystem` (`FindMenuScenes`, `TravelToSceneOnIsland`, `GetCurrentScene`).
Console: `lba.Travel DESERT`, `lba.Travel` (lists the islands), `lba.Scene 0`, `lba.IslandMenu`.
`-LBA2Shots=menu -LBA2MenuScenes=7,43,138` screenshots each level, opens the menu, screenshots it, shows the next
tab, screenshots again and picks the next place: a scene number, `-1` the first test level's start, `-2-N` its
station N, `-100-N` steps onto the level's gateway N (`-LBA2MenuScenes=-5,15,-1`: Arena station, museum, back).
Pain tests, menu closed: `-200-H` drops Twinsen from H meters, `-400` onto the smallest hit zone, `-500` walks him into
the first enemy that hurts (`-501` stops), `-300` presses Retry (`-LBA2MenuScenes=-207,-212,-400,-500,-501,-300`).

### Globe mode (prototype)

Islands placed on a planet can open on it: a small round planet (Twinsun: 2 km radius) whose sea curves away to a
real horizon, the island sitting on it as imported. `lba.Globe.Open CITABAU` opens Citadel (sunny) that way; so does
any travel to it with `?Globe`, `lba.Globe 1` or **Project Settings > Plugins > LBA2 > Globe > Globe**. For now only
Citadel is placed; the Dinofly is the sea vehicle (it floats and skims on the planet's sea).

How it holds together:

- **The island stays flat.** Its level is unchanged: on and near the island the world is exactly where the level
  has it, and everything (characters, scenes, zones, the storm formula) works as before. The sea is flat over a
  *cap* around the island (its land plus 20 m) and bends down to the sphere across a 150 m ring
  (`FLBAPlanetCap`, mirrored in `Shaders/Private/LBAPlanet.ush`). In globe mode the level's Water plugin ocean and its
  invisible sea floor are removed, and its seabed is swapped for a copy bent with the sea.
- **Out at sea the world turns under the player.** Whenever the sea's up under the player is more than 0.5 degrees
  from the world's +Z, everything but the player is rotated about the player to make it +Z again
  (`ULBAPlanetSubsystem`). Gameplay code keeps its flat-world assumptions (gravity -Z, Z = height) within that half
  degree. The camera is tilted by the angle left (`ULBAPlanetCameraModifier`), so the turns don't show; surface
  patterns (dither dots, cartoon sea) are laid out in the island level's space, and the sea's motion vectors follow
  the sea, so TSR and motion blur don't see them either. Back near the island, the world snaps back where the level
  has it, taking the player along. Invisible markers (volumes, starts, target points) never move.
- **The sea** is a disc of rings kept under the player and bent onto the planet by its material (`M_LBA_PlanetSea`:
  single layer water with the home island's water colour, so the palette shading draws it as cartoon water); sea
  queries (`GetSeaSurface`, used by the Dinofly's floating and its altitude for air density) are analytic.
- **Sky:** the island's sky atmosphere becomes the planet's (centered on it, 2 km ground radius); a second sun on
  the other side of the planet lights the other hemisphere (each sun is switched off below its horizon).
- **Placement** (`LBAPlanetBuilder`, island layer): an island goes where the original's holomap has it
  (HOLOMAP.HQR, ARROW.BIN: latitude, longitude) and is turned so its calibrated sun keeps its angles. The planet's
  sun is the direction that gives every listed island its sun's elevation (Citadel 31.3 and Desert 36.8 degrees,
  both exact). Output: `/Game/LBA2/Planets/DA_Planet_TWINSUN`, the sea disc and material, and each placed island's
  `SM_<ISLAND>_Seabed_Globe`. The planets list is a table in `LBAPlanetBuilder.cpp`.

Measured on Citadel (`-LBA2Shots=globe`, open the level with `?Globe`): about 20 turns flying 400 m out and as many
back; at each, the camera's path on the planet has no jerk (0.1 to 2 cm, as between ordinary frames). A frozen
frame turned 0.5 degrees differs from untouched ones by 1.1 (mean, 0-255; untouched frames 0.3), all gone the next
frame; turned 3 degrees, by 4.8, mostly the sky near the horizon (Unreal's height fog is flat in the world). A turn
moves about 540 actors in 4 ms; the snap back near the island takes one 36 ms frame. 140 fps on average in
standalone game. `-LBA2GlobeQuick` stops after the turn test, `-LBA2GlobeTurn=0.5` sets its angle.
Console: `lba.Globe.Debug 1` (state on screen; 2 logs every turn, 3 their cost by class), `lba.Globe.Turns 0`
(stops turning, the camera keeps correcting), `lba.Globe.Recenter 3` (turns the world 3 degrees: it must not show),
`lba.Globe.SeaErrors` (the sea material's compile errors).

Not done yet: other islands (stubs from the holomap and LBA1, Desert), the Hamalayi belt and its passes, a boat,
streaming more than one island, the fast travel menu landing on the globe, space; packaged builds don't cook the planet
asset yet. Height fog stays flat in the world (it shifts with each turn: visible only on large turns); views from high
above are hazy.

### Tests tab, feature test map and gateways

After the planets, the menu's **Tests** tab lists the test levels and sandboxes (**Project Settings > Plugins > LBA2 >
Test Levels**): the feature test map first, apart and in its own color, then the levels under *Test Levels Path*
(`/Game/LBA2/Tests`), then *Sandbox Levels* (the character gallery and Unreal's template levels). Each level lists its
stations under it, with what to try there; picking one goes straight to it. Test levels open with `LBAGameMode`
(*Play Test Levels As Twinsen*), so the template levels have Twinsen and the menu too.

The feature test map, `/Game/LBA2/Tests/L_FeatureTest` (*Feature Level*), is the project's default map (game and
editor). The **Tests** import layer builds it from prototype blocks (run it after Characters and Scenes; it only
rebuilds what it generated, your own actors stay). It is a tight course, all of it in sight from the start and
within 50 m, each zone a station of the menu:

- **Jumps** (orange, ahead left): steps up to platforms over a burning floor (gaps of 1, 2 and 3 m), ledges of 50 to
  125 cm.
- **Tower** (green, ahead right): a 10 m ladder, a balcony at 5 m, a diving board; falls hurt.
- **Arena** (blue, ahead): a walled square behind two sliding doors (sideways, upwards), the museum's enemies inside
  (they hurt on touch) and two people who can't be hurt, a burning tile, crates.
- **Slopes** (purple, right): ramps of 20 to 65 degrees.
- **Dinofly** (yellow, far left): a 25 m tower to land on (a fall from it kills), 12 m pillars close together, a crash
  wall, and gateways to where the game tries it best: the storm over Citadel (wind, rain), the harbour (the sea), the
  desert.
- **Gateways** (behind the start): every island, places of the game (Twinsen's house, the harbour, the museum, the
  sewers, the Dome, the Temple of Bu, the School of Magic, the Moon) and the sandboxes.

It is built for reading at a glance. Each zone has its own color (floor patch and structures, the template's
prototype grid recolored, `Tests/Materials`), dangers are red with yellow lines (burning floors, doors, the ladder,
the crash wall), and the sun comes high from behind the start, lighting the faces turned to the player. Signs are
dark boards on posts whose text uses `M_LBA_SignText`: the engine's text material made unlit and drawn after motion
blur, so neither the light nor the palette shading touches it (measured 15-18:1 against its board; the lit default
text came out at 1.1-1.5:1 against the scene).

Signs that explain controls (the start, Jumps, Arena, Dinofly) are `ALBAControlsSign` actors: a title, lines of key
glyphs with what they do, then notes. They show the device in use: keyboard keycaps and the mouse with its pressed
button, or the gamepad's colored face buttons, bumpers, triggers and sticks (Xbox layout), with the device named in
the corner, so a recording shows which one is played. `ULBAControlsSubsystem` watches Slate's input and switches on
a key, a click or a real mouse move, or on a gamepad button or a stick past its dead zone; the travel menu's footer
follows it too. The editor shows both, gamepad on the left and keyboard and mouse on the right.
`lba.Controls.Device 0|1|2|3` = automatic, keyboard and mouse, gamepad, both. Glyphs are drawn by Slate
(`SLBAKeyGlyph`), no textures, on a world-space widget whose material (`M_LBA_SignPanel`) is drawn after motion blur
like the sign text; the board sizes itself to them. Key lists live in `LBAControls` (`LBAControls.h`), shared by
the input mappings of Twinsen, the Dinofly and the menu and by the signs, so they can't drift apart.

Stations are `ALBATestStation` actors: player starts whose *Player Start Tag* names the station, so going there is
Unreal's URL portal (`L_FeatureTest#Arena`) and `ALBAGameMode` never starts at one otherwise. Levels list their
stations in an asset registry tag (`LBATestStations`), so the menu shows them without loading the level; place more
in any test level and they show up. Gateways are `ALBATravelGateway` pads: step on one to go to an island, a scene
(on a chosen island variant), a test level or a station; a player arriving on a pad must step off first.

### Level selector (editor)

The **Levels** button on the level editor's toolbar (next to Play; also **Tools > LBA2 Levels**, `lba.LevelSelector`)
opens the travel menu in an editor tab: the same widget, tabs and lists, rebuilt for each level opened. Picking a
place opens its level in the editor (asking to save changes first), frames it in the viewports and selects its start
or station; *Play here* plays in the editor from the place picked last (`ALBAGameMode` honours Play from Here).
`lba.LevelSelector.Go <scene number | ISLAND | station>` does the same as a pick.

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
| Sprite object (interiors: doors, signs, pickups) | `ALBASceneSprite`: a static mesh per sprite (`Sprites/SM_Sprite_NNN`, `SM_SpriteRaw_NNN`, `SM_SpriteAnim_NNN`, one atlas `T_Sprites`) with the sprite projected onto the visible faces of its box like the interior bricks: pixel exact from the original angle, solid from the others, the box as collision. Animated sprites (ANIM3DS) flip through their frames. Still sprites lba2remake made a 3D model for (its `model_replacements.json`: the museum's surveillance button and lever, chest lids, clover boxes, the casino entrance...) show that model instead (`Remake/sprites/SM_Remake_*`, its mesh as collision; *Use Remake Models*). |
| Sliding door (a sprite whose track opens it: `OPEN_LEFT/RIGHT/UP/DOWN`) | `ALBASceneDoor`, a sprite the player opens and closes (`ILBAInteractable`, see below), with its slide (`OpenOffset`: direction and width from the track), the original speed and the samples its track plays. |
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
- **Sliding doors**: E (or Space in the Normal behaviour, like the original's action) in front of a door opens it, again
  closes it. It slides into the wall like the original (by its width, 1000 units/s, with its sound) and closes by
  itself once Twinsen is away (2500 units, like the museum's script), never onto someone in the doorway. Every door
  opens: the story's conditions (keys, chapters) wait for scripts. Anything implementing `ILBAInteractable` can be
  used the same way (`ALBACharacter::TryInteract`).
- **Enemies**: objects the original lets Twinsen hurt (armour below 51, life points, no `NO_CHOC` flag: guards,
  soldiers, Esmers, robots, snakes, bats, crabs, mussels, fish-bulls... 486 objects in 138 scenes) are enemies
  (`ALBASceneActor::IsEnemy`), except allies and friendly characters (*Non Enemy Scene Entities*: the rebel Francos,
  Joe the Elf, a friendly Quetch, a Sup clerk). They get a `ULBAHitPointsComponent` and die from any blow
  (`ALBACharacter::Attack`, hit at the animation's original hit frame): stars fly, they stop blocking at once,
  play their hit reaction (the original has no death animation for them), then vanish with the original's death
  sound (*Enemy Death Sound*, sample 14). They don't fight back. Reloading the level brings them back.
- **Starting**: a level starts at the hero start of its lowest-numbered scene (islands: Citadel at the Lupin-Burg
  landing zone, scene 42), or at its plain `PlayerStart` without scenes.
- `lba.Scene <number>` goes to a scene's hero start; `lba.Scene` lists the imported scenes.

Not yet: scripts (objects stand in their start state, so some show that the story would hide or move), camera
zones (data only: the modern camera ignores them), givers, messages, escalators, rails, ambient sounds and music.
Doors and `lba.Scene` find scenes under **Project Settings > Plugins > LBA2 > Scenes Path**, and the LBA2Editor
module cooks every scene asset and its levels.

`-LBA2Shots=scenes -LBA2Doors=49,0 -LBA2SceneEnd=43` takes the doors to those scenes in turn (walking into the
ones that need it), then `lba.Scene 43`, with a shot in each level; `-LBA2Shots=ladder -LBA2Ladder=44:7` climbs a
ladder zone (scene:zone) up and back down; `-LBA2Shots=door -LBA2Door=15:5` opens a door (scene:object), walks
through it and checks it closes once Twinsen has left; `-LBA2Shots=attack -LBA2Attack=15:15` stands next to an enemy
(scene:object), punches it and chains a kick, and checks it dies and vanishes. `Tools/dump_scripts.py <scene> [object...]` disassembles
objects' track and life scripts (opcode tables from the lba2remake checkout).

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
  and color per island, storm clouds over Citadel). The Emerald Moon has no atmosphere. Citabau (nice
  weather) has a *clear sky* instead (`ClearSkyColor`): a deep cerulean sky with white clouds, which the
  sun lights as brightly as the sky is tinted (its *Cloud Scattered Luminance Scale*); the sky light and
  the water are calibrated for the brighter clouds.
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
- Community names (scene, entity, actor and zone names) and the travel menu's location tree come from
  lba2remake's metadata, bundled in `Resources/CommunityMetadata` with its MIT license; the menu's location icons
  (`Content/UI`) are lba2remake's too.

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
- [ ] Enclosed interiors (Enhanced look): walls and ceilings from a plan of rectangular rooms, connectors (stairs,
  passages, exits, alcoves) and spaces, each found by an explicit architectural rule (done), wallpaper per space
  and building (done); next: walk every library's grids for more rules, painting rules (neutral default per
  library, local overrides), see-through dithered walls between Twinsen and the camera, Original look stays
  open (enclosure, ceilings and bounce lights off, frames back)
- [x] Citadel's storm wind: a small hurricane around the island (calm eye, wall, gusts, turbulent ceiling) that carries the Dinofly and blows the rain
- [ ] Look, next: Citadel storm (Niagara splashes like the original's spray, lightning, wet surfaces), cloud shadows, more interior lights (candles, original bricks),
  sprites; shallows (a seabed sloping down from the coast), Zeelich's cloud sea
- [x] L3c Modern controls (third-person template style; procedural layer over the LBA clips rather than motion matching, too few clips)
- [x] Dinofly riding prototype: mount on the ground, walk, take off, hover (default) and glide flight, land, float, get off
- [x] Dinofly flight choices: pump A (climb rate), hold X (dive), Morse on B (brake, tires), stalls when not pumping, lift from speed in thick air, attitude from the flight's forces, procedural wings, pain (hit stars, flinch), death and retry
- [x] Island menu (M / Start) to travel between islands; packaged builds start on it
- [x] Travel menu by planet, island and scene (interiors included), after lba2remake's teleport menu
- [ ] Globe mode: islands on a round 2 km planet, the world turning under the player at sea (prototype: Citadel, the
      Dinofly at sea; next: island stubs, the Hamalayi belt, a boat, fast travel onto the globe, space)
- [x] Tests tab, feature test map (stations, gateways; default map), editor level selector on the toolbar
- [x] Twinsen's life points: falls, hit zones and enemies that hurt on touch, hit stars, death screen and retry
- [x] L4 Scenes: per-scene levels streamed into islands and interiors, objects, sprites, typed zone volumes,
  track points, hero starts; doors and ladders work in game
- [x] Sliding doors used by interacting (E), sprites replaced by lba2remake models
- [ ] Scenes, next: ambient sounds, givers and messages, escalators, camera zones (as hints for the modern camera)
- [ ] L5 Scripts: life/move scripts (on hold; interpreter recommended over Blueprint generation)
- [ ] L6 Text (String Tables), voices, music, sprites, inventory objects, holomap, screens
- [ ] Procedural wall/pattern builder using interior kit pieces
- [ ] Light-only remake GLBs as point lights
