#!/usr/bin/env python3
"""protocol_ref.py - onafhankelijke referentie-encoder voor het AVR v2-linkprotocol.

Geschreven uit PROTOCOL.md, NIET uit de C-code. Doel:
  1. de voorbeeld-hexdumps in PROTOCOL.md maken;
  2. test/vectors.h genereren, zodat de C-tests de C-encoder byte voor byte
     tegen deze encoder kunnen vergelijken en de C-parser berichten van deze
     encoder moet accepteren (en beschadigde varianten moet weigeren).
  3. de ESP-kant een werkend voorbeeld geven van hoe je de berichten bouwt.

Gebruik:  python -B protocol_ref.py [--vectors ../test/vectors.h] [--dump]
"""
import struct, sys, binascii

SYNC = 0xA5
T_HELLO, T_TABEL, T_ACK, T_VRAAG, T_ANTWOORD, T_STATUS, T_COMMANDO, T_EVENT, T_INJECT = range(1, 10)

# ---- CRC's -------------------------------------------------------------
def crc8(data: bytes, crc: int = 0x00) -> int:
    """CRC-8, poly 0x07, start 0x00, geen reflectie, geen eindxor (CRC-8/SMBUS)."""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, start 0xFFFF, geen reflectie, geen eindxor."""
    return binascii.crc_hqx(data, 0xFFFF)

assert crc8(b"123456789") == 0xF4
assert crc16(b"123456789") == 0x29B1

# ---- OpenTherm-framehulp ----------------------------------------------
def ot_parity(frame31: int) -> int:
    return bin(frame31 & 0x7FFFFFFF).count("1") & 1

def ot_frame(mtype: int, did: int, value: int) -> int:
    f = ((mtype & 7) << 28) | ((did & 0xFF) << 16) | (value & 0xFFFF)
    return f | (ot_parity(f) << 31)

def f88(c: float) -> int:
    return int(round(c * 256.0)) & 0xFFFF

# ---- berichtkader --------------------------------------------------------
def message(mtype: int, seq: int, payload: bytes) -> bytes:
    body = bytes([mtype, len(payload), seq & 0xFF]) + payload
    return bytes([SYNC]) + body + bytes([crc8(body)])

# ---- tabelregels ---------------------------------------------------------
# richting
TK, KT = 0, 1
# modus (bits 1..0)
M_PASS, M_VAST, M_VRAAG, M_BLOKKEER = 0, 1, 2, 3
# conditie (bits 4..2)
C_ALTIJD, C_REM, C_SUBST, C_OVERRIDE, C_CHOFF, C_REM_OF_CHOFF, C_MAXMOD = range(7)
F_MASKER, F_FORCEER, F_ECHO = 0x20, 0x40, 0x80
V_REMSP, V_AANVOER, V_RETOUR, V_OVERRIDE, V_MAXMOD = 0xFFFF, 0xFFFE, 0xFFFD, 0xFFFC, 0xFFFB

def modus(mode, cond=C_ALTIJD, flags=0):
    return (mode & 3) | ((cond & 7) << 2) | flags

def regel(did, richting, mod, p0=0, p1=0):
    return bytes([did, richting, mod, p0, p1])

def vast_waarde(did, richting, waarde, cond=C_ALTIJD, flags=0):
    return regel(did, richting, modus(M_VAST, cond, flags), waarde >> 8, waarde & 0xFF)

def vast_masker(did, richting, wis, zet, cond=C_ALTIJD):
    return regel(did, richting, modus(M_VAST, cond, F_MASKER), wis, zet)

def vraag_esp(did, richting, deadline_ms=0, standaard=0, cond=C_ALTIJD):
    assert deadline_ms % 4 == 0 and 0 <= deadline_ms <= 600
    return regel(did, richting, modus(M_VRAAG, cond), deadline_ms // 4, standaard)

def blokkeer(did, richting, antwoordtype=7, cond=C_ALTIJD):
    return regel(did, richting, modus(M_BLOKKEER, cond), 0, antwoordtype)

DEFAULT_TIJDEN = dict(deadline_ms=250, ketel_ms=400, esp_weg_s=60, nood_s=240, rem_hold_s=300)

def tabel_body(regels, versie=0, **t):
    p = dict(DEFAULT_TIJDEN); p.update(t)
    body = struct.pack(">BHHHHHB", versie, p["deadline_ms"], p["ketel_ms"], p["esp_weg_s"],
                       p["nood_s"], p["rem_hold_s"], len(regels))
    return body + b"".join(regels)

def tabel_payload(regels, versie=0, **t):
    body = tabel_body(regels, versie, **t)
    return body + struct.pack(">H", crc16(body))

def standaard_regels(dhw_aan=True):
    return [
        vast_masker(0, TK, 0x01, 0x02 if dhw_aan else 0x00, C_REM_OF_CHOFF),
        vast_waarde(1, TK, V_REMSP, C_REM),
        vast_waarde(16, TK, 0x0800, C_REM, F_FORCEER),
        vast_waarde(14, TK, V_MAXMOD, C_MAXMOD),
        vast_waarde(1, KT, 0, C_REM, F_ECHO),
        vast_waarde(16, KT, 0, C_REM, F_ECHO),
        vast_waarde(25, KT, V_AANVOER, C_SUBST),
        vast_waarde(28, KT, V_RETOUR, C_SUBST),
        vast_waarde(9, KT, V_OVERRIDE, C_OVERRIDE, F_FORCEER),
        vast_waarde(100, KT, 0x0000, C_OVERRIDE, F_FORCEER),
    ]

def standaard_tabel_body():
    return tabel_body(standaard_regels(), versie=0)

# ---- berichten -----------------------------------------------------------
def hello(seq, sw=(2, 0, 0), build=(26, 10, 26), proto=2, tabel_crc=0, reset=0x01, boot=0,
          kenmerken=0x001E, maxregels=16):
    p = bytes(sw) + bytes(build) + bytes([proto]) + struct.pack(">H", tabel_crc) + \
        bytes([reset, boot]) + struct.pack(">H", kenmerken) + bytes([maxregels])
    assert len(p) == 14
    return message(T_HELLO, seq, p)

def tabel(seq, regels, versie=1, **t):
    return message(T_TABEL, seq, tabel_payload(regels, versie, **t))

def ack(seq, bevestigd, status=0):
    return message(T_ACK, seq, bytes([bevestigd, status]))

def vraag(seq, richting, frame):
    return message(T_VRAAG, seq, bytes([richting]) + struct.pack(">I", frame))

A_DOORGEVEN, A_VERVANGEN, A_BLOKKEREN = 0, 1, 2
def antwoord(seq, vraag_seq, actie, frame=0):
    return message(T_ANTWOORD, seq, bytes([vraag_seq, actie]) + struct.pack(">I", frame))

def status(seq, **k):
    d = dict(master=0x03, slave=0x0A, req_sp=45, room=205, room_sp=210, boiler=355, mod=30, fault=0,
             link=0x1F, io=0x03, edges_stat=40, edges_boiler=38, frames=0x05, lines=0x0F,
             err_stat=0, err_boiler=0, spare=0x07, reset=0x01, isr_ovr=0, tabel_crc=0x1234,
             tabel_versie=1, tabel_n=10, crc_fouten=0, rx_overruns=0, vraag_timeouts=0, stale=0,
             sinds_esp=3, ev_gemist=0)
    d.update(k)
    p = struct.pack(">BBBhhhBB", d["master"], d["slave"], d["req_sp"], d["room"], d["room_sp"],
                    d["boiler"], d["mod"], d["fault"])
    p += bytes([d["link"], d["io"], d["edges_stat"], d["edges_boiler"], d["frames"], d["lines"],
                d["err_stat"], d["err_boiler"], d["spare"], d["reset"], d["isr_ovr"]])
    p += struct.pack(">HBB", d["tabel_crc"], d["tabel_versie"], d["tabel_n"])
    p += bytes([d["crc_fouten"], d["rx_overruns"], d["vraag_timeouts"], d["stale"]])
    p += struct.pack(">HB", d["sinds_esp"], d["ev_gemist"])
    assert len(p) == 33, len(p)
    return message(T_STATUS, seq, p)

def commando(seq, **k):
    d = dict(rem=1, subst=0, inv_rx_stat=0, inv_rx_boiler=0, noodbrug_aan=0, lamp_inv=0, sa_flame=0,
             force_sa=0, inv_tx_stat=0, inv_tx_boiler=0, swap=0, ch_off=0, demand=0, rem_direct=0,
             keteltype=0, brake_sp=10, wp_supply=0, wp_return=0, override=0, lamp=0, sa_mod=0,
             test_out=0, boiler_sp=0, max_mod=0, geldig=0)
    d.update(k)
    f1 = d["rem"] | d["subst"] << 1 | d["inv_rx_stat"] << 2 | d["inv_rx_boiler"] << 3 | \
         d["noodbrug_aan"] << 4 | d["lamp_inv"] << 5 | d["sa_flame"] << 6 | d["force_sa"] << 7
    f2 = d["inv_tx_stat"] | d["inv_tx_boiler"] << 1 | d["swap"] << 2 | d["ch_off"] << 3 | \
         d["demand"] << 4 | d["rem_direct"] << 5 | d["keteltype"] << 6
    p = bytes([f1, f2, d["brake_sp"]]) + struct.pack(">hhh", d["wp_supply"], d["wp_return"], d["override"]) + \
        bytes([d["lamp"], d["sa_mod"], d["test_out"], d["boiler_sp"], d["max_mod"], d["geldig"]])
    assert len(p) == 15
    return message(T_COMMANDO, seq, p)

EV_FROM_STAT, EV_FROM_BOILER, EV_TO_BOILER, EV_TO_STAT = range(4)
def event(seq, richting, frame):
    return message(T_EVENT, seq, bytes([richting]) + struct.pack(">I", frame))

I_NAAR_KETEL, I_OVERRIDE, I_OPHEFFEN = 0, 1, 2
def inject(seq, modus_, frame):
    return message(T_INJECT, seq, bytes([modus_]) + struct.pack(">I", frame))

# ---- voorbeelden ---------------------------------------------------------
def voorbeelden():
    ex = []
    ex.append(("HELLO (AVR 2.0.0, build 2026-10-26, protocol 2, standaardtabel, spanning aan)",
               hello(0x10, tabel_crc=crc16(standaard_tabel_body()))))
    ex.append(("TABEL (standaardtabel, 10 regels, versie 1, tijden standaard)",
               tabel(0x21, standaard_regels(), versie=1)))
    ex.append(("TABEL (klein: een VRAAG_ESP-regel op ID 27 K>T + blokkeer ID 5 T>K, deadline 100 ms)",
               tabel(0x22, [vraag_esp(27, KT, 100, 0), blokkeer(5, TK)], versie=2, deadline_ms=200)))
    ex.append(("ACK (op TABEL volgnr 0x21, status 0 = OK)", ack(0x11, 0x21, 0)))
    ex.append(("ACK (op TABEL volgnr 0x22, status 2 = buiten de grenzen)", ack(0x12, 0x22, 2)))
    f = ot_frame(1, 1, f88(45.0))
    ex.append(("VRAAG (richting T>K, Write-Data ID 1 = 45,0 C -> frame %08X)" % f, vraag(0x13, TK, f)))
    ex.append(("ANTWOORD DOORGEVEN (op VRAAG 0x13)", antwoord(0x24, 0x13, A_DOORGEVEN)))
    f2 = ot_frame(1, 1, f88(10.0))
    ex.append(("ANTWOORD VERVANGEN (op VRAAG 0x13, nieuw frame %08X = Write-Data ID 1 = 10,0 C)" % f2,
               antwoord(0x25, 0x13, A_VERVANGEN, f2)))
    ex.append(("ANTWOORD BLOKKEREN (op VRAAG 0x13)", antwoord(0x26, 0x13, A_BLOKKEREN)))
    ex.append(("STATUS (voorbeeldwaarden)", status(0x14)))
    ex.append(("COMMANDO (rem aan, 10 C, verder niets)", commando(0x27)))
    ex.append(("COMMANDO (rem uit, substitutie aan, aanvoer 35,5 C, retour 29,0 C, ketel-setpoint 0, geldigheid 30 s)",
               commando(0x28, rem=0, subst=1, wp_supply=355, wp_return=290, geldig=30)))
    ex.append(("EVENT (richting FROM_STAT, frame %08X)" % f, event(0x15, EV_FROM_STAT, f)))
    ex.append(("INJECT naar de ketel (Write-Data ID 27 = 8,5 C)",
               inject(0x29, I_NAAR_KETEL, ot_frame(1, 27, f88(8.5)))))
    ex.append(("INJECT override antwoord (ID 3 = 0x0100)", inject(0x2A, I_OVERRIDE, ot_frame(4, 3, 0x0100))))
    ex.append(("INJECT override opheffen (ID 3)", inject(0x2B, I_OPHEFFEN, ot_frame(4, 3, 0))))
    return ex

def hexs(b): return " ".join("%02X" % x for x in b)

def c_array(name, b):
    return "static const uint8_t %s[%d] PROGMEM = {%s};\n" % (name, len(b), ", ".join("0x%02X" % x for x in b))

def write_vectors(path):
    out = ["/* GEGENEREERD door tools/protocol_ref.py - niet met de hand bewerken. */\n",
           "#ifndef VECTORS_H\n#define VECTORS_H\n#include \"../platform.h\"\n\n"]
    out.append("#define REF_CRC8_CHECK 0xF4\n#define REF_CRC16_CHECK 0x29B1\n")
    out.append("#define REF_DEFAULT_TABEL_CRC 0x%04X\n\n" % crc16(standaard_tabel_body()))
    out.append(c_array("ref_default_body", standaard_tabel_body()))
    out.append(c_array("ref_hello", hello(0x10, tabel_crc=crc16(standaard_tabel_body()))))
    out.append(c_array("ref_tabel_default", tabel(0x21, standaard_regels(), versie=1)))
    out.append(c_array("ref_tabel_klein", tabel(0x22, [vraag_esp(27, KT, 100, 0), blokkeer(5, TK)], versie=2, deadline_ms=200)))
    out.append(c_array("ref_ack", ack(0x11, 0x21, 0)))
    f = ot_frame(1, 1, f88(45.0))
    out.append("#define REF_FRAME_W1_45 0x%08XUL\n" % f)
    out.append(c_array("ref_vraag", vraag(0x13, TK, f)))
    out.append(c_array("ref_antwoord_door", antwoord(0x24, 0x13, A_DOORGEVEN)))
    f2 = ot_frame(1, 1, f88(10.0))
    out.append("#define REF_FRAME_W1_10 0x%08XUL\n" % f2)
    out.append(c_array("ref_antwoord_verv", antwoord(0x25, 0x13, A_VERVANGEN, f2)))
    out.append(c_array("ref_antwoord_blok", antwoord(0x26, 0x13, A_BLOKKEREN)))
    out.append(c_array("ref_status", status(0x14)))
    out.append(c_array("ref_commando_rem", commando(0x27)))
    out.append(c_array("ref_commando_vrij", commando(0x28, rem=0, subst=1, wp_supply=355, wp_return=290, geldig=30)))
    out.append(c_array("ref_event", event(0x15, EV_FROM_STAT, f)))
    out.append(c_array("ref_inject_ketel", inject(0x29, I_NAAR_KETEL, ot_frame(1, 27, f88(8.5)))))
    out.append(c_array("ref_inject_ovr", inject(0x2A, I_OVERRIDE, ot_frame(4, 3, 0x0100))))
    out.append(c_array("ref_inject_ovr_weg", inject(0x2B, I_OPHEFFEN, ot_frame(4, 3, 0))))
    # tabel buiten de grenzen: deadline 700 ms (>600)
    out.append(c_array("ref_tabel_grens", tabel(0x2C, standaard_regels(), versie=3, deadline_ms=700)))
    # commando buiten de grenzen: brake_sp 95
    out.append(c_array("ref_commando_grens", commando(0x2D, brake_sp=95)))
    out.append("\n#endif\n")
    open(path, "w", newline="\n").write("".join(out))

if __name__ == "__main__":
    if "--vectors" in sys.argv:
        write_vectors(sys.argv[sys.argv.index("--vectors") + 1])
        print("vectors geschreven")
    if "--dump" in sys.argv:
        for naam, b in voorbeelden():
            print("### %s\n%s\n" % (naam, hexs(b)))
        print("tabel-CRC standaardtabel: 0x%04X" % crc16(standaard_tabel_body()))
