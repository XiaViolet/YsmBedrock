param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$taskOriginalProcessorArchitecture = $env:PROCESSOR_ARCHITECTURE
$previousInspector = $env:YSM_TEST_INSPECTOR
if (-not $env:PROCESSOR_ARCHITECTURE) {
    $env:PROCESSOR_ARCHITECTURE = switch ([System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()) {
        'X64' { 'AMD64' }
        'Arm64' { 'ARM64' }
        'X86' { 'x86' }
        default { throw 'Unrecognized build process architecture' }
    }
}
Push-Location -LiteralPath $projectRoot
try {
    $env:PATH = 'C:\Program Files\LLVM\bin;' + $env:PATH
    & xmake f -P $projectRoot -p windows -a x64 -m release -y
    if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
    & xmake -P $projectRoot -y
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    $targetPaths = @(& xmake lua -P $projectRoot -c 'import("core.project.config"); config.load(); import("core.project.project"); for _,name in ipairs({"YsmBedrock","ysm-inspect"}) do print(project.target(name):targetfile()) end')
    if ($LASTEXITCODE -ne 0 -or $targetPaths.Count -ne 2) { throw 'Could not locate build outputs' }
    $release = [IO.Directory]::CreateDirectory((Join-Path $projectRoot 'build/windows/x64/release')).FullName
    foreach ($targetPath in $targetPaths) {
        $source = (Get-Item -LiteralPath $targetPath).FullName
        $destination = Join-Path $release ([IO.Path]::GetFileName($source))
        if ($source -ne $destination) { Copy-Item -LiteralPath $source -Destination $destination -Force }
    }
    if ($Test) {
        $env:YSM_TEST_INSPECTOR = Join-Path $release 'ysm-inspect.exe'
        Write-Output "Running regression tests with $env:YSM_TEST_INSPECTOR"
        & py -3 -m unittest discover -s tests -v
        if ($LASTEXITCODE -ne 0) { throw 'Regression checks failed' }
    }
} finally {
    $env:YSM_TEST_INSPECTOR = $previousInspector
    $env:PROCESSOR_ARCHITECTURE = $taskOriginalProcessorArchitecture
    Pop-Location
}
