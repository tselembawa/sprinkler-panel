/*
 * The panel's own outputs, worked out from the alarm engine: relays K1-K4, the six LEDs
 * and the buzzer with a different rhythm for fire, B alarm and fault.
 * Pure logic (no hardware): app.c puts the result on the pins, the tests check it on the PC.
 */
#ifndef PANEL_IO_H
#define PANEL_IO_H

#include <stdint.h>

#include "alarm.h"

enum { PLED_A_RED = 0, PLED_BC_YELLOW, PLED_FAULT_YELLOW, PLED_POWER_GREEN, PLED_DRIFT_GREEN,
       PLED_SERVICE_YELLOW, PLED_COUNT };

typedef enum { BZ_OFF = 0, BZ_FIRE, BZ_B, BZ_FAULT } buzzer_pattern_t;

/* buzzer rhythm: on for on_ms at the start of every period_ms */
typedef struct { uint16_t on_ms, period_ms; } buzzer_rhythm_t;
extern const buzzer_rhythm_t panel_buzzer_rhythm[4];

#define PANEL_FLASH_MS 500u   /* flashing LED: 0.5 s on, 0.5 s off */

typedef struct {
    uint8_t relays;            /* ALM_K1_A ... ALM_K4_C, after service mode */
    uint8_t led[PLED_COUNT];   /* 1 = lit now (flashing already applied) */
    uint8_t buzzer;            /* 1 = sounding now (rhythm already applied) */
    uint8_t pattern;           /* buzzer_pattern_t */
} panel_out_t;

/* what waits for Kvittera decides the rhythm: fire before B alarm before fault */
buzzer_pattern_t panel_buzzer_pattern(const alm_t *a, uint32_t now_ms);
void panel_outputs(const alm_t *a, uint32_t now_ms, panel_out_t *o);

#endif /* PANEL_IO_H */
