param(
	[string]$Name,                       # output folder under Saved/LookDev/sides
	[string]$Grids = "35",
	[switch]$Unlit,                      # viewmode unlit (art without lighting)
	[string]$Exec = ""                   # more console commands for every shot
)
# Shots of every side and corner of the rooms of each grid (side_poses.py, from the last import's model dump), one
# sheet per grid: sides -X +X -Z +Z, then corners.
$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$o = "$Root\Saved\LookDev\sides\$Name"
New-Item -ItemType Directory -Force $o | Out-Null
$Levels = "$Root\Content\LBA2\Interiors\Grids\Levels"
$Labels = @("-X", "+X", "-Z", "+Z", "corner -X-Z", "corner +X-Z", "corner -X+Z", "corner +X+Z")
$cmds = @(); if ($Unlit) { $cmds += "viewmode unlit" }; if ($Exec) { $cmds += $Exec }
foreach ($g in $Grids.Split(",")) {
	$map = Get-ChildItem $Levels -Filter ("L_Grid_{0:D3}*.umap" -f [int]$g) | Select-Object -First 1
	$poses = python "$PSScriptRoot\side_poses.py" $g
	$shotArgs = @{ Map = "/Game/LBA2/Interiors/Grids/Levels/$($map.BaseName)"; Poses = $poses; OutDir = $o }
	if ($cmds) { $shotArgs.Exec = $cmds -join "," }
	& "$PSScriptRoot\shots.ps1" @shotArgs | Out-Null
	$sheetArgs = @()
	$i = 0
	Get-ChildItem $o -Filter "$($map.BaseName)_*.png" | Sort-Object Name | ForEach-Object {
		$sheetArgs += $_.FullName; $sheetArgs += ("R{0} {1}" -f [math]::Floor($i / 8), $Labels[$i % 8]); $i++ }
	python "$PSScriptRoot\sheet.py" ("$o\sheet_{0:D3}.png" -f [int]$g) @sheetArgs
	"sheet $o\sheet_{0:D3}.png" -f [int]$g
}
