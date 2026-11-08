# Drivable cars: plan

Started 2026-11-08. Phases 0-2 built the same day (see "What was built"). If asked to "continue the cars work": read this file, check the status
table, carry on with the first phase not done, and update the table as you go. Background: the boat (`LBABoat.h/.cpp`,
memory note `lba2-boat`) is the pattern to follow for boarding, leaving, input and camera.

## Goal

Make LBA2's cars drivable by Twinsen with Unreal's own vehicle physics (Chaos Vehicles), wherever the world has one,
with the original's look (its bodies and wheel animation) and a feel close to the original's buggy, only quicker.

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Spike: Chaos Vehicles enabled, a bare car driving on the desert's ground | done (2026-11-08) |
| 1 | The buggy: board, drive, leave, camera, wheels turning, settings, tests | built 2026-11-08, awaiting the user's feel test; not committed |
| 2 | Cars around the world: Citadel's parked buggy, the desert's, Proxima's | built 2026-11-08 (plus Citadel's scooters), reimported; not committed |
| 3 | Feel: engine sound, dust, self-righting, water, crashes | self-righting and water built (untested); sound, dust, crashes not started |
| 4 | Optional: the original's extras (shooting from the car) | not started |

## What LBA2 has

LBA2 has **one drivable car, the buggy** (`SOURCES/BUGGY.CPP` in `refs/lba2-classic-community`). Its other wheeled things
aren't cars: the flying taxis (Otringal, Celebration, Knartas) fly on scripted paths, the mine carts and wagons
(Temple of Bu, the Wannies' mine) run on rails, and the tank (Otringal's rune stone, the Platform) is a scripted enemy.
Otringal's broken car (entity 208, scene 89) is decor.

**Where the buggy's copies are**

| Where | What | Now in Unreal |
|---|---|---|
| Desert scenes 55-72, one each | entity 152 `DA_Car` (body 0 = BODY.HQR 219, the empty buggy) | 18 static scene copies, all visible |
| Citadel decor 85, Citabau decor 114 (one spot, two weathers) | island decor, same shape as BODY 219 | static decor mesh |
| Proxima, the Marked Stone's road end | Citadel decor 85's mesh, parked by `LBAPlanetSites` | static mesh (uncommitted work of the Phase 5 session) |
| Twinsen driving | entity 12 `DA_TwinsenCar` (already in `HiddenSceneEntities`) | hidden |

The 18 desert copies are placeholders for **one** buggy: the original shows it only in the scene where Twinsen left it
(`ListBuggy[0]`, its scene and position). Their life scripts:

- It first appears at the **Racetrack** (scene 57, point 42, facing 2048) once game var 168 is 1 (`INIT_BUGGY 2` forces
  that spot). After game var 74 reaches 3 it is gone for good (`SUICIDE`).
- Boarding: Use within 1500 units (1.9 m) while facing it within 768 (67 degrees), or landing on it. Use again leaves.
  Twinsen's behaviour becomes `C_BUGGY` and his body becomes the buggy with him in it.

**The original's numbers** (units converted at WorldScale 0.125, 512 units = 64 cm):

| | Original | In meters |
|---|---|---|
| Top speed | 3800 units/s | 4.75 m/s (17 km/h); Twinsen's modern run is 3.7 m/s |
| Reverse | 2000 units/s | 2.5 m/s |
| Acceleration / brake / coasting | 4 / 12 / 7 units/s per ms | 5 / 15 / 8.75 m/s^2; top speed in about 1 s |
| Turning | 1024 angle units/s at any speed above 0 | 90 deg/s, a 3 m radius at top speed; none when stopped |
| Front wheels drawn turned | 300 units | 26 deg |
| Wheels | x ±400, z +400 front, -350 rear | ±50 cm apart sideways, 94 cm wheelbase |
| Wheel diameters | 260 front, 360 rear | 32.5 cm, 45 cm |
| Engine sound | `SAMPLE_BUGGY`, pitch 4096 = 1x | pitch climbs with speed through 3 gears, one every 1.2 s of throttle |

Its animation is code, not clips (`DoAnimBuggy`): group 1 = chassis pitch and roll from the ground under the wheels,
groups 3 and 6 = front wheels steering, 4, 7, 9, 11 = wheels rolling, 12 = steering wheel, 14 = Twinsen's head turning
into the bend. Forbidden ground makes it slide (`InitSkating`). Throw while driving opens the hood and shoots a magic ball
(`GEN_BODY_TUNIQUE_TIR`).

