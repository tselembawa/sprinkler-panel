/*
 * Tests of the alarm engine. The first 16 mirror hmi/tests/test_sim_controller.py one by one,
 * so the C controller is held to exactly what the HMI was built and tested against.
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "modbus_map.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static alm_t ctl;          /* big: keep it out of the stack */
static uint32_t now;

static alm_point_cfg_t point(uint16_t pid, uint8_t cat, uint8_t dt, uint8_t delay, uint8_t channel)
{
    alm_point_cfg_t p;
    memset(&p, 0, sizeof(p));
    p.id = pid;
    p.line = 1;
    p.module = 1;
    p.channel = channel ? channel : (uint8_t)pid;
    p.category = cat;
    p.device_type = dt;
    p.contact_nc = 1;        /* NC: closed = normal, open = active */
    p.delay_s = delay;
    p.enabled = 1;
    p.section = 1;
    strcpy(p.lcd1, "TEST");
    snprintf(p.lcd2, sizeof(p.lcd2), "PUNKT %u", pid);
    return p;
}

/* "ACTIVE" / "NORMAL" / "OPEN" of the simulator, for an NC contact */
enum { NORMAL, ACTIVE, OPEN };
static void set(uint8_t channel, int cond)
{
    uint8_t v = cond == ACTIVE ? MB_IN_OPEN : cond == OPEN ? MB_IN_LINE_OPEN : MB_IN_CLOSED;
    alm_set_input(&ctl, 1, 1, channel, v, now);
}

static void fresh(void)
{
    now = 1000000u;
    alm_init(&ctl, now, 0, NULL, NULL);
}

static void load(const alm_point_cfg_t *p, uint16_t n)
{
    CHECK(alm_load(&ctl, p, n, 0, now, NULL, 0) == 0);
}

static uint8_t st(uint16_t pid)
{
    return ctl.rt[alm_find(&ctl, pid)].state;
}

static alm_status_t status(void)
{
    alm_status_t s;
    alm_status(&ctl, now, &s);
    return s;
}

static int last_event_is(uint8_t type)
{
    alm_event_t e[ALM_EVENT_BUF];
    uint16_t n = alm_events(&ctl, 0, e, ALM_EVENT_BUF);
    return n > 0 && e[n - 1].type == type;
}

static void test_saved_only_when_crc_matches(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    CHECK(ctl.config_crc == alm_config_crc(p, 1));
    CHECK(ctl.config_crc != alm_config_crc(NULL, 0));
    CHECK(ctl.config_version == 1);
}

static void test_invalid_config_is_rejected_and_old_one_kept(void)
{
    fresh();
    alm_point_cfg_t ok[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(ok, 1);
    uint32_t crc = ctl.config_crc, ver = ctl.config_version;
    alm_point_cfg_t bad[] = {point(1, CAT_B, DT_SERVICEVENTIL, 61, 0), point(2, CAT_B, DT_SERVICEVENTIL, 0, 1)};
    alm_cfg_error_t err[8];
    uint16_t n = alm_load(&ctl, bad, 2, 0, now, err, 8);
    int delay = 0, dup = 0;
    for (uint16_t i = 0; i < n && i < 8; i++) {
        delay |= err[i].code == CERR_DELAY_NOT_ALLOWED;
        dup |= err[i].code == CERR_DUPLICATE_ADDRESS;
    }
    CHECK(n >= 2 && delay && dup);
    CHECK(ctl.config_crc == crc && ctl.config_version == ver && ctl.n == 1);
}

static void test_delay_cancelled_and_expired(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_PRESSOSTAT, 40, 0)};
    load(p, 1);
    set(1, ACTIVE);
    CHECK(st(1) == PS_PENDING);
    now += 39000;
    alm_tick(&ctl, now);
    CHECK(st(1) == PS_PENDING);
    set(1, NORMAL);
    CHECK(st(1) == PS_NORMAL);
    set(1, ACTIVE);
    now += 41000;
    alm_tick(&ctl, now);
    CHECK(st(1) == PS_ALARM);
    CHECK(status().relays & ALM_K1_A);
}

