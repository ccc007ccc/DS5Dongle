#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m61_bt_tx_scheduler.h"

#define USB_FRAME_BYTES 8U

static void ingest_silent_epoch_pair(uint32_t generation,
                                     uint64_t captured_us)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES] = {0};

    m61_audio_epoch_ingest_usb(audio, sizeof(audio), generation,
                               captured_us, false, 256);
}

static m61_audio_epoch_pair_t make_pair(uint32_t generation,
                                        uint32_t epoch,
                                        uint64_t captured_us,
                                        uint8_t marker)
{
    m61_audio_epoch_pair_t pair;

    memset(&pair, 0, sizeof(pair));
    pair.generation = generation;
    pair.first_epoch = epoch;
    pair.first_captured_us = captured_us;
    pair.second_captured_us = captured_us + 10667U;
    pair.haptics[0][0] = marker;
    return pair;
}

static void assert_class_conserved(const m61_bt_tx_class_metrics_t *metrics)
{
    assert(metrics->accepted == metrics->transmitted + metrics->replaced +
                                    metrics->stale + metrics->dropped +
                                    metrics->pending);
}

static void select_success(m61_bt_tx_scheduler_t *scheduler,
                           uint64_t now_us,
                           m61_bt_tx_class_t expected)
{
    m61_bt_tx_selection_t selection;

    assert(m61_bt_tx_scheduler_select(scheduler, now_us,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.tx_class == expected);
    assert(m61_bt_tx_scheduler_selection_is_current(scheduler, &selection));
    assert(m61_bt_tx_scheduler_finish(scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));
}

static void test_realtime_replacement_and_order(void)
{
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;
    m61_bt_tx_metrics_t metrics;
    m61_audio_epoch_pair_t first = make_pair(1, 10, 1000, 0x10);
    m61_audio_epoch_pair_t second = make_pair(1, 12, 2000, 0x20);
    m61_audio_epoch_pair_t third = make_pair(1, 14, 3000, 0x30);

    m61_bt_tx_scheduler_init(&scheduler, 1);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &first));
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &second));
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &third));
    assert(m61_bt_tx_scheduler_select(&scheduler, 4000,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.payload.realtime->first_epoch == 12);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));
    assert(m61_bt_tx_scheduler_select(&scheduler, 4000,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.payload.realtime->first_epoch == 14);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));
    m61_bt_tx_scheduler_get_metrics(&scheduler, &metrics);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].accepted == 3);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].transmitted == 2);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].replaced == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].pending == 0);
    assert_class_conserved(
        &metrics.classes[M61_BT_TX_CLASS_REALTIME]);
}

static void test_stale_and_generation_filter(void)
{
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;
    m61_bt_tx_metrics_t metrics;
    m61_audio_epoch_pair_t old = make_pair(4, 0, 100, 1);
    m61_audio_epoch_pair_t current = make_pair(5, 2, 70000, 2);

    m61_bt_tx_scheduler_init(&scheduler, 4);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &old));
    assert(!m61_bt_tx_scheduler_select(
        &scheduler, 100 + M61_BT_TX_RT_MAX_AGE_US + 1,
        M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(!m61_bt_tx_scheduler_publish_realtime(&scheduler, &current));
    m61_bt_tx_scheduler_reset_generation(&scheduler, 5);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &current));
    assert(m61_bt_tx_scheduler_select(&scheduler, 70001,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    m61_bt_tx_scheduler_reset_generation(&scheduler, 6);
    assert(!m61_bt_tx_scheduler_selection_is_current(&scheduler, &selection));
    assert(!m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                       M61_BT_TX_FINISH_SUCCESS));
    m61_bt_tx_scheduler_get_metrics(&scheduler, &metrics);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].transmitted == 0);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].stale == 2);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].rejected == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].pending == 0);
    assert_class_conserved(
        &metrics.classes[M61_BT_TX_CLASS_REALTIME]);
}

static void test_retry_keeps_versioned_slot(void)
{
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t first_selection;
    m61_bt_tx_selection_t retry_selection;
    m61_bt_tx_metrics_t metrics;
    m61_audio_epoch_pair_t pair = make_pair(8, 0, 1000, 0x55);

    m61_bt_tx_scheduler_init(&scheduler, 8);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1001,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &first_selection));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1001,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &retry_selection));
    assert(retry_selection.version == first_selection.version);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &first_selection,
                                      M61_BT_TX_FINISH_RETRY));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1002,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &retry_selection));
    assert(retry_selection.version == first_selection.version);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &retry_selection,
                                      M61_BT_TX_FINISH_SUCCESS));
    pair = make_pair(8, 2, 2000, 0x66);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
    assert(m61_bt_tx_scheduler_select(&scheduler, 2001,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &retry_selection));
    assert(m61_bt_tx_scheduler_finish(&scheduler, &retry_selection,
                                      M61_BT_TX_FINISH_DROP));
    m61_bt_tx_scheduler_get_metrics(&scheduler, &metrics);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].retried == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].dropped == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].transmitted == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].pending == 0);
    assert_class_conserved(
        &metrics.classes[M61_BT_TX_CLASS_REALTIME]);
}

