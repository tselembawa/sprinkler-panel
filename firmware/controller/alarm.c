/* Alarm engine - see alarm.h. Kept close to hmi/app/sim_controller.py on purpose. */
#include "alarm.h"
#include "modbus_map.h"

#include <stddef.h>
#include <string.h>

#define INPUT_UNKNOWN 0xFFu

typedef enum { COND_NORMAL = 0, COND_ACTIVE, COND_OPEN, COND_SHORT } cond_t;

/* ---- helpers ---------------------------------------------------------------------------- */
static size_t text_len(const char *s, size_t max)   /* strnlen without needing POSIX */
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static int is_latching(uint8_t device_type)
{
    return device_type == DT_PRESSOSTAT || device_type == DT_FLODESVAKT;   /* decided */
}

static int in_alarm(uint8_t st)
{
    return st == PS_ALARM || st == PS_ACKED || st == PS_LATCHED;
}

static int lost_get(const alm_t *a, uint8_t addr)
{
    return (a->lost[addr >> 3] >> (addr & 7u)) & 1u;
}

uint32_t alm_time(const alm_t *a, uint32_t now_ms)
{
    return (uint32_t)(a->time_offset + (int64_t)(now_ms / 1000u));
}

static void event(alm_t *a, uint8_t type, uint8_t kind, uint16_t sid,
                  int32_t old_v, int32_t new_v, uint8_t user)
{
    alm_event_t *e = &a->events[a->ev_head];
    e->seq = ++a->seq;
    e->time = alm_time(a, a->now_ms);
    e->type = type;
    e->kind = kind;
    e->source_id = sid;
    e->old_v = old_v;
    e->new_v = new_v;
    e->user = user;
    a->ev_head = (uint16_t)((a->ev_head + 1u) % ALM_EVENT_BUF);
    if (a->ev_count < ALM_EVENT_BUF) {
        a->ev_count++;
    }
    if (a->on_event != NULL) {
        a->on_event(a->ctx, e);
    }
}

int alm_find(const alm_t *a, uint16_t point_id)
{
    for (uint16_t i = 0; i < a->n; i++) {
        if (a->cfg[i].id == point_id) {
            return (int)i;
        }
    }
    return -1;
}

int alm_is_enabled(const alm_t *a, int i)
{
    return a->cfg[i].enabled && !a->rt[i].disabled;
}

/* logical condition of a point from the physical input and NO/NC */
static cond_t condition(const alm_t *a, int i)
{
    const alm_point_cfg_t *c = &a->cfg[i];
    if (c->module > ALM_MAX_ADDR || c->channel < 1u || c->channel > 4u) {
        return COND_NORMAL;
    }
    switch (a->inputs[c->module][c->channel - 1u]) {
    case MB_IN_CLOSED:        return c->contact_nc ? COND_NORMAL : COND_ACTIVE;
    case MB_IN_OPEN:          return c->contact_nc ? COND_ACTIVE : COND_NORMAL;
    case MB_IN_LINE_OPEN:     return COND_OPEN;
    case MB_IN_MEASURE_FAULT: return COND_OPEN;
    case MB_IN_LINE_SHORT:    return COND_SHORT;
    default:                  return COND_NORMAL;   /* unknown / not used */
    }
}

static void set_state(alm_t *a, int i, uint8_t st, int log, uint8_t user)
{
    uint8_t old = a->rt[i].state;
    a->rt[i].state = st;
    a->rt[i].changed_ms = a->now_ms;
    if (log >= 0) {
        event(a, (uint8_t)log, SRC_POINT, a->cfg[i].id, old, st, user);
    }
}

static void raise_alarm(alm_t *a, int i, int auto_ack)
{
    a->rt[i].acked = (uint8_t)auto_ack;
    set_state(a, i, auto_ack ? PS_ACKED : PS_ALARM, EVT_ALARM_ON, 0);
    if (a->silenced && (a->cfg[i].category == CAT_A || a->cfg[i].category == CAT_B)) {
        a->silenced = 0;   /* a new alarm always makes the sounders sound again */
        event(a, EVT_RESOUND, SRC_POINT, a->cfg[i].id, 0, 0, 0);
    }
}

