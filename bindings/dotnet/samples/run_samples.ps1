$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$repoRoot = Resolve-Path (Join-Path $root "../../..")
$versionFile = Join-Path $repoRoot "VERSION"
$coreVersion = (Select-String -Path $versionFile -Pattern "^LIBZLINK_VERSION=(.+)$").Matches[0].Groups[1].Value
$coreLib = Join-Path $repoRoot "core/build/lib/libzlink.so.$coreVersion"

if (Test-Path $coreLib) {
    $env:ZLINK_LIBRARY_PATH = $coreLib
}

$samples = @(
    "RequestReplyAsync/RequestReplyAsync.csproj"
    "PairRecv/PairRecv.csproj"
    "MonitorRecv/MonitorRecv.csproj"
    "PubSubRecv/PubSubRecv.csproj"
    "DealerRouterRecv/DealerRouterRecv.csproj"
    "StreamRecv/StreamRecv.csproj"
    "StreamPacketCallback/StreamPacketCallback.csproj"
)

$passed = 0
$failed = 0

foreach ($sample in $samples) {
    Write-Host "RUN,$sample"
    & dotnet run --project "$root/$sample"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "OK,$sample"
        $passed++
    }
    else {
        Write-Host "FAIL,$sample"
        $failed++
    }
}

Write-Host "SUMMARY,passed,$passed,failed,$failed,total,$($samples.Count)"
if ($failed -ne 0) {
    exit 1
}
