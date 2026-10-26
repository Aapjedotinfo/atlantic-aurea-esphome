"""Decodeert een bitgeklokte 8N1-lijn (666 baud, 12000 cycli/bit) uit een lijst flanken [(cyc, level), ...]."""
def crc16(data, init=0xFFFF):
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def level_at(edges, t):
    lv = 1
    for cy, b in edges:
        if cy <= t: lv = b
        else: break
    return lv

def decode_bytes(edges, bitc=12000, tend=None):
    """Return list of (startcycle, byte, stopbit_ok)."""
    out = []; i = 0
    n = len(edges)
    t_end = tend or (edges[-1][0] + 20 * bitc if edges else 0)
    # zoek valling (idle=1 -> 0)
    k = 0; lv = 1; busy_until = -1
    while k < n:
        cy, b = edges[k]
        if cy >= busy_until and b == 0:
            start = cy
            val = 0
            for bit in range(8):
                t = start + int((1.5 + bit) * bitc)
                if level_at(edges, t): val |= 1 << bit
            stop = level_at(edges, start + int(9.5 * bitc))
            out.append((start, val, stop))
            busy_until = start + int(9.5 * bitc)
        k += 1
    return out

def split_frames(byts, gap=3 * 12000 * 10):
    """Groepeer bytes tot telegrammen: begin bij 0x19 (cb->wp), lengte = 3 + data[2] + 2... of 0x91 (wp->cb)"""
    frames = []; cur = []
    for st, v, sb in byts:
        cur.append((st, v))
    return cur