static void evaluate(alm_t *a, int i)
{
    const alm_point_cfg_t *c = &a->cfg[i];
    alm_point_rt_t *rt = &a->rt[i];
    const uint32_t now = a->now_ms;

    if (!alm_is_enabled(a, i)) {
        rt->state = PS_NORMAL;
        rt->acked = 1;
        rt->fault = PF_NONE;
        rt->pending_since_ms = 0;
        return;
    }

    cond_t cond = condition(a, i);

    /* line supervision (independent of the alarm state) */
    uint8_t fault;
    if (c->module <= ALM_MAX_ADDR && lost_get(a, c->module)) {
        fault = PF_MODULE_LOST;
    } else if (cond == COND_OPEN) {
        fault = PF_OPEN;
    } else if (cond == COND_SHORT) {
        fault = PF_SHORT;
    } else {
        fault = PF_NONE;
    }
    if (fault != rt->fault) {
        if (fault == PF_OPEN || fault == PF_SHORT) {
            event(a, fault == PF_OPEN ? EVT_LINE_OPEN : EVT_LINE_SHORT, SRC_POINT, c->id, 0, 0, 0);
            a->fault_unacked = 1;
        } else if (fault == PF_NONE && (rt->fault == PF_OPEN || rt->fault == PF_SHORT)) {
            event(a, EVT_LINE_OK, SRC_POINT, c->id, 0, 0, 0);
        }
        rt->fault = fault;
    }

    /* a faulty line cannot report an alarm, but an existing alarm is kept */
    const int active = cond == COND_ACTIVE && fault == PF_NONE;
    const int latching = is_latching(c->device_type);
    const int auto_ack = c->category == CAT_DRIFT;   /* indications need no acknowledge */

    switch (rt->state) {
    case PS_NORMAL:
        if (active) {
            if (c->delay_s > 0u) {
                rt->pending_since_ms = now;
                set_state(a, i, PS_PENDING, EVT_DELAY_STARTED, 0);
            } else {
                raise_alarm(a, i, auto_ack);
            }
        }
        break;
    case PS_PENDING:
        if (!active) {
            rt->pending_since_ms = 0;
            set_state(a, i, PS_NORMAL, EVT_DELAY_CANCELLED, 0);
        } else if (now - rt->pending_since_ms >= (uint32_t)c->delay_s * 1000u) {
            rt->pending_since_ms = 0;
            raise_alarm(a, i, auto_ack);
        }
        break;
    case PS_ALARM:
    case PS_ACKED:
        if (!active && fault == PF_NONE) {
            if (latching) {
                set_state(a, i, PS_LATCHED, EVT_ALARM_CLEARED, 0);
            } else {
                rt->acked = 1;
                set_state(a, i, PS_NORMAL, EVT_ALARM_CLEARED, 0);
            }
        }
        break;
    case PS_LATCHED:
        if (active) {
            raise_alarm(a, i, auto_ack);
        }
        break;
    default:
        break;
    }
}

