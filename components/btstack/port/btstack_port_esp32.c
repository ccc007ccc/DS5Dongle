/*
 * btstack_port_esp32.c — trimmed ESP32 VHCI port glue for the DS5Dongle
 * dual-chip firmware. Based on BTstack's port/esp32 template, reduced to
 * Classic-only over the internal controller (no UART/H4, no audio, no BLE).
 */

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include <stdint.h>
#include <stdio.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_freertos.h"
#include "btstack_tlv.h"
#include "btstack_tlv_esp32.h"
#include "classic/btstack_link_key_db.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci.h"
#include "hci_transport_esp32_vhci.h"

#include "esp_log.h"

uint32_t esp_log_timestamp(void);

uint32_t hal_time_ms(void) {
    return esp_log_timestamp();
}

uint8_t btstack_init(void) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_freertos_get_instance());

    hci_init(hci_transport_esp32_vhci_get_instance(), NULL);

    // link keys persisted in NVS through the TLV abstraction
    const btstack_tlv_t *tlv = btstack_tlv_esp32_get_instance();
    btstack_tlv_set_instance(tlv, NULL);
    const btstack_link_key_db_t *link_key_db =
        btstack_link_key_db_tlv_get_instance(tlv, NULL);
    hci_set_link_key_db(link_key_db);

    return ERROR_CODE_SUCCESS;
}

#endif // CONFIG_BT_ENABLED
