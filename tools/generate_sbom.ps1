[CmdletBinding()]
param(
    [string]$LockFile = "",
    [string]$OutputFile = "",
    [string]$ProductVersion = "0.0.0"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($LockFile)) {
    $LockFile = Join-Path $repoRoot "third_party/dependencies.lock.json"
}
if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    $OutputFile = Join-Path $repoRoot "build/notepad-safe.spdx.json"
}

$lockBytes = [IO.File]::ReadAllBytes($LockFile)
$sha256 = [Security.Cryptography.SHA256]::Create()
$lockDigest = ([BitConverter]::ToString($sha256.ComputeHash($lockBytes))).Replace("-", "").ToLowerInvariant()
$lock = [Text.Encoding]::UTF8.GetString($lockBytes) | ConvertFrom-Json

function Spdx-Id([string]$Name) {
    return "SPDXRef-" + ($Name -replace '[^A-Za-z0-9.-]', '-')
}

$productId = "SPDXRef-Notepad-Safe"
$packages = @([ordered]@{
    SPDXID = $productId
    name = "Notepad Safe"
    versionInfo = $ProductVersion
    downloadLocation = "NOASSERTION"
    filesAnalyzed = $false
    licenseConcluded = "GPL-3.0-only"
    licenseDeclared = "GPL-3.0-only"
    copyrightText = "NOASSERTION"
})
$relationships = @()

foreach ($component in @($lock.components)) {
    $id = Spdx-Id ([string]$component.name)
    $package = [ordered]@{
        SPDXID = $id
        name = [string]$component.name
        versionInfo = [string]$component.version
        downloadLocation = [string]$component.source
        filesAnalyzed = $false
        licenseConcluded = [string]$component.license
        licenseDeclared = [string]$component.license
        copyrightText = "NOASSERTION"
        comment = "Pinned revision: $([string]$component.revision); status: $([string]$component.status)"
    }
    $upstreamIntegrity = if ($component.PSObject.Properties.Name -contains "source_integrity") {
        $component.source_integrity
    } else {
        $component.integrity
    }
    if ([string]$upstreamIntegrity.kind -eq "sha256") {
        $package.checksums = @([ordered]@{
            algorithm = "SHA256"
            checksumValue = [string]$upstreamIntegrity.value
        })
    }
    if ([string]$component.integrity.kind -eq "source-tree-sha256") {
        $package.comment += "; vendored source-tree SHA-256: $([string]$component.integrity.value)"
    }
    $packages += $package
    $relationships += [ordered]@{
        spdxElementId = $productId
        relationshipType = "DEPENDS_ON"
        relatedSpdxElement = $id
    }
}

$document = [ordered]@{
    spdxVersion = "SPDX-2.3"
    dataLicense = "CC0-1.0"
    SPDXID = "SPDXRef-DOCUMENT"
    name = "Notepad-Safe-$ProductVersion"
    documentNamespace = "https://notepad-safe.invalid/spdx/$ProductVersion/$($lockDigest.Substring(0, 16))"
    creationInfo = [ordered]@{
        created = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        creators = @("Tool: Notepad-Safe-generate_sbom.ps1")
    }
    documentDescribes = @($productId)
    packages = $packages
    relationships = $relationships
    annotations = @([ordered]@{
        annotationDate = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        annotationType = "OTHER"
        annotator = "Tool: Notepad-Safe-generate_sbom.ps1"
        comment = "Generated from dependencies.lock.json SHA-256 $lockDigest"
    })
}

$parent = Split-Path -Parent $OutputFile
if ($parent) {
    [IO.Directory]::CreateDirectory($parent) | Out-Null
}
$json = $document | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($OutputFile, $json + "`n", [Text.UTF8Encoding]::new($false))
Write-Host "SPDX SBOM generated: $OutputFile ($($packages.Count) packages)"
