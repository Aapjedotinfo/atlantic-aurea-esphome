import re


def norm(t):
    t = t.replace("\t", " ")
    t = re.sub(r"\s+", " ", t).strip().lower()
    t = t.replace(" ,", ",").replace(", ", ",")
    t = re.sub(r"0x[0-9a-f]+", lambda m: str(int(m.group(0), 16)), t)
    return t


def canon(t):
    t = norm(t)
    m = re.match(r"(lsl|rol|tst|clr) (r\d+)$", t)
    if m:
        base = {"lsl": "add", "rol": "adc", "tst": "and", "clr": "eor"}[m.group(1)]
        return "%s %s,%s" % (base, m.group(2), m.group(2))
    m = re.match(r"ser (r\d+)$", t)
    if m:
        return "ldi %s,255" % m.group(1)
    m = re.match(r"(sbr|cbr) (r\d+),(\d+)$", t)
    if m:
        if m.group(1) == "sbr":
            return "ori %s,%s" % (m.group(2), m.group(3))
        return "andi %s,%d" % (m.group(2), (~int(m.group(3))) & 0xFF)
    return t
