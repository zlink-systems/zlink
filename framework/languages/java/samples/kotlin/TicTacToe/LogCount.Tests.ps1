$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$tokens = $null
$errors = $null
$runner = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'run_sample.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Runner parse failed.' }
$function = $runner.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Wait-LogCount'
}, $false)
Invoke-Expression $function.Extent.Text

# Exercise the production counter without waiting between fixture checks.
function Start-Sleep { param([int]$Milliseconds) }
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('zlink-log-count-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    $playA = Join-Path $fixture 'play-a.log'
    $playB = Join-Path $fixture 'play-b.log'
    $logs = Join-Path $fixture 'play-*.log'
    $evidence = 'tictactoe-lifecycle actor-bound actor=player-x'
    Wait-LogCount $logs $evidence 0
    Set-Content -LiteralPath $playA -Value 'tictactoe-ready'
    Set-Content -LiteralPath $playB -Value $evidence
    Wait-LogCount $playA 'tictactoe-ready' 1
    Wait-LogCount $logs $evidence 1
    Write-Host 'PASS exact path and evidence across Play logs'

    foreach ($negative in @('missing', 'duplicate')) {
        if ($negative -eq 'missing') {
            Set-Content -LiteralPath $playB -Value 'unrelated'
        } else {
            Set-Content -LiteralPath $playA -Value $evidence
            Set-Content -LiteralPath $playB -Value $evidence
        }
        $rejected = $false
        try { Wait-LogCount $logs $evidence 1 }
        catch [Management.Automation.RuntimeException] {
            if ($_.Exception.Message -notlike 'Timed out waiting for*') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "$negative evidence was accepted." }
        Write-Host "PASS rejected $negative evidence"
    }
} finally {
    Remove-Item -LiteralPath $playA, $playB -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixture -Force
}
