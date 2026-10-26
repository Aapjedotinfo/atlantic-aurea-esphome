#include "aurea_link2.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>

namespace esphome {
namespace aurea_link2 {

static const char *const TAG = "aurea_link2";

void AureaLink2::setup() {
  this->herbereken_tabel_(false);
  ESP_LOGCONFIG(TAG, "AVR-link v2: %u regels in de tabel, CRC 0x%04X", (unsigned) this->regels_.size(),
                this->tabel_crc_);
}

// ───────────────────────── Ontvangen ─────────────────────────

void AureaLink2::loop() {
  uint8_t b;
  while (this->available()) {
    if (!this->read_byte(&b))
      break;
    this->laatste_byte_us_ = micros();
    const int8_t r = this->rx_.voer(b);
    if (r == Ontvanger::BERICHT) {
      this->verwerk_();
    } else if (r == Ontvanger::FOUT) {
      // Bericht met slechte CRC of lengte: de ontvanger gooit nu alles weg tot er 5 ms stilte is.
      this->bad_crc_++;
      ESP_LOGW(TAG, "Bericht van de AVR met slechte CRC of lengte (%u tot nu toe)", this->bad_crc_);
    }
  }
  // Minstens 5 ms geen byte: resync (afgebroken bericht of einde van de weggooifase).
  if ((uint32_t) (micros() - this->laatste_byte_us_) >= STILTE_MS * 1000UL)
    this->rx_.stilte();

  // Een VRAAG waarop wij te laat zijn: de AVR heeft zijn standaardactie al gedaan.
  if (this->vraag_.open && (int32_t) (millis() - this->vraag_.einde_ms) >= 0) {
    this->vraag_.open = false;
    this->vraag_onbeantw_++;
    ESP_LOGW(TAG, "Geen antwoord op VRAAG ID %u (%s) binnen de deadline - de AVR doet de standaardactie (%s)",
             this->vraag_.id, this->vraag_.richting ? "K>T" : "T>K",
             this->vraag_.standaard ? "blokkeren" : "doorgeven");
  }

  // Een wachtende TABEL (HELLO of STATUS met afwijkende CRC, of een wijziging).
  if (this->tabel_wens_ && this->protocol_ok_ && !this->tabel_afgewezen_ &&
      (this->tabel_verstuurd_ms_ == 0 || (millis() - this->tabel_verstuurd_ms_) >= TABEL_HERHAAL_MS)) {
    this->tabel_wens_ = false;
    this->stuur_tabel_();
  }

  this->flush_tx_();

  const bool snel = !this->uit_.empty() || this->vraag_.open;
  if (snel && !this->hf_aan_) {
    this->hf_.start();
    this->hf_aan_ = true;
  } else if (!snel && this->hf_aan_) {
    this->hf_.stop();
    this->hf_aan_ = false;
  }
}

void AureaLink2::verwerk_() {
  const uint8_t type = this->rx_.type;
  const int vast = type_lengte(type);
  if (vast == -2)
    return;  // onbekend type met goede CRC: negeren (uitbreiding)
  if (vast >= 0 && this->rx_.lengte != vast) {
    this->bad_len_++;
    ESP_LOGW(TAG, "Bericht type 0x%02X met lengte %u (verwacht %d) genegeerd", type, this->rx_.lengte, vast);
    return;
  }

  // Het volgnummer van de AVR telt per verzonden bericht: een gat is een verloren bericht.
  if (this->rx_seq_bekend_) {
    const uint8_t gap = (uint8_t) (this->rx_.volgnr - this->rx_seq_ - 1);
    if (gap) {
      this->missed_ = (uint16_t) (this->missed_ + gap);
      ESP_LOGW(TAG, "%u bericht(en) van de AVR gemist (%u totaal)", gap, this->missed_);
    }
  }
  this->rx_seq_ = this->rx_.volgnr;
  this->rx_seq_bekend_ = true;

  switch (type) {
    case T_HELLO: this->handle_hello_(); break;
    case T_ACK: this->handle_ack_(); break;
    case T_VRAAG: this->handle_vraag_(); break;
    case T_STATUS: this->handle_status_(); break;
    case T_EVENT: this->handle_event_(); break;
    default: break;  // TABEL, ANTWOORD, COMMANDO en INJECT zijn onze eigen richting
  }
}

void AureaLink2::melding_(const std::string &tekst, bool waarschuwing) {
  if (waarschuwing)
    ESP_LOGW(TAG, "%s", tekst.c_str());
  else
    ESP_LOGI(TAG, "%s", tekst.c_str());
  if (this->melding_sensor_ != nullptr && tekst != this->laatste_melding_)
    this->melding_sensor_->publish_state(tekst);
  this->laatste_melding_ = tekst;
}

void AureaLink2::handle_hello_() {
  Hello h{};
  if (!lees_hello(this->rx_.payload, this->rx_.lengte, h))
    return;

  char buf[80];
  snprintf(buf, sizeof(buf), "%u.%u.%u (20%02u-%02u-%02u) protocol %u", h.sw_hoofd, h.sw_neven, h.sw_patch,
           h.jaar, h.maand, h.dag, h.protocol);
  this->versie_tekst_ = buf;
  if (this->versie_sensor_ != nullptr && this->versie_sensor_->state != this->versie_tekst_)
    this->versie_sensor_->publish_state(this->versie_tekst_);
  ESP_LOGI(TAG, "HELLO van de AVR: %s, tabel-CRC 0x%04X, resetreden 0x%02X, start %u, kenmerken 0x%04X", buf,
           h.tabel_crc, h.resetreden, h.bootteller, h.kenmerken);

  // Een onbekend protocol sturen we niets: de berichten zouden anders worden gelezen als iets wat ze niet zijn.
  this->protocol_ok_ = (h.protocol == PROTOCOL_VERSIE);
  const bool ouder = (h.sw_hoofd < this->min_sw_[0]) ||
                     (h.sw_hoofd == this->min_sw_[0] && h.sw_neven < this->min_sw_[1]) ||
                     (h.sw_hoofd == this->min_sw_[0] && h.sw_neven == this->min_sw_[1] &&
                      h.sw_patch < this->min_sw_[2]);
  if (h.protocol != PROTOCOL_VERSIE) {
    snprintf(buf, sizeof(buf), "AVR spreekt protocol %u, wij kennen %u: link uitgeschakeld", h.protocol,
             PROTOCOL_VERSIE);
    this->melding_(buf, true);
  } else if (ouder) {
    snprintf(buf, sizeof(buf), "AVR-firmware %u.%u.%u is ouder dan verwacht (%u.%u.%u)", h.sw_hoofd, h.sw_neven,
             h.sw_patch, this->min_sw_[0], this->min_sw_[1], this->min_sw_[2]);
    this->melding_(buf, true);
  } else if (!(h.kenmerken & 0x0008) || !(h.kenmerken & 0x0010)) {
    this->melding_("AVR ondersteunt EVENT of INJECT niet", true);
  } else if (h.max_regels < this->regels_.size()) {
    this->melding_("AVR kan minder regels aan dan onze tabel bevat", true);
  } else {
    this->melding_("OK", false);
  }
  if (!this->protocol_ok_)
    return;

  // Handshake (hoofdstuk 9): andere tabel-CRC dan de onze: TABEL; daarna COMMANDO.
  // Een HELLO betekent ook dat de AVR (opnieuw) is gestart: een eerder afgewezen tabel opnieuw proberen.
  this->tabel_afgewezen_ = false;
  if (h.tabel_crc != this->tabel_crc_) {
    this->tabel_ok_ = false;
    this->tabel_wens_ = true;
  }
  this->stuur_commando_();
}

void AureaLink2::handle_ack_() {
  const uint8_t seq = this->rx_.payload[0], status = this->rx_.payload[1];
  const char *wat = seq == this->tabel_seq_ ? "TABEL" : seq == this->cmd_seq_ ? "COMMANDO"
                    : seq == this->inject_seq_ ? "INJECT" : "bericht";
  if (seq == this->tabel_seq_ && this->tabel_verstuurd_ms_ != 0) {
    if (status == 0) {
      this->tabel_ok_ = true;
      ESP_LOGI(TAG, "TABEL aangenomen door de AVR (CRC 0x%04X)", this->tabel_crc_);
      if (this->laatste_melding_.rfind("TABEL", 0) == 0)
        this->melding_("OK", false);
    } else {
      this->tabel_ok_ = false;
      this->tabel_afgewezen_ = true;  // niet blijven herhalen tot de tabel verandert of de AVR herstart
      char buf[64];
      snprintf(buf, sizeof(buf), "TABEL afgewezen door de AVR (status %u)", status);
      this->melding_(buf, true);
    }
    return;
  }
  if (status != 0)
    ESP_LOGW(TAG, "AVR weigert %s (volgnr %u), status %u (1 lengte/inhoud, 2 buiten de grenzen, 4 bezet)", wat,
             seq, status);
}

void AureaLink2::handle_status_() {
  if (!lees_status(this->rx_.payload, this->rx_.lengte, this->status_))
    return;
  this->last_rx_ms_ = millis();
  this->have_data_ = true;

  // Zie de AVR een andere tabel dan de onze (ESP later gestart, of AVR teruggevallen), dan sturen we de onze.
  if (this->status_.tabel_crc != this->tabel_crc_ && !this->tabel_wens_ && !this->tabel_afgewezen_) {
    this->tabel_ok_ = false;
    this->tabel_wens_ = true;
  } else if (this->status_.tabel_crc == this->tabel_crc_) {
    this->tabel_ok_ = true;
  }
}

// Een rauw OpenTherm-frame zoals de brug het zag of stuurde (EVENT).
void AureaLink2::handle_event_() {
  const uint8_t dir = this->rx_.payload[0];
  if (dir > OT_TO_STAT)
    return;
  const uint32_t frame = lees_frame(this->rx_.payload + 1);
  const uint8_t id = (uint8_t) (frame >> 16);
  const uint8_t type = (uint8_t) ((frame >> 28) & 0x07);
  const uint16_t val = (uint16_t) (frame & 0xFFFF);

  // Een antwoord telt alleen als het er ook een is; UNKNOWN-DATAID of DATA-INVALID
  // dragen een leeg waardeveld dat anders als een geloofwaardige nul zou worden opgeslagen.
  const bool is_reply = (dir == OT_FROM_BOILER || dir == OT_TO_STAT);
  if (is_reply && type != 4 && type != 5) {
    if (this->raw_debug_)
      ESP_LOGD(TAG, "OT id=%3u afgewezen door de ketel (type %u)", id, type);
    return;
  }
  if (this->ot_ms_[dir][id] == 0)
    this->ids_seen_++;
  this->ot_val_[dir][id] = val;
  this->ot_ms_[dir][id] = millis();

  if (this->raw_debug_)
    ESP_LOGD(TAG, "OT %s id=%3u type=%u waarde=0x%04X",
             dir == OT_FROM_STAT     ? "thermostaat->"
             : dir == OT_FROM_BOILER ? "ketel->     "
             : dir == OT_TO_BOILER   ? "  ->ketel   "
                                     : "  ->thermost",
             id, type, val);
}

// ───────────────────────── VRAAG en ANTWOORD ─────────────────────────

void AureaLink2::handle_vraag_() {
  const uint8_t dir = this->rx_.payload[0];
  if (dir > K_NAAR_T)
    return;
  const uint32_t frame = lees_frame(this->rx_.payload + 1);
  const uint8_t id = (uint8_t) (frame >> 16);

  // Deadline en standaardactie komen uit onze eigen regel; zo weten we wanneer de AVR het opgeeft.
  uint32_t deadline = this->deadline_ms_;
  uint8_t standaard = 0;
  for (const auto &rc : this->regels_) {
    if (rc.actief && rc.r.id == id && rc.r.richting == dir && (rc.r.modus & 0x03) == 2) {
      if (rc.r.p0)
        deadline = (uint32_t) rc.r.p0 * 4;
      standaard = rc.r.p1;
      break;
    }
  }

  this->vraag_.open = true;
  this->vraag_.seq = this->rx_.volgnr;
  this->vraag_.richting = dir;
  this->vraag_.id = id;
  this->vraag_.frame = frame;
  this->vraag_.standaard = standaard;
  this->vraag_.einde_ms = millis() + deadline;

  const float waarde = (float) ((int16_t) (frame & 0xFFFF)) / 256.0f;
  if (this->raw_debug_)
    ESP_LOGD(TAG, "VRAAG %s id=%u frame=0x%08X (deadline %u ms)", dir ? "K>T" : "T>K", id, (unsigned) frame,
             (unsigned) deadline);

  bool behandeld = false;
  for (auto *t : this->vraag_triggers_) {
    if (!t->past(id, dir))
      continue;
    behandeld = true;
    t->trigger(id, dir, waarde, frame);
    if (!this->vraag_.open)
      break;
  }
  // Geen on_vraag voor dit ID: dan zelf meteen de standaardactie van de regel, niet wachten op de deadline.
  if (!behandeld && this->vraag_.open)
    this->antwoord_(standaard == 1 ? BLOKKEREN : DOORGEVEN, 0);
}

bool AureaLink2::antwoord_(uint8_t actie, uint32_t frame) {
  if (!this->vraag_.open) {
    ESP_LOGW(TAG, "Antwoord gegeven terwijl er geen VRAAG openstaat (te laat of al beantwoord)");
    return false;
  }
  uint8_t pl[6];
  antwoord_payload(pl, this->vraag_.seq, actie, frame);
  this->vraag_.open = false;
  this->zet_(T_ANTWOORD, pl, sizeof(pl), true);
  return true;
}

bool AureaLink2::antwoord_doorgeven() { return this->antwoord_(DOORGEVEN, 0); }
bool AureaLink2::antwoord_blokkeren() { return this->antwoord_(BLOKKEREN, 0); }

bool AureaLink2::antwoord_vervangen_frame(uint32_t frame) {
  if (!this->vraag_.open) {
    ESP_LOGW(TAG, "Vervangen zonder openstaande VRAAG");
    return false;
  }
  // De AVR beschouwt een frame met ander Data-ID of een verkeerd type als "geen antwoord".
  const uint8_t type = (uint8_t) ((frame >> 28) & 0x07);
  const bool type_ok = this->vraag_.richting == T_NAAR_K ? (type <= 1) : (type >= 4 && type <= 7);
  if ((uint8_t) (frame >> 16) != this->vraag_.id || !type_ok) {
    ESP_LOGW(TAG, "Vervangend frame 0x%08X past niet bij VRAAG ID %u (%s): niet verstuurd", (unsigned) frame,
             this->vraag_.id, this->vraag_.richting ? "K>T" : "T>K");
    return false;
  }
  return this->antwoord_(VERVANGEN, frame);
}

bool AureaLink2::antwoord_vervangen_waarde(float waarde) {
  if (!this->vraag_.open) {
    ESP_LOGW(TAG, "Vervangen zonder openstaande VRAAG");
    return false;
  }
  const uint32_t f = met_pariteit((this->vraag_.frame & 0x7FFF0000UL) | f88_from_(waarde));
  return this->antwoord_vervangen_frame(f);
}

// ───────────────────────── Zenden ─────────────────────────

// Zet een bericht in de wachtrij; het volgnummer krijgt het pas bij het verzenden.
// Een ANTWOORD gaat voor alles.
void AureaLink2::zet_(uint8_t type, const uint8_t *pl, uint8_t len, bool voorrang) {
  if (this->uit_.size() >= 12) {
    ESP_LOGW(TAG, "Zendwachtrij vol, bericht type 0x%02X weggegooid", type);
    return;
  }
  Uit u{};
  u.type = type;
  u.len = len;
  for (uint8_t i = 0; i < len; i++)
    u.d[i] = pl[i];
  if (voorrang)
    this->uit_.insert(this->uit_.begin(), u);
  else
    this->uit_.push_back(u);
}

void AureaLink2::flush_tx_() {
  if (this->uit_.empty())
    return;
  if ((int32_t) (micros() - this->tx_vrij_us_) < 0)
    return;  // minstens 5 ms stilte tussen twee berichten
  const Uit u = this->uit_.front();
  this->uit_.erase(this->uit_.begin());

  const uint8_t seq = this->tx_seq_++;
  if (u.type == T_TABEL) this->tabel_seq_ = seq;
  else if (u.type == T_COMMANDO) this->cmd_seq_ = seq;
  else if (u.type == T_INJECT) this->inject_seq_ = seq;

  uint8_t buf[MAX_BERICHT];
  const size_t n = bouw_bericht(buf, u.type, seq, u.d, u.len);
  this->write_array(buf, n);
  // Verzendtijd (260 us per byte) plus de 5 ms pauze, met 1 ms marge.
  this->tx_vrij_us_ = micros() + (uint32_t) n * 260UL + (STILTE_MS + 1) * 1000UL;
}

Tabel AureaLink2::bouw_tabel_() const {
  Tabel t;
  t.versie = this->tabel_versie_;
  t.deadline_ms = this->deadline_ms_;
  t.ketel_ms = this->ketel_ms_;
  t.esp_weg_s = this->esp_weg_s_;
  t.nood_s = this->nood_s_;
  t.rem_hold_s = this->rem_hold_s_;
  for (const auto &rc : this->regels_) {
    if (rc.actief && t.n < MAX_REGELS)
      t.r[t.n++] = rc.r;
  }
  return t;
}

void AureaLink2::herbereken_tabel_(bool gewijzigd) {
  if (gewijzigd)
    this->tabel_versie_++;  // alleen voor de weergave; de CRC verandert toch al
  this->tabel_crc_ = tabel_crc(this->bouw_tabel_());
  if (gewijzigd) {
    this->tabel_ok_ = false;
    this->tabel_afgewezen_ = false;
    this->tabel_wens_ = true;
  }
}

void AureaLink2::stuur_tabel_() {
  const Tabel t = this->bouw_tabel_();
  if (!tabel_geldig(t)) {
    this->melding_("Eigen tabel ongeldig (grenzen of dubbele regel): niet verstuurd", true);
    this->tabel_afgewezen_ = true;
    return;
  }
  uint8_t pl[12 + 5 * MAX_REGELS + 2];
  const size_t n = tabel_payload(t, pl);
  this->zet_(T_TABEL, pl, (uint8_t) n);
  this->tabel_verstuurd_ms_ = millis();
  ESP_LOGI(TAG, "TABEL naar de AVR: versie %u, %u regels, CRC 0x%04X", t.versie, t.n, this->tabel_crc_);
}

void AureaLink2::update() {
  if (this->protocol_ok_)
    this->stuur_commando_();
}

void AureaLink2::stuur_commando_() {
  Commando c;
  c.flags1 = (uint8_t) ((this->brake_on_ ? 0x01 : 0) | (this->subst_temp_ ? 0x02 : 0) |
                        (this->invert_stat_ ? 0x04 : 0) | (this->invert_boiler_ ? 0x08 : 0) |
                        (this->relay_on_ ? 0x10 : 0) | (this->lamp_invert_ ? 0x20 : 0) |
                        (this->sa_flame_ ? 0x40 : 0) | (this->force_sa_ ? 0x80 : 0));
  c.flags2 = (uint8_t) ((this->inv_tx_stat_ ? 0x01 : 0) | (this->inv_tx_boiler_ ? 0x02 : 0) |
                        (this->swap_ch_ ? 0x04 : 0) | (this->ch_off_ ? 0x08 : 0) |
                        (this->demand_enabled_ ? 0x10 : 0) | (this->brake_direct_ ? 0x20 : 0) |
                        (this->boiler_type_ << 6));
  c.brake_setpoint = (uint8_t) lroundf(this->brake_setpoint_);
  c.wp_supply = this->wp_supply_;
  c.wp_return = this->wp_return_;
  c.override_sp = this->override_setpoint_;
  c.lamp = this->lamp_mode_;
  c.sa_mod = this->sa_modulation_;
  c.test_out = this->test_out_;
  c.boiler_sp = this->boiler_setpoint_;
  c.max_mod = this->max_mod_;  // 0 = niet ingrijpen
  c.geldig_s = this->geldig_s_;
  uint8_t pl[15];
  commando_payload(c, pl);
  this->zet_(T_COMMANDO, pl, sizeof(pl));
}

// ───────────────────────── De tabel instellen ─────────────────────────

void AureaLink2::add_regel(uint8_t id, uint8_t richting, uint8_t modus, uint8_t p0, uint8_t p1, const char *naam,
                           bool actief) {
  this->regels_.push_back(RegelCfg{Regel{id, richting, modus, p0, p1}, naam, actief});
}

bool AureaLink2::set_regel_actief(const char *naam, bool actief) {
  for (auto &rc : this->regels_) {
    if (rc.naam == naam) {
      if (rc.actief == actief)
        return true;
      rc.actief = actief;
      this->herbereken_tabel_(true);
      ESP_LOGI(TAG, "Regel '%s' %s", naam, actief ? "actief" : "uit");
      return true;
    }
  }
  ESP_LOGW(TAG, "Regel '%s' bestaat niet", naam);
  return false;
}

void AureaLink2::set_deadline_ms(uint32_t ms) {
  this->deadline_ms_ = (uint16_t) clamp<uint32_t>(ms, 30, 600);
  this->herbereken_tabel_(true);
}
void AureaLink2::set_ketel_ms(uint32_t ms) {
  this->ketel_ms_ = (uint16_t) clamp<uint32_t>(ms, 100, 600);
  this->herbereken_tabel_(true);
}
void AureaLink2::set_esp_weg_s(uint32_t s) {
  this->esp_weg_s_ = (uint16_t) clamp<uint32_t>(s, 10, 600);
  if (this->nood_s_ < this->esp_weg_s_)
    this->nood_s_ = this->esp_weg_s_;
  this->herbereken_tabel_(true);
}
void AureaLink2::set_nood_s(uint32_t s) {
  this->nood_s_ = (uint16_t) clamp<uint32_t>(s, std::max<uint32_t>(60, this->esp_weg_s_), 1800);
  this->herbereken_tabel_(true);
}
void AureaLink2::set_rem_hold_s(uint32_t s) {
  this->rem_hold_s_ = (uint16_t) clamp<uint32_t>(s, 60, 900);
  this->herbereken_tabel_(true);
}

// ───────────────────────── Waarnemingen per Data-ID ─────────────────────────

static float f88_(uint16_t v) { return (float) ((int16_t) v) / 256.0f; }
static const uint32_t OT_STALE_MS = 120000;

float AureaLink2::get_ot(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return NAN;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0 || (millis() - t) > OT_STALE_MS)
    return NAN;
  return f88_(this->ot_val_[dir][data_id]);
}

int AureaLink2::get_ot_raw(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return -1;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0 || (millis() - t) > OT_STALE_MS)
    return -1;
  return this->ot_val_[dir][data_id];
}

