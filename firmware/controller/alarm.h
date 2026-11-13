/*
 * Alarm engine of the Main Controller.
 *
 * C port of the simulated controller (hmi/app/sim_controller.py), which the HMI has been
 * developed against: same state machine, same decisions, same tests (tests/test_alarm.c
 * mirrors hmi/tests/test_sim_controller.py).
 *
 *   NORMAL -> PENDING (delay <= 60 s) -> ALARM -> ACKED -> (cause gone) NORMAL
 *   latching device types (pressostat, flodesvakt): cause gone -> LATCHED until Aterstall
 *   C-larm = acknowledged B-larm whose cause is still there (K2 free again, K4 on)
 *   A line fault never hides an existing alarm; module lost = fault for all its points.
 *   Service mode: everything detected and shown, nothing transmitted (K1/K2/K4 idle, K3 OK).
 *
 * No hardware and no clock of its own: the caller passes the time (milliseconds since start)
 * to every function, so the same code runs in the STM32 and in the PC tests.
 */
#ifndef ALARM_H
#define ALARM_H

#include <stdint.h>

#define ALM_MAX_POINTS   256u
#define ALM_EVENT_BUF    128u
#define ALM_MAX_ADDR     247u   /* module addresses 1..247 (line ranges 1-60/61-120/... checked later) */
#define ALM_MAX_DELAY_S  60u
#define ALM_LCD_LEN      20u
#define ALM_HMI_FAULT_MS (120u * 60u * 1000u)   /* HMI lost this long -> system fault (decided) */

typedef enum { CAT_A = 0, CAT_B, CAT_DRIFT, CAT_FAULT, CAT_COUNT } alm_cat_t;

/* same order as DEVICE_TYPES in hmi/app/protocol.py */
typedef enum {
    DT_SERVICEVENTIL = 0, DT_UNDERSEKTIONSVENTIL, DT_LARMVENTIL, DT_PRESSOSTAT, DT_FLODESVAKT,
    DT_PUMP_RUN, DT_PUMP_FAULT, DT_TEMP, DT_GENERIC, DT_COUNT
} alm_devtype_t;

typedef enum { PS_NORMAL = 0, PS_PENDING, PS_ALARM, PS_ACKED, PS_LATCHED } alm_pstate_t;
typedef enum { PF_NONE = 0, PF_OPEN, PF_SHORT, PF_MODULE_LOST } alm_fault_t;

typedef struct {
    uint16_t id;           /* 1..999 */
    uint8_t line;          /* 1..4 */
    uint8_t module;        /* module address */
    uint8_t channel;       /* input 1..4 */
    uint8_t category;      /* alm_cat_t */
    uint8_t device_type;   /* alm_devtype_t */
    uint8_t contact_nc;    /* 1 = NC (closed = normal), 0 = NO (open = normal) */
    uint8_t delay_s;       /* 0..60 */
    uint8_t enabled;
    uint16_t section;
    char lcd1[ALM_LCD_LEN + 1];
    char lcd2[ALM_LCD_LEN + 1];
} alm_point_cfg_t;

typedef struct {
    uint8_t state;          /* alm_pstate_t */
    uint8_t acked;
    uint8_t fault;          /* alm_fault_t */
    uint8_t disabled;       /* runtime "Frankoppla" (not part of the configuration) */
    uint8_t fault_unacked;  /* line fault not acknowledged yet (cleared when it goes away) */
    uint32_t pending_since_ms;
    uint32_t changed_ms;
} alm_point_rt_t;

typedef enum {
    EVT_STARTUP = 0, EVT_CONFIG_ACTIVATED, EVT_DELAY_STARTED, EVT_DELAY_CANCELLED,
    EVT_ALARM_ON, EVT_ALARM_CLEARED, EVT_LINE_OPEN, EVT_LINE_SHORT, EVT_LINE_OK,
    EVT_MODULE_LOST, EVT_MODULE_BACK, EVT_ACK, EVT_RESET, EVT_SILENCE, EVT_UNSILENCE,
    EVT_RESOUND, EVT_SERVICE_ON, EVT_SERVICE_OFF, EVT_POINT_DISABLED, EVT_POINT_ENABLED,
    EVT_TIME_SET, EVT_HMI_LOST, EVT_HMI_BACK, EVT_COUNT
} alm_evtype_t;

typedef enum { SRC_SYSTEM = 0, SRC_POINT, SRC_MODULE } alm_srckind_t;

typedef struct {
    uint32_t seq;
    uint32_t time;        /* Unix seconds (controller clock) */
    uint8_t type;         /* alm_evtype_t */
    uint8_t kind;         /* alm_srckind_t */
    uint8_t user;
    uint16_t source_id;   /* point id, line*1000+module, or 0 */
    int32_t old_v;        /* point events: alm_pstate_t; TIME_SET: old Unix time */
    int32_t new_v;        /* SERVICE_ON: minutes; SERVICE_OFF: 0 timeout / 1 manual; CONFIG: version */
} alm_event_t;

