[CmdletBinding()]
param(
    [string]$LockFile = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "SourceTreeIntegrity.ps1")

if ([string]::IsNullOrWhiteSpace($LockFile)) {
    $LockFile = Join-Path $repoRoot "third_party/dependencies.lock.json"
}

function Fail([string]$Message) {
    throw "Supply-chain verification failed: $Message"
}

function Test-PinnedValue([string]$Value, [string]$Field, [string]$Component) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        Fail "$Component.$Field is empty"
    }
    if ($Value -match '(?i)^(latest|head|main|master|tbd|todo|unknown)$') {
        Fail "$Component.$Field is not pinned: $Value"
    }
}

function Test-UpstreamIntegrity([object]$Integrity, [string]$Name) {
    Test-PinnedValue ([string]$Integrity.kind) "source_integrity.kind" $Name
    Test-PinnedValue ([string]$Integrity.value) "source_integrity.value" $Name
    $kind = [string]$Integrity.kind
    if ($kind -eq "sha256" -and [string]$Integrity.value -notmatch '^[0-9a-f]{64}$') {
        Fail "$Name upstream SHA-256 must contain exactly 64 lowercase hex digits"
    }
    if ($kind -eq "git-commit" -and [string]$Integrity.value -notmatch '^[0-9a-f]{40}$') {
        Fail "$Name upstream Git commit must contain exactly 40 lowercase hex digits"
    }
    if ($kind -eq "pgp-signed-tag" -and
        [string]$Integrity.signer_fingerprint -notmatch '^[0-9A-F]{40}$') {
        Fail "$Name signer fingerprint must contain exactly 40 uppercase hex digits"
    }
}

if (-not (Test-Path -LiteralPath $LockFile -PathType Leaf)) {
    Fail "lock file not found: $LockFile"
}

$lock = Get-Content -LiteralPath $LockFile -Raw -Encoding UTF8 | ConvertFrom-Json
if ($lock.schema_version -ne 2) {
    Fail "unsupported schema_version '$($lock.schema_version)'"
}
if (-not $lock.policy.require_primary_source -or
    -not $lock.policy.require_integrity_verification -or
    -not $lock.policy.production_builds_are_offline) {
    Fail "lock policy must require primary sources, integrity verification, and offline production builds"
}

$components = @($lock.components)
if ($components.Count -eq 0) {
    Fail "components is empty"
}
$duplicates = $components | Group-Object name | Where-Object Count -gt 1
if ($duplicates) {
    Fail "duplicate component name(s): $(($duplicates.Name) -join ', ')"
}

$vendoredCount = 0
foreach ($component in $components) {
    $name = [string]$component.name
    Test-PinnedValue $name "name" $name
    Test-PinnedValue ([string]$component.version) "version" $name
    Test-PinnedValue ([string]$component.revision) "revision" $name
    Test-PinnedValue ([string]$component.source) "source" $name
    Test-PinnedValue ([string]$component.license) "license" $name

    if ($component.status -notin @("active", "toolchain")) {
        Fail "$name.status must be active or toolchain"
    }

    $hasLocalPath = $component.PSObject.Properties.Name -contains "local_path"
    if (-not $hasLocalPath -or [string]::IsNullOrWhiteSpace([string]$component.local_path)) {
        if ($component.status -ne "toolchain") {
            Fail "$name is an active runtime dependency but has no vendored local_path"
        }
        Test-UpstreamIntegrity $component.integrity $name
        continue
    }

    if ($component.status -ne "active") {
        Fail "$name has local_path but is not active"
    }
    Test-UpstreamIntegrity $component.source_integrity $name
    if ([string]$component.source_integrity.kind -eq "git-commit" -and
        [string]$component.source_integrity.value -ne [string]$component.revision) {
        Fail "$name revision must equal its locked Git commit"
    }
    if ([string]$component.source_integrity.kind -eq "pgp-signed-tag") {
        if ([string]$component.source_integrity.value -ne [string]$component.revision) {
            Fail "$name revision must equal its signed tag"
        }
        if ([string]$component.resolved_commit -notmatch '^[0-9a-f]{40}$') {
            Fail "$name signed tag must resolve to an exact 40-digit commit"
        }
    }
    if ([string]$component.integrity.kind -ne "source-tree-sha256") {
        Fail "$name local_path requires source-tree-sha256 integrity"
    }
    if ([string]$component.integrity.value -notmatch '^[0-9a-f]{64}$') {
        Fail "$name source-tree SHA-256 must contain exactly 64 lowercase hex digits"
    }

    $relative = ([string]$component.local_path).Replace('/', [IO.Path]::DirectorySeparatorChar)
    $localPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $relative))
    $vendoredRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "third_party/src"))
    if (-not $localPath.StartsWith($vendoredRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        Fail "$name local_path escapes third_party/src: $($component.local_path)"
    }

    $forbidden = @(Get-ChildItem -LiteralPath $localPath -File -Recurse -Force | Where-Object {
        $_.Name -in @(".qmake.stash", "CMakeCache.txt", "Makefile.Debug", "Makefile.Release") -or
        $_.Extension -in @(".obj", ".lib", ".dll", ".exe", ".pdb", ".ilk", ".pch")
    })
    $nestedMetadata = @(Get-ChildItem -LiteralPath $localPath -Directory -Recurse -Force | Where-Object {
        $_.Name -in @(".git", ".svn", "CMakeFiles") -or
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    })
    if ($forbidden.Count -gt 0 -or $nestedMetadata.Count -gt 0) {
        $first = @($forbidden.FullName) + @($nestedMetadata.FullName)
        Fail "$name contains generated artifacts or nested VCS metadata: $($first[0])"
    }

    $actual = Get-SourceTreeIntegrity -Path $localPath
    if ($actual.Sha256 -ne [string]$component.integrity.value) {
        Fail "$name source-tree digest mismatch (expected $($component.integrity.value), actual $($actual.Sha256))"
    }
    if ([int]$component.integrity.files -ne $actual.Files) {
        Fail "$name source-tree file count mismatch (expected $($component.integrity.files), actual $($actual.Files))"
    }
    if ($component.PSObject.Properties.Name -contains "patch_set") {
        $patchRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot ([string]$component.patch_set)))
        if (-not (Test-Path -LiteralPath $patchRoot -PathType Container) -or
            @(Get-ChildItem -LiteralPath $patchRoot -File -Filter "*.patch").Count -eq 0) {
            Fail "$name patch_set is missing or contains no patch: $patchRoot"
        }
    }
    $vendoredCount++
}

Write-Host "Supply-chain verified: $($components.Count) pinned components; $vendoredCount vendored source trees match real on-disk contents."
