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
| 3 | Kit: recurring pieces and simple houses as models | 3a props built (kit machinery, lamps, trees, bollards, signs, barrels, crates); shots `Saved/LookDev/lba1_review/phase3_props_before_after.png` (2026-11-07). 3b walls: tiling wall faces (72f2d74, shots `phase3b_walls_before_after.png`); 3b roofs: ramps drawn as slopes (Old Burg; shots `phase3b_roofs_before_after.png`). Proxim City's roofs not solved (see there). 3c simple objects: generated fences, railings, sandbags, buoys + remake oil drums (shots `phase3c_objects_before_after.png`, `phase3c_objects_close.png`). Next: Phase 3 extended (houses as buildings with openings and frames, stairs as steps, roofs per house, palms), see that section |
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
- **One round.** `Tools/lba1_round.ps1` (it was `Saved/LookDev/lba1_review/p3_round.ps1`; see "How to continue"). The 3a final sheet used these stubs-level poses on Principal's harbour:
  `-22597,17495,500,-29.1,0;-21882,17179,628,-29.1,90;-16762,20188,628,-29.1,180;-21634,16135,628,-29.1,270;-15364,17764,980,-29.1,135`.
- **Import.** Run `-run=LBA2Import -Layers=Islands -Islands=CITABAU,CITADEL,DESERT`; it rebuilds the planet's stubs too.
  About 37 `GetFullName` null warnings in the log were there before 3a.
- **Unity build.** A file-level name can clash with a local in another file of the same unity blob (C4459). This happened
  with `PlanetLevel` and `Scale`. Give locals distinct names.

### What was built (2026-11-07): 3b walls

- **Which cells are walls.** A wall brick (`FDistricts::IsWallBrick`) has both sides at least 90 % opaque and no top (at most
  10 %): the middle of a wall, whose top a brick above always hides. A cell is a wall when such a brick is placed by a
  one-column layout (1 x N x 1). That layout's bricks from the bottom up are its **style** (`FTown::WallStyles`: Principal's
  stone 537, 538), and the cell keeps its row (`WallRow`). Survey (towns' 15 scenes): 8,480 wall-brick cells, 7,100 of them in
  column layouts (5,239 in 537/538, 708 in 1, 476 in 1029/1030). The others (2x9x2, 5x4x1 palisades, 3x5x1 arches...) stay bricks.
  Caps (2, 844: they have a top) stay bricks too.
- **Drawing.** `LBAPlanetTowns`' `EmitWalls`: per scene, per side and slice, open faces of the same style and phase (row less
  height) greedily merged into rectangles, UVs by position (a cell along, a row up), into the scene mesh as one section per
  style (`MI_LBA1_<ISLAND>_Wall_<first brick>_<rows>`, 24 x 15-per-row tile, wrapping). A face is open where `CellsOf(Town,
  true)` has no brick beside it, the rule `EmitVisual` uses, and wall cells keep their brick there, so the bricks next to a
  wall still hide their faces against it. Wall bricks are left out of the atlas. Collision unchanged.
- **Art.** Every side shows the sprite's brighter side (+Z, the left half), lit by the engine. Before, the +X and back sides
  showed the dark side, whose ramp-11 pixels `MovePlantShadows` had moved to the plant ramp: near-black green.
  Measured (harbour, median sRGB): shaded sides (24,38,26) -> (49,74,77), lit sides (81,110,111) -> (76,109,110); LBA1's own
  faces have the same ratio (537: 97,115,116 lit, 43,57,55 shaded).
- **Quays** continue a wall's style downward (alternating 537/538) instead of repeating its lowest brick.
- Counts: 17 styles on Principal (5,420 wall triangles), 14 on Proxima (1,448), 11 on Rebellion (548).
- Checks: 157 tests pass; globe 152.2 fps.

### What was built (2026-11-07): 3b roofs, as ramps

