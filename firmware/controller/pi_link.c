/* Controller side of the Raspberry Pi link - see pi_link.h and docs/pi-link.md */
#include "pi_link.h"

#include <string.h>

#include "cobs.h"

/* buffers kept out of the stack (the STM32 has plenty of RAM, the stack is small) */
static alm_point_cfg_t staging[ALM_MAX_POINTS];
static uint8_t decoded[PL_MAX_FRAME];
static uint8_t raw[PL_MAX_FRAME];
static uint8_t encoded[PL_MAX_ENCODED];
static uint8_t resp[PL_MAX_PAYLOAD];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void send_frame(pi_link_t *l, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len > PL_MAX_PAYLOAD) {
        len = PL_MAX_PAYLOAD;
    }
    raw[0] = PL_VERSION;
    raw[1] = type;
    raw[2] = seq;
    raw[3] = 0;
    put16(&raw[4], len);
    if (len) {
        memcpy(&raw[PL_HEADER_LEN], payload, len);
    }
    put16(&raw[PL_HEADER_LEN + len], crc16_ccitt(raw, PL_HEADER_LEN + len));
    size_t n = cobs_encode(raw, PL_HEADER_LEN + len + 2u, encoded);
    encoded[n++] = 0u;
    if (l->write) {
        l->write(l->wctx, encoded, (uint16_t)n);
    }
}

void pi_link_init(pi_link_t *l, alm_t *alm, ctrl_module_t *modules, uint8_t module_count,
                  pi_write_fn write, void *wctx, uint32_t now_ms)
{
    memset(l, 0, sizeof(*l));
    l->alm = alm;
    l->modules = modules;
    l->module_count = module_count;
    l->write = write;
    l->wctx = wctx;
    l->last_rx_ms = now_ms;
}

void pi_link_log(pi_link_t *l, const char *text)
{
    size_t n = strlen(text);
    send_frame(l, PL_LOG, 0, (const uint8_t *)text, (uint16_t)(n > 200u ? 200u : n));
}

/* ---- request handlers: fill resp, return its length; *nack set on error ------------------- */
static int is_command(uint8_t type)
{
    return (type >= PL_CFG_BEGIN && type <= PL_CFG_ACTIVATE) ||
           (type >= PL_CMD_ACK && type <= PL_CMD_SERVICE) || type == PL_SET_TIME || type == PL_DEV_SET_KEY;
}

static uint8_t nack_of(alm_result_t r)
{
    switch (r) {
    case ALM_ACCESS_DENIED: return PL_NACK_ACCESS_DENIED;
    case ALM_BAD_DURATION:  return PL_NACK_BAD_DURATION;
    case ALM_UNKNOWN_POINT: return PL_NACK_UNKNOWN_POINT;
    case ALM_BAD_TIME:      return PL_NACK_BAD_TIME;
    default:                return 0;
    }
}

static uint16_t do_status(pi_link_t *l, uint32_t now)
{
    alm_status_t s;
    alm_status(l->alm, now, &s);
    put16(&resp[0], s.a);
    put16(&resp[2], s.b);
    put16(&resp[4], s.c);
    put16(&resp[6], s.drift);
    put16(&resp[8], s.fault);
    put16(&resp[10], s.disabled);
    resp[12] = s.relays_logical;
    resp[13] = s.relays;
    resp[14] = s.led_yellow_b;
    resp[15] = s.sounders;
    resp[16] = (uint8_t)((s.transmission_blocked ? 0x01u : 0u) | (s.silenced ? 0x02u : 0u) |
                         (s.service_active ? 0x04u : 0u) | (s.key_tech ? 0x08u : 0u) |
                         (s.buzzer ? 0x10u : 0u) | (s.led_red_a ? 0x20u : 0u) |
                         (s.led_yellow_fault ? 0x40u : 0u) | (s.led_green_drift ? 0x80u : 0u));
    resp[17] = (uint8_t)((s.led_green_power ? 0x01u : 0u) | (s.led_yellow_service ? 0x02u : 0u) |
                         (s.hmi_lost ? 0x04u : 0u));
    put32(&resp[18], s.service_remaining_s);
    put32(&resp[22], s.time);
    put32(&resp[26], s.uptime_s);
    put32(&resp[30], s.config_version);
    put32(&resp[34], s.config_crc);
    put32(&resp[38], s.last_seq);
    uint8_t n = 0;
    for (uint16_t addr = 1; addr <= ALM_MAX_ADDR && n < 16u; addr++) {
        if ((l->alm->lost[addr >> 3] >> (addr & 7u)) & 1u) {
            resp[PL_STATUS_LEN + 2u * n] = l->alm->lost_line[addr];
            resp[PL_STATUS_LEN + 2u * n + 1u] = (uint8_t)addr;
            n++;
        }
    }
    resp[42] = n;
    return (uint16_t)(PL_STATUS_LEN + 2u * n);
}

