<#
.SYNOPSIS
    Builds HandsOff and runs the E2E suite inside a clean Windows Sandbox instance.

.DESCRIPTION
    Creates a temporary .wsb configuration that mounts the repository read-only and maps an
    output folder read-write, launches Windows Sandbox, executes tests\e2e\run_e2e.ps1 inside
    the Sandbox interactive desktop session, and waits for results.json.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [int]$TimeoutSec = 180
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $SkipBuild) {
    & (Join-Path $repoRoot "build.ps1") -Werror -Test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Get-Command WindowsSandbox.exe -ErrorAction SilentlyContinue)) {
    throw "WindowsSandbox.exe not found. Enable the 'Windows Sandbox' optional feature to run Sandbox E2E tests."
}

$runDir = Join-Path $repoRoot "artifacts\sandbox_e2e"
if (Test-Path $runDir) { Remove-Item $runDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$sandboxBootCmd = @"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Sleep -Seconds 3; & 'C:\HandsOffRepo\tests\e2e\run_e2e.ps1' -BinDir 'C:\HandsOffRepo' -OutputDir 'C:\HandsOffOut' -EnableCrashDumps; New-Item -Path 'C:\HandsOffOut\_done.flag' -ItemType File -Force | Out-Null"
"@.Trim()

$wsbXml = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Disable</Networking>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$repoRoot</HostFolder>
      <SandboxFolder>C:\HandsOffRepo</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$runDir</HostFolder>
      <SandboxFolder>C:\HandsOffOut</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>$sandboxBootCmd</Command>
  </LogonCommand>
</Configuration>
"@

$wsbPath = Join-Path $runDir "handsoff_e2e.wsb"
$wsbXml | Set-Content -Path $wsbPath -Encoding utf8

Write-Host "[HandsOff Sandbox] Launching Windows Sandbox..." -ForegroundColor Cyan
$proc = Start-Process WindowsSandbox.exe -ArgumentList "`"$wsbPath`"" -PassThru

$doneFlag = Join-Path $runDir "_done.flag"
$resultsJson = Join-Path $runDir "results.json"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $doneFlag) { break }
    Start-Sleep -Seconds 2
}

if ($proc -and -not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $resultsJson)) {
    throw "Windows Sandbox E2E did not produce results.json within ${TimeoutSec}s."
}

$report = Get-Content $resultsJson -Raw | ConvertFrom-Json
Write-Host "[HandsOff Sandbox] OS: $($report.os.productName) Build $($report.os.currentBuild).$($report.os.ubr) ($($report.os.culture))"
foreach ($c in $report.cases) {
    Write-Host "  - $($c.name): $($c.status) ($($c.details))"
}
if (-not $report.passed) {
    Write-Error "Sandbox E2E reported failures or explorer.exe crashes."
    exit 1
}
Write-Host "[HandsOff Sandbox] All Sandbox E2E cases passed!" -ForegroundColor Green
