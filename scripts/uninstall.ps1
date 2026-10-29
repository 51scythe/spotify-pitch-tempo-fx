$ErrorActionPreference = 'Stop'
if (Get-Process Spotify,SpotifyPitchTempo -ErrorAction SilentlyContinue) {
    throw 'Quit Spotify and the Pitch + Tempo helper before uninstalling.'
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this uninstaller in a normal, non-administrator PowerShell.'
}
$spice = Join-Path $env:LOCALAPPDATA 'spicetify\spicetify.exe'
$extension = Join-Path $env:APPDATA 'spicetify\Extensions\pitch-tempo.js'
if (!(Test-Path -LiteralPath $spice)) { throw 'Spicetify is needed to remove the extension from the UI.' }
& $spice config extensions 'pitch-tempo.js-'
if ($LASTEXITCODE) { throw 'Could not remove extension configuration.' }
& $spice apply -n
if ($LASTEXITCODE) { throw 'Could not apply removal. Installed helper files retained.' }
if (Test-Path -LiteralPath $extension) { Remove-Item -LiteralPath $extension }
$shortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Spotify + Pitch & Tempo.lnk'
if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut }
$install = Join-Path $env:LOCALAPPDATA 'SpotifyPitchTempo'
# Delete only this project's known files; preserve any unrelated user additions.
foreach ($name in 'SpotifyPitchTempo.exe','spotify-pitch-tempo.dll','bridge-token.txt','README.md','THIRD-PARTY-NOTICES.md',
    'licenses\MinHook.txt','licenses\Signalsmith-Stretch.txt','licenses\Signalsmith-Linear.txt') {
    $path = Join-Path $install $name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
}
Write-Output 'Pitch + Tempo removed. Spicetify, other extensions and backups are preserved.'
