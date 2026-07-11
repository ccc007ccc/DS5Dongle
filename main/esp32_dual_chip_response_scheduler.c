#include "esp32_dual_chip_response_scheduler.h"

#include "dual_chip_spi_proto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

#define RESPONSE_CONTROL_CAPACITY 8U
#define RESPONSE_FEATURE_CAPACITY 8U
#define RESPONSE_INPUT_CAPACITY 4U
#define RESPONSE_MIC_CAPACITY 4U
#define RESPONSE_CONTROL_MAX 160U
#define RESPONSE_FEATURE_MAX 160U
#define RESPONSE_INPUT_MAX 80U
#define RESPONSE_MIC_MAX 71U
#define RESPONSE_LINK_MAX 56U
#define RESPONSE_STATUS_MAX 64U
#define RESPONSE_TELEMETRY_MAX 512U

typedef enum {
    RESPONSE_STORE_NONE = 0,
    RESPONSE_STORE_CONTROL,
    RESPONSE_STORE_FEATURE,
    RESPONSE_STORE_INPUT,
    RESPONSE_STORE_MIC,
    RESPONSE_STORE_LINK,
    RESPONSE_STORE_STATUS,
    RESPONSE_STORE_TELEMETRY,
} response_store_class_t;

typedef struct {
    ds5_sched_meta_t meta;
    uint32_t version;
    uint16_t wire_flags;
    uint16_t wire_sequence;
    uint8_t state;
    uint8_t type;
    uint8_t channel;
    uint8_t priority;
} response_header_t;

#define DEFINE_RESPONSE_SLOT(name, payload_size) \
    typedef struct {                               \
        response_header_t header;                  \
        uint8_t payload[payload_size];              \
    } name

DEFINE_RESPONSE_SLOT(response_control_slot_t, RESPONSE_CONTROL_MAX);
DEFINE_RESPONSE_SLOT(response_feature_slot_t, RESPONSE_FEATURE_MAX);
DEFINE_RESPONSE_SLOT(response_input_slot_t, RESPONSE_INPUT_MAX);
DEFINE_RESPONSE_SLOT(response_mic_slot_t, RESPONSE_MIC_MAX);
DEFINE_RESPONSE_SLOT(response_link_slot_t, RESPONSE_LINK_MAX);
DEFINE_RESPONSE_SLOT(response_status_slot_t, RESPONSE_STATUS_MAX);
DEFINE_RESPONSE_SLOT(response_telemetry_slot_t, RESPONSE_TELEMETRY_MAX);

typedef struct {
    response_control_slot_t control[RESPONSE_CONTROL_CAPACITY];
    response_feature_slot_t feature[RESPONSE_FEATURE_CAPACITY];
    response_input_slot_t input[RESPONSE_INPUT_CAPACITY];
    response_mic_slot_t mic[RESPONSE_MIC_CAPACITY];
    response_link_slot_t link;
    response_status_slot_t status;
    response_telemetry_slot_t telemetry;
    ds5_sched_metrics_t metrics;
} esp32_dual_response_storage_t;

esp32_dual_response_storage_t g_ds5_response_scheduler_storage;

_Static_assert(RESPONSE_CONTROL_CAPACITY ==
                   DS5_SCHED_ESP_RELIABLE_CONTROL_CAPACITY,
               "response control capacity drift");
_Static_assert(RESPONSE_FEATURE_CAPACITY ==
                   DS5_SCHED_ESP_FEATURE_RESPONSE_CAPACITY,
               "response feature capacity drift");
_Static_assert(RESPONSE_INPUT_CAPACITY ==
                   DS5_SCHED_ESP_INPUT_RESPONSE_CAPACITY,
               "response input capacity drift");
_Static_assert(RESPONSE_MIC_CAPACITY ==
                   DS5_SCHED_ESP_MIC_RESPONSE_CAPACITY,
               "response mic capacity drift");
_Static_assert(sizeof(esp32_dual_response_storage_t) <= 7200U,
               "ESP response scheduler exceeds its typed storage budget");

