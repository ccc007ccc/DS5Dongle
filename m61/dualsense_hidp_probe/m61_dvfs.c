#include "m61_dvfs.h"

#include <errno.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bflb_mtimer.h"
#include "easyflash.h"

#if defined(BL616)
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bl616_pds.h"
#endif

#define M61_DVFS_TASK_STACK_WORDS 512U
#define M61_DVFS_TASK_PRIORITY (configMAX_PRIORITIES - 6)
#define M61_DVFS_DOWNSHIFT_DELAY_MS 2000U
#define M61_DVFS_MIN_RESIDENCY_MS 500U
#define M61_DVFS_AUDIO_ACTIVITY_HOLD_MS 250U
#define M61_DVFS_CONFIG_KEY "m61_dvfs_v1"
#define M61_DVFS_CONFIG_MAGIC 0x44564631UL

typedef enum {
    M61_AUPLL_BAND_UNKNOWN = 0,
    M61_AUPLL_BAND_384,
    M61_AUPLL_BAND_400,
} m61_aupll_band_t;

typedef struct {
    uint32_t magic;
    uint16_t manual_mhz;
    uint8_t governor;
    uint8_t manual_profile;
} m61_dvfs_persistent_config_t;

static StaticTask_t dvfs_task_tcb;
static StackType_t dvfs_task_stack[M61_DVFS_TASK_STACK_WORDS];
static TaskHandle_t volatile dvfs_task_handle;

static volatile bool dvfs_initialized;
static volatile m61_dvfs_governor_t dvfs_governor =
    M61_DVFS_GOVERNOR_MANUAL;
static volatile m61_dvfs_profile_t dvfs_manual_profile =
    M61_DVFS_PROFILE_ECO;
static volatile uint32_t dvfs_manual_mhz = M61_DVFS_DEFAULT_MHZ;
static volatile bool dvfs_manual_experimental;
static volatile uint32_t dvfs_current_mhz = M61_DVFS_DEFAULT_MHZ;
static volatile uint32_t dvfs_requested_mhz = M61_DVFS_DEFAULT_MHZ;
static volatile uint32_t dvfs_client_floor[M61_DVFS_CLIENT_COUNT];
static volatile uint32_t dvfs_client_boost[M61_DVFS_CLIENT_COUNT];
static volatile TickType_t dvfs_client_boost_until[M61_DVFS_CLIENT_COUNT];
static volatile bool dvfs_speaker_active;
static volatile bool dvfs_mic_active;
static volatile bool dvfs_speaker_enabled;
static volatile bool dvfs_mic_enabled;
static volatile bool dvfs_speaker_stereo;
static volatile TickType_t dvfs_speaker_active_until;
static volatile TickType_t dvfs_mic_active_until;
static volatile uint32_t dvfs_transitions;
static volatile uint32_t dvfs_transition_failures;
static volatile int32_t dvfs_last_error;
static volatile bool dvfs_persistent_config_loaded;
static m61_aupll_band_t aupll_band = M61_AUPLL_BAND_UNKNOWN;

static uint32_t dvfs_profile_mhz(m61_dvfs_profile_t profile)
{
    static const uint16_t profile_mhz[M61_DVFS_PROFILE_COUNT] = {
        [M61_DVFS_PROFILE_ECO] = 320U,
        [M61_DVFS_PROFILE_BALANCED] = 384U,
        [M61_DVFS_PROFILE_PERFORMANCE] = 400U,
    };

    if ((unsigned int)profile >= M61_DVFS_PROFILE_COUNT) return 0U;
    return profile_mhz[profile];
}

uint32_t m61_dvfs_profile_frequency(m61_dvfs_profile_t profile)
{
    return dvfs_profile_mhz(profile);
}

const char *m61_dvfs_profile_name(m61_dvfs_profile_t profile)
{
    switch (profile) {
        case M61_DVFS_PROFILE_ECO: return "eco";
        case M61_DVFS_PROFILE_BALANCED: return "balanced";
        case M61_DVFS_PROFILE_PERFORMANCE: return "performance";
        default: return "custom";
    }
}

