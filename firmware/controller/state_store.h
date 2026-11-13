/*
 * Small state that changes now and then (service mode, Frånkopplade points), kept in flash.
 *
 * Two flash pages used as a log: every save appends a record, so a page is erased only when it
 * is full (the flash takes ~10 000 erases per page). The newest valid record wins. A reset in the
 * middle of a save leaves the previous record, never a broken one.
 *
 *   page:    [magic "STP1"][sequence] record record record ... (0xFF = free)
 *   record:  [0x5A 0xA5][length][CRC-32 of the data] data (padded to 8 bytes)
 *
 * No platform code: the flash is reached through st_flash_t (app.c on the STM32, RAM in tests).
 */
#ifndef STATE_STORE_H
#define STATE_STORE_H

#include <stdint.h>

typedef struct {
    const uint8_t *mem;       /* the two pages, readable like memory */
    uint32_t page_size;       /* bytes per page (2048 on the STM32G0) */
    int (*erase_page)(void *ctx, uint8_t page);                                     /* 0 = ok */
    int (*program)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);  /* 8-byte blocks */
    void *ctx;
} st_flash_t;

typedef struct {
    const st_flash_t *f;
    uint8_t page;             /* page being written */
    uint32_t write_off;       /* next free byte in it; page_size = full */
    uint32_t seq;
} state_store_t;

/* Find the newest record. Returns its length (0..max) and copies it to buf, or -1 if none. */
int state_store_init(state_store_t *s, const st_flash_t *f, uint8_t *buf, uint16_t max);

/* Append a record (len <= page_size - 24). 0 = ok, -1 = flash error. */
int state_store_save(state_store_t *s, const uint8_t *data, uint16_t len);

#endif /* STATE_STORE_H */
