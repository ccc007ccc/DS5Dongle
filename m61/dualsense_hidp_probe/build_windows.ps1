[CmdletBinding()]
param(
    [ValidateSet('Build', 'Clean', 'All')]
    [string]$Command = 'Build',

    [switch]$HpmProfile,

    [ValidateRange(0, 8)]
    [int]$HpmSampleShift = 4,

    [switch]$UsbGamepadO2 = $true,

    [ValidateSet(1, 2)]
    [int]$CodecPairDelayMs = 1,

    [ValidateSet(0, 1)]
    [int]$Crc32NibbleTable = 1,

    [switch]$RuntimeProfile,

    [ValidateSet(0, 384, 400, 420, 460, 480)]
    [int]$CpuOverclockMhz = 0,

    [switch]$PipelineProfile,

    [switch]$MicProfile,

    [switch]$OpusStageProfile,

    [ValidateSet('pvq-mdct-clusters', 'pvq-mdct-decode-clusters', 'pvq-mdct-decode-mdct')]
    [string]$OpusTcmProfile = 'pvq-mdct-decode-mdct',

    [string]$ToolchainBin = $env:M61_TOOLCHAIN_BIN,

    [string]$SdkPath = $env:BL_SDK_BASE,

    [string]$OpusLibrary = '',

    [switch]$RebuildOpus,

    [switch]$AllowUnverifiedDependencies
)

$ErrorActionPreference = 'Stop'

# GitHub's Windows runner exports SHELL=/usr/bin/bash. Bouffalo's MinGW make
# inherits it and then sends native Windows paths through Git Bash, which
# strips backslashes. The Windows build must use make's native command shell.
Remove-Item Env:SHELL -ErrorAction SilentlyContinue

$ProjectDir = Split-Path -Parent $PSCommandPath
$RepoRoot = (Resolve-Path (Join-Path $ProjectDir '..\..')).Path
$WorkspaceRoot = (Resolve-Path (Join-Path $ProjectDir '..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($ToolchainBin)) {
    $ToolchainBin = Join-Path $WorkspaceRoot 'tools\toolchain_gcc_t-head_windows\bin'
}
if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = Join-Path $WorkspaceRoot 'bl_mcu_sdk'
}
$BuildDirName = 'build-win'
$BuildDir = Join-Path $ProjectDir $BuildDirName
$OpusRoot = Join-Path $ProjectDir '.cache\third_party\opus-1.2.1'
$OpusSource = Join-Path $OpusRoot 'opus-1.2.1'
$OpusStageTag = if ($OpusStageProfile) { 'stage1' } else { 'stage0' }
$OpusBuild = Join-Path $OpusRoot "build-win-O2-LTO-e907-$OpusTcmProfile-$OpusStageTag"
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
$Python = (Get-Command python -ErrorAction Stop).Source

# The locked SDK embeds __DATE__/__TIME__ in board and driver strings. GCC
# derives those macros from SOURCE_DATE_EPOCH when it is set, so use the
# source commit time instead of the local wall clock.
if ([string]::IsNullOrWhiteSpace($env:SOURCE_DATE_EPOCH)) {
    $env:SOURCE_DATE_EPOCH = (& git -C $RepoRoot show -s --format=%ct HEAD).Trim()
}
if ($env:SOURCE_DATE_EPOCH -notmatch '^\d+$') {
    throw "SOURCE_DATE_EPOCH must be an integer Unix timestamp"
}
Write-Host "[m61-repro] SOURCE_DATE_EPOCH $env:SOURCE_DATE_EPOCH"

