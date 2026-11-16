"""
Generates the KiCad project for the module PCB v1 from parts.py:
    module_v1.kicad_pro, module_v1.kicad_sch (KiCad 8 format), module_v1.kicad_pcb (KiCad 5 format,
    KiCad 8/9 opens and converts it), placement.png (preview), and checks courtyards.

    python gen.py <path to a kicad-footprints checkout>
The footprints are copied from the library into the board; the schematic uses its own simple symbols.
"""
import math
import os
import re
import sys
import uuid

from parts import BOARD_W, BOARD_H, PARTS, POWER_NETS, BUS_NETS

for _p in PARTS:
    _p["pads"] = {str(k): v for k, v in _p["pads"].items()}
    _p["names"] = {str(k): v for k, v in _p["names"].items()}

HERE = os.path.dirname(os.path.abspath(__file__))
OX, OY = 50.0, 50.0                         # board position on the KiCad page
NS = uuid.UUID("6f1c3a2e-6d53-4f6b-9a55-1b2f0c9e7a10")


def stamp(ref):
    """8 hex digits: footprint timestamp in the board, last part of the symbol uuid in the schematic"""
    return uuid.uuid5(NS, "stamp/" + ref).hex[:8].upper()


def sym_uuid(ref):
    return "00000000-0000-0000-0000-0000" + stamp(ref).lower()


def uid(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))


# ---------------------------------------------------------------- footprints -----------------
# courtyard of the 2 mm LED trimmed to its body (+0.25) so eight of them fit at 2.54 mm
FP_PATCH = {"LED_THT:LED_Rectangular_W5.0mm_H2.0mm": [("-1.35)", "-1.25)"), (" 1.35)", " 1.25)")]}


def load_fp(lib_root, fp_id):
    lib, name = fp_id.split(":")
    with open(os.path.join(lib_root, lib + ".pretty", name + ".kicad_mod")) as f:
        text = f.read()
    for a, b in FP_PATCH.get(fp_id, []):
        text = "\n".join(ln.replace(a, b) if "F.CrtYd" in ln else ln for ln in text.split("\n"))
    return text


def rot(x, y, deg):
    a = math.radians(deg)
    return x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a)


def fp_geometry(text):
    """pads [(num, x, y, w, h, shape)] and courtyard boxes/circles in footprint coordinates."""
    pads = []
    for m in re.finditer(r"\(pad (\S+) (\S+) (\S+) \(at ([-\d.]+) ([-\d.]+)(?: [-\d.]+)?\) \(size ([-\d.]+) ([-\d.]+)\)", text):
        num, _, shape, x, y, w, h = m.groups()
        pads.append((num.strip('"'), float(x), float(y), float(w), float(h), shape))
    pts = []
    for m in re.finditer(r"\(fp_line \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(layer F\.CrtYd\)", text):
        x1, y1, x2, y2 = map(float, m.groups())
        pts += [(x1, y1), (x2, y2)]
    for m in re.finditer(r"\(fp_circle \(center ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(layer F\.CrtYd\)", text):
        cx, cy, ex, ey = map(float, m.groups())
        r = math.hypot(ex - cx, ey - cy)
        pts += [(cx + r * math.cos(t / 8 * math.pi), cy + r * math.sin(t / 8 * math.pi)) for t in range(16)]
    return pads, pts


def place_fp(text, p, nets):
    """KiCad 5 module text placed at the part position with nets on the pads."""
    x, y = p["at"]
    r = p["rot"]
    lines = text.strip().splitlines()
    head = re.match(r"\(module (\S+) (.*)", lines[0])
    out = [f"  (module {p['fp']} {head.group(2)}".rstrip() + f" (tstamp {stamp(p['ref'])})"]
    out.append(f"    (at {OX + x:.3f} {OY + y:.3f}{' ' + str(r) if r else ''})")
    out.append(f"    (path /{stamp(p['ref'])})")
    for ln in lines[1:]:
        ln = ln.replace("REF**", p["ref"])
        if ln.strip().startswith("(fp_text value"):
            ln = re.sub(r"\(fp_text value (\"[^\"]*\"|\S+)", '(fp_text value "%s"' % p["value"].replace('"', ""), ln)
        if r:   # child angles are absolute in KiCad 5 files
            def add_angle(m):
                a = (float(m.group(3) or 0) + r) % 360
                return f"(at {m.group(1)} {m.group(2)}{' %g' % a if a else ''})"
            if re.match(r"\s*\((pad|fp_text)", ln):
                ln = re.sub(r"\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)", add_angle, ln, count=1)
        m = re.match(r"(\s*\(pad (\S+) .*)\)\s*$", ln)
        if m and nets.get(m.group(2).strip('"')):
            n = nets[m.group(2).strip('"')]
            ln = f"{m.group(1)} (net {NET_ID[n]} {q(n)}))"
        out.append("  " + ln)
    return "\n".join(out)


