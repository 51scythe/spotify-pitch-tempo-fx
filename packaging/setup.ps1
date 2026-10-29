param([ValidateSet('Check','Install','Remove')][string]$Action='Check')
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$logDir=Join-Path $env:LOCALAPPDATA 'SpotifyPitchTempo-setup-logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$log=Join-Path $logDir ((Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Action+'.log')
Start-Transcript -LiteralPath $log | Out-Null
$code=0
$lock=New-Object Threading.Mutex($false,'Local\SpotifyPitchTempo-Setup')
$locked=$false
try {
    try { $locked=$lock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked=$true }
    if (!$locked) { throw 'Another FX setup operation is running. Wait for it to finish.' }
    Import-Module (Join-Path $PSScriptRoot 'installer.psm1') -Force -DisableNameChecking
    Assert-SptUser
    $context=Get-SptContext
    switch ($Action) {
        'Check' { Test-SptPayload $PSScriptRoot; Assert-SptSpotify $context; Write-Output 'CHECK PASSED. Quit Spotify before installing. This checks compatibility, not live audio support.' }
        'Install' { Assert-SptStopped; Invoke-SptInstall $context $PSScriptRoot }
        'Remove' { Assert-SptStopped; Invoke-SptUninstall $context }
    }
} catch {
    Write-Output ('ERROR: '+$_.Exception.Message)
    $code=1
} finally {
    if ($locked) { $lock.ReleaseMutex() }
    $lock.Dispose()
    Write-Output "Log: $log"
    Stop-Transcript | Out-Null
}
exit $code
