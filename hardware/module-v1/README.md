# Module 4/4, PCB v1 (όλα THT, 100 × 100 mm)

Πλακέτα-βάση για το NUCLEO-G031K8: τροφοδοσία 24 V, RS485 (MAX3485), 4 είσοδοι EOL, 4 ρελέ
changeover, DIP διεύθυνσης με LED. Σχέδιο και αποφάσεις: [docs/module-pcb-v1.md](../../docs/module-pcb-v1.md).

| Αρχείο | Τι είναι |
| --- | --- |
| `module_v1.kicad_pro` | το project: αυτό ανοίγεις στο KiCad |
| `module_v1.kicad_sch` | σχηματικό (οι συνδέσεις γίνονται με ετικέτες ίδιου ονόματος) |
| `module_v1.kicad_pcb` | πλακέτα με τα εξαρτήματα **τοποθετημένα**, χωρίς διαδρόμους ακόμα |
| `bom.csv` | λίστα υλικών |
| `placement.png` | πώς είναι τοποθετημένα (προεπισκόπηση) |
| `parts.py`, `gen.py` | από εδώ βγαίνουν τα παραπάνω (`python gen.py <kicad-footprints>`) |

## Βήματα στο KiCad (8 ή 9)

1. Άνοιξε το `module_v1.kicad_pro`. Αν πει ότι η πλακέτα είναι σε παλιά μορφή και θα μετατραπεί: **OK**.
2. **Σχηματικό**: Inspect → **Electrical Rules Checker** → Run. Στείλε μου ό,τι βγάλει.
3. **PCB Editor**: Tools → **Update PCB from Schematic** (F8) → Update. Δεν πρέπει να αλλάξει
   σχεδόν τίποτα· αν βγάλει λάθη, στείλε μου screenshot.
4. **Δρομολόγηση με Freerouting**: Plugin and Content Manager → «Freerouting» → Install.
   Μετά στο PCB: Tools → External Plugins → Freerouting → Start. Περιμένει μερικά λεπτά.
5. Πάτα **B** (γεμίζει η γείωση στην κάτω πλευρά).
6. Inspect → **Design Rules Checker** → Run DRC. Στείλε μου screenshot με τα σφάλματα.
7. Μόνο όταν είναι καθαρό: File → Fabrication Outputs → Gerbers + Drill → zip → JLCPCB.

Οι διάδρομοι για 24 V, GND, 5 V και τις επαφές των ρελέ είναι 1 mm (net class «Power»),
τα σήματα 0,3 mm.

## Πριν την παραγγελία: 3 έλεγχοι με το Nucleo στο χέρι

1. **Ποια πλευρά είναι το CN3**: κοιτώντας το Nucleo από πάνω με το USB **προς τα πάνω**, πρέπει
   η σειρά με τα **D1, D0, RST, GND, D2…** να είναι **δεξιά** και η σειρά με **VIN, GND, RST, +5V, A7…**
   **αριστερά**, με το **pin 1 (D1 / VIN) κάτω** (μακριά από το USB). Αν είναι αλλιώς, πες μου.
2. **Το jumper D2–GND** του Nucleo (CN3 pin 4–5) βγαίνει.
3. **Ρελέ SRD**: το KiCad λέει pin 3 = NO, pin 4 = NC. Με το πρώτο ρελέ στο χέρι, με πολύμετρο:
   ανάμεσα στο COM και στο NC πρέπει να **χτυπάει** (κλειστή) χωρίς τάση στο πηνίο.

## Τι αγοράζεις επιπλέον (δεν είναι στο bom.csv)

- 2 × θηλυκή κεφαλή 1×15, 2,54 mm (για το Nucleo)
- βάση DIP-8 και βάση DIP-16 (για MAX3485 και ULN2003A)
- 1 jumper 2,54 mm (τερματισμός 120 Ω)
- 4 × βίδα M3 + αποστάτες

## Firmware

Στο `firmware/module/mod_app.c`: `#define MOD_BOARD_PCB_V1 1` (DE στο A5, μέτρηση 24 V στο A6).
Στο breadboard μένει `0`.
