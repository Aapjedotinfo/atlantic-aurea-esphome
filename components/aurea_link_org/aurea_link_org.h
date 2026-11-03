#pragma once

// ╔═══════════════════════════════════════════════════════════════════╗
// ║  aurea_link_org - de ORIGINELE IC2 (Haddon T1.1) in de IC2-voet     ║
// ╚═══════════════════════════════════════════════════════════════════╝
//
// Voor wie geen AVR kan programmeren: laat de originele ATmega8 in de IC2-voet
// zitten. Hij doet zelf het OpenTherm-gesprek met thermostaat en ketel. Deze
// component speelt de rol van de originele IC1 (M1.1) en praat met hem over de
// bestaande 9615-baud-link (8N2, 12 bytes + som, zie
// reverse-engineering/analyse-volledig/RAPPORT.md 3.3).
//
// Dezelfde publieke methodes als aurea_link en aurea_link2, zodat aurea-wp.yaml
// ongewijzigd blijft. Wat de originele IC2 niet kan, geeft NaN ("onbekend") of
// is een no-op. Wat WEL kan:
//
//   van de IC2 (T1.1 -> M1.1):   ketelsetpoint (ID 1), kamersetpoint (ID 16)
//   naar de IC2 (M1.1 -> T1.1):  stand (0 = ketel vrij), aanvoer (-> ID 25),
//                                retour (-> ID 28), buitentemp (weggegooid),
//                                foutcode
//
// De "rem" van aurea-wp.yaml wordt hier: rem aan = de warmtepomp is beschikbaar
// (de IC2 houdt de ketel tegen en toont aanvoer/retour van de warmtepomp), rem
// uit = byte 0 is 0xF0 en de IC2 schakelt naar boiler-modus (alles ongewijzigd
// door naar de ketel). Een ketelsetpoint kiezen voor de bijstook kan niet.
//
// STATUS: afgeleid uit de firmware en een emulator, nog nooit op een echte IC2
// gedraaid. Begin met `alleen_luisteren: true`.

#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/components/uart/uart.h"

#include <cmath>

namespace esphome {
namespace aurea_link_org {

static const uint8_t FRAME_LEN = 12;
static const uint8_t NOT_AVAILABLE = 0xF0;  // byte 0 naar de IC2: warmtepomp niet beschikbaar

// Zelfde richtingen als aurea_link, voor get_ot().
enum OtDirection : uint8_t {
  OT_FROM_STAT = 0,    // wat de thermostaat vroeg
  OT_FROM_BOILER = 1,  // wat de ketel antwoordde (de originele IC2 geeft dit niet door)
  OT_TO_BOILER = 2,
  OT_TO_STAT = 3,      // wat wij de thermostaat laten zien (via de IC2)
};

class AureaLinkOrg : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;  // bewaking en "schop" als de IC2 stil is
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Configuratie ──
  void set_listen_only(bool v) { listen_only_ = v; }
  void set_reply_delay(uint16_t ms) { reply_delay_ms_ = ms; }
  void set_kick_after(uint16_t s) { kick_after_s_ = s; }
  void set_link_timeout(uint16_t s) { link_timeout_s_ = s; }

  // ── Wat de originele IC2 doorgeeft ──
  float get_room_temp() const { return NAN; }  // de IC2 geeft de kamertemperatuur niet door
  float get_room_setpoint() const;
  float get_requested_setpoint() const;
  float get_boiler_temp() const { return NAN; }
  float get_modulation() const { return NAN; }
  int get_fault_code() const { return fault_; }

  // Vlaggen die de IC2 niet doorgeeft: "onbekend" (lege optional) in plaats van een leugen.
  optional<bool> get_ch_demand() const { return {}; }
  optional<bool> get_dhw_demand() const { return {}; }
  optional<bool> get_ch_active() const { return {}; }
  optional<bool> get_dhw_active() const { return {}; }
  optional<bool> get_flame() const { return {}; }
  optional<bool> get_boiler_fault() const { return {}; }
  optional<bool> get_relay_active() const { return {}; }
  optional<bool> get_boiler_ok() const { return {}; }
  optional<bool> get_demand_out() const { return {}; }

