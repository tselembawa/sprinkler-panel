/* Controller side of the module protocol - see ctrl_modules.h */
#include "ctrl_modules.h"
#include "modbus_map.h"

#include <stddef.h>
#include <string.h>

#define UNKNOWN 0xFFFFu   /* "old value" of the first report after a module comes online */

static void emit(ctrl_event_fn fn, void *ctx, const ctrl_module_t *m, ctrl_event_t ev,
                 uint8_t index, uint16_t old_v, uint16_t new_v)
{
    if (fn != NULL) {
        fn(ctx, m, ev, index, old_v, new_v);
    }
}

void ctrl_module_init(ctrl_module_t *m, uint8_t addr, uint8_t line,
                      const uint8_t in_mode[4], const uint8_t out_mode[4])
{
    memset(m, 0, sizeof(*m));
    m->addr = addr;
    m->line = line;
    m->cfg[0] = 10u;                                    /* COMM_TIMEOUT_S */
    m->cfg[1] = (MB_FAILSAFE_HOLD << 0) | (MB_FAILSAFE_HOLD << 4) |
                (MB_FAILSAFE_HOLD << 8) | (MB_FAILSAFE_HOLD << 12);
    m->cfg[2] = 100u;                                   /* DEBOUNCE_MS */
    m->cfg[3] = 10u;                                    /* PULSE_100MS = 1 s */
    for (uint8_t n = 0; n < 4u; n++) {
        m->cfg[4u + n] = in_mode[n];                    /* EOL profile 0 */
        m->cfg[8u + n] = (uint16_t)(out_mode[n] | (50u << 8));   /* 500 mA */
    }
    m->cfg[12] = 140u;                                  /* SUPPLY_LOW at 14.0 V */
    m->state = MOD_START;
    m->last_error = MB_OK;
}

/* A poll failed. Running modules count down to LOST; a module that never came online
   (or is already lost) also becomes LOST after the same number of tries, once. */
static void failed(ctrl_module_t *m, mb_status_t s, ctrl_event_fn fn, void *ctx)
{
    m->last_error = s;
    if (m->fails < 255u) {
        m->fails++;
    }
    if (m->state == MOD_RUN) {
        emit(fn, ctx, m, EV_COMM_ERROR, 0, (uint16_t)s, m->fails);
    }
    if (m->state != MOD_LOST && m->fails >= CTRL_LOST_AFTER_FAILS) {
        m->state = MOD_LOST;
        m->have_data = 0;
        emit(fn, ctx, m, EV_MODULE_LOST, 0, (uint16_t)s, 0);
    }
}

static void start_module(mb_master_t *mb, ctrl_module_t *m, ctrl_event_fn fn, void *ctx)
{
    uint16_t id[MB_IR_IDENTITY_COUNT];
    mb_status_t s = mb_read_input(mb, m->addr, MB_IR_MODULE_TYPE, MB_IR_IDENTITY_COUNT, id);
    if (s != MB_OK) {
        failed(m, s, fn, ctx);
        return;
    }
    m->fails = 0;
    m->module_type = id[0];
    m->map_version = id[1];
    m->fw_version = id[2];
    m->serial = ((uint32_t)id[4] << 16) | id[5];

    if (id[0] != MB_MODULE_TYPE_4IN_4OUT || id[1] != MB_MAP_VERSION) {
        if (m->state != MOD_REJECTED) {
            m->state = MOD_REJECTED;
            emit(fn, ctx, m, EV_MODULE_WRONG_TYPE, 0, id[0], id[1]);
        }
        return;
    }
    if (MB_LINE_OF_ADDR(m->addr) != m->line || id[6] != m->addr) {
        if (m->state != MOD_REJECTED) {
            m->state = MOD_REJECTED;
            emit(fn, ctx, m, EV_MODULE_WRONG_LINE, 0, m->line, MB_LINE_OF_ADDR(id[6]));
        }
        return;
    }

    s = mb_write_multiple(mb, m->addr, MB_HR_CFG_START, MB_HR_CFG_COUNT, m->cfg);
    if (s == MB_OK) {
        s = mb_write_single(mb, m->addr, MB_HR_COMMAND, MB_COMMAND_CONFIG_DONE);
    }
    if (s != MB_OK) {
        failed(m, s, fn, ctx);
        return;
    }
    m->state = MOD_RUN;
    m->fails = 0;
    m->have_data = 0;
    m->out_cmd_known = 0;       /* send the outputs again */
    m->cycle = 0;
    emit(fn, ctx, m, EV_MODULE_ONLINE, 0, 0, 0);
}

