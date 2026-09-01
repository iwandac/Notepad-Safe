[CmdletBinding()]
param(
    [ValidateSet("Plan", "Full")]
    [string]$Mode = "Plan",

    [string[]]$PerformanceEvidence = @(),
    [string[]]$VisualEvidence = @(),
    [string[]]$SigningEvidence = @(),

    [switch]$AllowDirty,

    [ValidateRange(1, 64)]
    [int]$Jobs = 4
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildRoot = Join-Path $repoRoot "build-release-gate"
$buildDirectories = [ordered]@{
    on = Join-Path $buildRoot "on"
    off = Join-Path $buildRoot "off"
    asan = Join-Path $buildRoot "asan"
}
$manifestPath = Join-Path $buildRoot "release-gate-manifest.json"
$manifestHashPath = "$manifestPath.sha256"
$provenancePayloadPath = Join-Path $buildRoot "release-gate-provenance-payload.json"

$stageDefinitions = @(
    [ordered]@{ id = "RG-01"; name = "source-git-preflight"; title = "Source and Git preflight" },
    [ordered]@{ id = "RG-02"; name = "supply-chain-architecture"; title = "Supply-chain and architecture gates" },
    [ordered]@{ id = "RG-03"; name = "kdbx-on-release-matrix"; title = "KDBX ON Release full matrix" },
    [ordered]@{ id = "RG-04"; name = "kdbx-off-release-matrix"; title = "KDBX OFF applicable Release matrix" },
    [ordered]@{ id = "RG-05"; name = "asan-adjacent-matrix"; title = "MSVC ASAN adjacent matrix" },
    [ordered]@{ id = "RG-06"; name = "performance-evidence"; title = "Performance evidence" },
    [ordered]@{ id = "RG-07"; name = "manual-visual-evidence"; title = "Manual visual evidence" },
    [ordered]@{ id = "RG-08"; name = "portable-package-verify"; title = "Portable package and verification" },
    [ordered]@{ id = "RG-09"; name = "signing-evidence"; title = "Signing evidence" },
    [ordered]@{ id = "RG-10"; name = "provenance-manifest"; title = "Provenance manifest" }
)
$requiredStageIds = @("RG-01", "RG-02", "RG-03", "RG-04", "RG-05",
    "RG-06", "RG-07", "RG-08", "RG-09", "RG-10")

function Assert-StageContract {
    $actualIds = @($stageDefinitions | ForEach-Object { [string]$_.id })
    if (($actualIds -join "|") -ne ($requiredStageIds -join "|")) {
        throw "Release-gate stage contract changed or omitted a required stage. Expected: $($requiredStageIds -join ', ')"
    }
    $duplicateIds = @($actualIds | Group-Object | Where-Object Count -ne 1)
    if ($duplicateIds.Count -ne 0) {
        throw "Release-gate stage contract contains duplicate IDs: $(($duplicateIds.Name) -join ', ')"
    }
}

function Format-Command([string]$FilePath, [string[]]$Arguments) {
    $formatted = @($FilePath)
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') {
            $formatted += '"' + ($argument -replace '"', '\"') + '"'
        }
        else {
            $formatted += $argument
        }
    }
    return $formatted -join " "
}

function Get-Sha256([string]$Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $stream = [IO.File]::OpenRead($resolved)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha256.ComputeHash($stream)
        return ([BitConverter]::ToString($bytes)).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Resolve-GateInputPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-ProjectVersion {
    $cmakePath = Join-Path $repoRoot "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        return "unknown"
    }
    $content = Get-Content -LiteralPath $cmakePath -Raw
    $match = [regex]::Match($content, '(?im)^project\(notepad-safe\s+VERSION\s+([^\s\)]+)')
    if (-not $match.Success) {
        return "unknown"
    }
    return $match.Groups[1].Value
}

function Resolve-VsRoot {
    if (-not [string]::IsNullOrWhiteSpace($env:NOTEPADSAFE_VS_ROOT)) {
        return [IO.Path]::GetFullPath($env:NOTEPADSAFE_VS_ROOT)
    }
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $savedPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $vsWhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath 2>&1
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedPreference
        }
        if ($exitCode -ne 0) {
            throw "vswhere failed with exit code $exitCode"
        }
        $candidate = @($output | ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -Last 1)
        if ($candidate.Count -eq 1) {
            return [IO.Path]::GetFullPath($candidate[0])
        }
    }
    return "C:\Program Files\Microsoft Visual Studio\18\Insiders"
}

