#include "mod_core.h"

#include <string.h>

#include "modbus_rtu.h"

static const uint16_t default_cfg[MB_HR_CFG_COUNT] = {
    10, 0x2222, 100, 10, 0x0001, 0x0001, 0x0001, 0x0001, 0x3202, 0x3202, 0x3202, 0x3202, 140,
};

void mod_init(mod_t *m, uint8_t address, uint32_t serial, uint32_t now_ms)
{
    memset(m, 0, sizeof(*m));
    m->address = address;
    m->serial = serial;
    memcpy(m->cfg, default_cfg, sizeof(default_cfg));
    m->flags = MB_ST_RESTARTED;
    m->now_ms = now_ms;
    m->last_request_ms = now_ms;
    m->flash_t0_ms = now_ms;
    m->supply_mv = 24000u;
    m->temp_10c = 250;
    for (uint8_t n = 0; n < 4u; n++) {
        m->ohm[n] = MOD_OHM_OPEN;
        m->raw[n] = m->value[n] = MB_IN_LINE_OPEN;
        m->raw_since_ms[n] = now_ms;
    }
}

/* ---- inputs ---------------------------------------------------------------------------------- */
uint16_t mod_adc_to_ohm(uint16_t code, uint16_t r_ref_ohm)
{
    if (code >= 4090u) {
        return MOD_OHM_OPEN;
    }
    uint32_t r = (uint32_t)r_ref_ohm * code / (4095u - code);
    return r >= MOD_OHM_OPEN ? (uint16_t)(MOD_OHM_OPEN - 1u) : (uint16_t)r;
}

uint8_t mod_classify(uint16_t ohm, uint8_t mode)
{
    if (mode == MB_INCFG_NOT_USED) {
        return MB_IN_NOT_USED;
    }
    if (mode == MB_INCFG_UNSUPERVISED) {     /* plain contact: only closed or open */
        return ohm < MOD_OHM_CLOSED_BELOW ? MB_IN_CLOSED : MB_IN_OPEN;
    }
    if (ohm < MOD_OHM_SHORT_BELOW) return MB_IN_LINE_SHORT;
    if (ohm < MOD_OHM_CLOSED_BELOW) return MB_IN_CLOSED;
    if (ohm < MOD_OHM_OPEN_BELOW) return MB_IN_OPEN;
    if (ohm >= MOD_OHM_BREAK_FROM) return MB_IN_LINE_OPEN;
    return MB_IN_MEASURE_FAULT;
}

static uint8_t in_mode(const mod_t *m, uint8_t n)
{
    return (uint8_t)(m->cfg[4 + n] & 0xFFu);
}

static void accept(mod_t *m, uint8_t n, uint8_t v)
{
    if (m->value[n] != v) {
        m->value[n] = v;
        m->in_changes[n]++;
        m->changes++;
    }
}

void mod_set_loop_ohm(mod_t *m, uint8_t n, uint16_t ohm, uint32_t now_ms)
{
    if (n >= 4u) {
        return;
    }
    m->ohm[n] = ohm;
    uint8_t v = mod_classify(ohm, in_mode(m, n));
    if (!(m->measured & (1u << n))) {
        /* the first measurement after start counts at once (no false "Avbrott" for 100 ms) */
        m->measured |= (uint8_t)(1u << n);
        m->raw[n] = m->value[n] = v;
        m->raw_since_ms[n] = now_ms;
        return;
    }
    if (v != m->raw[n]) {
        m->raw[n] = v;
        m->raw_since_ms[n] = now_ms;
    }
}

static void debounce(mod_t *m, uint32_t now_ms)
{
    for (uint8_t n = 0; n < 4u; n++) {
        uint8_t v = mod_classify(m->ohm[n], in_mode(m, n));   /* the mode may have changed */
        if (v != m->raw[n]) {
            m->raw[n] = v;
            m->raw_since_ms[n] = now_ms;
        }
        if (!(m->measured & (1u << n))) {
            continue;                   /* nothing measured yet */
        }
        if (v == MB_IN_NOT_USED || m->value[n] == MB_IN_NOT_USED) {
            accept(m, n, v);            /* configuration changed, not a contact: no waiting */
        } else if (v != m->value[n] && now_ms - m->raw_since_ms[n] >= m->cfg[2]) {
            accept(m, n, v);
        }
    }
}

uint16_t mod_inputs_word(const mod_t *m)
{
    uint16_t w = 0;
    for (uint8_t n = 0; n < 4u; n++) {
        w |= (uint16_t)(m->value[n] & 0xFu) << (4u * n);
    }
    return w;
}

void mod_set_supply(mod_t *m, uint16_t mv, int16_t temp_10c, uint8_t internal_fault)
{
    m->supply_mv = mv;
    m->temp_10c = temp_10c;
    m->internal_fault = internal_fault;
}

