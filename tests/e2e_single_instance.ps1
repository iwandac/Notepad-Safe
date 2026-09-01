param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,
    [Parameter(Mandatory = $true)]
    [string]$OwnerHelper,
    [ValidateSet('All', 'MissingPath', 'ForceKill', 'CleanRestoreSame', 'CleanRestoreAlias')]
    [string]$Scenario = 'All'
)

$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$ownerHelperPath = (Resolve-Path -LiteralPath $OwnerHelper).Path
$testRoot = [IO.Path]::Combine(
    [IO.Path]::GetTempPath(),
    'notepadsafe-single-instance-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$ownedProcesses = New-Object System.Collections.ArrayList
$secondaryFailureTitle = [string]::Concat(
    [char]0x65E0, [char]0x6CD5, [char]0x8FDE, [char]0x63A5,
    [char]0x5230, [char]0x73B0, [char]0x6709, [char]0x7A97,
    [char]0x53E3)
$openErrorTitle = [string]::Concat([char]0x9519, [char]0x8BEF)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NotepadSafeWindowProbe {
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maximum);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);

    public static IntPtr FindVisibleWindow(int processId, string expectedTitle) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr hwnd, IntPtr ignored) {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner != (uint)processId || !IsWindowVisible(hwnd))
                return true;
            int length = GetWindowTextLengthW(hwnd);
            StringBuilder title = new StringBuilder(length + 1);
            GetWindowTextW(hwnd, title, title.Capacity);
            if (String.Equals(title.ToString(), expectedTitle, StringComparison.Ordinal)) {
                found = hwnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string[] VisibleTitles(int processId) {
        List<string> titles = new List<string>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr ignored) {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner == (uint)processId && IsWindowVisible(hwnd)) {
                int length = GetWindowTextLengthW(hwnd);
                StringBuilder title = new StringBuilder(length + 1);
                GetWindowTextW(hwnd, title, title.Capacity);
                titles.Add(title.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return titles.ToArray();
    }

    public static bool CloseWindow(IntPtr hwnd) {
        return hwnd != IntPtr.Zero && PostMessageW(hwnd, 0x0010, IntPtr.Zero, IntPtr.Zero);
    }

    public static bool IsVisibleEnabledWindow(int processId, IntPtr hwnd) {
        uint owner;
        GetWindowThreadProcessId(hwnd, out owner);
        return hwnd != IntPtr.Zero && owner == (uint)processId
            && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd);
    }
}
'@

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Start-TestApp([string]$Profile, [string[]]$Arguments,
                       [string]$WorkingDirectory) {
    $oldProfile = $env:NOTEPADSAFE_DATADIR
    $env:NOTEPADSAFE_DATADIR = $Profile
    try {
        $parameters = @{
            FilePath = $exePath
            PassThru = $true
            WorkingDirectory = $WorkingDirectory
            WindowStyle = 'Normal'
        }
        if ($Arguments -and $Arguments.Count -gt 0) {
            $parameters.ArgumentList = $Arguments
        }
        $process = Start-Process @parameters
        [void]$ownedProcesses.Add($process)
        return $process
    } finally {
        if ($null -eq $oldProfile) {
            Remove-Item Env:NOTEPADSAFE_DATADIR -ErrorAction SilentlyContinue
        } else {
            $env:NOTEPADSAFE_DATADIR = $oldProfile
        }
    }
}

