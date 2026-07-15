[CmdletBinding()]
param(
    [ValidateSet('Build', 'Clean', 'All')]
    [string]$Command = 'Build',

    [switch]$HpmProfile,

    [switch]$PipelineProfile,

    [switch]$MicProfile,

    [string]$ToolchainBin = 'C:\code\MCU\tools\toolchain_gcc_t-head_windows\bin',

    [string]$SdkPath = 'C:\code\MCU\bl_mcu_sdk',

    [string]$OpusLibrary = '',

    [switch]$RebuildOpus
)

$ErrorActionPreference = 'Stop'

$ProjectDir = Split-Path -Parent $PSCommandPath
$BuildDirName = 'build-win'
$BuildDir = Join-Path $ProjectDir $BuildDirName
$OpusRoot = Join-Path $ProjectDir '.cache\third_party\opus-1.2.1'
$OpusSource = Join-Path $OpusRoot 'opus-1.2.1'
$OpusBuild = Join-Path $OpusRoot 'build-win-O2-LTO-e907-pvq-mdct-clusters-stage0'
$OpusCMakeSource = Join-Path $ProjectDir 'cmake\opus-1.2.1-windows'
$DefaultOpusLibrary = Join-Path $OpusBuild '.libs\libopus.a'

function Resolve-ExistingPath([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Convert-ToCMakePath([string]$Path) {
    return $Path.Replace('\', '/')
}

$SdkPath = Resolve-ExistingPath $SdkPath 'Bouffalo SDK'
$ToolchainBin = Resolve-ExistingPath $ToolchainBin 'Windows T-Head toolchain'
$Gcc = Resolve-ExistingPath (Join-Path $ToolchainBin 'riscv64-unknown-elf-gcc.exe') 'RISC-V GCC'
$Make = Resolve-ExistingPath (Join-Path $SdkPath 'tools\make\mingw32-make.exe') 'SDK make'
$CMakeBin = Resolve-ExistingPath (Join-Path $SdkPath 'tools\cmake\bin') 'SDK CMake directory'
$CMake = Resolve-ExistingPath (Join-Path $CMakeBin 'cmake.exe') 'SDK CMake'
$Ninja = Resolve-ExistingPath (Join-Path $SdkPath 'tools\ninja\ninja.exe') 'SDK Ninja'
$GccAr = Resolve-ExistingPath (Join-Path $ToolchainBin 'riscv64-unknown-elf-gcc-ar.exe') 'RISC-V GCC AR'
$GccRanlib = Resolve-ExistingPath (Join-Path $ToolchainBin 'riscv64-unknown-elf-gcc-ranlib.exe') 'RISC-V GCC ranlib'

if ($Command -in @('Build', 'All') -and [string]::IsNullOrWhiteSpace($OpusLibrary)) {
    Resolve-ExistingPath $OpusSource 'Patched Opus 1.2.1 source' | Out-Null
    Resolve-ExistingPath $OpusCMakeSource 'Windows Opus CMake project' | Out-Null

    if ($RebuildOpus -and (Test-Path -LiteralPath $OpusBuild)) {
        $OpusRootResolved = (Resolve-Path -LiteralPath $OpusRoot).Path
        $OpusBuildFull = [System.IO.Path]::GetFullPath($OpusBuild)
        if (-not $OpusBuildFull.StartsWith(
                $OpusRootResolved + [System.IO.Path]::DirectorySeparatorChar,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing unexpected Opus build directory: $OpusBuildFull"
        }
        Remove-Item -LiteralPath $OpusBuildFull -Recurse -Force
    }

    $OpusConfigureArgs = @(
        '-S', (Convert-ToCMakePath $OpusCMakeSource),
        '-B', (Convert-ToCMakePath $OpusBuild),
        '-G', 'Ninja',
        "-DOPUS_SOURCE_DIR=$(Convert-ToCMakePath $OpusSource)",
        "-DCMAKE_C_COMPILER=$(Convert-ToCMakePath $Gcc)",
        "-DCMAKE_AR=$(Convert-ToCMakePath $GccAr)",
        "-DCMAKE_RANLIB=$(Convert-ToCMakePath $GccRanlib)",
        "-DCMAKE_MAKE_PROGRAM=$(Convert-ToCMakePath $Ninja)"
    )
    & $CMake @OpusConfigureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Windows Opus configure failed with exit code $LASTEXITCODE"
    }
    & $CMake --build (Convert-ToCMakePath $OpusBuild) --parallel 8
    if ($LASTEXITCODE -ne 0) {
        throw "Windows Opus build failed with exit code $LASTEXITCODE"
    }
    $OpusLibrary = $DefaultOpusLibrary
}
if ($Command -in @('Build', 'All')) {
    $OpusLibrary = Resolve-ExistingPath $OpusLibrary 'M61 Opus library'
}

$ProjectResolved = (Resolve-Path -LiteralPath $ProjectDir).Path
$BuildFull = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $BuildFull.StartsWith($ProjectResolved + [System.IO.Path]::DirectorySeparatorChar,
                               [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unexpected build directory: $BuildFull"
}

$env:BL_SDK_BASE = $SdkPath
$env:M61_OPUS_LIBRARY = $OpusLibrary
$env:PATH = "$ToolchainBin;$CMakeBin;$(Join-Path $SdkPath 'tools\make');$env:PATH"

$CrossCompile = Convert-ToCMakePath (Join-Path $ToolchainBin 'riscv64-unknown-elf-')
$HpmValue = if ($HpmProfile -or $PipelineProfile) { 'y' } else { 'n' }
$PipelineValue = if ($PipelineProfile) { 'y' } else { 'n' }
$MicValue = if ($MicProfile) { 'y' } else { 'n' }
$MakeArgs = @(
    "CHIP=bl616",
    "BOARD=bl616dk",
    "BUILD_DIR=$BuildDirName",
    "CROSS_COMPILE=$CrossCompile",
    "CONFIG_M61_HPM_PROFILE=$HpmValue",
    "CONFIG_M61_PIPELINE_PROFILE=$PipelineValue",
    "CONFIG_M61_MEMORY_BENCH=n",
    "CONFIG_M61_OPUS_STAGE_PROFILE=n",
    "CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=$MicValue"
)

Write-Host "[m61-hidp-win] SDK: $SdkPath"
Write-Host "[m61-hidp-win] GCC: $(& $Gcc --version | Select-Object -First 1)"
Write-Host "[m61-hidp-win] Opus: $OpusLibrary"
Write-Host "[m61-hidp-win] Build: $BuildFull"
Write-Host "[m61-hidp-win] HPM profile: $HpmValue"
Write-Host "[m61-hidp-win] Pipeline profile: $PipelineValue"
Write-Host "[m61-hidp-win] Mic profile: $MicValue"

Push-Location $ProjectDir
try {
    if ($Command -in @('Clean', 'All')) {
        if (Test-Path -LiteralPath $BuildFull) {
            Remove-Item -LiteralPath $BuildFull -Recurse -Force
        }
    }

    if ($Command -in @('Build', 'All')) {
        & $Make @MakeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Windows firmware build failed with exit code $LASTEXITCODE"
        }

        $Firmware = Get-ChildItem -LiteralPath (Join-Path $BuildFull 'build_out') `
            -Filter 'm61_dualsense_hidp_probe_bl616.bin' -ErrorAction Stop |
            Select-Object -First 1
        Write-Host "[m61-hidp-win] Firmware: $($Firmware.FullName)"
    }
} finally {
    Pop-Location
}
