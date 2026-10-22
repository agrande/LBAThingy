import unreal

# Exports interior enclosure meshes (and the grid meshes) as OBJ, to check their faces outside the engine.
out = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()) + "LookDev/enclosure/obj"
GRIDS = [0, 9, 7, 35, 22, 113, 82, 48, 38, 23]
paths = []
for grid in GRIDS:
    paths += ["/Game/LBA2/Interiors/Grids/Enclosure/SM_Grid_%03d_Enclosure" % grid, "/Game/LBA2/Interiors/Grids/Enclosure/SM_Grid_%03d_Ceiling" % grid,
              "/Game/LBA2/Interiors/Grids/SM_Grid_%03d" % grid]
for path in paths:
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        continue
    mesh = unreal.EditorAssetLibrary.load_asset(path)
    task = unreal.AssetExportTask()
    task.object = mesh
    task.filename = "%s/%s.obj" % (out, mesh.get_name())
    task.automated = True
    task.prompt = False
    task.replace_identical = True
    unreal.Exporter.run_asset_export_task(task)
unreal.log_warning("DUMP exported")