function Wait-NormalWindow($Process, [int]$TimeoutSeconds = 10) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "process $($Process.Id) exited before showing the primary window: $($Process.ExitCode)"
        }
        if ($Process.MainWindowHandle -ne 0) {
            Assert-True ($Process.MainWindowTitle -ne $secondaryFailureTitle) `
                "process $($Process.Id) displayed the secondary failure dialog"
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "process $($Process.Id) did not show a window within $TimeoutSeconds seconds"
}

function Read-OwnedRecoveryUi($Process, [long]$StartTicks, [bool]$IncludeTabs = $true) {
    $Process.Refresh()
    Assert-True (-not $Process.HasExited) 'recovery owner exited before public completion'
    Assert-True ($Process.StartTime.ToUniversalTime().Ticks -eq $StartTicks) `
        'recovery owner identity changed'
    $handle = [IntPtr]$Process.MainWindowHandle
    Assert-True ([NotepadSafeWindowProbe]::IsVisibleEnabledWindow($Process.Id, $handle)) `
        'recovery UI is not the visible enabled window of the owned process'
    $root = [Windows.Automation.AutomationElement]::FromHandle($handle)
    Assert-True ($root.Current.ProcessId -eq $Process.Id) 'UIA recovery root has another owner'
    $statusCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty,
        [Windows.Automation.ControlType]::StatusBar)
    $tabCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty,
        [Windows.Automation.ControlType]::Tab)
    $status = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $statusCondition)
    $tabBar = if ($IncludeTabs) {
        $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $tabCondition)
    } else { $null }
    $statusText = ''
    $names = @()
    $selected = @()
    if ($status -and $status.Current.ProcessId -eq $Process.Id -and -not $status.Current.IsOffscreen) {
        $statusText = $status.Current.Name
    }
    if ($tabBar -and $tabBar.Current.ProcessId -eq $Process.Id -and
        $tabBar.Current.ClassName -eq 'QTabBar') {
        $itemCondition = New-Object Windows.Automation.PropertyCondition(
            [Windows.Automation.AutomationElement]::ControlTypeProperty,
            [Windows.Automation.ControlType]::TabItem)
        foreach ($item in $tabBar.FindAll([Windows.Automation.TreeScope]::Children, $itemCondition)) {
            Assert-True ($item.Current.ProcessId -eq $Process.Id) 'recovery tab has another owner'
            $names += $item.Current.Name
        }
        $pattern = $null
        if ($tabBar.TryGetCurrentPattern([Windows.Automation.SelectionPattern]::Pattern, [ref]$pattern)) {
            foreach ($item in $pattern.Current.GetSelection()) {
                Assert-True ($item.Current.ProcessId -eq $Process.Id) 'selected recovery tab has another owner'
                $selected += $item.Current.Name
            }
        }
    }
    # Read only public chrome; never request editor TextPattern/ValuePattern.
    return [pscustomobject]@{ Status = $statusText; Tabs = $names; Selected = $selected }
}

function Wait-CleanRecoveryUi($Process, [string]$ExpectedFileName) {
    $startTicks = $Process.StartTime.ToUniversalTime().Ticks
    $budget = [Diagnostics.Stopwatch]::StartNew()
    $completedText = [string]::Concat([char]0x5DF2, [char]0x6062, [char]0x590D,
        ' 1 ', [char]0x4E2A, [char]0x6807, [char]0x7B7E, [char]0x9875)
    $last = $null
    while ($budget.ElapsedMilliseconds -lt 10000) {
        $last = Read-OwnedRecoveryUi $Process $startTicks
        if ($last.Status -eq $completedText -and @($last.Tabs).Count -eq 1 -and
            $last.Tabs[0] -eq $ExpectedFileName -and @($last.Selected).Count -eq 1 -and
            $last.Selected[0] -eq $ExpectedFileName) {
            Write-Output "CLEAN_RECOVERY_UI pid=$($Process.Id) status='$($last.Status)' tabs=1 selected='$ExpectedFileName'"
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "clean recovery public completion was not observed within 10 seconds: $($last | ConvertTo-Json -Compress)"
}

function Test-RestoreQueueInProgress($Ui) {
    $prefix = [string]::Concat([char]0x6B63, [char]0x5728, [char]0x6062,
        [char]0x590D, [char]0x4F1A, [char]0x8BDD, [char]0x2026)
    # WinPS 5.1's DLR cannot bind the four-char Concat overload reliably.
    [string]$suffix = -join [char[]]@(0x4E2A, 0x6807, 0x7B7E, 0x9875)
    [string]$pattern = '^' + [regex]::Escape($prefix) + ' ([0-9]+)/200 ' + [regex]::Escape($suffix) + '$'
    [string]$status = $Ui.Status
    $match = [regex]::Match($status, $pattern)
    return $match.Success -and [int]$match.Groups[1].Value -le 200
}

function Wait-RestoreQueueWindow($Process, [long]$StartTicks) {
    $budget = [Diagnostics.Stopwatch]::StartNew()
    $last = $null
    while ($budget.ElapsedMilliseconds -lt 10000) {
        $last = Read-OwnedRecoveryUi $Process $StartTicks $false
        if (Test-RestoreQueueInProgress $last) {
            Write-Output "RESTORE_WINDOW_UI pid=$($Process.Id) status='$($last.Status)'"
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "fixture did not expose a real 200-tab restore window within 10 seconds; status='$($last.Status)'"
}

function Wait-RestoreQueueDelivered($Process, [long]$StartTicks, [string[]]$Paths,
                                     [string]$ExternalName) {
    $budget = [Diagnostics.Stopwatch]::StartNew()
    $expectedNames = @($Paths | ForEach-Object { [IO.Path]::GetFileName($_) })
    $last = $null
    while ($budget.ElapsedMilliseconds -lt 15000) {
        $last = Read-OwnedRecoveryUi $Process $StartTicks
        $allPresent = @($last.Tabs).Count -eq 201
        foreach ($name in $expectedNames) {
            $allPresent = $allPresent -and @($last.Tabs | Where-Object { $_ -ceq $name }).Count -eq 1
        }
        if ($allPresent -and @($last.Selected).Count -eq 1 -and $last.Selected[0] -ceq $ExternalName) {
            Write-Output "RESTORE_DELIVERED_UI pid=$($Process.Id) tabs=201 selected='$ExternalName'"
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "restore did not publicly deliver all 201 tabs and select the external file within 15 seconds; status='$($last.Status)' tabs=$(@($last.Tabs).Count) selected='$($last.Selected -join ',')'"
}

function Wait-WindowTitle($Process, [string]$ExpectedTitle,
                          [int]$TimeoutSeconds = 10) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "process $($Process.Id) exited before displaying '$ExpectedTitle'"
        }
        if ($Process.MainWindowHandle -ne 0 -and
            $Process.MainWindowTitle -eq $ExpectedTitle) {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "process $($Process.Id) did not display '$ExpectedTitle' within $TimeoutSeconds seconds"
}

function Wait-OwnedWindowTitle($Process, [string]$ExpectedTitle,
                               [int]$TimeoutSeconds = 10) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "process $($Process.Id) exited before displaying owned window '$ExpectedTitle'"
        }
        $handle = [NotepadSafeWindowProbe]::FindVisibleWindow(
            $Process.Id, $ExpectedTitle)
        if ($handle -ne [IntPtr]::Zero) { return $handle }
        Start-Sleep -Milliseconds 25
    }
    $titles = [NotepadSafeWindowProbe]::VisibleTitles($Process.Id) -join ', '
    throw "process $($Process.Id) did not display owned window '$ExpectedTitle' within $TimeoutSeconds seconds; visible=[$titles]"
}

function Wait-SecondaryExit($Process, [string]$Label) {
    if (-not $Process.WaitForExit(8000)) {
        $Process.Refresh()
        $failureTitle = $Process.MainWindowTitle
        $failureHandle = $Process.MainWindowHandle
        if ($failureTitle -eq $secondaryFailureTitle) {
            [void]$Process.CloseMainWindow()
            Assert-True ($Process.WaitForExit(5000)) `
                "$Label failure dialog did not close"
            $Process.WaitForExit()
            $Process.Refresh()
            foreach ($candidate in $ownedProcesses) {
                $candidate.Refresh()
                if ($candidate.Id -ne $Process.Id -and -not $candidate.HasExited) {
                    Stop-Process -Id $candidate.Id -Force
                    Wait-Process -Id $candidate.Id -Timeout 5
                }
            }
            throw "$Label failed closed; exit=$($Process.ExitCode) title='$failureTitle' handle=$failureHandle"
        }
        throw "$Label did not exit; title='$failureTitle' handle=$failureHandle"
    }
    $Process.WaitForExit()
    $Process.Refresh()
    $exitCode = [int]$Process.ExitCode
    if ($exitCode -ne 0) {
        foreach ($candidate in $ownedProcesses) {
            $candidate.Refresh()
            if ($candidate.Id -ne $Process.Id -and -not $candidate.HasExited) {
                Stop-Process -Id $candidate.Id -Force
                [void]$candidate.WaitForExit(5000)
            }
        }
        throw "$Label exited with $exitCode, expected forwarded exit 0"
    }
}