Bodies with Twinsen: BODY.HQR 14 and 15 (16 groups, like 219), 22 and 23 (20 groups: probably the open hood for
shooting). Which is which (tunic, wizard tunic, shooting) is still to check in `DA_TwinsenCar`. 227 (18 groups) is
probably the broken car.

The buggy is small: about 1.7 m wide and 1.5 m long, 0.9 m high without Twinsen.

## Design

### Physics: Chaos Vehicles, with a box chassis

- Enable `ChaosVehiclesPlugin` in `LBA2Importer.uplugin` and add the `ChaosVehicles` module to `LBA2Runtime.Build.cs`.
  In 5.8 its `.uplugin` still says experimental, though it is the engine's standard vehicle system.
  `ChaosModularVehicle` (also in the engine) is newer and more experimental: not used.
- `ALBACar : APawn, ILBAInteractable`, like `ALBABoat`. Not `AWheeledVehiclePawn`, which forces a skeletal mesh with a
  physics asset as the root.
  - Root: a `UBoxComponent` chassis that simulates physics (mass about 300 kg, center of mass lowered), sized from the
    body's bounds above the wheels.
  - `UChaosWheeledVehicleMovementComponent` with 4 wheels without bones (`BoneName` None, placed by `AdditionalOffset`),
    front wheels steering, rear-wheel drive.
  - The LBA body as a child `USkeletalMeshComponent`, with no collision. It can switch between the empty buggy and the
    buggy with Twinsen without touching the physics.
  - Checked in the engine source: the movement component takes its body from any `UpdatedPrimitive`, and wheels with no
    bone use their offset (`ChaosVehicleMovementComponent.cpp` `GetBodyInstance`,
    `ChaosWheeledVehicleMovementComponent.cpp` `GetWheelRestingPosition`). The spike confirms it works in practice.
