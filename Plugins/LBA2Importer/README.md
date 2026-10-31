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
- **Indoor camera.** In an interior scene the camera comes in close behind Twinsen, like third-person
  games in buildings: 1.9 m instead of 3.6 m, aimed a little higher, a narrower field of view (75
  degrees), starting slightly lower. The room's walls and ceilings stop it, so it stays in the room
  (they ignore the camera channel, so that the far outdoor camera sees into rooms; the indoor camera
  sweeps against static geometry itself). `lba.Modern.IndoorCamera 0` keeps the outdoor camera
  everywhere.

Tunables are under *LBA|Modern* on the character: speeds, jump, camera distances, heights and indoor
field of view, lean, head turn, and the leg/head bones. `-LBA2Shots=modern` runs a scripted test
(starts, stops, walk, turns, reversal, jumps; side views with bursts of shots and a per-frame log).
`-LBA2Shots=camera` with `-LBA2HeroAt=x,y,yaw[,traceZ]` shoots the player's own camera around Twinsen
standing still (ahead, right, behind, left, up, down) and logs whether he is indoors, the arm length
and the camera height.

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
LB / RB) all work; Esc, B, M or Start close it. Packaged and standalone games start playing on the planet
([Globe mode](#globe-mode-prototype)); *Island Menu At Startup* opens the menu over it first instead (never in Play In
Editor or `-LBA2Shots` runs).

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

Nothing references the island levels, scenes, icons or planets, so the LBA2Editor module adds them to each cook
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

Islands placed on a planet can open on it: a tiny round planet (Twinsun: 320 m radius, the holomap's size) whose sea curves away to a
horizon, far islands sinking below it until they come close. The planet is a level of its own,
`/Game/LBA2/Planets/L_TWINSUN`: an empty persistent level whose sub-levels are the placed
islands' levels where they are on the planet's map, each with its scenes (Unreal never loads a sub-level's own
sub-levels, so the planet lists them itself), the belt and the home island's sea. The home island (Citadel, sunny: the
first placed) keeps its level's place, so its coordinates are the same as in its own level; its sky, lights and look are
the planet's, the other islands' go when it plays. Importing an island of the planet (Islands layer) or its scenes
(Scenes layer) rebuilds it; open in the editor (not playing), it shows every island's own sky and sun. A building's
exit leads out onto the planet whenever its island level is placed there (sunny Citadel, the desert island), whichever
level the player came in from; Citadel in the storm isn't on the planet, so its buildings still lead back to it.
The game starts in Twinsen's house (its interior level is the default map); an island not played yet opens as its
variant on the planet, so stepping out leads onto it.
`lba.Scene` on the planet stays on it.

In the editor, the toolbar's **Globe** button (next to Levels; also Tools > Play on the Globe, or `lba.Globe.Play`)
plays the planet's level in the level viewport without opening it here. An island's own level opens on the planet too
(the other islands streamed in around it): in game, `lba.Globe.Open CITABAU`, or any travel to it with `?Globe`,
`lba.Globe 1` or **Project Settings > Plugins > LBA2 > Globe > Globe**. Citadel
and the desert island are placed where the holomap has them, 226 m apart (54 m of sea between their coasts): open either one and the other is out there,
across the sea. The Dinofly and the ferry are the sea vehicles. All around, past the equator, the Hamalayi belt closes the islands' hemisphere.

How it holds together (the world is flat and still; only the view is round):

- **Islands laid flat on a map.** The opened island's level is where it was saved; every other placed island's level
  is streamed in as a level instance where it is on the planet's flat map (`FLBAPlanetIsland::GetLevelToMap`): the
  hemisphere of Citadel and the desert laid flat around its pole, distances from the pole and longitudes kept
  (azimuthal equidistant; the code calls it north, the holomap's -Y pole turned on top, but in the story it is the
  southern hemisphere), so distances between islands near the pole are kept too (Citadel to desert: 226 m on the
  planet and on the map; they stretch east-west by 11% at 45 degrees from the pole). Nothing moves: collision, navigation, gravity
  and every gameplay system see a flat world, static objects stay static. Streamed islands come without what the
  opened island's level brings for the whole planet (sun and lights, sky atmosphere, sky light, clouds, height fog,
  look volume, rain, storm, player starts) and without their scenes (empty islands for now).
