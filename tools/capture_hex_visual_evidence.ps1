[CmdletBinding()]
param(
    [ValidateSet("Capture", "Approve")]
    [string]$Mode = "Capture",

    [string]$BuildDirectory = "build-round1-hex-a11y",

    [string]$OutputDirectory = "",

    [string]$Manifest = "",

    [string]$Reviewer = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Get-TextSha256([string]$Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Invoke-GitLines([string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& git -C $repoRoot @Arguments 2>&1 | ForEach-Object { [string]$_ })
        $exit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $saved
    }
    if ($exit -ne 0) {
        throw "git $($Arguments -join ' ') failed: $($output -join [Environment]::NewLine)"
    }
    return $output
}

function Get-SourceIdentity {
    $head = (Invoke-GitLines @("rev-parse", "HEAD") | Select-Object -Last 1).Trim()
    $status = @(Invoke-GitLines @("status", "--porcelain=v1", "--untracked-files=all"))
    $trackedDiff = @(Invoke-GitLines @("diff", "--binary", "--no-ext-diff", "HEAD", "--")) -join "`n"
    $untrackedPaths = @(Invoke-GitLines @("ls-files", "--others", "--exclude-standard") |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object)
    $untracked = @()
    foreach ($relative in $untrackedPaths) {
        $full = Join-Path $repoRoot $relative
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $untracked += [ordered]@{
                path = ($relative -replace '\\', '/')
                sha256 = Get-Sha256 $full
            }
        }
    }
    $identityPayload = [ordered]@{
        head = $head
        status = @($status)
        tracked_diff_sha256 = Get-TextSha256 $trackedDiff
        untracked = @($untracked)
    }
    return [ordered]@{
        commit = $head
        dirty = ($status.Count -ne 0)
        status = @($status)
        tracked_diff_sha256 = $identityPayload.tracked_diff_sha256
        untracked = @($untracked)
        dirty_identity_sha256 = Get-TextSha256 ($identityPayload | ConvertTo-Json -Depth 8 -Compress)
    }
}

function Assert-CaptureMatrix($Captures) {
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($scale in @("100", "125", "200")) {
        foreach ($width in @(900, 1400, 1800)) {
            foreach ($theme in @("LIGHT", "DARK")) {
                foreach ($dock in @(0, 1)) {
                    [void]$expected.Add("HX-1D-B1-VIS-DPR$scale-W$width-$theme-DOCK$dock")
                }
            }
        }
    }
    $actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($capture in @($Captures)) {
        if (-not $actual.Add([string]$capture.case_id)) {
            throw "Duplicate visual case ID: $($capture.case_id)"
        }
        if ([string]$capture.fixture_tier -ne "B1") {
            throw "Visual capture is not a B1 fixture: $($capture.case_id)"
        }
        if ([int]$capture.visible_reference_row -ne 1000) {
            throw "Visual capture does not identify row 1000: $($capture.case_id)"
        }
        if ([double]$capture.selection_end -le [double]$capture.selection_start) {
            throw "Visual capture has no byte selection: $($capture.case_id)"
        }
    }
    if ($actual.Count -ne 36 -or -not $actual.SetEquals($expected)) {
        $missing = @($expected | Where-Object { -not $actual.Contains($_) })
        $extra = @($actual | Where-Object { -not $expected.Contains($_) })
        throw "Visual matrix mismatch: actual=$($actual.Count), missing=$($missing -join ','), extra=$($extra -join ',')"
    }
}

