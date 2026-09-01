[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [ValidateSet('Idle', 'Append')][string]$Mode = 'Idle',
    [ValidateRange(1, 86400)][int]$DurationSeconds = 1800,
    [ValidateRange(1, 300)][int]$RecoverySeconds = 30
)

# Windows PowerShell 5.1; deliberately ASCII source (no code-page dependency).
# This runs a copied production EXE, never a GUI test binary. UIA operates only
# on windows belonging to the owned PID; no product test hooks are used.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
if (-not $evidence.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'EvidenceDirectory must be an explicit new subdirectory of this repository.'
}
if (Test-Path -LiteralPath $evidence) { throw 'EvidenceDirectory already exists; refusing to overwrite evidence.' }
$ancestor = Split-Path -Parent $evidence
if (-not (Test-Path -LiteralPath $ancestor -PathType Container)) { throw 'EvidenceDirectory parent must already exist.' }
while ($ancestor) {
    $entry = Get-Item -LiteralPath $ancestor -Force
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse-point evidence ancestors are not allowed.' }
    if ($ancestor -eq $repo) { break }
    $ancestor = Split-Path -Parent $ancestor
}
$relative = $evidence.Substring($repo.Length + 1)
if ($relative -match '(^|[\\/])\.(git|codex|agents)([\\/]|$)') { throw 'Reserved repository directories cannot contain soak evidence.' }
$inputExe = (Resolve-Path -LiteralPath $ExePath).Path
if (-not (Test-Path -LiteralPath $inputExe -PathType Leaf) -or [IO.Path]::GetFileName($inputExe) -ine 'notepad-safe.exe') {
    throw 'ExePath must identify the formal static notepad-safe.exe.'
}
[void][IO.Directory]::CreateDirectory($evidence)
$utf8 = New-Object Text.UTF8Encoding($false)
$runId = [Guid]::NewGuid().ToString('N')
$report = [ordered]@{
    schema = 1; runId = $runId; mode = $Mode; requestedSeconds = $DurationSeconds
    recoverySeconds = $RecoverySeconds; classification = 'probe'; success = $false
    resourceAcceptance = 'not-evaluated'
    startedUtc = [DateTime]::UtcNow.ToString('o'); finishedUtc = $null; actualTotalSeconds = 0
    inputExe = $inputExe; copiedExe = $null; binarySha256 = $null; copiedBinarySha256 = $null
    processId = $null; processStartUtc = $null; gracefulExit = $false; exitCode = $null; forcedTermination = $false
    logicalProcessors = [Environment]::ProcessorCount; processor = $env:PROCESSOR_IDENTIFIER
    os = [Environment]::OSVersion.VersionString; osBuild = $null; physicalMemoryBytes = $null
    architecture = $env:PROCESSOR_ARCHITECTURE; powerShell = $PSVersionTable.PSVersion.ToString()
    sampleIntervalSeconds = 1; statusCheckIntervalSeconds = 5; appendBytesPerWrite = 131072
    statusObservationDefinition = 'Earlier captures rediscovered the status using a full UIA subtree search every 5 seconds. This capture binds one exact-ID, owned-PID label once, then reads its live Current Name, ProcessId, AutomationId and IsOffscreen. Invalidated/replaced labels fail the capture; no replacement discovery or cached status strings. Menu discovery and post-close disappearance checks still search the owned UI tree.'
    statusElementIdentity = $null
    cpuDefinition = 'CPU delta / monotonic wall delta * 100; one-core and divided-by-logical-CPU variants. Product PID only.'
    memoryDefinition = 'PrivateBytes from Process; CommitBytes from PROCESS_MEMORY_COUNTERS_EX.PrivateUsage (private commit, not an additional allocation). PeakCommitBytes is PeakPagefileUsage.'
    heartbeatDefinition = 'SendMessageTimeout WM_NULL, 100 ms; message-pump responsiveness only, not rendered-input latency.'
    source = $null; initialBytes = 34603008; appendWrites = 0; expectedBytes = 34603008
    actualBytes = $null; oracleSha256 = $null; sourceSha256 = $null; sourceVerified = $false
    phases = @(); errors = @(); uiaDiagnostics = @(); menuCleanup = @(); activations = @(); screenshot = 'Not captured: PrintWindow is a synchronous call without a safe cancellation deadline; no desktop screenshots are taken.'
}
$owned = $null
$ownedStartTicks = 0L
$mainWindow = [IntPtr]::Zero
$statusElement = $null
$statusElementId = $null
$csv = $null
$writer = $null
$oracle = $null
$oracleFinalized = $false
$headerWritten = $false
$previousCpu = $null
$previousSampleTime = $null
$clock = [Diagnostics.Stopwatch]::StartNew()

