#include "chofu_wp.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace chofu_wp {

static const char *const TAG = "chofu_wp";

// ═══════════════════════════════════════════════════════════════
//  De 4 roterende zend-telegrammen (controlbox -> warmtepomp)
//  data0/1/3 zijn vast; data2 wordt per keer opgebouwd (on/off + stand + CRC).
//  Bytes exact uit de werkende JGC-sketch.
// ═══════════════════════════════════════════════════════════════
static const uint8_t DATA0[] = {0x19, 0x00, 0x08, 0x00, 0x00, 0x00, 0xD9, 0xB5};
static const uint8_t DATA1[] = {0x19, 0x01, 0x0C, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0xAA, 0x35};
static const uint8_t DATA3[] = {0x19, 0x03, 0x08, 0xB2, 0x02, 0x00, 0xC1, 0x9A};

// Geldige berichtlengtes vanuit de WP (msgLength = lengtebyte + 1), uit JGC.
static const uint8_t VALID_LENGTHS[] = {13, 14, 19, 21};

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

void ChofuWP::setup() {
  ESP_LOGI(TAG, "Chofu 0x19-protocol gestart (666 baud, 4 roterende telegrammen)");
}

void ChofuWP::dump_config() {
  ESP_LOGCONFIG(TAG, "Chofu WP (666 baud):");
  ESP_LOGCONFIG(TAG, "  Setpoint aanvoer: %.1f°C (bereik %.0f-%.0f)", setpoint_, setpoint_min_,
                setpoint_max_);
  ESP_LOGCONFIG(TAG, "  Bedrijfsmodus: %s", cooling_ ? "koelen" : "verwarmen");
  ESP_LOGCONFIG(TAG, "  Regeling: delta-T-band %.1f-%.1f°C, max stand %u, stap elke %us", dt_low_,
                dt_high_, max_stand_, (unsigned) (step_interval_ms_ / 1000));
  this->check_uart_settings(666);
}

// ═══════════════════════════════════════════════════════════════
//  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, geen reflectie)
//  CRC over heel bericht incl. de 2 CRC-bytes == 0 bij foutvrije ontvangst.
// ═══════════════════════════════════════════════════════════════
uint16_t ChofuWP::crc_ccitt_(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t) data[i] << 8;
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
  }
  return crc;
}

bool ChofuWP::valid_msg_length_(uint8_t len) {
  for (uint8_t v : VALID_LENGTHS)
    if (len == v)
      return true;
  return false;
}

// ═══════════════════════════════════════════════════════════════
//  loop(): RX-bytes verwerken + (op de juiste momenten) een telegram zenden
// ═══════════════════════════════════════════════════════════════
void ChofuWP::loop() {
  this->handle_rx_();
  this->maybe_send_();
}

// ═══════════════════════════════════════════════════════════════
//  RX - 0x91 berichten van de warmtepomp, per ID (state machine uit JGC)
// ═══════════════════════════════════════════════════════════════
void ChofuWP::handle_rx_() {
  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b))
      break;
    is_receiving_ = true;
    last_byte_ms_ = millis();  // Voor de lijn-stiltebewaking in maybe_send_()

    switch (rx_state_) {
      case WAIT_START:
        if (b == 0x91) {
          rx_state_ = READ_HEADER;
          rx_hdr_idx_ = 0;
          frame_[0] = b;
          frame_len_ = 1;
        }
        break;

      case READ_HEADER:
        if (frame_len_ < sizeof(frame_))
          frame_[frame_len_++] = b;
        if (rx_hdr_idx_ == 0) {
          rx_id_ = b;
          if (rx_id_ > 3) {  // ID moet 0-3 zijn
            this->abort_frame_("ongeldig ID");
            break;
          }
        } else if (rx_hdr_idx_ == 1) {
          uint8_t msg_length = b + 1;
          if (!valid_msg_length_(msg_length)) {
            this->abort_frame_("ongeldige lengte");
            break;
          }
          rx_datalen_ = msg_length - 3;
          rx_idx_ = 0;
          rx_state_ = READ_PAYLOAD;
        }
        rx_hdr_idx_++;
        break;

      case READ_PAYLOAD:
        if (frame_len_ < sizeof(frame_))
          frame_[frame_len_++] = b;
        rx_idx_++;
        if (rx_idx_ >= rx_datalen_) {
          // Frame compleet: 0x91 + ID + len + <datalen> payload (laatste 2 = CRC).
          // GEEN aparte end-byte - de volgende byte is al het volgende telegram.
          this->finish_frame_();
          rx_state_ = WAIT_START;
        }
        break;
    }
  }
}

