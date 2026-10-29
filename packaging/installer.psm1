Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:SpotifyHash = '93BA5EE486E148114A1D6DB388CCE318B04D2A011376FF6301F5EE0DEA432A12'
$script:ExeHash = '638F9266A5D387204DBB9DADA2820F22ACA3848B496A0363535210EA21BB1A3F'

function Get-SptContext {
    [pscustomobject]@{
        Spotify = Join-Path $env:APPDATA 'Spotify'
        Install = Join-Path $env:LOCALAPPDATA 'SpotifyPitchTempo'
        Config = Join-Path $env:APPDATA 'spicetify'
        Backups = Join-Path $env:LOCALAPPDATA 'SpotifyPitchTempo-backups'
        Desktop = [Environment]::GetFolderPath('Desktop')
        Programs = Join-Path ([Environment]::GetFolderPath('Programs')) 'Spotify Pitch Tempo FX'
    }
}

function Assert-SptUser {
    if (![Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') {
        throw 'This release requires x64 Windows 10 or 11 on an Intel/AMD PC.'
    }
    if ([Environment]::OSVersion.Version.Major -lt 10) { throw 'Windows 10 or 11 is required.' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Close Setup and double-click it normally. Do not use Run as administrator.'
    }
    if ($env:SPICETIFY_CONFIG -or $env:SPICETIFY_STATE -or (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.spicetify'))) {
        throw 'This installer supports the standard %APPDATA%\spicetify configuration. A custom or legacy Spicetify location was detected.'
    }
}

function Assert-SptStopped {
    if (Get-Process Spotify,SpotifyPitchTempo -ErrorAction SilentlyContinue) {
        throw 'Quit Spotify completely (including its tray icon), then click Install or Remove again. The helper closes with Spotify.'
    }
}

function Get-SptIniValue($Path, $Section, $Key) {
    if (!(Test-Path -LiteralPath $Path)) { return '' }
    $current = ''
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\s*\[([^\]]+)\]') { $current = $Matches[1] }
        elseif ($current -eq $Section -and $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$')) {
            return $Matches[1].Trim('"')
        }
    }
    return ''
}

function Assert-SptSpotify($Context) {
    foreach ($name in @('Spotify.exe','Spotify.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $Context.Spotify $name))) {
            throw 'Desktop Spotify was not found in %APPDATA%\Spotify. Install the desktop version from spotify.com, open it once and sign in. Microsoft Store Spotify is not supported by this audio backend.'
        }
    }
    $hash = (Get-FileHash -LiteralPath (Join-Path $Context.Spotify 'Spotify.dll') -Algorithm SHA256).Hash
    $exeHash = (Get-FileHash -LiteralPath (Join-Path $Context.Spotify 'Spotify.exe') -Algorithm SHA256).Hash
    if ($hash -ne $script:SpotifyHash -or $exeHash -ne $script:ExeHash) {
        $version = (Get-Item -LiteralPath (Join-Path $Context.Spotify 'Spotify.exe')).VersionInfo.ProductVersion
        throw "Unsupported Spotify build ($version). This setup supports only desktop x64 Spotify 1.3.6.1921. Obtain an updated FX setup for your Spotify version. No installation changes were made."
    }
    if (!(Test-Path -LiteralPath (Join-Path $Context.Spotify 'Apps') -PathType Container)) { throw 'Spotify UI assets are missing. Repair Spotify first.' }
    $version = Get-SptIniValue (Join-Path $Context.Spotify 'prefs') '' 'app.last-launched-version'
    if (!$version.StartsWith('1.3.6.1921')) { throw 'Open the supported Spotify build once, sign in, then quit it and retry setup.' }
    Write-Output 'Compatible: Spotify 1.3.6.1921 x64 (native EXE and DLL hashes verified).'
}

