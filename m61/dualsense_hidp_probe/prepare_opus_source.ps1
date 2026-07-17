[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProjectDir = Split-Path -Parent $PSCommandPath
$RepoRoot = (Resolve-Path (Join-Path $ProjectDir '..\..')).Path
$LockPath = Join-Path $ProjectDir 'reproducible-build.lock.json'
$Lock = Get-Content -Raw -LiteralPath $LockPath | ConvertFrom-Json
$Opus = $Lock.opus
$Root = Join-Path $ProjectDir ".cache\third_party\opus-$($Opus.version)"
$Archive = Join-Path $Root "opus-$($Opus.version).tar.gz"
$Source = Join-Path $Root "opus-$($Opus.version)"
$Stamp = Join-Path $Source '.m61-source-lock.json'

$ExpectedStamp = [ordered]@{
    version = $Opus.version
    archiveSha256 = $Opus.archiveSha256
    patches = @($Opus.patches | ForEach-Object {
        [ordered]@{ path = $_.path; sha256 = $_.sha256 }
    })
} | ConvertTo-Json -Depth 5 -Compress

if (-not $Force -and (Test-Path -LiteralPath $Stamp)) {
    $CurrentStamp = Get-Content -Raw -LiteralPath $Stamp
    if ($CurrentStamp -eq $ExpectedStamp -and
        (Test-Path -LiteralPath (Join-Path $Source 'include\opus.h'))) {
        Write-Host "[m61-opus] Source already matches the lock file: $Source"
        return
    }
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null
if (-not (Test-Path -LiteralPath $Archive)) {
    Write-Host "[m61-opus] Downloading $($Opus.archiveUrl)"
    Invoke-WebRequest -Uri $Opus.archiveUrl -OutFile $Archive
}

$ArchiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $Opus.archiveSha256) {
    throw "Opus archive SHA256 mismatch: $ArchiveHash"
}

if (Test-Path -LiteralPath $Source) {
    $RootResolved = (Resolve-Path -LiteralPath $Root).Path
    $SourceFull = [System.IO.Path]::GetFullPath($Source)
    if (-not $SourceFull.StartsWith(
            $RootResolved + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing unexpected Opus source directory: $SourceFull"
    }
    Remove-Item -LiteralPath $SourceFull -Recurse -Force
}

& tar.exe -xzf $Archive -C $Root
if ($LASTEXITCODE -ne 0) {
    throw "Unable to extract Opus archive (exit $LASTEXITCODE)"
}

$SourceFull = [System.IO.Path]::GetFullPath($Source)
if (-not $SourceFull.StartsWith(
        $RepoRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Opus source is outside the repository: $SourceFull"
}
$RelativeSource = $SourceFull.Substring($RepoRoot.Length + 1).Replace('\', '/')
foreach ($Patch in $Opus.patches) {
    $PatchPath = Join-Path $RepoRoot $Patch.path
    Write-Host "[m61-opus] Applying $($Patch.path)"
    & git -C $RepoRoot apply --unidiff-zero --ignore-space-change --whitespace=nowarn `
        "--directory=$RelativeSource" $PatchPath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to apply Opus patch: $($Patch.path)"
    }
}

[System.IO.File]::WriteAllText(
    $Stamp,
    $ExpectedStamp,
    [System.Text.UTF8Encoding]::new($false))
Write-Host "[m61-opus] Prepared locked source: $Source"
