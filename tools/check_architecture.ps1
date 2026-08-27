param(
    [string]$SourceRoot = (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$message) {
    $failures.Add($message)
}

function Remove-CodeComments([string]$content) {
    $withoutBlocks = $content -replace '(?s)/\*.*?\*/', ''
    return $withoutBlocks -replace '(?m)//.*$', ''
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
    @{ Files = @('src/search/LargeFileSearch.h', 'src/search/LargeFileSearch.cpp', 'src/search/Pcre2Regex.h', 'src/search/Pcre2Regex.cpp', 'src/search/EncodedRegexCompiler.h', 'src/search/EncodedRegexCompiler.cpp'); Tokens = @('QWidget', 'MainWindow', 'EditorTab', 'SearchResultsDock', 'KeePass') },
    @{ Files = @('src/editor/DocumentSurface.h', 'src/app/DocumentProviderRegistry.h', 'src/app/DocumentProviderRegistry.cpp'); Tokens = @('MainWindow', 'EditorTab', 'QsciScintilla') }
)
foreach ($contract in $lowerContracts) {
    foreach ($relative in $contract.Files) {
        $path = Join-Path $SourceRoot $relative
        $content = Get-Content -LiteralPath $path -Raw
        # Dependency checks apply to code, not design notes in comments.  A
        # lower layer may document the UI type it deliberately does not use.
        $codeContent = Remove-CodeComments $content
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
$mainSource = (Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'src/window') `
    -Filter 'MainWindow*.cpp' | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Raw
    }) -join "`n"
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
foreach ($viewPolicyLeak in @('setSectionResizeMode(', 'm_treeView->setModel(')) {
    if ($searchDockSource.Contains($viewPolicyLeak)) {
        Add-Failure "SearchResultsDock bypasses SearchResultsViewPolicy: $viewPolicyLeak"
    }
}
$searchViewPolicy = Get-Content -LiteralPath (
    Join-Path $SourceRoot 'src/search/SearchResultsViewPolicy.cpp') -Raw
foreach ($requiredPolicy in @('setUniformRowHeights(true)', 'setWordWrap(false)',
                               'setTextElideMode(Qt::ElideRight)',
                               'setSectionResizeMode')) {
    if (-not $searchViewPolicy.Contains($requiredPolicy)) {
        Add-Failure "compact search-results policy is incomplete: $requiredPolicy"
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
    $domainContent = Remove-CodeComments (
        Get-Content -LiteralPath $domainFile.FullName -Raw)
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

$openWorkflow = Get-Content -LiteralPath (
    Join-Path $SourceRoot 'src/app/DocumentOpenWorkflow.cpp') -Raw
if (-not $openWorkflow.Contains('openBest(')) {
    Add-Failure 'DocumentOpenWorkflow bypasses capability-based provider selection'
}
foreach ($providerId in @('text.editable', 'text.windowed', 'binary.windowed')) {
    if ($openWorkflow.Contains($providerId)) {
        Add-Failure "DocumentOpenWorkflow hard-codes provider id: $providerId"
    }
}
$documentRuntimeLinks = [regex]::Match(
    ($cmakeLines -join "`n"),
    '(?s)target_link_libraries\(notepadsafe_document_runtime.*?\n\)').Value
if ($documentRuntimeLinks.Contains('notepadsafe_editor_runtime')) {
    Add-Failure 'document provider registry links back to concrete editor runtime'
}

foreach ($runner in @('run_tests.bat', 'run_all_gui_smoke.bat')) {
    $runnerText = Get-Content -LiteralPath (Join-Path $SourceRoot $runner) -Raw
    if (-not $runnerText.Contains('--no-tests=error')) {
        Add-Failure "$runner can pass with an empty CTest selection"
    }
}
if (-not ($cmakeLines -join "`n").Contains('gui;smoke;${tier};${feature}')) {
    Add-Failure 'GUI tests are not registered under the smoke gate label'
}

# Complexity budgets are ratchets, not targets. They make responsibility
# extraction durable: a future feature must go into a focused component rather
# than silently growing the four legacy coordinators again.
$complexityBudgets = @(
    @{ File = 'src/window/MainWindow.cpp'; Lines = 1694; Branches = 128 },
    @{ File = 'src/window/MainWindowCommands.cpp'; Lines = 1615; Branches = 204 },
    @{ File = 'src/window/MainWindowDocuments.cpp'; Lines = 1870; Branches = 240 },
    @{ File = 'src/window/MainWindowSession.cpp'; Lines = 551; Branches = 64 },
    @{ File = 'src/editor/EditorTab.cpp'; Lines = 1897; Branches = 241 },
    @{ File = 'src/editor/EditorTabCommands.cpp'; Lines = 764; Branches = 69 },
    @{ File = 'src/editor/EditorTabLargeDocument.cpp'; Lines = 2618; Branches = 326 },
    @{ File = 'src/editor/EditorTabLargeDocumentPersistence.cpp'; Lines = 727; Branches = 102 },
    @{ File = 'src/editor/EditorTabSearch.cpp'; Lines = 1683; Branches = 189 },
    @{ File = 'src/search/FindReplaceDialog.cpp'; Lines = 1499; Branches = 197 },
    @{ File = 'src/search/SearchResultsDock.cpp'; Lines = 523; Branches = 53 },
    @{ File = 'src/search/SearchResultsAggregateModel.h'; Lines = 293; Branches = 39 },
    @{ File = 'src/app/DocumentOpenWorkflow.cpp'; Lines = 111; Branches = 14 },
    @{ File = 'src/search/EditableTextSearch.cpp'; Lines = 110; Branches = 13 },
    @{ File = 'src/search/SearchResultsController.cpp'; Lines = 73; Branches = 4 },
    @{ File = 'src/search/SearchResultsDelegate.cpp'; Lines = 246; Branches = 17 },
    @{ File = 'src/search/SearchResultsViewPolicy.cpp'; Lines = 55; Branches = 4 },
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

# Splitting a large file must not reset the complexity counter. These aggregate
# ratchets cover every implementation fragment for the two legacy shells.
$aggregateBudgets = @(
    @{ Name = 'MainWindow'; Pattern = 'src/window/MainWindow*.cpp'; Lines = 5729; Branches = 636; Methods = 112 },
    @{ Name = 'EditorTab'; Pattern = 'src/editor/EditorTab*.cpp'; Lines = 7687; Branches = 927; Methods = 176 }
)
foreach ($budget in $aggregateBudgets) {
    $files = Get-ChildItem -Path (Join-Path $SourceRoot $budget.Pattern) -File
    $lines = 0
    $branches = 0
    $methods = 0
    foreach ($file in $files) {
        $content = Get-Content -LiteralPath $file.FullName -Raw
        $lines += @(Get-Content -LiteralPath $file.FullName).Count
        $branches += [regex]::Matches($content, '\bif\s*\(').Count
        $methods += [regex]::Matches(
            $content, "\b$($budget.Name)::[A-Za-z_][A-Za-z0-9_]*\s*\(").Count
    }
    if ($lines -gt $budget.Lines) {
        Add-Failure "$($budget.Name) aggregate exceeds line budget: $lines > $($budget.Lines)"
    }
    if ($branches -gt $budget.Branches) {
        Add-Failure "$($budget.Name) aggregate exceeds branch budget: $branches > $($budget.Branches)"
    }
    if ($methods -gt $budget.Methods) {
        Add-Failure "$($budget.Name) aggregate exceeds method budget: $methods > $($budget.Methods)"
    }
}

# Global ceiling prevents a new unlisted mega-file from bypassing the named
# ratchets. Existing responsibilities are now split below this ceiling.
$productionSources = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'src') `
    -Recurse -File | Where-Object Extension -in '.cpp', '.h'
foreach ($source in $productionSources) {
    $relative = $source.FullName.Substring($SourceRoot.Length + 1).Replace('\', '/')
    $sourceLines = @(Get-Content -LiteralPath $source.FullName).Count
    if ($sourceLines -gt 3000) {
        Add-Failure "$relative exceeds global source ceiling: $sourceLines > 3000"
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