- **The view bends.** From the camera on, every generated material moves its vertices down onto a sphere of the
  planet's radius with the camera on top (`LBAPlanetBendOffset` in `Shaders/Private/LBAPlanet.ush`, added to the master
  materials by `LBAMaterials::AddPlanetBend`): a point Distance away goes Distance along the sphere, its height above
  the sea becoming height above the sphere (far islands tilt away). There is no flat circle around the camera (there
  was one, 100 m, until 2026-10-30): the ground under the player curves as the planet's, without the distortion where
  a flat circle met the curve. It is centered on
  the camera's final location each frame (`ULBAPlanetCameraModifier` writes `MPC_LBA_Planet`), and last frame's camera
  gives the motion vectors (previous frame switch), so TSR and motion blur follow the bend. Without a planet the
  offset is zero. The palette shading undoes the bend (`LBAPlanetUnbend`) for its dither dots and cartoon sea, so
  they stay put on far surfaces. At 750 m radius, from the desert's shore, Citadel's center is 30 m down; from the water 285 m away,
  Citadel is entirely below the horizon (at 2 km radius it stood on it).
- **The sea** is the Water plugin's, as in the island levels: the opened island's sea level
  (`/Game/LBA2/Planets/L_<ISLAND>_GlobeSea`, streamed in) has one water zone over all the sea the bent view can show
  from the islands (out a quarter way around the planet: 1.5 to 1.7 km across for Twinsun, 0.8 m per
  water info texel: 2048 texels across, as the islands' levels at most; 4096 overran an 8 GB card's video memory) and an ocean per island, each covering its island's cell of the zone (straight cuts between the
  islands' land) and keeping its island's coastline dry, so low land inside the coasts stays dry (the desert's beaches
  no longer leak). The oceans take the opened island's ocean material and waves. The water material (`M_LBA_Water`,
  Unreal's ocean material clipped at the coast) bends like the generated masters, its normals turned with the bend.
  The zone has no far mesh (a few large flat tiles can't bend); beyond it, the planet's sea disc (`M_LBA_PlanetSea`,
  rings kept under the camera, bent by its material) lies 10 m lower, out of sight: far from the camera the water
  mesh's large tiles slope down towards the coasts. Without a sea level (or `lba.Globe.Sea 0`) the disc is the whole
  sea, at sea level. The islands' own water and invisible sea floors are removed, their seabeds stay (bent by their
  material). Sea queries (`GetSeaSurface`: the Dinofly's and the boat's floating, air density) answer the sea level,
  Z 0. The water's pixel shading (shore depth, foam, ripples) is looked up where the bend puts it, a little closer to
  the camera than where it is (2 m at 300 m, 19 m at 500 m), which only shows far away.
- **Sky:** the island's sky atmosphere becomes a planet of the same radius centered under the camera, so its horizon
  stays below the sea's. Its clouds keep their level's layer (5 to 15 km up: the cloud material forms nothing in a
  layer lower than 2 km). Such a small planet's sky is darker towards the horizon (low rays leave its air fast), so
  the sky light is made brighter by the island's `SkyLightGain` (Citadel 1.55, desert 1.35, measured at 2 km). At
  750 m the island's ground is as light as in its level (park 0.99 to 1.07); the sky near the horizon is about 20%
  darker, and so are its reflections on the sea: Citadel's harbour is (1, 112, 158) sRGB against (43, 156, 199) in
  the level (the sea disc gave (7, 48, 79)); open water (138, 202, 245) against (155, 206, 239).
