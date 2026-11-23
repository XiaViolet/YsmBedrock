[CmdletBinding()]
param([string]$OutputDirectory, [string]$ReleaseDirectory, [string]$BundledModel, [switch]$MouseDiagnostics)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $projectRoot '../../导出' }
if (!$ReleaseDirectory) { $ReleaseDirectory = Join-Path $projectRoot 'build/windows/x64/release' }
$output = [IO.Directory]::CreateDirectory($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)).FullName
$release = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
$manifest = Join-Path $projectRoot 'manifest.json'
$version = (Get-Content -LiteralPath $manifest -Raw -Encoding utf8 | ConvertFrom-Json).version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid release version: $version" }
$name = "YsmBedrock-$version"
$bedrockBootArchive = "$name-BedrockBoot.zip"
$archives = @("$name.mcmod", "$name-source.zip", "$name-preview.zip", $bedrockBootArchive)
foreach ($file in $archives) {
    if (Test-Path -LiteralPath (Join-Path $output $file)) { throw "Output already exists: $file" }
}
foreach ($file in @('YsmBedrock.dll', 'YsmLegacyV3.dll', 'ysm-inspect.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $release $file) -PathType Leaf)) { throw "Missing build output: $file" }
}
$official = Get-Content -LiteralPath "$projectRoot/licenses/wine-fox-source.json" -Raw -Encoding utf8 | ConvertFrom-Json
if ($official.filename -cnotmatch '^ysm-wine-fox\.(zip|ysm)$' -or $official.sha256 -notmatch '^[a-f0-9]{64}$') {
    throw 'Invalid official Wine Fox release record'
}
if (!$BundledModel) {
    $BundledModel = Join-Path "$projectRoot/assets" $official.filename
    if (!(Test-Path -LiteralPath $BundledModel) -and (Split-Path $projectRoot -Leaf) -eq 'source') {
        $BundledModel = Join-Path "$projectRoot/../mods/YsmBedrock/models" $official.filename
    }
}

$magical = Get-Content -LiteralPath "$projectRoot/licenses/magical-wine-fox-source.json" -Raw -Encoding utf8 | ConvertFrom-Json
if ($magical.filename -cne 'ysm-magical-wine-fox.zip') { throw 'Invalid Magical Wine Fox release record' }
$magicalPath = Join-Path "$projectRoot/assets" $magical.filename
if (!(Test-Path -LiteralPath $magicalPath) -and (Split-Path $projectRoot -Leaf) -eq 'source') {
    $magicalPath = Join-Path "$projectRoot/../mods/YsmBedrock/models" $magical.filename
}
$models = @($official, $magical)
$stage = Join-Path $output ('.ysm-package-' + [Guid]::NewGuid().ToString('N'))
$bundle = Join-Path $stage "$name-preview"
$mod = Join-Path $bundle 'mods/YsmBedrock'
$source = Join-Path $bundle 'source'
$published = @()
function New-ReleaseArchive([string]$root,[string]$path,[string]$prefix,[switch]$SourceArchive) {
    # Windows PowerShell 5.1 can emit backslashes with ZipFile.CreateFromDirectory.
    $zip = [IO.Compression.ZipFile]::Open($path,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
            $entry = $prefix + $file.FullName.Substring($root.Length+1).Replace('\','/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file.FullName,$entry,[IO.Compression.CompressionLevel]::Optimal)
        }
        if ($SourceArchive) {
            # The engineering bundle already carries this file under mods/YsmBedrock/models.
            foreach ($model in $models) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path "$mod/models" $model.filename),
                ("assets/"+$model.filename),[IO.Compression.CompressionLevel]::Optimal)
            }
        }
    } finally { $zip.Dispose() }
}
try {
    foreach ($dir in @("$mod/models", $source, "$source/tests/fixtures", "$source/tools/legacy_v3_bridge")) {
        [void][IO.Directory]::CreateDirectory($dir)
    }
    Copy-Item -LiteralPath (Join-Path $release 'YsmBedrock.dll'), (Join-Path $release 'YsmLegacyV3.dll'), $manifest -Destination $mod
    foreach ($dir in @('resource_packs', 'licenses')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $dir) -Destination $mod -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $mod
    Copy-Item -LiteralPath $BundledModel -Destination (Join-Path "$mod/models" $official.filename)
    Copy-Item -LiteralPath $magicalPath -Destination (Join-Path "$mod/models" $magical.filename)
    foreach ($model in $models) {
        if ((Get-FileHash -LiteralPath (Join-Path "$mod/models" $model.filename)).Hash -ne $model.sha256) {
            throw "Bundled model SHA-256 mismatch: $($model.filename)"
        }
    }
    [IO.File]::WriteAllBytes((Join-Path $mod 'target-render-probe.enable'), [byte[]]@())
    if ($MouseDiagnostics) {
        [IO.File]::WriteAllBytes((Join-Path $mod 'mouse-diagnostics.enable'), [byte[]]@())
    }

    foreach ($item in @('src','docs','packages','licenses','resource_packs','xmake.lua','manifest.json','README.md','THIRD_PARTY_NOTICES.md','.gitignore')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $item) -Destination $source -Recurse -Force
    }
    foreach ($dir in @('tests','tools')) {
        Get-ChildItem -LiteralPath (Join-Path $projectRoot $dir) -File | Where-Object Extension -in '.cpp','.h','.ps1','.py' |
            Copy-Item -Destination (Join-Path $source $dir)
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'legacy_v3_bridge/bridge.cpp'), (Join-Path $PSScriptRoot 'legacy_v3_bridge/CMakeLists.txt') -Destination "$source/tools/legacy_v3_bridge"
    # Only the project's synthetic fixtures are redistributable with the tests.
    $fixtures = @('MeshCoordinateLab.zip','SignedCubeLab.zip','SignedCubeLabAllCutout.zip','SignedFaceLab.zip',
        'SignedFaceLabAllCutout.zip','UvLabBoneMirror.zip','UvLabBox.zip','UvLabBoxMirror.zip','UvLabCubeMirror.zip',
        'UvLabMissingSize.zip','UvLabPlain.zip','WorldInputs.zip','java_particle_registry_1_20_1.json',
        'java_particle_registry_1_21_1.json','java_surface_animation.json','native_bone_basis_26_60_05.json','source_rotation_reference.json')
    foreach ($file in $fixtures) { Copy-Item -LiteralPath "$projectRoot/tests/fixtures/$file" -Destination "$source/tests/fixtures" }
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File -Force) {
        if ($file.Name -in 'ysm.json','room-sync.json','bedrock-live.json','server-model-client.json' -or
            $file.FullName.Substring($source.Length+1) -match '(^|[\\/])(models|room-share|room-cache|\.git|__pycache__)([\\/]|$)') {
            throw "Unexpected runtime or private model content: $($file.Name)"
        }
        if ($file.Extension -in '.ysm','.mcmod','.exe','.dll','.pdb','.log','.pyc','.zip') {
            if ($file.DirectoryName -ne (Join-Path $source 'tests\fixtures') -or $file.Name -notin $fixtures) {
                throw "Unexpected file in release source: $($file.Name)"
            }
        }
    }
    Copy-Item -LiteralPath "$projectRoot/README.md","$PSScriptRoot/ysm_relay.py","$PSScriptRoot/install.ps1","$release/ysm-inspect.exe","$release/YsmLegacyV3.dll" -Destination $bundle
    @"
