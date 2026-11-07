/*
 * LCD driver and LCD screens, on the PC. The driver talks to a model of an HD44780 behind
 * a PCF8574 (it decodes the I2C bytes like the real chips do), so what the test reads back
 * is what the display would show.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Icommon -Icontroller -o /tmp/test_lcd \
 *       tests/test_lcd.c controller/lcd.c controller/lcd_view.c controller/alarm.c && /tmp/test_lcd
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "lcd.h"
#include "lcd_view.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b)) != 0) { \
    printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); failures++; } } while (0)

/* ---- model of PCF8574 + HD44780 (20x4) ---------------------------------------------------- */
static struct {
    uint8_t present_addr;      /* 7-bit address that answers */
    uint8_t last_pins;
    int mode8;                 /* after power-up the HD44780 is in 8-bit mode */
    int have_high;
    uint8_t high;
    uint8_t ddram[0x80];
    uint8_t cgram[64];
    int in_cgram;
    uint8_t ac;                /* address counter */
    int display_on;
    long bytes;
} m;

static void hd_reset(uint8_t addr)
{
    memset(&m, 0, sizeof(m));
    m.present_addr = addr;
    m.mode8 = 1;
    memset(m.ddram, ' ', sizeof(m.ddram));
}

static void hd_exec(uint8_t v, int rs)
{
    if (rs) {
        if (m.in_cgram) {
            m.cgram[m.ac & 63u] = v;
            m.ac = (uint8_t)((m.ac + 1u) & 63u);
        } else {
            m.ddram[m.ac & 0x7Fu] = v;
            m.ac++;
            if (m.ac == 0x28u) m.ac = 0x40u;          /* 2-line mode wraps */
            else if (m.ac == 0x68u) m.ac = 0x00u;
        }
        return;
    }
    if (v & 0x80u) { m.ac = v & 0x7Fu; m.in_cgram = 0; }
    else if (v & 0x40u) { m.ac = v & 0x3Fu; m.in_cgram = 1; }
    else if (v & 0x20u) { if (!(v & 0x10u)) m.mode8 = 0; }
    else if (v & 0x08u) { m.display_on = (v & 0x04u) != 0; }
    else if (v == 0x01u) { memset(m.ddram, ' ', sizeof(m.ddram)); m.ac = 0; m.in_cgram = 0; }
}

static int model_write(void *ctx, uint8_t addr7, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (addr7 != m.present_addr) return -1;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t p = data[i];
        m.bytes++;
        if ((m.last_pins & 0x04u) && !(p & 0x04u)) {          /* EN falling edge */
            uint8_t nib = m.last_pins & 0xF0u;
            int rs = m.last_pins & 0x01u;
            if (m.mode8) {
                hd_exec(nib, rs);
            } else if (!m.have_high) {
                m.high = nib; m.have_high = 1;
            } else {
                m.have_high = 0;
                hd_exec((uint8_t)(m.high | (nib >> 4)), rs);
            }
        }
        m.last_pins = p;
    }
    return 0;
}

static void no_delay(uint32_t ms) { (void)ms; }

static void screen_row(int row, char out[LCD_COLS + 1])
{
    static const uint8_t base[4] = {0x00, 0x40, 0x14, 0x54};
    for (unsigned c = 0; c < LCD_COLS; c++) out[c] = (char)m.ddram[base[row] + c];
    out[LCD_COLS] = 0;
    for (int c = LCD_COLS - 1; c >= 0 && out[c] == ' '; c--) out[c] = 0;   /* trim for compare */
}

/* ---- driver tests -------------------------------------------------------------------------- */
static void test_finds_address_and_writes_rows(void)
{
    lcd_t l;
    hd_reset(0x3F);
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == 0);
    CHECK(l.addr7 == 0x3F && l.ok && m.display_on && !m.mode8);
    lcd_set_row(&l, 0, "HELLO");
    lcd_set_row(&l, 2, "THIRD ROW");
    lcd_set_row(&l, 3, "12345678901234567890TOO LONG");
    while (lcd_flush(&l, 7)) { }
    char r[21];
    screen_row(0, r); CHECK_STR(r, "HELLO");
    screen_row(1, r); CHECK_STR(r, "");
    screen_row(2, r); CHECK_STR(r, "THIRD ROW");
    screen_row(3, r); CHECK_STR(r, "12345678901234567890");

    /* only the changed characters go out again */
    long before = m.bytes;
    lcd_set_row(&l, 0, "HELLA");
    CHECK(lcd_flush(&l, 100) == 0);
    CHECK(m.bytes - before == 8);          /* set address (4 bytes) + one character (4 bytes) */
    screen_row(0, r); CHECK_STR(r, "HELLA");
}

