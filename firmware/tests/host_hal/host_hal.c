/* PC implementation of the HAL stand-in (see main.h) and a main() that runs app.c. */
#define _DEFAULT_SOURCE
#include "main.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

/* ---- flash ---------------------------------------------------------------------------- */
uint32_t host_flash_base;
static uint8_t *flash_mem;
static int flash_unlocked;

__attribute__((constructor)) static void host_flash_open(void)
{
    const char *path = getenv("HOST_FLASH");
    int fd = -1, fresh = 1;
    if (path) {
        fd = open(path, O_RDWR | O_CREAT, 0644);
        off_t len = lseek(fd, 0, SEEK_END);
        fresh = len < (off_t)FLASH_SIZE;
        if (fresh && ftruncate(fd, FLASH_SIZE) != 0) { perror("HOST_FLASH"); exit(1); }
    }
    void *m = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE,
                   (fd >= 0 ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS) | MAP_32BIT, fd, 0);
    if (m == MAP_FAILED) { perror("flash mmap"); exit(1); }
    flash_mem = m;
    host_flash_base = (uint32_t)(uintptr_t)m;
    if (fresh) memset(flash_mem, 0xFF, FLASH_SIZE);
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { flash_unlocked = 1; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { flash_unlocked = 0; return HAL_OK; }

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e, uint32_t *page_error)
{
    *page_error = 0xFFFFFFFFu;
    if (!flash_unlocked || e->TypeErase != FLASH_TYPEERASE_PAGES) return HAL_ERROR;
    uint32_t bank0 = e->Banks == FLASH_BANK_2 ? FLASH_BANK_SIZE : 0u;
    for (uint32_t p = e->Page; p < e->Page + e->NbPages; p++) {
        if (p >= FLASH_BANK_SIZE / FLASH_PAGE_SIZE) { *page_error = p; return HAL_ERROR; }
        memset(&flash_mem[bank0 + p * FLASH_PAGE_SIZE], 0xFF, FLASH_PAGE_SIZE);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    uint32_t off = address - host_flash_base;
    if (!flash_unlocked || type != FLASH_TYPEPROGRAM_DOUBLEWORD || (off & 7u) || off >= FLASH_SIZE) {
        return HAL_ERROR;
    }
    static const uint8_t erased[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(&flash_mem[off], erased, 8) != 0) return HAL_ERROR;   /* like PROGERR */
    memcpy(&flash_mem[off], &data, 8);
    return HAL_OK;
}

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
