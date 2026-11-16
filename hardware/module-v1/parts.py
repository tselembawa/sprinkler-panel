"""
Module 4/4 PCB v1 (all THT): parts, nets and placement in one place.
gen.py turns this into module_v1.kicad_sch + module_v1.kicad_pcb.

Coordinates: mm on the board, (0, 0) = top left corner, board 100 x 100.
'at' is where the footprint's own origin (normally pad 1) goes, 'rot' in degrees (KiCad, CCW).
"""

BOARD_W, BOARD_H = 100.0, 100.0

# footprint names (KiCad standard libraries)
FP = {
    "R":      "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
    "Rv":     "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical",
    "RN9":    "Resistor_THT:R_Array_SIP9",
    "C":      "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm",
    "CP8":    "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm",
    "LED3":   "LED_THT:LED_D3.0mm",
    "LEDR":   "LED_THT:LED_Rectangular_W5.0mm_H2.0mm",
    "DO41":   "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal",
    "DO15":   "Diode_THT:D_DO-15_P12.70mm_Horizontal",
    "DO15v":  "Diode_THT:D_DO-15_P5.08mm_Vertical_KathodeUp",
    "PTC":    "Fuse:Fuse_BelFuse_0ZRE0025FF_L9.6mm_W3.8mm",
    "DCDC":   "Converter_DCDC:Converter_DCDC_RECOM_R-78E-0.5_THT",
    "DIP8":   "Package_DIP:DIP-8_W7.62mm_Socket",
    "DIP16":  "Package_DIP:DIP-16_W7.62mm_Socket",
    "SW8":    "Button_Switch_THT:SW_DIP_SPSTx08_Slide_9.78x22.5mm_W7.62mm_P2.54mm",
    "SOCK15": "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical",
    "H2":     "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
    "H4":     "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
    "TB3":    "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3-5.08_1x03_P5.08mm_Horizontal",
    "TB4":    "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-4-5.08_1x04_P5.08mm_Horizontal",
    "TB8":    "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-8-5.08_1x08_P5.08mm_Horizontal",
    "RELAY":  "Relay_THT:Relay_SPDT_SANYOU_SRD_Series_Form_C",
    "HOLE":   "MountingHole:MountingHole_3.2mm_M3",
}

# Nucleo-32 headers (UM2591 table 9, MB1455 schematic)
CN3 = {1: "D1/PB6", 2: "D0/PB7", 3: "NRST", 4: "GND", 5: "D2/PA15", 6: "D3/PB1", 7: "D4/PA10",
       8: "D5/PA9", 9: "D6/PB0", 10: "D7/PB2", 11: "D8/PB8", 12: "D9/PA8", 13: "D10/PB9",
       14: "D11/PB5", 15: "D12/PB4"}
CN4 = {1: "VIN", 2: "GND", 3: "NRST", 4: "+5V", 5: "A7/PA7", 6: "A6/PA6", 7: "A5/PA11",
       8: "A4/PA12", 9: "A3/PA5", 10: "A2/PA4", 11: "A1/PA1", 12: "A0/PA0", 13: "AREF",
       14: "+3V3", 15: "D13/PB3"}

NC = None   # pin left unconnected

# part: ref, value, symbol kind, footprint key, at, rot, {pad: net}, pin names (for boxes), block
PARTS = []


def part(ref, value, kind, fp, at, rot, pads, names=None, block="", note=""):
    PARTS.append(dict(ref=ref, value=value, kind=kind, fp=FP[fp], at=at, rot=rot, pads=pads,
                      names=names or {}, block=block, note=note))


# ---------------------------------------------------------------- bus + power ----------------
# terminals at the top edge, wire entry outwards (rot 180): pin 1 is the rightmost one
bus_names = {1: "24V", 2: "0V", 3: "A", 4: "B"}
part("X1", "BUS IN", "conn", "TB4", (27.0, 5.6), 180, {1: "VBUS", 2: "GND", 3: "RS485_A", 4: "RS485_B"},
     bus_names, "Bus")
part("X2", "BUS OUT", "conn", "TB4", (89.3, 5.6), 180, {1: "VBUS", 2: "GND", 3: "RS485_A", 4: "RS485_B"},
     bus_names, "Bus")

part("F1", "PTC 0.25A 60V (0ZRE0025FF)", "fuse", "PTC", (64.8, 15.2), 0, {1: "VBUS", 2: "VBUS_F"}, block="Power")
part("D1", "1N5819", "diode", "DO41", (84.0, 15.2), 180, {1: "V24", 2: "VBUS_F"}, block="Power")
part("C1", "47uF 50V", "cpol", "CP8", (89.0, 16.2), 0, {1: "V24", 2: "GND"}, block="Power")
part("D2", "P6KE33A", "diode", "DO15", (64.8, 21.6), 0, {1: "V24", 2: "GND"}, block="Power")
part("U1", "TSR 1-2450 (24V->5V)", "dcdc", "DCDC", (84.0, 28.2), 0, {1: "V24", 2: "GND", 3: "+5V"},
     {1: "VIN", 2: "GND", 3: "VOUT"}, "Power")
