[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(1, 30)][int]$Rounds = 1
)

# Windows PowerShell 5.1. Measurement-host first paint only: not the formal EXE,
# interactive input/pixels, cold/warm startup, or performance-budget acceptance.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
$strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
$deadlineMs = 10000

function Full-Path([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:[\\/]') {
        throw 'Exe and OutputDirectory must be local absolute drive paths; UNC is not allowed.'
    }
    $full = [IO.Path]::GetFullPath($Path)
    return $full
}
function Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), $utf8)
}
function Field($Object, [string]$Name) {
    if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) {
        throw "Missing measurement field: $Name"
    }
    return $Object.$Name
}
function Decimal-Ticks($Object, [string]$Name) {
    $value = Field $Object $Name
    if ($value -isnot [string] -or $value -notmatch '^[0-9]+$') {
        throw "$Name must be a decimal 64-bit integer string."
    }
    $parsed = 0L
    if (-not [long]::TryParse($value, [ref]$parsed)) { throw "$Name exceeds signed 64-bit range." }
    return [decimal]$parsed
}
function Check-Measurement($Measurement, [string]$Mode, [int]$OwnedId, [int]$ExitCode) {
    if ((Field $Measurement 'schema') -cne 'startup-composition-measurement-v3') { throw 'Unexpected JSON schema.' }
    if ((Field $Measurement 'mode') -cne $Mode) { throw 'Measurement mode mismatch.' }
    $jsonPid = Field $Measurement 'pid'
    $jsonExit = Field $Measurement 'exit_code'
    if (($jsonPid -isnot [int] -and $jsonPid -isnot [long]) -or $jsonPid -ne $OwnedId) { throw 'Measurement PID mismatch.' }
    if (($jsonExit -isnot [int] -and $jsonExit -isnot [long]) -or $jsonExit -ne $ExitCode) { throw 'Reported/actual exit code mismatch.' }
    $success = Field $Measurement 'success'
    $paint = Field $Measurement 'paint_observed'
    $result = Field $Measurement 'result'
    if ($success -isnot [bool] -or $paint -isnot [bool] -or $result -isnot [string]) {
        throw 'Invalid success/paint/result types.'
    }
    $codes = @{
        first_paint_observed = @(0); invalid_mode = @(20); qpc_unavailable = @(21)
        clock_failed = @(21); profile_setup_failed = @(2); registration_failed = @(3, 4, 10, 11)
        first_paint_timeout = @(6); factory_before_paint = @(5); first_paint_budget_exceeded = @(8)
        profile_cleanup_failed = @(27)
    }
    if (-not $codes.ContainsKey($result) -or $ExitCode -notin $codes[$result] -or
        $success -ne ($result -ceq 'first_paint_observed')) { throw 'Inconsistent result/success/exit code.' }
    if (-not $success) { throw "Measurement host reported failure: $result (exit $ExitCode)." }
    if (-not $paint) { throw 'Successful result without observed paint.' }
    $frequency = Decimal-Ticks $Measurement 'qpc_frequency'
    $start = Decimal-Ticks $Measurement 'qpc_start'
    $end = Decimal-Ticks $Measurement 'qpc_first_paint'
    $nanoseconds = Decimal-Ticks $Measurement 'first_paint_elapsed_ns'
    if ($nanoseconds -ge [decimal]1500000000) { throw 'Successful first paint exceeds the host 1500 ms validity bound.' }
    if ($frequency -le 0 -or $start -le 0 -or $end -le $start) { throw 'QPC values are not positive and increasing.' }
    $computed = [decimal]::Truncate(($end - $start) * [decimal]1000000000 / $frequency)
    if ([Math]::Abs($computed - $nanoseconds) -gt 1) { throw 'QPC pair and elapsed nanoseconds disagree.' }
    $stages = Field $Measurement 'stage_qpc'
    if (@($stages.PSObject.Properties).Count -ne 5) { throw 'Expected exactly five stage QPC timestamps.' }
    $composition = Decimal-Ticks $stages 'composition_ready'
    $registration = Decimal-Ticks $stages 'registration_ready'
    $window = Decimal-Ticks $stages 'window_ready'
    $showBegin = Decimal-Ticks $stages 'show_begin'
    $showReturned = Decimal-Ticks $stages 'show_returned'
    if ($composition -lt $start -or $registration -lt $composition -or
        $window -lt $registration -or $showBegin -lt $window -or
        $showReturned -lt $showBegin -or $end -lt $showBegin) {
        throw 'Stage QPC timestamps violate startup execution order.'
    }
    # A first paint inside show() is valid: show_returned can follow first paint.
    # Keep the raw timestamps, rather than inventing a negative post-show wait
    # or adding overlapping show duration to the measured first-paint interval.
    $showEventCount = Field $Measurement 'show_event_count'
    if (($showEventCount -isnot [int] -and $showEventCount -isnot [long]) -or $showEventCount -ne 1) {
        throw 'Expected exactly one observed showEvent call.'
    }
    $showEvent = Field $Measurement 'show_event_qpc'
    if (@($showEvent.PSObject.Properties).Count -ne 2) { throw 'Expected exactly two showEvent QPC timestamps.' }
    $showEventEnter = Decimal-Ticks $showEvent 'enter'
    $showEventReturned = Decimal-Ticks $showEvent 'returned'
    if ($showEventEnter -lt $showBegin -or $showEventReturned -lt $showEventEnter -or
        $showEventReturned -gt $showReturned) {
        throw 'showEvent QPC timestamps fall outside the observed show call or are reversed.'
    }
    # No ordering against first paint is assumed for the nested showEvent span.
    $counts = Field $Measurement 'counts'
    if (@($counts.PSObject.Properties).Count -ne 5) { throw 'Expected exactly five factory/instance counters.' }
    foreach ($name in 'heavy', 'tools', 'documents', 'persistence', 'inference') {
        $count = Field $counts $name
        if (($count -isnot [int] -and $count -isnot [long]) -or $count -ne 0) {
            throw "Nonzero or invalid first-paint counter: $name"
        }
    }
    return [double]($nanoseconds / [decimal]1000000)
}
function Distribution($Samples) {
    $values = @($Samples | Where-Object { $_.success } | ForEach-Object { [double]$_.first_paint_ms } | Sort-Object)
    if ($values.Count -eq 0) { return [ordered]@{ count = 0; p50_ms = $null; p95_ms = $null; max_ms = $null } }
    return [ordered]@{
        count = $values.Count; p50_ms = $values[[int][Math]::Ceiling(0.50 * $values.Count) - 1]
        p95_ms = $values[[int][Math]::Ceiling(0.95 * $values.Count) - 1]; max_ms = $values[-1]
    }
}