function Wait-OwnedDialogDismissed($Process, [string]$DialogTitle,
                                   [IntPtr]$PrimaryHandle,
                                   [Diagnostics.Stopwatch]$Budget,
                                   [int]$TimeoutMilliseconds) {
    while ($Budget.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        $Process.Refresh()
        Assert-True (-not $Process.HasExited) `
            "primary $($Process.Id) exited while dismissing its error dialog"
        $dialog = [NotepadSafeWindowProbe]::FindVisibleWindow($Process.Id, $DialogTitle)
        if ($dialog -eq [IntPtr]::Zero -and
            [NotepadSafeWindowProbe]::IsVisibleEnabledWindow($Process.Id, $PrimaryHandle)) {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "primary $($Process.Id) did not dismiss its error and re-enable its window within the existing UI budget"
}

function Close-Primary($Process) {
    $Process.Refresh()
    Assert-True (-not $Process.HasExited) "primary $($Process.Id) exited unexpectedly"
    Assert-True $Process.CloseMainWindow() "could not request a graceful close for $($Process.Id)"
    Assert-True ($Process.WaitForExit(12000)) `
        "primary $($Process.Id) did not close within 12 seconds"
    $Process.WaitForExit()
    $Process.Refresh()
    Assert-True ($Process.ExitCode -eq 0) "primary $($Process.Id) close exit was $($Process.ExitCode)"
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $bytes = $sha256.ComputeHash($stream)
            return ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-EndpointName([string]$Profile) {
    $identity = [IO.Path]::GetFullPath($Profile).ToLowerInvariant()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))
    } finally {
        $sha.Dispose()
    }
    $hex = ([BitConverter]::ToString($digest)).Replace('-', '').ToLowerInvariant()
    return 'NotepadSafe.SingleInstance.' + $hex.Substring(0, 32)
}