function Get-ToolchainInfo {
    $vsRoot = Resolve-VsRoot
    $cmake = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    $ctest = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
    $ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
    $qtRoot = if (-not [string]::IsNullOrWhiteSpace($env:NOTEPADSAFE_QT6_DIR)) {
        [IO.Path]::GetFullPath($env:NOTEPADSAFE_QT6_DIR)
    }
    else {
        "E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64"
    }
    $msvcRoot = Join-Path $vsRoot "VC\Tools\MSVC"
    $msvcVersion = "unknown"
    if (Test-Path -LiteralPath $msvcRoot -PathType Container) {
        $latestMsvc = Get-ChildItem -LiteralPath $msvcRoot -Directory |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($null -ne $latestMsvc) {
            $msvcVersion = $latestMsvc.Name
        }
    }
    $cmakeVersion = if (Test-Path -LiteralPath $cmake -PathType Leaf) {
        (Get-Item -LiteralPath $cmake).VersionInfo.ProductVersion
    } else { "missing" }
    $ctestVersion = if (Test-Path -LiteralPath $ctest -PathType Leaf) {
        (Get-Item -LiteralPath $ctest).VersionInfo.ProductVersion
    } else { "missing" }
    $ninjaVersion = if (Test-Path -LiteralPath $ninja -PathType Leaf) {
        (Get-Item -LiteralPath $ninja).VersionInfo.ProductVersion
    } else { "missing" }
    $qtVersion = "unknown"
    $qtVersionMatch = [regex]::Match($qtRoot, '(?i)Qt[_-]([0-9]+\.[0-9]+\.[0-9]+)')
    if ($qtVersionMatch.Success) {
        $qtVersion = $qtVersionMatch.Groups[1].Value
    }
    return [ordered]@{
        visual_studio_root = $vsRoot
        msvc_version = $msvcVersion
        cmake = $cmake
        cmake_version = $cmakeVersion
        ctest = $ctest
        ctest_version = $ctestVersion
        ninja = $ninja
        ninja_version = $ninjaVersion
        vcvars = $vcvars
        qt_root = $qtRoot
        qt_version = $qtVersion
    }
}

function New-StageResult($Definition) {
    return [ordered]@{
        id = [string]$Definition.id
        name = [string]$Definition.name
        title = [string]$Definition.title
        status = "PENDING"
        started_utc = $null
        duration_seconds = 0.0
        commands = [Collections.ArrayList]::new()
        evidence = [Collections.ArrayList]::new()
        error = $null
    }
}

function Add-CommandRecord($Stage, [string]$Command, [int]$ExitCode,
        [double]$DurationSeconds) {
    [void]$Stage.commands.Add([ordered]@{
        command = $Command
        exit_code = $ExitCode
        duration_seconds = [Math]::Round($DurationSeconds, 3)
    })
}

function Invoke-GateCommand($Stage, [string]$FilePath, [string[]]$Arguments) {
    $display = Format-Command $FilePath $Arguments
    Write-Host "  > $display"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $FilePath @Arguments
        $exitCode = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    $timer.Stop()
    Add-CommandRecord $Stage $display $exitCode $timer.Elapsed.TotalSeconds
    if ($exitCode -ne 0) {
        throw "External command failed with exit code ${exitCode}: $display"
    }
}

function Invoke-GateCommandCapture($Stage, [string]$FilePath, [string[]]$Arguments) {
    $display = Format-Command $FilePath $Arguments
    Write-Host "  > $display"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { [string]$_ })
        $exitCode = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    $timer.Stop()
    Add-CommandRecord $Stage $display $exitCode $timer.Elapsed.TotalSeconds
    if ($exitCode -ne 0) {
        if ($output.Count -gt 0) {
            $output | ForEach-Object { Write-Host "    $_" }
        }
        throw "External command failed with exit code ${exitCode}: $display"
    }
    return $output
}

function Invoke-Stage($Stage, [scriptblock]$Action) {
    Write-Host "[$($Stage.id)] $($Stage.title)"
    $Stage.status = "RUNNING"
    $Stage.started_utc = [DateTime]::UtcNow.ToString("o")
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Action
        $Stage.status = "PASSED"
        Write-Host "[$($Stage.id)] PASSED"
    }
    catch {
        $Stage.status = "FAILED"
        $Stage.error = $_.Exception.Message
        Write-Host "[$($Stage.id)] FAILED: $($Stage.error)" -ForegroundColor Red
        throw
    }
    finally {
        $timer.Stop()
        $Stage.duration_seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
    }
}

