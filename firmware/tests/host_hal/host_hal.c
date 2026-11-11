/* PC implementation of the HAL stand-in (see main.h) and a main() that runs app.c. */
#define _DEFAULT_SOURCE
#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

GPIO_TypeDef host_gpioa, host_gpiob, host_gpioc;
static USART_TypeDef usart1, usart2;
UART_HandleTypeDef huart1 = {&usart1, -1, -1, HAL_UART_STATE_READY, 0};
UART_HandleTypeDef huart2 = {&usart2, 0, 1, HAL_UART_STATE_READY, 0};
static uint32_t t0;

static uint32_t ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

uint32_t HAL_GetTick(void) { return ms() - t0; }

static int readable(int fd, unsigned wait_ms)
{
    fd_set s;
    FD_ZERO(&s);
    FD_SET(fd, &s);
    struct timeval tv = {wait_ms / 1000u, (wait_ms % 1000u) * 1000u};
    return select(fd + 1, &s, NULL, NULL, &tv) > 0;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *p, uint16_t n, uint32_t timeout)
{
    (void)timeout;
    if (h == &huart1) {   /* RS485: drop stale bytes first, like the real flush request */
        uint8_t junk[64];
        while (readable(h->fd_in, 0) && read(h->fd_in, junk, sizeof(junk)) > 0) { }
    }
    return write(h->fd_out, p, n) == (ssize_t)n ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle(UART_HandleTypeDef *h, uint8_t *p, uint16_t size,
                                           uint16_t *rx_len, uint32_t timeout)
{
    uint16_t got = 0;
    unsigned wait = timeout;
    while (got < size && readable(h->fd_in, wait)) {
        ssize_t r = read(h->fd_in, p + got, size - got);
        if (r <= 0) break;
        got = (uint16_t)(got + r);
        wait = 10;   /* "idle line" */
    }
    *rx_len = got;
    return got ? HAL_OK : HAL_TIMEOUT;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin) { (void)port; (void)pin; return GPIO_PIN_RESET; }
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin) { (void)port; (void)pin; }

int host_uart_flag(UART_HandleTypeDef *h, uint32_t flag)
{
    (void)h; (void)flag;
    return 0;
}

/* panel outputs: kept in memory only */
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { (void)port; (void)init; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (state) port->dummy |= pin; else port->dummy &= ~(int)pin;
}

/* no LCD on the PC: nobody answers on I2C */
I2C_HandleTypeDef hi2c1;
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t addr, uint8_t *p, uint16_t n, uint32_t timeout)
{
    (void)h; (void)addr; (void)p; (void)n; (void)timeout;
    return HAL_ERROR;
}

void HAL_Delay(uint32_t ms)
{
    usleep(ms * 1000u);
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *p, uint16_t size)
{
    (void)size;
    if (h->RxState != HAL_UART_STATE_READY) return HAL_BUSY;
    h->rx_ptr = p;
    h->RxState = HAL_UART_STATE_BUSY_RX;
    return HAL_OK;
}

/* the "interrupt": deliver what arrived on stdin, one byte per callback */
static void deliver_rx(void)
{
    uint8_t buf[256];
    while (readable(huart2.fd_in, 0)) {
        ssize_t n = read(huart2.fd_in, buf, sizeof(buf));
        if (n <= 0) exit(0);   /* the other side closed: end of test */
        for (ssize_t i = 0; i < n; i++) {
            if (huart2.RxState != HAL_UART_STATE_BUSY_RX) continue;   /* not listening: lost */
            *huart2.rx_ptr = buf[i];
            huart2.RxState = HAL_UART_STATE_READY;
            HAL_UART_RxCpltCallback(&huart2);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: host_app <rs485 fd>\n"); return 2; }
    huart1.fd_in = huart1.fd_out = atoi(argv[1]);
    struct termios t;
    if (tcgetattr(huart1.fd_in, &t) == 0) { cfmakeraw(&t); tcsetattr(huart1.fd_in, TCSANOW, &t); }
    if (tcgetattr(0, &t) == 0) { cfmakeraw(&t); tcsetattr(0, TCSANOW, &t); }
    setvbuf(stdout, NULL, _IONBF, 0);
    t0 = ms();
    app_init();
    for (;;) {
        deliver_rx();
        app_loop();
        usleep(1000);
    }
}
