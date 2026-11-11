/*
 * Configuration in flash, on the PC: a RAM "flash" with the STM32 rules
 * (erase = 0xFF, programming only 8-byte blocks into erased flash).
 *   gcc -std=c11 -Wall -Wextra -Werror -Icommon -Icontroller -o /tmp/test_cfg_store \
 *       tests/test_cfg_store.c controller/cfg_store.c controller/alarm.c && /tmp/test_cfg_store
 */
#include <stdio.h>
#include <string.h>

#include "cfg_store.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define PAGE 2048u
#define SIZE (8u * PAGE)
static uint8_t flash[SIZE] __attribute__((aligned(8)));
static int erases, programs, fail_program_after = -1;

static int f_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    uint32_t first = off / PAGE, last = (off + len - 1u) / PAGE;
    for (uint32_t p = first; p <= last; p++) {
        memset(&flash[p * PAGE], 0xFF, PAGE);
    }
    erases++;
    return 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    if ((off & 7u) || (len & 7u) || off + len > SIZE) return -1;
    for (uint32_t i = 0; i < len; i++) {
        if (flash[off + i] != 0xFF) return -1;          /* not erased */
    }
    if (fail_program_after >= 0 && programs >= fail_program_after) return -1;   /* "reset" */
    memcpy(&flash[off], d, len);
    programs++;
    return 0;
}

static const cfg_store_t store = {flash, SIZE, f_erase, f_program, NULL};

static alm_point_cfg_t pts[3] = {
    {1, 1, 2, 1, CAT_B, DT_SERVICEVENTIL, 1, 0, 1, 1, "SERVICEVENTIL", "SEKTION 1"},
    {2, 1, 2, 2, CAT_A, DT_PRESSOSTAT, 0, 0, 1, 1, "PRESSOSTAT", "SEKTION 1"},
    {3, 1, 1, 4, CAT_A, DT_PRESSOSTAT, 1, 5, 1, 2, "PRESSOSTAT 2", "SEKTION 2"},
};

int main(void)
{
    uint16_t n = 99;
    memset(flash, 0xFF, sizeof(flash));
    CHECK(cfg_store_load(&store, &n) == NULL && n == 0);       /* new controller: nothing */

    CHECK(cfg_store_save(&store, pts, 3) == 0);
    const alm_point_cfg_t *p = cfg_store_load(&store, &n);
    CHECK(p != NULL && n == 3);
    CHECK(p && alm_config_crc(p, n) == alm_config_crc(pts, 3));
    CHECK(p && p[2].module == 1 && p[2].channel == 4 && strcmp(p[2].lcd1, "PRESSOSTAT 2") == 0);

    /* the alarm engine accepts it as it is */
    static alm_t a;
    alm_init(&a, 0, 0, NULL, NULL);
    alm_status_t s;
    alm_status(&a, 0, &s);
    CHECK(s.no_config && !(s.relays & ALM_K3_FAULT_OK) && s.led_yellow_fault);   /* no config = Fel */
    CHECK(alm_load(&a, p, n, 0, 0, NULL, 0) == 0);
    alm_status(&a, 0, &s);
    CHECK(!s.no_config && (s.relays & ALM_K3_FAULT_OK));

    /* the same configuration again: no flash wear */
    int e0 = erases;
    CHECK(cfg_store_save(&store, a.cfg, a.n) == 1);
    CHECK(erases == e0);

    /* a changed one is written */
    pts[1].delay_s = 10;
    CHECK(cfg_store_save(&store, pts, 3) == 0);
    p = cfg_store_load(&store, &n);
    CHECK(p && n == 3 && p[1].delay_s == 10);

    /* empty configuration saved on purpose is still "a configuration" */
    CHECK(cfg_store_save(&store, pts, 0) == 0);
    p = cfg_store_load(&store, &n);
    CHECK(p != NULL && n == 0);

    /* reset in the middle of saving: no configuration, never half of one */
    CHECK(cfg_store_save(&store, pts, 3) == 0);
    pts[0].delay_s = 3;
    programs = 0;
    fail_program_after = 1;
    CHECK(cfg_store_save(&store, pts, 3) == -1);
    fail_program_after = -1;
    CHECK(cfg_store_load(&store, &n) == NULL);

    /* damaged contents are not loaded */
    CHECK(cfg_store_save(&store, pts, 3) == 0);
    flash[CFG_STORE_HEADER + 5] ^= 0x01;
    CHECK(cfg_store_load(&store, &n) == NULL);

    /* too big for the region */
    static alm_point_cfg_t many[ALM_MAX_POINTS];
    const cfg_store_t small = {flash, 1024u, f_erase, f_program, NULL};
    CHECK(cfg_store_save(&small, many, 100) == -1);
    CHECK(cfg_store_bytes(ALM_MAX_POINTS) <= 16u * 1024u);    /* the 16 kB in app.c are enough */

    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "OK", checks, failures);
    return failures != 0;
}