function Add-Evidence($Stage, [string]$Path, [string]$Kind) {
    $resolved = (Resolve-Path -LiteralPath (Resolve-GateInputPath $Path)).Path
    $item = Get-Item -LiteralPath $resolved
    [void]$Stage.evidence.Add([ordered]@{
        kind = $Kind
        path = $resolved
        bytes = [int64]$item.Length
        sha256 = Get-Sha256 $resolved
    })
}

function Assert-JsonProperty($Object, [string]$Name, [string]$Kind) {
    if ($null -eq $Object -or
        -not ($Object.PSObject.Properties.Name -contains $Name)) {
        throw "$Kind JSON is missing required property '$Name'"
    }
}

function Read-EvidenceJson([string]$Path, [string]$Kind) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Kind evidence contains an empty path"
    }
    $fullPath = Resolve-GateInputPath $Path
    if ([IO.Path]::GetExtension($fullPath) -ne ".json") {
        throw "$Kind evidence must be a JSON file: $fullPath"
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Kind evidence file does not exist: $fullPath"
    }
    if ((Get-Item -LiteralPath $fullPath).Length -le 0) {
        throw "$Kind evidence file is empty: $fullPath"
    }
    try {
        return Get-Content -LiteralPath $fullPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    catch {
        throw "$Kind evidence is not valid JSON: $fullPath ($($_.Exception.Message))"
    }
}

function Assert-CommonEvidenceJson($Document, [string]$Kind, [string]$Commit) {
    foreach ($name in @("schema_version", "commit", "passed")) {
        Assert-JsonProperty $Document $name $Kind
    }
    if ([int]$Document.schema_version -ne 1) {
        throw "$Kind schema_version must be 1"
    }
    if ([string]$Document.commit -ne $Commit) {
        throw "$Kind commit must equal RG-01 HEAD ($Commit)"
    }
    if ($Document.passed -ne $true) {
        throw "$Kind passed must be true"
    }
}

function Assert-MetricResult($Results, [string]$Name) {
    Assert-JsonProperty $Results $Name "performance results"
    $metric = $Results.$Name
    foreach ($property in @("value", "unit", "passed")) {
        Assert-JsonProperty $metric $property "performance result '$Name'"
    }
    $value = 0.0
    if (-not [double]::TryParse([string]$metric.value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0) {
        throw "performance result '$Name'.value must be a finite non-negative number"
    }
    if ([string]::IsNullOrWhiteSpace([string]$metric.unit)) {
        throw "performance result '$Name'.unit must be non-empty"
    }
    if ($metric.passed -ne $true) {
        throw "performance result '$Name'.passed must be true"
    }
    return $value
}

function Assert-PerformanceEvidence([string]$Path, [string]$Commit) {
    $document = Read-EvidenceJson $Path "performance"
    Assert-CommonEvidenceJson $document "performance" $Commit
    foreach ($name in @("sample_count", "results")) {
        Assert-JsonProperty $document $name "performance"
    }
    if ([int]$document.sample_count -lt 30) {
        throw "performance sample_count must be at least 30"
    }
    $p50 = Assert-MetricResult $document.results "p50"
    $p95 = Assert-MetricResult $document.results "p95"
    [void](Assert-MetricResult $document.results "memory")
    [void](Assert-MetricResult $document.results "ui_stall")
    [void](Assert-MetricResult $document.results "cancel")
    if ($p95 -lt $p50) {
        throw "performance p95 must be greater than or equal to p50"
    }
    if ([string]$document.results.p50.unit -ne [string]$document.results.p95.unit) {
        throw "performance p50 and p95 must use the same unit"
    }
    return $document
}

function Resolve-VisualArtifactPath([string]$EvidencePath, [string]$ArtifactPath) {
    if ([IO.Path]::IsPathRooted($ArtifactPath)) {
        return [IO.Path]::GetFullPath($ArtifactPath)
    }
    $resolvedEvidence = Resolve-GateInputPath $EvidencePath
    $evidenceDirectory = Split-Path -Parent (Resolve-Path -LiteralPath $resolvedEvidence).Path
    return [IO.Path]::GetFullPath((Join-Path $evidenceDirectory $ArtifactPath))
}

function Assert-VisualEvidence([string]$Path, [string]$Commit) {
    $document = Read-EvidenceJson $Path "manual-visual"
    Assert-CommonEvidenceJson $document "manual-visual" $Commit
    foreach ($name in @("reviewer", "matrix_id", "artifacts")) {
        Assert-JsonProperty $document $name "manual-visual"
    }
    if ([string]::IsNullOrWhiteSpace([string]$document.reviewer)) {
        throw "manual-visual reviewer must be non-empty"
    }
    if ([string]::IsNullOrWhiteSpace([string]$document.matrix_id)) {
        throw "manual-visual matrix_id must be non-empty"
    }
    $artifacts = @($document.artifacts)
    if ($artifacts.Count -eq 0) {
        throw "manual-visual artifacts must contain at least one item"
    }
    foreach ($artifact in $artifacts) {
        foreach ($name in @("path", "sha256")) {
            Assert-JsonProperty $artifact $name "manual-visual artifact"
        }
        if ([string]::IsNullOrWhiteSpace([string]$artifact.path)) {
            throw "manual-visual artifact path must be non-empty"
        }
        if ([string]$artifact.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "manual-visual artifact sha256 must contain 64 hex digits"
        }
        $artifactPath = Resolve-VisualArtifactPath $Path ([string]$artifact.path)
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            throw "manual-visual artifact does not exist: $artifactPath"
        }
        if ((Get-Item -LiteralPath $artifactPath).Length -le 0) {
            throw "manual-visual artifact is empty: $artifactPath"
        }
        $actualHash = Get-Sha256 $artifactPath
        if ($actualHash -ne ([string]$artifact.sha256).ToLowerInvariant()) {
            throw "manual-visual artifact hash mismatch: $artifactPath"
        }
    }
    return $document
}

