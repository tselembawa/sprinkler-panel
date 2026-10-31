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

## Εγκατάσταση στο Raspberry Pi 5

```bash
cd ~
git clone https://github.com/tselembawa/sprinkler-panel.git
cd sprinkler-panel/hmi
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Από το PC ή το laptop: http://sprinkler-pi.local:8000

Αυτόματη εκκίνηση με το Pi: δες τις οδηγίες μέσα στο `hmi/scripts/sprinkler-hmi.service`
(άλλαξε το `pi` αν ο χρήστης σου έχει άλλο όνομα).

Τα δεδομένα (βάση και φωτογραφίες) αποθηκεύονται στο `hmi/data/`. Δεν ανεβαίνουν στο Git.

## Πρώτη δοκιμή (όπως στην προδιαγραφή)

1. Simulator → τσέκαρε **Nyckelbrytare i läge TEKNIKER**.
2. Larmpunkter → **+ Ny larmpunkt**: «Serviceventil Sektion 1», B-larm, NC → Spara.
3. Grafik → **+ Ny bild**, ανέβασε φωτογραφία, πάτα πάνω στη βάνα, διάλεξε την punkt → Placera → Klar.
4. Simulator → punkt 1 → **Aktiv**.
5. Αποτέλεσμα: η φωτογραφία ανοίγει μόνη της με τον marker να αναβοσβήνει, το K2 είναι aktiv,
   το κίτρινο LED αναβοσβήνει και η αλλαγή φαίνεται στις Händelser.
