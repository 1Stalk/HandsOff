<#
.SYNOPSIS
    Hyper-V multi-Windows E2E test matrix orchestrator for HandsOff (AI-agent friendly).

.DESCRIPTION
    For each VM configured in -MatrixConfig:
      1. Restores the VM to its clean snapshot (Restore-VMSnapshot).
      2. Starts the VM and waits for PowerShell Direct (Invoke-Command -VMName).
      3. Copies HandsOff.exe, HandsOff.dll, HandsOffTests.exe, and tests\e2e\run_e2e.ps1 into the VM.
      4. Executes run_e2e.ps1 inside the interactive desktop user session (Session 1) via a
         one-shot Scheduled Task (InteractiveToken) so real explorer.exe GUI dialogs can be tested.
      5. Pulls back results.json, screenshots, and any explorer.exe crash dumps (.dmp).
      6. Optionally runs cdb.exe on any collected crash dumps and writes matrix_summary.json.
#>
[CmdletBinding()]
param(
    [string]$MatrixConfig = "",
    [switch]$SkipBuild,
    [int]$VmTimeoutSec = 240
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
if ([string]::IsNullOrWhiteSpace($MatrixConfig)) {
    $MatrixConfig = Join-Path $scriptDir "vm-matrix.example.json"
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot "build.ps1") -Werror -Test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $MatrixConfig)) {
    throw "Matrix config not found: $MatrixConfig"
}

$config = Get-Content $MatrixConfig -Raw | ConvertFrom-Json
$matrixOut = Join-Path $repoRoot "artifacts\vm_matrix"
New-Item -ItemType Directory -Force -Path $matrixOut | Out-Null

$summary = [ordered]@{
    timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
    vms          = @()
    allPassed    = $true
}

$cdb = Get-Command cdb.exe -ErrorAction SilentlyContinue

foreach ($vm in $config.vms) {
    $vmName = $vm.vmName
    $snapName = $vm.snapshotName
    $user = $vm.username
    $passSec = ConvertTo-SecureString $vm.password -AsPlainText -Force
    $cred = New-Object System.Management.Automation.PSCredential($user, $passSec)

    $vmOutDir = Join-Path $matrixOut $vmName
    if (Test-Path $vmOutDir) { Remove-Item $vmOutDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $vmOutDir | Out-Null

    Write-Host "[Matrix] Restoring VM '$vmName' -> snapshot '$snapName'..." -ForegroundColor Cyan
    try {
        Stop-VM -Name $vmName -TurnOff -Force -ErrorAction SilentlyContinue
        Restore-VMSnapshot -VMName $vmName -Name $snapName -Confirm:$false
        Start-VM -Name $vmName

        $session = $null
        $readyDeadline = (Get-Date).AddSeconds(90)
        while ((Get-Date) -lt $readyDeadline) {
            try {
                $session = New-PSSession -VMName $vmName -Credential $cred -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Seconds 3
            }
        }
        if (-not $session) {
            throw "Timed out waiting for PowerShell Direct session on '$vmName'."
        }

        Invoke-Command -Session $session -ScriptBlock {
            New-Item -ItemType Directory -Force -Path "C:\HandsOffTest\out" | Out-Null
        }

        Copy-Item -ToSession $session -Path (Join-Path $repoRoot "HandsOff.exe") -Destination "C:\HandsOffTest\" -Force
        Copy-Item -ToSession $session -Path (Join-Path $repoRoot "HandsOff.dll") -Destination "C:\HandsOffTest\" -Force
        Copy-Item -ToSession $session -Path (Join-Path $repoRoot "tests\e2e\run_e2e.ps1") -Destination "C:\HandsOffTest\" -Force
        $testsBin = Join-Path $repoRoot "build\tests\HandsOffTests.exe"
        if (Test-Path $testsBin) {
            Copy-Item -ToSession $session -Path $testsBin -Destination "C:\HandsOffTest\" -Force
        }

        # Run inside the logged-on user's interactive desktop session (Session 1)
        Invoke-Command -Session $session -ArgumentList $user -ScriptBlock {
            param($TaskUser)
            $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -Command `"& 'C:\HandsOffTest\run_e2e.ps1' -BinDir 'C:\HandsOffTest' -OutputDir 'C:\HandsOffTest\out' -EnableCrashDumps; New-Item -Path 'C:\HandsOffTest\out\_done.flag' -ItemType File -Force | Out-Null`""
            $principal = New-ScheduledTaskPrincipal -UserId $TaskUser -LogonType Interactive -RunLevel Highest
            Register-ScheduledTask -TaskName "HandsOffE2E" -Action $action -Principal $principal -Force | Out-Null
            Start-ScheduledTask -TaskName "HandsOffE2E"
        }

        $done = $false
        $deadline = (Get-Date).AddSeconds($VmTimeoutSec)
        while ((Get-Date) -lt $deadline) {
            $done = Invoke-Command -Session $session -ScriptBlock { Test-Path "C:\HandsOffTest\out\_done.flag" }
            if ($done) { break }
            Start-Sleep -Seconds 3
        }
        if (-not $done) {
            throw "E2E run on '$vmName' did not finish within ${VmTimeoutSec}s."
        }

        Copy-Item -FromSession $session -Path "C:\HandsOffTest\out\*" -Destination $vmOutDir -Recurse -Force
        Remove-PSSession $session

        $resFile = Join-Path $vmOutDir "results.json"
        if (-not (Test-Path $resFile)) { throw "results.json missing from '$vmName'." }
        $vmResult = Get-Content $resFile -Raw | ConvertFrom-Json

        # Auto-analyze any explorer.exe crash dumps with cdb.exe if installed
        $dumpAnalyses = @()
        Get-ChildItem -Path $vmOutDir -Filter "*.dmp" -Recurse | ForEach-Object {
            if ($cdb) {
                $analysisPath = "$($_.FullName).analyze.txt"
                & $cdb.Source -z $_.FullName -y "$repoRoot\build" -c "!analyze -v; k; q" | Set-Content -Path $analysisPath -Encoding utf8
                $dumpAnalyses += $analysisPath
            }
        }

        $summary.vms += [ordered]@{
            vmName       = $vmName
            snapshot     = $snapName
            passed       = [bool]$vmResult.passed
            os           = $vmResult.os
            cases        = $vmResult.cases
            crashes      = $vmResult.explorerCrashes
            dumpAnalyses = $dumpAnalyses
            reportPath   = $resFile
        }
        if (-not $vmResult.passed) { $summary.allPassed = $false }
    } catch {
        $summary.allPassed = $false
        $summary.vms += [ordered]@{
            vmName = $vmName
            passed = $false
            error  = $_.Exception.Message
        }
    } finally {
        Stop-VM -Name $vmName -TurnOff -Force -ErrorAction SilentlyContinue
    }
}

$summaryPath = Join-Path $matrixOut "matrix_summary.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding utf8
Write-Host "[Matrix] Summary written to: $summaryPath" -ForegroundColor Cyan

if (-not $summary.allPassed) { exit 1 }
exit 0
