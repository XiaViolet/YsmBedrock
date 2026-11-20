param([Parameter(Mandatory=$true)][string]$ArtifactDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repo=Split-Path $PSScriptRoot -Parent
$version=(Get-Content -LiteralPath "$repo/manifest.json" -Raw -Encoding utf8 | ConvertFrom-Json).version
$base=Join-Path (Resolve-Path -LiteralPath $ArtifactDirectory).Path "YsmBedrock-$version"
function Check([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Entries([string]$path){
    $archive=[IO.Compression.ZipFile]::OpenRead($path)
    try {foreach($entry in $archive.Entries){$entry.FullName}} finally {$archive.Dispose()}
}
$official=Get-Content -LiteralPath "$repo/licenses/wine-fox-source.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$modelName=$official.filename
Check ($modelName -ceq 'ysm-wine-fox.zip') 'Unexpected official model release record'
function Check-Model([string]$path,[string]$entryName){
    $outer=[IO.Compression.ZipFile]::OpenRead($path)
    try {
        $model=$outer.GetEntry($entryName)
        Check ($null -ne $model) "Missing official model: $entryName"
        $stream=$model.Open();$hash=[Security.Cryptography.SHA256]::Create()
        try {$digest=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','')} finally {$stream.Dispose();$hash.Dispose()}
        Check ($digest -eq $official.sha256) "Official model changed: $entryName"
    } finally {$outer.Dispose()}
}
$mod=@(Entries "$base.mcmod")
$src=@(Entries "$base-source.zip")
$engineering=@(Entries "$base-preview.zip")
foreach($file in @('manifest.json','YsmBedrock.dll','YsmLegacyV3.dll','THIRD_PARTY_NOTICES.md','licenses/YSM-Apache-2.0.txt','resource_packs/ysm_particles/manifest.json','target-render-probe.enable')){
    Check ($file -cin $mod) "Missing install entry $file"
}
Check (@($mod -cmatch '^models/').Count -eq 1) 'Runtime must have exactly one bundled model'
Check (!(($mod -cne "models/$modelName") -match '\.(ysm|zip)$|^source/|^build/|^logs/|^room-(share|cache)/')) 'Unexpected private or development content in mcmod'
Check (@($src -match '^assets/').Count -eq 1) 'Source must have exactly one bundled model'
Check (@($engineering -match '/(assets|models)/[^/]+\.(zip|ysm)$').Count -eq 1) 'Engineering bundle contains duplicate or unrelated models'
Check-Model "$base.mcmod" "models/$modelName"
Check-Model "$base-source.zip" "assets/$modelName"
Check-Model "$base-preview.zip" "YsmBedrock-$version-preview/mods/YsmBedrock/models/$modelName"
foreach($file in @('.gitignore','README.md','THIRD_PARTY_NOTICES.md','licenses/YSM-Apache-2.0.txt','xmake.lua','src/Mod.cpp','tools/install.ps1','tools/legacy_v3_bridge/CMakeLists.txt','tests/fixtures/SignedFaceLab.zip')){
    Check ($file -cin $src) "Missing source entry $file"
}
Check (!($src -match '(^|/)(\.git|build|vendor|logs|models|room-cache|room-share|__pycache__)/|\.(ysm|exe|dll|log|pdb|pyc)$|(^|/)(ysm|bedrock-live|room-sync)\.json$')) 'Source archive contains runtime or personal data'
Write-Output 'PASS all three archives carry exactly one pinned official Wine Fox model; root manifest, DLLs, render flag, synthetic fixtures and licenses verified.'

$temp=Join-Path ([IO.Path]::GetTempPath()) ('ysm-install-regression-'+[Guid]::NewGuid().ToString('N'))
try {
    $instance=Join-Path $temp 'test instance'
    $modDir=Join-Path $instance 'mods/YsmBedrock'
    foreach($dir in @('.levilauncher','mods/LeviLamina')){[void][IO.Directory]::CreateDirectory((Join-Path $instance $dir))}
    [IO.File]::WriteAllBytes("$instance/Minecraft.Windows.exe",[byte[]]@())
    [IO.File]::WriteAllText("$instance/.levilauncher/instance.json",'{"gameVersion":"1.26.60.05"}')
    [IO.File]::WriteAllText("$instance/mods/LeviLamina/manifest.json",'{"version":"26.60.7"}')
    & "$repo/tools/install.ps1" -Instance $instance -Package "$base-preview.zip"
    Check (Test-Path -LiteralPath "$modDir/target-render-probe.enable" -PathType Leaf) 'Fresh install lost render flag'
    Check (@(Get-ChildItem -LiteralPath "$modDir/models" -Force).Count -eq 1) 'Fresh install has unexpected bundled assets'
    Check ((Get-FileHash -LiteralPath "$modDir/models/$modelName").Hash -eq $official.sha256) 'Official sample failed fresh install'
    $sentinels=@("models/$modelName",'models/private.ysm','room-share/shared.zip','room-cache/private/kept','room-sync.json','custom-preferences.json')
    $value='installation sentinel '+[Guid]::NewGuid().ToString()
    foreach($file in $sentinels){
        $path=Join-Path $modDir $file
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path,$value)
    }
    [IO.File]::WriteAllText("$modDir/target-render-probe.enable",$value)
    & "$repo/tools/install.ps1" -Instance $instance -Package "$base-preview.zip"
    Check ([IO.File]::ReadAllText("$modDir/target-render-probe.enable") -ceq $value) 'Update overwrote existing render flag'
    [IO.File]::Delete("$modDir/target-render-probe.enable")
    $expanded=Join-Path $temp expanded
    [IO.Compression.ZipFile]::ExtractToDirectory("$base-preview.zip",$expanded)
    $bundle=Join-Path $expanded "YsmBedrock-$version-preview"
    [IO.File]::WriteAllText("$modDir/YsmBedrock.dll",'old binary')
    & "$bundle/install.ps1" -Instance $instance
    Check (!(Test-Path -LiteralPath "$modDir/target-render-probe.enable")) 'Update re-enabled intentionally removed flag'
    Check ((Get-FileHash -LiteralPath "$modDir/YsmBedrock.dll").Hash -eq (Get-FileHash -LiteralPath "$bundle/mods/YsmBedrock/YsmBedrock.dll").Hash) 'DLL was not updated'
    foreach($file in $sentinels){Check ([IO.File]::ReadAllText((Join-Path $modDir $file)) -ceq $value) "Update overwrote $file"}
    $officialFiles=@(Get-ChildItem -LiteralPath "$modDir/models" -File | Where-Object {(Get-FileHash -LiteralPath $_.FullName).Hash -eq $official.sha256})
    Check ($officialFiles.Count -eq 1 -and $officialFiles[0].Name -cne $modelName) 'A conflicting personal filename hid or duplicated the official model'
    Check (!(Test-Path -LiteralPath "$modDir/resource_packs/resource_packs")) 'Nested resources after repeated updates'
    Write-Output 'PASS archive and extracted installers on an isolated Windows instance layout; repeated upgrades preserve private files, configuration, cache and render flag settings, and refresh the DLL.'
} finally {
    if ((Test-Path -LiteralPath $temp) -and ([IO.Directory]::GetParent((Resolve-Path -LiteralPath $temp).Path).FullName -eq [IO.Path]::GetTempPath().TrimEnd('\','/'))) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