function Get-StructuredEvidenceBlockers([string[]]$Paths, [string]$Kind,
        [string]$Commit) {
    $failures = [Collections.Generic.List[string]]::new()
    if (@($Paths).Count -eq 0) {
        $failures.Add("$Kind evidence was not supplied")
        return $failures.ToArray()
    }
    foreach ($path in @($Paths)) {
        try {
            if ($Kind -eq "performance") {
                [void](Assert-PerformanceEvidence $path $Commit)
            }
            else {
                [void](Assert-VisualEvidence $path $Commit)
            }
        }
        catch {
            $failures.Add($_.Exception.Message)
        }
    }
    return $failures.ToArray()
}

function Get-SigningPathBlockers([string[]]$Paths) {
    $failures = [Collections.Generic.List[string]]::new()
    if (@($Paths).Count -eq 0) {
        $failures.Add("signing evidence was not supplied")
        return $failures.ToArray()
    }
    if (@($Paths).Count -ne 1) {
        $failures.Add("signing evidence must name exactly one RG-08 product executable")
        return $failures.ToArray()
    }
    $expectedPath = [IO.Path]::GetFullPath(
        (Join-Path $buildDirectories.on "notepad-safe.exe"))
    foreach ($path in @($Paths)) {
        if ([string]::IsNullOrWhiteSpace($path)) {
            $failures.Add("signing evidence contains an empty path")
            continue
        }
        $fullPath = Resolve-GateInputPath $path
        if ([IO.Path]::GetExtension($fullPath).ToLowerInvariant() -ne ".exe") {
            $failures.Add("signing evidence must be the RG-08 notepad-safe.exe, not a text or generic evidence file: $fullPath")
            continue
        }
        if (-not $fullPath.Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
            $failures.Add("signing evidence must be the RG-08 product executable (expected $expectedPath, actual $fullPath)")
            continue
        }
        if ((Test-Path -LiteralPath $fullPath) -and
            (-not (Test-Path -LiteralPath $fullPath -PathType Leaf) -or
             (Get-Item -LiteralPath $fullPath).Length -le 0)) {
            $failures.Add("signing evidence must be a non-empty file: $fullPath")
        }
    }
    return $failures.ToArray()
}

