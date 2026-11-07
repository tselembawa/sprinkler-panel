#!/usr/bin/env python3
"""
Προσομοιωτής I/O modules για το PC (Modbus RTU slave), σύμφωνα με τον χάρτη registers v1
(docs/modbus-map.md). Παίζει ένα ή περισσότερα modules 4/4 πάνω στη γραμμή RS485 μέσω του
μετατροπέα USB-RS485, ώστε ο controller (STM32) να έχει κάποιον να ρωτήσει.

Χρήση (Windows):
    pip install pyserial
    python module_sim.py COM5                 # ένα module, διεύθυνση 2 (L1-M02)
    python module_sim.py COM5 -a 2 -a 3       # δύο modules

Πλήκτρα όσο τρέχει:
    1 2 3 4  αλλάζει την είσοδο IN1..IN4: CLOSED -> OPEN -> LINE_OPEN -> LINE_SHORT -> CLOSED
    n        επόμενο module (όταν υπάρχουν πολλά)
    r        επανεκκίνηση module (σαν να κόπηκε το ρεύμα του)
    s        σιωπή: το module σταματά να απαντά (δοκιμή «modul saknas») / ξανά
    v        χαμηλή τάση 24 V (18,5 V) / κανονική
    q        έξοδος
"""

import argparse
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None

# ---- register map v1 (same values as firmware/common/modbus_map.h) -----------------------
MODULE_TYPE = 0x0404
MAP_VERSION = 1
FW_VERSION = 0x0100
HW_VERSION = 1

IN_CLOSED, IN_OPEN, IN_LINE_OPEN, IN_LINE_SHORT, IN_NOT_USED, IN_MEASURE_FAULT = range(6)
IN_NAMES = ["CLOSED", "OPEN", "LINE_OPEN", "LINE_SHORT", "NOT_USED", "MEASURE_FAULT"]
IN_OHM = {IN_CLOSED: 1000, IN_OPEN: 2000, IN_LINE_OPEN: 0xFFFF, IN_LINE_SHORT: 0, IN_NOT_USED: 0}

ST_RESTARTED, ST_FAILSAFE, ST_SUPPLY_LOW, ST_SUPPLY_HIGH = 1, 2, 4, 8
ST_INTERNAL_FAULT, ST_DIP_CHANGED, ST_IDENTIFY = 16, 32, 64

CMD_OFF, CMD_ON, CMD_FLASH, CMD_FLASH_FAST, CMD_PULSE = range(5)
CMD_NAMES = ["OFF", "ON", "FLASH", "FLASH_FAST", "PULSE"]

HR_OUT_CMD, HR_COMMAND, HR_SYNC = 0x0100, 0x0101, 0x0102
HR_CFG_START, HR_CFG_COUNT = 0x0110, 12
COMMAND_NAMES = {1: "CONFIG_DONE", 2: "CLEAR_FLAGS", 3: "IDENTIFY_ON", 4: "IDENTIFY_OFF", 5: "REBOOT"}

EX_FUNCTION, EX_ADDRESS, EX_VALUE = 1, 2, 3

DEFAULT_CFG = [10, 0x2222, 100, 10, 0x0001, 0x0001, 0x0001, 0x0001, 0x3202, 0x3202, 0x3202, 0x3202]
SUPPLY_LOW_MV, SUPPLY_HIGH_MV = 20000, 30000


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(frame: bytes) -> bytes:
    c = crc16(frame)
    return frame + bytes([c & 0xFF, c >> 8])


def crc_ok(frame: bytes) -> bool:
    return len(frame) >= 4 and crc16(frame[:-2]) == frame[-2] | (frame[-1] << 8)


def nibble(word, n):
    return (word >> (4 * n)) & 0xF


