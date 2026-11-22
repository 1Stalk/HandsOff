<#
.SYNOPSIS
    End-to-end (E2E) test suite for HandsOff against the real Windows Explorer process.

.DESCRIPTION
    Runs in an interactive user desktop session (Session != 0) to verify:
      1. CTest unit/hook/prologue compatibility suite (HandsOffTests.exe).
      2. Hot-inject & hot-unload stability across multiple install/uninstall cycles.
      3. Real Explorer UI flows via UI Automation (File lock -> Unlock/Kill, Folder lock,
         and optional elevated lock via HandsOffSvc).
      4. explorer.exe crash detection (PID invariance, WER LocalDumps, Application Event Log).
#>
[CmdletBinding()]
param(
    [string]$BinDir = "",
    [string]$OutputDir = "",
    [switch]$SkipGuiTests,
    [switch]$EnableCrashDumps,
    [int]$DialogTimeoutSec = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$defaultRepoRoot = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
if ([string]::IsNullOrWhiteSpace($BinDir)) { $BinDir = $defaultRepoRoot }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $defaultRepoRoot "artifacts\e2e" }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$screenshotsDir = Join-Path $OutputDir "screenshots"
New-Item -ItemType Directory -Force -Path $screenshotsDir | Out-Null

$exePath = Join-Path $BinDir "HandsOff.exe"
$dllPath = Join-Path $BinDir "HandsOff.dll"
$testsExe = Join-Path $BinDir "build\tests\HandsOffTests.exe"
if (-not (Test-Path $testsExe)) {
    $altTests = Join-Path $BinDir "HandsOffTests.exe"
    if (Test-Path $altTests) { $testsExe = $altTests }
}

if (-not (Test-Path $exePath) -or -not (Test-Path $dllPath)) {
    throw "HandsOff.exe or HandsOff.dll not found in '$BinDir'."
}

$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)