part("R25", "10k", "res", "Rv", (64.8, 27.4), 0, {1: "V24", 2: "LED_PWR"}, block="Power")
part("D17", "LED green (24V)", "led", "LED3", (70.4, 27.4), 0, {1: "GND", 2: "LED_PWR"}, block="Power")

# supply measurement 100k/10k -> A6
part("R3", "100k 1%", "res", "R", (27.9, 17.0), 0, {1: "V24", 2: "VSENSE"}, block="Supply sense")
part("R4", "10k 1%", "res", "R", (27.9, 20.6), 0, {1: "VSENSE", 2: "GND"}, block="Supply sense")
part("C5", "100nF", "cap", "C", (27.9, 24.2), 0, {1: "VSENSE", 2: "GND"}, block="Supply sense")

# ---------------------------------------------------------------- Nucleo ---------------------
# USB at the top, pin 1 of both headers at the top (next to the USB), CN3 left, CN4 right
# (checked on the board: ST pinout drawing, USB up).
cn3_nets = {1: "UART_TX", 2: "UART_RX", 3: NC, 4: "GND", 5: "DIP1", 6: "OUT1", 7: "EXP_D4",
            8: "EXP_D5", 9: "OUT2", 10: "DIP2", 11: "DIP3", 12: "OUT3", 13: "DIP4", 14: "OUT4",
            15: "DIP5"}
cn4_nets = {1: NC, 2: "GND", 3: NC, 4: "+5V", 5: "DIP7", 6: "VSENSE", 7: "RS485_DE", 8: "DIP8",
            9: "IN4_ADC", 10: "IN3_ADC", 11: "IN2_ADC", 12: "IN1_ADC", 13: NC, 14: "+3V3", 15: "DIP6"}
NUCLEO_X_CN3, NUCLEO_X_CN4, NUCLEO_PIN1_Y = 41.91, 57.15, 12.7
part("J2", "CN4 (Nucleo right)", "conn", "SOCK15", (NUCLEO_X_CN4, NUCLEO_PIN1_Y), 0, cn4_nets, CN4, "Nucleo")
part("J1", "CN3 (Nucleo left)", "conn", "SOCK15", (NUCLEO_X_CN3, NUCLEO_PIN1_Y), 0, cn3_nets, CN3, "Nucleo")
part("J4", "EXP (3V3 GND D4 D5)", "conn", "H4", (61.0, 40.6), 0,
     {1: "+3V3", 2: "GND", 3: "EXP_D4", 4: "EXP_D5"}, {1: "3V3", 2: "GND", 3: "D4", 4: "D5"}, "Nucleo")

# ---------------------------------------------------------------- RS485 ----------------------
part("U2", "MAX3485 (DIP-8)", "ic", "DIP8", (67.3, 42.0), 90,
     {1: "UART_RX", 2: "RS485_DE", 3: "RS485_DE", 4: "UART_TX", 5: "GND", 6: "RS485_A", 7: "RS485_B", 8: "+3V3"},
     {1: "RO", 2: "~RE", 3: "DE", 4: "DI", 5: "GND", 6: "A", 7: "B", 8: "VCC"}, "RS485")
part("C2", "100nF", "cap", "C", (78.0, 33.0), 0, {1: "+3V3", 2: "GND"}, block="RS485")
part("R2", "10k", "res", "R", (78.0, 36.6), 0, {1: "RS485_DE", 2: "GND"}, block="RS485")
part("R1", "120", "res", "R", (78.0, 40.2), 0, {1: "RS485_A", 2: "TERM"}, block="RS485")
part("J3", "TERM 120R", "conn", "H2", (91.4, 37.4), 0, {1: "TERM", 2: "RS485_B"},
     {1: "T", 2: "B"}, "RS485")
part("D3", "P6KE15CA", "tvs", "DO15v", (78.0, 44.4), 0, {1: "RS485_A", 2: "GND"}, block="RS485")
part("D4", "P6KE15CA", "tvs", "DO15v", (87.0, 44.4), 0, {1: "RS485_B", 2: "GND"}, block="RS485")

# ---------------------------------------------------------------- inputs ---------------------
# X3 on the left edge (rot 270: wire entry to the left), IN1+ at the top
in_nets = {}
for n in range(4):
    in_nets[2 * n + 1] = f"IN{n + 1}_LOOP"
    in_nets[2 * n + 2] = "GND"
