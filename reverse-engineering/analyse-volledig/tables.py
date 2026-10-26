"""Decodeert de 10-bit-gekaderde telegramtabellen in M1.1 (0x0baa-0x0c4f) naar bytes + CRC-controle.
Tabelbits gaan MSB eerst de lijn op; het allereerste startbit (0) wordt door de code zelf gegenereerd (0x0886)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uartdec as U
BIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Haddon M1.1 - ATMEGA8@DIP28.BIN")
fl = open(BIN, "rb").read()[:8192]

def decode_slot(addr, nbytes):
    bits = "0" + "".join(format(b, "08b") for b in fl[addr:addr + nbytes])
    out = []; i = 0; ok = True
    while i + 10 <= len(bits):
        fr = bits[i:i + 10]
        if fr[0] != "0" or fr[9] != "1":
            ok = False
        out.append(int(fr[1:9][::-1], 2))
        i += 10
    return out, ok, bits[i:]

SLOTS = [("orphan1", 0xbaa, 10), ("orphan2", 0xbb4, 10), ("A=ID1", 0xbbe, 15), ("pad", 0xbcd, 1),
         ("C=ID3", 0xbce, 10), ("D=ID0", 0xbd8, 10)] + [("B[%d]" % k, 0xbe2 + 10 * k, 10) for k in range(11)]
if __name__ == "__main__":
    for name, a, n in SLOTS:
        if name == "pad":
            print("%-8s 0x%04x raw=%s" % (name, a, fl[a:a+n].hex())); continue
        by, ok, tail = decode_slot(a, n)
        # lengte volgens byte 2
        ln = by[2] if len(by) > 2 else 0
        fr = by[:ln] if ln <= len(by) else by
        print("%-8s 0x%04x raw=%s framing_ok=%s bytes=%s crc16=%04x staartbits=%s" % (
            name, a, fl[a:a + n].hex(" "), ok, " ".join("%02x" % x for x in fr), U.crc16(fr), tail))
