[CmdletBinding()]
param(
    [switch]$ForceRefresh,
    [switch]$SkipSupplyChainVerification
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $repoRoot ".deps"
$buildRoot = Join-Path $depsRoot "build/qscintilla-2.14.1-qt6.11.1-msvc"
$installRoot = Join-Path $depsRoot "install/qscintilla-2.14.1-qt6.11.1-msvc"
$library = Join-Path $installRoot "lib/qscintilla2_qt6.lib"
$marker = Join-Path $installRoot ".notepadsafe-build-inputs"
$lockPath = Join-Path $repoRoot "third_party/dependencies.lock.json"

function Fail([string]$Message) {
    throw "QScintilla bootstrap failed: $Message"
}

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
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
$lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$component = @($lock.components | Where-Object name -eq "QScintilla")
$qtComponent = @($lock.components | Where-Object name -eq "Qt")
if ($component.Count -ne 1 -or $qtComponent.Count -ne 1) {
    Fail "lock file must contain exactly one QScintilla and one Qt component"
}
$source = Join-Path $repoRoot ([string]$component[0].local_path)
$project = Join-Path $source "src/qscintilla.pro"
if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
    Fail "vendored qmake project is missing: $project"
}

$qtRoot = $env:NOTEPADSAFE_QT6_DIR
if ([string]::IsNullOrWhiteSpace($qtRoot)) {
    $qtRoot = "E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64"
}
$qmake = Join-Path $qtRoot "bin/qmake.exe"
if (-not (Test-Path -LiteralPath $qmake -PathType Leaf)) {
    Fail "qmake was not found: $qmake. Set NOTEPADSAFE_QT6_DIR to the pinned static Qt root."
}
$qtVersion = (& $qmake -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -ne [string]$qtComponent[0].version) {
    Fail "qmake reports Qt '$qtVersion'; expected $($qtComponent[0].version)"
}
$nmake = Get-Command nmake.exe -ErrorAction SilentlyContinue
if (-not $nmake) {
    Fail "nmake.exe was not found. Run this script after vcvars64.bat."
}

$inputKey = @(
    "qscintilla-tree=$($component[0].integrity.value)",
    "qt=$qtVersion",
    "qmake=$([IO.Path]::GetFullPath($qmake))",
    "vctools=$env:VCToolsVersion",
    "runtime=MT",
    "configuration=release-staticlib"
) -join "`n"
$cacheMatches = (Test-Path -LiteralPath $library -PathType Leaf) -and
    (Test-Path -LiteralPath $marker -PathType Leaf) -and
    ((Get-Content -LiteralPath $marker -Raw -Encoding UTF8).TrimEnd("`r", "`n") -eq $inputKey)

if ($ForceRefresh -or -not $cacheMatches) {
    Remove-DependencyCache $buildRoot
    Remove-DependencyCache $installRoot
    [void](New-Item -ItemType Directory -Path $buildRoot -Force)
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $library) -Force)

    Write-Host "Building QScintilla 2.14.1 from vendored source with Qt $qtVersion..."
    Push-Location $buildRoot
    try {
        Invoke-Native $qmake @(
            $project,
            "CONFIG+=release",
            "CONFIG+=staticlib",
            "CONFIG+=c++17",
            "QMAKE_CFLAGS_RELEASE=/O2 /MT /DNDEBUG",
            "QMAKE_CXXFLAGS_RELEASE=/O2 /MT /DNDEBUG"
        )
        Invoke-Native $nmake.Source @("/NOLOGO")
    }
    finally {
        Pop-Location
    }

    $built = @(Get-ChildItem -LiteralPath $buildRoot -File -Recurse -Filter "qscintilla2_qt6.lib")
    if ($built.Count -ne 1) {
        Fail "expected one qscintilla2_qt6.lib under $buildRoot; found $($built.Count)"
    }
    Copy-Item -LiteralPath $built[0].FullName -Destination $library -Force
    Set-Content -LiteralPath $marker -Value $inputKey -Encoding UTF8
}

if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
    Fail "source-built library is missing: $library"
}
Write-Host "QScintilla source-built static library is ready: $library"
