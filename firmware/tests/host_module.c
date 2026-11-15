/*
 * The module firmware logic (mod_core.c) on the PC, on a serial port / pty, instead of the
 * Python simulator: so the real controller code (host_app) talks to the real module code.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -Icommon -Imodule -o /tmp/host_module \
 *       tests/host_module.c module/mod_core.c common/modbus_rtu.c
 *   /tmp/host_module /dev/pts/N ADDRESS          (or: --fd N ADDRESS for an inherited descriptor)
 *
 * stdin, one command per line:   ohm N VALUE   (N = 1..4, loop resistance in Ohm)
 *                                supply MV
 * stdout: "out XXXX" whenever the outputs change (bit 0 = OUT1), "ready" at start.
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mod_core.h"

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: host_module PORT ADDRESS | host_module --fd N ADDRESS\n");
        return 2;
    }
    int fd, ai = 2;
    if (strcmp(argv[1], "--fd") == 0) {
        fd = atoi(argv[2]);
        ai = 3;
    } else {
        fd = open(argv[1], O_RDWR | O_NOCTTY);
    }
    if (fd < 0 || argc <= ai) {
        perror("port");
        return 1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(fd, TCSANOW, &t);
    }
    static mod_t m;
    mod_init(&m, (uint8_t)atoi(argv[ai]), 0x00C0FFEEu, now_ms());
    for (uint8_t n = 0; n < 4; n++) {
        mod_set_loop_ohm(&m, n, n == 1 ? 2000 : 1000, now_ms());   /* like the simulator */
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ready\n");

    uint8_t frame[MOD_MAX_FRAME], resp[MOD_MAX_FRAME];
    uint16_t len = 0;
    char line[128];
    size_t lpos = 0;
    int last_out = -1;
    for (;;) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {0, POLLIN, 0}};
        int r = poll(p, 2, len ? 3 : 5);          /* 3 ms of silence ends a frame (t3.5 ~ 1 ms) */
        uint32_t now = now_ms();
        if (r > 0 && (p[0].revents & POLLIN)) {
            uint8_t b[256];
            ssize_t k = read(fd, b, sizeof(b));
            for (ssize_t i = 0; i < k; i++) {
                if (len < sizeof(frame)) frame[len++] = b[i];
            }
        } else if (len) {
            uint16_t n = mod_handle(&m, frame, len, resp, now);
            len = 0;
            if (n) {
                if (write(fd, resp, n) != n) perror("write");
            }
        }
        if (r > 0 && (p[1].revents & (POLLIN | POLLHUP))) {
            char c;
            if (read(0, &c, 1) <= 0) return 0;
            if (c != '\n' && lpos < sizeof(line) - 1) {
                line[lpos++] = c;
            } else {
                line[lpos] = 0;
                lpos = 0;
                unsigned a, b;
                if (sscanf(line, "ohm %u %u", &a, &b) == 2 && a >= 1 && a <= 4) {
                    mod_set_loop_ohm(&m, (uint8_t)(a - 1), (uint16_t)b, now);
                } else if (sscanf(line, "supply %u", &a) == 1) {
                    mod_set_supply(&m, (uint16_t)a, 250, 0);
                }
            }
        }
        mod_tick(&m, now);
        int out = mod_outputs(&m, now);
        if (out != last_out) {
            printf("out %d%d%d%d\n", out & 1, (out >> 1) & 1, (out >> 2) & 1, (out >> 3) & 1);
            last_out = out;
        }
    }
}
