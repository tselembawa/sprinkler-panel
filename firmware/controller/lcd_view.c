/* What the LCD shows - see lcd_view.h */
#include "lcd_view.h"

#include <stdio.h>
#include <string.h>

#define CLOCK_VALID_FROM 1577836800u   /* 2020-01-01: before that the clock was never set */

enum { R_FIRE = 0, R_FAULT, R_B, R_PENDING, R_C, R_DRIFT, R_OFF, R_COUNT, R_NONE = 0xFF };

/* rank of point i, and whether it waits for Kvittera */
static uint8_t rank_of(const alm_t *a, uint16_t i, uint8_t *unacked)
{
    const alm_point_rt_t *rt = &a->rt[i];
    *unacked = 0;
    if (!alm_is_enabled(a, (int)i)) {
        return R_OFF;
    }
    if (rt->state == PS_ALARM || rt->state == PS_ACKED || rt->state == PS_LATCHED) {
        uint8_t cat = alm_display_category(a, (int)i);
        *unacked = (uint8_t)(!rt->acked && cat != 'D');
        switch (cat) {
        case 'A': return R_FIRE;
        case 'B': return R_B;
        case 'C': return R_C;
        case 'D': return R_DRIFT;
        default:  return R_FAULT;
        }
    }
    if (rt->state == PS_PENDING) {
        return R_PENDING;
    }
    if (rt->fault != PF_NONE) {
        return R_FAULT;
    }
    return R_NONE;
}

/* the k-th item in display order; returns the point index or -1. *count = all items */
static int item(const alm_t *a, uint16_t k, uint16_t *count)
{
    int found = -1;
    uint16_t seen = 0;
    for (uint8_t r = 0; r < R_COUNT; r++) {
        for (int want_unacked = 1; want_unacked >= 0; want_unacked--) {
            for (uint16_t i = 0; i < a->n; i++) {
                uint8_t u;
                if (rank_of(a, i, &u) == r && u == (uint8_t)want_unacked) {
                    if (seen == k) {
                        found = (int)i;
                    }
                    seen++;
                }
            }
        }
    }
    *count = seen;
    return found;
}

static void put(char *row, const char *text)
{
    strncpy(row, text, LCD_COLS);
    row[LCD_COLS] = '\0';
}

/* text on the left, more text pushed to the right if both fit */
static void put_lr(char *row, const char *left, const char *right)
{
    size_t l = strlen(left), r = strlen(right);
    if (l > LCD_COLS) {
        l = LCD_COLS;
    }
    memset(row, ' ', LCD_COLS);
    memcpy(row, left, l);
    if (r && l + 1u + r <= LCD_COLS) {
        memcpy(row + LCD_COLS - r, right, r);
        row[LCD_COLS] = '\0';
    } else {
        row[l] = '\0';
    }
}

static const char *fault_text(uint8_t f)
{
    switch (f) {
    case PF_OPEN:        return "AVBROTT";
    case PF_SHORT:       return "KORTSLUTNING";
    case PF_MODULE_LOST: return "MODUL SAKNAS";
    default:             return "";
    }
}

static const char *label_of(uint8_t r)
{
    switch (r) {
    case R_FIRE:    return "BRANDLARM";
    case R_FAULT:   return "FEL";
    case R_B:       return "B-LARM";
    case R_PENDING: return "F\xD6RDR\xD6JNING";
    case R_C:       return "C-LARM";
    case R_DRIFT:   return "DRIFT";
    default:        return "FR\xC5NKOPPLAD";
    }
}

/* ---- Swedish time ---------------------------------------------------------------------- */
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    /* H. Hinnant, "chrono-compatible low-level date algorithms" */
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int32_t)yoe + era * 400 + (*m <= 2u);
}

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2u;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* Unix time of the last Sunday of the month, 01:00 UTC (EU summer time changes) */
static int64_t last_sunday_0100(int32_t year, uint32_t month, uint32_t last_day)
{
    int32_t days = days_from_civil(year, month, last_day);
    int32_t weekday = (days + 4) % 7;       /* 1970-01-01 was a Thursday; 0 = Sunday */
    if (weekday < 0) {
        weekday += 7;
    }
    return (int64_t)(days - weekday) * 86400 + 3600;
}

void lcd_local_time(uint32_t unix_time, char date[11], char hhmm[6])
{
    int32_t y;
    uint32_t m, d;
    civil_from_days((int32_t)(unix_time / 86400u), &y, &m, &d);
    int64_t t = (int64_t)unix_time;
    int summer = t >= last_sunday_0100(y, 3, 31) && t < last_sunday_0100(y, 10, 31);
    t += summer ? 7200 : 3600;
    civil_from_days((int32_t)(t / 86400), &y, &m, &d);
    uint32_t sec = (uint32_t)(t % 86400);
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%04ld-%02lu-%02lu", (long)(y % 10000), (unsigned long)m, (unsigned long)d);
    memcpy(date, tmp, 10);
    date[10] = '\0';
    snprintf(tmp, sizeof(tmp), "%02lu:%02lu", (unsigned long)(sec / 3600u), (unsigned long)(sec / 60u % 60u));
    memcpy(hhmm, tmp, 5);
    hhmm[5] = '\0';
}