function Wait-Endpoint([string]$Endpoint, [int]$TimeoutSeconds = 8) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -eq $Endpoint }) {
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw "endpoint $Endpoint was not available within $TimeoutSeconds seconds"
}

function Read-Exact($Stream, [int]$Count) {
    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) { throw 'named pipe closed before a complete response' }
        $offset += $read
    }
    return $buffer
}

function Send-RawRequest([string]$Endpoint, [hashtable]$Request) {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new(
        '.', $Endpoint, [IO.Pipes.PipeDirection]::InOut,
        [IO.Pipes.PipeOptions]::None)
    try {
        $pipe.Connect(2000)
        $json = $Request | ConvertTo-Json -Compress -Depth 5
        $payload = [Text.Encoding]::UTF8.GetBytes($json)
        $header = [BitConverter]::GetBytes([uint32]$payload.Length)
        if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($header) }
        $pipe.Write($header, 0, $header.Length)
        $pipe.Write($payload, 0, $payload.Length)
        $pipe.Flush()
        $responseHeader = Read-Exact $pipe 4
        if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($responseHeader) }
        $responseSize = [BitConverter]::ToUInt32($responseHeader, 0)
        Assert-True ($responseSize -gt 0 -and $responseSize -le 65536) `
            "invalid response frame size $responseSize"
        $responseBytes = Read-Exact $pipe ([int]$responseSize)
        return ([Text.Encoding]::UTF8.GetString($responseBytes) | ConvertFrom-Json)
    } finally {
        $pipe.Dispose()
    }
}

function Write-SessionFixture([string]$Profile, [string[]]$Paths) {
    $tabs = @()
    foreach ($path in $Paths) {
        $tabs += @{
            filePath = $path
            encoding = 'UTF-8'
            syntax = 0
            cursorLine = 0
            cursorIndex = 0
            modified = $false
            autosavePath = ''
        }
    }
    $json = @{ version = 2; tabs = $tabs; activeTab = 0 } |
        ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText(
        [IO.Path]::Combine($Profile, 'session.json'), $json,
        [Text.UTF8Encoding]::new($false))
}

function Read-Session([string]$Profile) {
    $path = [IO.Path]::Combine($Profile, 'session.json')
    Assert-True (Test-Path -LiteralPath $path) "session.json was not saved in $Profile"
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

try {
    if ($Scenario -in @('All', 'CleanRestoreSame', 'CleanRestoreAlias')) {
        Add-Type -AssemblyName UIAutomationClient
        Add-Type -AssemblyName UIAutomationTypes
        $cleanVariants = if ($Scenario -eq 'All') {
            @('CleanRestoreSame', 'CleanRestoreAlias')
        } else { @($Scenario) }
        foreach ($variant in $cleanVariants) {
            $cleanRoot = [IO.Path]::Combine($testRoot, $variant)
            $cleanProfile = [IO.Path]::Combine($cleanRoot, 'profile')
            [IO.Directory]::CreateDirectory($cleanProfile) | Out-Null
            $cleanPath = [IO.Path]::Combine($cleanRoot, 'already-in-session.txt')
            [IO.File]::WriteAllText($cleanPath, 'clean session and explicit CLI own one document')
            $cleanHash = Get-Sha256 $cleanPath
            $explicitPath = $cleanPath
            if ($variant -eq 'CleanRestoreAlias') {
                $explicitPath = [IO.Path]::Combine($cleanRoot, 'same-physical-file.txt')
                New-Item -ItemType HardLink -Path $explicitPath -Target $cleanPath | Out-Null
            }
            Write-SessionFixture $cleanProfile @($cleanPath)
            $cleanPrimary = Start-TestApp $cleanProfile @($explicitPath) $cleanRoot
            Wait-NormalWindow $cleanPrimary
            # The manifest remains durable recovery responsibility. Completion
            # is the owner's public status plus its real selected single tab.
            Wait-CleanRecoveryUi $cleanPrimary ([IO.Path]::GetFileName($explicitPath))
            Close-Primary $cleanPrimary
            Assert-True ((Get-Sha256 $cleanPath) -eq $cleanHash) `
                "$variant changed the source bytes"
            $cleanSaved = Read-Session $cleanProfile
            $cleanTabs = @($cleanSaved.tabs)
            Assert-True ($cleanTabs.Count -eq 1) `
                "$variant created $($cleanTabs.Count) tabs for one physical file; expected 1"
            Assert-True (-not $cleanTabs[0].modified) "$variant introduced dirty state"
            Assert-True ([int]$cleanSaved.activeTab -eq 0) "$variant did not persist its selected single tab"
            Assert-True ([string]::Equals(
                [IO.Path]::GetFullPath($cleanTabs[0].filePath),
                [IO.Path]::GetFullPath($explicitPath),
                [StringComparison]::OrdinalIgnoreCase)) `
                "$variant replaced the explicitly opened document state with the old session"
            Write-Output "$variant PASS one_document=1 source_hash_unchanged=1 dirty=0"
        }
        if ($Scenario -ne 'All') { return }
    }
    if ($Scenario -eq 'All') {
    # A/B/C: one formal executable owns the profile; relative, absolute and
    # case-variant spellings of the same file all converge on one tab.
    $abcRoot = [IO.Path]::Combine($testRoot, 'abc')
    $abcProfile = [IO.Path]::Combine($abcRoot, 'profile')
    [IO.Directory]::CreateDirectory($abcProfile) | Out-Null
    $sameFile = [IO.Path]::Combine($abcRoot, 'same-file.txt')
    [IO.File]::WriteAllText($sameFile, 'single instance process evidence')
    $a = Start-TestApp $abcProfile @('same-file.txt') $abcRoot
    Wait-NormalWindow $a
    $abcLock = [IO.Path]::Combine($abcProfile, 'notepad-safe.session.lock')
    Assert-True (Test-Path -LiteralPath $abcLock) 'primary A did not hold the session-owner lock'
    $endpoint = Get-EndpointName $abcProfile
    $b = Start-TestApp $abcProfile @($sameFile) $abcRoot
    Wait-SecondaryExit $b 'absolute-path secondary B'
    $caseVariant = $sameFile.ToUpperInvariant()
    $c = Start-TestApp $abcProfile @($caseVariant) $abcRoot
    Wait-SecondaryExit $c 'case-variant secondary C'
    $activation = Start-TestApp $abcProfile @() $abcRoot
    Wait-SecondaryExit $activation 'activation-only secondary'
    $a.Refresh()
    Assert-True (-not $a.HasExited) 'primary A did not survive B/C forwarding'

    # A raw future-version frame uses a distinct file and must meet the stable
    # endpoint without opening it or creating another writer. This injector is
    # not a supported client, so an explicit rejection frame or a closed pipe
    # are both fail-closed; the protocol unit test checks the exact code.
    $futureOnlyFile = [IO.Path]::Combine($abcRoot, 'future-only.txt')
    [IO.File]::WriteAllText($futureOnlyFile, 'must never be opened')
    $futureResponse = $null
    try {
        $protocolResponse = Send-RawRequest $endpoint @{
            version = 2
            action = 'open_files'
            paths = @($futureOnlyFile)
        }
        $futureResponse = $protocolResponse
        Assert-True ($protocolResponse.status -eq 'rejected') `
            'future protocol request was not rejected'
        Assert-True ($protocolResponse.code -eq 'incompatible_version') `
            "future protocol rejection was $($protocolResponse.code)"
    } catch {
        Assert-True ($_.Exception.Message -like '*named pipe closed*') `
            "future protocol request failed unexpectedly: $($_.Exception.Message)"
    }
    $a.Refresh()
    Assert-True (-not $a.HasExited) 'primary A did not survive future protocol injection'

    Close-Primary $a
    Assert-True (-not (Test-Path -LiteralPath $abcLock)) `
        'primary A did not release the session-owner lock on clean exit'
    $abcSession = Read-Session $abcProfile
    $abcTabs = @($abcSession.tabs)
    Assert-True ($abcTabs.Count -eq 1) "A/B/C saved $($abcTabs.Count) tabs, expected one"
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($abcTabs[0].filePath),
        [IO.Path]::GetFullPath($sameFile),
        [StringComparison]::OrdinalIgnoreCase)) 'A/B/C session path did not converge'
    Assert-True (-not (@($abcTabs.filePath) -contains $futureOnlyFile)) `
        'future-version request opened its independent fixture'
    $sessionWriters = @(Get-ChildItem -LiteralPath $abcProfile -Filter session.json -Recurse)
    Assert-True ($sessionWriters.Count -eq 1) `
        "profile contains $($sessionWriters.Count) session writers"
    Write-Output "ABC PASS primary=$($a.Id) secondaryB=$($b.Id) secondaryC=$($c.Id) activation=$($activation.Id) tabs=1 writers=1"
    Write-Output "PROTOCOL PASS endpoint=$endpoint future_version_fail_closed=1"

    # Startup race: the lock, not named-pipe listen success, decides ownership
    # before either process has a window. Exactly one survives as writer while
    # the other forwards its distinct path and exits zero.
    $raceRoot = [IO.Path]::Combine($testRoot, 'startup-race')
    $raceProfile = [IO.Path]::Combine($raceRoot, 'profile')
    [IO.Directory]::CreateDirectory($raceProfile) | Out-Null
    $raceOne = [IO.Path]::Combine($raceRoot, 'race-one.txt')
    $raceTwo = [IO.Path]::Combine($raceRoot, 'race-two.txt')
    [IO.File]::WriteAllText($raceOne, 'race one')
    [IO.File]::WriteAllText($raceTwo, 'race two')
    $raceA = Start-TestApp $raceProfile @($raceOne) $raceRoot
    $raceB = Start-TestApp $raceProfile @($raceTwo) $raceRoot
    $raceDeadline = [DateTime]::UtcNow.AddSeconds(8)
    $racePrimary = $null
    $raceSecondary = $null
    while ([DateTime]::UtcNow -lt $raceDeadline) {
        $raceA.Refresh()
        $raceB.Refresh()
        if ($raceA.HasExited -xor $raceB.HasExited) {
            if ($raceA.HasExited) {
                $raceSecondary = $raceA
                $racePrimary = $raceB
            } else {
                $raceSecondary = $raceB
                $racePrimary = $raceA
            }
            break
        }
        Start-Sleep -Milliseconds 20
    }
    Assert-True ($null -ne $racePrimary) 'startup race did not converge to one primary'
    Assert-True ($raceSecondary.ExitCode -eq 0) 'startup-race secondary did not forward successfully'
    Wait-NormalWindow $racePrimary
    # The primary window becomes visible before StartupCoordinator's two
    # queued post-paint turns open its own CLI path and drain the forwarded
    # request. Wait for the GUI queue to become idle before closing it.
    [void]$racePrimary.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1000
    Close-Primary $racePrimary
    $raceSession = Read-Session $raceProfile
    $raceTabs = @($raceSession.tabs)
    Assert-True ($raceTabs.Count -eq 2) `
        "startup race saved $($raceTabs.Count) tabs, expected two"
    Write-Output "STARTUP_RACE PASS primary=$($racePrimary.Id) secondary=$($raceSecondary.Id) secondary_exit=0 tabs=2"

    # If the owner dies after taking the lock but before listen(), the waiting
    # process may become primary only after QLockFile stale-owner detection
    # grants it that same lock.
    $startupKillRoot = [IO.Path]::Combine($testRoot, 'startup-owner-kill')
    $startupKillProfile = [IO.Path]::Combine($startupKillRoot, 'profile')
    [IO.Directory]::CreateDirectory($startupKillProfile) | Out-Null
    $startupKillPath = [IO.Path]::Combine($startupKillRoot, 'after-owner-kill.txt')
    [IO.File]::WriteAllText($startupKillPath, 'waiter becomes owner only through stale lock recovery')
    $lockOnlyOwner = Start-Process -FilePath $ownerHelperPath `
        -ArgumentList @($startupKillProfile, 'lock-only') `
        -WorkingDirectory $startupKillRoot -WindowStyle Hidden -PassThru
    [void]$ownedProcesses.Add($lockOnlyOwner)
    $startupLock = [IO.Path]::Combine(
        $startupKillProfile, 'notepad-safe.session.lock')
    $lockDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not (Test-Path -LiteralPath $startupLock) -and
           [DateTime]::UtcNow -lt $lockDeadline) {
        Start-Sleep -Milliseconds 20
    }
    Assert-True (Test-Path -LiteralPath $startupLock) `
        'lock-only startup owner did not acquire QLockFile'
    $startupWaiter = Start-TestApp $startupKillProfile @($startupKillPath) $startupKillRoot
    Start-Sleep -Milliseconds 1000
    $startupWaiter.Refresh()
    Assert-True (-not $startupWaiter.HasExited) `
        'startup waiter exited before the lock owner was killed'
    Stop-Process -Id $lockOnlyOwner.Id -Force
    Assert-True ($lockOnlyOwner.WaitForExit(5000)) `
        'lock-only startup owner did not exit after force kill'
    Wait-NormalWindow $startupWaiter
    [void]$startupWaiter.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 250
    Close-Primary $startupWaiter
    $startupKillSession = Read-Session $startupKillProfile
    $startupKillTabs = @($startupKillSession.tabs)
    Assert-True ($startupKillTabs.Count -eq 1) `
        'stale-lock replacement did not save exactly one tab'
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($startupKillTabs[0].filePath),
        [IO.Path]::GetFullPath($startupKillPath),
        [StringComparison]::OrdinalIgnoreCase)) `
        'stale-lock replacement did not open/write the requested path'
    Write-Output "STARTUP_OWNER_KILL PASS killed=$($lockOnlyOwner.Id) replacement=$($startupWaiter.Id) stale_lock_recovered=true"

    # Requests arriving after endpoint ownership but before restore completion
    # remain queued and are delivered after all 200 restored tabs.
    $restoreRoot = [IO.Path]::Combine($testRoot, 'restore')
    $restoreProfile = [IO.Path]::Combine($restoreRoot, 'profile')
    [IO.Directory]::CreateDirectory($restoreProfile) | Out-Null
    $external = [IO.Path]::Combine($restoreRoot, 'external-during-restore.txt')
    $restorePaths = @()
    for ($index = 0; $index -lt 200; $index++) {
        $path = [IO.Path]::Combine($restoreRoot, ('restored-{0:D3}.txt' -f $index))
        [IO.File]::WriteAllText($path, "restore $index")
        $restorePaths += $path
    }
    [IO.File]::WriteAllText($external, 'external request')
    Write-SessionFixture $restoreProfile $restorePaths
    $restorePrimary = Start-TestApp $restoreProfile @() $restoreRoot
    $restoreEndpoint = Get-EndpointName $restoreProfile
    Wait-Endpoint $restoreEndpoint
    Wait-NormalWindow $restorePrimary
    $restoreStartTicks = $restorePrimary.StartTime.ToUniversalTime().Ticks
    Wait-RestoreQueueWindow $restorePrimary $restoreStartTicks
    $restoreSecondary = Start-TestApp $restoreProfile @($external) $restoreRoot
    Wait-SecondaryExit $restoreSecondary 'restore-time secondary'
    $afterAckUi = Read-OwnedRecoveryUi $restorePrimary $restoreStartTicks $false
    Assert-True (Test-RestoreQueueInProgress $afterAckUi) `
        "fixture missed ACK-before-completion: after secondary exit, public status='$($afterAckUi.Status)'"
    Write-Output "RESTORE_ACK_UI pid=$($restorePrimary.Id) secondary=$($restoreSecondary.Id) status='$($afterAckUi.Status)'"
    # Manifest existence is no longer a progress signal. The external request
    # becomes a selected real tab only through delivery after restore completion;
    # require the full original tab set too, not ACK or a painted window alone.
    Wait-RestoreQueueDelivered $restorePrimary $restoreStartTicks `
        @($restorePaths + $external) ([IO.Path]::GetFileName($external))
    Close-Primary $restorePrimary
    $restoredSession = Read-Session $restoreProfile
    $restoredTabs = @($restoredSession.tabs)
    Assert-True ($restoredTabs.Count -eq 201) `
        "restore queue saved $($restoredTabs.Count) tabs, expected 201"
    $restoredPaths = @($restoredTabs | ForEach-Object { $_.filePath })
    foreach ($expected in @($restorePaths + $external)) {
        Assert-True (@($restoredPaths | Where-Object {
            [string]::Equals(
                [IO.Path]::GetFullPath($_),
                [IO.Path]::GetFullPath($expected),
                [StringComparison]::OrdinalIgnoreCase)
        }).Count -eq 1) "restore queue lost or duplicated $expected"
    }
    $activePath = $restoredTabs[[int]$restoredSession.activeTab].filePath
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($activePath),
        [IO.Path]::GetFullPath($external),
        [StringComparison]::OrdinalIgnoreCase)) `
        'restore-time external request was not deterministically active last'
    Write-Output "RESTORE_QUEUE PASS primary=$($restorePrimary.Id) secondary=$($restoreSecondary.Id) acknowledged_while_public_restore_in_progress=true tabs=201 active=external"

    # An endpoint owner that accepts the request but never ACKs must hold the
    # formal executable in secondary/fail-closed mode. Closing its bounded
    # error dialog yields a nonzero exit and no session-capable window.
    $hungRoot = [IO.Path]::Combine($testRoot, 'unresponsive')
    $hungProfile = [IO.Path]::Combine($hungRoot, 'profile')
    [IO.Directory]::CreateDirectory($hungProfile) | Out-Null
    $hungEndpoint = Get-EndpointName $hungProfile
    $hungOwner = Start-Process -FilePath $ownerHelperPath `
        -ArgumentList @($hungProfile) -WorkingDirectory $hungRoot -WindowStyle Hidden -PassThru
    [void]$ownedProcesses.Add($hungOwner)
    try {
        Wait-Endpoint $hungEndpoint
        $hungStopwatch = [Diagnostics.Stopwatch]::StartNew()
        $hungSecondary = Start-TestApp $hungProfile @() $hungRoot
        Wait-WindowTitle $hungSecondary $secondaryFailureTitle 6
        $hungStopwatch.Stop()
        Assert-True ($hungStopwatch.ElapsedMilliseconds -le 5500) `
            "unresponsive-owner handshake exceeded the 5s budget plus scheduling tolerance: $($hungStopwatch.ElapsedMilliseconds)ms"
        Assert-True $hungSecondary.CloseMainWindow() 'could not close bounded unresponsive-owner error'
        Assert-True ($hungSecondary.WaitForExit(5000)) `
            'unresponsive-owner secondary did not exit after closing its error'
        $hungSecondary.Refresh()
        Assert-True ($null -ne $hungSecondary.ExitCode) `
            'unresponsive owner secondary had no observable exit code'
        Assert-True ($hungSecondary.ExitCode -eq 5) `
            "unresponsive owner secondary exited $($hungSecondary.ExitCode), expected 5"
        Assert-True (-not (Test-Path -LiteralPath ([IO.Path]::Combine($hungProfile, 'session.json')))) `
            'unresponsive owner secondary created session.json'
        $hungOwner.Refresh()
        Assert-True (-not $hungOwner.HasExited) 'unresponsive lock owner exited unexpectedly'
        Write-Output "UNRESPONSIVE PASS secondary=$($hungSecondary.Id) exit=$($hungSecondary.ExitCode) handshake_ms=$($hungStopwatch.ElapsedMilliseconds) session=false"
    } finally {
        Stop-Process -Id $hungOwner.Id -Force -ErrorAction SilentlyContinue
    }
    }

    if ($Scenario -in @('All', 'MissingPath')) {
    # A missing external path must be forwarded rather than discarded. The
    # ACK confirms queue admission, not completion of the visible error dialog.
    # After dismissal the primary must be enabled before a graceful close, and
    # its existing valid document must remain the sole session tab.
    $missingRoot = [IO.Path]::Combine($testRoot, 'missing')
    $missingProfile = [IO.Path]::Combine($missingRoot, 'profile')
    [IO.Directory]::CreateDirectory($missingProfile) | Out-Null
    $validPath = [IO.Path]::Combine($missingRoot, 'valid.txt')
    $missingPath = [IO.Path]::Combine($missingRoot, 'does-not-exist.txt')
    [IO.File]::WriteAllText($validPath, 'preserve this valid document')
    $missingPrimary = Start-TestApp $missingProfile @($validPath) $missingRoot
    Wait-NormalWindow $missingPrimary
    $missingPrimaryHandle = $missingPrimary.MainWindowHandle
    $missingUiBudget = [Diagnostics.Stopwatch]::StartNew()
    $missingSecondary = Start-TestApp $missingProfile @($missingPath) $missingRoot
    $missingError = Wait-OwnedWindowTitle $missingPrimary $openErrorTitle 8
    # Deliberately leave the modal open until the secondary exits: receiving
    # its ACK must not depend on this harness dismissing the product error.
    Wait-SecondaryExit $missingSecondary 'missing-path secondary'
    Assert-True ([NotepadSafeWindowProbe]::CloseWindow($missingError)) `
        'could not dismiss missing-path error'
    Wait-OwnedDialogDismissed $missingPrimary $openErrorTitle $missingPrimaryHandle `
        $missingUiBudget 8000
    Close-Primary $missingPrimary
    $missingSession = Read-Session $missingProfile
    $missingTabs = @($missingSession.tabs)
    Assert-True ($missingTabs.Count -eq 1) 'missing path damaged the existing session tab count'
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($missingTabs[0].filePath),
        [IO.Path]::GetFullPath($validPath),
        [StringComparison]::OrdinalIgnoreCase)) `
        'missing path replaced or damaged the existing valid document'
    Write-Output "MISSING_PATH PASS primary=$($missingPrimary.Id) visible_error=true ack_before_dismiss=true primary_reenabled=true valid_tabs=1"

    if ($Scenario -eq 'MissingPath') { return }
    }

    # A force-killed owner releases the kernel endpoint; the next formal
    # executable must acquire it as a real primary, not show the failure modal.
    $killRoot = [IO.Path]::Combine($testRoot, 'force-kill')
    $killProfile = [IO.Path]::Combine($killRoot, 'profile')
    [IO.Directory]::CreateDirectory($killProfile) | Out-Null
    $killedPrimary = Start-TestApp $killProfile @() $killRoot
    Wait-NormalWindow $killedPrimary
    Stop-Process -Id $killedPrimary.Id -Force
    Assert-True ($killedPrimary.WaitForExit(5000)) `
        'force-killed primary did not exit'
    $replacementPath = [IO.Path]::Combine($killRoot, 'replacement-open.txt')
    [IO.File]::WriteAllText($replacementPath, 'replacement owns and writes session')
    $replacement = Start-TestApp $killProfile @($replacementPath) $killRoot
    Wait-NormalWindow $replacement
    # The top-level window precedes StartupCoordinator's queued CLI open.
    # Let that work finish before the harness requests a graceful close.
    [void]$replacement.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1000
    Close-Primary $replacement
    $replacementSession = Read-Session $killProfile
    $replacementTabs = @($replacementSession.tabs)
    Assert-True ($replacementTabs.Count -eq 1) 'replacement did not save exactly one opened tab'
    Assert-True ([string]::Equals(
        [IO.Path]::GetFullPath($replacementTabs[0].filePath),
        [IO.Path]::GetFullPath($replacementPath),
        [StringComparison]::OrdinalIgnoreCase)) `
        'replacement did not become the real session writer for its opened file'
    Write-Output "FORCE_KILL PASS killed=$($killedPrimary.Id) replacement=$($replacement.Id) writer_path=replacement-open.txt"
} finally {
    foreach ($process in $ownedProcesses) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {}
    }
    if ($testRoot.StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $testRoot).StartsWith('notepadsafe-single-instance-')) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