const char *m61_dvfs_governor_name(m61_dvfs_governor_t governor)
{
    return governor == M61_DVFS_GOVERNOR_REALTIME ? "realtime" : "manual";
}

static bool dvfs_frequency_valid(uint32_t mhz, bool allow_experimental)
{
    if (mhz < M61_DVFS_DEFAULT_MHZ) return false;
    if (mhz <= M61_DVFS_VALIDATED_MAX_MHZ) return true;
    return allow_experimental && mhz <= M61_DVFS_EXPERIMENTAL_MAX_MHZ;
}

static uint32_t dvfs_sdmin_for_mhz(uint32_t mhz)
{
    /* SDK definition for a 40 MHz XTAL: sdmin = VCO(MHz) * 204.8.
     * 204.8 is 1024/5; round to the nearest register integer. */
    return (mhz * 1024U + 2U) / 5U;
}

static void dvfs_notify_worker(void)
{
    TaskHandle_t task = dvfs_task_handle;

    if (task == NULL) return;
    if (xPortIsInsideInterrupt()) {
        BaseType_t higher_priority_task_woken = pdFALSE;

        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    } else {
        xTaskNotifyGive(task);
    }
}

#if defined(BL616)
/* Clock-changing code and every SDK function it calls live in the SDK clock
 * SRAM section. This remains executable while the CPU is temporarily on
 * RC32M and the AUPLL is being retuned. */
static int ATTR_CLOCK_SECTION dvfs_apply_hardware_frequency(uint32_t target_mhz)
{
    BL_Err_Type err = SUCCESS;
    uint32_t pds_cfg;
    uint8_t saved_xclk;
    m61_aupll_band_t target_band = M61_AUPLL_BAND_UNKNOWN;

    if (!dvfs_frequency_valid(target_mhz, true)) return -EINVAL;

    taskENTER_CRITICAL();
    saved_xclk = HBN_Get_MCU_XCLK_Sel();

    /* Never reconfigure AUPLL while it is feeding the CPU. */
    HBN_Set_MCU_XCLK_Sel(HBN_MCU_XCLK_RC32M);
    HBN_Set_MCU_Root_CLK_Sel(HBN_MCU_ROOT_CLK_XCLK);
    err = GLB_Set_MCU_System_CLK_Div(0, 0);

    if (err == SUCCESS && target_mhz != M61_DVFS_DEFAULT_MHZ) {
        uint8_t xtal_type = GLB_XTAL_NONE;

        if (HBN_Get_Xtal_Type(&xtal_type) != SUCCESS ||
            xtal_type != GLB_XTAL_40M) {
            err = ERROR;
        } else {
            target_band = target_mhz <= 384U
                              ? M61_AUPLL_BAND_384
                              : M61_AUPLL_BAND_400;
            if (target_band != aupll_band) {
                err = target_band == M61_AUPLL_BAND_384
                          ? GLB_Config_AUDIO_PLL_To_384M()
                          : GLB_Config_AUDIO_PLL_To_400M();
                if (err == SUCCESS) aupll_band = target_band;
            }
            if (err == SUCCESS) {
                GLB_AUDIO_PLL_fine_tuning_sdmin(
                    dvfs_sdmin_for_mhz(target_mhz));
                bflb_mtimer_delay_us(200U);
            }
        }
    }

    /* BL616 PDS mux values are: 0=AUPLL/2, 1=AUPLL/1,
     * 2=WIFIPLL/240, 3=WIFIPLL/320. The previous prototype used 0 for the
     * default profile, which produced 245.76 or 200 MHz depending on the last
     * AUPLL setting. */
    if (err == SUCCESS && target_mhz != M61_DVFS_DEFAULT_MHZ) {
        GLB_PLL_CGEN_Clock_UnGate(GLB_PLL_CGEN_TOP_AUPLL_DIV1);
    } else {
        GLB_PLL_CGEN_Clock_UnGate(GLB_PLL_CGEN_TOP_WIFIPLL_320M);
    }
    pds_cfg = BL_RD_REG(PDS_BASE, PDS_CPU_CORE_CFG1);
    pds_cfg = BL_SET_REG_BITS_VAL(
        pds_cfg, PDS_REG_PLL_SEL,
        err == SUCCESS && target_mhz != M61_DVFS_DEFAULT_MHZ ? 1U : 3U);
    BL_WR_REG(PDS_BASE, PDS_CPU_CORE_CFG1, pds_cfg);

    /* On failure, restore the known-good WIFIPLL-320 path. */
    if (GLB_Set_MCU_System_CLK_Div(0, 3) != SUCCESS) err = ERROR;
    HBN_Set_MCU_Root_CLK_Sel(HBN_MCU_ROOT_CLK_PLL);
    HBN_Set_MCU_XCLK_Sel(saved_xclk);
    taskEXIT_CRITICAL();

    return err == SUCCESS ? 0 : -EIO;
}
#else
static int dvfs_apply_hardware_frequency(uint32_t target_mhz)
{
    (void)target_mhz;
    return -ENOTSUP;
}
#endif