if ($env:OS -ne 'Windows_NT') { throw 'This measurement runner requires Windows.' }
$inputExe = Full-Path $Exe
$output = Full-Path $OutputDirectory
if (-not (Test-Path -LiteralPath $inputExe -PathType Leaf)) { throw 'Exe does not identify a file.' }
if ($output.TrimEnd('\', '/') -eq [IO.Path]::GetPathRoot($output).TrimEnd('\', '/')) { throw 'OutputDirectory cannot be a filesystem root.' }
if (Test-Path -LiteralPath $output) {
    if (-not (Test-Path -LiteralPath $output -PathType Container) -or
        @(Get-ChildItem -LiteralPath $output -Force).Count -ne 0) { throw 'OutputDirectory must be new or empty.' }
}
$ancestor = $output
while ($ancestor) {
    if (Test-Path -LiteralPath $ancestor) {
        if (((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Reparse-point output paths are not allowed.'
        }
    }
    $ancestor = Split-Path -Parent $ancestor
}
[void][IO.Directory]::CreateDirectory($output)
$report = [ordered]@{
    schema = 'startup-composition-benchmark-v3'; started_utc = [DateTime]::UtcNow.ToString('o'); finished_utc = $null
    exe = $inputExe; exe_sha256 = (Sha256 $inputExe); exe_sha256_after = $null; binary_identity_stable = $false
    rounds = $Rounds; planned_samples = 2 * $Rounds; deadline_ms = $deadlineMs; success = $false
    classification = $(if ($Rounds -lt 30) { 'validity-pilot' } else { 'measurement-series-needs-review' })
    scope = 'Isolated composition test host: QApplication completion through actual first-paint callback; includes composition and unused registration cost.'
    exclusions = 'Not formal EXE startup, external process wait, input-to-render latency, or a controlled cold/warm-cache benchmark. No performance pass is inferred.'
    order = 'Odd rounds baseline then stress; even rounds stress then baseline; serial independent processes.'
    percentiles = 'Nearest rank ceil(p*N), successful values only; all failed samples retained and counted. First round and subsequent rounds reported separately.'
    parent_timing = 'process_wait_ms ends when WaitForExit returns; parent_elapsed_ms also includes JSON validation and process cleanup. Neither is first-paint latency.'
    stage_timing = 'stage_qpc preserves five decimal QPC timestamps; show_event_qpc records one real MainWindow::showEvent span inside show(). Neither span constrains first-paint ordering beyond show_begin. Stage percentiles are not additive; the event span is not a WTS-only measurement.'
    windows = [Environment]::OSVersion.VersionString; cpu = $env:PROCESSOR_IDENTIFIER
    logical_processors = [Environment]::ProcessorCount; ram_bytes = 'unknown'; qt = 'unknown'; compiler = 'unknown'
    build_configuration = 'unknown'; kdbx = 'unknown'; powershell = $PSVersionTable.PSVersion.ToString()
    metadata_notes = @(); samples = @(); groups = @(); stress_minus_baseline_p95_ms = $null; errors = @()
}
$report.metadata_notes += 'Environment metadata only; no CIM/WMI or network probing. RAM/Qt/build fields require separately verified evidence.'
$summaryPath = Join-Path $output 'summary.json'
Write-Json $summaryPath $report

try {
    for ($round = 1; $round -le $Rounds; ++$round) {
        $order = if ($round % 2) { @('baseline', 'stress') } else { @('stress', 'baseline') }
        foreach ($mode in $order) {
            $name = 'round-{0:D4}-{1}' -f $round, $mode
            $directory = Join-Path $output $name
            [void][IO.Directory]::CreateDirectory($directory)
            $stdout = Join-Path $directory 'stdout.log'
            $stderr = Join-Path $directory 'stderr.log'
            $samplePath = Join-Path $directory 'sample.json'
            $logPath = Join-Path $directory 'run.log'
            $sample = [ordered]@{
                round = $round; mode = $mode; sequence = $report.samples.Count + 1
                started_utc = [DateTime]::UtcNow.ToString('o'); finished_utc = $null; pid = $null
                arguments = @('--measure-startup=' + $mode); stdout = $stdout; stderr = $stderr; log = $logPath
                process_wait_ms = $null; parent_elapsed_ms = $null; normal_exit = $false; forced_termination = $false; exit_code = $null
                success = $false; first_paint_ms = $null; measurement = $null; errors = @()
            }
            $report.samples += $sample
            Write-Json $samplePath $sample
            Write-Json $summaryPath $report
            [IO.File]::WriteAllText($stdout, '', $utf8)
            [IO.File]::WriteAllText($stderr, '', $utf8)
            [IO.File]::WriteAllText($logPath, ($sample.started_utc + " starting $mode`r`n"), $utf8)
            $owned = $null
            $cleanupFailed = $false
            $watch = [Diagnostics.Stopwatch]::StartNew()
            try {
                $owned = Start-Process -FilePath $inputExe -ArgumentList $sample.arguments -WorkingDirectory (Split-Path -Parent $inputExe) -WindowStyle Normal -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
                $sample.pid = $owned.Id
                $null = $owned.Handle # Retain the exact process handle through exit; never re-find by PID/name.
                $remaining = [Math]::Max(0, $deadlineMs - [int][Math]::Ceiling($watch.Elapsed.TotalMilliseconds))
                $exited = $owned.WaitForExit($remaining)
                $sample.process_wait_ms = $watch.Elapsed.TotalMilliseconds
                if (-not $exited -or $sample.process_wait_ms -gt $deadlineMs) {
                    throw 'Process exceeded the 10000 ms external deadline.'
                }
                $sample.exit_code = $owned.ExitCode
                $sample.normal_exit = $sample.exit_code -eq 0
                $lines = @([IO.File]::ReadAllLines($stdout, $strictUtf8) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
                if ($lines.Count -ne 1) { throw 'Expected exactly one nonempty JSON stdout line.' }
                $sample.measurement = $lines[0] | ConvertFrom-Json -ErrorAction Stop
                $sample.first_paint_ms = Check-Measurement $sample.measurement $mode $sample.pid $sample.exit_code
                $sample.success = $true
            } catch {
                $sample.errors += $_.Exception.Message
            } finally {
                if ($owned) {
                    try {
                        if (-not $owned.HasExited) {
                            $owned.Kill() # Only the process object returned by this sample's Start-Process.
                            $sample.forced_termination = $true
                            if (-not $owned.WaitForExit(2000)) { throw 'Owned process did not exit after timeout termination.' }
                        }
                        $sample.exit_code = $owned.ExitCode
                    } catch {
                        $sample.errors += ('Owned process cleanup: ' + $_.Exception.Message)
                        $cleanupFailed = $true
                    }
                    finally { $owned.Dispose() }
                }
                $sample.parent_elapsed_ms = $watch.Elapsed.TotalMilliseconds
                $sample.finished_utc = [DateTime]::UtcNow.ToString('o')
                $sample.success = $sample.success -and $sample.normal_exit -and -not $sample.forced_termination -and $sample.errors.Count -eq 0
                [IO.File]::AppendAllText($logPath, ($sample.finished_utc + ' exit=' + $sample.exit_code + ' success=' + $sample.success + ' errors=' + ($sample.errors -join ' | ') + "`r`n"), $utf8)
                Write-Json $samplePath $sample
                Write-Json $summaryPath $report # Preserve every sample immediately, including failed launches/timeouts.
            }
            if ($cleanupFailed) { throw 'Owned process termination unconfirmed; refusing to launch another sample.' }
        }
    }
} catch { $report.errors += $_.Exception.Message }
finally {
    try {
        $report.exe_sha256_after = Sha256 $inputExe
        $report.binary_identity_stable = $report.exe_sha256_after -eq $report.exe_sha256
        if (-not $report.binary_identity_stable) { $report.errors += 'Executable changed during this run.' }
    } catch { $report.errors += ('Final executable identity: ' + $_.Exception.Message) }
    foreach ($mode in 'baseline', 'stress') {
        $samples = @($report.samples | Where-Object { $_.mode -eq $mode })
        $passed = @($samples | Where-Object { $_.success }).Count
        $first = @($samples | Where-Object { $_.round -eq 1 })
        $report.groups += [ordered]@{
            mode = $mode; total = $samples.Count; successful = $passed; failed = $samples.Count - $passed
            first_round = $(if ($first.Count) { $first[0] } else { $null })
            all_successful = (Distribution $samples)
            subsequent_successful = (Distribution @($samples | Where-Object { $_.round -gt 1 }))
        }
    }
    $baseline = $report.groups[0].all_successful
    $stress = $report.groups[1].all_successful
    if ($baseline.count -gt 0 -and $stress.count -gt 0) {
        $report.stress_minus_baseline_p95_ms = $stress.p95_ms - $baseline.p95_ms
    }
    $failed = @($report.samples | Where-Object { -not $_.success }).Count
    $report.success = $report.errors.Count -eq 0 -and $failed -eq 0 -and $report.samples.Count -eq $report.planned_samples -and $report.binary_identity_stable
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    Write-Json $summaryPath $report
}
Write-Output $summaryPath
if (-not $report.success) { exit 1 }
exit 0
