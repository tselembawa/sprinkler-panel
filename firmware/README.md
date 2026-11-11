# Firmware

```
common/       Κοινά για controller και modules
  modbus_map.h       Ο χάρτης registers (docs/modbus-map.md) σε C
  modbus_rtu.c/.h    Modbus RTU master: CRC, εντολές 03/04/06/16, έλεγχος απαντήσεων
  pi_link_proto.h    Το πρωτόκολλο Raspberry Pi ↔ controller (docs/pi-link.md): τύποι, κωδικοί, μεγέθη
  cobs.c/.h          COBS πλαισίωση + CRC-16/CCITT για τη σύνδεση με το Pi
controller/   Main Controller (NUCLEO-G0B1RE)
  alarm.c/.h         Η λογική των συναγερμών (μεταφορά του εικονικού controller του HMI σε C)
  ctrl_modules.c/.h  Ζωή κάθε module: εκκίνηση, ρύθμιση, κυκλική ερώτηση, MODULE_LOST, γεγονότα
  pi_link.c/.h       Η πλευρά του controller στη σύνδεση με το Pi: απαντά σε κάθε αίτημα
  lcd.c/.h           Οθόνη 20×4 (HD44780 + PCF8574 I2C): στέλνει μόνο ό,τι άλλαξε· ÅÄÖ, ελληνικά, ρωσικά
                     με δικούς μας χαρακτήρες (8 θέσεις, μοιράζονται ανά οθόνη)
  lcd_view.c/.h      Τι δείχνει η οθόνη: ηρεμία (ώρα Σουηδίας) ή συναγερμοί/σφάλματα ένα-ένα
  panel_io.c/.h      Έξοδοι του πίνακα: ρελέ K1–K4, 6 LED (με αναβόσβημα), βομβητής με ρυθμό A/B/Fel
  app.c/.h           Modules → συναγερμοί → ρελέ/LED/βομβητής/σειρήνες, σύνδεση με το Pi, LCD, watchdog
tests/        Δοκιμές στο PC (gcc)
  test_alarm.c       Τα tests του εικονικού controller + τα νέα, για τη C έκδοση
  host_hal/          Μικρό «ψεύτικο» HAL: τρέχει όλο το app.c στο PC
```

Μόνο το `app.c` χρησιμοποιεί το HAL της ST. Όλα τα άλλα δοκιμάζονται στο PC: ο controller
τρέχει ανάμεσα στον προσομοιωτή modules (`tools/module_sim`) και στον κώδικα του HMI
(`hmi/app/serial_controller.py`).

## Βήμα C3 στο STM32CubeIDE (project «Blink»)

Ρυθμίσεις CubeMX (`.ioc`):
- USART1 σε PC4/PC5, Asynchronous, 38400 8N1 (όπως πριν).
- USART2 (ST-LINK) 115200 8N1 (όπως πριν) + **NVIC Settings → «USART2/LPUART2 global interrupt» ✔**.
- **Project Manager → Linker Settings → Minimum Stack Size `0x1000`** (το πρόγραμμα μεγάλωσε).
- Generate Code.

Αρχεία: στο `Core/Src` τα `modbus_rtu.c`, `ctrl_modules.c`, `alarm.c`, `pi_link.c`, `cobs.c`, `app.c`
και στο `Core/Inc` τα `modbus_rtu.h`, `modbus_map.h`, `ctrl_modules.h`, `alarm.h`, `pi_link.h`,
`pi_link_proto.h`, `cobs.h`, `app.h`. Το `main.c` μένει όπως στο C2
(`#include "app.h"`, `app_init();`, `app_loop();`).

Η θύρα του ST-LINK δεν είναι πια για το PuTTY: μιλάει δυαδικά με το HMI. Τα μηνύματα που
έβγαιναν στο PuTTY φαίνονται τώρα στο παράθυρο του HMI (`CONTROLLER: ...`).

### Δοκιμή με το HMI στο PC (Windows)

1. Κλείσε το PuTTY.
2. Προσομοιωτής module στη θύρα της USB-RS485 (όπως πριν):
   `python tools\module_sim\module_sim.py COMy`
3. Το HMI με τον πραγματικό controller (η θύρα του ST-LINK βρίσκεται μόνη της):
   ```
   cd hmi
   pip install -r requirements.txt
   set SPRINKLER_CONTROLLER=serial
   python -m uvicorn app.main:app --port 8000
   ```
   (ή `set SPRINKLER_CONTROLLER=serial:COMx` για συγκεκριμένη θύρα). Browser: http://localhost:8000
4. Η καρτέλα «Simulator» έχει μόνο τον κλειδοδιακόπτη (μέχρι να μπει ο πραγματικός).

### Στο Raspberry Pi

