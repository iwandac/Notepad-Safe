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
        # Dependency checks apply to code, not design notes in comments.  A
        # lower layer may document the UI type it deliberately does not use.
        $codeContent = $content -replace '(?s)/\*.*?\*/', ''
        $codeContent = $codeContent -replace '(?m)//.*$', ''
        foreach ($token in $contract.Tokens) {
            if ($codeContent.Contains($token)) {
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
$mainSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/window/MainWindow.cpp') -Raw
foreach ($backendLeak in @('NOTEPADSAFE_HAS_KDBX_BACKEND',
                            'SecurityIdentity::',
                            'SecureDocumentService::inspect',
                            'KdbxFormatProbe',
                            'secure.kdbx')) {
    if ($mainSource.Contains($backendLeak)) {
        Add-Failure "MainWindow contains backend selection/business token: $backendLeak"
    }
}

$searchDockHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/search/SearchResultsDock.h') -Raw
foreach ($forbiddenState in @('EditorTab', 'QPointer<EditorTab>', 'TargetRevision',
                               'targetConnections', 'multiTargets')) {
    if ($searchDockHeader.Contains($forbiddenState)) {
        Add-Failure "SearchResultsDock view declares result-session state: $forbiddenState"
    }
}
foreach ($ownedSession in @('unique_ptr<SearchResultsController>',
                             'SearchResultsController m_')) {
    if ($searchDockHeader.Contains($ownedSession)) {
        Add-Failure "SearchResultsDock owns search-session controller: $ownedSession"
    }
}
$mainWindowHeaderForSearch = Get-Content -LiteralPath (
    Join-Path $SourceRoot 'src/window/MainWindow.h') -Raw
if (-not $mainWindowHeaderForSearch.Contains(
        'unique_ptr<SearchResultsController> m_searchResultsController')) {
    Add-Failure 'application shell does not own the search-session controller'
}
$searchDockSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/search/SearchResultsDock.cpp') -Raw
foreach ($forbiddenControllerWork in @('EditorTab', 'contentRevision()',
                                        '&EditorTab::contentChanged',
                                        'targetConnections')) {
    if ($searchDockSource.Contains($forbiddenControllerWork)) {
        Add-Failure "SearchResultsDock view performs controller work: $forbiddenControllerWork"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'src/search/SearchResultsController.cpp'))) {
    Add-Failure 'SearchResultsController implementation is missing'
}
$searchController = @(
    (Get-Content -LiteralPath (Join-Path $SourceRoot 'src/search/SearchResultsController.h') -Raw),
    (Get-Content -LiteralPath (Join-Path $SourceRoot 'src/search/SearchResultsController.cpp') -Raw)
) -join "`n"
foreach ($widgetLeak in @('EditorTab', 'QWidget', 'QPointer')) {
    if ($searchController.Contains($widgetLeak)) {
        Add-Failure "SearchResultsController depends on UI lifetime token: $widgetLeak"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'src/domain/PersistenceAdapterRegistry.cpp'))) {
    Add-Failure 'PersistenceAdapterRegistry implementation is missing'
}

$windowedProviderHeader = Get-Content -LiteralPath (
    Join-Path $SourceRoot 'src/editor/WindowedReadOnlyProvider.h') -Raw
if (-not $windowedProviderHeader.Contains('ReadOnlySelectionModel')) {
    Add-Failure 'WindowedReadOnlyProvider does not use the shared selection owner'
}
foreach ($duplicateSelectionState in @('m_selectionAnchor', 'm_selectionCaret')) {
    if ($windowedProviderHeader.Contains($duplicateSelectionState)) {
        Add-Failure "WindowedReadOnlyProvider duplicates selection state: $duplicateSelectionState"
    }
}

$domainFiles = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'src/domain') -File |
    Where-Object Extension -in '.cpp', '.h'
foreach ($domainFile in $domainFiles) {
    $domainContent = Get-Content -LiteralPath $domainFile.FullName -Raw
    foreach ($token in @('QWidget', 'QAction', 'MainWindow', 'EditorTab',
                         'SearchResultsDock', 'KeePass', 'Windows.h')) {
        if ($domainContent.Contains($token)) {
            Add-Failure "domain depends on UI/vendor token '$token': $($domainFile.Name)"
        }
    }
}