static void test_versioned_finish_keeps_new_mailbox(void)
{
    const uint8_t old_report[] = {0x31, 1};
    const uint8_t new_report[] = {0x31, 2};
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t old_selection;
    m61_bt_tx_selection_t new_selection;

    m61_bt_tx_scheduler_init(&scheduler, 9);
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, old_report,
                                                sizeof(old_report), true,
                                                9, 100));
    assert(m61_bt_tx_scheduler_select(&scheduler, 101,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &old_selection));
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, new_report,
                                                sizeof(new_report), true,
                                                9, 102));
    assert(!m61_bt_tx_scheduler_finish(&scheduler, &old_selection,
                                       M61_BT_TX_FINISH_SUCCESS));
    assert(m61_bt_tx_scheduler_select(&scheduler, 103,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &new_selection));
    assert(new_selection.version != old_selection.version);
    assert(new_selection.payload.state31->report[1] == 2);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &new_selection,
                                      M61_BT_TX_FINISH_SUCCESS));
}

static void test_mailbox_and_fairness(void)
{
    const uint8_t old31[] = {0x31, 1, 2};
    const uint8_t new31[] = {0x31, 9, 8, 7};
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;
    m61_bt_tx_metrics_t metrics;

    m61_bt_tx_scheduler_init(&scheduler, 11);
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, old31,
                                                sizeof(old31), true,
                                                11, 100));
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, new31,
                                                sizeof(new31), true,
                                                11, 200));
    assert(m61_bt_tx_scheduler_publish_state32(&scheduler, true, 11, 200));

    for (uint32_t i = 0; i < M61_BT_TX_MAX_RT_BURST; i++) {
        m61_audio_epoch_pair_t pair = make_pair(11, i * 2U,
                                                1000 + i, (uint8_t)i);
        assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
        select_success(&scheduler, 1100 + i, M61_BT_TX_CLASS_REALTIME);
    }
    assert(m61_bt_tx_scheduler_publish_realtime(
        &scheduler, &(m61_audio_epoch_pair_t){
                        .generation = 11,
                        .first_epoch = 10,
                        .first_captured_us = 1200,
                    }));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1201,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.tx_class == M61_BT_TX_CLASS_STATE31);
    assert(selection.payload.state31->len == sizeof(new31));
    assert(selection.payload.state31->includes_id);
    assert(memcmp(selection.payload.state31->report, new31,
                  sizeof(new31)) == 0);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_RETRY));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1202,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.tx_class == M61_BT_TX_CLASS_STATE31);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));

    select_success(&scheduler, 1203, M61_BT_TX_CLASS_REALTIME);
    select_success(&scheduler, 1204, M61_BT_TX_CLASS_STATE32);
    m61_bt_tx_scheduler_get_metrics(&scheduler, &metrics);
    assert(metrics.classes[M61_BT_TX_CLASS_STATE31].accepted == 2);
    assert(metrics.classes[M61_BT_TX_CLASS_STATE31].replaced == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_STATE31].retried == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_STATE31].transmitted == 1);
    assert(metrics.classes[M61_BT_TX_CLASS_STATE32].transmitted == 1);
    assert_class_conserved(&metrics.classes[M61_BT_TX_CLASS_STATE31]);
    assert_class_conserved(&metrics.classes[M61_BT_TX_CLASS_STATE32]);
}

static void test_eligibility_does_not_block_realtime(void)
{
    const uint8_t state31[] = {0x31, 1};
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;
    m61_audio_epoch_pair_t pair = make_pair(21, 0, 1000, 1);

    m61_bt_tx_scheduler_init(&scheduler, 21);
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, state31,
                                                sizeof(state31), true,
                                                21, 900));
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
    assert(m61_bt_tx_scheduler_select(
        &scheduler, 1001,
        M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_REALTIME), &selection));
    assert(selection.tx_class == M61_BT_TX_CLASS_REALTIME);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));
    assert(!m61_bt_tx_scheduler_select(
        &scheduler, 1002,
        M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_REALTIME), &selection));
    assert(m61_bt_tx_scheduler_select(
        &scheduler, 1002,
        M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE31), &selection));
    assert(selection.tx_class == M61_BT_TX_CLASS_STATE31);
}

