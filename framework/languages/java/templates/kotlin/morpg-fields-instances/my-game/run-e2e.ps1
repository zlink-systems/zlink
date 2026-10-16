param([string]$Scenario = '', [switch]$CheckStructure)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force .logs | Out-Null
    & ./gradlew.bat --no-daemon installDist :e2e:javaHome *> .logs/build.log
    if ($LASTEXITCODE -ne 0) { Get-Content .logs/build.log -Tail 35; exit 1 }
    $javaHomePath = (Get-Content .logs/java-home.txt -Raw).Trim()
    $arguments = @()
    if ($CheckStructure) { $arguments = @('--check-structure') }
    elseif ($Scenario) { $arguments = @('--scenario', $Scenario) }
    & "$javaHomePath/bin/java.exe" --enable-native-access=ALL-UNNAMED '-Dstdout.encoding=UTF-8' '-Dstderr.encoding=UTF-8' -cp 'e2e/build/install/e2e/lib/*' MyGame.e2e.ProgramKt @arguments
    exit $LASTEXITCODE
} finally { Pop-Location }
