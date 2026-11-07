/*
 * 20x4 character LCD (HD44780 or compatible) behind a PCF8574 I2C backpack (HW-61):
 * P0 RS, P1 RW, P2 EN, P3 backlight, P4-P7 = D4-D7, 4-bit mode.
 *
 * The program writes whole lines into a picture of the screen (lcd_set_row); lcd_flush()
 * sends only the characters that differ, a few per call, so the display never holds up
 * the RS485 polling or the Raspberry Pi link.
 *
 * Text is Latin-1 (what the Pi sends in lcd1/lcd2). Swedish letters: the common ROM A00
 * has only lowercase a/o with umlaut, so Å Ä Ö å are drawn as our own characters (CGRAM);
 * a display with ROM A02 has them all and gets the Latin-1 codes directly.
 *
 * Hardware independent: the I2C write and the delay are functions given to lcd_init(),
 * so the driver is tested on the PC against a model of the display.
 */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_COLS 20u
#define LCD_ROWS 4u

/* write len bytes to the 7-bit I2C address; returns 0 when the device answered (ACK) */
typedef int (*lcd_write_fn)(void *ctx, uint8_t addr7, const uint8_t *data, uint16_t len);
typedef void (*lcd_delay_fn)(uint32_t ms);

typedef enum { LCD_ROM_A00 = 0, LCD_ROM_A02 = 1 } lcd_rom_t;

typedef struct {
    lcd_write_fn write;
    void *ctx;
    lcd_delay_fn delay;
    uint8_t addr7;            /* found address (0x27 or 0x3F), 0 = no display */
    uint8_t ok;               /* initialised and answering */
    uint8_t rom;              /* lcd_rom_t */
    uint8_t backlight;
    uint8_t cursor;           /* DDRAM address the display will write next, 0xFF unknown */
    uint8_t scan;             /* where lcd_flush continues */
    uint32_t errors;
    uint8_t shown[LCD_ROWS][LCD_COLS];   /* what the display shows (display codes) */
    uint8_t want[LCD_ROWS][LCD_COLS];    /* what it should show */
} lcd_t;

/* finds the display (0x27, then 0x3F), initialises it, loads the Swedish letters.
   Returns 0 when a display answered. Safe to call again (e.g. after it was unplugged). */
int lcd_init(lcd_t *l, lcd_write_fn write, void *ctx, lcd_delay_fn delay, lcd_rom_t rom);

/* row 0..3: Latin-1 text, cut at 20 characters and padded with spaces */
void lcd_set_row(lcd_t *l, uint8_t row, const char *text);

/* sends at most max_chars changed characters; returns how many are still different */
uint16_t lcd_flush(lcd_t *l, uint16_t max_chars);

/* Latin-1 character -> display code for the given ROM */
uint8_t lcd_map(uint8_t c, uint8_t rom);

#endif /* LCD_H */
