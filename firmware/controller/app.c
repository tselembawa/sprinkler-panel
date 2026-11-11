/*
 * Controller application - see app.h.
 *
 * The modules on RS485 line 1 feed the alarm engine; the engine decides alarms, relays,
 * LEDs, buzzer and sounders. The Raspberry Pi (HMI) talks to the controller over the
 * ST-LINK USB port with the framed protocol of docs/pi-link.md: it reads status, points
 * and events, sends the configuration and the panel commands. What used to be printed
 * to PuTTY now goes to the Pi as LOG frames.
 *
 * Hardware (NUCLEO-G0B1RE):
 *   USART1  PC4 (D1, TX) / PC5 (D0, RX)  -> XY-485 -> RS485 line 1, 38400 8N1 (blocking)
 *   USART2  PA2 / PA3                    -> ST-LINK virtual COM port -> Raspberry Pi / PC,
 *                                           115200, RX by interrupt (CubeMX: NVIC
 *                                           "USART2/LPUART2 global interrupt" enabled)
 *   PA5     green LED LD4                -> blinks on every poll cycle
 *   PC13    blue button B1               -> Kvittera (like the button on the panel)
 *   I2C1    PB8 (D15, SCL) / PB9 (D14, SDA) -> 20x4 LCD with PCF8574 backpack, 100 kHz
 *
 * Panel outputs (set up here in app.c, nothing to do in CubeMX - leave these pins unused there):
 *   relays, 5 V relay board with optocouplers, active low, open drain (pins are 5 V tolerant):
 *     PB1  (A3) -> IN1 = K1 A-larm        PB5  (D4) -> IN2 = K2 B-larm
 *     PB4  (D5) -> IN3 = K3 Fel (fail-safe: energized while all is OK)
 *     PB14 (D6) -> IN4 = K4 C-larm
 *   LEDs, push-pull, active high, each with a series resistor (~330 ohm) to GND:
 *     PC7 (D9) red A    PB0 (D10) yellow B/C    PA7 (D11) yellow fault
 *     PA6 (D12) green power    PA0 (A0) green Drift    PA1 (A1) yellow service
 *   PA8 (D7) buzzer, push-pull, active high, through an NPN transistor
 *   IWDG watchdog 2 s: if the program hangs, the controller restarts and K3 drops
 */
#include "app.h"
#include "main.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "ctrl_modules.h"
#include "lcd.h"
#include "lcd_view.h"
#include "modbus_map.h"
#include "modbus_rtu.h"
#include "panel_io.h"
#include "pi_link.h"

extern UART_HandleTypeDef huart1;   /* RS485 line 1 */
extern UART_HandleTypeDef huart2;   /* Raspberry Pi (ST-LINK virtual COM port) */
extern I2C_HandleTypeDef hi2c1;     /* LCD */

#define POLL_PERIOD_MS     500u     /* one cycle over all modules of the line */
#define TICK_PERIOD_MS     100u     /* alarm engine timers (delays, service mode) */
#define ANSWER_TIMEOUT_MS  100u     /* 50 ms in the map; 100 ms while the PC plays the modules */
#define FRAME_GAP_MS       2u       /* >= 3.5 characters between frames (1.75 ms at 38400) */
#define BUTTON_DEBOUNCE_MS 30u
#define MAX_MODULES        16u      /* prototype: line 1 only */
#define SOUNDER_LINE       1u       /* first output rule: the sounders are OUT1 of L1-M02 */
#define SOUNDER_ADDR       2u
#define WIRED_LINES        1u       /* only line 1 has an RS485 port on the prototype */
#define LCD_ROM            LCD_ROM_A00   /* the display's character ROM (ours: A00, tested 2026-11-08) */
#define LCD_SPLASH_MS      2000u    /* start-up screen */
#define LCD_RETRY_MS       5000u    /* no display: look for it again */
#define LCD_RENDER_MS      250u
#define LCD_CHARS_PER_PASS 6u       /* a few characters per loop pass, ~0.5 ms each */
#define WATCHDOG_MS        2000u    /* IWDG: restart if the main loop stops for 2 s */

