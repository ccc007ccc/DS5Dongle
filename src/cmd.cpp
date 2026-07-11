//
// Created by awalol on 2026/5/4.
//

#include "cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "device/usbd.h"
#include "pico/time.h"
#include "audio.h"
#include "wake.h"

// spk_active (main.cpp) + audio_mic_active() (audio.cpp) are surfaced in the
// 0xf9 feature report so the config UI can display the real gated mic/speaker
// state, reflecting the disable_mic / disable_speaker settings.
extern bool spk_active;
static bool config_save_pending = false;
static bool usb_reconnect_pending = false;
static bool usb_reconnect_waiting = false;
static uint32_t usb_reconnect_deadline_ms = 0;
static uint32_t config_save_requested_ms = 0;
static uint32_t usb_reconnect_requested_ms = 0;
static constexpr uint32_t MAX_AUDIO_DEFERRAL_MS = 2000;

bool is_pico_cmd(uint8_t report_id) {
    if (report_id == 0xf6 ||
        report_id == 0xf7 ||
        report_id == 0xf8 ||
        report_id == 0xf9
    ) {
        return true;
    }
    return false;
}

uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (report_id == 0xf7) {
        printf("[HID] Receive 0xf7 getting config\n");
        if (sizeof(Config_body) > reqlen) {
            printf("[Config] Warning: Config_body overflow\n");
        }
        const auto len = std::min(sizeof(Config_body),static_cast<size_t>(reqlen));
        memcpy(buffer,&get_config(),len);
        return len;
    }
    if (report_id == 0xf8) {
        printf("[HID] Receive 0xf8 getting firmware version\n");
        const auto len = std::min(strlen(PICO_PROGRAM_VERSION_STRING), static_cast<size_t>(reqlen));
        memcpy(buffer, PICO_PROGRAM_VERSION_STRING, len);
        return len;
    }
    if (report_id == 0xf9) {
        // [-128,0]
        int8_t rssi = 0;
        bt_get_signal_strength(&rssi);
        if (reqlen == 0) {
            return 0;
        }
        buffer[0] = rssi;
        // byte 1: real audio gating state, for the config UI to display.
        //   bit7 = valid marker (firmware without this byte leaves it 0)
        //   bit0 = controller mic actually streaming (host opened it AND !disable_mic)
        //   bit1 = controller speaker actually driven (host opened it AND !disable_speaker)
        if (reqlen >= 2) {
            uint8_t flags = 0x80;
            if (audio_mic_active() && !get_config().disable_mic) flags |= 0x01;
            if (spk_active && !get_config().disable_speaker) flags |= 0x02;
            buffer[1] = flags;
            return 2;
        }
#if ENABLE_VERBOSE
        printf("[HID] 0xf9 RSSI=%d raw=0x%02X\n", rssi, buffer[0]);
#endif
        return 1;
    }
    return 0;
}

void pico_cmd_set(uint8_t report_id, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    if (bufsize == 0) {
        return;
    }

    // 0x01 update config in variable
    // 0x02 write config to flash
    // 0x03 reconnect tinyusb device;
    if (buffer[0] == 0x01) {
#if ENABLE_VERBOSE
        printf("[CMD] Enter config set func\n");
#endif
        set_config(buffer + 1, bufsize - 1);
    }
    if (buffer[0] == 0x02) {
        if (!config_save_pending) {
            config_save_requested_ms = to_ms_since_boot(get_absolute_time());
        }
        config_save_pending = true;
    }
    if (buffer[0] == 0x03) {
        if (!usb_reconnect_pending && !usb_reconnect_waiting) {
            usb_reconnect_requested_ms = to_ms_since_boot(get_absolute_time());
            usb_reconnect_pending = true;
        }
    }
}

void pico_cmd_task() {
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (usb_reconnect_waiting) {
        if ((int32_t)(now - usb_reconnect_deadline_ms) >= 0) {
            tud_connect();
            usb_reconnect_waiting = false;
        }
        return;
    }
    const bool audio_active = audio_realtime_active();
    if (config_save_pending &&
        (!audio_active || now - config_save_requested_ms >= MAX_AUDIO_DEFERRAL_MS)) {
        printf("[CMD] Deferred config save\n");
        config_save();
        config_save_pending = false;
        return;
    }
    if (usb_reconnect_pending &&
        (!audio_active || now - usb_reconnect_requested_ms >= MAX_AUDIO_DEFERRAL_MS)) {
        printf("[CMD] Deferred tud reconnect\n");
        wake_note_usb_reconnect();
        tud_disconnect();
        usb_reconnect_pending = false;
        usb_reconnect_waiting = true;
        usb_reconnect_deadline_ms = now + 150U;
    }
}
