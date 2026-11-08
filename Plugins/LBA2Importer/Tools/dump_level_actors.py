"""Dumps a level's actors whose label starts with a prefix (location, yaw, bounds) to JSON: aim points for shots.

Run headless (env LBA2_DUMP_MAP, LBA2_DUMP_PREFIX, LBA2_DUMP_OUT; defaults: L_TWINSUN, "Town_", Saved/LookDev/lba1_review):
  UnrealEditor-Cmd <uproject> -EnablePlugins=PythonScriptPlugin -run=pythonscript "-script=<this file>" -unattended -nop4 -nosplash
The LBA1 town actors' bound centers (Town_<Island>_Scene_NNN) are good aim points: their location is the town's frame origin,
often off the town. Bent meshes have a large negative Z bounds extension, so only the center's x and y mean much.
Instanced actors also list their instances' locations ("instances"). The kit's props are one actor per copy (Kit_<Island>_<model>_NNN).
"""
import json
import os

import unreal

MAP = os.environ.get("LBA2_DUMP_MAP", "/Game/LBA2/Planets/L_TWINSUN")
PREFIX = os.environ.get("LBA2_DUMP_PREFIX", "Town_")
OUT = os.environ.get("LBA2_DUMP_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../Saved/LookDev/lba1_review"))

unreal.EditorLoadingAndSavingUtils.load_map(MAP)
actors = []
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    label = actor.get_actor_label()
    if not label.startswith(PREFIX):
        continue
    origin, extent = actor.get_actor_bounds(False)
    location = actor.get_actor_location()
    actors.append(dict(name=label, loc=[location.x, location.y, location.z], yaw=actor.get_actor_rotation().yaw,
                       center=[origin.x, origin.y, origin.z], extent=[extent.x, extent.y, extent.z]))
    for component in actor.get_components_by_class(unreal.InstancedStaticMeshComponent):
        points = []
        for index in range(component.get_instance_count()):
            # Returned alone, or after the call's success flag, depending on the engine's binding.
            result = component.get_instance_transform(index, True)
            t = (result[-1] if isinstance(result, tuple) else result).translation
            points.append([round(t.x), round(t.y), round(t.z)])
        actors[-1].setdefault("instances", []).extend(points)
os.makedirs(OUT, exist_ok=True)
path = os.path.join(OUT, "actors_%s.json" % MAP.rsplit("/", 1)[-1])
json.dump(actors, open(path, "w"), indent=1)
unreal.log("Dumped %d actors to %s" % (len(actors), path))
