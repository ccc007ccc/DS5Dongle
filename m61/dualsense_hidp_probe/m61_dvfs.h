#ifndef M61_DVFS_H
#define M61_DVFS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M61_DVFS_DEFAULT_MHZ 320U
#define M61_DVFS_VALIDATED_MAX_MHZ 400U
#define M61_DVFS_EXPERIMENTAL_MAX_MHZ 480U

typedef enum {
    M61_DVFS_PROFILE_ECO = 0,
    M61_DVFS_PROFILE_BALANCED,
    M61_DVFS_PROFILE_PERFORMANCE,
    M61_DVFS_PROFILE_COUNT,
} m61_dvfs_profile_t;

typedef enum {
    M61_DVFS_GOVERNOR_MANUAL = 0,
    M61_DVFS_GOVERNOR_REALTIME,
} m61_dvfs_governor_t;

/* Independent clients can publish persistent floors or short boost requests.
 * The realtime governor always selects the maximum active request. */
typedef enum {
    M61_DVFS_CLIENT_AUDIO = 0,
    M61_DVFS_CLIENT_BLUETOOTH,
    M61_DVFS_CLIENT_USB,
    M61_DVFS_CLIENT_APPLICATION,
    M61_DVFS_CLIENT_COUNT,
} m61_dvfs_client_t;

typedef struct {
    bool initialized;
    bool persistent_config_loaded;
    bool experimental_requested;
    m61_dvfs_governor_t governor;
    m61_dvfs_profile_t manual_profile;
    uint32_t current_mhz;
    uint32_t requested_mhz;
    uint32_t manual_mhz;
    uint32_t realtime_floor_mhz;
    uint32_t transitions;
    uint32_t transition_failures;
    int32_t last_error;
    bool speaker_active;
    bool mic_active;
    bool speaker_stereo;
} m61_dvfs_status_t;

/* Initialize after the Bluetooth controller has completed clock setup.
 * Repeated calls are harmless. boot_mhz=0 selects the 320 MHz default. */
int m61_dvfs_init(uint32_t boot_mhz);

uint32_t m61_dvfs_profile_frequency(m61_dvfs_profile_t profile);
const char *m61_dvfs_profile_name(m61_dvfs_profile_t profile);
const char *m61_dvfs_governor_name(m61_dvfs_governor_t governor);

/* Manual controls are asynchronous: only the DVFS worker touches the PLL.
 * Requests up to 400 MHz use the project-validated range. Frequencies above
 * 400 MHz require allow_experimental=true. */
int m61_dvfs_set_profile(m61_dvfs_profile_t profile);
int m61_dvfs_set_custom_frequency(uint32_t mhz, bool allow_experimental);
int m61_dvfs_set_governor(m61_dvfs_governor_t governor);

/* Flash persistence is explicit to avoid wear from interactive/Web sliders.
 * Experimental (>400 MHz) settings are deliberately never persisted. */
int m61_dvfs_save_persistent_config(void);
int m61_dvfs_clear_persistent_config(void);

/* O(1), allocation-free workload APIs suitable for normal task or interrupt
 * context. A floor of zero releases that client's persistent request. */
int m61_dvfs_request_floor(m61_dvfs_client_t client, uint32_t mhz);
int m61_dvfs_request_boost(m61_dvfs_client_t client,
                           uint32_t mhz,
                           uint32_t hold_ms);
void m61_dvfs_set_audio_enabled(bool speaker_enabled,
                                bool mic_enabled,
                                bool speaker_stereo);
/* Refresh activity leases from the codec task. Steady-state refreshes only
 * update timestamps and do not wake the DVFS worker. */
void m61_dvfs_note_audio_activity(bool speaker_work, bool mic_work);

uint32_t m61_dvfs_measure_cpu_mhz(void);
void m61_dvfs_get_status(m61_dvfs_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
