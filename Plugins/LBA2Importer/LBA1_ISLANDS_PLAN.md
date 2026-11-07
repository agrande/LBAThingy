# LBA1 islands: improvement plan

Started 2026-11-06. If asked to "continue the LBA1 islands work": read this file, check the status table, carry on with
the first phase not done, and update the table as you go. Background: the "Stub islands" and "LBA1's towns" parts of
`README.md`, and the memory notes `lba2-stub-islands` and `lba2-hamalayi-belt-plan`.

## Goal

Principal, Proxima and Rebellion should look like they belong on the same planet as Citadel and the desert, while each
keeps a character of its own. Today they are LBA1 bricks sitting on an untextured prototype terrain.

The street plans and the scale stay. The towns gradually become LBA2-style models, with the bricks only as the fallback
for what isn't replaced yet.

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Remove the two duplicate islets | done (1378cba); re-imported, stale assets deleted, shots `Saved/LookDev/lba1_review/islets_gone_sheet.png` (2026-11-07) |
| 1 | Textured terrain, with LBA1 tiles where they fit | done (1378cba), the palette answer for Phase 2 still open; shots `Saved/LookDev/lba1_review/phase1_before_after.png` (2026-11-07) |
| 2 | The color system, and town ground becoming terrain | committed 7546de2; shots `Saved/LookDev/lba1_review/phase2_before_after.png` (2026-11-07) |
| 3 | Kit: recurring pieces and simple houses as models | 3a props built (kit machinery, lamps, trees, bollards, signs, barrels, crates); shots `Saved/LookDev/lba1_review/phase3_props_before_after.png` (2026-11-07). Next: 3b simple houses |
| 4 | Trees, rocks and paths | not started |
| 5 | Rune stones on real islets | not started |

Show the user each phase's shots before starting the next, and check every look change against reference shots with
measured colors (memory note `lba2-compare-with-original`).

## The user's decisions (2026-11-06)

- **Terrain:** use real ground textures, per-pixel ground kinds and rougher cliffs. LBA1's ground tiles may be used as
  terrain textures where that makes sense and the colors match.
- **Colors:** keep diversity between islands. Similar ground kinds, especially natural ones, stay consistent across the
  planet and match LBA2. LBA1's tile textures give each island its unique character.
- **Town ground:** done straight after the color work, or as part of it, since the two overlap.
- **Recurring pieces, in order of preference:**
  1. An equivalent model from LBA2's islands, or from lba2remake's indoor replacement models.
  2. Simple houses (cubes, or assemblies of cubes): cubes with textures extracted from the tiles.
  3. Models that look assembled from a few primitives and don't suffer much from one plain color per primitive:
     generated, within reasonable limits.
  4. Anything else: left for later.

  This is a fast first iteration, not a final one.
- **Empty land:** trees, rocks and paths first. People and interiors much later.
- **Rune stones:** give them real islets.
- **Islets:** remove the small holomap islets next to the Dome of the Slate and next to the desert island's cave islet.
  They are holomap duplicates of the dome's rock and of the desert's hacienda-facing islet.

## What was measured

Tools: `Tools/lba1_kit_survey.py` and `Tools/lba1_recolor_proto.py`, writing to `Saved/LookDev/lba1_kit`. Review shots
are in `Saved/LookDev/lba1_review`.

**Palettes**
- Both games use 16-step ramps, but only 15 of 256 entries match (`palettes.png`).
- The look shader snaps every pixel to the nearest color of the home island's palette (Citabau's, through its 64^3 lookup
  table, `LBANearestPaletteIndex`). So LBA1's colors already land on LBA2's palette, but on whichever ramp happens to be
  nearest. LBA1's teal grass stays teal because Citabau's palette has a teal ramp (`recolor_proto.png`, "today" column).
- Ground tiles mostly sit on one or two LBA1 ramps: grass on ramps 2 and 11, earth on 8, streets and dark ground on 15.
- Citabau's ground texture (`citabau_ground.png`): grass is on ramp 7 (median 136,148,100), rock on ramps 14, 3 and 13
  (144,140,132), paving on 14 and 3.

**Pieces** (`pieces.png`, `pieces_big.png`, `pieces.json`)
- 325 distinct pieces and 17,607 instances across the towns' 15 scenes, grouped by their bricks.
- 7 pieces make 50 % of the instances, 30 make 75 %, 89 make 90 %.
- 168 pieces are flat (one cell high): 11,806 instances, 67 %. Mostly ground, which Phase 2 turns into terrain.
- Only 52 pieces are 4 or more cells tall (511 instances).
- Ground tile families: grass bricks 588-605, earth 550-561, dark ground 964-972, paving 509-513.