- **Wheel layout from the data, automatically.** The wheels' positions and radii come from the body's wheel groups
  (their pivots and their points' extent), checked against `BUGGY.CPP`'s numbers. The movement component's runtime
  setters (`SetWheelRadius` and the like) apply them, so one pair of native wheel classes (`ULBACarFrontWheel`,
  `ULBACarRearWheel`) serves any car.
- **Tuned in m/s, not in newton-meters.** `ULBACarSettings` (Project Settings > Game > LBA2 Car, `DefaultGame.ini`), like
  the Dinofly's: top speed, reverse speed, acceleration, braking, coasting, steering angle, how steering eases off with
  speed, ground clearance, suspension, grip, assists. The engine torque, gear ratios, brake torque and drag that Chaos
  needs are computed from those, and a read-only Derived summary shows top speed, 0 to top speed time, stopping
  distance, turning radius at speed, and the sideways g that would roll it.
  - S brakes, then reverses (`bReverseAsBrake`), as in the original.
  - 3 automatic gears, so the engine sound can shift like the original's.
- **Arcade assists**, from Chaos's own options: steering that eases off with speed, the stabilizing and torque controls,
  a low center of mass, and self-righting (a slow roll back upright after about 2 s on its side or roof).

### Driving

- **Boarding:** Use (E / gamepad X) within about 2 m, as on the boat and in the original. `ALBACharacter::StartRiding`
  attaches Twinsen and hides him, since the car's body includes him. The car switches to the body with Twinsen in the
  right tunic.
- **Leaving:** Use again while slow (under about 1 m/s). Twinsen gets out at the first free spot: the driver's side, then
  the other side, then behind (the original's `PosObjetAroundAnother`). The boat's `FindLandingSpot` gets generalized.
- **Controls** (an input mapping context made in C++, like the boat's):

  | | Keyboard | Gamepad |
  |---|---|---|
  | Accelerate | W | RT |
  | Brake, then reverse | S | LT |
  | Steer | A / D | left stick |
  | Handbrake | Space | A |
  | Leave | E | X |
  | Look | mouse | right stick (inverted vertically, like the others) |

  The same controls in Authentic and Modern modes: the original's buggy already steers like a car. The controls signs
  (`LBAControlsSign`) show these while driving.
- **Camera:** a chase camera on a spring arm with lag, following the car's heading but not its pitch and roll. Free look
  eases back behind after 1.5 s, as on the boat and the Dinofly. The field of view grows a little with speed.
- **Wheels and Twinsen move as in the original**, through `ULBAAnimInstance`'s procedural pose: wheel groups roll by
  the wheels' own rotation (`UChaosVehicleWheel::GetRotationAngle`), the front wheels and steering wheel follow the steer
  angle, Twinsen's head turns into bends, and the wheels follow their suspension (`FLBAProceduralPose` needs bone
  offsets as well as rotations for that). The chassis group stays at rest, since the whole car now tilts on its own.
  Group-to-bone mapping (`bone_NN`) to be checked on the imported mesh.

### Where the cars come from

- **The desert:** `DA_Car` joins `HiddenSceneEntities`, so its 18 scene copies are hidden like the ferry's. One `ALBACar`
  is parked at the Racetrack spot (scene 57, point 42), spawned by the scene layer in folder `Cars`.
- **Citadel and Citabau:** the island layer turns decor with the buggy's shape into a car, as `FindBoatBodies` does for
  the ferry. That becomes one `FindVehicleBodies(entity)` used for both, with `ULBA2Settings::Car` (`DA_Car`) next to
  `Boat`.
- **Proxima:** `LBAPlanetSites` spawns an `ALBACar` instead of the static mesh at the Marked Stone's road end. Do this after
  the Phase 5 session's changes to `LBAPlanetSites` are committed.
- **Test levels:** a car pad on the feature test map with a ramp, a slope and a slalom.

### The globe and the sea

- The planet's world is flat and only the view is bent, so physics needs nothing special (as with the boat).
- Check that an island's sub-level stays loaded while a car drives on it in `L_TWINSUN`.
- Chaos has no buoyancy. Throttle fades out between about 20 and 40 cm of water (`FindWaterSurface`), so the car can
  splash through shallows but not drive into the sea. If it ends up deeper anyway (a jump off a cliff), Twinsen is put
  out at the nearest dry spot and the car goes back to where he boarded it.

## Phases

**0. Spike (small).**
- Plugin and module on, a bare `ALBACar` (box, 4 offset wheels, the `DA_Car` mesh) on `L_DESERT`.
- Done when it drives on the island ground and into the town without catching, stays upright through hard turns at
  12 m/s, and stays stable in the fixed-step test runs.
- Chaos at the snapshot runs' 1/15 s frame step needs physics substepping, so the car sequence may need to run in real
  time instead (`-LBA2ShotStep=0`).
- If the box root misbehaves: fall back to a skeletal mesh root with a generated physics asset.

**1. The buggy.**
- Boarding and leaving, controls, camera, wheel and head animation, `ULBACarSettings` with its Derived summary.
- The desert's car at the Racetrack, its scene copies hidden.
- Tests (below).

**2. Cars around the world.** Citadel and Citabau's decor buggy, Proxima's, the test map pad.

**3. Feel.** Done in this order, each one shown to the user to judge in play:
- Engine sound: the original's `SAMPLE_BUGGY`, pitch from the Chaos engine's RPM across its 3 gears, matching the
  original's pitch range (0.73x to 2.7x).
- Dust from the wheels on sand and earth, skid marks later.
- Water, as above, and self-righting.
- Crashes that hurt Twinsen (`ULBAHitPointsComponent`, as on the Dinofly), only if the user wants them.

**4. Optional.** Shooting the magic ball from the open hood (the 20-group bodies), sliding on forbidden ground.

## Checks

- Automation tests `LBA2.Car` on a generated flat test world with a ramp and a 10% slope:
  - top speed and reverse speed within 5% of the settings, and the 0 to top speed time;
  - stopping distance;
  - turning radius at full lock;
  - no rollover in a full-lock turn at top speed;
  - parked on the slope, the car doesn't creep;
  - switching body (empty to driven) leaves the velocity unchanged.
- Snapshot run `-LBA2Shots=car` on `L_DESERT`: board at the Racetrack, full throttle 4 s, slalom, brake, reverse, leave.
  It logs speed, heading and wheel contacts and takes shots (`Saved/LookDev/car`).
- Feel is judged by the user in play: stills don't show it.
- The usual: `LBA2.Enclosure`, globe frame rate (one car parked per island should cost nothing while asleep).

## Decisions for the user

1. **Top speed.** The original's 4.75 m/s is barely faster than Twinsen running (3.7 m/s). Recommended: 12 m/s, between
   the Dinofly's cruise (8 m/s) and the boat (30 m/s), with acceleration scaled to keep the original's snappy feel
   (top speed in about 2 s).
2. **How many cars.** Recommended: one per place that has one (the desert, Citadel, Proxima). The alternative is every
   desert scene copy drivable: 18 buggies on one island, where the original only ever shows one.
3. **Feel.** Recommended: arcade-leaning (the assists above on). The alternative is a plainer simulation.
4. **Crashes.** Recommended: harmless at first. The alternative is that they hurt like the Dinofly's.
5. **Story gating.** The original only lets Twinsen have the buggy once he has won it and takes it away later. Recommended:
   ignore that (no scripts yet), the car is always there.

## Risks

- A 94 cm wheelbase at 12 m/s can be twitchy in yaw. Steering that eases off with speed and Chaos's stabilizing controls
  are the first answers; a slightly wider track or longer wheelbase for the physics only (the mesh unchanged) is the
  fallback.
- Small wheels (32.5 cm) on island ground with steps and loose decor. Sweep wheels (shape traces) instead of raycasts,
  and enough ground clearance.
- The Proxima part touches `LBAPlanetSites`, which another session is editing: coordinate before changing it.

## The user's decisions (2026-11-08)

- "Go ahead" on the recommendations, then "that seems too fast" at 12 m/s: the buggy tops out at **8 m/s** (3 s to top
  speed), the scooter at 6 m/s. Settings: Project Settings > Game > LBA2 Cars.
- Citadel's Rabibunny scooter too (or at least out of the sky): done, drivable.

## What was built (2026-11-08)

- `LBA2Runtime/LBACar.{h,cpp}`: `ALBACar` (the buggy) and `ALBAScooter`; `ULBACarMovementComponent`
  (Chaos's wheeled vehicle); wheel classes per kind; `FLBACarGeometry` (wheel places and sizes from the bodies).
  `LBACarSettings.{h,cpp}`: `ULBACarSettings` (Buggy, Scooter, riding) with a Derived summary, `FLBACarDrive` (the
  Chaos engine, 3 gears and brakes computed from the m/s numbers). `ULBA2Settings` Car / CarDriven / Scooter; DA_Car and
  DA_ScooterRabibunny added to HiddenSceneEntities.
- Placement: island layer turns decor shaped like DA_Car's body into cars (`FindBodiesLike`, CITADEL 85 = CITABAU 114);
  scene layer parks the Desert's buggy at the Racetrack (scene 57 point 42, a metre left of the opponent's kart) and a
  scooter at each Citadel stand (scenes 42, 45 point 1, 46 point 0, 48, 50 point 0); `LBAPlanetSites` parks a car at
  Proxima's Marked Stone road end. Cars set themselves down on the ground at start (held until ground is found).
- The scooter: the Rabibunny drives, Twinsen rides behind (body 106); four wheels hidden under its skirt.
- Controls: W/S or RT/LT (S brakes then backs up), A/D or left stick X, Space / gamepad A handbrake, E / X in and out,
  mouse / right stick look (eases back behind after 1.5 s). Debug: `lba.Car.Debug 1`, `lba.Car.Spawn [scooter]`.
- Test: `-LBA2Shots=car` (`-LBA2Car=scooter|spawn`, `-LBA2CarYaw=`, `-LBA2HeroAt=`; use `-LBA2ShotStep=0`): logs every car,
  speeds, gears, suspension, times to half and 90% of the top speed, stopping distance. Flat runs on the feature map:
  `-LBA2HeroAt=-4000,-3000,90 -LBA2CarYaw=90`.
- Measured (feature map, flat): buggy 8.1 m/s, 90% of it after 3.3 s, stops from 7.6 m/s in 0.84 s, coasts 2.5 m/s^2,
  reverse 3 m/s, about 70 deg/s at full lock at top speed, never rolled; scooter 6.2 m/s. On L_DESERT and L_CITADEL:
  board, drive, reverse, leave all work; all six Citadel cars settled on the ground (the two stands 10 m up are on hills).

## Pitfalls (Chaos Vehicles on a box root)

- Wheels need a `BoneName` even without a skeleton (any name: on a non-skeletal root only `AdditionalOffset` counts).
- Chaos wakes only skeletal mesh roots on input: the box chassis must be woken while driven, or the physics side skips it.
- Chaos builds its suspension constraints only for a skeletal mesh root with a physics asset (`FixupSkeletalMesh`):
  `ULBACarMovementComponent` builds them for the box. Without them the chassis lay on the ground and slid on it: the car
  seemed to need 4.8 times the torque and stopped within a second when let go.
- Don't toggle the chassis' simulation after the vehicle is built (it went idle); settle with gravity off instead.
- Engine braking is engine rpm x EngineBrakeEffect (N m): 0.02 braked these small cars as hard as the brakes; 0.002.
- Any brake locks these small light wheels, which then slide at full grip: coasting is a drag force on the chassis,
  not Chaos's idle brake. Spinning wheels lose nearly all grip: traction control on.

## Open

- The user's feel test. Engine sound (the original's SAMPLE_BUGGY by rpm), dust, crashes that hurt.
- Self-righting and the water rule are built but not exercised by a test. No `LBA2.Car` automation tests yet.
- Not committed. The editor must be closed to build; reimport Islands (CITABAU, CITADEL, DESERT) and Scenes for the cars.
