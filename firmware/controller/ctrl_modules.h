/*
 * Controller side of the module protocol: keeps one I/O module alive on its RS485 line.
 *
 * For every module, once per cycle, ctrl_module_poll():
 *   - (re)starts it: reads the identity, checks type / map version / line, writes the
 *     configuration and CONFIG_DONE (docs/modbus-map.md section 5)
 *   - reads the fast status (4 registers); when something changed, the full block (19)
 *   - sends the output command when it changed
 *   - after 3 cycles without a valid answer: MODULE_LOST
 * Everything that happens is reported as an event through a callback, so the alarm logic
 * (later) and the debug print (now) see exactly the same thing.
 *
 * Hardware independent (only modbus_rtu): tested on the PC against the module simulator.
 */
#ifndef CTRL_MODULES_H
#define CTRL_MODULES_H

#include <stdint.h>
#include "modbus_rtu.h"

#define CTRL_LOST_AFTER_FAILS   3u
#define CTRL_FULL_READ_EVERY    10u

typedef enum {
    MOD_START = 0,     /* needs identity check + configuration */
    MOD_RUN,           /* configured, polled every cycle */
    MOD_LOST,          /* no answer: keeps trying to start it again */
    MOD_REJECTED       /* answers, but wrong type / map / line: keeps checking */
} ctrl_mod_state_t;

typedef enum {
    EV_MODULE_ONLINE = 0,   /* configured and running (index: -) */
    EV_MODULE_LOST,         /* no answer for CTRL_LOST_AFTER_FAILS cycles */
    EV_MODULE_RESTARTED,    /* module reported RESTARTED: it lost power or was reset */
    EV_MODULE_WRONG_TYPE,   /* old = type found, new = map version found */
    EV_MODULE_WRONG_LINE,   /* address does not belong to this line (old = line expected) */
    EV_INPUT_CHANGED,       /* index = input 0..3, old/new = MB_IN_* */
    EV_INPUT_TRANSIENT,     /* input changed and back between two polls (index, new = count) */
    EV_STATUS_CHANGED,      /* old/new = STATUS flags */
    EV_OUTPUT_CHANGED,      /* index = output 0..3, old/new = 4-bit output value */
    EV_COMM_ERROR           /* one failed poll (old = mb_status_t), before LOST */
} ctrl_event_t;

typedef struct ctrl_module ctrl_module_t;

typedef void (*ctrl_event_fn)(void *ctx, const ctrl_module_t *m, ctrl_event_t ev,
                              uint8_t index, uint16_t old_value, uint16_t new_value);

struct ctrl_module {
    /* set by ctrl_module_init */
    uint8_t addr;
    uint8_t line;                 /* RS485 line this module is polled on (1..4) */
    uint16_t cfg[13];             /* configuration block 0x0110.. (MB_HR_CFG_COUNT) */
    uint16_t out_cmd;             /* wanted outputs (MB_CMD_* per nibble), set by the application */

    /* state, read only for the application */
    ctrl_mod_state_t state;
    uint8_t fails;                /* consecutive failed polls */
    uint8_t cycle;
    uint8_t have_data;
    uint16_t out_cmd_sent;
    uint8_t out_cmd_known;
    uint16_t module_type, map_version, fw_version;
    uint32_t serial;
    uint16_t status, inputs, outputs, changes;
    uint16_t in_changes[4];
    uint16_t in_ohm[4];
    uint16_t out_ma[4];
    uint16_t supply_mv;
    int16_t temp_x10;
    uint16_t crc_errors;
    mb_status_t last_error;
};

/* in_mode / out_mode: MB_INCFG_* / MB_OUTCFG_* per channel; other settings get the defaults
   of the register map (timeout 10 s, failsafe HOLD, debounce 100 ms, pulse 1 s, 500 mA,
   low supply 14.0 V). */
void ctrl_module_init(ctrl_module_t *m, uint8_t addr, uint8_t line,
                      const uint8_t in_mode[4], const uint8_t out_mode[4]);

void ctrl_module_poll(mb_master_t *mb, ctrl_module_t *m, ctrl_event_fn on_event, void *ctx);

/* Ask the module to blink its identify LED (60 s) / stop. Returns the Modbus result. */
mb_status_t ctrl_module_identify(mb_master_t *mb, ctrl_module_t *m, int on);

const char *ctrl_input_name(uint8_t v);
const char *ctrl_event_name(ctrl_event_t ev);

#endif /* CTRL_MODULES_H */
