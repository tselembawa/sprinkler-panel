/*
 * 20x4 character LCD (HD44780 or compatible) behind a PCF8574 I2C backpack (HW-61):
 * P0 RS, P1 RW, P2 EN, P3 backlight, P4-P7 = D4-D7, 4-bit mode.
 *
 * The program writes whole lines into a picture of the screen (lcd_set_row); lcd_flush()
 * sends only the characters that differ, a few per call, so the display never holds up
 * the RS485 polling or the Raspberry Pi link.
 *
 * Text codes (what lcd_set_row takes):
 *   0x20-0x7E  ASCII
 *   0xA0-0xFF  Latin-1 (what the Pi sends in lcd1/lcd2: Å Ä Ö Æ Ø ...)
 *   0x80-0x9B  letters the display does not have: LCD_GR_* (Greek), LCD_CY_* (Cyrillic)
 * The display has room for only 8 letters of our own (CGRAM). They are given out per
 * screen: the letters a screen needs get a place (earlier rows first), letters that no
 * longer appear give their place to new ones. If one screen needs more than 8, the rest
 * are written with a look-alike. Letters the ROM has (A00: ä ö) are used directly.
 *
 * Hardware independent: the I2C write and the delay are functions given to lcd_init(),
 * so the driver is tested on the PC against a model of the display.
 */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_COLS 20u
#define LCD_ROWS 4u
#define LCD_SLOTS 8u

/* write len bytes to the 7-bit I2C address; returns 0 when the device answered (ACK) */
typedef int (*lcd_write_fn)(void *ctx, uint8_t addr7, const uint8_t *data, uint16_t len);
typedef void (*lcd_delay_fn)(uint32_t ms);

typedef enum { LCD_ROM_A00 = 0, LCD_ROM_A02 = 1 } lcd_rom_t;

/* letters the display does not have (text codes 0x80-0x9B) */
enum {
    LCD_GR_GAMMA = 0x80,   /* Γ, also Cyrillic Г */
    LCD_GR_DELTA,          /* Δ */
    LCD_GR_THETA,          /* Θ */
    LCD_GR_LAMDA,          /* Λ */
    LCD_GR_PI,             /* Π, also Cyrillic П */
    LCD_GR_SIGMA,          /* Σ */
    LCD_GR_PHI,            /* Φ, also Cyrillic Ф */
    LCD_GR_OMEGA,          /* Ω */
    LCD_GR_XI,             /* Ξ */
    LCD_GR_PSI,            /* Ψ */
    LCD_CY_BE,             /* Б */
    LCD_CY_DE,             /* Д */
    LCD_CY_ZHE,            /* Ж */
    LCD_CY_ZE,             /* З */
    LCD_CY_I,              /* И */
    LCD_CY_SHORT_I,        /* Й */
    LCD_CY_EL,             /* Л */
    LCD_CY_U,              /* У */
    LCD_CY_TSE,            /* Ц */
    LCD_CY_CHE,            /* Ч */
    LCD_CY_SHA,            /* Ш */
    LCD_CY_SHCHA,          /* Щ */
    LCD_CY_HARD,           /* Ъ */
    LCD_CY_YERU,           /* Ы */
    LCD_CY_SOFT,           /* Ь */
    LCD_CY_E,              /* Э */
    LCD_CY_YU,             /* Ю */
    LCD_CY_YA,             /* Я */
    LCD_EXTRA_END
};

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
    uint8_t text_changed;     /* text[] changed: work out want[] and the own letters again */
    uint8_t slot_glyph[LCD_SLOTS];   /* which own letter is in each CGRAM place, 0xFF none */
    uint8_t slot_reload;      /* bit per place: its dots still have to be sent */
    uint32_t errors;
    uint8_t text[LCD_ROWS][LCD_COLS];    /* text codes as given */
    uint8_t shown[LCD_ROWS][LCD_COLS];   /* what the display shows (display codes) */
    uint8_t want[LCD_ROWS][LCD_COLS];    /* what it should show */
} lcd_t;

/* finds the display (0x27, then 0x3F) and initialises it.
   Returns 0 when a display answered. Safe to call again (e.g. after it was unplugged). */
int lcd_init(lcd_t *l, lcd_write_fn write, void *ctx, lcd_delay_fn delay, lcd_rom_t rom);

/* row 0..3: text codes, cut at 20 characters and padded with spaces */
void lcd_set_row(lcd_t *l, uint8_t row, const char *text);

/* sends at most max_chars changed characters; returns how many are still different */
uint16_t lcd_flush(lcd_t *l, uint16_t max_chars);

#endif /* LCD_H */
