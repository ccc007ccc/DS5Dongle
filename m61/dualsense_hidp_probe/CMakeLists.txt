cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(.)
sdk_add_include_directories(../../main)

# The BL616 Opus archive is built with one section per function. Keep the
# encoder's CELT hot path and lookup tables in the SDK's copied ITCM region;
# the decoder and cold control paths remain XIP to preserve heap headroom.
set(OPUS_RAM_OBJECTS
    opus_encoder.c.o
    celt_encoder.c.o
    bands.c.o
    celt.c.o
    mdct.c.o
    kiss_fft.c.o
    pitch.c.o
    celt_lpc.c.o
    quant_bands.c.o
    rate.c.o
    vq.c.o
    cwrs.c.o
    entcode.c.o
    entenc.c.o
    laplace.c.o
    mathops.c.o
    modes.c.o
    opus.c.o
    analysis.c.o
    mlp.c.o
    mlp_data.c.o
)

set(BL616_LINKER_BASE
    "$ENV{BL_SDK_BASE}/bsp/board/bl616dk/bl616_common.ld.in")
if(NOT EXISTS "${BL616_LINKER_BASE}")
    message(FATAL_ERROR "BL616 linker template not found: ${BL616_LINKER_BASE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${BL616_LINKER_BASE}")
file(READ "${BL616_LINKER_BASE}" BL616_LINKER_TEXT)
set(OPUS_RAM_LINKER_RULES "")
foreach(OPUS_OBJECT IN LISTS OPUS_RAM_OBJECTS)
    string(APPEND OPUS_RAM_LINKER_RULES
           "        *libopus.a:${OPUS_OBJECT}*(.text* .rodata* .srodata*)\n")
endforeach()
set(TCM_LINKER_ANCHOR "        *(.tcm_code.*)")
string(FIND "${BL616_LINKER_TEXT}" "${TCM_LINKER_ANCHOR}"
       TCM_LINKER_ANCHOR_OFFSET)
if(TCM_LINKER_ANCHOR_OFFSET EQUAL -1)
    message(FATAL_ERROR "BL616 linker template has no ITCM input anchor")
endif()
string(REPLACE "${TCM_LINKER_ANCHOR}"
       "${OPUS_RAM_LINKER_RULES}${TCM_LINKER_ANCHOR}"
       BL616_OPUS_RAM_LINKER_TEXT "${BL616_LINKER_TEXT}")
set(BL616_OPUS_RAM_LINKER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bl616_opus_ram.ld.in")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${BL616_OPUS_RAM_LINKER}" "${BL616_OPUS_RAM_LINKER_TEXT}")
sdk_set_linker_script_macro("${BL616_OPUS_RAM_LINKER}")

target_sources(app PRIVATE m61_usb_gamepad.c)
target_sources(app PRIVATE m61_audio_epoch.c)
target_sources(app PRIVATE m61_esp32_transport.c)
target_sources(app PRIVATE m61_spi_scheduler.c)
target_sources(app PRIVATE m61_ds5_dse.c)
target_sources(app PRIVATE m61_ds5_bridge_config.c)
target_sources(app PRIVATE m61_ds5_inactivity.c)
target_sources(app PRIVATE m61_ps_shortcut.c)
target_sources(app PRIVATE ../../main/dualsense_output.c)
target_sources(app PRIVATE ../../main/dualsense_parser.c)
target_sources(app PRIVATE ../../main/dual_chip_scheduler_types.c)
target_sources(app PRIVATE ../../main/dual_chip_spi_proto.c)
sdk_set_main_file(main.c)

project(m61_dualsense_hidp_probe)