/* New values from the module: compare with the old ones and report what changed. */
static void apply(ctrl_module_t *m, const uint16_t *r, int full, ctrl_event_fn fn, void *ctx)
{
    const uint16_t status = (uint16_t)(r[0] & ~MB_ST_RESTARTED);
    const uint16_t inputs = r[1], outputs = r[2], changes = r[3];

    if (m->have_data && status != m->status) {
        emit(fn, ctx, m, EV_STATUS_CHANGED, 0, m->status, status);
    }
    for (uint8_t n = 0; n < 4u; n++) {
        uint8_t was = MB_NIBBLE(m->inputs, n), now = MB_NIBBLE(inputs, n);
        if (!m->have_data) {
            emit(fn, ctx, m, EV_INPUT_CHANGED, n, UNKNOWN, now);
        } else if (was != now) {
            emit(fn, ctx, m, EV_INPUT_CHANGED, n, was, now);
        } else if (full) {
            /* same state as before, but the counter moved: it went somewhere and came back */
            uint16_t moved = (uint16_t)(r[4u + n] - m->in_changes[n]);
            if (moved >= 2u) {
                emit(fn, ctx, m, EV_INPUT_TRANSIENT, n, now, moved);
            }
        }
        /* outputs: report supervision changes only (bit 3 follows flashing all the time) */
        uint8_t ow = MB_NIBBLE(m->outputs, n) & 0x7u, on = MB_NIBBLE(outputs, n) & 0x7u;
        if (m->have_data && ow != on) {
            emit(fn, ctx, m, EV_OUTPUT_CHANGED, n, ow, on);
        }
    }
    m->status = status;
    m->inputs = inputs;
    m->outputs = outputs;
    m->changes = changes;
    if (full) {
        for (uint8_t n = 0; n < 4u; n++) {
            m->in_changes[n] = r[4u + n];
            m->in_ohm[n] = r[8u + n];
            m->out_ma[n] = r[12u + n];
        }
        m->supply_mv = r[16];
        m->temp_x10 = (int16_t)r[17];
        m->crc_errors = r[18];
    }
    m->have_data = 1;
}

void ctrl_module_poll(mb_master_t *mb, ctrl_module_t *m, ctrl_event_fn fn, void *ctx)
{
    if (m->state != MOD_RUN) {
        start_module(mb, m, fn, ctx);
        return;
    }

    uint16_t r[MB_IR_FULL_COUNT];
    mb_status_t s = mb_read_input(mb, m->addr, MB_IR_STATUS, MB_IR_FAST_COUNT, r);
    if (s != MB_OK) {
        failed(m, s, fn, ctx);
        return;
    }
    m->fails = 0;

    if (r[0] & MB_ST_RESTARTED) {
        emit(fn, ctx, m, EV_MODULE_RESTARTED, 0, 0, 0);
        m->state = MOD_START;
        start_module(mb, m, fn, ctx);
        return;
    }

    int full = !m->have_data || r[3] != m->changes || ++m->cycle >= CTRL_FULL_READ_EVERY;
    if (full) {
        m->cycle = 0;
        s = mb_read_input(mb, m->addr, MB_IR_STATUS, MB_IR_FULL_COUNT, r);
        if (s != MB_OK) {
            failed(m, s, fn, ctx);
            return;
        }
    }
    apply(m, r, full, fn, ctx);

    if (!m->out_cmd_known || m->out_cmd != m->out_cmd_sent) {
        s = mb_write_single(mb, m->addr, MB_HR_OUT_CMD, m->out_cmd);
        if (s == MB_OK) {
            m->out_cmd_sent = m->out_cmd;
            m->out_cmd_known = 1;
        } else {
            failed(m, s, fn, ctx);
        }
    }
}

mb_status_t ctrl_module_identify(mb_master_t *mb, ctrl_module_t *m, int on)
{
    return mb_write_single(mb, m->addr, MB_HR_COMMAND,
                           on ? MB_COMMAND_IDENTIFY_ON : MB_COMMAND_IDENTIFY_OFF);
}

const char *ctrl_input_name(uint8_t v)
{
    static const char *const names[] = {"CLOSED", "OPEN", "LINE_OPEN", "LINE_SHORT",
                                        "NOT_USED", "MEASURE_FAULT"};
    return v < 6u ? names[v] : "?";
}

const char *ctrl_event_name(ctrl_event_t ev)
{
    static const char *const names[] = {"ONLINE", "LOST", "RESTARTED", "WRONG_TYPE", "WRONG_LINE",
                                        "INPUT", "TRANSIENT", "STATUS", "OUTPUT", "COMM_ERROR"};
    return (unsigned)ev < sizeof(names) / sizeof(names[0]) ? names[ev] : "?";
}
