param(
    [string] $Compiler = "",
    [string] $JsonInclude = "",
    [string] $OutputDir = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (!$Compiler) {
    $cmd = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($cmd) { $Compiler = $cmd.Source }
    else { $Compiler = Join-Path $env:ProgramFiles "LLVM\bin\clang++.exe" }
}
if (!$JsonInclude) {
    $packages = Join-Path $env:LOCALAPPDATA ".xmake\packages\n\nlohmann_json"
    $file = Get-ChildItem -LiteralPath $packages -Filter json.hpp -Recurse |
        Where-Object { $_.FullName -like "*\v3.12.0\*\include\nlohmann\json.hpp" } | Select-Object -First 1
    if (!$file) { throw "Pass -JsonInclude with the directory containing nlohmann/json.hpp." }
    $JsonInclude = $file.Directory.Parent.FullName
}
if (!$OutputDir) { $OutputDir = Join-Path $repo "build\verification\room-0.29.19" }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
[void](New-Item -ItemType Directory -Path $OutputDir -Force)
$testExe = Join-Path $OutputDir "room-protocol-tests.exe"
$log = Join-Path $OutputDir "protocol.txt"
$compilerVersion = (& $Compiler --version | Select-Object -First 1)
Push-Location -LiteralPath $repo
try {
    & $Compiler -std=c++20 -O1 -Isrc "-I$JsonInclude" tests/room_protocol_regression.cpp src/RoomProtocol.cpp src/MolangRoaming.cpp -o $testExe
    if ($LASTEXITCODE) { throw "Room protocol regression build failed ($LASTEXITCODE)." }
    @("Platform-independent room protocol regression, in-memory network and stores", "$compilerVersion", "Run UTC $([DateTime]::UtcNow.ToString('o'))") | Set-Content -Encoding UTF8 -LiteralPath $log
    & $testExe | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE) { throw "Room protocol regression failed ($LASTEXITCODE)." }
} finally { Pop-Location }
