param([string]$Output, [int]$ClickX=-1, [int]$ClickY=-1, [int]$Key=0, [int]$Wheel=0, [switch]$Foreground)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class SptWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int show);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref Point p);
}
'@
$process=Get-Process Spotify | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (!$process) { throw 'Spotify window not found' }
$h=$process.MainWindowHandle
if ($Foreground) { $null=[SptWindow]::ShowWindow($h,9); $null=[SptWindow]::SetForegroundWindow($h); Start-Sleep -Milliseconds 500 }
if ($ClickX -ge 0 -and $ClickY -ge 0) {
    $point=[IntPtr](($ClickY -shl 16) -bor $ClickX)
    $null=[SptWindow]::PostMessage($h,0x200,[IntPtr]::Zero,$point)
    $null=[SptWindow]::PostMessage($h,0x201,[IntPtr]1,$point)
    $null=[SptWindow]::PostMessage($h,0x202,[IntPtr]::Zero,$point)
    Start-Sleep -Milliseconds 500
}
if ($Key) {
    $null=[SptWindow]::PostMessage($h,0x100,[IntPtr]$Key,[IntPtr]1)
    $null=[SptWindow]::PostMessage($h,0x101,[IntPtr]$Key,[IntPtr]0xC0000001L)
    Start-Sleep -Milliseconds 500
}
if ($Wheel) {
    $pt=New-Object SptWindow+Point
    $bounds=New-Object SptWindow+Rect
    $null=[SptWindow]::GetClientRect($h,[ref]$bounds)
    $pt.X=[int]($bounds.Right/2); $pt.Y=[int]($bounds.Bottom/2)
    $null=[SptWindow]::ClientToScreen($h,[ref]$pt)
    $point=[IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xffff))
    $null=[SptWindow]::PostMessage($h,0x20A,[IntPtr]($Wheel -shl 16),$point)
    Start-Sleep -Milliseconds 500
}
if ($Output) {
    $rect=New-Object SptWindow+Rect
    $null=[SptWindow]::GetClientRect($h,[ref]$rect)
    $bitmap=New-Object Drawing.Bitmap($rect.Right,$rect.Bottom)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $dc=$graphics.GetHdc()
    try { $null=[SptWindow]::PrintWindow($h,$dc,3) } finally { $graphics.ReleaseHdc($dc) }
    try { $bitmap.Save([IO.Path]::GetFullPath($Output)) } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
