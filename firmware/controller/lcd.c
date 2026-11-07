/* 20x4 HD44780 LCD over a PCF8574 I2C backpack - see lcd.h */
#include "lcd.h"

#include <string.h>

#define PIN_RS 0x01u
#define PIN_EN 0x04u
#define PIN_BL 0x08u

static const uint8_t row_addr[LCD_ROWS] = {0x00u, 0x40u, 0x14u, 0x54u};

/* Our own characters, 5x8 dots. Used through the codes 0x08-0x0F (the same CGRAM slots
   as 0x00-0x07, which keeps 0 out of C strings). */
enum { CG_AA = 0, CG_AE, CG_OE, CG_aa };
static const uint8_t cgram[4][8] = {
    {0x04, 0x0A, 0x04, 0x0E, 0x11, 0x1F, 0x11, 0x00},   /* Å */
    {0x0A, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x11, 0x00},   /* Ä */
    {0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00},   /* Ö */
    {0x04, 0x0A, 0x04, 0x0E, 0x01, 0x0F, 0x11, 0x0F},   /* å */
};

uint8_t lcd_map(uint8_t c, uint8_t rom)
{
    if (c >= 0x20u && c < 0x7Fu) {
        return c;
    }
    if (rom == LCD_ROM_A02) {
        return c >= 0xA0u ? c : (uint8_t)'?';     /* A02 follows Latin-1 for these letters */
    }
    switch (c) {   /* ROM A00 */
    case 0xC5: return 0x08u + CG_AA;   /* Å */
    case 0xC4: return 0x08u + CG_AE;   /* Ä */
    case 0xD6: return 0x08u + CG_OE;   /* Ö */
    case 0xE5: return 0x08u + CG_aa;   /* å */
    case 0xE4: return 0xE1u;           /* ä */
    case 0xF6: return 0xEFu;           /* ö */
    case 0xFC: return 0xF5u;           /* ü */
    case 0xDC: return (uint8_t)'U';
    case 0xC9: return (uint8_t)'E';    /* É */
    case 0xE9: return (uint8_t)'e';    /* é */
    case 0xB0: return 0xDFu;           /* ° */
    default:   return (uint8_t)'?';
    }
}

/* ---- low level --------------------------------------------------------------------------- */
static uint8_t buf[64];
static uint16_t buf_len;

static void put_nibble(lcd_t *l, uint8_t nibble_hi, uint8_t rs)
{
    uint8_t b = (uint8_t)((nibble_hi & 0xF0u) | (l->backlight ? PIN_BL : 0u) | (rs ? PIN_RS : 0u));
    buf[buf_len++] = (uint8_t)(b | PIN_EN);   /* EN high ... */
    buf[buf_len++] = b;                       /* ... EN low: the display takes the nibble */
}

static void put_byte(lcd_t *l, uint8_t v, uint8_t rs)
{
    put_nibble(l, v, rs);
    put_nibble(l, (uint8_t)(v << 4), rs);
}

static int send(lcd_t *l)
{
    int r = 0;
    if (buf_len) {
        r = l->write(l->ctx, l->addr7, buf, buf_len);
        buf_len = 0;
    }
    if (r != 0) {
        l->ok = 0;
        l->errors++;
    }
    return r;
}

static int command(lcd_t *l, uint8_t cmd, uint32_t wait_ms)
{
    put_byte(l, cmd, 0);
    int r = send(l);
    if (wait_ms) {
        l->delay(wait_ms);
    }
    return r;
}

/* ---- public ------------------------------------------------------------------------------- */
int lcd_init(lcd_t *l, lcd_write_fn write, void *ctx, lcd_delay_fn delay, lcd_rom_t rom)
{
    memset(l, 0, sizeof(*l));
    l->write = write;
    l->ctx = ctx;
    l->delay = delay;
    l->rom = (uint8_t)rom;
    l->backlight = 1;
    l->cursor = 0xFFu;
    memset(l->want, ' ', sizeof(l->want));

    static const uint8_t candidates[] = {0x27u, 0x3Fu};
    const uint8_t idle = PIN_BL;
    for (unsigned k = 0; k < sizeof(candidates) && !l->addr7; k++) {
        if (write(ctx, candidates[k], &idle, 1) == 0) {
            l->addr7 = candidates[k];
        }
    }
    if (!l->addr7) {
        return -1;
    }
    l->ok = 1;

    /* HD44780 power-up into 4-bit mode (datasheet figure 24) */
    delay(50);
    put_nibble(l, 0x30u, 0); if (send(l)) return -1; delay(5);
    put_nibble(l, 0x30u, 0); if (send(l)) return -1; delay(1);
    put_nibble(l, 0x30u, 0); if (send(l)) return -1; delay(1);
    put_nibble(l, 0x20u, 0); if (send(l)) return -1; delay(1);
    if (command(l, 0x28u, 1) ||       /* 4 bit, "2 lines" (= 4 lines on a 20x4), 5x8 */
        command(l, 0x08u, 1) ||       /* display off */
        command(l, 0x01u, 3) ||       /* clear */
        command(l, 0x06u, 1)) {       /* cursor moves right, no shift */
        return -1;
    }
    /* our characters into CGRAM */
    if (command(l, 0x40u, 1)) {
        return -1;
    }
    for (unsigned c = 0; c < 4u; c++) {
        for (unsigned r = 0; r < 8u; r++) {
            put_byte(l, cgram[c][r], 1);
        }
        if (send(l)) {
            return -1;
        }
    }
    if (command(l, 0x0Cu, 1)) {       /* display on, no cursor, no blink */
        return -1;
    }
    memset(l->shown, ' ', sizeof(l->shown));   /* after "clear" */
    l->cursor = 0xFFu;                           /* the CGRAM writes moved the address */
    return 0;
}

void lcd_set_row(lcd_t *l, uint8_t row, const char *text)
{
    if (row >= LCD_ROWS) {
        return;
    }
    uint8_t c = 0;
    for (; c < LCD_COLS && text[c]; c++) {
        l->want[row][c] = lcd_map((uint8_t)text[c], l->rom);
    }
    for (; c < LCD_COLS; c++) {
        l->want[row][c] = ' ';
    }
}

uint16_t lcd_flush(lcd_t *l, uint16_t max_chars)
{
    const uint16_t cells = LCD_ROWS * LCD_COLS;
    uint16_t sent = 0;
    if (l->ok) {
        for (uint16_t k = 0; k < cells && sent < max_chars; k++) {
            uint16_t pos = (uint16_t)((l->scan + k) % cells);
            uint8_t row = (uint8_t)(pos / LCD_COLS), col = (uint8_t)(pos % LCD_COLS);
            if (l->want[row][col] == l->shown[row][col]) {
                continue;
            }
            uint8_t addr = (uint8_t)(row_addr[row] + col);
            if (l->cursor != addr || buf_len + 8u > sizeof(buf)) {
                if (send(l)) {
                    break;
                }
                put_byte(l, (uint8_t)(0x80u | addr), 0);
            }
            put_byte(l, l->want[row][col], 1);
            l->cursor = (uint8_t)(addr + 1u);
            l->shown[row][col] = l->want[row][col];
            sent++;
            l->scan = (uint16_t)((pos + 1u) % cells);
        }
        send(l);
    }
    uint16_t left = 0;
    for (uint8_t r = 0; r < LCD_ROWS; r++) {
        for (uint8_t c = 0; c < LCD_COLS; c++) {
            left = (uint16_t)(left + (l->want[r][c] != l->shown[r][c]));
        }
    }
    return left;
}