uint32_t m61_dvfs_measure_cpu_mhz(void)
{
    uint64_t start_us = bflb_mtimer_get_time_us();
    uint64_t elapsed_us;
    uint32_t start_cycles;
    uint32_t end_cycles;

    __asm volatile("csrr %0, mcycle" : "=r"(start_cycles));
    do {
        elapsed_us = bflb_mtimer_get_time_us() - start_us;
    } while (elapsed_us < 2000U);
    __asm volatile("csrr %0, mcycle" : "=r"(end_cycles));

    return (uint32_t)(((uint64_t)(uint32_t)(end_cycles - start_cycles) +
                       elapsed_us / 2U) /
                      elapsed_us);
}

static bool dvfs_tick_before(TickType_t a, TickType_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint32_t dvfs_audio_activity_floor(TickType_t now,
                                          TickType_t *next_expiry)
{
    TickType_t speaker_until = dvfs_speaker_active_until;
    TickType_t mic_until = dvfs_mic_active_until;
    bool speaker = dvfs_speaker_active &&
                   dvfs_tick_before(now, speaker_until);
    bool mic = dvfs_mic_active && dvfs_tick_before(now, mic_until);

    if (speaker) *next_expiry = speaker_until;
    if (mic && (*next_expiry == 0U ||
                dvfs_tick_before(mic_until, *next_expiry))) {
        *next_expiry = mic_until;
    }
    if (speaker && mic) return 400U;
    if (speaker || mic) return 384U;
    return 0U;
}

static uint32_t dvfs_realtime_floor(TickType_t now,
                                    TickType_t *next_expiry)
{
    uint32_t target = M61_DVFS_DEFAULT_MHZ;

    *next_expiry = 0U;
    for (unsigned int i = 0; i < M61_DVFS_CLIENT_COUNT; i++) {
        uint32_t floor = dvfs_client_floor[i];
        uint32_t boost = dvfs_client_boost[i];
        TickType_t until = dvfs_client_boost_until[i];

        if (floor > target) target = floor;
        if (boost != 0U && dvfs_tick_before(now, until)) {
            if (boost > target) target = boost;
            if (*next_expiry == 0U || dvfs_tick_before(until, *next_expiry)) {
                *next_expiry = until;
            }
        } else if (boost != 0U) {
            dvfs_client_boost[i] = 0U;
        }
    }
    {
        TickType_t audio_expiry = 0U;
        uint32_t audio_floor =
            dvfs_audio_activity_floor(now, &audio_expiry);

        if (audio_floor > target) target = audio_floor;
        if (audio_expiry != 0U &&
            (*next_expiry == 0U ||
             dvfs_tick_before(audio_expiry, *next_expiry))) {
            *next_expiry = audio_expiry;
        }
    }
    return target;
}

static void dvfs_record_transition(uint32_t requested, int err)
{
    dvfs_requested_mhz = requested;
    dvfs_last_error = err;
    if (err == 0) {
        dvfs_current_mhz = requested;
        dvfs_transitions++;
    } else {
        dvfs_current_mhz = M61_DVFS_DEFAULT_MHZ;
        dvfs_transition_failures++;
    }
}

static void dvfs_worker(void *parameter)
{
    TickType_t last_transition = xTaskGetTickCount();
    TickType_t downshift_since = 0U;
    uint32_t pending_downshift = 0U;

    (void)parameter;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t next_expiry;
        uint32_t target;
        bool manual = dvfs_governor == M61_DVFS_GOVERNOR_MANUAL;

        if (manual) {
            target = dvfs_manual_mhz;
            next_expiry = 0U;
        } else {
            target = dvfs_realtime_floor(now, &next_expiry);
        }
        dvfs_requested_mhz = target;

        if (target > dvfs_current_mhz ||
            (manual && target != dvfs_current_mhz)) {
            int err = dvfs_apply_hardware_frequency(target);

            dvfs_record_transition(target, err);
            last_transition = now;
            pending_downshift = 0U;
            downshift_since = 0U;
        } else if (target < dvfs_current_mhz) {
            if (pending_downshift != target) {
                pending_downshift = target;
                downshift_since = now;
            }
            if ((TickType_t)(now - downshift_since) >=
                    pdMS_TO_TICKS(M61_DVFS_DOWNSHIFT_DELAY_MS) &&
                (TickType_t)(now - last_transition) >=
                    pdMS_TO_TICKS(M61_DVFS_MIN_RESIDENCY_MS)) {
                int err = dvfs_apply_hardware_frequency(target);

                dvfs_record_transition(target, err);
                last_transition = now;
                pending_downshift = 0U;
                downshift_since = 0U;
            }
        } else {
            pending_downshift = 0U;
            downshift_since = 0U;
        }

        TickType_t wait = portMAX_DELAY;

        now = xTaskGetTickCount();
        if (pending_downshift != 0U) {
            TickType_t deadline = downshift_since +
                                  pdMS_TO_TICKS(M61_DVFS_DOWNSHIFT_DELAY_MS);
            wait = dvfs_tick_before(now, deadline) ? deadline - now : 1U;
        }
        if (next_expiry != 0U && dvfs_tick_before(now, next_expiry)) {
            TickType_t expiry_wait = next_expiry - now;

            if (wait == portMAX_DELAY || expiry_wait < wait) wait = expiry_wait;
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

int m61_dvfs_init(uint32_t boot_mhz)
{
    m61_dvfs_persistent_config_t saved = {0};
    size_t saved_len = 0U;
    int err;

    if (dvfs_initialized) return 0;
    if (boot_mhz == 0U) boot_mhz = M61_DVFS_DEFAULT_MHZ;
    if (!dvfs_frequency_valid(boot_mhz, true)) return -EINVAL;

    if (ef_get_env_blob(M61_DVFS_CONFIG_KEY,
                        &saved,
                        sizeof(saved),
                        &saved_len) == sizeof(saved) &&
        saved_len == sizeof(saved) &&
        saved.magic == M61_DVFS_CONFIG_MAGIC &&
        saved.governor <= M61_DVFS_GOVERNOR_REALTIME &&
        dvfs_frequency_valid(saved.manual_mhz, false)) {
        boot_mhz = saved.manual_mhz;
        dvfs_governor = (m61_dvfs_governor_t)saved.governor;
        dvfs_manual_profile =
            saved.manual_profile < M61_DVFS_PROFILE_COUNT
                ? (m61_dvfs_profile_t)saved.manual_profile
                : M61_DVFS_PROFILE_COUNT;
        dvfs_persistent_config_loaded = true;
    }

    err = dvfs_apply_hardware_frequency(boot_mhz);
    dvfs_record_transition(boot_mhz, err);
    if (err != 0) return err;

    dvfs_manual_mhz = boot_mhz;
    dvfs_manual_experimental = boot_mhz > M61_DVFS_VALIDATED_MAX_MHZ;
    if (!dvfs_persistent_config_loaded) {
        if (boot_mhz == 320U) dvfs_manual_profile = M61_DVFS_PROFILE_ECO;
        else if (boot_mhz == 384U) dvfs_manual_profile = M61_DVFS_PROFILE_BALANCED;
        else if (boot_mhz == 400U) dvfs_manual_profile = M61_DVFS_PROFILE_PERFORMANCE;
        else dvfs_manual_profile = M61_DVFS_PROFILE_COUNT;
    }

    dvfs_task_handle = xTaskCreateStatic(dvfs_worker,
                                         "m61_dvfs",
                                         M61_DVFS_TASK_STACK_WORDS,
                                         NULL,
                                         M61_DVFS_TASK_PRIORITY,
                                         dvfs_task_stack,
                                         &dvfs_task_tcb);
    if (dvfs_task_handle == NULL) return -ENOMEM;
    dvfs_initialized = true;
    return 0;
}

int m61_dvfs_set_profile(m61_dvfs_profile_t profile)
{
    uint32_t mhz = dvfs_profile_mhz(profile);

    if (mhz == 0U) return -EINVAL;
    dvfs_manual_profile = profile;
    dvfs_manual_mhz = mhz;
    dvfs_manual_experimental = false;
    dvfs_governor = M61_DVFS_GOVERNOR_MANUAL;
    dvfs_notify_worker();
    return 0;
}

int m61_dvfs_set_custom_frequency(uint32_t mhz, bool allow_experimental)
{
    if (!dvfs_frequency_valid(mhz, allow_experimental)) return -EINVAL;
    dvfs_manual_profile = M61_DVFS_PROFILE_COUNT;
    dvfs_manual_mhz = mhz;
    dvfs_manual_experimental = mhz > M61_DVFS_VALIDATED_MAX_MHZ;
    dvfs_governor = M61_DVFS_GOVERNOR_MANUAL;
    dvfs_notify_worker();
    return 0;
}

int m61_dvfs_set_governor(m61_dvfs_governor_t governor)
{
    if (governor != M61_DVFS_GOVERNOR_MANUAL &&
        governor != M61_DVFS_GOVERNOR_REALTIME) {
        return -EINVAL;
    }
    dvfs_governor = governor;
    dvfs_notify_worker();
    return 0;
}

int m61_dvfs_save_persistent_config(void)
{
    m61_dvfs_persistent_config_t saved;
    EfErrCode err;

    if (dvfs_manual_mhz > M61_DVFS_VALIDATED_MAX_MHZ) return -EPERM;
    saved.magic = M61_DVFS_CONFIG_MAGIC;
    saved.manual_mhz = (uint16_t)dvfs_manual_mhz;
    saved.governor = (uint8_t)dvfs_governor;
    saved.manual_profile = (uint8_t)dvfs_manual_profile;
    err = ef_set_env_blob(M61_DVFS_CONFIG_KEY, &saved, sizeof(saved));
    if (err == EF_NO_ERR) err = ef_save_env();
    if (err != EF_NO_ERR) return -EIO;
    dvfs_persistent_config_loaded = true;
    return 0;
}

int m61_dvfs_clear_persistent_config(void)
{
    EfErrCode err = ef_del_and_save_env(M61_DVFS_CONFIG_KEY);

    if (err != EF_NO_ERR) return -EIO;
    dvfs_persistent_config_loaded = false;
    return 0;
}

int m61_dvfs_request_floor(m61_dvfs_client_t client, uint32_t mhz)
{
    if ((unsigned int)client >= M61_DVFS_CLIENT_COUNT) return -EINVAL;
    if (mhz != 0U && !dvfs_frequency_valid(mhz, false)) return -EINVAL;
    dvfs_client_floor[client] = mhz;
    __asm volatile("" : : : "memory");
    dvfs_notify_worker();
    return 0;
}

int m61_dvfs_request_boost(m61_dvfs_client_t client,
                           uint32_t mhz,
                           uint32_t hold_ms)
{
    TickType_t now;

    if ((unsigned int)client >= M61_DVFS_CLIENT_COUNT) return -EINVAL;
    if (mhz != 0U && !dvfs_frequency_valid(mhz, false)) return -EINVAL;
    if (mhz != 0U && hold_ms == 0U) return -EINVAL;

    now = xPortIsInsideInterrupt() ? xTaskGetTickCountFromISR()
                                   : xTaskGetTickCount();
    dvfs_client_boost[client] = mhz;
    dvfs_client_boost_until[client] =
        mhz == 0U ? now : now + pdMS_TO_TICKS(hold_ms);
    __asm volatile("" : : : "memory");
    dvfs_notify_worker();
    return 0;
}

void m61_dvfs_set_audio_enabled(bool speaker_enabled,
                                bool mic_enabled,
                                bool speaker_stereo)
{
    bool notify = false;

    dvfs_speaker_enabled = speaker_enabled;
    dvfs_mic_enabled = mic_enabled;
    dvfs_speaker_stereo = speaker_stereo;
    if (!speaker_enabled && dvfs_speaker_active) {
        dvfs_speaker_active = false;
        dvfs_speaker_active_until = 0U;
        notify = true;
    }
    if (!mic_enabled && dvfs_mic_active) {
        dvfs_mic_active = false;
        dvfs_mic_active_until = 0U;
        notify = true;
    }
    if (notify) dvfs_notify_worker();
}

void m61_dvfs_note_audio_activity(bool speaker_work, bool mic_work)
{
    TickType_t now;
    uint32_t old_floor;
    uint32_t new_floor;
    TickType_t ignored_expiry = 0U;

    speaker_work = speaker_work && dvfs_speaker_enabled;
    mic_work = mic_work && dvfs_mic_enabled;
    if (!speaker_work && !mic_work) return;
    now = xPortIsInsideInterrupt() ? xTaskGetTickCountFromISR()
                                   : xTaskGetTickCount();
    old_floor = dvfs_audio_activity_floor(now, &ignored_expiry);
    if (speaker_work) {
        dvfs_speaker_active = true;
        dvfs_speaker_active_until =
            now + pdMS_TO_TICKS(M61_DVFS_AUDIO_ACTIVITY_HOLD_MS);
    }
    if (mic_work) {
        dvfs_mic_active = true;
        dvfs_mic_active_until =
            now + pdMS_TO_TICKS(M61_DVFS_AUDIO_ACTIVITY_HOLD_MS);
    }
    ignored_expiry = 0U;
    new_floor = dvfs_audio_activity_floor(now, &ignored_expiry);
    /* A steady stream only extends its deadline. The worker already wakes at
     * the previous deadline, observes the extension, and sleeps again. */
    if (new_floor > old_floor) dvfs_notify_worker();
}

void m61_dvfs_get_status(m61_dvfs_status_t *status)
{
    TickType_t ignored_expiry;
    TickType_t now;

    if (status == NULL) return;
    status->initialized = dvfs_initialized;
    status->persistent_config_loaded = dvfs_persistent_config_loaded;
    status->experimental_requested = dvfs_manual_experimental;
    status->governor = dvfs_governor;
    status->manual_profile = dvfs_manual_profile;
    status->current_mhz = dvfs_current_mhz;
    status->requested_mhz = dvfs_requested_mhz;
    status->manual_mhz = dvfs_manual_mhz;
    now = xTaskGetTickCount();
    status->realtime_floor_mhz = dvfs_realtime_floor(now, &ignored_expiry);
    status->transitions = dvfs_transitions;
    status->transition_failures = dvfs_transition_failures;
    status->last_error = dvfs_last_error;
    status->speaker_active = dvfs_speaker_active &&
                             dvfs_tick_before(now,
                                              dvfs_speaker_active_until);
    status->mic_active = dvfs_mic_active &&
                         dvfs_tick_before(now, dvfs_mic_active_until);
    status->speaker_stereo = dvfs_speaker_stereo;
}
