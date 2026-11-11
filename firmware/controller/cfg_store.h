/*
 * The active configuration in non-volatile memory (the controller's own flash).
 *
 * After a reset the controller starts with the configuration it had, not with a built-in
 * example, and without waiting for the Raspberry Pi. No platform code here: the flash is
 * reached through the functions in cfg_store_t (app.c for the STM32, RAM in the tests).
 *
 * Layout: header 16 bytes  [magic][record size][count][config CRC][check]
 *         then count x alm_point_cfg_t, exactly as in the alarm engine.
 * The header is written last: a reset while saving leaves "no configuration", never half a one.
 */
#ifndef CFG_STORE_H
#define CFG_STORE_H

#include <stdint.h>

#include "alarm.h"

#define CFG_STORE_MAGIC   0x31464353u     /* "SCF1" */
#define CFG_STORE_HEADER  16u

typedef struct {
    const uint8_t *mem;       /* the region, readable like memory (flash is memory mapped) */
    uint32_t size;            /* bytes */
    /* erase the pages that cover [offset, offset+len); 0 = ok */
    int (*erase)(void *ctx, uint32_t offset, uint32_t len);
    /* write len bytes (multiple of 8) at offset (multiple of 8) into erased flash; 0 = ok */
    int (*program)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);
    void *ctx;
} cfg_store_t;

/* Bytes needed for n points. */
uint32_t cfg_store_bytes(uint16_t n);

/* The stored points (pointing into the region) or NULL if there is no valid configuration.
   *n = number of points (may be 0: an empty configuration that was saved on purpose). */
const alm_point_cfg_t *cfg_store_load(const cfg_store_t *s, uint16_t *n);

/* Save. 0 = written, 1 = the same configuration is already stored (nothing written),
   -1 = does not fit or the flash reported an error. */
int cfg_store_save(const cfg_store_t *s, const alm_point_cfg_t *pts, uint16_t n);

#endif /* CFG_STORE_H */
