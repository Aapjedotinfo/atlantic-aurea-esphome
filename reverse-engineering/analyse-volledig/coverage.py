#!/usr/bin/env python3
"""coverage.py - dekkingskaart + round-trip per chip.
Uitvoer: dekking_<chip>.csv, listing_<chip>.txt (live+dode code), roundtrip_<chip>.txt.
Round-trip methoden:
  A) avrlib.encode(decode(w)) == w voor elke instructie (eigen encoder)
  B) tekst -> avr-as (GNU binutils) -> bin == BIN (onafhankelijk gereedschap)
  C) avr-objdump lineaire sweep == eigen decode (tekstvergelijking per instructie)
"""
import os, sys, json, subprocess, re
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import avrlib as A, analyze as Z
import verify_helpers as VH
BIN = r"C:\Program Files (x86)\Arduino\hardware\tools\avr\bin"
AS, OC, OD, LD = [os.path.join(BIN, x) for x in ("avr-as.exe", "avr-objcopy.exe", "avr-objdump.exe", "avr-ld.exe")]

TABLES = {
    "M1.1": [
        (0x0baa, 0x0bb3, "data-tabel", "weestelegram 1: 19 02 08 00 FF 00 9E C9 (10-bit-framing, 10 B)"),
        (0x0bb4, 0x0bbd, "data-tabel", "weestelegram 2: 19 03 08 00 00 00 37 67 (10 B)"),
        (0x0bbe, 0x0bcc, "data-tabel", "segment A: ID1 19 01 0C ... (15 B)"),
        (0x0bcd, 0x0bcd, "data-tabel", "opvulbyte 0x00 tussen segment A en C (nooit gelezen)"),
        (0x0bce, 0x0bd7, "data-tabel", "segment C: ID3 19 03 08 B2 02 00 C1 9A (10 B)"),
        (0x0bd8, 0x0be1, "data-tabel", "segment D: ID0 19 00 08 00 00 00 D9 B5 (10 B)"),
        (0x0be2, 0x0c4f, "data-tabel", "segment B: ID2-varianten, 11 x 10 B, index = stand/4"),
    ],
    "T1.1": [],
}
VEC = ["RESET", "INT0", "INT1", "TIMER2_COMP", "TIMER2_OVF", "TIMER1_CAPT", "TIMER1_COMPA", "TIMER1_COMPB",
       "TIMER1_OVF", "TIMER0_OVF", "SPI_STC", "USART_RXC", "USART_UDRE", "USART_TXC", "ADC", "EE_RDY",
       "ANA_COMP", "TWI", "SPM_RDY"]