/* ---- screens -------------------------------------------------------------------------------- */
void lcd_view_init(lcd_view_t *v)
{
    memset(v, 0, sizeof(*v));
}

void lcd_view_next(lcd_view_t *v, uint32_t now_ms)
{
    v->index++;
    v->next_switch_ms = now_ms + LCD_VIEW_SWITCH_MS;
}

static void quiet_screen(const alm_t *a, uint32_t now_ms, char rows[LCD_ROWS][LCD_COLS + 1])
{
    alm_status_t s;
    alm_status(a, now_ms, &s);
    put(rows[0], "SPRINKLERCENTRAL");
    if (s.service_active) {
        char tmp[40];
        unsigned long min = (s.service_remaining_s + 59u) / 60u;
        snprintf(tmp, sizeof(tmp), "SERVICE %lu MIN KVAR", min > 999u ? 999u : min);
        put(rows[1], tmp);
    } else {
        put(rows[1], "DRIFT NORMAL");
    }
    if (s.time >= CLOCK_VALID_FROM) {
        char date[11], hhmm[6];
        lcd_local_time(s.time, date, hhmm);
        put_lr(rows[2], date, hhmm);
    } else {
        put(rows[2], "TID EJ INST\xC4LLD");
    }
    if (a->n == 0) {
        put(rows[3], "INGEN KONFIGURATION");
    } else if (s.hmi_lost) {
        put(rows[3], "PANEL (HMI) SAKNAS");
    } else {
        rows[3][0] = '\0';
    }
}

void lcd_view_render(lcd_view_t *v, const alm_t *a, uint32_t now_ms, char rows[LCD_ROWS][LCD_COLS + 1])
{
    for (uint8_t r = 0; r < LCD_ROWS; r++) {
        rows[r][0] = '\0';
    }
    uint16_t count;
    int first = item(a, 0, &count);
    if (count == 0) {
        v->count = 0;
        quiet_screen(a, now_ms, rows);
        return;
    }
    uint16_t first_id = a->cfg[first].id;
    if (count != v->count || first_id != v->first_id) {
        /* something new or something gone: start again from the most important */
        v->count = count;
        v->first_id = first_id;
        v->index = 0;
        v->next_switch_ms = now_ms + LCD_VIEW_SWITCH_MS;
    } else if ((int32_t)(now_ms - v->next_switch_ms) >= 0) {
        v->index++;
        v->next_switch_ms = now_ms + LCD_VIEW_SWITCH_MS;
    }
    if (v->index >= count) {
        v->index = 0;
    }
    uint16_t dummy;
    int i = item(a, v->index, &dummy);
    if (i < 0) {
        i = first;
    }
    const alm_point_cfg_t *c = &a->cfg[i];
    const alm_point_rt_t *rt = &a->rt[i];
    uint8_t unacked;
    uint8_t r = rank_of(a, (uint16_t)i, &unacked);

    char nn[12] = "";
    if (count > 1u) {
        snprintf(nn, sizeof(nn), "%u/%u", (unsigned)(v->index + 1u), (unsigned)count);
    }
    put_lr(rows[0], label_of(r), nn);
    put(rows[1], c->lcd1);
    put(rows[2], c->lcd2);

    char addr[16];
    snprintf(addr, sizeof(addr), "L%u-M%02u-IN%u", c->line, c->module, c->channel);
    char state[32] = "";
    switch (r) {
    case R_FAULT:
        if (rt->state == PS_NORMAL || rt->state == PS_PENDING) {
            put_lr(rows[3], fault_text(rt->fault), addr);
            return;
        }
        /* a fault-category point in alarm: like the others */
        /* fall through */
    case R_FIRE:
    case R_B:
    case R_C:
    case R_DRIFT:
        if (r == R_DRIFT) {
            strcpy(state, "AKTIV");
        } else if (rt->state == PS_LATCHED) {
            strcpy(state, "\xC5TERST\xC4LL MED NYCKEL");
        } else {
            strcpy(state, unacked ? "EJ KVITTERAD" : "KVITTERAD");
        }
        if (rt->fault != PF_NONE) {
            strncat(state, " +", sizeof(state) - strlen(state) - 1u);
            strncat(state, fault_text(rt->fault), sizeof(state) - strlen(state) - 1u);
        }
        put(rows[3], state);
        return;
    case R_PENDING: {
        int16_t left = alm_pending_left_s(a, i, now_ms);
        snprintf(state, sizeof(state), "LARM OM %d S", left < 0 ? 0 : left);
        put(rows[3], state);
        return;
    }
    default:
        put(rows[3], addr);
        return;
    }
}