if (-not $AllowUnverifiedDependencies) {
    & $Python (Join-Path $RepoRoot 'tools\verify_m61_build_environment.py') `
        --sdk $SdkPath --toolchain-bin $ToolchainBin
    if ($LASTEXITCODE -ne 0) {
        throw "M61 dependency verification failed with exit code $LASTEXITCODE"
    }
}

if ($Command -in @('Build', 'All') -and [string]::IsNullOrWhiteSpace($OpusLibrary)) {
    & (Join-Path $ProjectDir 'prepare_opus_source.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw "Locked Opus source preparation failed with exit code $LASTEXITCODE"
    }
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
        "-DCMAKE_MAKE_PROGRAM=$(Convert-ToCMakePath $Ninja)",
        "-DM61_OPUS_STAGE_PROFILE=$(if ($OpusStageProfile) { 'ON' } else { 'OFF' })"
        "-DM61_OPUS_TCM_PROFILE=$OpusTcmProfile"
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
$UsbGamepadO2Value = if ($UsbGamepadO2) { 'y' } else { 'n' }
$PipelineValue = if ($PipelineProfile) { 'y' } else { 'n' }
$MicValue = if ($MicProfile) { 'y' } else { 'n' }
$RuntimeValue = if ($RuntimeProfile) { 'y' } else { 'n' }
$OpusStageValue = if ($OpusStageProfile) { 'y' } else { 'n' }
$UsbGamepadO2Bool = if ($UsbGamepadO2Value -eq 'y') { 'true' } else { 'false' }
$Crc32NibbleTableBool = if ($Crc32NibbleTable -eq 1) { 'true' } else { 'false' }
$HpmBool = if ($HpmValue -eq 'y') { 'true' } else { 'false' }
$RuntimeBool = if ($RuntimeValue -eq 'y') { 'true' } else { 'false' }
$PipelineBool = if ($PipelineValue -eq 'y') { 'true' } else { 'false' }
$OpusStageBool = if ($OpusStageValue -eq 'y') { 'true' } else { 'false' }
$MicBool = if ($MicValue -eq 'y') { 'true' } else { 'false' }
$MakeArgs = @(
    "SHELL=cmd.exe",
    "CHIP=bl616",
    "BOARD=bl616dk",
    "BUILD_DIR=$BuildDirName",
    "CROSS_COMPILE=$CrossCompile",
    "CONFIG_M61_HPM_PROFILE=$HpmValue",
    "CONFIG_M61_HPM_SAMPLE_SHIFT=$HpmSampleShift",
    "CONFIG_M61_USB_GAMEPAD_O2=$UsbGamepadO2Value",
    "CONFIG_M61_CODEC_PAIR_DELAY_MS=$CodecPairDelayMs",
    "CONFIG_M61_CRC32_NIBBLE_TABLE=$Crc32NibbleTable",
    "CONFIG_M61_RUNTIME_PROFILE=$RuntimeValue",
    "CONFIG_M61_CPU_OVERCLOCK_MHZ=$CpuOverclockMhz",
    "CONFIG_M61_PIPELINE_PROFILE=$PipelineValue",
    "CONFIG_M61_MEMORY_BENCH=n",
    "CONFIG_M61_OPUS_STAGE_PROFILE=$OpusStageValue",
    "CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=$MicValue"
)

Write-Host "[m61-hidp-win] SDK: $SdkPath"
Write-Host "[m61-hidp-win] GCC: $(& $Gcc --version | Select-Object -First 1)"
Write-Host "[m61-hidp-win] Opus: $OpusLibrary"
Write-Host "[m61-hidp-win] Build: $BuildFull"
Write-Host "[m61-hidp-win] HPM profile: $HpmValue"
Write-Host "[m61-hidp-win] HPM sample shift: $HpmSampleShift (about 1/$([Math]::Pow(2, $HpmSampleShift)))"
Write-Host "[m61-hidp-win] USB gamepad TU O2: $UsbGamepadO2Value"
Write-Host "[m61-hidp-win] Codec pair delay: $CodecPairDelayMs ms"
Write-Host "[m61-hidp-win] CRC32 nibble table: $Crc32NibbleTable (Flash-resident)"
Write-Host "[m61-hidp-win] Runtime profile: $RuntimeValue (diagnostic only)"
Write-Host "[m61-hidp-win] CPU overclock: $CpuOverclockMhz MHz (0=off)"
Write-Host "[m61-hidp-win] Pipeline profile: $PipelineValue"
Write-Host "[m61-hidp-win] Mic profile: $MicValue"
Write-Host "[m61-hidp-win] Opus stage profile: $OpusStageValue"
Write-Host "[m61-hidp-win] Opus TCM profile: $OpusTcmProfile"

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
        $Elf = Get-ChildItem -LiteralPath (Join-Path $BuildFull 'build_out') `
            -Filter 'm61_dualsense_hidp_probe_bl616.elf' -ErrorAction Stop |
            Select-Object -First 1
        $Map = Get-ChildItem -LiteralPath (Join-Path $BuildFull 'build_out') `
            -Filter 'm61_dualsense_hidp_probe_bl616.map' -ErrorAction Stop |
            Select-Object -First 1
        $Manifest = Join-Path $Firmware.DirectoryName `
            'm61_dualsense_hidp_probe_bl616.manifest.json'
        $ManifestArgs = @(
            (Join-Path $RepoRoot 'tools\generate_m61_build_manifest.py'),
            '--firmware', $Firmware.FullName,
            '--elf', $Elf.FullName,
            '--map', $Map.FullName,
            '--sdk', $SdkPath,
            '--toolchain-bin', $ToolchainBin,
            '--output', $Manifest,
            '--source-date-epoch', $env:SOURCE_DATE_EPOCH,
            '--setting', 'chip=bl616',
            '--setting', 'board=bl616dk',
            '--setting', 'wramLengthBytes=163840',
            '--setting', 'opusVariant=O2-LTO-e907-d4-fastpath',
            '--setting', "opusTcmProfile=$OpusTcmProfile",
            '--setting', "usbGamepadO2=$UsbGamepadO2Bool",
            '--setting', "codecPairDelayMs=$CodecPairDelayMs",
            '--setting', "crc32NibbleTable=$Crc32NibbleTableBool",
            '--setting', "hpmProfile=$HpmBool",
            '--setting', "runtimeProfile=$RuntimeBool",
            '--setting', "pipelineProfile=$PipelineBool",
            '--setting', "opusStageProfile=$OpusStageBool",
            '--setting', "micDefaultEnabled=$MicBool",
            '--setting', "compileTimeCpuOverclockMhz=$CpuOverclockMhz",
            '--setting', 'runtimeCpuGovernor=manual',
            '--setting', 'runtimeCpuMhz=320',
            '--setting', 'speakerRoute=auto'
        )
        & $Python @ManifestArgs
        if ($LASTEXITCODE -ne 0) {
            throw "M61 build manifest generation failed with exit code $LASTEXITCODE"
        }
        Write-Host "[m61-hidp-win] Firmware: $($Firmware.FullName)"
    }
} finally {
    Pop-Location
}