$domainTargetText = [regex]::Match(
    ($cmakeLines -join "`n"),
    '(?s)add_library\(notepadsafe_domain\s+STATIC.*?\n\)').Value
if ($domainTargetText.Contains('DraftStore.cpp')) {
    Add-Failure 'domain target owns platform-encrypted draft persistence'
}
if ($domainTargetText.Contains('DocumentProviderRegistry.cpp')) {
    Add-Failure 'domain target owns executable editor provider adapters'
}

# Complexity budgets are ratchets, not targets. They make responsibility
# extraction durable: a future feature must go into a focused component rather
# than silently growing the four legacy coordinators again.
$complexityBudgets = @(
    @{ File = 'src/window/MainWindow.cpp'; Lines = 3341; Branches = 348 },
    @{ File = 'src/window/MainWindowDocuments.cpp'; Lines = 1873; Branches = 240 },
    @{ File = 'src/window/MainWindowSession.cpp'; Lines = 563; Branches = 64 },
    @{ File = 'src/editor/EditorTab.cpp'; Lines = 2684; Branches = 330 },
    @{ File = 'src/editor/EditorTabLargeDocument.cpp'; Lines = 3320; Branches = 440 },
    @{ File = 'src/editor/EditorTabSearch.cpp'; Lines = 1683; Branches = 189 },
    @{ File = 'src/search/FindReplaceDialog.cpp'; Lines = 1499; Branches = 197 },
    @{ File = 'src/search/SearchResultsDock.cpp'; Lines = 523; Branches = 53 },
    @{ File = 'src/search/SearchResultsAggregateModel.h'; Lines = 293; Branches = 39 },
    @{ File = 'src/app/DocumentOpenWorkflow.cpp'; Lines = 101; Branches = 14 },
    @{ File = 'src/search/EditableTextSearch.cpp'; Lines = 110; Branches = 13 },
    @{ File = 'src/search/SearchResultsController.cpp'; Lines = 73; Branches = 4 },
    @{ File = 'src/search/SearchResultsDelegate.cpp'; Lines = 246; Branches = 17 },
    @{ File = 'src/window/FileCredentialDialog.cpp'; Lines = 131; Branches = 4 },
    @{ File = 'src/window/SensitiveText.cpp'; Lines = 41; Branches = 4 }
)
foreach ($budget in $complexityBudgets) {
    $path = Join-Path $SourceRoot $budget.File
    $lines = @(Get-Content -LiteralPath $path).Count
    $content = Get-Content -LiteralPath $path -Raw
    $branches = [regex]::Matches($content, '\bif\s*\(').Count
    if ($lines -gt $budget.Lines) {
        Add-Failure "$($budget.File) exceeds line budget: $lines > $($budget.Lines)"
    }
    if ($branches -gt $budget.Branches) {
        Add-Failure "$($budget.File) exceeds branch budget: $branches > $($budget.Branches)"
    }
}

# Global ceiling prevents a new unlisted mega-file from bypassing the named
# ratchets. Existing responsibilities are now split below this ceiling.
$productionSources = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'src') `
    -Recurse -File | Where-Object Extension -in '.cpp', '.h'
foreach ($source in $productionSources) {
    $relative = $source.FullName.Substring($SourceRoot.Length + 1).Replace('\', '/')
    $sourceLines = @(Get-Content -LiteralPath $source.FullName).Count
    if ($sourceLines -gt 3500) {
        Add-Failure "$relative exceeds global source ceiling: $sourceLines > 3500"
    }
}

$activeCmakeText = $activeCmake -join "`n"
if ($activeCmakeText -match
        'target_compile_definitions\(notepadsafe_vault\s+PUBLIC\s+NOTEPADSAFE_HAS_KDBX_BACKEND') {
    Add-Failure 'KDBX backend macro leaks through the public vault interface'
}
if ($activeCmakeText -match '(?i)TIMEOUT\s+3600') {
    Add-Failure 'monolithic 3600-second test timeout is forbidden'
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
