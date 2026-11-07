/*
 * Controller side of the Raspberry Pi link (docs/pi-link.md): receives frames byte by byte,
 * answers every request from the alarm engine and the module table, and notices when the
 * Pi goes quiet (HMI_LOST).
 *
 * Hardware independent: bytes come in through pi_link_rx_byte(), answers go out through
 * the write function given to pi_link_init(). Tested on the PC against the HMI's Python code.
 */
#ifndef PI_LINK_H
#define PI_LINK_H

#include <stdint.h>

#include "alarm.h"
#include "ctrl_modules.h"
#include "pi_link_proto.h"

#define PI_FW_MAJOR 0u
#define PI_FW_MINOR 5u

typedef void (*pi_write_fn)(void *ctx, const uint8_t *data, uint16_t len);

typedef struct {
    alm_t *alm;
    ctrl_module_t *modules;
    uint8_t module_count;
    pi_write_fn write;
    void *wctx;

    uint8_t rx[PL_MAX_ENCODED];
    uint16_t rx_len;
    uint8_t rx_overflow;

    uint32_t last_rx_ms;
    uint8_t ever_heard;

    /* the last command and its answer: when the very next frame repeats it (same seq + type,
       the Pi did not get our answer), the answer is sent again and the command is not run twice */
    uint8_t last_seq, last_type, have_last;
    uint8_t last_answer[PL_MAX_PAYLOAD];
    uint16_t last_answer_len;
    uint8_t last_answer_type;

    /* configuration being uploaded */
    uint16_t cfg_expected, cfg_received;
    uint8_t cfg_state;            /* 0 idle, 1 receiving, 2 checked (CFG_END OK) */

    uint8_t language;             /* chosen on the Pi (0 sv, 1 en, 2 fi, 3 no, 4 da, 5 el, 6 ru) */

    uint32_t frames_ok, frames_bad;
} pi_link_t;

void pi_link_init(pi_link_t *l, alm_t *alm, ctrl_module_t *modules, uint8_t module_count,
                  pi_write_fn write, void *wctx, uint32_t now_ms);
void pi_link_rx_byte(pi_link_t *l, uint8_t b, uint32_t now_ms);
void pi_link_tick(pi_link_t *l, uint32_t now_ms);
/* text for the Pi's log (LOG frame, seq 0) */
void pi_link_log(pi_link_t *l, const char *text);

#endif /* PI_LINK_H */