static void test_fire_alarm_latches_until_reset_with_key(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0)};
    load(p, 1);
    set(1, ACTIVE);
    set(1, NORMAL);
    CHECK(st(1) == PS_LATCHED);
    CHECK(status().relays & ALM_K1_A);
    CHECK(alm_reset(&ctl, 0, now, NULL, NULL, 0, NULL) == ALM_ACCESS_DENIED);   /* no key */
    alm_set_key(&ctl, 1);
    CHECK(alm_reset(&ctl, 0, now, NULL, NULL, 0, NULL) == ALM_OK);
    CHECK(st(1) == PS_NORMAL);
    CHECK(!(status().relays & ALM_K1_A));
}

static void test_reset_does_not_clear_active_fire_alarm(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_PRESSOSTAT, 0, 0)};
    load(p, 1);
    set(1, ACTIVE);
    alm_set_key(&ctl, 1);
    uint16_t ids[4], nrem = 0, nreset = 9;
    CHECK(alm_reset(&ctl, 0, now, &nreset, ids, 4, &nrem) == ALM_OK);
    CHECK(nrem == 1 && ids[0] == 1 && nreset == 0);
    CHECK(st(1) == PS_ALARM);
}

static void test_non_latching_point_resets_itself(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    set(1, ACTIVE);
    CHECK(st(1) == PS_ALARM);
    set(1, NORMAL);
    CHECK(st(1) == PS_NORMAL);
}

static void test_acknowledged_b_alarm_becomes_c_and_rearms_k2(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0), point(2, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 2);
    set(1, ACTIVE);
    alm_status_t s = status();
    CHECK((s.relays & ALM_K2_B) && !(s.relays & ALM_K4_C));
    CHECK(s.led_yellow_b == LED_FLASH);

    alm_ack(&ctl, 0, now);
    s = status();
    CHECK(!(s.relays & ALM_K2_B) && (s.relays & ALM_K4_C));
    CHECK(s.led_yellow_b == LED_STEADY);
    CHECK(alm_display_category(&ctl, alm_find(&ctl, 1)) == 'C');

    set(2, ACTIVE);                       /* a new B-larm is transmitted again on K2 */
    s = status();
    CHECK((s.relays & ALM_K2_B) && (s.relays & ALM_K4_C));

    set(1, NORMAL);                       /* the C-larm disappears with its cause */
    CHECK(st(1) == PS_NORMAL);
}

static void test_a_alarm_never_becomes_c(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0)};
    load(p, 1);
    set(1, ACTIVE);
    alm_ack(&ctl, 0, now);
    CHECK(alm_display_category(&ctl, alm_find(&ctl, 1)) == 'A');
    CHECK(!(status().relays & ALM_K4_C));
}

static void test_line_fault_drops_k3_and_keeps_alarm(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0)};
    load(p, 1);
    CHECK(status().relays & ALM_K3_FAULT_OK);
    set(1, ACTIVE);
    set(1, OPEN);
    int i = alm_find(&ctl, 1);
    CHECK(ctl.rt[i].fault == PF_OPEN);
    CHECK(ctl.rt[i].state == PS_ALARM);   /* an existing alarm is never hidden by a fault */
    CHECK(!(status().relays & ALM_K3_FAULT_OK));
}

static void test_module_lost_is_a_fault(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    alm_set_module_lost(&ctl, 1, 1, 1, now);
    CHECK(ctl.rt[alm_find(&ctl, 1)].fault == PF_MODULE_LOST);
    CHECK(!(status().relays & ALM_K3_FAULT_OK));
    alm_set_module_lost(&ctl, 1, 1, 0, now);
    CHECK(status().relays & ALM_K3_FAULT_OK);
}

static void test_disabled_point_gives_no_alarm(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    alm_set_key(&ctl, 1);
    CHECK(alm_disable(&ctl, 1, 1, 0, now) == ALM_OK);
    set(1, ACTIVE);
    CHECK(st(1) == PS_NORMAL);
    CHECK(status().disabled == 1);
    CHECK(ctl.config_crc == alm_config_crc(p, 1));   /* runtime only: CRC unchanged */
    CHECK(alm_disable(&ctl, 99, 1, 0, now) == ALM_UNKNOWN_POINT);
}

static void test_events_have_increasing_sequence(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    set(1, ACTIVE);
    alm_ack(&ctl, 0, now);
    alm_event_t e[ALM_EVENT_BUF];
    uint16_t n = alm_events(&ctl, 0, e, ALM_EVENT_BUF);
    CHECK(n >= 3);
    for (uint16_t i = 1; i < n; i++) {
        CHECK(e[i].seq == e[i - 1].seq + 1u);
    }
    alm_event_t later[4];
    CHECK(alm_events(&ctl, e[n - 2].seq, later, 4) == 1 && later[0].seq == e[n - 1].seq);
}