static portMUX_TYPE s_response_mux = portMUX_INITIALIZER_UNLOCKED;
static esp32_dual_response_irq_cb_t s_irq_cb;
static void *s_irq_ctx;
static uint32_t s_publish_sequence;
static uint32_t s_generation;
#define RESPONSE_CONTROL_MAX_BURST 2U
#define RESPONSE_NONCONTROL_SLOTS 9U

static uint8_t s_control_burst;
static uint8_t s_noncontrol_cursor;
uint8_t g_ds5_owner_esp_response_scheduler;

static ds5_sched_class_t sched_class_for_store(response_store_class_t store)
{
    switch (store) {
    case RESPONSE_STORE_INPUT:
        return DS5_SCHED_INPUT_REALTIME;
    case RESPONSE_STORE_MIC:
        return DS5_SCHED_MIC_STREAM;
    case RESPONSE_STORE_LINK:
        return DS5_SCHED_LINK_STATE;
    case RESPONSE_STORE_TELEMETRY:
        return DS5_SCHED_TELEMETRY;
    default:
        return DS5_SCHED_RELIABLE_CONTROL;
    }
}

static ds5_sched_class_metrics_t *metrics_for_store(response_store_class_t store)
{
    ds5_sched_class_t sched_class = sched_class_for_store(store);
    return &g_ds5_response_scheduler_storage
                .metrics.classes[(uint8_t)sched_class - 1U];
}

static bool slot_pending(const response_header_t *header)
{
    return header->state == DS5_SCHED_SLOT_READY ||
           header->state == DS5_SCHED_SLOT_SENDING;
}

static uint8_t pending_count_locked(void)
{
    uint8_t count = 0;

#define COUNT_PENDING(array)                                                   \
    do {                                                                       \
        for (size_t i = 0; i < sizeof(array) / sizeof((array)[0]); i++) {      \
            count += slot_pending(&(array)[i].header) ? 1U : 0U;               \
        }                                                                      \
    } while (0)

    COUNT_PENDING(g_ds5_response_scheduler_storage.control);
    COUNT_PENDING(g_ds5_response_scheduler_storage.feature);
    COUNT_PENDING(g_ds5_response_scheduler_storage.input);
    COUNT_PENDING(g_ds5_response_scheduler_storage.mic);
#undef COUNT_PENDING
    count += slot_pending(&g_ds5_response_scheduler_storage.link.header) ? 1U : 0U;
    count += slot_pending(&g_ds5_response_scheduler_storage.status.header) ? 1U : 0U;
    count += slot_pending(&g_ds5_response_scheduler_storage.telemetry.header) ? 1U : 0U;
    return count;
}

static void sync_irq_locked(void)
{
    if (s_irq_cb != NULL) {
        s_irq_cb(pending_count_locked() != 0U, s_irq_ctx);
    }
}

static response_store_class_t classify_response(uint8_t type, uint16_t flags)
{
    if ((flags & DS5_DUAL_FLAG_ACK) != 0U ||
        type == DS5_DUAL_MSG_HELLO || type == DS5_DUAL_MSG_TIME_SYNC) {
        return RESPONSE_STORE_CONTROL;
    }
    switch (type) {
    case DS5_DUAL_MSG_BT_RX_FEATURE_REPORT:
        return RESPONSE_STORE_FEATURE;
    case DS5_DUAL_MSG_BT_RX_INPUT:
        return RESPONSE_STORE_INPUT;
    case DS5_DUAL_MSG_BT_RX_MIC_OPUS:
        return RESPONSE_STORE_MIC;
    case DS5_DUAL_MSG_BT_STATE:
        return RESPONSE_STORE_LINK;
    case DS5_DUAL_MSG_FLOW_CREDIT:
        return RESPONSE_STORE_STATUS;
    case DS5_DUAL_MSG_STATS:
        return RESPONSE_STORE_TELEMETRY;
    default:
        return RESPONSE_STORE_CONTROL;
    }
}

static void init_header(response_header_t *header,
                        response_store_class_t store,
                        uint8_t type,
                        uint16_t flags,
                        uint8_t channel,
                        uint8_t priority,
                        uint16_t wire_sequence,
                        uint32_t timestamp_us,
                        size_t payload_len)
{
    header->version++;
    header->wire_flags = flags;
    header->wire_sequence = wire_sequence;
    header->type = type;
    header->channel = channel;
    header->priority = priority;
    ds5_sched_meta_init(&header->meta,
                        timestamp_us,
                        s_generation,
                        s_publish_sequence++,
                        (uint16_t)payload_len,
                        sched_class_for_store(store),
                        0);
    header->state = DS5_SCHED_SLOT_READY;
}

