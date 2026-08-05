param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$DotnetArgs
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..")
$project = Join-Path $rootDir "tests/Zlink.Tests/Zlink.Tests.csproj"

Write-Host "[dotnet-tests] running $project"
try {
    & dotnet test $project @DotnetArgs
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet test failed with exit code $LASTEXITCODE"
    }
    Write-Host "[dotnet-tests] PASS"
}
catch {
    Write-Host "[dotnet-tests] FAIL"
    throw
}