static uint16_t do_points(pi_link_t *l, const uint8_t *p, uint32_t now)
{
    uint16_t from = get16(p);
    uint8_t max = p[2] > 50u ? 50u : p[2];
    const alm_t *a = l->alm;
    uint8_t count = 0;
    put16(&resp[0], a->n);
    for (uint16_t i = from; i < a->n && count < max; i++, count++) {
        uint8_t *r = &resp[3u + PL_POINT_REC_LEN * count];
        int16_t left = alm_pending_left_s(a, (int)i, now);
        put16(&r[0], a->cfg[i].id);
        r[2] = a->cfg[i].category;
        r[3] = a->rt[i].state;
        r[4] = a->rt[i].acked;
        r[5] = a->rt[i].fault;
        r[6] = (uint8_t)alm_is_enabled(a, (int)i);
        r[7] = alm_display_category(a, (int)i);
        r[8] = alm_condition(a, (int)i);
        r[9] = left < 0 ? 0xFFu : (uint8_t)(left > 254 ? 254 : left);
    }
    resp[2] = count;
    return (uint16_t)(3u + PL_POINT_REC_LEN * count);
}

static uint16_t do_modules(pi_link_t *l)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < l->module_count && count < 40u; i++, count++) {
        const ctrl_module_t *m = &l->modules[i];
        uint8_t *r = &resp[1u + PL_MODULE_REC_LEN * count];
        r[0] = m->addr;
        r[1] = m->line;
        r[2] = (uint8_t)m->state;
        put16(&r[3], m->inputs);
        put16(&r[5], m->status);
        put16(&r[7], m->supply_mv);
        put16(&r[9], m->fw_version);
        r[11] = m->fails;
    }
    resp[0] = count;
    return (uint16_t)(1u + PL_MODULE_REC_LEN * count);
}

static uint16_t do_events(pi_link_t *l, const uint8_t *p)
{
    static alm_event_t ev[24];
    uint8_t max = p[4] > 24u ? 24u : p[4];
    uint16_t n = alm_events(l->alm, get32(p), ev, max);
    resp[0] = (uint8_t)n;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t *r = &resp[1u + PL_EVENT_REC_LEN * i];
        put32(&r[0], ev[i].seq);
        put32(&r[4], ev[i].time);
        r[8] = ev[i].type;
        r[9] = ev[i].kind;
        r[10] = ev[i].user;
        put16(&r[11], ev[i].source_id);
        put32(&r[13], (uint32_t)ev[i].old_v);
        put32(&r[17], (uint32_t)ev[i].new_v);
    }
    return (uint16_t)(1u + PL_EVENT_REC_LEN * n);
}

static void unpack_point(const uint8_t *r, alm_point_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->id = get16(&r[0]);
    c->line = r[2];
    c->module = r[3];
    c->channel = r[4];
    c->category = r[5];
    c->device_type = r[6];
    c->contact_nc = r[7];
    c->delay_s = r[8];
    c->enabled = r[9];
    c->section = get16(&r[10]);
    memcpy(c->lcd1, &r[12], ALM_LCD_LEN);
    memcpy(c->lcd2, &r[12 + ALM_LCD_LEN], ALM_LCD_LEN);
}

