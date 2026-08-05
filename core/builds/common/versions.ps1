# PowerShell version definitions for Windows builds

param()

$script:LIBZLINK_VERSION = "6.0.3"

# Export variables to caller scope
if ($MyInvocation.InvocationName -ne '.') {
    $global:LIBZLINK_VERSION = $script:LIBZLINK_VERSION
}

Write-Host "==================================="
Write-Host "Build Configuration"
Write-Host "==================================="
Write-Host "libzlink version:    $script:LIBZLINK_VERSION"
Write-Host "==================================="
