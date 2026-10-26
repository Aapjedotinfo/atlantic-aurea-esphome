"""harness.py - hulpfuncties om M1.1 / T1.1 in de eigen emulator te laten lopen met gesimuleerde ingangen."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu, uartdec as U

F = 8_000_000            # 8 MHz kristal (zie index.html: kristal onder IC2-voet; 666,67 baud volgt exact uit 1500 us = 6 ticks x 250 us)
BIT666 = 12000           # cycli per bit op 666,67 baud (1,5 ms)
BIT9600 = 8 * 104        # U2X, UBRR=103 -> 832 cycli per bit (9615 baud)


def mk(chip):
    f = os.path.join(os.path.dirname(HERE), "Haddon %s - ATMEGA8@DIP28.BIN" % chip)
    return emu.CPU(open(f, "rb").read()[:8192], chip)


def set_pin(c, port, bit, level):
    """Drijf een extern signaal op PINx.bit (port = emu.PINB/PINC/PIND)."""
    c.ext_drv[port] |= (1 << bit)
    if level:
        c.ext_val[port] |= (1 << bit)
    else:
        c.ext_val[port] &= ~(1 << bit)


def release_pin(c, port, bit):
    c.ext_drv[port] &= ~(1 << bit)


def sched_pin(c, cycle, port, bit, level):
    c.schedule(cycle, lambda cc, p=port, b=bit, l=level: set_pin(cc, p, b, l))


def send_bytes_8n1(c, port, bit, start, data, bitc=BIT666, idle=1, gap_bits=2, invert=False):
    """Zet bytes als 8N1 (start 0, 8 data LSB eerst, stop 1) op een pin; return eindcyclus."""
    t = start
    lv = lambda x: (1 - x) if invert else x
    for byte in data:
        sched_pin(c, t, port, bit, lv(0)); t += bitc
        for i in range(8):
            sched_pin(c, t, port, bit, lv((byte >> i) & 1)); t += bitc
        sched_pin(c, t, port, bit, lv(1)); t += bitc * (1 + gap_bits - 1) if gap_bits else bitc
    return t


def send_uart9600(c, start, data, gap_bits=0):
    """Injecteer bytes in de hardware-UART-ontvanger (RXC), 11 bit-tijden per byte (8N2)."""
    t = start
    for b in data:
        c.inject_rx(t + 9 * BIT9600, b)    # RXC komt aan het eind van het stopbit
        t += (11 + gap_bits) * BIT9600
    return t


def link_frame(payload11):
    """12-byte inter-MCU-frame: 11 bytes + 8-bit som."""
    assert len(payload11) == 11
    return list(payload11) + [sum(payload11) & 0xFF]


def line_edges(c, port, bit, t0=0):
    out = []
    lvl = None
    for cy, a, v in c.portlog:
        if a != port or cy < t0:
            continue
        b = (v >> bit) & 1
        if b != lvl:
            out.append((cy, b))
            lvl = b
    return out


def decode_frames(bytes_with_time):
    """Splits (start,val,stop) op 0x19-kop; lengte = byte[2]. Return lijst (start, bytes, crc_ok)."""
    frames = []
    seq = [(st, v) for st, v, sb in bytes_with_time]
    i = 0
    while i < len(seq):
        if seq[i][1] == 0x19 and i + 2 < len(seq):
            ln = seq[i + 2][1]
            if ln >= 5 and i + ln <= len(seq):
                fr = [v for _, v in seq[i:i + ln]]
                frames.append((seq[i][0], fr, U.crc16(fr) == 0))
                i += ln
                continue
        i += 1
    return frames


def tx666_frames(c, t0=0):
    e = line_edges(c, emu.PORTC, 3, t0)
    return decode_frames(U.decode_bytes(e))


def reply91(id_, payload, msglen=None):
    """Bouw een 0x91-antwoord van de warmtepomp: 91 ID len(=msglen-1) payload CRC16 (big-endian), msglen in {13,14,19,21}."""
    if msglen is None:
        msglen = {0: 13, 1: 14, 2: 14, 3: 21}.get(id_, 14)
    body = [0x91, id_, msglen - 1] + list(payload)
    body += [0] * (msglen - 2 - len(body))
    body = body[:msglen - 2]
    crc = U.crc16(body)
    return body + [crc >> 8, crc & 0xFF]


def temp_payload(t_supply, t_return, t_out, tail=(0, 0, 0)):
    """ID2: drie signed 16-bit little-endian waarden in 0,1 graad."""
    out = []
    for t in (t_supply, t_return, t_out):
        v = int(round(t * 10)) & 0xFFFF
        out += [v & 0xFF, v >> 8]
    return out + list(tail)


class Outdoor:
    """Simpele buitenunit: antwoordt na elk compleet CB-telegram met reply_fn(cb_frame)->bytes|None."""
    def __init__(self, c, reply_fn, delay_ms=20.0, low_after_ms=40.0):
        self.c, self.fn, self.delay = c, reply_fn, int(delay_ms * 8000)
        self.low_after = int(low_after_ms * 8000)
        self.seen = 0
        set_pin(c, emu.PINC, 4, 1)
        self.log = []

    def poll(self, t0_cursor=0):
        c = self.c
        log = c.portlog
        if not hasattr(self, "pi"):
            self.pi, self.lvl, self.edges, self.done_t = 0, None, [], -1
        while self.pi < len(log):
            cy, a, v = log[self.pi]
            self.pi += 1
            if a == emu.PORTC:
                b = (v >> 3) & 1
                if b != self.lvl:
                    self.edges.append((cy, b))
                    self.lvl = b
        # venster: alleen de laatste ~3 s aan flanken bewaren (houdt het decoderen goedkoop)
        cut = c.cyc - 3 * F
        if self.edges and self.edges[0][0] < cut:
            k = 0
            while k < len(self.edges) and self.edges[k][0] < cut:
                k += 1
            # begin op een stijgende flank (rust) zodat decode_bytes niet midden in een byte start
            while k < len(self.edges) and self.edges[k][1] == 0:
                k += 1
            self.edges = self.edges[k:]
        for st, f, ok in decode_frames(U.decode_bytes(self.edges)):
            if st <= self.done_t:
                continue
            end = st + len(f) * 10 * BIT666
            if end + 1 > c.cyc:
                break
            self.done_t = st
            r = self.fn(f)
            if r:
                t = max(end + self.delay, c.cyc + 1)
                te = send_bytes_8n1(c, emu.PINC, 4, t, r, gap_bits=0)
                if self.low_after:       # (zie RAPPORT: frame-einde-detectie in sub_0308 eist >=129 ticks laag)
                    sched_pin(c, te, emu.PINC, 4, 0)
                    sched_pin(c, te + self.low_after, emu.PINC, 4, 1)
                self.log.append((t, f, r))


def run_with(c, secs, outdoor=None, step_s=0.05, hook=None):
    n = int(secs / step_s)
    for i in range(n):
        c.run(int((i + 1) * step_s * F) + c_start(c))
        if outdoor:
            outdoor.poll()
        if hook:
            hook(c, (i + 1) * step_s)


def c_start(c):
    return getattr(c, "_t0", 0)
