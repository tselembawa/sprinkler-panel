/*
 * Host (PC) test driver for the controller module protocol.
 * Talks Modbus over a file descriptor (a pseudo terminal in the tests, connected to the
 * Python module simulator) and is driven line by line on stdin:
 *     poll            one cycle over all modules, prints the events
 *     out <n> <hex>   set the wanted outputs of module index n
 *     ident <n> <0|1> identify LED of module n
 *     quit
 * Every command ends with a line "done". Events are printed as
 *     EV <addr> <NAME> <index> <old> <new>
 *
 * Usage: host_ctrl <fd> <addr>:<line> [<addr>:<line> ...]
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "ctrl_modules.h"
#include "modbus_map.h"
#include "modbus_rtu.h"

static int wait_readable(int fd, unsigned ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {ms / 1000u, (ms % 1000u) * 1000u};
    return select(fd + 1, &set, NULL, NULL, &tv);
}

/* send, then collect bytes until 10 ms of silence (or the timeout before the first byte) */
static int fd_xfer(void *ctx, const uint8_t *tx, uint16_t tx_len,
                   uint8_t *rx, uint16_t rx_max, uint32_t timeout_ms)
{
    int fd = *(int *)ctx;
    uint8_t junk[256];
    while (wait_readable(fd, 0) > 0 && read(fd, junk, sizeof(junk)) > 0) {
        /* flush */
    }
    if (write(fd, tx, tx_len) != (ssize_t)tx_len) {
        return -1;
    }
    if (rx_max == 0u) {
        return 0;
    }
    int got = 0;
    unsigned wait = timeout_ms;
    while (got < rx_max) {
        int r = wait_readable(fd, wait);
        if (r < 0 && errno != EINTR) {
            return -1;
        }
        if (r <= 0) {
            break;
        }
        ssize_t n = read(fd, rx + got, (size_t)(rx_max - got));
        if (n <= 0) {
            break;
        }
        got += (int)n;
        wait = 10;
    }
    return got;
}

static void on_event(void *ctx, const ctrl_module_t *m, ctrl_event_t ev,
                     uint8_t index, uint16_t old_v, uint16_t new_v)
{
    (void)ctx;
    printf("EV %u %s %u %u %u\n", m->addr, ctrl_event_name(ev), index, old_v, new_v);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: host_ctrl <fd> <addr>:<line> ...\n");
        return 2;
    }
    int fd = atoi(argv[1]);
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(fd, TCSANOW, &t);
    }
    mb_master_t mb;
    mb_master_init(&mb, fd_xfer, &fd, 100u);

    enum { MAXM = 8 };
    ctrl_module_t mods[MAXM];
    int count = 0;
    static const uint8_t in_mode[4] = {MB_INCFG_SUPERVISED, MB_INCFG_SUPERVISED,
                                       MB_INCFG_NOT_USED, MB_INCFG_NOT_USED};
    static const uint8_t out_mode[4] = {MB_OUTCFG_SUPERVISED, MB_OUTCFG_NOT_USED,
                                        MB_OUTCFG_NOT_USED, MB_OUTCFG_NOT_USED};
    for (int i = 2; i < argc && count < MAXM; i++) {
        unsigned a, l;
        if (sscanf(argv[i], "%u:%u", &a, &l) == 2) {
            ctrl_module_init(&mods[count++], (uint8_t)a, (uint8_t)l, in_mode, out_mode);
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned n, v;
        if (strncmp(line, "poll", 4) == 0) {
            for (int i = 0; i < count; i++) {
                ctrl_module_poll(&mb, &mods[i], on_event, NULL);
            }
        } else if (sscanf(line, "out %u %x", &n, &v) == 2 && n < (unsigned)count) {
            mods[n].out_cmd = (uint16_t)v;
        } else if (sscanf(line, "ident %u %u", &n, &v) == 2 && n < (unsigned)count) {
            printf("IDENT %s\n", mb_status_text(ctrl_module_identify(&mb, &mods[n], (int)v)));
        } else if (strncmp(line, "quit", 4) == 0) {
            break;
        }
        printf("done\n");
    }
    return 0;
}
