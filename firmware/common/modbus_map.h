/*
 * Modbus RTU register map, controller <-> I/O modules (version 1).
 * Source of truth: docs/modbus-map.md ("Modbus: controller <-> modules" in the specification).
 * Shared by the controller firmware, the module firmware and (as a copy) the PC module simulator.
 */
#ifndef MODBUS_MAP_H
#define MODBUS_MAP_H

#include <stdint.h>

#define MB_MAP_VERSION            1u
#define MB_MODULE_TYPE_4IN_4OUT   0x0404u

/* Addresses per RS485 line (decided): L1 1-60, L2 61-120, L3 121-180, L4 181-240. */
#define MB_ADDR_BROADCAST         0u
#define MB_ADDR_PER_LINE          60u
#define MB_ADDR_MAX               240u
#define MB_LINE_OF_ADDR(a)        ((uint8_t)(((a) - 1u) / MB_ADDR_PER_LINE + 1u))   /* 1..4 */

/* Function codes */
#define MB_FC_READ_HOLDING        0x03u
#define MB_FC_READ_INPUT          0x04u
#define MB_FC_WRITE_SINGLE        0x06u
#define MB_FC_WRITE_MULTIPLE      0x10u

/* Exception codes */
#define MB_EX_ILLEGAL_FUNCTION    0x01u
#define MB_EX_ILLEGAL_ADDRESS     0x02u
#define MB_EX_ILLEGAL_VALUE       0x03u
#define MB_EX_DEVICE_FAILURE      0x04u

/* ---- Input registers (FC 04): module -> controller ------------------------------------ */
/* Identity */
#define MB_IR_MODULE_TYPE         0x0000u
#define MB_IR_MAP_VERSION         0x0001u
#define MB_IR_FW_VERSION          0x0002u
#define MB_IR_HW_VERSION          0x0003u
#define MB_IR_SERIAL_HI           0x0004u
#define MB_IR_SERIAL_LO           0x0005u
#define MB_IR_DIP_ADDRESS         0x0006u
#define MB_IR_IDENTITY_COUNT      8u

/* Fast status, read every cycle */
#define MB_IR_STATUS              0x0010u
#define MB_IR_INPUTS              0x0011u
#define MB_IR_OUTPUTS             0x0012u
#define MB_IR_CHANGES             0x0013u
#define MB_IR_FAST_COUNT          4u

/* Details (0x0010-0x0022 is one contiguous block of 19 registers) */
#define MB_IR_IN_CHANGES(n)       (0x0014u + (n))      /* n = 0..3 */
#define MB_IR_IN_OHM(n)           (0x0018u + (n))
#define MB_IR_OUT_MA(n)           (0x001Cu + (n))
#define MB_IR_SUPPLY_MV           0x0020u
#define MB_IR_TEMP                0x0021u
#define MB_IR_CRC_ERRORS          0x0022u
#define MB_IR_FULL_COUNT          19u

/* STATUS bits */
#define MB_ST_RESTARTED           (1u << 0)
#define MB_ST_FAILSAFE            (1u << 1)
#define MB_ST_SUPPLY_LOW          (1u << 2)   /* below MB_HR_SUPPLY_LOW_100MV */
#define MB_ST_SUPPLY_HIGH         (1u << 3)   /* above 30.0 V */
#define MB_ST_INTERNAL_FAULT      (1u << 4)
#define MB_ST_DIP_CHANGED         (1u << 5)
#define MB_ST_IDENTIFY            (1u << 6)

/* Input values, 4 bits per input in INPUTS (IN1 = bits 0-3 ... IN4 = bits 12-15) */
#define MB_IN_CLOSED              0u   /* contact closed, ~1 kOhm */
#define MB_IN_OPEN                1u   /* contact open,   ~2 kOhm */
#define MB_IN_LINE_OPEN           2u   /* wire break (Avbrott) */
#define MB_IN_LINE_SHORT          3u   /* short circuit (Kortslutning) */
#define MB_IN_NOT_USED            4u
#define MB_IN_MEASURE_FAULT       5u

/* Output values, 4 bits per output in OUTPUTS: bit 3 = output on, bits 0-2 = supervision */
#define MB_OUT_ON_BIT             0x8u
#define MB_OUT_SUP_OK             0u
#define MB_OUT_SUP_LINE_OPEN      1u
#define MB_OUT_SUP_LINE_SHORT     2u
#define MB_OUT_SUP_OVERCURRENT    3u
#define MB_OUT_SUP_NOT_SUPERVISED 4u

#define MB_NIBBLE(word, n)        ((uint8_t)(((word) >> (4u * (n))) & 0xFu))

/* ---- Holding registers (FC 03 / 06 / 16): controller -> module ------------------------ */
#define MB_HR_OUT_CMD             0x0100u   /* 4 bits per output, values MB_CMD_* */
#define MB_HR_COMMAND             0x0101u   /* one-shot action, reads 0 */
#define MB_HR_SYNC                0x0102u   /* broadcast: restart flash phase */

#define MB_CMD_OFF                0u
#define MB_CMD_ON                 1u
#define MB_CMD_FLASH              2u   /* 1 Hz */
#define MB_CMD_FLASH_FAST         3u   /* 2 Hz */
#define MB_CMD_PULSE              4u

#define MB_COMMAND_CONFIG_DONE    1u
#define MB_COMMAND_CLEAR_FLAGS    2u
#define MB_COMMAND_IDENTIFY_ON    3u
#define MB_COMMAND_IDENTIFY_OFF   4u
#define MB_COMMAND_REBOOT         5u

/* Configuration block, written with one FC 16 after every module start */
#define MB_HR_CFG_START           0x0110u
#define MB_HR_COMM_TIMEOUT_S      0x0110u   /* 2..60, default 10 */
#define MB_HR_FAILSAFE_OUT        0x0111u   /* 4 bits per output: 0 OFF, 1 ON, 2 HOLD, 3 FLASH */
#define MB_HR_DEBOUNCE_MS         0x0112u   /* 20..2000, default 100 */
#define MB_HR_PULSE_100MS         0x0113u   /* default 10 = 1 s */
#define MB_HR_IN_CFG(n)           (0x0114u + (n))   /* low byte mode, high byte EOL profile */
#define MB_HR_OUT_CFG(n)          (0x0118u + (n))   /* low byte mode, high byte current limit /10 mA */
#define MB_HR_SUPPLY_LOW_100MV    0x011Cu   /* SUPPLY_LOW below this, in 0.1 V: 100..300, default 140 = 14.0 V */
#define MB_HR_CFG_COUNT           13u

#define MB_FAILSAFE_OFF           0u
#define MB_FAILSAFE_ON            1u
#define MB_FAILSAFE_HOLD          2u   /* decided default: outputs keep their state */
#define MB_FAILSAFE_FLASH         3u

#define MB_INCFG_NOT_USED         0u
#define MB_INCFG_SUPERVISED       1u
#define MB_INCFG_UNSUPERVISED     2u

#define MB_OUTCFG_NOT_USED        0u
#define MB_OUTCFG_UNSUPERVISED    1u
#define MB_OUTCFG_SUPERVISED      2u

#endif /* MODBUS_MAP_H */
