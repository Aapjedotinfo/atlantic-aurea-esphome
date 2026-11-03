#include "aurea_link_org.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace aurea_link_org {

static const char *const TAG = "aurea_link_org";

// Tussen twee bytes van een frame zit ~1 ms. Meer dan 50 ms stilte = een nieuw frame begint.
static const uint32_t BYTE_GAP_MS = 50;

uint8_t AureaLinkOrg::sum8_(const uint8_t *p, uint8_t n) {
  uint8_t s = 0;
  for (uint8_t i = 0; i < n; i++) s += p[i];
  return s;
}

void AureaLinkOrg::setup() {}

void AureaLinkOrg::dump_config() {
  ESP_LOGCONFIG(TAG, "Aurea link, originele IC2 (Haddon T1.1):");
  ESP_LOGCONFIG(TAG, "  Alleen luisteren: %s", YESNO(this->listen_only_));
  ESP_LOGCONFIG(TAG, "  Antwoordvertraging: %u ms", this->reply_delay_ms_);
  ESP_LOGCONFIG(TAG, "  Schop na stilte: %u s", this->kick_after_s_);
  ESP_LOGCONFIG(TAG, "  Link-timeout: %u s", this->link_timeout_s_);
}

bool AureaLinkOrg::get_link_ok() const {
  return this->last_rx_ms_ != 0 && (millis() - this->last_rx_ms_) < (uint32_t) this->link_timeout_s_ * 1000UL;
}

float AureaLinkOrg::get_requested_setpoint() const {
  return this->get_link_ok() ? (float) this->sp_whole_ : NAN;
}

float AureaLinkOrg::get_room_setpoint() const {
  // 0 betekent "geen vraag" in het IC2-frame, dus geen kamersetpoint.
  if (!this->get_link_ok() || this->room_whole_ == 0) return NAN;
  return this->room_whole_ + this->room_frac_ / 256.0f;
}

float AureaLinkOrg::get_ot(int data_id, OtDirection dir) const {
  if (dir == OT_FROM_STAT) {
    if (data_id == 1) return this->get_requested_setpoint();
    if (data_id == 16) return this->get_room_setpoint();
  } else if (dir == OT_TO_STAT) {
    if (data_id == 25) return this->supply_;
    if (data_id == 28) return this->return_;
    if (data_id == 27) return this->outside_;
  }
  return NAN;
}

int AureaLinkOrg::get_ot_age(int data_id, OtDirection dir) const {
  if (dir == OT_FROM_STAT && (data_id == 1 || data_id == 16) && this->last_rx_ms_ != 0)
    return (int) ((millis() - this->last_rx_ms_) / 1000);
  return -1;
}

void AureaLinkOrg::set_wp_temps(float supply, float ret) {
  this->supply_ = supply;
  this->return_ = ret;
}

void AureaLinkOrg::set_standalone_modulation(float pct) {
  if (std::isnan(pct) || pct < 0) {
    this->stand_ = 0;
    return;
  }
  this->stand_ = (uint8_t) std::min(40.0f, std::round(pct / 10.0f));
}

void AureaLinkOrg::write_boiler(uint8_t data_id, float value) {
  if (data_id == 27) this->outside_ = value;
}

uint8_t AureaLinkOrg::build_stand_() const {
  // Rem aan = de warmtepomp heeft het gezag over de ketel. De IC2 beschouwt stand 0 als "warmtepomp doet niets" en
  // geeft de ketel dan vrij. Daarom minstens 1 zolang de rem aan staat, ook als de compressor stilstaat.
  if (this->brake_on_) return std::max<uint8_t>(1, this->stand_);
  return this->stand_;
}

void AureaLinkOrg::loop() {
  const uint32_t now = millis();
  while (this->available()) {
    uint8_t byte;
    if (!this->read_byte(&byte)) break;
    if (this->rx_idx_ > 0 && now - this->last_byte_ms_ > BYTE_GAP_MS) this->rx_idx_ = 0;  // hersynchroniseren
    this->last_byte_ms_ = now;
    this->rx_[this->rx_idx_++] = byte;
    if (this->rx_idx_ == FRAME_LEN) {
      this->rx_idx_ = 0;
      if (sum8_(this->rx_, FRAME_LEN - 1) == this->rx_[FRAME_LEN - 1]) {
        this->handle_frame_(this->rx_);
      } else {
        this->bad_++;
        ESP_LOGW(TAG, "Frame met foute som genegeerd (%d tot nu toe)", this->bad_);
      }
    }
  }
  if (this->reply_pending_ && (int32_t) (now - this->reply_at_ms_) >= 0) {
    this->reply_pending_ = false;
    this->send_frame_();
  }
}