void mod_set_dip(mod_t *m, uint8_t dip)
{
    if (dip != m->address) {
        m->flags |= MB_ST_DIP_CHANGED;    /* the new address counts only after a restart */
    }
}

/* ---- outputs --------------------------------------------------------------------------------- */
static uint8_t effective_cmd(const mod_t *m, uint8_t n)
{
    if (m->failsafe) {
        switch (MB_NIBBLE(m->cfg[1], n)) {
        case MB_FAILSAFE_OFF:   return MB_CMD_OFF;
        case MB_FAILSAFE_ON:    return MB_CMD_ON;
        case MB_FAILSAFE_FLASH: return MB_CMD_FLASH;
        default:                break;    /* HOLD */
        }
    }
    return MB_NIBBLE(m->out_cmd, n);
}

static int output_on(const mod_t *m, uint8_t n, uint32_t now_ms)
{
    uint32_t t = now_ms - m->flash_t0_ms;
    switch (effective_cmd(m, n)) {
    case MB_CMD_ON:         return 1;
    case MB_CMD_FLASH:      return (t % 1000u) < 500u;
    case MB_CMD_FLASH_FAST: return (t % 500u) < 250u;
    case MB_CMD_PULSE:      return (int32_t)(now_ms - m->pulse_until_ms[n]) < 0;
    default:                return 0;
    }
}

uint8_t mod_outputs(const mod_t *m, uint32_t now_ms)
{
    uint8_t bits = 0;
    for (uint8_t n = 0; n < 4u; n++) {
        if ((m->cfg[8 + n] & 0xFFu) != MB_OUTCFG_NOT_USED && output_on(m, n, now_ms)) {
            bits |= (uint8_t)(1u << n);
        }
    }
    return bits;
}

static uint16_t outputs_word(const mod_t *m)
{
    uint16_t w = 0;
    uint8_t on = mod_outputs(m, m->now_ms);
    for (uint8_t n = 0; n < 4u; n++) {
        /* the breadboard has LEDs, no line supervision: say so (the controller counts it as OK) */
        uint16_t nib = MB_OUT_SUP_NOT_SUPERVISED | ((on >> n) & 1u ? MB_OUT_ON_BIT : 0u);
        w |= (uint16_t)(nib << (4u * n));
    }
    return w;
}

uint8_t mod_led(const mod_t *m, uint32_t now_ms)
{
    if (m->address == 0u) {
        return (now_ms % 200u) < 100u;                     /* no address: very fast */
    }
    if ((int32_t)(now_ms - m->identify_until_ms) < 0) {
        return (now_ms % 250u) < 125u;                     /* "Hitta modul" */
    }
    if (m->failsafe) {
        return (now_ms % 1000u) < 500u;                    /* no controller */
    }
    return (now_ms % 2000u) < 60u;                         /* all fine: short blink every 2 s */
}

/* ---- status ---------------------------------------------------------------------------------- */
uint16_t mod_status(const mod_t *m)
{
    uint16_t st = m->flags;
    if (m->supply_mv < (uint32_t)m->cfg[12] * 100u) st |= MB_ST_SUPPLY_LOW;
    if (m->supply_mv > MOD_SUPPLY_HIGH_MV) st |= MB_ST_SUPPLY_HIGH;
    if (m->internal_fault) st |= MB_ST_INTERNAL_FAULT;
    if ((int32_t)(m->now_ms - m->identify_until_ms) < 0) st |= MB_ST_IDENTIFY;
    return st;
}

void mod_tick(mod_t *m, uint32_t now_ms)
{
    m->now_ms = now_ms;
    debounce(m, now_ms);
    if (!m->failsafe && now_ms - m->last_request_ms > (uint32_t)m->cfg[0] * 1000u) {
        m->failsafe = 1;
        m->flags |= MB_ST_FAILSAFE;
    }
}