typedef void (*alm_event_fn)(void *ctx, const alm_event_t *ev);

typedef enum {
    ALM_OK = 0, ALM_ACCESS_DENIED, ALM_BAD_DURATION, ALM_UNKNOWN_POINT, ALM_BAD_TIME
} alm_result_t;

/* configuration validation errors (same codes as the HMI shows) */
typedef enum {
    CERR_OUT_OF_RANGE = 1, CERR_DUPLICATE_ID, CERR_DUPLICATE_ADDRESS, CERR_INVALID,
    CERR_DELAY_NOT_ALLOWED, CERR_TOO_LONG, CERR_TOO_MANY
} alm_cfg_err_code_t;

typedef enum {
    CF_ID = 0, CF_LINE, CF_MODULE, CF_CHANNEL, CF_CATEGORY, CF_DEVICE_TYPE, CF_DELAY, CF_LCD1, CF_LCD2
} alm_cfg_field_t;

typedef struct {
    uint16_t point_id;
    uint8_t field;        /* alm_cfg_field_t */
    uint8_t code;         /* alm_cfg_err_code_t */
} alm_cfg_error_t;

/* relay bits */
#define ALM_K1_A        0x01u
#define ALM_K2_B        0x02u
#define ALM_K3_FAULT_OK 0x04u   /* fail-safe: energized when everything is OK */
#define ALM_K4_C        0x08u

typedef enum { SOUNDERS_OFF = 0, SOUNDERS_SOUNDING, SOUNDERS_SILENCED } alm_sounders_t;
typedef enum { LED_OFF = 0, LED_STEADY, LED_FLASH } alm_led_t;

typedef struct {
    uint16_t a, b, c, drift, fault, disabled;
    uint8_t relays_logical;        /* what K1-K4 would be without service mode */
    uint8_t relays;                /* what they are */
    uint8_t transmission_blocked;
    uint8_t led_red_a, led_yellow_b, led_yellow_fault, led_green_drift, led_green_power,
            led_yellow_service;
    uint8_t buzzer;                /* panel buzzer: something unacknowledged */
    uint8_t sounders;              /* alm_sounders_t, external sounders */
    uint8_t silenced;
    uint8_t service_active;
    uint32_t service_remaining_s;
    uint8_t key_tech;
    uint32_t time;
    uint32_t uptime_s;
    uint32_t config_version, config_crc;
    uint32_t last_seq;
    uint16_t lost_modules;
    uint8_t hmi_lost;              /* no contact with the Raspberry Pi */
    uint8_t no_config;             /* no configuration yet: the panel supervises nothing */
} alm_status_t;

typedef struct {
    alm_point_cfg_t cfg[ALM_MAX_POINTS];
    alm_point_rt_t rt[ALM_MAX_POINTS];
    uint16_t n;
    uint32_t config_version, config_crc;
    uint8_t configured;            /* 0 until a configuration was loaded: K3 off (Fel) */

    uint8_t inputs[ALM_MAX_ADDR + 1u][4];   /* last MB_IN_* value per module address / input */
    uint8_t lost[(ALM_MAX_ADDR + 8u) / 8u]; /* module lost, bit per address */
    uint8_t lost_unacked[(ALM_MAX_ADDR + 8u) / 8u]; /* ... and not acknowledged yet */
    uint8_t lost_line[ALM_MAX_ADDR + 1u];
    uint16_t lost_count;

    uint8_t key_tech, silenced, service_active;
    uint32_t service_until_ms;
    uint32_t service_end_unix;     /* end of service in real time, 0 while the clock is unknown */
    uint8_t service_restored;      /* came back after a restart: corrected when the clock is known */
    uint8_t hmi_lost;
    uint32_t hmi_lost_since_ms;

    uint32_t now_ms, started_ms;
    /* The HAL millisecond counter is 32 bits and starts again from 0 after 49.7 days.
       mono_ms keeps counting (64 bits): uptime and the clock are taken from it. */
    uint64_t mono_ms;                       /* ms since alm_init, never wraps */
    uint32_t mono_raw;                      /* the 32-bit now_ms when mono_ms was last updated */
    int64_t time_offset;                    /* Unix time = time_offset + mono_ms / 1000 */

    uint32_t seq;
    alm_event_t events[ALM_EVENT_BUF];
    uint16_t ev_head, ev_count;
    alm_event_fn on_event;
    void *ctx;
} alm_t;

void alm_init(alm_t *a, uint32_t now_ms, uint32_t start_seq, alm_event_fn on_event, void *ctx);

/* Validate and activate a configuration. Returns the number of errors (0 = activated);
   on errors the active configuration stays as it was. */
