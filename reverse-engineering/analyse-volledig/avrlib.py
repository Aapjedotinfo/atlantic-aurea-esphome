#!/usr/bin/env python3
"""avrlib.py - eigen, onafhankelijke AVR (ATmega8 / avr4-core) decoder + encoder.

Doel: (1) een tweede, onafhankelijke decoder naast avrdis.py, (2) een encoder
zodat disassembly -> opcode round-trip bewezen kan worden (los van avr-as),
(3) basis voor de emulator (emu.py).

Alle opcodes worden beschreven met (mask, value, naam, operandvelden).
Verificatie tegen GNU binutils avr-objdump staat in verify_decoder.py.
"""

# ---- ATmega8 IO-namen (IO-adres 0x00-0x3F; data-adres = IO + 0x20) ----
IO = {
    0x3F: "SREG", 0x3E: "SPH", 0x3D: "SPL", 0x3B: "GICR", 0x3A: "GIFR",
    0x39: "TIMSK", 0x38: "TIFR", 0x37: "SPMCR", 0x36: "TWCR", 0x35: "MCUCR",
    0x34: "MCUCSR", 0x33: "TCCR0", 0x32: "TCNT0", 0x31: "OSCCAL/OCDR",
    0x30: "SFIOR", 0x2F: "TCCR1A", 0x2E: "TCCR1B", 0x2D: "TCNT1H",
    0x2C: "TCNT1L", 0x2B: "OCR1AH", 0x2A: "OCR1AL", 0x29: "OCR1BH",
    0x28: "OCR1BL", 0x27: "ICR1H?", 0x26: "ICR1H", 0x25: "ICR1L",
    0x24: "TCCR2", 0x23: "TCNT2", 0x22: "OCR2", 0x21: "WDTCR",
    0x20: "UBRRH/UCSRC", 0x1F: "EEARH", 0x1E: "EEARL", 0x1D: "EEDR",
    0x1C: "EECR", 0x18: "PORTB", 0x17: "DDRB", 0x16: "PINB", 0x15: "PORTC",
    0x14: "DDRC", 0x13: "PINC", 0x12: "PORTD", 0x11: "DDRD", 0x10: "PIND",
    0x0F: "SPDR", 0x0E: "SPSR", 0x0D: "SPCR", 0x0C: "UDR", 0x0B: "UCSRA",
    0x0A: "UCSRB", 0x09: "UBRRL", 0x08: "ACSR", 0x07: "ADMUX",
    0x06: "ADCSRA", 0x05: "ADCH", 0x04: "ADCL", 0x03: "TWDR", 0x02: "TWAR",
    0x01: "TWSR", 0x00: "TWBR",
}
# ATmega8: ICR1 = 0x26/0x25 (data 0x46/0x45). 0x27 bestaat niet: opruimen.
IO.pop(0x27, None)

VEC_NAMES = ["RESET", "INT0", "INT1", "TIMER2_COMP", "TIMER2_OVF", "TIMER1_CAPT",
             "TIMER1_COMPA", "TIMER1_COMPB", "TIMER1_OVF", "TIMER0_OVF", "SPI_STC",
             "USART_RXC", "USART_UDRE", "USART_TXC", "ADC", "EE_RDY", "ANA_COMP",
             "TWI", "SPM_RDY"]