void AureaLinkOrg::handle_frame_(const uint8_t *b) {
  const bool eerste = !this->link_seen_;
  this->link_seen_ = true;
  this->last_rx_ms_ = millis();
  this->frames_++;
  this->sp_whole_ = b[0];
  this->room_whole_ = b[1];
  this->room_frac_ = b[2];
  this->last_idx0_ = b[0];
  this->last_idx1_ = b[1];
  this->last_idx2_ = b[2];

  // De IC2 geeft de zes laatst van ons ontvangen bytes (idx 5..10) terug. Komen die overeen met wat wij stuurden,
  // dan werkt de richting ESP32 -> IC2.
  if (this->tx_valid_) {
    const bool ok = std::memcmp(b + 5, this->tx_ + 5, 6) == 0;
    if (!this->echo_known_ || ok != this->echo_ok_)
      ESP_LOGI(TAG, "Echo van de IC2 %s", ok ? "klopt met ons laatste frame" : "wijkt af van ons laatste frame");
    this->echo_known_ = true;
    this->echo_ok_ = ok;
  }

  const bool uitgebreid = this->raw_debug_ || this->listen_only_ || eerste;
  if (uitgebreid) {
    ESP_LOGI(TAG, "IC2 -> ESP: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X  (ketelsetpoint %u, kamer %u+%u/256)",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[0], b[1], b[2]);
  } else {
    ESP_LOGD(TAG, "IC2 -> ESP: ketelsetpoint %u, kamer %u+%u/256", b[0], b[1], b[2]);
  }

  if (!this->listen_only_) {
    this->reply_pending_ = true;
    this->reply_at_ms_ = millis() + this->reply_delay_ms_;
  }
}

void AureaLinkOrg::send_frame_() {
  uint8_t *t = this->tx_;
  const bool beschikbaar = this->brake_on_;
  // Byte 0: 0xF0 = warmtepomp niet beschikbaar (de IC2 gaat naar boiler-modus). Anders gewoon de echo van zijn byte 0.
  t[0] = beschikbaar ? std::min<uint8_t>(this->last_idx0_, NOT_AVAILABLE - 1) : NOT_AVAILABLE;
  t[1] = this->last_idx1_;
  t[2] = this->last_idx2_;
  t[3] = 0;                 // credit-teller van de originele M1.1, door de IC2 genegeerd
  t[4] = this->sp_whole_;   // gevraagde aanvoer, genegeerd
  t[5] = this->sp_whole_;   // doelwaarde, genegeerd
  t[6] = this->build_stand_();
  auto temp = [](float v) -> uint8_t {
    if (std::isnan(v)) return 0;
    return (uint8_t) std::max(0.0f, std::min(255.0f, std::round(v)));
  };
  t[7] = temp(this->supply_);  // wordt Data-ID 25 naar de thermostaat
  t[8] = temp(this->return_);  // wordt Data-ID 28
  t[9] = std::isnan(this->outside_) ? 0 : (uint8_t) (int8_t) std::max(-127.0f, std::min(127.0f, std::round(this->outside_)));
  t[10] = this->fault_;        // <> 0 zet de IC2 in boiler-modus en laat zijn LED knipperen
  t[11] = sum8_(t, FRAME_LEN - 1);
  this->write_array(t, FRAME_LEN);
  this->tx_valid_ = true;
  this->last_tx_ms_ = millis();
  if (this->raw_debug_)
    ESP_LOGI(TAG, "ESP -> IC2: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", t[0], t[1], t[2], t[3], t[4],
             t[5], t[6], t[7], t[8], t[9], t[10], t[11]);
  else
    ESP_LOGD(TAG, "ESP -> IC2: warmtepomp %s, stand %u, aanvoer %u, retour %u", beschikbaar ? "beschikbaar" : "niet beschikbaar",
             t[6], t[7], t[8]);
}

void AureaLinkOrg::update() {
  // De IC2 stuurt alleen als wij 5 s stil zijn geweest en hij net met de thermostaat gepraat heeft. Hoort hij van ons niets
  // meer, dan zet hij na 240 s de noodbrug om. Blijft het stil, dan sturen we zelf een frame. Dat zou de IC2 in
  // beweging moeten zetten; zo niet, dan staat de link niet goed en zie je dat aan "Brug verbonden".
  if (this->listen_only_) return;
  const uint32_t now = millis();
  const uint32_t kick = (uint32_t) this->kick_after_s_ * 1000UL;
  if (now < kick) return;
  if ((this->last_rx_ms_ == 0 || now - this->last_rx_ms_ > kick) && (this->last_tx_ms_ == 0 || now - this->last_tx_ms_ > kick))
    this->send_frame_();
}

}  // namespace aurea_link_org
}  // namespace esphome