static void test_set_time_requires_key_and_is_logged(void)
{
    fresh();
    CHECK(alm_set_time(&ctl, 1800000000u, 0, now) == ALM_ACCESS_DENIED);
    alm_set_key(&ctl, 1);
    CHECK(alm_set_time(&ctl, 1800000000u, 0, now) == ALM_OK);
    CHECK(status().time == 1800000000u);
    alm_event_t e[ALM_EVENT_BUF];
    uint16_t n = alm_events(&ctl, 0, e, ALM_EVENT_BUF);
    CHECK(e[n - 1].type == EVT_TIME_SET && e[n - 1].time == 1800000000u);
    CHECK(alm_set_time(&ctl, 5, 0, now) == ALM_BAD_TIME);
    now += 5000;
    CHECK(status().time == 1800000005u);   /* the clock runs on */
}

static void test_silence_sounders_and_resound_on_new_alarm(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0), point(2, CAT_A, DT_PRESSOSTAT, 0, 0)};
    load(p, 2);
    set(1, ACTIVE);
    CHECK(status().sounders == SOUNDERS_SOUNDING);
    CHECK(alm_silence(&ctl, 1, 0, now) == ALM_ACCESS_DENIED);
    alm_set_key(&ctl, 1);
    CHECK(alm_silence(&ctl, 1, 0, now) == ALM_OK);
    CHECK(status().sounders == SOUNDERS_SILENCED);
    CHECK(status().relays & ALM_K1_A);   /* silencing never stops the transmission */
    set(2, ACTIVE);                      /* a new fire alarm */
    CHECK(status().sounders == SOUNDERS_SOUNDING);
    alm_event_t e[ALM_EVENT_BUF];
    uint16_t n = alm_events(&ctl, 0, e, ALM_EVENT_BUF);
    int resound = 0;
    for (uint16_t i = 0; i < n; i++) {
        resound |= e[i].type == EVT_RESOUND;
    }
    CHECK(resound);
}

static void test_service_mode_times_out(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    alm_set_key(&ctl, 1);
    CHECK(alm_service(&ctl, 30, 0, now) == ALM_OK);
    alm_status_t s = status();
    CHECK(s.service_active && s.led_yellow_service && !s.led_green_power);
    now += 29u * 60000u;
    alm_tick(&ctl, now);
    CHECK(status().service_remaining_s == 60);
    now += 61000u;
    alm_tick(&ctl, now);
    CHECK(!status().service_active && status().led_green_power);
    CHECK(last_event_is(EVT_SERVICE_OFF));
    alm_service(&ctl, 30, 0, now);       /* alarms detected in service mode, but not transmitted */
    set(1, ACTIVE);
    s = status();
    CHECK((s.relays_logical & ALM_K2_B) && !(s.relays & ALM_K2_B));
    CHECK(alm_service(&ctl, 481, 0, now) == ALM_BAD_DURATION);
}

static void test_service_mode_blocks_all_transmission_and_releases_at_end(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0), point(2, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 2);
    alm_set_key(&ctl, 1);
    alm_service(&ctl, 60, 0, now);
    set(1, ACTIVE);                      /* fire alarm while the technician works */
    set(2, OPEN);                        /* and a cut cable */
    alm_status_t s = status();
    CHECK(s.transmission_blocked);
    CHECK(s.relays == ALM_K3_FAULT_OK);
    CHECK((s.relays_logical & ALM_K1_A) && !(s.relays_logical & ALM_K3_FAULT_OK));
    CHECK(s.led_red_a && s.sounders == SOUNDERS_SOUNDING);   /* still shown and heard locally */
    alm_service(&ctl, 0, 0, now);        /* service ends: the active alarm goes out at once */
    s = status();
    CHECK((s.relays & ALM_K1_A) && !(s.relays & ALM_K3_FAULT_OK) && !s.transmission_blocked);
}