/* ---- configuration until the Raspberry Pi sends its own --------------------------------- */
static const alm_point_cfg_t default_config[] = {
    /* id line mod ch  category  device type        NC  delay en sect  LCD line 1     LCD line 2 */
    {1,  1,   2,  1,  CAT_B,    DT_SERVICEVENTIL,  1,  0,    1, 1,   "SERVICEVENTIL", "SEKTION 1"},
    {2,  1,   2,  2,  CAT_A,    DT_PRESSOSTAT,     0,  0,    1, 1,   "PRESSOSTAT",    "SEKTION 1"},
};
#define DEFAULT_COUNT ((uint16_t)(sizeof(default_config) / sizeof(default_config[0])))

static alm_t engine;                /* ~40 kB, static */
static mb_master_t line1;
static ctrl_module_t modules[MAX_MODULES];
static uint8_t module_count;
static uint32_t modules_for_version = 0xFFFFFFFFu;
static pi_link_t pi;
static lcd_t lcd;
static lcd_view_t lcd_screen;
static uint32_t lcd_next_render, lcd_next_retry;
static uint8_t lcd_lang;

static uint32_t next_cycle, next_tick, last_frame_end;
static uint8_t poll_index;
static GPIO_PinState button_idle, button_last;
static uint32_t button_changed_at;
static uint8_t button_pressed;
static alm_status_t shown;          /* last reported outputs */
static uint8_t shown_valid;

/* ---- bytes from the Pi: interrupt -> ring buffer -> pi_link in the main loop ----------- */
#define RX_RING 512u                /* power of two */
static volatile uint16_t rx_head, rx_tail;
static uint8_t rx_ring[RX_RING];
static uint8_t rx_byte;
static volatile uint32_t rx_dropped;

static void rx_start(void)
{
    HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart2) {
        return;
    }
    uint16_t next = (uint16_t)((rx_head + 1u) & (RX_RING - 1u));
    if (next != rx_tail) {
        rx_ring[rx_head] = rx_byte;
        rx_head = next;
    } else {
        rx_dropped++;
    }
    rx_start();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2) {   /* overrun, noise...: the frame CRC catches it, keep listening */
        rx_start();
    }
}

static void service_pi(void)
{
    const uint32_t now = HAL_GetTick();
    while (rx_tail != rx_head) {
        uint8_t b = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1u) & (RX_RING - 1u));
        pi_link_rx_byte(&pi, b, now);
    }
    if (huart2.RxState == HAL_UART_STATE_READY) {
        rx_start();             /* reception stopped after an error: start it again */
    }
}

static void pi_write(void *ctx, const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit((UART_HandleTypeDef *)ctx, (uint8_t *)data, len, 200u);
}

/* ---- text for the Pi's log (LOG frames, one per line) ---------------------------------------- */
static char log_line[200];
static size_t log_len;

static void out(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || log_len >= sizeof(log_line) - 1u) {
            log_line[log_len] = '\0';
            if (log_len) {
                pi_link_log(&pi, log_line);
            }
            log_len = 0;
            if (c == '\n') {
                continue;
            }
        }
        log_line[log_len++] = c;
    }
}

/* ---- RS485 transport for the Modbus master ---------------------------------------------- */
static int rs485_xfer(void *ctx, const uint8_t *tx, uint16_t tx_len,
                      uint8_t *rx, uint16_t rx_max, uint32_t timeout_ms)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;

    while (HAL_GetTick() - last_frame_end < FRAME_GAP_MS) {
        /* keep the silent gap Modbus needs between frames */
    }
    __HAL_UART_SEND_REQ(h, UART_RXDATA_FLUSH_REQUEST);
    __HAL_UART_CLEAR_FLAG(h, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF |
                             UART_CLEAR_PEF | UART_CLEAR_IDLEF);

    HAL_StatusTypeDef st = HAL_UART_Transmit(h, (uint8_t *)tx, tx_len, 50u);
    last_frame_end = HAL_GetTick();
    if (st != HAL_OK) {
        return -1;
    }
    if (rx_max == 0u) {
        return 0;
    }
    uint16_t got = 0;
    st = HAL_UARTEx_ReceiveToIdle(h, rx, rx_max, &got, timeout_ms);
    last_frame_end = HAL_GetTick();
    if (st == HAL_OK || st == HAL_TIMEOUT) {
        return (int)got;
    }
    return -1;
}

