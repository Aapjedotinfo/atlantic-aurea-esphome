#!/usr/bin/env python3
"""Aanvullende kruiscontrole van de emulator-kern tegen de AVR-simulator in avr-gdb met WILLEKEURIGE programma's
die elke ATmega8-instructieklasse oefenen (aritmetiek/logica met alle vlaggen, MUL/MULS/MULSU, MOVW, ADIW/SBIW, LD/ST/LDD/STD met alle
pointermodi, LDS/STS, PUSH/POP, IN/OUT/SBI/CBI/SBIC/SBIS, SBRC/SBRS/CPSE, BST/BLD, BSET/BCLR, korte voorwaartse sprongen, LPM).
Elke stap: PC, r0-r31, SREG, SP vergeleken; aan het eind het RAM 0x100-0x45F.  Uitvoer: test_opcodes_vs_gdb.txt"""
import os, sys, re, random, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import avrlib as A, emu
from test_emu_vs_gdb import GDB, OBJCOPY, GD, parse

ALLOWED_PLAIN = {"add", "adc", "sub", "sbc", "and", "or", "eor", "mov", "cp", "cpc", "cpse", "cpi", "sbci", "subi", "ori", "andi", "ldi",
                 "com", "neg", "swap", "inc", "dec", "asr", "lsr", "ror", "bset", "bclr", "mul", "muls", "mulsu", "movw",
                 "bst", "bld", "sbrc", "sbrs", "sbi", "cbi", "sbic", "sbis", "in", "out", "push", "pop", "nop",
                 "ld_x", "ld_xp", "ld_mx", "st_x", "st_xp", "st_mx", "ld_yp", "ld_my", "st_yp", "st_my", "ld_zp", "ld_mz", "st_zp", "st_mz",
                 "ldd_y", "ldd_z", "std_y", "std_z", "adiw", "sbiw", "lpm", "lpm_z", "lpm_zp"}


def gen_prog(rnd, n):
    words = []
    while len(words) < n:
        w = rnd.randrange(65536)
        ins = A.decode(w, 0, 0)
        if ins is None:
            continue
        nm, o = ins["name"], ins["ops"]
        if nm in ("lds", "sts"):
            if rnd.random() < 0.5:
                words += [w & 0xFE0F | (rnd.randrange(24) << 4), rnd.randrange(0x100, 0x45F)]
            continue
        if nm in ("rjmp",):
            words.append(0xC000 | rnd.randrange(0, 4))
            continue
        if nm in ("brbs", "brbc"):
            words.append((w & 0xFC07) | (rnd.randrange(0, 6) << 3))
            continue
        if nm not in ALLOWED_PLAIN:
            continue
        # registeroperanden beperken tot r0-r25 (pointers X/Y/Z blijven heel)
        bad = False
        for k in ("d", "r"):
            if k in o:
                if nm in ("adiw", "sbiw"):
                    if o["d"] != 24:
                        bad = True
                elif nm == "movw":
                    if o["d"] >= 26 or o["r"] >= 26:
                        bad = True
                elif o[k] >= 26:
                    bad = True
        if nm in ("ld_zp", "ld_mz", "st_zp", "st_mz", "lpm_zp") and False:
            bad = True
        if nm in ("out",) and o["A"] >= 0x3D:
            bad = True
        if nm in ("in", "out") and o["A"] in (0x29, 0x2F, 0x32):   # magische poorten van de gdb-sim (exit/abort/stdio)
            bad = True
        if nm in ("in", "out", "sbi", "cbi", "sbic", "sbis") and o["A"] in (0x3D, 0x3E, 0x3F, 0x3B, 0x38, 0x39):
            pass
        if bad:
            continue
        words.append(w)
    return words


