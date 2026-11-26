@echo off
setlocal
for /f "tokens=2 delims=:" %%c in ('chcp') do set "zlink_previous_cp=%%c"
chcp 65001 >nul
where pwsh >nul 2>nul
if errorlevel 1 (
  echo PowerShell 7 ^(pwsh^) is required: https://aka.ms/powershell 1>&2
  chcp %zlink_previous_cp% >nul
  exit /b 1
)
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-e2e.ps1" %*
set "zlink_result=%errorlevel%"
chcp %zlink_previous_cp% >nul
exit /b %zlink_result%
