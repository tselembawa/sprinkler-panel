# Module 4/4: πρωτότυπο με NUCLEO-G031K8 (βήμα M1)

Το πρώτο πραγματικό I/O module στο breadboard: 4 επιτηρούμενες είσοδοι, 4 έξοδοι (LED),
διεύθυνση από DIP, Modbus RTU πάνω από RS485 ακριβώς όπως ο προσομοιωτής (χάρτης v1,
[modbus-map.md](modbus-map.md)). Ο controller δεν αλλάζει: για εκείνον είναι ένα module ακόμα.

## Τι κάνει

- **Είσοδοι IN1–IN4:** μετρά την αντίσταση του βρόχου (EOL 1 kΩ σε σειρά + 1 kΩ παράλληλα στην
  επαφή) κάθε 5 ms και βγάζει CLOSED (≈ 1 kΩ), OPEN (≈ 2 kΩ), AVBROTT (κομμένο), KORTSLUTNING.
  Φίλτρο 100 ms (ρυθμίζεται από τον controller): ένα «τσίμπημα» μικρότερο δεν μετράει.
- **Έξοδοι OUT1–OUT4:** ON / FLASH 1 Hz / FLASH 2 Hz / PULSE, όπως τις ζητά ο controller.
  Στο breadboard είναι LED, χωρίς επιτήρηση γραμμής (το λέει στον controller: NOT_SUPERVISED).
- **Χωρίς controller > 10 s:** failsafe· οι έξοδοι κρατούν την κατάστασή τους (HOLD).
- **LED LD3 της πλακέτας:** σύντομο αναβόσβημα κάθε 2 s = όλα καλά · αργό = δεν το ρωτάει ο
  controller · πολύ γρήγορο = δεν έχει διεύθυνση (όλα τα DIP OFF) · γρήγορο = «Hitta modul».
- **Μηνύματα** στο PuTTY (θύρα του ST-LINK, 115200): διεύθυνση, σειριακός, αλλαγές εξόδων.

## Καλωδίωση

| Τι | Pin Nucleo | STM32 | Σύνδεση |
| --- | --- | --- | --- |
| RS485 RO (RXD) | D0 | PB7 | προς RX του Nucleo |
| RS485 DI (TXD) | D1 | PB6 | από TX του Nucleo |
| RS485 VCC / GND | 3V3 ή 5V / GND | | όπως στον controller |
| IN1 / IN2 / IN3 / IN4 | A0 / A1 / A2 / A3 | PA0 / PA1 / PA4 / PA5 | βλ. «Είσοδος» |
| OUT1 / OUT2 / OUT3 / OUT4 | D3 / D6 / D9 / D11 | PB1 / PB0 / PA8 / PB5 | LED + 330 Ω προς GND |
| DIP 1 / 2 / 3 / 4 | D2 / D7 / D8 / D10 | PA15 / PB2 / PB8 / PB9 | άλλη άκρη όλων στο GND |
| DIP 5 / 6 / 7 / 8 | D12 / D13 / A7 / A4 | PB4 / PB3 / PA7 / PA12 | |
| Τάση 24 V (αργότερα) | A6 | PA6 | 100 kΩ / 10 kΩ (στο firmware: `MOD_SUPPLY_RTOP`) |

Αν δεν υπάρχει επικοινωνία, **αλλάξτε μεταξύ τους D0 και D1**: τα εγχειρίδια της ST δεν
συμφωνούν σε ποιο είναι το TX. Αν το RS485 module έχει ακροδέκτες **DE/RE** (MAX485),
ενώστε τους και πηγαίνετέ τους στο **A5 (PA11)**, και στο `mod_app.c` βγάλτε το σχόλιο από
το `#define MOD_DE_PIN`.

### Είσοδος (για κάθε IN)

```
3V3 ──[1 kΩ]──┬── A0 (IN1)
              │
              └──[1 kΩ σε σειρά]──┬──────────┐
                                  │          │
                               επαφή     [1 kΩ] (παράλληλα)
                                  │          │
GND ──────────────────────────────┴──────────┘
```

| Μέτρηση | Αντίσταση βρόχου | Τάση στο pin | Αποτέλεσμα |
| --- | --- | --- | --- |
| Επαφή κλειστή | 1 kΩ | 1,65 V | CLOSED |
| Επαφή ανοιχτή | 2 kΩ | 2,2 V | OPEN |
| Καλώδιο κομμένο | ∞ | 3,3 V | LINE_OPEN (Avbrott) |
| Βραχυκύκλωμα | 0 | 0 V | LINE_SHORT (Kortslutning) |