function Test-SptPayload($Payload) {
    $manifestPath = Join-Path $Payload 'manifest.json'
    if (!(Test-Path -LiteralPath $manifestPath)) { throw 'Setup payload is incomplete. Extract/download the complete package again.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.protocol -ne 3 -or $manifest.spotifyDllSha256 -ne $script:SpotifyHash) { throw 'Unsupported setup manifest.' }
    $base = [IO.Path]::GetFullPath($Payload).TrimEnd('\') + '\'
    $seen = @{}
    foreach ($entry in $manifest.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $Payload $entry.path))
        if (!$path.StartsWith($base, [StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($path)) { throw 'Invalid path in setup manifest.' }
        $seen[$path] = $true
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "Setup file is missing or damaged: $($entry.path)"
        }
    }
    foreach ($required in @('bin\SpotifyPitchTempo.exe','bin\spotify-pitch-tempo.dll','extension\pitch-tempo.js','spicetify\spicetify.exe','setup.ps1','installer.psm1','Remove.cmd','START-HERE.txt','THIRD-PARTY-NOTICES.md')) {
        if (!$seen.ContainsKey([IO.Path]::GetFullPath((Join-Path $Payload $required)))) { throw "Required file absent from manifest: $required" }
    }
    $template = [IO.File]::ReadAllText((Join-Path $Payload 'extension\pitch-tempo.js'))
    if (!$template.Contains('__BRIDGE_TOKEN__')) { throw 'Setup must contain the token-free extension template.' }
    Write-Output "Package integrity verified: $($manifest.files.Count) files, release $($manifest.release)."
}

function New-SptToken {
    $bytes = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return -join ($bytes | ForEach-Object { $_.ToString('x2') })
}

function Get-SptTouchedPaths($Context) {
    @(
        (Join-Path $Context.Spotify 'Apps'),
        $Context.Install,
        $Context.Config,
        (Join-Path $Context.Desktop 'Spotify + Pitch & Tempo.lnk'),
        $Context.Programs
    )
}

function New-SptSnapshot($Context, $Operation) {
    $stamp = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $backup = Join-Path $Context.Backups ("setup-$Operation-$stamp")
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    $entries = @()
    foreach ($path in (Get-SptTouchedPaths $Context)) {
        $exists = Test-Path -LiteralPath $path
        $entry = [pscustomobject]@{ path=$path; existed=$exists; directory=$false; saved=('item-' + $entries.Count) }
        if ($exists) {
            $item = Get-Item -LiteralPath $path -Force
            $entry.directory = $item.PSIsContainer
            $links = @($item) + @(if ($entry.directory) { Get-ChildItem -LiteralPath $path -Force -Recurse })
            if (@($links | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) {
                throw "Cannot make an exact backup of linked/junction-based assets: $path. Use a standard Spotify/Spicetify installation."
            }
            Copy-Item -LiteralPath $path -Destination (Join-Path $backup $entry.saved) -Recurse -Force
        }
        $entries += $entry
    }
    $entries | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'snapshot.json') -Encoding UTF8
    return $backup
}

function Restore-SptSnapshot($Backup) {
    $entries = Get-Content -LiteralPath (Join-Path $Backup 'snapshot.json') -Raw | ConvertFrom-Json
    foreach ($entry in $entries) {
        if (Test-Path -LiteralPath $entry.path) { Remove-Item -LiteralPath $entry.path -Recurse -Force }
        if ($entry.existed) {
            New-Item -ItemType Directory -Path (Split-Path $entry.path -Parent) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $Backup $entry.saved) -Destination $entry.path -Recurse -Force
        }
    }
}