SREG_BITS = "CZNVSHTI"


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# (mask, value, name, fmt)  fmt-codes beschreven in fields()
PAT = [
    (0xFFFF, 0x0000, "nop", ""),
    (0xFF00, 0x0100, "movw", "W"),
    (0xFF00, 0x0200, "muls", "hh"),
    (0xFF88, 0x0300, "mulsu", "h3"),
    (0xFF88, 0x0308, "fmul", "h3"),
    (0xFF88, 0x0380, "fmuls", "h3"),
    (0xFF88, 0x0388, "fmulsu", "h3"),
    (0xFC00, 0x0400, "cpc", "dr"),
    (0xFC00, 0x0800, "sbc", "dr"),
    (0xFC00, 0x0C00, "add", "dr"),
    (0xFC00, 0x1000, "cpse", "dr"),
    (0xFC00, 0x1400, "cp", "dr"),
    (0xFC00, 0x1800, "sub", "dr"),
    (0xFC00, 0x1C00, "adc", "dr"),
    (0xFC00, 0x2000, "and", "dr"),
    (0xFC00, 0x2400, "eor", "dr"),
    (0xFC00, 0x2800, "or", "dr"),
    (0xFC00, 0x2C00, "mov", "dr"),
    (0xF000, 0x3000, "cpi", "dK"),
    (0xF000, 0x4000, "sbci", "dK"),
    (0xF000, 0x5000, "subi", "dK"),
    (0xF000, 0x6000, "ori", "dK"),
    (0xF000, 0x7000, "andi", "dK"),
    (0xD208, 0x8000, "ldd_z", "dq"),   # ld Rd,Z / ldd Rd,Z+q
    (0xD208, 0x8008, "ldd_y", "dq"),
    (0xD208, 0x8200, "std_z", "dq"),
    (0xD208, 0x8208, "std_y", "dq"),
    (0xFE0F, 0x9000, "lds", "d16"),
    (0xFE0F, 0x9001, "ld_zp", "d"),
    (0xFE0F, 0x9002, "ld_mz", "d"),
    (0xFE0F, 0x9004, "lpm_z", "d"),
    (0xFE0F, 0x9005, "lpm_zp", "d"),
    (0xFE0F, 0x9006, "elpm_z", "d"),
    (0xFE0F, 0x9007, "elpm_zp", "d"),
    (0xFE0F, 0x9009, "ld_yp", "d"),
    (0xFE0F, 0x900A, "ld_my", "d"),
    (0xFE0F, 0x900C, "ld_x", "d"),
    (0xFE0F, 0x900D, "ld_xp", "d"),
    (0xFE0F, 0x900E, "ld_mx", "d"),
    (0xFE0F, 0x900F, "pop", "d"),
    (0xFE0F, 0x9200, "sts", "d16"),
    (0xFE0F, 0x9201, "st_zp", "d"),
    (0xFE0F, 0x9202, "st_mz", "d"),
    (0xFE0F, 0x9209, "st_yp", "d"),
    (0xFE0F, 0x920A, "st_my", "d"),
    (0xFE0F, 0x920C, "st_x", "d"),
    (0xFE0F, 0x920D, "st_xp", "d"),
    (0xFE0F, 0x920E, "st_mx", "d"),
    (0xFE0F, 0x920F, "push", "d"),
    (0xFE0F, 0x9400, "com", "d"),
    (0xFE0F, 0x9401, "neg", "d"),
    (0xFE0F, 0x9402, "swap", "d"),
    (0xFE0F, 0x9403, "inc", "d"),
    (0xFE0F, 0x9405, "asr", "d"),
    (0xFE0F, 0x9406, "lsr", "d"),
    (0xFE0F, 0x9407, "ror", "d"),
    (0xFF8F, 0x9408, "bset", "s"),
    (0xFF8F, 0x9488, "bclr", "s"),
    (0xFFFF, 0x9409, "ijmp", ""),
    (0xFFFF, 0x9509, "icall", ""),
    (0xFFFF, 0x9508, "ret", ""),
    (0xFFFF, 0x9518, "reti", ""),
    (0xFFFF, 0x9588, "sleep", ""),
    (0xFFFF, 0x9598, "break", ""),
    (0xFFFF, 0x95A8, "wdr", ""),
    (0xFFFF, 0x95C8, "lpm", ""),
    (0xFFFF, 0x95E8, "spm", ""),
    (0xFE0F, 0x940A, "dec", "d"),
    (0xFE0E, 0x940C, "jmp", "k22"),
    (0xFE0E, 0x940E, "call", "k22"),
    (0xFF00, 0x9600, "adiw", "pK6"),
    (0xFF00, 0x9700, "sbiw", "pK6"),
    (0xFF00, 0x9800, "cbi", "Ab"),
    (0xFF00, 0x9900, "sbic", "Ab"),
    (0xFF00, 0x9A00, "sbi", "Ab"),
    (0xFF00, 0x9B00, "sbis", "Ab"),
    (0xFC00, 0x9C00, "mul", "dr"),
    (0xF800, 0xB000, "in", "dA"),
    (0xF800, 0xB800, "out", "Ad"),
    (0xF000, 0xC000, "rjmp", "k12"),
    (0xF000, 0xD000, "rcall", "k12"),
    (0xF000, 0xE000, "ldi", "dK"),
    (0xFC00, 0xF000, "brbs", "k7b"),
    (0xFC00, 0xF400, "brbc", "k7b"),
    (0xFE08, 0xF800, "bld", "db"),
    (0xFE08, 0xFA00, "bst", "db"),
    (0xFE08, 0xFC00, "sbrc", "db"),
    (0xFE08, 0xFE00, "sbrs", "db"),
]

