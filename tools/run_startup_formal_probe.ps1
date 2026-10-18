[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [ValidateSet('Empty')][string]$Mode = 'Empty'
)

# Windows PowerShell 5.1, ASCII source. One formal-EXE Empty validity pilot.
# No sample loop, credentials, document fixture, product hook, or performance verdict.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
$strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
$budgetMs = 10000

function Local-AbsolutePath([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:[\\/]') { throw 'Use an absolute local drive path; UNC and relative paths are forbidden.' }
    return [IO.Path]::GetFullPath($Path)
}
function Reject-ReparseAncestors([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if (((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Reparse-point paths are forbidden.'
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
}
function Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function Save-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), $utf8)
}
function Exact-Keys($Object, [string[]]$Names) {
    if ($null -eq $Object) { throw 'Expected a JSON object, not null.' }
    $actual = @($Object.PSObject.Properties | ForEach-Object { $_.Name })
    if ($actual.Count -ne $Names.Count) { throw 'Unexpected JSON property count.' }
    foreach ($name in $Names) {
        if (-not ($actual -ccontains $name)) { throw "Missing exact JSON property: $name" }
    }
}
function Decimal-Ticks($Value) {
    if ($Value -isnot [string] -or $Value -notmatch '^[0-9]+$') { throw 'QPC values must be decimal integer strings.' }
    $parsed = 0L
    if (-not [long]::TryParse($Value, [ref]$parsed) -or $parsed -le 0) { throw 'QPC value is outside positive Int64 range.' }
    return [decimal]$parsed
}
function Parse-StartupTraceLine($Line) {
    if ($Line -isnot [string] -or $Line.Length -gt 65536 -or $Line -match '[^\x20-\x7e]') {
        throw 'Expected one bounded printable ASCII JSON line.'
    }
    # The fixed producer uses only literal ASCII keys, fixed stage/schema
    # strings and decimal ticks. Escapes are unnecessary and could hide a
    # duplicate key before ConvertFrom-Json merges its object projection.
    if ($Line.Contains('\')) { throw 'Escaped startup trace text is forbidden.' }
    $raw = $Line.Trim()
    if (-not $raw.StartsWith('{') -or -not $raw.EndsWith('}')) { throw 'Expected a single JSON object.' }
    if ([regex]::Matches($raw, '"[^"\\]*"\s*:').Count -ne 35) { throw 'Unexpected raw JSON key count or duplicate property.' }
    $parsed = $raw | ConvertFrom-Json -ErrorAction Stop
    if ($parsed -isnot [System.Management.Automation.PSCustomObject]) { throw 'Expected a single JSON object.' }
    return $parsed
}
function Check-StartupTrace($Trace, [int]$OwnedId, [long]$Frequency, [int]$ActualExit,
                            [decimal]$ParentStart, [decimal]$ParentExit) {
    Exact-Keys $Trace @('schema', 'pid', 'qpc_frequency', 'exit_code', 'invalid_clock',
        'overflow', 'duplicate_stage', 'invalid_stage', 'complete', 'valid', 'paint_counts', 'events')
    if ($Trace.schema -isnot [string] -or $Trace.schema -cne 'startup-trace-v1') { throw 'Unexpected startup trace schema.' }
    if (($Trace.pid -isnot [int] -and $Trace.pid -isnot [long]) -or $Trace.pid -ne $OwnedId) { throw 'Trace PID mismatch.' }
    if (($Trace.exit_code -isnot [int] -and $Trace.exit_code -isnot [long]) -or
        $ActualExit -ne 0 -or $Trace.exit_code -ne $ActualExit) { throw 'Trace/process exit is not a matching normal zero.' }
    foreach ($name in 'invalid_clock', 'overflow', 'duplicate_stage', 'invalid_stage', 'complete', 'valid') {
        if ($Trace.$name -isnot [bool]) { throw "Invalid boolean trace field: $name" }
    }
    if ($Trace.invalid_clock -or $Trace.overflow -or $Trace.duplicate_stage -or $Trace.invalid_stage -or
        -not $Trace.complete -or -not $Trace.valid) { throw 'Trace reports an incomplete or invalid observation.' }
    if ((Decimal-Ticks $Trace.qpc_frequency) -ne [decimal]$Frequency) { throw 'Parent/child QPC frequencies differ.' }
    Exact-Keys $Trace.paint_counts @('heavy', 'tools', 'documents', 'persistence', 'inference')
    foreach ($name in 'heavy', 'tools', 'documents', 'persistence', 'inference') {
        $count = $Trace.paint_counts.$name
        if (($count -isnot [int] -and $count -isnot [long]) -or $count -ne 0) { throw "Nonzero/invalid first-paint count: $name" }
    }
    $names = @('application_ready', 'composition_ready', 'window_ready', 'show_begin', 'show_returned',
        'first_paint', 'post_paint_idle', 'restore_completed', 'event_loop_finished')
    if ($Trace.events -isnot [array] -or $Trace.events.Count -ne 9) { throw 'Expected exactly nine recorded events.' }
    $seen = @{}
    $positions = @{}
    $previous = $ParentStart
    foreach ($event in $Trace.events) {
        Exact-Keys $event @('stage', 'ticks')
        if ($event.stage -isnot [string] -or -not ($names -ccontains $event.stage) -or $seen.ContainsKey($event.stage)) {
            throw 'Unexpected or duplicate event stage.'
        }
        $tick = Decimal-Ticks $event.ticks
        if ($tick -lt $previous -or $tick -gt $ParentExit) { throw 'Events are not monotonic within the owned process interval.' }
        $positions[$event.stage] = $seen.Count
        $seen[$event.stage] = $tick
        $previous = $tick
    }
    foreach ($pair in @(@('application_ready', 'composition_ready'), @('composition_ready', 'window_ready'),
        @('window_ready', 'show_begin'), @('show_begin', 'show_returned'), @('show_begin', 'first_paint'),
        @('first_paint', 'post_paint_idle'), @('show_returned', 'post_paint_idle'),
        @('post_paint_idle', 'restore_completed'), @('restore_completed', 'event_loop_finished'))) {
        if ($seen[$pair[1]] -lt $seen[$pair[0]] -or $positions[$pair[1]] -le $positions[$pair[0]]) {
            throw 'Trace stages violate the actual startup partial order.'
        }
    }
    # first_paint can occur inside show(): do not impose an order against show_returned.
    return $seen
}
function Normalize-MenuName([string]$Name) {
    return ($Name -replace '\s*\(&?[A-Za-z0-9]\)\s*$', '' -replace '&', '').Trim()
}
function Qpc-Milliseconds([decimal]$From, [decimal]$To, [long]$Frequency) {
    if ($To -lt $From -or $Frequency -le 0) { throw 'Invalid observer timing interval.' }
    return [double](($To - $From) * [decimal]1000 / [decimal]$Frequency)
}

if ($env:OS -ne 'Windows_NT' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'Windows PowerShell 5.1 is required.' }
$inputExe = Local-AbsolutePath $ExePath
$evidence = Local-AbsolutePath $EvidenceDirectory
if (-not (Test-Path -LiteralPath $inputExe -PathType Leaf) -or [IO.Path]::GetFileName($inputExe) -ine 'notepad-safe.exe') {
    throw 'ExePath must identify the formal notepad-safe.exe.'
}
if (Test-Path -LiteralPath $evidence) { throw 'EvidenceDirectory must be new; existing evidence is never overwritten.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $evidence) -PathType Container)) { throw 'EvidenceDirectory parent must already exist.' }
if ($evidence.TrimEnd('\', '/') -eq [IO.Path]::GetPathRoot($evidence).TrimEnd('\', '/')) { throw 'EvidenceDirectory cannot be a drive root.' }
Reject-ReparseAncestors $inputExe
Reject-ReparseAncestors $evidence
[void][IO.Directory]::CreateDirectory($evidence)
$profile = Join-Path $evidence 'profile'
[void][IO.Directory]::CreateDirectory($profile)
$stdout = Join-Path $evidence 'stdout.log'
$stderr = Join-Path $evidence 'stderr.log'
$observerPath = Join-Path $evidence 'observer.json'
$ownershipPath = Join-Path $evidence 'ownership.json'
$menuPng = Join-Path $evidence 'file-menu.png'
$report = [ordered]@{
    schema = 'startup-formal-probe-v1'; mode = $Mode; samples = 1; classification = 'incomplete-pilot'
    observation_valid = $false; performance_acceptance = 'not-evaluated-single-pilot'; pixel_review = 'pending-root-visual-review'
    started_utc = [DateTime]::UtcNow.ToString('o'); finished_utc = $null
    exe = $inputExe; exe_sha256 = (Sha256 $inputExe); exe_sha256_after = $null; binary_stable = $false
    profile = $profile; stdout = $stdout; stderr = $stderr; ownership = $ownershipPath
    pid = $null; process_start_utc_ticks = $null; retained_handle = $null; exit_code = $null
    normal_exit = $false; forced_termination = $false; process_creation_qpc = $null; process_exit_observed_qpc = $null
    qpc_frequency = [Diagnostics.Stopwatch]::Frequency.ToString(); window_visible_observed_qpc = $null
    input_before_qpc = $null; input_after_qpc = $null; capture_before_qpc = $null; capture_after_qpc = $null
    main_hwnd = $null; menu_hwnd = $null; menu_click = $null; menu_rectangle = $null; menu_image = $null; menu_frames = @()
    foreground_requested = $null; discovered_windows = @(); trace = $null; timings_ms = $null
    budget_ms = $budgetMs; process_interval_ms = $null; errors = @()
    deadline_definition = '10000 ms checked before/after synchronous UIA/native calls. No hard preemption of a stuck COM call; ownership.json enables exact external supervision. Cleanup can take an additional 2000 ms.'
    presentation_definition = 'Real guarded mouse input and only the owned, unobscured QMenu rectangle captured. Screenshot completion is an observation upper bound, not a DWM presentation timestamp. UIA visibility alone is never a presentation verdict.'
    timing_definition = 'No observer overhead is subtracted. A paint-to-capture upper bound above 100 ms is inconclusive about product responsiveness, not a product failure. One sample cannot establish 500/1500 ms P95 gates.'
    frame_definition = 'Frame 0 retains the original timing sample. Three diagnostic frames follow, each after a fixed 50 ms wait; actual QPC intervals include observation work. No hash/clarity stopping rule or animation setting change.'
}
Save-Json $observerPath $report
$owned = $null
$ownedStartTicks = 0L
$ownedHandle = [IntPtr]::Zero
$mainHwnd = [IntPtr]::Zero
$mainElement = $null
$fileItem = $null
$menuHwnd = [IntPtr]::Zero
$menuElement = $null
$processClock = $null
$observedExit = $false
$frequency = [Diagnostics.Stopwatch]::Frequency
$fileName = -join @([char]0x6587, [char]0x4ef6)
# The menu bar is hidden; menus open from the title bar's main menu button.
$mainMenuName = -join @([char]0x4e3b, [char]0x83dc, [char]0x5355)
$newName = -join @([char]0x65b0, [char]0x5efa)

function Assert-Budget([string]$Stage) {
    if ($processClock -and $processClock.Elapsed.TotalMilliseconds -ge $budgetMs) { throw "Observer exceeded its checked 10000 ms process budget at $Stage." }
}
function Assert-Owned {
    if (-not $owned) { throw 'No owned process.' }
    $owned.Refresh()
    if ($owned.HasExited) { throw 'Owned process exited before the observation completed.' }
    if ($owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks -or $owned.Handle -ne $ownedHandle) {
        throw 'Owned process identity changed; refusing PID reuse.'
    }
}
function Wait-Observed([scriptblock]$Predicate, [string]$Stage) {
    do {
        Assert-Budget $Stage
        Assert-Owned
        $ready = & $Predicate
        Assert-Budget $Stage
        if ($ready) { return }
        Start-Sleep -Milliseconds 10
    } while ($true)
}
function Owned-MenuItems($Root, $Scope) {
    Assert-Budget 'menu-items-before'
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::MenuItem)
    $items = $Root.FindAll($Scope, $condition)
    Assert-Budget 'menu-items-after'
    $result = @()
    foreach ($item in $items) {
        $state = $item.Current
        Assert-Budget 'menu-item-state'
        if ($state.ProcessId -eq $owned.Id -and -not $state.IsOffscreen) { $result += $item }
    }
    return $result
}
function Capture-OwnedMenuFrame([int]$Index, [string]$Path) {
    Assert-Owned
    Assert-Budget 'capture-uia-before'
    $state = $menuElement.Current
    Assert-Budget 'capture-uia-state'
    if ($state.ProcessId -ne $owned.Id -or $state.NativeWindowHandle -ne $menuHwnd.ToInt64() -or $state.IsOffscreen -or $state.ClassName -cne 'QMenu' -or
        $state.ControlType -ne [Windows.Automation.ControlType]::Menu) { throw 'Same owned QMenu is unavailable before capture.' }
    $uia = $state.BoundingRectangle
    $native = [FormalStartupNative]::Bounds($menuHwnd, $owned.Id)
    $area = [int[]]@([Math]::Max($native[0], [Math]::Ceiling($uia.Left)), [Math]::Max($native[1], [Math]::Ceiling($uia.Top)),
        [Math]::Min($native[2], [Math]::Floor($uia.Right)), [Math]::Min($native[3], [Math]::Floor($uia.Bottom)))
    $frame = [ordered]@{
        index = $Index; menu_hwnd = $menuHwnd.ToInt64().ToString(); rectangle = $area; png = $Path
        capture_before_qpc = $null; pixel_copy_before_qpc = $null; pixel_copy_after_qpc = $null; capture_after_qpc = $null
        previous_pixel_copy_interval_ms = $null
    }
    $report.menu_frames += $frame
    if ($Index -eq 0) { $report.menu_hwnd = $frame.menu_hwnd; $report.menu_rectangle = $area }
    Assert-Owned
    Assert-Budget 'capture-before'
    $frame.capture_before_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    if ($Index -eq 0) { $report.capture_before_qpc = $frame.capture_before_qpc }
    [FormalStartupNative]::RequireCapture($menuHwnd, $owned.Id, $area)
    $bitmap = $null; $graphics = $null
    try {
        $bitmap = New-Object Drawing.Bitmap(($area[2] - $area[0]), ($area[3] - $area[1]))
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $frame.pixel_copy_before_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
        $graphics.CopyFromScreen($area[0], $area[1], 0, 0, $bitmap.Size, [Drawing.CopyPixelOperation]::SourceCopy)
        $frame.pixel_copy_after_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
        $graphics.Dispose(); $graphics = $null
        Assert-Owned
        Assert-Budget 'capture-post-uia-before'
        $afterState = $menuElement.Current
        Assert-Budget 'capture-post-uia-after'
        if ($afterState.ProcessId -ne $owned.Id -or $afterState.NativeWindowHandle -ne $menuHwnd.ToInt64() -or $afterState.IsOffscreen -or $afterState.ClassName -cne 'QMenu' -or
            $afterState.ControlType -ne [Windows.Automation.ControlType]::Menu -or
            -not $afterState.BoundingRectangle.Equals($uia)) { throw 'Menu identity or geometry changed during capture.' }
        $afterNative = [FormalStartupNative]::Bounds($menuHwnd, $owned.Id)
        if (($afterNative -join ',') -cne ($native -join ',')) { throw 'Native menu geometry changed during capture.' }
        [FormalStartupNative]::RequireCapture($menuHwnd, $owned.Id, $area)
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($graphics) { $graphics.Dispose() }
        if ($bitmap) { $bitmap.Dispose() }
    }
    $frame.capture_after_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    if ($Index -eq 0) { $report.capture_after_qpc = $frame.capture_after_qpc; $report.menu_image = $Path }
    if ($Index -gt 0) {
        $frame.previous_pixel_copy_interval_ms = Qpc-Milliseconds ([decimal]$report.menu_frames[$Index - 1].pixel_copy_before_qpc) ([decimal]$frame.pixel_copy_before_qpc) $frequency
    }
    Assert-Budget 'capture-after'
    Save-Json $observerPath $report
}

try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    Add-Type -AssemblyName System.Drawing
    # Reuse only the vetted native helper definition, never execute the soak
    # script's body, load its parameters, or invoke its caption fallback.
    $nativeSourcePath = Join-Path $PSScriptRoot 'run_tail_resource_soak.ps1'
    $parseTokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($nativeSourcePath, [ref]$parseTokens, [ref]$parseErrors)
    if (@($parseErrors).Count) { throw 'Native helper source has parser errors.' }
    $nativeNodes = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
        $node.Value.Contains('public static class TailSoakNative')
    }, $true))
    if ($nativeNodes.Count -ne 1) { throw 'Expected exactly one existing native helper definition.' }
    Add-Type -TypeDefinition $nativeNodes[0].Value
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FormalStartupNative {
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern IntPtr GetTopWindow(IntPtr parent);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    private static bool Owned(IntPtr window, int pid) {
        uint actual; GetWindowThreadProcessId(window, out actual);
        return window != IntPtr.Zero && actual == (uint)pid;
    }
    public static void SetPhysicalCoordinates() {
        if (SetThreadDpiAwarenessContext(new IntPtr(-4)) == IntPtr.Zero)
            throw new InvalidOperationException("Cannot establish per-monitor physical observer coordinates.");
    }
    public static int[] Bounds(IntPtr window, int pid) {
        Rect rect;
        if (!Owned(window, pid) || !GetWindowRect(window, out rect))
            throw new InvalidOperationException("Cannot read owned window bounds.");
        return new int[] { rect.Left, rect.Top, rect.Right, rect.Bottom };
    }
    public static void RequireHit(IntPtr window, int pid, int x, int y) {
        var point = new Point { X = x, Y = y };
        if (!Owned(window, pid) || !Owned(GetForegroundWindow(), pid) || WindowFromPoint(point) != window)
            throw new InvalidOperationException("Foreground or exact click/capture hit is not the owned target.");
    }
    public static void RequireCapture(IntPtr window, int pid, int[] area) {
        int[] bounds = Bounds(window, pid);
        if (!IsWindowVisible(window) || area.Length != 4 || area[0] < bounds[0] || area[1] < bounds[1]
            || area[2] > bounds[2] || area[3] > bounds[3] || area[2] <= area[0] || area[3] <= area[1]
            || area[2] - area[0] > 4096 || area[3] - area[1] > 4096)
            throw new InvalidOperationException("Capture must remain within a bounded owned menu rectangle.");
        bool reached = false;
        IntPtr above = GetTopWindow(IntPtr.Zero);
        for (int count = 0; above != IntPtr.Zero && count < 512; ++count, above = GetWindow(above, 2)) {
            if (above == window) { reached = true; break; }
            Rect other;
            if (IsWindowVisible(above) && GetWindowRect(above, out other)
                && other.Left < area[2] && other.Right > area[0] && other.Top < area[3] && other.Bottom > area[1])
                throw new InvalidOperationException("Another visible window overlaps the menu capture.");
        }
        if (!reached) throw new InvalidOperationException("Cannot verify menu Z-order ownership.");
        foreach (double fx in new double[] { 0.1, 0.5, 0.9 })
            foreach (double fy in new double[] { 0.1, 0.5, 0.9 })
                RequireHit(window, pid, area[0] + (int)((area[2] - area[0]) * fx), area[1] + (int)((area[3] - area[1]) * fy));
    }
    public static void CloseOwned(IntPtr window, int pid) {
        if (!Owned(window, pid) || !PostMessageW(window, 0x0010, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("Cannot post close to the exact owned MainWindow.");
    }
}
'@
    [FormalStartupNative]::SetPhysicalCoordinates()
    $savedEnvironment = @{}
    foreach ($name in 'NOTEPADSAFE_STARTUP_TRACE', 'NOTEPADSAFE_DATADIR', 'QT_FORCE_STDERR_LOGGING', 'QT_ASSUME_STDERR_HAS_CONSOLE') {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        [Environment]::SetEnvironmentVariable('NOTEPADSAFE_STARTUP_TRACE', '1', 'Process')
        [Environment]::SetEnvironmentVariable('NOTEPADSAFE_DATADIR', $profile, 'Process')
        [Environment]::SetEnvironmentVariable('QT_FORCE_STDERR_LOGGING', '1', 'Process')
        [Environment]::SetEnvironmentVariable('QT_ASSUME_STDERR_HAS_CONSOLE', '1', 'Process')
        $processClock = [Diagnostics.Stopwatch]::StartNew()
        $report.process_creation_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
        $owned = Start-Process -FilePath $inputExe -WorkingDirectory (Split-Path -Parent $inputExe) -WindowStyle Normal -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } finally {
        foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    }
    $ownedHandle = $owned.Handle
    $ownedStartTicks = $owned.StartTime.ToUniversalTime().Ticks
    $report.pid = $owned.Id
    $report.process_start_utc_ticks = $ownedStartTicks.ToString()
    $report.retained_handle = $ownedHandle.ToInt64().ToString()
    # This record is written before ANY UIA call, including MainWindow discovery.
    Save-Json $ownershipPath ([ordered]@{
        schema = 'startup-probe-ownership-v1'; pid = $owned.Id; process_start_utc_ticks = $ownedStartTicks.ToString()
        exe = $inputExe; exe_sha256 = $report.exe_sha256; retained_handle = $report.retained_handle
        process_creation_qpc = $report.process_creation_qpc; qpc_frequency = $report.qpc_frequency
    })
    Save-Json $observerPath $report
    Wait-Observed {
        foreach ($candidate in [TailSoakNative]::VisibleWindows($owned.Id)) {
            Assert-Budget 'main-uia-before'
            $element = [Windows.Automation.AutomationElement]::FromHandle($candidate)
            $state = $element.Current
            Assert-Budget 'main-uia-after'
            if ($state.ProcessId -ne $owned.Id) { throw 'UIA window escaped the owned PID.' }
            if ($state.ClassName -ceq 'MainWindow' -and $state.ControlType -eq [Windows.Automation.ControlType]::Window -and
                -not $state.IsOffscreen -and $state.NativeWindowHandle -eq $candidate.ToInt64()) {
                $script:mainHwnd = $candidate; $script:mainElement = $element
                return $true
            }
            $report.discovered_windows += [ordered]@{ hwnd = $candidate.ToInt64().ToString(); class_name = $state.ClassName; control_type = $state.ControlType.ProgrammaticName }
        }
        return $false
    } 'find-owned-mainwindow'
    $report.window_visible_observed_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $report.main_hwnd = $mainHwnd.ToInt64().ToString()
    $report.foreground_requested = [TailSoakNative]::SetForegroundWindow($mainHwnd)
    Wait-Observed { [TailSoakNative]::GetForegroundWindow() -eq $mainHwnd } 'foreground-owned-mainwindow'
    Wait-Observed {
        $condition = New-Object Windows.Automation.PropertyCondition(
            [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Button)
        $buttons = @($mainElement.FindAll([Windows.Automation.TreeScope]::Descendants, $condition) | Where-Object {
            $_.Current.ProcessId -eq $owned.Id -and $_.Current.Name -ceq $mainMenuName
        })
        Assert-Budget 'main-menu-button-state'
        if ($buttons.Count -gt 1) { throw 'Ambiguous main menu button.' }
        if ($buttons.Count -eq 1) { $script:menuButton = $buttons[0]; return $true }
        return $false
    } 'find-main-menu-button'
    $buttonState = $menuButton.Current
    Assert-Budget 'main-menu-button-final-state'
    if ($buttonState.IsOffscreen -or -not $buttonState.IsEnabled) { throw 'Main menu button is not visible and enabled.' }
    $buttonBounds = $buttonState.BoundingRectangle
    if ($buttonBounds.IsEmpty -or $buttonBounds.Width -le 0) { throw 'Main menu button has no clickable geometry.' }
    $buttonX = [int][Math]::Floor($buttonBounds.X + $buttonBounds.Width / 2)
    $buttonY = [int][Math]::Floor($buttonBounds.Y + $buttonBounds.Height / 2)
    [FormalStartupNative]::RequireHit($mainHwnd, $owned.Id, $buttonX, $buttonY)
    [TailSoakNative]::Click($owned.Id, $buttonX, $buttonY)
    # The main menu popup lists the classic menus; 文件 opens the file menu.
    Wait-Observed {
        foreach ($candidate in [TailSoakNative]::VisibleWindows($owned.Id)) {
            if ($candidate -eq $mainHwnd) { continue }
            $element = [Windows.Automation.AutomationElement]::FromHandle($candidate)
            if ($element.Current.ProcessId -ne $owned.Id -or $element.Current.ClassName -cne 'QMenu') { continue }
            $matches = @(Owned-MenuItems $element ([Windows.Automation.TreeScope]::Descendants) | Where-Object {
                $name = Normalize-MenuName $_.Current.Name
                $name -ceq $fileName -or $name -ceq 'File'
            })
            if ($matches.Count -eq 1) { $script:fileItem = $matches[0]; $script:mainMenuHwnd = $candidate; return $true }
        }
        return $false
    } 'find-real-file-menuitem'
    $itemState = $fileItem.Current
    Assert-Budget 'file-menuitem-final-state'
    if ($itemState.ProcessId -ne $owned.Id -or $itemState.IsOffscreen -or -not $itemState.IsEnabled) { throw 'File menu is not owned, visible and enabled.' }
    $bounds = $itemState.BoundingRectangle
    if ($bounds.IsEmpty -or $bounds.Width -le 0 -or $bounds.Height -le 0) { throw 'File MenuItem has no clickable geometry.' }
    $clickX = [int][Math]::Floor($bounds.X + $bounds.Width / 2)
    $clickY = [int][Math]::Floor($bounds.Y + $bounds.Height / 2)
    $report.menu_click = @($clickX, $clickY)
    Assert-Owned
    Assert-Budget 'input-before'
    [FormalStartupNative]::RequireHit($mainMenuHwnd, $owned.Id, $clickX, $clickY)
    $report.input_before_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    [TailSoakNative]::Click($owned.Id, $clickX, $clickY)
    $report.input_after_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    Assert-Budget 'input-after'
    Wait-Observed {
        foreach ($candidate in [TailSoakNative]::VisibleWindows($owned.Id)) {
            if ($candidate -eq $mainHwnd -or $candidate -eq $mainMenuHwnd) { continue }
            $element = [Windows.Automation.AutomationElement]::FromHandle($candidate)
            $state = $element.Current
            Assert-Budget 'popup-state'
            if ($state.ProcessId -ne $owned.Id) { throw 'Popup UIA identity escaped owned PID.' }
            if ($state.ClassName -cne 'QMenu' -or $state.ControlType -ne [Windows.Automation.ControlType]::Menu -or $state.IsOffscreen) { continue }
            $newItems = @(Owned-MenuItems $element ([Windows.Automation.TreeScope]::Descendants) | Where-Object {
                $name = Normalize-MenuName $_.Current.Name
                $name -ceq $newName -or $name -ceq 'New'
            })
            if ($newItems.Count -ne 1) { continue }
            $script:menuHwnd = $candidate; $script:menuElement = $element
            return $true
        }
        return $false
    } 'find-owned-qmenu'
    Capture-OwnedMenuFrame 0 $menuPng
    # Fixed diagnostic observations, not a wait-until-clear or performance loop.
    for ($frameIndex = 1; $frameIndex -le 3; ++$frameIndex) {
        Assert-Owned
        Assert-Budget 'diagnostic-frame-wait-before'
        Start-Sleep -Milliseconds 50
        Assert-Budget 'diagnostic-frame-wait-after'
        Capture-OwnedMenuFrame $frameIndex (Join-Path $evidence ('file-menu-frame-' + $frameIndex + '.png'))
    }
    Assert-Owned
    [TailSoakNative]::Escape($owned.Id)
    [FormalStartupNative]::CloseOwned($mainHwnd, $owned.Id)
    $remaining = [Math]::Max(0, $budgetMs - [int][Math]::Ceiling($processClock.Elapsed.TotalMilliseconds))
    if (-not $owned.WaitForExit($remaining)) { throw 'Owned process did not exit within the checked startup-observation budget.' }
    $report.process_exit_observed_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $report.process_interval_ms = $processClock.Elapsed.TotalMilliseconds
    $report.exit_code = $owned.ExitCode
    $report.normal_exit = $report.exit_code -eq 0
    $observedExit = $true
    Assert-Budget 'normal-exit'
} catch {
    $report.errors += $_.Exception.Message
} finally {
    if ($owned) {
        try {
            if (-not $owned.HasExited) {
                # Never rediscover by PID/name, never kill another process or a tree.
                if ($owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks -or $owned.Handle -ne $ownedHandle) {
                    throw 'Cleanup ownership mismatch; no process termination attempted.'
                }
                $owned.Kill()
                $report.forced_termination = $true
                if (-not $owned.WaitForExit(2000)) { throw 'Owned process termination remains unconfirmed.' }
            }
            $report.exit_code = $owned.ExitCode
            if (-not $report.process_exit_observed_qpc) { $report.process_exit_observed_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString() }
        } catch { $report.errors += ('Owned cleanup: ' + $_.Exception.Message) }
        finally { $owned.Dispose() }
    }
    try {
        $report.exe_sha256_after = Sha256 $inputExe
        $report.binary_stable = $report.exe_sha256_after -ceq $report.exe_sha256
        if (-not $report.binary_stable) { throw 'Executable changed during the pilot.' }
    } catch { $report.errors += ('Binary identity: ' + $_.Exception.Message) }
    try {
        if (-not (Test-Path -LiteralPath $stdout -PathType Leaf) -or (Get-Item -LiteralPath $stdout).Length -gt 65536) { throw 'Trace stdout missing or exceeds 64 KiB.' }
        $lines = @([IO.File]::ReadAllLines($stdout, $strictUtf8) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($lines.Count -ne 1) { throw 'Expected exactly one JSON trace line.' }
        $trace = Parse-StartupTraceLine $lines[0]
        $ticks = Check-StartupTrace $trace $report.pid $frequency $report.exit_code ([decimal]$report.process_creation_qpc) ([decimal]$report.process_exit_observed_qpc)
        $report.trace = $trace
        if (-not $observedExit -or -not $report.normal_exit -or $report.forced_termination) { throw 'Actual final process exit was not an observed normal zero.' }
        if (-not $report.menu_image -or -not (Test-Path -LiteralPath $menuPng -PathType Leaf)) { throw 'No guarded menu pixel evidence was captured.' }
        if ($report.menu_frames.Count -ne 4 -or @($report.menu_frames | Where-Object { -not $_.capture_after_qpc }).Count -ne 0) {
            throw 'The four fixed menu observations did not all complete.'
        }
        $paintToCapture = Qpc-Milliseconds $ticks.first_paint ([decimal]$report.capture_after_qpc) $frequency
        $report.timings_ms = [ordered]@{
            application_to_first_paint = (Qpc-Milliseconds $ticks.application_ready $ticks.first_paint $frequency)
            create_to_visible_observation = (Qpc-Milliseconds ([decimal]$report.process_creation_qpc) ([decimal]$report.window_visible_observed_qpc) $frequency)
            create_to_menu_capture_upper = (Qpc-Milliseconds ([decimal]$report.process_creation_qpc) ([decimal]$report.capture_after_qpc) $frequency)
            # Visibility/UIA can permit real input before the later exported
            # main first-paint timestamp. This signed observational delta is
            # not a duration gate and must not invalidate such early input.
            paint_to_input_dispatch_signed = [double](([decimal]$report.input_before_qpc - $ticks.first_paint) * [decimal]1000 / [decimal]$frequency)
            input_call = (Qpc-Milliseconds ([decimal]$report.input_before_qpc) ([decimal]$report.input_after_qpc) $frequency)
            input_to_menu_capture_upper = (Qpc-Milliseconds ([decimal]$report.input_before_qpc) ([decimal]$report.capture_after_qpc) $frequency)
            capture_call = (Qpc-Milliseconds ([decimal]$report.capture_before_qpc) ([decimal]$report.capture_after_qpc) $frequency)
            paint_to_menu_capture_upper = $paintToCapture
            paint_to_capture_100ms_observation = $(if ($paintToCapture -lt 100) { 'within-upper-bound-single-sample-awaiting-pixel-review' } else { 'upper-bound-exceeded-product-cause-inconclusive' })
            internal_500ms_and_external_1500ms_p95 = 'not-evaluated-single-pilot'
        }
        $report.observation_valid = $report.errors.Count -eq 0 -and $report.binary_stable
        if ($report.observation_valid) { $report.classification = 'pilot-capture-complete-needs-visual-review' }
    } catch { $report.errors += ('Trace/observation validation: ' + $_.Exception.Message) }
    if ($processClock -and -not $report.process_interval_ms) { $report.process_interval_ms = $processClock.Elapsed.TotalMilliseconds }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    Save-Json $observerPath $report
}
Write-Output $observerPath
if (-not $report.observation_valid) { exit 1 }
exit 0
