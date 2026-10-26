#pragma once

// ╔═══════════════════════════════════════════════════════════════╗
// ║  aurea_link2 - link met de AVR-brug v2 (protocolversie 2)      ║
// ╚═══════════════════════════════════════════════════════════════╝
//
// Opvolger van aurea_link. De AVR zit tussen thermostaat en CV-ketel; deze
// component praat met hem over 38400 baud 8N1 met het gekaderde protocol uit
// firmware/avr-bridge-v2/PROTOCOL.md (SYNC, type, lengte, volgnr, payload,
// CRC-8). Wat er verandert ten opzichte van aurea_link:
//
//  - De ESP stuurt een TABEL: per (Data-ID, richting) wat de AVR met een
//    OpenTherm-frame doet (doorgeven, vast invullen, de ESP vragen, blokkeren).
//    De tabel staat in de yaml (`onderscheppen:`), niet meer in de AVR.
//  - Bij een regel "vraag" houdt de AVR het frame vast en vraagt het de ESP
//    (VRAAG). Een `on_vraag:` in de yaml antwoordt met `doorgeven`, `vervangen`
//    of `blokkeren` (ANTWOORD) binnen de deadline; anders doet de AVR de
//    standaardactie van de regel.
//  - Waarnemingen (STATUS) en sturing (COMMANDO) werken zoals de vorige
//    component, met dezelfde get_/set_-functies.
//
// De coder en decoder staan in aurea_link2_proto.h.

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <cmath>
#include <string>
#include <vector>

#include "aurea_link2_proto.h"

namespace esphome {
namespace aurea_link2 {

// Richting van een VRAAG en van een tabelregel.
static const uint8_t T_NAAR_K = 0;  // frame van de thermostaat, nog niet naar de ketel
static const uint8_t K_NAAR_T = 1;  // antwoord van de ketel, nog niet naar de thermostaat

// Richtingen van EVENT's (zoals in aurea_link).
enum OtDirection : uint8_t {
  OT_FROM_STAT = 0,    // de thermostaat vroeg dit
  OT_FROM_BOILER = 1,  // de ketel antwoordde dit
  OT_TO_BOILER = 2,    // dit stuurde de brug door naar de ketel
  OT_TO_STAT = 3,      // dit antwoordde de brug de thermostaat
};

// Data-ID 0, hoge byte: wat de thermostaat wil.
static const uint8_t MASTER_CH_ENABLE = 0x01;
static const uint8_t MASTER_DHW_ENABLE = 0x02;
static const uint8_t MASTER_COOLING = 0x04;

// Data-ID 0, lage byte: wat de ketel doet.
static const uint8_t SLAVE_FAULT = 0x01;
static const uint8_t SLAVE_CH_ACTIVE = 0x02;
static const uint8_t SLAVE_DHW_ACTIVE = 0x04;
static const uint8_t SLAVE_FLAME = 0x08;

// STATUS.link
static const uint8_t LINK_STAT_ALIVE = 0x01;
static const uint8_t LINK_BOILER_ALIVE = 0x02;
static const uint8_t LINK_BRAKE_ACTIVE = 0x04;
static const uint8_t LINK_POLICY_VALID = 0x08;
static const uint8_t LINK_ESP_PRESENT = 0x10;
static const uint8_t LINK_DEFAULT_TABLE = 0x20;
static const uint8_t LINK_QUESTION_OPEN = 0x40;
static const uint8_t LINK_HANDSHAKE = 0x80;

// STATUS.io
static const uint8_t IO_DIP2_HIGH = 0x01;
static const uint8_t IO_BRIDGE_ON = 0x02;
static const uint8_t IO_K3_ON = 0x04;
static const uint8_t IO_ONOFF_BOILER = 0x08;
static const uint8_t IO_BRIDGE_AUTO = 0x10;
static const uint8_t IO_AVR_ANSWERS = 0x20;

// Keteltype in COMMANDO.flags2 (bits 7..6).
static const uint8_t KETEL_DIP = 0;
static const uint8_t KETEL_OPENTHERM = 1;
static const uint8_t KETEL_AANUIT = 2;

class AureaLink2;

// Een `on_vraag:` uit de yaml, eventueel beperkt tot een Data-ID en richting.
// Argumenten: data_id, richting (0 T>K, 1 K>T), waarde (f8.8 als float), frame (ruw).
class VraagTrigger : public Trigger<uint8_t, uint8_t, float, uint32_t> {
 public:
  void set_data_id(uint8_t id) { this->data_id_ = id; }
  void set_richting(uint8_t r) { this->richting_ = r; }
  bool past(uint8_t id, uint8_t r) const {
    return (this->data_id_ < 0 || this->data_id_ == id) && (this->richting_ < 0 || this->richting_ == r);
  }

