/* COBS (Consistent Overhead Byte Stuffing): frames without 0x00 inside, 0x00 ends a frame. */
#ifndef COBS_H
#define COBS_H

#include <stddef.h>
#include <stdint.h>

/* out must hold len + len/254 + 1 bytes. Returns the encoded length (without the 0x00). */
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out);
/* Returns the decoded length, or -1 if the data is not valid COBS or does not fit. */
int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_max);

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection (Python: binascii.crc_hqx(d, 0xFFFF)) */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

#endif
