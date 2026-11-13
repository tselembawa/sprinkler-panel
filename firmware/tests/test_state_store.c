/*
 * Service mode and Frånkopplade points survive a restart (decided 2026-11-13).
 *   gcc -std=c11 -Wall -Wextra -Werror -Icommon -Icontroller -o /tmp/test_state_store \
 *       tests/test_state_store.c controller/state_store.c controller/alarm.c && /tmp/test_state_store
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "modbus_map.h"
#include "state_store.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- RAM flash with the STM32 rules -------------------------------------------------------- */
#define PAGE 2048u
static uint8_t flash[2 * PAGE] __attribute__((aligned(8)));
static int erases[2], fail_after = -1, programs;

static int f_erase(void *ctx, uint8_t page)
{
    (void)ctx;
    memset(&flash[page * PAGE], 0xFF, PAGE);
    erases[page]++;
    return 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    if ((off & 7u) || (len & 7u) || off + len > sizeof(flash)) return -1;
    for (uint32_t i = 0; i < len; i++) {
        if (flash[off + i] != 0xFF) return -1;
    }
    if (fail_after >= 0 && programs >= fail_after) return -1;     /* power cut */
    memcpy(&flash[off], d, len);
    programs++;
    return 0;
}

static const st_flash_t fl = {flash, PAGE, f_erase, f_program, NULL};

static void test_store_log(void)
{
    memset(flash, 0xFF, sizeof(flash));
    state_store_t s;
    uint8_t buf[600];
    CHECK(state_store_init(&s, &fl, buf, sizeof(buf)) == -1);          /* new controller */

    uint8_t a[20] = "first record", b[30] = "second, a bit longer";
    CHECK(state_store_save(&s, a, sizeof(a)) == 0);
    CHECK(state_store_save(&s, b, sizeof(b)) == 0);
    state_store_t r;
    CHECK(state_store_init(&r, &fl, buf, sizeof(buf)) == (int)sizeof(b) && memcmp(buf, b, sizeof(b)) == 0);

    /* many saves: pages are erased only when full, the newest always wins */
    uint8_t rec[40];
    for (int i = 0; i < 1000; i++) {
        memset(rec, i & 0xFF, sizeof(rec));
        rec[0] = (uint8_t)(i >> 8);
        CHECK(state_store_save(&s, rec, sizeof(rec)) == 0);
    }
    CHECK(state_store_init(&r, &fl, buf, sizeof(buf)) == (int)sizeof(rec) && buf[1] == (999 & 0xFF) && buf[0] == 3);
    CHECK(erases[0] + erases[1] < 1000 / 30);                         /* ~42 records per page */

    /* power cut in the middle of a save: the previous record is still there */
    memset(rec, 0x77, sizeof(rec));
    programs = 0;
    fail_after = 1;                                                    /* data written, header not */
    CHECK(state_store_save(&s, rec, sizeof(rec)) == -1);
    fail_after = -1;
    CHECK(state_store_init(&r, &fl, buf, sizeof(buf)) == (int)sizeof(rec) && buf[1] == (999 & 0xFF));
    /* and saving goes on after the restart */
    memset(rec, 0x55, sizeof(rec));
    CHECK(state_store_save(&r, rec, sizeof(rec)) == 0);
    CHECK(state_store_init(&s, &fl, buf, sizeof(buf)) == (int)sizeof(rec) && buf[5] == 0x55);

    /* power cut just after switching page (header written, no record yet): the old page counts */
    for (int i = 0; i < 200; i++) {
        state_store_t t;
        CHECK(state_store_init(&t, &fl, buf, sizeof(buf)) >= 0);
        if (t.write_off + 8u + 40u > PAGE) {                            /* next save switches page */
            memset(rec, 0x99, sizeof(rec));
            programs = 0;
            fail_after = 1;                                            /* page header only */
            CHECK(state_store_save(&t, rec, sizeof(rec)) == -1);
            fail_after = -1;
            CHECK(state_store_init(&t, &fl, buf, sizeof(buf)) == (int)sizeof(rec) && buf[0] != 0x99);
            CHECK(state_store_save(&t, rec, sizeof(rec)) == 0);
            CHECK(state_store_init(&t, &fl, buf, sizeof(buf)) == (int)sizeof(rec) && buf[0] == 0x99);
            break;
        }
        memset(rec, 0x11, sizeof(rec));
        CHECK(state_store_save(&t, rec, sizeof(rec)) == 0);
    }
}

/* ---- the panel: service and Frånkopplade over a restart ------------------------------------ */
static alm_t ctl;
static alm_persist_t ps;
static uint8_t packed[ALM_PERSIST_MAX];

static alm_point_cfg_t pts[3];

static void boot(uint32_t now)
{
    alm_init(&ctl, now, 0, NULL, NULL);
    for (int i = 0; i < 3; i++) {
        memset(&pts[i], 0, sizeof(pts[i]));
        pts[i].id = (uint16_t)(i + 1);
        pts[i].line = 1;
        pts[i].module = 1;
        pts[i].channel = (uint8_t)(i + 1);
        pts[i].category = CAT_B;
        pts[i].device_type = DT_SERVICEVENTIL;
        pts[i].contact_nc = 1;
        pts[i].enabled = 1;
        pts[i].section = 1;
        strcpy(pts[i].lcd1, "TEST");
    }
    CHECK(alm_load(&ctl, pts, 3, 0, now, NULL, 0) == 0);
}