function Save-Report {
    [IO.File]::WriteAllText((Join-Path $evidence 'summary.json'), ($report | ConvertTo-Json -Depth 8), $utf8)
}
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function Confirm-SourceEvidence($Report, [bool]$OracleFinalized) {
    $Report.sourceVerified = $false
    if (-not $Report.source) { $Report.success = $false; return }
    try {
        if (-not (Test-Path -LiteralPath $Report.source -PathType Leaf)) {
            throw 'The registered source is missing or is no longer a file.'
        }
        $Report.actualBytes = (Get-Item -LiteralPath $Report.source).Length
        $Report.sourceSha256 = Get-Sha256 $Report.source
        $Report.sourceVerified = $OracleFinalized -and $Report.actualBytes -eq $Report.expectedBytes -and $Report.sourceSha256 -eq $Report.oracleSha256
        if (-not $Report.sourceVerified) { throw 'Final source size/hash differs from the external incremental oracle.' }
    } catch { $Report.errors += ('Source verification: ' + $_.Exception.Message); $Report.success = $false }
}
function Text-FromCodePoints([int[]]$Values) {
    return -join ($Values | ForEach-Object { [char]$_ })
}
$viewName = Text-FromCodePoints @(0x89c6,0x56fe)
$tailName = Text-FromCodePoints @(0x8ddf,0x968f,0x6587,0x4ef6,0x672b,0x5c3e)
$resumeName = Text-FromCodePoints @(0x56de,0x5230,0x672b,0x5c3e)
$statusName = Text-FromCodePoints @(0x6587,0x4ef6,0x8ddf,0x968f,0x72b6,0x6001)
$statusPrefix = $statusName + [char]0xff1a
$followingText = Text-FromCodePoints @(0x6b63,0x5728,0x8ddf,0x968f,0x6587,0x4ef6,0x672b,0x5c3e)
$offText = Text-FromCodePoints @(0x6587,0x4ef6,0x8ddf,0x968f,0x5df2,0x5173,0x95ed)
$editorName = Text-FromCodePoints @(0x53ea,0x8bfb,0x6587,0x6863,0x5185,0x5bb9)