YsmBedrock $version
LeviLauncher：在对应实例中导入 $name.mcmod。
BedrockBoot：退出对应游戏实例，在模组管理中选择 LeviLamina，点击「导入模组」，直接选择 $bedrockBootArchive。
工程包：解压后退出该游戏实例，运行 .\install.ps1 -Instance '游戏实例目录'。
适用 Minecraft 1.26.60.05、LeviLamina 26.60.7、26.60.8 或 26.60.9 的 Windows 客户端。
Alt+Y 打开模型库并导入模型，Z 打开动作转盘。房间共享和验证范围见 README.md。
内置官方基础款酒狐和魔法酒狐。署名、来源及模型许可见 mods/YsmBedrock/licenses。
"@ | Set-Content -LiteralPath "$bundle/安装说明.txt" -Encoding utf8
    $checks = Join-Path $projectRoot "build/verification/release-$version"
    foreach ($file in @('protocol.txt','native-room-integration.txt','ui-test.txt','mouse-replay.txt','native-touch.txt','relay-render.txt','packaging.txt','official-model.json','regression.txt')) {
        if (Test-Path -LiteralPath (Join-Path $checks $file)) {
            [void][IO.Directory]::CreateDirectory("$bundle/verification")
            Copy-Item -LiteralPath (Join-Path $checks $file) -Destination "$bundle/verification"
        }
    }
    New-ReleaseArchive $mod (Join-Path $stage $archives[0]) ''
    # BedrockBoot extracts the ZIP into ll.mods and scans its immediate subdirectories.
    New-ReleaseArchive $mod (Join-Path $stage $bedrockBootArchive) 'YsmBedrock/'
    New-ReleaseArchive $source (Join-Path $stage $archives[1]) '' -SourceArchive
    New-ReleaseArchive $bundle (Join-Path $stage $archives[2]) "$name-preview/"
    foreach ($file in $archives) {
        $dest = Join-Path $output $file
        [IO.File]::Move((Join-Path $stage $file), $dest)
        $published += $dest
    }
} catch {
    foreach ($file in $published) { Remove-Item -LiteralPath $file -Force }
    throw
} finally {
    if ((Test-Path -LiteralPath $stage) -and ([IO.Directory]::GetParent((Resolve-Path -LiteralPath $stage).Path).FullName -eq $output)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
$published | ForEach-Object { Get-FileHash -LiteralPath $_ -Algorithm SHA256 }
