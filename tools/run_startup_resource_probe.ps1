[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

# One owned formal-process Empty diagnostic. No ETW, debugger, input, screenshots,
# WM_NULL, UIA during sampling, or performance-acceptance verdict.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'Windows PowerShell 5.1 is required.'
}
$utf8 = New-Object Text.UTF8Encoding($false)
$strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
$frequency = [Diagnostics.Stopwatch]::Frequency
$budgetMs = 10000
$samplingMs = 2000
$requestedDelayMs = 20
if ($frequency -le 0) { throw 'Invalid parent QPC frequency.' }

# Import only named pure functions, never run another script's body.
$formalPath = Join-Path $PSScriptRoot 'run_startup_formal_probe.ps1'
$nativePath = Join-Path $PSScriptRoot 'run_tail_resource_soak.ps1'
$tokens = $null; $parseErrors = $null
$formalAst = [Management.Automation.Language.Parser]::ParseFile($formalPath, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count) { throw 'Formal probe source has parser errors.' }
foreach ($name in 'Local-AbsolutePath', 'Reject-ReparseAncestors', 'Sha256', 'Exact-Keys', 'Decimal-Ticks', 'Parse-StartupTraceLine', 'Check-StartupTrace') {
    $nodes = @($formalAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
    }, $true))
    if ($nodes.Count -ne 1) { throw ('Missing/ambiguous pure helper: ' + $name) }
    Invoke-Expression $nodes[0].Extent.Text
}
function Save-ResourceJson([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), $utf8)
}
function Resource-Counter($Value) {
    if ($Value -isnot [string] -or $Value -notmatch '^[0-9]+$') { throw 'Counters must be unsigned decimal strings.' }
    $parsed = [uint64]0
    if (-not [uint64]::TryParse($Value, [ref]$parsed)) { throw 'Counter exceeds UInt64.' }
    return [decimal]$parsed
}
function Check-ResourceSamples($Rows, [long]$Frequency, [decimal]$Begin, [decimal]$End) {
    if ($Frequency -le 0 -or $Begin -le 0 -or $End -lt $Begin -or $Rows -isnot [array] -or $Rows.Count -lt 2) {
        throw 'Invalid sampling interval/frequency or fewer than two complete samples.'
    }
    $counterNames = @('read_operations', 'write_operations', 'other_operations', 'read_bytes', 'write_bytes', 'other_bytes')
    $previousAfter = $Begin; $previousBefore = $null; $previousCpu = [decimal]0; $previousIo = @{}
    for ($index = 0; $index -lt $Rows.Count; ++$index) {
        $row = $Rows[$index]
        Exact-Keys $row @('index', 'query_before_qpc', 'query_after_qpc', 'cpu_total_100ns', 'io_cumulative', 'previous_query_before_interval_ms', 'error')
        if (($row.index -isnot [int] -and $row.index -isnot [long]) -or $row.index -ne $index -or $null -ne $row.error) {
            throw 'Invalid resource sample index or recorded query error.'
        }
        $before = Decimal-Ticks $row.query_before_qpc
        $after = Decimal-Ticks $row.query_after_qpc
        if ($before -lt $previousAfter -or $after -lt $before -or $after -gt $End) { throw 'Resource query brackets are unordered or outside sampling interval.' }
        $cpu = Resource-Counter $row.cpu_total_100ns
        if ($cpu -gt [decimal][long]::MaxValue -or $cpu -lt $previousCpu) { throw 'CPU ticks overflow or roll back.' }
        Exact-Keys $row.io_cumulative $counterNames
        foreach ($name in $counterNames) {
            $value = Resource-Counter $row.io_cumulative.$name
            if ($previousIo.ContainsKey($name) -and $value -lt $previousIo[$name]) { throw ('I/O counter rolls back: ' + $name) }
            $previousIo[$name] = $value
        }
        if ($null -eq $previousBefore) {
            if ($null -ne $row.previous_query_before_interval_ms) { throw 'First sample must not invent a previous interval.' }
        } else {
            $actual = $row.previous_query_before_interval_ms
            if ($actual -isnot [int] -and $actual -isnot [long] -and $actual -isnot [double] -and $actual -isnot [decimal]) {
                throw 'Actual query spacing must be numeric.'
            }
            $expected = ($before - $previousBefore) * [decimal]1000 / [decimal]$Frequency
            if ([Math]::Abs([decimal]$actual - $expected) -gt [decimal]0.000000001) { throw 'Actual query spacing disagrees with QPC.' }
        }
        $previousAfter = $after; $previousBefore = $before; $previousCpu = $cpu
    }
}
function Resource-ShowCoverage($Rows, [decimal]$ShowBegin, [decimal]$ShowReturned) {
    if ($ShowReturned -lt $ShowBegin) { throw 'Invalid show interval.' }
    $outerBefore = $null; $outerAfter = $null; $inside = @()
    foreach ($row in $Rows) {
        if ([decimal]$row.query_after_qpc -le $ShowBegin) { $outerBefore = $row.index }
        if ($null -eq $outerAfter -and [decimal]$row.query_before_qpc -ge $ShowReturned) { $outerAfter = $row.index }
        if ([decimal]$row.query_before_qpc -ge $ShowBegin -and [decimal]$row.query_after_qpc -le $ShowReturned) { $inside += $row.index }
    }
    return [ordered]@{
        show_begin_qpc = $ShowBegin.ToString([Globalization.CultureInfo]::InvariantCulture)
        show_returned_qpc = $ShowReturned.ToString([Globalization.CultureInfo]::InvariantCulture)
        outer_pair_available = ($null -ne $outerBefore -and $null -ne $outerAfter -and $outerBefore -lt $outerAfter)
        outer_before_index = $outerBefore; outer_after_index = $outerAfter
        inner_pair_available = ($inside.Count -ge 2)
        inner_first_index = $(if ($inside.Count) { $inside[0] } else { $null })
        inner_last_index = $(if ($inside.Count) { $inside[-1] } else { $null })
        interpretation = 'Indices only. Every CPU/I/O read lies somewhere within its query bracket; no exact show cost is calculated. Missing pairs mean insufficient coverage, not a fabricated zero.'
    }
}

