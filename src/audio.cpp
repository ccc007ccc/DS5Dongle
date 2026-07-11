//
// Created by awalol on 2026/3/5.
//

/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "audio.h"
#include "audio_epoch_pairer.h"
#include "bt.h"
#if ENABLE_DEBUG
#include "debug.h"
#endif
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/util/queue.h"
#include "config.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define SPEAKER_OPUS_SIZE 200
#define REPORT_SIZE       547
#define REPORT_ID         0x39
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48
#define MIC_CHANNELS      1
#define MIC_FRAMES        480
#define MIC_OPUS_SIZE     71   // bytes per opus-encoded mic frame from the DualSense

using std::clamp;
using std::max;

extern uint8_t packetCounter;
static bool plug_headset = false;
static bool mic_active = false; // host has opened the mic IN interface (alt != 0)
static uint32_t last_audio_out_ms = 0;
static volatile uint32_t audio_generation = 1;
alignas(8) static uint32_t audio_core1_stack[7000];
queue_t mic_fifo;
queue_t mic_decode_fifo;
queue_t audio_epoch_raw_fifo;
queue_t audio_epoch_complete_fifo;

struct audio_epoch_raw {
    uint32_t generation;
    uint32_t epoch;
    float speaker[512 * 2];
    uint8_t haptics[SAMPLE_SIZE];
    bool speaker_enabled;
};

static_assert(sizeof(audio_epoch_raw) == 4172,
              "audio epoch raw storage budget drift");
static_assert(sizeof(AudioEpochComplete) == 276,
              "audio epoch complete storage budget drift");

struct mic_element {
    uint8_t data[MIC_OPUS_SIZE];
};

struct mic_decode_element {
    int16_t data[MIC_FRAMES * MIC_CHANNELS];
    uint16_t len;
};

static void publish_complete_epoch(const AudioEpochComplete &epoch) {
    if (queue_is_full(&audio_epoch_complete_fifo)) {
        queue_try_remove(&audio_epoch_complete_fifo, nullptr);
    }
    queue_try_add(&audio_epoch_complete_fifo, &epoch);
}

void set_headset(bool state) {
    plug_headset = state;
}

// Called from tud_audio_set_itf_cb when the host opens/closes the mic IN
// interface. Gates controller-mic streaming so it only runs while recording.
void set_mic_active(bool active) {
    mic_active = active;
    update_mic_status();
}

bool audio_mic_active() {
    return mic_active;
}

bool audio_realtime_active() {
    return to_ms_since_boot(get_absolute_time()) - last_audio_out_ms < 100U;
}

void audio_reset_realtime() {
    audio_generation++;
}

void update_mic_status() {
    uint8_t pkt[142]{};
    pkt[0] = 0x32;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 1;
    pkt[4] = (mic_active && !get_config().disable_mic) ? 0b00000011 : 0b00000010;
    bt_write(pkt, sizeof(pkt));
}

