#include "cfg_store.h"

#include <string.h>

typedef struct {
    uint32_t magic;
    uint16_t rec_size;
    uint16_t count;
    uint32_t crc;          /* alm_config_crc of the points */
    uint32_t check;        /* magic ^ crc ^ count ^ rec_size: catches a damaged header */
} store_header_t;

static uint32_t header_check(const store_header_t *h)
{
    return h->magic ^ h->crc ^ ((uint32_t)h->count << 16) ^ h->rec_size ^ 0xA5A5A5A5u;
}

uint32_t cfg_store_bytes(uint16_t n)
{
    uint32_t b = CFG_STORE_HEADER + (uint32_t)n * (uint32_t)sizeof(alm_point_cfg_t);
    return (b + 7u) & ~7u;
}

const alm_point_cfg_t *cfg_store_load(const cfg_store_t *s, uint16_t *n)
{
    store_header_t h;
    *n = 0;
    if (s->size < CFG_STORE_HEADER) {
        return NULL;
    }
    memcpy(&h, s->mem, sizeof(h));
    if (h.magic != CFG_STORE_MAGIC || h.rec_size != sizeof(alm_point_cfg_t) ||
        h.count > ALM_MAX_POINTS || h.check != header_check(&h) ||
        cfg_store_bytes(h.count) > s->size) {
        return NULL;
    }
    const alm_point_cfg_t *pts = (const alm_point_cfg_t *)(const void *)(s->mem + CFG_STORE_HEADER);
    if (alm_config_crc(pts, h.count) != h.crc) {
        return NULL;
    }
    *n = h.count;
    return pts;
}

/* program in blocks through a small buffer: the source need not be 8-byte aligned or padded */
static int program_bytes(const cfg_store_t *s, uint32_t off, const uint8_t *src, uint32_t len)
{
    uint8_t blk[64];
    while (len > 0u) {
        uint32_t k = len < sizeof(blk) ? len : (uint32_t)sizeof(blk);
        memset(blk, 0xFF, sizeof(blk));
        memcpy(blk, src, k);
        uint32_t padded = (k + 7u) & ~7u;
        if (s->program(s->ctx, off, blk, padded) != 0) {
            return -1;
        }
        off += padded;
        src += k;
        len -= k;
    }
    return 0;
}

int cfg_store_save(const cfg_store_t *s, const alm_point_cfg_t *pts, uint16_t n)
{
    const uint32_t bytes = cfg_store_bytes(n);
    if (n > ALM_MAX_POINTS || bytes > s->size) {
        return -1;
    }
    const uint32_t crc = alm_config_crc(pts, n);
    uint16_t have = 0;
    const alm_point_cfg_t *old = cfg_store_load(s, &have);
    if (old && have == n && alm_config_crc(old, have) == crc) {
        return 1;                       /* the CRC covers every field of every point */
    }
    if (s->erase(s->ctx, 0, bytes) != 0) {
        return -1;
    }
    if (n > 0u && program_bytes(s, CFG_STORE_HEADER, (const uint8_t *)pts,
                                (uint32_t)n * (uint32_t)sizeof(alm_point_cfg_t)) != 0) {
        return -1;
    }
    store_header_t h;
    h.magic = CFG_STORE_MAGIC;
    h.rec_size = (uint16_t)sizeof(alm_point_cfg_t);
    h.count = n;
    h.crc = crc;
    h.check = header_check(&h);
    if (program_bytes(s, 0, (const uint8_t *)&h, sizeof(h)) != 0) {
        return -1;
    }
    uint16_t back = 0;
    return (cfg_store_load(s, &back) != NULL && back == n) ? 0 : -1;
}