static int find_free_or_oldest_ready(response_header_t *headers,
                                     size_t stride,
                                     size_t count,
                                     bool allow_replace,
                                     bool *replaced)
{
    int oldest = -1;
    uint32_t oldest_sequence = UINT32_MAX;

    *replaced = false;
    for (size_t i = 0; i < count; i++) {
        response_header_t *header =
            (response_header_t *)((uint8_t *)headers + i * stride);
        if (header->state == DS5_SCHED_SLOT_FREE) {
            return (int)i;
        }
        if (allow_replace && header->state == DS5_SCHED_SLOT_READY &&
            header->meta.sequence < oldest_sequence) {
            oldest = (int)i;
            oldest_sequence = header->meta.sequence;
        }
    }
    if (oldest >= 0) {
        *replaced = true;
    }
    return oldest;
}

static bool publish_array(response_store_class_t store,
                          response_header_t *headers,
                          size_t stride,
                          size_t count,
                          uint8_t *payloads,
                          size_t payload_offset,
                          size_t payload_capacity,
                          bool allow_replace,
                          uint8_t type,
                          uint16_t flags,
                          uint8_t channel,
                          uint8_t priority,
                          uint16_t wire_sequence,
                          uint32_t timestamp_us,
                          const uint8_t *payload,
                          size_t payload_len)
{
    bool replaced;
    int selected = find_free_or_oldest_ready(headers,
                                             stride,
                                             count,
                                             allow_replace,
                                             &replaced);
    ds5_sched_class_metrics_t *metrics = metrics_for_store(store);

    if (selected < 0 || payload_len > payload_capacity) {
        metrics->admission_rejected++;
        return false;
    }
    response_header_t *header =
        (response_header_t *)((uint8_t *)headers + selected * stride);
    uint8_t *dst = payloads + selected * stride + payload_offset;
    if (replaced) {
        metrics->replaced++;
    } else {
        metrics->pending++;
        if (metrics->pending > metrics->highwater) {
            metrics->highwater = metrics->pending;
        }
    }
    memcpy(dst, payload, payload_len);
    init_header(header,
                store,
                type,
                flags,
                channel,
                priority,
                wire_sequence,
                timestamp_us,
                payload_len);
    metrics->accepted++;
    return true;
}

static bool publish_mailbox(response_store_class_t store,
                            response_header_t *header,
                            uint8_t *dst,
                            size_t capacity,
                            uint8_t type,
                            uint16_t flags,
                            uint8_t channel,
                            uint8_t priority,
                            uint16_t wire_sequence,
                            uint32_t timestamp_us,
                            const uint8_t *payload,
                            size_t payload_len)
{
    ds5_sched_class_metrics_t *metrics = metrics_for_store(store);

    if (payload_len > capacity) {
        metrics->admission_rejected++;
        return false;
    }
    if (slot_pending(header)) {
        metrics->coalesced++;
    } else {
        metrics->pending++;
        if (metrics->pending > metrics->highwater) {
            metrics->highwater = metrics->pending;
        }
    }
    memcpy(dst, payload, payload_len);
    init_header(header,
                store,
                type,
                flags,
                channel,
                priority,
                wire_sequence,
                timestamp_us,
                payload_len);
    metrics->accepted++;
    return true;
}

void esp32_dual_response_scheduler_init(esp32_dual_response_irq_cb_t irq_cb,
                                        void *irq_ctx)
{
    g_ds5_owner_esp_response_scheduler = 1U;
    portENTER_CRITICAL(&s_response_mux);
    memset(&g_ds5_response_scheduler_storage,
           0,
           sizeof(g_ds5_response_scheduler_storage));
    s_irq_cb = irq_cb;
    s_irq_ctx = irq_ctx;
    s_publish_sequence = 0;
    s_generation = 0;
    s_control_burst = 0U;
    s_noncontrol_cursor = 0U;
    sync_irq_locked();
    portEXIT_CRITICAL(&s_response_mux);
}

