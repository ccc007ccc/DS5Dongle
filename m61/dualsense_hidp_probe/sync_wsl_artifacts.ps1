[CmdletBinding()]
param(
    [ValidateSet('Production', 'Core', 'Pipeline', 'Stage', 'Mic', 'Any')]
    [string]$ExpectedProfile = 'Core',

    [string]$Distro = 'Ubuntu',

    [string]$WslBuildOut = $env:M61_WSL_BUILD_OUT,

    [string]$Destination = (Join-Path $PSScriptRoot 'build\build_out')
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($WslBuildOut)) {
    throw 'Pass -WslBuildOut or set M61_WSL_BUILD_OUT to the absolute WSL build/build_out path.'
}

function Convert-WslPathToUnc {
    param(
        [Parameter(Mandatory)] [string]$Distribution,
        [Parameter(Mandatory)] [string]$Path
    )

    if (-not $Path.StartsWith('/')) {
        throw "WSL path must be absolute: $Path"
    }
    if ($Path -match '(^|/)\.\.(/|$)') {
        throw "WSL path must not contain '..': $Path"
    }

    $Relative = $Path.TrimStart('/').Replace('/', '\')
    $Candidates = @(
        "\\wsl.localhost\$Distribution\$Relative",
        "\\wsl$\$Distribution\$Relative"
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath $Candidate) {
            return $Candidate
        }
    }
    throw "WSL build output not found in distro '$Distribution': $Path"
}

$Source = Convert-WslPathToUnc -Distribution $Distro -Path $WslBuildOut
$BuildRoot = Split-Path -Parent $Source
$FlagsFile = Join-Path $BuildRoot 'CMakeFiles\app.dir\flags.make'

if (-not (Test-Path -LiteralPath $FlagsFile)) {
    throw "Application compiler flags not found: $FlagsFile"
}
$Flags = Get-Content -Raw -LiteralPath $FlagsFile
$HasCore = $Flags -match '(^|\s)-DCONFIG_M61_HPM_PROFILE=1(\s|$)'
$HasPipeline =
    $Flags -match '(^|\s)-DCONFIG_M61_PIPELINE_PROFILE=1(\s|$)'
$HasStage =
    $Flags -match '(^|\s)-DCONFIG_M61_OPUS_STAGE_PROFILE=1(\s|$)'
$HasMic =
    $Flags -match '(^|\s)-DCONFIG_M61_DS5_MIC_DEFAULT_ENABLED=1(\s|$)'

switch ($ExpectedProfile) {
    'Production' {
        if ($HasCore -or $HasPipeline -or $HasStage -or $HasMic) {
            throw 'Expected a production build, but profiling or mic diagnostics are enabled.'
        }
    }
    'Core' {
        if (-not $HasCore -or $HasPipeline -or $HasStage -or $HasMic) {
            throw 'Expected core HPM only: pipeline and Opus stage profiling must be off.'
        }
    }
    'Pipeline' {
        if (-not $HasCore -or -not $HasPipeline -or $HasStage -or $HasMic) {
            throw 'Expected pipeline profiling without Opus stage profiling.'
        }
    }
    'Stage' {
        if (-not $HasCore -or $HasPipeline -or -not $HasStage -or $HasMic) {
            throw 'Expected Opus stage profiling with core HPM and pipeline profiling off.'
        }
    }
    'Mic' {
        if (-not $HasCore -or $HasPipeline -or $HasStage -or -not $HasMic) {
            throw 'Expected mic diagnostics with core HPM only.'
        }
    }
}

$Required = @(
    'boot2_bl616_isp_release_v8.1.8.bin',
    'efusedata.bin',
    'efusedata_mask.bin',
    'efusedata_raw.bin',
    'm61_dualsense_hidp_probe_bl616.bin',
    'm61_dualsense_hidp_probe_bl616.elf',
    'm61_dualsense_hidp_probe_bl616.map',
    'mfg_bl616_gu_ef9d197c5_v2.50.bin',
    'partition.bin'
)

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($Name in $Required) {
    $SourceFile = Join-Path $Source $Name
    if (-not (Test-Path -LiteralPath $SourceFile)) {
        throw "Required build artifact is missing: $SourceFile"
    }
    Copy-Item -Force -LiteralPath $SourceFile -Destination $Destination

    $DestinationFile = Join-Path $Destination $Name
    $SourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SourceFile).Hash
    $DestinationHash =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $DestinationFile).Hash
    if ($SourceHash -ne $DestinationHash) {
        throw "SHA256 mismatch after copying: $Name"
    }
}

$Firmware = Join-Path $Destination 'm61_dualsense_hidp_probe_bl616.bin'
$FirmwareHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Firmware).Hash
Write-Host "[m61-wsl-sync] Profile: $ExpectedProfile"
Write-Host "[m61-wsl-sync] Source:  $Source"
Write-Host "[m61-wsl-sync] Target:  $(Resolve-Path -LiteralPath $Destination)"
Write-Host "[m61-wsl-sync] Firmware SHA256: $FirmwareHash"
