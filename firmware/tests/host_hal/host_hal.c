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

GPIO_TypeDef host_gpioa, host_gpioc;
static USART_TypeDef usart1, usart2;
UART_HandleTypeDef huart1 = {&usart1, -1, -1};
UART_HandleTypeDef huart2 = {&usart2, 0, 1};
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
    if (h != &huart2 || flag != UART_FLAG_RXNE || !readable(0, 0)) return 0;
    char c;
    if (read(0, &c, 1) != 1) exit(0);   /* stdin closed: end of test */
    h->Instance->RDR = (uint8_t)c;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: host_app <rs485 fd>\n"); return 2; }
    huart1.fd_in = huart1.fd_out = atoi(argv[1]);
    struct termios t;
    if (tcgetattr(huart1.fd_in, &t) == 0) { cfmakeraw(&t); tcsetattr(huart1.fd_in, TCSANOW, &t); }
    setvbuf(stdout, NULL, _IONBF, 0);
    t0 = ms();
    app_init();
    for (;;) {
        app_loop();
        usleep(1000);
    }
}