int AureaLink2::get_ot_age(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return -1;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0)
    return -1;
  return (int) ((millis() - t) / 1000);
}

// ───────────────────────── INJECT ─────────────────────────

// f8.8: hoge byte hele graden met teken, lage byte 1/256.
uint16_t AureaLink2::f88_from_(float v) {
  if (std::isnan(v))
    return 0;
  if (v > 127.0f)
    v = 127.0f;
  if (v < -128.0f)
    v = -128.0f;
  return (uint16_t) ((int16_t) lroundf(v * 256.0f));
}

void AureaLink2::send_inject_(uint8_t mode, uint32_t frame) {
  uint8_t pl[5];
  inject_payload(pl, mode, frame);
  this->zet_(T_INJECT, pl, sizeof(pl));
}

void AureaLink2::send_to_boiler(uint32_t frame) { this->send_inject_(0, met_pariteit(frame)); }

void AureaLink2::write_boiler(uint8_t data_id, float value) {
  const uint32_t f = (1UL << 28) | ((uint32_t) data_id << 16) | f88_from_(value);
  ESP_LOGD(TAG, "Naar de ketel: schrijf ID %u = %.1f", data_id, value);
  this->send_to_boiler(f);
}

void AureaLink2::read_boiler(uint8_t data_id) { this->send_to_boiler((uint32_t) data_id << 16); }

