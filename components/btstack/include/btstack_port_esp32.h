#ifndef BTSTACK_PORT_ESP32_H
#define BTSTACK_PORT_ESP32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize BTstack for the ESP32 internal controller (VHCI):
// memory pools, FreeRTOS run loop, HCI transport and NVS-backed link key DB.
// Call once after nvs_flash_init(); does not power on the controller.
uint8_t btstack_init(void);

#ifdef __cplusplus
}
#endif

#endif
