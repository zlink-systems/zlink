[CmdletBinding()]
param(
    [string]$Scenario = "all",
    [switch]$G4Child,
    [switch]$B8Child
)

Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
. "$PSScriptRoot/../../zoneworld-common.ps1"

# The common runner owns the Redis lifecycle, bind-checked Kotlin port allocation,
# Gradle build lock, and the complete ZoneWorld scenario ledger.
Invoke-ZlinkZoneWorldSample -Language Kotlin -SampleDir $PSScriptRoot `
    -RunnerPath $MyInvocation.MyCommand.Path -Scenario $Scenario `
    -G4Child:$G4Child -B8Child:$B8Child
