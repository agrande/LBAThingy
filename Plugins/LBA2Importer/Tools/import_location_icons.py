"""Imports the travel menu's location icons (Saved/LookDev/locations/T_Location_*.png, from export_locations.py) as
UI textures under the plugin's content, /LBA2Importer/UI. Run headless:
  UnrealEditor-Cmd <uproject> -EnablePlugins=PythonScriptPlugin -run=pythonscript -script=<this file>
"""
import glob, os
import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE = os.path.join(PROJECT, "Saved", "LookDev", "locations")
DESTINATION = "/LBA2Importer/UI"

tasks = []
for png in sorted(glob.glob(os.path.join(SOURCE, "T_Location_*.png"))):
    task = unreal.AssetImportTask()
    task.filename = png
    task.destination_path = DESTINATION
    task.destination_name = os.path.splitext(os.path.basename(png))[0]
    task.automated = True
    task.replace_existing = True
    task.save = False
    tasks.append(task)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

for task in tasks:
    for path in task.imported_object_paths:
        texture = unreal.load_asset(path)
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        texture.set_editor_property("srgb", True)
        unreal.EditorAssetLibrary.save_loaded_asset(texture, False)
        unreal.log_warning("DUMP imported %s" % path)
