/*
 * I/O module on the NUCLEO-G031K8 (breadboard prototype): the STM32 side.
 * Measures, switches pins, moves bytes. Everything the module decides is in mod_core.c.
 *
 * CubeMX (project "Module"): USART1 on PB6/PB7, Asynchronous, 38400 8N1, NVIC "USART1 global
 * interrupt" on. USART2 (ST-LINK, 115200) as the board gives it. Nothing else: GPIO, ADC and
 * watchdog are set up here (the ADC with registers, so no ADC driver is needed in CubeMX).
 *
 *   RS485 module   RO -> D0 (PB7, USART1_RX)   DI <- D1 (PB6, USART1_TX)   (DE/RE: see MOD_DE_PIN)
 *   Inputs  IN1 A0 (PA0)  IN2 A1 (PA1)  IN3 A2 (PA4)  IN4 A3 (PA5):
 *           3V3 - 1 kOhm - pin - loop (1 kOhm in series, contact with 1 kOhm across) - GND
 *   Outputs OUT1 D3 (PB1)  OUT2 D6 (PB0)  OUT3 D9 (PA8)  OUT4 D11 (PB5): LED + 330 Ohm to GND
 *   DIP     1 D2 (PA15)  2 D7 (PB2)  3 D8 (PB8)  4 D10 (PB9)  5 D12 (PB4)  6 D13 (PB3)
 *           7 A7 (PA7)  8 A4 (PA12): ON = to GND, address = sum (1, 2, 4, ... 128)
 *   LED     LD3 on the board (PC6): short blink = fine, slow = no controller, fast = IDENTIFY
 *   Supply  A6 (PA6): 24 V through 100 kOhm / 10 kOhm (MOD_SUPPLY_RTOP/RBOT; 0 = not fitted)
 */
#include "mod_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "mod_core.h"

extern UART_HandleTypeDef huart1;      /* RS485 */
extern UART_HandleTypeDef huart2;      /* ST-LINK: log text for PuTTY */

#define R_REF_OHM          1000u       /* resistor from 3V3 to each input pin */
#define MOD_SUPPLY_RTOP    0u          /* 100000u when the 24 V divider is fitted */
#define MOD_SUPPLY_RBOT    10000u
/* #define MOD_DE_PIN  GPIO_PIN_11       RS485 module with DE/RE (MAX485): A5 (PA11). The
                                         modules with automatic direction need nothing. */
#define WATCHDOG_MS        1000u

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} pin_t;

static const pin_t out_pins[4] = {{GPIOB, GPIO_PIN_1}, {GPIOB, GPIO_PIN_0}, {GPIOA, GPIO_PIN_8}, {GPIOB, GPIO_PIN_5}};
static const pin_t dip_pins[8] = {
    {GPIOA, GPIO_PIN_15}, {GPIOB, GPIO_PIN_2}, {GPIOB, GPIO_PIN_8}, {GPIOB, GPIO_PIN_9},
    {GPIOB, GPIO_PIN_4},  {GPIOB, GPIO_PIN_3}, {GPIOA, GPIO_PIN_7}, {GPIOA, GPIO_PIN_12},
};
static const pin_t led_pin = {GPIOC, GPIO_PIN_6};
static const uint8_t in_channels[4] = {0, 1, 4, 5};     /* ADC_IN0, IN1, IN4, IN5 */
#define CH_SUPPLY   6u
#define CH_TEMP     12u
#define CH_VREFINT  13u
/* factory calibration in system memory (STM32G0 reference manual), measured at 3.0 V */
#define VREFINT_CAL     (*(const uint16_t *)0x1FFF75AAuL)
#define TS_CAL1         (*(const uint16_t *)0x1FFF75A8uL)    /* at 30 C */
#define TS_CAL2         (*(const uint16_t *)0x1FFF75CAuL)    /* at 130 C */
#define CAL_VREF_MV     3000u

static mod_t mod;

/* ---- log on the ST-LINK port ------------------------------------------------------------------ */
static void out(const char *fmt, ...)
{
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), 50);
    }
}

