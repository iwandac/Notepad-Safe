[CmdletBinding()]
param(
    [switch]$ForceRefresh
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $repoRoot ".deps"
$downloadRoot = Join-Path $depsRoot "downloads"
$sourceRoot = Join-Path $depsRoot "src"
$buildRoot = Join-Path $depsRoot "build"
$installRoot = Join-Path $depsRoot "install"
$lockPath = Join-Path $repoRoot "third_party/dependencies.lock.json"
$patchPath = Join-Path $repoRoot "third_party/patches/keepassxc-2.7.12/0001-qt-6.11-qstringlist.patch"

function Fail([string]$Message) {
    throw "Crypto dependency bootstrap failed: $Message"
}

function Get-Component([string]$Name) {
    $match = @($script:lock.components | Where-Object name -eq $Name)
    if ($match.Count -ne 1) {
        Fail "expected exactly one lock entry for '$Name'"
    }
    return $match[0]
}

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
}

function Assert-Tool([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Fail "'$Name' was not found. $Hint"
    }
}

function Get-VerifiedArchive([object]$Component, [string]$FileName) {
    $archivePath = Join-Path $downloadRoot $FileName
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        Write-Host "Downloading $($Component.name) $($Component.version)..."
        Invoke-WebRequest -UseBasicParsing -Uri $Component.source -OutFile $archivePath
    }

    $actual = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = [string]$Component.integrity.value
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $archivePath -Force
        Fail "$($Component.name) archive SHA-256 mismatch (expected $expected, actual $actual)"
    }
    return $archivePath
}

function Expand-LockedArchive(
    [object]$Component,
    [string]$FileName,
    [string]$DirectoryName
) {
    $destination = Join-Path $sourceRoot $DirectoryName
    if ($ForceRefresh -and (Test-Path -LiteralPath $destination)) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    if (Test-Path -LiteralPath $destination -PathType Container) {
        return $destination
    }

    $archivePath = Get-VerifiedArchive $Component $FileName
    Write-Host "Extracting $($Component.name) $($Component.version)..."
    # Windows' bundled bsdtar can reject otherwise valid dotted paths from
    # upstream archives when the destination uses extended-path handling.
    # Python's standard tarfile reader handles the same locked archives
    # consistently on supported Windows hosts.
    Invoke-Native "python" @("-m", "tarfile", "-e", $archivePath, $sourceRoot)
    if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
        Fail "$($Component.name) archive did not create expected directory: $destination"
    }
    return $destination
}

if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
    Fail "lock file not found: $lockPath"
}
$script:lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json

Assert-Tool "git" "Install Git for Windows."
Assert-Tool "python" "Install Python 3 and add it to PATH."

foreach ($directory in @($depsRoot, $downloadRoot, $sourceRoot, $buildRoot, $installRoot)) {
    [void](New-Item -ItemType Directory -Path $directory -Force)
}

$keepassxc = Get-Component "KeePassXC"
$botan = Get-Component "Botan"
$argon2 = Get-Component "Argon2 reference library"
$zlib = Get-Component "zlib"

$keepassxcSource = Expand-LockedArchive $keepassxc "keepassxc-2.7.12-src.tar.xz" "keepassxc-2.7.12"
$botanSource = Expand-LockedArchive $botan "Botan-3.12.0.tar.xz" "Botan-3.12.0"
$zlibSource = Expand-LockedArchive $zlib "zlib-1.3.2.tar.xz" "zlib-1.3.2"

