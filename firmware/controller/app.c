/*
 * Controller application, step C1 - see app.h.
 *
 * Hardware (NUCLEO-G0B1RE):
 *   USART1  PC4 (D1, TX) / PC5 (D0, RX)  -> XY-485 -> RS485 line 1, 38400 8N1
 *   USART2  PA2 / PA3                    -> ST-LINK virtual COM port -> PC terminal, 115200
 *   PA5     green LED LD4                -> blinks on every poll
 *   PC13    blue button B1               -> switches OUT1 of the first module ON / OFF
 */
#include "app.h"
#include "main.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ctrl_modules.h"
#include "modbus_map.h"
#include "modbus_rtu.h"

extern UART_HandleTypeDef huart1;   /* RS485 line 1 */
extern UART_HandleTypeDef huart2;   /* PC terminal */

#define POLL_PERIOD_MS     500u     /* one cycle over all modules of the line */
#define ANSWER_TIMEOUT_MS  100u     /* 50 ms in the map; 100 ms while the PC plays the modules */
#define FRAME_GAP_MS       2u       /* >= 3.5 characters between frames (1.75 ms at 38400) */
#define SUMMARY_PERIOD_MS  10000u
#define BUTTON_DEBOUNCE_MS 30u

/* ---- what is on line 1 (later this comes from the configuration sent by the Pi) -------- */
typedef struct {
    const char *name;
    uint8_t normally_closed;     /* 1 = NC contact (closed = Normal), 0 = NO (open = Normal) */
} point_t;

static const point_t points[4] = {
    {"Serviceventil", 1},
    {"Pressostat", 0},
    {NULL, 0},
    {NULL, 0},
};

static mb_master_t line1;
static ctrl_module_t modules[1];
#define MODULE_COUNT (sizeof(modules) / sizeof(modules[0]))

static uint32_t next_poll, next_summary, last_frame_end;
static GPIO_PinState button_idle, button_last;
static uint32_t button_changed_at;
static uint8_t button_pressed;

/* ---- printing to the PC ---------------------------------------------------------------- */
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
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, 100);
}

static void stamp(void)
{
    uint32_t t = HAL_GetTick();
    out("[%5lu.%03lu] ", (unsigned long)(t / 1000u), (unsigned long)(t % 1000u));
}

/* ---- RS485 transport for the Modbus master ---------------------------------------------- */
static int rs485_xfer(void *ctx, const uint8_t *tx, uint16_t tx_len,
                      uint8_t *rx, uint16_t rx_max, uint32_t timeout_ms)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;

    while (HAL_GetTick() - last_frame_end < FRAME_GAP_MS) {
        /* keep the silent gap Modbus needs between frames */
    }
    /* forget anything left in the receiver (noise, a late answer) */
    __HAL_UART_SEND_REQ(h, UART_RXDATA_FLUSH_REQUEST);
    __HAL_UART_CLEAR_FLAG(h, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF |
                             UART_CLEAR_PEF | UART_CLEAR_IDLEF);

    /* the XY-485 switches to "send" by itself while TX is active */
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
        return (int)got;   /* 0 = no answer; a cut answer is caught by the CRC check */
    }
    return -1;
}

/* ---- events from the module protocol ----------------------------------------------------- */
static const char *meaning(uint8_t in, uint8_t v)
{
    if (points[in].name == NULL) {
        return "";
    }
    switch (v) {
    case MB_IN_CLOSED: return points[in].normally_closed ? "  = Normal" : "  = AKTIV";
    case MB_IN_OPEN:   return points[in].normally_closed ? "  = AKTIV" : "  = Normal";
    case MB_IN_LINE_OPEN:  return "  = FEL: Avbrott";
    case MB_IN_LINE_SHORT: return "  = FEL: Kortslutning";
    default: return "";
    }
}

