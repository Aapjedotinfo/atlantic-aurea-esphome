#!/usr/bin/env python3
"""static_analysis.py - functielijst, aanroepgraaf, IO-gebruik, RAM-kruisverwijzingen en LPM/Z-patronen per chip.
Uitvoer: functies_<chip>.md, ram_<chip>.csv, io_<chip>.csv, aanroepgraaf_<chip>.md, lpm_<chip>.txt
Constante-propagatie is eenvoudig: binnen een basisblok worden LDI-waarden van r26-r31 gevolgd en
bij LD/ST/LDD/STD via X/Y/Z omgezet naar absolute RAM-adressen."""
import os, sys, json, collections
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import avrlib as A, analyze as Z

NAMES = {
 "M1.1": {
  0x0000: "RESET-vector", 0x0002: "INT0-vector(stub)", 0x0010: "T1OVF-vector", 0x0016: "RXC-vector",
  0x002a: "reset_main", 0x0052: "init_ram", 0x0118: "init_timer1", 0x0124: "init_ports", 0x0142: "init_uart9600",
  0x0158: "dood_adc_init", 0x015e: "dood_acsr_init", 0x0164: "init_wdt", 0x0172: "wdr_kick",
  0x0176: "link_rx_frame", 0x02ae: "sw_cv_only_PC0", 0x02d2: "btn_test_PC1", 0x0308: "rx666_parser",
  0x04be: "stand_regelaar", 0x062e: "bewaking_65_lockout", 0x072e: "seconden_watchdog_66", 0x083a: "tx666_machine",
  0x0a40: "link_tx_to_T11", 0x0a62: "uart_putc", 0x0a6c: "ISR_T1OVF", 0x0b66: "ISR_INT0", 0x0b98: "ISR_RXC"},
 "T1.1": {
  0x0000: "RESET-vector", 0x0002: "INT0-vector(stub)", 0x0010: "T1OVF-vector", 0x0016: "RXC-vector",
  0x002a: "reset_main", 0x0056: "init_ram", 0x0120: "init_timer1", 0x012c: "init_ports", 0x0146: "init_uart9600",
  0x015c: "init_comparator", 0x0162: "init_wdt", 0x0170: "wdr_kick", 0x0174: "link_rx_frame",
  0x0232: "bouw_link_frame", 0x0286: "ot_rx_thermostaat_ACO", 0x0398: "ot_rx_ketel_PD5", 0x0482: "ot_verwerk",
  0x0870: "ot_parity8", 0x0872: "ot_parity_tail", 0x0892: "ot_tx_ketel_PD3", 0x0958: "ot_tx_thermostaat_PD4",
  0x0a24: "link_timeout_noodbrug", 0x0aa0: "led_knippercode", 0x0b7e: "link_tx_naar_M11", 0x0ba0: "uart_putc",
  0x0baa: "ISR_T1OVF", 0x0c98: "ISR_INT0", 0x0cc6: "ISR_RXC"},
}


