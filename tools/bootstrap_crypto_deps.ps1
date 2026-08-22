[CmdletBinding()]
param(
    [switch]$ForceRefresh,
    [switch]$SkipSupplyChainVerification
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $repoRoot ".deps"
$buildRoot = Join-Path $depsRoot "build"
$installRoot = Join-Path $depsRoot "install"
$lockPath = Join-Path $repoRoot "third_party/dependencies.lock.json"

function Fail([string]$Message) {
    throw "Crypto dependency bootstrap failed: $Message"
}

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
}

function Get-Component([string]$Name) {
    $match = @($script:lock.components | Where-Object name -eq $Name)
    if ($match.Count -ne 1) {
        Fail "expected exactly one lock entry for '$Name'"
    }
    return $match[0]
}

function Remove-DependencyCache([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $full = (Resolve-Path -LiteralPath $Path).Path
    $safeRoot = [IO.Path]::GetFullPath($depsRoot) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($safeRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "refusing to remove cache outside .deps: $full"
    }
    Remove-Item -LiteralPath $full -Recurse -Force
}

if (-not $SkipSupplyChainVerification) {
    & (Join-Path $PSScriptRoot "verify_supply_chain.ps1")
}
$script:lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$keepassxc = Get-Component "KeePassXC"
$botan = Get-Component "Botan"
$argon2 = Get-Component "Argon2 reference library"
$zlib = Get-Component "zlib"

$keepassxcSource = Join-Path $repoRoot ([string]$keepassxc.local_path)
$botanSource = Join-Path $repoRoot ([string]$botan.local_path)
$argon2Source = Join-Path $repoRoot ([string]$argon2.local_path)
$zlibSource = Join-Path $repoRoot ([string]$zlib.local_path)
foreach ($required in @(
    (Join-Path $keepassxcSource "src/format/Kdbx4Reader.cpp"),
    (Join-Path $botanSource "configure.py"),
    (Join-Path $argon2Source "src/argon2.c"),
    (Join-Path $zlibSource "zlib.h")
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Fail "committed dependency source is incomplete: $required"
    }
}

$botanBuild = Join-Path $buildRoot "botan-3.12.0-msvc-mt"
$botanInstall = Join-Path $installRoot "botan-3.12.0"
$botanConfig = Join-Path $botanInstall "lib/cmake/Botan-3.12.0/botan-config.cmake"
$marker = Join-Path $botanInstall ".notepadsafe-build-inputs"
$modules = "aes,base64,cbc,chacha,ctr,gcm,hmac,salsa20,sha2_32,sha2_64,system_rng,twofish"
$inputKey = @(
    "botan-tree=$($botan.integrity.value)",
    "vctools=$env:VCToolsVersion",
    "runtime=MT",
    "modules=$modules"
) -join "`n"
$cacheMatches = (Test-Path -LiteralPath $botanConfig -PathType Leaf) -and
    (Test-Path -LiteralPath $marker -PathType Leaf) -and
    ((Get-Content -LiteralPath $marker -Raw -Encoding UTF8).TrimEnd("`r", "`n") -eq $inputKey)

if ($ForceRefresh -or -not $cacheMatches) {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Fail "cl.exe was not found. Run this script after vcvars64.bat."
    }
    $ninjaProgram = $env:NOTEPADSAFE_NINJA
    if ([string]::IsNullOrWhiteSpace($ninjaProgram)) {
        $ninjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
        if ($ninjaCommand) { $ninjaProgram = $ninjaCommand.Source }
    }
    if ([string]::IsNullOrWhiteSpace($ninjaProgram)) {
        $vsRoot = $env:NOTEPADSAFE_VS_ROOT
        if ([string]::IsNullOrWhiteSpace($vsRoot)) {
            $vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Insiders"
        }
        $ninjaProgram = Join-Path $vsRoot "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
    }
    if (-not (Test-Path -LiteralPath $ninjaProgram -PathType Leaf)) {
        Fail "Ninja was not found. Set NOTEPADSAFE_NINJA or NOTEPADSAFE_VS_ROOT."
    }

    Remove-DependencyCache $botanBuild
    Remove-DependencyCache $botanInstall
    [void](New-Item -ItemType Directory -Path $buildRoot -Force)
    [void](New-Item -ItemType Directory -Path $installRoot -Force)

    Write-Host "Building minimized Botan 3.12.0 static /MT library from vendored source..."
    $env:VSLANG = "1033"
    # build.bat selects UTF-8 console output. Match Python's subprocess decoder
    # so a localized MSVC banner cannot crash Botan's compiler-version probe.
    $env:PYTHONUTF8 = "1"
    & chcp.com 65001 | Out-Null
    Invoke-Native "python" @(
        (Join-Path $botanSource "configure.py"),
        "--cc=msvc",
        "--os=windows",
        "--cpu=x86_64",
        "--msvc-runtime=MT",
        "--disable-shared-library",
        "--build-targets=static",
        "--build-tool=ninja",
        "--without-documentation",
        "--minimized-build",
        "--enable-modules=$modules",
        "--with-build-dir=$botanBuild",
        "--prefix=$botanInstall"
    )
    Invoke-Native $ninjaProgram @("-C", $botanBuild, "install")
    Set-Content -LiteralPath $marker -Value $inputKey -Encoding UTF8
}

if (-not (Test-Path -LiteralPath $botanConfig -PathType Leaf)) {
    Fail "source-built Botan package is missing: $botanConfig"
}

Write-Host "Crypto dependency sources and generated Botan library are ready."
Write-Host "  KeePassXC: $keepassxcSource"
Write-Host "  Botan:     $botanInstall"
Write-Host "  Argon2:    $argon2Source"
Write-Host "  zlib:      $zlibSource"