  // ── Diagnose ──
  bool get_link_ok() const;
  // De IC2 meldt niet of er een thermostaat meepraat; zolang hij frames stuurt nemen we aan van wel.
  bool get_thermostat_ok() const { return get_link_ok(); }
  bool get_brake_active() const { return get_link_ok() && brake_on_; }
  // true = de IC2 gaf onze laatste bytes terug (idx 5..10 van zijn frame): het bewijs dat ESP32 -> IC2 werkt.
  optional<bool> get_echo_ok() const { return echo_known_ ? optional<bool>(echo_ok_) : optional<bool>(); }
  int get_missed() const { return bad_; }
  int get_frames() const { return frames_; }
  float get_age() const { return last_rx_ms_ == 0 ? NAN : (millis() - last_rx_ms_) / 1000.0f; }
  // De dipswitch (PC2) leest deze variant niet uit; zie "Keteltype (dipswitch)" in aurea-wp.yaml.
  bool get_front_switch() const { return true; }
  float get_boot_count() const { return NAN; }
  int get_ot_ids_seen() const { return link_seen_ ? 2 : 0; }

  // ── Beleid, gezet vanuit aurea-wp.yaml ──
  void set_brake(bool on) { brake_on_ = on; }
  bool get_brake() const { return brake_on_; }
  void set_brake_setpoint(float) {}  // de IC2 schrijft zelf 10 graden; niet instelbaar
  // Aanvoer en retour van de warmtepomp: de IC2 toont ze aan de thermostaat (ID 25 en 28).
  void set_wp_temps(float supply, float ret);
  // aurea-wp.yaml geeft hier stand * 10; in de IC2 telt alleen "stand > 0" mee.
  void set_standalone_modulation(float pct);
  void set_fault_code(uint8_t code) { fault_ = code; }
  void set_raw_debug(bool on) { raw_debug_ = on; }

  // Alles hieronder kan met de originele IC2 niet. Aanwezig zodat aurea-wp.yaml compileert.
  void set_ch_off(bool) {}
  void set_max_modulation(float) {}
  void set_subst_temp(bool) {}  // met de rem aan toont de IC2 altijd de warmtepomptemperaturen
  void set_override_setpoint(float) {}
  void set_boiler_setpoint(float) {}
  void set_lamp_mode(int) {}
  void set_standalone_flame(bool) {}
  void set_force_standalone(bool) {}
  void set_relay(bool) {}
  void set_relay_follows_switch(bool) {}
  void set_demand_enabled(bool) {}
  void read_boiler(uint8_t) {}
  void send_to_boiler(uint32_t) {}
  void override_to_thermostat(uint8_t, float) {}
  void clear_override(uint8_t) {}
  // write_boiler(27, x) is de buitentemperatuur (elke minuut door aurea-wp.yaml); de IC2 gooit hem weg
  // maar het frame heeft er een veld voor, dus we vullen het.
  void write_boiler(uint8_t data_id, float value);

  // Enkel ID 1 en 16 (van de thermostaat) en 25, 28, 27 (naar de thermostaat) zijn bekend.
  float get_ot(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  int get_ot_raw(int, OtDirection = OT_FROM_BOILER) const { return -1; }
  int get_ot_age(int data_id, OtDirection dir = OT_FROM_BOILER) const;

 protected:
  void handle_frame_(const uint8_t *b);
  void send_frame_();
  uint8_t build_stand_() const;
  static uint8_t sum8_(const uint8_t *p, uint8_t n);

  // Instellingen
  bool listen_only_{false};
  uint16_t reply_delay_ms_{150};
  uint16_t kick_after_s_{30};
  uint16_t link_timeout_s_{30};
  bool raw_debug_{false};

  // Ontvangst
  uint8_t rx_[FRAME_LEN]{};
  uint8_t rx_idx_{0};
  uint32_t last_byte_ms_{0};
  uint32_t last_rx_ms_{0};
  uint32_t last_tx_ms_{0};
  bool link_seen_{false};
  uint16_t frames_{0}, bad_{0};

  // Wat de IC2 meldde (T1.1 -> M1.1)
  uint8_t sp_whole_{0}, room_whole_{0}, room_frac_{0};
  uint8_t last_idx0_{0}, last_idx1_{0}, last_idx2_{0};

  // Wat wij sturen (M1.1 -> T1.1)
  uint8_t tx_[FRAME_LEN]{};
  bool tx_valid_{false};
  bool brake_on_{false};
  uint8_t stand_{0};
  float supply_{NAN}, return_{NAN}, outside_{NAN};
  uint8_t fault_{0};

  // Antwoord inplannen
  bool reply_pending_{false};
  uint32_t reply_at_ms_{0};

  bool echo_known_{false}, echo_ok_{false};
};

}  // namespace aurea_link_org
}  // namespace esphome
