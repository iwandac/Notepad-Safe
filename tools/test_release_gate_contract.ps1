[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$sourceGate = Join-Path $PSScriptRoot "run_release_gate.ps1"
$expectedStages = @("RG-01", "RG-02", "RG-03", "RG-04", "RG-05",
    "RG-06", "RG-07", "RG-08", "RG-09", "RG-10")
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("notepadsafe-release-gate-contract-" + [Guid]::NewGuid().ToString("N"))

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { [string]$_ })
        $exitCode = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($exitCode -ne 0) {
        throw "Contract setup command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$($output -join "`n")"
    }
    return $output
}

try {
    $toolsRoot = Join-Path $tempRoot "tools"
    [void](New-Item -ItemType Directory -Path $toolsRoot)
    Copy-Item -LiteralPath $sourceGate -Destination (Join-Path $toolsRoot "run_release_gate.ps1")
    Set-Content -LiteralPath (Join-Path $tempRoot "CMakeLists.txt") -Encoding UTF8 `
        -Value "project(notepad-safe VERSION 0.0.0)`n"
    Set-Content -LiteralPath (Join-Path $tempRoot ".gitignore") -Encoding UTF8 `
        -Value "/build-*/`n"
    Set-Content -LiteralPath (Join-Path $tempRoot "build_incr.bat") -Encoding ASCII `
        -Value "@exit /b 99`r`n"
    foreach ($toolName in @("verify_supply_chain.ps1", "check_architecture.ps1",
            "check_async_threading.ps1", "verify_release_package.ps1")) {
        Set-Content -LiteralPath (Join-Path $toolsRoot $toolName) -Encoding UTF8 `
            -Value "throw 'contract fixture must not execute this tool'`n"
    }

    Invoke-Checked git @("-C", $tempRoot, "init", "--quiet") | Out-Null
    Invoke-Checked git @("-C", $tempRoot, "add", ".") | Out-Null
    Invoke-Checked git @("-C", $tempRoot, "-c", "user.name=Release Gate Contract",
        "-c", "user.email=release-gate-contract@invalid", "commit", "--quiet",
        "-m", "contract fixture") | Out-Null

    $gate = Join-Path $toolsRoot "run_release_gate.ps1"
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $planOutput = @(& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass `
            -File $gate -Mode Plan 2>&1 | ForEach-Object { [string]$_ })
        $planExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($planExit -ne 0) {
        throw "Plan contract failed with exit code $planExit`n$($planOutput -join "`n")"
    }
    foreach ($id in $expectedStages) {
        $count = @($planOutput | Where-Object { $_ -match "^$id \|" }).Count
        if ($count -ne 1) {
            throw "Plan must print required stage $id exactly once (actual: $count)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $tempRoot "build-release-gate")) {
        throw "Plan mode created build-release-gate; Plan must be read-only"
    }

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $fullOutput = @(& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass `
            -File $gate -Mode Full 2>&1 | ForEach-Object { [string]$_ })
        $fullExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($fullExit -eq 0) {
        throw "Full mode passed without performance, visual, and signing evidence"
    }
    foreach ($blockedStage in @("RG-06", "RG-07", "RG-09")) {
        if (-not ($fullOutput -match "\[$blockedStage\] BLOCKED:")) {
            throw "Missing evidence did not explicitly block $blockedStage`n$($fullOutput -join "`n")"
        }
    }
    $manifest = Join-Path $tempRoot "build-release-gate\release-gate-manifest.json"
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "Failed Full run did not produce a provenance manifest`n$($fullOutput -join "`n")"
    }
    $manifestObject = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    if ($manifestObject.releasable -ne $false) {
        throw "Missing-evidence manifest was incorrectly marked releasable"
    }
    $manifestIds = @($manifestObject.stages | ForEach-Object { [string]$_.id })
    if (($manifestIds -join "|") -ne ($expectedStages -join "|")) {
        throw "Manifest omitted or reordered a required release-gate stage"
    }
    foreach ($blockedStage in @("RG-06", "RG-07", "RG-09")) {
        $result = @($manifestObject.stages | Where-Object id -eq $blockedStage)
        if ($result.Count -ne 1 -or $result[0].status -ne "BLOCKED") {
            throw "Manifest did not preserve the explicit $blockedStage evidence blocker"
        }
    }
    $provenance = @($manifestObject.stages | Where-Object id -eq "RG-10")
    if ($provenance.Count -ne 1 -or $provenance[0].status -ne "PASSED" -or
        @($provenance[0].evidence).Count -ne 1) {
        throw "Failed Full run did not complete the hashed provenance stage"
    }

    # A JSON-shaped file and a non-empty text file are not release evidence.
    # Keep them below the ignored gate root so the Git preflight stays clean;
    # the run must reject them before creating any build configuration.
    $invalidRoot = Join-Path $tempRoot "build-release-gate\contract-invalid-evidence"
    [void](New-Item -ItemType Directory -Path $invalidRoot -Force)
    $invalidPerformance = Join-Path $invalidRoot "performance.json"
    $invalidVisual = Join-Path $invalidRoot "visual.json"
    $invalidVisualArtifact = Join-Path $invalidRoot "visual.png"
    $textSignature = Join-Path $invalidRoot "signature.txt"
    $headOutput = @(Invoke-Checked git @("-C", $tempRoot, "rev-parse", "HEAD"))
    $fixtureCommit = $headOutput[-1].Trim()
    [ordered]@{
        schema_version = 1
        commit = $fixtureCommit
        passed = $true
        sample_count = 1
        results = [ordered]@{}
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $invalidPerformance -Encoding UTF8
    Set-Content -LiteralPath $invalidVisualArtifact -Encoding UTF8 -Value "fake pixels"
    [ordered]@{
        schema_version = 1
        commit = $fixtureCommit
        passed = $true
        reviewer = "fake"
        matrix_id = "fake"
        artifacts = @([ordered]@{
            path = "visual.png"
            sha256 = ("0" * 64)
        })
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $invalidVisual -Encoding UTF8
    Set-Content -LiteralPath $textSignature -Encoding UTF8 -Value "not a signed binary"

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $invalidOutput = @(& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass `
            -File $gate -Mode Full `
            -PerformanceEvidence $invalidPerformance `
            -VisualEvidence $invalidVisual `
            -SigningEvidence $textSignature 2>&1 | ForEach-Object { [string]$_ })
        $invalidExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($invalidExit -eq 0) {
        throw "Full mode accepted fake JSON and a text file as release evidence"
    }
    if (-not ($invalidOutput -match "performance sample_count must be at least 30")) {
        throw "Fake performance JSON was not rejected by the sample/schema contract"
    }
    if (-not ($invalidOutput -match "manual-visual artifact hash mismatch")) {
        throw "Fake visual JSON was not rejected by the artifact hash contract"
    }
    if (-not ($invalidOutput -match "signing evidence must be the RG-08 notepad-safe.exe")) {
        throw "Plain text was not rejected as signing evidence"
    }
    foreach ($buildName in @("on", "off", "asan")) {
        if (Test-Path -LiteralPath (Join-Path $tempRoot "build-release-gate\$buildName")) {
            throw "Invalid evidence reached the expensive $buildName build"
        }
    }

    # Even a real, potentially valid system-signed executable is the wrong
    # product identity. RG-09 accepts only the exact RG-08 Notepad Safe output.
    $systemExecutable = Join-Path $PSHOME "powershell.exe"
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $externalSigningOutput = @(& "$PSHOME\powershell.exe" `
            -NoProfile -ExecutionPolicy Bypass -File $gate -Mode Full `
            -PerformanceEvidence $invalidPerformance `
            -VisualEvidence $invalidVisual `
            -SigningEvidence $systemExecutable 2>&1 |
            ForEach-Object { [string]$_ })
        $externalSigningExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($externalSigningExit -eq 0 -or
        -not ($externalSigningOutput -match "signing evidence must be the RG-08 product executable")) {
        throw "An external system executable was not rejected as the signing identity"
    }

    Write-Host "Release-gate contract: PASS (fixed stages, structured evidence, exact-product Authenticode path, read-only Plan)"
}
finally {
    if (Test-Path -LiteralPath $tempRoot -PathType Container) {
        $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
        $systemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if ($resolvedTemp.StartsWith($systemTemp, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedTemp).StartsWith("notepadsafe-release-gate-contract-")) {
            Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
        }
    }
}