part("X3", "INPUTS", "conn", "TB8", (5.2, 17.0), 270, in_nets,
     {1: "IN1+", 2: "IN1-", 3: "IN2+", 4: "IN2-", 5: "IN3+", 6: "IN3-", 7: "IN4+", 8: "IN4-"}, "Inputs")
for n in range(4):
    y = 17.0 + 10.16 * n
    part(f"R{5 + n}", "1k 1%", "res", "R", (13.4, y), 0, {1: "+3V3", 2: f"IN{n + 1}_LOOP"}, block="Inputs")
    part(f"R{9 + n}", "4k7", "res", "R", (13.4, y + 3.4), 0, {1: f"IN{n + 1}_LOOP", 2: f"IN{n + 1}_ADC"},
         block="Inputs")
    part(f"C{6 + n}", "100nF", "cap", "C", (13.4, y + 6.8), 0, {1: f"IN{n + 1}_ADC", 2: "GND"}, block="Inputs")

# ---------------------------------------------------------------- outputs --------------------
part("U3", "ULN2003A (DIP-16)", "ic", "DIP16", (36.0, 66.0), 90,
     {1: "OUT1", 2: "OUT2", 3: "OUT3", 4: "OUT4", 5: "GND", 6: "GND", 7: "GND", 8: "GND", 9: "V24",
      10: NC, 11: NC, 12: NC, 13: "K4_COIL", 14: "K3_COIL", 15: "K2_COIL", 16: "K1_COIL"},
     {1: "I1", 2: "I2", 3: "I3", 4: "I4", 5: "I5", 6: "I6", 7: "I7", 8: "GND", 9: "COM",
      10: "O7", 11: "O6", 12: "O5", 13: "O4", 14: "O3", 15: "O2", 16: "O1"}, "Outputs")
for k in range(4):
    x = 13.4 + 5.08 * k
    part(f"R{13 + k}", "1k", "res", "Rv", (x, 66.0), 90, {1: f"OUT{k + 1}", 2: f"LED_OUT{k + 1}"}, block="Outputs")
    part(f"D{5 + k}", "LED red 3mm", "led", "LED3", (x, 58.6), 0,
         {1: "GND", 2: f"LED_OUT{k + 1}"}, block="Outputs")

relay_names = {1: "COM", 2: "COIL", 3: "NO", 4: "NC", 5: "COIL"}
for k in range(4):
    x0 = 9.0 + 21.0 * k
    part(f"K{k + 1}", "SRD-24VDC-SL-C", "relay", "RELAY", (x0 + 1.55, 77.5), 0,
         {1: f"K{k + 1}_C", 2: "V24", 3: f"K{k + 1}_NO", 4: f"K{k + 1}_NC", 5: f"K{k + 1}_COIL"},
         relay_names, "Outputs")
    part(f"X{4 + k}", f"OUT{k + 1}", "conn", "TB3", (x0 + 10.05 - 5.08, 94.4), 0,
         {1: f"K{k + 1}_C", 2: f"K{k + 1}_NO", 3: f"K{k + 1}_NC"}, {1: "C", 2: "NO", 3: "NC"}, "Outputs")

# ---------------------------------------------------------------- DIP address ----------------
# switch k joins pad k (GND row) and pad 17-k (DIPk row); LEDs and the resistor array above
part("SW1", "DIP 8 (address)", "dip", "SW8", (68.6, 66.0), 90,
     {**{k: "GND" for k in range(1, 9)}, **{17 - k: f"DIP{k}" for k in range(1, 9)}},
     {**{k: f"{k}" for k in range(1, 9)}, **{17 - k: f"{k}'" for k in range(1, 9)}}, "Address")
part("RN1", "8x470R bussed (4609X-101-471)", "rnet", "RN9", (66.06, 48.6), 0,
     {1: "+3V3", **{k + 1: f"LED_DIP{k}" for k in range(1, 9)}},
     {1: "COM", **{k + 1: f"R{k}" for k in range(1, 9)}}, "Address")
for k in range(1, 9):
    x = 68.6 + 2.54 * (k - 1)
    y = 55.4                              # 2 mm wide LEDs, one per switch
    part(f"D{8 + k}", "LED yellow 2x5mm", "led", "LEDR", (x, y), 90,
         {1: f"DIP{k}", 2: f"LED_DIP{k}"}, block="Address")

# ---------------------------------------------------------------- mounting -------------------
for i, (x, y) in enumerate([(4, 4), (96, 4), (4, 96), (96, 96)]):
    part(f"H{i + 1}", "M3", "hole", "HOLE", (x, y), 0, {}, block="Mechanical")

POWER_NETS = {"VBUS", "VBUS_F", "V24", "GND", "+5V"} | {f"K{k}_{c}" for k in range(1, 5) for c in ("C", "NO", "NC")}