/* ---- C-only: physical inputs, NO/NC, config reload ------------------------------------- */
static void test_no_and_nc_contacts(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0), point(2, CAT_A, DT_PRESSOSTAT, 0, 0)};
    p[1].contact_nc = 0;                 /* pressostat: NO */
    load(p, 2);
    alm_set_input(&ctl, 1, 1, 1, MB_IN_CLOSED, now);
    alm_set_input(&ctl, 1, 1, 2, MB_IN_OPEN, now);
    CHECK(st(1) == PS_NORMAL && st(2) == PS_NORMAL);
    alm_set_input(&ctl, 1, 1, 2, MB_IN_CLOSED, now);     /* water: the NO contact closes */
    CHECK(st(2) == PS_ALARM);
    alm_set_input(&ctl, 1, 1, 1, MB_IN_LINE_SHORT, now);
    CHECK(ctl.rt[alm_find(&ctl, 1)].fault == PF_SHORT);
}

static void test_reload_keeps_state_of_unchanged_points(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_FLODESVAKT, 0, 0), point(2, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 2);
    set(1, ACTIVE);
    set(1, NORMAL);
    CHECK(st(1) == PS_LATCHED);
    strcpy(p[1].lcd2, "ANDRAD");         /* edit point 2 only */
    load(p, 2);
    CHECK(st(1) == PS_LATCHED);          /* the fire alarm is not lost by saving a change */
    CHECK(ctl.config_version == 2);
}

static void test_hmi_lost_gives_fault_after_120_minutes(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_B, DT_SERVICEVENTIL, 0, 0)};
    load(p, 1);
    alm_set_hmi_lost(&ctl, 1, now);
    CHECK(last_event_is(EVT_HMI_LOST));
    CHECK(status().hmi_lost && (status().relays & ALM_K3_FAULT_OK));   /* not a fault yet */
    now += 119u * 60000u;
    CHECK(status().relays & ALM_K3_FAULT_OK);
    now += 61000u;
    CHECK(!(status().relays & ALM_K3_FAULT_OK) && status().fault == 1);
    alm_set_key(&ctl, 1);
    alm_service(&ctl, 30, 0, now);                 /* not in service mode */
    CHECK(status().relays_logical & ALM_K3_FAULT_OK);
    alm_service(&ctl, 0, 0, now);
    alm_set_hmi_lost(&ctl, 0, now);
    CHECK(last_event_is(EVT_HMI_BACK) && (status().relays & ALM_K3_FAULT_OK));
}

static void test_sync_time_only_once_without_key(void)
{
    fresh();
    CHECK(alm_sync_time(&ctl, 1800000000u, now) == ALM_OK);
    CHECK(status().time == 1800000000u);
    CHECK(alm_sync_time(&ctl, 1900000000u, now) == ALM_ACCESS_DENIED);   /* already valid */
    CHECK(status().time == 1800000000u);
}

static void test_pending_left_and_condition(void)
{
    fresh();
    alm_point_cfg_t p[] = {point(1, CAT_A, DT_PRESSOSTAT, 30, 0)};
    load(p, 1);
    CHECK(alm_pending_left_s(&ctl, 0, now) == -1 && alm_condition(&ctl, 0) == 0);
    set(1, ACTIVE);
    now += 10000u;
    CHECK(alm_pending_left_s(&ctl, 0, now) == 20 && alm_condition(&ctl, 0) == 1);
}

int main(void)
{
    test_saved_only_when_crc_matches();
    test_invalid_config_is_rejected_and_old_one_kept();
    test_delay_cancelled_and_expired();
    test_fire_alarm_latches_until_reset_with_key();
    test_reset_does_not_clear_active_fire_alarm();
    test_non_latching_point_resets_itself();
    test_acknowledged_b_alarm_becomes_c_and_rearms_k2();
    test_a_alarm_never_becomes_c();
    test_line_fault_drops_k3_and_keeps_alarm();
    test_module_lost_is_a_fault();
    test_disabled_point_gives_no_alarm();
    test_events_have_increasing_sequence();
    test_set_time_requires_key_and_is_logged();
    test_silence_sounders_and_resound_on_new_alarm();
    test_service_mode_times_out();
    test_service_mode_blocks_all_transmission_and_releases_at_end();
    test_no_and_nc_contacts();
    test_reload_keeps_state_of_unchanged_points();
    test_hmi_lost_gives_fault_after_120_minutes();
    test_sync_time_only_once_without_key();
    test_pending_left_and_condition();
    printf("%s: %d checks, %d failures (21 tests)\n", failures ? "FAILED" : "alarm tests passed", checks, failures);
    return failures ? 1 : 0;
}
