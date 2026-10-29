param([Parameter(Mandatory=$true)][string]$SetupExe,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory=Join-Path $root ('reports\setup-test-'+(Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$sandbox=Join-Path $root ('build\setup-test-'+[Guid]::NewGuid().ToString('N'))
$payload=Join-Path $sandbox 'extracted setup & spaces'
New-Item -ItemType Directory -Path $sandbox | Out-Null
$process=Start-Process -FilePath ([IO.Path]::GetFullPath($SetupExe)) -ArgumentList @('--extract-only',('"'+$payload+'"')) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Self-extraction failed' }
Import-Module (Join-Path $payload 'installer.psm1') -Force -DisableNameChecking
Test-SptPayload $payload
$script:checks=@()
function Check($Name,[bool]$Passed) {
    $script:checks += [pscustomobject]@{check=$Name;passed=$Passed}
    if (!$Passed) { throw "FAILED: $Name" }
    Write-Output "PASS: $Name"
}
function Rejected([scriptblock]$Call) { try { & $Call | Out-Null; return $false } catch { return $true } }
function TreeHash($Paths) {
    $result=@{}
    foreach ($path in $Paths) {
        if (Test-Path -LiteralPath $path) {
            foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -Force -File) { $result[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
        }
    }
    return ($result.GetEnumerator() | Sort-Object Name | ForEach-Object {$_.Name+' '+$_.Value}) -join "`n"
}
function RunNormal([string]$Script,[string]$Name) {
    $path=Join-Path $sandbox ($Name+'.ps1')
    $result=Join-Path $OutputDirectory ($Name+'-result.txt')
    $log=Join-Path $OutputDirectory ($Name+'.log')
    $wrapped=@"
`$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding(`$false)
Start-Transcript -LiteralPath '$($log.Replace("'","''"))' | Out-Null
try {
$Script
 [IO.File]::WriteAllText('$($result.Replace("'","''"))','PASS')
} catch {
 [IO.File]::WriteAllText('$($result.Replace("'","''"))','FAIL: '+`$_.Exception.Message)
} finally { Stop-Transcript | Out-Null }
"@
    [IO.File]::WriteAllText($path,$wrapped)
    & (Join-Path $root 'build\launch-user.exe') "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $path
    # launch-user returns a detached PID; wait for the explicit result, bounded.
    $deadline=(Get-Date).AddMinutes(3)
    while (!(Test-Path -LiteralPath $result) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if (!(Test-Path -LiteralPath $result)) { throw "$Name timed out" }
    $answer=[IO.File]::ReadAllText($result)
    if ($answer -ne 'PASS') { throw $answer }
}
try {
    Check 'self-contained EXE extraction and manifest' $true
    $real=Get-SptContext
    $beforeReal=TreeHash @($real.Install,$real.Config,(Join-Path $real.Spotify 'Apps'))
    $bad=Get-SptContext
    $bad.Spotify=Join-Path $sandbox 'unsupported'
    New-Item -ItemType Directory -Path $bad.Spotify | Out-Null
    [IO.File]::WriteAllText((Join-Path $bad.Spotify 'Spotify.exe'),'unsupported')
    [IO.File]::WriteAllText((Join-Path $bad.Spotify 'Spotify.dll'),'unsupported')
    Check 'unsupported Spotify rejected' (Rejected { Assert-SptSpotify $bad })
    $context=[pscustomobject]@{
        Spotify=Join-Path $sandbox 'profile\Roaming\Spotify'
        Install=Join-Path $sandbox 'profile\Local\SpotifyPitchTempo'
        Config=Join-Path $sandbox 'profile\Roaming\spicetify'
        Backups=Join-Path $sandbox 'profile\Local\Backups'
        Desktop=Join-Path $sandbox 'profile\Desktop'
        Programs=Join-Path $sandbox 'profile\Programs\FX'
    }
    $contextPath=Join-Path $sandbox 'context.json'
    $context | ConvertTo-Json | Set-Content -LiteralPath $contextPath -Encoding UTF8
    New-Item -ItemType Directory -Path $context.Spotify,(Join-Path $context.Spotify 'Apps'),$context.Desktop | Out-Null
    # Only local snapshots are used as test fixtures; none go into the release.
    foreach ($name in @('Spotify.exe','Spotify.dll','v8_context_snapshot.bin')) {
        Copy-Item -LiteralPath (Join-Path $real.Spotify $name) -Destination (Join-Path $context.Spotify $name)
    }
    Copy-Item -Path (Join-Path $real.Config 'Backup\*.spa') -Destination (Join-Path $context.Spotify 'Apps')
    [IO.File]::WriteAllText((Join-Path $context.Spotify 'prefs'),'app.last-launched-version="1.3.6.1921.g7086c071a617"')
    $prefix=@"
Import-Module '$($payload.Replace("'","''"))\installer.psm1' -Force -DisableNameChecking
`$c=Get-Content -LiteralPath '$($contextPath.Replace("'","''"))' -Raw | ConvertFrom-Json
# Child Spicetify uses a scratch Windows profile; no real config/prefs is copied.
`$env:APPDATA=Split-Path `$c.Config -Parent
`$env:LOCALAPPDATA=Split-Path `$c.Install -Parent
`$env:USERPROFILE=Split-Path `$env:APPDATA -Parent
Remove-Item Env:SPICETIFY_CONFIG,Env:SPICETIFY_STATE -ErrorAction SilentlyContinue
# Make optional upstream checks fail locally: installation must use bundled assets.
`$env:HTTPS_PROXY='http://127.0.0.1:1'
`$env:HTTP_PROXY='http://127.0.0.1:1'
`$env:NO_PROXY=''
"@
    $beforeFresh=TreeHash (Get-SptTouchedPaths $context)
    RunNormal ($prefix+@'

$m=Get-Module installer
& $m { Set-Item Function:script:New-SptShortcut { throw 'TEST injected shortcut failure' } }
$failed=$false
try { Invoke-SptInstall $c PAYLOAD } catch { $failed=$_.Exception.Message -like '*TEST injected shortcut failure*' }
if (!$failed) { throw 'Fresh install failure injection did not stop installation' }
'@.Replace('PAYLOAD',("'"+$payload.Replace("'","''")+"'"))) 'fresh-rollback'
    Check 'failed fresh install restores stock UI and absent helper/config' (($beforeFresh -ceq (TreeHash (Get-SptTouchedPaths $context))) -and !(Test-Path -LiteralPath $context.Install) -and !(Test-Path -LiteralPath $context.Config))
    RunNormal ($prefix+"`nAssert-SptUser`nInvoke-SptInstall `$c '$($payload.Replace("'","''"))'") 'fresh-install'
    Check 'fresh install with optional upstream requests unavailable' ([IO.File]::ReadAllText((Join-Path $OutputDirectory 'fresh-install.log')).Contains('Using local CSS map instead'))
    $token=[IO.File]::ReadAllText((Join-Path $context.Install 'bridge-token.txt'))
    Check 'fresh install creates private token' ($token -match '^[a-f0-9]{64}$')
    Check 'new token differs from this PC' ($token -ne [IO.File]::ReadAllText((Join-Path $real.Install 'bridge-token.txt')))
    Check 'desktop and Start menu shortcuts created' ((Test-Path -LiteralPath (Join-Path $context.Desktop 'Spotify + Pitch & Tempo.lnk')) -and (Test-Path -LiteralPath (Join-Path $context.Programs 'Remove Pitch Tempo FX.lnk')))
    Check 'bundled Spicetify installed alongside helper' (Test-Path -LiteralPath (Join-Path $context.Install 'spicetify\spicetify.exe'))
    RunNormal ($prefix+"`nInvoke-SptInstall `$c '$($payload.Replace("'","''"))'") 'repair-install'
    Check 'repair preserves private token' ($token -eq [IO.File]::ReadAllText((Join-Path $context.Install 'bridge-token.txt')))
    $before=TreeHash (Get-SptTouchedPaths $context)
    RunNormal ($prefix+@'

# Fail after a real UI apply, to exercise rollback of modified UI and installed files.
$m=Get-Module installer
& $m {
    $script:realSpice=(Get-Item Function:Invoke-SptSpice).ScriptBlock
    Set-Item Function:script:Invoke-SptSpice {
        param($Exe,$Arguments)
        & $script:realSpice $Exe $Arguments
        if ($Arguments -contains 'apply') { throw 'TEST injected apply failure' }
    }
}
$failed=$false
try { Invoke-SptInstall $c PAYLOAD } catch { $failed=$_.Exception.Message -like '*TEST injected apply failure*' }
if (!$failed) { throw 'Failure injection did not stop installation' }
'@.Replace('PAYLOAD',("'"+$payload.Replace("'","''")+"'"))) 'rollback'
    Check 'failed apply restores exact pre-operation files' ($before -ceq (TreeHash (Get-SptTouchedPaths $context)))
    # Ensure removal does not delete unrelated extension configuration/files.
    [IO.File]::WriteAllText((Join-Path $context.Config 'Extensions\test-other.js'),'// unrelated extension fixture')
    RunNormal ($prefix+@'

Invoke-SptSpice (Join-Path $c.Install 'spicetify\spicetify.exe') @('config','extensions','test-other.js')
Invoke-SptUninstall $c
'@) 'remove'
    Check 'uninstall removes helper and injected UI reference' (!(Test-Path -LiteralPath (Join-Path $context.Install 'SpotifyPitchTempo.exe')) -and !([IO.File]::ReadAllText((Join-Path $context.Spotify 'Apps\xpui\index.html')).Contains('pitch-tempo.js')))
    Check 'uninstall preserves other extensions' ((Test-Path -LiteralPath (Join-Path $context.Config 'Extensions\test-other.js')) -and (Get-SptIniValue (Join-Path $context.Config 'config-xpui.ini') 'AdditionalOptions' 'extensions') -eq 'test-other.js')
    Check 'uninstall preserves Spotify native files' ((Get-FileHash -LiteralPath (Join-Path $context.Spotify 'Spotify.dll') -Algorithm SHA256).Hash -eq '93BA5EE486E148114A1D6DB388CCE318B04D2A011376FF6301F5EE0DEA432A12')
    RunNormal ($prefix+"`nInvoke-SptInstall `$c '$($payload.Replace("'","''"))'") 'reinstall-with-other-extension'
    Check 'install preserves existing extension configuration and deploys it' ((Get-SptIniValue (Join-Path $context.Config 'config-xpui.ini') 'AdditionalOptions' 'extensions').Contains('test-other.js') -and [IO.File]::ReadAllText((Join-Path $context.Spotify 'Apps\xpui\index.html')).Contains('test-other.js'))
    $beforeRemove=TreeHash (Get-SptTouchedPaths $context)
    RunNormal ($prefix+@'

$m=Get-Module installer
& $m {
    $script:realSpice=(Get-Item Function:Invoke-SptSpice).ScriptBlock
    Set-Item Function:script:Invoke-SptSpice {
        param($Exe,$Arguments)
        & $script:realSpice $Exe $Arguments
        if ($Arguments -contains 'apply') { throw 'TEST injected remove failure' }
    }
}
$failed=$false
try { Invoke-SptUninstall $c } catch { $failed=$_.Exception.Message -like '*TEST injected remove failure*' }
if (!$failed) { throw 'Remove failure injection did not stop removal' }
'@) 'remove-rollback'
    Check 'failed removal restores exact pre-operation files' ($beforeRemove -ceq (TreeHash (Get-SptTouchedPaths $context)))
    $template=Join-Path $payload 'extension\pitch-tempo.js'
    [IO.File]::AppendAllText($template,'// tampered')
    Check 'corrupted payload rejected before install' (Rejected { Test-SptPayload $payload })
    Check 'real installed helper/config/UI untouched by sandbox tests' ($beforeReal -ceq (TreeHash @($real.Install,$real.Config,(Join-Path $real.Spotify 'Apps'))))
} finally {
    $script:checks | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'checks.json') -Encoding UTF8
    Write-Output "Evidence: $OutputDirectory"
    Write-Output "Scratch retained for inspection: $sandbox"
}