// Frame compleet: CRC valideren (CRC over heel frame == 0) + waarden extraheren.
void ChofuWP::finish_frame_() {
  is_receiving_ = false;
  prev_msg_in_ended_ms_ = millis();  // ALTIJD - zo volgt de TX-timing de WP-cadans

  const uint16_t crc = crc_ccitt_(frame_, frame_len_);

  if (raw_debug_) {
    char hex[sizeof(frame_) * 3 + 1];
    for (size_t i = 0; i < frame_len_; i++)
      sprintf(hex + i * 3, "%02X ", frame_[i]);
    hex[frame_len_ * 3] = '\0';
    ESP_LOGD(TAG, "FRAME ID%u [%u] %s crc=%04X %s", rx_id_, (unsigned) frame_len_, hex, crc,
             crc == 0 ? "OK" : "FOUT");
  }

  if (crc != 0)
    return;  // Corrupt frame negeren

  last_valid_rx_ms_ = millis();

  // Temperaturen: signed 16-bit LITTLE-endian (LSB eerst), /10.
  switch (rx_id_) {
    case 1:
      // ID1 idx4 was aangemerkt als "defrost", maar metingen laten zien dat
      // die byte gewoon 0x01 wordt zodra de unit draait (en 0x00 als hij stil
      // staat) - het is een bedrijfsstatus, geen defrost. Tot we de echte
      // defrost-byte kennen niet meer als defrost publiceren.
      break;
    case 2:
      t_supply_ = (int16_t) ((frame_[4] << 8) | frame_[3]) / 10.0f;
      t_return_ = (int16_t) ((frame_[6] << 8) | frame_[5]) / 10.0f;
      t_outside_ = (int16_t) ((frame_[8] << 8) | frame_[7]) / 10.0f;
      break;
    case 3:
      wp_speed_ = frame_[9];
      wp_power_w_ = 25.6f * frame_[10];
      break;
    default:
      break;
  }
}

// Frame afgekeurd in de header (ongeldig ID/lengte). Belangrijk: ook hier de
// timing bijwerken, anders blijft is_receiving_ hangen en valt maybe_send_()
// terug op de blinde timeout - precies wat botsingen veroorzaakte.
void ChofuWP::abort_frame_(const char *reason) {
  if (raw_debug_)
    ESP_LOGD(TAG, "RX afgebroken (%s) - resync", reason);
  is_receiving_ = false;
  prev_msg_in_ended_ms_ = millis();
  rx_state_ = WAIT_START;
}

// ═══════════════════════════════════════════════════════════════
//  TX - stuur het volgende telegram als de master/slave-timing dat toelaat
// ═══════════════════════════════════════════════════════════════
void ChofuWP::maybe_send_() {
  const uint32_t now = millis();

  // HARDE botsingsbescherming: nooit zenden zolang er nog bytes binnendruppelen.
  // Vertrouwt niet op de parser-state (die kan ontsporen op een corrupt frame),
  // maar puur op de lijn zelf. Voorkomt dat ons telegram midden in een frame
  // van de warmtepomp belandt.
  if (now - last_byte_ms_ < LINE_IDLE_MS)
    return;

  const bool after_rx = (now - prev_msg_in_ended_ms_ >= SEND_DELAY_MS) && !is_receiving_;
  const bool timed_out = (now - prev_msg_in_ended_ms_ >= SEND_TIMEOUT_MS);
  const bool spaced = (now - prev_msg_sent_ms_ >= MIN_SEND_INTERVAL_MS);
  if (!((after_rx || timed_out) && spaced))
    return;

  switch (telegram_count_) {
    case 0:
      this->write_array(DATA0, sizeof(DATA0));
      break;
    case 1:
      this->write_array(DATA1, sizeof(DATA1));
      break;
    case 2: {
      uint8_t data2[8];
      this->build_data2_(data2);
      this->write_array(data2, sizeof(data2));
      break;
    }
    case 3:
      this->write_array(DATA3, sizeof(DATA3));
      break;
  }

  telegram_count_ = (telegram_count_ + 1) % 4;
  prev_msg_sent_ms_ = now;
}

// data2: 19 02 08 <modus> <stand> 00 <crc_hi> <crc_lo>
// modus: 0x00 = uit, 0x01 = verwarmen, 0x02 = koelen.
// LET OP: 0x02 komt uit één forumpost en is niet door JGC gebruikt - het is
// dus experimenteel. De WP negeert het mogelijk gewoon.
void ChofuWP::build_data2_(uint8_t *out) {
  out[0] = 0x19;
  out[1] = 0x02;
  out[2] = 0x08;
  if (stand_ == 0) {
    out[3] = 0x00;  // uit
    out[4] = 0x00;
  } else {
    out[3] = cooling_ ? 0x02 : 0x01;  // koelen of verwarmen
    out[4] = stand_;
  }
  out[5] = 0x00;
  uint16_t crc = crc_ccitt_(out, 6);
  out[6] = (crc >> 8) & 0xFF;  // high byte
  out[7] = crc & 0xFF;         // low byte
}

// ═══════════════════════════════════════════════════════════════
//  Regeling - elke 5s (update_interval). Simpel: aanvoer -> stand.
// ═══════════════════════════════════════════════════════════════
void ChofuWP::update() { this->run_control_(); }