 protected:
  int16_t data_id_{-1};
  int8_t richting_{-1};
};

class AureaLink2 : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;    // ontvangen, antwoorden en zenden
  void update() override;  // COMMANDO sturen (elke 2 s)
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── De tabel (uit de yaml) ──
  void add_regel(uint8_t id, uint8_t richting, uint8_t modus, uint8_t p0, uint8_t p1, const char *naam,
                 bool actief);
  // Een regel (op naam) tijdens de run aan- of uitzetten; de tabel gaat dan opnieuw naar de AVR.
  bool set_regel_actief(const char *naam, bool actief);
  void add_vraag_trigger(VraagTrigger *t) { this->vraag_triggers_.push_back(t); }

  // Tijden uit de TABEL, met de grenzen uit PROTOCOL.md hoofdstuk 8. Buiten de grenzen wordt vastgeklemd.
  void set_deadline_ms(uint32_t ms);   // standaard-deadline ESP-antwoord, 30..600
  void set_ketel_ms(uint32_t ms);      // wachttijd op het ketelantwoord, 100..600
  void set_esp_weg_s(uint32_t s);      // ESP-weg-tijd, 10..600
  void set_nood_s(uint32_t s);         // tijd tot de noodbrug, 60..1800 en >= esp_weg_s
  void set_rem_hold_s(uint32_t s);     // houdtijd van de rem, 60..900
  // Alle vijf tegelijk, voor de yaml (bij de start; telt niet als wijziging).
  void set_tijden(uint32_t deadline_ms, uint32_t ketel_ms, uint32_t esp_weg_s, uint32_t nood_s, uint32_t rem_hold_s) {
    this->deadline_ms_ = (uint16_t) deadline_ms; this->ketel_ms_ = (uint16_t) ketel_ms;
    this->esp_weg_s_ = (uint16_t) esp_weg_s; this->nood_s_ = (uint16_t) nood_s;
    this->rem_hold_s_ = (uint16_t) rem_hold_s;
  }
  void set_tabel_versie(uint8_t v) { this->tabel_versie_ = v; }
  void set_minimale_versie(uint8_t hoofd, uint8_t neven, uint8_t patch) {
    this->min_sw_[0] = hoofd; this->min_sw_[1] = neven; this->min_sw_[2] = patch;
  }
  void set_avr_versie_sensor(text_sensor::TextSensor *s) { this->versie_sensor_ = s; }
  void set_avr_melding_sensor(text_sensor::TextSensor *s) { this->melding_sensor_ = s; }

  // ── VRAAG beantwoorden (te gebruiken uit `on_vraag:`) ──
  // Elk van deze werkt op de VRAAG die nu openstaat; false als er geen is.
  bool antwoord_doorgeven();
  bool antwoord_blokkeren();
  bool antwoord_vervangen_frame(uint32_t frame);
  // Zelfde frame (type en Data-ID), andere waarde (f8.8). De pariteit rekenen wij uit.
  bool antwoord_vervangen_waarde(float waarde);
  bool get_vraag_open() const { return this->vraag_.open; }

  // ── Waarnemingen (STATUS) ──
  float get_room_temp() const { return tenths_(status_.room_temp); }
  float get_room_setpoint() const { return tenths_(status_.room_setpoint); }
  float get_boiler_temp() const { return tenths_(status_.boiler_temp); }
  // Wat de thermostaat vraagt VOOR de rem.
  float get_requested_setpoint() const { return have_data_ ? status_.requested_setpoint : NAN; }
  float get_modulation() const { return have_data_ ? status_.modulation : NAN; }
  int get_fault_code() const { return have_data_ ? status_.fault_code : -1; }

