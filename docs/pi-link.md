# Πρωτόκολλο Raspberry Pi ↔ Main Controller — v1

Υλοποίηση της ενότητας «Πρωτόκολλο Pi ↔ controller» της προδιαγραφής.
Κώδικας: `firmware/common/pi_link_proto.h` (C) και `hmi/app/link_proto.py` (Python) — οι δύο πλευρές
πρέπει να συμφωνούν byte προς byte.

## Φυσικό επίπεδο

- Πρωτότυπο: το USB καλώδιο του Nucleo (ST-LINK virtual COM port = USART2 του STM32). Στο Pi
  εμφανίζεται ως `/dev/ttyACM0`, στα Windows ως `COMx` («STLink Virtual COM Port»).
- Τελικό προϊόν: UART 3,3 V με digital isolator (GPIO14/15 του Pi).
- 115200 baud, 8N1.

## Πλαίσιο

| Πεδίο | Μέγεθος | Περιεχόμενο |
| --- | --- | --- |
| ver | 1 | 1 |
| type | 1 | τύπος μηνύματος |
| seq | 1 | 1–255 από το Pi· η απάντηση έχει τον ίδιο. 0 = μήνυμα του controller χωρίς ερώτηση (LOG) |
| flags | 1 | 0 (δεσμευμένο) |
| len | 2 | μήκος payload, little endian, ≤ 512 |
| payload | len | |
| crc | 2 | CRC-16/CCITT-FALSE (poly 0x1021, αρχή 0xFFFF) όλων των παραπάνω, little endian |

Όλο το πλαίσιο κωδικοποιείται με **COBS** και τελειώνει με **0x00**.
Όλοι οι αριθμοί μέσα στο payload είναι **little endian**.

## Κανόνες

- Το Pi ρωτάει, ο controller απαντάει (εκτός από LOG). Αναμονή απάντησης 300 ms, έως 3 επαναλήψεις
  με το **ίδιο seq**. Εντολές με seq που έχει ήδη εκτελεστεί δεν ξαναεκτελούνται: ο controller ξαναστέλνει
  την ίδια απάντηση.
- Λάθος → απάντηση **NACK (0x7F)** με τον ίδιο seq, payload: κωδικός (1 byte) + προαιρετικά στοιχεία.
- Ο controller δεν ακούει το Pi για 5 s → event **HMI_LOST**· ξανά επαφή → **HMI_BACK**.
  Μετά από 120 λεπτά χωρίς Pi → System Fault (K3), όχι σε service mode.

## Μηνύματα

| Κωδ. | Όνομα | Αίτημα | Απάντηση |
| --- | --- | --- | --- |
| 0x01 | PING | – | proto u8, fw_major u8, fw_minor u8, cfg_version u32, cfg_crc u32, uptime_s u32, last_seq u32 |
| 0x02 | GET_STATUS | – | βλ. «Status» |
| 0x04 | GET_POINTS | από index u16, max u8 | total u16, count u8, count × «Point» |
| 0x05 | GET_MODULES | – | count u8, count × «Module» |
| 0x06 | GET_EVENTS | από seq u32, max u8 | count u8, count × «Event» |
| 0x10 | CFG_BEGIN | πλήθος σημείων u16 | – |
| 0x11 | CFG_PUT | έως 9 εγγραφές «Point config» (52 bytes) | – |
| 0x12 | CFG_END | CRC-32 u32 | errors u16, έως 100 × (point_id u16, field u8, code u8) |
| 0x13 | CFG_ACTIVATE | user u8 | version u32, crc u32 |
| 0x14 | CFG_INFO | – | version u32, crc u32, points u16 |
| 0x20 | CMD_ACK | user u8 | πλήθος κβιταρισμένων u16 |
| 0x21 | CMD_SILENCE | user u8, on u8 | silenced u8 |
| 0x22 | CMD_RESET | user u8 | reset u16, remaining u16, έως 50 × point_id u16 |
| 0x23 | CMD_DISABLE | point_id u16, disabled u8, user u8 | enabled u8 |
| 0x25 | CMD_SERVICE | minutes u16 (0 = τέλος), user u8 | active u8 |
| 0x30 | SET_TIME | unix u32, user u8, auto u8 | time u32 |
| 0x40 | DEV_SET_KEY | tech u8 | – (μόνο στο πρωτότυπο, μέχρι τον κλειδοδιακόπτη) |
| 0x60 | LOG | (controller → Pi, seq 0) κείμενο ASCII | – |
| 0x7F | NACK | – | code u8 [+ στοιχεία] |

`SET_TIME` με auto = 1 περνάει χωρίς κλειδί μόνο όταν το ρολόι του controller δεν έχει οριστεί ακόμα
(το Pi δίνει την ώρα μετά από κάθε εκκίνηση του controller, μέχρι να μπει RTC με μπαταρία).

### Κωδικοί NACK