def q(s):
    return '"%s"' % s if re.search(r"[\s()+~/]", s) or not s else s


# ---------------------------------------------------------------- nets -----------------------
ALL_NETS = sorted({n for p in PARTS for n in p["pads"].values() if n})
ALL_NETS.remove("GND")
ALL_NETS.insert(0, "GND")
NET_ID = {n: i + 1 for i, n in enumerate(ALL_NETS)}


def check_nets():
    pins = {}
    for p in PARTS:
        for pad, n in p["pads"].items():
            if n:
                pins.setdefault(n, []).append(f"{p['ref']}.{pad}")
    single = {n: v for n, v in pins.items() if len(v) < 2}
    return pins, single


# ---------------------------------------------------------------- PCB ------------------------
SILK = []


def write_pcb(lib_root):
    mods, boxes, render = [], [], []
    for p in PARTS:
        text = load_fp(lib_root, p["fp"])
        pads, pts = fp_geometry(text)
        x, y = p["at"]
        wpts = [(x + a, y + b) for a, b in (rot(px, py, p["rot"]) for px, py in pts)]
        xs, ys = [a for a, _ in wpts], [b for _, b in wpts]
        boxes.append((p["ref"], min(xs), min(ys), max(xs), max(ys)))
        wpads = []
        for num, px, py, w, h, shape in pads:
            a, b = rot(px, py, p["rot"])
            if p["rot"] % 180:
                w, h = h, w
            wpads.append((num, x + a, y + b, w, h, shape, p["pads"].get(num)))
        render.append((p, wpads))
        for num, *_ in pads:
            if num and num not in p["pads"] and p["kind"] != "hole":
                raise SystemExit(f"{p['ref']}: pad {num} not in parts.py")
        mods.append(place_fp(text, p, p["pads"]))

    w, h = BOARD_W, BOARD_H
    edge = [(0, 0, w, 0), (w, 0, w, h), (w, h, 0, h), (0, h, 0, 0)]
    gr = [f"  (gr_line (start {OX + a} {OY + b}) (end {OX + c} {OY + d}) (layer Edge.Cuts) (width 0.1))"
          for a, b, c, d in edge]
    texts = [("SPRINKLER I/O 4/4 v1", 50, 99.0, "B.SilkS", 1.2), ("USB", 49.5, 3.2, "F.SilkS", 1.2),
             ("NUCLEO-G031K8", 49.5, 30.0, "F.SilkS", 1.2), ("D1", 38.7, 12.7, "F.SilkS", 0.9),
             ("VIN", 60.9, 12.7, "F.SilkS", 0.9), ("DIP: ON = 1", 79.0, 68.45, "F.SilkS", 0.9),
             ("TERM", 91.4, 34.9, "F.SilkS", 0.9), ("BUS IN", 19.4, 13.6, "F.SilkS", 0.8),
             ("BUS OUT", 81.7, 12.9, "B.SilkS", 0.8)]
    for x0 in (27.0, 89.3):                                   # bus terminals, pin 1 on the right
        for i, name in enumerate(("24V", "0V", "A", "B")):
            texts.append((name, x0 - 5.08 * i, 12.0, "F.SilkS", 0.8))
    for n in range(4):                                        # inputs
        texts.append((f"IN{n + 1}", 13.2, 17.0 + 10.16 * n, "F.SilkS", 0.8))
        texts.append(("0V", 13.2, 22.08 + 10.16 * n, "F.SilkS", 0.8))
    for k in range(4):                                        # relay terminals
        x0 = 9.0 + 21.0 * k + 10.05 - 5.08
        for i, name in enumerate(("C", "NO", "NC")):
            texts.append((name, x0 + 5.08 * i, 87.2, "F.SilkS", 0.9))
        if k < 3:
            texts.append(("30V 2A", x0 + 15.6, 87.2, "F.SilkS", 0.8))
    SILK[:] = [t for t in texts if t[3] == 'F.SilkS']
    for s, a, b, layer, size in texts:
        mirror = " (justify mirror)" if layer.startswith("B") else ""
        gr.append(f'  (gr_text "{s}" (at {OX + a} {OY + b}) (layer {layer}) '
                  f'(effects (font (size {size} {size}) (thickness {0.15 if size < 1.2 else 0.2})){mirror}))')
    # Nucleo outline on the silkscreen so it goes in the right way round
    nx0, nx1, ny0, ny1 = 40.5, 58.6, 6.0, 53.5
    for a, b, c, d in [(nx0, ny0, nx1, ny0), (nx1, ny0, nx1, ny1), (nx1, ny1, nx0, ny1), (nx0, ny1, nx0, ny0)]:
        gr.append(f"  (gr_line (start {OX + a} {OY + b}) (end {OX + c} {OY + d}) (layer F.SilkS) (width 0.15))")

    nets = "\n".join(f"  (net {NET_ID[n]} {q(n)})" for n in ALL_NETS)
    sig = [n for n in ALL_NETS if n not in POWER_NETS and n not in BUS_NETS]
    bus = [n for n in ALL_NETS if n in BUS_NETS]
    pwr = [n for n in ALL_NETS if n in POWER_NETS]
    netclass = (
        '  (net_class Default "Signals"\n    (clearance 0.25) (trace_width 0.3) (via_dia 0.8) (via_drill 0.4)\n'
        '    (uvia_dia 0.3) (uvia_drill 0.1)\n' + "".join(f"    (add_net {q(n)})\n" for n in sig) + "  )\n"
        '  (net_class Power "24V, 5V, GND and relay contacts"\n    (clearance 0.4) (trace_width 1.0) (via_dia 1.2) (via_drill 0.6)\n'
        '    (uvia_dia 0.3) (uvia_drill 0.1)\n' + "".join(f"    (add_net {q(n)})\n" for n in pwr) + "  )\n"
        '  (net_class Bus "24 V through to the next module"\n    (clearance 0.4) (trace_width 2.0) (via_dia 1.6) (via_drill 0.8)\n'
        '    (uvia_dia 0.3) (uvia_drill 0.1)\n' + "".join(f"    (add_net {q(n)})\n" for n in bus) + "  )")
    zone = (f"  (zone (net {NET_ID['GND']}) (net_name GND) (layer B.Cu) (tstamp 0) (hatch edge 0.508)\n"
            "    (connect_pads (clearance 0.4)) (min_thickness 0.254)\n"
            "    (fill (arc_segments 32) (thermal_gap 0.5) (thermal_bridge_width 0.6))\n"
            f"    (polygon (pts (xy {OX + 0.5} {OY + 0.5}) (xy {OX + w - 0.5} {OY + 0.5}) "
            f"(xy {OX + w - 0.5} {OY + h - 0.5}) (xy {OX + 0.5} {OY + h - 0.5}))))")
    pcb = f"""(kicad_pcb (version 20171130) (host pcbnew "5.1.6")
  (general (thickness 1.6) (drawings {len(gr)}) (tracks 0) (zones 1) (modules {len(mods)}) (nets {len(ALL_NETS) + 1}))
  (page A4)
  (title_block (title "Sprinkler I/O module 4/4") (rev "v1") (comment 1 "Carrier for NUCLEO-G031K8, all THT"))
  (layers
    (0 F.Cu signal) (31 B.Cu signal) (32 B.Adhes user) (33 F.Adhes user) (34 B.Paste user)
    (35 F.Paste user) (36 B.SilkS user) (37 F.SilkS user) (38 B.Mask user) (39 F.Mask user)
    (40 Dwgs.User user) (41 Cmts.User user) (42 Eco1.User user) (43 Eco2.User user)
    (44 Edge.Cuts user) (45 Margin user) (46 B.CrtYd user) (47 F.CrtYd user) (48 B.Fab user) (49 F.Fab user)
  )
  (setup
    (last_trace_width 0.3) (trace_clearance 0.25) (zone_clearance 0.4) (zone_45_only no)
    (trace_min 0.2) (via_size 0.8) (via_drill 0.4) (via_min_size 0.4) (via_min_drill 0.3)
    (uvia_size 0.3) (uvia_drill 0.1) (uvias_allowed no) (uvia_min_size 0.2) (uvia_min_drill 0.1)
    (edge_width 0.1) (segment_width 0.2) (pcb_text_width 0.3) (pcb_text_size 1.5 1.5)
    (mod_edge_width 0.12) (mod_text_size 1 1) (mod_text_width 0.15) (pad_size 1.524 1.524)
    (pad_drill 0.762) (pad_to_mask_clearance 0.05) (aux_axis_origin 0 0) (visible_elements FFFFFF7F)
    (pcbplotparams
      (layerselection 0x010fc_ffffffff) (usegerberextensions false) (usegerberattributes true)
      (usegerberadvancedattributes true) (creategerberjobfile true) (excludeedgelayer true)
      (linewidth 0.100000) (plotframeref false) (viasonmask false) (mode 1) (useauxorigin false)
      (hpglpennumber 1) (hpglpenspeed 20) (hpglpendiameter 15.000000) (psnegative false)
      (psa4output false) (plotreference true) (plotvalue true) (plotinvisibletext false)
      (padsonsilk false) (subtractmaskfromsilk false) (outputformat 1) (mirror false)
      (drillshape 1) (scaleselection 1) (outputdirectory ""))
  )
  (net 0 "")
{nets}
{netclass}
{chr(10).join(mods)}
{chr(10).join(gr)}
{zone}
)
"""
    with open(os.path.join(HERE, "module_v1.kicad_pcb"), "w") as f:
        f.write(pcb)
    return boxes, render


