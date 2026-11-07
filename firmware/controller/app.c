/*
 * Controller application, step C2 - see app.h.
 *
 * The modules on RS485 line 1 feed the alarm engine; the engine decides alarms, relays,
 * LEDs, buzzer and sounders. Everything is printed to the PC, and the PC keyboard (PuTTY)
 * plays the panel buttons until the Raspberry Pi is connected.
 *
 * Hardware (NUCLEO-G0B1RE):
 *   USART1  PC4 (D1, TX) / PC5 (D0, RX)  -> XY-485 -> RS485 line 1, 38400 8N1
 *   USART2  PA2 / PA3                    -> ST-LINK virtual COM port -> PuTTY, 115200
 *   PA5     green LED LD4                -> blinks on every poll
 *   PC13    blue button B1               -> Kvittera (like the button on the panel)
 */
#include "app.h"
#include "main.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "ctrl_modules.h"
#include "modbus_map.h"
#include "modbus_rtu.h"

extern UART_HandleTypeDef huart1;   /* RS485 line 1 */
extern UART_HandleTypeDef huart2;   /* PC terminal */

#define POLL_PERIOD_MS     500u     /* one cycle over all modules of the line */
#define TICK_PERIOD_MS     100u     /* alarm engine timers (delays, service mode) */
#define ANSWER_TIMEOUT_MS  100u     /* 50 ms in the map; 100 ms while the PC plays the modules */
#define FRAME_GAP_MS       2u       /* >= 3.5 characters between frames (1.75 ms at 38400) */
#define BUTTON_DEBOUNCE_MS 30u
#define LINE1              1u

/* ---- configuration (later it comes from the Raspberry Pi) ------------------------------ */
static const alm_point_cfg_t config[] = {
    /* id line mod ch  category  device type        NC  delay en sect  LCD line 1     LCD line 2 */
    {1,  1,   2,  1,  CAT_B,    DT_SERVICEVENTIL,  1,  0,    1, 1,   "SERVICEVENTIL", "SEKTION 1"},
    {2,  1,   2,  2,  CAT_A,    DT_PRESSOSTAT,     0,  0,    1, 1,   "PRESSOSTAT",    "SEKTION 1"},
};
#define POINT_COUNT ((uint16_t)(sizeof(config) / sizeof(config[0])))

static alm_t engine;                /* ~40 kB, static */
static mb_master_t line1;
static ctrl_module_t modules[1];
#define MODULE_COUNT (sizeof(modules) / sizeof(modules[0]))

static uint32_t next_poll, next_tick, last_frame_end;
static GPIO_PinState button_idle, button_last;
static uint32_t button_changed_at;
static uint8_t button_pressed;
static alm_status_t shown;          /* last printed outputs */
static uint8_t shown_valid;

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
        return;   /* what it means is printed by the engine events */
    case EV_MODULE_ONLINE:
        alm_set_module_lost(&engine, m->line, m->addr, 0, now);
        stamp();
        out("Modul L%u-M%02u ONLINE  fw %u.%u\r\n", m->line, m->addr,
            m->fw_version >> 8, m->fw_version & 0xFFu);
        return;
    case EV_MODULE_LOST:
    case EV_MODULE_WRONG_TYPE:
    case EV_MODULE_WRONG_LINE:
        alm_set_module_lost(&engine, m->line, m->addr, 1, now);
        stamp();
        out("Modul L%u-M%02u %s\r\n", m->line, m->addr,
            ev == EV_MODULE_LOST ? "SAKNAS" : ev == EV_MODULE_WRONG_TYPE ? "FEL MODULTYP" : "FEL LINJE");
        return;
    case EV_MODULE_RESTARTED:
        stamp();
        out("Modul L%u-M%02u startade om, konfigureras igen\r\n", m->line, m->addr);
        return;
    case EV_STATUS_CHANGED:
        stamp();
        out("Modul L%u-M%02u status 0x%04X%s\r\n", m->line, m->addr, new_v,
            (new_v & MB_ST_SUPPLY_LOW) ? "  LAG MATNING" : "");
        (void)old_v;
        return;
    case EV_INPUT_TRANSIENT:
        stamp();
        out("Modul L%u-M%02u IN%u kort puls\r\n", m->line, m->addr, index + 1u);
        return;
    default:
        return;   /* comm errors before LOST etc.: not printed at this step */
    }
}

/* ---- events from the alarm engine --------------------------------------------------------- */
static const char *point_name(uint16_t id)
{
    for (uint16_t i = 0; i < POINT_COUNT; i++) {
        if (config[i].id == id) {
            return config[i].lcd1;
        }
    }
    return "?";
}