void AureaLink2::override_to_thermostat(uint8_t data_id, float value) {
  ESP_LOGD(TAG, "Naar de thermostaat: ID %u wordt %.1f", data_id, value);
  this->send_inject_(1, override_frame(data_id, f88_from_(value)));
}

void AureaLink2::clear_override(uint8_t data_id) { this->send_inject_(2, override_frame(data_id, 0)); }

// ───────────────────────── Sturing ─────────────────────────

void AureaLink2::set_brake(bool on) {
  if (on == this->brake_on_)
    return;
  this->brake_on_ = on;
  ESP_LOGI(TAG, "Rem %s", on ? "aan - ketel wordt afgeknepen" : "uit - ketel mag leveren");
}

void AureaLink2::set_brake_direct(bool on) {
  if (on == this->brake_direct_)
    return;
  this->brake_direct_ = on;
  ESP_LOGI(TAG, "Rem %s", on ? "direct (houdtijd overgeslagen)" : "weer met houdtijd");
}

void AureaLink2::set_ch_off(bool on) {
  if (on == this->ch_off_)
    return;
  this->ch_off_ = on;
  ESP_LOGI(TAG, "CV-vlag naar de ketel %s", on ? "onderdrukt - ketelpomp blijft stil" : "doorgegeven");
}