1 UNKNOWN_TYPE · 2 BAD_LENGTH · 3 ACCESS_DENIED · 4 BAD_DURATION · 5 UNKNOWN_POINT · 6 BAD_TIME ·
7 CFG_SEQUENCE · 8 CFG_TOO_MANY · 9 CFG_CRC_MISMATCH (+ CRC που υπολόγισε ο controller, u32) · 10 BAD_VERSION

### Status (GET_STATUS) — 43 bytes + λίστα

| Θέση | Τύπος | Πεδίο |
| --- | --- | --- |
| 0 | 6 × u16 | A, B, C, Drift, Fault, Disabled |
| 12 | u8 | relays_logical (bit 0 K1, 1 K2, 2 K3 OK, 3 K4) |
| 13 | u8 | relays (ό,τι είναι πραγματικά, μετά το service mode) |
| 14 | u8 | LED B/C: 0 off, 1 steady, 2 flash |
| 15 | u8 | σειρήνες: 0 off, 1 sounding, 2 silenced |
| 16 | u8 | bit 0 transmission_blocked, 1 silenced, 2 service, 3 key_tech, 4 buzzer, 5 LED A, 6 LED fault, 7 LED drift |
| 17 | u8 | bit 0 LED power (Drift), 1 LED service, 2 HMI lost |
| 18 | 6 × u32 | service_remaining_s, time, uptime_s, cfg_version, cfg_crc, last_seq |
| 42 | u8 | πλήθος χαμένων modules (≤ 16), ακολουθούν ζεύγη line u8, address u8 |

### Point (GET_POINTS) — 10 bytes

id u16, category u8 (0 A, 1 B, 2 DRIFT, 3 FAULT), state u8 (0 NORMAL, 1 PENDING, 2 ALARM, 3 ACKED,
4 LATCHED), acked u8, fault u8 (0 –, 1 OPEN, 2 SHORT, 3 MODULE_LOST), enabled u8, display u8
('A' 'B' 'C' 'D' 'F'), condition u8 (0 NORMAL, 1 ACTIVE, 2 OPEN, 3 SHORT), pending_left_s u8 (255 = –)

### Event (GET_EVENTS) — 21 bytes

seq u32, time u32, type u8, kind u8 (0 SYSTEM, 1 POINT, 2 MODULE), user u8, source_id u16, old i32, new i32.
Τύποι: 0 STARTUP, 1 CONFIG_ACTIVATED, 2 DELAY_STARTED, 3 DELAY_CANCELLED, 4 ALARM_ON, 5 ALARM_CLEARED,
6 LINE_OPEN, 7 LINE_SHORT, 8 LINE_OK, 9 MODULE_LOST, 10 MODULE_BACK, 11 ACK, 12 RESET, 13 SILENCE,
14 UNSILENCE, 15 RESOUND, 16 SERVICE_ON, 17 SERVICE_OFF, 18 POINT_DISABLED, 19 POINT_ENABLED,
20 TIME_SET, 21 HMI_LOST, 22 HMI_BACK.

### Module (GET_MODULES) — 12 bytes

address u8, line u8, state u8 (0 START, 1 RUN, 2 LOST, 3 REJECTED), inputs u16, status u16,
supply_mv u16, fw u16, fails u8

### Point config (CFG_PUT) — 52 bytes

id u16, line u8, module u8, channel u8, category u8, device_type u8 (σειρά του `DEVICE_TYPES`),
contact_nc u8, delay_s u8, enabled u8, section u16, lcd1[20], lcd2[20] (Latin-1, συμπληρωμένα με 0).
Το CRC-32 (zlib) της ρύθμισης υπολογίζεται πάνω σε αυτές τις εγγραφές με σειρά id — το ίδιο στο Pi και στον controller.

## Υλοποίηση στο πρωτότυπο

- Controller: `firmware/controller/pi_link.c` — RX με interrupt σε ring buffer, απαντήσεις από το `app_loop`.
  Τα modules ρωτιούνται ένα-ένα ανά πέρασμα του loop, ώστε το Pi να απαντιέται ανάμεσα στις
  (blocking) μεταφορές RS485.
- Pi: `hmi/app/serial_controller.py` (`SPRINKLER_CONTROLLER=serial[:PORT]`), ίδιες μέθοδοι με τον εικονικό controller.
- Αρίθμηση γεγονότων: ο controller ξεκινά από 0 σε κάθε εκκίνηση (μέχρι να μπει FRAM). Το HMI
  κρατά συνεχή αρίθμηση στη βάση του με offset και καταλαβαίνει την επανεκκίνηση από το
  ρολόι που δεν έχει οριστεί, το uptime που πήγε πίσω ή το seq που μίκρυνε.
- Μετά από επανεκκίνηση του controller το Pi ξαναστέλνει ώρα (SET_TIME auto) και ρύθμιση (αν το CRC διαφέρει).
  Οι προσωρινές καταστάσεις (Frånkopplad, service, κλειδί) χάνονται μέχρι να μπει FRAM.
- Το Pi δεν απαντά → το HMI δείχνει «Ingen kontakt med centralen» και **δεν** κάνει restart
  (ο systemd watchdog συνεχίζει να τρέφεται). Δοκιμάζει ξανά κάθε 1 s.
