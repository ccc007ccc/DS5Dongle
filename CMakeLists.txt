cmake_minimum_required(VERSION 3.16)

# Current mainline firmware is the M61-only adapter under:
#   m61/dualsense_hidp_probe
#
# This ESP-IDF project is kept as an ESP32 fallback and compatibility target.
# Keep project(ds5_dualsense_bridge_esp32) stable because existing build and
# flash helper scripts derive artifact names from it.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ds5_dualsense_bridge_esp32)
