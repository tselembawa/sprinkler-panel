# Sprinkler panel

Addressable sprinkler alarm control panel — Άγγελος & Ρομαίος.

Η πλήρης προδιαγραφή και το πρωτόκολλο βρίσκονται στο έγγραφο του project
(καρτέλες «Προδιαγραφή v2» και «Δεδομένα & πρωτόκολλο»).

## Δομή

```
hmi/                     Εφαρμογή Raspberry Pi (οθόνη αφής + web από laptop)
  app/protocol.py        Κοινοί ορισμοί: κατηγορίες, τύποι συσκευών, όρια, CRC
  app/sim_controller.py  Εικονικός Main Controller (ίδια λογική με το μελλοντικό firmware)
  app/controller_link.py Επιλογή controller: εικονικός τώρα, σειριακός (UART) αργότερα
  app/db.py              SQLite: larmpunkter, εικόνες, markers, ιστορικό
  app/main.py            Web server (Starlette) και API
  app/static/            UI στα σουηδικά (HTML/CSS/JavaScript)
  tests/                 Αυτόματα tests της λογικής των alarms
  scripts/               Αυτόματη εκκίνηση στο Pi (systemd)
```

## Τι κάνει η έκδοση 0.1

- Larmpunkter: δημιουργία, αλλαγή, διαγραφή από την οθόνη. Αποθηκεύονται **μόνο** αν ο
  controller επιβεβαιώσει την ίδια ρύθμιση (CRC). Λάθη εμφανίζονται με εξήγηση.
- Μηχανή καταστάσεων: Normal → Pending (delay ≤ 60 s) → Alarm → Kvitterat → Latched → Reset.
- Latching μόνο για pressostat και flödesvakt.
- C-larm: κβιταρισμένο B-larm με αιτία που παραμένει. Το K2 ελευθερώνεται, ενεργοποιείται το K4.
- Relays K1–K4 (K3 fail-safe), LED με αναβόσβημα / σταθερό, buzzer.
- Επιτήρηση γραμμής (Avbrott / Kortslutning) και χαμένο module → fel.
- Κλειδοδιακόπτης: χωρίς κλειδί μόνο Kvittera· με κλειδί Återställ, ρυθμίσεις, frånkoppling, markers.
- Grafik: ανέβασμα φωτογραφίας, markers με ένα πάτημα, αναβόσβημα σε alarm,
  αυτόματη εμφάνιση της φωτογραφίας όταν έρθει νέο alarm.
- Ιστορικό με αναζήτηση και εξαγωγή CSV.
- Simulator: «ανοίγεις» βάνες, ενεργοποιείς pressostat, κόβεις καλώδια, βγάζεις modules.

Δεν υπάρχουν ακόμα: κανόνες εξόδων / modules 4/4 εξόδων, Tysta larmdon, service mode,
κωδικός ADMIN, σύνδεση με πραγματικό controller μέσω UART.

## Εκτέλεση στο PC (WSL Ubuntu)

```bash
cd sprinkler-panel/hmi
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-dev.txt
python -m pytest -q                       # τα tests πρέπει να περνούν όλα
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Άνοιξε στον browser των Windows: http://localhost:8000

## Εγκατάσταση στο Raspberry Pi (5 ή, για δοκιμές, 3B)

Σε Raspberry Pi OS **με desktop**:

```bash
cd ~
git clone https://github.com/tselembawa/sprinkler-panel.git
cd sprinkler-panel
bash hmi/scripts/install-pi.sh
sudo reboot
```

Μετά την επανεκκίνηση η σελίδα του πίνακα ανοίγει μόνη της σε πλήρη οθόνη (kiosk).
Στο Pi 3 ο browser τρέχει αυτόματα χωρίς επιτάχυνση γραφικών, γιατί με αυτήν κρασάρει.

Έξοδος από το kiosk μέχρι το επόμενο reboot (Ctrl+Alt+T για terminal):
`pkill -f kiosk.sh; pkill -f -- --kiosk` · ξανά: `nohup ~/sprinkler-panel/hmi/scripts/kiosk.sh >/dev/null 2>&1 &`
Από laptop στο ίδιο δίκτυο: http://<όνομα-του-pi>.local:8000

Το `install-pi.sh` στήνει:

| | Τι κάνει |
| --- | --- |
| `sprinkler-hmi` | Ο server ξεκινά με το Pi και ξαναξεκινά αν πέσει. **Watchdog επιπέδου 1:** η εφαρμογή στέλνει «είμαι καλά» στο systemd μόνο όσο απαντούν ο controller και η βάση και τρέχει ο συγχρονισμός των events. Αν σταματήσει για 30 s, το systemd την ξαναξεκινά. |
| Kiosk (`kiosk.sh`) | Auto-login, Chromium σε kiosk mode στη σελίδα του πίνακα, χωρίς σβήσιμο οθόνης. Αν ο browser κλείσει, ξανανοίγει. |
| `sprinkler-kiosk-watchdog` | Αν η σελίδα «παγώσει» ή κρασάρει («Aw, Snap!») και σταματήσει να ρωτάει τον server για 30 s, ξεκινά ξανά ο browser (μετά περιμένει 90 s να φορτώσει). |
| Hardware watchdog | **Επίπεδο 2:** αν κολλήσει ολόκληρο το Linux, το chip του Pi κάνει reboot μόνο του σε περίπου 15 s. |

Έλεγχος υγείας: `curl -s localhost:8000/api/health`. Logs: `journalctl -u sprinkler-hmi -f`

Οι συναγερμοί δεν εξαρτώνται από τίποτα από τα παραπάνω: τους χειρίζεται ο Main Controller.

## Πρώτη δοκιμή (όπως στην προδιαγραφή)

1. Simulator → τσέκαρε **Nyckelbrytare i läge TEKNIKER**.
2. Larmpunkter → **+ Ny larmpunkt**: «Serviceventil Sektion 1», B-larm, NC → Spara.
3. Grafik → **+ Ny bild**, ανέβασε φωτογραφία, πάτα πάνω στη βάνα, διάλεξε την punkt → Placera → Klar.
4. Simulator → punkt 1 → **Aktiv**.
5. Αποτέλεσμα: η φωτογραφία ανοίγει μόνη της με τον marker να αναβοσβήνει, το K2 είναι aktiv,
   το κίτρινο LED αναβοσβήνει και η αλλαγή φαίνεται στις Händelser.