/* returns the answer length, or sets *nack (and the answer holds the NACK details) */
static uint16_t handle(pi_link_t *l, uint8_t type, const uint8_t *p, uint16_t len, uint32_t now,
                       uint8_t *nack)
{
    alm_t *a = l->alm;
#define NEED(n) do { if (len != (n)) { *nack = PL_NACK_BAD_LENGTH; return 0; } } while (0)
    switch (type) {
    case PL_PING: {
        alm_status_t s;
        alm_status(a, now, &s);
        resp[0] = PL_VERSION;
        resp[1] = PI_FW_MAJOR;
        resp[2] = PI_FW_MINOR;
        put32(&resp[3], s.config_version);
        put32(&resp[7], s.config_crc);
        put32(&resp[11], s.uptime_s);
        put32(&resp[15], s.last_seq);
        return 19;
    }
    case PL_GET_STATUS:
        return do_status(l, now);
    case PL_GET_POINTS:
        NEED(3);
        return do_points(l, p, now);
    case PL_GET_MODULES:
        return do_modules(l);
    case PL_GET_EVENTS:
        NEED(5);
        return do_events(l, p);

    case PL_CFG_BEGIN: {
        NEED(2);
        uint16_t n = get16(p);
        if (n > ALM_MAX_POINTS) {
            *nack = PL_NACK_CFG_TOO_MANY;
            return 0;
        }
        l->cfg_expected = n;
        l->cfg_received = 0;
        l->cfg_state = 1;
        return 0;
    }
    case PL_CFG_PUT: {
        if (len % PL_CFG_REC_LEN != 0u) {
            *nack = PL_NACK_BAD_LENGTH;
            return 0;
        }
        uint16_t k = (uint16_t)(len / PL_CFG_REC_LEN);
        if (l->cfg_state != 1u || l->cfg_received + k > l->cfg_expected) {
            *nack = PL_NACK_CFG_SEQUENCE;
            return 0;
        }
        for (uint16_t i = 0; i < k; i++) {
            unpack_point(&p[i * PL_CFG_REC_LEN], &staging[l->cfg_received++]);
        }
        return 0;
    }
    case PL_CFG_END: {
        NEED(4);
        if (l->cfg_state != 1u || l->cfg_received != l->cfg_expected) {
            *nack = PL_NACK_CFG_SEQUENCE;
            return 0;
        }
        uint32_t crc = alm_config_crc(staging, l->cfg_received);
        if (crc != get32(p)) {
            *nack = PL_NACK_CFG_CRC_MISMATCH;
            put32(&resp[0], crc);
            l->cfg_state = 0;
            return 4;
        }
        static alm_cfg_error_t errs[100];
        uint16_t nerr = alm_validate(staging, l->cfg_received, errs, 100);
        put16(&resp[0], nerr);
        uint16_t shown = nerr > 100u ? 100u : nerr;
        for (uint16_t i = 0; i < shown; i++) {
            put16(&resp[2u + 4u * i], errs[i].point_id);
            resp[4u + 4u * i] = errs[i].field;
            resp[5u + 4u * i] = errs[i].code;
        }
        l->cfg_state = nerr ? 0u : 2u;
        return (uint16_t)(2u + 4u * shown);
    }
    case PL_CFG_ACTIVATE: {
        NEED(1);
        if (l->cfg_state != 2u) {
            *nack = PL_NACK_CFG_SEQUENCE;
            return 0;
        }
        l->cfg_state = 0;
        if (alm_load(a, staging, l->cfg_received, p[0], now, NULL, 0) != 0) {
            *nack = PL_NACK_CFG_SEQUENCE;
            return 0;
        }
        put32(&resp[0], a->config_version);
        put32(&resp[4], a->config_crc);
        return 8;
    }
    case PL_CFG_INFO:
        put32(&resp[0], a->config_version);
        put32(&resp[4], a->config_crc);
        put16(&resp[8], a->n);
        return 10;

    case PL_CMD_ACK:
        NEED(1);
        put16(&resp[0], alm_ack(a, p[0], now));
        return 2;
    case PL_CMD_SILENCE: {
        NEED(2);
        alm_result_t r = alm_silence(a, p[1], p[0], now);
        if (r != ALM_OK) {
            *nack = nack_of(r);
            return 0;
        }
        resp[0] = a->silenced;
        return 1;
    }
    case PL_CMD_RESET: {
        NEED(1);
        static uint16_t ids[50];
        uint16_t reset = 0, remaining = 0;
        alm_result_t r = alm_reset(a, p[0], now, &reset, ids, 50, &remaining);
        if (r != ALM_OK) {
            *nack = nack_of(r);
            return 0;
        }
        put16(&resp[0], reset);
        put16(&resp[2], remaining);
        uint16_t shown = remaining > 50u ? 50u : remaining;
        for (uint16_t i = 0; i < shown; i++) {
            put16(&resp[4u + 2u * i], ids[i]);
        }
        return (uint16_t)(4u + 2u * shown);
    }
    case PL_CMD_DISABLE: {
        NEED(4);
        alm_result_t r = alm_disable(a, get16(p), p[2], p[3], now);
        if (r != ALM_OK) {
            *nack = nack_of(r);
            return 0;
        }
        resp[0] = p[2] ? 0u : 1u;
        return 1;
    }
    case PL_CMD_SERVICE: {
        NEED(3);
        alm_result_t r = alm_service(a, get16(p), p[2], now);
        if (r != ALM_OK) {
            *nack = nack_of(r);
            return 0;
        }
        resp[0] = a->service_active;
        return 1;
    }
    case PL_SET_TIME: {
        NEED(6);
        alm_result_t r = p[5] ? alm_sync_time(a, get32(p), now) : alm_set_time(a, get32(p), p[4], now);
        if (r == ALM_ACCESS_DENIED && p[5]) {
            r = ALM_OK;   /* automatic sync: the clock was already set, nothing to do */
        }
        if (r != ALM_OK) {
            *nack = nack_of(r);
            return 0;
        }
        put32(&resp[0], alm_time(a, now));
        return 4;
    }
    case PL_DEV_SET_KEY:
        NEED(1);
        alm_set_key(a, p[0]);
        return 0;
    default:
        *nack = PL_NACK_UNKNOWN_TYPE;
        return 0;
    }
#undef NEED
}