**Remake (`refs/lba2remake/www`)**
- `models/lba2/layouts` has LBA2's indoor models, with descriptive file names (searchable), indexed by
  `metadata/lba2/layouts.json`.
- `models/lba1/layouts` has only 3: barrel, box, cloth bench. They replace layouts in `metadata/lba1/layouts.json`, keyed
  by LBA1 scene (the barrel and box in most scenes, including 11 and 12). Scene 0 also has 14 "mirror" flags marking
  wall-like layouts.
- `models/lba1/iso_scenes/*.glb`: of our scenes only 11 has content (100,960 triangles); 12 is empty. By its node names it
  looks like a bake of the scene rather than hand modeling. Not opened yet: check before relying on it.

## Phase 0: remove the islets

`LBAPlanetStubs::Build` names a found island from a table, else `Islet_N`. Skip the unnamed ones (the only two are these),
then re-import and delete the stale `SM_LBA_Stub_TWINSUN_Islet_*` assets. Check with shots next to the dome and the
desert's cave islet.

## Phase 1: textured terrain

Today: `M_LBA_Color` vertex colors on a 1 m grid, so kinds change per vertex (the sawtooth edges and blocky patches).

1. **Tile extraction.** Unproject LBA1's top diamonds and side faces into square textures, packed per family. The sampling
   in `lba1_recolor_proto.py` is crude: its dotted seams are a prototype artifact, the real extraction must not have them.
2. **New master material** for the stubs (bent like the others, `LBAMaterials::AddPlanetBend`):
   - A kind mask per island, sampled per pixel with a noise threshold: hard edges with an irregular outline, no alpha
     blending, so pixels stay close to palette colors.
   - World-projected tiling textures per kind, with cliffs projected sideways.
   - Anti-repetition if it shows (the sea's hex tiling in `LBAWaterCartoon.ush` is a precedent).
3. **Texture sources.** Rock from Citabau's ground texture (it has large rock tiles). Grass and earth from LBA1's tiles
   recolored, or Citabau's where that looks better. Sand from the desert's ground. Citabau has almost no sand: 303 pixels.
4. **Shape.**
   - Jitter the mesh vertices, as the belt does (+-0.2 cell, see Pitfalls), so cliffs stop following the grid.
   - Break up cliffs: fewer perfect contour rings and flat-topped mesas, rougher risers.
   - Stop grass dripping down risers: the kind should be decided by slope at pixel level, not interpolated between vertices.
5. **Reference shot:** Citadel at `b/L_TWINSUN_14.png`.

The belt has the same contour look, but it is out of scope unless the master material carries over cheaply.

### What was built (2026-11-07), and where it left the plan above

- `Import/LBAPlanetGround` makes the textures and `MI_LBA_StubGround`. `Shaders/Private/LBAStubGround.ush` picks a kind per pixel.
- **Kind shares are vertex colors, not a mask texture:** same 1 m resolution, no extra asset. Phase 2's towns need finer,
  tile-exact kinds, so they will need a real mask or their own mesh section.
- **Cliffs are projected from above, not sideways:** that is what makes Citadel's vertical streaks (`sunny_47`, `b/L_TWINSUN_14`).
- **Grass is Citabau's tile, not LBA1's:** LBA1's on the olive ramp was fine-grained and flat next to LBA2's leafy tile
  (`t3` against `t4`). Keep LBA1's for lawns inside towns in Phase 2 if it suits them. Earth is LBA1's on LBA2's ramp.
- **LBA1 tiles fade:** each kind's two 3 x 3 layouts go dark to light along z and back, which shows as a checker from afar. Only
  the evenly lit bricks where they meet are used (earth 553-558; grass would be 594-599). Phase 2 must deal with this for town ground.
- **Repeats:** mirroring tiles draws symmetric motifs, so the grass tile is shifted at random per repeat and every kind's grid wanders.
- Pads and the shelf under the sea stay plain vertex colors, so the pads' rectangles and sawtooth rims remain until Phase 2.

Known limits: sand's tile can still be faintly seen from high up. Earth patches keep stair-stepped outlines a few meters wide (they follow the holomap painting's kinds; cause not looked into). Big meadows look empty until Phase 4.

## Phase 2: the color system and town ground

### The palette rule (the user's OK: 2026-11-07, "LBA2 ramp")

A tile is a **pattern** from LBA1 plus a **ramp** from LBA2.

At import, each pixel's brightness is placed on a chosen ramp of Citabau's palette, as an exact palette color, so the
look shader treats it exactly like LBA2's own art. Which ramp is decided per kind of tile, by a table:

