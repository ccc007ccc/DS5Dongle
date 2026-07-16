cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Allow reproducible A/B tests against a source-built Opus without modifying
# the shared Bouffalo SDK checkout.  The SDK registers its prebuilt archive
# before this project is evaluated, so replace that one entry in SDK_LIBS.
if(DEFINED ENV{M61_OPUS_LIBRARY} AND NOT "$ENV{M61_OPUS_LIBRARY}" STREQUAL "")
    file(REAL_PATH "$ENV{M61_OPUS_LIBRARY}" M61_OPUS_LIBRARY_PATH)
    if(NOT EXISTS "${M61_OPUS_LIBRARY_PATH}")
        message(FATAL_ERROR "M61_OPUS_LIBRARY not found: ${M61_OPUS_LIBRARY_PATH}")
    endif()
    get_property(M61_SDK_LIBS GLOBAL PROPERTY SDK_LIBS)
    list(FILTER M61_SDK_LIBS EXCLUDE REGEX "/components/multimedia/opus/libs/.*/libopus\\.a$")
    set_property(GLOBAL PROPERTY SDK_LIBS "${M61_SDK_LIBS}")
    sdk_add_static_library("${M61_OPUS_LIBRARY_PATH}")
    message(STATUS "M61 Opus override: ${M61_OPUS_LIBRARY_PATH}")
endif()

sdk_add_include_directories(.)
sdk_add_compile_definitions(-DCONFIG_BT_L2CAP_TX_MTU=672)
target_sources(app PRIVATE m61_usb_gamepad.c)
target_sources(app PRIVATE m61_audio_epoch.c)
target_sources(app PRIVATE m61_bt_tx_scheduler.c)
target_sources(app PRIVATE m61_realtime_scheduler.c)
target_sources(app PRIVATE m61_perf_profile.c)
target_sources(app PRIVATE dualsense_output.c)
target_sources(app PRIVATE dualsense_parser.c)
if(NOT DEFINED CONFIG_M61_CPU_OVERCLOCK_MHZ)
    set(CONFIG_M61_CPU_OVERCLOCK_MHZ 0)
endif()
if(NOT CONFIG_M61_CPU_OVERCLOCK_MHZ MATCHES "^(0|384|400|420|460|480)$")
    message(FATAL_ERROR "CONFIG_M61_CPU_OVERCLOCK_MHZ must be 0, 384, 400, 420, 460, or 480")
endif()
if(NOT DEFINED CONFIG_M61_CODEC_PAIR_DELAY_MS)
    set(CONFIG_M61_CODEC_PAIR_DELAY_MS 1)
endif()
if(NOT CONFIG_M61_CODEC_PAIR_DELAY_MS MATCHES "^(1|2)$")
    message(FATAL_ERROR "CONFIG_M61_CODEC_PAIR_DELAY_MS must be 1 or 2")
endif()
if(NOT DEFINED CONFIG_M61_RUNTIME_PROFILE)
    set(CONFIG_M61_RUNTIME_PROFILE 0)
endif()
if(NOT CONFIG_M61_RUNTIME_PROFILE MATCHES "^(0|1|y|n)$")
    message(FATAL_ERROR "CONFIG_M61_RUNTIME_PROFILE must be 0/1 or y/n")
endif()
set(M61_RUNTIME_PROFILE_VALUE 0)
if(CONFIG_M61_RUNTIME_PROFILE STREQUAL "1" OR
   CONFIG_M61_RUNTIME_PROFILE STREQUAL "y")
    set(M61_RUNTIME_PROFILE_VALUE 1)
endif()
if(M61_RUNTIME_PROFILE_VALUE)
    if(NOT TARGET libfreertos)
        message(FATAL_ERROR "FreeRTOS target unavailable for runtime profiling")
    endif()
    # Task switch hooks live in the kernel translation unit; keep the flag
    # target-local so Bluetooth/RF/USB SDK libraries retain release codegen.
    target_compile_definitions(libfreertos PRIVATE
        CONFIG_M61_RUNTIME_PROFILE=1)
    target_sources(app PRIVATE m61_runtime_profile.c)
endif()
target_compile_definitions(app PRIVATE
    CONFIG_M61_CPU_OVERCLOCK_MHZ=${CONFIG_M61_CPU_OVERCLOCK_MHZ}
    CONFIG_M61_CODEC_PAIR_DELAY_MS=${CONFIG_M61_CODEC_PAIR_DELAY_MS}
    CONFIG_M61_RUNTIME_PROFILE=${M61_RUNTIME_PROFILE_VALUE})
if(CONFIG_M61_USB_GAMEPAD_O2)
    # A/B only the USB/codec translation unit.  Keep the rest of the app at
    # the SDK's -Os default so code layout and BT/USB task footprints remain
    # comparable with the established baseline.
    set_source_files_properties(m61_usb_gamepad.c PROPERTIES
        COMPILE_OPTIONS "-O2")
    message(STATUS "M61 USB gamepad TU optimization: -O2")
endif()
if(NOT DEFINED CONFIG_M61_HPM_SAMPLE_SHIFT)
    set(CONFIG_M61_HPM_SAMPLE_SHIFT 4)
endif()
if(CONFIG_M61_HPM_PROFILE)
    # Profiling is an application-only feature.  Do not use
    # sdk_add_compile_definitions() here: that propagates the flag into the
    # prebuilt SDK/driver targets as well and has broken RF initialization on
    # real BL616 hardware.
    target_compile_definitions(app PRIVATE
        CONFIG_M61_HPM_PROFILE=1
        CONFIG_M61_HPM_SAMPLE_SHIFT=${CONFIG_M61_HPM_SAMPLE_SHIFT})
    # The profile status command prints the full USB/codec/BT snapshot.  Keep
    # its diagnostic-only stack headroom out of production firmware and out
    # of every realtime task.
    target_compile_definitions(libshell PRIVATE SHELL_THREAD_STACK_SIZE=2048)
endif()
if(CONFIG_M61_DS5_MIC_DEFAULT_ENABLED)
    # Diagnostic-only until full-duplex pressure tests prove enough headroom.
    target_compile_definitions(app PRIVATE
        CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=1)
endif()
if(CONFIG_M61_PIPELINE_PROFILE)
    target_compile_definitions(app PRIVATE
        CONFIG_M61_HPM_PROFILE=1
        CONFIG_M61_PIPELINE_PROFILE=1)
endif()
if(CONFIG_M61_MEMORY_BENCH)
    # Test-only: adds 44 KiB of executable test data in Flash and OCRAM.  Keep
    # it out of production builds so it cannot change codec layout or RAM use.
    target_sources(app PRIVATE m61_memory_bench.c m61_memory_bench.S)
    target_compile_definitions(app PRIVATE
        CONFIG_M61_MEMORY_BENCH=1
        CONFIG_M61_HPM_PROFILE=1)
endif()
if(CONFIG_M61_OPUS_STAGE_PROFILE)
    # Test-only hooks called from a source-built Opus archive.
    target_sources(app PRIVATE m61_opus_stage_profile.c)
    target_compile_definitions(app PRIVATE
        CONFIG_M61_OPUS_STAGE_PROFILE=1
        CONFIG_M61_HPM_PROFILE=1)
endif()
sdk_set_main_file(main.c)

project(m61_dualsense_hidp_probe)
