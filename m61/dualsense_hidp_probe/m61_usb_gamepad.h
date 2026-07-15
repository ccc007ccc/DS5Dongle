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
#define M61_DS5_SPEAKER_OPUS_LEN 200
#define M61_DS5_MIC_OPUS_LEN 71

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
void m61_usb_gamepad_submit_mic_opus(const uint8_t *data, size_t len);
int m61_usb_gamepad_set_decoder_benchmark(bool enabled);
bool m61_usb_gamepad_decoder_benchmark_enabled(void);
bool m61_usb_gamepad_audio_mic_enabled(void);
bool m61_usb_gamepad_audio_in_active(void);
bool m61_usb_gamepad_audio_speaker_active(void);
uint32_t m61_usb_gamepad_audio_generation(void);
void m61_usb_gamepad_realtime_task(void);
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
    uint8_t feature_set_queue_depth;
    uint8_t feature_set_queue_high_water;
    uint32_t audio_open;
    uint32_t audio_close;
    uint32_t audio_out_packets;
    uint32_t audio_out_bytes;
    uint32_t audio_out_frames_48_packets;
    uint32_t audio_out_frames_49_packets;
    uint32_t audio_out_frames_other_packets;
    uint32_t audio_out_interval_samples;
    uint32_t audio_out_interval_us_last;
    uint32_t audio_out_interval_us_average;
    uint32_t audio_out_interval_us_min;
    uint32_t audio_out_interval_us_max;
    uint32_t audio_epoch_interval_samples;
    uint32_t audio_epoch_interval_us_last;
    uint32_t audio_epoch_interval_us_average;
    uint32_t audio_epoch_interval_us_min;
    uint32_t audio_epoch_interval_us_max;
    uint32_t audio_ingress_dropped;
    uint32_t audio_ingress_gaps;
    uint32_t audio_in_packets;
    uint32_t audio_in_bytes;
    uint32_t audio_haptic_blocks;
    uint32_t audio_haptic_sample_pairs;
    uint32_t audio_haptic_nonzero_blocks;
    uint32_t audio_haptic_queue_dropped;
    uint32_t audio_haptic_deadline_pairs;
    uint32_t audio_speaker_frames;
    uint32_t audio_speaker_encoded;
    uint32_t audio_speaker_encode_errors;
    uint32_t audio_speaker_queue_dropped;
    uint32_t audio_speaker_opus_dropped;
    uint32_t audio_speaker_encode_cancelled;
    uint32_t audio_speaker_encode_us_total;
    uint32_t audio_speaker_encode_us_last;
    uint32_t audio_speaker_encode_us_max;
    uint8_t perf_profile_enabled;
    uint32_t perf_encode_samples;
    uint32_t perf_encode_us_p50;
    uint32_t perf_encode_us_p95;
    uint32_t perf_encode_us_p99;
    uint32_t perf_encode_cycles_last;
    uint32_t perf_encode_cycles_average;
    uint32_t perf_encode_cycles_max;
    uint32_t perf_encode_instret_average;
    uint32_t perf_icache_access_average;
    uint32_t perf_icache_miss_average;
    uint32_t perf_icache_miss_ppm;
    uint32_t perf_dcache_read_average;
    uint32_t perf_dcache_read_miss_average;
    uint32_t perf_dcache_read_miss_ppm;
    uint32_t perf_decode_samples;
    uint32_t perf_decode_us_last;
    uint32_t perf_decode_us_average;
    uint32_t perf_decode_us_max;
    uint32_t perf_decode_us_p50;
    uint32_t perf_decode_us_p95;
    uint32_t perf_decode_us_p99;
    uint32_t perf_decode_cycles_last;
    uint32_t perf_decode_cycles_average;
    uint32_t perf_decode_cycles_max;
    uint32_t perf_decode_instret_average;
    uint32_t perf_decode_icache_access_average;
    uint32_t perf_decode_icache_miss_average;
    uint32_t perf_decode_icache_miss_ppm;
    uint32_t perf_decode_dcache_read_average;
    uint32_t perf_decode_dcache_read_miss_average;
    uint32_t perf_decode_dcache_read_miss_ppm;
    uint32_t perf_ingress_samples;
    uint32_t perf_ingress_age_us_last;
    uint32_t perf_ingress_age_us_max;
    uint32_t perf_ingress_age_us_p95;
    uint32_t perf_ingress_age_us_p99;
    uint32_t perf_irq_mask_cycles_max;
    uint16_t audio_speaker_last_opus_len;
    uint32_t audio_mic_opus_packets;
    uint32_t audio_mic_opus_nonzero;
    uint32_t audio_mic_opus_dropped;
    uint32_t audio_mic_decoded;
    uint32_t audio_mic_decode_errors;
    uint32_t audio_decoder_benchmark_frames;
    uint32_t audio_decoder_benchmark_errors;
    uint32_t audio_mic_pcm_bytes;
    uint32_t audio_mic_pcm_nonzero_samples;
    uint32_t audio_mic_usb_nonzero_packets;
    uint32_t audio_mic_usb_nonzero_bytes;
    uint32_t audio_mic_underflow;
    uint32_t audio_codec_stack_hwm;
    uint32_t audio_codec_heap_free;
    uint32_t audio_codec_heap_min;
    uint32_t audio_codec_encoder_size;
    uint32_t audio_codec_decoder_size;
    int audio_codec_encoder_error;
    int audio_codec_decoder_error;
    uint32_t audio_set_volume;
    uint32_t audio_set_mute;
    uint32_t audio_set_freq;
    uint8_t last_speed;
    uint8_t last_string_index;
    uint8_t last_report_id;
    uint8_t last_report_type;
    uint8_t last_out_report_id;
    uint8_t last_out_flags0;
    uint8_t last_out_flags1;
    uint8_t last_out_flags2;
    uint8_t last_out_rumble_right;
    uint8_t last_out_rumble_left;
    uint8_t last_out_audio_control;
    uint8_t last_out_mute_light;
    uint8_t last_out_audio_mute;
    uint8_t last_out_player_lights;
    uint8_t last_out_light_fade;
    uint8_t last_out_light_brightness;
    uint8_t last_out_led_red;
    uint8_t last_out_led_green;
    uint8_t last_out_led_blue;
    uint8_t audio_last_open_intf;
    uint8_t audio_last_close_intf;
    uint8_t audio_out_open;
    uint8_t audio_in_open;
    uint8_t audio_ingress_depth;
    uint8_t audio_ingress_high_water;
    uint8_t audio_haptic_queue_depth;
    uint8_t audio_speaker_queue_depth;
    uint8_t audio_speaker_opus_queue_depth;
    uint8_t audio_mic_opus_queue_depth;
    uint16_t audio_mic_ring_bytes;
    uint8_t audio_codec_started;
    uint8_t audio_codec_stage;
    uint8_t audio_codec_encoder_ready;
    uint8_t audio_codec_decoder_ready;
    uint8_t audio_decoder_benchmark_enabled;
    uint8_t audio_speaker_opus_force_mono;
    uint16_t audio_speaker_opus_bandwidth;
    uint32_t audio_speaker_opus_bitrate;
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

typedef struct {
    uint32_t samples;
    uint32_t config_mask;
    uint32_t toc_changes;
    uint32_t length_changes;
    uint32_t stereo_packets;
    uint16_t length_min;
    uint16_t length_max;
    uint16_t last_length;
    uint8_t last_toc;
    uint8_t frame_code_mask;
} m61_opus_packet_shape_t;

typedef struct {
    m61_opus_packet_shape_t speaker;
    m61_opus_packet_shape_t mic;
} m61_opus_packet_audit_t;

void m61_usb_gamepad_get_diag(m61_usb_gamepad_diag_t *diag);
void m61_usb_gamepad_get_opus_packet_audit(m61_opus_packet_audit_t *audit);

#ifdef __cplusplus
}
#endif
