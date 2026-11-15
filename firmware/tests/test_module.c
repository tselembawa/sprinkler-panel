/*
 * Module firmware logic on the PC.
 *   gcc -std=c11 -Wall -Wextra -Werror -Icommon -Imodule -o /tmp/test_module \
 *       tests/test_module.c module/mod_core.c common/modbus_rtu.c && /tmp/test_module
 */
#include <stdio.h>
#include <string.h>

#include "mod_core.h"
#include "modbus_rtu.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static mod_t m;
static uint8_t resp[MOD_MAX_FRAME];
static uint32_t now = 1000;

static uint16_t req(const uint8_t *f, uint16_t n)
{
    uint8_t buf[MOD_MAX_FRAME];
    memcpy(buf, f, n);
    n = mb_append_crc(buf, n);
    return mod_handle(&m, buf, n, resp, now);
}

static uint16_t read_ir(uint16_t reg)
{
    uint8_t f[] = {2, 4, (uint8_t)(reg >> 8), (uint8_t)reg, 0, 1};
    CHECK(req(f, sizeof(f)) == 7 && resp[1] == 4);
    return (uint16_t)(resp[3] << 8 | resp[4]);
}

static void write_hr(uint16_t reg, uint16_t v)
{
    uint8_t f[] = {2, 6, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(v >> 8), (uint8_t)v};
    CHECK(req(f, sizeof(f)) == 8 && resp[1] == 6);
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        now += 5;
        mod_tick(&m, now);
    }
}

