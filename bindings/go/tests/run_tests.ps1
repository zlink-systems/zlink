$ErrorActionPreference = 'Stop'

function Invoke-Stage {
    param(
        [string] $Name,
        [scriptblock] $Action
    )

    Write-Host "==> $Name"
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Name exited with status $LASTEXITCODE"
    }
    $script:passed++
}

$passed = 0
$exitCode = 0
$oldDebug = $env:GODEBUG
$oldExperiment = $env:GOEXPERIMENT
Push-Location (Join-Path $PSScriptRoot '..')
try {
    $env:GODEBUG = (@($oldDebug, 'cgocheck=1') | Where-Object { $_ }) -join ','
    $env:GOEXPERIMENT = (@($oldExperiment, 'cgocheck2') | Where-Object { $_ }) -join ','

    Invoke-Stage 'go test all Go packages (cgo pointer checks enabled)' { go test ./... }
    Invoke-Stage 'go vet all Go packages' { go vet ./... }
    Invoke-Stage 'go test Go perf module' { go -C perf test ./... }
    Invoke-Stage 'go vet Go perf module' { go -C perf vet ./... }
    Invoke-Stage 'raw contract and hot-path guards' {
        go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory|EveryPackagedCoreExportHasAnOwner)' -count=1
    }
    Invoke-Stage 'sample smoke' {
        foreach ($sample in Get-ChildItem -Path samples -Directory -Filter '*_sample' | Sort-Object Name) {
            Write-Host "==> samples/$($sample.Name)"
            go run "./samples/$($sample.Name)"
            if ($LASTEXITCODE -ne 0) {
                throw "sample $($sample.Name) exited with status $LASTEXITCODE"
            }
        }
    }
    Write-Host "Go tests: passed ($passed stages)"
}
catch {
    [Console]::Error.WriteLine("Go tests: failed ($passed stages passed). $($_.Exception.Message)")
    $exitCode = 1
}
finally {
    Pop-Location
    $env:GODEBUG = $oldDebug
    $env:GOEXPERIMENT = $oldExperiment
}
exit $exitCode