/* ---- RS485 receive: one byte at a time into a ring, the main loop cuts frames ----------------- */
static uint8_t rx_byte;
static volatile uint8_t ring[256];
static volatile uint8_t ring_head;
static uint8_t ring_tail;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h)
{
    if (h == &huart1) {
        ring[ring_head] = rx_byte;
        ring_head++;
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *h)
{
    if (h == &huart1) {                       /* noise, overrun: go on listening */
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

static void rs485_send(const uint8_t *data, uint16_t len)
{
#ifdef MOD_DE_PIN
    HAL_GPIO_WritePin(GPIOA, MOD_DE_PIN, GPIO_PIN_SET);
#endif
    HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 100);    /* returns after the last stop bit */
#ifdef MOD_DE_PIN
    HAL_GPIO_WritePin(GPIOA, MOD_DE_PIN, GPIO_PIN_RESET);
#endif
    ring_tail = ring_head;                    /* forget our own echo, if the transceiver gives one */
}

/* ---- ADC (registers: one channel at a time, slow sampling for the 1 kOhm sources) ------------- */
static void adc_init(void)
{
    __HAL_RCC_ADC_CLK_ENABLE();
    ADC1->CR = 0;
    ADC1->CFGR2 = ADC_CFGR2_CKMODE_0;                         /* PCLK / 2 */
    ADC1_COMMON->CCR |= ADC_CCR_VREFEN | ADC_CCR_TSEN;
    ADC1->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1);
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {
    }
    ADC1->SMPR = 7u;                                          /* 160.5 cycles: 20 us at 8 MHz */
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {
    }
}

static uint16_t adc_read(uint8_t ch)
{
    ADC1->ISR = ADC_ISR_CCRDY;
    ADC1->CHSELR = 1uL << ch;
    while (!(ADC1->ISR & ADC_ISR_CCRDY)) {
    }
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC)) {
    }
    return (uint16_t)ADC1->DR;
}

static uint16_t adc_avg(uint8_t ch)
{
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += adc_read(ch);
    }
    return (uint16_t)(sum / 8u);
}

static void measure(uint32_t now)
{
    for (uint8_t n = 0; n < 4u; n++) {
        mod_set_loop_ohm(&mod, n, mod_adc_to_ohm(adc_avg(in_channels[n]), R_REF_OHM), now);
    }
    /* supply, temperature, and a self-check of the analog part */
    uint16_t vref = adc_avg(CH_VREFINT);
    uint32_t vdda_mv = vref ? (uint32_t)VREFINT_CAL * CAL_VREF_MV / vref : 0u;
    int32_t ts = (int32_t)adc_avg(CH_TEMP) * (int32_t)vdda_mv / (int32_t)CAL_VREF_MV;
    int32_t c1 = TS_CAL1, c2 = TS_CAL2;
    int32_t t10 = c2 != c1 ? 300 + (ts - c1) * 1000 / (c2 - c1) : 250;
    uint16_t supply = 24000u;
    if (MOD_SUPPLY_RTOP) {
        uint32_t pin_mv = (uint32_t)adc_avg(CH_SUPPLY) * vdda_mv / 4095u;
        supply = (uint16_t)(pin_mv * (MOD_SUPPLY_RTOP + MOD_SUPPLY_RBOT) / MOD_SUPPLY_RBOT);
    }
    uint8_t bad = vdda_mv < 2900u || vdda_mv > 3600u;           /* 3.3 V supply out of limits */
    mod_set_supply(&mod, supply, (int16_t)t10, bad);
}

/* ---- pins --------------------------------------------------------------------------------------- */
static void pin_out(const pin_t *p)
{
    GPIO_InitTypeDef g = {0};
    HAL_GPIO_WritePin(p->port, p->pin, GPIO_PIN_RESET);
    g.Pin = p->pin;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(p->port, &g);
}

static uint8_t read_dip(void)
{
    uint8_t a = 0;
    for (uint8_t i = 0; i < 8u; i++) {
        if (HAL_GPIO_ReadPin(dip_pins[i].port, dip_pins[i].pin) == GPIO_PIN_RESET) {
            a |= (uint8_t)(1u << i);
        }
    }
    return a;
}

