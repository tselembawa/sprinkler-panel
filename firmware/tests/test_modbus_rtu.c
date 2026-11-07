/* Unit tests for modbus_rtu.c (host gcc). */
#include <stdio.h>
#include <string.h>

#include "modbus_rtu.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* scripted transport: remembers the request, returns a prepared answer */
static uint8_t last_tx[256];
static uint16_t last_tx_len;
static uint8_t answer[256];
static int answer_len;

static int fake_xfer(void *ctx, const uint8_t *tx, uint16_t tx_len,
                     uint8_t *rx, uint16_t rx_max, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    memcpy(last_tx, tx, tx_len);
    last_tx_len = tx_len;
    if (rx_max == 0) {
        return 0;
    }
    if (answer_len > 0) {
        memcpy(rx, answer, (size_t)answer_len);
    }
    return answer_len;
}

static void set_answer(const uint8_t *a, int len, int add_crc)
{
    memcpy(answer, a, (size_t)len);
    answer_len = add_crc ? mb_append_crc(answer, (uint16_t)len) : len;
}

int main(void)
{
    /* the classic example of the Modbus specification: 01 03 00 00 00 0A -> CRC C5 CD */
    uint8_t f[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    CHECK(mb_append_crc(f, 6) == 8);
    CHECK(f[6] == 0xC5 && f[7] == 0xCD);
    CHECK(mb_crc_ok(f, 8));
    f[3] ^= 1;
    CHECK(!mb_crc_ok(f, 8));

    mb_master_t m;
    mb_master_init(&m, fake_xfer, NULL, 50);
    uint16_t regs[4];

    /* read input registers: request bytes and decoding */
    const uint8_t ans1[] = {0x02, 0x04, 0x08, 0x00, 0x01, 0x44, 0x10, 0x00, 0x00, 0x12, 0x34};
    set_answer(ans1, sizeof(ans1), 1);
    CHECK(mb_read_input(&m, 2, 0x0010, 4, regs) == MB_OK);
    const uint8_t req1[] = {0x02, 0x04, 0x00, 0x10, 0x00, 0x04};
    CHECK(last_tx_len == 8 && memcmp(last_tx, req1, 6) == 0);
    CHECK(regs[0] == 0x0001 && regs[1] == 0x4410 && regs[2] == 0 && regs[3] == 0x1234);

    /* no answer, bad CRC, wrong address, wrong length */
    answer_len = 0;
    CHECK(mb_read_input(&m, 2, 0x0010, 4, regs) == MB_ERR_TIMEOUT);
    set_answer(ans1, sizeof(ans1), 1);
    answer[4] ^= 0xFF;
    CHECK(mb_read_input(&m, 2, 0x0010, 4, regs) == MB_ERR_CRC);
    const uint8_t wrong_addr[] = {0x03, 0x04, 0x08, 0, 0, 0, 0, 0, 0, 0, 0};
    set_answer(wrong_addr, sizeof(wrong_addr), 1);
    CHECK(mb_read_input(&m, 2, 0x0010, 4, regs) == MB_ERR_FRAME);
    set_answer(ans1, 9, 1);   /* too short for 4 registers */
    CHECK(mb_read_input(&m, 2, 0x0010, 4, regs) == MB_ERR_FRAME);

    /* exception answer */
    const uint8_t ex[] = {0x02, 0x84, 0x02};
    set_answer(ex, sizeof(ex), 1);
    CHECK(mb_read_input(&m, 2, 0x0999, 1, regs) == MB_ERR_EXCEPTION);
    CHECK(m.last_exception == 0x02);

    /* write single: echo answer */
    const uint8_t ws[] = {0x02, 0x06, 0x01, 0x00, 0x00, 0x01};
    set_answer(ws, sizeof(ws), 1);
    CHECK(mb_write_single(&m, 2, 0x0100, 0x0001) == MB_OK);
    CHECK(memcmp(last_tx, ws, 6) == 0);
    set_answer(ws, sizeof(ws), 1);
    CHECK(mb_write_single(&m, 2, 0x0100, 0x0002) == MB_ERR_FRAME);   /* echo does not match */

    /* write multiple */
    const uint16_t cfg[3] = {10, 0x2222, 100};
    const uint8_t wm[] = {0x02, 0x10, 0x01, 0x10, 0x00, 0x03};
    set_answer(wm, sizeof(wm), 1);
    CHECK(mb_write_multiple(&m, 2, 0x0110, 3, cfg) == MB_OK);
    const uint8_t wm_req[] = {0x02, 0x10, 0x01, 0x10, 0x00, 0x03, 0x06, 0x00, 0x0A, 0x22, 0x22, 0x00, 0x64};
    CHECK(last_tx_len == sizeof(wm_req) + 2 && memcmp(last_tx, wm_req, sizeof(wm_req)) == 0);

    /* broadcast: sent, no answer expected */
    answer_len = 0;
    CHECK(mb_write_single(&m, 0, 0x0102, 1) == MB_OK);

    /* argument checks */
    CHECK(mb_read_input(&m, 2, 0, 0, regs) == MB_ERR_ARG);
    CHECK(mb_read_input(&m, 2, 0, 126, regs) == MB_ERR_ARG);
    CHECK(mb_read_input(&m, 0, 0, 1, regs) == MB_ERR_ARG);

    printf("%s (%d failures)\n", failures ? "FAILED" : "all modbus_rtu tests passed", failures);
    return failures ? 1 : 0;
}