uint16_t alm_load(alm_t *a, const alm_point_cfg_t *pts, uint16_t n, uint8_t user, uint32_t now_ms,
                  alm_cfg_error_t *errors, uint16_t max_errors);

/* Only check a configuration (same checks as alm_load). Returns the number of errors. */
uint16_t alm_validate(const alm_point_cfg_t *pts, uint16_t n, alm_cfg_error_t *errors, uint16_t max_errors);

/* CRC-32 of a configuration in its canonical binary form (points in id order).
   Record = 52 bytes, little endian: id(2) line module channel category device_type contact_nc
   delay_s enabled section(2) lcd1[20] lcd2[20] (text padded with 0). The Pi computes the same. */
uint32_t alm_config_crc(const alm_point_cfg_t *pts, uint16_t n);

/* From the module protocol: input value (MB_IN_*) of module `addr` input `channel` (1..4). */
void alm_set_input(alm_t *a, uint8_t line, uint8_t addr, uint8_t channel, uint8_t mb_in, uint32_t now_ms);
void alm_set_module_lost(alm_t *a, uint8_t line, uint8_t addr, int lost, uint32_t now_ms);
void alm_set_key(alm_t *a, int tech);
/* Contact with the Raspberry Pi lost / back (decided: event at once, system fault after 120 min,
   not in service mode). */
void alm_set_hmi_lost(alm_t *a, int lost, uint32_t now_ms);
void alm_tick(alm_t *a, uint32_t now_ms);

/* commands from the panel (Pi or local buttons) */
uint16_t alm_ack(alm_t *a, uint8_t user, uint32_t now_ms);
alm_result_t alm_reset(alm_t *a, uint8_t user, uint32_t now_ms, uint16_t *reset_count,
                       uint16_t *remaining_ids, uint16_t max_remaining, uint16_t *remaining_count);
alm_result_t alm_silence(alm_t *a, int silence, uint8_t user, uint32_t now_ms);
alm_result_t alm_service(alm_t *a, uint16_t minutes, uint8_t user, uint32_t now_ms);
alm_result_t alm_disable(alm_t *a, uint16_t point_id, int disabled, uint8_t user, uint32_t now_ms);
alm_result_t alm_set_time(alm_t *a, uint32_t unix_time, uint8_t user, uint32_t now_ms);
/* Set the clock without the key, only while it has never been set (no RTC yet: after power-up
   the Pi gives the time). Returns ALM_ACCESS_DENIED if the clock is already valid. */
alm_result_t alm_sync_time(alm_t *a, uint32_t unix_time, uint32_t now_ms);

void alm_status(const alm_t *a, uint32_t now_ms, alm_status_t *s);
uint32_t alm_time(const alm_t *a, uint32_t now_ms);
int alm_clock_valid(const alm_t *a, uint32_t now_ms);

/* What survives a restart of the controller (kept in flash by app.c, decided 2026-11-13):
   service mode and the "Frånkopplade" points, until someone changes them. */
typedef struct {
    uint8_t service_active;
    uint32_t service_left_s;       /* when it was saved */
    uint32_t service_end_unix;     /* 0 = the clock was unknown */
    uint16_t n_disabled;
    uint16_t disabled[ALM_MAX_POINTS];   /* point ids */
} alm_persist_t;
void alm_persist_get(const alm_t *a, uint32_t now_ms, alm_persist_t *p);
/* After alm_load at start-up. Service comes back with the time it had left; when the clock
   becomes known (Pi, later RTC) the time the panel was off is taken away - and if it has run out,
   service ends at once. */
void alm_persist_restore(alm_t *a, const alm_persist_t *p, uint32_t now_ms);
/* bytes for the flash (format 1, little endian): ver, service, n, left_s, end_unix, ids */
#define ALM_PERSIST_MAX (12u + 2u * ALM_MAX_POINTS)
uint16_t alm_persist_pack(const alm_persist_t *p, uint8_t *buf);
int alm_persist_unpack(alm_persist_t *p, const uint8_t *buf, uint16_t len);   /* 0 = ok */

/* point access */
int alm_find(const alm_t *a, uint16_t point_id);              /* index or -1 */
uint8_t alm_display_category(const alm_t *a, int index);      /* 'A', 'B', 'C', 'D' (drift), 'F' */
int alm_is_enabled(const alm_t *a, int index);
uint8_t alm_condition(const alm_t *a, int index);             /* 0 NORMAL 1 ACTIVE 2 OPEN 3 SHORT */
int16_t alm_pending_left_s(const alm_t *a, int index, uint32_t now_ms);   /* -1 when not pending */

/* events with seq > from_seq, oldest first; returns how many were copied */
uint16_t alm_events(const alm_t *a, uint32_t from_seq, alm_event_t *out, uint16_t max);

const char *alm_event_name(uint8_t type);
const char *alm_state_name(uint8_t state);
const char *alm_fault_name(uint8_t fault);

#endif /* ALARM_H */