def check_boxes(boxes):
    problems = []
    for ref, x0, y0, x1, y1 in boxes:
        if x0 < 0 or y0 < 0 or x1 > BOARD_W or y1 > BOARD_H:
            problems.append(f"{ref} outside the board ({x0:.1f},{y0:.1f})-({x1:.1f},{y1:.1f})")
    keep = (40.5, 6.0, 58.6, 53.5)          # under the Nucleo: only its sockets
    for ref, x0, y0, x1, y1 in boxes:
        if ref not in ("J1", "J2") and x0 < keep[2] and x1 > keep[0] and y0 < keep[3] and y1 > keep[1]:
            problems.append(f"{ref} under the Nucleo")
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            ov_x = min(a[3], b[3]) - max(a[1], b[1])
            ov_y = min(a[4], b[4]) - max(a[2], b[2])
            if ov_x > 0.05 and ov_y > 0.05:
                problems.append(f"{a[0]} / {b[0]} overlap {ov_x:.2f} x {ov_y:.2f} mm")
    return problems


def render_png(render, boxes, path):
    from PIL import Image, ImageDraw, ImageFont
    s = 8
    img = Image.new("RGB", (int(BOARD_W * s) + 40, int(BOARD_H * s) + 40), (20, 60, 30))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
    except OSError:
        font = ImageFont.load_default()
    T = lambda x, y: (20 + x * s, 20 + y * s)
    d.rectangle([T(0, 0), T(BOARD_W, BOARD_H)], fill=(10, 90, 40), outline=(255, 255, 0))
    d.rectangle([T(40.5, 6.0), T(58.6, 53.5)], outline=(120, 160, 255))
    for ref, x0, y0, x1, y1 in boxes:
        d.rectangle([T(x0, y0), T(x1, y1)], outline=(230, 230, 230))
    for p, pads in render:
        for num, x, y, w, h, shape, net in pads:
            col = (200, 160, 60) if net else (120, 120, 120)
            if p["kind"] == "hole":
                col = (30, 30, 30)
            box = [T(x - w / 2, y - h / 2), T(x + w / 2, y + h / 2)]
            (d.rectangle if shape == "rect" else d.ellipse)(box, fill=col)
        xs = [x for _, x, *_ in pads] or [p["at"][0]]
        ys = [y for _, _, y, *_ in pads] or [p["at"][1]]
        d.text(T(sum(xs) / len(xs) - 1.5, sum(ys) / len(ys) - 1), p["ref"], fill=(255, 255, 255), font=font)
    for t, a, b, _, size in SILK:
        d.text(T(a - len(t) * size * 0.4, b - size * 0.7), t, fill=(255, 255, 120), font=font)
    img.save(path)


