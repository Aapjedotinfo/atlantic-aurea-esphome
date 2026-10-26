"""Systematisch bewijs (door uitputtende emulatie) dat sub_083a (0x083a) nooit de weestelegrammen op 0x0baa/0x0bb4 uitleest.

sub_083a is de enige routine die via LPM tabelbytes ophaalt (plus de vaste LPM op 0x0752 met Z=0x0bbe).
Alle toestandsvariabelen die het Z-adres bepalen, worden volledig doorgelopen:
  [0x6d] (r17): alle 256 waarden (alleen bits 0,1,7 worden getest; we draaien 8 bitcombinaties x 2 vulwaarden voor de overige bits)
  [0x6c] (r18): alle 256 waarden
  [0x6e] (stand): alle 256 waarden
  [0x6a]: 0 (enige tak met LPM) en een aantal niet-nul waarden (bewijs: geen LPM)
  [0x68],[0x69],PINC.4 : 2-3 representatieve waarden (zij sturen alleen tijd/gap-takken, geen Z)
De routine wordt als subroutine aangeroepen (terugkeeradres = sentinel), zonder randapparatuur.
Resultaat: alle bereikte LPM-adressen; vergelijking met de tabelgebieden.
"""
import os, sys, itertools, time
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu, harness as H

SENT = 0x0F00 // 2          # sentinel in leeg flashgebied (voor ret)
c = H.mk("M1.1")
c.raw_io = False
lpm_all = {}
zbyseg = {}
runs = 0
t0 = time.time()


def call(pc_word=0x083a // 2, maxsteps=3000):
    c.sp = 0x45F
    c.push(SENT & 0xFF)
    c.push(SENT >> 8)
    c.pc = pc_word
    n = 0
    while c.pc != SENT and n < maxsteps:
        c.dec[c.pc](c)
        n += 1
    return n


# vul alleen wat nodig is
mem = c.mem
flags_vals = []
for bits in itertools.product((0, 1), repeat=3):
    for fill in (0x00, 0x7C):
        flags_vals.append((bits[0] | (bits[1] << 1) | (bits[2] << 7)) | fill)
steps_max = 0
for fv in flags_vals:
    for v6c in range(256):
        for v6e in range(256):
            c.lpm_log = {}
            c.portlog.clear()
            mem[0x6d] = fv
            mem[0x6c] = v6c
            mem[0x6e] = v6e
            mem[0x6a] = 0
            mem[0x68] = 1
            mem[0x69] = 0x80
            c.ext_drv[emu.PINC] = 0x10
            c.ext_val[emu.PINC] = 0x10
            n = call()
            steps_max = max(steps_max, n)
            runs += 1
            for z in c.lpm_log:
                lpm_all[z] = lpm_all.get(z, 0) + 1
# niet-nul [0x6a]: nooit LPM
nonzero_lpm = 0
for fv in flags_vals[:8]:
    for v6a in (1, 2, 5, 8, 0x80, 0xFF):
        for v6c in (0, 1, 10, 45, 255):
            c.lpm_log = {}
            mem[0x6d] = fv; mem[0x6c] = v6c; mem[0x6e] = 20; mem[0x6a] = v6a; mem[0x68] = 0; mem[0x69] = 0
            for pinc4 in (0, 1):
                c.ext_val[emu.PINC] = pinc4 << 4
                call()
                nonzero_lpm += len(c.lpm_log)
lo, hi = min(lpm_all), max(lpm_all)
orph = [z for z in lpm_all if 0x0baa <= z <= 0x0bbd]
below = [z for z in lpm_all if z < 0x0bbe]
in_tbl = sorted(z for z in lpm_all if 0x0bbe <= z <= 0x0c4f)
out_tbl = sorted(z for z in lpm_all if z > 0x0c4f)
rep = []
rep.append("Uitputtende emulatie van sub_083a: %d aanroepen in %.0f s (max %d instructies per aanroep)" % (runs, time.time() - t0, steps_max))
rep.append("Aangeroepen met alle 256 x 256 combinaties van [0x6c] x [0x6e] en %d vlag-bytes [0x6d]; [0x6a]=0" % len(flags_vals))
rep.append("Aantal verschillende LPM-adressen: %d ; laagste Z = 0x%04x ; hoogste Z = 0x%04x" % (len(lpm_all), lo, hi))
rep.append("LPM-adressen in 0x0baa..0x0bbd (weestelegrammen): %s" % ([hex(z) for z in orph] or "GEEN"))
rep.append("LPM-adressen onder 0x0bbe: %s" % ([hex(z) for z in below] or "GEEN"))
rep.append("LPM-adressen binnen tabelgebied 0x0bbe..0x0c4f: %d van de 146" % len(in_tbl))
rep.append("  waarvan nooit gelezen binnen dat gebied: %s" % ([hex(z) for z in range(0x0bbe, 0x0c50) if z not in lpm_all] or "geen"))
rep.append("LPM-adressen boven 0x0c4f (leeg 0xFF-flash; alleen bij [0x6e]>>2 > 10): %d adressen, tot 0x%04x" % (len(out_tbl), max(out_tbl) if out_tbl else 0))
rep.append("Met [0x6a] <> 0: totaal %d LPM's (verwacht 0)" % nonzero_lpm)
out = "\n".join(rep)
open(os.path.join(HERE, "e4_lpm_sweep.txt"), "w", encoding="utf-8").write(out + "\n")
print(out)