- **Lighting:** no Lumen on the planet. Lumen (and distance field AO) trace the distance fields of the flat world,
  which the bend doesn't move: a bent surface lies d^2 / 2R below the flat one at d from the camera (22 m at 120 m on
  Twinsun), buried in the flat world there, so it lost its sky light the further it was: an island 90 m away was dark
  navy, light grey from 15 m, and things lit up as the camera came close. The planet actor's post process
  (`ALBAPlanetActor::GetLighting`, unbound, over the island's) turns global illumination off and reflections to screen
  space; the sky light casts no shadows (they would be distance field AO) and is turned down to 0.85
  (`lba.Globe.UnoccludedSkyLight`), measured unbent against Lumen on Citabau: open pier 120 against 126, meadow 81
  against 80, town 105 against 103 (walls under eaves about 8% darker, plants 5% lighter: no bounce light).
  `lba.Globe.Lumen 1` brings Lumen back.
- **The sea's reflections without Lumen:** the sea's normals turn with the bend, so far water reflects directions under
  the flat world's horizon, where the sky light is black (as in the island levels). The far sea went deep blue with
  pale streaks and a dark rim at the horizon; unbent, it looks the same with or without Lumen. Lighting that lower half
  of the sky light would also light the land from below (walls and cliffs went pale), and the atmosphere's own sky down
  there is far too bright on a 320 m planet (its ground is lit by the sun times the sky's tint boost). So the sea
  adds it alone: `M_LBA_Water` (v3) gives off the sky light sampled at its reflection mirrored above the horizon, times
  water's Fresnel, where the reflection goes below it (`LBAPlanetSeaSkyBelowDirection`/`Weight` in LBAPlanet.ush),
  scaled by `MPC_LBA_Planet`'s `PlanetSkyBelow`: `lba.Globe.SeaSkyBelow` (0.75) without Lumen, 0 with it. Measured on
  Citabau against Lumen: far sea (131, 179, 215) against (136, 177, 212) (it was (45, 104, 138)), horizon rim
  (110, 185, 229) against (105, 178, 222) (was (18, 78, 115)). The sea disc's material doesn't have it (it is hidden
  under the globe oceans).
- **The Hamalayi belt** (`LBAPlanetBelt`, `/Game/LBA2/Planets/L_TWINSUN_Belt`, streamed in on the map around the
  opened island, `ULBAPlanetAsset::BeltLevel`): low icy mountains all around, 424 to 513 m from the pole, traced
  from the original's holomap (HOLOMAP.HQR: its texture gives the belt's shore, the foot of its snow and its far
  edge at each longitude, its altitudes where the crest is higher or lower), a third as wide as the holomap's band.
  Across it: a pale shelf about 24 m wide under 0.3 to 2.5 m of water (the water over it turns light cyan, like the
  holomap's band between the belt and the deep sea), a flank up to 6 m (walkable, the Dinofly lands there), a cliff
  band up to 16 m at 68 degrees (steeper than Twinsen walks: 55), snow up to crests of 20 to 30 m, the far side down
  into the other hemisphere's sea. Colored like the holomap: blue-white ice and snow on the islands' side (light blue
  flank, deeper blue cliffs, no bare rock), brown rock on the far side. Nothing but its shape stops anyone: boats run aground, Twinsen stops at the cliff, the Dinofly flies over it
  (32 sectors of 11.25 degrees, vertex colored with `M_LBA_Color`, collision from their triangles; 200k
  triangles). Laid on the map, its length is stretched sideways by about half (4 km around at the shore against
  2.8 km on a real planet of that size). `lba.Globe.Belt 0` leaves it out; it is laid out for the asset's radius,
  so `lba.Globe.Radius` leaves it out too.
- **Stub islands** (`LBAPlanetStubs`, `/Game/LBA2/Planets/L_TWINSUN_Stubs`, `ULBAPlanetAsset::StubLevel`, streamed in
  with the belt): the islands the holomap paints but the game never visits, copied from its painting as a first
  prototype of the rest of the hemisphere. LBA2's holomap texture is LBA1's repainted (same islands, same places;
  only Citadel and the desert were redrawn, plus two islets), so LBA1's arrows name them: Principal (32,600 m2 of
  land, 4 times Citadel's), Rebellion (15,700), Proxima (8,500), Islet_1 south-west of Citadel, Islet_2 east of the
  desert. The painted land, smoothed, is the coast. The ground follows a grammar measured on LBA2's own islands
  (`Tools/terrain_stats.py` over every .ILE, prototyped in `Tools/stub_grammar.py`): a low shore at 0.4 m, flat
  benches at a few levels (2.8 m, then 2.5 to 6 m apart, up to 0.15 sqrt(land m2): 25 m on Principal), each where a
  wanted height passes halfway to it (the painting read by hue: olive lowland, brown ridges, pale stone highest; plus
  broad noise for hill masses), cliffs 1.4 m wide between benches and along headlands, ramps 9 m wide on a quarter of
  the edges, beaches in bays (10 m of sand before the first bench), rolling ground over it all; a pale shelf under the
  sea around them. Measured like LBA2's: about a third of the land flat (LBA2 20-40 %), most of the coast cliffs
  (Citadel 60 %), a tenth beaches (Citadel 10 %). Vertex colored with LBA2's own ground colors (rock on cliffs, the
  coast's rim and steep ground, sand on beaches, grass or earth on benches as painted, pale stone on a pale-painted top
  bench); collision from their triangles. Painted land under a placed island is left out. `lba.Globe.Stubs 0` leaves
  them out.
- **Placement** (`LBAPlanetBuilder`, island layer): an island goes where the original's holomap has it
  (HOLOMAP.HQR, ARROW.BIN: two angles that turn the arrow onto the globe as HOLOGLOB.CPP does, not latitude and
  longitude themselves) and is turned as it is painted on the holomap's globe (a heading in the planets table), else
  so its calibrated sun keeps its angles; the planet's sun is the direction that gives every listed island its sun's elevation (Citadel 31.3 and desert 36.8 degrees, both
  exact). One sun lights the flat world: the opened island's. Output: `/Game/LBA2/Planets/DA_Planet_TWINSUN` (radius,
  islands, each island's coastline, ocean material and waves, sea level), the sea levels, the sea disc
  and its material; every placed island's import rebuilds every sea level. The planets list (radius, islands) is a table in
  `LBAPlanetBuilder.cpp`; the builder logs each pair's distance on the planet, on the map and between their coasts, and warns when two
  islands' coasts come within 20 m of each other on the map. Each island's ocean is a rectangle of the zone keeping its
  coastline dry, the rectangles split by straight cuts through the sea between the islands' land, turned all together
  when no cut along the world's axes fits (islands close together diagonally). Twinsun: Citadel at latitude 53.5,
  longitude -141.2, heading 2 (its painting's: 0.69 of overlap, mirrored 0.59; its sun's would be -14.9), about halfway
  from the pole (204 m) to the belt's shore (446 m there); the desert island at 84.2, -9.6, 32 m from the pole, heading
  -20, its racetrack facing the tip of Citadel's lighthouse spit as on the holomap (also its painting's best fit, 0.76
  of overlap; its sun's -74.7 fits at 0.63), 54 m of sea between their coasts. The radius is the holomap's: on a 320 m planet Citadel
  island has the same land area as painted on its globe (8477 against 8480 m2); the desert's painting, smeared over
  the pole, is 1.3 times bigger. `Tools/dump_planet.py` (in the
  editor) then `Tools/holomap_fit.py` lay the holomap's texture on the flat map with the placed coastlines and fit
  each island to its painting (`Saved/LookDev/holomap`).
- **LBA1 against LBA2** (`Tools/lba1.py` reads LBA1's scenes, grids, libraries and bricks, env LBA1_GAME, default the
  GOG install; `Tools/lba1_rosetta.py` writes `Saved/LookDev/rosetta`): how LBA2 turned LBA1's Citadel and desert
  into its islands, to place LBA1's towns on the stub islands the same way. LBA1's iso scenes are joined by exits at
  their edges, which put every scene of an island in one frame (31,744 units apart, two cells overlapping); doors
  into buildings both games kept are matched, and a scale, turn and offset fitted. Citadel: LBA1's five outdoor
  scenes are LBA2's spine, in the same order and the same directions, one scene per cube (Outside the Citadel ->
  Flower's Circle, near the pharmacy -> Lupin-Bourg statue, near the tavern -> Lupin-Bourg landing zone, near
  Twinsen's house -> Twinsen's House Area, Harbour -> Harbour); Wizard's Tent, Dome, Lighthouse and Woodbridge are new.
  Best fit 0.86 and -6 degrees, but no single fit holds (door residuals 5 to 10 m): the districts moved as blocks.
  Inside Lupin-Bourg, the distances stayed or grew (tavern to pharmacy x1.11, pharmacy to ticket office x1.5), while
  the gap from town down to Twinsen's house shrank (x0.69) and the harbour moved about 12 m further out west. The
  tavern is Chez Luc, LBA1's architect's house is the Neighbour's house (2 m off under the fit), the warehouse
  probably Paul's house. Land: LBA1's five scenes 6,300 m2 of ground, LBA2's island 8,100 m2. The desert shares two
  points: the well into the Temple of Bu (LBA1's hero script changes scene in a scenic zone, a ("zone", scene,
  number) key; LBA2's door to the Well; same height within 3 cm) and the ferry (LBA1's model 27, found only in the
  ferry ports, moored at the military camp, an ("actor", scene, index) key; LBA2's ferry in Town Square). From the
  well, the ferry lies 18 degrees further round in LBA2 and 1.43 times as far (69 -> 99 m). The sheet turns LBA1 18
  degrees about the well, unscaled: "Outside the Temple of Bu" covers LBA2's Temple of Bu cube, and the camp's
  shore faces north like LBA2's harbour but lies 30 m short of it, on the Graveyard and Center Dunes. As on Citadel,
  the harbour moved as its own block; with only two points the turn is a hint (without it the harbour is 39 m off
  instead of 30). The island was redrawn and grown around the temple: two LBA1 scenes (2,800 m2) against 19,700 m2.