void AureaLink2::set_max_modulation(float pct) {
  // 100 of meer (of NaN) is geen begrenzing; 0 is voor de brug "niet ingrijpen", dus het minimum is 1 %.
  const uint8_t v = (std::isnan(pct) || pct >= 100.0f) ? 0 : (uint8_t) clamp(lroundf(pct), 1L, 99L);
  if (v == this->max_mod_)
    return;
  this->max_mod_ = v;
  if (v)
    ESP_LOGI(TAG, "Maximale ketelmodulatie %u %%", v);
  else
    ESP_LOGI(TAG, "Ketelmodulatie weer vrij");
}

void AureaLink2::set_brake_setpoint(float c) { this->brake_setpoint_ = clamp(c, 0.0f, 90.0f); }

void AureaLink2::set_subst_temp(bool on) {
  if (on == this->subst_temp_)
    return;
  this->subst_temp_ = on;
  ESP_LOGI(TAG, "Temperatuursubstitutie naar de thermostaat %s", on ? "aan" : "uit");
}

void AureaLink2::set_invert_stat(bool on) {
  if (on == this->invert_stat_)
    return;
  this->invert_stat_ = on;
  ESP_LOGI(TAG, "Leesrichting thermostaatkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink2::set_invert_boiler(bool on) {
  if (on == this->invert_boiler_)
    return;
  this->invert_boiler_ = on;
  ESP_LOGI(TAG, "Leesrichting ketelkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink2::set_relay(bool on) {
  if (on == this->relay_on_)
    return;
  this->relay_on_ = on;
  ESP_LOGI(TAG, "Noodbrug (K1+K2) op verzoek %s", on ? "aan" : "uit");
}

void AureaLink2::set_lamp_invert(bool on) {
  if (on == this->lamp_invert_)
    return;
  this->lamp_invert_ = on;
  ESP_LOGI(TAG, "Statuslampje %s", on ? "omgekeerd" : "normaal");
}

void AureaLink2::set_override_setpoint(float c) {
  // Buiten een zinnig kamerbereik betekent "uit": 0 heft de override op.
  const int16_t v = (c < 5.0f || c > 30.0f) ? 0 : (int16_t) lroundf(c * 10.0f);
  if (v == this->override_setpoint_)
    return;
  this->override_setpoint_ = v;
  if (v == 0)
    ESP_LOGI(TAG, "Override opgeheven - de thermostaat regelt weer zelf");
  else
    ESP_LOGI(TAG, "Thermostaat op afstand gezet op %.1f C", v / 10.0f);
}

void AureaLink2::set_standalone_modulation(float pct) {
  this->sa_modulation_ = (uint8_t) clamp((int) lroundf(pct), 0, 100);
}

void AureaLink2::set_standalone_flame(bool on) { this->sa_flame_ = on; }

void AureaLink2::set_force_standalone(bool on) {
  if (on == this->force_sa_)
    return;
  this->force_sa_ = on;
  ESP_LOGI(TAG, "Zelfstandige stand %s", on ? "afgedwongen" : "weer automatisch");
}

void AureaLink2::set_invert_tx_stat(bool on) {
  if (on == this->inv_tx_stat_)
    return;
  this->inv_tx_stat_ = on;
  ESP_LOGI(TAG, "Zendpolariteit thermostaatkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink2::set_invert_tx_boiler(bool on) {
  if (on == this->inv_tx_boiler_)
    return;
  this->inv_tx_boiler_ = on;
  ESP_LOGI(TAG, "Zendpolariteit ketelkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink2::set_swap_channels(bool on) {
  if (on == this->swap_ch_)
    return;
  this->swap_ch_ = on;
  ESP_LOGW(TAG, "Kanalen %s: thermostaat en ketel zijn %s", on ? "gewisseld" : "normaal",
           on ? "omgedraaid" : "zoals afgeleid uit de firmware");
}

void AureaLink2::set_test_out(uint8_t mask, bool on) {
  const uint8_t was = this->test_out_;
  if (on)
    this->test_out_ |= (mask & 0x1F);
  else
    this->test_out_ &= (uint8_t) ~mask;
  if (this->test_out_ != was)
    ESP_LOGW(TAG, "Testuitgangen nu 0x%02X - dit stuurt pinnen aan waarvan de functie onbekend is",
             this->test_out_);
}

void AureaLink2::set_boiler_setpoint(float c) {
  const uint8_t v = (uint8_t) clamp((int) lroundf(c), 0, 90);
  if (v == this->boiler_setpoint_)
    return;
  this->boiler_setpoint_ = v;
  if (v == 0)
    ESP_LOGI(TAG, "Ketel uit");
  else
    ESP_LOGI(TAG, "Ketel rechtstreeks op %u C gezet", v);
}

void AureaLink2::set_demand_enabled(bool on) {
  if (on == this->demand_enabled_)
    return;
  this->demand_enabled_ = on;
  ESP_LOGI(TAG, "Ketelvraag-relais K3 %s", on ? "vrijgegeven" : "geblokkeerd");
}

void AureaLink2::set_lamp_mode(int mode) { this->lamp_mode_ = (uint8_t) clamp(mode, 0, 4); }

void AureaLink2::set_wp_temps(float supply, float ret) {
  // NaN = "de warmtepomp weet het even niet": dan liever de vorige waarde dan een nul (ijskoud water).
  if (!std::isnan(supply))
    this->wp_supply_ = (int16_t) clamp(lroundf(supply * 10.0f), -300L, 1500L);
  if (!std::isnan(ret))
    this->wp_return_ = (int16_t) clamp(lroundf(ret * 10.0f), -300L, 1500L);
}

void AureaLink2::set_boiler_type(int type) {
  const uint8_t v = (uint8_t) clamp(type, 0, 2);
  if (v == this->boiler_type_)
    return;
  this->boiler_type_ = v;
  ESP_LOGI(TAG, "Keteltype: %s", v == KETEL_OPENTHERM ? "OpenTherm-ketel" : v == KETEL_AANUIT ? "aan/uit-ketel"
                                                                                                : "volgens dipschakelaar");
}

void AureaLink2::set_valid_time(int s) {
  this->geldig_s_ = s <= 0 ? 0 : (uint8_t) clamp(s, 10, 255);
}

void AureaLink2::dump_config() {
  ESP_LOGCONFIG(TAG, "Aurea AVR-link v2 (protocol %u):", PROTOCOL_VERSIE);
  ESP_LOGCONFIG(TAG, "  Tabelversie %u, %u regels, CRC 0x%04X", this->tabel_versie_,
                (unsigned) this->regels_.size(), this->tabel_crc_);
  ESP_LOGCONFIG(TAG, "  Deadline ESP-antwoord %u ms, wachttijd ketel %u ms", this->deadline_ms_, this->ketel_ms_);
  ESP_LOGCONFIG(TAG, "  ESP-weg-tijd %u s, noodbrugtijd %u s, rem-houdtijd %u s", this->esp_weg_s_, this->nood_s_,
                this->rem_hold_s_);
  for (const auto &rc : this->regels_)
    ESP_LOGCONFIG(TAG, "  Regel %-16s id=%3u %s modus=0x%02X p=%02X%02X %s", rc.naam.c_str(), rc.r.id,
                  rc.r.richting ? "K>T" : "T>K", rc.r.modus, rc.r.p0, rc.r.p1, rc.actief ? "" : "(uit)");
  // UART-instellingen (38400 8N1) worden bij het valideren van de yaml gecontroleerd.
}

}  // namespace aurea_link2
}  // namespace esphome