$inputExe = Local-AbsolutePath $ExePath
$evidence = Local-AbsolutePath $EvidenceDirectory
if (-not (Test-Path -LiteralPath $inputExe -PathType Leaf) -or [IO.Path]::GetFileName($inputExe) -ine 'notepad-safe.exe') { throw 'ExePath must be the formal notepad-safe.exe.' }
if (Test-Path -LiteralPath $evidence) { throw 'EvidenceDirectory must be new.' }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $evidence) -PathType Container)) { throw 'Evidence parent must already exist.' }
if ($evidence.TrimEnd('\', '/') -eq [IO.Path]::GetPathRoot($evidence).TrimEnd('\', '/')) { throw 'EvidenceDirectory cannot be a drive root.' }
Reject-ReparseAncestors $inputExe
Reject-ReparseAncestors $evidence
[void][IO.Directory]::CreateDirectory($evidence)
$profile = Join-Path $evidence 'profile'
[void][IO.Directory]::CreateDirectory($profile)
$stdout = Join-Path $evidence 'stdout.log'; $stderr = Join-Path $evidence 'stderr.log'
$ownershipPath = Join-Path $evidence 'ownership.json'
$observerPath = Join-Path $evidence 'observer.json'
$samplesPath = Join-Path $evidence 'samples.jsonl'
$report = [ordered]@{
    schema = 'startup-resource-probe-v1'; mode = 'Empty'; observation_valid = $false
    classification = 'incomplete-resource-diagnostic'; performance_acceptance = 'not-evaluated'
    started_utc = [DateTime]::UtcNow.ToString('o'); finished_utc = $null
    exe = $inputExe; exe_sha256 = (Sha256 $inputExe); exe_sha256_after = $null; binary_stable = $false
    formal_helper_sha256 = (Sha256 $formalPath); native_helper_sha256 = (Sha256 $nativePath)
    profile = $profile; stdout = $stdout; stderr = $stderr; ownership = $ownershipPath; samples = $samplesPath; samples_sha256 = $null
    pid = $null; retained_handle = $null; process_start_utc_ticks = $null
    qpc_frequency = $frequency.ToString(); process_creation_qpc = $null; process_exit_observed_qpc = $null
    sampling_begin_qpc = $null; sampling_end_qpc = $null; sampling_requested_ms = $samplingMs; requested_between_queries_sleep_ms = $requestedDelayMs
    first_query_before_qpc = $null; last_query_after_qpc = $null; create_to_first_query_ms = $null; sample_count = 0
    uia_begin_qpc = $null; main_hwnd = $null; close_requested_qpc = $null
    exit_code = $null; normal_exit = $false; forced_termination = $false; budget_ms = $budgetMs
    trace = $null; show_coverage = $null; errors = @()
    sampling_definition = 'Two-second window starts immediately after ownership publication. 20ms is requested sleep between queries, not actual cadence. Startup-to-first-query gap and every query bracket are retained. No UIA/native window query/input/heartbeat/screenshot occurs within this window.'
    resource_definition = 'Process.TotalProcessorTime.Ticks is cumulative process CPU in 100ns units. I/O contains six cumulative UInt64 process counters. These are not function-exclusive CPU, disk throughput, or simultaneous CPU/I/O reads.'
    deadline_definition = '10s checked before/after synchronous calls, including UIA only after sampling. COM cannot be forcibly preempted here; ownership.json enables external supervision. Exact-owned cleanup may take another 2s.'
}
Save-ResourceJson $observerPath $report
$owned = $null; $ownedHandle = [IntPtr]::Zero; $ownedStartTicks = 0L
$clock = $null; $observedExit = $false; $writer = $null
$rows = New-Object 'Collections.Generic.List[object]'
function Assert-ResourceBudget([string]$Phase) {
    if ($clock -and $clock.Elapsed.TotalMilliseconds -ge $budgetMs) { throw ('Checked 10s budget exceeded at ' + $Phase) }
}
function Assert-ResourceOwner {
    if (-not $owned) { throw 'No owned process.' }
    $owned.Refresh()
    if ($owned.HasExited -or $owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks -or $owned.Handle -ne $ownedHandle) {
        throw 'Owned process exited or identity changed.'
    }
}

try {
    # Assembly/type preparation occurs before launch, not inside the sampling window.
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $nativeAst = [Management.Automation.Language.Parser]::ParseFile($nativePath, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count) { throw 'Native helper source has parser errors.' }
    $nativeNodes = @($nativeAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.StringConstantExpressionAst] -and $node.Value.Contains('public static class TailSoakNative')
    }, $true))
    if ($nativeNodes.Count -ne 1) { throw 'Expected one native helper definition.' }
    Add-Type -TypeDefinition $nativeNodes[0].Value
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StartupResourceNative {
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    public static void CloseOwned(IntPtr window, int pid) {
        uint actual; GetWindowThreadProcessId(window, out actual);
        if (window == IntPtr.Zero || actual != (uint)pid || !PostMessageW(window, 0x0010, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("Cannot close exact owned MainWindow.");
    }
}
'@
    $writer = New-Object IO.StreamWriter($samplesPath, $false, $utf8)
    $savedEnvironment = @{}
    foreach ($name in 'NOTEPADSAFE_STARTUP_TRACE', 'NOTEPADSAFE_DATADIR', 'QT_FORCE_STDERR_LOGGING', 'QT_ASSUME_STDERR_HAS_CONSOLE') {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        [Environment]::SetEnvironmentVariable('NOTEPADSAFE_STARTUP_TRACE', '1', 'Process')
        [Environment]::SetEnvironmentVariable('NOTEPADSAFE_DATADIR', $profile, 'Process')
        [Environment]::SetEnvironmentVariable('QT_FORCE_STDERR_LOGGING', '1', 'Process')
        [Environment]::SetEnvironmentVariable('QT_ASSUME_STDERR_HAS_CONSOLE', '1', 'Process')
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $report.process_creation_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
        $owned = Start-Process -FilePath $inputExe -WorkingDirectory (Split-Path -Parent $inputExe) -WindowStyle Normal -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } finally {
        foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    }
    $ownedHandle = $owned.Handle; $ownedStartTicks = $owned.StartTime.ToUniversalTime().Ticks
    $report.pid = $owned.Id; $report.retained_handle = $ownedHandle.ToInt64().ToString(); $report.process_start_utc_ticks = $ownedStartTicks.ToString()
    Save-ResourceJson $ownershipPath ([ordered]@{
        schema = 'startup-probe-ownership-v1'; pid = $owned.Id; process_start_utc_ticks = $report.process_start_utc_ticks
        exe = $inputExe; exe_sha256 = $report.exe_sha256; retained_handle = $report.retained_handle
        process_creation_qpc = $report.process_creation_qpc; qpc_frequency = $report.qpc_frequency
    })
    $report.sampling_begin_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $samplingDeadline = [decimal]$report.sampling_begin_qpc + [decimal]$frequency * $samplingMs / 1000
    while ([decimal][Diagnostics.Stopwatch]::GetTimestamp() -lt $samplingDeadline) {
        Assert-ResourceBudget 'sample-before'
        Assert-ResourceOwner
        $cpu = $null; $io = $null; $queryError = $null
        $queryBefore = [Diagnostics.Stopwatch]::GetTimestamp()
        try {
            $cpu = $owned.TotalProcessorTime.Ticks
            $io = [TailSoakNative]::Io($ownedHandle)
        } catch { $queryError = $_.Exception.Message }
        finally { $queryAfter = [Diagnostics.Stopwatch]::GetTimestamp() }
        $row = [ordered]@{
            index = $rows.Count; query_before_qpc = $queryBefore.ToString(); query_after_qpc = $queryAfter.ToString()
            cpu_total_100ns = $(if ($null -ne $cpu) { $cpu.ToString() } else { $null })
            io_cumulative = $(if ($null -ne $io) { [ordered]@{
                read_operations = $io.ReadOperations.ToString(); write_operations = $io.WriteOperations.ToString(); other_operations = $io.OtherOperations.ToString()
                read_bytes = $io.ReadBytes.ToString(); write_bytes = $io.WriteBytes.ToString(); other_bytes = $io.OtherBytes.ToString()
            } } else { $null })
            previous_query_before_interval_ms = $(if ($rows.Count) {
                ([decimal]$queryBefore - [decimal]$rows[$rows.Count - 1].query_before_qpc) * [decimal]1000 / [decimal]$frequency
            } else { $null })
            error = $queryError
        }
        $rows.Add($row)
        $writer.WriteLine(($row | ConvertTo-Json -Compress -Depth 4)); $writer.Flush()
        if ($queryError) { throw ('Resource query failed: ' + $queryError) }
        Assert-ResourceOwner
        Assert-ResourceBudget 'sample-after'
        Start-Sleep -Milliseconds $requestedDelayMs
    }
    $report.sampling_end_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $writer.Dispose(); $writer = $null
    Save-ResourceJson $observerPath $report
    # First UIA/window-discovery activity happens after the sampling window.
    $report.uia_begin_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $mainHwnd = [IntPtr]::Zero
    while ($mainHwnd -eq [IntPtr]::Zero) {
        Assert-ResourceOwner
        Assert-ResourceBudget 'main-uia-before'
        foreach ($candidate in [TailSoakNative]::VisibleWindows($owned.Id)) {
            Assert-ResourceBudget 'candidate-uia-before'
            $element = [Windows.Automation.AutomationElement]::FromHandle($candidate)
            $state = $element.Current
            Assert-ResourceBudget 'candidate-uia-after'
            if ($state.ProcessId -ne $owned.Id) { throw 'UIA identity escaped owned PID.' }
            if ($state.ClassName -ceq 'MainWindow' -and $state.ControlType -eq [Windows.Automation.ControlType]::Window -and
                -not $state.IsOffscreen -and $state.NativeWindowHandle -eq $candidate.ToInt64()) { $mainHwnd = $candidate; break }
        }
        if ($mainHwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 10 }
    }
    Assert-ResourceOwner
    Assert-ResourceBudget 'close-before'
    $report.main_hwnd = $mainHwnd.ToInt64().ToString()
    $report.close_requested_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    [StartupResourceNative]::CloseOwned($mainHwnd, $owned.Id)
    $remaining = [Math]::Max(0, $budgetMs - [int][Math]::Ceiling($clock.Elapsed.TotalMilliseconds))
    if (-not $owned.WaitForExit($remaining)) { throw 'Owned process did not exit within checked 10s budget.' }
    $report.process_exit_observed_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString()
    $report.exit_code = [TailSoakNative]::ExitCode($ownedHandle)
    $report.normal_exit = $report.exit_code -eq 0; $observedExit = $true
    Assert-ResourceBudget 'normal-exit'
} catch { $report.errors += $_.Exception.Message }
finally {
    if (-not $report.sampling_end_qpc -and $report.sampling_begin_qpc) { $report.sampling_end_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString() }
    if ($writer) {
        try { $writer.Dispose() }
        catch { $report.errors += ('Raw sample writer cleanup: ' + $_.Exception.Message) }
    }
    if ($owned) {
        try {
            if (-not $owned.HasExited) {
                if ($owned.StartTime.ToUniversalTime().Ticks -ne $ownedStartTicks -or $owned.Handle -ne $ownedHandle) { throw 'Cleanup ownership mismatch; refusing termination.' }
                $owned.Kill(); $report.forced_termination = $true
                if (-not $owned.WaitForExit(2000)) { throw 'Owned termination remains unconfirmed.' }
            }
            $report.exit_code = [TailSoakNative]::ExitCode($ownedHandle)
            if (-not $report.process_exit_observed_qpc) { $report.process_exit_observed_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString() }
        } catch { $report.errors += ('Owned cleanup: ' + $_.Exception.Message) }
        finally { $owned.Dispose() }
    }
    $report.sample_count = $rows.Count
    if ($rows.Count) {
        $report.first_query_before_qpc = $rows[0].query_before_qpc; $report.last_query_after_qpc = $rows[$rows.Count - 1].query_after_qpc
        $report.create_to_first_query_ms = ([decimal]$report.first_query_before_qpc - [decimal]$report.process_creation_qpc) * [decimal]1000 / [decimal]$frequency
    }
    try {
        $report.exe_sha256_after = Sha256 $inputExe
        $report.binary_stable = $report.exe_sha256_after -ceq $report.exe_sha256
        if (-not $report.binary_stable -or (Sha256 $formalPath) -cne $report.formal_helper_sha256 -or (Sha256 $nativePath) -cne $report.native_helper_sha256) { throw 'Executable/helper source changed during diagnostic.' }
        $report.samples_sha256 = Sha256 $samplesPath
        $rawRows = @([IO.File]::ReadAllLines($samplesPath, $strictUtf8) | ForEach-Object { $_ | ConvertFrom-Json -ErrorAction Stop })
        if ($rawRows.Count -ne $rows.Count) { throw 'Persisted raw sample count mismatch.' }
        Check-ResourceSamples $rawRows $frequency ([decimal]$report.sampling_begin_qpc) ([decimal]$report.sampling_end_qpc)
        if (-not (Test-Path -LiteralPath $stdout -PathType Leaf) -or (Get-Item -LiteralPath $stdout).Length -gt 65536) { throw 'Missing/oversized stdout trace.' }
        $lines = @([IO.File]::ReadAllLines($stdout, $strictUtf8) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($lines.Count -ne 1) { throw 'Expected one stdout trace line.' }
        $trace = Parse-StartupTraceLine $lines[0]
        $ticks = Check-StartupTrace $trace $report.pid $frequency $report.exit_code ([decimal]$report.process_creation_qpc) ([decimal]$report.process_exit_observed_qpc)
        $report.trace = $trace
        if (-not $observedExit -or -not $report.normal_exit -or $report.forced_termination) { throw 'Actual final exit was not an observed normal zero.' }
        if ([decimal]$report.sampling_begin_qpc -lt [decimal]$report.process_creation_qpc -or
            [decimal]$report.uia_begin_qpc -lt [decimal]$report.sampling_end_qpc -or
            [decimal]$report.close_requested_qpc -lt [decimal]$report.uia_begin_qpc -or
            [decimal]$report.process_exit_observed_qpc -lt [decimal]$report.close_requested_qpc) { throw 'Sampling/UIA/close phase order invalid.' }
        if (([decimal]$report.sampling_end_qpc - [decimal]$report.sampling_begin_qpc) * 1000 / $frequency -lt $samplingMs) { throw 'Requested two-second sampling window did not complete.' }
        $report.show_coverage = Resource-ShowCoverage $rawRows $ticks.show_begin $ticks.show_returned
        $report.observation_valid = $report.errors.Count -eq 0
        if ($report.observation_valid) { $report.classification = 'owned-resource-diagnostic-complete-not-performance-acceptance' }
    } catch { $report.errors += ('Evidence validation: ' + $_.Exception.Message) }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    Save-ResourceJson $observerPath $report
}
Write-Output $observerPath
if (-not $report.observation_valid) { exit 1 }
exit 0
