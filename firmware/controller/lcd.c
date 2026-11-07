/* 20x4 HD44780 LCD over a PCF8574 I2C backpack - see lcd.h */
#include "lcd.h"

#include <string.h>

#define PIN_RS 0x01u
#define PIN_EN 0x04u
#define PIN_BL 0x08u
#define NO_GLYPH 0xFFu

static const uint8_t row_addr[LCD_ROWS] = {0x00u, 0x40u, 0x14u, 0x54u};

/* ---- letters of our own ---------------------------------------------------------------------
   5x8 dots each. Index = text code - 0x80 for the Greek/Cyrillic ones, then the Nordic
   letters the A00 ROM lacks. Each has a look-alike for when the 8 places are taken. */
typedef struct {
    uint8_t dots[8];
    uint8_t fallback;      /* display code used when there is no free place */
} glyph_t;

enum { G_AA = LCD_EXTRA_END - 0x80, G_AE, G_OE, G_aa, G_AEL, G_OSL, G_ael, G_osl, G_COUNT };

static const glyph_t glyphs[G_COUNT] = {
    /* Greek (Γ Π Φ are also the Cyrillic Г П Ф) */
    {{0x1F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00}, 'r'},   /* Γ */
    {{0x04, 0x0A, 0x0A, 0x11, 0x11, 0x11, 0x1F, 0x00}, 'D'},   /* Δ */
    {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x0E, 0x00}, 'O'},   /* Θ */
    {{0x04, 0x0A, 0x0A, 0x11, 0x11, 0x11, 0x11, 0x00}, '^'},   /* Λ */
    {{0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00}, 'n'},   /* Π */
    {{0x1F, 0x10, 0x08, 0x04, 0x08, 0x10, 0x1F, 0x00}, 'E'},   /* Σ */
    {{0x04, 0x0E, 0x15, 0x15, 0x15, 0x0E, 0x04, 0x00}, 'O'},   /* Φ */
    {{0x0E, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x1B, 0x00}, 'O'},   /* Ω */
    {{0x1F, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x1F, 0x00}, 'E'},   /* Ξ */
    {{0x15, 0x15, 0x15, 0x0E, 0x04, 0x04, 0x04, 0x00}, 'Y'},   /* Ψ */
    /* Cyrillic */
    {{0x1F, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x1E, 0x00}, '6'},   /* Б */
    {{0x06, 0x0A, 0x0A, 0x0A, 0x0A, 0x1F, 0x11, 0x00}, 'D'},   /* Д */
    {{0x15, 0x15, 0x0E, 0x04, 0x0E, 0x15, 0x15, 0x00}, 'X'},   /* Ж */
    {{0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E, 0x00}, '3'},   /* З */
    {{0x11, 0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x00}, 'U'},   /* И */
    {{0x0A, 0x04, 0x11, 0x13, 0x15, 0x19, 0x11, 0x00}, 'U'},   /* Й */
    {{0x07, 0x09, 0x09, 0x09, 0x09, 0x09, 0x11, 0x00}, 'n'},   /* Л */
    {{0x11, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E, 0x00}, 'Y'},   /* У */
    {{0x12, 0x12, 0x12, 0x12, 0x12, 0x1F, 0x01, 0x00}, 'U'},   /* Ц */
    {{0x11, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x01, 0x00}, '4'},   /* Ч */
    {{0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x00}, 'W'},   /* Ш */
    {{0x15, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x01, 0x00}, 'W'},   /* Щ */
    {{0x18, 0x08, 0x08, 0x0E, 0x09, 0x09, 0x0E, 0x00}, 'b'},   /* Ъ */
    {{0x11, 0x11, 0x11, 0x1D, 0x13, 0x13, 0x1D, 0x00}, 'b'},   /* Ы */
    {{0x10, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x1E, 0x00}, 'b'},   /* Ь */
    {{0x0E, 0x11, 0x01, 0x07, 0x01, 0x11, 0x0E, 0x00}, '3'},   /* Э */
    {{0x12, 0x15, 0x15, 0x1D, 0x15, 0x15, 0x12, 0x00}, 'O'},   /* Ю */
    {{0x0F, 0x11, 0x11, 0x0F, 0x05, 0x09, 0x11, 0x00}, 'R'},   /* Я */
    /* Nordic letters the A00 ROM does not have */
    {{0x04, 0x0A, 0x04, 0x0E, 0x11, 0x1F, 0x11, 0x00}, 'A'},   /* Å */
    {{0x0A, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x11, 0x00}, 'A'},   /* Ä */
    {{0x0A, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00}, 'O'},   /* Ö */
    {{0x04, 0x0A, 0x04, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, 'a'},   /* å */
    {{0x0F, 0x14, 0x14, 0x1F, 0x14, 0x14, 0x17, 0x00}, 'A'},   /* Æ */
    {{0x0E, 0x13, 0x15, 0x15, 0x15, 0x19, 0x0E, 0x00}, 'O'},   /* Ø */
    {{0x00, 0x00, 0x1A, 0x05, 0x0F, 0x14, 0x0F, 0x00}, 'a'},   /* æ */
    {{0x00, 0x00, 0x0E, 0x13, 0x15, 0x19, 0x0E, 0x00}, 0xEF},  /* ø (fallback ö) */
};

