/* Panel outputs - see panel_io.h */
#include "panel_io.h"

#include <string.h>

const buzzer_rhythm_t panel_buzzer_rhythm[4] = {
    [BZ_OFF]   = {0u, 1000u},
    [BZ_FIRE]  = {200u, 400u},     /* fast beep-beep */
    [BZ_B]     = {1000u, 2000u},   /* slow: 1 s on, 1 s off */
    [BZ_FAULT] = {100u, 4000u},    /* short beep every 4 s */
};

buzzer_pattern_t panel_buzzer_pattern(const alm_t *a, uint32_t now_ms)
{
    alm_status_t s;
    alm_status(a, now_ms, &s);
    if (!s.buzzer) {
        return BZ_OFF;
    }
    int b = 0;
    for (uint16_t i = 0; i < a->n; i++) {
        const alm_point_rt_t *rt = &a->rt[i];
        if (!alm_is_enabled(a, (int)i) || rt->acked ||
            (rt->state != PS_ALARM && rt->state != PS_LATCHED)) {
            continue;
        }
        if (a->cfg[i].category == CAT_A) {
            return BZ_FIRE;
        }
        if (a->cfg[i].category == CAT_B) {
            b = 1;
        }
    }
    return b ? BZ_B : BZ_FAULT;
}

void panel_outputs(const alm_t *a, uint32_t now_ms, panel_out_t *o)
{
    alm_status_t s;
    alm_status(a, now_ms, &s);
    memset(o, 0, sizeof(*o));
    o->relays = s.relays;
    const uint8_t flash_on = (now_ms % (2u * PANEL_FLASH_MS)) < PANEL_FLASH_MS;
    o->led[PLED_A_RED] = s.led_red_a;
    o->led[PLED_BC_YELLOW] = s.led_yellow_b == LED_FLASH ? flash_on : (s.led_yellow_b == LED_STEADY);
    o->led[PLED_FAULT_YELLOW] = s.led_yellow_fault;
    o->led[PLED_POWER_GREEN] = s.led_green_power;
    o->led[PLED_DRIFT_GREEN] = s.led_green_drift;
    o->led[PLED_SERVICE_YELLOW] = s.led_yellow_service;
    o->pattern = (uint8_t)panel_buzzer_pattern(a, now_ms);
    const buzzer_rhythm_t *r = &panel_buzzer_rhythm[o->pattern];
    o->buzzer = o->pattern != BZ_OFF && (now_ms % r->period_ms) < r->on_ms;
}