Το Nucleo στο USB του Pi (`/dev/ttyACM0`):
`SPRINKLER_CONTROLLER=serial bash hmi/scripts/install-pi.sh` και reboot.

## LCD (βήμα L1)

CubeMX: pin **PB8 → I2C1_SCL**, **PB9 → I2C1_SDA**, Connectivity → I2C1 → **I2C**, Standard Mode 100 kHz.
Αρχεία: `lcd.c`, `lcd_view.c` στο `Core/Src`, `lcd.h`, `lcd_view.h` στο `Core/Inc`.
Καλώδια: GND–GND, VCC–5V, SDA–D14 (PB9), SCL–D15 (PB8). Τα PB8/PB9 είναι FT_f (αντέχουν 5 V).
Στην εκκίνηση δείχνει 6 s δοκιμαστική οθόνη: η γραμμή 2 (A00) ή η 3 (A02) δείχνει σωστά ÅÄÖ åäö
→ `LCD_ROM` στο `app.c`.

## Ρελέ, LED, βομβητής, watchdog (βήμα O1)

Χωρίς CubeMX: τα pins ρυθμίζονται στο `app.c` (αφήστε τα «ελεύθερα» στο CubeMX).

| Pin | Arduino | Σύνδεση |
| --- | --- | --- |
| PB1 | A3 | IN1 ρελέ = K1 A-larm (όχι D3/PB3: τραβάει το ρελέ ~0,5 s μετά από reset) |
| PB5 | D4 | IN2 ρελέ = K2 B-larm |
| PB4 | D5 | IN3 ρελέ = K3 Fel (fail-safe) |
| PB14 | D6 | IN4 ρελέ = K4 C-larm |
| PA8 | D7 | βομβητής (μέσω NPN) |
| PC7 | D9 | LED κόκκινο A |
| PB0 | D10 | LED κίτρινο B/C |
| PA7 | D11 | LED κίτρινο Fel |
| PA6 | D12 | LED πράσινο ρεύμα |
| PA0 | A0 | LED πράσινο Drift |
| PA1 | A1 | LED κίτρινο Service |

Ρελέ: board 5 V με οπτοζεύκτες, active low· τα pins είναι open-drain (FT, αντέχουν 5 V).
LED: 330 Ω σε σειρά, κάθοδος στο GND. Watchdog (IWDG) 2 s.

Στο `main.c`, στο `USER CODE BEGIN Init` (αμέσως μετά το `HAL_Init()`): `app_early_init();`.
Έτσι τα ρελέ/LED παίρνουν τη θέση «off» μέσα σε λίγα μs μετά από reset, πριν από ρολόι, UART, I2C.
Όσο το STM32 είναι σε reset (κουμπί πατημένο, watchdog, προγραμματισμός) τα pins «αιωρούνται»:
γι' αυτό σε κάθε είσοδο IN του board ρελέ μπαίνει **αντίσταση 10 kΩ προς τα 5 V του board**
(κρατά το ρελέ σίγουρα off). Στην τελική πλακέτα: driver με pull-down στη βάση/gate, ώστε
«pin χωρίς οδήγηση = ρελέ off».

## Δοκιμές στο PC

```bash
cd firmware
CF="-std=c11 -Wall -Wextra -Werror -Icommon -Icontroller"
gcc $CF -o /tmp/test_modbus_rtu tests/test_modbus_rtu.c common/modbus_rtu.c && /tmp/test_modbus_rtu
gcc $CF -o /tmp/test_alarm tests/test_alarm.c controller/alarm.c && /tmp/test_alarm
gcc $CF -o /tmp/test_lcd tests/test_lcd.c controller/lcd.c controller/lcd_view.c controller/alarm.c && /tmp/test_lcd
gcc $CF -o /tmp/test_panel tests/test_panel.c controller/panel_io.c controller/alarm.c && /tmp/test_panel
gcc $CF -o /tmp/host_ctrl tests/host_ctrl.c common/modbus_rtu.c controller/ctrl_modules.c
gcc -std=gnu11 -Wall -Wextra -Werror -Itests/host_hal -Icommon -Icontroller -o /tmp/host_app \
    tests/host_hal/host_hal.c controller/app.c controller/alarm.c controller/ctrl_modules.c \
    controller/pi_link.c controller/lcd.c controller/lcd_view.c controller/panel_io.c \
    common/modbus_rtu.c common/cobs.c
cd .. && HOST_CTRL=/tmp/host_ctrl HOST_APP=/tmp/host_app python -m pytest tools/module_sim   # θέλει pyserial
```

Όλο το σύστημα σε ένα Linux PC χωρίς hardware: `python tools/module_sim/dev_bench.py /tmp/host_app`
και μετά το HMI με το `SPRINKLER_CONTROLLER=serial:/dev/pts/N` που τυπώνει.
