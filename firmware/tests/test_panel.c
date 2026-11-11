/*
 * Panel outputs on the PC: relays, LEDs (flashing) and the buzzer rhythms.
 *   gcc -std=c11 -Wall -Wextra -Werror -Icommon -Icontroller -o /tmp/test_panel \
 *       tests/test_panel.c controller/panel_io.c controller/alarm.c && /tmp/test_panel
 */
#include <stdio.h>
#include <string.h>

#include "panel_io.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static alm_t a;
static uint32_t now = 1000000u;

static const alm_point_cfg_t pts[] = {
    {1, 1, 2, 1, CAT_B, DT_SERVICEVENTIL, 1, 0, 1, 1, "SERVICEVENTIL", "SEKTION 1"},
    {2, 1, 2, 2, CAT_A, DT_PRESSOSTAT, 0, 0, 1, 1, "PRESSOSTAT", "SEKTION 1"},
    {3, 1, 2, 3, CAT_DRIFT, DT_PUMP_RUN, 0, 0, 1, 1, "PUMP", ""},
};

/* how many ms of the next 'span' ms the buzzer sounds */
static unsigned sounding_ms(uint32_t span)
{
    unsigned on = 0;
    panel_out_t o;
    for (uint32_t t = 0; t < span; t += 10u) {
        panel_outputs(&a, now + t, &o);
        on += o.buzzer ? 10u : 0u;
    }
    return on;
}

int main(void)
{
    panel_out_t o;
    alm_init(&a, now, 0, NULL, NULL);
    CHECK(alm_load(&a, pts, 3, 0, now, NULL, 0) == 0);
    alm_set_input(&a, 1, 2, 1, 0, now);   /* serviceventil closed (NC: normal) */
    alm_set_input(&a, 1, 2, 2, 1, now);   /* pressostat open (NO: normal) */
    alm_set_input(&a, 1, 2, 3, 1, now);   /* pump off */

    panel_outputs(&a, now, &o);
    CHECK(o.relays == ALM_K3_FAULT_OK);   /* quiet: only the fail-safe fault relay pulled */
    CHECK(o.led[PLED_POWER_GREEN] && !o.led[PLED_A_RED] && !o.led[PLED_BC_YELLOW]);
    CHECK(o.pattern == BZ_OFF && sounding_ms(5000) == 0);

    /* B alarm: K2, yellow LED flashing, slow buzzer 1 s on / 1 s off */
    alm_set_input(&a, 1, 2, 1, 1, now);
    panel_outputs(&a, now, &o);
    CHECK(o.relays == (ALM_K2_B | ALM_K3_FAULT_OK));
    CHECK(o.pattern == BZ_B);
    CHECK(sounding_ms(4000) == 2000);
    int lit = 0, dark = 0;
    for (uint32_t t = 0; t < 1000; t += 50) {
        panel_outputs(&a, now + t, &o);
        lit += o.led[PLED_BC_YELLOW];
        dark += !o.led[PLED_BC_YELLOW];
    }
    CHECK(lit == 10 && dark == 10);       /* flashing 0.5 s / 0.5 s */

    /* fire as well: fire rhythm wins (fast, 200 ms on / 200 ms off) */
    alm_set_input(&a, 1, 2, 2, 0, now);
    panel_outputs(&a, now, &o);
    CHECK(o.pattern == BZ_FIRE && o.led[PLED_A_RED]);
    CHECK(o.relays == (ALM_K1_A | ALM_K2_B | ALM_K3_FAULT_OK));
    CHECK(sounding_ms(4000) == 2000);
    panel_outputs(&a, now + 100u, &o); CHECK(o.buzzer);
    panel_outputs(&a, now + 300u, &o); CHECK(!o.buzzer);

    /* Kvittera: buzzer quiet, B becomes C (K4, yellow steady), fire stays on K1 */
    alm_ack(&a, 1, now);
    panel_outputs(&a, now, &o);
    CHECK(o.pattern == BZ_OFF && sounding_ms(5000) == 0);
    CHECK(o.relays == (ALM_K1_A | ALM_K4_C | ALM_K3_FAULT_OK));
    panel_outputs(&a, now + 600u, &o);
    CHECK(o.led[PLED_BC_YELLOW]);         /* steady, also in the "off" half */

    /* a cut cable: K3 drops, fault LED, short beep every 4 s */
    alm_set_input(&a, 1, 2, 3, 2, now);   /* pump line LINE_OPEN */
    panel_outputs(&a, now, &o);
    CHECK(!(o.relays & ALM_K3_FAULT_OK) && o.led[PLED_FAULT_YELLOW]);
    CHECK(o.pattern == BZ_FAULT);
    CHECK(sounding_ms(8000) == 200);

    /* service mode: nothing transmitted (K1/K2/K4 idle), service LED, power LED off */
    alm_set_key(&a, 1);
    alm_ack(&a, 1, now);
    CHECK(alm_service(&a, 30, 1, now) == ALM_OK);
    panel_outputs(&a, now, &o);
    CHECK((o.relays & (ALM_K1_A | ALM_K2_B | ALM_K4_C)) == 0);
    CHECK(o.led[PLED_SERVICE_YELLOW] && !o.led[PLED_POWER_GREEN]);

    printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