def analyse(chip):
    flash, ee = Z.read_flash(chip)
    words = Z.words_of(flash)
    live, xref = Z.trace(words, Z.seeds_for(words))
    # functie-entries
    entries = {0x002a // 2: "reset_main"}
    for t, l in xref.items():
        if any(k == "call" for _, k in l):
            entries[t] = None
    for v in (1, 8, 11):
        entries[v] = None
    nm = {a // 2: n for a, n in NAMES[chip].items()}
    # intraprocedurele floodfill
    func_of = collections.defaultdict(set)
    func_ins = {}
    for e in sorted(entries):
        seen = set()
        work = [e]
        while work:
            a = work.pop()
            while a in live and a not in seen:
                seen.add(a)
                i = live[a]
                n_ = i["name"]
                if n_ in ("brbs", "brbc", "rjmp"):
                    work.append(i["target"])
                if n_ in ("cpse", "sbrc", "sbrs", "sbic", "sbis"):
                    nxt = a + i["size"]
                    nx = live.get(nxt)
                    work.append(nxt + (nx["size"] if nx else 1))
                if n_ in ("rjmp", "ret", "reti"):
                    break
                a += i["size"]
        func_ins[e] = seen
        for a in seen:
            func_of[a].add(e)
    # aanroepgraaf
    callers = collections.defaultdict(set)
    callees = collections.defaultdict(set)
    jumps_into = collections.defaultdict(set)
    for e, ins in func_ins.items():
        for a in ins:
            i = live[a]
            if i["name"] == "rcall":
                callees[e].add(i["target"])
                callers[i["target"]].add(e)
    # IO en RAM
    io_rows = []
    ram_rows = []
    lpm_sites = []
    for e, ins in sorted(func_ins.items()):
        consts = {}
        for a in sorted(ins):
            i = live[a]
            n_, o = i["name"], i["ops"]
            if a in xref and any(k != "skip" for _, k in xref[a]):
                pass
            # IO
            if n_ in ("in", "out", "sbi", "cbi", "sbic", "sbis"):
                rw = {"in": "R", "out": "W", "sbi": "W(bit %d=1)" % o.get("b", 0), "cbi": "W(bit %d=0)" % o.get("b", 0),
                      "sbic": "R(bit %d)" % o.get("b", 0), "sbis": "R(bit %d)" % o.get("b", 0)}[n_]
                io_rows.append((e * 2, a * 2, n_, A.ioname(o["A"]), o["A"], rw))
            # constante propagatie (grof, per lineaire stroom)
            if n_ == "ldi" and o["d"] in (26, 27, 28, 29, 30, 31):
                consts[o["d"]] = o["K"]
            elif n_ in ("eor",) and o["d"] == o["r"] and o["d"] in (26, 27, 28, 29, 30, 31):
                consts[o["d"]] = 0
            elif n_ in ("ld_x", "ld_xp", "ld_mx", "st_x", "st_xp", "st_mx", "ld_yp", "st_yp", "ld_my", "st_my",
                        "ld_zp", "st_zp", "ld_mz", "st_mz", "ldd_y", "ldd_z", "std_y", "std_z"):
                suf = n_.split("_")[1] if "_" in n_ else ""
                reg = {"x": 26, "y": 28, "z": 30}[[ch for ch in (suf if n_[0:2] in ("ld", "st") and n_[2] == "_" else n_[-1]) if ch in "xyz"][0]]
                lo, hi = consts.get(reg), consts.get(reg + 1)
                isw = n_.startswith("st")
                if lo is not None and hi is not None:
                    addr = (hi << 8) | lo
                    if n_.startswith("ldd") or n_.startswith("std"):
                        addr += o["q"]
                    if "m" in suf and n_[2] == "_":
                        addr -= 1
                    ram_rows.append((addr, e * 2, a * 2, "W" if isw else "R", n_))
                    if n_[2] == "_" and "p" in suf:
                        v = addr + 1
                        consts[reg], consts[reg + 1] = v & 0xFF, v >> 8
                    if n_[2] == "_" and "m" in suf:
                        consts[reg], consts[reg + 1] = addr & 0xFF, addr >> 8
                else:
                    ram_rows.append((None, e * 2, a * 2, "W" if isw else "R", n_ + " (ptr onbekend)"))
            elif n_ in ("lds", "sts"):
                ram_rows.append((o["k"], e * 2, a * 2, "W" if n_ == "sts" else "R", n_))
            elif n_ in ("lpm", "lpm_z", "lpm_zp"):
                lpm_sites.append((a * 2, e * 2, consts.get(30), consts.get(31)))
            elif n_ in ("adiw", "sbiw", "add", "adc", "sub", "sbc", "mov", "movw", "subi", "sbci", "andi", "ori") and (
                    o.get("d") in (26, 27, 28, 29, 30, 31)):
                if n_ in ("mov", "movw", "add", "adc", "sub", "sbc") or (o.get("d") in (26, 27, 28, 29, 30, 31)):
                    consts.pop(o["d"], None)
                    if n_ == "movw":
                        consts.pop(o["d"] + 1, None)
                    if n_ in ("adiw", "sbiw"):
                        consts.pop(o["d"] + 1, None)
            if n_ in ("rjmp", "ret", "reti", "rcall", "brbs", "brbc") and False:
                consts = {}
    return dict(live=live, xref=xref, func_ins=func_ins, callers=callers, callees=callees, io=io_rows, ram=ram_rows,
                lpm=lpm_sites, nm=nm, words=words, entries=entries)


def write(chip):
    r = analyse(chip)
    tag = chip.replace(".", "")
    nm = r["nm"]
    fname = lambda e: nm.get(e, "sub_%04x" % (e * 2))
    with open(os.path.join(HERE, "functies_%s.md" % tag), "w", encoding="utf-8") as f:
        f.write("# Functies %s (automatisch uit CFG; namen/doel handmatig, zie RAPPORT.md)\n\n" % chip)
        f.write("| adres | naam | bytes | instr | aangeroepen door | roept aan | IO-registers (R/W) |\n|---|---|---|---|---|---|---|\n")
        for e in sorted(r["func_ins"]):
            ins = r["func_ins"][e]
            size = sum(r["live"][a]["size"] * 2 for a in ins)
            ios = collections.OrderedDict()
            for fe, a, n_, name, A_, rw in r["io"]:
                if fe == e * 2:
                    ios.setdefault(name, set()).add(rw[0])
            ios_s = ", ".join("%s(%s)" % (k, "".join(sorted(v))) for k, v in ios.items())
            cl = ", ".join(fname(x) for x in sorted(r["callers"].get(e, []))) or ("vector" if e in (1, 8, 11) else ("reset" if e == 0x15 else "-"))
            ce = ", ".join(fname(x) for x in sorted(r["callees"].get(e, []))) or "-"
            f.write("| 0x%04x | %s | %d | %d | %s | %s | %s |\n" % (e * 2, fname(e), size, len(ins), cl, ce, ios_s))
    with open(os.path.join(HERE, "io_%s.csv" % tag), "w", encoding="utf-8") as f:
        f.write("functie;adres;instructie;register;io_adres;toegang\n")
        for fe, a, n_, name, A_, rw in r["io"]:
            f.write("0x%04x %s;0x%04x;%s;%s;0x%02x;%s\n" % (fe, fname(fe // 2), a, n_, name, A_, rw))
    byaddr = collections.defaultdict(list)
    for addr, fe, a, rw, n_ in r["ram"]:
        byaddr[addr].append((fname(fe // 2), a, rw, n_))
    with open(os.path.join(HERE, "ram_%s.csv" % tag), "w", encoding="utf-8") as f:
        f.write("ram_adres;lezers(functie@pc);schrijvers(functie@pc);opmerking\n")
        for addr in sorted(byaddr, key=lambda x: (-1 if x is None else x)):
            rd = sorted({"%s@%04x" % (fn, a) for fn, a, rw, n_ in byaddr[addr] if rw == "R"})
            wr = sorted({"%s@%04x" % (fn, a) for fn, a, rw, n_ in byaddr[addr] if rw == "W"})
            f.write("%s;%s;%s;\n" % ("onbekend" if addr is None else "0x%02x" % addr, " ".join(rd), " ".join(wr)))
    with open(os.path.join(HERE, "aanroepgraaf_%s.md" % tag), "w", encoding="utf-8") as f:
        f.write("# Aanroepgraaf %s\n\n```\n" % chip)
        main = 0x3c // 2 if chip == "M1.1" else 0x40 // 2
        f.write("reset (0x002a) -> init_* ; hoofdlus 0x%04x ->\n" % (main * 2))
        for e in sorted(r["func_ins"]):
            for t in sorted(r["callees"].get(e, [])):
                f.write("%s -> %s\n" % (fname(e), fname(t)))
        f.write("```\n\nTail-sprongen (RJMP naar functie-entry buiten eigen functie):\n\n")
        for e, ins in sorted(r["func_ins"].items()):
            for a in sorted(ins):
                i = r["live"][a]
                if i["name"] == "rjmp" and i["target"] in r["func_ins"] and i["target"] != e and i["target"] in r["entries"]:
                    f.write("- %s@%04x rjmp -> %s\n" % (fname(e), a * 2, fname(i["target"])))
        f.write("\nHoofdlus-volgorde (aanroepen tussen 0x%04x en rjmp terug):\n\n" % (main * 2))
        a = main
        order = []
        while a in r["live"]:
            i = r["live"][a]
            if i["name"] == "rcall":
                order.append(fname(i["target"]))
            if i["name"] == "rjmp":
                break
            a += i["size"]
        f.write(" -> ".join(order) + "\n")
    with open(os.path.join(HERE, "lpm_%s.txt" % tag), "w", encoding="utf-8") as f:
        f.write("LPM-plaatsen (flash-leesinstructies) in %s:\n" % chip)
        for a, fe, zl, zh in r["lpm"]:
            f.write("  0x%04x in %s ; Z-constante bekend uit LDI: %s\n" % (
                a, fname(fe // 2), "0x%02x%02x" % (zh, zl) if zl is not None and zh is not None else "nee (opgebouwd)"))
        f.write("\nOverige instructies die programmageheugen/Z in controle-flow gebruiken: IJMP/ICALL/EIJMP/EICALL/SPM: %s\n" % (
            [hex(a * 2) for a, i in r["live"].items() if i["name"] in ("ijmp", "icall", "spm")] or "geen"))
    return r


if __name__ == "__main__":
    for chip in ("M1.1", "T1.1"):
        r = write(chip)
        print(chip, "functies:", len(r["func_ins"]), "RAM-toegangen:", len(r["ram"]), "IO-toegangen:", len(r["io"]), "LPM:", len(r["lpm"]))