static void test_no_display(void)
{
    lcd_t l;
    hd_reset(0x50);                        /* nothing at 0x27 / 0x3F */
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == -1);
    CHECK(!l.ok && l.addr7 == 0);
    lcd_set_row(&l, 0, "X");
    CHECK(lcd_flush(&l, 10) > 0);           /* nothing sent, still to do */
}

static void test_swedish_letters(void)
{
    lcd_t l;
    hd_reset(0x27);
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == 0);
    lcd_set_row(&l, 0, "\xC5\xC4\xD6 \xE5\xE4\xF6");     /* ÅÄÖ åäö in Latin-1 */
    while (lcd_flush(&l, 50)) { }
    const uint8_t want[] = {0x08, 0x09, 0x0A, ' ', 0x0B, 0xE1, 0xEF};
    CHECK(memcmp(m.ddram, want, sizeof(want)) == 0);
    /* the dot pictures were loaded: row 1 of Å is the ring (01010), of Ä the dots (01010) */
    CHECK(m.cgram[0 * 8 + 0] == 0x04 && m.cgram[0 * 8 + 1] == 0x0A);
    CHECK(m.cgram[2 * 8 + 6] == 0x0E);
    /* ROM A02: Latin-1 codes as they are, no places used */
    hd_reset(0x27);
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A02) == 0);
    lcd_set_row(&l, 0, "\xC5\xC4\xD6 \xE5\xE4\xF6\x01");
    while (lcd_flush(&l, 50)) { }
    const uint8_t want2[] = {0xC5, 0xC4, 0xD6, ' ', 0xE5, 0xE4, 0xF6, '?'};
    CHECK(memcmp(m.ddram, want2, sizeof(want2)) == 0);
}

static void test_display_unplugged_then_back(void)
{
    lcd_t l;
    hd_reset(0x27);
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == 0);
    m.present_addr = 0;                    /* cable out */
    lcd_set_row(&l, 0, "ABC");
    lcd_flush(&l, 10);
    CHECK(!l.ok && l.errors > 0);
    hd_reset(0x27);                        /* back (powered up again) */
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == 0);
    lcd_set_row(&l, 0, "ABC");
    while (lcd_flush(&l, 10)) { }
    char r[21];
    screen_row(0, r); CHECK_STR(r, "ABC");
}

/* ---- time ----------------------------------------------------------------------------------- */
static void test_swedish_time(void)
{
    char d[11], t[6];
    lcd_local_time(1794091260u, d, t); CHECK_STR(d, "2026-11-07"); CHECK_STR(t, "23:41");   /* CET */
    lcd_local_time(1782907200u, d, t); CHECK_STR(d, "2026-07-01"); CHECK_STR(t, "14:00");   /* CEST */
    lcd_local_time(1774745999u, d, t); CHECK_STR(t, "01:59");     /* just before summer time */
    lcd_local_time(1774746000u, d, t); CHECK_STR(t, "03:00");     /* summer time starts */
    lcd_local_time(1792889999u, d, t); CHECK_STR(t, "02:59");     /* last minute of summer time */
    lcd_local_time(1792890000u, d, t); CHECK_STR(t, "02:00");     /* winter time again */
    lcd_local_time(1798759800u, d, t); CHECK_STR(d, "2027-01-01"); CHECK_STR(t, "00:30");   /* new year */
}

/* ---- screens -------------------------------------------------------------------------------- */
static alm_t eng;
static uint32_t now = 1000;
static uint8_t lang = LANG_SV;

static void rows_of(lcd_view_t *v, char rows[4][21])
{
    lcd_view_render(v, &eng, lang, now, rows);
}

