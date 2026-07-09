cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
sdk_add_include_directories(../../main)
target_sources(app PRIVATE m61_usb_gamepad.c)
target_sources(app PRIVATE m61_esp32_transport.c)
target_sources(app PRIVATE m61_ds5_dse.c)
target_sources(app PRIVATE m61_ds5_bridge_config.c)
target_sources(app PRIVATE ../../main/dualsense_output.c)
target_sources(app PRIVATE ../../main/dualsense_parser.c)
target_sources(app PRIVATE ../../main/dual_chip_spi_proto.c)
sdk_set_main_file(main.c)

project(m61_dualsense_hidp_probe)
