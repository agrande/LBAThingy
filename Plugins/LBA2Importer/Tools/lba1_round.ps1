param(
	[string]$Name,                                   # output folder and sheet name under Saved/LookDev/lba1_review
	[string]$Poses = "towns",                        # "x,y,z,pitch,yaw;..." or a named set: towns
	[string]$Labels = "",                            # "a;b;..." (named sets bring their own)
	[switch]$NoBuild,
	[switch]$NoImport,
	[string]$Map = "/Game/LBA2/Planets/L_TWINSUN"    # or /Game/LBA2/Planets/L_TWINSUN_Stubs (unlit, map coordinates)
)
# One round of the LBA1 islands work (LBA1_ISLANDS_PLAN.md): build, re-import the islands (the planet's stubs and LBA1 towns
# with them), shoot, and make a sheet. Run it from PowerShell: Git Bash rewrites /Game/... paths and the game shoots nothing.
# Check other sessions first (they share the checkout): builds and imports change what they see.
$p = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$out = "$p\Saved\LookDev\lba1_review"
$named = @{
	# Ringed around the LBA1 town actors' bound centers on L_TWINSUN (dumped with dump_level_actors.py), 20-25 m off.
	"towns" = @(
		"49811,33986,1300,-20,41.5|proxim 1", "53340,33770,1300,-20,131.5|proxim 2", "53556,37299,1300,-20,221.5|proxim 3",
		"50027,37515,1300,-20,311.5|proxim 4", "9585,58876,1300,-20,-78.1|harbour 1", "7654,55914,1300,-20,11.9|harbour 2",
		"10616,53983,1300,-20,101.9|harbour 3", "12547,56945,1300,-20,191.9|harbour 4", "126,64196,1000,-20,75.8|belooga 1",
		"-1321,66625,1000,-20,-14.2|belooga 2", "5063,61402,1300,-20,-78.1|old burg 1", "5888,57488,1300,-20,101.9|old burg 2")
}
if ($named.ContainsKey($Poses)) {
	$set = $named[$Poses]
	$Poses = ($set | ForEach-Object { ($_ -split "\|")[0] }) -join ";"
	if (-not $Labels) { $Labels = ($set | ForEach-Object { ($_ -split "\|")[1] }) -join ";" }
}
if (-not $NoBuild) {
	& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" LBAThingyEditor Win64 Development "-Project=$p\LBAThingy.uproject" -WaitMutex | Select-String -Pattern "error|Result:" | Select-Object -First 10
}
if (-not $NoImport) {
	New-Item -ItemType Directory -Force $out | Out-Null
	& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$p\LBAThingy.uproject" "-run=LBA2Import" "-Layers=Islands" "-Islands=CITABAU,CITADEL,DESERT" -unattended -nop4 -nosplash -stdout -FullStdOutLogOutput 2>$null | Out-File "$out\$Name.import.log" -Encoding utf8
	"import exit $LASTEXITCODE"
	Select-String -Path "$out\$Name.import.log" -Pattern "LBA1 kit.*(Warning|no mesh|can't)|LBA1 towns on" | Select-Object -First 8 | ForEach-Object { $_.Line }
}
& "$PSScriptRoot\shots.ps1" -Map $Map -Poses $Poses -OutDir "$out\$Name" | Out-Null
$list = $Poses -split ";"
$names = if ($Labels) { $Labels -split ";" } else { @() }
$a = @()
# Shots are named after the map (L_TWINSUN_00.png, L_TWINSUN_Stubs_00.png).
for ($i = 0; $i -lt $list.Count; $i++) { $a += "$out\$Name\$(Split-Path $Map -Leaf)_$($i.ToString('00')).png", $(if ($i -lt $names.Count) { $names[$i] } else { "$Name $i" }) }
python "$PSScriptRoot\sheet.py" "$out\${Name}_sheet.png" @a
