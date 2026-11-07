/* What the LCD shows - see lcd_view.h */
#include "lcd_view.h"

#include <stdio.h>
#include <string.h>

#define CLOCK_VALID_FROM 1577836800u   /* 2020-01-01: before that the clock was never set */

enum { R_FIRE = 0, R_FAULT, R_B, R_PENDING, R_C, R_DRIFT, R_OFF, R_COUNT, R_NONE = 0xFF };

/* ---- fixed texts, UTF-8 here, at most 20 characters on the display ------------------------- */
enum {
    T_TITLE, T_NORMAL, T_SERVICE, T_NO_TIME, T_NO_CONFIG, T_HMI_LOST,
    T_FIRE, T_FAULT, T_B, T_PENDING, T_C, T_DRIFT, T_OFF,
    T_UNACKED, T_ACKED, T_LATCHED, T_OPEN, T_SHORT, T_MODULE_LOST, T_DELAY, T_ACTIVE, T_COUNT
};

static const char *const texts[LANG_COUNT][T_COUNT] = {
    [LANG_SV] = {"SPRINKLERCENTRAL", "DRIFT NORMAL", "SERVICE %ld MIN KVAR", "TID EJ INSTÄLLD",
                 "INGEN KONFIGURATION", "PANEL (HMI) SAKNAS",
                 "BRANDLARM", "FEL", "B-LARM", "FÖRDRÖJNING", "C-LARM", "DRIFT", "FRÅNKOPPLAD",
                 "EJ KVITTERAD", "KVITTERAD", "ÅTERSTÄLL MED NYCKEL", "AVBROTT", "KORTSLUTNING",
                 "MODUL SAKNAS", "LARM OM %ld S", "AKTIV"},
    [LANG_EN] = {"SPRINKLER PANEL", "NORMAL OPERATION", "SERVICE %ld MIN LEFT", "TIME NOT SET",
                 "NO CONFIGURATION", "PANEL (HMI) MISSING",
                 "FIRE ALARM", "FAULT", "B ALARM", "DELAY", "C ALARM", "OPERATION", "DISABLED",
                 "NOT ACKNOWLEDGED", "ACKNOWLEDGED", "RESET WITH KEY", "OPEN CIRCUIT", "SHORT CIRCUIT",
                 "MODULE MISSING", "ALARM IN %ld S", "ACTIVE"},
    [LANG_FI] = {"SPRINKLERKESKUS", "NORMAALI TILA", "HUOLTO %ld MIN", "AIKAA EI ASETETTU",
                 "EI KONFIGURAATIOTA", "HMI-PANEELI PUUTTUU",
                 "PALOHÄLYTYS", "VIKA", "B-HÄLYTYS", "VIIVE", "C-HÄLYTYS", "KÄYNTI", "POIS KYTKETTY",
                 "KUITTAAMATTA", "KUITATTU", "PALAUTA AVAIMELLA", "KATKOS", "OIKOSULKU",
                 "MODUULI PUUTTUU", "HÄLYTYS %ld S", "AKTIIVINEN"},
    [LANG_NO] = {"SPRINKLERSENTRAL", "NORMAL DRIFT", "SERVICE %ld MIN IGJ.", "TID IKKE STILT",
                 "INGEN KONFIGURASJON", "PANEL (HMI) MANGLER",
                 "BRANNALARM", "FEIL", "B-ALARM", "FORSINKELSE", "C-ALARM", "DRIFT", "FRAKOBLET",
                 "IKKE KVITTERT", "KVITTERT", "NULLSTILL MED NØKKEL", "BRUDD", "KORTSLUTNING",
                 "MODUL MANGLER", "ALARM OM %ld S", "AKTIV"},
    [LANG_DA] = {"SPRINKLERCENTRAL", "NORMAL DRIFT", "SERVICE %ld MIN", "TID IKKE INDSTILLET",
                 "INGEN KONFIGURATION", "PANEL (HMI) MANGLER",
                 "BRANDALARM", "FEJL", "B-ALARM", "FORSINKELSE", "C-ALARM", "DRIFT", "FRAKOBLET",
                 "IKKE KVITTERET", "KVITTERET", "NULSTIL MED NØGLE", "AFBRYDELSE", "KORTSLUTNING",
                 "MODUL MANGLER", "ALARM OM %ld S", "AKTIV"},
    [LANG_EL] = {"ΠΙΝΑΚΑΣ SPRINKLER", "ΚΑΝΟΝΙΚΗ ΛΕΙΤΟΥΡΓΙΑ", "SERVICE %ld ΛΕΠΤΑ", "ΔΕΝ ΕΧΕΙ ΟΡΙΣΤΕΙ ΩΡΑ",
                 "ΔΕΝ ΥΠΑΡΧΟΥΝ ΣΗΜΕΙΑ", "HMI ΕΚΤΟΣ ΣΥΝΔΕΣΗΣ",
                 "ΠΥΡΚΑΓΙΑ", "ΣΦΑΛΜΑ", "ΣΥΝΑΓΕΡΜΟΣ B", "ΚΑΘΥΣΤΕΡΗΣΗ", "ΣΥΝΑΓΕΡΜΟΣ C", "ΛΕΙΤΟΥΡΓΙΑ",
                 "ΑΠΕΝΕΡΓΟΠΟΙΗΜΕΝΟ", "ΔΕΝ ΑΝΑΓΝΩΡΙΣΤΗΚΕ", "ΑΝΑΓΝΩΡΙΣΤΗΚΕ", "ΕΠΑΝΑΦΟΡΑ ΜΕ ΚΛΕΙΔΙ",
                 "ΔΙΑΚΟΠΗ", "ΒΡΑΧΥΚΥΚΛΩΜΑ", "ΑΠΩΛΕΙΑ MODULE", "ΣΥΝΑΓΕΡΜΟΣ ΣΕ %ld S", "ΕΝΕΡΓΟ"},
    /* Russian: words chosen so that one screen needs at most 8 letters of our own */
    [LANG_RU] = {"ПУЛЬТ СПРИНКЛЕРА", "НОРМАЛЬНАЯ РАБОТА", "СЕРВИС: %ld МИН", "ВРЕМЯ НЕ УСТАНОВЛЕНО",
                 "НЕТ ТОЧЕК", "НЕТ СВЯЗИ С HMI",
                 "ПОЖАР", "НЕИСПРАВНОСТЬ", "ТРЕВОГА B", "ЗАДЕРЖКА", "ТРЕВОГА C", "РАБОТА", "ОТКЛЮЧЕНО",
                 "НЕ КВИТИРОВАНО", "КВИТИРОВАНО", "СБРОС КЛЮЧОМ", "ОБРЫВ", "КОРОТКОЕ ЗАМЫКАНИЕ",
                 "НЕТ МОДУЛЯ", "ТРЕВОГА ЧЕРЕЗ %ld С", "АКТИВНО"},
};