/* ---- events from the modules -> alarm engine ----------------------------------------------- */
static void on_module_event(void *ctx, const ctrl_module_t *m, ctrl_event_t ev,
                            uint8_t index, uint16_t old_v, uint16_t new_v)
{
    (void)ctx;
    const uint32_t now = HAL_GetTick();
    switch (ev) {
    case EV_INPUT_CHANGED:
        alm_set_input(&engine, m->line, m->addr, (uint8_t)(index + 1u), (uint8_t)new_v, now);
        return;   /* what it means is reported by the engine events */
    case EV_MODULE_ONLINE:
        alm_set_module_lost(&engine, m->line, m->addr, 0, now);
        out("Modul L%u-M%02u ONLINE  fw %u.%u\n", m->line, m->addr,
            m->fw_version >> 8, m->fw_version & 0xFFu);
        return;
    case EV_MODULE_LOST:
    case EV_MODULE_WRONG_TYPE:
    case EV_MODULE_WRONG_LINE:
        alm_set_module_lost(&engine, m->line, m->addr, 1, now);
        out("Modul L%u-M%02u %s\n", m->line, m->addr,
            ev == EV_MODULE_LOST ? "SAKNAS" : ev == EV_MODULE_WRONG_TYPE ? "FEL MODULTYP" : "FEL LINJE");
        return;
    case EV_MODULE_RESTARTED:
        out("Modul L%u-M%02u startade om, konfigureras igen\n", m->line, m->addr);
        return;
    case EV_STATUS_CHANGED:
        out("Modul L%u-M%02u status 0x%04X%s\n", m->line, m->addr, new_v,
            (new_v & MB_ST_SUPPLY_LOW) ? "  LAG MATNING" : "");
        (void)old_v;
        return;
    case EV_INPUT_TRANSIENT:
        out("Modul L%u-M%02u IN%u kort puls\n", m->line, m->addr, index + 1u);
        return;
    default:
        return;
    }
}

/* ---- events from the alarm engine: the Pi reads them with GET_EVENTS; short log line ---- */
static void on_alarm_event(void *ctx, const alm_event_t *e)
{
    (void)ctx;
    if (e->kind == SRC_POINT) {
        out("#%lu %s P%u\n", (unsigned long)e->seq, alm_event_name(e->type), e->source_id);
    } else if (e->kind == SRC_MODULE) {
        out("#%lu %s L%u-M%02u\n", (unsigned long)e->seq, alm_event_name(e->type),
            e->source_id / 1000u, e->source_id % 1000u);
    } else {
        out("#%lu %s\n", (unsigned long)e->seq, alm_event_name(e->type));
    }
}

/* ---- outputs: report when something changed ------------------------------------------------ */
static void show_outputs(void)
{
    alm_status_t s;
    alm_status(&engine, HAL_GetTick(), &s);
    if (shown_valid && s.relays == shown.relays && s.buzzer == shown.buzzer &&
        s.sounders == shown.sounders) {
        return;
    }
    shown = s;
    shown_valid = 1;
    static const char *const snd[] = {"tysta", "LJUDER", "tystade"};
    out("Utgangar: K1 A=%u K2 B=%u K3 FelOK=%u K4 C=%u | summer=%u | larmdon=%s\n",
        (s.relays & ALM_K1_A) ? 1u : 0u, (s.relays & ALM_K2_B) ? 1u : 0u,
        (s.relays & ALM_K3_FAULT_OK) ? 1u : 0u, (s.relays & ALM_K4_C) ? 1u : 0u,
        s.buzzer, snd[s.sounders]);
}

