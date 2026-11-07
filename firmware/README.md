# Firmware

```
common/       Κοινά για controller και modules
  modbus_map.h     Ο χάρτης registers (docs/modbus-map.md) σε C
  modbus_rtu.c/.h  Modbus RTU master: CRC, εντολές 03/04/06/16, έλεγχος απαντήσεων
controller/   Main Controller (NUCLEO-G0B1RE)
  alarm.c/.h         Η λογική των συναγερμών (μεταφορά του εικονικού controller του HMI σε C)
  ctrl_modules.c/.h  Ζωή κάθε module: εκκίνηση, ρύθμιση, κυκλική ερώτηση, MODULE_LOST, γεγονότα
  app.c/.h           Βήμα C2: modules → συναγερμοί → relays/LED/σειρήνες, πλήκτρα από το PuTTY
tests/        Δοκιμές στο PC (gcc)
  test_alarm.c       Τα 16 tests του εικονικού controller + 2 νέα, για τη C έκδοση
  host_hal/          Μικρό «ψεύτικο» HAL: τρέχει όλο το app.c στο PC απέναντι στον προσομοιωτή
```

Τα `modbus_rtu` και `ctrl_modules` δεν εξαρτώνται από το hardware: δοκιμάζονται στο PC απέναντι
στον προσομοιωτή modules (`tools/module_sim`). Μόνο το `app.c` χρησιμοποιεί το HAL της ST.

## Βήμα C1 στο STM32CubeIDE (project «Blink»)

Ρυθμίσεις CubeMX: USART1 σε PC4/PC5, Asynchronous, 38400 8N1. USART2 (ST-LINK) 115200 (προεπιλογή).

1. Αντέγραψε στο `Core/Src`: `modbus_rtu.c`, `ctrl_modules.c`, `alarm.c`, `app.c`
   και στο `Core/Inc`: `modbus_rtu.h`, `modbus_map.h`, `ctrl_modules.h`, `alarm.h`, `app.h`.
2. Στο `main.c`:
   - `USER CODE BEGIN Includes`: `#include "app.h"`
   - `USER CODE BEGIN 2`: `app_init();`
   - `USER CODE BEGIN 3`: μόνο `app_loop();`
3. Build → Run.

Στο PC:
- PuTTY στη θύρα COM του **ST-LINK** («STMicroelectronics STLink Virtual COM Port»), 115200: εκεί τυπώνει ο controller.
- `python tools\module_sim\module_sim.py COMx` στη θύρα COM της **USB-RS485**: εκεί «ζει» το module L1-M02.

## Δοκιμές στο PC

```bash
cd firmware
CF="-std=c11 -Wall -Wextra -Werror -Icommon -Icontroller"
gcc $CF -o /tmp/test_modbus_rtu tests/test_modbus_rtu.c common/modbus_rtu.c && /tmp/test_modbus_rtu
gcc $CF -o /tmp/test_alarm tests/test_alarm.c controller/alarm.c && /tmp/test_alarm
gcc $CF -o /tmp/host_ctrl tests/host_ctrl.c common/modbus_rtu.c controller/ctrl_modules.c
gcc -std=gnu11 -Wall -Wextra -Werror -Itests/host_hal -Icommon -Icontroller -o /tmp/host_app \
    tests/host_hal/host_hal.c controller/app.c controller/alarm.c controller/ctrl_modules.c common/modbus_rtu.c
cd .. && HOST_CTRL=/tmp/host_ctrl HOST_APP=/tmp/host_app python -m pytest tools/module_sim   # θέλει pyserial
```
