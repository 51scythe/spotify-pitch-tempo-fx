param([Parameter(Mandatory=$true)][string]$Output)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
$graphics=[Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
    $bitmap.Save([IO.Path]::GetFullPath($Output))
} finally { $graphics.Dispose(); $bitmap.Dispose() }
