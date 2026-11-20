[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Instance, [string]$Package)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$instanceRoot = (Resolve-Path -LiteralPath $Instance).Path
$game = Join-Path $instanceRoot 'Minecraft.Windows.exe'
if (!(Test-Path -LiteralPath $game -PathType Leaf)) { throw '请选择含有 Minecraft.Windows.exe 的实例目录' }
$instanceInfo = Get-Content -LiteralPath "$instanceRoot/.levilauncher/instance.json" -Raw -Encoding utf8 | ConvertFrom-Json
$loaderInfo = Get-Content -LiteralPath "$instanceRoot/mods/LeviLamina/manifest.json" -Raw -Encoding utf8 | ConvertFrom-Json
if ($instanceInfo.gameVersion -ne '1.26.60.05' -or $loaderInfo.version -notin @('26.60.7','26.60.8')) {
    throw '适用 Minecraft 1.26.60.05 与 LeviLamina 26.60.7 或 26.60.8 的 Windows 客户端'
}
if (Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $game }) {
    throw '请先正常退出这个游戏实例'
}
$destination = [IO.Path]::GetFullPath((Join-Path $instanceRoot 'mods/YsmBedrock'))
$stage = Join-Path ([IO.Path]::GetTempPath()) ('ysm-install-' + [Guid]::NewGuid().ToString('N'))
$required = @('manifest.json','YsmBedrock.dll','YsmLegacyV3.dll','THIRD_PARTY_NOTICES.md')
function Test-ManagedFile([string]$name) {
    $name -cin $required -or $name -ceq 'target-render-probe.enable' -or $name -cmatch '^models/ysm-wine-fox\.(zip|ysm)$' -or
        $name.StartsWith('resource_packs/',[StringComparison]::Ordinal) -or $name.StartsWith('licenses/',[StringComparison]::Ordinal)
}
function Get-TargetPath([string]$base,[string]$name) {
    foreach ($part in $name.Split('/')) {
        if (!$part -or $part -match '[\\:*?"<>|\x00-\x1f]|[ .]$' -or $part -in '.','..') { throw "Invalid package path: $name" }
    }
    $path = [IO.Path]::GetFullPath((Join-Path $base $name))
    if (!$path.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)) { throw "Invalid destination: $name" }
    return $path
}
function Assert-NoLink([string]$path) {
    while ($path -and $path -ne $instanceRoot) {
        if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Install target is a link: $path"
        }
        $path = [IO.Path]::GetDirectoryName($path)
    }
}
try {
    $payload = Join-Path $stage 'payload'
    [void][IO.Directory]::CreateDirectory($payload)
    if ($Package) {
        $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
        try {
            $manifests = @($zip.Entries | Where-Object FullName -cmatch '^(?:[^/]+/)?mods/YsmBedrock/manifest\.json$')
            if ($manifests.Count -ne 1) { throw '请选择包含 mods/YsmBedrock 的工程 ZIP' }
            $prefix = $manifests[0].FullName.Substring(0,$manifests[0].FullName.Length-'manifest.json'.Length)
            $total = 0L
            foreach ($entry in $zip.Entries) {
                if (!$entry.Name -or !$entry.FullName.StartsWith($prefix,[StringComparison]::Ordinal)) { continue }
                $name = $entry.FullName.Substring($prefix.Length)
                if (!(Test-ManagedFile $name)) { continue }
                $path = Get-TargetPath $payload $name
                $total += $entry.Length
                if ($total -gt 256MB) { throw '工程包的安装文件超过 256 MiB' }
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$path,$false)
            }
        } finally { $zip.Dispose() }
    } else {
        $bundled = Join-Path $PSScriptRoot 'mods/YsmBedrock'
        $root = Split-Path $PSScriptRoot -Parent
        if (Test-Path -LiteralPath "$bundled/manifest.json" -PathType Leaf) {
            $content = $bundled
            $binaries = $bundled
            $modelDir = Join-Path $bundled 'models'
        } else {
            $content = $root
            $binaries = Join-Path $root 'build/windows/x64/release'
            $modelDir = Join-Path $root 'assets'
        }
        Copy-Item -LiteralPath "$binaries/YsmBedrock.dll","$binaries/YsmLegacyV3.dll","$content/manifest.json","$content/THIRD_PARTY_NOTICES.md" -Destination $payload
        Copy-Item -LiteralPath "$content/resource_packs","$content/licenses" -Destination $payload -Recurse
        $record = Get-Content -LiteralPath "$content/licenses/wine-fox-source.json" -Raw -Encoding utf8 | ConvertFrom-Json
        if ($record.filename -cnotmatch '^ysm-wine-fox\.(zip|ysm)$') { throw 'Invalid official model filename' }
        [void][IO.Directory]::CreateDirectory("$payload/models")
        Copy-Item -LiteralPath (Join-Path $modelDir $record.filename) -Destination "$payload/models"
        [IO.File]::WriteAllBytes("$payload/target-render-probe.enable",[byte[]]@())
    }
    foreach ($file in ($required + @('resource_packs/ysm_particles/manifest.json','licenses/YSM-Apache-2.0.txt','target-render-probe.enable'))) {
        if (!(Test-Path -LiteralPath (Join-Path $payload $file) -PathType Leaf)) { throw "Missing package file: $file" }
    }
    $manifest = Get-Content -LiteralPath "$payload/manifest.json" -Raw -Encoding utf8 | ConvertFrom-Json
    if ($manifest.name -cne 'YsmBedrock' -or $manifest.entry -cne 'YsmBedrock.dll' -or $manifest.platform -cne 'client') {
        throw '工程包的模组清单不匹配'
    }
    $record = Get-Content -LiteralPath "$payload/licenses/wine-fox-source.json" -Raw -Encoding utf8 | ConvertFrom-Json
    $models = @(Get-ChildItem -LiteralPath "$payload/models" -File)
    if ($models.Count -ne 1 -or $record.filename -cnotmatch '^ysm-wine-fox\.(zip|ysm)$' -or $models[0].Name -cne $record.filename -or
        (Get-FileHash -LiteralPath $models[0].FullName -Algorithm SHA256).Hash -ne $record.sha256) { throw '官方酒狐模型文件校验失败' }
    $newInstall = !(Test-Path -LiteralPath "$destination/manifest.json" -PathType Leaf)
    $copies = @(foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse -Force) {
        $name = $file.FullName.Substring($payload.Length+1).Replace('\','/')
        if (!(Test-ManagedFile $name) -or (!$newInstall -and $name -eq 'target-render-probe.enable')) { continue }
        $target = Get-TargetPath $destination $name
        Assert-NoLink $target
        $isModel = $name.StartsWith('models/',[StringComparison]::Ordinal)
        if ($isModel) {
            # Keep a personal model with the same filename and reuse an identical prior installation.
            $index = 0
            $reuse = $false
            while (Test-Path -LiteralPath $target) {
                if ([IO.File]::Exists($target) -and (Get-Item -LiteralPath $target).Length -eq $file.Length -and
                    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $record.sha256) { $reuse=$true; break }
                $index++
                $target = Get-TargetPath $destination ("models/" + $file.BaseName + "-official-" + $index + $file.Extension)
                Assert-NoLink $target
            }
            if ($reuse) { continue }
        }
        if ([IO.Directory]::Exists($target)) { throw "Target is a directory: $target" }
        [pscustomobject]@{ Target=$target; Source=$file.FullName; IsModel=$isModel; Backup=$null }
    })
    $changed = @()
    try {
        foreach ($item in $copies) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.Target))
            $temporary = $item.Target + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
            try {
                [IO.File]::Copy($item.Source,$temporary)
                if (!$item.IsModel -and [IO.File]::Exists($item.Target)) {
                    $item.Backup = $temporary + '.bak'
                    [IO.File]::Replace($temporary,$item.Target,$item.Backup)
                } else {
                    [IO.File]::Move($temporary,$item.Target)
                }
                $changed += $item
            } finally {
                if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
            }
        }
        [void][IO.Directory]::CreateDirectory("$destination/models")
    } catch {
        $originalError = $_
        [array]::Reverse($changed)
        foreach ($item in $changed) {
            if ($item.Backup) {
                [IO.File]::Copy($item.Backup,$item.Target,$true)
                [IO.File]::Delete($item.Backup)
            }
            elseif ([IO.File]::Exists($item.Target)) { [IO.File]::Delete($item.Target) }
        }
        throw $originalError
    }
    foreach ($item in $changed) { if ($item.Backup) { [IO.File]::Delete($item.Backup) } }
    Write-Output "已安装 YsmBedrock $($manifest.version) 到 $destination。"
} finally {
    if ((Test-Path -LiteralPath $stage) -and ([IO.Directory]::GetParent((Resolve-Path -LiteralPath $stage).Path).FullName -eq [IO.Path]::GetTempPath().TrimEnd('\','/'))) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
