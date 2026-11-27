[CmdletBinding()]
param(
    [string]$ReleaseDirectory,
    [string]$OutputDirectory,
    [string]$BaseMod
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path $PSScriptRoot -Parent
if (!$ReleaseDirectory) { $ReleaseDirectory = Join-Path $root 'build/windows/x64/release' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'dist' }
$release = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
$output = [IO.Directory]::CreateDirectory($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)).FullName
$dll = Join-Path $release 'YsmBedrock.dll'
if (!(Test-Path -LiteralPath $dll -PathType Leaf)) { throw "Missing $dll. Build the localized source first." }
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($dll))
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'YsmBedrock.dll is not a Windows PE binary' }
    $reader.BaseStream.Position = 0x3c
    $reader.BaseStream.Position = $reader.ReadInt32()
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'YsmBedrock.dll must be a Windows x64 binary' }
} finally { $reader.Dispose() }
$version = (Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json).version
$name = "YsmBedrock-$version-localized"
$stage = Join-Path ([IO.Path]::GetTempPath()) ('ysm-localized-' + [Guid]::NewGuid().ToString('N'))
$mod = Join-Path $stage 'YsmBedrock'
[void][IO.Directory]::CreateDirectory($mod)
$base = $null
function Write-Archive([string]$destination, [string]$prefix) {
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
    $archive = [IO.Compression.ZipFile]::Open($destination, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $mod -File -Recurse -Force) {
            $entry = $prefix + $file.FullName.Substring($mod.Length+1).Replace('\','/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$entry,[IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
}
try {
    if ($BaseMod) { $base = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $BaseMod).Path) }
    Copy-Item -LiteralPath $dll,(Join-Path $root 'manifest.json'),(Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $mod
    foreach ($dir in @('resource_packs','licenses')) { Copy-Item -LiteralPath (Join-Path $root $dir) -Destination $mod -Recurse }
    $legacy = Join-Path $release 'YsmLegacyV3.dll'
    if (Test-Path -LiteralPath $legacy -PathType Leaf) { Copy-Item -LiteralPath $legacy -Destination $mod }
    elseif ($base) {
        $entry = $base.GetEntry('YsmLegacyV3.dll')
        if ($entry) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $mod 'YsmLegacyV3.dll')) }
    }
    if (!(Test-Path -LiteralPath (Join-Path $mod 'YsmLegacyV3.dll'))) { Write-Warning 'Legacy v3 .ysm support is unavailable without YsmLegacyV3.dll. Other supported formats are unaffected.' }
    foreach ($recordName in @('wine-fox-source.json','magical-wine-fox-source.json')) {
        $record = Get-Content -LiteralPath (Join-Path $root "licenses/$recordName") -Raw -Encoding UTF8 | ConvertFrom-Json
        $model = Join-Path $root "assets/$($record.filename)"
        $models = Join-Path $mod 'models'
        $destination = Join-Path $models $record.filename
        if (Test-Path -LiteralPath $model -PathType Leaf) {
            [void][IO.Directory]::CreateDirectory($models)
            Copy-Item -LiteralPath $model -Destination $destination
        } elseif ($base) {
            $entry = $base.GetEntry("models/$($record.filename)")
            if ($entry) {
                [void][IO.Directory]::CreateDirectory($models)
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$destination)
            }
        }
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $record.sha256) { throw "Bundled model checksum mismatch: $($record.filename)" }
        } else { Write-Warning "Optional bundled model not included: $($record.filename)" }
    }
    Write-Archive (Join-Path $output "$name.mcmod") ''
    Write-Archive (Join-Path $output "$name-BedrockBoot.zip") 'YsmBedrock/'
    Get-Item -LiteralPath (Join-Path $output "$name.mcmod"),(Join-Path $output "$name-BedrockBoot.zip") | Select-Object FullName,Length
} finally {
    if ($base) { $base.Dispose() }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