void esp32_dual_response_reset_generation(uint32_t generation)
{
    portENTER_CRITICAL(&s_response_mux);
    if (generation == s_generation) {
        portEXIT_CRITICAL(&s_response_mux);
        return;
    }
    for (size_t store = RESPONSE_STORE_CONTROL;
         store <= RESPONSE_STORE_TELEMETRY;
         store++) {
        ds5_sched_class_metrics_t *metrics = metrics_for_store(
            (response_store_class_t)store);
        if (metrics->pending > 0U) {
            metrics->replaced += metrics->pending;
            metrics->pending = 0;
        }
    }
    memset(g_ds5_response_scheduler_storage.control,
           0,
           sizeof(g_ds5_response_scheduler_storage.control));
    memset(g_ds5_response_scheduler_storage.feature,
           0,
           sizeof(g_ds5_response_scheduler_storage.feature));
    memset(g_ds5_response_scheduler_storage.input,
           0,
           sizeof(g_ds5_response_scheduler_storage.input));
    memset(g_ds5_response_scheduler_storage.mic,
           0,
           sizeof(g_ds5_response_scheduler_storage.mic));
    memset(&g_ds5_response_scheduler_storage.link,
           0,
           sizeof(g_ds5_response_scheduler_storage.link));
    memset(&g_ds5_response_scheduler_storage.status,
           0,
           sizeof(g_ds5_response_scheduler_storage.status));
    memset(&g_ds5_response_scheduler_storage.telemetry,
           0,
           sizeof(g_ds5_response_scheduler_storage.telemetry));
    s_generation = generation;
    s_control_burst = 0U;
    s_noncontrol_cursor = 0U;
    sync_irq_locked();
    portEXIT_CRITICAL(&s_response_mux);
}

void esp32_dual_response_reset_metrics(void)
{
    portENTER_CRITICAL(&s_response_mux);
    memset(&g_ds5_response_scheduler_storage.metrics,
           0,
           sizeof(g_ds5_response_scheduler_storage.metrics));
    for (size_t store = RESPONSE_STORE_CONTROL;
         store <= RESPONSE_STORE_TELEMETRY;
         store++) {
        ds5_sched_class_metrics_t *metrics = metrics_for_store(
            (response_store_class_t)store);
        response_store_class_t current = (response_store_class_t)store;
        uint64_t pending = 0;
        switch (current) {
        case RESPONSE_STORE_CONTROL:
            for (size_t i = 0; i < RESPONSE_CONTROL_CAPACITY; i++) {
                pending += slot_pending(
                    &g_ds5_response_scheduler_storage.control[i].header);
            }
            break;
        case RESPONSE_STORE_FEATURE:
            for (size_t i = 0; i < RESPONSE_FEATURE_CAPACITY; i++) {
                pending += slot_pending(
                    &g_ds5_response_scheduler_storage.feature[i].header);
            }
            break;
        case RESPONSE_STORE_INPUT:
            for (size_t i = 0; i < RESPONSE_INPUT_CAPACITY; i++) {
                pending += slot_pending(
                    &g_ds5_response_scheduler_storage.input[i].header);
            }
            break;
        case RESPONSE_STORE_MIC:
            for (size_t i = 0; i < RESPONSE_MIC_CAPACITY; i++) {
                pending += slot_pending(
                    &g_ds5_response_scheduler_storage.mic[i].header);
            }
            break;
        case RESPONSE_STORE_LINK:
            pending = slot_pending(
                &g_ds5_response_scheduler_storage.link.header);
            break;
        case RESPONSE_STORE_STATUS:
            pending = slot_pending(
                &g_ds5_response_scheduler_storage.status.header);
            break;
        case RESPONSE_STORE_TELEMETRY:
            pending = slot_pending(
                &g_ds5_response_scheduler_storage.telemetry.header);
            break;
        default:
            break;
        }
        metrics->pending += pending;
        metrics->highwater = metrics->pending;
    }
    portEXIT_CRITICAL(&s_response_mux);
}