static void on_alarm_event(void *ctx, const alm_event_t *e)
{
    (void)ctx;
    stamp();
    out("#%-4lu ", (unsigned long)e->seq);
    switch (e->type) {
    case EVT_ALARM_ON:
    case EVT_ALARM_CLEARED:
    case EVT_DELAY_STARTED:
    case EVT_DELAY_CANCELLED: {
        int i = alm_find(&engine, e->source_id);
        char cat = i >= 0 ? (char)alm_display_category(&engine, i) : '?';
        out("P%u %-14s %-15s %s -> %s  (%c)\r\n", e->source_id, point_name(e->source_id),
            alm_event_name(e->type), alm_state_name((uint8_t)e->old_v),
            alm_state_name((uint8_t)e->new_v), cat);
        break;
    }
    case EVT_LINE_OPEN:  out("P%u %-14s FEL: Avbrott\r\n", e->source_id, point_name(e->source_id)); break;
    case EVT_LINE_SHORT: out("P%u %-14s FEL: Kortslutning\r\n", e->source_id, point_name(e->source_id)); break;
    case EVT_LINE_OK:    out("P%u %-14s linjen OK igen\r\n", e->source_id, point_name(e->source_id)); break;
    case EVT_MODULE_LOST: out("FEL: modul %u saknas\r\n", e->source_id); break;
    case EVT_MODULE_BACK: out("modul %u tillbaka\r\n", e->source_id); break;
    case EVT_SERVICE_ON:  out("SERVICELAGE %ld min - inget overfors\r\n", (long)e->new_v); break;
    case EVT_SERVICE_OFF: out("Servicelage slut (%s)\r\n", e->new_v ? "manuellt" : "tiden ute"); break;
    case EVT_RESOUND:     out("Nytt larm: larmdonen ljuder igen\r\n"); break;
    default:              out("%s\r\n", alm_event_name(e->type)); break;
    }
}

/* ---- outputs: print when something changed --------------------------------------------------- */
static void show_outputs(int force)
{
    alm_status_t s;
    alm_status(&engine, HAL_GetTick(), &s);
    if (!force && shown_valid && s.relays == shown.relays && s.led_red_a == shown.led_red_a &&
        s.led_yellow_b == shown.led_yellow_b && s.led_yellow_fault == shown.led_yellow_fault &&
        s.led_green_drift == shown.led_green_drift && s.led_yellow_service == shown.led_yellow_service &&
        s.buzzer == shown.buzzer && s.sounders == shown.sounders && s.key_tech == shown.key_tech) {
        return;
    }
    shown = s;
    shown_valid = 1;
    static const char *const ledb[] = {"av", "fast", "BLINKAR"};
    static const char *const snd[] = {"tysta", "LJUDER", "tystade"};
    out("          RELAN: K1 A=%s  K2 B=%s  K3 Fel=%s  K4 C=%s%s\r\n",
        (s.relays & ALM_K1_A) ? "TILL" : "fran", (s.relays & ALM_K2_B) ? "TILL" : "fran",
        (s.relays & ALM_K3_FAULT_OK) ? "OK(dragen)" : "FEL(slappt)", (s.relays & ALM_K4_C) ? "TILL" : "fran",
        s.transmission_blocked ? "   [SERVICE: blockerat]" : "");
    out("          LED: A=%s B/C=%s Fel=%s Drift=%s Service=%s | Summer=%s | Larmdon=%s | Nyckel=%s\r\n",
        s.led_red_a ? "ROD" : "av", ledb[s.led_yellow_b], s.led_yellow_fault ? "GUL" : "av",
        s.led_green_power ? "gron" : "av", s.led_yellow_service ? "GUL" : "av",
        s.buzzer ? "PA" : "av", snd[s.sounders], s.key_tech ? "TEKNIKER" : "anvandare");
}

/* ---- the panel buttons, from the PC keyboard (PuTTY) ------------------------------------------ */
static void help(void)
{
    out("\r\nTangenter i PuTTY:  t = nyckel TEKNIKER/anvandare   k = Kvittera   a = Aterstall\r\n");
    out("                    s = Tysta larmdon (pa/av)   v = Servicelage 30 min / slut   ? = status\r\n");
    out("Bla knappen pa kortet = Kvittera.  Larmdon = OUT1 pa modul L1-M02.\r\n\r\n");
}