void __not_in_flash_func(audio_bt_task)() {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && !cfg.disable_mic;
#if !DISABLE_SPEAKER_PROC
    const bool speaker_enabled = !cfg.disable_speaker;
#endif

    static AudioEpochPairer pairer;
    AudioEpochComplete first{};
    AudioEpochComplete current{};
    if (!queue_try_remove(&audio_epoch_complete_fifo, &current)) return;
    if (current.generation != audio_generation) {
        pairer.reset();
        return;
    }
    AudioEpochComplete second{};
    if (!pairer.push(current, &first, &second)) return;

    uint8_t pkt[REPORT_SIZE]{};
    pkt[0] = REPORT_ID;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 6;
    pkt[4] = mic_enabled ? 0b01111111 : 0b01111110;
    // byte 4 研究
    // bit 6 是必须的
    // 其余 bit 每多设置一个为0，就需要将pkt[3] - 1，然后将下面这些缩短一个字节的数据。
    // 最终实测，可以只保留一个 buf_len + packetCounter
    // pkt 5-7 的注释是根据 Nielk1 采样到的数据进行猜测。但是实际上修改还是发现有任何效果
    const auto buf_len = cfg.audio_buffer_length;
    pkt[5] = buf_len; // VolumeHeadphones - guess but no work
    pkt[6] = buf_len; // VolumeMic - guess but no work
    pkt[7] = buf_len; // VolumeSpeaker - guess but no work
    pkt[8] = buf_len; // AudioBufferLength
    pkt[9] = packetCounter += 2;
    pkt[10] = 0x12 | 1 << 6 | 1 << 7;
    pkt[11] = SAMPLE_SIZE;
    memcpy(pkt + 12, first.haptics.data(), SAMPLE_SIZE);
    memcpy(pkt + 12 + SAMPLE_SIZE, second.haptics.data(), SAMPLE_SIZE);
#if !DISABLE_SPEAKER_PROC
    if (speaker_enabled && first.speaker_enabled) {
        pkt[140] = (plug_headset ? 0x16 : 0x13) | 1 << 6 | 1 << 7;
        pkt[141] = SPEAKER_OPUS_SIZE;
        memcpy(pkt + 142, first.speaker_opus.data(), SPEAKER_OPUS_SIZE);
        memcpy(pkt + 142 + SPEAKER_OPUS_SIZE, second.speaker_opus.data(),
               SPEAKER_OPUS_SIZE);
    }
#endif
    bt_write(pkt, sizeof(pkt));
}

void __not_in_flash_func(audio_loop)() {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && !cfg.disable_mic;
    const bool speaker_enabled = !cfg.disable_speaker;

    // Mic playback: drain decoded mic PCM into the USB IN endpoint
    static mic_decode_element mic_pb{};
    if (queue_try_remove(&mic_decode_fifo, &mic_pb)) {
        if (mic_enabled) {
            // The controller mic is mono, but the USB descriptor presents a 2-channel
            // mic (matching the real DS5) so Windows doesn't conflict with its cached
            // DS5 audio format. Pack each mono int16 sample as L/R in one 32-bit word.
            static uint32_t mic_stereo[MIC_FRAMES];
            const int mono_samples = mic_pb.len / 2;
            for (int i = 0; i < mono_samples; i++) {
                const uint16_t sample = static_cast<uint16_t>(mic_pb.data[i]);
                mic_stereo[i] = static_cast<uint32_t>(sample) | (static_cast<uint32_t>(sample) << 16);
            }
            const uint16_t stereo_len = (uint16_t) (mono_samples * sizeof(uint32_t));
            uint16_t written = tud_audio_write(mic_stereo, stereo_len);
            if (written != stereo_len) {
                // Gated behind ENABLE_VERBOSE: when the host has not opened the mic
                // interface (the common case -- most games never do) tud_audio_write
                // short-writes every frame, so an unconditional log would flood
                // core0's hot path with the newlib formatting chain.
#if ENABLE_VERBOSE
                printf("[Audio] Warning: USB mic FIFO wrote %u/%u bytes\n", written, stereo_len);
#endif
            }
        }
    }

    audio_bt_task();

    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }
    last_audio_out_ms = to_ms_since_boot(get_absolute_time());

    static audio_epoch_raw epoch{};
    static uint16_t epoch_frames = 0;
    static uint32_t next_epoch = 0;
    static uint32_t observed_generation = audio_generation;
    static int32_t haptics_sum_l = 0;
    static int32_t haptics_sum_r = 0;
    if (observed_generation != audio_generation) {
        epoch = {};
        epoch_frames = 0;
        next_epoch = 0;
        haptics_sum_l = 0;
        haptics_sum_r = 0;
        observed_generation = audio_generation;
    }
    const float haptics_gain = cfg.haptics_gain;
    for (int i = 0; i < frames; i++) {
#if !DISABLE_SPEAKER_PROC
        epoch.speaker[epoch_frames * 2U] =
            speaker_enabled ? raw[i * INPUT_CHANNELS] / 32768.0f : 0.0f;
        epoch.speaker[epoch_frames * 2U + 1U] =
            speaker_enabled ? raw[i * INPUT_CHANNELS + 1] / 32768.0f : 0.0f;
#endif
        haptics_sum_l += raw[i * INPUT_CHANNELS + 2];
        haptics_sum_r += raw[i * INPUT_CHANNELS + 3];
        if ((epoch_frames % 16U) == 15U) {
            const uint16_t haptic_index = (epoch_frames / 16U) * 2U;
            const int val_l = static_cast<int>((haptics_sum_l / 16.0f) /
                                               32768.0f * haptics_gain * 127.0f);
            const int val_r = static_cast<int>((haptics_sum_r / 16.0f) /
                                               32768.0f * haptics_gain * 127.0f);
            epoch.haptics[haptic_index] =
                static_cast<uint8_t>(static_cast<int8_t>(clamp(val_l, -128, 127)));
            epoch.haptics[haptic_index + 1U] =
                static_cast<uint8_t>(static_cast<int8_t>(clamp(val_r, -128, 127)));
            haptics_sum_l = 0;
            haptics_sum_r = 0;
        }
        epoch_frames++;
        if (epoch_frames == 512U) {
            epoch.epoch = next_epoch++;
            epoch.generation = observed_generation;
            epoch.speaker_enabled = speaker_enabled;
#if DISABLE_SPEAKER_PROC
            AudioEpochComplete complete{};
            complete.generation = epoch.generation;
            complete.epoch = epoch.epoch;
            complete.speaker_enabled = false;
            memcpy(complete.haptics.data(), epoch.haptics, SAMPLE_SIZE);
            publish_complete_epoch(complete);
#else
            if (queue_is_full(&audio_epoch_raw_fifo)) {
                queue_try_remove(&audio_epoch_raw_fifo, nullptr);
            }
            queue_try_add(&audio_epoch_raw_fifo, &epoch);
#endif
            epoch = {};
            epoch_frames = 0;
        }
    }
}