static void test_screens(void)
{
    static const alm_point_cfg_t pts[] = {
        {1, 1, 2, 1, CAT_B, DT_SERVICEVENTIL, 1, 0, 1, 1, "SERVICEVENTIL", "SEKTION 1"},
        {2, 1, 2, 2, CAT_A, DT_PRESSOSTAT, 0, 0, 1, 1, "PRESSOSTAT", "SEKTION 1 \xC5\xC4\xD6"},
        {3, 1, 2, 3, CAT_A, DT_PRESSOSTAT, 0, 10, 1, 2, "PRESSOSTAT", "SEKTION 2"},
    };
    alm_init(&eng, now, 0, NULL, NULL);
    lcd_view_t v;
    lcd_view_init(&v);
    char r[4][21];

    rows_of(&v, r);
    CHECK_STR(r[0], "SPRINKLERCENTRAL");
    CHECK_STR(r[2], "TID EJ INST\xC4LLD");
    CHECK_STR(r[3], "INGEN KONFIGURATION");

    CHECK(alm_load(&eng, pts, 3, 0, now, NULL, 0) == 0);
    alm_sync_time(&eng, 1794091260u, now);
    for (uint8_t ch = 1; ch <= 3; ch++) {
        alm_set_input(&eng, 1, 2, ch, ch == 1 ? 0 : 1, now);   /* serviceventil closed, others open */
    }
    rows_of(&v, r);
    CHECK_STR(r[1], "DRIFT NORMAL");
    CHECK_STR(r[2], "2026-11-07     23:41");
    CHECK_STR(r[3], "");

    /* serviceventil opens: B alarm */
    alm_set_input(&eng, 1, 2, 1, 1, now);
    rows_of(&v, r);
    CHECK_STR(r[0], "B-LARM");
    CHECK_STR(r[1], "SERVICEVENTIL");
    CHECK_STR(r[2], "SEKTION 1");
    CHECK_STR(r[3], "EJ KVITTERAD");

    /* pressostat 2 closes: fire comes first, two items */
    alm_set_input(&eng, 1, 2, 2, 0, now);
    rows_of(&v, r);
    CHECK_STR(r[0], "BRANDLARM        1/2");
    CHECK_STR(r[2], "SEKTION 1 \xC5\xC4\xD6");
    now += LCD_VIEW_SWITCH_MS;
    rows_of(&v, r);
    CHECK_STR(r[0], "B-LARM           2/2");
    now += LCD_VIEW_SWITCH_MS;
    rows_of(&v, r);
    CHECK_STR(r[0], "BRANDLARM        1/2");

    /* Kvittera: B becomes C, fire stays (latching, acknowledged) */
    alm_ack(&eng, 1, now);
    rows_of(&v, r);
    CHECK_STR(r[0], "BRANDLARM        1/2");
    CHECK_STR(r[3], "KVITTERAD");
    now += LCD_VIEW_SWITCH_MS;
    rows_of(&v, r);
    CHECK_STR(r[0], "C-LARM           2/2");

    /* the cable of the serviceventil is cut while it is in C: state + fault */
    alm_set_input(&eng, 1, 2, 1, 2, now);     /* LINE_OPEN */
    for (int k = 0; k < 2; k++) {
        rows_of(&v, r);
        if (strncmp(r[0], "C-LARM", 6) == 0) break;
        now += LCD_VIEW_SWITCH_MS;
    }
    CHECK_STR(r[3], "KVITTERAD +AVBROTT");

    /* water stops: pressostat latched until reset with the key */
    alm_set_input(&eng, 1, 2, 2, 1, now);
    lcd_view_init(&v);
    rows_of(&v, r);
    CHECK_STR(r[0], "BRANDLARM        1/2");
    CHECK_STR(r[3], "\xC5TERST\xC4LL MED NYCKEL");

    /* delayed pressostat 3 */
    alm_set_key(&eng, 1);
    uint16_t reset = 0, remaining = 0;
    alm_reset(&eng, 1, now, &reset, NULL, 0, &remaining);
    alm_set_input(&eng, 1, 2, 1, 0, now);     /* serviceventil OK again */
    alm_set_input(&eng, 1, 2, 3, 0, now);     /* pressostat 3 closes: 10 s delay */
    lcd_view_init(&v);
    rows_of(&v, r);
    CHECK_STR(r[0], "F\xD6RDR\xD6JNING");
    CHECK_STR(r[3], "LARM OM 10 S");

    /* a line fault alone, with the address */
    alm_set_input(&eng, 1, 2, 3, 1, now);
    alm_set_input(&eng, 1, 2, 1, 3, now);     /* LINE_SHORT on the serviceventil */
    rows_of(&v, r);
    CHECK_STR(r[0], "FEL");
    CHECK_STR(r[3], "KORTSLUTNING");          /* 12 + address 10 does not fit: address left out */
    alm_set_input(&eng, 1, 2, 1, 2, now);
    rows_of(&v, r);
    CHECK_STR(r[3], "AVBROTT   L1-M02-IN1");

    /* disconnected point */
    alm_set_input(&eng, 1, 2, 1, 0, now);
    alm_disable(&eng, 3, 1, 1, now);
    rows_of(&v, r);
    CHECK_STR(r[0], "FR\xC5NKOPPLAD");
    CHECK_STR(r[3], "L1-M02-IN3");
}