bool esp32_dual_response_publish(uint8_t type,
                                 uint16_t flags,
                                 uint8_t channel,
                                 uint8_t priority,
                                 uint16_t wire_sequence,
                                 uint32_t timestamp_us,
                                 const uint8_t *payload,
                                 size_t payload_len)
{
    response_store_class_t store = classify_response(type, flags);
    bool accepted = false;

    if (payload == NULL || payload_len == 0U ||
        payload_len > DS5_DUAL_SPI_MAX_PAYLOAD) {
        return false;
    }

    portENTER_CRITICAL(&s_response_mux);
    switch (store) {
    case RESPONSE_STORE_CONTROL:
        accepted = publish_array(
            store,
            &g_ds5_response_scheduler_storage.control[0].header,
            sizeof(response_control_slot_t),
            RESPONSE_CONTROL_CAPACITY,
            (uint8_t *)g_ds5_response_scheduler_storage.control,
            offsetof(response_control_slot_t, payload),
            RESPONSE_CONTROL_MAX,
            false,
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_FEATURE:
        accepted = publish_array(
            store,
            &g_ds5_response_scheduler_storage.feature[0].header,
            sizeof(response_feature_slot_t),
            RESPONSE_FEATURE_CAPACITY,
            (uint8_t *)g_ds5_response_scheduler_storage.feature,
            offsetof(response_feature_slot_t, payload),
            RESPONSE_FEATURE_MAX,
            false,
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_INPUT:
        accepted = publish_array(
            store,
            &g_ds5_response_scheduler_storage.input[0].header,
            sizeof(response_input_slot_t),
            RESPONSE_INPUT_CAPACITY,
            (uint8_t *)g_ds5_response_scheduler_storage.input,
            offsetof(response_input_slot_t, payload),
            RESPONSE_INPUT_MAX,
            true,
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_MIC:
        accepted = publish_array(
            store,
            &g_ds5_response_scheduler_storage.mic[0].header,
            sizeof(response_mic_slot_t),
            RESPONSE_MIC_CAPACITY,
            (uint8_t *)g_ds5_response_scheduler_storage.mic,
            offsetof(response_mic_slot_t, payload),
            RESPONSE_MIC_MAX,
            true,
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_LINK:
        accepted = publish_mailbox(
            store,
            &g_ds5_response_scheduler_storage.link.header,
            g_ds5_response_scheduler_storage.link.payload,
            sizeof(g_ds5_response_scheduler_storage.link.payload),
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_STATUS:
        accepted = publish_mailbox(
            store,
            &g_ds5_response_scheduler_storage.status.header,
            g_ds5_response_scheduler_storage.status.payload,
            sizeof(g_ds5_response_scheduler_storage.status.payload),
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    case RESPONSE_STORE_TELEMETRY:
        accepted = publish_mailbox(
            store,
            &g_ds5_response_scheduler_storage.telemetry.header,
            g_ds5_response_scheduler_storage.telemetry.payload,
            sizeof(g_ds5_response_scheduler_storage.telemetry.payload),
            type, flags, channel, priority, wire_sequence, timestamp_us,
            payload, payload_len);
        break;
    default:
        break;
    }
    sync_irq_locked();
    portEXIT_CRITICAL(&s_response_mux);
    return accepted;
}

static int oldest_ready(response_header_t *headers,
                        size_t stride,
                        size_t count)
{
    int selected = -1;
    uint32_t sequence = UINT32_MAX;

    for (size_t i = 0; i < count; i++) {
        response_header_t *header =
            (response_header_t *)((uint8_t *)headers + i * stride);
        if (header->state == DS5_SCHED_SLOT_READY &&
            header->meta.sequence < sequence) {
            selected = (int)i;
            sequence = header->meta.sequence;
        }
    }
    return selected;
}

static bool select_array(response_store_class_t store,
                         response_header_t *headers,
                         size_t stride,
                         size_t count,
                         uint8_t *payloads,
                         size_t payload_offset,
                         response_header_t *selected_header,
                         uint8_t *selected_payload,
                         esp32_dual_response_token_t *token)
{
    int selected = oldest_ready(headers, stride, count);
    if (selected < 0) {
        return false;
    }
    response_header_t *header =
        (response_header_t *)((uint8_t *)headers + selected * stride);
    header->state = DS5_SCHED_SLOT_SENDING;
    *selected_header = *header;
    memcpy(selected_payload,
           payloads + selected * stride + payload_offset,
           header->meta.length);
    token->storage_class = (uint8_t)store;
    token->slot = (uint8_t)selected;
    token->version = header->version;
    return true;
}

static bool select_mailbox(response_store_class_t store,
                           response_header_t *header,
                           const uint8_t *payload,
                           response_header_t *selected_header,
                           uint8_t *selected_payload,
                           esp32_dual_response_token_t *token)
{
    if (header->state != DS5_SCHED_SLOT_READY) {
        return false;
    }
    header->state = DS5_SCHED_SLOT_SENDING;
    *selected_header = *header;
    memcpy(selected_payload, payload, header->meta.length);
    token->storage_class = (uint8_t)store;
    token->slot = 0;
    token->version = header->version;
    return true;
}

bool esp32_dual_response_stage(uint8_t *frame,
                               size_t frame_capacity,
                               size_t *frame_len,
                               esp32_dual_response_token_t *token)
{
    response_header_t selected = {0};
    uint8_t payload[DS5_DUAL_SPI_MAX_PAYLOAD];
    bool valid = false;

    if (frame == NULL || token == NULL || frame_len == NULL) {
        return false;
    }
    memset(token, 0, sizeof(*token));
    *frame_len = 0;

    portENTER_CRITICAL(&s_response_mux);
#define SELECT_ARRAY(store, member, type_name)                                \
    select_array(store,                                                       \
                 &g_ds5_response_scheduler_storage.member[0].header,          \
                 sizeof(type_name),                                           \
                 sizeof(g_ds5_response_scheduler_storage.member) /            \
                     sizeof(g_ds5_response_scheduler_storage.member[0]),       \
                 (uint8_t *)g_ds5_response_scheduler_storage.member,           \
                 offsetof(type_name, payload),                                \
                 &selected, payload, token)

#define SELECT_CONTROL()                                                      \
    (SELECT_ARRAY(RESPONSE_STORE_CONTROL, control, response_control_slot_t) || \
     SELECT_ARRAY(RESPONSE_STORE_FEATURE, feature, response_feature_slot_t))
#define SELECT_LINK()                                                         \
    select_mailbox(RESPONSE_STORE_LINK,                                       \
                   &g_ds5_response_scheduler_storage.link.header,             \
                   g_ds5_response_scheduler_storage.link.payload,             \
                   &selected, payload, token)
#define SELECT_STATUS()                                                       \
    select_mailbox(RESPONSE_STORE_STATUS,                                     \
                   &g_ds5_response_scheduler_storage.status.header,           \
                   g_ds5_response_scheduler_storage.status.payload,           \
                   &selected, payload, token)
#define SELECT_TELEMETRY()                                                    \
    select_mailbox(RESPONSE_STORE_TELEMETRY,                                  \
                   &g_ds5_response_scheduler_storage.telemetry.header,        \
                   g_ds5_response_scheduler_storage.telemetry.payload,        \
                   &selected, payload, token)
#define SELECT_NONCONTROL_DEFAULT()                                           \
    (SELECT_ARRAY(RESPONSE_STORE_INPUT, input, response_input_slot_t) ||       \
     SELECT_ARRAY(RESPONSE_STORE_MIC, mic, response_mic_slot_t) ||            \
     SELECT_LINK() || SELECT_STATUS() || SELECT_TELEMETRY())

    if (s_control_burst < RESPONSE_CONTROL_MAX_BURST) {
        valid = SELECT_CONTROL();
    }
    if (!valid) {
        switch (s_noncontrol_cursor) {
        case 1U:
        case 5U:
            valid = SELECT_ARRAY(RESPONSE_STORE_MIC, mic,
                                 response_mic_slot_t);
            break;
        case 3U:
            valid = SELECT_LINK();
            break;
        case 6U:
            valid = SELECT_STATUS();
            break;
        case 8U:
            valid = SELECT_TELEMETRY();
            break;
        default:
            valid = SELECT_ARRAY(RESPONSE_STORE_INPUT, input,
                                 response_input_slot_t);
            break;
        }
        if (!valid) {
            valid = SELECT_NONCONTROL_DEFAULT();
        }
        if (valid) {
            s_noncontrol_cursor =
                (uint8_t)((s_noncontrol_cursor + 1U) % RESPONSE_NONCONTROL_SLOTS);
            s_control_burst = 0U;
        }
    }
    if (!valid) {
        valid = SELECT_CONTROL();
    }
    if (valid && (token->storage_class == RESPONSE_STORE_CONTROL ||
                  token->storage_class == RESPONSE_STORE_FEATURE)) {
        if (s_control_burst < RESPONSE_CONTROL_MAX_BURST) {
            s_control_burst++;
        }
    }
#undef SELECT_NONCONTROL_DEFAULT
#undef SELECT_TELEMETRY
#undef SELECT_STATUS
#undef SELECT_LINK
#undef SELECT_CONTROL
#undef SELECT_ARRAY
    sync_irq_locked();
    portEXIT_CRITICAL(&s_response_mux);

    if (!valid) {
        return false;
    }
    ds5_dual_spi_header_t header;
    ds5_dual_spi_header_init(&header,
                             selected.type,
                             selected.wire_flags,
                             selected.channel,
                             selected.priority,
                             selected.wire_sequence,
                             (uint32_t)selected.meta.created_us,
                             selected.meta.length);
    if (!ds5_dual_spi_finalize_frame(frame,
                                     frame_capacity,
                                     &header,
                                     payload)) {
        esp32_dual_response_finish(token, false);
        return false;
    }
    *frame_len = ds5_dual_spi_frame_len(selected.meta.length);
    return true;
}

static response_header_t *header_for_token(
    const esp32_dual_response_token_t *token)
{
    switch ((response_store_class_t)token->storage_class) {
    case RESPONSE_STORE_CONTROL:
        return token->slot < RESPONSE_CONTROL_CAPACITY ?
            &g_ds5_response_scheduler_storage.control[token->slot].header : NULL;
    case RESPONSE_STORE_FEATURE:
        return token->slot < RESPONSE_FEATURE_CAPACITY ?
            &g_ds5_response_scheduler_storage.feature[token->slot].header : NULL;
    case RESPONSE_STORE_INPUT:
        return token->slot < RESPONSE_INPUT_CAPACITY ?
            &g_ds5_response_scheduler_storage.input[token->slot].header : NULL;
    case RESPONSE_STORE_MIC:
        return token->slot < RESPONSE_MIC_CAPACITY ?
            &g_ds5_response_scheduler_storage.mic[token->slot].header : NULL;
    case RESPONSE_STORE_LINK:
        return &g_ds5_response_scheduler_storage.link.header;
    case RESPONSE_STORE_STATUS:
        return &g_ds5_response_scheduler_storage.status.header;
    case RESPONSE_STORE_TELEMETRY:
        return &g_ds5_response_scheduler_storage.telemetry.header;
    default:
        return NULL;
    }
}

void esp32_dual_response_finish(const esp32_dual_response_token_t *token,
                                bool success)
{
    if (token == NULL || token->storage_class == RESPONSE_STORE_NONE) {
        return;
    }
    portENTER_CRITICAL(&s_response_mux);
    response_header_t *header = header_for_token(token);
    if (header != NULL && header->version == token->version &&
        header->state == DS5_SCHED_SLOT_SENDING) {
        ds5_sched_class_metrics_t *metrics = metrics_for_store(
            (response_store_class_t)token->storage_class);
        if (success) {
            header->state = DS5_SCHED_SLOT_FREE;
            metrics->transmitted++;
            if (metrics->pending > 0U) {
                metrics->pending--;
            }
        } else {
            header->state = DS5_SCHED_SLOT_READY;
            metrics->transport_failed++;
        }
    }
    sync_irq_locked();
    portEXIT_CRITICAL(&s_response_mux);
}

uint8_t esp32_dual_response_pending_count(void)
{
    uint8_t pending;
    portENTER_CRITICAL(&s_response_mux);
    pending = pending_count_locked();
    portEXIT_CRITICAL(&s_response_mux);
    return pending;
}

void esp32_dual_response_get_metrics(ds5_sched_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_response_mux);
    *metrics = g_ds5_response_scheduler_storage.metrics;
    portEXIT_CRITICAL(&s_response_mux);
}
