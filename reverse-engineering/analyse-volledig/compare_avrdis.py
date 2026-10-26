#!/usr/bin/env python3
"""Vergelijkt de instructiekaart van avrdis.py (bestaande .lst) met de eigen trace."""
import os, sys, re, json
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
import analyze as Z, avrlib as A, avrdis, verify_helpers as VH
out = []
for chip in ("M1.1", "T1.1"):
    flash, ee = Z.read_flash(chip)
    words = Z.words_of(flash)
    live, _ = Z.trace(words, Z.seeds_for(words))
    # avrdis zelf draaien (niet het .lst-bestand vertrouwen)
    w2, insns, code, calls, xref = avrdis.trace_disassemble(flash)
    mine = set(live)
    theirs = set(insns)
    only_mine = sorted(mine - theirs)
    only_theirs = sorted(theirs - mine)
    # mnemonic-vergelijking op gemeenschappelijke adressen
    diff = []
    for a in sorted(mine & theirs):
        mn, opd, size, tgt = insns[a]
        mt = A.mnemonic_text(live[a], None, a, numeric_io=True)
        t = (mn + " " + opd).split(";")[0].strip()
        t = re.sub(r"[A-Za-z0-9_/]+\((0x[0-9a-f]+)\)", lambda m: m.group(1), t)
        t = re.sub(r"(movw|adiw|sbiw) (r\d+):r\d+", r"\1 \2", t, flags=re.I)
        t = re.sub(r"(,\s*r\d+):r\d+", r"\1", t)
        if VH.canon(mt) != VH.canon(t) or size != live[a]["size"]:
            diff.append((a * 2, mt, t))
    out.append("== %s: eigen live-instructies %d, avrdis %d; alleen eigen: %s; alleen avrdis: %s; mnemonic-afwijkingen: %d" % (
        chip, len(mine), len(theirs), [hex(a * 2) for a in only_mine][:10], [hex(a * 2) for a in only_theirs][:10], len(diff)))
    for d in diff[:15]:
        out.append("   %04x mijn=%s avrdis=%s" % d)
open(os.path.join(HERE, "vergelijk_avrdis.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
print("\n".join(out))