| Kind | Ramp | Per island? |
|---|---|---|
| Grass | 7, step 10 (LBA2's olive) | No |
| Earth | 1, step 11 | No |
| Rock | 14 | No |
| Sand | 6 | No |
| Paving, walls, wood, roofs | Nearest LBA2 ramp to LBA1's own hue | Yes: the place for island color |

Each island's character then comes from three things:
1. Its LBA1 tile patterns and the proportions of each kind.
2. Its man-made ramps: roofs, wood, wall stone.
3. At most a small brightness step on natural ground. Citabau's palette also has a greener ramp (8), available as an
   exception for one island if it looks right.

Lessons from the prototype (`recolor_proto.png`):
- Grass needs its contrast lowered (try 0.5 to 0.7). The olive ramp's steps are wider, so the pattern gets noisy.
- Earth is nearly right already.
- Moving a kind to the next ramp along is usually garbage (pink and green, or red), so variety must be hand-picked per
  kind, never a numeric offset.
- These numbers are from sRGB swatches. Confirm them in engine against the lba2cc reference shots.

Recolor per brick family rather than per palette entry, because ramps are shared: ramp 11 appears in both grass and walls.

### Town ground becomes terrain

This also fixes most of what looks broken at street level.

1. Classify each column's top brick: grass, earth, dark ground, paving, water, or other.
2. Flat ground kinds stop being bricks. They go into the terrain's kind mask, with the pad's height exactly at street level.
3. Paving stays in the mask too, as a kind with its LBA1 tile, unless a mesh proves necessary.
4. Only raised things remain as bricks or kit pieces.

The result: no rectangular town outlines (a town's edge was mostly ground columns), no pad showing through gaps, and no
64 cm noise. Then re-shape the land at town edges with the grammar instead of the 6 m ease, and bed in the Water Tower.

Keep the bricks' collision until each ground kind is proven walkable.

### What was built (2026-11-07), and where it left the plan above

- **Colors:** `LBAPlanetGround::MakeTownPalette` moves LBA1's palette to Citabau's ramp for ramp, each color to the step as bright.
  Plants 2 -> ramp 7, earth 8 -> ramp 1; the others to the ramp nearest at equal brightness: 3->2, 4->12, 5->4, 6->5, 7->8, 9->6,
  10->10, 11->11, 12->7, 13->14, 14->4, 15->3. No table per island was needed.
- **The table's steps changed for bricks:** brick art is drawn already shaded, while terrain is lit (a tile renders at about 0.55
  of its value). So the plan's steps (grass 10, earth 11) are right for lit terrain textures only; on bricks the middle goes to the
  original's on-screen values, grass step 8 and earth step 7.
- **Per brick family became per pixel:** ramp 11 is plants' shadows and also stone. Ramp 11 pixels within 3 pixels of a plant's move
  with the plant, which also handles moss on stone without any family table.
- **Ground went on a mesh, not a mask:** a mask laid in the map's frame needs exact street heights on a 1 m grid, and districts are
  turned. Quads on LBA1's own cells, in the stub's mesh with the same material and pivot, keep exact heights, and the ground around
  them comes up flush, so the seam doesn't show.
- **Classification is automatic:** flat tile (top diamond at least 90 % covered, at most 60 pixels outside) by its ramps, grass/earth
  shares from the mix, so transition tiles (910-939) give fractional kinds. Only dark ground is a range (964-972).
- **Paving stayed bricks**, recolored: its courses must follow the streets. The kerbs (840-843) and other paving variants too.
- **A new kind:** dark ground, carried in the vertices' UV x.
- **Measured** (same light, median sRGB): town earth (69,47,8) against Citabau's (71,53,17); town grass (78,104,59) against (82,111,67).
- **Pitfall:** a master material's graph only rebuilds when `Version` in `LBAMaterialLibrary.cpp` goes up. Forgetting it leaves the old
  Custom node calling the new `.ush`, the compile fails and the default material is used, unbent: the land floats, flat grey.

Known limits: earth patches inside towns keep some of LBA1's crenellated edges. Rune stones' grass slabs stand out until Phase 5. Terraced
plant bricks and palisades still look blocky until Phase 3. Earth renders darker than the original's (88,60,32) on the whole planet, on
Citabau too: not a stub island matter. The bricks' collision is still what Twinsen walks on in towns.

## Phase 3: the kit

1. **Table:** `Tools/lba1_kit.json`, from piece key (size and bricks) to an action:
   `terrain:<kind>`, `lba2:<body>`, `remake:<file>`, `cube:<texture>`, `primitive:<spec>` or `brick`.
   Work down `pieces_big.png` by count x size until the visible return drops.
2. **Candidates seen so far** (check each):
   - Lamp (x27): Citadel's lamppost (`CITADEL.OBL` body 26; body indices differ per island).
   - Barrels (x79), boxes: remake's LBA1 layouts.
   - Pine (x116), palm: LBA2 tree bodies.
   - Pyramid bollard (x91): the harbour's railing bollard (body 50).
   - Boulder heaps (x59, x40, x35...): LBA2 rock bodies.
   - Picket fence (x59), iron railing (x100, x84): primitives or remake models.
   - Life buoy, sign boards, hay bales: primitives.
   - Stone wall blocks (x2742, x708, x259): merged into cubes.
   - Roof slopes (x37, x33, x32...): sloped primitives from the bricks' shapes.
3. **Simple houses:** connected wall bricks become greedy-merged boxes, with tiling wall textures on all four sides and
   sloped roofs. This also fixes the backs of buildings, which the iso camera never showed.
4. **The brown lattice** (Proxim City, Rebellion, rune stones): probably the x71 palisade or cliff piece, 5x4x1, used in
   scenes 24, 44 and 47. Its sprite covers only part of each cell, so the extrusion leaves gaps. Unconfirmed: check first.
   If so, treat it as `terrain:cliff` or a cube.
5. **Instancing:** use instanced meshes per kit model. Keep the negative bounds extension the bent culling needs.
6. **Collision:** keep it from the bricks' shapes in this iteration, and check Twinsen against the visuals.

### What was built (2026-11-07): 3a, the props

- `Import/LBAPlanetKit` reads the table, `Resources/Models/lba1_kit.json` (not `Tools/`: the importer reads it), and the remake's
  own LBA1 replacements (`www/metadata/lba1/layouts.json`: barrel, box, cloth bench), keyed by size + bricks.
  `FDistricts::MakeTown` finds whole layouts with a model (`FTown::Pieces`, `Replaced`); `LBAPlanetTowns` skips their bricks when
  drawing (collision kept), lays terrain under them, and instances the models per island (`Kit_<island>_<model>`).
- Table entries: Citadel's lamppost (26), tree (1) for both pines, the harbour's bollard rail (50, x and z), sign board (5).
  304 instances. Orientation: LBA2 bodies at yaw 0 in a district's frame face as on their island; the rail runs along x.
- `Tools/lba2_decor_sheet.py` draws an island's decor bodies (`decor_<ISLAND>.png`); pieces_big.png labels now give each piece's
  example `scene:layout`.
- **Boulders left as bricks.** The boulder heaps (17:68, 17:77, 12:67...) are LBA1's rocky shores and terrace edges, many half
  buried. No LBA2 decor body fits: rocks are big textured terrain chunks (Citadel 2 sits 2.25 m over its pivot, 3 is red), the low
  dark ones (Citadel 8-10) are slabs. Next try: a generated rock (preference 3), or leave them to Phase 4's rocks.
- `"fit": "height"` and `"every"` exist for that (scaled to the piece's height and set on its floor, copies along it) but no entry
  uses them now.
- Checks: 157 tests pass; globe 154.4 fps (155.5 before).
- Not done in 3a: iron railings (12:152, 12:153 x184: remake iron fence models?), picket fences, life buoys, the brown palisade
  (24:59), roof slopes, walls. The sign board model (1 m) is smaller than LBA1's (1.3 m).

### Notes for 3b, the simple houses (written 2026-11-07, before starting)

- **What to reuse.** 3a already lets a cell's look change while its collision stays bricks. `FTown::Replaced` marks cells
  that aren't drawn. `CellsOf(Town, bVisual)` returns 0 for them only when drawing; collision passes false. `EmitGround`
  lays terrain under a replaced cell, and the atlas leaves replaced bricks out. Houses can mark their wall and roof cells
  the same way and add their own geometry. The difference: a house isn't one layout, it's a span of wall bricks across
  many layouts. So the merge has to work on cells by brick family, after `MakeTown` has filled the bricks, not by layout
  key.
- **Order of work.** First survey which bricks are walls and which are roof slopes: plan item 2 lists the big counts
  (x2742, x708, x259; roof slopes x37, x33, x32). `Tools/lba1_kit_survey.py`'s pieces sheet labels each piece
  `scene:layout`. Then build the wall boxes and show them. Do the roofs after that.
- **Frame.** Cell x→X and z→Y, y up. `CellCm` = 64 cm, `StepCm` = 32 cm, then `District.FrameToMap`. This is the same
  convention as the kit's placement code in `LBAPlanetTowns.cpp`.
- **One round.** `Saved/LookDev/lba1_review/p3_round.ps1 -Name <n> -Poses "<poses>" -Labels "a;b;..."` builds, imports,
  shoots and makes a sheet. It isn't versioned. It shoots `L_TWINSUN`; switch `-Map` to `/Game/LBA2/Planets/L_TWINSUN_Stubs`
  to aim with map coordinates (see Poses). The 3a final sheet used these stubs-level poses on Principal's harbour:
  `-22597,17495,500,-29.1,0;-21882,17179,628,-29.1,90;-16762,20188,628,-29.1,180;-21634,16135,628,-29.1,270;-15364,17764,980,-29.1,135`.
- **Import.** Run `-run=LBA2Import -Layers=Islands -Islands=CITABAU,CITADEL,DESERT`; it rebuilds the planet's stubs too.
  About 37 `GetFullName` null warnings in the log were there before 3a.
- **Unity build.** A file-level name can clash with a local in another file of the same unity blob (C4459). This happened
  with `PlanetLevel` and `Scale`. Give locals distinct names.

## Phase 4: trees, rocks and paths

- Scatter LBA2 trees and rocks on grass benches. Measure the density on Citabau rather than guessing.
- Keep them off towns, beaches and paths.
- Add paths between the districts as an earth kind in the mask, routed along benches and ramps so they can be walked.

## Phase 5: rune stones

The stone scenes are straight-edged slabs in the sea. Grow grammar islets around them (a stub grid with pads, as on the
main islands), so the slabs sit inside real land with a coast.

## Checks after each phase

- **Performance:** globe test on `L_TWINSUN`, real time (`-Poses globe -Extra "-LBA2ShotStep=0"`): 152.9 fps before Phase 1, 154.0 after, 155.5 after Phase 2, 154.4 after 3a (2026-11-07).
- **Tests:** `LBA2.Enclosure` 157 tests pass; `-LBA2Shots=belt` holds Twinsen at 3.7 m (2026-11-07).
- **Street level:** `shots.ps1 -Map /Game/LBA2/Planets/L_TWINSUN -Poses hero -Extra "-LBA2HeroAt=6286,56485,0"`
- **Comparison:** the same camera poses as `Saved/LookDev/lba1_review/a` and `b`, side by side.

### Poses

Stub level coordinates ("map") to `L_TWINSUN` world, in cm:
```
a = map.x + 15874;  b = map.y + 12773
world.x = 0.6537 a - 0.7567 b + 36832
world.y = 0.7567 a + 0.6537 b + 39232
world yaw = map yaw + 49.18
```
Actor positions come from dumping `L_TWINSUN_Stubs` with a headless python script. On 2026-11-07 this formula didn't land on
kit instances' dumped positions (cameras aimed with it saw sea or other streets): use it with care. What worked: shoot
`-Map /Game/LBA2/Planets/L_TWINSUN_Stubs` itself, where poses are map coordinates as dumped (unbent and unlit, enough to judge
shapes). Useful world poses, `x,y,z,pitch,yaw`:
- Harbour from the south-east: `14529,54368,2200,-16,139.2`
- Harbour from the west: `4208,50069,2200,-14,49.2`
- Principal from above: `7644,66135,24000,-89,139.2`
- Hills behind the harbour: `9938,64349,1500,-12,139.2`
- Water Tower: `16439,66521,2400,-12,139.2`; nearer `9815,65514,2600,-12,139.2`, from above `7545,67474,5000,-89,139.2`
- Rebellion harbour: from above `63186,72729,7000,-89,139.2`, low `65456,70769,1500,-12,139.2`
- Proxim City from the south: `62128,30690,1800,-14,139.2`
- Citadel, for reference: `26000,26500,2600,-11,50`

### Pitfalls

- Up on a top-down shot is map +y and right is map -x (Unreal is left-handed).
- A mesh's bounds center can be in water: scene 11's is in the harbour basin. Twinsen dropped there ended up T-posing
  under the surface (not investigated).
- A camera inside a hill shows only slivers of back faces. That is the pose's fault, not a bug.
- The whitish sea at low angles happens around Citadel too; it isn't an LBA1 islands problem.
- Other sessions share this checkout: commit by path, and check peers before building or importing.
- Keep vertex jitter under a quarter of a cell. At 0.3 about 0.006 % of cells fold a triangle; `AddTriangleFacing` turns it
  up, so on a cliff it is culled from the side, and the sea plane under the mountain shows through the hole. Fixed in the stubs
  and the belt (0.2) on 2026-11-07. Citadel isn't jittered, so sea seen on its cliffs from afar (the user's report, same day)
  has another cause, not found yet.