  bool get_ch_demand() const { return status_.master_status & MASTER_CH_ENABLE; }
  bool get_dhw_demand() const { return status_.master_status & MASTER_DHW_ENABLE; }
  bool get_ch_active() const { return status_.slave_status & SLAVE_CH_ACTIVE; }
  bool get_dhw_active() const { return status_.slave_status & SLAVE_DHW_ACTIVE; }
  bool get_flame() const { return status_.slave_status & SLAVE_FLAME; }
  bool get_boiler_fault() const { return status_.slave_status & SLAVE_FAULT; }

  // ── Diagnose ──
  bool get_link_ok() const { return last_rx_ms_ != 0 && (millis() - last_rx_ms_) < LINK_TIMEOUT_MS; }
  bool get_thermostat_ok() const { return get_link_ok() && (status_.link & LINK_STAT_ALIVE); }
  bool get_boiler_ok() const { return get_link_ok() && (status_.link & LINK_BOILER_ALIVE); }
  // Rem TOEGEPAST (na de houdtijd), niet alleen gevraagd.
  bool get_brake_active() const { return get_link_ok() && (status_.link & LINK_BRAKE_ACTIVE); }
  bool get_avr_has_policy() const { return get_link_ok() && (status_.link & LINK_POLICY_VALID); }
  bool get_esp_present_by_avr() const { return get_link_ok() && (status_.link & LINK_ESP_PRESENT); }
  bool get_default_table() const { return get_link_ok() && (status_.link & LINK_DEFAULT_TABLE); }
  bool get_handshake_done() const { return get_link_ok() && (status_.link & LINK_HANDSHAKE); }
  int get_missed() const { return missed_; }
  int get_stat_edges() const { return have_data_ ? status_.flanken_stat : -1; }
  int get_boiler_edges() const { return have_data_ ? status_.flanken_ketel : -1; }
  int get_stat_frames() const { return have_data_ ? (status_.frames & 0x0F) : -1; }
  int get_parity_fails() const { return have_data_ ? (status_.frames >> 4) : -1; }
  // Berichten van de AVR met slechte CRC of lengte, door ons weggegooid.
  int get_bad_checksums() const { return bad_crc_; }
  float get_age() const { return last_rx_ms_ == 0 ? NAN : (millis() - last_rx_ms_) / 1000.0f; }

  bool get_line_stat() const { return status_.lijnen & 0x01; }
  bool get_line_boiler() const { return status_.lijnen & 0x02; }
  int get_lines_raw() const { return have_data_ ? status_.lijnen : -1; }

  // Dipschakelaar 2: hoog = aan/uit-ketel. In aurea_link was dit "frontschakelaar
  // ingedrukt" (= laag = OpenTherm-ketel); get_front_switch() houdt die betekenis.
  bool get_dip2_high() const { return status_.io & IO_DIP2_HIGH; }
  bool get_front_switch() const { return !(status_.io & IO_DIP2_HIGH); }
  // Noodbrugrelais K1+K2 (PB2) aan, K3 (PC5) aan.
  bool get_relay_active() const { return status_.io & IO_BRIDGE_ON; }
  bool get_demand_out() const { return status_.io & IO_K3_ON; }
  bool get_onoff_boiler() const { return status_.io & IO_ONOFF_BOILER; }
  bool get_bridge_auto() const { return status_.io & IO_BRIDGE_AUTO; }
  bool get_avr_answers() const { return status_.io & IO_AVR_ANSWERS; }

  int get_spare_inputs() const { return have_data_ ? status_.spare : -1; }
  int get_reset_cause() const { return have_data_ ? (status_.reset & 0x0F) : -1; }
  int get_boot_count() const { return have_data_ ? (status_.reset >> 4) : -1; }
  int get_isr_overruns() const { return have_data_ ? status_.isr_overruns : -1; }
  int get_rx_errors_stat() const { return have_data_ ? status_.rx_fout_stat : -1; }
  int get_rx_errors_boiler() const { return have_data_ ? status_.rx_fout_ketel : -1; }

