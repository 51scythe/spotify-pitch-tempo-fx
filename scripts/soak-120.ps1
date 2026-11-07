# Read-only soak: wait for playback, then log applied tempo / silent packets for 4 minutes.
param([int]$SpotifyPid, [string]$Out)
$root = Split-Path $PSScriptRoot -Parent
$deadline = (Get-Date).AddMinutes(10)
do { $s = python -I "$root\scripts\status.py" $SpotifyPid | ConvertFrom-Json; Start-Sleep -Seconds 2 } until ($s.active -or (Get-Date) -gt $deadline)
$start = Get-Date
while ((Get-Date) -lt $start.AddMinutes(8)) {
    $s = python -I "$root\scripts\status.py" $SpotifyPid | ConvertFrom-Json
    "{0:N0}s active={1} tempo={2} applied={3} pitch={4} silent={5} allApplied={6}" -f ((Get-Date)-$start).TotalSeconds, $s.active, $s.tempo, $s.appliedTempo, $s.appliedPitch, $s.silentBuffers, $s.allApplied | Add-Content $Out
    Start-Sleep -Seconds 3
}
"done" | Add-Content $Out