int main(void)
{
    /* ADC -> Ohm -> value: 1 kOhm reference to 3.3 V */
    CHECK(mod_adc_to_ohm(2048, 1000) > 950 && mod_adc_to_ohm(2048, 1000) < 1050);   /* closed */
    CHECK(mod_adc_to_ohm(2730, 1000) > 1900 && mod_adc_to_ohm(2730, 1000) < 2100);  /* open */
    CHECK(mod_adc_to_ohm(4095, 1000) == MOD_OHM_OPEN && mod_adc_to_ohm(0, 1000) == 0);
    CHECK(mod_classify(1000, MB_INCFG_SUPERVISED) == MB_IN_CLOSED);
    CHECK(mod_classify(2000, MB_INCFG_SUPERVISED) == MB_IN_OPEN);
    CHECK(mod_classify(100, MB_INCFG_SUPERVISED) == MB_IN_LINE_SHORT);
    CHECK(mod_classify(MOD_OHM_OPEN, MB_INCFG_SUPERVISED) == MB_IN_LINE_OPEN);
    CHECK(mod_classify(5000, MB_INCFG_SUPERVISED) == MB_IN_MEASURE_FAULT);
    CHECK(mod_classify(100, MB_INCFG_UNSUPERVISED) == MB_IN_CLOSED);
    CHECK(mod_classify(MOD_OHM_OPEN, MB_INCFG_UNSUPERVISED) == MB_IN_OPEN);
    CHECK(mod_classify(1000, MB_INCFG_NOT_USED) == MB_IN_NOT_USED);

    mod_init(&m, 2, 0x12345678u, now);
    for (uint8_t n = 0; n < 4; n++) mod_set_loop_ohm(&m, n, n == 1 ? 2000 : 1000, now);
    /* identity and the first measurement at once (no 100 ms of "Avbrott" after start) */
    CHECK(read_ir(MB_IR_MODULE_TYPE) == MB_MODULE_TYPE_4IN_4OUT && read_ir(MB_IR_DIP_ADDRESS) == 2);
    CHECK(read_ir(MB_IR_SERIAL_LO) == 0x5678 && read_ir(MB_IR_FW_VERSION) == MOD_FW_VERSION);
    CHECK(read_ir(MB_IR_INPUTS) == 0x0010);
    CHECK(read_ir(MB_IR_STATUS) & MB_ST_RESTARTED);

    /* not for us / bad CRC: silent, CRC errors counted */
    uint8_t other[] = {3, 4, 0, 0x10, 0, 4};
    CHECK(req(other, sizeof(other)) == 0);
    uint8_t bad[] = {2, 4, 0, 0x10, 0, 4, 0x12, 0x34};
    CHECK(mod_handle(&m, bad, sizeof(bad), resp, now) == 0 && read_ir(MB_IR_CRC_ERRORS) == 1);

    /* configuration with FC 16: IN3/IN4 not used, then CONFIG_DONE */
    uint8_t cfg[7 + 26] = {2, 0x10, 0x01, 0x10, 0, 13, 26};
    const uint16_t vals[13] = {10, 0x2222, 100, 10, 1, 1, 0, 0, 0x3202, 0, 0, 0, 140};
    for (int i = 0; i < 13; i++) { cfg[7 + 2 * i] = (uint8_t)(vals[i] >> 8); cfg[8 + 2 * i] = (uint8_t)vals[i]; }
    CHECK(req(cfg, sizeof(cfg)) == 8 && resp[1] == 0x10);
    CHECK(read_ir(MB_IR_INPUTS) == 0x4410);
    write_hr(MB_HR_COMMAND, MB_COMMAND_CONFIG_DONE);
    CHECK(!(read_ir(MB_IR_STATUS) & MB_ST_RESTARTED));
    /* a wrong value anywhere in FC 16: nothing changes */
    cfg[8] = 99;                                             /* timeout 99 s: out of range */
    CHECK(req(cfg, sizeof(cfg)) == 5 && resp[1] == 0x90 && resp[2] == MB_EX_ILLEGAL_VALUE);
    uint8_t rd[] = {2, 3, 0x01, 0x10, 0, 1};
    CHECK(req(rd, sizeof(rd)) == 7 && resp[4] == 10);
    uint8_t unknown[] = {2, 4, 0x00, 0x30, 0, 1};
    CHECK(req(unknown, sizeof(unknown)) == 5 && resp[2] == MB_EX_ILLEGAL_ADDRESS);
    uint8_t fc5[] = {2, 5, 0, 0, 0xFF, 0};
    CHECK(req(fc5, sizeof(fc5)) == 5 && resp[2] == MB_EX_ILLEGAL_FUNCTION);

    /* debounce: a 50 ms blip is ignored, a lasting change counts after 100 ms */
    uint16_t ch0 = read_ir(MB_IR_CHANGES);
    mod_set_loop_ohm(&m, 0, 2000, now); run(50);
    mod_set_loop_ohm(&m, 0, 1000, now); run(200);
    CHECK(read_ir(MB_IR_INPUTS) == 0x4410 && read_ir(MB_IR_CHANGES) == ch0);
    mod_set_loop_ohm(&m, 0, MOD_OHM_OPEN, now); run(60);
    CHECK((read_ir(MB_IR_INPUTS) & 0xF) == MB_IN_CLOSED);
    run(60);
    CHECK((read_ir(MB_IR_INPUTS) & 0xF) == MB_IN_LINE_OPEN && read_ir(MB_IR_CHANGES) == ch0 + 1);
    CHECK(read_ir(MB_IR_IN_CHANGES(0)) == 1 && read_ir(MB_IR_IN_OHM(0)) == MOD_OHM_OPEN);
    mod_set_loop_ohm(&m, 0, 1000, now); run(150);

    /* outputs: OUT1 used (supervised in the map, LEDs here: not supervised), others not used */
    write_hr(MB_HR_OUT_CMD, 0x0001);
    CHECK(mod_outputs(&m, now) == 0x1 && (read_ir(MB_IR_OUTPUTS) & 0xF) == (MB_OUT_ON_BIT | MB_OUT_SUP_NOT_SUPERVISED));
    write_hr(MB_HR_OUT_CMD, 0x1111);                         /* OUT2-4 not used: stay off */
    CHECK(mod_outputs(&m, now) == 0x1);
    write_hr(MB_HR_OUT_CMD, 0x0002);                         /* flash 1 Hz */
    uint8_t sync[] = {0, 6, 0x01, 0x02, 0, 1};
    CHECK(req(sync, sizeof(sync)) == 0);                     /* broadcast: no answer */
    CHECK(mod_outputs(&m, now) == 1 && mod_outputs(&m, now + 600) == 0 && mod_outputs(&m, now + 1100) == 1);
    write_hr(MB_HR_OUT_CMD, 0x0004);                         /* pulse 1 s */
    CHECK(mod_outputs(&m, now + 900) == 1 && mod_outputs(&m, now + 1100) == 0);

    /* failsafe: HOLD keeps OUT1 on; a request ends failsafe, CLEAR_FLAGS clears the flag */
    write_hr(MB_HR_OUT_CMD, 0x0001);
    run(10500);
    CHECK(m.failsafe && mod_outputs(&m, now) == 1);
    CHECK((read_ir(MB_IR_STATUS) & MB_ST_FAILSAFE) && !m.failsafe);
    write_hr(MB_HR_COMMAND, MB_COMMAND_CLEAR_FLAGS);
    CHECK(!(read_ir(MB_IR_STATUS) & MB_ST_FAILSAFE) && read_ir(MB_IR_CRC_ERRORS) == 0);

    /* supply, identify, DIP changed, reboot request */
    mod_set_supply(&m, 13000, 215, 0);
    CHECK(read_ir(MB_IR_STATUS) & MB_ST_SUPPLY_LOW);
    CHECK(read_ir(MB_IR_SUPPLY_MV) == 13000 && read_ir(MB_IR_TEMP) == 215);
    mod_set_supply(&m, 24000, 215, 0);
    write_hr(MB_HR_COMMAND, MB_COMMAND_IDENTIFY_ON);
    CHECK(read_ir(MB_IR_STATUS) & MB_ST_IDENTIFY);
    write_hr(MB_HR_COMMAND, MB_COMMAND_IDENTIFY_OFF);
    CHECK(!(read_ir(MB_IR_STATUS) & MB_ST_IDENTIFY));
    mod_set_dip(&m, 2);
    CHECK(!(read_ir(MB_IR_STATUS) & MB_ST_DIP_CHANGED));
    mod_set_dip(&m, 3);
    CHECK(read_ir(MB_IR_STATUS) & MB_ST_DIP_CHANGED);
    write_hr(MB_HR_COMMAND, MB_COMMAND_REBOOT);
    CHECK(m.reboot);

    /* no address (all DIP off): silent */
    mod_init(&m, 0, 1, now);
    uint8_t any[] = {0, 4, 0, 0x10, 0, 4};
    CHECK(req(any, sizeof(any)) == 0);

    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "OK", checks, failures);
    return failures != 0;
}