  // Nieuw in v2 (hoofdstuk 10): wat de AVR over het linkverkeer meldt.
  int get_avr_crc_errors() const { return have_data_ ? status_.crc_fouten : -1; }
  int get_avr_uart_overruns() const { return have_data_ ? status_.uart_overruns : -1; }
  int get_questions_expired() const { return have_data_ ? status_.vraag_verlopen : -1; }
  int get_boiler_stale() const { return have_data_ ? status_.ketel_oud : -1; }
  int get_events_missed() const { return have_data_ ? status_.ev_gemist : -1; }
  int get_avr_table_crc() const { return have_data_ ? status_.tabel_crc : -1; }
  int get_avr_table_n() const { return have_data_ ? status_.tabel_n : -1; }
  int get_avr_silence_s() const { return have_data_ ? status_.sinds_esp : -1; }
  // CRC van onze eigen tabel, en of de AVR hem heeft aangenomen.
  int get_table_crc() const { return this->tabel_crc_; }
  bool get_table_accepted() const { return this->tabel_ok_; }
  // Ontvangen en weggegooide VRAAG's zonder antwoord van ons binnen de deadline.
  int get_questions_unanswered() const { return this->vraag_onbeantw_; }
  std::string get_avr_version() const { return this->versie_tekst_; }

  // ── Sturing (COMMANDO), zoals in aurea_link ──
  void set_brake(bool on);
  bool get_brake() const { return brake_on_; }
  void set_brake_setpoint(float c);
  float get_brake_setpoint() const { return brake_setpoint_; }
  // Rem zonder houdtijd toepassen (voor storingen).
  void set_brake_direct(bool on);
  void set_ch_off(bool on);
  bool get_ch_off() const { return ch_off_; }
  void set_max_modulation(float pct);
  float get_max_modulation() const { return max_mod_ ? max_mod_ : 100.0f; }
  void set_subst_temp(bool on);
  bool get_subst_temp() const { return subst_temp_; }
  void set_invert_stat(bool on);
  void set_invert_boiler(bool on);
  // Noodbrug (K1+K2) aan op verzoek; werkt alleen bij een OpenTherm-ketel.
  void set_relay(bool on);
  bool get_relay() const { return relay_on_; }
  void set_lamp_invert(bool on);
  void set_override_setpoint(float c);
  float get_override_setpoint() const { return override_setpoint_ / 10.0f; }
  void set_standalone_modulation(float pct);
  void set_standalone_flame(bool on);
  void set_force_standalone(bool on);
  // 0 = automatisch, 1 = uit, 2 = aan, 3 = traag knipperen, 4 = snel
  void set_lamp_mode(int mode);
  void set_invert_tx_stat(bool on);
  void set_invert_tx_boiler(bool on);
  void set_swap_channels(bool on);
  void set_test_out(uint8_t mask, bool on);
  uint8_t get_test_out() const { return test_out_; }
  // K3, het droge contact voor een aan/uit-ketel, vrijgeven.
  void set_demand_enabled(bool on);
  void set_boiler_setpoint(float c);
  void set_wp_temps(float supply, float ret);
  // Keteltype: 0 = dipschakelaar, 1 = OpenTherm-ketel, 2 = aan/uit-ketel.
  void set_boiler_type(int type);
  // Geldigheid van ons COMMANDO in seconden: 0 = de AVR gebruikt zijn esp_weg_s, anders 10..255.
  void set_valid_time(int s);

