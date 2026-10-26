#!/usr/bin/env python3
"""Verifieert (a) avrlib.decode/encode en (b) avrdis.py tegen GNU avr-objdump
voor ALLE 65536 opcodes (met 0x0000 als tweede woord).
Uitvoer: verify_decoder.txt (samenvatting + verschillen)."""
import os, re, subprocess, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import avrlib
import avrdis

OBJDUMP = r"C:\Program Files (x86)\Arduino\hardware\tools\avr\bin\avr-objdump.exe"
TMP = os.path.join(HERE, "_allops.bin")

# binaire: voor opcode w: [w, 0x0000]; adres = 4*w
with open(TMP, "wb") as f:
    for w in range(65536):
        f.write(struct.pack("<HH", w, 0))
out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "avr:4", TMP],
                     capture_output=True, text=True).stdout
ref = {}   # w -> (text, size_words)
rows = []
for line in out.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)", line)
    if m:
        rows.append((int(m.group(1), 16), m.group(3).split(";")[0].strip(), len(m.group(2).split())))
# 1-woord: er volgt een entry op adres+2 (de nop) ; 2-woord: niet
byaddr = {a: (t, n) for a, t, n in rows}
for w in range(65536):
    a = 4 * w
    t, n = byaddr[a]
    ref[w] = (t, n // 2)
def norm(t):
    t = t.replace("\t", " ")
    t = re.sub(r"\s+", " ", t).strip().lower()
    t = t.replace(" ,", ",").replace(", ", ",")
    # getallen -> decimaal
    def num(m):
        s = m.group(0)
        return str(int(s, 16)) if s.startswith("0x") else s
    t = re.sub(r"0x[0-9a-f]+", num, t)
    return t

ALIAS_SAME = {
    "lsl": "add", "rol": "adc", "tst": "and", "clr": "eor",
}
def canon(t):
    """Maak aliasen gelijk: lsl rX -> add rX,rX enz."""
    t = norm(t)
    m = re.match(r"(lsl|rol|tst|clr) (r\d+)$", t)
    if m:
        return "%s %s,%s" % (ALIAS_SAME[m.group(1)], m.group(2), m.group(2))
    m = re.match(r"ser (r\d+)$", t)
    if m:
        return "ldi %s,255" % m.group(1)
    m = re.match(r"(sbr|cbr) (r\d+),(\d+)$", t)
    if m:
        if m.group(1) == "sbr":
            return "ori %s,%s" % (m.group(2), m.group(3))
        return "andi %s,%d" % (m.group(2), (~int(m.group(3))) & 0xFF)
    return t

bad_mine, bad_dis, bad_rt = [], [], []
undef_ref = 0
for w in range(65536):
    rt, rsz = ref[w]
    isundef = rt.startswith(".word") or rt.startswith("???")
    if isundef:
        undef_ref += 1
    ins = avrlib.decode(w, 0, 0)
    # --- eigen decoder ---
    if ins is None:
        if not isundef and not rt.startswith("jmp") and not rt.startswith("call"):
            bad_mine.append((w, "None", rt))
    else:
        if isundef:
            bad_mine.append((w, avrlib.mnemonic_text(ins, numeric_io=True), rt))
        else:
            mt = avrlib.mnemonic_text(ins, pc=0, numeric_io=True)
            if canon(mt) != canon(rt) or ins["size"] != rsz:
                bad_mine.append((w, mt, rt))
            # round-trip encode
            enc = avrlib.encode(ins)
            if enc[0] != w:
                bad_rt.append((w, enc[0]))
    # --- avrdis.py ---
    mn, opd, size, tgt = avrdis.decode(w, 0, 0)
    dtxt = (mn + " " + opd).strip()
    dtxt = dtxt.split(";")[0].strip()
    if isundef:
        if mn != ".WORD":
            bad_dis.append((w, dtxt, rt))
    else:
        d2 = canon(dtxt)
        r2 = canon(rt)
        # avrdis schrijft CBI(0x..), IO-namen: haal naam weg
        d2 = re.sub(r"[a-z0-9_/]+\((0x[0-9a-f]+)\)", lambda m: m.group(1), d2)
        d2 = norm(d2)
        d2 = re.sub(r'movw (r\d+):r\d+,(r\d+):r\d+', r'movw ,', d2)
        # relatieve sprongen: avrdis print ".+40" = zelfde als objdump
        if d2 != r2 or size != rsz:
            bad_dis.append((w, dtxt, rt))

with open(os.path.join(HERE, "verify_decoder.txt"), "w", encoding="utf-8") as f:
    f.write("Verificatie van decoders tegen avr-objdump (binutils 2.26, -m avr:4) over alle 65536 opcodes\n")
    f.write("objdump: %d opcodes ongedefinieerd (.word)\n" % undef_ref)
    f.write("avrlib.decode verschillen: %d\n" % len(bad_mine))
    f.write("avrlib encode round-trip fouten: %d\n" % len(bad_rt))
    f.write("avrdis.py verschillen: %d\n\n" % len(bad_dis))
    f.write("--- avrlib verschillen (w, mijn, objdump) ---\n")
    for x in bad_mine:
        f.write("%04x | %s | %s\n" % x)
    f.write("\n--- avrdis.py verschillen (w, avrdis, objdump) ---\n")
    # groepeer
    for x in bad_dis:
        f.write("%04x | %s | %s\n" % x)
print("undef", undef_ref, "mine", len(bad_mine), "rt", len(bad_rt), "avrdis", len(bad_dis))
os.remove(TMP)
