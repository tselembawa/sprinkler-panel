#include "state_store.h"

#include <string.h>

#define PAGE_MAGIC  0x31505453u      /* "STP1" */
#define REC_MAGIC   0xA55Au
#define HDR         8u

static uint32_t crc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int erased(const uint8_t *p, uint32_t len)
{
    while (len--) {
        if (*p++ != 0xFFu) return 0;
    }
    return 1;
}

static uint32_t pad8(uint32_t n)
{
    return (n + 7u) & ~7u;
}

/* Scan one page: newest valid record (offset/length) and where writing can go on.
   Returns 1 if the page has a valid page header. */
static int scan(const st_flash_t *f, uint8_t page, uint32_t *seq, int32_t *rec_off, uint16_t *rec_len,
                uint32_t *write_off)
{
    const uint8_t *m = f->mem + (uint32_t)page * f->page_size;
    *rec_off = -1;
    *rec_len = 0;
    if (rd32(m) != PAGE_MAGIC) {
        *write_off = f->page_size;
        return 0;
    }
    *seq = rd32(m + 4);
    uint32_t off = HDR;
    while (off + HDR <= f->page_size) {
        const uint8_t *r = m + off;
        if (erased(r, HDR)) {
            if (!erased(r, f->page_size - off)) {
                *write_off = f->page_size;           /* data of an unfinished save: use the other page */
                return 1;
            }
            break;                                   /* free from here on */
        }
        uint16_t magic = (uint16_t)(r[0] | (r[1] << 8));
        uint16_t len = (uint16_t)(r[2] | (r[3] << 8));
        if (magic != REC_MAGIC || off + HDR + pad8(len) > f->page_size ||
            crc32(r + HDR, len) != rd32(r + 4)) {
            *write_off = f->page_size;               /* broken (reset while writing): page "full" */
            return 1;
        }
        *rec_off = (int32_t)(off + HDR);
        *rec_len = len;
        off += HDR + pad8(len);
    }
    *write_off = off;
    return 1;
}

int state_store_init(state_store_t *s, const st_flash_t *f, uint8_t *buf, uint16_t max)
{
    s->f = f;
    uint32_t seq[2] = {0, 0}, wo[2];
    int32_t ro[2];
    uint16_t rl[2];
    int ok[2];
    for (uint8_t p = 0; p < 2; p++) {
        ok[p] = scan(f, p, &seq[p], &ro[p], &rl[p], &wo[p]);
    }
    /* the newer page first; if it has no record yet (reset just after switching), the older one */
    int newer = (ok[0] && ok[1]) ? ((int32_t)(seq[1] - seq[0]) > 0 ? 1 : 0) : (ok[1] ? 1 : 0);
    if (!ok[0] && !ok[1]) {
        s->page = 1;                     /* the first save switches to page 0 */
        s->write_off = f->page_size;
        s->seq = 0;
        return -1;
    }
    s->page = (uint8_t)newer;
    s->write_off = wo[newer];
    s->seq = seq[newer];
    int use = newer;
    if (ro[newer] < 0 && ok[1 - newer] && ro[1 - newer] >= 0) {
        use = 1 - newer;
    }
    if (ro[use] < 0 || rl[use] > max) {
        return -1;
    }
    memcpy(buf, f->mem + (uint32_t)use * f->page_size + (uint32_t)ro[use], rl[use]);
    return rl[use];
}

static int append(state_store_t *s, const uint8_t *data, uint16_t len)
{
    const st_flash_t *f = s->f;
    uint8_t blk[64];
    uint32_t base = (uint32_t)s->page * f->page_size;
    uint32_t off = s->write_off;
    /* data first, header last: a reset in between leaves a record that does not count */
    uint32_t done = 0;
    while (done < len) {
        uint32_t k = len - done < sizeof(blk) ? len - done : (uint32_t)sizeof(blk);
        memset(blk, 0xFF, sizeof(blk));
        memcpy(blk, data + done, k);
        if (f->program(f->ctx, base + off + HDR + done, blk, pad8(k)) != 0) {
            return -1;
        }
        done += k;
    }
    uint8_t h[HDR];
    h[0] = (uint8_t)REC_MAGIC; h[1] = (uint8_t)(REC_MAGIC >> 8);
    h[2] = (uint8_t)len; h[3] = (uint8_t)(len >> 8);
    wr32(h + 4, crc32(data, len));
    if (f->program(f->ctx, base + off, h, HDR) != 0) {
        return -1;
    }
    s->write_off = off + HDR + pad8(len);
    return 0;
}

int state_store_save(state_store_t *s, const uint8_t *data, uint16_t len)
{
    const st_flash_t *f = s->f;
    if (HDR + HDR + pad8(len) > f->page_size) {
        return -1;
    }
    if (s->write_off + HDR + pad8(len) > f->page_size) {
        /* page full: start the other one (the old page stays valid until this one has a record) */
        uint8_t next = (uint8_t)(1u - s->page);
        if (f->erase_page(f->ctx, next) != 0) {
            return -1;
        }
        uint8_t h[HDR];
        wr32(h, PAGE_MAGIC);
        wr32(h + 4, s->seq + 1u);
        if (f->program(f->ctx, (uint32_t)next * f->page_size, h, HDR) != 0) {
            return -1;
        }
        s->page = next;
        s->seq++;
        s->write_off = HDR;
    }
    if (append(s, data, len) != 0) {
        s->write_off = f->page_size;     /* the rest of this page may be dirty: next save switches */
        return -1;
    }
    return 0;
}