TWO_WORD = {"lds", "sts", "jmp", "call"}


def decode(w, w2=0, pc=0):
    """Decodeer woord w (pc in WOORD-adres). Return dict of None bij ongeldig.
    Velden: name, size(woorden), ops(dict), target(woord-adres of None)."""
    for mask, val, name, fmt in PAT:
        if (w & mask) != val:
            continue
        o = {}
        d = (w >> 4) & 0x1F
        r = ((w >> 5) & 0x10) | (w & 0xF)
        if fmt in ("dr",):
            o["d"], o["r"] = d, r
        elif fmt == "dK":
            o["d"] = 16 + ((w >> 4) & 0xF)
            o["K"] = ((w >> 4) & 0xF0) | (w & 0xF)
        elif fmt == "d" or fmt == "d16":
            o["d"] = d
            if fmt == "d16":
                o["k"] = w2
        elif fmt == "dq":
            o["d"] = d
            o["q"] = ((w >> 8) & 0x20) | ((w >> 7) & 0x18) | (w & 7)
        elif fmt == "W":
            o["d"], o["r"] = ((w >> 4) & 0xF) * 2, (w & 0xF) * 2
        elif fmt == "hh":
            o["d"], o["r"] = 16 + ((w >> 4) & 0xF), 16 + (w & 0xF)
        elif fmt == "h3":
            o["d"], o["r"] = 16 + ((w >> 4) & 7), 16 + (w & 7)
        elif fmt == "s":
            o["s"] = (w >> 4) & 7
        elif fmt == "k22":
            o["k"] = (((w >> 3) & 0x3E) | (w & 1)) << 16 | w2
        elif fmt == "pK6":
            o["d"] = 24 + 2 * ((w >> 4) & 3)
            o["K"] = ((w >> 2) & 0x30) | (w & 0xF)
        elif fmt == "Ab":
            o["A"], o["b"] = (w >> 3) & 0x1F, w & 7
        elif fmt == "dA":
            o["d"], o["A"] = d, ((w >> 5) & 0x30) | (w & 0xF)
        elif fmt == "Ad":
            o["d"], o["A"] = d, ((w >> 5) & 0x30) | (w & 0xF)
        elif fmt == "k12":
            o["k"] = sx(w & 0xFFF, 12)
        elif fmt == "k7b":
            o["k"], o["b"] = sx((w >> 3) & 0x7F, 7), w & 7
        elif fmt == "db":
            o["d"], o["b"] = d, w & 7
        size = 2 if name in TWO_WORD else 1
        tgt = None
        if name in ("rjmp", "rcall"):
            tgt = pc + 1 + o["k"]
        elif name in ("brbs", "brbc"):
            tgt = pc + 1 + o["k"]
        elif name in ("jmp", "call"):
            tgt = o["k"]
        return {"name": name, "size": size, "ops": o, "target": tgt, "w": w, "w2": w2 if size == 2 else None}
    return None


