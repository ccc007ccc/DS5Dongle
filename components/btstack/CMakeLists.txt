# Trimmed BTstack component for the DS5Dongle ESP32 dual-chip firmware.
# Classic BR/EDR host only (HCI + L2CAP + SDP server + link key DB over NVS),
# transport is the ESP32 VHCI interface (controller-only IDF configuration).
idf_component_register(
    SRCS
        "src/ad_parser.c"
        "src/btstack_ring_buffer.c"
        "src/btstack_linked_list.c"
        "src/btstack_memory.c"
        "src/btstack_memory_pool.c"
        "src/btstack_run_loop.c"
        "src/btstack_run_loop_base.c"
        "src/btstack_tlv.c"
        "src/btstack_util.c"
        "src/hci.c"
        "src/hci_cmd.c"
        "src/hci_dump.c"
        "src/l2cap.c"
        "src/l2cap_signaling.c"
        "src/classic/btstack_link_key_db_tlv.c"
        "src/classic/sdp_server.c"
        "src/classic/sdp_util.c"
        "platform/freertos/btstack_run_loop_freertos.c"
        "port/hci_transport_esp32_vhci.c"
        "port/btstack_tlv_esp32.c"
        "port/btstack_port_esp32.c"
    INCLUDE_DIRS
        "include"
        "src"
        "platform/freertos"
        "platform/embedded"
    REQUIRES
        bt
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