function Invoke-SptSpice($Exe, [string[]]$Arguments) {
    if (!(Test-Path -LiteralPath $Exe -PathType Leaf)) { throw 'The bundled Spicetify executable is missing.' }
    # Windows PowerShell treats native stderr (including progress) as error records.
    # Decide success from the exit code; retain both streams in the setup log.
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Exe @Arguments 2>&1 | ForEach-Object {
            $line = if ($_ -is [Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
            $line = [regex]::Replace($line,'\x1b\[[0-9;?]*[A-Za-z]','')
            if (![string]::IsNullOrWhiteSpace($line)) { Write-Output $line }
        }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($code -ne 0) { throw "Spicetify failed (exit $code): $($Arguments -join ' ')" }
}

function New-SptShortcut($Path, $Target, $Arguments, $Working, $Icon) {
    New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    $shortcut.Arguments = $Arguments
    $shortcut.WorkingDirectory = $Working
    $shortcut.IconLocation = $Icon
    $shortcut.Description = 'Spotify-only pitch, tempo, reverb, distortion and muffle'
    $shortcut.Save()
}

function Invoke-SptInstall($Context, $Payload) {
    Test-SptPayload $Payload
    Assert-SptSpotify $Context
    $configFile = Join-Path $Context.Config 'config-xpui.ini'
    $backupVersion = Get-SptIniValue $configFile 'Backup' 'version'
    $withVersion = Get-SptIniValue $configFile 'Backup' 'with'
    $spotifyVersion = Get-SptIniValue (Join-Path $Context.Spotify 'prefs') '' 'app.last-launched-version'
    $stock = Test-Path -LiteralPath (Join-Path $Context.Spotify 'Apps\xpui.spa')
    $backupAvailable = Test-Path -LiteralPath (Join-Path $Context.Config 'Backup\xpui.spa')
    if (!$stock -and (!$backupAvailable -or $backupVersion -ne $spotifyVersion)) {
        throw 'Spotify is already customized but has no matching Spicetify backup. Restore/repair its UI first, then rerun this setup.'
    }
    $tokenPath = Join-Path $Context.Install 'bridge-token.txt'
    $token = if (Test-Path -LiteralPath $tokenPath) { [IO.File]::ReadAllText($tokenPath).Trim() } else { New-SptToken }
    if ($token -notmatch '^[0-9a-f]{64}$') { throw 'The existing private bridge token is invalid. Keep a backup and repair this installation first.' }
    Write-Output 'Backing up Spotify UI, existing helper, Spicetify configuration and shortcuts...'
    $backup = New-SptSnapshot $Context 'install'
    Write-Output "Backup: $backup"
    try {
        New-Item -ItemType Directory -Path $Context.Install,(Join-Path $Context.Config 'Extensions') -Force | Out-Null
        foreach ($name in @('SpotifyPitchTempo.exe','spotify-pitch-tempo.dll')) {
            Copy-Item -LiteralPath (Join-Path $Payload "bin\$name") -Destination (Join-Path $Context.Install $name) -Force
        }
        [IO.File]::WriteAllText($tokenPath,$token)
        # Private CLI copy: existing global Spicetify executables and PATH stay intact.
        $spiceDir = Join-Path $Context.Install 'spicetify'
        if (Test-Path -LiteralPath $spiceDir) { Remove-Item -LiteralPath $spiceDir -Recurse -Force }
        Copy-Item -LiteralPath (Join-Path $Payload 'spicetify') -Destination $spiceDir -Recurse
        foreach ($dir in @('licenses','third-party-source')) {
            $dest = Join-Path $Context.Install $dir
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            Copy-Item -Path (Join-Path $Payload "$dir\*") -Destination $dest -Recurse -Force
        }
        foreach ($name in @('START-HERE.txt','THIRD-PARTY-NOTICES.md','Remove.cmd','setup.ps1','installer.psm1')) {
            Copy-Item -LiteralPath (Join-Path $Payload $name) -Destination (Join-Path $Context.Install $name) -Force
        }
        $extension = [IO.File]::ReadAllText((Join-Path $Payload 'extension\pitch-tempo.js')).Replace('__BRIDGE_TOKEN__',$token)
        [IO.File]::WriteAllText((Join-Path $Context.Config 'Extensions\pitch-tempo.js'),$extension,(New-Object Text.UTF8Encoding($false)))
        $spice = Join-Path $spiceDir 'spicetify.exe'
        Invoke-SptSpice $spice @('config','spotify_path',$Context.Spotify,'prefs_path',(Join-Path $Context.Spotify 'prefs'))
        Invoke-SptSpice $spice @('config','extensions','pitch-tempo.js','expose_apis','1')
        if ($stock) { Invoke-SptSpice $spice @('backup','apply','-n') }
        elseif ($withVersion -ne '2.46.4' -or !(Test-Path -LiteralPath (Join-Path $Context.Config 'Extracted\Raw\xpui\index.html'))) {
            Invoke-SptSpice $spice @('restore','backup','apply','-n')
        } else { Invoke-SptSpice $spice @('apply','-n') }
        $index = Join-Path $Context.Spotify 'Apps\xpui\index.html'
        $deployed = Join-Path $Context.Spotify 'Apps\xpui\extensions\pitch-tempo.js'
        if (!(Test-Path -LiteralPath $index) -or !([IO.File]::ReadAllText($index).Contains('extensions/pitch-tempo.js')) -or
            !(Test-Path -LiteralPath $deployed) -or !([IO.File]::ReadAllText($deployed).Contains($token))) {
            throw 'Spicetify did not deploy the FX extension. Restoring the previous installation.'
        }
        Assert-SptSpotify $Context
        $helper = Join-Path $Context.Install 'SpotifyPitchTempo.exe'
        $icon = (Join-Path $Context.Spotify 'Spotify.exe') + ',0'
        New-SptShortcut (Join-Path $Context.Desktop 'Spotify + Pitch & Tempo.lnk') $helper '--launch --background' $Context.Install $icon
        New-SptShortcut (Join-Path $Context.Programs 'Spotify + Pitch & Tempo.lnk') $helper '--launch --background' $Context.Install $icon
        New-SptShortcut (Join-Path $Context.Programs 'Remove Pitch Tempo FX.lnk') (Join-Path $Context.Install 'Remove.cmd') '' $Context.Install $icon
        [pscustomobject]@{release='1.0.0-fx3';protocol=3;backup=$backup;installed=(Get-Date).ToString('o')} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Context.Install 'installation.json') -Encoding UTF8
        Write-Output 'SUCCESS: installed. Open Spotify + Pitch & Tempo from your desktop, press play, then open the sliders icon. Effects start off.'
    } catch {
        $failure = $_.Exception.Message
        Write-Output "Installation failed: $failure"
        try { Restore-SptSnapshot $backup; Write-Output 'Previous UI, helper, configuration and shortcuts restored.' }
        catch { throw "Installation failed: $failure. Automatic restore also failed: $($_.Exception.Message). Backup: $backup" }
        throw $failure
    }
}

function Invoke-SptUninstall($Context) {
    if (!(Test-Path -LiteralPath $Context.Install)) { Write-Output 'Nothing to remove: helper is not installed.'; return }
    $spice = Join-Path $Context.Install 'spicetify\spicetify.exe'
    if (!(Test-Path -LiteralPath $spice)) { $spice = Join-Path $env:LOCALAPPDATA 'spicetify\spicetify.exe' }
    $index = Join-Path $Context.Spotify 'Apps\xpui\index.html'
    $deployed = Join-Path $Context.Spotify 'Apps\xpui\extensions\pitch-tempo.js'
    $present = (Test-Path -LiteralPath $index) -and ([IO.File]::ReadAllText($index).Contains('pitch-tempo.js'))
    if ($present -and !(Test-Path -LiteralPath $spice)) { throw 'Spicetify is missing. Reinstall the FX setup before removing it.' }
    $backup = New-SptSnapshot $Context 'remove'
    Write-Output "Removal backup: $backup"
    try {
        $config = Join-Path $Context.Config 'config-xpui.ini'
        # Remove only our list entry, even if Spotify was updated or uninstalled.
        if (Test-Path -LiteralPath $config) {
            $text = [IO.File]::ReadAllText($config)
            $text = [regex]::Replace($text,'(?m)^([ \t]*extensions[ \t]*=[ \t]*)([^\r\n]*)', {
                param($match)
                $entries = @($match.Groups[2].Value.Split('|') | ForEach-Object {$_.Trim()} | Where-Object {$_ -and $_ -ne 'pitch-tempo.js'})
                return $match.Groups[1].Value + ($entries -join '|')
            })
            [IO.File]::WriteAllText($config,$text,(New-Object Text.UTF8Encoding($false)))
        }
        if ($present) {
            Invoke-SptSpice $spice @('apply','-n')
            if ([IO.File]::ReadAllText($index).Contains('pitch-tempo.js')) { throw 'UI still references the extension; removal was not completed.' }
        }
        foreach ($path in @((Join-Path $Context.Config 'Extensions\pitch-tempo.js'),$deployed,(Join-Path $Context.Desktop 'Spotify + Pitch & Tempo.lnk'))) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
        foreach ($name in @('Spotify + Pitch & Tempo.lnk','Remove Pitch Tempo FX.lnk')) {
            $path=Join-Path $Context.Programs $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
        foreach ($name in @('SpotifyPitchTempo.exe','spotify-pitch-tempo.dll','bridge-token.txt','START-HERE.txt','README.md','THIRD-PARTY-NOTICES.md',
            'Remove.cmd','setup.ps1','installer.psm1','installation.json','spicetify','third-party-source',
            'licenses\MinHook.txt','licenses\Signalsmith-Stretch.txt','licenses\Signalsmith-Linear.txt','licenses\Spicetify-LGPL-2.1.txt')) {
            $path=Join-Path $Context.Install $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
        Write-Output 'SUCCESS: FX removed. Spotify, other Spicetify customizations and backups are preserved.'
    } catch {
        $failure=$_.Exception.Message
        try { Restore-SptSnapshot $backup; Write-Output 'Previous installation restored after removal failed.' }
        catch { throw "Removal failed: $failure. Restore failed: $($_.Exception.Message). Backup: $backup" }
        throw $failure
    }
}

Export-ModuleMember -Function *-Spt*
