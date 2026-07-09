//
// btstack_config.h — DS5Dongle ESP32 dual-chip firmware
// Classic BR/EDR host only, VHCI transport, link keys in NVS via TLV.
//
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_ASSERT
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_INCLUDE_PREFIX
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// HCI Controller to Host Flow Control - required for VHCI
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL

// Logging
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// Classic only — BLE is not used by the DualSense bridge
#define ENABLE_CLASSIC

#define NVM_NUM_LINK_KEYS 16

// DualSense HIDP uses Basic mode L2CAP with MTU 672
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)

#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 20
#define HCI_HOST_SCO_PACKET_LEN 60
#define HCI_HOST_SCO_PACKET_NUM 10

#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_SERVICES 4
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_SERVICE_RECORD_ITEMS 4

#endif
