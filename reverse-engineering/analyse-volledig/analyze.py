#!/usr/bin/env python3
"""analyze.py - recursieve codetracering + listing met eigen decoder (avrlib).
Gebruik: python analyze.py M1.1|T1.1 -> schrijft listing_<chip>.txt
Extra seeds (data-sprongen / ijmp-doelen) via EXTRA_SEEDS."""
import os, sys, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import avrlib as A

BASE = os.path.dirname(HERE)
CHIPS = {"M1.1": os.path.join(BASE, "Haddon M1.1 - ATMEGA8@DIP28.BIN"),
         "T1.1": os.path.join(BASE, "Haddon T1.1 - ATMEGA8@DIP28.BIN")}
FLASH_WORDS = 4096


def read_flash(chip):
    d = open(CHIPS[chip], "rb").read()
    return d[:8192], d[8192:]


def words_of(flash):
    return [flash[i] | (flash[i + 1] << 8) for i in range(0, len(flash), 2)]


def trace(words, seeds):
    n = len(words)
    insns = {}
    xref = {}   # target -> list of (from, kind)
    work = list(seeds)
    covered = set()
    while work:
        a = work.pop()
        while 0 <= a < n and a not in insns:
            w = words[a]
            ins = A.decode(w, words[a + 1] if a + 1 < n else 0, a)
            if ins is None:
                # ongeldig/niet-uitvoerbaar op deze AVR: stop dit pad
                insns[a] = {"name": ".word", "size": 1, "ops": {}, "target": None, "w": w, "w2": None, "bad": True}
                break
            insns[a] = ins
            sz = ins["size"]
            nm = ins["name"]
            t = ins["target"]
            if t is not None:
                kind = "call" if nm in ("rcall", "call") else ("jump" if nm in ("rjmp", "jmp") else "branch")
                xref.setdefault(t, []).append((a, kind))
                work.append(t)
            if nm in ("cpse", "sbrc", "sbrs", "sbic", "sbis"):
                nxt = a + sz
                nx = A.decode(words[nxt], words[nxt + 1] if nxt + 1 < n else 0, nxt)
                land = nxt + (nx["size"] if nx else 1)
                xref.setdefault(land, []).append((a, "skip"))
                work.append(land)
            if nm in ("rjmp", "jmp", "ret", "reti", "ijmp"):
                break
            a += sz
    return insns, xref


def coverage_bytes(insns, nwords):
    cov = [None] * nwords
    for a, ins in insns.items():
        for k in range(ins["size"]):
            cov[a + k] = "code"
    return cov


def label(a, xref, calls):
    return ("sub_%04x" % (a * 2)) if a in calls else ("L_%04x" % (a * 2))


def build_listing(chip, words, insns, xref, cov, names=None, comments=None):
    names = names or {}
    comments = comments or {}
    calls = {t for t, l in xref.items() if any(k == "call" for _, k in l)}

    def sym(t):
        if t in names:
            return names[t]
        if t in xref and t != 0:
            return label(t, xref, calls)
        return None
    out = []
    n = len(words)
    i = 0
    lastcode_end = max(insns) + 1
    while i < lastcode_end + 8 and i < n:
        if i in insns:
            ins = insns[i]
            lab = None
            if i in names:
                lab = names[i]
            elif i in xref and i != 0:
                lab = label(i, xref, calls)
            if lab:
                refs = ", ".join("%04x:%s" % (f * 2, k) for f, k in sorted(xref.get(i, []))[:8])
                out.append("")
                out.append("%s:   ; xref: %s" % (lab, refs))
            if ins.get("bad"):
                txt = ".word 0x%04x   ; ONGELDIG" % ins["w"]
            else:
                txt = A.mnemonic_text(ins, sym, i)
            raw = " ".join("%04x" % words[i + k] for k in range(ins["size"]))
            cm = comments.get(i, "")
            out.append("%04x  %-10s %-28s %s" % (i * 2, raw, txt, ("; " + cm) if cm else ""))
            i += ins["size"]
        else:
            j = i
            while j < n and j not in insns:
                j += 1
            # splits FF-runs af (>=8 woorden) van data
            seg = words[i:j]
            k0 = i
            if not all(w == 0xFFFF for w in seg):
                # trim trailing FF run
                e = j
                while e > i and words[e - 1] == 0xFFFF:
                    e -= 1
                if j - e >= 8:
                    out.append("")
                    out.append("; --- DATA %04x-%04x (%d bytes)" % (i * 2, e * 2 - 1, (e - i) * 2))
                    for k in range(i, e, 8):
                        row = words[k:min(k + 8, e)]
                        bs = b"".join(w.to_bytes(2, "little") for w in row)
                        out.append("%04x  %s" % (k * 2, " ".join("%02x" % b for b in bs)))
                    out.append("")
                    out.append("; --- %04x-%04x : %d woorden 0xFFFF (vrij)" % (e * 2, j * 2 - 1, j - e))
                    i = j
                    continue
            if all(w == 0xFFFF for w in seg):
                out.append("")
                out.append("; --- %04x-%04x : %d woorden 0xFFFF (vrij)" % (i * 2, j * 2 - 1, j - i))
            else:
                out.append("")
                out.append("; --- DATA %04x-%04x (%d bytes)" % (i * 2, j * 2 - 1, (j - i) * 2))
                for k in range(i, j, 8):
                    row = words[k:min(k + 8, j)]
                    bs = b"".join(w.to_bytes(2, "little") for w in row)
                    out.append("%04x  %s" % (k * 2, " ".join("%02x" % b for b in bs)))
            i = j
    return "\n".join(out)


def seeds_for(words):
    s = [0]
    for v in range(0, 19):
        if (words[v] & 0xF000) == 0xC000:
            s.append(v)
    return s


if __name__ == "__main__":
    chip = sys.argv[1]
    flash, ee = read_flash(chip)
    words = words_of(flash)
    seeds = seeds_for(words)
    extra = json.load(open(os.path.join(HERE, "extra_seeds.json"))).get(chip, []) if os.path.exists(
        os.path.join(HERE, "extra_seeds.json")) else []
    insns, xref = trace(words, seeds + [e // 2 for e in extra])
    cov = coverage_bytes(insns, len(words))
    txt = build_listing(chip, words, insns, xref, cov)
    open(os.path.join(HERE, "listing_%s.txt" % chip.replace(".", "")), "w", encoding="utf-8").write(txt)
    print(chip, "instructies:", len(insns), "codewoorden:", sum(1 for c in cov if c))
