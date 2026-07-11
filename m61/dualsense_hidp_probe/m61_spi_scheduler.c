#include "m61_spi_scheduler.h"

#include "dual_chip_spi_proto.h"

#include "FreeRTOS.h"
#include "bflb_core.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "bflb_spi.h"
#include "task.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#ifndef CONFIG_M61_ESP32_SPI_ENABLE
#define CONFIG_M61_ESP32_SPI_ENABLE 0
#endif

#ifndef CONFIG_M61_ESP32_SPI_READY
#define CONFIG_M61_ESP32_SPI_READY 0
#endif

#ifndef CONFIG_M61_ESP32_SPI_SCLK_PIN
#define CONFIG_M61_ESP32_SPI_SCLK_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_MOSI_PIN
#define CONFIG_M61_ESP32_SPI_MOSI_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_MISO_PIN
#define CONFIG_M61_ESP32_SPI_MISO_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_CS_PIN
#define CONFIG_M61_ESP32_SPI_CS_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_FREQ_HZ
#define CONFIG_M61_ESP32_SPI_FREQ_HZ 8000000
#endif

#ifndef CONFIG_M61_ESP32_READY_PIN
#define CONFIG_M61_ESP32_READY_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_IRQ_PIN
#define CONFIG_M61_ESP32_IRQ_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_ACK_POLL_COUNT
#define CONFIG_M61_ESP32_ACK_POLL_COUNT 32
#endif

#define M61_SPI_TRANSACTION_LEN \
    (DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD)
#define M61_SPI_READY_WAIT_MS 20U
#define M61_SPI_SCHED_TASK_STACK_WORDS 1536U
#define M61_SPI_SCHED_TASK_PRIORITY (configMAX_PRIORITIES - 3)
#define M61_SPI_SCHED_FALLBACK_MS 1U
#define M61_SPI_CONTROL_MAX_ATTEMPTS 2U

#define M61_SPI_NOTIFY_RT (1UL << 0)
#define M61_SPI_NOTIFY_STATE31 (1UL << 1)
#define M61_SPI_NOTIFY_STATE32 (1UL << 2)
#define M61_SPI_NOTIFY_CONTROL (1UL << 3)
#define M61_SPI_NOTIFY_IRQ (1UL << 4)
#define M61_SPI_NOTIFY_GENERATION (1UL << 5)

typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[M61_SPI_SCHED_RT_REPORT_MAX];
} m61_spi_rt_slot_t;

typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[M61_SPI_SCHED_STATE31_MAX];
} m61_spi_state31_slot_t;

typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[M61_SPI_SCHED_STATE32_MAX];
} m61_spi_state32_slot_t;

typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t type;
    uint8_t channel;
    uint8_t priority;
    uint8_t attempts;
    uint16_t wire_flags;
    uint16_t wire_seq;
    uint8_t payload[M61_SPI_SCHED_CONTROL_PAYLOAD_MAX];
} m61_spi_control_slot_t;

typedef struct {
    m61_spi_rt_slot_t rt[DS5_SCHED_M61_RT_REPORT_CAPACITY];
    m61_spi_state31_slot_t state31;
    m61_spi_state32_slot_t state32;
    m61_spi_control_slot_t control[DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY];
} m61_spi_typed_storage_t;

typedef struct {
    bool active;
    uint8_t slot_index;
    uint8_t type;
    uint8_t attempts;
    uint16_t seq;
    TickType_t deadline_tick;
} m61_spi_ack_wait_t;

typedef struct {
    bool ready;
    struct bflb_device_s *gpio;
    struct bflb_device_s *spi;
    TaskHandle_t task;
    m61_spi_scheduler_rx_cb_t rx_cb;
    void *rx_cb_ctx;
    uint16_t seq_ctrl;
    uint16_t seq_output;
    uint16_t seq_audio;
    uint32_t publish_sequence;
    uint32_t generation;
    bool poll_requested;
    m61_spi_ack_wait_t ack_wait;
    m61_spi_typed_storage_t store;
    m61_spi_scheduler_stats_t stats;
} m61_spi_scheduler_state_t;

static m61_spi_scheduler_state_t s_scheduler;
uint8_t g_ds5_owner_m61_spi_scheduler;
static uint8_t s_spi_tx_frame[M61_SPI_TRANSACTION_LEN];
static uint8_t s_spi_rx_frame[M61_SPI_TRANSACTION_LEN];

_Static_assert(sizeof(m61_spi_rt_slot_t) == 584U,
               "M61 SPI realtime slot layout drift");
_Static_assert(sizeof(m61_spi_state31_slot_t) == 112U,
               "M61 SPI state31 slot layout drift");
_Static_assert(sizeof(m61_spi_state32_slot_t) == 176U,
               "M61 SPI state32 slot layout drift");
_Static_assert(sizeof(m61_spi_control_slot_t) == 200U,
               "M61 SPI control slot layout drift");
_Static_assert(sizeof(m61_spi_typed_storage_t) == 3056U,
               "M61 SPI typed storage layout drift");