static void evaluate_all(alm_t *a)
{
    /* in id order, like the simulator (events come out in the same order) */
    uint16_t last = 0;
    for (uint16_t k = 0; k < a->n; k++) {
        int best = -1;
        for (uint16_t i = 0; i < a->n; i++) {
            if (a->cfg[i].id > last && (best < 0 || a->cfg[i].id < a->cfg[best].id)) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        last = a->cfg[best].id;
        evaluate(a, best);
    }
}

/* ---- life cycle ------------------------------------------------------------------------- */
void alm_init(alm_t *a, uint32_t now_ms, uint32_t start_seq, alm_event_fn on_event, void *ctx)
{
    memset(a, 0, sizeof(*a));
    memset(a->inputs, INPUT_UNKNOWN, sizeof(a->inputs));
    a->now_ms = a->started_ms = now_ms;
    a->seq = start_seq;        /* the real controller keeps this in FRAM across restarts */
    a->on_event = on_event;
    a->ctx = ctx;
    a->config_crc = alm_config_crc(NULL, 0);
    event(a, EVT_STARTUP, SRC_SYSTEM, 0, 0, 0, 0);
}

/* ---- configuration ---------------------------------------------------------------------- */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

/* canonical binary record of one point: fixed layout, little endian, LCD text padded */
static uint32_t crc_point(uint32_t crc, const alm_point_cfg_t *p)
{
    uint8_t b[12 + 2u * ALM_LCD_LEN];
    memset(b, 0, sizeof(b));
    b[0] = (uint8_t)(p->id & 0xFFu);
    b[1] = (uint8_t)(p->id >> 8);
    b[2] = p->line;
    b[3] = p->module;
    b[4] = p->channel;
    b[5] = p->category;
    b[6] = p->device_type;
    b[7] = p->contact_nc ? 1u : 0u;
    b[8] = p->delay_s;
    b[9] = p->enabled ? 1u : 0u;
    b[10] = (uint8_t)(p->section & 0xFFu);
    b[11] = (uint8_t)(p->section >> 8);
    memcpy(&b[12], p->lcd1, text_len(p->lcd1, ALM_LCD_LEN));
    memcpy(&b[12 + ALM_LCD_LEN], p->lcd2, text_len(p->lcd2, ALM_LCD_LEN));
    return crc32_update(crc, b, sizeof(b));
}

uint32_t alm_config_crc(const alm_point_cfg_t *pts, uint16_t n)
{
    uint32_t crc = 0;
    uint16_t last = 0;
    for (uint16_t k = 0; k < n; k++) {      /* points in id order */
        int best = -1;
        for (uint16_t i = 0; i < n; i++) {
            if (pts[i].id > last && (best < 0 || pts[i].id < pts[best].id)) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        last = pts[best].id;
        crc = crc_point(crc, &pts[best]);
    }
    return crc;
}

static int same_cfg(const alm_point_cfg_t *x, const alm_point_cfg_t *y)
{
    return x->id == y->id && x->line == y->line && x->module == y->module &&
           x->channel == y->channel && x->category == y->category &&
           x->device_type == y->device_type && !x->contact_nc == !y->contact_nc &&
           x->delay_s == y->delay_s && !x->enabled == !y->enabled && x->section == y->section &&
           strncmp(x->lcd1, y->lcd1, ALM_LCD_LEN) == 0 && strncmp(x->lcd2, y->lcd2, ALM_LCD_LEN) == 0;
}

static uint16_t validate(const alm_point_cfg_t *pts, uint16_t n, alm_cfg_error_t *errs, uint16_t max)
{
    uint16_t count = 0;
#define ERR(pid, fld, cd) do { if (count < max && errs) { errs[count].point_id = (pid); \
    errs[count].field = (fld); errs[count].code = (cd); } count++; } while (0)

    if (n > ALM_MAX_POINTS) {
        ERR(0, CF_ID, CERR_TOO_MANY);
        return count;
    }
    for (uint16_t i = 0; i < n; i++) {
        const alm_point_cfg_t *p = &pts[i];
        if (p->id < 1u || p->id > 999u) ERR(p->id, CF_ID, CERR_OUT_OF_RANGE);
        if (p->line < 1u || p->line > 4u) ERR(p->id, CF_LINE, CERR_OUT_OF_RANGE);
        if (p->module < 1u || p->module > ALM_MAX_ADDR) ERR(p->id, CF_MODULE, CERR_OUT_OF_RANGE);
        if (p->channel < 1u || p->channel > 4u) ERR(p->id, CF_CHANNEL, CERR_OUT_OF_RANGE);
        for (uint16_t j = 0; j < i; j++) {
            if (pts[j].id == p->id) {
                ERR(p->id, CF_ID, CERR_DUPLICATE_ID);
                break;
            }
        }
        for (uint16_t j = 0; j < i; j++) {
            if (pts[j].line == p->line && pts[j].module == p->module && pts[j].channel == p->channel) {
                ERR(p->id, CF_CHANNEL, CERR_DUPLICATE_ADDRESS);
                break;
            }
        }
        if (p->category >= CAT_COUNT) ERR(p->id, CF_CATEGORY, CERR_INVALID);
        if (p->device_type >= DT_COUNT) ERR(p->id, CF_DEVICE_TYPE, CERR_INVALID);
        if (p->delay_s > ALM_MAX_DELAY_S) ERR(p->id, CF_DELAY, CERR_DELAY_NOT_ALLOWED);
        if (text_len(p->lcd1, ALM_LCD_LEN + 1u) > ALM_LCD_LEN) ERR(p->id, CF_LCD1, CERR_TOO_LONG);
        if (text_len(p->lcd2, ALM_LCD_LEN + 1u) > ALM_LCD_LEN) ERR(p->id, CF_LCD2, CERR_TOO_LONG);
    }
#undef ERR
    return count;
}

/* the previous configuration while the new one is installed (static: too big for the stack) */
static alm_point_cfg_t old_cfg[ALM_MAX_POINTS];
static alm_point_rt_t old_rt[ALM_MAX_POINTS];

