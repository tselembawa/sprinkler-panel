/* 20x4 HD44780 LCD over a PCF8574 I2C backpack - see lcd.h */
#include "lcd.h"

#include <string.h>

#define PIN_RS 0x01u
#define PIN_EN 0x04u
#define PIN_BL 0x08u

static const uint8_t row_addr[LCD_ROWS] = {0x00u, 0x40u, 0x14u, 0x54u};

/* Our own characters, 5x8 dots. Used through the codes 0x08-0x0F (the same CGRAM slots
   as 0x00-0x07, which keeps 0 out of C strings). */
static const uint8_t cg_nordic[8][8] = {
    {0x04, 0x0A, 0x04, 0x0E, 0x11, 0x1F, 0x11, 0x00},   /* Å */
    {0x0A, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x11, 0x00},   /* Ä */
    {0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00},   /* Ö */
    {0x04, 0x0A, 0x04, 0x0E, 0x01, 0x0F, 0x11, 0x0F},   /* å */
    {0x0F, 0x14, 0x14, 0x1F, 0x14, 0x14, 0x17, 0x00},   /* Æ */
    {0x0E, 0x13, 0x15, 0x15, 0x15, 0x19, 0x0E, 0x00},   /* Ø */
    {0x00, 0x00, 0x1A, 0x05, 0x0F, 0x14, 0x0F, 0x00},   /* æ */
    {0x00, 0x00, 0x0E, 0x13, 0x15, 0x19, 0x0E, 0x00},   /* ø */
};
static const uint8_t nordic_latin1[8] = {0xC5, 0xC4, 0xD6, 0xE5, 0xC6, 0xD8, 0xE6, 0xF8};

static const uint8_t cg_greek[8][8] = {
    {0x1F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00},   /* Γ */
    {0x04, 0x0A, 0x0A, 0x11, 0x11, 0x11, 0x1F, 0x00},   /* Δ */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x0E, 0x00},   /* Θ */
    {0x04, 0x0A, 0x0A, 0x11, 0x11, 0x11, 0x11, 0x00},   /* Λ */
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00},   /* Π */
    {0x1F, 0x10, 0x08, 0x04, 0x08, 0x10, 0x1F, 0x00},   /* Σ */
    {0x04, 0x0E, 0x15, 0x15, 0x15, 0x0E, 0x04, 0x00},   /* Φ */
    {0x0E, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x1B, 0x00},   /* Ω */
};

uint8_t lcd_map(uint8_t c, uint8_t rom, uint8_t charset)
{
    if (c >= 0x20u && c < 0x7Fu) {
        return c;
    }
    if (c >= LCD_GR_GAMMA && c <= LCD_GR_OMEGA) {
        return charset == LCD_SET_GREEK ? (uint8_t)(0x08u + (c - LCD_GR_GAMMA)) : (uint8_t)'?';
    }
    if (rom == LCD_ROM_A02) {
        return c >= 0xA0u ? c : (uint8_t)'?';     /* A02 follows Latin-1 for these letters */
    }
    if (charset == LCD_SET_NORDIC) {
        for (uint8_t k = 0; k < 8u; k++) {
            if (nordic_latin1[k] == c) {
                return (uint8_t)(0x08u + k);
            }
        }
    }
    switch (c) {   /* ROM A00 */
    case 0xC5: case 0xC4: case 0xC6: return (uint8_t)'A';   /* Å Ä Æ without own characters */
    case 0xD6: case 0xD8:            return (uint8_t)'O';   /* Ö Ø */
    case 0xE5: case 0xE6:            return (uint8_t)'a';   /* å æ */
    case 0xF8:                       return 0xEFu;          /* ø -> ö */
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
static int load_charset(lcd_t *l, uint8_t charset)
{
    const uint8_t (*glyphs)[8] = charset == LCD_SET_GREEK ? cg_greek : cg_nordic;
    if (command(l, 0x40u, 1)) {
        return -1;
    }
    for (unsigned c = 0; c < 8u; c++) {
        for (unsigned r = 0; r < 8u; r++) {
            put_byte(l, glyphs[c][r], 1);
        }
        if (send(l)) {
            return -1;
        }
    }
    l->charset = charset;
    l->cursor = 0xFFu;   /* the CGRAM writes moved the address */
    return 0;
}

int lcd_set_charset(lcd_t *l, lcd_charset_t charset)
{
    if (!l->ok || l->charset == (uint8_t)charset) {
        l->charset = (uint8_t)charset;
        return l->ok ? 0 : -1;
    }
    if (load_charset(l, (uint8_t)charset)) {
        return -1;
    }
    memset(l->shown, 0, sizeof(l->shown));   /* draw everything again with the new letters */
    return 0;
}

int lcd_init(lcd_t *l, lcd_write_fn write, void *ctx, lcd_delay_fn delay, lcd_rom_t rom,
             lcd_charset_t charset)
{
    memset(l, 0, sizeof(*l));
    l->write = write;
    l->ctx = ctx;
    l->delay = delay;
    l->rom = (uint8_t)rom;
    l->charset = (uint8_t)charset;
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
    if (load_charset(l, (uint8_t)charset)) {
        return -1;
    }
    if (command(l, 0x0Cu, 1)) {       /* display on, no cursor, no blink */
        return -1;
    }
    memset(l->shown, ' ', sizeof(l->shown));   /* after "clear" */
    return 0;
}

void lcd_set_row(lcd_t *l, uint8_t row, const char *text)
{
    if (row >= LCD_ROWS) {
        return;
    }
    uint8_t c = 0;
    for (; c < LCD_COLS && text[c]; c++) {
        l->want[row][c] = lcd_map((uint8_t)text[c], l->rom, l->charset);
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
