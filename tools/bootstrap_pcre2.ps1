[CmdletBinding()]
param(
    [switch]$ForceRefresh
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repoRoot "third_party/dependencies.lock.json"
$sourceRoot = Join-Path $repoRoot ".deps/src"
$keyRoot = Join-Path $repoRoot ".deps/gpg/pcre2"
$downloadRoot = Join-Path $repoRoot ".deps/downloads"

function Fail([string]$Message) {
    throw "PCRE2 bootstrap failed: $Message"
}

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Program @Arguments
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($exitCode -ne 0) {
        Fail "command failed ($exitCode): $Program $($Arguments -join ' ')"
    }
}

if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
    Fail "lock file not found: $lockPath"
}
$lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$matches = @($lock.components | Where-Object name -eq "PCRE2")
if ($matches.Count -ne 1) {
    Fail "expected exactly one PCRE2 lock entry"
}
$component = $matches[0]
$tag = [string]$component.revision
$commit = [string]$component.resolved_commit
$fingerprint = ([string]$component.integrity.signer_fingerprint).ToUpperInvariant()
if ($component.status -ne "active" -or
    $component.integrity.kind -ne "pgp-signed-tag" -or
    $commit -notmatch '^[0-9a-f]{40}$' -or
    $fingerprint -notmatch '^[0-9A-F]{40}$') {
    Fail "lock entry is not an active, exact signed-tag pin"
}

$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    Fail "Git for Windows was not found"
}
$gpg = Get-Command gpg -ErrorAction SilentlyContinue
if (-not $gpg) {
    $gitRoot = Split-Path -Parent (Split-Path -Parent $git.Source)
    $gitGpg = Join-Path $gitRoot "usr/bin/gpg.exe"
    if (Test-Path -LiteralPath $gitGpg -PathType Leaf) {
        $gpg = Get-Item -LiteralPath $gitGpg
    }
}
if (-not $gpg) {
    Fail "GnuPG was not found (Git for Windows normally provides usr/bin/gpg.exe)"
}

[void](New-Item -ItemType Directory -Path $sourceRoot -Force)
[void](New-Item -ItemType Directory -Path $keyRoot -Force)
[void](New-Item -ItemType Directory -Path $downloadRoot -Force)
$destination = Join-Path $sourceRoot "pcre2-$($component.version)"
if ($ForceRefresh -and (Test-Path -LiteralPath $destination)) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}
if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
    Invoke-Native $git.Source @(
        "clone", "--filter=blob:none", "--branch", $tag, "--single-branch",
        [string]$component.source, $destination
    )
}

$actualCommit = (& $git.Source -C $destination rev-parse "$tag^{commit}").Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $commit) {
    Fail "tag resolves to '$actualCommit', expected '$commit'; use -ForceRefresh"
}

$oldGnuPgHome = $env:GNUPGHOME
try {
    $env:GNUPGHOME = $keyRoot
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $keyListing = @(& $gpg.FullName --batch --with-colons --fingerprint $fingerprint 2>$null)
    $ErrorActionPreference = $savedErrorAction
    if (-not ($keyListing -match "^fpr:::::::::${fingerprint}:")) {
        Write-Host "Fetching the pinned PCRE2 release-signing key..."
        $keyFile = Join-Path $downloadRoot "pcre2-$fingerprint.asc"
        Invoke-WebRequest -UseBasicParsing `
            -Uri "https://keys.openpgp.org/vks/v1/by-fingerprint/$fingerprint" `
            -OutFile $keyFile
        $ErrorActionPreference = "Continue"
        & $gpg.FullName --batch --import $keyFile
        $keyListing = @(& $gpg.FullName --batch --with-colons --fingerprint $fingerprint 2>$null)
        $ErrorActionPreference = $savedErrorAction
        if (-not ($keyListing -match "^fpr:::::::::${fingerprint}:")) {
            Fail "downloaded key does not contain the locked fingerprint $fingerprint"
        }
    }
    $ErrorActionPreference = "Continue"
    $verifyOutput = @(& $git.Source -c "gpg.program=$($gpg.FullName)" -C $destination `
        verify-tag --raw $tag 2>&1)
    $verifyExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($verifyExitCode -ne 0) {
        Fail "signed tag verification failed: $($verifyOutput -join [Environment]::NewLine)"
    }
    $validSignature = $verifyOutput | Where-Object {
        (($_.ToString()) -replace '\s', '') -match "VALIDSIG.*$fingerprint"
    }
    if (-not $validSignature) {
        Fail "tag signature is valid but not from the locked fingerprint $fingerprint"
    }
}
finally {
    $env:GNUPGHOME = $oldGnuPgHome
}

Invoke-Native $git.Source @("-C", $destination, "checkout", "--detach", $commit)
Write-Host "PCRE2 $($component.version) ready at locked commit $commit."