def ioname(a):
    return IO.get(a, "IO%02X" % a)


BRS = ["brcs", "breq", "brmi", "brvs", "brlt", "brhs", "brts", "brie"]
BRC = ["brcc", "brne", "brpl", "brvc", "brge", "brhc", "brtc", "brid"]
BSET = ["sec", "sez", "sen", "sev", "ses", "seh", "set", "sei"]
BCLR = ["clc", "clz", "cln", "clv", "cls", "clh", "clt", "cli"]


def mnemonic_text(ins, sym=None, pc=0, numeric_io=False, as_syntax=False):
    """Tekstvorm in avr-as-syntax. sym(wordaddr)->label (optioneel)."""
    n, o = ins["name"], ins["ops"]
    R = lambda i: "r%d" % i

    def tg():
        t = ins["target"]
        if sym:
            s = sym(t)
            if s:
                return s
        if as_syntax:
            return ".%+d" % ((t - pc - 1) * 2)   # avr-as: zelfde als objdump (relatief t.o.v. volgende instructie)
        return ".%+d" % ((t - pc - 1) * 2)  # objdump-stijl: relatief t.o.v. volgende instructie

    def io(a):
        return "0x%02x" % a if numeric_io else ioname(a)
    if n in ("nop", "ijmp", "icall", "ret", "reti", "sleep", "break", "wdr", "lpm", "spm"):
        return n
    if n == "movw":
        return "movw r%d, r%d" % (o["d"], o["r"])
    if n in ("muls", "mulsu", "fmul", "fmuls", "fmulsu", "cpc", "sbc", "add", "cpse", "cp", "sub",
             "adc", "and", "eor", "or", "mov", "mul"):
        return "%s %s, %s" % (n, R(o["d"]), R(o["r"]))
    if n in ("cpi", "sbci", "subi", "ori", "andi", "ldi"):
        return "%s %s, 0x%02x" % (n, R(o["d"]), o["K"])
    if n.startswith("ldd_") or n.startswith("std_"):
        yz = "Y" if n.endswith("y") else "Z"
        ptr = yz if o["q"] == 0 else "%s+%d" % (yz, o["q"])
        if n.startswith("ldd"):
            return "ld %s, %s" % (R(o["d"]), ptr) if o["q"] == 0 else "ldd %s, %s" % (R(o["d"]), ptr)
        return "st %s, %s" % (ptr, R(o["d"])) if o["q"] == 0 else "std %s, %s" % (ptr, R(o["d"]))
    if n == "lds":
        return "lds %s, 0x%04x" % (R(o["d"]), o["k"])
    if n == "sts":
        return "sts 0x%04x, %s" % (o["k"], R(o["d"]))
    m = {"ld_zp": "ld %s, Z+", "ld_mz": "ld %s, -Z", "lpm_z": "lpm %s, Z", "lpm_zp": "lpm %s, Z+",
         "elpm_z": "elpm %s, Z", "elpm_zp": "elpm %s, Z+", "ld_yp": "ld %s, Y+", "ld_my": "ld %s, -Y",
         "ld_x": "ld %s, X", "ld_xp": "ld %s, X+", "ld_mx": "ld %s, -X", "pop": "pop %s",
         "com": "com %s", "neg": "neg %s", "swap": "swap %s", "inc": "inc %s", "asr": "asr %s",
         "lsr": "lsr %s", "ror": "ror %s", "dec": "dec %s", "push": "push %s"}
    if n in m:
        return m[n] % R(o["d"])
    m2 = {"st_zp": "st Z+, %s", "st_mz": "st -Z, %s", "st_yp": "st Y+, %s", "st_my": "st -Y, %s",
          "st_x": "st X, %s", "st_xp": "st X+, %s", "st_mx": "st -X, %s"}
    if n in m2:
        return m2[n] % R(o["d"])
    if n == "bset":
        return BSET[o["s"]]
    if n == "bclr":
        return BCLR[o["s"]]
    if n in ("jmp", "call"):
        return "%s 0x%x" % (n, o["k"] * 2)
    if n in ("adiw", "sbiw"):
        return "%s r%d, %d" % (n, o["d"], o["K"])
    if n in ("cbi", "sbic", "sbi", "sbis"):
        return "%s %s, %d" % (n, io(o["A"]), o["b"])
    if n == "in":
        return "in %s, %s" % (R(o["d"]), io(o["A"]))
    if n == "out":
        return "out %s, %s" % (io(o["A"]), R(o["d"]))
    if n in ("rjmp", "rcall"):
        return "%s %s" % (n, tg())
    if n == "brbs":
        return "%s %s" % (BRS[o["b"]], tg())
    if n == "brbc":
        return "%s %s" % (BRC[o["b"]], tg())
    if n in ("bld", "bst", "sbrc", "sbrs"):
        return "%s %s, %d" % (n, R(o["d"]), o["b"])
    raise ValueError(n)


