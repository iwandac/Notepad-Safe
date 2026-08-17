[CmdletBinding()]
param(
    [string]$LockFile = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($LockFile)) {
    $LockFile = Join-Path $repoRoot "third_party/dependencies.lock.json"
}

function Fail([string]$Message) {
    throw "Supply-chain verification failed: $Message"
}

function To-Hex([byte[]]$Bytes) {
    return ([BitConverter]::ToString($Bytes)).Replace("-", "").ToLowerInvariant()
}

function Test-PinnedValue([string]$Value, [string]$Field, [string]$Component) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        Fail "$Component.$Field is empty"
    }
    if ($Value -match '(?i)^(latest|head|main|master|tbd|todo|unknown)$') {
        Fail "$Component.$Field is not pinned: $Value"
    }
}

if (-not (Test-Path -LiteralPath $LockFile -PathType Leaf)) {
    Fail "lock file not found: $LockFile"
}

$lock = Get-Content -LiteralPath $LockFile -Raw -Encoding UTF8 | ConvertFrom-Json
if ($lock.schema_version -ne 1) {
    Fail "unsupported schema_version '$($lock.schema_version)'"
}
if (-not $lock.policy.require_primary_source -or
    -not $lock.policy.require_integrity_verification) {
    Fail "lock policy must require primary sources and integrity verification"
}

$components = @($lock.components)
if ($components.Count -eq 0) {
    Fail "components is empty"
}
$duplicates = $components | Group-Object name | Where-Object Count -gt 1
if ($duplicates) {
    Fail "duplicate component name(s): $(($duplicates.Name) -join ', ')"
}

Push-Location $repoRoot
try {
    foreach ($component in $components) {
        $name = [string]$component.name
        Test-PinnedValue $name "name" $name
        Test-PinnedValue ([string]$component.version) "version" $name
        Test-PinnedValue ([string]$component.revision) "revision" $name
        Test-PinnedValue ([string]$component.source) "source" $name
        Test-PinnedValue ([string]$component.license) "license" $name
        Test-PinnedValue ([string]$component.integrity.kind) "integrity.kind" $name
        Test-PinnedValue ([string]$component.integrity.value) "integrity.value" $name

        if ($component.status -notin @("active", "planned")) {
            Fail "$name.status must be active or planned"
        }
        if ([string]$component.integrity.kind -eq "sha256" -and
            [string]$component.integrity.value -notmatch '^[0-9a-f]{64}$') {
            Fail "$name SHA-256 must contain exactly 64 lowercase hex digits"
        }
        if ([string]$component.integrity.kind -eq "pgp-signed-tag" -and
            [string]$component.integrity.signer_fingerprint -notmatch '^[0-9A-F]{40}$') {
            Fail "$name signer fingerprint must contain exactly 40 uppercase hex digits"
        }
        if ([string]$component.integrity.kind -eq "pgp-signed-tag" -and
            [string]$component.resolved_commit -notmatch '^[0-9a-f]{40}$') {
            Fail "$name resolved_commit must pin the signed tag to exactly 40 lowercase hex digits"
        }

        if ([string]::IsNullOrWhiteSpace([string]$component.local_path)) {
            continue
        }
        if ($component.status -ne "active") {
            Fail "$name has local_path but is not active"
        }
        if ([string]$component.integrity.kind -ne "git-index-manifest-sha256") {
            Fail "$name local_path requires git-index-manifest-sha256 integrity"
        }

        $localPath = ([string]$component.local_path).Replace("\", "/").TrimEnd("/")
        if (-not (Test-Path -LiteralPath $localPath -PathType Container)) {
            Fail "$name vendored directory not found: $localPath"
        }

        $excluded = @($component.integrity.exclude_directories)
        $indexLines = @(& git ls-files -s -- "$localPath/*")
        if ($LASTEXITCODE -ne 0) {
            Fail "git ls-files failed for $name"
        }
        $indexLines = @($indexLines | Where-Object {
            $line = $_
            -not ($excluded | Where-Object { $line -match "/$([regex]::Escape([string]$_))/" })
        })
        $indexLines = @($indexLines | Sort-Object { ($_ -split "`t", 2)[1] })
        if ($indexLines.Count -eq 0) {
            Fail "$name vendored manifest is empty"
        }

        $manifest = [System.Text.StringBuilder]::new()
        foreach ($line in $indexLines) {
            $parts = $line -split "`t", 2
            if ($parts.Count -ne 2) {
                Fail "$name has an unexpected git index record: $line"
            }
            $metadata = $parts[0] -split ' '
            [void]$manifest.Append($parts[1])
            [void]$manifest.Append([char]0)
            [void]$manifest.Append($metadata[1].ToLowerInvariant())
            [void]$manifest.Append("`n")
        }

        $utf8 = [System.Text.UTF8Encoding]::new($false)
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        $actual = To-Hex $sha256.ComputeHash($utf8.GetBytes($manifest.ToString()))
        if ($actual -ne [string]$component.integrity.value) {
            Fail "$name manifest digest mismatch (expected $($component.integrity.value), actual $actual)"
        }
        if ($component.integrity.tracked_files -and
            [int]$component.integrity.tracked_files -ne $indexLines.Count) {
            Fail "$name tracked file count mismatch"
        }

        $diffArgs = @("diff", "--quiet", "--", $localPath)
        foreach ($directory in $excluded) {
            $diffArgs += ":(exclude)$localPath/$directory/**"
        }
        & git @diffArgs
        if ($LASTEXITCODE -ne 0) {
            Fail "$name vendored source has uncommitted modifications"
        }

        $untracked = @(& git ls-files --others --exclude-standard -- "$localPath/*")
        $untracked = @($untracked | Where-Object {
            $path = $_
            -not ($excluded | Where-Object { $path -match "/$([regex]::Escape([string]$_))/" })
        })
        if ($untracked.Count -gt 0) {
            Fail "$name vendored source contains untracked files"
        }
    }
}
finally {
    Pop-Location
}

Write-Host "Supply-chain lock verified: $($components.Count) components; active vendored sources are clean."