/* ---- registers ------------------------------------------------------------------------------- */
static int input_register(const mod_t *m, uint16_t reg, uint16_t *v)
{
    switch (reg) {
    case MB_IR_MODULE_TYPE: *v = MB_MODULE_TYPE_4IN_4OUT; return 0;
    case MB_IR_MAP_VERSION: *v = MB_MAP_VERSION; return 0;
    case MB_IR_FW_VERSION:  *v = MOD_FW_VERSION; return 0;
    case MB_IR_HW_VERSION:  *v = MOD_HW_VERSION; return 0;
    case MB_IR_SERIAL_HI:   *v = (uint16_t)(m->serial >> 16); return 0;
    case MB_IR_SERIAL_LO:   *v = (uint16_t)m->serial; return 0;
    case MB_IR_DIP_ADDRESS: *v = m->address; return 0;
    case 0x0007:            *v = 0; return 0;
    case MB_IR_STATUS:      *v = mod_status(m); return 0;
    case MB_IR_INPUTS:      *v = mod_inputs_word(m); return 0;
    case MB_IR_OUTPUTS:     *v = outputs_word(m); return 0;
    case MB_IR_CHANGES:     *v = m->changes; return 0;
    case MB_IR_SUPPLY_MV:   *v = m->supply_mv; return 0;
    case MB_IR_TEMP:        *v = (uint16_t)m->temp_10c; return 0;
    case MB_IR_CRC_ERRORS:  *v = m->crc_errors; return 0;
    default: break;
    }
    if (reg >= MB_IR_IN_CHANGES(0) && reg <= MB_IR_IN_CHANGES(3)) {
        *v = m->in_changes[reg - MB_IR_IN_CHANGES(0)];
        return 0;
    }
    if (reg >= MB_IR_IN_OHM(0) && reg <= MB_IR_IN_OHM(3)) {
        uint8_t n = (uint8_t)(reg - MB_IR_IN_OHM(0));
        *v = m->value[n] == MB_IN_NOT_USED ? 0u : m->ohm[n];
        return 0;
    }
    if (reg >= MB_IR_OUT_MA(0) && reg <= MB_IR_OUT_MA(3)) {
        *v = 0;                          /* no current measurement on the breadboard */
        return 0;
    }
    return -1;
}

static int holding_register(const mod_t *m, uint16_t reg, uint16_t *v)
{
    if (reg == MB_HR_OUT_CMD) { *v = m->out_cmd; return 0; }
    if (reg == MB_HR_COMMAND || reg == MB_HR_SYNC) { *v = 0; return 0; }
    if (reg >= MB_HR_CFG_START && reg < MB_HR_CFG_START + MB_HR_CFG_COUNT) {
        *v = m->cfg[reg - MB_HR_CFG_START];
        return 0;
    }
    return -1;
}

/* 0 = fine, else exception code */
static uint8_t check_value(uint16_t reg, uint16_t v)
{
    int ok;
    if (reg == MB_HR_OUT_CMD) {
        ok = 1;
        for (uint8_t n = 0; n < 4u; n++) ok = ok && MB_NIBBLE(v, n) <= MB_CMD_PULSE;
    } else if (reg == MB_HR_COMMAND) {
        ok = v >= MB_COMMAND_CONFIG_DONE && v <= MB_COMMAND_REBOOT;
    } else if (reg == MB_HR_SYNC) {
        ok = 1;
    } else if (reg == MB_HR_COMM_TIMEOUT_S) {
        ok = v >= 2u && v <= 60u;
    } else if (reg == MB_HR_FAILSAFE_OUT) {
        ok = 1;
        for (uint8_t n = 0; n < 4u; n++) ok = ok && MB_NIBBLE(v, n) <= MB_FAILSAFE_FLASH;
    } else if (reg == MB_HR_DEBOUNCE_MS) {
        ok = v >= 20u && v <= 2000u;
    } else if (reg == MB_HR_PULSE_100MS) {
        ok = v >= 1u && v <= 600u;
    } else if (reg >= MB_HR_IN_CFG(0) && reg <= MB_HR_IN_CFG(3)) {
        ok = (v & 0xFFu) <= MB_INCFG_UNSUPERVISED && (v >> 8) == 0u;
    } else if (reg >= MB_HR_OUT_CFG(0) && reg <= MB_HR_OUT_CFG(3)) {
        ok = (v & 0xFFu) <= MB_OUTCFG_SUPERVISED;
    } else if (reg == MB_HR_SUPPLY_LOW_100MV) {
        ok = v >= 100u && v <= 300u;
    } else {
        return MB_EX_ILLEGAL_ADDRESS;
    }
    return ok ? 0u : MB_EX_ILLEGAL_VALUE;
}

static void command(mod_t *m, uint16_t v, uint32_t now_ms)
{
    switch (v) {
    case MB_COMMAND_CONFIG_DONE:  m->flags &= (uint16_t)~MB_ST_RESTARTED; break;
    case MB_COMMAND_CLEAR_FLAGS:
        m->flags &= (uint16_t)~(MB_ST_FAILSAFE | MB_ST_DIP_CHANGED);
        m->crc_errors = 0;
        break;
    case MB_COMMAND_IDENTIFY_ON:  m->identify_until_ms = now_ms + 60000u; break;
    case MB_COMMAND_IDENTIFY_OFF: m->identify_until_ms = now_ms; break;
    case MB_COMMAND_REBOOT:       m->reboot = 1; break;
    default: break;
    }
}

