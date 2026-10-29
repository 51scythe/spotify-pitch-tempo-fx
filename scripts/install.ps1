param([switch]$NoApply)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$install = Join-Path $env:LOCALAPPDATA 'SpotifyPitchTempo'
$spiceHome = Join-Path $env:LOCALAPPDATA 'spicetify'
$spiceConfig = Join-Path $env:APPDATA 'spicetify'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $root "reports\install-$stamp.log"
Start-Transcript -LiteralPath $log -Force
try {
    if (Get-Process Spotify,SpotifyPitchTempo -ErrorAction SilentlyContinue) { throw 'Quit Spotify and the helper before installing or updating.' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this installer in a normal, non-administrator PowerShell.' }
    New-Item -ItemType Directory -Path $install, $spiceHome, "$spiceConfig\Extensions" -Force | Out-Null
    $native = Get-Process SpotifyPitchTempo -ErrorAction SilentlyContinue
    if ($native) { throw 'Close Spotify Pitch + Tempo before installing an update.' }
    $backup = Join-Path $env:LOCALAPPDATA "SpotifyPitchTempo-backups\$stamp"
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($name in @('SpotifyPitchTempo.exe','spotify-pitch-tempo.dll','README.md')) {
        if (Test-Path -LiteralPath "$install\$name") { Copy-Item -LiteralPath "$install\$name" -Destination "$backup\$name" }
    }
    Copy-Item "$root\build\SpotifyPitchTempo.exe", "$root\build\spotify-pitch-tempo.dll" -Destination $install -Force
    $tokenPath = Join-Path $install 'bridge-token.txt'
    if (!(Test-Path -LiteralPath $tokenPath)) {
        $bytes = New-Object byte[] 32
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        $rng.GetBytes($bytes)
        $rng.Dispose()
        $token = -join ($bytes | ForEach-Object { $_.ToString('x2') })
        [IO.File]::WriteAllText($tokenPath, $token)
    } else { $token = [IO.File]::ReadAllText($tokenPath).Trim() }
    if ($token -notmatch '^[0-9a-f]{64}$') { throw 'Invalid existing bridge token.' }
    Copy-Item "$root\vendor\spicetify-2.46.4\*" -Destination $spiceHome -Recurse -Force
    $spice = Join-Path $spiceHome 'spicetify.exe'
    $config = Join-Path $spiceConfig 'config-xpui.ini'
    if (Test-Path -LiteralPath $config) {
        Copy-Item -LiteralPath $config -Destination "$root\reports\config-xpui-before-$stamp.ini"
    }
    Copy-Item "$env:APPDATA\Spotify\Apps" -Destination "$backup\Apps" -Recurse
    if (Test-Path -LiteralPath "$spiceConfig\Extensions\pitch-tempo.js") {
        Copy-Item -LiteralPath "$spiceConfig\Extensions\pitch-tempo.js" -Destination "$backup\pitch-tempo.js"
    }
    $extension = [IO.File]::ReadAllText("$root\src\pitch-tempo.js").Replace('__BRIDGE_TOKEN__', $token)
    [IO.File]::WriteAllText("$spiceConfig\Extensions\pitch-tempo.js", $extension, (New-Object Text.UTF8Encoding($false)))
    & $spice config spotify_path "$env:APPDATA\Spotify" prefs_path "$env:APPDATA\Spotify\prefs"
    if ($LASTEXITCODE) { throw 'Spicetify configuration failed.' }
    & $spice config extensions pitch-tempo.js
    if ($LASTEXITCODE) { throw 'Extension configuration failed.' }
    if (!$NoApply) {
        if (Test-Path -LiteralPath "$spiceConfig\Backup\xpui.spa") { & $spice apply -n }
        else { & $spice backup apply -n }
        if ($LASTEXITCODE) { throw 'Spicetify apply failed.' }
    }
    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $shortcut = $shell.CreateShortcut((Join-Path $desktop 'Spotify + Pitch & Tempo.lnk'))
    $shortcut.TargetPath = Join-Path $install 'SpotifyPitchTempo.exe'
    $shortcut.Arguments = '--launch --background'
    $shortcut.WorkingDirectory = $install
    $shortcut.IconLocation = "$env:APPDATA\Spotify\Spotify.exe,0"
    $shortcut.Description = 'Spotify with native pitch, tempo, reverb, distortion and muffle controls'
    $shortcut.Save()
    Copy-Item "$root\README.md" -Destination $install -Force
    Copy-Item "$root\THIRD-PARTY-NOTICES.md" -Destination $install -Force
    New-Item -ItemType Directory -Path "$install\licenses" -Force | Out-Null
    Copy-Item "$root\vendor\minhook\LICENSE.txt" -Destination "$install\licenses\MinHook.txt" -Force
    Copy-Item "$root\vendor\signalsmith-stretch\LICENSE.txt" -Destination "$install\licenses\Signalsmith-Stretch.txt" -Force
    Copy-Item "$root\vendor\signalsmith-linear\LICENSE.txt" -Destination "$install\licenses\Signalsmith-Linear.txt" -Force
    Write-Output "Installed native backend: $install"
    Write-Output "Installed Spotify extension: $spiceConfig\Extensions\pitch-tempo.js"
    [IO.File]::WriteAllText("$root\reports\install-result.txt", 'SUCCESS')
} catch {
    [IO.File]::WriteAllText("$root\reports\install-result.txt", "FAILED: $_")
    throw
} finally { Stop-Transcript }