class ModbusException(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


class Module:
    """One 4-input / 4-output module. All times come from clock() so tests can control them."""

    def __init__(self, address, serial_no=None, clock=time.monotonic, log=print):
        self.address = address
        self.serial_no = serial_no if serial_no is not None else 0x00A50000 + address
        self.clock = clock
        self.log = log
        # physical contact states (what is wired on the "breadboard")
        self.contacts = [IN_CLOSED, IN_OPEN, IN_CLOSED, IN_CLOSED]
        self.supply_mv = 24000
        self.silent = False
        self.power_on()

    # -- life cycle -------------------------------------------------------------------------
    def power_on(self):
        self.cfg = list(DEFAULT_CFG)
        self.flags = ST_RESTARTED
        self.out_cmd = 0
        self.changes = 0
        self.in_changes = [0, 0, 0, 0]
        self.crc_errors = 0
        self.failsafe_active = False
        self.identify_until = 0.0
        self.pulse_until = [0.0] * 4
        self.flash_t0 = self.clock()
        self.last_request = self.clock()
        self._last_inputs = self.inputs_word()

    # -- what the module reports -----------------------------------------------------------
    def input_value(self, n):
        mode = self.cfg[4 + n] & 0xFF
        state = self.contacts[n]
        if mode == 0:
            return IN_NOT_USED
        if mode == 2:  # unsupervised: only open / closed
            return IN_CLOSED if state in (IN_CLOSED, IN_LINE_SHORT) else IN_OPEN
        return state

    def inputs_word(self):
        return sum(self.input_value(n) << (4 * n) for n in range(4))

    def effective_cmd(self, n):
        if self.failsafe_active:
            fs = nibble(self.cfg[1], n)
            return {0: CMD_OFF, 1: CMD_ON, 2: nibble(self.out_cmd, n), 3: CMD_FLASH}[fs]
        return nibble(self.out_cmd, n)

    def output_on(self, n):
        cmd = self.effective_cmd(n)
        t = self.clock() - self.flash_t0
        if cmd == CMD_ON:
            return True
        if cmd == CMD_FLASH:
            return (t % 1.0) < 0.5
        if cmd == CMD_FLASH_FAST:
            return (t % 0.5) < 0.25
        if cmd == CMD_PULSE:
            return self.clock() < self.pulse_until[n]
        return False

    def outputs_word(self):
        word = 0
        for n in range(4):
            mode = self.cfg[8 + n] & 0xFF
            sup = 0 if mode == 2 else 4
            on = 8 if (mode != 0 and self.output_on(n)) else 0
            word |= (on | sup) << (4 * n)
        return word

    def status(self):
        st = self.flags
        if self.supply_mv < SUPPLY_LOW_MV:
            st |= ST_SUPPLY_LOW
        if self.supply_mv > SUPPLY_HIGH_MV:
            st |= ST_SUPPLY_HIGH
        if self.clock() < self.identify_until:
            st |= ST_IDENTIFY
        return st

    def input_register(self, reg):
        if reg == 0x00:
            return MODULE_TYPE
        if reg == 0x01:
            return MAP_VERSION
        if reg == 0x02:
            return FW_VERSION
        if reg == 0x03:
            return HW_VERSION
        if reg == 0x04:
            return (self.serial_no >> 16) & 0xFFFF
        if reg == 0x05:
            return self.serial_no & 0xFFFF
        if reg == 0x06:
            return self.address
        if reg == 0x07:
            return 0
        if reg == 0x10:
            return self.status()
        if reg == 0x11:
            return self.inputs_word()
        if reg == 0x12:
            return self.outputs_word()
        if reg == 0x13:
            return self.changes & 0xFFFF
        if 0x14 <= reg <= 0x17:
            return self.in_changes[reg - 0x14] & 0xFFFF
        if 0x18 <= reg <= 0x1B:
            v = self.input_value(reg - 0x18)
            return IN_OHM.get(v, 0)
        if 0x1C <= reg <= 0x1F:
            return 120 if self.output_on(reg - 0x1C) else 0
        if reg == 0x20:
            return self.supply_mv
        if reg == 0x21:
            return 235  # 23.5 C
        if reg == 0x22:
            return self.crc_errors & 0xFFFF
        raise ModbusException(EX_ADDRESS)

    def holding_register(self, reg):
        if reg == HR_OUT_CMD:
            return self.out_cmd
        if reg in (HR_COMMAND, HR_SYNC):
            return 0
        if HR_CFG_START <= reg < HR_CFG_START + HR_CFG_COUNT:
            return self.cfg[reg - HR_CFG_START]
        raise ModbusException(EX_ADDRESS)

    # -- writes -----------------------------------------------------------------------------
    @staticmethod
    def check_value(reg, value):
        """Raise ModbusException(03) for a value out of range, (02) for an unknown register."""
        if reg == HR_OUT_CMD:
            ok = all(nibble(value, n) <= CMD_PULSE for n in range(4))
        elif reg == HR_COMMAND:
            ok = value in COMMAND_NAMES
        elif reg == HR_SYNC:
            ok = True
        elif reg == 0x0110:
            ok = 2 <= value <= 60
        elif reg == 0x0111:
            ok = all(nibble(value, n) <= 3 for n in range(4))
        elif reg == 0x0112:
            ok = 20 <= value <= 2000
        elif reg == 0x0113:
            ok = 1 <= value <= 600
        elif 0x0114 <= reg <= 0x0117:
            ok = (value & 0xFF) <= 2 and (value >> 8) == 0
        elif 0x0118 <= reg <= 0x011B:
            ok = (value & 0xFF) <= 2
        else:
            raise ModbusException(EX_ADDRESS)
        if not ok:
            raise ModbusException(EX_VALUE)

    def write_register(self, reg, value):
        if reg == HR_OUT_CMD:
            if value != self.out_cmd:
                for n in range(4):
                    if nibble(value, n) == CMD_PULSE and nibble(self.out_cmd, n) != CMD_PULSE:
                        self.pulse_until[n] = self.clock() + self.cfg[3] / 10.0
                self.log(f"[M{self.address:02d}] έξοδοι: " + " ".join(
                    f"OUT{n + 1}={CMD_NAMES[nibble(value, n)]}" for n in range(4)))
                self.out_cmd = value
                self.changes += 1
        elif reg == HR_COMMAND:
            self.command(value)
        elif reg == HR_SYNC:
            self.flash_t0 = self.clock()
        else:
            self.cfg[reg - HR_CFG_START] = value

    def command(self, value):
        self.log(f"[M{self.address:02d}] εντολή {COMMAND_NAMES[value]}")
        if value == 1:
            self.flags &= ~ST_RESTARTED
        elif value == 2:
            self.flags &= ~(ST_FAILSAFE | ST_DIP_CHANGED)
            self.crc_errors = 0
        elif value == 3:
            self.identify_until = self.clock() + 60
        elif value == 4:
            self.identify_until = 0.0
        elif value == 5:
            self.power_on()

    # -- request handling -------------------------------------------------------------------
    def handle(self, frame: bytes):
        """Handle one CRC-checked request addressed to this module (or broadcast).
        Returns the answer frame, or None (broadcast / silent)."""
        broadcast = frame[0] == 0
        if self.silent:
            return None
        self.last_request = self.clock()
        if self.failsafe_active:
            self.failsafe_active = False
            self.log(f"[M{self.address:02d}] επικοινωνία ξανά — οι έξοδοι ακολουθούν τον controller")
        fc = frame[1]
        try:
            if fc in (0x03, 0x04):
                if broadcast:
                    return None
                start, count = int.from_bytes(frame[2:4], "big"), int.from_bytes(frame[4:6], "big")
                if not 1 <= count <= 125:
                    raise ModbusException(EX_VALUE)
                get = self.input_register if fc == 0x04 else self.holding_register
                values = [get(start + i) for i in range(count)]
                data = b"".join(v.to_bytes(2, "big") for v in values)
                return with_crc(bytes([self.address, fc, len(data)]) + data)
            if fc == 0x06:
                reg, value = int.from_bytes(frame[2:4], "big"), int.from_bytes(frame[4:6], "big")
                self.check_value(reg, value)
                self.write_register(reg, value)
                return None if broadcast else with_crc(bytes([self.address]) + frame[1:6])
            if fc == 0x10:
                start, count = int.from_bytes(frame[2:4], "big"), int.from_bytes(frame[4:6], "big")
                if not 1 <= count <= 123 or frame[6] != 2 * count:
                    raise ModbusException(EX_VALUE)
                values = [int.from_bytes(frame[7 + 2 * i:9 + 2 * i], "big") for i in range(count)]
                for i, v in enumerate(values):      # check everything before changing anything
                    self.check_value(start + i, v)
                for i, v in enumerate(values):
                    self.write_register(start + i, v)
                if start <= HR_CFG_START < start + count:
                    self.log(f"[M{self.address:02d}] πήρε ρύθμιση: timeout {self.cfg[0]} s, "
                             f"είσοδοι {[c & 0xFF for c in self.cfg[4:8]]}")
                return None if broadcast else with_crc(bytes([self.address]) + frame[1:6])
            raise ModbusException(EX_FUNCTION)
        except ModbusException as e:
            if broadcast:
                return None
            return with_crc(bytes([self.address, fc | 0x80, e.code]))

    def tick(self):
        """Called often: communication timeout -> failsafe."""
        if not self.failsafe_active and self.clock() - self.last_request > self.cfg[0]:
            self.failsafe_active = True
            self.flags |= ST_FAILSAFE
            self.log(f"[M{self.address:02d}] FAILSAFE: καμία ερώτηση για {self.cfg[0]} s")

    # -- things the user does ----------------------------------------------------------------
    def set_contact(self, n, state):
        before = self.input_value(n)
        self.contacts[n] = state
        after = self.input_value(n)
        if after != before:
            self.in_changes[n] += 1
            self.changes += 1

    def cycle_contact(self, n):
        self.set_contact(n, (self.contacts[n] + 1) % 4)

    def describe(self):
        ins = " ".join(f"IN{n + 1}={IN_NAMES[self.input_value(n)]}" for n in range(4))
        extra = []
        if self.flags & ST_RESTARTED:
            extra.append("περιμένει ρύθμιση")
        if self.silent:
            extra.append("ΣΙΩΠΗ")
        if self.supply_mv < SUPPLY_LOW_MV:
            extra.append(f"τάση {self.supply_mv / 1000:.1f} V")
        if self.failsafe_active:
            extra.append("FAILSAFE")
        tail = f"  ({', '.join(extra)})" if extra else ""
        return f"[L{(self.address - 1) // 60 + 1}-M{self.address:02d}] {ins}{tail}"


class FrameReader:
    """Splits the incoming byte stream into requests, using the expected length of each
    function code and the CRC (no timing needed, works with the USB latency of Windows)."""

    def __init__(self, on_bad_crc=None):
        self.buf = bytearray()
        self.last_byte = 0.0
        self.on_bad_crc = on_bad_crc

    def feed(self, data: bytes, now: float):
        if data:
            if self.buf and now - self.last_byte > 0.05:   # stale half frame
                self.buf.clear()
            self.buf += data
            self.last_byte = now
        frames = []
        while len(self.buf) >= 4:
            fc = self.buf[1]
            if fc in (0x03, 0x04, 0x06):
                need = 8
            elif fc == 0x10:
                if len(self.buf) < 7:
                    break
                need = 9 + self.buf[6]
            else:   # unknown function: look for any CRC-valid frame
                need = next((n for n in range(4, len(self.buf) + 1) if crc_ok(bytes(self.buf[:n]))), None)
                if need is None:
                    if len(self.buf) > 256:
                        del self.buf[0]
                        continue
                    break
            if len(self.buf) < need:
                break
            frame = bytes(self.buf[:need])
            if crc_ok(frame):
                frames.append(frame)
                del self.buf[:need]
            else:
                if self.on_bad_crc:
                    self.on_bad_crc(self.buf[0])
                del self.buf[0]   # resync
        return frames


class Bus:
    def __init__(self, modules):
        self.modules = {m.address: m for m in modules}
        self.reader = FrameReader(on_bad_crc=self._bad_crc)
        self.requests = 0

    def _bad_crc(self, addr):
        if addr in self.modules:
            self.modules[addr].crc_errors += 1

    def process(self, data: bytes, now: float):
        """Returns the answers to send, in order."""
        answers = []
        for frame in self.reader.feed(data, now):
            addr = frame[0]
            if addr == 0:
                for m in self.modules.values():
                    m.handle(frame)
            elif addr in self.modules:
                self.requests += 1
                ans = self.modules[addr].handle(frame)
                if ans:
                    answers.append(ans)
        for m in self.modules.values():
            m.tick()
        return answers


# ---- keyboard ------------------------------------------------------------------------------
def key_reader():
    """Returns a function that gives one pressed key or None, without waiting."""
    if sys.platform.startswith("win"):
        import msvcrt

        def get():
            return msvcrt.getwch() if msvcrt.kbhit() else None
        return get, lambda: None
    import select
    import termios
    import tty
    if not sys.stdin.isatty():
        return (lambda: None), (lambda: None)
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setcbreak(fd)

    def get():
        r, _, _ = select.select([sys.stdin], [], [], 0)
        return sys.stdin.read(1) if r else None
    return get, (lambda: termios.tcsetattr(fd, termios.TCSADRAIN, old))


def run(port, addresses, baud=38400, keys=True, modules=None, stop=None):
    """Main loop. Tests pass their own `modules` and a threading.Event `stop`."""
    if serial is None:
        print("Λείπει το pyserial. Γράψε:  pip install pyserial")
        return 1
    ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0.005)
    modules = modules or [Module(a) for a in addresses]
    addresses = [m.address for m in modules]
    bus = Bus(modules)
    sel = 0
    get_key, restore = key_reader() if keys else ((lambda: None), (lambda: None))
    if keys:
        print("\nΠλήκτρα" + __doc__.split("Πλήκτρα", 1)[1])
    print(f"Ακούω στο {port}, {baud} baud. Modules: " + ", ".join(f"M{a:02d}" for a in addresses))
    for m in modules:
        print(m.describe())
    last_report, last_requests = time.monotonic(), 0
    echo = None        # does the USB-RS485 adapter hear its own answers? (detected on the first answer)
    leftover = b""
    try:
        while not (stop and stop.is_set()):
            data = leftover + ser.read(ser.in_waiting or 1)
            leftover = b""
            for ans in bus.process(data, time.monotonic()):
                ser.write(ans)
                ser.flush()
                if echo is not False:
                    ser.timeout = 0.05
                    back = ser.read(len(ans))
                    ser.timeout = 0.005
                    if back == ans:
                        echo = True
                    else:
                        leftover += back
                        if echo is None:
                            echo = False
            k = get_key()
            if k:
                m = modules[sel]
                if k in "1234":
                    m.cycle_contact(int(k) - 1)
                elif k == "n":
                    sel = (sel + 1) % len(modules)
                elif k == "r":
                    m.power_on()
                    print(f"[M{m.address:02d}] επανεκκίνηση")
                elif k == "s":
                    m.silent = not m.silent
                elif k == "v":
                    m.supply_mv = 18500 if m.supply_mv >= SUPPLY_LOW_MV else 24000
                elif k == "q":
                    break
                print(modules[sel].describe())
            now = time.monotonic()
            if now - last_report >= 10:
                rate = (bus.requests - last_requests) / (now - last_report)
                print(f"   … {rate:.1f} ερωτήσεις/s από τον controller")
                last_report, last_requests = now, bus.requests
    except KeyboardInterrupt:
        pass
    finally:
        restore()
        ser.close()
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Προσομοιωτής I/O modules (Modbus RTU slave)")
    p.add_argument("port", help="σειριακή θύρα του USB-RS485, π.χ. COM5 ή /dev/ttyUSB0")
    p.add_argument("-a", "--address", type=int, action="append",
                   help="διεύθυνση module (μπορεί πολλές φορές). Προεπιλογή: 2")
    p.add_argument("-b", "--baud", type=int, default=38400)
    p.add_argument("--no-keys", action="store_true", help="χωρίς πλήκτρα (για αυτόματες δοκιμές)")
    args = p.parse_args(argv)
    addresses = args.address or [2]
    for a in addresses:
        if not 1 <= a <= 240:
            p.error(f"μη έγκυρη διεύθυνση {a} (1–240)")
    return run(args.port, addresses, args.baud, keys=not args.no_keys)


if __name__ == "__main__":
    sys.exit(main())