/* ---- the modules follow the configuration --------------------------------------------------- */
static void build_modules(void)
{
    const uint32_t now = HAL_GetTick();
    uint8_t in_used[MAX_MODULES][4];
    memset(in_used, 0, sizeof(in_used));
    module_count = 0;
    /* a module that is no longer in the configuration is no longer a fault */
    for (uint16_t addr = 1; addr <= ALM_MAX_ADDR; addr++) {
        if (!((engine.lost[addr >> 3] >> (addr & 7u)) & 1u)) {
            continue;
        }
        uint8_t used = 0;
        for (uint16_t i = 0; i < engine.n && !used; i++) {
            used = engine.cfg[i].module == addr;
        }
        if (!used) {
            alm_set_module_lost(&engine, engine.lost_line[addr], (uint8_t)addr, 0, now);
        }
    }
    for (uint16_t i = 0; i < engine.n; i++) {
        const alm_point_cfg_t *c = &engine.cfg[i];
        if (c->line > WIRED_LINES) {
            /* no RS485 port for this line on the prototype: say so instead of showing normal */
            alm_set_module_lost(&engine, c->line, c->module, 1, now);
            continue;
        }
        uint8_t k;
        for (k = 0; k < module_count; k++) {
            if (modules[k].addr == c->module) {
                break;
            }
        }
        if (k == module_count) {
            if (module_count >= MAX_MODULES) {
                out("FEL: fler an %u moduler - resten pollas inte\n", MAX_MODULES);
                alm_set_module_lost(&engine, c->line, c->module, 1, now);
                continue;
            }
            modules[k].addr = c->module;
            modules[k].line = c->line;
            module_count++;
        }
        if (c->channel >= 1u && c->channel <= 4u) {
            in_used[k][c->channel - 1u] = 1;
        }
    }
    for (uint8_t k = 0; k < module_count; k++) {
        uint8_t in_mode[4], out_mode[4];
        for (uint8_t ch = 0; ch < 4u; ch++) {
            in_mode[ch] = in_used[k][ch] ? MB_INCFG_SUPERVISED : MB_INCFG_NOT_USED;
            out_mode[ch] = MB_OUTCFG_NOT_USED;
        }
        if (modules[k].line == SOUNDER_LINE && modules[k].addr == SOUNDER_ADDR) {
            out_mode[0] = MB_OUTCFG_SUPERVISED;
        }
        uint8_t addr = modules[k].addr, line = modules[k].line;
        ctrl_module_init(&modules[k], addr, line, in_mode, out_mode);
    }
    pi.module_count = module_count;
    poll_index = 0;
    modules_for_version = engine.config_version;
    out("Konfiguration v%lu: %u punkter, %u moduler\n", (unsigned long)engine.config_version,
        engine.n, module_count);
}

/* first output rule: the sounders (OUT1 of L1-M02) follow the engine */
static void drive_module_outputs(void)
{
    alm_status_t s;
    alm_status(&engine, HAL_GetTick(), &s);
    uint8_t cmd = (s.sounders == SOUNDERS_SOUNDING) ? MB_CMD_ON : MB_CMD_OFF;
    for (uint8_t k = 0; k < module_count; k++) {
        if (modules[k].line == SOUNDER_LINE && modules[k].addr == SOUNDER_ADDR) {
            modules[k].out_cmd = (uint16_t)((modules[k].out_cmd & ~0x000Fu) | cmd);
        }
    }
}

/* ---- blue button = Kvittera ------------------------------------------------------------------ */
static void check_button(void)
{
    GPIO_PinState now = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    uint32_t t = HAL_GetTick();
    if (now != button_last) {
        button_last = now;
        button_changed_at = t;
        return;
    }
    if (t - button_changed_at < BUTTON_DEBOUNCE_MS) {
        return;
    }
    uint8_t pressed = (now != button_idle);
    if (pressed && !button_pressed) {
        out("Bla knappen: Kvittera, %u kvitterade\n", alm_ack(&engine, 1, t));
    }
    button_pressed = pressed;
}

/* ---- LCD ------------------------------------------------------------------------------------- */
static int lcd_i2c_write(void *ctx, uint8_t addr7, const uint8_t *data, uint16_t len)
{
    return HAL_I2C_Master_Transmit((I2C_HandleTypeDef *)ctx, (uint16_t)(addr7 << 1), (uint8_t *)data, len, 10u)
                   == HAL_OK ? 0 : -1;
}

