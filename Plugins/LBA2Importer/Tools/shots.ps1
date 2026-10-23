param(
	[string]$Map,          # e.g. /Game/LBA2/Islands/CITABAU/L_CITABAU
	[string]$Poses,        # "x,y,z,pitch,yaw;..."
	[string]$OutDir,
	[string]$Exec = "",    # console commands, comma separated
	[string[]]$Extra = @() # more command line arguments (e.g. -LBA2HeroAt=x,y,yaw)
)
$Editor = "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = (Get-ChildItem (Resolve-Path "$PSScriptRoot\..\..\..") -Filter *.uproject | Select-Object -First 1).FullName
New-Item -ItemType Directory -Force $OutDir | Out-Null
$A = @("`"$Project`"", $Map, "-game", "-windowed", "-ResX=1280", "-ResY=720", "-LBA2Shots=$Poses", "-LBA2ShotDir=`"$OutDir`"", "-nosplash", "-unattended")
if ($Exec -ne "") { $A += "-ExecCmds=`"$Exec`"" }
$A += $Extra
# Wait for the editor only: -Wait also waits for its children, and the adb server it may start never exits.
$p = Start-Process -FilePath $Editor -ArgumentList $A -PassThru
$p.WaitForExit()
Get-ChildItem $OutDir | Select-Object Name, Length