def run(chip):
    flash, ee = Z.read_flash(chip)
    words = Z.words_of(flash)
    seeds = Z.seeds_for(words)
    live, xref = Z.trace(words, seeds)
    extra = json.load(open(os.path.join(HERE, "extra_seeds.json")))[chip]
    allins, xref2 = Z.trace(words, seeds + [e // 2 for e in extra])
    dead = {a: i for a, i in allins.items() if a not in live}
    cat = [None] * 8192
    desc = [""] * 8192
    for v in range(19):
        w = words[v]
        c = "vector" if (w & 0xF000) == 0xC000 else "vector-leeg"
        for k in (0, 1):
            cat[v * 2 + k] = c
            desc[v * 2 + k] = "vector %d %s" % (v, VEC[v])
    for a, i in live.items():
        if a < 19:
            continue
        for k in range(i["size"] * 2):
            cat[a * 2 + k] = "code-live"
    for a, i in dead.items():
        for k in range(i["size"] * 2):
            cat[a * 2 + k] = "code-dood"
    for s, e, c, d in TABLES[chip]:
        for b in range(s, e + 1):
            cat[b] = c
            desc[b] = d
    for b in range(8192):
        if cat[b] is None:
            cat[b] = "vrij-0xFF" if flash[b] == 0xFF else "ONVERKLAARD"
        if cat[b] in ("vector-leeg", "vrij-0xFF"):
            assert flash[b] == 0xFF
    # --- A) eigen encoder round-trip
    img = bytearray(b"\xff" * 8192)
    all_i = {**live, **dead}
    for a, i in all_i.items():
        if i.get("bad"):
            continue
        enc = A.encode(i)
        for k, w in enumerate(enc):
            img[(a + k) * 2] = w & 0xFF
            img[(a + k) * 2 + 1] = w >> 8
    for b in range(8192):
        if cat[b] == "data-tabel":
            img[b] = flash[b]
    diffA = [b for b in range(8192) if img[b] != flash[b]]
    A_ok = not diffA
    # --- B) avr-as
    lines = [".text", ".global _s", "_s:"]
    n = len(words)
    a = 0
    while a < n:
        if a in all_i and not all_i[a].get("bad") and not (a < 19 and cat[a * 2] != "vector"):
            i = all_i[a]
            lines.append("  " + A.mnemonic_text(i, None, a, numeric_io=True, as_syntax=True))
            a += i["size"]
        elif a < 19 and cat[a * 2] == "vector":
            lines.append("  " + A.mnemonic_text(A.decode(words[a], 0, a), None, a, numeric_io=True, as_syntax=True))
            a += 1
        else:
            lines.append("  .word 0x%04x" % words[a])
            a += 1
    tag = chip.replace(".", "")
    sfile = os.path.join(HERE, "_rt_%s.s" % tag)
    open(sfile, "w").write("\n".join(lines) + "\n")
    ofile = sfile[:-2] + ".o"
    bfile = sfile[:-2] + ".bin"
    r1 = subprocess.run([AS, "-mmcu=atmega8", "-o", ofile, sfile], capture_output=True, text=True)
    efile = sfile[:-2] + ".elf"
    r3 = subprocess.run([LD, "-m", "avr4", "-Ttext=0", "-o", efile, ofile], capture_output=True, text=True)
    subprocess.run([OC, "-O", "binary", "-j", ".text", efile, bfile], capture_output=True, text=True)
    asm = open(bfile, "rb").read()
    diffB = [b for b in range(min(len(asm), 8192)) if asm[b] != flash[b]]
    B_ok = (asm == flash)
    # --- C) objdump lineair
    od = subprocess.run([OD, "-D", "-b", "binary", "-m", "avr:4", Z.CHIPS[chip]], capture_output=True, text=True).stdout
    odmap = {}
    for l in od.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)", l)
        if m:
            odmap[int(m.group(1), 16)] = m.group(3).split(";")[0].strip()
    nC = 0
    badC = []
    for a, i in all_i.items():
        if i.get("bad"):
            continue
        mt = A.mnemonic_text(i, None, a, numeric_io=True)
        ot = odmap.get(a * 2)
        if ot is None:
            continue
        nC += 1
        if VH.canon(mt) != VH.canon(ot):
            badC.append((a * 2, mt, ot))
    # --- tekstlisting
    txt = Z.build_listing(chip, words, all_i, xref2, None)
    out = []
    for ln in txt.split("\n"):
        m = re.match(r"([0-9a-f]{4})  ", ln)
        if m and (int(m.group(1), 16) // 2) in dead:
            ln = ln + "   ; [DOOD: onbereikbaar]"
        out.append(ln)
    open(os.path.join(HERE, "listing_%s.txt" % tag), "w", encoding="utf-8").write("\n".join(out))
    # --- CSV
    rows = []
    s = 0
    for b in range(1, 8193):
        if b == 8192 or (cat[b], desc[b]) != (cat[s], desc[s]):
            rows.append((s, b - 1, cat[s], desc[s]))
            s = b
    blank = (ee == b"\xff" * 512)
    rows.append((0x2000, 0x21FF, "eeprom-leeg" if blank else "EEPROM-INHOUD", "512 B alle 0xFF" if blank else "?"))
    with open(os.path.join(HERE, "dekking_%s.csv" % tag), "w", encoding="utf-8") as f:
        f.write("start;einde;bytes;categorie;omschrijving\n")
        for s, e, c, d in rows:
            f.write("0x%04x;0x%04x;%d;%s;%s\n" % (s, e, e - s + 1, c, d))
    tot = {}
    for c in cat:
        tot[c] = tot.get(c, 0) + 1
    rep = ["Chip %s" % chip,
           "Flash 8192 B, telling per categorie (bytes): %s" % json.dumps(tot, ensure_ascii=False),
           "Live instructies: %d ; dode instructies: %d" % (len(live), len(dead)),
           "ONVERKLAARD bytes: %d" % tot.get("ONVERKLAARD", 0),
           "Round-trip A (eigen encoder, byte voor byte): %s (verschillen %d)" % ("OK" if A_ok else "FOUT", len(diffA)),
           "Round-trip B (avr-as assembleert listing, byte voor byte): %s (lengte %d, verschillen %d) stderr=%s%s" % (
               "OK" if B_ok else "FOUT", len(asm), len(diffB), r1.stderr.strip()[:200], r3.stderr.strip()[:200]),
           "Vergelijking C (avr-objdump vs eigen decode): %d instructies, %d afwijkingen" % (nC, len(badC))
           ] + ["   afw %04x: mijn=%s objdump=%s" % x for x in badC[:20]]
    open(os.path.join(HERE, "roundtrip_%s.txt" % tag), "w", encoding="utf-8").write("\n".join(rep) + "\n")
    print("\n".join(rep))
    for f in (sfile, ofile, bfile, efile):
        try:
            os.remove(f)
        except OSError:
            pass
    return live, dead, cat


if __name__ == "__main__":
    for c in ("M1.1", "T1.1"):
        run(c)