void ChofuWP::run_control_() {
  const uint32_t now = millis();

  // Master-schakelaar uit -> alles uit
  if (!system_on_) {
    stand_ = 0;
    return;
  }

  // Handmatige modus -> stand rechtstreeks
  if (!auto_mode_) {
    stand_ = clamp<uint8_t>(manual_stand_, 0, max_stand_);
    return;
  }

  // Fail-safe: geen geldig telegram -> niet blind vermogen blijven vragen
  if (last_valid_rx_ms_ == 0 || (now - last_valid_rx_ms_) > COMM_TIMEOUT_MS) {
    if (stand_ != 0)
      ESP_LOGW(TAG, "Geen geldige communicatie - terug naar stand 0");
    stand_ = 0;
    return;
  }

  if (std::isnan(t_supply_) || std::isnan(t_return_))
    return;  // Nog geen data binnen; huidige stand aanhouden

  // Koelen keert alles om: dan wil je de aanvoer juist OMLAAG naar het
  // setpoint, en is de retour warmer dan de aanvoer. Door beide grootheden
  // mode-afhankelijk te definiëren blijft de regel-tabel hieronder identiek:
  // err > 0 = meer vermogen nodig, dt = hoeveel de WP daadwerkelijk verzet.
  const float dt = cooling_ ? (t_return_ - t_supply_) : (t_supply_ - t_return_);
  const float err = cooling_ ? (t_supply_ - setpoint_) : (setpoint_ - t_supply_);
  const float band = 0.3f;                       // dode zone rond setpoint

  // Absolute vorstbeveiliging bij koelen (default 5°C, onder de fabrieksgrens
  // van 6.5°C - zit legitiem bedrijf dus nooit in de weg).
  if (cooling_ && t_supply_ < cooling_min_supply_) {
    if (stand_ != 0)
      ESP_LOGW(TAG, "Koelen: aanvoer %.1f°C te laag - stop", t_supply_);
    stand_ = 0;
    return;
  }

  // ── Richting kiezen: +1 meer / -1 minder / 0 houden ──
  // Delta-T tempert de keuze: aanvoer te koud MAAR delta-T al hoog -> HOUDEN.
  // (Retour warmt vanzelf op; extra gas zou overschieten.) Bewust omgekeerd
  // t.o.v. de oude .ino-formule die juist bijstookte bij hoge delta-T.
  int8_t dir = 0;
  if (err > band) {                       // aanvoer te laag -> meer willen...
    dir = (dt >= dt_high_) ? 0 : +1;      // ...tenzij delta-T al te hoog is
  } else if (err < -band) {               // aanvoer te hoog -> minder
    dir = -1;
  } else {                                // rond setpoint
    if (dt > dt_high_)                     // levert te agressief -> gas terug
      dir = -1;
  }

  // ── Stap-modulatie: max ±1 stand per step_interval (niet volgas) ──
  if (dir != 0 && (now - last_step_ms_) >= step_interval_ms_) {
    int ns = clamp((int) stand_ + dir, 0, (int) max_stand_);
    if ((uint8_t) ns != stand_) {
      stand_ = (uint8_t) ns;
      last_step_ms_ = now;
      ESP_LOGI(TAG, "AUTO: aanvoer %.1f/%.1f (fout %.1f) dT %.1f -> stand %u", t_supply_,
               setpoint_, err, dt, stand_);
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  Setters
// ═══════════════════════════════════════════════════════════════
void ChofuWP::set_setpoint(float v) {
  setpoint_ = clamp(v, setpoint_min_, setpoint_max_);
  ESP_LOGI(TAG, "Setpoint aanvoer: %.1f°C", setpoint_);
}

void ChofuWP::set_cooling(bool c) {
  if (c == cooling_)
    return;
  cooling_ = c;
  stand_ = 0;          // Altijd via stand 0 wisselen, nooit vliegend omkeren
  last_step_ms_ = 0;   // Mag daarna direct weer opbouwen
  ESP_LOGI(TAG, "Bedrijfsmodus: %s", c ? "KOELEN" : "verwarmen");
}

void ChofuWP::set_system_on(bool on) {
  system_on_ = on;
  ESP_LOGI(TAG, "Systeem: %s", on ? "AAN" : "UIT");
}

void ChofuWP::set_auto_mode(bool a) {
  auto_mode_ = a;
  ESP_LOGI(TAG, "Modus: %s", a ? "auto" : "handmatig");
}

void ChofuWP::set_manual_stand(int s) {
  manual_stand_ = clamp(s, 0, (int) max_stand_);
  // Zonder deze melding is een te hoog gevraagde stand onzichtbaar: de slider
  // laat 'm toe, de code knipt 'm af en de WP rapporteert netjes de lagere
  // stand terug - wat makkelijk te lezen is als "de unit kan niet hoger".
  if (s != (int) manual_stand_)
    ESP_LOGW(TAG, "Stand %d gevraagd maar begrensd op %u (max_stand). Verhoog max_stand in de YAML.",
             s, manual_stand_);
}

}  // namespace chofu_wp
}  // namespace esphome
