"""Imports the travel menu's icons as UI textures under the plugin's content, /LBA2Importer/UI: its own
(Saved/LookDev/menu_icons/T_Menu_*.png, from make_menu_icons.py) and the remake's location icons
(Saved/LookDev/locations/T_Location_*.png, from export_locations.py; LBA2_ICONS=menu imports only the first). Run headless:
  UnrealEditor-Cmd <uproject> -EnablePlugins=PythonScriptPlugin -run=pythonscript -script=<this file>
"""
import glob, os
import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCES = [os.path.join(PROJECT, "Saved", "LookDev", "menu_icons", "T_Menu_*.png")]
if os.environ.get("LBA2_ICONS") != "menu":
    SOURCES.append(os.path.join(PROJECT, "Saved", "LookDev", "locations", "T_Location_*.png"))
DESTINATION = "/LBA2Importer/UI"

tasks = []
for png in sorted(sum((glob.glob(pattern) for pattern in SOURCES), [])):
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