Write-Host "Checking KeePassXC compatibility patch..."
$keepassxcPatchDirectory = ".deps/src/keepassxc-2.7.12"
Push-Location $repoRoot
try {
    # A failed --reverse probe is expected for a pristine tree. PowerShell 5
    # otherwise promotes native stderr to a terminating error under Stop.
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & git apply --reverse --check --directory=$keepassxcPatchDirectory $patchPath 2>$null
    $alreadyPatched = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $savedErrorAction

    if ($alreadyPatched) {
        Write-Host "KeePassXC compatibility patch is already applied."
    }
    else {
        $ErrorActionPreference = "Continue"
        & git apply --check --directory=$keepassxcPatchDirectory $patchPath
        $patchApplies = $LASTEXITCODE -eq 0
        $ErrorActionPreference = $savedErrorAction
        if (-not $patchApplies) {
            Fail "KeePassXC compatibility patch neither applies nor reverses cleanly; use -ForceRefresh"
        }
        Invoke-Native "git" @("apply", "--directory=$keepassxcPatchDirectory", $patchPath)
    }
}
finally {
    $ErrorActionPreference = "Stop"
    Pop-Location
}

$argon2Source = Join-Path $sourceRoot "argon2-20190702"
if ($ForceRefresh -and (Test-Path -LiteralPath $argon2Source)) {
    Remove-Item -LiteralPath $argon2Source -Recurse -Force
}
if (-not (Test-Path -LiteralPath $argon2Source -PathType Container)) {
    Write-Host "Cloning Argon2 at locked commit..."
    Invoke-Native "git" @("clone", "--filter=blob:none", "--no-checkout", $argon2.source, $argon2Source)
    Invoke-Native "git" @("-C", $argon2Source, "checkout", "--detach", [string]$argon2.revision)
}
$argonHead = (& git -C $argon2Source rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $argonHead -ne [string]$argon2.integrity.value) {
    Fail "Argon2 checkout is not the locked commit; use -ForceRefresh"
}

$botanBuild = Join-Path $buildRoot "botan-3.12.0-msvc-mt"
$botanInstall = Join-Path $installRoot "botan-3.12.0"
$botanConfig = Join-Path $botanInstall "lib/cmake/Botan-3.12.0/botan-config.cmake"
if ($ForceRefresh) {
    foreach ($directory in @($botanBuild, $botanInstall)) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force
        }
    }
}
if (-not (Test-Path -LiteralPath $botanConfig -PathType Leaf)) {
    Assert-Tool "cl" "Run the first bootstrap from an x64 Visual Studio Developer PowerShell."
    $ninjaProgram = $env:NOTEPADSAFE_NINJA
    if ([string]::IsNullOrWhiteSpace($ninjaProgram)) {
        $ninjaCommand = Get-Command "ninja" -ErrorAction SilentlyContinue
        if ($ninjaCommand) {
            $ninjaProgram = $ninjaCommand.Source
        }
    }
    if ([string]::IsNullOrWhiteSpace($ninjaProgram)) {
        $vsRoot = $env:NOTEPADSAFE_VS_ROOT
        if ([string]::IsNullOrWhiteSpace($vsRoot)) {
            $vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Insiders"
        }
        $candidate = Join-Path $vsRoot "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $ninjaProgram = $candidate
        }
    }
    if ([string]::IsNullOrWhiteSpace($ninjaProgram) -or
        -not (Test-Path -LiteralPath $ninjaProgram -PathType Leaf)) {
        Fail "Ninja was not found. Set NOTEPADSAFE_NINJA or NOTEPADSAFE_VS_ROOT."
    }

    Write-Host "Building minimized Botan 3.12.0 static /MT library..."
    $env:VSLANG = "1033"
    $configure = Join-Path $botanSource "configure.py"
    $modules = "aes,base64,cbc,chacha,ctr,gcm,hmac,salsa20,sha2_32,sha2_64,system_rng,twofish"
    Invoke-Native "python" @(
        $configure,
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
}

foreach ($required in @(
    (Join-Path $keepassxcSource "src/format/Kdbx4Reader.cpp"),
    (Join-Path $argon2Source "src/argon2.c"),
    (Join-Path $zlibSource "zlib.h"),
    $botanConfig
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Fail "required dependency artifact is missing: $required"
    }
}

Write-Host "Crypto dependencies are ready and locked."
Write-Host "  KeePassXC: $keepassxcSource"
Write-Host "  Botan:     $botanInstall"
Write-Host "  Argon2:    $argon2Source"
Write-Host "  zlib:      $zlibSource"
