function Initialize-Console {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = $utf8
    $script:OutputEncoding = $utf8
}