function Invoke-Build([string]$BuildDirectory, [string]$Kdbx, [string]$Asan,
        $Stage, [string[]]$Targets) {
    $names = @("NOTEPADSAFE_BUILD_DIR", "NOTEPADSAFE_BUILD_JOBS",
        "NOTEPADSAFE_BUILD_TESTS", "NOTEPADSAFE_BUILD_DIAGNOSTICS",
        "NOTEPADSAFE_WITH_KDBX", "NOTEPADSAFE_ENABLE_ASAN")
    $saved = @{}
    foreach ($name in $names) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    }
    try {
        $env:NOTEPADSAFE_BUILD_DIR = $BuildDirectory
        $env:NOTEPADSAFE_BUILD_JOBS = [string]$Jobs
        $env:NOTEPADSAFE_BUILD_TESTS = "ON"
        $env:NOTEPADSAFE_BUILD_DIAGNOSTICS = "OFF"
        $env:NOTEPADSAFE_WITH_KDBX = $Kdbx
        $env:NOTEPADSAFE_ENABLE_ASAN = $Asan
        Invoke-GateCommand $Stage (Join-Path $repoRoot "build_incr.bat") $Targets
    }
    finally {
        foreach ($name in $names) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], "Process")
        }
    }
}

function Test-FrozenTestfiles {
    $names = @(
        "tinytextfile.txt", "tinytextfile-utf-16be.txt",
        "tinytextfile-utf-16be-utf-8-bom.txt", "tinytestfile",
        "smalltextfile.txt", "smalltextfile-utf-8-utf-16le.txt",
        "smalltextfile-utf-8.txt", "smalltestfile"
    )
    $missing = @($names | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $repoRoot "testfiles\$_") -PathType Leaf)
    })
    if ($missing.Count -ne 0) {
        throw "Frozen release testfiles matrix is incomplete: $($missing -join ', ')"
    }
}

function Write-Manifest($Manifest, $ProvenanceStage) {
    if (-not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $buildRoot)
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $ProvenanceStage.status = "RUNNING"
    $ProvenanceStage.error = $null
    $ProvenanceStage.started_utc = [DateTime]::UtcNow.ToString("o")
    $Manifest.finished_utc = [DateTime]::UtcNow.ToString("o")
    $utf8 = [Text.UTF8Encoding]::new($false)

    # The payload is immutable and can therefore be hashed from inside the
    # final manifest without the self-reference paradox of hashing a JSON file
    # that contains its own digest.
    $payloadJson = $Manifest | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($provenancePayloadPath, $payloadJson + "`n", $utf8)
    $payloadHash = Get-Sha256 $provenancePayloadPath
    [void]$ProvenanceStage.evidence.Add([ordered]@{
        kind = "provenance-payload"
        path = $provenancePayloadPath
        bytes = [int64](Get-Item -LiteralPath $provenancePayloadPath).Length
        sha256 = $payloadHash
    })
    $timer.Stop()
    $ProvenanceStage.duration_seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
    $ProvenanceStage.status = "PASSED"
    $json = $Manifest | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($manifestPath, $json + "`n", $utf8)
    $finalHash = Get-Sha256 $manifestPath
    [IO.File]::WriteAllText($manifestHashPath, "$finalHash  release-gate-manifest.json`n", $utf8)
    Write-Host "[RG-10] PASSED: $manifestPath"
    Write-Host "Manifest SHA-256: $finalHash"
}

Assert-StageContract
$projectVersion = Get-ProjectVersion
$toolchain = Get-ToolchainInfo

if ($Mode -eq "Plan") {
    Write-Host "Notepad Safe release gate plan (read-only)"
    Write-Host "Repository: $repoRoot"
    Write-Host "Product version: $projectVersion"
    Write-Host "Build root: $buildRoot"
    foreach ($definition in $stageDefinitions) {
        Write-Host "$($definition.id) | $($definition.name) | $($definition.title)"
    }
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $dirty = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=all 2>&1 |
            ForEach-Object { [string]$_ })
        $gitExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($gitExit -ne 0) {
        throw "git status failed with exit code $gitExit"
    }
    if ($dirty.Count -eq 0) {
        Write-Host "Full-mode Git preflight: clean"
    }
    else {
        Write-Host "Full-mode Git preflight: BLOCKED by dirty paths (use -AllowDirty only for an unreleasable developer run)"
        $dirty | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host "Plan completed without creating build directories or a manifest."
    exit 0
}

