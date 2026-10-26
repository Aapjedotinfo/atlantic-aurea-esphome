"""ot.py - OpenTherm Manchester-codering (1 kbit/s, 34 bits) voor de emulator-experimenten met T1.1."""
import bisect

BITC = 8000          # 1 ms @ 8 MHz
MT = {"READ": 0, "WRITE": 1, "INVALID": 2, "RESERVED": 3, "READACK": 4, "WRITEACK": 5, "DATAINVALID": 6, "UNKNOWNID": 7}


def frame(msgtype, did, data16):
    v = ((msgtype & 7) << 28) | ((did & 0xFF) << 16) | (data16 & 0xFFFF)
    if bin(v).count("1") & 1:
        v |= 1 << 31         # even pariteit over alle 32 bits
    return v


def bits34(frame32):
    return [1] + [(frame32 >> (31 - i)) & 1 for i in range(32)] + [1]


def edges(frame32, t0, idle=1):
    """Lijst (cyclus, niveau) met idle=1-conventie: bit 1 = eerst laag, dan hoog; bit 0 = eerst hoog, dan laag.
    idle=0 geeft de geinverteerde lijn (ketelzijde ontvangst op PD5)."""
    out = []
    t = t0
    for b in bits34(frame32):
        first, second = (0, 1) if b else (1, 0)
        out.append((t, first if idle else 1 - first))
        out.append((t + BITC // 2, second if idle else 1 - second))
        t += BITC
    out.append((t, 1 if idle else 0))
    return out, t


class Line:
    """Tijdsafhankelijk logisch niveau; get(cyc) voor de comparator-callback."""
    def __init__(self, idle=1):
        self.idle = idle
        self.t = [-1]
        self.v = [idle]

    def add(self, edges_):
        for cy, lv in edges_:
            self.t.append(cy)
            self.v.append(lv)

    def get(self, cyc):
        return self.v[bisect.bisect_right(self.t, cyc) - 1]


class Stream:
    """Incrementele decoder: voed met c.portlog; geeft complete frames zodra de lijn >= 2 bits stil is."""
    def __init__(self, port, bit, idle=1):
        self.port, self.bit, self.idle = port, bit, idle
        self.pi = 0
        self.lvl = idle
        self.ev = []
        self.done = []

    def poll(self, c):
        log = c.portlog
        while self.pi < len(log):
            cy, a, v = log[self.pi]
            self.pi += 1
            if a != self.port:
                continue
            b = (v >> self.bit) & 1
            if b != self.lvl:
                self.ev.append((cy, b))
                self.lvl = b
        if self.ev and c.cyc - self.ev[-1][0] > 2 * BITC:
            r = decode_events(self.ev, self.idle)
            self.done.extend(r)
            self.ev = []
            return r
        return []


def decode_events(ev, idle=1):
    def at(t):
        r = idle
        for cy, b in ev:
            if cy <= t:
                r = b
            else:
                break
        return r
    out = []
    used_until = -1
    for cy, b in ev:
        if cy < used_until or b != 0:
            continue
        bits = [at(cy + int((k + 0.75) * BITC)) for k in range(34)]
        halves_ok = all(at(cy + int((k + 0.25) * BITC)) != bits[k] for k in range(34))
        val = 0
        for k in range(1, 33):
            val = (val << 1) | bits[k]
        par_ok = (bin(val).count("1") & 1) == 0
        out.append((cy, val, par_ok and halves_ok and bits[0] == 1 and bits[33] == 1))
        used_until = cy + 34 * BITC
    return out


def decode(portlog, port, bit, tmin=0, idle=1):
    """Decodeer OpenTherm-frames uit een poortlog (lijn idle=1). Return [(t_start, frame32|None, parity_ok)]."""
    ev = []
    lvl = idle
    for cy, a, v in portlog:
        if a != port:
            continue
        b = (v >> bit) & 1
        if b != lvl:
            ev.append((cy, b))
            lvl = b

    def at(t):
        r = idle
        for cy, b in ev:
            if cy <= t:
                r = b
            else:
                break
        return r
    out = []
    i = 0
    used_until = -1
    for cy, b in ev:
        if cy < tmin or cy < used_until or b != 0:
            continue
        # startbit = '1': eerste helft laag. Bits bemonsteren in tweede helft (3/4)
        bits = [at(cy + int((k + 0.75) * BITC)) for k in range(34)]
        # controleer midden-overgangen: eerste helft van elk bit is het complement
        halves_ok = all(at(cy + int((k + 0.25) * BITC)) != bits[k] for k in range(34))
        val = 0
        for k in range(1, 33):
            val = (val << 1) | bits[k]
        par_ok = (bin(val).count("1") & 1) == 0
        out.append((cy, val, par_ok and halves_ok and bits[0] == 1 and bits[33] == 1))
        used_until = cy + 34 * BITC
    return out


def pretty(f32):
    mt = (f32 >> 28) & 7
    name = {v: k for k, v in MT.items()}[mt]
    return "%s ID%d data=%04x" % (name, (f32 >> 16) & 0xFF, f32 & 0xFFFF)
