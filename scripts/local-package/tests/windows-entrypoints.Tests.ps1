[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") {
    throw "This contract test requires Windows PowerShell."
}

$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))

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

    $files = [Collections.Generic.List[IO.FileInfo]]::new()
    foreach ($relativePath in $RelativePaths) {
        $path = Join-Path $RepositoryRoot $relativePath
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files.Add((Get-Item -LiteralPath $path))
            continue
        }
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            throw "Windows entrypoint path was not found: $relativePath"
        }
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File -Filter "*.ps1") {
            $files.Add($file)
        }
    }
    return @($files | Sort-Object FullName -Unique)
}

$parseFiles = Get-RepositoryFiles -RelativePaths @(
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

$syntaxFailures = [Collections.Generic.List[string]]::new()
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
    throw "PowerShell 5 syntax validation failed:`n$($syntaxFailures -join [Environment]::NewLine)"
}

$sampleContracts = @(
    [PSCustomObject]@{ Language = "cpp"; Root = "framework/languages/cpp/samples"; Expected = 7 },
    [PSCustomObject]@{ Language = "dotnet"; Root = "framework/languages/dotnet/samples"; Expected = 7 },
    [PSCustomObject]@{ Language = "java"; Root = "framework/languages/java/samples/java"; Expected = 7 },
    [PSCustomObject]@{ Language = "kotlin"; Root = "framework/languages/java/samples/kotlin"; Expected = 7 },
    [PSCustomObject]@{ Language = "node"; Root = "framework/languages/node/samples"; Expected = 7 }
)

foreach ($contract in $sampleContracts) {
    $sampleRoot = Join-Path $RepositoryRoot $contract.Root
    $shellRunners = @(Get-ChildItem -LiteralPath $sampleRoot -Recurse -File -Filter "run_sample.sh")
    if ($shellRunners.Count -ne $contract.Expected) {
        throw "$($contract.Language) sample inventory contains $($shellRunners.Count) shell runners; expected $($contract.Expected)."
    }
    foreach ($shellRunner in $shellRunners) {
        $powerShellRunner = Join-Path $shellRunner.DirectoryName "run_sample.ps1"
        if (-not (Test-Path -LiteralPath $powerShellRunner -PathType Leaf)) {
            $relative = Get-RepositoryRelativePath -Path $shellRunner.FullName
            throw "Windows sample runner is missing beside $relative"
        }
    }

    $powerShellRunners = @(Get-ChildItem -LiteralPath $sampleRoot -Recurse -File -Filter "run_sample.ps1")
    if ($powerShellRunners.Count -ne $contract.Expected) {
        throw "$($contract.Language) sample inventory contains $($powerShellRunners.Count) Windows runners; expected $($contract.Expected)."
    }
    foreach ($powerShellRunner in $powerShellRunners) {
        $shellRunner = Join-Path $powerShellRunner.DirectoryName "run_sample.sh"
        if (-not (Test-Path -LiteralPath $shellRunner -PathType Leaf)) {
            $relative = Get-RepositoryRelativePath -Path $powerShellRunner.FullName
            throw "Shell sample runner is missing beside $relative"
        }
    }
}

foreach ($language in @("cpp", "dotnet", "java", "node")) {
    foreach ($entrypoint in @("build-windows.ps1", "samples/run_samples.ps1")) {
        $path = Join-Path $RepositoryRoot "framework/languages/$language/$entrypoint"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required Windows entrypoint was not found: framework/languages/$language/$entrypoint"
        }
    }
}

Write-Output "Windows entrypoint contract passed: $($parseFiles.Count) PowerShell files, 35 sample runners."