$stageResults = [ordered]@{}
foreach ($definition in $stageDefinitions) {
    $stageResults[[string]$definition.id] = New-StageResult $definition
}
$manifest = [ordered]@{
    schema_version = 1
    product = "Notepad Safe"
    product_version = $projectVersion
    mode = $Mode
    started_utc = [DateTime]::UtcNow.ToString("o")
    finished_utc = $null
    releasable = $false
    release_blockers = [Collections.ArrayList]::new()
    source = [ordered]@{
        repository = $repoRoot
        commit = $null
        branch = $null
        dirty = $null
        dirty_paths = @()
        allow_dirty = [bool]$AllowDirty
    }
    environment = [ordered]@{
        windows = [Environment]::OSVersion.VersionString
        powershell = $PSVersionTable.PSVersion.ToString()
        process_architecture = $env:PROCESSOR_ARCHITECTURE
        visual_studio_root = $toolchain.visual_studio_root
        msvc_version = $toolchain.msvc_version
        cmake = $toolchain.cmake
        cmake_version = $toolchain.cmake_version
        ctest = $toolchain.ctest
        ctest_version = $toolchain.ctest_version
        ninja = $toolchain.ninja
        ninja_version = $toolchain.ninja_version
        qt_root = $toolchain.qt_root
        qt_version = $toolchain.qt_version
    }
    build_directories = $buildDirectories
    stages = @($requiredStageIds | ForEach-Object { $stageResults[$_] })
}