  // ── Elk Data-ID dat over de bus is gekomen (uit EVENT's) ──
  float get_ot(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  int get_ot_raw(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  int get_ot_age(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  int get_ot_ids_seen() const { return ids_seen_; }

  // ── Zelf iets laten versturen (INJECT) ──
  void send_to_boiler(uint32_t frame);
  void write_boiler(uint8_t data_id, float value);
  void read_boiler(uint8_t data_id);
  void override_to_thermostat(uint8_t data_id, float value);
  void clear_override(uint8_t data_id);

  void set_raw_debug(bool on) { raw_debug_ = on; }

 protected:
  struct RegelCfg {
    Regel r;
    std::string naam;
    bool actief;
  };
  struct Uit {
    uint8_t type;
    uint8_t len;
    uint8_t d[MAX_PAYLOAD];
  };
  struct Vraag {
    bool open{false};
    uint8_t seq{0}, richting{0}, id{0}, standaard{0};
    uint32_t frame{0};
    uint32_t einde_ms{0};
  };

  void verwerk_();
  void handle_hello_();
  void handle_ack_();
  void handle_vraag_();
  void handle_status_();
  void handle_event_();
  bool antwoord_(uint8_t actie, uint32_t frame);
  void zet_(uint8_t type, const uint8_t *pl, uint8_t len, bool voorrang = false);
  void flush_tx_();
  void stuur_tabel_();
  void stuur_commando_();
  void herbereken_tabel_(bool gewijzigd);
  Tabel bouw_tabel_() const;
  void send_inject_(uint8_t mode, uint32_t frame);
  void melding_(const std::string &tekst, bool waarschuwing);
  static uint16_t f88_from_(float v);
  float tenths_(int16_t v) const { return have_data_ ? v / 10.0f : NAN; }

  static const uint32_t LINK_TIMEOUT_MS = 15000;
  static const uint32_t TABEL_HERHAAL_MS = 3000;  // niet vaker dan dit een TABEL sturen

  // Ontvangst en zending
  Ontvanger rx_;
  uint32_t laatste_byte_us_{0};
  uint32_t tx_vrij_us_{0};
  uint8_t tx_seq_{0};
  std::vector<Uit> uit_;
  HighFrequencyLoopRequester hf_;
  bool hf_aan_{false};
  bool rx_seq_bekend_{false};
  uint8_t rx_seq_{0};
  uint16_t missed_{0}, bad_crc_{0}, bad_len_{0};
  uint8_t tabel_seq_{0}, cmd_seq_{0}, inject_seq_{0};

  // Tabel en tijden
  std::vector<RegelCfg> regels_;
  uint8_t tabel_versie_{1};
  uint16_t deadline_ms_{250}, ketel_ms_{400}, esp_weg_s_{60}, nood_s_{240}, rem_hold_s_{300};
  uint16_t tabel_crc_{0};
  bool tabel_wens_{false}, tabel_ok_{false}, tabel_afgewezen_{false};
  uint32_t tabel_verstuurd_ms_{0};
  bool protocol_ok_{true};

  // VRAAG
  std::vector<VraagTrigger *> vraag_triggers_;
  Vraag vraag_;
  uint16_t vraag_onbeantw_{0};

  // HELLO
  uint8_t min_sw_[3]{2, 0, 0};
  std::string versie_tekst_;
  text_sensor::TextSensor *versie_sensor_{nullptr};
  text_sensor::TextSensor *melding_sensor_{nullptr};
  std::string laatste_melding_;

  // Waarnemingen
  Status status_{};
  bool have_data_{false};
  uint32_t last_rx_ms_{0};
  bool raw_debug_{false};
  uint16_t ot_val_[4][256]{};
  uint32_t ot_ms_[4][256]{};
  uint16_t ids_seen_{0};

  // Sturing
  bool brake_on_{false}, brake_direct_{false};
  bool subst_temp_{false};
  bool invert_stat_{false}, invert_boiler_{false};
  bool relay_on_{false};
  bool lamp_invert_{false};
  bool sa_flame_{false}, force_sa_{false};
  bool inv_tx_stat_{false}, inv_tx_boiler_{false}, swap_ch_{false};
  bool demand_enabled_{false};
  bool ch_off_{false};
  int16_t override_setpoint_{0};
  uint8_t lamp_mode_{0}, sa_modulation_{0}, test_out_{0}, max_mod_{0}, boiler_setpoint_{0};
  uint8_t boiler_type_{KETEL_DIP};
  uint8_t geldig_s_{0};
  float brake_setpoint_{10.0f};
  int16_t wp_supply_{0}, wp_return_{0};
};

// ── Acties voor `on_vraag:` ──
template<typename... Ts> class DoorgevenAction : public Action<Ts...>, public Parented<AureaLink2> {
 public:
  void play(const Ts &...x) override { this->parent_->antwoord_doorgeven(); }
};

template<typename... Ts> class BlokkerenAction : public Action<Ts...>, public Parented<AureaLink2> {
 public:
  void play(const Ts &...x) override { this->parent_->antwoord_blokkeren(); }
};

template<typename... Ts> class VervangenAction : public Action<Ts...>, public Parented<AureaLink2> {
 public:
  TEMPLATABLE_VALUE(uint32_t, frame)
  TEMPLATABLE_VALUE(float, waarde)
  void play(const Ts &...x) override {
    if (this->frame_.has_value())
      this->parent_->antwoord_vervangen_frame(this->frame_.value(x...));
    else
      this->parent_->antwoord_vervangen_waarde(this->waarde_.value(x...));
  }
};

}  // namespace aurea_link2
}  // namespace esphome