/* text code -> ROM code, or NO_GLYPH with *g = the letter of our own it needs */
static uint8_t rom_code(uint8_t c, uint8_t rom, uint8_t *g)
{
    *g = NO_GLYPH;
    if (c >= 0x20u && c < 0x7Fu) {
        return c;
    }
    if (c >= 0x80u && c < LCD_EXTRA_END) {
        *g = (uint8_t)(c - 0x80u);
        return NO_GLYPH;
    }
    if (rom == LCD_ROM_A02) {
        return c >= 0xA0u ? c : (uint8_t)'?';     /* A02 follows Latin-1 */
    }
    switch (c) {   /* ROM A00 */
    case 0xC5: *g = G_AA;  return NO_GLYPH;   /* Å */
    case 0xC4: *g = G_AE;  return NO_GLYPH;   /* Ä */
    case 0xD6: *g = G_OE;  return NO_GLYPH;   /* Ö */
    case 0xE5: *g = G_aa;  return NO_GLYPH;   /* å */
    case 0xC6: *g = G_AEL; return NO_GLYPH;   /* Æ */
    case 0xD8: *g = G_OSL; return NO_GLYPH;   /* Ø */
    case 0xE6: *g = G_ael; return NO_GLYPH;   /* æ */
    case 0xF8: *g = G_osl; return NO_GLYPH;   /* ø */
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

/* Work out want[] from text[]: give the 8 places to the letters this screen needs. */
static void layout(lcd_t *l)
{
    uint8_t need[LCD_SLOTS];
    uint8_t n_need = 0;
    uint8_t g;
    /* letters needed, in order of first appearance (upper rows first), at most 8 */
    for (uint8_t r = 0; r < LCD_ROWS; r++) {
        for (uint8_t c = 0; c < LCD_COLS; c++) {
            if (rom_code(l->text[r][c], l->rom, &g) != NO_GLYPH) {
                continue;
            }
            uint8_t k;
            for (k = 0; k < n_need && need[k] != g; k++) {
            }
            if (k == n_need && n_need < LCD_SLOTS) {
                need[n_need++] = g;
            }
        }
    }
    /* keep the places that already hold a needed letter; free the others */
    uint8_t slot_of[LCD_SLOTS];
    uint8_t used = 0;
    for (uint8_t k = 0; k < n_need; k++) {
        slot_of[k] = NO_GLYPH;
        for (uint8_t s = 0; s < LCD_SLOTS; s++) {
            if (l->slot_glyph[s] == need[k]) {
                slot_of[k] = s;
                used |= (uint8_t)(1u << s);
            }
        }
    }
    for (uint8_t k = 0; k < n_need; k++) {
        if (slot_of[k] != NO_GLYPH) {
            continue;
        }
        for (uint8_t s = 0; s < LCD_SLOTS; s++) {
            if (!(used & (1u << s))) {
                slot_of[k] = s;
                used |= (uint8_t)(1u << s);
                l->slot_glyph[s] = need[k];
                l->slot_reload |= (uint8_t)(1u << s);
                break;
            }
        }
    }
    /* the display codes */
    for (uint8_t r = 0; r < LCD_ROWS; r++) {
        for (uint8_t c = 0; c < LCD_COLS; c++) {
            uint8_t code = rom_code(l->text[r][c], l->rom, &g);
            if (code == NO_GLYPH) {
                code = glyphs[g].fallback;
                for (uint8_t k = 0; k < n_need; k++) {
                    if (need[k] == g) {
                        code = (uint8_t)(0x08u + slot_of[k]);   /* 0x08-0x0F = places 0-7 */
                    }
                }
            }
            l->want[r][c] = code;
        }
    }
    l->text_changed = 0;
}

/* ---- low level --------------------------------------------------------------------------- */
static uint8_t buf[80];
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

/* send the dots of the places that changed */
static int load_slots(lcd_t *l)
{
    for (uint8_t s = 0; s < LCD_SLOTS; s++) {
        if (!(l->slot_reload & (1u << s))) {
            continue;
        }
        put_byte(l, (uint8_t)(0x40u | (s << 3)), 0);   /* CGRAM address of place s */
        for (uint8_t r = 0; r < 8u; r++) {
            put_byte(l, glyphs[l->slot_glyph[s]].dots[r], 1);
        }
        if (send(l)) {
            return -1;
        }
        l->slot_reload &= (uint8_t)~(1u << s);
        l->cursor = 0xFFu;   /* the CGRAM writes moved the address */
    }
    return 0;
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
    memset(l->slot_glyph, NO_GLYPH, sizeof(l->slot_glyph));
    memset(l->text, ' ', sizeof(l->text));
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
        command(l, 0x06u, 1) ||       /* cursor moves right, no shift */
        command(l, 0x0Cu, 1)) {       /* display on, no cursor, no blink */
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
    uint8_t line[LCD_COLS];
    uint8_t c = 0;
    for (; c < LCD_COLS && text[c]; c++) {
        line[c] = (uint8_t)text[c];
    }
    for (; c < LCD_COLS; c++) {
        line[c] = ' ';
    }
    if (memcmp(l->text[row], line, LCD_COLS) != 0) {
        memcpy(l->text[row], line, LCD_COLS);
        l->text_changed = 1;
    }
}

uint16_t lcd_flush(lcd_t *l, uint16_t max_chars)
{
    const uint16_t cells = LCD_ROWS * LCD_COLS;
    uint16_t sent = 0;
    if (l->text_changed) {
        layout(l);
    }
    if (l->ok && l->slot_reload) {
        load_slots(l);
    }
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