void audio_init() {
    queue_init(&audio_epoch_raw_fifo, sizeof(audio_epoch_raw), 1);
    queue_init(&audio_epoch_complete_fifo, sizeof(AudioEpochComplete), 4);
    // Mic queues are read from audio_loop on core0 every iteration, so they
    // must exist regardless of the speaker-proc build flag.
    queue_init(&mic_fifo, sizeof(mic_element), 2);
    queue_init(&mic_decode_fifo, sizeof(mic_decode_element), 2);
#if !DISABLE_SPEAKER_PROC
#if ENABLE_DEBUG
    // 通常 stack 最大使用 25836 bytes 即 stack[6459]
    debug_fill_core1_stack_watermark(audio_core1_stack,
                                     sizeof(audio_core1_stack) / sizeof(audio_core1_stack[0]));
#endif
    multicore_launch_core1_with_stack(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
#endif
}

static OpusEncoder *encoder;
static OpusDecoder *decoder; // mic decoder
static WDL_Resampler resampler_audio;

// Speaker path: a complete USB audio epoch from core0 is resampled and encoded,
// then returned with its matching haptics block as one typed epoch.
static void __not_in_flash_func(speaker_proc)() {
    static audio_epoch_raw raw_epoch{};
    if (!queue_try_remove(&audio_epoch_raw_fifo, &raw_epoch)) {
        return;
    }
    AudioEpochComplete complete{};
    complete.epoch = raw_epoch.epoch;
    complete.generation = raw_epoch.generation;
    complete.speaker_enabled = raw_epoch.speaker_enabled &&
                               !get_config().disable_speaker;
    memcpy(complete.haptics.data(), raw_epoch.haptics, SAMPLE_SIZE);
    if (!complete.speaker_enabled) {
        if (complete.generation != audio_generation) return;
        publish_complete_epoch(complete);
        return;
    }
    // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
    WDL_ResampleSample *in_buf;
    int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
    for (int i = 0; i < nframes * 2; i++) {
        in_buf[i] = raw_epoch.speaker[i];
    }
    static WDL_ResampleSample out_buf[480 * 2];
    resampler_audio.ResampleOut(out_buf, nframes, 480, 2);

    static uint8_t out[SPEAKER_OPUS_SIZE];
    const int encoded_len = opus_encode_float(encoder, out_buf, 480, out, sizeof(out));
    if (encoded_len <= 0) {
#if ENABLE_VERBOSE
        printf("[Audio] OpusEncoder encode failed: %d\n", encoded_len);
#endif
        return;
    }

    memcpy(complete.speaker_opus.data(), out, encoded_len);
    if (encoded_len < (int) sizeof(complete.speaker_opus)) {
        memset(complete.speaker_opus.data() + encoded_len, 0,
               complete.speaker_opus.size() - encoded_len);
    }
    if (complete.generation == audio_generation) {
        publish_complete_epoch(complete);
    }
}

// Mic path: opus packets from the controller (core0 mic_fifo) -> opus decode ->
// PCM into mic_decode_fifo for audio_loop to push to the USB IN endpoint.
static void __not_in_flash_func(mic_proc)() {
    static mic_element mic_packet{};
    if (!queue_try_remove(&mic_fifo, &mic_packet)) {
        return;
    }
    if (!mic_active || get_config().disable_mic) {
        return;
    }
    static mic_decode_element decode_element{};
    auto decoded_samples = opus_decode(decoder, mic_packet.data, MIC_OPUS_SIZE, decode_element.data, MIC_FRAMES, false);
    if (decoded_samples <= 0) {
        // Gated behind ENABLE_VERBOSE: printf pulls the newlib formatting chain
        // (flash) onto core1's path. Release builds compile it out so core1's
        // audio loop stays fully RAM-resident (no XIP fetches on this core).
#if ENABLE_VERBOSE
        printf("[Audio] OpusDecoder decode failed: %d\n", decoded_samples);
#endif
        return;
    }
    decode_element.len = decoded_samples * MIC_CHANNELS * sizeof(int16_t);
    if (queue_is_full(&mic_decode_fifo)) {
        queue_try_remove(&mic_decode_fifo, NULL);
    }
    queue_try_add(&mic_decode_fifo, &decode_element);
}

void __not_in_flash_func(core1_entry)() {
    // Register core1 as a flash-safe victim so core0's flash_safe_execute() really
    // parks this core while flash is accessed, instead of letting it fault on XIP.
    // Used by config_save() (flash erase/program) and the BOOTSEL poll (which briefly
    // floats QSPI CSn) - the latter makes polling BOOTSEL safe while audio streams on
    // core1. Requires PICO_FLASH_ASSUME_CORE1_SAFE=0.
    flash_safe_execute_core_init();
    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_AUDIO, &error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    resampler_audio.SetMode(true, 0, false);
    resampler_audio.SetRates(51200, 48000);
    resampler_audio.SetFeedMode(true);
    resampler_audio.Prealloc(2, 512, 480);
    decoder = opus_decoder_create(48000, MIC_CHANNELS, &error);
    if (error != 0) {
        printf("[Audio] OpusDecoder create failed\n");
    }

    while (true) {
        speaker_proc();
        mic_proc();
    }
}

// data points at the opus mic payload, len is the bytes available there.
// In RAM (consistent with the BT-receive path) and validates len so a short
// or malformed report can't over-read past the packet buffer.
void __not_in_flash_func(mic_add_queue)(uint8_t *data, uint16_t len) {
    if (!mic_active || get_config().disable_mic) return;
    if (len < MIC_OPUS_SIZE) return;
    static mic_element mic_packet{};
    memcpy(mic_packet.data, data, MIC_OPUS_SIZE);
    if (queue_is_full(&mic_fifo)) {
        queue_try_remove(&mic_fifo, NULL);
    }
    queue_try_add(&mic_fifo, &mic_packet);
}