static void pins_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    for (uint8_t n = 0; n < 4u; n++) {
        pin_out(&out_pins[n]);
    }
    pin_out(&led_pin);
#ifdef MOD_DE_PIN
    pin_out(&(pin_t){GPIOA, MOD_DE_PIN});
#endif
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    for (uint8_t i = 0; i < 8u; i++) {
        g.Pin = dip_pins[i].pin;
        HAL_GPIO_Init(dip_pins[i].port, &g);
    }
    g.Mode = GPIO_MODE_ANALOG;                                 /* inputs and supply */
    g.Pull = GPIO_NOPULL;
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &g);
}

/* ---- watchdog ------------------------------------------------------------------------------------ */
static void watchdog_start(void)
{
    if (RCC->CSR & RCC_CSR_IWDGRSTF) {
        out("Omstart av watchdog!\r\n");
    }
    RCC->CSR |= RCC_CSR_RMVF;
    __HAL_RCC_DBGMCU_CLK_ENABLE();
    DBG->APBFZ1 |= DBG_APB_FZ1_DBG_IWDG_STOP;                  /* paused while debugging */
    IWDG->KR = 0xCCCC;
    IWDG->KR = 0x5555;
    IWDG->PR = 3;                                              /* 32 kHz / 32 = 1 ms */
    IWDG->RLR = WATCHDOG_MS - 1u;
    while (IWDG->SR) {
    }
    IWDG->KR = 0xAAAA;
}

/* ---- main ---------------------------------------------------------------------------------------- */
static uint8_t frame[MOD_MAX_FRAME], resp[MOD_MAX_FRAME];
static uint16_t frame_len;
static uint32_t last_byte_ms, last_measure_ms, last_dip_ms;
static uint8_t last_out = 0xFF;

void mod_app_init(void)
{
    pins_init();
    HAL_Delay(5);                                              /* DIP pull-ups settle */
    uint8_t addr = read_dip();
    uint32_t serial = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    mod_init(&mod, addr > MB_ADDR_MAX ? 0u : addr, serial, HAL_GetTick());
    adc_init();
    measure(HAL_GetTick());                                    /* first values before any question */
    out("\r\nModul 4/4 fw %u.%u, adress %u (DIP), serie %08lX\r\n", MOD_FW_VERSION >> 8,
        MOD_FW_VERSION & 0xFFu, addr, (unsigned long)serial);
    if (addr == 0u || addr > MB_ADDR_MAX) {
        out("INGEN GILTIG ADRESS: stall DIP 1-240 och starta om\r\n");
    }
    HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    watchdog_start();
}

void mod_app_loop(void)
{
    IWDG->KR = 0xAAAA;
    uint32_t now = HAL_GetTick();

    while (ring_tail != ring_head) {                           /* bytes in: build the frame */
        if (frame_len < sizeof(frame)) {
            frame[frame_len++] = ring[ring_tail];
        }
        ring_tail++;
        last_byte_ms = now;
    }
    if (frame_len && now - last_byte_ms >= 2u) {               /* > 1.75 ms silence: frame done */
        uint16_t n = mod_handle(&mod, frame, frame_len, resp, now);
        frame_len = 0;
        if (n) {
            rs485_send(resp, n);
        }
        if (mod.reboot) {
            HAL_Delay(5);
            NVIC_SystemReset();
        }
    }

    if (now - last_measure_ms >= 5u) {                         /* inputs every 5 ms */
        last_measure_ms = now;
        measure(now);
    }
    if (now - last_dip_ms >= 500u) {
        last_dip_ms = now;
        mod_set_dip(&mod, read_dip());
    }
    mod_tick(&mod, now);

    uint8_t o = mod_outputs(&mod, now);
    for (uint8_t k = 0; k < 4u; k++) {
        HAL_GPIO_WritePin(out_pins[k].port, out_pins[k].pin, (o >> k) & 1u ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    if (o != last_out) {
        last_out = o;
        out("Utgangar: %u%u%u%u\r\n", o & 1u, (o >> 1) & 1u, (o >> 2) & 1u, (o >> 3) & 1u);
    }
    HAL_GPIO_WritePin(led_pin.port, led_pin.pin, mod_led(&mod, now) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
