Set-StrictMode -Version 2.0

function ConvertTo-LowerHex([byte[]]$Bytes) {
    return ([BitConverter]::ToString($Bytes)).Replace("-", "").ToLowerInvariant()
}

function Get-FileSha256([Parameter(Mandatory = $true)][string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ConvertTo-LowerHex $sha256.ComputeHash($stream)
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Get-SourceTreeIntegrity([Parameter(Mandatory = $true)][string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Source tree does not exist: $Path"
    }

    $root = (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\', '/')
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    $entries = @{}
    $relativePaths = [System.Collections.Generic.List[string]]::new()

    foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Vendored source must not contain a reparse point: $($file.FullName)"
        }
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        $entries[$relative] = $file.FullName
        $relativePaths.Add($relative)
    }

    if ($relativePaths.Count -eq 0) {
        throw "Source tree is empty: $root"
    }
    $paths = $relativePaths.ToArray()
    [Array]::Sort($paths, [StringComparer]::Ordinal)

    $manifest = [Text.StringBuilder]::new()
    foreach ($relative in $paths) {
        $fileDigest = Get-FileSha256 -Path $entries[$relative]
        [void]$manifest.Append($relative)
        [void]$manifest.Append([char]0)
        [void]$manifest.Append($fileDigest)
        [void]$manifest.Append("`n")
    }

    $utf8 = [Text.UTF8Encoding]::new($false)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $treeDigest = ConvertTo-LowerHex $sha256.ComputeHash($utf8.GetBytes($manifest.ToString()))
    }
    finally {
        $sha256.Dispose()
    }

    return [pscustomobject]@{
        Path = $root
        Sha256 = $treeDigest
        Files = $paths.Count
    }
}