function New-ContactSheet([object[]]$Captures, [string]$Directory, [string]$ScaleLabel) {
    Add-Type -AssemblyName System.Drawing
    $cellWidth = 420
    $cellHeight = 270
    $labelHeight = 34
    $columns = 3
    $rows = 4
    $sheet = [Drawing.Bitmap]::new($columns * $cellWidth, $rows * ($cellHeight + $labelHeight))
    $graphics = [Drawing.Graphics]::FromImage($sheet)
    $font = [Drawing.Font]::new("Segoe UI", 10)
    $brush = [Drawing.Brushes]::White
    try {
        $graphics.Clear([Drawing.Color]::FromArgb(32, 32, 32))
        $ordered = @($Captures | Sort-Object { [int]$_.logical_window_size.width }, theme, dock_visible)
        for ($index = 0; $index -lt $ordered.Count; ++$index) {
            $capture = $ordered[$index]
            $column = $index % $columns
            $row = [Math]::Floor($index / $columns)
            $x = $column * $cellWidth
            $y = $row * ($cellHeight + $labelHeight)
            $sourcePath = Join-Path $Directory ([string]$capture.path)
            $image = [Drawing.Image]::FromFile($sourcePath)
            try {
                $graphics.DrawImage($image, $x, $y, $cellWidth, $cellHeight)
            }
            finally {
                $image.Dispose()
            }
            $graphics.DrawString([string]$capture.case_id, $font, $brush,
                                 [single]($x + 4), [single]($y + $cellHeight + 5))
        }
        $path = Join-Path $Directory "contact-sheet-dpr-$ScaleLabel.png"
        $sheet.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        return $path
    }
    finally {
        $font.Dispose()
        $graphics.Dispose()
        $sheet.Dispose()
    }
}

function Assert-ArtifactHashes($Document, [string]$ManifestPath) {
    $directory = Split-Path -Parent $ManifestPath
    foreach ($artifact in @($Document.artifacts)) {
        $path = if ([IO.Path]::IsPathRooted([string]$artifact.path)) {
            [IO.Path]::GetFullPath([string]$artifact.path)
        } else {
            [IO.Path]::GetFullPath((Join-Path $directory ([string]$artifact.path)))
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Visual artifact is missing: $path"
        }
        if ((Get-Sha256 $path) -ne ([string]$artifact.sha256).ToLowerInvariant()) {
            throw "Visual artifact hash mismatch: $path"
        }
    }
}

if ($Mode -eq "Approve") {
    if ([string]::IsNullOrWhiteSpace($Manifest) -or
        [string]::IsNullOrWhiteSpace($Reviewer)) {
        throw "Approve requires -Manifest and a non-empty -Reviewer after manual review"
    }
    $manifestPath = Resolve-RepoPath $Manifest
    $document = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document.passed -eq $true) {
        throw "Manifest is already approved"
    }
    Assert-CaptureMatrix @($document.captures)
    Assert-ArtifactHashes $document $manifestPath
    $identity = Get-SourceIdentity
    if ([string]$document.commit -ne [string]$identity.commit -or
        [string]$document.source_identity.dirty_identity_sha256 -ne
            [string]$identity.dirty_identity_sha256) {
        throw "Workspace identity changed after capture; recapture before approval"
    }
    $document.passed = $true
    $document.reviewer = $Reviewer
    $document.manual_review.status = "approved"
    $document.manual_review.reviewed_at_utc = [DateTime]::UtcNow.ToString("o")
    $approvedPath = Join-Path (Split-Path -Parent $manifestPath) "hex-visual-evidence-approved.json"
    $document | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $approvedPath -Encoding UTF8
    Write-Host "Approved manual visual evidence: $approvedPath"
    exit 0
}