try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class TailSoakNative {
    private delegate bool EnumProc(IntPtr window, IntPtr ignored);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr ignored);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool GetWindowRect(IntPtr window, out Rect bounds);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool GetTitleBarInfo(IntPtr window, ref TitleBarInfo info);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] private static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetProcessIoCounters(IntPtr process, out IoCounters counters);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("psapi.dll", SetLastError=true)] private static extern bool GetProcessMemoryInfo(IntPtr process, out MemoryCounters counters, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
    [StructLayout(LayoutKind.Sequential)] public struct IoCounters {
        public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct TitleBarInfo {
        public uint Size;
        public Rect Bounds;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst=6)] public uint[] States;
    }
    [StructLayout(LayoutKind.Sequential)] public struct MemoryCounters {
        public uint Size, PageFaults;
        public UIntPtr PeakWorkingSet, WorkingSet, PeakPagedPool, PagedPool, PeakNonPagedPool, NonPagedPool, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    }
    [StructLayout(LayoutKind.Sequential)] private struct MemoryStatus {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, AvailableExtended;
    }
    public sealed class Pulse { public bool Responded; public double Milliseconds; }
    public sealed class CaptionPointProbe {
        public string Stage;
        public int X, Y;
        public long PointWindow;
        public uint PointPid;
        public bool CoordinatesValid, HitTestResponded, Accepted;
        public long HitTest;
        public int HitTestError;
    }
    public sealed class CaptionAttempt {
        public long OwnedWindow;
        public int OwnedPid;
        public bool Visible, Enabled, Minimized, Maximized, Positioned, TitleInfoRead;
        public int PositionError, TitleInfoError;
        public int[] WindowBounds, TitleBounds;
        public uint[] TitleStates;
        public List<CaptionPointProbe> Points = new List<CaptionPointProbe>();
    }
    public static CaptionAttempt LastCaptionAttempt { get; private set; }
    public static bool OwnedWindow(IntPtr window, int pid) {
        uint actual; GetWindowThreadProcessId(window, out actual);
        return window != IntPtr.Zero && actual == (uint)pid;
    }
    public static IntPtr[] VisibleWindows(int pid) {
        var windows = new List<IntPtr>();
        EnumWindows(delegate(IntPtr window, IntPtr ignored) {
            if (OwnedWindow(window, pid) && IsWindowVisible(window)) windows.Add(window);
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
    public static Pulse Heartbeat(IntPtr window, int pid) {
        if (!OwnedWindow(window, pid)) throw new InvalidOperationException("Owned window disappeared.");
        var watch = Stopwatch.StartNew(); UIntPtr ignored;
        bool ok = SendMessageTimeoutW(window, 0, UIntPtr.Zero, IntPtr.Zero, 3, 100, out ignored) != IntPtr.Zero;
        return new Pulse { Responded = ok, Milliseconds = watch.Elapsed.TotalMilliseconds };
    }
    private static void RequireForeground(int pid) {
        if (!OwnedWindow(GetForegroundWindow(), pid)) throw new InvalidOperationException("Foreground no longer belongs to the owned process; input aborted.");
    }
    private static bool IsOwnedCaptionPoint(IntPtr window, int pid, Point point, string stage) {
        var probe = new CaptionPointProbe { Stage = stage, X = point.X, Y = point.Y };
        LastCaptionAttempt.Points.Add(probe);
        IntPtr pointWindow = WindowFromPoint(point);
        probe.PointWindow = pointWindow.ToInt64();
        uint pointPid; GetWindowThreadProcessId(pointWindow, out pointPid); probe.PointPid = pointPid;
        // WM_NCHITTEST carries signed 16-bit screen coordinates. Reject an
        // unrepresentable point rather than accidentally testing another one.
        probe.CoordinatesValid = point.X >= short.MinValue && point.X <= short.MaxValue && point.Y >= short.MinValue && point.Y <= short.MaxValue;
        if (!probe.CoordinatesValid || !OwnedWindow(window, pid)) return false;
        int packed = unchecked((ushort)(short)point.X | ((int)(ushort)(short)point.Y << 16));
        UIntPtr hit;
        // Even when occluded, query only the original owned window so the
        // evidence distinguishes wrong geometry from an overlapping window.
        probe.HitTestResponded = SendMessageTimeoutW(window, 0x84, UIntPtr.Zero, new IntPtr(packed), 3, 100, out hit) != IntPtr.Zero;
        probe.HitTestError = probe.HitTestResponded ? 0 : Marshal.GetLastWin32Error();
        probe.HitTest = unchecked((long)hit.ToUInt64());
        probe.Accepted = probe.HitTestResponded && probe.HitTest == 2 && pointWindow == window
            && pointPid == (uint)pid && WindowFromPoint(point) == window && OwnedWindow(window, pid);
        return probe.Accepted; // HTCAPTION, never client/system menu/buttons.
    }
    public static void ClickOwnedCaption(IntPtr window, int pid) {
        var attempt = new CaptionAttempt { OwnedWindow = window.ToInt64(), OwnedPid = pid };
        LastCaptionAttempt = attempt;
        if (!OwnedWindow(window, pid)) throw new InvalidOperationException("Caption diagnostic refused a window outside the owned PID.");
        attempt.Visible = IsWindowVisible(window); attempt.Enabled = IsWindowEnabled(window);
        attempt.Minimized = IsIconic(window); attempt.Maximized = IsZoomed(window);
        Rect windowBounds;
        if (GetWindowRect(window, out windowBounds))
            attempt.WindowBounds = new int[] { windowBounds.Left, windowBounds.Top, windowBounds.Right, windowBounds.Bottom };
        if (!OwnedWindow(window, pid) || !attempt.Visible || !attempt.Enabled || attempt.Minimized)
            throw new InvalidOperationException("Owned caption activation requires the original visible, enabled, non-minimized window.");
        // HWND_TOP (not TOPMOST), NOSIZE | NOMOVE | NOACTIVATE. This only
        // exposes the owned title bar; it does not bypass foreground locking.
        attempt.Positioned = SetWindowPos(window, IntPtr.Zero, 0, 0, 0, 0, 0x13);
        attempt.PositionError = attempt.Positioned ? 0 : Marshal.GetLastWin32Error();
        if (!attempt.Positioned) throw new System.ComponentModel.Win32Exception(attempt.PositionError);
        var info = new TitleBarInfo { Size = (uint)Marshal.SizeOf(typeof(TitleBarInfo)), States = new uint[6] };
        attempt.TitleInfoRead = GetTitleBarInfo(window, ref info);
        attempt.TitleInfoError = attempt.TitleInfoRead ? 0 : Marshal.GetLastWin32Error();
        if (!attempt.TitleInfoRead) throw new System.ComponentModel.Win32Exception(attempt.TitleInfoError);
        attempt.TitleBounds = new int[] { info.Bounds.Left, info.Bounds.Top, info.Bounds.Right, info.Bounds.Bottom };
        attempt.TitleStates = info.States;
        if (info.Bounds.Right <= info.Bounds.Left || info.Bounds.Bottom <= info.Bounds.Top)
            throw new InvalidOperationException("Owned window has no usable native title bar.");
        foreach (double fraction in new double[] { 0.25, 0.4, 0.55, 0.7 }) {
            var point = new Point {
                X = info.Bounds.Left + (int)((info.Bounds.Right - info.Bounds.Left) * fraction),
                Y = info.Bounds.Top + (info.Bounds.Bottom - info.Bounds.Top) / 2
            };
            if (!IsOwnedCaptionPoint(window, pid, point, "BeforeMove")) continue;
            if (!SetCursorPos(point.X, point.Y)) throw new InvalidOperationException("Cannot position mouse over the owned title bar.");
            if (!IsOwnedCaptionPoint(window, pid, point, "AfterMove"))
                throw new InvalidOperationException("Owned caption point changed; activation click aborted.");
            // The only input path permitted without foreground ownership:
            // two exact HWND/PID/HTCAPTION checks immediately precede click.
            mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
            return;
        }
        throw new InvalidOperationException("No unobscured, verified caption point belongs to the owned window.");
    }
    public static void Click(int pid, int x, int y) {
        RequireForeground(pid);
        var point = new Point { X = x, Y = y };
        if (!OwnedWindow(WindowFromPoint(point), pid)) throw new InvalidOperationException("Click point no longer belongs to the owned process; input aborted.");
        if (!SetCursorPos(x, y)) throw new InvalidOperationException("Cannot position mouse.");
        RequireForeground(pid);
        if (!OwnedWindow(WindowFromPoint(point), pid)) throw new InvalidOperationException("Click point ownership changed; input aborted.");
        mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Escape(int pid) {
        RequireForeground(pid);
        keybd_event(0x1b, 0, 0, UIntPtr.Zero); keybd_event(0x1b, 0, 2, UIntPtr.Zero);
    }
    public static void CtrlKey(int pid, byte key) {
        RequireForeground(pid);
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        try { keybd_event(key, 0, 0, UIntPtr.Zero); keybd_event(key, 0, 2, UIntPtr.Zero); }
        finally { keybd_event(0x11, 0, 2, UIntPtr.Zero); }
    }
    public static IoCounters Io(IntPtr process) {
        IoCounters counters;
        if (!GetProcessIoCounters(process, out counters)) throw new System.ComponentModel.Win32Exception();
        return counters;
    }
    public static int ExitCode(IntPtr process) {
        uint code;
        if (!GetExitCodeProcess(process, out code)) throw new System.ComponentModel.Win32Exception();
        return unchecked((int)code);
    }
    public static MemoryCounters Memory(IntPtr process) {
        MemoryCounters counters;
        if (!GetProcessMemoryInfo(process, out counters, (uint)Marshal.SizeOf(typeof(MemoryCounters)))) throw new System.ComponentModel.Win32Exception();
        return counters;
    }
    public static ulong PhysicalMemory() {
        var status = new MemoryStatus(); status.Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
        if (!GlobalMemoryStatusEx(ref status)) throw new System.ComponentModel.Win32Exception();
        return status.TotalPhysical;
    }
}
'@
    [void][TailSoakNative]::SetProcessDPIAware()
    $report.physicalMemoryBytes = [TailSoakNative]::PhysicalMemory()
    $osKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion')
    if ($osKey) {
        try { $report.osBuild = '{0}.{1}' -f $osKey.GetValue('CurrentBuildNumber'), $osKey.GetValue('UBR') }
        finally { $osKey.Dispose() }
    }

    function Assert-Owned {
        if (-not $owned) { throw 'No process was started.' }
        $owned.Refresh()
        if ($owned.HasExited) { throw "Owned process exited unexpectedly: $($owned.ExitCode)" }
        if ($owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks) { throw 'Process start time changed; refusing PID reuse.' }
        if ($mainWindow -ne [IntPtr]::Zero -and -not [TailSoakNative]::OwnedWindow($mainWindow, $owned.Id)) {
            throw 'The recorded main window no longer belongs to the owned PID.'
        }
    }
    function Wait-Condition([scriptblock]$Predicate, [string]$Failure, [int]$Milliseconds = 10000) {
        $wait = [Diagnostics.Stopwatch]::StartNew()
        do {
            Assert-Owned
            if (& $Predicate) { return }
            Start-Sleep -Milliseconds 50
        } while ($wait.ElapsedMilliseconds -lt $Milliseconds)
        throw $Failure
    }
    function Find-OwnedElement([string]$Name, [string]$IdSuffix = '') {
        Assert-Owned
        foreach ($window in [TailSoakNative]::VisibleWindows($owned.Id)) {
            $root = [Windows.Automation.AutomationElement]::FromHandle($window)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $owned.Id)
            $elements = $root.FindAll([Windows.Automation.TreeScope]::Subtree, $condition)
            foreach ($element in $elements) {
                $state = $element.Current
                if ($state.ProcessId -ne $owned.Id -or $state.IsOffscreen) { continue }
                if ($IdSuffix) {
                    if ($state.AutomationId.EndsWith($IdSuffix, [StringComparison]::Ordinal)) { return $element }
                    continue
                }
                $normalized = Normalize-UiName $state.Name
                if ($normalized -eq $Name -or $state.HelpText -eq $Name) { return $element }
            }
        }
        return $null
    }
    function Normalize-UiName([string]$Name) {
        # Qt can expose the source label's (&V) mnemonic as (V) in UIA.
        # Remove only that trailing single-character mnemonic, not arbitrary
        # words or a different menu title such as View versus Display.
        return ($Name -replace '\s*\(&?[A-Za-z0-9]\)\s*$', '' -replace '&', '').Trim()
    }
    function Save-OwnedUiDiagnostic([string]$Reason) {
        Assert-Owned
        $nodes = New-Object 'System.Collections.Generic.List[object]'
        foreach ($window in [TailSoakNative]::VisibleWindows($owned.Id)) {
            $root = [Windows.Automation.AutomationElement]::FromHandle($window)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $owned.Id)
            foreach ($element in $root.FindAll([Windows.Automation.TreeScope]::Subtree, $condition)) {
                $state = $element.Current
                if ($state.ProcessId -ne $owned.Id) { continue }
                $type = $state.ControlType.ProgrammaticName
                # Never call ValuePattern/TextPattern or serialize an editor,
                # document, text, custom or pane Name: providers may use body
                # text there. Only known UI chrome and named probes are logged.
                $safeName = $type -in @('ControlType.Menu', 'ControlType.MenuBar', 'ControlType.MenuItem',
                    'ControlType.Button', 'ControlType.CheckBox', 'ControlType.ToolBar', 'ControlType.Window', 'ControlType.TabItem')
                $isStatusLabel = $state.AutomationId.EndsWith('.tailStatusLabel', [StringComparison]::Ordinal)
                $safeName = $safeName -or $isStatusLabel -or $state.Name -in @($statusName, $editorName)
                $name = if ($safeName) { $state.Name } else { '<content-name-redacted>' }
                $node = [ordered]@{
                    windowHandle = $window.ToInt64(); processId = $state.ProcessId
                    name = $name; normalizedName = $(if ($safeName) { Normalize-UiName $name } else { $name })
                    type = $type; enabled = $state.IsEnabled; offscreen = $state.IsOffscreen
                    hasKeyboardFocus = $state.HasKeyboardFocus; keyboardFocusable = $state.IsKeyboardFocusable
                    controlId = $state.AutomationId; className = $state.ClassName
                    nativeHandle = $state.NativeWindowHandle; runtimeId = ($element.GetRuntimeId() -join '.')
                }
                if ($isStatusLabel -or $state.Name -eq $statusName) {
                    # These two named status controls expose product status,
                    # not document contents. Never read other controls' help.
                    $node.helpText = $state.HelpText
                    $fullDescription = [Windows.Automation.AutomationElement].GetField('FullDescriptionProperty')
                    $node.fullDescription = '<property unavailable in this UIA assembly>'
                    if ($fullDescription) {
                        try {
                            $value = $element.GetCurrentPropertyValue($fullDescription.GetValue($null), $true)
                            $node.fullDescription = if ($value -eq [Windows.Automation.AutomationElement]::NotSupported) { '<not supported>' } else { [string]$value }
                        } catch { $node.fullDescription = '<read failed: ' + $_.Exception.Message + '>' }
                    }
                }
                if ($node.normalizedName -in @($viewName, $tailName, $resumeName)) {
                    $pattern = $null
                    $node.toggleState = '<pattern unavailable>'
                    if ($element.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
                        $node.toggleState = $pattern.Current.ToggleState.ToString()
                    }
                    $pattern = $null
                    $node.expandCollapseState = '<pattern unavailable>'
                    if ($element.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pattern)) {
                        $node.expandCollapseState = $pattern.Current.ExpandCollapseState.ToString()
                    }
                }
                $nodes.Add([pscustomobject]$node)
            }
        }
        $path = Join-Path $evidence ('uia-targets-{0}.json' -f ($report.uiaDiagnostics.Count + 1))
        $foreground = [TailSoakNative]::GetForegroundWindow()
        $foregroundOwned = [TailSoakNative]::OwnedWindow($foreground, $owned.Id)
        $snapshot = [ordered]@{
            reason = $Reason; utc = [DateTime]::UtcNow.ToString('o'); ownedPid = $owned.Id
            expectedViewName = $viewName; foregroundOwned = $foregroundOwned
            foregroundHandle = $(if ($foregroundOwned) { $foreground.ToInt64() } else { 0 })
            nodes = @($nodes.ToArray())
        }
        [IO.File]::WriteAllText($path, ($snapshot | ConvertTo-Json -Depth 6), $utf8)
        $report.uiaDiagnostics += $path
        Save-Report
    }
    function Click-OwnedElement($Element) {
        Assert-Owned
        if (-not $Element) { throw 'UIA click target is absent.' }
        $state = $Element.Current
        if ($state.ProcessId -ne $owned.Id -or -not $state.IsEnabled -or $state.IsOffscreen) {
            throw 'UIA click target is not visible/enabled in the owned process.'
        }
        $bounds = $state.BoundingRectangle
        if ($bounds.IsEmpty -or $bounds.Width -le 0 -or $bounds.Height -le 0) {
            throw 'UIA click target has no usable real bounds.'
        }
        [TailSoakNative]::Click($owned.Id, [int]($bounds.X + $bounds.Width / 2), [int]($bounds.Y + $bounds.Height / 2))
    }
    function Get-ViewExpansion {
        $view = Find-OwnedElement $viewName
        $pattern = $null
        if (-not $view -or -not $view.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pattern)) {
            throw 'Owned View menu does not expose an observable expanded/collapsed state.'
        }
        return $pattern.Current.ExpandCollapseState.ToString()
    }
    function Activate-OwnedWindow {
        Assert-Owned
        $record = [ordered]@{
            method = 'SetForegroundWindow'; nativeReturn = $null
            foregroundBefore = [TailSoakNative]::GetForegroundWindow().ToInt64()
            foregroundAfter = $null; captionClicked = $false; captionAttempt = $null; complete = $false
        }
        $report.activations += $record
        try {
            $record.nativeReturn = [TailSoakNative]::SetForegroundWindow($mainWindow)
            if ([TailSoakNative]::GetForegroundWindow() -ne $mainWindow) {
                Assert-Owned
                $record.method = 'SetForegroundWindow+OwnedCaptionClick'
                [TailSoakNative]::ClickOwnedCaption($mainWindow, $owned.Id)
                $record.captionClicked = $true
            }
            Wait-Condition { [TailSoakNative]::GetForegroundWindow() -eq $mainWindow } 'Cannot foreground the owned main window.' 3000
            $record.complete = $true
        } finally {
            if ($record.method -eq 'SetForegroundWindow+OwnedCaptionClick') {
                $record.captionAttempt = [TailSoakNative]::LastCaptionAttempt
            }
            $record.foregroundAfter = [TailSoakNative]::GetForegroundWindow().ToInt64()
            Save-Report
        }
    }
    function Dismiss-OwnedMenu([string]$Reason) {
        $before = Get-ViewExpansion
        $record = [pscustomobject]@{ reason = $Reason; before = $before; escapeSent = $false; after = $null; complete = $false }
        $report.menuCleanup += $record
        try {
            if ($before -in @('Expanded', 'PartiallyExpanded')) {
                Activate-OwnedWindow
                if ((Get-ViewExpansion) -in @('Expanded', 'PartiallyExpanded')) {
                    [TailSoakNative]::Escape($owned.Id)
                    $record.escapeSent = $true
                }
            }
            Wait-Condition { (Get-ViewExpansion) -eq 'Collapsed' } 'Owned View menu did not collapse before close.' 3000
            $record.after = Get-ViewExpansion
            $record.complete = $true
        } finally { Save-Report }
    }
    function Click-TailMenu {
        try {
            Activate-OwnedWindow
            $view = Find-OwnedElement $viewName
            Save-OwnedUiDiagnostic 'Before clicking the View menu'
            Click-OwnedElement $view
            $script:tailMenuElement = $null
            Wait-Condition { $script:tailMenuElement = Find-OwnedElement $tailName; return $null -ne $script:tailMenuElement } 'Tail menu command not exposed by the formal EXE.' 3000
            Click-OwnedElement $script:tailMenuElement
            $script:tailMenuElement = $null
            Wait-Condition { (Get-ViewExpansion) -eq 'Collapsed' } 'View menu did not collapse after the real Tail click.' 3000
            Save-OwnedUiDiagnostic 'After clicking Tail and confirming View collapsed'
        } catch {
            $failure = $_
            try { Save-OwnedUiDiagnostic ('Menu click failed: ' + $failure.Exception.Message) }
            catch { $report.errors += ('UIA diagnostic: ' + $_.Exception.Message) }
            throw $failure
        }
    }
    function Read-TailStatus {
        Assert-Owned
        if ($null -eq $script:statusElement) {
            $candidate = Find-OwnedElement '' '.tailStatusLabel'
            if (-not $candidate) { return $null }
            $state = $candidate.Current
            if ($state.ProcessId -ne $owned.Id -or $state.IsOffscreen -or
                -not $state.AutomationId.EndsWith('.tailStatusLabel', [StringComparison]::Ordinal)) {
                throw 'The initial Tail status label failed exact identity/visibility validation.'
            }
            $script:statusElementId = $state.AutomationId
            $script:statusElement = $candidate
            $report.statusElementIdentity = [ordered]@{
                processId = $state.ProcessId; automationId = $state.AutomationId
                runtimeId = ($candidate.GetRuntimeId() -join '.')
            }
            Save-Report
        }
        # Keep this exact bounded reference. UIA element invalidation propagates
        # as a capture failure; never rediscover a substitute during sampling.
        $state = $script:statusElement.Current
        if ($state.ProcessId -ne $owned.Id -or $state.IsOffscreen -or
            -not [string]::Equals($state.AutomationId, $script:statusElementId, [StringComparison]::Ordinal)) {
            throw 'The bound Tail status label identity or visibility changed.'
        }
        $name = $state.Name
        if (-not $name.StartsWith($statusPrefix, [StringComparison]::Ordinal)) {
            throw 'The exact Tail status label does not expose the required product-state Name prefix.'
        }
        return $name.Substring($statusPrefix.Length)
    }
    function Assert-TailStatus([string]$Expected) {
        $status = Read-TailStatus
        if (-not $status -or -not $status.StartsWith($Expected, [StringComparison]::Ordinal)) {
            $actual = if ($status) { $status } else { '<not exposed>' }
            throw "UIA did not confirm expected Tail status '$Expected': $actual"
        }
    }

    $bin = Join-Path $evidence 'binary'
    $runProfile = Join-Path $evidence 'profile'
    [void][IO.Directory]::CreateDirectory($bin)
    [void][IO.Directory]::CreateDirectory($runProfile)
    $copiedExe = Join-Path $bin 'notepad-safe.exe'
    $report.binarySha256 = Get-Sha256 $inputExe
    [IO.File]::Copy($inputExe, $copiedExe, $false)
    $report.copiedExe = $copiedExe
    $report.copiedBinarySha256 = Get-Sha256 $copiedExe
    if ($report.binarySha256 -ne $report.copiedBinarySha256) { throw 'Copied binary hash mismatch.' }
    $report.source = Join-Path $evidence ('tail-' + $runId + '.log')
    $block = New-Object byte[] 131072
    $line = [Text.Encoding]::ASCII.GetBytes(('x' * 126) + "`r`n")
    for ($offset = 0; $offset -lt $block.Length; $offset += 128) { [Array]::Copy($line, 0, $block, $offset, 128) }
    function New-SourceChunk([string]$Marker) {
        $chunk = [byte[]]$block.Clone()
        $end = [Text.Encoding]::ASCII.GetBytes($Marker.PadRight(126, '_') + "`r`n")
        if ($end.Length -ne 128) { throw 'EOF marker exceeds the fixed complete line.' }
        [Array]::Copy($end, 0, $chunk, $chunk.Length - 128, 128)
        return ,$chunk
    }
    $oracle = [Security.Cryptography.SHA256]::Create()
    $writer = New-Object IO.FileStream($report.source, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
    for ($index = 0; $index -lt 264; ++$index) {
        $chunk = if ($index -eq 263) { New-SourceChunk ('TAIL_INITIAL_EOF_' + $runId) } else { $block }
        $writer.Write($chunk, 0, $chunk.Length)
        [void]$oracle.TransformBlock($chunk, 0, $chunk.Length, $chunk, 0)
    }
    $writer.Flush($true)
    $writer.Dispose(); $writer = $null
    $oldDataRoot = $env:NOTEPADSAFE_DATADIR
    try {
        $env:NOTEPADSAFE_DATADIR = $runProfile
        $owned = Start-Process -FilePath $copiedExe -ArgumentList ('"' + $report.source + '"') -WorkingDirectory $bin -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $evidence 'stdout.log') -RedirectStandardError (Join-Path $evidence 'stderr.log')
        $ownedStartTicks = $owned.StartTime.ToUniversalTime().Ticks
        # Keep a real process handle alive before exit; Start-Process's wrapper
        # can otherwise return an empty ExitCode after process lookup expires.
        $tracked = [Diagnostics.Process]::GetProcessById($owned.Id)
        if ($tracked.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks) {
            $tracked.Dispose(); throw 'Launch tracking refused PID reuse.'
        }
        [void]$tracked.Handle
        $owned.Dispose(); $owned = $tracked
    } finally {
        if ($null -eq $oldDataRoot) { Remove-Item Env:NOTEPADSAFE_DATADIR -ErrorAction SilentlyContinue }
        else { $env:NOTEPADSAFE_DATADIR = $oldDataRoot }
    }
    $report.processId = $owned.Id
    $report.processStartUtc = $owned.StartTime.ToUniversalTime().ToString('o')
    Save-Report
    Wait-Condition { $owned.Refresh(); return $owned.MainWindowHandle -ne [IntPtr]::Zero } 'Formal EXE did not expose a main window.'
    $mainWindow = $owned.MainWindowHandle
    $script:editorElement = $null
    Wait-Condition { $script:editorElement = Find-OwnedElement $editorName; return $null -ne $script:editorElement } 'Real read-only document content was not exposed.'
    Activate-OwnedWindow
    $bounds = $script:editorElement.Current.BoundingRectangle
    [TailSoakNative]::Click($owned.Id, [int]($bounds.X + $bounds.Width / 2), [int]($bounds.Y + $bounds.Height / 2))
    [TailSoakNative]::CtrlKey($owned.Id, 0x23) # Ctrl+End, actual editor input.
    Click-TailMenu
    Wait-Condition {
        $status = Read-TailStatus
        return $status -and $status.StartsWith($followingText, [StringComparison]::Ordinal)
    } 'Tail did not enter Following after Ctrl+End and the real menu command.' 10000
    $script:editorElement = $null
    $csv = New-Object IO.StreamWriter((Join-Path $evidence 'samples.csv'), $false, $utf8)

    function Write-Sample([string]$Phase, [double]$PhaseSeconds) {
        Assert-Owned
        $at = $clock.Elapsed.TotalSeconds
        $cpu = $owned.TotalProcessorTime.TotalSeconds
        $cpuPercent = $null
        if ($null -ne $script:previousCpu -and $at -gt $script:previousSampleTime) {
            $cpuPercent = 100 * ($cpu - $script:previousCpu) / ($at - $script:previousSampleTime)
        }
        $pulse = [TailSoakNative]::Heartbeat($mainWindow, $owned.Id)
        $io = [TailSoakNative]::Io($owned.Handle)
        $memory = [TailSoakNative]::Memory($owned.Handle)
        $row = [pscustomobject][ordered]@{
            Utc = [DateTime]::UtcNow.ToString('o'); Phase = $Phase; PhaseSeconds = $PhaseSeconds
            ElapsedSeconds = $at; CpuTotalSeconds = $cpu; CpuPercentOneCore = $cpuPercent
            CpuPercentMachine = $(if ($null -eq $cpuPercent) { $null } else { $cpuPercent / [Environment]::ProcessorCount })
            WorkingSetBytes = $owned.WorkingSet64; PrivateBytes = $owned.PrivateMemorySize64
            CommitBytes = $memory.PrivateUsage.ToUInt64(); PeakCommitBytes = $memory.PeakPagefileUsage.ToUInt64()
            PeakWorkingSetBytes = $owned.PeakWorkingSet64; Handles = $owned.HandleCount; Threads = $owned.Threads.Count
            ReadOperations = $io.ReadOperations; WriteOperations = $io.WriteOperations; OtherOperations = $io.OtherOperations
            ReadBytes = $io.ReadBytes; WriteBytes = $io.WriteBytes; OtherBytes = $io.OtherBytes
            MessagePumpResponded = $pulse.Responded; MessagePumpMs = $pulse.Milliseconds
            AppendWrites = $report.appendWrites; ExpectedSourceBytes = $report.expectedBytes
        }
        $lines = @($row | ConvertTo-Csv -NoTypeInformation)
        if (-not $script:headerWritten) { $csv.WriteLine($lines[0]); $script:headerWritten = $true }
        $csv.WriteLine($lines[1]); $csv.Flush()
        $script:previousCpu = $cpu; $script:previousSampleTime = $at
        if (-not $pulse.Responded) { throw 'Owned message pump failed the bounded WM_NULL probe; sample retained.' }
    }
    function Sample-Phase([string]$Name, [int]$Seconds, [bool]$Append) {
        $phase = [ordered]@{
            name = $Name; requestedSeconds = $Seconds; actualSeconds = 0; samples = 0
            complete = $false; maximumScheduleLatenessMs = 0
            lastSamplePhaseSeconds = $null; endToLastSampleSeconds = $null
        }
        $report.phases += $phase
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $nextSample = 0.0
        $nextStatus = 0.0
        try {
            while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
                $now = $watch.Elapsed.TotalSeconds
                if ($now -lt $nextSample) { Start-Sleep -Milliseconds ([int][Math]::Min(200, [Math]::Max(1, ($nextSample - $now) * 1000))); continue }
                $phase.maximumScheduleLatenessMs = [Math]::Max($phase.maximumScheduleLatenessMs, ($now - $nextSample) * 1000)
                if ($Append -and $phase.samples -gt 0) {
                    $sequence = $report.appendWrites + 1
                    $chunk = New-SourceChunk ('TAIL_APPEND_' + $sequence + '_' + $runId)
                    $writer.Write($chunk, 0, $chunk.Length); $writer.Flush($true)
                    [void]$oracle.TransformBlock($chunk, 0, $chunk.Length, $chunk, 0)
                    $report.appendWrites = $sequence; $report.expectedBytes += $chunk.Length
                }
                Write-Sample $Name $now
                ++$phase.samples
                $phase.lastSamplePhaseSeconds = $now
                if ($now -ge $nextStatus) {
                    if ($Name -eq 'Following') { Assert-TailStatus $followingText }
                    elseif ($Name -eq 'TailOff') { Assert-TailStatus $offText }
                    elseif (Find-OwnedElement '' '.tailStatusLabel') { throw "Tail status remained visible during $Name." }
                    $nextStatus = $now + 5
                }
                # Record actual scheduling, never burst catch-up writes after a stall.
                $nextSample = $now + 1
            }
            $phase.complete = $true
        } finally {
            $phase.actualSeconds = $watch.Elapsed.TotalSeconds
            if ($null -ne $phase.lastSamplePhaseSeconds) {
                $phase.endToLastSampleSeconds = $phase.actualSeconds - $phase.lastSamplePhaseSeconds
            }
            Save-Report
        }
    }

    if ($Mode -eq 'Append') {
        $writer = New-Object IO.FileStream($report.source, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
    }
    Sample-Phase 'Following' $DurationSeconds ($Mode -eq 'Append')
    if ($writer) { $writer.Dispose(); $writer = $null }
    Assert-TailStatus $followingText
    Click-TailMenu
    Wait-Condition {
        $status = Read-TailStatus
        return $status -and $status.StartsWith($offText, [StringComparison]::Ordinal)
    } 'Tail did not turn off through the menu.' 3000
    Sample-Phase 'TailOff' $RecoverySeconds $false
    Activate-OwnedWindow
    [TailSoakNative]::CtrlKey($owned.Id, 0x57) # Ctrl+W, actual document close.
    Wait-Condition { $null -eq (Find-OwnedElement '' '.tailStatusLabel') -and $null -eq (Find-OwnedElement $editorName) } 'Closing the source tab did not remove its Tail/read-only controls.' 3000
    Sample-Phase 'TabClosed' $RecoverySeconds $false
    Assert-Owned
    Dismiss-OwnedMenu 'Normal application close'
    if (-not $owned.CloseMainWindow() -or -not $owned.WaitForExit(10000)) { throw 'Owned application did not close normally.' }
    $owned.Refresh(); $report.exitCode = [TailSoakNative]::ExitCode($owned.Handle)
    $report.gracefulExit = $true
    if ($report.exitCode -ne 0) { throw 'Owned application exited with a nonzero code.' }
    $report.success = $true
} catch {
    $report.errors += $_.Exception.Message
    $report.errors += $_.ScriptStackTrace
    # Preserve the final real status and popup/focus state before cleanup can
    # close menus or the application. Diagnostic failure cannot mask the cause.
    if ($owned -and (Get-Command Save-OwnedUiDiagnostic -ErrorAction SilentlyContinue)) {
        try { Save-OwnedUiDiagnostic 'Run failed, before process cleanup' }
        catch { $report.errors += ('Final UIA diagnostic: ' + $_.Exception.Message) }
    }
} finally {
    if ($writer) {
        try { $writer.Dispose() }
        catch { $report.errors += ('Source writer cleanup: ' + $_.Exception.Message); $report.success = $false }
    }
    if ($csv) {
        try { $csv.Dispose() }
        catch { $report.errors += ('CSV cleanup: ' + $_.Exception.Message); $report.success = $false }
    }
    if ($owned) {
        try {
            $owned.Refresh()
            if (-not $owned.HasExited) {
                if ($owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks) { throw 'Cleanup refused: PID/start time mismatch.' }
                if (Get-Command Dismiss-OwnedMenu -ErrorAction SilentlyContinue) {
                    try { Dismiss-OwnedMenu 'Failure cleanup before normal close' }
                    catch { $report.errors += ('Menu cleanup: ' + $_.Exception.Message) }
                }
                if (-not $owned.CloseMainWindow() -or -not $owned.WaitForExit(5000)) {
                    $owned.Kill(); $report.forcedTermination = $true
                    if (-not $owned.WaitForExit(5000)) { throw 'Owned process did not terminate after cleanup kill.' }
                } else {
                    $report.gracefulExit = $true
                }
            }
            $owned.Refresh(); $report.exitCode = [TailSoakNative]::ExitCode($owned.Handle)
        } catch { $report.errors += ('Cleanup: ' + $_.Exception.Message); $report.success = $false }
        finally { $owned.Dispose() }
    }
    if ($oracle) {
        try {
            [void]$oracle.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            $report.oracleSha256 = [BitConverter]::ToString($oracle.Hash).Replace('-', '').ToLowerInvariant()
            $oracleFinalized = $true
        } catch { $report.errors += ('Oracle: ' + $_.Exception.Message); $report.success = $false }
        finally { $oracle.Dispose() }
    }
    Confirm-SourceEvidence $report $oracleFinalized
    $report.success = $report.success -and $report.sourceVerified
    $report.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $report.actualTotalSeconds = $clock.Elapsed.TotalSeconds
    if ($report.success -and $report.sourceVerified -and $DurationSeconds -ge 1800 -and $report.phases.Count -eq 3) { $report.classification = 'capture-complete-needs-review' }
    Save-Report
}
Write-Output (Join-Path $evidence 'summary.json')
if (-not $report.success) { throw ('Tail resource run failed; evidence retained: ' + ($report.errors -join ' | ')) }
