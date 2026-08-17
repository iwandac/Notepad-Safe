[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$ProductExecutable,
    [Parameter(Mandatory = $true)][string]$LockFile,
    [Parameter(Mandatory = $true)][string]$ProductVersion
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Fail([string]$Message) {
    throw "Release-package verification failed: $Message"
}

function Read-EntryText($Entry) {
    $stream = $Entry.Open()
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Hash-Stream($Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Stream))).Replace("-", "").ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function Hash-File([string]$Path) {
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Path))
    try { return Hash-Stream $stream } finally { $stream.Dispose() }
}

if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) { Fail "ZIP not found: $Package" }
if (-not (Test-Path -LiteralPath $ProductExecutable -PathType Leaf)) { Fail "exe not found: $ProductExecutable" }
if (-not (Test-Path -LiteralPath $LockFile -PathType Leaf)) { Fail "lock not found: $LockFile" }

$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package))
try {
    $files = @($archive.Entries | Where-Object { -not $_.FullName.EndsWith("/") })
    if ($files.Count -eq 0) { Fail "ZIP contains no files" }
    $roots = @($files | ForEach-Object { ($_.FullName -split "/", 2)[0] } | Sort-Object -Unique)
    if ($roots.Count -ne 1) { Fail "ZIP must have exactly one top-level directory" }
    $root = $roots[0]
    if ($root -ne "NotepadSafe-$ProductVersion-windows-x64") {
        Fail "unexpected top-level directory: $root"
    }
    $expected = @(
        "$root/notepad-safe.exe",
        "$root/LICENSE",
        "$root/README.md",
        "$root/THIRD_PARTY_NOTICES.md",
        "$root/metadata/dependencies.lock.json",
        "$root/metadata/notepad-safe.spdx.json",
        "$root/docs/TARGET_EXPERIENCE.md",
        "$root/docs/SUPPLY_CHAIN.md"
    ) | Sort-Object
    $actual = @($files.FullName | Sort-Object)
    if (($actual -join "`n") -ne ($expected -join "`n")) {
        Fail "archive file manifest differs from the release allow-list"
    }

    $byName = @{}
    foreach ($entry in $files) { $byName[$entry.FullName] = $entry }
    $license = Read-EntryText $byName["$root/LICENSE"]
    if ($license -notmatch "GNU GENERAL PUBLIC LICENSE" -or $license -notmatch "Version 3") {
        Fail "LICENSE is not the complete GPLv3 text"
    }

    $sourceLockHash = Hash-File $LockFile
    $lockStream = $byName["$root/metadata/dependencies.lock.json"].Open()
    try { $packedLockHash = Hash-Stream $lockStream } finally { $lockStream.Dispose() }
    if ($packedLockHash -ne $sourceLockHash) { Fail "packed dependency lock differs from source" }

    $lock = Get-Content -LiteralPath $LockFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $sbom = (Read-EntryText $byName["$root/metadata/notepad-safe.spdx.json"]) | ConvertFrom-Json
    if ($sbom.spdxVersion -ne "SPDX-2.3") { Fail "SBOM is not SPDX 2.3" }
    if (@($sbom.packages).Count -ne @($lock.components).Count + 1) {
        Fail "SBOM package count does not match the dependency lock"
    }
    $product = @($sbom.packages | Where-Object name -eq "Notepad Safe")
    if ($product.Count -ne 1 -or $product[0].versionInfo -ne $ProductVersion) {
        Fail "SBOM product version does not match the package"
    }
    foreach ($component in @($lock.components)) {
        $match = @($sbom.packages | Where-Object name -eq $component.name)
        if ($match.Count -ne 1 -or $match[0].versionInfo -ne $component.version) {
            Fail "SBOM component mismatch: $($component.name)"
        }
    }

    $exeStream = $byName["$root/notepad-safe.exe"].Open()
    try { $packedExeHash = Hash-Stream $exeStream } finally { $exeStream.Dispose() }
    $builtExeHash = Hash-File $ProductExecutable
    if ($packedExeHash -ne $builtExeHash) { Fail "packed exe differs from the verified build output" }
}
finally {
    $archive.Dispose()
}

Write-Host "Release package verified: exact manifest, GPLv3, lock, SPDX SBOM and executable hash."
