#!/usr/bin/env python3
"""Kruiscontrole van de eigen emulator-kern tegen de AVR-simulator in avr-gdb (onafhankelijke implementatie).
Beide draaien dezelfde firmware (WDR vervangen door NOP, want gdb-sim kent WDR niet), vanaf de resetvector,
met dezelfde willekeurige beginwaarden voor registers, RAM en IO-registers en zonder randapparatuur
(IO = gewoon geheugen). Na elke instructie worden PC, r0-r31, SREG en SP vergeleken.
Gebruik: python test_emu_vs_gdb.py [stappen] [runs]
"""
import os, sys, re, random, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu

BIN = r"C:\Program Files (x86)\Arduino\hardware\tools\avr\bin"
GDB = os.path.join(BIN, "avr-gdb.exe")
OBJCOPY = os.path.join(BIN, "avr-objcopy.exe")
GD = os.path.join(HERE, "_gdb")
os.makedirs(GD, exist_ok=True)
SRC = {"M1.1": os.path.join(os.path.dirname(HERE), "Haddon M1.1 - ATMEGA8@DIP28.BIN"),
       "T1.1": os.path.join(os.path.dirname(HERE), "Haddon T1.1 - ATMEGA8@DIP28.BIN")}


def patched(chip):
    d = bytearray(open(SRC[chip], "rb").read()[:8192])
    for i in range(0, 8192, 2):
        if d[i] == 0xA8 and d[i + 1] == 0x95:
            d[i] = d[i + 1] = 0
    return bytes(d)


def mkelf(chip):
    tag = chip.replace(".", "").lower()
    b = os.path.join(GD, tag + ".bin")
    open(b, "wb").write(patched(chip))
    e = os.path.join(GD, tag + ".elf")
    subprocess.run([OBJCOPY, "-I", "binary", "-O", "elf32-avr", "--rename-section",
                    ".data=.text,contents,alloc,load,readonly,code", b, e], check=True)
    return e


def run_gdb(elf, init_regs, init_io, init_ram, steps):
    lines = ["set pagination off", "set confirm off", "target sim", "load", "tbreak *0", "run"]
    for i, v in enumerate(init_regs):
        lines.append("set {unsigned char}0x%x = %d" % (0x800000 + i, v))
    for a, v in init_io.items():
        lines.append("set {unsigned char}0x%x = %d" % (0x800000 + 0x20 + a, v))
    for i, v in enumerate(init_ram):
        lines.append("set {unsigned char}0x%x = %d" % (0x800060 + i, v))
    lines += ["set $i = 0", "while $i < %d" % steps, "stepi", "info registers", "set $i = $i + 1", "end",
              "x/160xb 0x800060"]
    sf = os.path.join(GD, "run.gdb")
    open(sf, "w").write("\n".join(lines) + "\n")
    r = subprocess.run([GDB, "-q", "-batch", "-x", sf, elf], capture_output=True, text=True, timeout=900)
    return r.stdout


def parse(out, steps):
    states = []
    cur = {}
    for ln in out.splitlines():
        m = re.match(r"^r(\d+)\s+0x([0-9a-f]+)", ln)
        if m:
            cur[int(m.group(1))] = int(m.group(2), 16)
            continue
        m = re.match(r"^SREG\s+0x([0-9a-f]+)", ln)
        if m:
            cur["sreg"] = int(m.group(1), 16)
            continue
        m = re.match(r"^SP\s+0x([0-9a-f]+)", ln)
        if m:
            cur["sp"] = int(m.group(1), 16) & 0xFFFF
            continue
        m = re.match(r"^pc\s+0x([0-9a-f]+)", ln)
        if m:
            cur["pc"] = int(m.group(1), 16)
            states.append(cur)
            cur = {}
    ram = bytearray()
    for ln in out.splitlines():
        m = re.match(r"^0x8000[0-9a-f]+(?: <[^>]*>)?:\s+(.*)$", ln)
        if m and ("0x" in m.group(1)):
            ram += bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1)))
    return states, bytes(ram)


def run_mine(chip, init_regs, init_io, init_ram, steps):
    c = emu.CPU(patched(chip), chip)
    c.raw_io = True
    for i, v in enumerate(init_regs):
        c.r[i] = v
    for a, v in init_io.items():
        c.io[a] = v
    for i, v in enumerate(init_ram):
        c.mem[0x60 + i] = v
    # gdb-sim stackpointer begint op 0 -> SP wordt door de code gezet
    c.sp = 0
    st = []
    for _ in range(steps):
        c.step_raw()
        st.append({"pc": c.pc * 2, "sreg": c.sreg, "sp": c.sp, **{i: c.r[i] for i in range(32)}})
    return st, bytes(c.mem[0x60:0x60 + 160])


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    report = []
    allok = True
    for chip in ("M1.1", "T1.1"):
        elf = mkelf(chip)
        for seed in range(runs):
            rnd = random.Random(1000 * seed + (1 if chip == "M1.1" else 2))
            regs = [rnd.randrange(256) if seed else 0 for _ in range(32)]
            # IO: constante pin-/statuswaarden (geen randapparatuur); UCSRA bit 6 en 7 aan om pollinglussen te laten doorlopen
            io = {a: rnd.randrange(256) for a in (0x16, 0x13, 0x10, 0x08, 0x0C, 0x05, 0x04, 0x38)}
            io[0x0B] = 0xFF
            ram = [rnd.randrange(256) if seed else 0 for _ in range(160)]
            out = run_gdb(elf, regs, io, ram, steps)
            g, gram = parse(out, steps)
            m, mram = run_mine(chip, regs, io, ram, steps)
            n = min(len(g), len(m))
            first = None
            for i in range(n):
                a, b = g[i], m[i]
                if any(a.get(k) != b.get(k) for k in b):
                    first = i
                    break
            ok = first is None and n >= steps - 1
            allok &= ok
            msg = "%s seed=%d: gdb-stappen=%d eigen=%d; eerste afwijking: %s; RAM-eind gelijk: %s" % (
                chip, seed, len(g), len(m), "geen" if first is None else
                "stap %d gdb=%s mijn=%s" % (first, {k: hex(v) for k, v in g[first].items() if g[first][k] != m[first].get(k)},
                                          {k: hex(v) for k, v in m[first].items() if g[first].get(k) != v}),
                (gram[:160] == mram[:160]) if gram else "n/a")
            print(msg)
            report.append(msg)
    open(os.path.join(HERE, "test_emu_vs_gdb.txt"), "w", encoding="utf-8").write(
        "Kruiscontrole eigen emulator vs avr-gdb sim (%d stappen x %d runs per chip)\n" % (steps, runs) + "\n".join(report) +
        "\nTOTAAL: %s\n" % ("OK" if allok else "AFWIJKINGEN"))


if __name__ == "__main__":
    main()
