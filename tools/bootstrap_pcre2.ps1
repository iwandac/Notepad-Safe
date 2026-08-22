[CmdletBinding()]
param(
    [switch]$ForceRefresh,
    [switch]$SkipSupplyChainVerification
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot "third_party/src/pcre2-10.47"

if ($ForceRefresh) {
    Write-Warning "-ForceRefresh no longer downloads PCRE2. Restore or update the committed source tree through the audited dependency-update workflow."
}
if (-not $SkipSupplyChainVerification) {
    & (Join-Path $PSScriptRoot "verify_supply_chain.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "PCRE2 bootstrap failed: supply-chain verification failed"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt") -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $source "src/pcre2_compile.c") -PathType Leaf)) {
    throw "PCRE2 bootstrap failed: committed source tree is incomplete: $source"
}

Write-Host "PCRE2 10.47 source is ready for the CMake static 8-bit build: $source"
