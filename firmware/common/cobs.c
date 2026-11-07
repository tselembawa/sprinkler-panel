#include "cobs.h"

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t r = 0, w = 1, code_pos = 0;
    uint8_t code = 1;
    while (r < len) {
        if (in[r] == 0u) {
            out[code_pos] = code;
            code_pos = w++;
            code = 1;
        } else {
            out[w++] = in[r];
            code++;
            if (code == 0xFFu) {
                out[code_pos] = code;
                code_pos = w++;
                code = 1;
            }
        }
        r++;
    }
    out[code_pos] = code;
    return w;
}

int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_max)
{
    size_t r = 0, w = 0;
    while (r < len) {
        uint8_t code = in[r];
        if (code == 0u || r + code > len) {
            return -1;
        }
        r++;
        for (uint8_t i = 1; i < code; i++) {
            if (w >= out_max || in[r] == 0u) {
                return -1;
            }
            out[w++] = in[r++];
        }
        if (code != 0xFFu && r < len) {
            if (w >= out_max) {
                return -1;
            }
            out[w++] = 0u;
        }
    }
    return (int)w;
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc ^= (uint16_t)(*data++ << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
