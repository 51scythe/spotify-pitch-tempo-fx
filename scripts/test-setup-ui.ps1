param([Parameter(Mandatory=$true)][string]$SetupExe,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory=Join-Path $root ('reports\setup-ui-'+(Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory.' }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$exe=[IO.Path]::GetFullPath($SetupExe)
$pidFile=Join-Path $OutputDirectory 'pid.txt'
$launch=Join-Path $OutputDirectory 'launch.ps1'
[IO.File]::WriteAllText($launch,"(Start-Process -FilePath '$($exe.Replace("'","''"))' -WindowStyle Normal -PassThru).Id | Set-Content -LiteralPath '$($pidFile.Replace("'","''"))'")
& (Join-Path $root 'build\launch-user.exe') "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $launch
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SetupCapture {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h,uint msg,IntPtr w,StringBuilder text);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder text,int count);
}
'@
$deadline=(Get-Date).AddSeconds(20)
while (!(Test-Path -LiteralPath $pidFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
$process=Get-Process -Id ([int](Get-Content -LiteralPath $pidFile))
do { $process.Refresh(); if ($process.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 } while ((Get-Date) -lt $deadline)
$window=[Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
function Elements { $window.FindAll([Windows.Automation.TreeScope]::Descendants,[Windows.Automation.Condition]::TrueCondition) }
function Click($Name) {
    $button=Elements | Where-Object {$_.Current.Name -eq $Name} | Select-Object -First 1
    if (!$button) { throw "Button not found: $Name" }
    [void][SetupCapture]::PostMessage([IntPtr]$button.Current.NativeWindowHandle,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
}
function ReadOutput {
    foreach ($element in (Elements)) {
        $h=[IntPtr]$element.Current.NativeWindowHandle
        $class=New-Object Text.StringBuilder 256
        [void][SetupCapture]::GetClassName($h,$class,256)
        if ($class.ToString() -like '*EDIT*') {
            $text=New-Object Text.StringBuilder 65536
            [void][SetupCapture]::SendMessage($h,0xD,[IntPtr]65536,$text)
            return $text.ToString()
        }
    }
    throw 'Setup log textbox not found'
}
function WaitText($Text) {
    $deadline=(Get-Date).AddSeconds(45)
    do {
        if (@(Elements | Where-Object {$_.Current.Name -eq $Text}).Count) { return }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "UI did not reach: $Text"
}
function Capture($Name) {
    $r=New-Object SetupCapture+Rect
    [void][SetupCapture]::GetWindowRect($process.MainWindowHandle,[ref]$r)
    $bitmap=New-Object Drawing.Bitmap(($r.Right-$r.Left),($r.Bottom-$r.Top))
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $dc=$graphics.GetHdc()
    try { [void][SetupCapture]::PrintWindow($process.MainWindowHandle,$dc,2) } finally { $graphics.ReleaseHdc($dc) }
    try { $bitmap.Save((Join-Path $OutputDirectory ($Name+'.png'))) } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    Capture 'ready'
    Click 'Check compatibility'
    WaitText 'Check completed successfully.'
    Capture 'check-passed'
    $text=ReadOutput
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'check-output.txt'),$text)
    if (!$text.Contains('CHECK PASSED.')) { throw 'UI success did not include the compatibility result' }
    # Exercise the real entry-point guard without touching the live installation.
    if (Get-Process Spotify,SpotifyPitchTempo -ErrorAction SilentlyContinue) {
        Click 'Install / Repair'
        WaitText 'Install stopped. See the reason in the log below.'
        Capture 'running-spotify-blocked'
        $text=ReadOutput
        if (!$text.Contains('Quit Spotify completely')) { throw 'Expected running-Spotify rejection was absent' }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'blocked-output.txt'),$text)
    }
    'PASS: normal-user GUI, compatibility check, status/log rendering and running-app guard'
} finally { Click 'Close' }
Write-Output "UI evidence: $OutputDirectory"