uint16_t alm_load(alm_t *a, const alm_point_cfg_t *pts, uint16_t n, uint8_t user, uint32_t now_ms,
                  alm_cfg_error_t *errors, uint16_t max_errors)
{
    a->now_ms = now_ms;
    uint16_t nerr = validate(pts, n, errors, max_errors);
    if (nerr) {
        return nerr;
    }
    const uint16_t old_n = a->n;
    memcpy(old_cfg, a->cfg, sizeof(alm_point_cfg_t) * old_n);
    memcpy(old_rt, a->rt, sizeof(alm_point_rt_t) * old_n);

    for (uint16_t i = 0; i < n; i++) {
        a->cfg[i] = pts[i];
        a->cfg[i].lcd1[ALM_LCD_LEN] = '\0';
        a->cfg[i].lcd2[ALM_LCD_LEN] = '\0';
        memset(&a->rt[i], 0, sizeof(alm_point_rt_t));
        a->rt[i].acked = 1;
        a->rt[i].changed_ms = now_ms;
        for (uint16_t j = 0; j < old_n; j++) {
            if (old_cfg[j].id != pts[i].id) {
                continue;
            }
            if (same_cfg(&old_cfg[j], &pts[i])) {
                a->rt[i] = old_rt[j];               /* unchanged point keeps its state */
            } else {
                a->rt[i].disabled = old_rt[j].disabled;   /* disablement survives edits */
            }
            break;
        }
    }
    a->n = n;
    a->config_version++;
    a->config_crc = alm_config_crc(pts, n);
    event(a, EVT_CONFIG_ACTIVATED, SRC_SYSTEM, 0, 0, (int32_t)a->config_version, user);
    evaluate_all(a);
    return 0;
}

/* ---- inputs from the modules -------------------------------------------------------------- */
void alm_set_input(alm_t *a, uint8_t line, uint8_t addr, uint8_t channel, uint8_t mb_in, uint32_t now_ms)
{
    (void)line;   /* addresses are unique over all lines */
    a->now_ms = now_ms;
    if (addr == 0u || addr > ALM_MAX_ADDR || channel < 1u || channel > 4u) {
        return;
    }
    a->inputs[addr][channel - 1u] = mb_in;
    evaluate_all(a);
}

void alm_set_module_lost(alm_t *a, uint8_t line, uint8_t addr, int lost, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (addr == 0u || addr > ALM_MAX_ADDR) {
        return;
    }
    const uint16_t sid = (uint16_t)(line * 1000u + addr);
    const uint8_t bit = (uint8_t)(1u << (addr & 7u));
    if (lost && !lost_get(a, addr)) {
        a->lost[addr >> 3] |= bit;
        a->lost_line[addr] = line;
        a->lost_count++;
        event(a, EVT_MODULE_LOST, SRC_MODULE, sid, 0, 0, 0);
        a->fault_unacked = 1;
    } else if (!lost && lost_get(a, addr)) {
        a->lost[addr >> 3] &= (uint8_t)~bit;
        a->lost_count--;
        event(a, EVT_MODULE_BACK, SRC_MODULE, sid, 0, 0, 0);
    }
    evaluate_all(a);
}

void alm_set_key(alm_t *a, int tech)
{
    a->key_tech = tech ? 1u : 0u;
}

void alm_tick(alm_t *a, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (a->service_active && (int32_t)(now_ms - a->service_until_ms) >= 0) {
        a->service_active = 0;
        event(a, EVT_SERVICE_OFF, SRC_SYSTEM, 0, 0, 0, 0);   /* new_v 0 = timeout */
    }
    evaluate_all(a);
}

/* ---- commands ------------------------------------------------------------------------------ */
uint16_t alm_ack(alm_t *a, uint8_t user, uint32_t now_ms)
{
    a->now_ms = now_ms;
    uint16_t n = 0;
    for (uint16_t i = 0; i < a->n; i++) {
        alm_point_rt_t *rt = &a->rt[i];
        if (rt->state == PS_ALARM) {
            set_state(a, (int)i, PS_ACKED, -1, user);
            rt->acked = 1;
            n++;
        } else if (rt->state == PS_LATCHED && !rt->acked) {
            rt->acked = 1;
            n++;
        }
    }
    if (a->fault_unacked) {
        a->fault_unacked = 0;
        n++;
    }
    event(a, EVT_ACK, SRC_SYSTEM, 0, 0, 0, user);
    return n;
}

