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
 * unacknowledged before acknowledged. Text is Latin-1 (the LCD driver maps ÅÄÖ).
 * Time is shown in Swedish time (CET/CEST, EU summer time rules).
 */
#ifndef LCD_VIEW_H
#define LCD_VIEW_H

#include <stdint.h>

#include "alarm.h"
#include "lcd.h"

#define LCD_VIEW_SWITCH_MS 3000u

typedef struct {
    uint16_t index;          /* which item is shown */
    uint16_t count;          /* items last time */
    uint16_t first_id;       /* most important item last time */
    uint32_t next_switch_ms;
} lcd_view_t;

void lcd_view_init(lcd_view_t *v);
/* fills rows[0..3] (Latin-1, each at most 20 characters + 0) */
void lcd_view_render(lcd_view_t *v, const alm_t *a, uint32_t now_ms, char rows[LCD_ROWS][LCD_COLS + 1]);
/* show the next item now (for a "next" button later) */
void lcd_view_next(lcd_view_t *v, uint32_t now_ms);

/* Unix time -> "YYYY-MM-DD" and "HH:MM" in Swedish local time */
void lcd_local_time(uint32_t unix_time, char date[11], char hhmm[6]);

#endif /* LCD_VIEW_H */
