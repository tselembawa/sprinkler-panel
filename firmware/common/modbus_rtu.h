/*
 * Modbus RTU master: frame building, checking and the four function codes we use.
 *
 * Hardware independent: the actual sending/receiving is done by a "transport" function
 * that the application provides (on the STM32 it uses the HAL UART; in the host tests
 * a simulated module). This keeps the protocol code testable on a PC.
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>

#define MB_MAX_FRAME   256u
#define MB_MAX_READ    125u   /* registers per read  (Modbus limit) */
#define MB_MAX_WRITE   123u   /* registers per write (Modbus limit) */

/*
 * Transport: send tx[0..tx_len), then receive one answer frame into rx (at most rx_max bytes),
 * waiting at most timeout_ms for it. Returns the number of bytes received
 * (0 = no answer), or a negative value on a hardware error.
 * For broadcast (address 0) it is called with rx_max = 0: send only.
 */
typedef int (*mb_xfer_fn)(void *ctx, const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_max, uint32_t timeout_ms);

typedef enum {
    MB_OK = 0,
    MB_ERR_TIMEOUT,     /* no answer */
    MB_ERR_CRC,         /* answer with wrong CRC (noise, collision) */
    MB_ERR_FRAME,       /* answer from the wrong address / function / length */
    MB_ERR_EXCEPTION,   /* module answered with a Modbus exception (see last_exception) */
    MB_ERR_TRANSPORT,   /* UART error */
    MB_ERR_ARG          /* invalid call (count out of range, ...) */
} mb_status_t;

typedef struct {
    uint32_t ok, timeout, crc, frame, exception, transport;
} mb_stats_t;

typedef struct {
    mb_xfer_fn xfer;
    void *ctx;
    uint32_t timeout_ms;      /* how long to wait for an answer */
    uint8_t last_exception;   /* exception code of the last MB_ERR_EXCEPTION */
    mb_stats_t stats;
} mb_master_t;

void mb_master_init(mb_master_t *m, mb_xfer_fn xfer, void *ctx, uint32_t timeout_ms);

mb_status_t mb_read_input(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t count, uint16_t *out);
mb_status_t mb_read_holding(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t count, uint16_t *out);
mb_status_t mb_write_single(mb_master_t *m, uint8_t addr, uint16_t reg, uint16_t value);
mb_status_t mb_write_multiple(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t count, const uint16_t *values);

const char *mb_status_text(mb_status_t s);

/* Low level, also used by the tests */
uint16_t mb_crc16(const uint8_t *data, uint16_t len);
uint16_t mb_append_crc(uint8_t *frame, uint16_t len);   /* returns len + 2 */
int mb_crc_ok(const uint8_t *frame, uint16_t len);

#endif /* MODBUS_RTU_H */