_Static_assert(sizeof(m61_spi_typed_storage_t) <=
                   DS5_SCHED_M61_PLANNED_TYPED_STORAGE_BYTES,
               "M61 SPI typed storage exceeds shared scheduler reservation");

static bool pin_configured(int pin)
{
    return pin >= 0 && pin < GPIO_PIN_MAX;
}

static uint64_t local_time_us(void)
{
    return bflb_mtimer_get_time_us();
}

static uint16_t next_seq(uint8_t channel)
{
    switch (channel) {
    case DS5_DUAL_CHANNEL_AUDIO:
        return s_scheduler.seq_audio++;
    case DS5_DUAL_CHANNEL_OUTPUT:
        return s_scheduler.seq_output++;
    default:
        return s_scheduler.seq_ctrl++;
    }
}

static void notify_task(uint32_t bits)
{
    TaskHandle_t task = s_scheduler.task;

    if (task == NULL) {
        return;
    }
    if (xPortIsInsideInterrupt()) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xTaskNotifyFromISR(task, bits, eSetBits, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    } else {
        xTaskNotify(task, bits, eSetBits);
    }
}

static bool ready_pin_active(void)
{
    if (s_scheduler.gpio == NULL ||
        !pin_configured(CONFIG_M61_ESP32_READY_PIN)) {
        return true;
    }
    return bflb_gpio_read(s_scheduler.gpio,
                          (uint8_t)CONFIG_M61_ESP32_READY_PIN);
}

static bool irq_pin_active(void)
{
    if (s_scheduler.gpio == NULL ||
        !pin_configured(CONFIG_M61_ESP32_IRQ_PIN)) {
        return false;
    }
    return bflb_gpio_read(s_scheduler.gpio,
                          (uint8_t)CONFIG_M61_ESP32_IRQ_PIN);
}

