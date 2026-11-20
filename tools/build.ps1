param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$taskOriginalProcessorArchitecture = $env:PROCESSOR_ARCHITECTURE
if (-not $env:PROCESSOR_ARCHITECTURE) {
    $env:PROCESSOR_ARCHITECTURE = switch ([System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()) {
        'X64' { 'AMD64' }
        'Arm64' { 'ARM64' }
        'X86' { 'x86' }
        default { throw '无法识别构建进程的机器类型' }
    }
}
Push-Location -LiteralPath $projectRoot
try {
    $env:PATH = 'C:\Program Files\LLVM\bin;' + $env:PATH
    & xmake f -P $projectRoot -p windows -a x64 -m release -y
    if ($LASTEXITCODE -ne 0) { throw '配置失败' }
    & xmake -P $projectRoot -y
    if ($LASTEXITCODE -ne 0) { throw '构建失败' }
    if ($Test) {
        & py -3 -m unittest discover -s tests -v
        if ($LASTEXITCODE -ne 0) { throw '自动检查失败' }
    }
} finally {
    $env:PROCESSOR_ARCHITECTURE = $taskOriginalProcessorArchitecture
    Pop-Location
}
