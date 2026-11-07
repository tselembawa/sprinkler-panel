/* Modbus RTU master - see modbus_rtu.h */
#include "modbus_rtu.h"

#include <stddef.h>

/* CRC-16/MODBUS: polynomial 0xA001 (reflected 0x8005), start 0xFFFF, low byte sent first. */
uint16_t mb_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8u; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

uint16_t mb_append_crc(uint8_t *frame, uint16_t len)
{
    uint16_t crc = mb_crc16(frame, len);
    frame[len] = (uint8_t)(crc & 0xFFu);
    frame[len + 1u] = (uint8_t)(crc >> 8);
    return (uint16_t)(len + 2u);
}

int mb_crc_ok(const uint8_t *frame, uint16_t len)
{
    if (len < 4u) {
        return 0;
    }
    uint16_t crc = mb_crc16(frame, (uint16_t)(len - 2u));
    return frame[len - 2u] == (uint8_t)(crc & 0xFFu) && frame[len - 1u] == (uint8_t)(crc >> 8);
}

void mb_master_init(mb_master_t *m, mb_xfer_fn xfer, void *ctx, uint32_t timeout_ms)
{
    m->xfer = xfer;
    m->ctx = ctx;
    m->timeout_ms = timeout_ms;
    m->last_exception = 0;
    m->stats = (mb_stats_t){0};
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static mb_status_t count(mb_master_t *m, mb_status_t s)
{
    switch (s) {
    case MB_OK:            m->stats.ok++; break;
    case MB_ERR_TIMEOUT:   m->stats.timeout++; break;
    case MB_ERR_CRC:       m->stats.crc++; break;
    case MB_ERR_FRAME:     m->stats.frame++; break;
    case MB_ERR_EXCEPTION: m->stats.exception++; break;
    case MB_ERR_TRANSPORT: m->stats.transport++; break;
    default: break;
    }
    return s;
}

/*
 * Send a request and check the answer: right CRC, right address, right function or an
 * exception for that function, and the expected length. On success rx/len hold the answer.
 */
static mb_status_t transact(mb_master_t *m, uint8_t *tx, uint16_t tx_len,
                            uint8_t *rx, uint16_t expected_len)
{
    const uint8_t addr = tx[0];
    const uint8_t fc = tx[1];
    tx_len = mb_append_crc(tx, tx_len);

    if (addr == 0u) {   /* broadcast: nobody answers */
        int r = m->xfer(m->ctx, tx, tx_len, NULL, 0u, 0u);
        return count(m, r < 0 ? MB_ERR_TRANSPORT : MB_OK);
    }

    int n = m->xfer(m->ctx, tx, tx_len, rx, MB_MAX_FRAME, m->timeout_ms);
    if (n < 0) {
        return count(m, MB_ERR_TRANSPORT);
    }
    if (n == 0) {
        return count(m, MB_ERR_TIMEOUT);
    }
    uint16_t len = (uint16_t)n;
    if (!mb_crc_ok(rx, len)) {
        return count(m, MB_ERR_CRC);
    }
    if (rx[0] != addr) {
        return count(m, MB_ERR_FRAME);
    }
    if (rx[1] == (uint8_t)(fc | 0x80u) && len == 5u) {
        m->last_exception = rx[2];
        return count(m, MB_ERR_EXCEPTION);
    }
    if (rx[1] != fc || len != expected_len) {
        return count(m, MB_ERR_FRAME);
    }
    return MB_OK;   /* counted by the caller after its own checks */
}

static mb_status_t read_regs(mb_master_t *m, uint8_t fc, uint8_t addr, uint16_t start,
                             uint16_t n, uint16_t *out)
{
    if (n == 0u || n > MB_MAX_READ || addr == 0u || out == NULL) {
        return MB_ERR_ARG;
    }
    uint8_t tx[8];
    uint8_t rx[MB_MAX_FRAME];
    tx[0] = addr;
    tx[1] = fc;
    put16(&tx[2], start);
    put16(&tx[4], n);
    /* answer: addr, fc, byte count, 2*n data bytes, crc */
    mb_status_t s = transact(m, tx, 6u, rx, (uint16_t)(5u + 2u * n));
    if (s != MB_OK) {
        return s;
    }
    if (rx[2] != (uint8_t)(2u * n)) {
        return count(m, MB_ERR_FRAME);
    }
    for (uint16_t i = 0; i < n; i++) {
        out[i] = get16(&rx[3u + 2u * i]);
    }
    return count(m, MB_OK);
}

mb_status_t mb_read_input(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t n, uint16_t *out)
{
    return read_regs(m, 0x04u, addr, start, n, out);
}

mb_status_t mb_read_holding(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t n, uint16_t *out)
{
    return read_regs(m, 0x03u, addr, start, n, out);
}

mb_status_t mb_write_single(mb_master_t *m, uint8_t addr, uint16_t reg, uint16_t value)
{
    uint8_t tx[8];
    uint8_t rx[MB_MAX_FRAME];
    tx[0] = addr;
    tx[1] = 0x06u;
    put16(&tx[2], reg);
    put16(&tx[4], value);
    mb_status_t s = transact(m, tx, 6u, rx, 8u);
    if (s != MB_OK || addr == 0u) {
        return s;
    }
    /* the answer is an echo of the request */
    if (get16(&rx[2]) != reg || get16(&rx[4]) != value) {
        return count(m, MB_ERR_FRAME);
    }
    return count(m, MB_OK);
}

mb_status_t mb_write_multiple(mb_master_t *m, uint8_t addr, uint16_t start, uint16_t n,
                              const uint16_t *values)
{
    if (n == 0u || n > MB_MAX_WRITE || values == NULL) {
        return MB_ERR_ARG;
    }
    uint8_t tx[MB_MAX_FRAME];
    uint8_t rx[MB_MAX_FRAME];
    tx[0] = addr;
    tx[1] = 0x10u;
    put16(&tx[2], start);
    put16(&tx[4], n);
    tx[6] = (uint8_t)(2u * n);
    for (uint16_t i = 0; i < n; i++) {
        put16(&tx[7u + 2u * i], values[i]);
    }
    mb_status_t s = transact(m, tx, (uint16_t)(7u + 2u * n), rx, 8u);
    if (s != MB_OK || addr == 0u) {
        return s;
    }
    if (get16(&rx[2]) != start || get16(&rx[4]) != n) {
        return count(m, MB_ERR_FRAME);
    }
    return count(m, MB_OK);
}

const char *mb_status_text(mb_status_t s)
{
    switch (s) {
    case MB_OK:            return "OK";
    case MB_ERR_TIMEOUT:   return "no answer";
    case MB_ERR_CRC:       return "bad CRC";
    case MB_ERR_FRAME:     return "bad frame";
    case MB_ERR_EXCEPTION: return "exception";
    case MB_ERR_TRANSPORT: return "UART error";
    case MB_ERR_ARG:       return "bad argument";
    default:               return "?";
    }
}