static const char *result_text(alm_result_t r)
{
    switch (r) {
    case ALM_OK:            return "OK";
    case ALM_ACCESS_DENIED: return "NEKAD - vrid nyckeln till TEKNIKER (t)";
    case ALM_BAD_DURATION:  return "fel tid";
    case ALM_UNKNOWN_POINT: return "okand punkt";
    case ALM_BAD_TIME:      return "fel klocka";
    default:                return "?";
    }
}

static void key(char c)
{
    const uint32_t now = HAL_GetTick();
    alm_status_t s;
    switch (c) {
    case 't':
    case 'T':
        alm_set_key(&engine, !engine.key_tech);
        stamp();
        out("Nyckel: %s\r\n", engine.key_tech ? "TEKNIKER" : "anvandare");
        break;
    case 'k':
    case 'K':
        stamp();
        out("Kvittera: %u kvitterade\r\n", alm_ack(&engine, 1, now));
        break;
    case 'a':
    case 'A': {
        uint16_t reset = 0, remaining = 0;
        alm_result_t r = alm_reset(&engine, 1, now, &reset, NULL, 0, &remaining);
        stamp();
        if (r == ALM_OK) {
            out("Aterstall: %u aterstallda, %u kvar (orsaken finns kvar)\r\n", reset, remaining);
        } else {
            out("Aterstall: %s\r\n", result_text(r));
        }
        break;
    }
    case 's':
    case 'S':
        alm_status(&engine, now, &s);
        stamp();
        out("Tysta larmdon: %s\r\n", result_text(alm_silence(&engine, !s.silenced, 1, now)));
        break;
    case 'v':
    case 'V':
        alm_status(&engine, now, &s);
        stamp();
        out("Servicelage: %s\r\n", result_text(alm_service(&engine, s.service_active ? 0u : 30u, 1, now)));
        break;
    case '?':
        help();
        show_outputs(1);
        break;
    default:
        break;
    }
}

static void check_terminal(void)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_OREF);
    }
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
        key((char)(huart2.Instance->RDR & 0xFFu));
    }
}

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
        key('k');
    }
    button_pressed = pressed;
}

/* first output rule: the sounders (OUT1 of L1-M02) follow the engine */
static void drive_module_outputs(void)
{
    alm_status_t s;
    alm_status(&engine, HAL_GetTick(), &s);
    uint8_t cmd = (s.sounders == SOUNDERS_SOUNDING) ? MB_CMD_ON : MB_CMD_OFF;
    modules[0].out_cmd = (uint16_t)((modules[0].out_cmd & ~0x000Fu) | cmd);
}

/* ---- main entry points ------------------------------------------------------------------- */
void app_init(void)
{
    static const uint8_t in_mode[4] = {MB_INCFG_SUPERVISED, MB_INCFG_SUPERVISED,
                                       MB_INCFG_NOT_USED, MB_INCFG_NOT_USED};
    static const uint8_t out_mode[4] = {MB_OUTCFG_SUPERVISED, MB_OUTCFG_NOT_USED,
                                        MB_OUTCFG_NOT_USED, MB_OUTCFG_NOT_USED};
    const uint32_t now = HAL_GetTick();

    out("\r\n=== Sprinkler controller - steg C2: larmlogik ===\r\n");
    alm_init(&engine, now, 0, on_alarm_event, NULL);
    alm_cfg_error_t err[4];
    uint16_t nerr = alm_load(&engine, config, POINT_COUNT, 0, now, err, 4);
    if (nerr) {
        out("KONFIGURATIONSFEL: %u fel (punkt %u)\r\n", nerr, err[0].point_id);
    }
    mb_master_init(&line1, rs485_xfer, &huart1, ANSWER_TIMEOUT_MS);
    ctrl_module_init(&modules[0], 2u, LINE1, in_mode, out_mode);   /* L1-M02 */

    button_idle = button_last = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    button_changed_at = now;
    help();
    show_outputs(1);
    next_poll = next_tick = now;
}

void app_loop(void)
{
    uint32_t t = HAL_GetTick();
    if ((int32_t)(t - next_poll) >= 0) {
        next_poll = t + POLL_PERIOD_MS;
        drive_module_outputs();
        for (uint8_t i = 0; i < MODULE_COUNT; i++) {
            ctrl_module_poll(&line1, &modules[i], on_module_event, NULL);
        }
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    }
    t = HAL_GetTick();
    if ((int32_t)(t - next_tick) >= 0) {
        next_tick = t + TICK_PERIOD_MS;
        alm_tick(&engine, t);
        show_outputs(0);
    }
    check_terminal();
    check_button();
}
