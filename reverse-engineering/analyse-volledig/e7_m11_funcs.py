"""Functie-niveau experimenten op M1.1: elke regelroutine wordt als subroutine aangeroepen met voorgeschreven
RAM/registers/pinnen (zonder timers), zodat de beslisfuncties volledig worden getabelleerd.
Uitvoer: e7_m11_funcs.txt
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu, harness as H

SENT = 0x780


def newcpu():
    c = H.mk("M1.1")
    return c


def call(c, entry, maxsteps=20000):
    c.sp = 0x45F
    c.push(SENT & 0xFF)
    c.push(SENT >> 8)
    c.pc = entry // 2
    n = 0
    while c.pc != SENT and n < maxsteps:
        c.dec[c.pc](c)
        n += 1
    return n


def setstate(c, **kw):
    m = c.mem
    for k, v in kw.items():
        if k.startswith("m"):
            m[int(k[1:], 16)] = v
        elif k.startswith("r"):
            c.r[int(k[1:])] = v
    return c


out = []
P = lambda *a: out.append(" ".join(str(x) for x in a))
# ------------------------------------------------------------------ A. stand_regelaar (0x04be): doelwaarde-klem en start/stop
P("== A. stand_regelaar 0x04be: invloed [0x6f] (gevraagde aanvoertemperatuur van de thermostaat via T1.1)")
P("   voorwaarden: r23=0 (niet geblokkeerd), [0x6d]=0 (TX stil), flowtemp [0x71]=30, relais (PB0) uit -> start; of aan -> geen start")
P("[0x6f] | relais uit: PB0 / [0x70] / [0x6e] / [0x65] / r23 / r24 | relais aan: PB0 / [0x70] / [0x6e] / [0x65] / r24.0")
for v in (0, 19, 20, 33, 34, 40, 54, 55, 56, 60, 61, 62, 100, 255):
    res = []
    for relay in (0, 1):
        c = newcpu()
        c.io[emu.PORTB] = 0x0a | relay
        c.io[emu.DDRB] = 0x1f
        setstate(c, m6f=v, m6d=0, m71=30, m65=0x80, m6e=(20 if relay else 0), m64=5, r23=0, r24=0)
        call(c, 0x04be)
        pb0 = c.io[emu.PORTB] & 1
        res.append("PB0=%d [70]=%02x [6e]=%02x [65]=%02x r23=%02x r24=%02x" % (pb0, c.mem[0x70], c.mem[0x6e], c.mem[0x65], c.r[23], c.r[24]))
    P("%02x(%3d) | %s | %s" % (v, v, res[0], res[1]))

# ------------------------------------------------------------------ B. regelstap: aanvoer versus doel
P("")
P("== B. stand_regelaar: regelstap op [0x6e] (relais aan, doel 45, [0x64] groot genoeg zodat een stap wordt gezet)")
P("flow [0x71] | [0x6e] voor -> na  (r23.1 = 0, [0x64]=0xfa) | [0x6e] voor -> na (r23.1=1, [0x64]=0x1e)")
for flow in (20, 30, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 55):
    res = []
    for mode in (0, 1):
        for cur in (14, 20, 36, 37, 38, 39, 40):
            pass
    line = []
    for cur in (14, 36, 38, 39, 40):
        r = []
        for mode in (0, 1):
            c = newcpu()
            c.io[emu.PORTB] = 0x0b
            c.io[emu.DDRB] = 0x1f
            setstate(c, m6f=45, m6d=0, m71=flow, m65=0x80, m6e=cur, m64=(0xfa if mode == 0 else 0x1e), r23=(0 if mode == 0 else 2), r24=0)
            call(c, 0x04be)
            r.append("%02x" % c.mem[0x6e])
        line.append("%02x->%s" % (cur, "/".join(r)))
    P("flow=%3d | %s" % (flow, "  ".join(line)))

# ------------------------------------------------------------------ C. bewaking_65_lockout (0x062e): 5-graden-regel
P("")
P("== C. bewaking_65_lockout 0x062e: buitentemperatuur-regel (SRAM [0x73], 8-bit; conditie: [0x73] >= 200 (negatief) of [0x73] < 5)")
P("[0x73] | relais uit,[0x65]=0xb4 | relais aan,[0x65]=0xb4 | relais aan,[0x65]=0x50 | relais uit,[0x65]=0x50")
for v in (0, 1, 3, 4, 5, 6, 20, 100, 199, 200, 201, 250, 255):
    cols = []
    for relay, h in ((0, 0xb4), (1, 0xb4), (1, 0x50), (0, 0x50)):
        c = newcpu()
        c.io[emu.PORTB] = 0x0a | relay
        c.io[emu.DDRB] = 0x1f
        setstate(c, m73=v, m6d=0, m65=h, m6f=45, m70=45, m71=40, m6e=(20 if relay else 0), r23=0, r24=0, m60=1)
        c.mem[0xa3] = 0
        call(c, 0x062e)
        cols.append("[65]=%02x r23=%02x PB0=%d [6e]=%02x f0=%s" % (c.mem[0x65], c.r[23], c.io[emu.PORTB] & 1, c.mem[0x6e], "ja" if c.mem[0xa4] == 0xf0 else "nee"))
    P("%02x(%3d) | %s" % (v, v, " | ".join(cols)))
P("   (f0 = byte 0 van het frame naar T1.1 is 0xF0 gezet; alleen als r23.5 het frame 'klaar' meldt, hier r23.5=0 dus niet getoond)")

# ------------------------------------------------------------------ C2. zelfde maar met r23.5=1
P("")
P("== C2. idem met r23.5=1 (frame-buffer klaar) om het F0-pad te zien")
for v in (4, 5):
    for relay, h in ((0, 0xb4), (1, 0xb4), (1, 0x50)):
        c = newcpu()
        c.io[emu.PORTB] = 0x0a | relay
        c.io[emu.DDRB] = 0x1f
        setstate(c, m73=v, m6d=0, m65=h, m6f=45, m70=45, m71=40, m6e=(20 if relay else 0), r23=0x20, r24=0)
        call(c, 0x062e)
        P("[0x73]=%d relais=%d [0x65] in=%02x -> [0x65]=%02x r23=%02x PB0=%d [6e]=%02x frame-byte0=%02x" % (v, relay, h, c.mem[0x65], c.r[23], c.io[emu.PORTB] & 1, c.mem[0x6e], c.mem[0xa4]))

# ------------------------------------------------------------------ D. schakelaars
P("")
P("== D. sw_cv_only_PC0 (0x02ae) en btn_test_PC1 (0x02d2)")
for pc0 in (0, 1):
    c = newcpu()
    c.io[emu.PORTB] = 0x0b; c.io[emu.DDRB] = 0x1f
    H.set_pin(c, emu.PINC, 0, pc0)
    setstate(c, m6d=0, m65=0x80, m6e=20, r23=2)
    call(c, 0x02ae)
    P("PC0=%d: PB0=%d [6e]=%02x [65]=%02x r23=%02x" % (pc0, c.io[emu.PORTB] & 1, c.mem[0x6e], c.mem[0x65], c.r[23]))
for pc1 in (0, 1):
    for spd in (0x13, 0x14):
        c = newcpu()
        c.io[emu.PORTB] = 0x0a; c.io[emu.DDRB] = 0x1f
        H.set_pin(c, emu.PINC, 1, pc1)
        setstate(c, m6d=0, m6f=spd, m65=0x30, m6e=0, r23=1)
        call(c, 0x02d2)
        P("PC1=%d [0x6f]=%02x: PB0=%d [6e]=%02x [65]=%02x [64]=%02x r23=%02x" % (pc1, spd, c.io[emu.PORTB] & 1, c.mem[0x6e], c.mem[0x65], c.mem[0x64], c.r[23]))

# ------------------------------------------------------------------ E. seconden_watchdog_66 (0x072e)
P("")
P("== E. seconden_watchdog_66 0x072e: [0x66] = seconden sinds laatste ID1-antwoord")
for v in (0, 0x18, 0x19, 0x1a, 0x1d, 0x1e, 0x1f, 0x28, 0xff):
    for r235 in (0, 0x20):
        c = newcpu()
        c.io[emu.PORTB] = 0x0b; c.io[emu.DDRB] = 0x1f
        setstate(c, m66=v, m67=0, m6e=20, m74=0, m6d=0x0c, m6c=7, r23=r235)
        call(c, 0x072e)
        P("[0x66]=%02x r23.5=%d -> [0x66]=%02x [0x6d]=%02x [0x6c]=%02x [0x6e]=%02x [0x74]=%02x PB0=%d r23=%02x r20=%02x" % (v, 1 if r235 else 0, c.mem[0x66], c.mem[0x6d], c.mem[0x6c], c.mem[0x6e], c.mem[0x74], c.io[emu.PORTB] & 1, c.r[23], c.r[20]))
P("")
P("== F. seconden_watchdog_66: [0x67] (seconden sinds laatste geldige T1.1-frame)")
for v in (0, 0x1d, 0x1e, 0xe0, 0xef, 0xf0, 0xf1, 0xff):
    c = newcpu()
    c.io[emu.PORTB] = 0x0b; c.io[emu.DDRB] = 0x1f
    setstate(c, m66=0, m67=v, m6e=20, m6f=45, m74=0, m6d=0, r23=0x20)
    call(c, 0x072e)
    P("[0x67]=%02x -> [0x67]=%02x [0x6f]=%02x [0x6e]=%02x PB0=%d PB4=%d [0x74]=%02x [0x73]=%02x" % (v, c.mem[0x67], c.mem[0x6f], c.mem[0x6e], c.io[emu.PORTB] & 1, (c.io[emu.PORTB] >> 4) & 1, c.mem[0x74], c.mem[0x73]))

open(os.path.join(HERE, "e7_m11_funcs.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
print("\n".join(out))
