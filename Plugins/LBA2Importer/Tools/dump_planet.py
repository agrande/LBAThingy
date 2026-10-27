"""Dumps a planet asset's islands (place, heading, land center, coastline) to Saved/LookDev/holomap/planet.json.

Run in the editor: UnrealEditor-Cmd <uproject> -EnablePlugins=PythonScriptPlugin -run=pythonscript
"-script=<this file>" (env LBA2_PLANET: the asset, default Twinsun's). Read by holomap_fit.py.
"""
import json
import os

import unreal

ASSET = os.environ.get("LBA2_PLANET", "/Game/LBA2/Planets/DA_Planet_TWINSUN")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../Saved/LookDev/holomap")

planet = unreal.load_asset(ASSET)
data = {"radius": planet.get_editor_property("radius"), "islands": []}
for island in planet.get_editor_property("islands"):
    center = island.get_editor_property("center")
    data["islands"].append({
        "name": str(island.get_editor_property("island")),
        "lat": island.get_editor_property("latitude"),
        "lon": island.get_editor_property("longitude"),
        "heading": island.get_editor_property("heading"),
        "center": [center.x, center.y],
        "coast": [[p.x, p.y] for p in island.get_editor_property("coastline")],
    })
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "planet.json"), "w") as f:
    json.dump(data, f)
unreal.log_warning("DUMP %s: %d islands" % (ASSET, len(data["islands"])))