static void lcd_start(uint32_t now)
{
    lcd_lang = pi.language;
    if (lcd_init(&lcd, lcd_i2c_write, &hi2c1, HAL_Delay, LCD_ROM) != 0) {
        lcd_next_retry = now + LCD_RETRY_MS;
        return;
    }
    out("LCD hittad pa adress 0x%02X\n", lcd.addr7);
    /* test screen: which character set? Row 2 uses our own letters, row 3 the ROM A02 codes */
    char row[24];
    snprintf(row, sizeof(row), "SPRINKLER  FW %u.%u", PI_FW_MAJOR, PI_FW_MINOR);
    lcd_set_row(&lcd, 0, row);
    snprintf(row, sizeof(row), "LCD OK  ADR 0x%02X", lcd.addr7);
    lcd_set_row(&lcd, 1, row);
    lcd_next_render = now + LCD_SPLASH_MS;
}

static void lcd_service(void)
{
    uint32_t t = HAL_GetTick();
    if (!lcd.ok) {
        if ((int32_t)(t - lcd_next_retry) >= 0) {
            lcd_start(t);
        }
        return;
    }
    if (pi.language != lcd_lang) {   /* language changed on the Pi: show it at once */
        lcd_lang = pi.language;
        lcd_next_render = t;
    }
    if ((int32_t)(t - lcd_next_render) >= 0) {
        lcd_next_render = t + LCD_RENDER_MS;
        char rows[LCD_ROWS][LCD_COLS + 1];
        lcd_view_render(&lcd_screen, &engine, lcd_lang, t, rows);
        for (uint8_t r = 0; r < LCD_ROWS; r++) {
            lcd_set_row(&lcd, r, rows[r]);
        }
    }
    lcd_flush(&lcd, LCD_CHARS_PER_PASS);
    if (!lcd.ok) {
        out("LCD svarar inte\n");
        lcd_next_retry = t + LCD_RETRY_MS;
    }
}

/* ---- panel outputs: relays, LEDs, buzzer ----------------------------------------------------- */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t active_low;      /* 1: the output is "on" when the pin is low */
    uint8_t open_drain;      /* 1: low or released (never drives 3.3 V) */
} out_pin_t;

/* K1 is NOT on PB3 (D3): on the Nucleo PB3 pulls the relay on for ~0.5 s after every reset,
 * before and regardless of our code (tested on hardware). K1 goes to SOS/the fire brigade. */
static const out_pin_t relay_pins[4] = {           /* bit 0..3 = K1..K4 (ALM_K1_A ...) */
    {GPIOB, GPIO_PIN_1, 1, 1}, {GPIOB, GPIO_PIN_5, 1, 1},
    {GPIOB, GPIO_PIN_4, 1, 1}, {GPIOB, GPIO_PIN_14, 1, 1},
};
static const out_pin_t led_pins[PLED_COUNT] = {     /* order of panel_io.h PLED_* */
    {GPIOC, GPIO_PIN_7, 0, 0}, {GPIOB, GPIO_PIN_0, 0, 0}, {GPIOA, GPIO_PIN_7, 0, 0},
    {GPIOA, GPIO_PIN_6, 0, 0}, {GPIOA, GPIO_PIN_0, 0, 0}, {GPIOA, GPIO_PIN_1, 0, 0},
};
static const out_pin_t buzzer_pin = {GPIOA, GPIO_PIN_8, 0, 0};
static panel_out_t panel_now;

