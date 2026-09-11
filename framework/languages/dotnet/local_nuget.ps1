# Shared by the Windows build and sample runners. Keep local package identity
# and verification in one place so a same-version rebuild cannot use old DLLs.
function Invoke-ZlinkDotnetBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Project,
        [string]$LocalPackageRoot,
        [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug'
    )

    $previousPackages = $env:NUGET_PACKAGES
    $previousRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
    try {
        if ($LocalPackageRoot) {
            $LocalPackageRoot = (Resolve-Path -LiteralPath $LocalPackageRoot).Path
            $feed = Join-Path $LocalPackageRoot 'nuget'
            $packages = @(Get-ChildItem -LiteralPath $feed -Filter '*.nupkg' -File | Sort-Object Name)
            if ($packages.Count -eq 0) { throw "No local NuGet packages found in $feed" }
            $digestInput = ($packages | ForEach-Object {
                $_.Name + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }) -join "`n"
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($digestInput))).Replace('-', '').ToLowerInvariant()
            } finally { $sha.Dispose() }
            # Keep paths short even when the feed lives in a long Issue worktree.
            # NuGet owns synchronization for concurrent restores into this cache.
            $cacheRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'zlink/nuget'
            $env:NUGET_PACKAGES = Join-Path $cacheRoot $digest.Substring(0, 16)
            $env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot
            & dotnet restore $Project --force --verbosity quiet "-p:Configuration=$Configuration"
            if ($LASTEXITCODE -ne 0) { throw "dotnet restore failed for $Project" }

            $projectPaths = @([IO.Path]::GetFullPath($Project))
            if ([IO.Path]::GetExtension($Project) -eq '.sln') {
                $solutionDirectory = Split-Path -Parent $projectPaths[0]
                $projectPaths = @(Get-Content -LiteralPath $Project | ForEach-Object {
                    if ($_ -match '^Project\("[^"]+"\)\s*=\s*"[^"]+",\s*"([^"]+\.csproj)"') {
                        [IO.Path]::GetFullPath((Join-Path $solutionDirectory $Matches[1]))
                    }
                })
                if ($projectPaths.Count -eq 0) { throw "No C# projects found in solution: $Project" }
            }
            $evaluations = @(foreach ($projectPath in $projectPaths) {
                # Ask MSBuild for its actual paths and native-copy policy instead
                # of assuming Debug/net8.0 or that libraries copy runtime assets.
                $json = & dotnet msbuild $projectPath "-p:Configuration=$Configuration" -getProperty:ProjectAssetsFile,TargetDir,CopyLocalLockFileAssemblies
                if ($LASTEXITCODE -ne 0) { throw "dotnet project evaluation failed for $projectPath" }
                $properties = ($json -join [Environment]::NewLine | ConvertFrom-Json).Properties
                Assert-ZlinkLocalNuGetAssets -AssetsPath $properties.ProjectAssetsFile -Feed $feed -Cache $env:NUGET_PACKAGES
                $properties
            })
            & dotnet build $Project --configuration $Configuration --no-restore --maxcpucount:1 --nologo --verbosity minimal
        } else {
            & dotnet build $Project --configuration $Configuration --maxcpucount:1 --nologo --verbosity minimal
        }
        if ($LASTEXITCODE -ne 0) { throw "dotnet build failed for $Project" }
        if ($LocalPackageRoot) {
            foreach ($properties in $evaluations) {
                $outputDirectory = if ($properties.CopyLocalLockFileAssemblies -eq 'true') { $properties.TargetDir } else { $null }
                Assert-ZlinkLocalNuGetAssets -AssetsPath $properties.ProjectAssetsFile -Feed $feed -Cache $env:NUGET_PACKAGES -OutputDirectory $outputDirectory
            }
        }
    } finally {
        $env:NUGET_PACKAGES = $previousPackages
        $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousRoot
    }
}

function Assert-ZlinkLocalNuGetAssets {
    param(
        [Parameter(Mandatory = $true)][string]$AssetsPath,
        [Parameter(Mandatory = $true)][string]$Feed,
        [Parameter(Mandatory = $true)][string]$Cache,
        [string]$OutputDirectory
    )

    $assets = Get-Content -Raw -LiteralPath $AssetsPath | ConvertFrom-Json
    $cachePath = [IO.Path]::GetFullPath($Cache).TrimEnd('\', '/')
    $folders = @($assets.packageFolders.PSObject.Properties)
    if ($folders.Count -eq 0 -or @($folders | Where-Object {
        [IO.Path]::GetFullPath($_.Name).TrimEnd('\', '/') -ne $cachePath
    }).Count -ne 0) {
        throw "Restore used a package folder outside its local-package cache: $AssetsPath"
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    foreach ($library in $assets.libraries.PSObject.Properties) {
        if ($library.Value.type -ne 'package' -or $library.Name -notmatch '^Zlink(?:\.[^/]*)?/(.+)$') { continue }
        $id, $version = $library.Name.Split('/')
        $packageName = "$id.$version.nupkg"
        $source = Join-Path $Feed $packageName
        $extracted = Join-Path $Cache $library.Value.path
        $cached = Join-Path $extracted $packageName.ToLowerInvariant()
        if (-not (Test-Path -LiteralPath $source) -or -not (Test-Path -LiteralPath $cached)) {
            throw "Missing required local package or restored archive: $packageName"
        }
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash) {
            throw "Restored package differs from local feed: $packageName"
        }
        $archive = [IO.Compression.ZipFile]::OpenRead($source)
        try {
            foreach ($entry in $archive.Entries) {
                if ($entry.FullName -notmatch '^runtimes/win-x64/native/[^/]+$') { continue }
                $stream = $entry.Open()
                $sha = [Security.Cryptography.SHA256]::Create()
                try { $expected = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
                finally { $stream.Dispose(); $sha.Dispose() }
                $paths = @(Join-Path $extracted $entry.FullName)
                if ($OutputDirectory) { $paths += Join-Path $OutputDirectory $entry.FullName }
                foreach ($path in $paths) {
                    if (-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) {
                        throw "Native library differs from local package: $path"
                    }
                }
            }
        } finally { $archive.Dispose() }
    }
}