if ($EnableCrashDumps -and $isElevated) {
    $werKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\explorer.exe"
    New-Item -Path $werKey -Force | Out-Null
    Set-ItemProperty -Path $werKey -Name "DumpFolder" -Value $OutputDir -Type ExpandString
    Set-ItemProperty -Path $werKey -Name "DumpCount" -Value 5 -Type DWord
    Set-ItemProperty -Path $werKey -Name "DumpType" -Value 2 -Type DWord
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$helperCode = @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Automation;

public static class HandsOffE2EHelper
{
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr childAfter, string className, string windowTitle);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left, Top, Right, Bottom;
    }

    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_CLOSE = 0x0010;

    public static string ReadWindowText(IntPtr hWnd)
    {
        var sb = new StringBuilder(512);
        GetWindowText(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static IntPtr FindVisibleWindowByClassForPids(string className, HashSet<int> pids)
    {
        IntPtr cur = IntPtr.Zero;
        while ((cur = FindWindowEx(IntPtr.Zero, cur, className, null)) != IntPtr.Zero)
        {
            if (!IsWindowVisible(cur)) continue;
            uint pid;
            GetWindowThreadProcessId(cur, out pid);
            if (pids == null || pids.Contains((int)pid))
            {
                return cur;
            }
        }
        return IntPtr.Zero;
    }

    public static bool CaptureWindowPng(IntPtr hWnd, string outputPath)
    {
        RECT rc;
        if (!GetWindowRect(hWnd, out rc)) return false;
        int w = rc.Right - rc.Left;
        int h = rc.Bottom - rc.Top;
        if (w <= 0 || h <= 0) return false;
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp))
        {
            g.CopyFromScreen(rc.Left, rc.Top, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            bmp.Save(outputPath, ImageFormat.Png);
        }
        return true;
    }

    public static void ClickChildBarRightButton(IntPtr hwndBar)
    {
        RECT rc;
        if (!GetClientRect(hwndBar, out rc)) return;
        int w = rc.Right - rc.Left;
        int h = rc.Bottom - rc.Top;
        int x = Math.Max(4, w - 24);
        int y = Math.Max(4, h / 2);
        IntPtr lParam = (IntPtr)((y << 16) | (x & 0xFFFF));
        SendMessage(hwndBar, WM_LBUTTONDOWN, (IntPtr)1, lParam);
        Thread.Sleep(25);
        SendMessage(hwndBar, WM_LBUTTONUP, IntPtr.Zero, lParam);
    }

    public static List<string> CollectUiaTexts(IntPtr hWnd)
    {
        var list = new List<string>();
        try
        {
            var root = AutomationElement.FromHandle(hWnd);
            if (root == null) return list;
            var all = root.FindAll(TreeScope.Descendants, Condition.TrueCondition);
            foreach (AutomationElement el in all)
            {
                string name = el.Current.Name;
                if (!string.IsNullOrWhiteSpace(name))
                {
                    list.Add(name);
                }
            }
        }
        catch { }
        return list;
    }

    public static bool TryInvokeConfirmYesIfPresent(HashSet<int> explorerPids)
    {
        IntPtr cur = IntPtr.Zero;
        while ((cur = FindWindowEx(IntPtr.Zero, cur, "#32770", null)) != IntPtr.Zero)
        {
            if (!IsWindowVisible(cur)) continue;
            uint pid;
            GetWindowThreadProcessId(cur, out pid);
            if (!explorerPids.Contains((int)pid)) continue;
            try
            {
                var root = AutomationElement.FromHandle(cur);
                var buttons = root.FindAll(
                    TreeScope.Descendants,
                    new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
                foreach (AutomationElement btn in buttons)
                {
                    string name = (btn.Current.Name ?? "").Trim();
                    if (name.Equals("Yes", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("Да", StringComparison.OrdinalIgnoreCase))
                    {
                        object pat;
                        if (btn.TryGetCurrentPattern(InvokePattern.Pattern, out pat))
                        {
                            ((InvokePattern)pat).Invoke();
                            return true;
                        }
                    }
                }
            }
            catch { }
        }
        return false;
    }
}
"@

Add-Type -TypeDefinition $helperCode -ReferencedAssemblies @("System.Drawing", "System.Windows.Forms", "UIAutomationClient", "UIAutomationTypes")

function Get-ExplorerState {
    $procs = @(Get-Process -Name explorer -ErrorAction SilentlyContinue)
    $pids = @($procs | ForEach-Object { $_.Id } | Sort-Object)
    $loadedDllPids = @()
    foreach ($p in $procs) {
        try {
            $hasDll = $p.Modules | Where-Object { $_.ModuleName -ieq "HandsOff.dll" }
            if ($hasDll) { $loadedDllPids += $p.Id }
        } catch {
            # Protected/different-session Explorer module enumeration may throw; ignore
        }
    }
    return [PSCustomObject]@{
        Pids          = $pids
        LoadedDllPids = $loadedDllPids
    }
}

function Get-RecentExplorerCrashEvents {
    param([datetime]$Since)
    $events = @()
    try {
        $found = Get-WinEvent -FilterHashtable @{
            LogName   = "Application"
            StartTime = $Since
        } -ErrorAction SilentlyContinue | Where-Object {
            ($_.Id -eq 1000 -or $_.Id -eq 1001) -and ($_.Message -match "explorer\.exe")
        }
        if ($found) { $events = @($found | ForEach-Object { "$($_.TimeCreated): ID=$($_.Id) $($_.Message)" }) }
    } catch {}
    return $events
}

$osInfo = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
$testRunStart = Get-Date
$initialDumps = @(Get-ChildItem -Path "$env:LOCALAPPDATA\CrashDumps" -Filter "explorer.exe*.dmp" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
$baselineExplorer = Get-ExplorerState

$results = [ordered]@{
    timestampUtc   = (Get-Date).ToUniversalTime().ToString("o")
    os             = [ordered]@{
        productName    = $osInfo.ProductName
        displayVersion = $osInfo.DisplayVersion
        currentBuild   = $osInfo.CurrentBuild
        ubr            = $osInfo.UBR
        editionId      = $osInfo.EditionID
        culture        = [System.Globalization.CultureInfo]::CurrentUICulture.Name
        sessionId      = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
        isElevated     = $isElevated
    }
    baselineExplorerPids = $baselineExplorer.Pids
    cases          = @()
    explorerCrashes = @()
    passed         = $false
}

function Add-CaseResult {
    param([string]$Name, [string]$Status, [string]$Details, [double]$DurationSec, [string]$Screenshot = "")
    $results.cases += [ordered]@{
        name        = $Name
        status      = $Status
        details     = $Details
        durationSec = [math]::Round($DurationSec, 2)
        screenshot  = $Screenshot
    }
    $color = switch ($Status) {
        "passed"  { "Green" }
        "skipped" { "Yellow" }
        default   { "Red" }
    }
    Write-Host ("[{0}] {1} ({2}s) - {3}" -f $Status.ToUpper(), $Name, [math]::Round($DurationSec, 2), $Details) -ForegroundColor $color
}

function Assert-ExplorerHealthy {
    param([int[]]$ExpectedPids, [string]$CaseName)
    $nowState = Get-ExplorerState
    $missing = @($ExpectedPids | Where-Object { $nowState.Pids -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "explorer.exe PID(s) [$($missing -join ',')] disappeared during '$CaseName' (possible crash/restart)!"
    }
}

# 1. Unit / LockFinder / Prologue compatibility test suite
if (Test-Path $testsExe) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $out = & $testsExe 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "Exit code $LASTEXITCODE`n$out" }
        Add-CaseResult -Name "unit_and_hook_compat" -Status "passed" -Details ($out.Trim() -replace "`r?`n", " | ") -DurationSec $sw.Elapsed.TotalSeconds
    } catch {
        Add-CaseResult -Name "unit_and_hook_compat" -Status "failed" -Details $_.Exception.Message -DurationSec $sw.Elapsed.TotalSeconds
    }
} else {
    Add-CaseResult -Name "unit_and_hook_compat" -Status "skipped" -Details "HandsOffTests.exe not present at $testsExe" -DurationSec 0
}

# 2. Hot-inject / Hot-unload stability cycle
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    for ($cycle = 1; $cycle -le 3; $cycle++) {
        & $exePath install | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Cycle $cycle install failed with exit $LASTEXITCODE" }
        Start-Sleep -Milliseconds 350
        $stateLoaded = Get-ExplorerState
        if ($stateLoaded.LoadedDllPids.Count -eq 0) {
            throw "Cycle ${cycle}: HandsOff.dll was not found inside explorer.exe after install."
        }
        Assert-ExplorerHealthy -ExpectedPids $baselineExplorer.Pids -CaseName "install_cycle_$cycle"

        & $exePath uninstall | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Cycle $cycle uninstall failed with exit $LASTEXITCODE" }
        Start-Sleep -Milliseconds 400
        $stateUnloaded = Get-ExplorerState
        if ($stateUnloaded.LoadedDllPids.Count -gt 0) {
            throw "Cycle ${cycle}: HandsOff.dll remained loaded in explorer.exe PIDs [$($stateUnloaded.LoadedDllPids -join ',')] after uninstall."
        }
        Assert-ExplorerHealthy -ExpectedPids $baselineExplorer.Pids -CaseName "uninstall_cycle_$cycle"
    }
    Add-CaseResult -Name "install_uninstall_stability" -Status "passed" -Details "3x hot-inject & hot-unload cycles succeeded with stable explorer.exe PID(s)." -DurationSec $sw.Elapsed.TotalSeconds
} catch {
    Add-CaseResult -Name "install_uninstall_stability" -Status "failed" -Details $_.Exception.Message -DurationSec $sw.Elapsed.TotalSeconds
}

