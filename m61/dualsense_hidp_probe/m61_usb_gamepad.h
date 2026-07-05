#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dualsense_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M61_DS5_USB_INPUT_REPORT_ID 0x01
#define M61_DS5_USB_INPUT_PAYLOAD_LEN 63
#define M61_DS5_USB_OUTPUT_MAX_LEN 64
#define M61_DS5_USB_FEATURE_MAX_LEN 64
#define M61_DS5_HAPTICS_BLOCK_LEN 64

typedef struct {
    uint8_t report_id;
    uint8_t report_type;
    uint32_t len;
    uint8_t data[M61_DS5_USB_OUTPUT_MAX_LEN];
} m61_usb_gamepad_host_report_t;

void m61_usb_gamepad_init(void);
void m61_usb_gamepad_deinit(void);
int m61_usb_gamepad_reinit(void);
void m61_usb_gamepad_send_report01(const uint8_t *payload, size_t len);
void m61_usb_gamepad_send_state(const dualsense_state_t *state);
void m61_usb_gamepad_store_feature_report(uint8_t report_id, const uint8_t *data, size_t len);
bool m61_usb_gamepad_take_feature_request(uint8_t *report_id, uint32_t *requested_len);
bool m61_usb_gamepad_take_host_report(m61_usb_gamepad_host_report_t *report);
bool m61_usb_gamepad_take_haptics_block(uint8_t *data, size_t len);
bool m61_usb_gamepad_ready(void);
bool m61_usb_gamepad_configured(void);
bool m61_usb_gamepad_busy(void);
uint32_t m61_usb_gamepad_sent_count(void);
uint32_t m61_usb_gamepad_drop_count(void);
int m61_usb_gamepad_init_result(void);
uint8_t m61_usb_gamepad_last_event(void);
uint32_t m61_usb_gamepad_event_count(uint8_t event);

typedef struct {
    uint32_t device_desc;
    uint32_t config_desc;
    uint32_t qualifier_desc;
    uint32_t string_desc;
    uint32_t string0_desc;
    uint32_t hid_get_report;
    uint32_t hid_get_idle;
    uint32_t hid_get_protocol;
    uint32_t hid_set_report;
    uint32_t hid_set_idle;
    uint32_t hid_set_protocol;
    uint32_t hid_out_report;
    uint32_t feature_cache_hits;
    uint32_t feature_cache_misses;
    uint32_t feature_cache_stores;
    uint32_t host_report_dropped;
    uint32_t audio_open;
    uint32_t audio_close;
    uint32_t audio_out_packets;
    uint32_t audio_out_bytes;
    uint32_t audio_in_packets;
    uint32_t audio_in_bytes;
    uint32_t audio_haptic_blocks;
    uint32_t audio_haptic_sample_pairs;
    uint32_t audio_haptic_nonzero_blocks;
    uint32_t audio_haptic_queue_dropped;
    uint32_t audio_set_volume;
    uint32_t audio_set_mute;
    uint32_t audio_set_freq;
    uint8_t last_speed;
    uint8_t last_string_index;
    uint8_t last_report_id;
    uint8_t last_report_type;
    uint8_t last_out_report_id;
    uint8_t audio_last_open_intf;
    uint8_t audio_last_close_intf;
    uint8_t audio_out_open;
    uint8_t audio_in_open;
    uint8_t audio_haptic_queue_depth;
    uint8_t audio_haptic_last_peak;
    uint8_t audio_haptic_downsample;
    uint8_t audio_haptic_resample_mode;
    uint16_t audio_haptic_gain_q8;
    uint8_t audio_speaker_mute;
    uint8_t audio_mic_mute;
    int audio_speaker_volume_db;
    int audio_mic_volume_db;
    uint32_t last_out_report_len;
    uint32_t reg_dev_ctl;
    uint32_t reg_dev_adr;
    uint32_t reg_phy_tst;
    uint32_t reg_otg_csr;
    uint32_t reg_glb_isr;
    uint32_t reg_dev_igr;
    uint32_t reg_isg0;
    uint32_t reg_isg2;
    uint32_t reg_isg3;
    uint32_t reg_vdma_ctrl;
    uint32_t reg_vdma_cxfps1;
} m61_usb_gamepad_diag_t;

void m61_usb_gamepad_get_diag(m61_usb_gamepad_diag_t *diag);

#ifdef __cplusplus
}
#endif
