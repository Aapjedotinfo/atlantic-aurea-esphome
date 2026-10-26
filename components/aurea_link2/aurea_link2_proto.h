#pragma once

// ╔═══════════════════════════════════════════════════════════════╗
// ║  aurea_link2 - coder en decoder van het AVR-linkprotocol v2    ║
// ╚═══════════════════════════════════════════════════════════════╝
//
// Alleen berichtindeling, CRC's en ontvangst-resync; geen ESPHome, geen UART.
// Daardoor is alles `constexpr` en te controleren met static_assert tegen de
// testvectoren (zie PROTOCOL.md hoofdstuk 12, firmware/avr-bridge-v2).
//
// Bindend: firmware/avr-bridge-v2/PROTOCOL.md (protocolversie 2).

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace aurea_link2 {

static constexpr uint8_t SYNC = 0xA5;
static constexpr uint8_t PROTOCOL_VERSIE = 2;
static constexpr uint8_t MAX_PAYLOAD = 96;
static constexpr uint8_t MAX_REGELS = 16;
static constexpr uint8_t KOP_LEN = 4;  // SYNC, type, lengte, volgnr
static constexpr size_t MAX_BERICHT = MAX_PAYLOAD + 5;
static constexpr uint32_t STILTE_MS = 5;  // pauze tussen berichten en resync-drempel

enum Type : uint8_t {
  T_HELLO = 0x01,
  T_TABEL = 0x02,
  T_ACK = 0x03,
  T_VRAAG = 0x04,
  T_ANTWOORD = 0x05,
  T_STATUS = 0x06,
  T_COMMANDO = 0x07,
  T_EVENT = 0x08,
  T_INJECT = 0x09,
};

// Vaste payloadlengte per type; -1 = variabel (TABEL), -2 = onbekend type.
constexpr int type_lengte(uint8_t t) {
  switch (t) {
    case T_HELLO: return 14;
    case T_TABEL: return -1;
    case T_ACK: return 2;
    case T_VRAAG: return 5;
    case T_ANTWOORD: return 6;
    case T_STATUS: return 33;
    case T_COMMANDO: return 15;
    case T_EVENT: return 5;
    case T_INJECT: return 5;
    default: return -2;
  }
}

// ── CRC's ──
// CRC-8/SMBUS: poly 0x07, start 0. Controle over "123456789" = 0xF4.
constexpr uint8_t crc8_byte(uint8_t crc, uint8_t b) {
  crc = (uint8_t) (crc ^ b);
  for (uint8_t i = 0; i < 8; i++)
    crc = (uint8_t) ((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
  return crc;
}
constexpr uint8_t crc8(const uint8_t *p, size_t n, uint8_t crc = 0) {
  for (size_t i = 0; i < n; i++)
    crc = crc8_byte(crc, p[i]);
  return crc;
}
// CRC-16/CCITT-FALSE: poly 0x1021, start 0xFFFF. Controle over "123456789" = 0x29B1.
constexpr uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc = (uint16_t) (crc ^ ((uint16_t) p[i] << 8));
    for (uint8_t k = 0; k < 8; k++)
      crc = (uint16_t) ((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1));
  }
  return crc;
}

// ── Berichtkader ──
// Schrijft SYNC, type, lengte, volgnr, payload, CRC-8. Geeft de totale lengte.
constexpr size_t bouw_bericht(uint8_t *uit, uint8_t type, uint8_t volgnr, const uint8_t *pl, uint8_t len) {
  uit[0] = SYNC;
  uit[1] = type;
  uit[2] = len;
  uit[3] = volgnr;
  for (uint8_t i = 0; i < len; i++)
    uit[4 + i] = pl[i];
  uit[4 + len] = crc8(uit + 1, (size_t) len + 3);
  return (size_t) len + 5;
}

// ── Ontvanger met resync (hoofdstuk 3) ──
//
// voer() krijgt elke ontvangen byte. stilte() moet worden aangeroepen zodra er
// minstens 5 ms geen byte is binnengekomen. Na een fout (CRC, lengte) gooit de
// ontvanger alles weg tot de volgende stilte(); een afgebroken bericht (stilte
// midden in het bericht) telt als begin van een nieuwe zoektocht naar 0xA5.
class Ontvanger {
 public:
  enum Uitkomst : int8_t { NIETS = 0, BERICHT = 1, FOUT = -1 };

  uint8_t type{0}, lengte{0}, volgnr{0};
  uint8_t payload[MAX_PAYLOAD]{};
  uint16_t afgebroken{0};

  constexpr int8_t voer(uint8_t b) {
    switch (st_) {
      case WACHT:
        if (b == SYNC) {
          st_ = TYPE;
          crc_ = 0;
        }
        return NIETS;
      case TYPE:
        type = b;
        crc_ = crc8_byte(crc_, b);
        st_ = LENGTE;
        return NIETS;
      case LENGTE:
        if (b > MAX_PAYLOAD) {
          st_ = WEG;
          return FOUT;
        }
        lengte = b;
        crc_ = crc8_byte(crc_, b);
        st_ = SEQ;
        return NIETS;
      case SEQ:
        volgnr = b;
        crc_ = crc8_byte(crc_, b);
        n_ = 0;
        st_ = lengte ? PAYLOAD : CRC;
        return NIETS;
      case PAYLOAD:
        payload[n_++] = b;
        crc_ = crc8_byte(crc_, b);
        if (n_ >= lengte)
          st_ = CRC;
        return NIETS;
      case CRC:
        if (b == crc_) {
          st_ = WACHT;
          return BERICHT;
        }
        st_ = WEG;
        return FOUT;
      default:  // WEG
        return NIETS;
    }
  }

  constexpr void stilte() {
    if (st_ != WACHT && st_ != WEG)
      afgebroken++;
    st_ = WACHT;
  }

  constexpr bool in_bericht() const { return st_ != WACHT && st_ != WEG; }

 protected:
  enum Staat : uint8_t { WACHT, TYPE, LENGTE, SEQ, PAYLOAD, CRC, WEG };
  Staat st_{WACHT};
  uint8_t crc_{0}, n_{0};
};

// ── HELLO (AVR -> ESP) ──
struct Hello {
  uint8_t sw_hoofd, sw_neven, sw_patch, jaar, maand, dag, protocol;
  uint16_t tabel_crc;
  uint8_t resetreden, bootteller;
  uint16_t kenmerken;
  uint8_t max_regels;
};
constexpr bool lees_hello(const uint8_t *p, uint8_t len, Hello &h) {
  if (len != 14)
    return false;
  h.sw_hoofd = p[0];
  h.sw_neven = p[1];
  h.sw_patch = p[2];
  h.jaar = p[3];
  h.maand = p[4];
  h.dag = p[5];
  h.protocol = p[6];
  h.tabel_crc = (uint16_t) ((p[7] << 8) | p[8]);
  h.resetreden = p[9];
  h.bootteller = p[10];
  h.kenmerken = (uint16_t) ((p[11] << 8) | p[12]);
  h.max_regels = p[13];
  return true;
}

// ── TABEL (ESP -> AVR) ──
struct Regel {
  uint8_t id, richting, modus, p0, p1;
};
struct Tabel {
  uint8_t versie{0};
  uint16_t deadline_ms{250}, ketel_ms{400}, esp_weg_s{60}, nood_s{240}, rem_hold_s{300};
  uint8_t n{0};
  Regel r[MAX_REGELS]{};
};

// Bytes 0 t/m 11+5n van de TABEL-payload (wat de CRC-16 dekt). Geeft de lengte.
constexpr size_t tabel_body(const Tabel &t, uint8_t *u) {
  u[0] = t.versie;
  u[1] = (uint8_t) (t.deadline_ms >> 8);
  u[2] = (uint8_t) t.deadline_ms;
  u[3] = (uint8_t) (t.ketel_ms >> 8);
  u[4] = (uint8_t) t.ketel_ms;
  u[5] = (uint8_t) (t.esp_weg_s >> 8);
  u[6] = (uint8_t) t.esp_weg_s;
  u[7] = (uint8_t) (t.nood_s >> 8);
  u[8] = (uint8_t) t.nood_s;
  u[9] = (uint8_t) (t.rem_hold_s >> 8);
  u[10] = (uint8_t) t.rem_hold_s;
  u[11] = t.n;
  for (uint8_t i = 0; i < t.n; i++) {
    u[12 + 5 * i] = t.r[i].id;
    u[13 + 5 * i] = t.r[i].richting;
    u[14 + 5 * i] = t.r[i].modus;
    u[15 + 5 * i] = t.r[i].p0;
    u[16 + 5 * i] = t.r[i].p1;
  }
  return (size_t) 12 + 5 * t.n;
}
constexpr uint16_t tabel_crc(const Tabel &t) {
  uint8_t u[12 + 5 * MAX_REGELS]{};
  return crc16(u, tabel_body(t, u));
}
// Body plus CRC-16 (hoge byte eerst) = de payload van het TABEL-bericht.
constexpr size_t tabel_payload(const Tabel &t, uint8_t *u) {
  const size_t n = tabel_body(t, u);
  const uint16_t c = crc16(u, n);
  u[n] = (uint8_t) (c >> 8);
  u[n + 1] = (uint8_t) c;
  return n + 2;
}

// Dezelfde controles als de AVR (hoofdstuk 4.2, 5, 8): een tabel die hier
// niet door komt wordt daar geheel afgewezen.
constexpr bool tabel_geldig(const Tabel &t) {
  if (t.deadline_ms < 30 || t.deadline_ms > 600)
    return false;
  if (t.ketel_ms < 100 || t.ketel_ms > 600)
    return false;
  if (t.esp_weg_s < 10 || t.esp_weg_s > 600)
    return false;
  if (t.nood_s < 60 || t.nood_s > 1800 || t.nood_s < t.esp_weg_s)
    return false;
  if (t.rem_hold_s < 60 || t.rem_hold_s > 900)
    return false;
  if (t.n > MAX_REGELS)
    return false;
  for (uint8_t i = 0; i < t.n; i++) {
    const Regel &r = t.r[i];
    if (r.richting > 1)
      return false;
    for (uint8_t j = 0; j < i; j++)
      if (t.r[j].id == r.id && t.r[j].richting == r.richting)
        return false;
    const uint8_t modus = r.modus & 0x03, cond = (r.modus >> 2) & 0x07;
    const bool masker = r.modus & 0x20, forceer = r.modus & 0x40, echo = r.modus & 0x80;
    if (cond > 6)
      return false;
    if (modus != 1 && (masker || forceer || echo))
      return false;
    if (masker && r.richting != 0)
      return false;
    if (echo && r.richting != 1)
      return false;
    if (modus == 0 && (r.p0 || r.p1))
      return false;
    if (modus == 2 && (r.p0 > 150 || r.p1 > 1))
      return false;
    if (modus == 3 && (r.p0 != 0 || (r.p1 != 0 && r.p1 != 6 && r.p1 != 7)))
      return false;
  }
  return true;
}

// ── COMMANDO (ESP -> AVR) ──
struct Commando {
  uint8_t flags1{0}, flags2{0}, brake_setpoint{0};
  int16_t wp_supply{0}, wp_return{0}, override_sp{0};
  uint8_t lamp{0}, sa_mod{0}, test_out{0}, boiler_sp{0}, max_mod{0}, geldig_s{0};
};
constexpr void commando_payload(const Commando &c, uint8_t *u) {
  u[0] = c.flags1;
  u[1] = c.flags2;
  u[2] = c.brake_setpoint;
  u[3] = (uint8_t) ((uint16_t) c.wp_supply >> 8);
  u[4] = (uint8_t) c.wp_supply;
  u[5] = (uint8_t) ((uint16_t) c.wp_return >> 8);
  u[6] = (uint8_t) c.wp_return;
  u[7] = (uint8_t) ((uint16_t) c.override_sp >> 8);
  u[8] = (uint8_t) c.override_sp;
  u[9] = c.lamp;
  u[10] = c.sa_mod;
  u[11] = c.test_out;
  u[12] = c.boiler_sp;
  u[13] = c.max_mod;
  u[14] = c.geldig_s;
}

// ── STATUS (AVR -> ESP) ──
struct Status {
  uint8_t master_status, slave_status, requested_setpoint;
  int16_t room_temp, room_setpoint, boiler_temp;
  uint8_t modulation, fault_code, link, io, flanken_stat, flanken_ketel, frames, lijnen, rx_fout_stat,
      rx_fout_ketel, spare, reset, isr_overruns;
  uint16_t tabel_crc;
  uint8_t tabel_versie, tabel_n, crc_fouten, uart_overruns, vraag_verlopen, ketel_oud;
  uint16_t sinds_esp;
  uint8_t ev_gemist;
};
constexpr bool lees_status(const uint8_t *p, uint8_t len, Status &s) {
  if (len != 33)
    return false;
  s.master_status = p[0];
  s.slave_status = p[1];
  s.requested_setpoint = p[2];
  s.room_temp = (int16_t) ((p[3] << 8) | p[4]);
  s.room_setpoint = (int16_t) ((p[5] << 8) | p[6]);
  s.boiler_temp = (int16_t) ((p[7] << 8) | p[8]);
  s.modulation = p[9];
  s.fault_code = p[10];
  s.link = p[11];
  s.io = p[12];
  s.flanken_stat = p[13];
  s.flanken_ketel = p[14];
  s.frames = p[15];
  s.lijnen = p[16];
  s.rx_fout_stat = p[17];
  s.rx_fout_ketel = p[18];
  s.spare = p[19];
  s.reset = p[20];
  s.isr_overruns = p[21];
  s.tabel_crc = (uint16_t) ((p[22] << 8) | p[23]);
  s.tabel_versie = p[24];
  s.tabel_n = p[25];
  s.crc_fouten = p[26];
  s.uart_overruns = p[27];
  s.vraag_verlopen = p[28];
  s.ketel_oud = p[29];
  s.sinds_esp = (uint16_t) ((p[30] << 8) | p[31]);
  s.ev_gemist = p[32];
  return true;
}

// ── OpenTherm-frames in VRAAG, ANTWOORD, EVENT en INJECT (4 bytes, MSB eerst) ──
constexpr uint32_t lees_frame(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
constexpr void schrijf_frame(uint8_t *p, uint32_t f) {
  p[0] = (uint8_t) (f >> 24);
  p[1] = (uint8_t) (f >> 16);
  p[2] = (uint8_t) (f >> 8);
  p[3] = (uint8_t) f;
}
// Pariteitsbit (bit 31) zo dat het hele frame een even aantal enen heeft.
constexpr uint32_t met_pariteit(uint32_t f) {
  f &= 0x7FFFFFFFUL;
  uint32_t x = f;
  uint8_t enen = 0;
  while (x) {
    enen = (uint8_t) (enen + (x & 1));
    x >>= 1;
  }
  return (enen & 1) ? (f | 0x80000000UL) : f;
}

// ANTWOORD: actie 0 doorgeven, 1 vervangen (frame), 2 blokkeren.
enum Actie : uint8_t { DOORGEVEN = 0, VERVANGEN = 1, BLOKKEREN = 2 };
constexpr void antwoord_payload(uint8_t *u, uint8_t vraag_seq, uint8_t actie, uint32_t frame) {
  u[0] = vraag_seq;
  u[1] = actie;
  schrijf_frame(u + 2, actie == VERVANGEN ? frame : 0);
}
// INJECT: modus 0 naar de ketel, 1 antwoord vervangen, 2 opheffen.
constexpr void inject_payload(uint8_t *u, uint8_t modus, uint32_t frame) {
  u[0] = modus;
  schrijf_frame(u + 1, frame);
}
// Frame zoals de testvectoren het voor modus 1 en 2 geven: READ-ACK (type 4).
constexpr uint32_t override_frame(uint8_t id, uint16_t waarde) {
  return met_pariteit((4UL << 28) | ((uint32_t) id << 16) | waarde);
}

}  // namespace aurea_link2
}  // namespace esphome