Όρια: < 400 Ω βραχυκύκλωμα · 400–1499 κλειστή · 1500–3999 ανοιχτή · ≥ 8000 κομμένο ·
ενδιάμεσα «σφάλμα μέτρησης». Για δοκιμή χωρίς διακόπτη: η επαφή είναι ένα καλώδιο που
βάζεις και βγάζεις· κόψε το καλώδιο του βρόχου για Avbrott, βραχυκύκλωσε το pin στο GND για Kortslutning.

### Διεύθυνση (DIP)

ON = προς GND. Η διεύθυνση είναι το άθροισμα: DIP1 = 1, DIP2 = 2, DIP3 = 4, DIP4 = 8,
DIP5 = 16, DIP6 = 32, DIP7 = 64, DIP8 = 128. Γραμμή 1: 1–60. Παράδειγμα: **module 1 → μόνο DIP1 ON**,
module 2 → μόνο DIP2 ON, module 3 → DIP1 + DIP2. Διαβάζεται στην εκκίνηση· αλλαγή μετά
φαίνεται ως DIP_CHANGED και ισχύει μετά από reset.

## STM32CubeIDE: νέο project «Module»

1. File → New → STM32 Project → **Board Selector: NUCLEO-G031K8** → όνομα `Module`.
   «Initialize all peripherals with their default mode?» → Yes.
2. Pinout: **PB6 → USART1_TX**, **PB7 → USART1_RX**. Connectivity → USART1 → Mode **Asynchronous**,
   Parameter Settings **38400, 8 bits, None, 1**. NVIC Settings: **USART1 global interrupt ✔**.
3. USART2 (ST-LINK) μένει όπως είναι (115200).
4. Τα υπόλοιπα pins του πίνακα πιο πάνω να μένουν **ελεύθερα** (τα ρυθμίζει το firmware).
5. Generate Code.
6. Αρχεία: στο `Core/Src` τα `mod_app.c`, `mod_core.c`, `modbus_rtu.c` · στο `Core/Inc` τα
   `mod_app.h`, `mod_core.h`, `modbus_map.h`, `modbus_rtu.h`.
7. `main.c`: `USER CODE BEGIN Includes` → `#include "mod_app.h"` · `USER CODE BEGIN 2` →
   `mod_app_init();` · `USER CODE BEGIN 3` → `mod_app_loop();`
8. Build → Run.

Αν το μαύρο κουμπί reset δεν κάνει τίποτα: σε μερικές πλακέτες G031 το pin NRST είναι ρυθμισμένο
ως GPIO από το εργοστάσιο (option byte NRST_MODE). Το Run από το CubeIDE δουλεύει κανονικά.

## Δοκιμή με τον πίνακα

1. Το πραγματικό module σε μια διεύθυνση που έχει σημεία στη ρύθμιση (π.χ. 1), ο προσομοιωτής
   (menu.bat, επιλογή 2) **μόνο** για τις άλλες (π.χ. 2). Και τα δύο στο ίδιο ζεύγος A/B.
2. Στο HMI → **Moduler**: το module 1 με «1.0», τάση 24.0 V (δεν μετριέται ακόμα), οι 4 είσοδοι.
3. Βγάλε/βάλε το καλώδιο της επαφής: B-larm / A-larm. Κόψε τον βρόχο: Fel Avbrott.
4. Σειρήνα: αν το module είναι το L1-M02, το OUT1 (D3) ανάβει με τις σειρήνες.

## Δοκιμές στο PC

```bash
cd firmware
gcc -std=c11 -Wall -Wextra -Werror -Icommon -Imodule -o /tmp/test_module \
    tests/test_module.c module/mod_core.c common/modbus_rtu.c && /tmp/test_module
gcc -std=gnu11 -Wall -Wextra -Werror -Icommon -Imodule -o /tmp/host_module \
    tests/host_module.c module/mod_core.c common/modbus_rtu.c
cd .. && HOST_APP=/tmp/host_app HOST_MODULE=/tmp/host_module python -m pytest tools/module_sim/test_module_e2e.py
```

Το δεύτερο τρέχει τον **πραγματικό κώδικα του controller** με τον **πραγματικό κώδικα του module**
(χωρίς τον Python προσομοιωτή): ONLINE, συναγερμοί, Avbrott/Kortslutning, φίλτρο, χαμηλή τάση,
MODULE_LOST και επιστροφή.