static void on_event(void *ctx, const ctrl_module_t *m, ctrl_event_t ev,
                     uint8_t index, uint16_t old_v, uint16_t new_v)
{
    (void)ctx;
    stamp();
    out("L%u-M%02u ", m->line, m->addr);
    switch (ev) {
    case EV_MODULE_ONLINE:
        out("ONLINE  fw %u.%u  serial %08lX\r\n", m->fw_version >> 8, m->fw_version & 0xFFu,
            (unsigned long)m->serial);
        break;
    case EV_MODULE_LOST:
        out("MODUL SAKNAS (%s)\r\n", mb_status_text((mb_status_t)old_v));
        break;
    case EV_MODULE_RESTARTED:
        out("startade om - skickar konfigurationen igen\r\n");
        break;
    case EV_MODULE_WRONG_TYPE:
        out("FEL MODULTYP (typ 0x%04X, karta v%u)\r\n", old_v, new_v);
        break;
    case EV_MODULE_WRONG_LINE:
        out("FEL LINJE (ska vara linje %u, adressen hor till linje %u)\r\n", old_v, new_v);
        break;
    case EV_INPUT_CHANGED: {
        const char *name = points[index].name ? points[index].name : "-";
        if (old_v == 0xFFFFu) {
            out("IN%u %-13s %s%s\r\n", index + 1u, name, ctrl_input_name((uint8_t)new_v),
                meaning(index, (uint8_t)new_v));
        } else {
            out("IN%u %-13s %s -> %s%s\r\n", index + 1u, name, ctrl_input_name((uint8_t)old_v),
                ctrl_input_name((uint8_t)new_v), meaning(index, (uint8_t)new_v));
        }
        break;
    }
    case EV_INPUT_TRANSIENT:
        out("IN%u kort puls (%u andringar mellan tva fragor)\r\n", index + 1u, new_v);
        break;
    case EV_STATUS_CHANGED:
        out("status 0x%04X -> 0x%04X%s%s\r\n", old_v, new_v,
            (new_v & MB_ST_SUPPLY_LOW) ? "  LAG MATNING" : "",
            (new_v & MB_ST_FAILSAFE) ? "  FAILSAFE" : "");
        break;
    case EV_OUTPUT_CHANGED:
        out("OUT%u overvakning %u -> %u\r\n", index + 1u, old_v, new_v);
        break;
    case EV_COMM_ERROR:
        out("inget/fel svar (%s), forsok %u\r\n", mb_status_text((mb_status_t)old_v), new_v);
        break;
    default:
        out("?\r\n");
        break;
    }
}

static void summary(void)
{
    for (uint8_t i = 0; i < MODULE_COUNT; i++) {
        const ctrl_module_t *m = &modules[i];
        static const char *const st[] = {"START", "RUN", "SAKNAS", "AVVISAD"};
        stamp();
        out("L%u-M%02u %-7s", m->line, m->addr, st[m->state]);
        if (m->state == MOD_RUN) {
            out(" IN:");
            for (uint8_t n = 0; n < 4u; n++) {
                uint8_t v = MB_NIBBLE(m->inputs, n);
                out(" %s", v == MB_IN_NOT_USED ? "-" : ctrl_input_name(v));
            }
            out("  %u.%u V", m->supply_mv / 1000u, (m->supply_mv % 1000u) / 100u);
        }
        out("  | ok %lu, inget svar %lu, CRC %lu\r\n", (unsigned long)line1.stats.ok,
            (unsigned long)line1.stats.timeout, (unsigned long)line1.stats.crc);
    }
}

/* ---- blue button: OUT1 of the first module on / off -------------------------------------- */
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
        ctrl_module_t *m = &modules[0];
        uint8_t cmd = (MB_NIBBLE(m->out_cmd, 0) == MB_CMD_OFF) ? MB_CMD_ON : MB_CMD_OFF;
        m->out_cmd = (uint16_t)((m->out_cmd & ~0x000Fu) | cmd);
        stamp();
        out("Knapp: L%u-M%02u OUT1 -> %s\r\n", m->line, m->addr, cmd == MB_CMD_ON ? "ON" : "OFF");
    }
    button_pressed = pressed;
}

/* ---- main entry points ------------------------------------------------------------------- */
void app_init(void)
{
    static const uint8_t in_mode[4] = {MB_INCFG_SUPERVISED, MB_INCFG_SUPERVISED,
                                       MB_INCFG_NOT_USED, MB_INCFG_NOT_USED};
    static const uint8_t out_mode[4] = {MB_OUTCFG_SUPERVISED, MB_OUTCFG_NOT_USED,
                                        MB_OUTCFG_NOT_USED, MB_OUTCFG_NOT_USED};

    mb_master_init(&line1, rs485_xfer, &huart1, ANSWER_TIMEOUT_MS);
    ctrl_module_init(&modules[0], 2u, 1u, in_mode, out_mode);   /* L1-M02 */

    button_idle = button_last = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    button_changed_at = HAL_GetTick();

    out("\r\n=== Sprinkler controller - steg C1: RS485 linje 1, Modbus 38400 ===\r\n");
    out("Fragar L1-M02 var %u ms. Bla knappen: OUT1 pa/av.\r\n\r\n", POLL_PERIOD_MS);
    next_poll = HAL_GetTick();
    next_summary = HAL_GetTick() + SUMMARY_PERIOD_MS;
}

void app_loop(void)
{
    uint32_t t = HAL_GetTick();
    if ((int32_t)(t - next_poll) >= 0) {
        next_poll = t + POLL_PERIOD_MS;
        for (uint8_t i = 0; i < MODULE_COUNT; i++) {
            ctrl_module_poll(&line1, &modules[i], on_event, NULL);
        }
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    }
    if ((int32_t)(t - next_summary) >= 0) {
        next_summary = t + SUMMARY_PERIOD_MS;
        summary();
    }
    check_button();
}