def encode(ins):
    """Terug naar opcode-woord(en) uit een decode()-dict (onafhankelijk van tekst).
    Gebruikt de PAT-tabel en veldindeling in omgekeerde richting."""
    n, o = ins["name"], ins["ops"]
    for mask, val, name, fmt in PAT:
        if name != n:
            continue
        w = val
        if fmt == "dr":
            w |= ((o["d"] & 0x1F) << 4) | ((o["r"] & 0x10) << 5) | (o["r"] & 0xF)
        elif fmt == "dK":
            w |= ((o["d"] - 16) << 4) | ((o["K"] & 0xF0) << 4) | (o["K"] & 0xF)
        elif fmt in ("d", "d16"):
            w |= o["d"] << 4
        elif fmt == "dq":
            q = o["q"]
            w |= (o["d"] << 4) | ((q & 0x20) << 8) | ((q & 0x18) << 7) | (q & 7)
        elif fmt == "W":
            w |= ((o["d"] // 2) << 4) | (o["r"] // 2)
        elif fmt == "hh":
            w |= ((o["d"] - 16) << 4) | (o["r"] - 16)
        elif fmt == "h3":
            w |= ((o["d"] - 16) << 4) | (o["r"] - 16)
        elif fmt == "s":
            w |= o["s"] << 4
        elif fmt == "k22":
            k = o["k"]
            w |= ((k >> 16) & 0x3E) << 3 | ((k >> 16) & 1)
        elif fmt == "pK6":
            w |= (((o["d"] - 24) // 2) << 4) | ((o["K"] & 0x30) << 2) | (o["K"] & 0xF)
        elif fmt == "Ab":
            w |= (o["A"] << 3) | o["b"]
        elif fmt == "dA":
            w |= (o["d"] << 4) | ((o["A"] & 0x30) << 5) | (o["A"] & 0xF)
        elif fmt == "Ad":
            w |= (o["d"] << 4) | ((o["A"] & 0x30) << 5) | (o["A"] & 0xF)
        elif fmt == "k12":
            w |= o["k"] & 0xFFF
        elif fmt == "k7b":
            w |= ((o["k"] & 0x7F) << 3) | o["b"]
        elif fmt == "db":
            w |= (o["d"] << 4) | o["b"]
        out = [w]
        if ins["size"] == 2:
            out.append(o["k"] & 0xFFFF if fmt == "d16" else ins["w2"])
        return out
    raise ValueError(n)


def load_words(path, nbytes=8192):
    d = open(path, "rb").read()
    return [d[i] | (d[i + 1] << 8) for i in range(0, nbytes, 2)]
