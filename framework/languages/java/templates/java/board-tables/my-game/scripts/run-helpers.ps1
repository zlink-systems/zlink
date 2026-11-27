$e2eClasspath = 'e2e/build/install/e2e/lib/*'

function Assert-PowerShellVersion {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
    # Java node output is UTF-8; consoles with a legacy code page (for example 949) would garble its symbols.
    if ($IsWindows) { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) }
}

function Build-Local {
    Write-Host 'Preparing: building the server (the first run downloads Gradle and dependencies and can take a few minutes)...'
    New-Item -ItemType Directory -Force '.logs' | Out-Null
    $gradlew = if ($IsWindows) { './gradlew.bat' } else { './gradlew' }
    & $gradlew --no-daemon --max-workers=4 installDist :e2e:javaHome *> '.logs/build.log'
    if ($LASTEXITCODE -ne 0) {
        Get-Content '.logs/build.log' -Tail 35 | Write-Host
        exit 1
    }
    Write-Host 'Build complete. Starting servers...'
}

function Get-LocalJava {
    if ((Test-Path '.logs/java-home.txt' -PathType Leaf) -and
        (Test-Path 'e2e/build/install/e2e/lib' -PathType Container)) {
        $javaHomePath = (Get-Content '.logs/java-home.txt' -Raw).Trim()
        $binary = if ($IsWindows) { 'bin/java.exe' } else { 'bin/java' }
        $java = Join-Path $javaHomePath $binary
        if (Test-Path -LiteralPath $java -PathType Leaf) { return $java }
    }
}

function Test-RunningServer {
    if (-not (Test-Path '.logs/server.json' -PathType Leaf)) { return $false }
    $java = Get-LocalJava
    if (-not $java) { return $false }
    & $java -cp "$e2eClasspath" MyGame.e2e.ServerHost --is-running | Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Assert-NoRunningServer {
    if (Test-RunningServer) {
        throw 'A server is already running. Stop it with Ctrl+C in its terminal before building.'
    }
}