static void test_same_generation_reset_and_invalid_finish(void)
{
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;
    m61_bt_tx_metrics_t metrics;
    m61_audio_epoch_pair_t pair = make_pair(31, 0, 1000, 1);

    m61_bt_tx_scheduler_init(&scheduler, 31);
    assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1001,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(!m61_bt_tx_scheduler_finish(
        &scheduler, &selection, (m61_bt_tx_finish_t)-1));
    assert(m61_bt_tx_scheduler_selection_is_current(&scheduler, &selection));
    m61_bt_tx_scheduler_reset_generation(&scheduler, 31);
    assert(!m61_bt_tx_scheduler_selection_is_current(&scheduler, &selection));
    m61_bt_tx_scheduler_get_metrics(&scheduler, &metrics);
    assert(metrics.classes[M61_BT_TX_CLASS_REALTIME].stale == 1);
    assert_class_conserved(
        &metrics.classes[M61_BT_TX_CLASS_REALTIME]);
}

static void test_dropped_realtime_consumes_fairness_budget(void)
{
    const uint8_t state31[] = {0x31, 1};
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;

    m61_bt_tx_scheduler_init(&scheduler, 41);
    assert(m61_bt_tx_scheduler_publish_state31(&scheduler, state31,
                                                sizeof(state31), true,
                                                41, 100));
    for (uint32_t i = 0; i < M61_BT_TX_MAX_RT_BURST; i++) {
        m61_audio_epoch_pair_t pair = make_pair(41, i * 2U,
                                                1000 + i, (uint8_t)i);

        assert(m61_bt_tx_scheduler_publish_realtime(&scheduler, &pair));
        assert(m61_bt_tx_scheduler_select(&scheduler, 1100 + i,
                                          M61_BT_TX_ELIGIBLE_ALL,
                                          &selection));
        assert(selection.tx_class == M61_BT_TX_CLASS_REALTIME);
        assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                          M61_BT_TX_FINISH_DROP));
    }
    assert(m61_bt_tx_scheduler_publish_realtime(
        &scheduler, &(m61_audio_epoch_pair_t){
                        .generation = 41,
                        .first_epoch = 10,
                        .first_captured_us = 1200,
                    }));
    assert(m61_bt_tx_scheduler_select(&scheduler, 1201,
                                      M61_BT_TX_ELIGIBLE_ALL, &selection));
    assert(selection.tx_class == M61_BT_TX_CLASS_STATE31);
}

static void test_direct_epoch_ingest_backpressure(void)
{
    m61_bt_tx_scheduler_t scheduler;
    m61_bt_tx_selection_t selection;

    m61_audio_epoch_init(51);
    m61_bt_tx_scheduler_init(&scheduler, 51);
    ingest_silent_epoch_pair(51, 1000);
    assert(m61_bt_tx_scheduler_ingest_epoch_pair(&scheduler));
    ingest_silent_epoch_pair(51, 22000);
    assert(m61_bt_tx_scheduler_ingest_epoch_pair(&scheduler));
    ingest_silent_epoch_pair(51, 43000);
    assert(!m61_bt_tx_scheduler_ingest_epoch_pair(&scheduler));

    assert(m61_bt_tx_scheduler_select(&scheduler, 44000,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &selection));
    assert(selection.payload.realtime->first_epoch == 0U);
    assert(m61_bt_tx_scheduler_finish(&scheduler, &selection,
                                      M61_BT_TX_FINISH_SUCCESS));
    assert(m61_bt_tx_scheduler_ingest_epoch_pair(&scheduler));
    assert(m61_bt_tx_scheduler_select(&scheduler, 45000,
                                      M61_BT_TX_ELIGIBLE_ALL,
                                      &selection));
    assert(selection.payload.realtime->first_epoch == 2U);
}

int main(void)
{
    test_realtime_replacement_and_order();
    test_stale_and_generation_filter();
    test_retry_keeps_versioned_slot();
    test_versioned_finish_keeps_new_mailbox();
    test_mailbox_and_fairness();
    test_eligibility_does_not_block_realtime();
    test_same_generation_reset_and_invalid_finish();
    test_dropped_realtime_consumes_fairness_budget();
    test_direct_epoch_ingest_backpressure();
    puts("M61 Bluetooth TX scheduler tests passed.");
    return 0;
}