/* Greek capitals U+0391..U+03A9: the Latin look-alike, or a letter of our own */
static const uint8_t greek_caps[25] = {
    'A', 'B', LCD_GR_GAMMA, LCD_GR_DELTA, 'E', 'Z', 'H', LCD_GR_THETA, 'I', 'K', LCD_GR_LAMDA, 'M',
    'N', LCD_GR_XI, 'O', LCD_GR_PI, 'P', '?', LCD_GR_SIGMA, 'T', 'Y', LCD_GR_PHI, 'X',
    LCD_GR_PSI, LCD_GR_OMEGA,
};

/* Cyrillic capitals U+0410..U+042F (А..Я) */
static const uint8_t cyrillic_caps[32] = {
    'A', LCD_CY_BE, 'B', LCD_GR_GAMMA, LCD_CY_DE, 'E', LCD_CY_ZHE, LCD_CY_ZE,
    LCD_CY_I, LCD_CY_SHORT_I, 'K', LCD_CY_EL, 'M', 'H', 'O', LCD_GR_PI,
    'P', 'C', 'T', LCD_CY_U, LCD_GR_PHI, 'X', LCD_CY_TSE, LCD_CY_CHE,
    LCD_CY_SHA, LCD_CY_SHCHA, LCD_CY_HARD, LCD_CY_YERU, LCD_CY_SOFT, LCD_CY_E, LCD_CY_YU, LCD_CY_YA,
};

void lcd_text(char *dst, size_t dst_size, const char *utf8)
{
    const uint8_t *s = (const uint8_t *)utf8;
    size_t n = 0;
    while (*s && n + 1u < dst_size) {
        uint32_t cp;
        if (s[0] < 0x80u) {
            cp = *s++;
        } else if ((s[0] & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
            s += 2;
        } else {
            /* 3- and 4-byte characters are not used: skip the whole character */
            s++;
            while ((*s & 0xC0u) == 0x80u) {
                s++;
            }
            cp = '?';
        }
        uint8_t out;
        if (cp < 0x100u) {
            out = (uint8_t)cp;                    /* ASCII and Latin-1 (Å Ä Ö Æ Ø ...) */
        } else if (cp >= 0x391u && cp <= 0x3A9u) {
            out = greek_caps[cp - 0x391u];
        } else if (cp >= 0x410u && cp <= 0x42Fu) {
            out = cyrillic_caps[cp - 0x410u];
        } else if (cp == 0x401u) {
            out = 'E';                            /* Ё */
        } else {
            out = '?';
        }
        dst[n++] = (char)out;
    }
    dst[n] = '\0';
}

static const char *txt(uint8_t lang, unsigned t)
{
    return texts[lang < LANG_COUNT ? lang : LANG_SV][t];
}

/* a fixed text in LCD form; up to two numbers through printf */
static void tx(char *dst, size_t size, uint8_t lang, unsigned t, long num)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), txt(lang, t), num);
    lcd_text(dst, size, tmp);
}

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

