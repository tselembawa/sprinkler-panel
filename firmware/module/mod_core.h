/*
 * I/O module, 4 inputs / 4 outputs (type 0x0404): everything the module decides, without the STM32.
 *
 * The C twin of tools/module_sim/module_sim.py (the simulator the controller was tested with):
 * Modbus RTU slave with the register map v1 (docs/modbus-map.md), supervised inputs, outputs with
 * flash/pulse, failsafe on lost communication, identify. mod_app.c only measures, switches pins
 * and moves bytes; this file is tested on the PC (tests/test_module.c, and against the real
 * controller code in tools/module_sim/test_module_e2e.py).
 */
#ifndef MOD_CORE_H
#define MOD_CORE_H

#include <stdint.h>

#include "modbus_map.h"

#define MOD_FW_VERSION      0x0100u     /* 1.0 */
#define MOD_HW_VERSION      0x0001u     /* breadboard with NUCLEO-G031K8 */
#define MOD_SUPPLY_HIGH_MV  30000u
#define MOD_OHM_OPEN        0xFFFFu     /* IN_OHM when the loop is open */
#define MOD_MAX_FRAME       256u

/* Input loop: 1 kOhm in series + 1 kOhm across the contact (EOL profile 0).
   Contact closed ~1 kOhm, open ~2 kOhm. Limits in Ohm: */
#define MOD_OHM_SHORT_BELOW     400u    /* < 400: short circuit (Kortslutning) */
#define MOD_OHM_CLOSED_BELOW   1500u    /* 400-1499: closed */
#define MOD_OHM_OPEN_BELOW     4000u    /* 1500-3999: open */
#define MOD_OHM_BREAK_FROM     8000u    /* >= 8000: wire break (Avbrott); 4000-7999: measure fault */

typedef struct {
    uint8_t address;                 /* from the DIP switch at start, 1..240 (0 = none: silent) */
    uint32_t serial;

    uint16_t cfg[MB_HR_CFG_COUNT];   /* 0x0110.. */
    uint16_t flags;                  /* RESTARTED, FAILSAFE, DIP_CHANGED (the others are live) */
    uint16_t out_cmd;
    uint16_t changes;
    uint16_t in_changes[4];
    uint16_t crc_errors;
    uint8_t failsafe;

    /* inputs: measured loop resistance, the value it means, debounced value */
    uint16_t ohm[4];
    uint8_t raw[4];                  /* classified now (before debounce) */
    uint8_t value[4];                /* reported (after debounce) */
    uint32_t raw_since_ms[4];
    uint8_t measured;                /* bit n: IN n has had its first measurement */

    uint16_t supply_mv;
    int16_t temp_10c;
    uint8_t internal_fault;

    uint32_t now_ms;
    uint32_t last_request_ms;
    uint32_t flash_t0_ms;
    uint32_t identify_until_ms;
    uint32_t pulse_until_ms[4];
    uint8_t reboot;                  /* COMMAND = REBOOT: mod_app.c restarts after the answer */
} mod_t;

void mod_init(mod_t *m, uint8_t address, uint32_t serial, uint32_t now_ms);

/* Measurements, as often as they come (the inputs every few ms). */
void mod_set_loop_ohm(mod_t *m, uint8_t n, uint16_t ohm, uint32_t now_ms);   /* n = 0..3 */
void mod_set_supply(mod_t *m, uint16_t mv, int16_t temp_10c, uint8_t internal_fault);
void mod_set_dip(mod_t *m, uint8_t dip);   /* DIP read again: a different value -> DIP_CHANGED */

/* Called often (every ms or so): debounce, failsafe. */
void mod_tick(mod_t *m, uint32_t now_ms);

/* One received frame (CRC included). Returns the length of the answer in resp (0 = no answer:
   not for us, broadcast, bad CRC). */
uint16_t mod_handle(mod_t *m, const uint8_t *frame, uint16_t len, uint8_t *resp, uint32_t now_ms);

/* What the pins should do now: bit n = OUTn+1 on. */
uint8_t mod_outputs(const mod_t *m, uint32_t now_ms);
/* Identify LED: on/off now (blinks fast while IDENTIFY, slow heartbeat otherwise). */
uint8_t mod_led(const mod_t *m, uint32_t now_ms);

/* Loop resistance -> input value (for the given mode). */
uint8_t mod_classify(uint16_t ohm, uint8_t mode);
/* ADC: divider 3.3 V - R_ref - node - loop - GND. code 0..4095 -> Ohm (0xFFFF = open). */
uint16_t mod_adc_to_ohm(uint16_t code, uint16_t r_ref_ohm);

uint16_t mod_status(const mod_t *m);
uint16_t mod_inputs_word(const mod_t *m);

#endif /* MOD_CORE_H */
