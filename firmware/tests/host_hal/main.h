/*
 * Tiny stand-in for the STM32 HAL, so that controller/app.c can run on the PC in the
 * end-to-end tests: USART1 = a pseudo terminal to the module simulator,
 * USART2 = stdin/stdout (the Pi link; bytes delivered like the RX interrupt would).
 * Only what app.c uses.
 */
#ifndef HOST_MAIN_H
#define HOST_MAIN_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;

typedef struct { volatile uint32_t RDR; } USART_TypeDef;
typedef enum { HAL_UART_STATE_RESET = 0, HAL_UART_STATE_READY = 0x20, HAL_UART_STATE_BUSY_RX = 0x22 } HAL_UART_StateTypeDef;
typedef struct { USART_TypeDef *Instance; int fd_in, fd_out; HAL_UART_StateTypeDef RxState; uint8_t *rx_ptr; } UART_HandleTypeDef;
typedef struct { int dummy; } GPIO_TypeDef;

extern GPIO_TypeDef host_gpioa, host_gpiob, host_gpioc;
#define GPIOA (&host_gpioa)
#define GPIOB (&host_gpiob)
#define GPIOC (&host_gpioc)
#define GPIO_PIN_0  0x0001u
#define GPIO_PIN_1  0x0002u
#define GPIO_PIN_3  0x0008u
#define GPIO_PIN_4  0x0010u
#define GPIO_PIN_5  0x0020u
#define GPIO_PIN_6  0x0040u
#define GPIO_PIN_7  0x0080u
#define GPIO_PIN_8  0x0100u
#define GPIO_PIN_13 0x2000u
#define GPIO_PIN_14 0x4000u
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_OUTPUT_OD 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *p, uint16_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle(UART_HandleTypeDef *h, uint8_t *p, uint16_t size,
                                           uint16_t *rx_len, uint32_t timeout);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin);
int host_uart_flag(UART_HandleTypeDef *h, uint32_t flag);
typedef struct { int unused; } I2C_HandleTypeDef;
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t addr, uint8_t *p, uint16_t n, uint32_t timeout);
void HAL_Delay(uint32_t ms);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *p, uint16_t size);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *h);

#define UART_FLAG_RXNE 1u
#define UART_FLAG_ORE  2u
#define UART_RXDATA_FLUSH_REQUEST 0u
#define UART_CLEAR_OREF 0u
#define UART_CLEAR_NEF  0u
#define UART_CLEAR_FEF  0u
#define UART_CLEAR_PEF  0u
#define UART_CLEAR_IDLEF 0u
#define __HAL_UART_SEND_REQ(h, r)     ((void)(h), (void)(r))
#define __HAL_UART_CLEAR_FLAG(h, f)   ((void)(h), (void)(f))
#define __HAL_UART_GET_FLAG(h, f)     host_uart_flag((h), (f))

#endif
