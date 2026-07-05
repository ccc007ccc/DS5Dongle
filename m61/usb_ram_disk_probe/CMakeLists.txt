cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
target_sources(app PRIVATE msc_ram_probe.c)
sdk_set_main_file(main.c)

project(m61_usb_ram_disk_probe)
