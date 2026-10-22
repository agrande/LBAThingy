param(
	[string]$Name,                       # output folder under Saved/LookDev/rooms
	[string]$Grids = "9,7",
	[int]$Count = 4,                     # poses per grid (room_poses.py)
	[string]$Exec = "",                  # console commands for every shot
	[switch]$Spread,                     # poses spread over the floor (room_poses.py -spread)
	[switch]$NoImport,
	[switch]$NoBuild
)
# Build, reimport interior grids, capture eye-level shots inside the largest room of each, one sheet per run.
$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$o = "$Root\Saved\LookDev\rooms\$Name"
New-Item -ItemType Directory -Force $o | Out-Null
if (-not $NoBuild -and -not $NoImport) {
	$build = & "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" LBAThingyEditor Win64 Development "-Project=$Root\LBAThingy.uproject" -WaitMutex
	$build | Select-String "error|Result:"
	if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; exit 1 }
}
if (-not $NoImport) {
	& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$Root\LBAThingy.uproject" "-run=LBA2Import" "-Layers=Interiors" "-Grids=$Grids" -unattended -nosplash > "$o\import.log"
	Select-String -Path "$o\import.log" -Pattern "LogLBA2: (Warning|Error)|error\(s\)" | ForEach-Object { $_.Line -replace '^.*LogLBA2: ', '' }
}
$Levels = "$Root\Content\LBA2\Interiors\Grids\Levels"
$sheetArgs = @()
foreach ($g in $Grids.Split(",")) {
	$map = Get-ChildItem $Levels -Filter ("L_Grid_{0:D3}*.umap" -f [int]$g) | Select-Object -First 1
	$poseArgs = @($g, $Count); if ($Spread) { $poseArgs += "-spread" }
	$poses = (python "$PSScriptRoot\room_poses.py" @poseArgs) -replace ',-10,', ',-3,'
	$shotArgs = @{ Map = "/Game/LBA2/Interiors/Grids/Levels/$($map.BaseName)"; Poses = $poses; OutDir = $o }
	if ($Exec) { $shotArgs.Exec = $Exec }
	& "$PSScriptRoot\shots.ps1" @shotArgs | Out-Null
	Get-ChildItem $o -Filter "$($map.BaseName)_*.png" | ForEach-Object { $sheetArgs += $_.FullName; $sheetArgs += $_.BaseName.Substring(7) }
}
python "$PSScriptRoot\sheet.py" "$o\sheet.png" @sheetArgs
"sheet $o\sheet.png"