# ---------------------------------------------------------------- schematic ------------------
G = 2.54


def sym_def(kind, p):
    """returns (lib symbol name, body s-expr, [(pad, x, y, angle_to_body, side)]) in symbol coordinates"""
    pads = sorted(p["pads"].keys(), key=lambda k: int(k))
    if kind in ("res", "cap", "cpol", "fuse", "diode", "led", "tvs"):
        body = {
            "res": "(rectangle (start -1.016 -2.54) (end 1.016 2.54) (stroke (width 0.254) (type default)) (fill (type none)))",
            "fuse": "(rectangle (start -1.016 -2.54) (end 1.016 2.54) (stroke (width 0.254) (type default)) (fill (type none))) (polyline (pts (xy -1.5 -2) (xy 1.5 2)) (stroke (width 0.2) (type default)) (fill (type none)))",
            "cap": "(polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) (stroke (width 0.508) (type default)) (fill (type none))) (polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) (stroke (width 0.508) (type default)) (fill (type none)))",
            "cpol": "(rectangle (start -2.286 0.508) (end 2.286 1.016) (stroke (width 0.254) (type default)) (fill (type none))) (rectangle (start 2.286 -1.016) (end -2.286 -0.508) (stroke (width 0.254) (type default)) (fill (type outline))) (polyline (pts (xy -1.778 2.286) (xy -0.762 2.286)) (stroke (width 0.2) (type default)) (fill (type none))) (polyline (pts (xy -1.27 2.794) (xy -1.27 1.778)) (stroke (width 0.2) (type default)) (fill (type none)))",
        }.get(kind)
        if kind in ("diode", "led", "tvs"):     # pad 1 = cathode at the top, pad 2 = anode at the bottom
            body = ("(polyline (pts (xy -1.27 -1.27) (xy 1.27 -1.27) (xy 0 1.27) (xy -1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type none))) "
                    "(polyline (pts (xy -1.27 1.27) (xy 1.27 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))")
            body = body.replace("(xy -1.27 -1.27) (xy 1.27 -1.27) (xy 0 1.27)", "(xy -1.27 -1.27) (xy 1.27 -1.27) (xy 0 1.27)")
            if kind == "led":
                body += " (polyline (pts (xy 1.8 0) (xy 3.2 1.2)) (stroke (width 0.2) (type default)) (fill (type none))) (polyline (pts (xy 1.8 -1) (xy 3.2 0.2)) (stroke (width 0.2) (type default)) (fill (type none)))"
            if kind == "tvs":
                body += " (polyline (pts (xy -1.27 1.27) (xy -1.8 0.8)) (stroke (width 0.254) (type default)) (fill (type none))) (polyline (pts (xy 1.27 1.27) (xy 1.8 1.8)) (stroke (width 0.254) (type default)) (fill (type none)))"
        pins = [(pads[0], 0, 3.81, 270, "top"), (pads[1], 0, -3.81, 90, "bottom")]
        return kind, body, pins, 1.27
    # boxes: first half of the pins on the left, rest on the right
    n = len(pads)
    left = pads[: (n + 1) // 2] if kind not in ("conn",) else pads
    right = [k for k in pads if k not in left]
    rows = max(len(left), len(right), 1)
    hh = rows * G / 2 + G / 2
    width = 15.24 if kind != "conn" else 10.16
    hw = width / 2
    body = (f"(rectangle (start {-hw} {hh:.2f}) (end {hw} {-hh:.2f}) "
            "(stroke (width 0.254) (type default)) (fill (type background)))")
    pins = []
    for i, k in enumerate(left):
        pins.append((k, -hw - G, hh - G - i * G, 0, "left"))
    for i, k in enumerate(right):
        pins.append((k, hw + G, hh - G - i * G, 180, "right"))
    return f"{kind}_{p['ref']}", body, pins, G


def write_sch():
    root = uid("sch")
    libs, insts, labels, ncs, txts = [], [], [], [], []
    blocks = {}
    for p in PARTS:
        if p["kind"] == "hole":
            continue
        blocks.setdefault(p["block"], []).append(p)
    # blocks stacked in three columns on an A2 sheet
    columns = [(25, ["Bus", "Power", "Supply sense", "Inputs"]), (200, ["Nucleo", "Outputs"]),
               (390, ["RS485", "Address"])]
    order = []
    for cx, names in columns:
        for nm in names:
            order.append((cx, nm))
    col_next = {cx: 35.0 for cx, _ in columns}
    seen = set()
    for bx, block in order:
        parts = blocks[block]
        by = col_next[bx]
        txts.append(f'  (text "{block}" (exclude_from_sim no) (at {bx} {by - 8} 0) '
                    f'(effects (font (size 2.54 2.54) bold) (justify left bottom)) (uuid "{uid("t", block)}"))')
        x, y, rowh = bx, by, 0
        for p in parts:
            name, body, pins, plen = sym_def(p["kind"], p)
            hh = max(abs(py) for _, _, py, _, _ in pins) if pins else 5
            span_x = max(abs(px) for _, px, _, _, _ in pins) if pins else 5
            w = 2 * span_x + 30
            if x + w > bx + 175 and x > bx:
                x, y, rowh = bx, y + rowh + 12, 0
            sx, sy = round((x + span_x + 12) / 1.27) * 1.27, round((y + hh + 4) / 1.27) * 1.27
            rowh = max(rowh, 2 * hh + 8)
            x += w
            col_next[bx] = y + rowh + 25
            lib_id = f"mod:{name}"
            if name not in seen:
                seen.add(name)
                pin_s = []
                for pad, px, py, ang, side in pins:
                    pname = p["names"].get(pad, "~") if p["kind"] not in ("res", "cap", "cpol", "fuse", "diode", "led", "tvs") else "~"
                    pin_s.append(f'      (pin passive line (at {px:.2f} {py:.2f} {ang}) (length {plen}) '
                                 f'(name "{pname}" (effects (font (size 1.27 1.27)))) '
                                 f'(number "{pad}" (effects (font (size 1.27 1.27)))))')
                hide_names = "(pin_names (offset 0) hide)" if p["kind"] in ("res", "cap", "cpol", "fuse", "diode", "led", "tvs") else "(pin_names (offset 1.016))"
                libs.append(f'''    (symbol "{lib_id}" {hide_names} (exclude_from_sim no) (in_bom yes) (on_board yes)
      (property "Reference" "{re.sub(r"[0-9]+$", "", p["ref"])}" (at 0 0 0) (effects (font (size 1.27 1.27))))
      (property "Value" "{name}" (at 0 0 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (property "Description" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "{name}_0_1" {body})
      (symbol "{name}_1_1"
{chr(10).join(pin_s)}
      )
    )''')
            ref_y = sy - hh - 1.5
            insts.append(f'''  (symbol (lib_id "{lib_id}") (at {sx:.2f} {sy:.2f} 0) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
    (uuid "{sym_uuid(p["ref"])}")
    (property "Reference" "{p["ref"]}" (at {sx + 3:.2f} {ref_y:.2f} 0) (effects (font (size 1.27 1.27)) (justify left)))
    (property "Value" "{p["value"]}" (at {sx + 3:.2f} {ref_y + 1.6 + 2 * hh + 1.5:.2f} 0) (effects (font (size 1.27 1.27)) (justify left)))
    (property "Footprint" "{p["fp"]}" (at {sx:.2f} {sy:.2f} 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "~" (at {sx:.2f} {sy:.2f} 0) (effects (font (size 1.27 1.27)) hide))
    (property "Description" "{p["note"]}" (at {sx:.2f} {sy:.2f} 0) (effects (font (size 1.27 1.27)) hide))
{chr(10).join(f'    (pin "{pad}" (uuid "{uid("pin", p["ref"], pad)}"))' for pad, *_ in pins)}
    (instances (project "module_v1" (path "/{root}" (reference "{p["ref"]}") (unit 1))))
  )''')
            for pad, px, py, ang, side in pins:
                ex, ey = sx + px, sy - py
                net = p["pads"].get(pad)
                if not net:
                    ncs.append(f'  (no_connect (at {ex:.2f} {ey:.2f}) (uuid "{uid("nc", p["ref"], pad)}"))')
                    continue
                la, just = {"left": (180, "right"), "right": (0, "left"), "top": (90, "left"), "bottom": (270, "right")}[side]
                labels.append(f'  (label "{net}" (at {ex:.2f} {ey:.2f} {la}) (fields_autoplaced yes) '
                              f'(effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid "{uid("lbl", p["ref"], pad)}"))')
    # mounting holes: symbols without pins
    for p in PARTS:
        if p["kind"] != "hole":
            continue
        i = int(p["ref"][1:])
        name = "hole"
        if name not in seen:
            seen.add(name)
            libs.append(f'''    (symbol "mod:hole" (exclude_from_sim yes) (in_bom no) (on_board yes)
      (property "Reference" "H" (at 0 0 0) (effects (font (size 1.27 1.27))))
      (property "Value" "M3" (at 0 0 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (property "Description" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "hole_0_1" (circle (center 0 0) (radius 1.6) (stroke (width 0.254) (type default)) (fill (type none))))
    )''')
        sx, sy = 400 + 10 * i, 395
        insts.append(f'''  (symbol (lib_id "mod:hole") (at {sx} {sy} 0) (unit 1) (exclude_from_sim yes) (in_bom no) (on_board yes) (dnp no)
    (uuid "{sym_uuid(p["ref"])}")
    (property "Reference" "{p["ref"]}" (at {sx} {sy - 3} 0) (effects (font (size 1.27 1.27))))
    (property "Value" "M3" (at {sx} {sy + 3} 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "{p["fp"]}" (at {sx} {sy} 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "~" (at {sx} {sy} 0) (effects (font (size 1.27 1.27)) hide))
    (property "Description" "" (at {sx} {sy} 0) (effects (font (size 1.27 1.27)) hide))
    (instances (project "module_v1" (path "/{root}" (reference "{p["ref"]}") (unit 1))))
  )''')
    notes = ("Nets are joined by labels with the same name.\\n"
             "Nucleo: remove the D2-GND jumper (CN3 pin 4-5). VIN unused.\\n"
             "Inputs: EOL 1k series + 1k across the contact (profile 0). Relay contacts max 30 V.")
    txts.append(f'  (text "{notes}" (exclude_from_sim no) (at 400 370 0) (effects (font (size 1.8 1.8)) (justify left bottom)) (uuid "{uid("notes")}"))')
    sch = f'''(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")
  (uuid "{root}")
  (paper "A2")
  (title_block (title "Sprinkler I/O module 4/4") (rev "v1") (comment 1 "Carrier for NUCLEO-G031K8, all THT"))
  (lib_symbols
{chr(10).join(libs)}
  )
{chr(10).join(txts)}
{chr(10).join(labels)}
{chr(10).join(ncs)}
{chr(10).join(insts)}
  (sheet_instances (path "/" (page "1")))
)
'''
    with open(os.path.join(HERE, "module_v1.kicad_sch"), "w") as f:
        f.write(sch)


def write_pro():
    with open(os.path.join(HERE, "module_v1.kicad_pro"), "w") as f:
        f.write('{\n  "meta": { "filename": "module_v1.kicad_pro", "version": 1 },\n'
                '  "board": { "design_settings": { "rules": { "min_clearance": 0.2, "min_track_width": 0.2 } } },\n'
                '  "sheets": [ [ "%s", "Root" ] ]\n}\n' % uid("sch"))


def bom():
    rows = {}
    for p in PARTS:
        if p["kind"] == "hole":
            continue
        rows.setdefault((p["value"], p["fp"].split(":")[1]), []).append(p["ref"])
    key = lambda r: (re.sub(r"\d+", "", r), int(re.sub(r"\D", "", r) or 0))
    with open(os.path.join(HERE, "bom.csv"), "w") as f:
        f.write("qty;value;refs;footprint\n")
        for (v, fp), refs in sorted(rows.items(), key=lambda kv: key(sorted(kv[1], key=key)[0])):
            refs = sorted(refs, key=key)
            f.write(f"{len(refs)};{v};{' '.join(refs)};{fp}\n")


if __name__ == "__main__":
    lib_root = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KICAD_FOOTPRINTS", "")
    pins, single = check_nets()
    boxes, render = write_pcb(lib_root)
    write_sch()
    write_pro()
    bom()
    render_png(render, boxes, os.path.join(HERE, "placement.png"))
    probs = check_boxes(boxes)
    print(f"{len(PARTS)} parts, {len(ALL_NETS)} nets")
    for n, v in single.items():
        print(f"  net with one pin: {n} {v}")
    for pr in probs:
        print("  " + pr)