# Helper to spawn an external lock-holder process using PowerShell (kept alive until killed or handle closed)
function Start-LockHolderProcess {
    param([string]$TargetFile, [switch]$Elevated)
    $readyName = "Local\HandsOffE2EReady_" + [guid]::NewGuid().ToString("N")
    $script = @"
`$fs = [System.IO.File]::Open('$($TargetFile.Replace("'", "''"))', [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
`$ev = [System.Threading.EventWaitHandle]::OpenExisting('$readyName')
[void]`$ev.Set()
`$ev.Dispose()
Start-Sleep -Seconds 45
`$fs.Dispose()
"@
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($script))
    $ev = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::ManualReset, $readyName)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -WindowStyle Hidden -EncodedCommand $encoded"
    $psi.UseShellExecute = [bool]$Elevated
    if ($Elevated) { $psi.Verb = "runas"; $psi.WindowStyle = "Hidden" }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $ev.WaitOne(8000)) {
        $ev.Dispose()
        if ($proc -and -not $proc.HasExited) { $proc.Kill() }
        throw "Lock holder process did not signal ready within 8s."
    }
    $ev.Dispose()
    return $proc
}

function Invoke-ExplorerDeleteUiScenario {
    param(
        [string]$CaseName,
        [string]$SelectPath,
        [string]$CheckGonePath,
        [int]$LockHolderPid
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $screenshotPath = Join-Path $screenshotsDir "$CaseName.png"
    $explorerProc = $null
    $openedHwndsBefore = @()
    $cur = [IntPtr]::Zero
    $pidSet = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($p in (Get-ExplorerState).Pids) { [void]$pidSet.Add($p) }

    while (($cur = [HandsOffE2EHelper]::FindWindowEx([IntPtr]::Zero, $cur, "CabinetWClass", $null)) -ne [IntPtr]::Zero) {
        $openedHwndsBefore += $cur
    }

    try {
        $shellApp = New-Object -ComObject Shell.Application
        $parentDir = Split-Path -Parent $SelectPath
        $parentUri = ([System.Uri]$parentDir).AbsoluteUri.TrimEnd('/')
        $leafName = Split-Path -Leaf $parentDir

        $explorerProc = Start-Process explorer.exe -ArgumentList "/select,`"$SelectPath`"" -PassThru
        $targetWin = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline) {
            try {
                foreach ($w in @($shellApp.Windows())) {
                    $locUrl = "$($w.LocationURL)".TrimEnd('/')
                    if ($locUrl -ieq $parentUri) {
                        $targetWin = [IntPtr][int64]$w.HWND
                        if ($targetWin -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($targetWin)) {
                            break
                        }
                    }
                }
            } catch {}
            if ($targetWin -ne [IntPtr]::Zero) { break }

            $c = [IntPtr]::Zero
            while (($c = [HandsOffE2EHelper]::FindWindowEx([IntPtr]::Zero, $c, "CabinetWClass", $null)) -ne [IntPtr]::Zero) {
                if (-not [HandsOffE2EHelper]::IsWindowVisible($c)) { continue }
                $txt = [HandsOffE2EHelper]::ReadWindowText($c)
                if (($openedHwndsBefore -notcontains $c) -or ($txt -like "*$leafName*")) {
                    $targetWin = $c
                    break
                }
            }
            if ($targetWin -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 150
        }
        if ($targetWin -eq [IntPtr]::Zero) {
            throw "Timed out waiting for File Explorer window selecting '$SelectPath'."
        }

        Start-Sleep -Milliseconds 700
        [void][HandsOffE2EHelper]::SetForegroundWindow($targetWin)
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait("+{DEL}")

        # Handle optional Shift+Delete confirmation dialog
        $opHwnd = [IntPtr]::Zero
        $barHwnd = [IntPtr]::Zero
        $dlgDeadline = (Get-Date).AddSeconds($DialogTimeoutSec)
        while ((Get-Date) -lt $dlgDeadline) {
            [void][HandsOffE2EHelper]::TryInvokeConfirmYesIfPresent($pidSet)
            $opHwnd = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSet)
            if ($opHwnd -ne [IntPtr]::Zero) {
                $barHwnd = [HandsOffE2EHelper]::FindWindowEx($opHwnd, [IntPtr]::Zero, "HandsOffActionBarWnd", $null)
                if ($barHwnd -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($barHwnd)) {
                    break
                }
            }
            Start-Sleep -Milliseconds 150
        }

        if ($opHwnd -eq [IntPtr]::Zero) {
            throw "OperationStatusWindow did not appear within ${DialogTimeoutSec}s."
        }
        if ($barHwnd -eq [IntPtr]::Zero -or -not [HandsOffE2EHelper]::IsWindowVisible($barHwnd)) {
            [void][HandsOffE2EHelper]::CaptureWindowPng($opHwnd, $screenshotPath)
            throw "HandsOffActionBarWnd was not visible inside OperationStatusWindow."
        }

        Start-Sleep -Milliseconds 200
        [void][HandsOffE2EHelper]::CaptureWindowPng($opHwnd, $screenshotPath)
        $uiaTexts = [HandsOffE2EHelper]::CollectUiaTexts($opHwnd)
        $joinedTexts = $uiaTexts -join " | "
        if ($joinedTexts -notmatch [regex]::Escape("$LockHolderPid")) {
            throw "Dialog text did not display culprit PID $LockHolderPid. Texts: $joinedTexts"
        }

        [HandsOffE2EHelper]::ClickChildBarRightButton($barHwnd)

        $doneDeadline = (Get-Date).AddSeconds($DialogTimeoutSec)
        while ((Get-Date) -lt $doneDeadline) {
            if (-not (Test-Path $CheckGonePath)) { break }
            Start-Sleep -Milliseconds 150
        }
        if (Test-Path $CheckGonePath) {
            throw "Target '$CheckGonePath' still exists after clicking HandsOff button and waiting ${DialogTimeoutSec}s."
        }

        Assert-ExplorerHealthy -ExpectedPids $baselineExplorer.Pids -CaseName $CaseName
        Add-CaseResult -Name $CaseName -Status "passed" -Details "Dialog showed PID $LockHolderPid; target deleted after click." -DurationSec $sw.Elapsed.TotalSeconds -Screenshot $screenshotPath
    } catch {
        $shot = if (Test-Path $screenshotPath) { $screenshotPath } else { "" }
        Add-CaseResult -Name $CaseName -Status "failed" -Details $_.Exception.Message -DurationSec $sw.Elapsed.TotalSeconds -Screenshot $shot
    } finally {
        if ($targetWin -and $targetWin -ne [IntPtr]::Zero -and ($openedHwndsBefore -notcontains $targetWin)) {
            [void][HandsOffE2EHelper]::PostMessage($targetWin, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

# 3. Interactive GUI E2E scenarios
$sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
if ($SkipGuiTests -or $sessionId -eq 0 -or $baselineExplorer.Pids.Count -eq 0) {
    Add-CaseResult -Name "gui_e2e_suite" -Status "skipped" -Details "Skipped GUI tests (SkipGuiTests=$SkipGuiTests, SessionId=$sessionId, ExplorerCount=$($baselineExplorer.Pids.Count))." -DurationSec 0
} else {
    $tempRoot = Join-Path $env:TEMP ("HandsOff_E2E_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    try {
        & $exePath install | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to install HandsOff before GUI tests." }
        Start-Sleep -Milliseconds 400

        # Case A: File locked by normal process
        $fileCaseDir = Join-Path $tempRoot "file_case"
        New-Item -ItemType Directory -Force -Path $fileCaseDir | Out-Null
        $lockedFile = Join-Path $fileCaseDir "locked_doc.txt"
        "data" | Out-File -FilePath $lockedFile -Encoding ascii
        $holderA = Start-LockHolderProcess -TargetFile $lockedFile
        try {
            Invoke-ExplorerDeleteUiScenario -CaseName "ui_file_lock_kill" -SelectPath $lockedFile -CheckGonePath $lockedFile -LockHolderPid $holderA.Id
        } finally {
            if ($holderA -and -not $holderA.HasExited) { $holderA.Kill() }
        }

        # Case B: Folder locked by nested file (sub\nested.txt)
        $folderParent = Join-Path $tempRoot "folder_parent"
        $lockedFolder = Join-Path $folderParent "locked_dir"
        $subDir = Join-Path $lockedFolder "sub"
        New-Item -ItemType Directory -Force -Path $subDir | Out-Null
        $nestedFile = Join-Path $subDir "nested.txt"
        "nested" | Out-File -FilePath $nestedFile -Encoding ascii
        $holderB = Start-LockHolderProcess -TargetFile $nestedFile
        try {
            Invoke-ExplorerDeleteUiScenario -CaseName "ui_folder_lock_nested" -SelectPath $lockedFolder -CheckGonePath $lockedFolder -LockHolderPid $holderB.Id
        } finally {
            if ($holderB -and -not $holderB.HasExited) { $holderB.Kill() }
        }
    } finally {
        & $exePath uninstall | Out-Null
        Start-Sleep -Milliseconds 300
        Remove-Item -Path $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Final crash check
$finalDumps = @(Get-ChildItem -Path "$env:LOCALAPPDATA\CrashDumps" -Filter "explorer.exe*.dmp" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
$newDumps = @($finalDumps | Where-Object { $initialDumps -notcontains $_ })
$werDumps = @(Get-ChildItem -Path $OutputDir -Filter "explorer.exe*.dmp" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
$crashEvents = Get-RecentExplorerCrashEvents -Since $testRunStart
$results.explorerCrashes = @($newDumps + $werDumps + $crashEvents)

$anyFailed = @($results.cases | Where-Object { $_.status -eq "failed" }).Count -gt 0
$results.passed = (-not $anyFailed) -and ($results.explorerCrashes.Count -eq 0)

$jsonPath = Join-Path $OutputDir "results.json"
$results | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding utf8
Write-Host "[HandsOff E2E] Report written to: $jsonPath" -ForegroundColor Cyan

if (-not $results.passed) {
    exit 1
}
exit 0