alm_result_t alm_reset(alm_t *a, uint8_t user, uint32_t now_ms, uint16_t *reset_count,
                       uint16_t *remaining_ids, uint16_t max_remaining, uint16_t *remaining_count)
{
    a->now_ms = now_ms;
    if (!a->key_tech) {
        return ALM_ACCESS_DENIED;
    }
    uint16_t reset = 0, remaining = 0;
    for (uint16_t i = 0; i < a->n; i++) {
        alm_point_rt_t *rt = &a->rt[i];
        if (rt->state == PS_LATCHED) {
            rt->acked = 1;
            set_state(a, (int)i, PS_NORMAL, -1, user);
            reset++;
        } else if ((rt->state == PS_ALARM || rt->state == PS_ACKED) && is_latching(a->cfg[i].device_type)) {
            if (remaining_ids != NULL && remaining < max_remaining) {
                remaining_ids[remaining] = a->cfg[i].id;
            }
            remaining++;
        }
    }
    event(a, EVT_RESET, SRC_SYSTEM, 0, 0, 0, user);
    if (a->silenced) {
        int any = 0;
        for (uint16_t i = 0; i < a->n; i++) {
            any |= in_alarm(a->rt[i].state);
        }
        if (!any) {
            a->silenced = 0;
        }
    }
    if (reset_count) *reset_count = reset;
    if (remaining_count) *remaining_count = remaining;
    return ALM_OK;
}

alm_result_t alm_silence(alm_t *a, int silence, uint8_t user, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (!a->key_tech) {
        return ALM_ACCESS_DENIED;
    }
    if ((silence ? 1u : 0u) != a->silenced) {
        a->silenced = silence ? 1u : 0u;
        event(a, silence ? EVT_SILENCE : EVT_UNSILENCE, SRC_SYSTEM, 0, 0, 0, user);
    }
    return ALM_OK;
}

alm_result_t alm_service(alm_t *a, uint16_t minutes, uint8_t user, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (!a->key_tech) {
        return ALM_ACCESS_DENIED;
    }
    if (minutes > 480u) {
        return ALM_BAD_DURATION;
    }
    if (minutes == 0u) {
        if (a->service_active) {
            a->service_active = 0;
            event(a, EVT_SERVICE_OFF, SRC_SYSTEM, 0, 0, 1, user);   /* new_v 1 = manual */
        }
    } else {
        a->service_active = 1;
        a->service_until_ms = now_ms + (uint32_t)minutes * 60000u;
        event(a, EVT_SERVICE_ON, SRC_SYSTEM, 0, 0, minutes, user);
    }
    return ALM_OK;
}

alm_result_t alm_disable(alm_t *a, uint16_t point_id, int disabled, uint8_t user, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (!a->key_tech) {
        return ALM_ACCESS_DENIED;
    }
    int i = alm_find(a, point_id);
    if (i < 0) {
        return ALM_UNKNOWN_POINT;
    }
    a->rt[i].disabled = disabled ? 1u : 0u;
    event(a, disabled ? EVT_POINT_DISABLED : EVT_POINT_ENABLED, SRC_POINT, point_id, 0, 0, user);
    evaluate(a, i);
    return ALM_OK;
}

alm_result_t alm_set_time(alm_t *a, uint32_t unix_time, uint8_t user, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if (!a->key_tech) {
        return ALM_ACCESS_DENIED;
    }
    if (unix_time < 946684800u || unix_time > 4102444800u) {   /* 2000 .. 2100 */
        return ALM_BAD_TIME;
    }
    uint32_t old = alm_time(a, now_ms);
    a->time_offset = (int64_t)unix_time - (int64_t)(now_ms / 1000u);
    event(a, EVT_TIME_SET, SRC_SYSTEM, 0, (int32_t)old, (int32_t)unix_time, user);
    return ALM_OK;
}

/* ---- status ------------------------------------------------------------------------------- */
uint8_t alm_display_category(const alm_t *a, int i)
{
    switch (a->cfg[i].category) {
    case CAT_A:     return 'A';
    case CAT_B:     return a->rt[i].state == PS_ACKED ? 'C' : 'B';
    case CAT_DRIFT: return 'D';
    default:        return 'F';
    }
}