/* how many letters of our own a screen needs (must fit in the 8 places) */
static int own_letters(char r[4][21])
{
    int seen[256] = {0}, n = 0;
    static const uint8_t nordic[] = {0xC5, 0xC4, 0xD6, 0xE5, 0xC6, 0xD8, 0xE6, 0xF8};
    for (int k = 0; k < 4; k++) {
        for (const uint8_t *p = (const uint8_t *)r[k]; *p; p++) {
            int own = (*p >= 0x80 && *p < LCD_EXTRA_END) || memchr(nordic, *p, sizeof(nordic)) != NULL;
            if (own && !seen[*p]) { seen[*p] = 1; n++; }
        }
    }
    return n;
}

static void test_languages_and_greek(void)
{
    /* the fixed texts follow the language, the point texts stay */
    static const alm_point_cfg_t pts[] = {
        {2, 1, 2, 2, CAT_A, DT_PRESSOSTAT, 0, 0, 1, 1, "PRESSOSTAT", "SEKTION 1"},
    };
    alm_init(&eng, now, 0, NULL, NULL);
    CHECK(alm_load(&eng, pts, 1, 0, now, NULL, 0) == 0);
    alm_set_input(&eng, 1, 2, 2, 0, now);      /* water: fire alarm */
    lcd_view_t v;
    char r[4][21];

    lang = LANG_EN; lcd_view_init(&v); rows_of(&v, r);
    CHECK_STR(r[0], "FIRE ALARM"); CHECK_STR(r[1], "PRESSOSTAT"); CHECK_STR(r[3], "NOT ACKNOWLEDGED");
    lang = LANG_FI; lcd_view_init(&v); rows_of(&v, r);
    CHECK_STR(r[0], "PALOH\xC4LYTYS");
    lang = LANG_NO; alm_ack(&eng, 1, now); lcd_view_init(&v); rows_of(&v, r);
    CHECK_STR(r[3], "KVITTERT");
    alm_set_input(&eng, 1, 2, 2, 1, now);      /* latched */
    rows_of(&v, r);
    CHECK_STR(r[3], "NULLSTILL MED N\xD8KKEL");

    /* Greek: own characters for Γ Δ Θ Λ Π Σ Φ Ω, Latin letters for the rest */
    lang = LANG_EL; lcd_view_init(&v); rows_of(&v, r);
    const char want0[] = {LCD_GR_PI, 'Y', 'P', 'K', 'A', LCD_GR_GAMMA, 'I', 'A', 0};   /* ΠΥΡΚΑΓΙΑ */
    CHECK_STR(r[0], want0);
    const char want3[] = {'E', LCD_GR_PI, 'A', 'N', 'A', LCD_GR_PHI, 'O', 'P', 'A', ' ', 'M', 'E', ' ',
                          'K', LCD_GR_LAMDA, 'E', 'I', LCD_GR_DELTA, 'I', 0};          /* ΕΠΑΝΑΦΟΡΑ ΜΕ ΚΛΕΙΔΙ */
    CHECK_STR(r[3], want3);

    /* Russian */
    lang = LANG_RU; lcd_view_init(&v);
    alm_init(&eng, now, 0, NULL, NULL);
    CHECK(alm_load(&eng, pts, 1, 0, now, NULL, 0) == 0);
    alm_set_input(&eng, 1, 2, 2, 0, now);
    rows_of(&v, r);
    const char ru0[] = {LCD_GR_PI, 'O', LCD_CY_ZHE, 'A', 'P', 0};                  /* ПОЖАР */
    CHECK_STR(r[0], ru0);
    const char ru3[] = {'H', 'E', ' ', 'K', 'B', LCD_CY_I, 'T', LCD_CY_I, 'P', 'O', 'B', 'A', 'H', 'O', 0};
    CHECK_STR(r[3], ru3);                                                            /* НЕ КВИТИРОВАНО */

    /* on the display: Greek, Swedish and Cyrillic letters share the 8 places */
    lcd_t l;
    hd_reset(0x27);
    CHECK(lcd_init(&l, model_write, NULL, no_delay, LCD_ROM_A00) == 0);
    lcd_set_row(&l, 0, want0);                              /* ΠΥΡΚΑΓΙΑ: Π place 0, Γ place 1 */
    lcd_set_row(&l, 1, "\xC5\xC4\xD6");                    /* ÅÄÖ: places 2, 3, 4 */
    while (lcd_flush(&l, 50)) { }
    char row[21];
    screen_row(0, row);
    CHECK(row[0] == 0x08 + 0 && row[5] == 0x08 + 1);
    CHECK(m.cgram[0] == 0x1F && m.cgram[1] == 0x11);        /* Π */
    CHECK(m.cgram[8] == 0x1F && m.cgram[9] == 0x10);        /* Γ */
    screen_row(1, row);
    CHECK((uint8_t)row[0] == 0x0A && (uint8_t)row[1] == 0x0B && (uint8_t)row[2] == 0x0C);
    CHECK(m.cgram[2 * 8 + 0] == 0x04 && m.cgram[2 * 8 + 1] == 0x0A);   /* Å (ring) */

    /* a new screen with 10 letters of our own: 8 get a place, the last two a look-alike */
    const char ten[] = {LCD_CY_BE, LCD_CY_DE, LCD_CY_ZHE, LCD_CY_ZE, LCD_CY_I, LCD_CY_SHORT_I,
                        LCD_CY_EL, LCD_CY_U, LCD_CY_TSE, LCD_CY_CHE, 0};
    lcd_set_row(&l, 0, ten);
    lcd_set_row(&l, 1, "");
    while (lcd_flush(&l, 50)) { }
    screen_row(0, row);
    for (int k = 0; k < 8; k++) CHECK((uint8_t)row[k] >= 0x08 && (uint8_t)row[k] <= 0x0F);
    CHECK(row[8] == 'U' && row[9] == '4');                  /* Ц, Ч */
    int slot_b = row[0] - 0x08;
    CHECK(m.cgram[slot_b * 8 + 3] == 0x1E);                  /* Б has its belly in row 3 */
    /* the same letter again: same place, nothing sent to CGRAM */
    const char again[] = {LCD_CY_BE, 0};
    lcd_set_row(&l, 2, again);
    lcd_flush(&l, 50);
    CHECK(l.slot_reload == 0);
    screen_row(2, row);
    CHECK(row[0] == 0x08 + slot_b);

    /* every fixed text of every language fits on the display */
    for (uint8_t lg = 0; lg < LANG_COUNT; lg++) {
        lang = lg;
        lcd_view_init(&v);
        alm_init(&eng, now, 0, NULL, NULL);
        rows_of(&v, r);
        for (int k = 0; k < 4; k++) CHECK(strlen(r[k]) <= 20 && strchr(r[k], '?') == NULL);
        CHECK(own_letters(r) <= 8);
        /* fire alarm with a cut cable, acknowledged, and a second item */
        CHECK(alm_load(&eng, pts, 1, 0, now, NULL, 0) == 0);
        alm_sync_time(&eng, 1794091260u, now);
        alm_set_input(&eng, 1, 2, 2, 0, now);
        rows_of(&v, r);
        CHECK(own_letters(r) <= 8);
        alm_set_input(&eng, 1, 2, 2, 2, now);
        rows_of(&v, r);
        CHECK(own_letters(r) <= 8);
        alm_ack(&eng, 1, now);
        rows_of(&v, r);
        CHECK(own_letters(r) <= 8);
        alm_set_hmi_lost(&eng, 1, now);
        alm_init(&eng, now, 0, NULL, NULL);
        alm_set_hmi_lost(&eng, 1, now);
        rows_of(&v, r);
        CHECK(own_letters(r) <= 8);
    }
    char t[32];
    lcd_text(t, sizeof(t), "ΞΨ\xE2\x82\xAC");   /* Greek we draw, and the euro sign we cannot */
    const char want_t[] = {LCD_GR_XI, LCD_GR_PSI, '?', 0};
    CHECK_STR(t, want_t);
    lang = LANG_SV;
}

int main(void)
{
    test_finds_address_and_writes_rows();
    test_no_display();
    test_swedish_letters();
    test_display_unplugged_then_back();
    test_swedish_time();
    test_screens();
    test_languages_and_greek();
    printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
