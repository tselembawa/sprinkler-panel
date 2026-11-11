# Main Controller – pins (NUCLEO-G0B1RE, STM32G0B1RET6, LQFP64)

Το chip έχει **60 I/O** (PA0–15, PB0–15, PC0–15, PD0–6/8/9, PF0–2).
**19** τα χρησιμοποιούμε, **7** είναι δεσμευμένα από την πλακέτα, **34** είναι ελεύθερα.

## Σε χρήση (19)

| # | Pin | Nucleo | Χρήση | Ρύθμιση |
| --- | --- | --- | --- | --- |
| 1 | PA2 | (ST-LINK) | USART2 TX → Raspberry Pi | CubeMX |
| 2 | PA3 | (ST-LINK) | USART2 RX ← Raspberry Pi (interrupt) | CubeMX |
| 3 | PC4 | D1 | USART1 TX → XY-485 (RS485 line 1) | CubeMX |
| 4 | PC5 | D0 | USART1 RX ← XY-485 | CubeMX |
| 5 | PB8 | D15 | I2C1 SCL → LCD 20×4 | CubeMX |
| 6 | PB9 | D14 | I2C1 SDA ↔ LCD 20×4 | CubeMX |
| 7 | PB1 | A3 | Ρελέ K1 A-larm (open-drain, active low) | app.c |
| 8 | PB5 | D4 | Ρελέ K2 B-larm | app.c |
| 9 | PB4 | D5 | Ρελέ K3 Fel (fail-safe) | app.c |
| 10 | PB14 | D6 | Ρελέ K4 C-larm | app.c |
| 11 | PA8 | D7 | Βομβητής (μέσω NPN) | app.c |
| 12 | PC7 | D9 | LED κόκκινο A-larm | app.c |
| 13 | PB0 | D10 | LED κίτρινο B/C-larm | app.c |
| 14 | PA7 | D11 | LED κίτρινο Fel | app.c |
| 15 | PA6 | D12 | LED πράσινο Drift (ρεύμα / λειτουργία) | app.c |
| 16 | PA0 | A0 | LED πράσινο Driftindikering | app.c |
| 17 | PA1 | A1 | LED κίτρινο Service | app.c |
| 18 | PA5 | D13 | LED LD4 της πλακέτας (αναβοσβήνει σε κάθε κύκλο ερωτήσεων) | CubeMX |
| 19 | PC13 | κουμπί B1 | Μπλε κουμπί = Kvittera | CubeMX |

## Δεσμευμένα από την πλακέτα (7)

| Pin | Γιατί |
| --- | --- |
| PA13, PA14 | SWD: ο προγραμματιστής/debugger (ST-LINK) |
| PF2 | NRST: το κουμπί RESET |
| PC14, PC15 | κρύσταλλος 32,768 kHz (LSE) – χρήσιμος για ρολόι RTC αργότερα |
| PF0, PF1 | είσοδος ρολογιού HSE (OSC_IN/OUT) |

## Ελεύθερα (34)

| Θύρα | Pins | Στα headers Arduino |
| --- | --- | --- |
| A (6) | PA4, PA9, PA10, PA11, PA12, PA15 | PA4 = A2, PA9 = D8, PA10 = D2 |
| B (9) | PB2, PB3, PB6, PB7, PB10, PB11, PB12, PB13, PB15 | PB3 = D3, PB11 = A4, PB12 = A5 |
| C (10) | PC0, PC1, PC2, PC3, PC6, PC8, PC9, PC10, PC11, PC12 | – |
| D (9) | PD0, PD1, PD2, PD3, PD4, PD5, PD6, PD8, PD9 | – |

Στα headers Arduino μένουν ελεύθερα μόνο **D2, D3, D8, A2, A4, A5**· τα υπόλοιπα είναι στις
εξωτερικές διπλές σειρές (morpho CN7/CN10).

Σημειώσεις:
- PB3 (D3): ελεύθερο ξανά (το K1 πήγε στο PB1/A3). Το «άναμμα του K1 μετά από reset» δεν ήταν
  το pin: ήταν η ενσωματωμένη δοκιμαστική ρύθμιση, που τώρα την αντικατέστησε η ρύθμιση στο flash.
- PA9/PA10 (και PA8): μπορεί να έχουν pull-down μετά από reset (σημείωση 4 του datasheet) – καλύτερα για εξόδους.
- PA11/PA12 είναι USB D−/D+ αν κάποτε θέλουμε USB στον controller.

## Τι θα χρειαστούμε ακόμα (σχέδιο)

| Τι | Pins |
| --- | --- |
| Κουμπιά πίνακα: Kvittera, Tysta, Återställ + κλειδοδιακόπτης | 4 είσοδοι |
| RS485 lines 2–4 (USART3/4/5 ή LPUART) | 6 (TX/RX × 3) |
| FRAM + RTC | 0 αν μπουν στο ίδιο I2C με την LCD, 4 αν σε SPI |
| Επιτήρηση τροφοδοσίας (δίκτυο, μπαταρία, γείωση) | 3–4 αναλογικές είσοδοι |

Χωράνε άνετα στα 34 ελεύθερα.