- **LBA1's towns on the stub islands** (`LBAPlanetDistricts`, `Formats/LBA1` reads LBA1's data from the importer's
  LBA1 Directory, else the GOG install; prototype `Tools/district_pads.py`, sheets in `Saved/LookDev/district_pads`):
  each LBA1 outdoor scene of Principal, Proxima and Rebellion, grouped with the ones it joins by edge exits into a
  district, is laid on its stub as a pad of ground reserved for its town. Where: the mean of its scenes' holomap markers
  (LBA1 keeps one per scene, RESS.HQR entry 8 by scene number; on Citadel each is 12-40 m off the truth, their mean
  13 m). Which way: LBA1's x east and z north, unscaled. Then fitted to the stub's coast: the offset (within 45 m) and
  turn (within 30 degrees) where its land lies on the painted land and its water on the sea, near the markers and
  heading 0, largest district first and each off the ones before. The pad: its land at LBA1's ground (the level 20 %
  of the top bricks around each cell, 11 m, are under: streets, not roofs; over each scene's own sea level, which LBA1
  keeps per scene), its water carved into the sea at least 1.2 m deep, the stub's ground eased into it over 6 m, its
  colors the ground's top bricks'. Land a pad needs off the painted coast is grown (the stub's grid reaches 60 m
  further); LBA1's small water (fountains, gutters) and brick-less holes under 30 m2 are ground. Districts: Principal
  harbour (7 scenes: the harbour, Outside the fortress, Old Burg, Ruins, Outside the library, Military camp, Peg Leg
  Street; 7,500 m2 of land on the south coast, turned 9 degrees, 11 m from its markers, 74 % of its land on painted
  land and 94 % of its water on the sea; Old Burg's streets stand 3.5-5 m over the harbour), Port Belooga and the Water
  tower (each alone, reached by ferry or a path; within 6 m of their markers); Proxim City (2 scenes, 10 m off) and its
  three rune stone scenes, islets in the sea where their markers are; Rebellion harbour (moved 29 m from its markers,
  out of the bay's water onto its shore). Each district gets a note in the stub level
  (`District_<island>_<name>`) and a log line with its fit and its LBA1 frame's transform to the map, for building its
  scenes there (L4).

Tests: `-LBA2Shots=globe` on Citadel (open the level with `?Globe`): the Dinofly flies out to sea and back, floats,
shots from 500 m and 1.5 km up (the whole planet, both islands on it). 102 fps on average in standalone game (123
with the sea disc alone).
`-LBA2GlobeTo=DESERT`: across to the desert until within its reach, looks back from
above, lands. `-LBA2Shots=boat -LBA2BoatTo=DESERT` crosses by ferry, `-LBA2BoatTo=BELT` drives at the belt until
it runs aground. `-LBA2Shots=belt`: the Dinofly flies to the belt and over it, the planet from above the pole, then
it lands on the flank and Twinsen walks and jumps up it (logs how high he got: 7 m, stopped at the cliff; shots
Saved/LookDev/belt). Shots: Saved/LookDev/flatglobe750 (and
flatglobe1000, flatglobe1 for 1 and 2 km, compare_750_1000.png, flatglobe_cmp.png against the flat level; the Water
plugin sea against the disc in globesea, sheets sheet_a to sheet_j).
Console: `lba.Globe.Bend 0` (the flat world unbent, to compare),
`lba.Globe.Radius 1000` (m, for the next level opened: places and bend; the sea disc is built for the asset's radius),
`lba.Globe.Debug 1` (camera height, nearest island and how far it sinks, horizon distance), `lba.Globe.Islands 0`
(the next island opened comes alone), `lba.Globe.Sea 0` (the sea disc alone, for the next island opened), `lba.Globe.Belt 0` (no belt, next island opened),
`lba.Globe.SeaDiscDrop 60` (cm under the sea level's water), `lba.Globe.SkyLightGain 1.4`, `lba.Globe.Lumen 1`
(Lumen as in the island levels, to compare), `lba.Globe.UnoccludedSkyLight 0.85`, `lba.Globe.SeaSkyBelow 0.75`
(0: the sea reflects black below the flat horizon), `lba.Globe.Clouds 5 10`
(km), `lba.Globe.SeaErrors` (the sea disc material's compile errors).

Next: the other southern islands from LBA1, first approximations
of their shapes, then if possible 3D conversions placed on the map and bent like the rest. Later: space travel, the
other hemisphere, the other islands' scenes and looks (they borrow the home island's sun, sky, sea color and look),
streaming by distance (every placed island is loaded), the fast travel menu landing on the globe; packaged builds don't
cook the planet asset (nor its sea levels) yet; the small planet's darker horizon sky, which its sea reflects. Bounds
aren't grown by the bend, and the water mesh culls its tiles where they are flat (a far object or tile whose
flat place is off screen while its bent one is on screen would be culled; not seen in the shots). Every generated
material now has a world position offset (zero without a planet): virtual shadow maps may stop caching those meshes
outside globe mode too (not measured; `ShadowCacheInvalidationBehavior` Rigid on static meshes would fix it).

### The ferry (boat)

Each island has one ferry Twinsen can steer (`ALBABoat`, prototype), moored where the island's own copy of the ferry
stood: the island layer recognizes the decor whose body has the ferry's shape (Project Settings > LBA2 > Mounts > Boat,
`DA_Ferry`: as many points and polygons, about the same box) and moors a boat there instead (Citadel's harbour by the
pier, the desert's town harbour, out through the lighthouses' channel). Each scene also had its own ferry (one at
Citadel, three on the desert island, all shown at once here): those scene objects are hidden (Hidden Scene Entities).

Use (E / gamepad X) next to it: Twinsen goes into the cabin and the player drives. W/S or the left stick, or the
triggers: throttle (against the motion it brakes, then backs up at 5 m/s); A/D or the stick: rudder. Mouse / right
stick look around. Use again, slow (under 3 m/s) and next to land, to step ashore: dry ground first, else water up to
60 cm deep, within 4 m of the hull. `ULBABoatMovementComponent`: 30 m/s top speed (0 to 25 m/s in about 4 s, quadratic
drag), the rudder turns 75 degrees/s under way, 40% of it when still, at most 1.1 g sideways (about 23 degrees/s and a
70 m circle at full speed), the keel kills sideways slides at 2.5/s; it floats at the surface (the planet's sea or
Water bodies) and runs aground where its keel (60 cm) meets the seabed. The hull rises when planing, leans into
turns and rocks. In globe mode it crosses from Citadel to the desert island in about 15 s at full speed.
`lba.Boat.Debug 1` shows its speed, `lba.Boat.Spawn` puts one in front of the player. Test: `-LBA2Shots=boat` on
`L_CITABAU?Globe` (boards, steps ashore and back, backs out, full speed, turns, stops, refuses to leave at sea; shots
and log), `-LBA2BoatTo=DESERT` crosses to the desert instead.

### Tests tab, feature test map and gateways

After the planets, the menu's **Tests** tab lists the test levels and sandboxes (**Project Settings > Plugins > LBA2 >
Test Levels**): the feature test map first, apart and in its own color, then the levels under *Test Levels Path*
(`/Game/LBA2/Tests`), then *Sandbox Levels* (the character gallery and Unreal's template levels). Each level lists its
stations under it, with what to try there; picking one goes straight to it. Test levels open with `LBAGameMode`
(*Play Test Levels As Twinsen*), so the template levels have Twinsen and the menu too.

The feature test map, `/Game/LBA2/Tests/L_FeatureTest` (*Feature Level*), is the editor's startup map (the game's
is the planet, see Globe mode). The **Tests** import layer builds it from prototype blocks (run it after Characters and Scenes; it only
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
  Interior bricks are drawn already shaded and the original never shades them: their colors sit
  anywhere on their ramps, and neighbours on two ramps part in the highlights (a terracotta floor
  tile split into brown and cream patches). The importer marks the grid, enclosure, ceiling and
  frames with a custom stencil (`LBA::PredrawnArtStencil`), and those pixels take the palette's
  typical brightness of each row (median over its colors, an extra row of the shade texture)
  instead of their own shade paths, all their colors alike.
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
  `MI_LBA_Water`, `MI_LBA_Water_Ocean`; the plugin's own depth mask feeds the opacity mask; it also bends with globe
  mode's view, no offset without a planet). The terrain renders into the
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
| `Water/M_LBA_Water`, `MI_LBA_Water`, `MI_LBA_Water_Ocean` (copies of the Water plugin's ocean chain, clipped, bent in globe mode) | every water level | yes, unless a newer importer changes them |
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
- [ ] Globe mode: islands laid flat on a 320 m planet's map, the view bent round (prototype: Citadel, the
      desert, the Water plugin's sea, the Hamalayi belt, the Dinofly and the ferry at sea; next: LBA1's southern
      islands, fast travel onto the globe, space)
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
