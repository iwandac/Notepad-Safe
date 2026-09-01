[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [ValidateRange(0, 2000)][int]$DiscoveryDelayMs = 0
)

# Windows PowerShell 5.1, ASCII source. Low-interference formal-process probe:
# no UI Automation, screen capture, input injection, cache flush, or network.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StartupTraceOnlyNative {
    [DllImport("user32.dll", SetLastError=true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", SetLastError=true)]
    private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    public static void CloseOwned(IntPtr window, int expectedPid) {
        uint pid;
        if (window == IntPtr.Zero || GetWindowThreadProcessId(window, out pid) == 0 || pid != (uint)expectedPid)
            throw new InvalidOperationException("Main window ownership mismatch.");
        if (!PostMessageW(window, 0x0010, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("Cannot post WM_CLOSE to the owned main window.");
    }
    public static int ExitCode(IntPtr process) {
        uint code;
        if (process == IntPtr.Zero || !GetExitCodeProcess(process, out code))
            throw new InvalidOperationException("Cannot read the owned process exit code.");
        return unchecked((int)code);
    }
}
'@

function Absolute-Local([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:[\\/]') { throw 'Use an absolute local drive path.' }
    return [IO.Path]::GetFullPath($Path)
}
function Hash-File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Tick([object]$Value) {
    if ($Value -isnot [string] -or $Value -notmatch '^[0-9]+$') { throw 'Invalid QPC tick.' }
    $parsed = 0L
    if (-not [long]::TryParse($Value, [ref]$parsed) -or $parsed -le 0) { throw 'QPC tick is outside Int64.' }
    return [decimal]$parsed
}

if ($env:OS -ne 'Windows_NT' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'Windows PowerShell 5.1 is required.'
}
$exe = Absolute-Local $ExePath
$evidence = Absolute-Local $EvidenceDirectory
if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or [IO.Path]::GetFileName($exe) -ine 'notepad-safe.exe') {
    throw 'ExePath must identify notepad-safe.exe.'
}
if (Test-Path -LiteralPath $evidence) { throw 'EvidenceDirectory must be new.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $evidence) -PathType Container)) {
    throw 'EvidenceDirectory parent must exist.'
}
[void][IO.Directory]::CreateDirectory($evidence)
$profile = Join-Path $evidence 'profile'
[void][IO.Directory]::CreateDirectory($profile)
$stdout = Join-Path $evidence 'stdout.log'
$stderr = Join-Path $evidence 'stderr.log'
$observation = Join-Path $evidence 'observation.json'
$hashBefore = Hash-File $exe
$saved = @{}
$process = $null
$actualExitCode = $null
$creation = [Diagnostics.Stopwatch]::GetTimestamp()
try {
    foreach ($name in 'NOTEPADSAFE_STARTUP_TRACE', 'NOTEPADSAFE_DATADIR', 'QT_FORCE_STDERR_LOGGING', 'QT_ASSUME_STDERR_HAS_CONSOLE') {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    [Environment]::SetEnvironmentVariable('NOTEPADSAFE_STARTUP_TRACE', '1', 'Process')
    [Environment]::SetEnvironmentVariable('NOTEPADSAFE_DATADIR', $profile, 'Process')
    [Environment]::SetEnvironmentVariable('QT_FORCE_STDERR_LOGGING', '1', 'Process')
    [Environment]::SetEnvironmentVariable('QT_ASSUME_STDERR_HAS_CONSOLE', '1', 'Process')
    $process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) `
        -WindowStyle Normal -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $processHandle = $process.Handle
    if ($DiscoveryDelayMs -gt 0) { Start-Sleep -Milliseconds $DiscoveryDelayMs }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $window = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { throw 'Process exited before main-window observation.' }
        $window = $process.MainWindowHandle
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 5
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Owned main window was not observed.' }
    # This wait is after window discovery and is excluded from the child trace.
    # It only lets the already-scheduled first paint and idle/restore milestones run.
    Start-Sleep -Milliseconds 750
    [StartupTraceOnlyNative]::CloseOwned($window, $process.Id)
    if (-not $process.WaitForExit(8000)) { throw 'Owned process did not exit normally.' }
    $actualExitCode = [StartupTraceOnlyNative]::ExitCode($processHandle)
    if ($actualExitCode -ne 0) { throw ('Owned process returned nonzero: ' + $actualExitCode) }
} finally {
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
    if ($process -and -not $process.HasExited) {
        $process.Kill()
        [void]$process.WaitForExit(2000)
    }
}
$exitObserved = [Diagnostics.Stopwatch]::GetTimestamp()
if ((Hash-File $exe) -cne $hashBefore) { throw 'Formal binary changed during observation.' }
$lines = @([IO.File]::ReadAllLines($stdout, $utf8) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($lines.Count -ne 1 -or $lines[0].Length -gt 65536 -or $lines[0] -match '[^\x20-\x7e]') {
    throw 'Expected one bounded printable ASCII trace line.'
}
$trace = $lines[0] | ConvertFrom-Json -ErrorAction Stop
if ($trace.schema -cne 'startup-trace-v1' -or -not $trace.valid -or -not $trace.complete `
    -or $trace.invalid_clock -or $trace.overflow -or $trace.duplicate_stage -or $trace.invalid_stage) {
    throw 'Child startup trace is invalid or incomplete.'
}
if ($trace.pid -ne $process.Id -or $trace.exit_code -ne 0 -or $trace.events.Count -ne 9) {
    throw 'Child trace identity, exit, or stage count mismatch.'
}
$events = @{}
foreach ($event in $trace.events) {
    if ($events.ContainsKey($event.stage)) { throw 'Duplicate trace stage.' }
    $events[$event.stage] = Tick $event.ticks
}
$frequency = Tick $trace.qpc_frequency
$firstPaintMs = [double](($events.first_paint - $events.application_ready) * [decimal]1000 / $frequency)
$createToPaintMs = [double](($events.first_paint - [decimal]$creation) * [decimal]1000 / $frequency)
if ($firstPaintMs -lt 0 -or $createToPaintMs -lt 0 -or [decimal]$exitObserved -lt $events.event_loop_finished) {
    throw 'Trace timing falls outside the owned process interval.'
}
$result = [ordered]@{
    schema = 'startup-trace-only-probe-v1'
    observation_valid = $true
    exe = $exe
    exe_sha256 = $hashBefore
    profile = $profile
    pid = $process.Id
    exit_code = $actualExitCode
    qpc_frequency = $trace.qpc_frequency
    process_creation_qpc = $creation.ToString()
    process_exit_observed_qpc = $exitObserved.ToString()
    application_to_first_paint_ms = $firstPaintMs
    creation_to_first_paint_ms = $createToPaintMs
    discovery_delay_ms = $DiscoveryDelayMs
    trace = $trace
    limits = @(
        'No UI Automation, input injection, screen capture, cache flush, or network.',
        'Optional discovery delay occurs entirely in the observer and does not alter child timing.',
        'The fixed 750 ms post-window wait is excluded from child first-paint timing.',
        'This probe validates internal first paint only, not visible input presentation.'
    )
}
[IO.File]::WriteAllText($observation, ($result | ConvertTo-Json -Depth 12), $utf8)
Write-Output $observation
