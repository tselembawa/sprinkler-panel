/*
 * What the panel's 20x4 LCD shows, worked out from the alarm engine.
 *
 *   Quiet:                       Something active (one item at a time, every 3 s the next):
 *   SPRINKLERCENTRAL             BRANDLARM        1/2
 *   DRIFT NORMAL                 PRESSOSTAT              <- lcd1 of the point
 *   2026-11-07     23:41         SEKTION 1               <- lcd2 of the point
 *                                EJ KVITTERAD
 *
 * Order: fire (A), faults, B, delays, C, operation (Drift), disconnected points;
 * unacknowledged before acknowledged. Time is shown in Swedish time (CET/CEST).
 *
 * The fixed texts follow the language chosen on the Pi (sv en fi no da el); the point
 * texts lcd1/lcd2 are shown as written. Output is Latin-1 plus LCD_GR_* for Greek
 * (the LCD driver turns that into display codes).
 */
#ifndef LCD_VIEW_H
#define LCD_VIEW_H

#include <stddef.h>
#include <stdint.h>

#include "alarm.h"
#include "lcd.h"

#define LCD_VIEW_SWITCH_MS 3000u

/* languages, same order as the HMI (hmi/tools/build_i18n.py LANGS) */
typedef enum { LANG_SV = 0, LANG_EN, LANG_FI, LANG_NO, LANG_DA, LANG_EL, LANG_COUNT } lcd_lang_t;

typedef struct {
    uint16_t index;          /* which item is shown */
    uint16_t count;          /* items last time */
    uint16_t first_id;       /* most important item last time */
    uint32_t next_switch_ms;
} lcd_view_t;

void lcd_view_init(lcd_view_t *v);
/* fills rows[0..3] (Latin-1, each at most 20 characters + 0) */
void lcd_view_render(lcd_view_t *v, const alm_t *a, uint8_t lang, uint32_t now_ms,
                     char rows[LCD_ROWS][LCD_COLS + 1]);
/* the character set the language needs */
lcd_charset_t lcd_view_charset(uint8_t lang);
/* UTF-8 text (as written in the C source) -> LCD text (Latin-1 + LCD_GR_*) */
void lcd_text(char *dst, size_t dst_size, const char *utf8);
/* show the next item now (for a "next" button later) */
void lcd_view_next(lcd_view_t *v, uint32_t now_ms);

/* Unix time -> "YYYY-MM-DD" and "HH:MM" in Swedish local time */
void lcd_local_time(uint32_t unix_time, char date[11], char hhmm[6]);

#endif /* LCD_VIEW_H */
