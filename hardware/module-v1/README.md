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
3. **PCB Editor**: Tools → **Update PCB from Schematic** (F8). Αν θέλει να **προσθέσει** εξαρτήματα
   (αντί να βρει τα υπάρχοντα), τσέκαρε το **«Re-link footprints to schematic symbols based on their
   reference designators»** και μετά Update. Δεν πρέπει να αλλάξει σχεδόν τίποτα· αν βγάλει λάθη,
   στείλε μου screenshot.
4. **Δρομολόγηση με Freerouting**: Plugin and Content Manager → «Freerouting» → Install.
   Μετά στο PCB: Tools → External Plugins → Freerouting → Start. Περιμένει μερικά λεπτά.
5. Πάτα **B** (γεμίζει η γείωση στην κάτω πλευρά).
6. Inspect → **Design Rules Checker** → Run DRC. Στείλε μου screenshot με τα σφάλματα.
7. Μόνο όταν είναι καθαρό: File → Fabrication Outputs → Gerbers + Drill → zip → JLCPCB.

Οι διάδρομοι για 24 V, GND, 5 V και τις επαφές των ρελέ είναι 1 mm (net class «Power»), το 24 V
του bus από X1 σε X2 2 mm (net class «Bus»), τα σήματα 0,3 mm.

## Πριν την παραγγελία: έλεγχοι με το Nucleo στο χέρι

1. ~~Ποια πλευρά είναι το CN3~~: επιβεβαιωμένο. Με το USB **προς τα πάνω**: CN3 (D1, D0, RST, GND, D2…)
   **αριστερά**, CN4 (VIN, GND, RST, +5V, A7…) **δεξιά**, **pin 1 πάνω** δίπλα στο USB.
2. **Το jumper D2–GND** του Nucleo (CN3 pin 4–5) βγαίνει.
3. **Ρελέ SRD**: το KiCad λέει pin 3 = NO, pin 4 = NC. Με το πρώτο ρελέ στο χέρι, με πολύμετρο:
   ανάμεσα στο COM και στο NC πρέπει να **χτυπάει** (κλειστή) χωρίς τάση στο πηνίο.
4. **Πόσο προεξέχει το Nucleo**: μέτρησε από το κέντρο του pin 1 (D1) μέχρι την άκρη της πλακέτας
   του Nucleo προς το USB, και από το pin 15 (D12) μέχρι την άλλη άκρη. Στο PCB υπάρχουν 12 mm προς
   το USB και 9 mm προς την άλλη πλευρά (μετά είναι η βάση του ULN2003).
5. **A4 και A5 στο breadboard** (δεν έχουν δοκιμαστεί ακόμα): DIP8 μόνο του ON + restart → διεύθυνση 128·
   και το `MOD_DE_PIN` με RS485 module που έχει DE/RE, αν υπάρχει.

## Τι αγοράζεις επιπλέον (δεν είναι στο bom.csv)

- 2 × θηλυκή κεφαλή 1×15, 2,54 mm (για το Nucleo)
- βάση DIP-8 και βάση DIP-16 (για MAX3485 και ULN2003A)
- 1 jumper 2,54 mm (τερματισμός 120 Ω)
- 4 × βίδα M3 + αποστάτες

## Firmware

Στο `firmware/module/mod_app.c`: `#define MOD_BOARD_PCB_V1 1` (DE στο A5, μέτρηση 24 V στο A6).
Στο breadboard μένει `0`.
