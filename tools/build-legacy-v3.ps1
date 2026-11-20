param(
    [string]$CMake = 'cmake',
    [string]$NativeSource,
    [string]$Toolchain,
    [string]$JsonInclude
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (-not $NativeSource) { $NativeSource = Join-Path $project 'vendor/ysm-native-upstream' }
$NativeSource = (Resolve-Path -LiteralPath $NativeSource).Path
if (-not $Toolchain) {
    $Toolchain = (Get-ChildItem -LiteralPath "$NativeSource/build" -Filter conan_toolchain.cmake -File -Recurse |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $Toolchain) { throw 'Build the official native project dependencies first' }
if (-not $JsonInclude) {
    $JsonInclude = (Get-ChildItem -Path "$env:LOCALAPPDATA/.xmake/packages/n/nlohmann_json/*/*/include" -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'nlohmann/json.hpp') } | Select-Object -First 1).FullName
}
if (-not $JsonInclude) { throw 'Build the main xmake project first to obtain nlohmann/json' }
$out = Join-Path $project 'build/legacy-v3-bridge'
$configure = @('-S', (Join-Path $PSScriptRoot 'legacy_v3_bridge'), '-B', $out, '-G', 'Visual Studio 17 2022',
    '-A', 'x64', '-DTARGET_ARCH=X64', '-DYSM_DEPENDENCY_PROVIDER=conan', '-DCMAKE_CONFIGURATION_TYPES=Release',
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain", "-DYSM_NATIVE_SOURCE=$NativeSource", "-DYSM_JSON_INCLUDE=$JsonInclude")
& $CMake @configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMake --build $out --config Release --target YsmLegacyV3 --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -LiteralPath (Join-Path $out 'Release/YsmLegacyV3.dll') -Destination (Join-Path $project 'build/windows/x64/release') -Force