static void write_register(mod_t *m, uint16_t reg, uint16_t v, uint32_t now_ms)
{
    if (reg == MB_HR_OUT_CMD) {
        if (v != m->out_cmd) {
            for (uint8_t n = 0; n < 4u; n++) {
                if (MB_NIBBLE(v, n) == MB_CMD_PULSE && MB_NIBBLE(m->out_cmd, n) != MB_CMD_PULSE) {
                    m->pulse_until_ms[n] = now_ms + (uint32_t)m->cfg[3] * 100u;
                }
            }
            m->out_cmd = v;
            m->changes++;
        }
    } else if (reg == MB_HR_COMMAND) {
        command(m, v, now_ms);
    } else if (reg == MB_HR_SYNC) {
        m->flash_t0_ms = now_ms;
    } else {
        m->cfg[reg - MB_HR_CFG_START] = v;
    }
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint16_t exception(const mod_t *m, uint8_t fc, uint8_t code, uint8_t *resp)
{
    resp[0] = m->address;
    resp[1] = (uint8_t)(fc | 0x80u);
    resp[2] = code;
    return mb_append_crc(resp, 3);
}

uint16_t mod_handle(mod_t *m, const uint8_t *f, uint16_t len, uint8_t *resp, uint32_t now_ms)
{
    m->now_ms = now_ms;
    if (len < 4u || m->address == 0u) {
        return 0;
    }
    if (f[0] != m->address && f[0] != MB_ADDR_BROADCAST) {
        return 0;                        /* for another module (CRC not even looked at) */
    }
    if (!mb_crc_ok(f, len)) {
        m->crc_errors++;
        return 0;
    }
    const int broadcast = f[0] == MB_ADDR_BROADCAST;
    m->last_request_ms = now_ms;
    m->failsafe = 0;                     /* the controller is back: outputs follow it again */
    const uint8_t fc = f[1];

    if (fc == MB_FC_READ_INPUT || fc == MB_FC_READ_HOLDING) {
        if (broadcast || len != 8u) {
            return broadcast ? 0u : exception(m, fc, MB_EX_ILLEGAL_VALUE, resp);
        }
        uint16_t start = get16(&f[2]), count = get16(&f[4]);
        if (count < 1u || count > 125u || 3u + 2u * count + 2u > MOD_MAX_FRAME) {
            return exception(m, fc, MB_EX_ILLEGAL_VALUE, resp);
        }
        resp[0] = m->address;
        resp[1] = fc;
        resp[2] = (uint8_t)(2u * count);
        for (uint16_t i = 0; i < count; i++) {
            uint16_t v;
            int r = fc == MB_FC_READ_INPUT ? input_register(m, (uint16_t)(start + i), &v)
                                           : holding_register(m, (uint16_t)(start + i), &v);
            if (r != 0) {
                return exception(m, fc, MB_EX_ILLEGAL_ADDRESS, resp);
            }
            resp[3 + 2 * i] = (uint8_t)(v >> 8);
            resp[4 + 2 * i] = (uint8_t)v;
        }
        return mb_append_crc(resp, (uint16_t)(3u + 2u * count));
    }
    if (fc == MB_FC_WRITE_SINGLE) {
        if (len != 8u) {
            return broadcast ? 0u : exception(m, fc, MB_EX_ILLEGAL_VALUE, resp);
        }
        uint16_t reg = get16(&f[2]), v = get16(&f[4]);
        uint8_t ex = check_value(reg, v);
        if (ex) {
            return broadcast ? 0u : exception(m, fc, ex, resp);
        }
        write_register(m, reg, v, now_ms);
        if (broadcast) {
            return 0;
        }
        memcpy(resp, f, 6);
        resp[0] = m->address;
        return mb_append_crc(resp, 6);
    }
    if (fc == MB_FC_WRITE_MULTIPLE) {
        uint16_t start = len >= 9u ? get16(&f[2]) : 0u, count = len >= 9u ? get16(&f[4]) : 0u;
        if (len < 9u || count < 1u || count > 123u || f[6] != 2u * count || len != 9u + 2u * count) {
            return broadcast ? 0u : exception(m, fc, MB_EX_ILLEGAL_VALUE, resp);
        }
        for (uint16_t i = 0; i < count; i++) {     /* check everything before changing anything */
            uint8_t ex = check_value((uint16_t)(start + i), get16(&f[7 + 2 * i]));
            if (ex) {
                return broadcast ? 0u : exception(m, fc, ex, resp);
            }
        }
        for (uint16_t i = 0; i < count; i++) {
            write_register(m, (uint16_t)(start + i), get16(&f[7 + 2 * i]), now_ms);
        }
        debounce(m, now_ms);                       /* new input modes count at once */
        if (broadcast) {
            return 0;
        }
        memcpy(resp, f, 6);
        resp[0] = m->address;
        return mb_append_crc(resp, 6);
    }
    return broadcast ? 0u : exception(m, fc, MB_EX_ILLEGAL_FUNCTION, resp);
}
