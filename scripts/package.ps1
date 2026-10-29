param([string]$Version='1.0.0-fx3', [string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory=Join-Path $root "dist\$Version" }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists. Choose a new release directory.' }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$scratch=Join-Path $root ('build\package-'+[Guid]::NewGuid().ToString('N'))
$payload=Join-Path $scratch 'payload'
$spiceSource=Join-Path $root 'vendor\spicetify-cli-2.46.4-source.zip'
if ((Get-FileHash -LiteralPath $spiceSource -Algorithm SHA256).Hash -ne '9BAA895EEBB1223B8BD1924B1A171A00B1B41D6AC0500BA1BDEA0089EFB75ABF') { throw 'Spicetify source archive hash mismatch.' }
New-Item -ItemType Directory -Path $OutputDirectory,$payload,(Join-Path $payload 'bin'),(Join-Path $payload 'extension'),(Join-Path $payload 'licenses'),(Join-Path $payload 'third-party-source') -Force | Out-Null
try {
    foreach ($name in @('SpotifyPitchTempo.exe','spotify-pitch-tempo.dll')) { Copy-Item -LiteralPath (Join-Path $root "build\$name") -Destination (Join-Path $payload "bin\$name") }
    Copy-Item -LiteralPath (Join-Path $root 'src\pitch-tempo.js') -Destination (Join-Path $payload 'extension\pitch-tempo.js')
    Copy-Item -LiteralPath (Join-Path $root 'vendor\spicetify-2.46.4') -Destination (Join-Path $payload 'spicetify') -Recurse
    Copy-Item -LiteralPath $spiceSource -Destination (Join-Path $payload 'third-party-source\spicetify-cli-2.46.4-source.zip')
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead($spiceSource)
    try {
        $entry=$zip.GetEntry('cli-2.46.4/LICENSE')
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $payload 'licenses\Spicetify-LGPL-2.1.txt'))
    } finally { $zip.Dispose() }
    $licenses=@{'minhook\LICENSE.txt'='MinHook.txt';'signalsmith-stretch\LICENSE.txt'='Signalsmith-Stretch.txt';'signalsmith-linear\LICENSE.txt'='Signalsmith-Linear.txt'}
    foreach ($key in $licenses.Keys) { Copy-Item -LiteralPath (Join-Path $root "vendor\$key") -Destination (Join-Path $payload ('licenses\'+$licenses[$key])) }
    foreach ($name in @('setup.ps1','installer.psm1','Remove.cmd','START-HERE.txt')) { Copy-Item -LiteralPath (Join-Path $root "packaging\$name") -Destination (Join-Path $payload $name) }
    Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination $payload
    $files=@(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{path=$_.FullName.Substring($payload.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    [pscustomobject]@{release=$Version;protocol=3;spotifyVersion='1.3.6.1921';spotifyDllSha256='93BA5EE486E148114A1D6DB388CCE318B04D2A011376FF6301F5EE0DEA432A12';files=$files} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $payload 'manifest.json') -Encoding UTF8
    Import-Module (Join-Path $root 'packaging\installer.psm1') -Force -DisableNameChecking
    Test-SptPayload $payload
    $payloadZip=Join-Path $scratch 'payload.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($payload,$payloadZip,[IO.Compression.CompressionLevel]::Optimal,$false)
    $exe=Join-Path $OutputDirectory 'Spotify-Pitch-Tempo-FX-Setup.exe'
    $compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:winexe /platform:x64 /optimize+ "/out:$exe" "/win32manifest:$root\packaging\setup.manifest" "/resource:$payloadZip,payload.zip" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll (Join-Path $root 'packaging\Setup.cs')
    if ($LASTEXITCODE) { throw 'Setup compilation failed.' }
    Copy-Item -LiteralPath (Join-Path $payload 'START-HERE.txt') -Destination $OutputDirectory
    # A clean rebuild kit makes recovery possible after losing the development PC.
    $source=Join-Path $scratch 'source'
    New-Item -ItemType Directory -Path $source,(Join-Path $source 'vendor') -Force | Out-Null
    foreach ($name in @('src','scripts','packaging')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination (Join-Path $source $name) -Recurse }
    foreach ($name in @('README.md','THIRD-PARTY-NOTICES.md')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination $source }
    foreach ($name in @('minhook','signalsmith-stretch','signalsmith-linear','spicetify-2.46.4')) { Copy-Item -LiteralPath (Join-Path $root "vendor\$name") -Destination (Join-Path $source "vendor\$name") -Recurse }
    Get-ChildItem -LiteralPath $source -Recurse -Force | Where-Object {$_.Name -eq '.git' -or $_.Name -eq '__pycache__'} | Remove-Item -Recurse -Force
    Copy-Item -LiteralPath $spiceSource -Destination (Join-Path $source 'vendor')
    [IO.Compression.ZipFile]::CreateFromDirectory($source,(Join-Path $OutputDirectory 'Spotify-Pitch-Tempo-FX-Source.zip'),[IO.Compression.CompressionLevel]::Optimal,$false)
    Get-ChildItem -LiteralPath $OutputDirectory -File | Sort-Object Name | ForEach-Object {
        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash+'  '+$_.Name
    } | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ASCII
    Write-Output "Release created: $OutputDirectory"
} finally {
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
