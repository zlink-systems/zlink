$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../../samples/sample_runner.ps1')

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('zlink-local-nuget-test-' + [Guid]::NewGuid().ToString('N'))
$previousRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
$previousCache = $env:NUGET_PACKAGES
$validate = ${function:Assert-ZlinkLocalNuGetAssets}
try {
    $feed = Join-Path $testRoot 'nuget'
    $cache = Join-Path $testRoot 'cache'
    $projectDirectory = Join-Path $testRoot 'project with spaces'
    $project = Join-Path $projectDirectory 'Test.csproj'
    $packageDirectory = Join-Path $cache 'zlink/1.0.0'
    $nativeRelative = 'runtimes/win-x64/native/zlink.dll'
    $native = Join-Path $packageDirectory $nativeRelative
    $outputDirectory = Join-Path $projectDirectory 'custom-output/Release/net8.0'
    $outputNative = Join-Path $outputDirectory $nativeRelative
    New-Item -ItemType Directory -Force -Path $feed, (Split-Path $native), (Split-Path $outputNative), (Join-Path $projectDirectory 'obj') | Out-Null
    [IO.File]::WriteAllText($project, '<Project />')
    $source = Join-Path $feed 'Zlink.1.0.0.nupkg'
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($source, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry($nativeRelative)
        $stream = $entry.Open()
        try { $stream.WriteByte(42) } finally { $stream.Dispose() }
    } finally { $zip.Dispose() }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageDirectory 'zlink.1.0.0.nupkg')
    [IO.File]::WriteAllBytes($native, [byte[]]@(42))
    [IO.File]::WriteAllBytes($outputNative, [byte[]]@(42))
    $assetsPath = Join-Path $projectDirectory 'obj/project.assets.json'
    $assets = @{
        packageFolders = @{ $cache = @{} }
        libraries = @{ 'Zlink/1.0.0' = @{ type = 'package'; path = 'zlink/1.0.0' } }
    }
    [IO.File]::WriteAllText($assetsPath, ($assets | ConvertTo-Json -Depth 5))
    Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache -OutputDirectory $outputDirectory

    function Expect-Rejection([scriptblock]$Action, [string]$Pattern) {
        try { & $Action } catch {
            if ($_.Exception.Message -notlike $Pattern) { throw }
            return
        }
        throw "Expected rejection: $Pattern"
    }
    [IO.File]::WriteAllBytes($native, [byte[]]@(43))
    Expect-Rejection { Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache } '*native library differs*'
    [IO.File]::WriteAllBytes($native, [byte[]]@(42))
    [IO.File]::WriteAllBytes($outputNative, [byte[]]@(43))
    Expect-Rejection { Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache -OutputDirectory $outputDirectory } '*native library differs*'
    [IO.File]::WriteAllBytes((Join-Path $packageDirectory 'zlink.1.0.0.nupkg'), [byte[]]@(43))
    Expect-Rejection { Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache } '*Restored package differs*'
    $assets.packageFolders = @{ (Join-Path $testRoot 'stale-global-cache') = @{} }
    [IO.File]::WriteAllText($assetsPath, ($assets | ConvertTo-Json -Depth 5))
    Expect-Rejection { Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache } '*outside its local-package cache*'
    $assets.packageFolders = @{}
    [IO.File]::WriteAllText($assetsPath, ($assets | ConvertTo-Json -Depth 5))
    Expect-Rejection { Assert-ZlinkLocalNuGetAssets -AssetsPath $assetsPath -Feed $feed -Cache $cache } '*outside its local-package cache*'

    # Mock only the external build and separately verified asset check. This
    # observes the environment passed to restore/build, without a native run.
    $script:observedCaches = @()
    $script:buildExitCode = 0
    $script:failCommand = 'restore'
    $script:evaluatedProjects = @()
    $script:verifiedOutputs = @()
    $script:copyLocal = 'true'
    function dotnet {
        $script:observedCaches += $env:NUGET_PACKAGES
        $global:LASTEXITCODE = if ($args[0] -eq $script:failCommand) { $script:buildExitCode } else { 0 }
        if ($args[0] -eq 'msbuild') {
            $script:evaluatedProjects += $args[1]
            @{ Properties = @{
                ProjectAssetsFile = $assetsPath
                TargetDir = $outputDirectory
                CopyLocalLockFileAssemblies = $script:copyLocal
            } } | ConvertTo-Json
        }
    }
    function Assert-ZlinkLocalNuGetAssets {
        param($AssetsPath, $Feed, $Cache, $OutputDirectory)
        if ($OutputDirectory) { $script:verifiedOutputs += $OutputDirectory }
    }
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $testRoot
    $env:NUGET_PACKAGES = 'stale-user-cache'
    Invoke-SampleDotnetBuild $project
    $firstCache = $script:observedCaches[0]
    Invoke-SampleDotnetBuild $project
    if ($firstCache -eq 'stale-user-cache' -or @($script:observedCaches | Where-Object { $_ -ne $firstCache }).Count) {
        throw 'Local package mode did not isolate a deterministic cache.'
    }
    if ($env:NUGET_PACKAGES -ne 'stale-user-cache') { throw 'Caller cache was not restored.' }
    $solution = Join-Path $testRoot 'Test.sln'
    [IO.File]::WriteAllText($solution, 'Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = "Test", "project with spaces\Test.csproj", "{00000000-0000-0000-0000-000000000001}"')
    Invoke-ZlinkDotnetBuild -Project $solution -LocalPackageRoot $testRoot -Configuration Release
    if ($script:evaluatedProjects[-1] -ne $project) { throw 'Solution did not evaluate its project.' }
    if ($script:verifiedOutputs[-1] -ne $outputDirectory) { throw 'Evaluated output directory was not verified.' }
    $outputCount = $script:verifiedOutputs.Count
    $script:copyLocal = 'false'
    Invoke-ZlinkDotnetBuild -Project $solution -LocalPackageRoot $testRoot -Configuration Release
    if ($script:verifiedOutputs.Count -ne $outputCount) { throw 'Library output incorrectly required native assets.' }
    [IO.File]::WriteAllBytes($source, [byte[]]@(44))
    Invoke-SampleDotnetBuild $project
    if ($script:observedCaches[-1] -eq $firstCache) { throw 'Changed package content reused the old cache.' }
    $script:buildExitCode = 1
    Expect-Rejection { Invoke-SampleDotnetBuild $project } '*dotnet restore failed*'
    if ($env:NUGET_PACKAGES -ne 'stale-user-cache') { throw 'Failure leaked the isolated cache.' }
    $script:failCommand = 'msbuild'
    Expect-Rejection { Invoke-SampleDotnetBuild $project } '*dotnet project evaluation failed*'
    $script:failCommand = 'build'
    Expect-Rejection { Invoke-SampleDotnetBuild $project } '*dotnet build failed*'
    if ($env:NUGET_PACKAGES -ne 'stale-user-cache' -or $env:ZLINK_LOCAL_PACKAGE_ROOT -ne $testRoot) {
        throw 'Build failure leaked the local package environment.'
    }
    Write-Host 'PASS local NuGet runner: valid assets, stale archive/native/output/cache rejection, content-addressed cache, solution/project outputs, library copy policy, environment restoration'
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousRoot
    $env:NUGET_PACKAGES = $previousCache
    ${function:Assert-ZlinkLocalNuGetAssets} = $validate
    Remove-Item Function:dotnet -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'zlink-local-nuget-test-*') {
        throw "Unsafe test cleanup path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