void alm_status(const alm_t *a, uint32_t now_ms, alm_status_t *s)
{
    memset(s, 0, sizeof(*s));
    uint16_t fault_points = 0;
    int unacked = a->fault_unacked;
    int fire = 0;
    for (uint16_t i = 0; i < a->n; i++) {
        const alm_point_rt_t *rt = &a->rt[i];
        if (!alm_is_enabled(a, (int)i)) {
            s->disabled++;
            continue;
        }
        const uint8_t cat = a->cfg[i].category;
        const int alarm = in_alarm(rt->state);
        if (rt->fault != PF_NONE) fault_points++;
        if (cat == CAT_A && alarm) { s->a++; fire = 1; }
        if (cat == CAT_B && alarm && !rt->acked) s->b++;
        if (cat == CAT_B && rt->state == PS_ACKED) s->c++;
        if (cat == CAT_DRIFT && alarm) s->drift++;
        if (cat == CAT_FAULT && alarm) fault_points++;
        if (cat != CAT_DRIFT && alarm && !rt->acked) unacked = 1;
    }
    s->fault = (uint16_t)(fault_points + a->lost_count);

    uint8_t logical = 0;
    if (s->a) logical |= ALM_K1_A;
    if (s->b) logical |= ALM_K2_B;
    if (s->fault == 0u) logical |= ALM_K3_FAULT_OK;
    if (s->c) logical |= ALM_K4_C;
    s->relays_logical = logical;
    s->service_active = a->service_active;
    /* in service mode nothing is transmitted: K1/K2/K4 idle, K3 stays energized for
       software-detected faults (a dead controller still drops K3 through the hardware) */
    s->relays = a->service_active ? ALM_K3_FAULT_OK : logical;
    s->transmission_blocked = a->service_active;

    s->led_red_a = s->a > 0u;
    s->led_yellow_b = s->b ? LED_FLASH : (s->c ? LED_STEADY : LED_OFF);
    s->led_yellow_fault = s->fault > 0u;
    s->led_green_drift = s->drift > 0u;
    s->led_green_power = !a->service_active;
    s->led_yellow_service = a->service_active;
    s->buzzer = (uint8_t)unacked;
    s->sounders = fire ? (a->silenced ? SOUNDERS_SILENCED : SOUNDERS_SOUNDING) : SOUNDERS_OFF;
    s->silenced = a->silenced;
    if (a->service_active && (int32_t)(a->service_until_ms - now_ms) > 0) {
        s->service_remaining_s = (a->service_until_ms - now_ms) / 1000u;
    }
    s->key_tech = a->key_tech;
    s->time = alm_time(a, now_ms);
    s->uptime_s = (now_ms - a->started_ms) / 1000u;
    s->config_version = a->config_version;
    s->config_crc = a->config_crc;
    s->last_seq = a->seq;
    s->lost_modules = a->lost_count;
}

uint16_t alm_events(const alm_t *a, uint32_t from_seq, alm_event_t *out, uint16_t max)
{
    uint16_t n = 0;
    uint16_t start = (uint16_t)((a->ev_head + ALM_EVENT_BUF - a->ev_count) % ALM_EVENT_BUF);
    for (uint16_t k = 0; k < a->ev_count && n < max; k++) {
        const alm_event_t *e = &a->events[(start + k) % ALM_EVENT_BUF];
        if (e->seq > from_seq) {
            out[n++] = *e;
        }
    }
    return n;
}

const char *alm_event_name(uint8_t t)
{
    static const char *const names[EVT_COUNT] = {
        "STARTUP", "CONFIG_ACTIVATED", "DELAY_STARTED", "DELAY_CANCELLED", "ALARM_ON",
        "ALARM_CLEARED", "LINE_OPEN", "LINE_SHORT", "LINE_OK", "MODULE_LOST", "MODULE_BACK",
        "ACK", "RESET", "SILENCE", "UNSILENCE", "RESOUND", "SERVICE_ON", "SERVICE_OFF",
        "POINT_DISABLED", "POINT_ENABLED", "TIME_SET"};
    return t < EVT_COUNT ? names[t] : "?";
}

const char *alm_state_name(uint8_t st)
{
    static const char *const names[] = {"NORMAL", "PENDING", "ALARM", "ACKED", "LATCHED"};
    return st < 5u ? names[st] : "?";
}

const char *alm_fault_name(uint8_t f)
{
    static const char *const names[] = {"", "OPEN", "SHORT", "MODULE_LOST"};
    return f < 4u ? names[f] : "?";
}