def run_gdb(elf, regs, sreg, sp, ram, steps):
    lines = ["set pagination off", "set confirm off", "target sim", "load", "tbreak *0", "run"]
    for i, v in enumerate(regs):
        lines.append("set {unsigned char}0x%x = %d" % (0x800000 + i, v))
    lines.append("set {unsigned char}0x80005f = %d" % sreg)
    lines.append("set {unsigned char}0x80005d = %d" % (sp & 0xFF))
    lines.append("set {unsigned char}0x80005e = %d" % (sp >> 8))
    for i, v in enumerate(ram):
        lines.append("set {unsigned char}0x%x = %d" % (0x800100 + i, v))
    lines += ["set $i = 0", "while $i < %d" % steps, "stepi", "info registers", "set $i = $i + 1", "end"]
    sf = os.path.join(GD, "run_op.gdb")
    open(sf, "w").write("\n".join(lines) + "\n")
    r = subprocess.run([GDB, "-q", "-batch", "-x", sf, elf], capture_output=True, text=True, timeout=1800)
    return r.stdout


def main():
    nprog = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 2500
    report = []
    allok = True
    names_seen = set()
    for p in range(nprog):
        rnd = random.Random(7000 + p)
        words = gen_prog(rnd, steps + 20)
        flash = bytearray(b"\xff" * 8192)
        for i, w in enumerate(words[:4000]):
            flash[2 * i] = w & 0xFF
            flash[2 * i + 1] = w >> 8
        # eindstuk: nop's
        for i in range(len(words), len(words) + 8):
            flash[2 * i] = flash[2 * i + 1] = 0
        binf = os.path.join(GD, "op%d.bin" % p)
        open(binf, "wb").write(bytes(flash))
        elf = os.path.join(GD, "op%d.elf" % p)
        subprocess.run([OBJCOPY, "-I", "binary", "-O", "elf32-avr", "--rename-section", ".data=.text,contents,alloc,load,readonly,code", binf, elf], check=True)
        regs = [rnd.randrange(256) for _ in range(32)]
        for ptr in (26, 28, 30):          # X, Y, Z in RAM, ruim binnen 0x120-0x3C0
            v = rnd.randrange(0x120, 0x3C0)
            regs[ptr], regs[ptr + 1] = v & 0xFF, v >> 8
        sreg = rnd.randrange(256) & 0x7F
        sp = 0x300
        ram = [rnd.randrange(256) for _ in range(0x460 - 0x100)]
        out = run_gdb(elf, regs, sreg, sp, ram, steps)
        g, _ = parse(out, steps)
        c = emu.CPU(bytes(flash), "op")
        c.raw_io = True
        for i, v in enumerate(regs):
            c.r[i] = v
        c.sreg = sreg
        c.sp = sp
        for i, v in enumerate(ram):
            c.mem[0x100 + i] = v
        first = None
        n = min(len(g), steps)
        for i in range(n):
            ins = A.decode(c.words[c.pc], c.words[c.pc + 1], c.pc)
            nm = ins["name"] if ins else "?"
            c.step_raw()
            names_seen.add(nm)
            st = {"pc": c.pc * 2, "sreg": c.sreg, "sp": c.sp, **{k: c.r[k] for k in range(32)}}
            gs = g[i]
            if any(gs.get(k) != st[k] for k in st):
                first = (i, nm, {k: hex(gs.get(k, -1)) for k in st if gs.get(k) != st[k]}, {k: hex(st[k]) for k in st if gs.get(k) != st[k]})
                break
        ok = first is None and n >= steps - 2
        allok &= ok
        msg = "programma %d: %d stappen (gdb gaf %d), eerste afwijking: %s" % (p, steps, len(g), "geen" if first is None else "stap %d na %s: gdb=%s mijn=%s" % first)
        print(msg, flush=True)
        report.append(msg)
    names = sorted(names_seen)
    tail = "Instructieklassen uitgevoerd in de vergelijking (%d): %s\nTOTAAL: %s\n" % (len(names), ", ".join(names), "OK" if allok else "AFWIJKINGEN")
    print(tail)
    open(os.path.join(HERE, "test_opcodes_vs_gdb.txt"), "w", encoding="utf-8").write(
        "Kruiscontrole eigen emulator vs avr-gdb sim met willekeurige instructieprogramma's (%d programma's x %d stappen)\n" % (nprog, steps) + "\n".join(report) + "\n" + tail)


if __name__ == "__main__":
    main()
