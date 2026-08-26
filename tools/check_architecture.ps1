param(
    [string]$SourceRoot = (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$message) {
    $failures.Add($message)
}

$cmakePath = Join-Path $SourceRoot 'CMakeLists.txt'
$cmakeLines = Get-Content -LiteralPath $cmakePath
$activeCmake = $cmakeLines | Where-Object { $_ -notmatch '^\s*#' }
$productRefs = foreach ($line in $activeCmake) {
    foreach ($match in [regex]::Matches($line, 'src/[A-Za-z0-9_./-]+\.cpp')) {
        $match.Value.Replace('/', '\')
    }
}
foreach ($group in ($productRefs | Group-Object | Where-Object Count -gt 1)) {
    Add-Failure "product source is listed more than once in CMake: $($group.Name) ($($group.Count)x)"
}

$testMarker = [Array]::IndexOf($cmakeLines, 'if(NOTEPADSAFE_BUILD_TESTS)')
if ($testMarker -lt 0) {
    Add-Failure 'NOTEPADSAFE_BUILD_TESTS section was not found'
} else {
    for ($index = $testMarker + 1; $index -lt $cmakeLines.Count; ++$index) {
        $line = $cmakeLines[$index]
        if ($line -match '^\s*#') { continue }
        if ($line -match 'src/[A-Za-z0-9_./-]+\.cpp') {
            Add-Failure "test/diagnostic target compiles product source directly at CMakeLists.txt:$($index + 1)"
        }
    }
}

$lowerContracts = @(
    @{ Files = @('src/domain/FileTierPolicy.h', 'src/domain/FileTierPolicy.cpp'); Tokens = @('QWidget', 'MainWindow', 'EditorTab', 'KeePass', 'Windows.h') },
    @{ Files = @('src/search/LargeFileSearch.h', 'src/search/LargeFileSearch.cpp', 'src/search/Pcre2Regex.h', 'src/search/Pcre2Regex.cpp', 'src/search/EncodedRegexCompiler.h', 'src/search/EncodedRegexCompiler.cpp'); Tokens = @('QWidget', 'MainWindow', 'EditorTab', 'SearchResultsDock', 'KeePass') }
)
foreach ($contract in $lowerContracts) {
    foreach ($relative in $contract.Files) {
        $path = Join-Path $SourceRoot $relative
        $content = Get-Content -LiteralPath $path -Raw
        foreach ($token in $contract.Tokens) {
            if ($content.Contains($token)) {
                Add-Failure "forbidden dependency '$token' in $relative"
            }
        }
    }
}

$mainHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/window/MainWindow.h') -Raw
foreach ($concrete in @('unique_ptr<SecuritySessionService>',
                        'unique_ptr<SecureDocumentService>')) {
    if ($mainHeader.Contains($concrete)) {
        Add-Failure "MainWindow owns concrete security adapter: $concrete"
    }
}

$concreteFactories = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'src') -Recurse -File |
    Where-Object Extension -in '.cpp', '.h' |
    Select-String -Pattern 'make_unique<SecuritySessionService>|make_unique<SecureDocumentService>'
foreach ($hit in $concreteFactories) {
    if ($hit.Path -notlike '*SecurityServiceFactory.cpp') {
        Add-Failure "concrete security service constructed outside composition factory: $($hit.Path):$($hit.LineNumber)"
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "Architecture boundary: OK ($($productRefs.Count) unique product source entries)"