static bool wait_ready_for_exchange(void)
{
    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(M61_SPI_READY_WAIT_MS);

    do {
        if (ready_pin_active()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return ready_pin_active();
}

static void spi_cs_select(void)
{
    bflb_gpio_reset(s_scheduler.gpio,
                    (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);
    bflb_mtimer_delay_us(2);
}

static void spi_cs_deselect(void)
{
    bflb_mtimer_delay_us(2);
    bflb_gpio_set(s_scheduler.gpio,
                  (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);
}

static bool decode_matching_ack(const uint8_t *frame,
                                size_t frame_len,
                                ds5_dual_ack_t *ack)
{
    ds5_dual_spi_header_t header;

    if (frame == NULL || ack == NULL || frame_len < DS5_DUAL_SPI_HEADER_LEN ||
        frame[0] == 0U ||
        !ds5_dual_spi_decode_header(frame, frame_len, &header) ||
        !ds5_dual_spi_validate_frame(frame, frame_len, &header) ||
        (header.flags & DS5_DUAL_FLAG_ACK) == 0U) {
        return false;
    }
    return ds5_dual_ack_decode(frame + DS5_DUAL_SPI_HEADER_LEN,
                               header.length,
                               ack);
}

static int exchange(const uint8_t *frame,
                    size_t frame_len,
                    uint8_t tx_type,
                    uint16_t tx_seq)
{
    ds5_dual_spi_header_t rx_header;
    int err;

    if (!wait_ready_for_exchange()) {
        s_scheduler.stats.ready_timeouts++;
        s_scheduler.stats.last_error = -EAGAIN;
        return -EAGAIN;
    }

    memset(s_spi_tx_frame, 0, sizeof(s_spi_tx_frame));
    memset(s_spi_rx_frame, 0, sizeof(s_spi_rx_frame));
    if (frame != NULL && frame_len > 0U) {
        memcpy(s_spi_tx_frame, frame, frame_len);
    }

    spi_cs_select();
    err = bflb_spi_poll_exchange(s_scheduler.spi,
                                 s_spi_tx_frame,
                                 s_spi_rx_frame,
                                 sizeof(s_spi_tx_frame));
    spi_cs_deselect();
    s_scheduler.stats.transactions++;
    if (frame_len > 0U) {
        s_scheduler.stats.tx_bytes += frame_len;
        s_scheduler.stats.last_tx_type = tx_type;
        s_scheduler.stats.last_tx_seq = tx_seq;
    }
    if (err < 0) {
        s_scheduler.stats.transfer_errors++;
        s_scheduler.stats.last_error = err;
        return err;
    }

    if (s_spi_rx_frame[0] != 0U) {
        if (!ds5_dual_spi_decode_header(s_spi_rx_frame,
                                        sizeof(s_spi_rx_frame),
                                        &rx_header)) {
            s_scheduler.stats.rx_frame_errors++;
        } else if (!ds5_dual_spi_validate_frame(s_spi_rx_frame,
                                                sizeof(s_spi_rx_frame),
                                                &rx_header)) {
            s_scheduler.stats.rx_crc_errors++;
        } else {
            s_scheduler.stats.rx_frames++;
            if (s_scheduler.rx_cb != NULL) {
                s_scheduler.rx_cb(s_spi_rx_frame,
                                  sizeof(s_spi_rx_frame),
                                  s_scheduler.rx_cb_ctx);
            }
        }
    }
    s_scheduler.stats.last_error = 0;
    return 0;
}

static bool ack_status_should_retry(int status)
{
    return status == -ETIMEDOUT || status == -ENOMEM || status == -EAGAIN ||
           status == -EBUSY;
}

static void free_control_slot(uint8_t index)
{
    uintptr_t irq_flags = bflb_irq_save();

    memset(&s_scheduler.store.control[index], 0,
           sizeof(s_scheduler.store.control[index]));
    s_scheduler.store.control[index].control.state = DS5_SCHED_SLOT_FREE;
    bflb_irq_restore(irq_flags);
}

static void complete_ack_wait(int status)
{
    uint8_t index = s_scheduler.ack_wait.slot_index;

    if (status == 0) {
        s_scheduler.stats.control_transmitted++;
    } else if (ack_status_should_retry(status) &&
               s_scheduler.ack_wait.attempts < M61_SPI_CONTROL_MAX_ATTEMPTS) {
        uintptr_t irq_flags = bflb_irq_save();
        m61_spi_control_slot_t *slot = &s_scheduler.store.control[index];

        slot->control.state = DS5_SCHED_SLOT_READY;
        slot->attempts = s_scheduler.ack_wait.attempts;
        bflb_irq_restore(irq_flags);
        s_scheduler.stats.control_retried++;
        memset(&s_scheduler.ack_wait, 0, sizeof(s_scheduler.ack_wait));
        notify_task(M61_SPI_NOTIFY_CONTROL);
        return;
    } else {
        s_scheduler.stats.control_failed++;
        s_scheduler.stats.last_error = status;
    }

    free_control_slot(index);
    memset(&s_scheduler.ack_wait, 0, sizeof(s_scheduler.ack_wait));
}

static void process_ack_from_last_rx(void)
{
    ds5_dual_ack_t ack;

    if (!s_scheduler.ack_wait.active ||
        !decode_matching_ack(s_spi_rx_frame, sizeof(s_spi_rx_frame), &ack) ||
        ack.seq != s_scheduler.ack_wait.seq ||
        ack.type != s_scheduler.ack_wait.type) {
        return;
    }
    complete_ack_wait(ack.status);
}

static int exchange_and_process_ack(const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t type,
                                    uint16_t seq)
{
    int err = exchange(frame, frame_len, type, seq);

    if (err == 0) {
        process_ack_from_last_rx();
    }
    return err;
}

static bool take_oldest_rt(uint8_t *index)
{
    bool found = false;
    uint32_t oldest = 0U;
    uintptr_t irq_flags = bflb_irq_save();

    for (uint8_t i = 0; i < DS5_SCHED_M61_RT_REPORT_CAPACITY; i++) {
        m61_spi_rt_slot_t *slot = &s_scheduler.store.rt[i];

        if (slot->control.state != DS5_SCHED_SLOT_READY) {
            continue;
        }
        if (!found || (int32_t)(slot->meta.sequence - oldest) < 0) {
            found = true;
            oldest = slot->meta.sequence;
            *index = i;
        }
    }
    if (found) {
        s_scheduler.store.rt[*index].control.state = DS5_SCHED_SLOT_SENDING;
    }
    bflb_irq_restore(irq_flags);
    return found;
}

static int send_rt(uint8_t index)
{
    m61_spi_rt_slot_t *slot = &s_scheduler.store.rt[index];
    ds5_dual_spi_header_t header;
    uint8_t frame[M61_SPI_TRANSACTION_LEN];
    uint16_t seq = next_seq(DS5_DUAL_CHANNEL_AUDIO);
    size_t frame_len = ds5_dual_spi_frame_len(slot->meta.length);
    int err;

    ds5_dual_spi_header_init(&header,
                             DS5_DUAL_MSG_BT_TX_AUDIO_RT,
                             DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
                             DS5_DUAL_CHANNEL_AUDIO,
                             DS5_DUAL_PRIORITY_RT,
                             seq,
                             0U,
                             slot->meta.length);
    if (!ds5_dual_spi_finalize_frame(frame, sizeof(frame), &header,
                                     slot->payload)) {
        err = -EINVAL;
    } else {
        err = exchange_and_process_ack(frame, frame_len,
                                       DS5_DUAL_MSG_BT_TX_AUDIO_RT, seq);
    }

    {
        uintptr_t irq_flags = bflb_irq_save();
        slot->control.state = DS5_SCHED_SLOT_FREE;
        bflb_irq_restore(irq_flags);
    }
    if (err == 0) {
        s_scheduler.stats.rt_transmitted++;
    } else {
        s_scheduler.stats.rt_failed++;
    }
    return err;
}

static int send_state31(void)
{
    m61_spi_state31_slot_t snapshot;
    ds5_dual_spi_header_t header;
    uint8_t frame[M61_SPI_TRANSACTION_LEN];
    uint16_t seq;
    size_t frame_len;
    int err;

    {
        uintptr_t irq_flags = bflb_irq_save();
        if (s_scheduler.store.state31.control.state != DS5_SCHED_SLOT_READY) {
            bflb_irq_restore(irq_flags);
            return -ENOENT;
        }
        s_scheduler.store.state31.control.state = DS5_SCHED_SLOT_SENDING;
        memcpy(&snapshot, &s_scheduler.store.state31, sizeof(snapshot));
        bflb_irq_restore(irq_flags);
    }
    seq = next_seq(DS5_DUAL_CHANNEL_OUTPUT);
    frame_len = ds5_dual_spi_frame_len(snapshot.meta.length);
    ds5_dual_spi_header_init(&header, DS5_DUAL_MSG_BT_TX_REPORT,
                             DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
                             DS5_DUAL_CHANNEL_OUTPUT, DS5_DUAL_PRIORITY_HID,
                             seq, 0U, snapshot.meta.length);
    err = ds5_dual_spi_finalize_frame(frame, sizeof(frame), &header,
                                      snapshot.payload) ?
        exchange_and_process_ack(frame, frame_len,
                                 DS5_DUAL_MSG_BT_TX_REPORT, seq) : -EINVAL;
    {
        uintptr_t irq_flags = bflb_irq_save();
        if (s_scheduler.store.state31.meta.sequence == snapshot.meta.sequence) {
            s_scheduler.store.state31.control.state = DS5_SCHED_SLOT_FREE;
        } else {
            s_scheduler.store.state31.control.state = DS5_SCHED_SLOT_READY;
        }
        bflb_irq_restore(irq_flags);
    }
    if (err == 0) {
        s_scheduler.stats.state31_transmitted++;
    }
    return err;
}

static int send_state32(void)
{
    m61_spi_state32_slot_t snapshot;
    ds5_dual_spi_header_t header;
    uint8_t frame[M61_SPI_TRANSACTION_LEN];
    uint16_t seq;
    size_t frame_len;
    int err;

    {
        uintptr_t irq_flags = bflb_irq_save();
        if (s_scheduler.store.state32.control.state != DS5_SCHED_SLOT_READY) {
            bflb_irq_restore(irq_flags);
            return -ENOENT;
        }
        s_scheduler.store.state32.control.state = DS5_SCHED_SLOT_SENDING;
        memcpy(&snapshot, &s_scheduler.store.state32, sizeof(snapshot));
        bflb_irq_restore(irq_flags);
    }
    seq = next_seq(DS5_DUAL_CHANNEL_OUTPUT);
    frame_len = ds5_dual_spi_frame_len(snapshot.meta.length);
    ds5_dual_spi_header_init(&header, DS5_DUAL_MSG_BT_TX_REPORT,
                             DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
                             DS5_DUAL_CHANNEL_OUTPUT, DS5_DUAL_PRIORITY_HID,
                             seq, 0U, snapshot.meta.length);
    err = ds5_dual_spi_finalize_frame(frame, sizeof(frame), &header,
                                      snapshot.payload) ?
        exchange_and_process_ack(frame, frame_len,
                                 DS5_DUAL_MSG_BT_TX_REPORT, seq) : -EINVAL;
    {
        uintptr_t irq_flags = bflb_irq_save();
        if (s_scheduler.store.state32.meta.sequence == snapshot.meta.sequence) {
            s_scheduler.store.state32.control.state = DS5_SCHED_SLOT_FREE;
        } else {
            s_scheduler.store.state32.control.state = DS5_SCHED_SLOT_READY;
        }
        bflb_irq_restore(irq_flags);
    }
    if (err == 0) {
        s_scheduler.stats.state32_transmitted++;
    }
    return err;
}

static bool take_oldest_control(uint8_t *index, bool aged_only)
{
    bool found = false;
    uint32_t oldest = 0U;
    uint64_t now_us = local_time_us();
    uintptr_t irq_flags;

    if (s_scheduler.ack_wait.active) {
        return false;
    }
    irq_flags = bflb_irq_save();
    for (uint8_t i = 0; i < DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY; i++) {
        m61_spi_control_slot_t *slot = &s_scheduler.store.control[i];

        if (slot->control.state != DS5_SCHED_SLOT_READY ||
            (aged_only && now_us - slot->meta.created_us < 2000U)) {
            continue;
        }
        if (!found || (int32_t)(slot->meta.sequence - oldest) < 0) {
            found = true;
            oldest = slot->meta.sequence;
            *index = i;
        }
    }
    if (found) {
        s_scheduler.store.control[*index].control.state = DS5_SCHED_SLOT_SENDING;
    }
    bflb_irq_restore(irq_flags);
    return found;
}

static int send_control(uint8_t index)
{
    m61_spi_control_slot_t *slot = &s_scheduler.store.control[index];
    ds5_dual_spi_header_t header;
    uint8_t frame[M61_SPI_TRANSACTION_LEN];
    uint16_t seq = next_seq(slot->channel);
    size_t frame_len = ds5_dual_spi_frame_len(slot->meta.length);
    bool reliable = (slot->wire_flags & DS5_DUAL_FLAG_RELIABLE) != 0U;
    int err;

    slot->wire_seq = seq;
    slot->attempts++;
    ds5_dual_spi_header_init(&header, slot->type, slot->wire_flags,
                             slot->channel, slot->priority, seq, 0U,
                             slot->meta.length);
    if (!ds5_dual_spi_finalize_frame(frame, sizeof(frame), &header,
                                     slot->payload)) {
        err = -EINVAL;
    } else {
        err = exchange_and_process_ack(frame, frame_len, slot->type, seq);
    }
    if (err < 0) {
        if (reliable && slot->attempts < M61_SPI_CONTROL_MAX_ATTEMPTS) {
            uintptr_t irq_flags = bflb_irq_save();
            slot->control.state = DS5_SCHED_SLOT_READY;
            bflb_irq_restore(irq_flags);
            s_scheduler.stats.control_retried++;
            notify_task(M61_SPI_NOTIFY_CONTROL);
        } else {
            s_scheduler.stats.control_failed++;
            free_control_slot(index);
        }
        return err;
    }
    if (!reliable) {
        s_scheduler.stats.control_transmitted++;
        free_control_slot(index);
        return 0;
    }

    if (slot->meta.generation != s_scheduler.generation ||
        slot->control.state != DS5_SCHED_SLOT_SENDING) {
        free_control_slot(index);
        return -ESTALE;
    }

    s_scheduler.ack_wait.active = true;
    s_scheduler.ack_wait.slot_index = index;
    s_scheduler.ack_wait.type = slot->type;
    s_scheduler.ack_wait.attempts = slot->attempts;
    s_scheduler.ack_wait.seq = seq;
    s_scheduler.ack_wait.deadline_tick = xTaskGetTickCount() +
        pdMS_TO_TICKS(CONFIG_M61_ESP32_ACK_POLL_COUNT > 0 ?
                      CONFIG_M61_ESP32_ACK_POLL_COUNT : 1);
    process_ack_from_last_rx();
    return 0;
}

static bool rt_pending(void)
{
    for (uint8_t i = 0; i < DS5_SCHED_M61_RT_REPORT_CAPACITY; i++) {
        if (s_scheduler.store.rt[i].control.state == DS5_SCHED_SLOT_READY) {
            return true;
        }
    }
    return false;
}

static bool any_work_pending(void)
{
    if (rt_pending() || irq_pin_active() ||
        s_scheduler.store.state31.control.state == DS5_SCHED_SLOT_READY ||
        s_scheduler.store.state32.control.state == DS5_SCHED_SLOT_READY) {
        return true;
    }
    for (uint8_t i = 0; i < DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY; i++) {
        if (s_scheduler.store.control[i].control.state == DS5_SCHED_SLOT_READY) {
            return true;
        }
    }
    return false;
}

static bool take_poll_request(void)
{
    bool requested;
    uintptr_t irq_flags = bflb_irq_save();

    requested = s_scheduler.poll_requested;
    s_scheduler.poll_requested = false;
    bflb_irq_restore(irq_flags);
    return requested;
}

static bool service_one(void)
{
    uint8_t index;

    if (take_oldest_rt(&index)) {
        (void)send_rt(index);
        return true;
    }
    if (take_poll_request() || irq_pin_active()) {
        if (s_scheduler.ack_wait.active) {
            s_scheduler.stats.ack_polls++;
        }
        (void)exchange_and_process_ack(NULL, 0U, 0U, 0U);
        if (s_scheduler.ack_wait.active &&
            (int32_t)(xTaskGetTickCount() -
                      s_scheduler.ack_wait.deadline_tick) >= 0) {
            complete_ack_wait(-ETIMEDOUT);
        }
        return true;
    }
    if (s_scheduler.ack_wait.active &&
        (int32_t)(xTaskGetTickCount() -
                  s_scheduler.ack_wait.deadline_tick) >= 0) {
        complete_ack_wait(-ETIMEDOUT);
        return true;
    }
    if (take_oldest_control(&index, true)) {
        (void)send_control(index);
        return true;
    }
    if (s_scheduler.store.state31.control.state == DS5_SCHED_SLOT_READY) {
        (void)send_state31();
        return true;
    }
    if (s_scheduler.store.state32.control.state == DS5_SCHED_SLOT_READY) {
        (void)send_state32();
        return true;
    }
    if (take_oldest_control(&index, false)) {
        (void)send_control(index);
        return true;
    }
    return false;
}

static void scheduler_task(void *arg)
{
    uint32_t notified;

    (void)arg;
    while (1) {
        uint32_t transactions = 0U;

        while (transactions < 4U && service_one()) {
            transactions++;
        }
        if (transactions == 4U) {
            vTaskDelay(1U);
            if (any_work_pending()) {
                continue;
            }
        }
        if (xTaskNotifyWait(0U, UINT32_MAX, &notified,
                            pdMS_TO_TICKS(M61_SPI_SCHED_FALLBACK_MS)) != pdTRUE) {
            s_scheduler.stats.fallback_wakes++;
            if (!pin_configured(CONFIG_M61_ESP32_IRQ_PIN) &&
                s_scheduler.ack_wait.active) {
                s_scheduler.poll_requested = true;
            }
        }
        (void)notified;
    }
}

static void irq_gpio_callback(uint8_t pin)
{
    if (pin != (uint8_t)CONFIG_M61_ESP32_IRQ_PIN) {
        return;
    }
    s_scheduler.poll_requested = true;
    s_scheduler.stats.irq_notifications++;
    notify_task(M61_SPI_NOTIFY_IRQ);
}

static int init_hardware(void)
{
    struct bflb_spi_config_s spi_cfg = {
        .freq = CONFIG_M61_ESP32_SPI_FREQ_HZ,
        .role = SPI_ROLE_MASTER,
        .mode = SPI_MODE0,
        .data_width = SPI_DATA_WIDTH_8BIT,
        .bit_order = SPI_BIT_MSB,
        .byte_order = SPI_BYTE_LSB,
        .tx_fifo_threshold = 0,
        .rx_fifo_threshold = 0,
    };

    if (!CONFIG_M61_ESP32_SPI_ENABLE || !CONFIG_M61_ESP32_SPI_READY) {
        return -ENOTCONN;
    }
    if (!pin_configured(CONFIG_M61_ESP32_SPI_SCLK_PIN) ||
        !pin_configured(CONFIG_M61_ESP32_SPI_MOSI_PIN) ||
        !pin_configured(CONFIG_M61_ESP32_SPI_MISO_PIN) ||
        !pin_configured(CONFIG_M61_ESP32_SPI_CS_PIN)) {
        return -EINVAL;
    }
    s_scheduler.gpio = bflb_device_get_by_name("gpio");
    s_scheduler.spi = bflb_device_get_by_name("spi0");
    if (s_scheduler.gpio == NULL || s_scheduler.spi == NULL) {
        return -ENODEV;
    }

    bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_SCLK_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT |
                       GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_MOSI_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT |
                       GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_MISO_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT |
                       GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_set(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);

    if (pin_configured(CONFIG_M61_ESP32_READY_PIN)) {
        bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_READY_PIN,
                       GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
    }
    if (pin_configured(CONFIG_M61_ESP32_IRQ_PIN)) {
        bflb_gpio_init(s_scheduler.gpio, (uint8_t)CONFIG_M61_ESP32_IRQ_PIN,
                       GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
        bflb_irq_disable(s_scheduler.gpio->irq_num);
        bflb_gpio_int_init(s_scheduler.gpio,
                           (uint8_t)CONFIG_M61_ESP32_IRQ_PIN,
                           GPIO_INT_TRIG_MODE_SYNC_RISING_EDGE);
        bflb_gpio_irq_attach((uint8_t)CONFIG_M61_ESP32_IRQ_PIN,
                             irq_gpio_callback);
        bflb_irq_enable(s_scheduler.gpio->irq_num);
    }

    bflb_spi_init(s_scheduler.spi, &spi_cfg);
    bflb_spi_feature_control(s_scheduler.spi, SPI_CMD_SET_CS_INTERVAL, 0);
    return 0;
}

int m61_spi_scheduler_init(m61_spi_scheduler_rx_cb_t rx_cb, void *rx_cb_ctx)
{
    int err;

    g_ds5_owner_m61_spi_scheduler = 1U;
    memset(&s_scheduler, 0, sizeof(s_scheduler));
    s_scheduler.rx_cb = rx_cb;
    s_scheduler.rx_cb_ctx = rx_cb_ctx;
    s_scheduler.generation = 1U;
    s_scheduler.stats.generation = 1U;
    err = init_hardware();
    if (err < 0) {
        s_scheduler.stats.last_error = err;
        return err;
    }
    if (xTaskCreate(scheduler_task, "ds5_spi_sched",
                    M61_SPI_SCHED_TASK_STACK_WORDS, NULL,
                    M61_SPI_SCHED_TASK_PRIORITY, &s_scheduler.task) != pdPASS) {
        s_scheduler.stats.last_error = -ENOMEM;
        return -ENOMEM;
    }
    s_scheduler.ready = true;
    if (irq_pin_active()) {
        notify_task(M61_SPI_NOTIFY_IRQ);
    }
    return 0;
}

bool m61_spi_scheduler_ready(void)
{
    return s_scheduler.ready;
}

int m61_spi_submit_rt_report(const uint8_t *report,
                             size_t len,
                             uint32_t generation,
                             uint64_t created_us)
{
    int selected = -1;
    int oldest_ready = -1;
    uint32_t oldest_sequence = 0U;
    uint32_t sequence;
    uintptr_t irq_flags;

    if (report == NULL || len != M61_SPI_SCHED_RT_REPORT_MAX ||
        report[0] != 0x39U) {
        return -EINVAL;
    }
    irq_flags = bflb_irq_save();
    if (generation != s_scheduler.generation) {
        bflb_irq_restore(irq_flags);
        return -ESTALE;
    }
    for (uint8_t i = 0; i < DS5_SCHED_M61_RT_REPORT_CAPACITY; i++) {
        m61_spi_rt_slot_t *slot = &s_scheduler.store.rt[i];

        if (slot->control.state == DS5_SCHED_SLOT_FREE) {
            selected = i;
            break;
        }
        if (slot->control.state == DS5_SCHED_SLOT_READY &&
            (oldest_ready < 0 ||
             (int32_t)(slot->meta.sequence - oldest_sequence) < 0)) {
            oldest_ready = i;
            oldest_sequence = slot->meta.sequence;
        }
    }
    if (selected < 0) {
        selected = oldest_ready;
        if (selected >= 0) {
            s_scheduler.stats.rt_replaced++;
        }
    }
    if (selected < 0) {
        bflb_irq_restore(irq_flags);
        return -EAGAIN;
    }
    sequence = ++s_scheduler.publish_sequence;
    s_scheduler.store.rt[selected].control.state = DS5_SCHED_SLOT_WRITING;
    s_scheduler.store.rt[selected].control.version++;
    bflb_irq_restore(irq_flags);

    memcpy(s_scheduler.store.rt[selected].payload, report, len);
    ds5_sched_meta_init(&s_scheduler.store.rt[selected].meta,
                        created_us, generation, sequence, (uint16_t)len,
                        DS5_SCHED_RT_ACTUATION, 0U);

    irq_flags = bflb_irq_save();
    if (generation == s_scheduler.generation) {
        s_scheduler.store.rt[selected].control.state = DS5_SCHED_SLOT_READY;
        s_scheduler.stats.rt_accepted++;
    } else {
        s_scheduler.store.rt[selected].control.state = DS5_SCHED_SLOT_FREE;
    }
    bflb_irq_restore(irq_flags);
    if (generation != s_scheduler.generation) {
        return -ESTALE;
    }
    notify_task(M61_SPI_NOTIFY_RT);
    return 0;
}

static int publish_state(void *slot_ptr,
                         size_t slot_size,
                         size_t payload_offset,
                         size_t payload_capacity,
                         ds5_sched_class_t sched_class,
                         uint32_t notify_bit,
                         const uint8_t *report,
                         size_t len,
                         uint32_t generation,
                         uint64_t created_us)
{
    ds5_sched_meta_t *meta = slot_ptr;
    ds5_sched_slot_control_t *control =
        (ds5_sched_slot_control_t *)((uint8_t *)slot_ptr + sizeof(*meta));
    uint8_t *payload = (uint8_t *)slot_ptr + payload_offset;
    uintptr_t irq_flags;

    (void)slot_size;
    if (report == NULL || len != payload_capacity ||
        (sched_class == DS5_SCHED_LATEST_STATE_31 && report[0] != 0x31U) ||
        (sched_class == DS5_SCHED_LATEST_STATE_32 && report[0] != 0x32U)) {
        return -EINVAL;
    }
    irq_flags = bflb_irq_save();
    if (generation != s_scheduler.generation) {
        bflb_irq_restore(irq_flags);
        return -ESTALE;
    }
    if (control->state == DS5_SCHED_SLOT_READY ||
        control->state == DS5_SCHED_SLOT_SENDING) {
        if (sched_class == DS5_SCHED_LATEST_STATE_31) {
            s_scheduler.stats.state31_coalesced++;
        } else {
            s_scheduler.stats.state32_coalesced++;
        }
    }
    memcpy(payload, report, len);
    ds5_sched_meta_init(meta, created_us, generation,
                        ++s_scheduler.publish_sequence, (uint16_t)len,
                        sched_class, 0U);
    control->version++;
    control->state = DS5_SCHED_SLOT_READY;
    if (sched_class == DS5_SCHED_LATEST_STATE_31) {
        s_scheduler.stats.state31_accepted++;
    } else {
        s_scheduler.stats.state32_accepted++;
    }
    bflb_irq_restore(irq_flags);
    notify_task(notify_bit);
    return 0;
}

int m61_spi_publish_state31(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us)
{
    return publish_state(&s_scheduler.store.state31,
                         sizeof(s_scheduler.store.state31),
                         offsetof(m61_spi_state31_slot_t, payload),
                         M61_SPI_SCHED_STATE31_MAX,
                         DS5_SCHED_LATEST_STATE_31,
                         M61_SPI_NOTIFY_STATE31,
                         report, len, generation, created_us);
}

int m61_spi_publish_state32(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us)
{
    return publish_state(&s_scheduler.store.state32,
                         sizeof(s_scheduler.store.state32),
                         offsetof(m61_spi_state32_slot_t, payload),
                         M61_SPI_SCHED_STATE32_MAX,
                         DS5_SCHED_LATEST_STATE_32,
                         M61_SPI_NOTIFY_STATE32,
                         report, len, generation, created_us);
}

int m61_spi_submit_control(const m61_spi_control_request_t *request)
{
    int selected = -1;
    uintptr_t irq_flags;

    if (request == NULL ||
        (request->payload == NULL && request->payload_len != 0U) ||
        request->payload_len > M61_SPI_SCHED_CONTROL_PAYLOAD_MAX) {
        return -EINVAL;
    }
    irq_flags = bflb_irq_save();
    if (request->generation != s_scheduler.generation) {
        bflb_irq_restore(irq_flags);
        return -ESTALE;
    }
    for (uint8_t i = 0; i < DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY; i++) {
        if (s_scheduler.store.control[i].control.state == DS5_SCHED_SLOT_FREE) {
            selected = i;
            break;
        }
    }
    if (selected < 0) {
        s_scheduler.stats.control_rejected++;
        bflb_irq_restore(irq_flags);
        return -EAGAIN;
    }
    s_scheduler.store.control[selected].control.state = DS5_SCHED_SLOT_WRITING;
    s_scheduler.store.control[selected].control.version++;
    if (request->payload_len > 0U) {
        memcpy(s_scheduler.store.control[selected].payload,
               request->payload, request->payload_len);
    }
    ds5_sched_meta_init(&s_scheduler.store.control[selected].meta,
                        request->created_us, request->generation,
                        ++s_scheduler.publish_sequence,
                        request->payload_len, DS5_SCHED_RELIABLE_CONTROL, 0U);
    s_scheduler.store.control[selected].type = request->type;
    s_scheduler.store.control[selected].channel = request->channel;
    s_scheduler.store.control[selected].priority = request->priority;
    s_scheduler.store.control[selected].wire_flags = request->flags;
    s_scheduler.store.control[selected].attempts = 0U;
    s_scheduler.store.control[selected].control.state = DS5_SCHED_SLOT_READY;
    s_scheduler.stats.control_accepted++;
    bflb_irq_restore(irq_flags);
    notify_task(M61_SPI_NOTIFY_CONTROL);
    return 0;
}

void m61_spi_scheduler_set_generation(uint32_t generation)
{
    uintptr_t irq_flags;

    if (generation == 0U) {
        generation = 1U;
    }
    irq_flags = bflb_irq_save();
    if (generation != s_scheduler.generation) {
        for (uint8_t i = 0; i < DS5_SCHED_M61_RT_REPORT_CAPACITY; i++) {
            ds5_sched_slot_control_t *control =
                &s_scheduler.store.rt[i].control;

            if (control->state == DS5_SCHED_SLOT_READY) {
                s_scheduler.stats.rt_stale++;
            }
            control->version++;
            control->state =
                (control->state == DS5_SCHED_SLOT_WRITING ||
                 control->state == DS5_SCHED_SLOT_SENDING) ?
                    DS5_SCHED_SLOT_EVICTED : DS5_SCHED_SLOT_FREE;
        }
        s_scheduler.store.state31.control.version++;
        s_scheduler.store.state31.control.state = DS5_SCHED_SLOT_FREE;
        s_scheduler.store.state32.control.version++;
        s_scheduler.store.state32.control.state = DS5_SCHED_SLOT_FREE;
        for (uint8_t i = 0;
             i < DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY; i++) {
            ds5_sched_slot_control_t *control =
                &s_scheduler.store.control[i].control;

            control->version++;
            control->state =
                (control->state == DS5_SCHED_SLOT_WRITING ||
                 control->state == DS5_SCHED_SLOT_SENDING) ?
                    DS5_SCHED_SLOT_EVICTED : DS5_SCHED_SLOT_FREE;
        }
        if (s_scheduler.ack_wait.active) {
            s_scheduler.store.control[s_scheduler.ack_wait.slot_index]
                .control.state = DS5_SCHED_SLOT_FREE;
        }
        memset(&s_scheduler.ack_wait, 0, sizeof(s_scheduler.ack_wait));
        s_scheduler.generation = generation;
        s_scheduler.stats.generation = generation;
        s_scheduler.stats.generation_resets++;
    }
    bflb_irq_restore(irq_flags);
    notify_task(M61_SPI_NOTIFY_GENERATION);
}

void m61_spi_scheduler_notify_irq(void)
{
    uintptr_t irq_flags = bflb_irq_save();
    s_scheduler.poll_requested = true;
    bflb_irq_restore(irq_flags);
    notify_task(M61_SPI_NOTIFY_IRQ);
}

void m61_spi_scheduler_get_stats(m61_spi_scheduler_stats_t *stats)
{
    uintptr_t irq_flags;

    if (stats == NULL) {
        return;
    }
    irq_flags = bflb_irq_save();
    *stats = s_scheduler.stats;
    bflb_irq_restore(irq_flags);
}

void m61_spi_scheduler_reset_stats(void)
{
    uintptr_t irq_flags = bflb_irq_save();
    uint32_t generation = s_scheduler.generation;

    memset(&s_scheduler.stats, 0, sizeof(s_scheduler.stats));
    s_scheduler.stats.generation = generation;
    bflb_irq_restore(irq_flags);
}