$buildRoot = Resolve-RepoPath $BuildDirectory
$executable = Join-Path $buildRoot "hex_visual_evidence.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build the visual harness first: $executable"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $buildRoot ("hex-visual-evidence-" +
        [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ"))
}
$outputRoot = Resolve-RepoPath $OutputDirectory
if (Test-Path -LiteralPath $outputRoot) {
    if (@(Get-ChildItem -LiteralPath $outputRoot -Force).Count -ne 0) {
        throw "Capture output directory must be new or empty: $outputRoot"
    }
} else {
    [void](New-Item -ItemType Directory -Path $outputRoot)
}

$identityBefore = Get-SourceIdentity
$runs = @(
    @{ label = "100"; scale = "1.0"; dpr = "1.0" },
    @{ label = "125"; scale = "1.25"; dpr = "1.25" },
    @{ label = "200"; scale = "2.0"; dpr = "2.0" }
)
$oldQpa = $env:QT_QPA_PLATFORM
$oldScale = $env:QT_SCALE_FACTOR
try {
    $env:QT_QPA_PLATFORM = "windows:dpiawareness=0"
    foreach ($run in $runs) {
        $env:QT_SCALE_FACTOR = $run.scale
        & $executable --output $outputRoot --scale-label $run.label --expected-dpr $run.dpr
        if ($LASTEXITCODE -ne 0) {
            throw "Visual harness failed for DPR $($run.dpr) with exit code $LASTEXITCODE"
        }
    }
}
finally {
    $env:QT_QPA_PLATFORM = $oldQpa
    $env:QT_SCALE_FACTOR = $oldScale
}

$identityAfter = Get-SourceIdentity
if ($identityBefore.dirty_identity_sha256 -ne $identityAfter.dirty_identity_sha256) {
    throw "Workspace changed while capturing visual evidence; discard this capture and retry"
}

$captures = @()
$runMetadata = @()
foreach ($run in $runs) {
    $path = Join-Path $outputRoot "capture-dpr-$($run.label).json"
    $metadata = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $runMetadata += $metadata
    $captures += @($metadata.captures)
}
Assert-CaptureMatrix $captures

$artifacts = @()
foreach ($capture in $captures) {
    $path = Join-Path $outputRoot ([string]$capture.path)
    $artifacts += [ordered]@{
        path = [string]$capture.path
        sha256 = Get-Sha256 $path
        case_id = [string]$capture.case_id
        artifact_kind = "full-product-window"
    }
}
foreach ($run in $runs) {
    $scaleCaptures = @($captures | Where-Object {
        [string]$_.qt_scale_factor -eq [string]$run.scale
    })
    $sheetPath = New-ContactSheet $scaleCaptures $outputRoot $run.label
    $artifacts += [ordered]@{
        path = [IO.Path]::GetFileName($sheetPath)
        sha256 = Get-Sha256 $sheetPath
        case_id = "HX-1D-B1-VIS-CONTACT-DPR$($run.label)"
        artifact_kind = "contact-sheet"
    }
}

$manifestDocument = [ordered]@{
    schema_version = 1
    commit = [string]$identityAfter.commit
    passed = $false
    reviewer = "PENDING_ROOT_MANUAL_REVIEW"
    matrix_id = "HX-1D-B1-VISUAL-MATRIX-v1"
    generated_at_utc = [DateTime]::UtcNow.ToString("o")
    scope = [ordered]@{
        fixture_tier = "B1"
        b2_covered = $false
        b3_covered = $false
        limitation = "Only B1 is captured; B2 and B3 are outside this evidence package."
    }
    source_identity = $identityAfter
    harness = [ordered]@{
        path = $executable
        sha256 = Get-Sha256 $executable
        product_libraries = @("notepadsafe_app", "notepadsafe_editor_runtime")
    }
    manual_review = [ordered]@{
        status = "pending"
        instruction = "Inspect all 36 originals; contact sheets are navigation aids only. Run Approve explicitly after review."
        reviewed_at_utc = $null
    }
    matrix = [ordered]@{
        expected_capture_count = 36
        dpr = @(1.0, 1.25, 2.0)
        logical_width = @(900, 1400, 1800)
        theme = @("light", "dark")
        dock_visible = @($false, $true)
    }
    captures = @($captures)
    artifacts = @($artifacts)
}
$manifestPath = Join-Path $outputRoot "hex-visual-evidence-capture.json"
$manifestDocument | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Captured $($captures.Count) pending-review B1 visual cases: $manifestPath"
Write-Host "dirty=$($identityAfter.dirty) dirty_identity_sha256=$($identityAfter.dirty_identity_sha256)"
Write-Host "Automatic capture deliberately leaves passed=false."
