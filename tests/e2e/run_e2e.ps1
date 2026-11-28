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

# Persist the full console log to disk so it survives even if the console window disappears.
$transcriptPath = Join-Path $OutputDir "run.log"
try { Start-Transcript -Path $transcriptPath -Force | Out-Null } catch {}

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

Add-Type -AssemblyName WindowsBase
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

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, UIntPtr dwExtraInfo);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadLibrary(string lpFileName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int LoadString(IntPtr hInstance, uint uID, StringBuilder lpBuffer, int nMaxCount);

    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();

    // SetForegroundWindow alone is silently ignored when we are not the foreground process
    // (focus-stealing prevention). Attach to the current foreground thread's input queue first.
    public static bool ForceForeground(IntPtr hWnd)
    {
        for (int attempt = 0; attempt < 4; attempt++)
        {
            if (IsIconic(hWnd)) ShowWindow(hWnd, 9); // SW_RESTORE
            IntPtr fg = GetForegroundWindow();
            if (fg == hWnd) return true;
            uint pid;
            uint fgThread = fg != IntPtr.Zero ? GetWindowThreadProcessId(fg, out pid) : 0;
            uint me = GetCurrentThreadId();
            bool attached = false;
            if (fgThread != 0 && fgThread != me) attached = AttachThreadInput(me, fgThread, true);
            try { SetForegroundWindow(hWnd); }
            finally { if (attached) AttachThreadInput(me, fgThread, false); }
            Thread.Sleep(80);
            if (GetForegroundWindow() == hWnd) return true;
        }
        return GetForegroundWindow() == hWnd;
    }

    public static void CaptureScreenPng(string outputPath)
    {
        var b = System.Windows.Forms.SystemInformation.VirtualScreen;
        using (var bmp = new Bitmap(b.Width, b.Height, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp))
        {
            g.CopyFromScreen(b.Left, b.Top, 0, 0, b.Size, CopyPixelOperation.SourceCopy);
            bmp.Save(outputPath, ImageFormat.Png);
        }
    }

    public static void FocusWindowClientCenter(IntPtr hWnd)
    {
        ForceForeground(hWnd);
        RECT rc;
        if (GetWindowRect(hWnd, out rc))
        {
            int cx = Math.Max(rc.Left + 40, rc.Right - 60);
            int cy = Math.Max(rc.Top + 140, rc.Bottom - 60);
            SetCursorPos(cx, cy);
            Thread.Sleep(30);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
            Thread.Sleep(30);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
        }
    }

    public static string LoadShell32CleanString(uint id)
    {
        IntPtr h = LoadLibrary("shell32.dll");
        if (h == IntPtr.Zero) return "";
        var sb = new StringBuilder(512);
        int len = LoadString(h, id, sb, sb.Capacity);
        if (len <= 0) return "";
        return sb.ToString().Replace("&", "").Trim();
    }

    public static bool TryInvokeReplaceFileInOpStatus(IntPtr opHwnd, string[] labels)
    {
        try
        {
            var root = AutomationElement.FromHandle(opHwnd);
            if (root == null) return false;
            var all = root.FindAll(TreeScope.Descendants, Condition.TrueCondition);
            foreach (AutomationElement el in all)
            {
                string name = (el.Current.Name ?? "").Replace("&", "").Trim();
                if (string.IsNullOrEmpty(name)) continue;
                bool matched = false;
                foreach (var lbl in labels)
                {
                    if (!string.IsNullOrEmpty(lbl) && name.Equals(lbl, StringComparison.OrdinalIgnoreCase))
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;

                object pat;
                if (el.TryGetCurrentPattern(InvokePattern.Pattern, out pat))
                {
                    ((InvokePattern)pat).Invoke();
                    return true;
                }
                var rect = el.Current.BoundingRectangle;
                if (!rect.IsEmpty && rect.Width > 0 && rect.Height > 0)
                {
                    SetForegroundWindow(opHwnd);
                    int cx = (int)(rect.Left + rect.Width / 2);
                    int cy = (int)(rect.Top + rect.Height / 2);
                    SetCursorPos(cx, cy);
                    Thread.Sleep(30);
                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
                    Thread.Sleep(30);
                    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
                    return true;
                }
            }
        }
        catch { }
        return false;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left, Top, Right, Bottom;
    }

    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint BM_CLICK = 0x00F5;
    public const int IDYES = 6;
    public const int VK_RETURN = 0x0D;

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

    public static string ReadClassName(IntPtr hWnd)
    {
        var sb = new StringBuilder(128);
        GetClassName(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static List<string> DescribeVisibleDialogs(HashSet<int> explorerPids)
    {
        var list = new List<string>();
        EnumWindows((hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) return true;
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            string cls = ReadClassName(hWnd);
            bool interestingCls = cls == "#32770" || cls == "OperationStatusWindow" || cls == "NativeHWNDHost";
            bool isExp = explorerPids != null && explorerPids.Contains((int)pid);
            if (!interestingCls && (!isExp || cls == "Shell_TrayWnd" || cls == "Progman" || cls == "WorkerW" || cls == "CabinetWClass"))
                return true;
            list.Add(string.Format("hwnd=0x{0:X} pid={1} cls='{2}' title='{3}'", (long)hWnd, pid, cls, ReadWindowText(hWnd)));
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static List<string> DescribeChildrenAndUia(IntPtr hWnd)
    {
        var list = new List<string>();
        IntPtr ch = IntPtr.Zero;
        while ((ch = FindWindowEx(hWnd, ch, null, null)) != IntPtr.Zero)
        {
            RECT rc;
            GetWindowRect(ch, out rc);
            list.Add(string.Format("child:cls={0},vis={1},rect=({2},{3},{4},{5})",
                ReadClassName(ch), IsWindowVisible(ch), rc.Left, rc.Top, rc.Right, rc.Bottom));
        }
        try
        {
            var root = AutomationElement.FromHandle(hWnd);
            if (root != null)
            {
                var all = root.FindAll(TreeScope.Descendants, Condition.TrueCondition);
                foreach (AutomationElement el in all)
                {
                    list.Add(string.Format("uia:{0}|name='{1}'|id='{2}'",
                        el.Current.ControlType.ProgrammaticName,
                        (el.Current.Name ?? "").Replace("\r", " ").Replace("\n", " "),
                        el.Current.AutomationId));
                }
            }
        }
        catch { }
        return list;
    }

    public static bool TryInvokeConfirmYesIfPresent(HashSet<int> explorerPids)
    {
        var candidates = new List<IntPtr>();
        EnumWindows((hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) return true;
            string cls = ReadClassName(hWnd);
            if (cls == "Shell_TrayWnd" || cls == "Progman" || cls == "WorkerW" || cls == "CabinetWClass" || cls == "OperationStatusWindow")
                return true;
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (cls == "#32770" || (explorerPids != null && explorerPids.Contains((int)pid)))
            {
                candidates.Add(hWnd);
            }
            return true;
        }, IntPtr.Zero);

        foreach (IntPtr cur in candidates)
        {
            try
            {
                IntPtr hYesBtn = GetDlgItem(cur, IDYES);
                if (hYesBtn != IntPtr.Zero && IsWindowVisible(hYesBtn))
                {
                    SetForegroundWindow(cur);
                    SendMessage(hYesBtn, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
                    return true;
                }

                var root = AutomationElement.FromHandle(cur);
                if (root == null) continue;
                var buttons = root.FindAll(
                    TreeScope.Descendants,
                    new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
                bool hasNoBtn = false;
                AutomationElement yesEl = null;
                foreach (AutomationElement btn in buttons)
                {
                    string autoId = (btn.Current.AutomationId ?? "").Trim();
                    string name = (btn.Current.Name ?? "").Replace("&", "").Trim();
                    if (autoId.Equals("CommandButton_7", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("No", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("\u041d\u0435\u0442", StringComparison.OrdinalIgnoreCase))
                    {
                        hasNoBtn = true;
                    }
                    if (autoId.Equals("CommandButton_6", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("Yes", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("\u0414\u0430", StringComparison.OrdinalIgnoreCase))
                    {
                        yesEl = btn;
                    }
                }

                if (yesEl != null && hasNoBtn)
                {
                    SetForegroundWindow(cur);
                    int nativeHwnd = yesEl.Current.NativeWindowHandle;
                    if (nativeHwnd != 0)
                    {
                        SendMessage((IntPtr)nativeHwnd, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
                    }
                    object pat;
                    if (yesEl.TryGetCurrentPattern(InvokePattern.Pattern, out pat))
                    {
                        ((InvokePattern)pat).Invoke();
                    }
                    if (IsWindowVisible(cur))
                    {
                        var rect = yesEl.Current.BoundingRectangle;
                        if (!rect.IsEmpty && rect.Width > 0 && rect.Height > 0)
                        {
                            int cx = (int)(rect.Left + rect.Width / 2);
                            int cy = (int)(rect.Top + rect.Height / 2);
                            SetCursorPos(cx, cy);
                            Thread.Sleep(30);
                            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
                            Thread.Sleep(30);
                            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
                        }
                    }
                    if (IsWindowVisible(cur))
                    {
                        PostMessage(cur, WM_COMMAND, (IntPtr)IDYES, IntPtr.Zero);
                    }
                    return true;
                }
            }
            catch { }
        }
        return false;
    }
}
"@

Add-Type -TypeDefinition $helperCode -ReferencedAssemblies @("WindowsBase", "System.Drawing", "System.Windows.Forms", "UIAutomationClient", "UIAutomationTypes")

function Get-ExplorerState {
    $procs = @(Get-Process -Name explorer -ErrorAction SilentlyContinue)
    $pids = @($procs | ForEach-Object { $_.Id } | Sort-Object)
    $shellPids = @()
    $trayHwnd = [HandsOffE2EHelper]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, "Shell_TrayWnd", $null)
    if ($trayHwnd -ne [IntPtr]::Zero) {
        [uint32]$trayPid = 0
        [void][HandsOffE2EHelper]::GetWindowThreadProcessId($trayHwnd, [ref]$trayPid)
        if ($trayPid -ne 0) { $shellPids = @([int]$trayPid) }
    }
    if ($shellPids.Count -eq 0 -and $pids.Count -gt 0) {
        $oldest = $procs | Sort-Object StartTime -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($oldest) { $shellPids = @([int]$oldest.Id) } else { $shellPids = @($pids[0]) }
    }
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
        ShellPids     = $shellPids
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
    $shellArr = @($baselineExplorer.ShellPids)
    $expectedShell = if ($shellArr.Count -gt 0) { $shellArr } else { @($ExpectedPids) }
    $missing = @($expectedShell | Where-Object { @($nowState.Pids) -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "Shell explorer.exe PID(s) [$($missing -join ',')] disappeared during '$CaseName' (possible crash/restart)!"
    }
    $newCrashes = @(Get-RecentExplorerCrashEvents -Since $testRunStart)
    if ($newCrashes.Count -gt 0) {
        throw "explorer.exe crash event detected during '$CaseName': $($newCrashes -join ' | ')"
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
    # NOTE: do NOT pass "-WindowStyle Hidden" for the non-elevated child. With UseShellExecute=$false it
    # inherits OUR console, and powershell.exe would hide that shared console window (the terminal
    # "closes" while the script keeps running invisibly). CreateNoWindow gives it its own hidden console.
    $psi.UseShellExecute = [bool]$Elevated
    if ($Elevated) {
        $psi.Arguments = "-NoProfile -WindowStyle Hidden -EncodedCommand $encoded"
        $psi.Verb = "runas"
        $psi.WindowStyle = "Hidden"
    } else {
        $psi.Arguments = "-NoProfile -EncodedCommand $encoded"
        $psi.CreateNoWindow = $true
    }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $ev.WaitOne(8000)) {
        $ev.Dispose()
        if ($proc -and -not $proc.HasExited) { $proc.Kill() }
        throw "Lock holder process did not signal ready within 8s."
    }
    $ev.Dispose()
    return $proc
}

function Test-ClipboardHasFile {
    param([string]$Path)
    try {
        $list = @(Get-Clipboard -Format FileDropList -ErrorAction Stop)
        return (@($list | ForEach-Object { $_.FullName }) -contains $Path)
    } catch { return $false }
}

# Paste into an open Explorer folder window. Attempt 0 uses the shell COM "paste" verb (no focus or
# mouse needed, deterministic); later attempts fall back to real input (click + Ctrl+V).
function Invoke-ExplorerPaste {
    param($FolderWinObj, [IntPtr]$Hwnd, [int]$Attempt)
    if ($Attempt -eq 0 -and $FolderWinObj) {
        try { $FolderWinObj.Document.Folder.Self.InvokeVerb("paste"); return } catch {}
    }
    [void][HandsOffE2EHelper]::ForceForeground($Hwnd)
    [HandsOffE2EHelper]::FocusWindowClientCenter($Hwnd)
    Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait("^v")
}

function Invoke-ExplorerDeleteUiScenario {
    param(
        [string]$CaseName,
        [string]$SelectPath,
        [string]$CheckGonePath,
        [int]$LockHolderPid,
        [switch]$UseShellOpen
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
        $itemLeaf = Split-Path -Leaf $SelectPath
        $parentUri = ([System.Uri]$parentDir).AbsoluteUri.TrimEnd('/')
        $leafName = Split-Path -Leaf $parentDir

        if ($UseShellOpen) {
            $shellApp.Open($parentDir)
        } else {
            $explorerProc = Start-Process explorer.exe -ArgumentList "/select,`"$SelectPath`"" -PassThru
        }
        $targetWin = [IntPtr]::Zero
        $shellWinObj = $null
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline) {
            try {
                foreach ($w in @($shellApp.Windows())) {
                    $locUrl = "$($w.LocationURL)".TrimEnd('/')
                    if ($locUrl -ieq $parentUri) {
                        $candHwnd = [IntPtr][int64]$w.HWND
                        if ($candHwnd -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($candHwnd)) {
                            $targetWin = $candHwnd
                            $shellWinObj = $w
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

        Start-Sleep -Milliseconds 600
        if ($UseShellOpen -and $shellWinObj) {
            try {
                $folderItem = $shellWinObj.Document.Folder.ParseName($itemLeaf)
                if ($folderItem) {
                    $shellWinObj.Document.SelectItem($folderItem, 29) # select, deselect others, ensure visible, focus
                }
            } catch {}
        }
        [void][HandsOffE2EHelper]::SetForegroundWindow($targetWin)
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait("+{DEL}")

        # Handle optional Shift+Delete confirmation dialog
        $opHwnd = [IntPtr]::Zero
        $barHwnd = [IntPtr]::Zero
        $confirmedCount = 0
        $dlgDeadline = (Get-Date).AddSeconds($DialogTimeoutSec)
        while ((Get-Date) -lt $dlgDeadline) {
            if ([HandsOffE2EHelper]::TryInvokeConfirmYesIfPresent($pidSet)) {
                $confirmedCount++
                Start-Sleep -Milliseconds 250
                if ($confirmedCount -ge 2) {
                    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
                    Start-Sleep -Milliseconds 200
                }
            }
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
            $vis = [HandsOffE2EHelper]::DescribeVisibleDialogs($pidSet)
            $extra = if ($vis.Count -gt 0) { " Visible dialogs: " + ($vis -join "; ") } else { "" }
            throw "OperationStatusWindow did not appear within ${DialogTimeoutSec}s.$extra"
        }
        if ($barHwnd -eq [IntPtr]::Zero -or -not [HandsOffE2EHelper]::IsWindowVisible($barHwnd)) {
            [void][HandsOffE2EHelper]::CaptureWindowPng($opHwnd, $screenshotPath)
            [uint32]$opPid = 0
            [void][HandsOffE2EHelper]::GetWindowThreadProcessId($opHwnd, [ref]$opPid)
            $loadedPids = (Get-ExplorerState).LoadedDllPids
            $diagList = [HandsOffE2EHelper]::DescribeChildrenAndUia($opHwnd) -join " | "
            throw "HandsOffActionBarWnd not visible (barHwnd=$barHwnd, opPid=$opPid, loadedPids=[$($loadedPids -join ',')]). Tree: $diagList"
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
        $leftOp = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSet)
        if ($leftOp -ne [IntPtr]::Zero) {
            [void][HandsOffE2EHelper]::PostMessage($leftOp, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 300
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
    $traceLog = Join-Path $OutputDir "handsoff_trace.log"
    if (Test-Path $traceLog) { Remove-Item $traceLog -Force }
    $traceRegKey = "HKCU:\Software\HandsOff"
    New-Item -Path $traceRegKey -Force | Out-Null
    Set-ItemProperty -Path $traceRegKey -Name "TraceFile" -Value $traceLog -Type String
    try {
        & $exePath install | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to install HandsOff before GUI tests (exit=$LASTEXITCODE)." }
        Start-Sleep -Milliseconds 400

        # Case A: File locked by normal process
        $fileCaseDir = Join-Path $tempRoot "file_case"
        New-Item -ItemType Directory -Force -Path $fileCaseDir | Out-Null
        $lockedFile = Join-Path $fileCaseDir "locked_doc.txt"
        "data" | Out-File -FilePath $lockedFile -Encoding ascii
        $holderA = Start-LockHolderProcess -TargetFile $lockedFile
        try {
            Invoke-ExplorerDeleteUiScenario -CaseName "ui_file_lock_kill" -SelectPath $lockedFile -CheckGonePath $lockedFile -LockHolderPid $holderA.Id -UseShellOpen
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

        # Case C: Move file with replace onto a file locked by another process (Ctrl+X -> Ctrl+V -> Replace -> Kill)
        $moveCaseDir = Join-Path $tempRoot "move_replace_case"
        $moveSrcDir = Join-Path $moveCaseDir "src"
        $moveDstDir = Join-Path $moveCaseDir "dst"
        New-Item -ItemType Directory -Force -Path $moveSrcDir | Out-Null
        New-Item -ItemType Directory -Force -Path $moveDstDir | Out-Null
        $moveSrcFile = Join-Path $moveSrcDir "catalog.db"
        $moveDstFile = Join-Path $moveDstDir "catalog.db"
        [System.IO.File]::WriteAllText($moveSrcFile, "new_version")
        [System.IO.File]::WriteAllText($moveDstFile, "locked_old")
        $holderC = Start-LockHolderProcess -TargetFile $moveDstFile
        try {
            $swC = [System.Diagnostics.Stopwatch]::StartNew()
            $shotC = Join-Path $screenshotsDir "ui_move_replace_locked.png"
            $pidSetC = New-Object 'System.Collections.Generic.HashSet[int]'
            foreach ($p in (Get-ExplorerState).Pids) { [void]$pidSetC.Add($p) }
            $shellAppC = New-Object -ComObject Shell.Application
            $openedBeforeC = @()
            $cw = [IntPtr]::Zero
            while (($cw = [HandsOffE2EHelper]::FindWindowEx([IntPtr]::Zero, $cw, "CabinetWClass", $null)) -ne [IntPtr]::Zero) {
                $openedBeforeC += $cw
            }

            $srcWinC = [IntPtr]::Zero
            $dstWinC = [IntPtr]::Zero
            $dstWinObjC = $null
            $pasteAttempts = 0
            try {
                $srcUriC = ([System.Uri]$moveSrcDir).AbsoluteUri.TrimEnd('/')
                $dstUriC = ([System.Uri]$moveDstDir).AbsoluteUri.TrimEnd('/')
                $shellAppC.Open($moveSrcDir)
                $srcWinObjC = $null
                $dl = (Get-Date).AddSeconds(10)
                while ((Get-Date) -lt $dl) {
                    foreach ($w in @($shellAppC.Windows())) {
                        if ("$($w.LocationURL)".TrimEnd('/') -ieq $srcUriC) {
                            $h = [IntPtr][int64]$w.HWND
                            if ($h -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($h)) {
                                $srcWinC = $h; $srcWinObjC = $w; break
                            }
                        }
                    }
                    if ($srcWinC -ne [IntPtr]::Zero) { break }
                    Start-Sleep -Milliseconds 150
                }
                if ($srcWinC -eq [IntPtr]::Zero) { throw "Timed out opening move source folder." }

                Start-Sleep -Milliseconds 600
                $itemC = $srcWinObjC.Document.Folder.ParseName("catalog.db")
                if (-not $itemC) { throw "catalog.db not found in move source folder view." }
                # Cut, then VERIFY the clipboard really holds the file; retry (COM verb, then Ctrl+X).
                $cutOk = $false
                for ($cutTry = 0; $cutTry -lt 4 -and -not $cutOk; $cutTry++) {
                    $srcWinObjC.Document.SelectItem($itemC, 29)
                    if ($cutTry -lt 2) {
                        try { $itemC.InvokeVerb("cut") } catch {}
                    } else {
                        [void][HandsOffE2EHelper]::ForceForeground($srcWinC)
                        Start-Sleep -Milliseconds 250
                        [System.Windows.Forms.SendKeys]::SendWait("^x")
                    }
                    $cutDeadline = (Get-Date).AddMilliseconds(1500)
                    while ((Get-Date) -lt $cutDeadline) {
                        if (Test-ClipboardHasFile -Path $moveSrcFile) { $cutOk = $true; break }
                        Start-Sleep -Milliseconds 100
                    }
                }
                if (-not $cutOk) { Write-Host "[HandsOff E2E] WARN: could not confirm clipboard cut state for catalog.db; continuing." -ForegroundColor Yellow }

                $shellAppC.Open($moveDstDir)
                $dl2 = (Get-Date).AddSeconds(10)
                while ((Get-Date) -lt $dl2) {
                    foreach ($w in @($shellAppC.Windows())) {
                        if ("$($w.LocationURL)".TrimEnd('/') -ieq $dstUriC) {
                            $h = [IntPtr][int64]$w.HWND
                            if ($h -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($h)) {
                                $dstWinC = $h; $dstWinObjC = $w; break
                            }
                        }
                    }
                    if ($dstWinC -ne [IntPtr]::Zero) { break }
                    Start-Sleep -Milliseconds 150
                }
                if ($dstWinC -eq [IntPtr]::Zero) { throw "Timed out opening move destination folder." }

                Start-Sleep -Milliseconds 700
                $pasteAttempts = 0
                $nextPasteAt = Get-Date

                $replaceLabels = @(
                    ([HandsOffE2EHelper]::LoadShell32CleanString(0x81D6)),
                    ([HandsOffE2EHelper]::LoadShell32CleanString(0x81D5)),
                    "Replace the file in the destination"
                )
                $opHwndC = [IntPtr]::Zero
                $barHwndC = [IntPtr]::Zero
                $replaceClicked = $false
                $dlgDlC = (Get-Date).AddSeconds($DialogTimeoutSec)
                while ((Get-Date) -lt $dlgDlC) {
                    $opHwndC = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSetC)
                    if ($opHwndC -eq [IntPtr]::Zero -and -not $replaceClicked -and $pasteAttempts -lt 3 -and (Get-Date) -ge $nextPasteAt) {
                        Invoke-ExplorerPaste -FolderWinObj $dstWinObjC -Hwnd $dstWinC -Attempt $pasteAttempts
                        $pasteAttempts++
                        $nextPasteAt = (Get-Date).AddSeconds(3.5)
                    }
                    if ($opHwndC -ne [IntPtr]::Zero) {
                        if (-not $replaceClicked -and [HandsOffE2EHelper]::TryInvokeReplaceFileInOpStatus($opHwndC, $replaceLabels)) {
                            $replaceClicked = $true
                            Start-Sleep -Milliseconds 350
                            continue
                        }
                        $barHwndC = [HandsOffE2EHelper]::FindWindowEx($opHwndC, [IntPtr]::Zero, "HandsOffActionBarWnd", $null)
                        if ($barHwndC -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($barHwndC)) {
                            break
                        }
                    }
                    Start-Sleep -Milliseconds 150
                }

                if ($opHwndC -eq [IntPtr]::Zero) {
                    throw "OperationStatusWindow did not appear during move-with-replace."
                }
                if ($barHwndC -eq [IntPtr]::Zero -or -not [HandsOffE2EHelper]::IsWindowVisible($barHwndC)) {
                    [void][HandsOffE2EHelper]::CaptureWindowPng($opHwndC, $shotC)
                    $diagC = [HandsOffE2EHelper]::DescribeChildrenAndUia($opHwndC) -join " | "
                    throw "HandsOffActionBarWnd not visible during move-with-replace (replaceClicked=$replaceClicked). Tree: $diagC"
                }

                Start-Sleep -Milliseconds 200
                [void][HandsOffE2EHelper]::CaptureWindowPng($opHwndC, $shotC)
                $textsC = ([HandsOffE2EHelper]::CollectUiaTexts($opHwndC)) -join " | "
                if ($textsC -notmatch [regex]::Escape("$($holderC.Id)")) {
                    throw "Move-with-replace dialog did not display culprit PID $($holderC.Id). Texts: $textsC"
                }

                [HandsOffE2EHelper]::ClickChildBarRightButton($barHwndC)

                $doneDlC = (Get-Date).AddSeconds($DialogTimeoutSec)
                while ((Get-Date) -lt $doneDlC) {
                    if (-not (Test-Path $moveSrcFile)) { break }
                    Start-Sleep -Milliseconds 150
                }
                if (Test-Path $moveSrcFile) {
                    throw "Source file '$moveSrcFile' still exists after move-with-replace unlock."
                }
                $dstContent = [System.IO.File]::ReadAllText($moveDstFile)
                if ($dstContent -ne "new_version") {
                    throw "Destination file content mismatch after move-with-replace: '$dstContent'"
                }

                Assert-ExplorerHealthy -ExpectedPids $baselineExplorer.Pids -CaseName "ui_move_replace_locked"
                Add-CaseResult -Name "ui_move_replace_locked" -Status "passed" -Details "Replace dialog + File In Use showed PID $($holderC.Id); moved and replaced destination." -DurationSec $swC.Elapsed.TotalSeconds -Screenshot $shotC
            } catch {
                $failMsg = $_.Exception.Message
                $sc = if (Test-Path $shotC) { $shotC } else { "" }
                try {
                    $shotFailC = Join-Path $screenshotsDir "ui_move_replace_locked_fail.png"
                    [HandsOffE2EHelper]::CaptureScreenPng($shotFailC)
                    $sc = $shotFailC
                    $dlgs = ([HandsOffE2EHelper]::DescribeVisibleDialogs($pidSetC)) -join " ; "
                    $failMsg += " [diag: pastes=$pasteAttempts clipboardHasSrc=$(Test-ClipboardHasFile -Path $moveSrcFile) srcExists=$(Test-Path $moveSrcFile) fg=0x$([HandsOffE2EHelper]::GetForegroundWindow().ToString('X')) dialogs=$dlgs]"
                } catch {}
                Add-CaseResult -Name "ui_move_replace_locked" -Status "failed" -Details $failMsg -DurationSec $swC.Elapsed.TotalSeconds -Screenshot $sc
            } finally {
                foreach ($hw in @($srcWinC, $dstWinC)) {
                    if ($hw -and $hw -ne [IntPtr]::Zero -and ($openedBeforeC -notcontains $hw)) {
                        [void][HandsOffE2EHelper]::PostMessage($hw, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
                    }
                }
                $leftOpC = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSetC)
                if ($leftOpC -ne [IntPtr]::Zero) {
                    [void][HandsOffE2EHelper]::PostMessage($leftOpC, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 300
                }
            }
        } finally {
            if ($holderC -and -not $holderC.HasExited) { $holderC.Kill() }
        }

        # Case D: Multi-process folder lock (two distinct processes holding two files inside one folder)
        $multiParent = Join-Path $tempRoot "multi_parent"
        $multiFolder = Join-Path $multiParent "multi_dir"
        New-Item -ItemType Directory -Force -Path $multiFolder | Out-Null
        $multiFile1 = Join-Path $multiFolder "part1.txt"
        $multiFile2 = Join-Path $multiFolder "part2.txt"
        "p1" | Out-File -FilePath $multiFile1 -Encoding ascii
        "p2" | Out-File -FilePath $multiFile2 -Encoding ascii
        $holderD1 = Start-LockHolderProcess -TargetFile $multiFile1
        $holderD2 = Start-LockHolderProcess -TargetFile $multiFile2
        try {
            $swD = [System.Diagnostics.Stopwatch]::StartNew()
            $shotD = Join-Path $screenshotsDir "ui_multi_folder_lock_kill.png"
            $pidSetD = New-Object 'System.Collections.Generic.HashSet[int]'
            foreach ($p in (Get-ExplorerState).Pids) { [void]$pidSetD.Add($p) }
            $shellAppD = New-Object -ComObject Shell.Application
            $openedBeforeD = @()
            $cw = [IntPtr]::Zero
            while (($cw = [HandsOffE2EHelper]::FindWindowEx([IntPtr]::Zero, $cw, "CabinetWClass", $null)) -ne [IntPtr]::Zero) {
                $openedBeforeD += $cw
            }

            $targetWinD = [IntPtr]::Zero
            try {
                $parentUriD = ([System.Uri]$multiParent).AbsoluteUri.TrimEnd('/')
                $shellAppD.Open($multiParent)
                $winObjD = $null
                $dl = (Get-Date).AddSeconds(10)
                while ((Get-Date) -lt $dl) {
                    foreach ($w in @($shellAppD.Windows())) {
                        if ("$($w.LocationURL)".TrimEnd('/') -ieq $parentUriD) {
                            $h = [IntPtr][int64]$w.HWND
                            if ($h -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($h)) {
                                $targetWinD = $h; $winObjD = $w; break
                            }
                        }
                    }
                    if ($targetWinD -ne [IntPtr]::Zero) { break }
                    Start-Sleep -Milliseconds 150
                }
                if ($targetWinD -eq [IntPtr]::Zero) { throw "Timed out opening multi_parent folder." }
                Start-Sleep -Milliseconds 600
                $itemD = $winObjD.Document.Folder.ParseName("multi_dir")
                if ($itemD) { $winObjD.Document.SelectItem($itemD, 29) }
                [void][HandsOffE2EHelper]::SetForegroundWindow($targetWinD)
                Start-Sleep -Milliseconds 250
                [System.Windows.Forms.SendKeys]::SendWait("+{DEL}")

                $dlgDlD = (Get-Date).AddSeconds($DialogTimeoutSec)
                $killedClicks = 0
                $confirmedD = 0
                while ((Get-Date) -lt $dlgDlD) {
                    if (-not (Test-Path $multiFolder)) { break }
                    if ([HandsOffE2EHelper]::TryInvokeConfirmYesIfPresent($pidSetD)) {
                        $confirmedD++
                        Start-Sleep -Milliseconds 250
                        if ($confirmedD -ge 2) { [System.Windows.Forms.SendKeys]::SendWait("{ENTER}") }
                        continue
                    }
                    $opD = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSetD)
                    if ($opD -ne [IntPtr]::Zero) {
                        $barD = [HandsOffE2EHelper]::FindWindowEx($opD, [IntPtr]::Zero, "HandsOffActionBarWnd", $null)
                        if ($barD -ne [IntPtr]::Zero -and [HandsOffE2EHelper]::IsWindowVisible($barD)) {
                            if ($killedClicks -eq 0) {
                                Start-Sleep -Milliseconds 200
                                [void][HandsOffE2EHelper]::CaptureWindowPng($opD, $shotD)
                            }
                            [HandsOffE2EHelper]::ClickChildBarRightButton($barD)
                            $killedClicks++
                            Start-Sleep -Milliseconds 500
                            continue
                        }
                    }
                    Start-Sleep -Milliseconds 150
                }
                if (Test-Path $multiFolder) {
                    throw "multi_dir still exists after multi-locker unlock (clicks=$killedClicks)."
                }
                Assert-ExplorerHealthy -ExpectedPids $baselineExplorer.Pids -CaseName "ui_multi_folder_lock_kill"
                Add-CaseResult -Name "ui_multi_folder_lock_kill" -Status "passed" -Details "Unlocked 2 concurrent blockers (PIDs $($holderD1.Id), $($holderD2.Id)) in $killedClicks click(s)." -DurationSec $swD.Elapsed.TotalSeconds -Screenshot $shotD
            } catch {
                $sd = if (Test-Path $shotD) { $shotD } else { "" }
                Add-CaseResult -Name "ui_multi_folder_lock_kill" -Status "failed" -Details $_.Exception.Message -DurationSec $swD.Elapsed.TotalSeconds -Screenshot $sd
            } finally {
                if ($targetWinD -ne [IntPtr]::Zero -and ($openedBeforeD -notcontains $targetWinD)) {
                    [void][HandsOffE2EHelper]::PostMessage($targetWinD, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
                }
                $leftOpD = [HandsOffE2EHelper]::FindVisibleWindowByClassForPids("OperationStatusWindow", $pidSetD)
                if ($leftOpD -ne [IntPtr]::Zero) {
                    [void][HandsOffE2EHelper]::PostMessage($leftOpD, [HandsOffE2EHelper]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 300
                }
            }
        } finally {
            if ($holderD1 -and -not $holderD1.HasExited) { $holderD1.Kill() }
            if ($holderD2 -and -not $holderD2.HasExited) { $holderD2.Kill() }
        }

        # Case E: Elevated lock holder + HandsOffSvc & HKLM overlay registration (requires Admin session)
        if (-not $isElevated) {
            Add-CaseResult -Name "ui_elevated_lock" -Status "skipped" -Details "Requires elevated session to verify HandsOffSvc & HKLM registration." -DurationSec 0
        } else {
            $elevDir = Join-Path $tempRoot "elevated_case"
            New-Item -ItemType Directory -Force -Path $elevDir | Out-Null
            $elevFile = Join-Path $elevDir "admin_locked.txt"
            "admin" | Out-File -FilePath $elevFile -Encoding ascii
            $holderE = Start-LockHolderProcess -TargetFile $elevFile -Elevated
            try {
                $svc = Get-Service -Name "HandsOffSvc" -ErrorAction SilentlyContinue
                if (-not $svc) { throw "HandsOffSvc service was not registered by elevated install." }
                $hklmOverlay12 = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\            HandsOff"
                $hklmOverlay3  = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\   HandsOff"
                if (-not (Test-Path -LiteralPath $hklmOverlay12) -and -not (Test-Path -LiteralPath $hklmOverlay3)) {
                    throw "HKLM ShellIconOverlayIdentifiers key missing after elevated install."
                }
                Invoke-ExplorerDeleteUiScenario -CaseName "ui_elevated_lock" -SelectPath $elevFile -CheckGonePath $elevFile -LockHolderPid $holderE.Id -UseShellOpen
                Start-Sleep -Milliseconds 500
                $svcAfter = Get-Service -Name "HandsOffSvc" -ErrorAction SilentlyContinue
                if ($svcAfter -and $svcAfter.Status -ne "Stopped") {
                    throw "HandsOffSvc stayed in state '$($svcAfter.Status)' instead of stopping after unlock."
                }
            } catch {
                Add-CaseResult -Name "ui_elevated_lock" -Status "failed" -Details $_.Exception.Message -DurationSec 0
            } finally {
                if ($holderE -and -not $holderE.HasExited) { $holderE.Kill() }
            }
        }
    } catch {
        Add-CaseResult -Name "gui_e2e_suite" -Status "failed" -Details $_.Exception.Message -DurationSec 0
    } finally {
        Remove-ItemProperty -Path $traceRegKey -Name "TraceFile" -ErrorAction SilentlyContinue
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

if ($results.passed) {
    $passedCount = @($results.cases | Where-Object { $_.status -eq "passed" }).Count
    $totalCount = @($results.cases).Count
    $ver = if ($results.os.displayVersion) { $results.os.displayVersion } else { "<Version from winver>" }
    $buildStr = if ($null -ne $results.os.ubr) { "$($results.os.currentBuild).$($results.os.ubr)" } else { "$($results.os.currentBuild)" }
    $arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
    $osGuess = [string]$results.os.productName
    [int]$buildNum = 0
    if ([int]::TryParse([string]$results.os.currentBuild, [ref]$buildNum) -and $buildNum -ge 22000 -and $osGuess -match '^Windows 10\b') {
        $osGuess = $osGuess -replace '^Windows 10\b', 'Windows 11'
    }
    $readmeRow = "| $osGuess | $ver | ``$buildStr`` ($arch) | ``$($results.os.culture)`` | Host | **Pass ($passedCount/$totalCount E2E)** |"
    Write-Host "[HandsOff E2E] Want to add this system to README.md? Verify via Win+R -> winver and paste:" -ForegroundColor DarkCyan
    Write-Host $readmeRow -ForegroundColor Gray
}

try { Stop-Transcript | Out-Null } catch {}
if (-not $results.passed) {
    exit 1
}
exit 0
