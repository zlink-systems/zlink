[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))

function Get-RepositoryRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $rootPrefix = $RepositoryRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the repository: $fullPath"
    }
    return $fullPath.Substring($rootPrefix.Length)
}

function Get-RepositoryFiles {
    param([Parameter(Mandatory = $true)][string[]]$RelativePaths)

    $files = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
    foreach ($relativePath in $RelativePaths) {
        $path = Join-Path $RepositoryRoot $relativePath
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files.Add((Get-Item -LiteralPath $path))
            continue
        }
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            throw "Windows contract path was not found: $relativePath"
        }
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File -Filter "*.ps1") {
            $files.Add($file)
        }
    }
    return @($files | Sort-Object FullName -Unique)
}

function Assert-SourceMatch {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $path = Join-Path $RepositoryRoot $RelativePath
    $source = Get-Content -LiteralPath $path -Raw
    if ($source -notmatch $Pattern) {
        throw "${Message}: $RelativePath"
    }
}

function Get-CanonicalVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Key
    )

    $path = Join-Path $RepositoryRoot $RelativePath
    $matches = @(Select-String -LiteralPath $path -Pattern ("^" + [regex]::Escape($Key) + "=([0-9]+\.[0-9]+\.[0-9]+)$"))
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Key=X.Y.Z in $RelativePath"
    }
    return $matches[0].Matches[0].Groups[1].Value
}

$parseFiles = Get-RepositoryFiles -RelativePaths @(
    "core/builds/windows",
    "scripts/local-package",
    "scripts/gate/rebuild-dev.ps1",
    "framework/languages/cpp/build-windows.ps1",
    "framework/languages/cpp/samples",
    "framework/languages/dotnet/build-windows.ps1",
    "framework/languages/dotnet/samples",
    "framework/languages/java/build-windows.ps1",
    "framework/languages/java/samples",
    "framework/languages/node/build-windows.ps1",
    "framework/languages/node/samples"
)

$syntaxFailures = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $parseFiles) {
    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $file.FullName,
        [ref]$tokens,
        [ref]$parseErrors)
    foreach ($parseError in @($parseErrors)) {
        $relative = Get-RepositoryRelativePath -Path $file.FullName
        $syntaxFailures.Add(
            "$relative`:$($parseError.Extent.StartLineNumber): $($parseError.Message)")
    }
}
if ($syntaxFailures.Count -ne 0) {
    throw "PowerShell syntax validation failed:`n$($syntaxFailures -join [Environment]::NewLine)"
}

$sampleContracts = @(
    [PSCustomObject]@{ Language = "cpp"; Root = "framework/languages/cpp/samples"; ShellExpected = 7; PowerShellExpected = 7 },
    [PSCustomObject]@{ Language = "dotnet"; Root = "framework/languages/dotnet/samples"; ShellExpected = 7; PowerShellExpected = 7 },
    [PSCustomObject]@{ Language = "java"; Root = "framework/languages/java/samples/java"; ShellExpected = 7; PowerShellExpected = 6 },
    [PSCustomObject]@{ Language = "kotlin"; Root = "framework/languages/java/samples/kotlin"; ShellExpected = 7; PowerShellExpected = 6 },
    [PSCustomObject]@{ Language = "node"; Root = "framework/languages/node/samples"; ShellExpected = 7; PowerShellExpected = 7 }
)

foreach ($contract in $sampleContracts) {
    $sampleRoot = Join-Path $RepositoryRoot $contract.Root
    $shellRunners = @(Get-ChildItem -LiteralPath $sampleRoot -Recurse -File -Filter "run_sample.sh")
    $powerShellRunners = @(Get-ChildItem -LiteralPath $sampleRoot -Recurse -File -Filter "run_sample.ps1")
    if ($shellRunners.Count -ne $contract.ShellExpected -or $powerShellRunners.Count -ne $contract.PowerShellExpected) {
        throw "$($contract.Language) sample inventory changed; expected shell=$($contract.ShellExpected), Windows=$($contract.PowerShellExpected), found shell=$($shellRunners.Count), Windows=$($powerShellRunners.Count)"
    }
    foreach ($powerShellRunner in $powerShellRunners) {
        if (-not (Test-Path -LiteralPath (Join-Path $powerShellRunner.DirectoryName "run_sample.sh") -PathType Leaf)) {
            throw "Shell sample runner is missing beside $(Get-RepositoryRelativePath -Path $powerShellRunner.FullName)"
        }
    }
}

