cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
sdk_add_compile_definitions(-DCONFIG_BT_L2CAP_TX_MTU=672)
target_sources(app PRIVATE m61_usb_gamepad.c)
target_sources(app PRIVATE m61_audio_epoch.c)
target_sources(app PRIVATE m61_bt_tx_scheduler.c)
target_sources(app PRIVATE m61_perf_profile.c)
target_sources(app PRIVATE dualsense_output.c)
target_sources(app PRIVATE dualsense_parser.c)
if(CONFIG_M61_HPM_PROFILE)
    # Profiling is an application-only feature.  Do not use
    # sdk_add_compile_definitions() here: that propagates the flag into the
    # prebuilt SDK/driver targets as well and has broken RF initialization on
    # real BL616 hardware.
    target_compile_definitions(app PRIVATE CONFIG_M61_HPM_PROFILE=1)
endif()
sdk_set_main_file(main.c)

project(m61_dualsense_hidp_probe)
