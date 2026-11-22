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

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $SkipBuild) {
    & (Join-Path $repoRoot "build.ps1") -Werror -Test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Get-Command WindowsSandbox.exe -ErrorAction SilentlyContinue)) {
    throw "WindowsSandbox.exe not found. Enable the 'Windows Sandbox' optional feature to run Sandbox E2E tests."
}

function Stop-ExistingSandbox {
    foreach ($name in @("WindowsSandboxClient", "WindowsSandbox")) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            [void]$_.CloseMainWindow()
        }
    }
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name "vmmemWindowsSandbox", "WindowsSandboxClient", "WindowsSandbox" -ErrorAction SilentlyContinue)) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    Get-Process -Name "WindowsSandbox", "WindowsSandboxClient", "WindowsSandboxServer" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

Stop-ExistingSandbox

$runDir = Join-Path $repoRoot "artifacts\sandbox_e2e"
if (Test-Path $runDir) { Remove-Item $runDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$sandboxOutHost = Join-Path $env:TEMP ("HandsOffSandboxOut_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $sandboxOutHost | Out-Null

$bootPs1 = @"
"boot_started $(Get-Date -Format o)" | Add-Content -Path 'C:\HandsOffOut\progress.log'
Start-Transcript -Path 'C:\HandsOffOut\sandbox_boot.log' -Force | Out-Null
try {
    Start-Sleep -Seconds 5
    "running_e2e $(Get-Date -Format o)" | Add-Content -Path 'C:\HandsOffOut\progress.log'
    & 'C:\HandsOffRepo\tests\e2e\run_e2e.ps1' -BinDir 'C:\HandsOffRepo' -OutputDir 'C:\HandsOffOut' -EnableCrashDumps
    "e2e_finished exit=$LASTEXITCODE $(Get-Date -Format o)" | Add-Content -Path 'C:\HandsOffOut\progress.log'
} catch {
    `$_ | Out-String | Add-Content -Path 'C:\HandsOffOut\sandbox_boot_error.txt'
} finally {
    Stop-Transcript | Out-Null
    New-Item -Path 'C:\HandsOffOut\_done.flag' -ItemType File -Force | Out-Null
}
"@
$bootPs1 | Set-Content -Path (Join-Path $sandboxOutHost "boot.ps1") -Encoding utf8

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
      <HostFolder>$sandboxOutHost</HostFolder>
      <SandboxFolder>C:\HandsOffOut</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\HandsOffOut\boot.ps1</Command>
  </LogonCommand>
</Configuration>
"@

$wsbPath = Join-Path $runDir "handsoff_e2e.wsb"
$wsbXml | Set-Content -Path $wsbPath -Encoding utf8

Write-Host "[HandsOff Sandbox] Launching Windows Sandbox..." -ForegroundColor Cyan
$proc = Start-Process WindowsSandbox.exe -ArgumentList "`"$wsbPath`"" -PassThru

$doneFlag = Join-Path $sandboxOutHost "_done.flag"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $doneFlag) { break }
    Start-Sleep -Seconds 2
}

if ($proc -and -not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
Stop-ExistingSandbox

Copy-Item -Path (Join-Path $sandboxOutHost "*") -Destination $runDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path $sandboxOutHost -Recurse -Force -ErrorAction SilentlyContinue

$resultsJson = Join-Path $runDir "results.json"

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