$cppVersion = Get-CanonicalVersion -RelativePath "bindings/cpp/VERSION" -Key "ZLINK_BINDING_VERSION"
$dotnetVersion = Get-CanonicalVersion -RelativePath "bindings/dotnet/VERSION" -Key "ZLINK_BINDING_VERSION"
$javaVersion = Get-CanonicalVersion -RelativePath "bindings/java/VERSION" -Key "ZLINK_BINDING_VERSION"
$nodeVersion = Get-CanonicalVersion -RelativePath "bindings/node/VERSION" -Key "ZLINK_BINDING_VERSION"

Assert-SourceMatch -RelativePath "bindings/cpp/CMakeLists.txt" `
    -Pattern ("project\(zlink_cpp VERSION " + [regex]::Escape($cppVersion) + " LANGUAGES CXX\)") `
    -Message "C++ package version does not match bindings/cpp/VERSION"
Assert-SourceMatch -RelativePath "bindings/dotnet/src/Zlink/Zlink.csproj" `
    -Pattern "ZLinkBindingVersionFile.*\.\.\/\.\.\/VERSION" `
    -Message ".NET package must read bindings/dotnet/VERSION"
Assert-SourceMatch -RelativePath "bindings/dotnet/src/Zlink/Zlink.csproj" `
    -Pattern "(?s)ZLinkWindowsX64NativeRoot.*runtimes\\win-x64\\native\\zlink\.dll" `
    -Message ".NET Windows payload must use win-x64"
Assert-SourceMatch -RelativePath "bindings/java/build.gradle" `
    -Pattern ("(?m)^version = '" + [regex]::Escape($javaVersion) + "'$") `
    -Message "Java package version does not match bindings/java/VERSION"
Assert-SourceMatch -RelativePath "scripts/local-package/java/windows-package.init.gradle" `
    -Pattern "native/windows-x86_64/zlink\.dll" `
    -Message "Java Windows resource must use windows-x86_64"
Assert-SourceMatch -RelativePath "bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java" `
    -Pattern '(?s)a\.equals\("amd64"\) \|\| a\.equals\("x86_64"\).*return "x86_64"' `
    -Message "Java x64 loader mapping is missing"
Assert-SourceMatch -RelativePath "bindings/node/package.json" `
    -Pattern ('"version"\s*:\s*"' + [regex]::Escape($nodeVersion) + '"') `
    -Message "Node package version does not match bindings/node/VERSION"
Assert-SourceMatch -RelativePath "bindings/node/src/zlink/runtime/native/native_load_paths.ts" `
    -Pattern '\$\{process\.platform\}-\$\{process\.arch\}' `
    -Message "Node prebuild path must derive from the native platform and architecture"
Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern "prebuilds\\win32-x64" `
    -Message "Node Windows package output must use win32-x64"
Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern '-A", "x64"' `
    -Message "C++ Windows package generator must use x64"

Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern ([regex]::Escape('install\zlink-cpp\$bindingVersion')) `
    -Message "C++ package output must be versioned"
Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern ([regex]::Escape('Zlink.$bindingVersion.nupkg')) `
    -Message ".NET package output must be versioned"
Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern ([regex]::Escape('systems\zlink\zlink\$bindingVersion\zlink-$bindingVersion.jar')) `
    -Message "Java package output must be versioned"
Assert-SourceMatch -RelativePath "scripts/local-package/build-windows.ps1" `
    -Pattern ([regex]::Escape('zlink-systems-zlink-$bindingVersion.tgz')) `
    -Message "Node package output must be versioned"

Write-Output "Windows x64 static contract passed: $($parseFiles.Count) PowerShell files, 33 Windows sample runners, C++/$cppVersion .NET/$dotnetVersion Java/$javaVersion Node/${nodeVersion}."