$failed = $false
$failureMessage = $null
try {
    Invoke-Stage $stageResults["RG-01"] {
        $rootOutput = @(Invoke-GateCommandCapture $stageResults["RG-01"] git `
            @("-C", $repoRoot, "rev-parse", "--show-toplevel"))
        $actualRoot = [IO.Path]::GetFullPath($rootOutput[-1])
        if (-not $actualRoot.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Gate must run against its own Git root: $actualRoot"
        }
        $commitOutput = @(Invoke-GateCommandCapture $stageResults["RG-01"] git `
            @("-C", $repoRoot, "rev-parse", "HEAD"))
        $branchOutput = @(Invoke-GateCommandCapture $stageResults["RG-01"] git `
            @("-C", $repoRoot, "symbolic-ref", "--short", "HEAD"))
        $statusOutput = @(Invoke-GateCommandCapture $stageResults["RG-01"] git `
            @("-C", $repoRoot, "status", "--porcelain=v1", "--untracked-files=all"))
        Invoke-GateCommand $stageResults["RG-01"] git `
            @("-C", $repoRoot, "diff", "--check")
        Invoke-GateCommand $stageResults["RG-01"] git `
            @("-C", $repoRoot, "diff", "--cached", "--check")
        $manifest.source.commit = $commitOutput[-1].Trim()
        $manifest.source.branch = $branchOutput[-1].Trim()
        $manifest.source.dirty_paths = @($statusOutput)
        $manifest.source.dirty = ($statusOutput.Count -ne 0)
        if ($manifest.source.dirty -and -not $AllowDirty) {
            throw "Dirty worktree is forbidden. Re-run only after cleaning it; -AllowDirty is development-only."
        }
        if ($AllowDirty) {
            [void]$manifest.release_blockers.Add(
                "-AllowDirty was used; this development run can never be marked releasable")
        }
        foreach ($requiredPath in @(
            "build_incr.bat", "tools\verify_supply_chain.ps1",
            "tools\check_architecture.ps1", "tools\check_async_threading.ps1",
            "tools\verify_release_package.ps1")) {
            if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $requiredPath) -PathType Leaf)) {
                throw "Required release-gate input is missing: $requiredPath"
            }
        }
        if ($projectVersion -eq "unknown") {
            throw "CMake project version could not be resolved"
        }
        foreach ($isolatedPath in @("build-release-gate/on", "build-release-gate/off",
                "build-release-gate/asan")) {
            Invoke-GateCommand $stageResults["RG-01"] git @(
                "-C", $repoRoot, "check-ignore", "--quiet", $isolatedPath)
        }
        foreach ($toolPath in @($toolchain.cmake, $toolchain.ctest, $toolchain.ninja,
                $toolchain.vcvars, $toolchain.qt_root)) {
            if (-not (Test-Path -LiteralPath $toolPath)) {
                throw "Required toolchain path is missing: $toolPath"
            }
        }
    }

    # Reject structurally invalid evidence before any build. Signing outputs
    # may be created by the packaging/signing workflow under the ON build
    # directory, so Authenticode validity is deliberately checked at RG-09.
    $evidenceChecks = @(
        @{ stage = "RG-06"; blockers = @(Get-StructuredEvidenceBlockers `
            $PerformanceEvidence "performance" $manifest.source.commit) },
        @{ stage = "RG-07"; blockers = @(Get-StructuredEvidenceBlockers `
            $VisualEvidence "manual-visual" $manifest.source.commit) },
        @{ stage = "RG-09"; blockers = @(Get-SigningPathBlockers $SigningEvidence) }
    )
    $evidenceFailures = [Collections.Generic.List[string]]::new()
    foreach ($check in $evidenceChecks) {
        $blockers = @($check.blockers)
        if ($blockers.Count -ne 0) {
            $stage = $stageResults[$check.stage]
            $stage.status = "BLOCKED"
            $stage.error = $blockers -join "; "
            Write-Host "[$($stage.id)] BLOCKED: $($stage.error)" -ForegroundColor Red
            foreach ($blocker in $blockers) {
                $evidenceFailures.Add($blocker)
                [void]$manifest.release_blockers.Add($blocker)
            }
        }
    }
    if ($evidenceFailures.Count -ne 0) {
        throw "Required release evidence is missing or invalid: $($evidenceFailures -join '; ')"
    }

    Invoke-Stage $stageResults["RG-02"] {
        Invoke-GateCommand $stageResults["RG-02"] "$PSHOME\powershell.exe" @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            (Join-Path $repoRoot "tools\verify_supply_chain.ps1"))
        Invoke-GateCommand $stageResults["RG-02"] "$PSHOME\powershell.exe" @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            (Join-Path $repoRoot "tools\check_architecture.ps1"),
            "-SourceRoot", $repoRoot)
        Invoke-GateCommand $stageResults["RG-02"] "$PSHOME\powershell.exe" @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            (Join-Path $repoRoot "tools\check_async_threading.ps1"))
    }

    $allTestTargets = @("notepad-safe", "notepadsafe_non_gui_tests",
        "notepadsafe_gui_smoke_tests", "e2e_startup_verify", "e2e_l2_content_visible")
    Invoke-Stage $stageResults["RG-03"] {
        Test-FrozenTestfiles
        Invoke-Build $buildDirectories.on "ON" "OFF" $stageResults["RG-03"] $allTestTargets
        Invoke-GateCommand $stageResults["RG-03"] $toolchain.ctest @(
            "--test-dir", $buildDirectories.on, "--build-config", "Release",
            "--no-tests=error", "--output-on-failure")
    }

    Invoke-Stage $stageResults["RG-04"] {
        Test-FrozenTestfiles
        Invoke-Build $buildDirectories.off "OFF" "OFF" $stageResults["RG-04"] $allTestTargets
        Invoke-GateCommand $stageResults["RG-04"] $toolchain.ctest @(
            "--test-dir", $buildDirectories.off, "--build-config", "Release",
            "--no-tests=error", "--output-on-failure")
    }

    Invoke-Stage $stageResults["RG-05"] {
        Test-FrozenTestfiles
        Invoke-Build $buildDirectories.asan "ON" "ON" $stageResults["RG-05"] $allTestTargets
        Invoke-GateCommand $stageResults["RG-05"] $toolchain.ctest @(
            "--test-dir", $buildDirectories.asan, "--build-config", "Release",
            "--no-tests=error", "--output-on-failure", "-L", "^adjacent$")
    }

    Invoke-Stage $stageResults["RG-06"] {
        foreach ($path in $PerformanceEvidence) {
            [void](Assert-PerformanceEvidence $path $manifest.source.commit)
            Add-Evidence $stageResults["RG-06"] $path "performance"
        }
    }
    Invoke-Stage $stageResults["RG-07"] {
        foreach ($path in $VisualEvidence) {
            $visualDocument = Assert-VisualEvidence $path $manifest.source.commit
            Add-Evidence $stageResults["RG-07"] $path "manual-visual"
            foreach ($artifact in @($visualDocument.artifacts)) {
                $artifactPath = Resolve-VisualArtifactPath $path ([string]$artifact.path)
                Add-Evidence $stageResults["RG-07"] $artifactPath "manual-visual-artifact"
            }
        }
    }

    Invoke-Stage $stageResults["RG-08"] {
        Invoke-Build $buildDirectories.on "ON" "OFF" $stageResults["RG-08"] @("portable_package")
        $package = Join-Path $buildDirectories.on `
            "NotepadSafe-$projectVersion-windows-x64.zip"
        $executable = Join-Path $buildDirectories.on "notepad-safe.exe"
        Invoke-GateCommand $stageResults["RG-08"] "$PSHOME\powershell.exe" @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            (Join-Path $repoRoot "tools\verify_release_package.ps1"),
            "-Package", $package, "-ProductExecutable", $executable,
            "-LockFile", (Join-Path $repoRoot "third_party\dependencies.lock.json"),
            "-ProductVersion", $projectVersion)
        Add-Evidence $stageResults["RG-08"] $package "portable-package"
        Add-Evidence $stageResults["RG-08"] $executable "release-executable"
    }

    Invoke-Stage $stageResults["RG-09"] {
        $pathBlockers = @(Get-SigningPathBlockers $SigningEvidence)
        if ($pathBlockers.Count -ne 0) {
            throw $pathBlockers -join "; "
        }
        foreach ($path in $SigningEvidence) {
            $resolved = (Resolve-Path -LiteralPath (Resolve-GateInputPath $path)).Path
            $signature = Get-AuthenticodeSignature -LiteralPath $resolved
            if ([string]$signature.Status -ne "Valid") {
                throw "Authenticode signature is not valid for ${resolved}: $($signature.Status) $($signature.StatusMessage)"
            }
            if ($null -eq $signature.SignerCertificate) {
                throw "Authenticode signer certificate is missing: $resolved"
            }
            if ($null -eq $signature.TimeStamperCertificate) {
                throw "Authenticode timestamp certificate is missing: $resolved"
            }
            $item = Get-Item -LiteralPath $resolved
            $versionInfo = $item.VersionInfo
            foreach ($identityValue in @($versionInfo.ProductName, $versionInfo.FileDescription)) {
                if ([string]$identityValue -notmatch '(?i)Notepad\s+Safe') {
                    throw "Signed binary VersionInfo does not identify Notepad Safe: $resolved"
                }
            }
            $acceptedVersions = @($projectVersion, "$projectVersion.0")
            if ([string]$versionInfo.ProductVersion -notin $acceptedVersions -or
                [string]$versionInfo.FileVersion -notin $acceptedVersions) {
                throw "Signed binary VersionInfo must match CMake project version $projectVersion (ProductVersion=$($versionInfo.ProductVersion), FileVersion=$($versionInfo.FileVersion))"
            }
            [void]$stageResults["RG-09"].evidence.Add([ordered]@{
                kind = "authenticode-signed-binary"
                path = $resolved
                bytes = [int64]$item.Length
                sha256 = Get-Sha256 $resolved
                status = [string]$signature.Status
                product_name = $versionInfo.ProductName
                file_description = $versionInfo.FileDescription
                product_version = $versionInfo.ProductVersion
                file_version = $versionInfo.FileVersion
                signer_subject = $signature.SignerCertificate.Subject
                signer_thumbprint = $signature.SignerCertificate.Thumbprint
                signer_not_before = $signature.SignerCertificate.NotBefore.ToUniversalTime().ToString("o")
                signer_not_after = $signature.SignerCertificate.NotAfter.ToUniversalTime().ToString("o")
                timestamp_subject = $signature.TimeStamperCertificate.Subject
                timestamp_thumbprint = $signature.TimeStamperCertificate.Thumbprint
                timestamp_not_before = $signature.TimeStamperCertificate.NotBefore.ToUniversalTime().ToString("o")
                timestamp_not_after = $signature.TimeStamperCertificate.NotAfter.ToUniversalTime().ToString("o")
            })
        }
    }

    if ($AllowDirty) {
        throw "All requested checks completed, but -AllowDirty makes the result unreleasable."
    }
    $manifest.releasable = $true
}
catch {
    $failed = $true
    $failureMessage = $_.Exception.Message
    if (-not $manifest.release_blockers.Contains($failureMessage)) {
        [void]$manifest.release_blockers.Add($failureMessage)
    }
    foreach ($id in $requiredStageIds) {
        $stage = $stageResults[$id]
        if ($stage.status -eq "PENDING") {
            $stage.status = "BLOCKED"
            $stage.error = "Blocked by an earlier release-gate failure"
        }
    }
}
finally {
    Write-Manifest $manifest $stageResults["RG-10"]
}

if ($failed -or -not $manifest.releasable) {
    Write-Error "Release gate failed: $failureMessage"
    exit 1
}

Write-Host "Release gate PASSED: releasable=true"
exit 0