- **Ramps are slopes.** Every town cell of a ramp shape (2 to 5: Old Burg's thatch roofs, stairs, grass slopes; 2,039 cells
  in the towns' 15 scenes) is drawn along its collision surface instead of as a cube (`EmitSlopes` in `LBAPlanetTowns`). Its UVs
  are the iso projection of the slope into its brick's sprite (`IsoPixel`: x right-down, z left-down, 24 x 12 pixels a cell,
  15 a step), so it shows exactly what the camera saw. The sprites go whole into their own atlas
  (`T_LBA1_<ISLAND>_Slopes`, grown 2 pixels so edges don't sample transparency): a slope's art spans the sprite's top and
  sides, which the brick atlas packs apart.
- **Ramps facing away** (2 and 3: the iso camera saw them edge on, their sprites are slivers) show the art of the facing
  ramp (4, 5; the mirror shape first) whose mean color is nearest, read along the slope.
- Ramp sides, where open, are a flat fill of the brick's side color (`BrickFill`). Collision unchanged.
- Shots: `Saved/LookDev/lba1_review/phase3b_roofs_before_after.png`. Old Burg's roofs are continuous thatch; stairs are slopes
  with their steps' art.
- Checks: 157 tests pass; globe 155.1 fps.
- **Left:** stepped stone gable edges under the slopes (the wall cells under a roof); corner ramps (6 to 13, ~100 cells) stay cubes.

### Proxim City's roofs (not solved)

Its thatch roofs are not ramps: solid cells whose art draws a slope, one step up per cell, in 3 x 3 and 3 x 4 pieces
(42:186 and 42:188 facing the camera, 42:190 the edge-on back, x58), interleaved with composite slices of slope plus the wall
under the eave (42:187 x6) and gable ends (42:189 x30). Tried and reverted (2026-11-07): fitting each solid piece with a ramp
plane plus its cube-like bricks (intersection over union in the piece's composed sprite, at least 0.7, the plane climbing 3
steps or more). It finds the three pure slopes (0.78-0.89) but:
- per-cell sprites leave holes in the fitted slopes, because a piece's slope art straddles the cells' sprites (the projection
  must read the piece's composed image instead);
- the composite slices (42:187: 0.62, 42:189: 0.34) don't fit, so every other slice of a roof stays a staircase;
- palisades (24:59, 24:60 and Rebellion's copies) and a Principal 1x8x3 piece fit too (0.71-0.78). The real roof pieces have
  blank filler cells (brick 223) around their slope, a third of their cells; those don't.
Better next try: whole roofs per house (a gable over the footprint of a connected run of thatch cells, ridge along its
long side, a tiling thatch texture unprojected from 42:186), or remake-style roof models in the kit table.
The forum notes below ("Insights from the MBN grid conversion thread") back the whole-roof route.

### Insights from the MBN grid conversion thread (read 2026-11-07)

Source: forum.magicball.net/t/easy-isometric-grid-maps-conversion/29551 (2024-12 to 2026-11). A community artist converts
LBA1 grids to 3D in Blender, first by hand on Proxima's harbour (Proxim City), then with a rule-driven one-click importer
tested on Proxima and Citadel. These are lessons for our own pipeline, not a spec: don't copy their models, textures or
look (the user's instruction). Pictures are in the thread; post numbers given as `#n`.

**Roofs (the main reason this was read)**
- LBA1's roofs are not sloped in the grid data, unlike other slopes (#4: "Adeline didn't make the roofs sloped"). Even
  the expert redid them by hand at first, then built a **roof system** in the importer: whole roofs generated per house,
  not per tile (#188, #193, #207). This confirms our failed per-piece slope fit: stop fitting bricks, build per house.
- Their generated roof shape (#188 "structural shell", #199 white placeholders, #202 with house volumes shown):
  - a plain gable over the house's rectangular footprint, **ridge along the long side**;
  - the **gable ends are wall**: the stone wall goes up as a triangle under the roof, the roof material only on the two
    slopes;
  - **eaves overhang** the walls on the long sides and stick out a little past the gable walls;
  - the roof is a **slab with visible thickness** (the thatch edge shows along the eave and the verge), not a zero-thickness
    plane;
  - a separate **ridge cap** runs along the ridge, thicker than the thatch.
  Overhang and thickness look like roughly a third to half a cell in the shots. Judged by eye, not measured.
- The **ridge cap matters to readers**. In LBA1 the ridge is drawn as a thick band of big rounded pieces (people read it
  as coconut husks, #119, #135). Forum readers objected when it was turned into rolled loops, and liked a solid band (logs,
  clay ridge tiles, a thick thatch roll; #118-#123, #139). For us: a ridge piece along the ridge, colored from the ridge
  pixels of the facing roof sprite, is likely worth more than detailing the slopes.
- **Footprint from the house volume.** Their houses are boxes found from the grid (footprint + wall height); the roof sits
  on that box; door and window assets can cut through the roof system when they reach it (#207). One failure they hit: the
  generated slope came down lower than a door opening inside allowed (#201). So check the eave height against the tallest
  opening in the long walls, and raise the eave (or lower the pitch) where it clashes.
- Overhanging parts that the rules can't infer (Citadel's garage) were marked by hand with boxes (#193). Expect a few
  per-house overrides; keep a place for them (our plan overrides, memory note `lba2-grid-plan-tools`).
- Texture: a **trim sheet** for roofs (thatch field, edge, ridge) rather than one unique texture per house (#82, #64).
  Thatch drawn too pale reads as "white" (#105): keep it on the darker straw steps of the chosen ramp.

**Grid conversion lessons that apply to our towns**
- **Grid art alone leaves holes** at the backs of buildings; they cross-check the art with LBA1's collision shapes and
  rebuild missing geometry from collision (#191, #207). We already draw from the bricks' shapes; same conclusion.
- Some collision openings are wrong (a cellar window with a huge hole, door tiles inside a neighbour's house generating a
  second door, #201, #207). Rules need **fill boxes and cut boxes with priorities**: fill the bad hole, then cut the real
  opening; a higher-priority box cuts through lower ones' geometry (#207).
- **Systems for linear things.** Tiles are assigned to a material, an asset, or a system. Systems follow edge paths
  derived from the tiles: curbs and wall capping stones as a profile swept along the path, fences, stairs, roofs; cliffs as
  a path extruded upward (#193, #207). The more tiles get assigned, the more of every new grid fills itself in.
  Our kit table (`lba1_kit.json`) is the per-layout half of this; walls, caps, kerbs and roofs are the system half.
- **Assets on walls** carry a pivot rule: standing on the ground against a wall, hanging on a wall, or free-standing.
  Wall openings are sized in half-bricks along and bricks up, with an offset (sewer windows sit half a brick off, #193).
- **Neighbouring scenes duplicate border rows.** Grids that join repeat the neighbour's edge cells; without slicing them
  off you get two fences in the same place (#206). Check our districts where two scenes meet before blaming the kit.
  Their rule for which scenes join: if you can walk across the screen edge it is directly connected; if you need a vehicle
  it is far (#193, #197). Several regulars disagree for some scenes, so treat it as a default, not a fact.
- **Interior faces.** Their first cleanup cast a ray one cell along each face's normal and deleted faces that hit another
  face, plus loose triangles under slopes, with props masked out first, since props made neighbouring level faces
  disappear (#11, #12). We avoid most of this by deciding faces from cells; the prop-masking pitfall still applies to any
  face test that sees kit pieces.
- **After the rules, simplify.** Merge the repeated grid faces into larger polygons but keep a one-cell band next to other
  materials and walls, so corner dirt can be painted there (#207). Our greedy wall merge is the same idea.
- **Ground.** Grass and dirt marked per tile quadrant and turned into vertex-color splat weights, blended by height, "no
  more tiles" (#199, #207). Anti-repetition: random rotation per cell with a hex mask for grass and dirt; paving can't be
  rotated, so mirror or flip some tiles and add grime (#147). Repetition that is invisible from the iso angle shows from
  new angles (#145): check textures from low, turned cameras too.
- **Tile edges.** LBA1 tile art has no margin, so mips and AA make seams flicker (#9). Keep growing atlas islands (as the
  slopes atlas does, 2 px).
- **Sorting.** Sloped blocks with transparency drew in the wrong order (#7, #9). Keep town pieces opaque or masked.
- Their scale: a cell 0.5 m wide, a step 0.25 m (#202, tool panel). Same 2:1 ratio as our 64 cm / 32 cm.
- What gave the strongest "3D" read in their shots, apart from roofs: capping stones on walls, door and window frames as
  separate pieces, bevelled level edges, stairs as modelled steps (#64, #143, #151).

### Simple objects left as bricks (survey, 2026-11-07)

Ranked by cells (count x size), not ground, not walls, not in the kit:
- palisade 24:59 (5x4x1, x71) and 24:60, Rebellion's 59:59/59:60: brown log walls with jagged tops;
- picket fences 12:143 (1x3x4, x82), 13:144 (4x3x1, x59); iron railings 12:152 (x100), 12:153 (x84);
- boulder rows 17:68 (x59), 12:67, 12:69, 17:74-77 (left to Phase 4's rocks);
- sandbags 11:172 (2x1x1, x165), 11:173 (1x1x2, x146); wooden platform 11:231 (3x1x3, x88); crate stacks 11:177 (2x3x2, x55);
- life buoys 11:104 (x11); tree trunks 12:112; arches and doorways 13:126, 11:239, 59:84 (walls with art, fine as bricks).
Most suit preference 3 (generated primitives: posts and rails for fences and railings, boxes for sandbags and platforms,
a torus for buoys), with colors from their sprites.

### What was built (2026-11-07): 3c, simple objects as generated models

- **A generated kind in the kit table:** `"primitive": "fence" | "railing" | "sack" | "buoy"` in `lba1_kit.json`.
  `LBAPlanetKit::BuildPrimitive` builds it in the piece's frame (centered on its footprint, floor at 0, along its longer side;
  a piece along z is the same shape mirrored across x = y), sized from the example piece's cells. `LBAPlanetTowns` makes one
  mesh per island and model (`Towns/Kit/SM_LBA1_<ISLAND>_Kit_<name>`, the vertex-color master, no collision, bent bounds) and
  instances it like the other kit models.
- **Colors from the bricks:** Main is the LBA1 ramp with the most pixels weighted by step (so the lit cloth of a sack seen
  along its length wins over its shadow ramp); Accent the other ramp with at least 8 % of the pixels whose color is
  furthest from Main (the buoy's teal bands, not the grey shading its white), else Main. Each at the step 80 % of that
  ramp's pixels are at or below, through the town palette (`MakeTownPalette`). Models are lit, so the bright end of the
  ramp is the one that matches the predrawn bricks.
- **Measured** (harbour 3, median sRGB of the green pixels): sandbags as bricks (60,76,51), as models (66,98,59), like the
  planet's lit town grass (78,104,59) on the same olive ramp. A lower step (the median) made them (23,50,32): darker but
  no less green, so the hue is the palette shading's and the 80 % step stays. If the user wants them more olive, the
  choice is another ramp, not another step.
- **Shapes:** pickets about 5 a cell on two rails (12:143, 13:144); iron railings, three bars a cell bent over at the top away
  from the iso camera, on two rails (12:152, 12:153); sandbags as rounded boxes (11:172, 11:173); the life buoy as an upright
  ring with four bands (11:104).
- **The blue oil drums** (11:178, x79) are the remake's `layouts/CA_152_oil_barrel.glb` (teal, like LBA1's), fitted to the
  piece's height. A table `remake` path is under `www/models/lba2` (`lba1/...` reaches the remake's LBA1 models).
- **Left as bricks:** palisades 24:59/24:60 and 59:59 (by their sprites, rocky earth walls, not logs: closer to cliffs, Phase 4),
  the wooden platform 11:231 (flat), the crate 11:177, boulder rows.
- `Tools/dump_level_actors.py` now lists an instanced actor's instances, to aim cameras at single pieces.
- Counts (Principal / Proxima): sandbags 284 / 13, railings 126 / 58, picket fences 79 / 31, buoys 7 / 4, oil drums 68 / 11.
- Checks: LBA2.Enclosure passes (exit code 0); globe 149.9 and 150.6 fps (155.1 after 3b, but other sessions' uncommitted water and sea work
  was in this build too, so the drop isn't isolated).

### How to continue Phase 3 (written 2026-11-07, after 3b)

Two open items, either can go first: **simple objects as primitives**, and **Proxim City's roofs**. Show the user each one's
before/after sheet before the next. (Simple objects were done as 3c, above: what is left of them is listed there.)

**Tools** (all in `Tools/`, run from the plugin folder):
- `lba1_round.ps1 -Name <n> [-Poses towns|"x,y,z,pitch,yaw;..."] [-Labels "a;b"] [-NoBuild] [-NoImport] [-Map ...]`: build,
  Islands import, shots on `L_TWINSUN`, sheet `Saved/LookDev/lba1_review/<n>_sheet.png`. `-Poses towns` is 12 close views
  ringed around the towns (Proxim City 1-4, the harbour 1-4, Port Belooga 1-2, Old Burg 1-2). **Run it from PowerShell.** For
  a before/after: `-NoBuild -NoImport` on the current import first, then the full round; sheet the two folders side by side.
- `lba1_town_survey.py iso <scene>` (how a house is built), `bricks`, `walls`, `ramps`, `section <scene>`, `left` (pieces
  still drawn as bricks, by cells).
- `lba1_slope_fit.py <scene>:<layout>...` or `all`: the ramp-plane fit tried on Proxim City's roofs, with its results.
- `lba1_kit_survey.py` (pieces sheets, `pieces.json`), `lba2_decor_sheet.py` (LBA2 decor bodies to pick from).
- `dump_level_actors.py` (headless; env `LBA2_DUMP_MAP`, `LBA2_DUMP_PREFIX`): actors' positions and bounds, to aim cameras.
- Checks: tests `UnrealEditor-Cmd <uproject> "-ExecCmds=Automation RunTests LBA2.Enclosure; Now; Quit" -unattended -nop4
  -nosplash -nullrhi` (157 pass); globe fps `Tools/shots.ps1 -Map /Game/LBA2/Planets/L_TWINSUN -Poses globe -Extra
  "-LBA2ShotStep=0"`, last line "globe done: N fps" in `Saved/Logs/LBAThingy.log` (155.1 after 3b), on an idle machine.

**Where the code is.** `Import/LBAPlanetDistricts` builds a town's cells (`FDistricts::MakeTown`: bricks, shapes, kit pieces
`Pieces`/`Replaced`, wall styles). `Import/LBAPlanetTowns` draws them per scene: `EmitVisual` (cubes, from `LBABrickMesh`),
`EmitWalls`, `EmitSlopes`, `EmitGround` (terrain), and instances kit models (`Kit_<island>_<model>`). `Import/LBAPlanetKit` reads
`Resources/Models/lba1_kit.json` (a piece by example `scene:layout` to an LBA2 decor body or a remake GLB). A cell drawn by
something else keeps its brick for collision; hide it from `CellsOf(Town, true)` (as `Replaced` does) or skip its faces in
`EmitVisual`'s KeepFace (as walls and ramps do), not both blindly: a brick left in the visual cells still hides its neighbors'
faces against it.

**Simple objects as primitives** (suggested route):
1. Add a generated model kind to the kit: e.g. `"primitive": "fence" | "railing" | "box" | "torus"` in `lba1_kit.json`, a
   field in `LBAPlanetKit::FModel`, and in `LBAPlanetTowns`' instancer build its `UStaticMesh` once per model with
   `FLBAMeshBuilder` (`Towns/SM_LBA1_Kit_<name>`, the color material with vertex colors) instead of finding an asset. The
   placement code (footprint center, `every`, `yaw`, `fit`) already works for any mesh.
2. Shapes from the piece's size (posts at each cell along the long side, two rails; a box the piece's size; a torus 1 cell
   across), colors from its sprites' mean colors (moved to the home palette, as `MakeTownPalette`).
3. Start with the most visible: picket fences (12:143 along z, 13:144 along x), iron railings (12:152, 12:153), sandbags
   (11:172, 11:173), life buoys (11:104). Palisades (24:59, 24:60): maybe a row of log cylinders with jagged tops.
4. Shots: Proxim City and Port Belooga (fences), the harbour (sandbags, buoys, railings).

**Proxim City's roofs** (suggested route, see "Proxim City's roofs" above for what failed):
1. `lba1_town_survey.py iso 42` and `section 42`: the roofs are 1-cell-deep slices along z (42:186/188 facing, 42:190 back,
   42:187 slope over wall, 42:189 gable end), slopes rising along x one step per cell, ridges along z.
2. Per house: the connected cells of those layouts (or of thatch-colored bricks) give a footprint and a ridge height. Emit two
   slope quads and the gable triangles; hide the layouts' cells from the visual cells (collision kept). From the forum notes
   above: gable triangles in the wall's style, the roof as a thick slab with eaves past the walls, a ridge cap along the
   ridge, and the eave kept above the tallest door in the long walls.
3. Texture: the facing slope's art read through the iso projection, from the composed image of 42:186 (not per-cell sprites:
   the art straddles them), tiled along the ridge; the back slope uses the same art mirrored, as `EmitSlopes` does.
4. Shots: `-Poses towns`, Proxim City 1-4.

Also left from 3b: Old Burg's stepped gable edges under its slopes, and corner ramps (shapes 6-13) still drawn as cubes.

### Phase 3 extended: houses as buildings (the user's review, 2026-11-07)

The user's verdict after 3c: Phase 3 is far from done. Goal: **most houses get a decent 3D representation, with proper
door and window frames** (that is what players look at), **all sorts of stairs work**, and recurring pieces still wrong or
left as bricks get replaced (palm trees: LBA2 has palms). Phases 4 and 5 wait.

**The user's decisions (2026-11-07):**
- **Frames are 3D models built from LBA1's tiles**, not LBA2 bodies: LBA1 has a different architecture, and it keeps the
  islands distinct.
- **Principal stays closest to LBA1's architecture.**
- **Proxima is the experimental island:** a blend of LBA1 and Citadel's architecture and style (it is close to Citadel,
  shares some culture, and sits at the same latitude). It looks the strangest today and needs the most reinventing (see the
  rune stones work, memory note `lba2-proxima-rune-stones`), so LBA2/Citadel pieces and freer changes are welcome there.
- Rebellion: not discussed yet.

**What the towns look like now** (`iso_42.png` vs `phase3c_objects_*`): a house is a box of tiled wall faces with its
openings painted flat on them (arched doorways, round windows, signs), a roof as thatch slopes (Old Burg) or stepped bricks
(Proxim City), stairs as textured slopes. From a low camera the doorways read as pictures, not holes.

**Approach: an explicit building model first** (memory note `lba2-enclosure-process-lessons`: the interiors went slowly
because rooms, walls and openings were never explicit objects). Per town scene, derive from the grid:
- `FLBA1House`: footprint (connected wall cells under one roof, or one walled block), base and eave height, wall style,
  roof kind (gable on the long side, flat/terrace, none), ridge height.
- `FLBA1Opening`: wall side, position along the wall, width, height, kind (arched door, square door, window, round window,
  cellar hatch, archway through), found from the layouts that draw them (below) and checked against collision (a door has
  walkable air behind it or is a scene exit).
- `FLBA1Stair`: start cell, direction, rise, run, width, kind (straight, against a wall, corner, down into a cellar).
Draw the model as a top-down plan per scene (houses colored, openings and stairs marked) and check it against the iso
render before making any geometry, as the grid plan tools do (`lba2-grid-plan-tools`). Overrides for what the rules miss.

**Then geometry, in this order** (each its own round with a before/after sheet, regression scenes 42, 13, 11, 12, 59):
1. **Walls with real openings.** Cut the openings through the wall faces (`EmitWalls` already merges faces: split around
   openings), give the wall thickness (half a cell, as LBA2's Citadel houses), a dark recess or the door leaf behind.
2. **Frames as kit models made from LBA1's tiles.** Stone arch frame, square frame with lintel, window frame with sill,
   wooden door leaf: shapes generated per opening, textured and colored from the opening's own bricks (the arch stones,
   the door planks). On Proxima, Citadel-style frames or LBA2 bodies may be blended in (`decor_CITADEL.png`).
3. **Roofs per house.** The "Proxim City's roofs" route above, as part of the house model: gable over the footprint, gable
   walls in stone, slab with eaves, ridge cap, eave kept above the tallest door. Old Burg's ramp roofs move to it too if it
   looks better.
4. **Stairs as steps.** Modeled steps (tread + riser per step, LBA1's 32 cm rise) with side walls/stringers where the
   original draws them, collision kept as a ramp so Twinsen walks them (check with `-LBA2HeroAt` on each kind). Corner
   ramps (shapes 6-13) as proper corners. Kinds seen: straight flights (Proxim City, the harbour), wide steps between
   terraces (scene 42 front), cellar stairs (42, under a hatch), Rebellion's 59:117 and 59:82, 12:244, 12:14, 12:15.
5. **Trees and remaining props.** Palms as LBA2 palms (44:206 is mapped to LBA2's pine today; find every palm piece, scene 42
   has palms too), conical trees checked against LBA2's, benches, flower pots, wall plaques and signs (11:20, 12:14), the
   brazier 11:48, then down the `left` list.

**Pieces that draw openings and stairs** (counts over the towns' 15 scenes, from `pieces.json`):
- arched doorways / gates 3x5x1: 13:126 (x10, Principal + Proxima), 11:239, 59:84; arches 3x3x1: 11:15 (x11), 11:153 (x11);
  big arch 13:155 (3x9x1), 17:25 (3x7x1 portal).
- door/window pillars 1x9x1: 11:148, 11:149, 11:151, 11:152 (x8-9 each), 12:182, 12:183, 44:207, 44:208.
- round window 12:14 (2x4x1); wall plaques 11:19, 11:20.
- stairs: 11:14 (1x3x3, x12), 12:244 (2x4x2), 12:15 (2x8x2), 12:19 (1x8x3), 59:117 (2x4x6), 59:82 (1x3x5); plus the ramp-shape
  cells already drawn as slopes by `EmitSlopes`.
Many doors are also painted inside plain column wall layouts, not separate pieces: the opening finder needs the wall art
too (dark arch-shaped pixels in a wall face), not only these layouts. Survey that first.

### The building model, prototype (2026-11-07)

`Tools/lba1_houses.py` (run from `Tools/`, writes `Saved/LookDev/lba1_houses`): `model <scene>...` draws the model over the
scene's iso render (`model_<scene>.png`; sheets `model_houses_sheet.png`, `model_blocks_sheet.png`). Also `houses`,
`openings`, `explore` (top view, every placed layout boxed) and `layouts <scene> [ids]` (composed layouts sheet).

**What the grids showed** (these settle the open question above: doors are separate pieces, not wall art):
- **Houses are hollow shells.** Only the two walls the camera saw (+x, +z) exist; the roof floats over them; the back walls
  and often the floor under the house were never drawn (Proxim City's houses are hollow 4 rows down into their terrace).
  So a house must be found as a footprint and its back walls made.
- **Roofs:** Proxim City's are solid 3x4x1 slices with the slope painted on (heights are flat), ridge cells empty between
  the two slopes; Old Burg's (13) are small ramp layouts (1x2x1, 1x1x1) whose back slope is slivers on every third cell.
  Some roof slices are buried under a terrace (42 x 19-21 z 0-10).
- **Doorways are holes** in the wall with an arch piece over them (42:195, 42:198: 3x3x1) and jamb pillars (193, 197:
  1x9x1). Closed doors and windows are pieces in the wall (42:191 arched wooden door, 42:102 small arched cellar door, 13:126,
  13:154/155); plaques 42:103/104, 13:19. Some doors sit in a pit down steps (42's top-left house), some up steps (one side
  of a house stands higher than the other: per wall, not per house).
- **Flat-roofed blocks** (the harbour 11, the fortress 12, Rebellion's ruins 59) have the same pieces in terrace walls: no
  houses there, only openings.
- **Palms** are two layouts: a trunk (12:112, 1x7x1) and a crown (12:113 / 42:113, 5x2x5), so neither matched the kit's
  "pine" entry. 12:111 / 42:111 (2x9x2) is the round conifer.

**Rules as built:**
1. Roof cells: layouts of at least 6 cells with 2 thatch bricks, or ramp layouts with one; plus thatch bricks; only those
   with nothing over them. Columns joined across one cell (the ridge), 8-connected.
2. Ridge axis: ramp shapes (2/5 rise along x, 3/4 along z), else the slices' thin side.
3. Neighbouring roofs split where a falling row is followed by a rising one (cells of a shared row go by their own
   direction), or, without ramps, at height valleys.
4. Footprint: the roof's box, trimmed along the ridge to the rows that have both slopes (gaps up to 2 closed: slivers).
5. Eave: the roof pieces' lowest row; floor: the street level just outside the +x and +z walls; per wall, its own base
   (the street outside it) and 3 rows below it for pits; under the street, air is solid unless a hole above continues.
6. Openings in a house wall: air holes (not touching the wall's ends or top) with the pieces around them as their frame;
   other pieces alone are closed doors (reaching the base) or wall pieces.
7. Other walls (blocks): every +x / +z plane with at least 6 exposed wall cells; air and pieces not reachable from the
   plane's border (air with solid in front counts as solid: hollow terraces) are holes; air holes need a frame piece or wall
   on both sides; holes on house walls are left to the house.

**Results:** 42: 4 / 4 houses, all doors, closed doors and plaques right. 13: 12 houses of about 14; misses the house built
into the big terrace (only its front half) and the steep two-part roof at x 5-10 z 15-28. Blocks: the harbour's and
fortress's doorways, round windows, banners found.

**Stairs (added 2026-11-07):** the towns' stairs are ramp cells (shapes 2-5) of four grey stone bricks, one per direction
(1032, 1031, 1295, 546: 741 cells, saturation 0.11-0.19), each cell drawn as two steps (16 cm rise, 32 cm tread). Grass
(573-578), rock (6399, 6408, saturation 0.27) and thatch ramps are slopes and roofs. Rule: a flight = stair-brick ramp
cells of one direction, joined across at the same row and along one row up per cell; it keeps direction, width, run,
bottom and top rows, and per side what stands beside the steps: wall (solid over the step: a cheek), drop (air at the
step's row) or street. 59 flights in 11, 12, 13, 42, 59, among them the short flights down into the pits in front of Old
Burg's and Proxim City's doors. Geometry plan: modeled steps (two per cell), the ramp kept as collision so Twinsen walks
them as today; cheek walls continue the wall beside; drops get a stringer.

**What the indoor enclosure work gives this** (memory notes `lba2-enclosed-interiors`, `lba2-grid-plan-tools`):
- Process: explicit model, plan view, then geometry; one rule per round over a fixed regression set (here 42, 13, 11, 12,
  59); later, the model saved as text with golden tests, like `Tests/Plans` / `LBA2.Enclosure.Plans`.
- Walls are foot + body + top layout segments, and cut walls are cropped layouts that continue with the rest of their own
  layout (`LBAWallSurvey`, the wallpaper profile in `LBAIsoImporter.cpp`): the way to build a house's undrawn back walls and
  gable triangles in its front walls' style, rather than one repeated cell.
- Doorways: jambs, lintels, thresholds; a header from the lintel or a door height; passage underside and flanks; the far
  end closed dark (like the original's exits): the same for a house's doorway recess.
- Frames: pieces cut from the grid as whole instances and drawn separately (`CutFrames`, the Frames mesh): the arch and
  jamb pieces here become the frame models.
- Stairs: the indoor plan's R1 (floors at no level's height climbing one cell at a time between two levels) is the
  general form; the towns only needed the ramp bricks. Indoors, enclosure faces must never close a slope cell (it blocked
  Twinsen's stairs): keep stair cells out of any wall generation here too. The remake has a stairs model
  (`CI_096_097_098_099_stairs`) for the look of steps.
- Tools: player-like poses (`room_poses.py`, `side_poses.py`) and a build-import-shots round (`rooms.ps1`) to copy for town
  close-ups; capture the original first.

**Next in the model:** a piece-kind table (round window, arched door, plaque, banner, cellar door: the kind decides the
frame; today a round window just above a walkway reads as "closed door"); then port to C++
(`LBAPlanetDistricts`, per district rather than per scene) and the geometry steps above.

## Phase 4: trees, rocks and paths

- Scatter LBA2 trees and rocks on grass benches. Measure the density on Citabau rather than guessing.
- Keep them off towns, beaches and paths.
- Add paths between the districts as an earth kind in the mask, routed along benches and ramps so they can be walked.

## Phase 5: rune stones

The stone scenes are straight-edged slabs in the sea. Grow grammar islets around them (a stub grid with pads, as on the
main islands), so the slabs sit inside real land with a coast.

## Checks after each phase

- **Performance:** globe test on `L_TWINSUN`, real time (`-Poses globe -Extra "-LBA2ShotStep=0"`): 152.9 fps before Phase 1, 154.0 after, 155.5 after Phase 2, 154.4 after 3a, 152.2 after 3b walls, 150.6 after 3c (2026-11-07). Another session's CPU-heavy job
  (a Python script at 100 % of a core) brought the same build to 101-119 fps: check the machine is idle before trusting a number.
- **Tests:** `LBA2.Enclosure` 157 tests pass; `-LBA2Shots=belt` holds Twinsen at 3.7 m (2026-11-07).
- **Street level:** `shots.ps1 -Map /Game/LBA2/Planets/L_TWINSUN -Poses hero -Extra "-LBA2HeroAt=6286,56485,0"`
- **Comparison:** the same camera poses as `Saved/LookDev/lba1_review/a` and `b`, side by side.

### Poses

Stub level coordinates ("map") to `L_TWINSUN` world, in cm (fitted 2026-11-07 from the town actors dumped in both levels;
the older formula, with +36832/+39232 after a map offset, was about 45 m off):
```
world.x = 0.6537 map.x - 0.7567 map.y + 33033
world.y = 0.7567 map.x + 0.6537 map.y + 57228
world yaw = map yaw + 49.18
```
Actor positions come from dumping either level with a headless python script (`unreal.EditorLoadingAndSavingUtils.load_map`,
then `get_all_level_actors`, `get_actor_bounds`). Town actors' bound centers are good aim points. Also: shoot
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
- Run `lba1_round.ps1` and `shots.ps1` from PowerShell: Git Bash rewrites `/Game/...` into `C:/Program Files/Git/Game/...`, and the
  game then loads nothing and shoots nothing. Shots of a map are named after it (`L_TWINSUN_Stubs_00.png`).
- Keep vertex jitter under a quarter of a cell. At 0.3 about 0.006 % of cells fold a triangle; `AddTriangleFacing` turns it
  up, so on a cliff it is culled from the side, and the sea plane under the mountain shows through the hole. Fixed in the stubs
  and the belt (0.2) on 2026-11-07. Citadel isn't jittered, so sea seen on its cliffs from afar (the user's report, same day)
  has another cause, not found yet.