static unsigned fault_t(uint8_t f)
{
    switch (f) {
    case PF_OPEN:  return T_OPEN;
    case PF_SHORT: return T_SHORT;
    default:       return T_MODULE_LOST;
    }
}

static unsigned label_t(uint8_t r)
{
    switch (r) {
    case R_FIRE:    return T_FIRE;
    case R_FAULT:   return T_FAULT;
    case R_B:       return T_B;
    case R_PENDING: return T_PENDING;
    case R_C:       return T_C;
    case R_DRIFT:   return T_DRIFT;
    default:        return T_OFF;
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

static void quiet_screen(const alm_t *a, uint8_t lang, uint32_t now_ms, char rows[LCD_ROWS][LCD_COLS + 1])
{
    alm_status_t s;
    alm_status(a, now_ms, &s);
    char tmp[64];
    tx(tmp, sizeof(tmp), lang, T_TITLE, 0);
    put(rows[0], tmp);
    if (s.service_active) {
        unsigned long min = (s.service_remaining_s + 59u) / 60u;
        tx(tmp, sizeof(tmp), lang, T_SERVICE, (long)(min > 999u ? 999u : min));
    } else {
        tx(tmp, sizeof(tmp), lang, T_NORMAL, 0);
    }
    put(rows[1], tmp);
    if (s.time >= CLOCK_VALID_FROM) {
        char date[11], hhmm[6];
        lcd_local_time(s.time, date, hhmm);
        put_lr(rows[2], date, hhmm);
    } else {
        tx(tmp, sizeof(tmp), lang, T_NO_TIME, 0);
        put(rows[2], tmp);
    }
    if (a->n == 0) {
        tx(tmp, sizeof(tmp), lang, T_NO_CONFIG, 0);
        put(rows[3], tmp);
    } else if (s.hmi_lost) {
        tx(tmp, sizeof(tmp), lang, T_HMI_LOST, 0);
        put(rows[3], tmp);
    } else {
        rows[3][0] = '\0';
    }
}

void lcd_view_render(lcd_view_t *v, const alm_t *a, uint8_t lang, uint32_t now_ms,
                     char rows[LCD_ROWS][LCD_COLS + 1])
{
    for (uint8_t r = 0; r < LCD_ROWS; r++) {
        rows[r][0] = '\0';
    }
    uint16_t count;
    int first = item(a, 0, &count);
    if (count == 0) {
        v->count = 0;
        quiet_screen(a, lang, now_ms, rows);
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
    char label[64];
    tx(label, sizeof(label), lang, label_t(r), 0);
    put_lr(rows[0], label, nn);
    put(rows[1], c->lcd1);
    put(rows[2], c->lcd2);

    char addr[16];
    snprintf(addr, sizeof(addr), "L%u-M%02u-IN%u", c->line, c->module, c->channel);
    char state[64] = "", tmp[64];
    switch (r) {
    case R_FAULT:
        if (rt->state == PS_NORMAL || rt->state == PS_PENDING) {
            tx(tmp, sizeof(tmp), lang, fault_t(rt->fault), 0);
            put_lr(rows[3], tmp, addr);
            return;
        }
        /* a fault-category point in alarm: like the others */
        /* fall through */
    case R_FIRE:
    case R_B:
    case R_C:
    case R_DRIFT:
        tx(state, sizeof(state), lang,
           r == R_DRIFT ? T_ACTIVE : rt->state == PS_LATCHED ? T_LATCHED : unacked ? T_UNACKED : T_ACKED, 0);
        if (rt->fault != PF_NONE) {
            tx(tmp, sizeof(tmp), lang, fault_t(rt->fault), 0);
            strncat(state, " +", sizeof(state) - strlen(state) - 1u);
            strncat(state, tmp, sizeof(state) - strlen(state) - 1u);
        }
        put(rows[3], state);
        return;
    case R_PENDING: {
        int16_t left = alm_pending_left_s(a, i, now_ms);
        tx(state, sizeof(state), lang, T_DELAY, left < 0 ? 0 : left);
        put(rows[3], state);
        return;
    }
    default:
        put(rows[3], addr);
        return;
    }
}