static void pin_set(const out_pin_t *p, uint8_t on)
{
    HAL_GPIO_WritePin(p->port, p->pin, (on ^ p->active_low) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void pin_init(const out_pin_t *p)
{
    GPIO_InitTypeDef g = {0};
    pin_set(p, 0);                       /* "off" level first, so nothing clicks at start-up */
    g.Pin = p->pin;
    g.Mode = p->open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(p->port, &g);
}

static uint8_t panel_ready;

static void panel_init(void)
{
    if (panel_ready) {
        return;
    }
    panel_ready = 1;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    for (unsigned k = 0; k < 4u; k++) {
        pin_init(&relay_pins[k]);
    }
    for (unsigned k = 0; k < PLED_COUNT; k++) {
        pin_init(&led_pins[k]);
    }
    pin_init(&buzzer_pin);
}

/* called every loop pass, works every 20 ms: enough for the buzzer rhythm and the flashing LED */
static void panel_update(void)
{
    static uint32_t next;
    if ((int32_t)(HAL_GetTick() - next) < 0) {
        return;
    }
    next = HAL_GetTick() + 20u;
    panel_out_t o;
    panel_outputs(&engine, HAL_GetTick(), &o);
    for (unsigned k = 0; k < 4u; k++) {
        pin_set(&relay_pins[k], (uint8_t)((o.relays >> k) & 1u));
    }
    for (unsigned k = 0; k < PLED_COUNT; k++) {
        pin_set(&led_pins[k], o.led[k]);
    }
    pin_set(&buzzer_pin, o.buzzer);
    if (o.pattern != panel_now.pattern) {
        static const char *const names[] = {"av", "BRAND (snabb)", "B-LARM (langsam)", "FEL (kort pip)"};
        out("Summer: %s\n", names[o.pattern]);
    }
    panel_now = o;
}

/* ---- watchdog ------------------------------------------------------------------------------- */
static void watchdog_start(void)
{
#ifdef IWDG
    if (RCC->CSR & RCC_CSR_IWDGRSTF) {
        out("OBS: omstart av watchdog - programmet hade fastnat\n");
    }
    RCC->CSR |= RCC_CSR_RMVF;             /* clear the reset flags for next time */
    __HAL_RCC_DBGMCU_CLK_ENABLE();
    DBG->APBFZ1 |= DBG_APB_FZ1_DBG_IWDG_STOP;   /* stopped at a breakpoint: no restart */
    IWDG->KR = 0xCCCCu;                   /* start (also starts the 32 kHz LSI clock) */
    IWDG->KR = 0x5555u;                   /* unlock PR and RLR */
    IWDG->PR = 3u;                        /* /32 -> 1000 counts per second */
    IWDG->RLR = WATCHDOG_MS - 1u;
    for (uint32_t n = 0; IWDG->SR != 0u && n < 100000u; n++) {
        /* wait until the new values are taken */
    }
    IWDG->KR = 0xAAAAu;
#endif
}

static void watchdog_kick(void)
{
#ifdef IWDG
    IWDG->KR = 0xAAAAu;
#endif
}

/* ---- main entry points ------------------------------------------------------------------- */
/* Right after HAL_Init(), before the clock and the other peripherals: from reset until here the
 * relay pins float; the shorter that is, the less a relay board can do on its own. */
void app_early_init(void)
{
    panel_init();
}

void app_init(void)
{
    const uint32_t now = HAL_GetTick();

    pi_link_init(&pi, &engine, modules, 0, pi_write, &huart2, now);
    panel_init();                        /* already done if main.c calls app_early_init() */
    rx_start();
    out("Sprinkler controller fw %u.%u\n", PI_FW_MAJOR, PI_FW_MINOR);
    watchdog_start();

    alm_init(&engine, now, 0, on_alarm_event, NULL);
    alm_cfg_error_t err[4];
    uint16_t nerr = alm_load(&engine, default_config, DEFAULT_COUNT, 0, now, err, 4);
    if (nerr) {
        out("KONFIGURATIONSFEL: %u fel (punkt %u)\n", nerr, err[0].point_id);
    }
    mb_master_init(&line1, rs485_xfer, &huart1, ANSWER_TIMEOUT_MS);
    build_modules();

    lcd_view_init(&lcd_screen);
    lcd_start(HAL_GetTick());

    button_idle = button_last = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    button_changed_at = now;
    show_outputs();
    next_cycle = next_tick = now;
}

void app_loop(void)
{
    watchdog_kick();
    service_pi();

    if (engine.config_version != modules_for_version) {
        build_modules();        /* the Pi activated a new configuration */
    }

    /* one module per pass, so the Pi is answered between the (blocking) RS485 transfers */
    uint32_t t = HAL_GetTick();
    if (poll_index < module_count) {
        if (poll_index == 0u) {
            drive_module_outputs();
        }
        ctrl_module_poll(&line1, &modules[poll_index], on_module_event, NULL);
        poll_index++;
        service_pi();
    } else if ((int32_t)(t - next_cycle) >= 0) {
        next_cycle = t + POLL_PERIOD_MS;
        poll_index = 0;
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    }

    t = HAL_GetTick();
    if ((int32_t)(t - next_tick) >= 0) {
        next_tick = t + TICK_PERIOD_MS;
        alm_tick(&engine, t);
        pi_link_tick(&pi, t);
        show_outputs();
    }
    panel_update();
    lcd_service();
    check_button();
}
