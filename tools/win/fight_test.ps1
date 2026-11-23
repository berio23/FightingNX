# A PC run that reaches a real Fight Now fight (Tyson vs Ali, Boardwalk Hall, ~67 s after boot) instead of the
# new-profile intro (knocked down, stand up). Seeds a fresh user folder with a save from out\pc-renut\seed_save
# (a copy of a profile: <xuid>\45410915\...; never the player's own folder) and drives the menus:
#   20 s START (title) - 24 s A (autosave notice) - 32 s A (Fight Now) - 37..62 s A (defaults: boxers, venue,
#   controls), then the fight runs on its own (the opponent AI fights back).
# Usage: fight_test.ps1 -Name <run name> [-Seconds 100] [-VideoAt 75 -VideoFrames 240 -VideoEvery 1] [-ExeArgs "..."]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$Seconds = 100,
    [int]$VideoAt = 0,
    [int]$VideoFrames = 0,
    [int]$VideoEvery = 1,
    [string]$ExeArgs = "",
    [string]$Exe = "out\pc-renut\sa2.exe",
    [int]$Tries = 3
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$seed = "out\pc-renut\seed_save"
if (-not (Test-Path $seed)) { Write-Error "no save in $seed"; exit 1 }
$user = "out\pc-renut\${Name}_user"
$cache = "out\pc-renut\${Name}_cache"
$script = "20:START,24:A,32:A,37:A,42:A,47:A,52:A,57:A,62:A"
$log = $null
for ($try = 1; $try -le $Tries; $try++) {
    Remove-Item -Recurse -Force $user, $cache -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $user | Out-Null
    Get-ChildItem $seed | Copy-Item -Recurse -Destination $user
    $args = "--sa2_renderer=renut --resolution_scale=1 --fnc_frame_rate=30 " +
            "--user_data_root=$root/$user --cache_root=$root/$cache --input_script=$script $ExeArgs"
    $bt = @("-Exe", $Exe, "-Seconds", $Seconds, "-ExeArgs", $args)
    if ($VideoFrames -gt 0) { $bt += @("-VideoAt", $VideoAt, "-VideoFrames", $VideoFrames, "-VideoEvery", $VideoEvery) }
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 @bt *> "out\${Name}_run.txt"
    $log = (Get-ChildItem out\pc-renut\logs\fnc-nx_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    # A known PC-only start-up race (stage3.md, "Open"): a null call on the audio thread in the first seconds.
    if (-not (Select-String -Path $log -Pattern "\[FATAL\]" -Quiet)) { break }
    Write-Host "try $try hit the start-up audio race; again"
}
Write-Host "log: $log"