static void dispatch(pi_link_t *l, uint8_t type, uint8_t seq, const uint8_t *p, uint16_t len, uint32_t now)
{
    if (is_command(type) && l->have_last && seq == l->last_seq && type == l->last_type) {
        /* the Pi did not get our answer and asks again: answer again, do not run it twice */
        send_frame(l, l->last_answer_type, seq, l->last_answer, l->last_answer_len);
        return;
    }
    l->have_last = 0;   /* any other frame in between: a later equal seq is a new request */
    uint8_t nack = 0;
    uint16_t n = handle(l, type, p, len, now, &nack);
    uint8_t rtype = type;
    const uint8_t *payload = resp;
    uint8_t nack_buf[8];
    if (nack) {
        nack_buf[0] = nack;
        memcpy(&nack_buf[1], resp, n > 4u ? 4u : n);   /* e.g. the CRC of CFG_CRC_MISMATCH */
        n = (uint16_t)(1u + (n > 4u ? 4u : n));
        payload = nack_buf;
        rtype = PL_NACK;
    }
    if (is_command(type)) {
        l->have_last = 1;
        l->last_seq = seq;
        l->last_type = type;
        l->last_answer_type = rtype;
        l->last_answer_len = n;
        memcpy(l->last_answer, payload, l->last_answer_len);
    }
    send_frame(l, rtype, seq, payload, n);
}

void pi_link_rx_byte(pi_link_t *l, uint8_t b, uint32_t now_ms)
{
    if (b != 0u) {
        if (l->rx_len < sizeof(l->rx)) {
            l->rx[l->rx_len++] = b;
        } else {
            l->rx_overflow = 1;
        }
        return;
    }
    /* 0x00: end of a frame */
    uint16_t enc_len = l->rx_len;
    uint8_t overflow = l->rx_overflow;
    l->rx_len = 0;
    l->rx_overflow = 0;
    if (enc_len == 0u) {
        return;
    }
    int n = overflow ? -1 : cobs_decode(l->rx, enc_len, decoded, sizeof(decoded));
    if (n < (int)(PL_HEADER_LEN + 2u)) {
        l->frames_bad++;
        return;
    }
    uint16_t len = get16(&decoded[4]);
    if ((uint16_t)n != PL_HEADER_LEN + len + 2u ||
        crc16_ccitt(decoded, PL_HEADER_LEN + len) != get16(&decoded[PL_HEADER_LEN + len])) {
        l->frames_bad++;
        return;
    }
    l->frames_ok++;
    l->last_rx_ms = now_ms;
    l->ever_heard = 1;
    if (l->alm->hmi_lost) {
        alm_set_hmi_lost(l->alm, 0, now_ms);
    }
    if (decoded[0] != PL_VERSION) {
        uint8_t code = PL_NACK_BAD_VERSION;
        send_frame(l, PL_NACK, decoded[2], &code, 1);
        return;
    }
    dispatch(l, decoded[1], decoded[2], &decoded[PL_HEADER_LEN], len, now_ms);
}

void pi_link_tick(pi_link_t *l, uint32_t now_ms)
{
    if (!l->alm->hmi_lost && now_ms - l->last_rx_ms > PL_HMI_LOST_MS) {
        alm_set_hmi_lost(l->alm, 1, now_ms);
    }
}
