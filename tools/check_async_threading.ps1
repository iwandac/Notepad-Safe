$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repoRoot 'src'
$violations = @()
$patterns = @(
    'QtConcurrent::run\s*\(',
    '\bQThreadPool\s+[A-Za-z_][A-Za-z0-9_]*',
    '\bstd::thread\b',
    'QThread::create\s*\(',
    'new\s+QThread\b'
)

Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
    Where-Object {
        $_.Extension -in @('.cpp', '.h', '.hpp') -and
        $_.FullName -notlike "*\src\async\*"
    } |
    ForEach-Object {
        $path = $_.FullName
        $lineNumber = 0
        Get-Content -LiteralPath $path | ForEach-Object {
            $lineNumber++
            $line = $_
            foreach ($pattern in $patterns) {
                if ($line -match $pattern) {
                    $relative = [IO.Path]::GetRelativePath($repoRoot, $path)
                    $violations += "${relative}:${lineNumber}: $line"
                    break
                }
            }
        }
    }

if ($violations.Count -gt 0) {
    Write-Error ("First-party concurrency must use src/async:`n" +
                 ($violations -join "`n"))
}

Write-Host 'Async threading boundary: OK'