/* save before "power off", restore after: through the byte format, as in app.c */
static void power_cycle(uint32_t off_ms, uint32_t new_now)
{
    alm_persist_get(&ctl, ctl.now_ms, &ps);
    uint16_t n = alm_persist_pack(&ps, packed);
    (void)off_ms;
    boot(new_now);
    alm_persist_t back;
    CHECK(alm_persist_unpack(&back, packed, n) == 0);
    alm_persist_restore(&ctl, &back, new_now);
}

static uint32_t left_min(uint32_t now)
{
    alm_status_t s;
    alm_status(&ctl, now, &s);
    return s.service_active ? (s.service_remaining_s + 30u) / 60u : 0u;
}

static void test_service_and_disabled_over_restart(void)
{
    const uint32_t T = 1794000000u;      /* Unix time given by the Pi */
    uint32_t now = 5000;
    boot(now);
    alm_sync_time(&ctl, T, now);
    alm_set_key(&ctl, 1);
    CHECK(alm_service(&ctl, 60, 1, now) == ALM_OK);
    CHECK(alm_disable(&ctl, 2, 1, 1, now) == ALM_OK);
    alm_set_key(&ctl, 0);

    /* 30 minutes later the panel goes off for 20 minutes */
    now += 30u * 60000u;
    alm_tick(&ctl, now);
    CHECK(left_min(now) == 30);
    power_cycle(20u * 60000u, 100);
    now = 100;
    CHECK(!alm_clock_valid(&ctl, now));
    CHECK(left_min(now) == 30);                                        /* as if it had 30 left */
    CHECK(!alm_is_enabled(&ctl, alm_find(&ctl, 2)) && alm_is_enabled(&ctl, alm_find(&ctl, 1)));
    alm_status_t s;
    alm_status(&ctl, now, &s);
    CHECK(s.service_active && s.relays == ALM_K3_FAULT_OK && s.disabled == 1);   /* nothing is sent */

    /* the Pi gives the time: the 20 minutes off are taken away */
    now += 3000;
    CHECK(alm_sync_time(&ctl, T + 30u * 60u + 20u * 60u + 3u, now) == ALM_OK);
    CHECK(left_min(now) == 10);
    now += 10u * 60000u + 1000u;
    alm_tick(&ctl, now);
    CHECK(left_min(now) == 0);

    /* off for 3 hours: service has run out as soon as the time is known */
    boot(7000);
    now = 7000;
    alm_sync_time(&ctl, T, now);
    alm_set_key(&ctl, 1);
    alm_service(&ctl, 60, 1, now);
    alm_set_key(&ctl, 0);
    now += 10u * 60000u;
    alm_tick(&ctl, now);
    power_cycle(3u * 3600u * 1000u, 200);
    now = 200;
    CHECK(left_min(now) == 50);                                        /* until the Pi answers */
    alm_sync_time(&ctl, T + 10u * 60u + 3u * 3600u, now);
    alm_status(&ctl, now, &s);
    CHECK(!s.service_active && (s.relays & ALM_K3_FAULT_OK));

    /* service started before the clock was known: comes back with what it had left */
    boot(300);
    now = 300;
    alm_set_key(&ctl, 1);
    alm_service(&ctl, 20, 1, now);
    alm_set_key(&ctl, 0);
    now += 5u * 60000u;
    alm_tick(&ctl, now);
    power_cycle(60000u, 400);
    now = 400;
    CHECK(left_min(now) == 15);
    alm_sync_time(&ctl, T, now);                                       /* nothing to take away */
    CHECK(left_min(now) == 15);

    /* Frånkopplade stay until someone enables them, also over several restarts */
    boot(500);
    alm_set_key(&ctl, 1);
    alm_disable(&ctl, 3, 1, 1, 500);
    alm_set_key(&ctl, 0);
    power_cycle(0, 600);
    power_cycle(0, 700);
    CHECK(!alm_is_enabled(&ctl, alm_find(&ctl, 3)));
    alm_set_input(&ctl, 1, 1, 3, MB_IN_OPEN, 700);                    /* disabled: no alarm */
    alm_status(&ctl, 700, &s);
    CHECK(s.b == 0 && s.disabled == 1);
    alm_set_key(&ctl, 1);
    alm_disable(&ctl, 3, 0, 1, 700);
    power_cycle(0, 800);
    CHECK(alm_is_enabled(&ctl, alm_find(&ctl, 3)));

    /* a disabled point that is no longer in the configuration is simply forgotten */
    alm_persist_t p;
    memset(&p, 0, sizeof(p));
    p.n_disabled = 2;
    p.disabled[0] = 1;
    p.disabled[1] = 99;
    boot(900);
    alm_persist_restore(&ctl, &p, 900);
    alm_persist_get(&ctl, 900, &p);
    CHECK(p.n_disabled == 1 && p.disabled[0] == 1);

    /* damaged bytes are refused */
    CHECK(alm_persist_unpack(&p, packed, 5) != 0);
    packed[0] = 7;
    CHECK(alm_persist_unpack(&p, packed, 12) != 0);
}

int main(void)
{
    test_store_log();
    test_service_and_disabled_over_restart();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "OK", checks, failures);
    return failures != 0;
}
