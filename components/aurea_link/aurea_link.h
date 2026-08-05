#pragma once

// ╔═══════════════════════════════════════════════════════════════╗
// ║  aurea_link - parameterlink met de ATmega328P in de IC2-socket ║
// ╚═══════════════════════════════════════════════════════════════╝
//
// De AVR voert het OpenTherm-gesprek tussen thermostaat en CV-ketel en zit
// daar als doorgeefluik tussen. Deze component draagt geen OpenTherm-frames
// maar alleen parameters: "rem de ketel af op X graden", "vertel de
// thermostaat dat de aanvoer Y is", en leest terug wat er waargenomen wordt.
//
// Alle regellogica hoort hier, in ESPHome, waar je 'm zonder programmer kunt
// bijstellen. De AVR kent alleen de twee substituties en zijn terugvalgedrag.
//
// Zie ../../firmware/avr-bridge/avr-bridge.ino en
// ../../docs/volledige-controlbox.md.

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

#include <cmath>

namespace esphome {
namespace aurea_link {

// ── Berichtindeling, moet exact overeenkomen met avr-bridge.ino ──
//
//  AVR -> ESP32 (17 bytes)          ESP32 -> AVR (12 bytes)
//   0  0xF0                          0  0xF1
//   1  masterstatus (thermostaat)    1  vlaggen: rem, temp-substitutie
//   2  slavestatus (ketel)           2  setpoint dat de rem oplegt (hele °C)
//   3  gevraagd setpoint (hele °C)   3-4 aanvoer die de thermostaat ziet
//   4-5  kamertemperatuur            5-6 retour die de thermostaat ziet
//   6-7  room setpoint               7-8 override kamersetpoint (0 = geen)
//   8-9  keteltemperatuur            9  lampstand (0 auto, 1 uit, 2 aan, 3/4 knipper)
//                                   10  modulatie in de zelfstandige stand
//                                   11  8-bits optelsom
//   10 modulatie (%)
//   11 storingscode
//   12 linkvlaggen + volgnummer
//   13 ruwe flanken thermostaatkant sinds vorig bericht
//   14 ruwe flanken ketelkant
//   15 lage nibble: complete frames; hoge nibble: afgekeurd op pariteit
//   16 8-bits optelsom
//
// Temperaturen gaan als getekende tienden graden, big-endian.

static const uint8_t STATUS_START = 0xF0;
static const uint8_t STATUS_LEN = 24;
static const uint8_t CMD_START = 0xF1;
static const uint8_t CMD_LEN = 16;

// ── Elk OpenTherm-frame, rauw doorgegeven ──
//
//   0  0xF2
//   1  richting (zie hieronder)
//   2-5 het frame, MSB eerst
//   6  8-bits optelsom
//
// De AVR stuurt hier alles doorheen wat er over de bus gaat, in beide
// richtingen, inclusief wat hij er zelf van maakt. Uitpakken doen wij, zodat
// een nieuw Data-ID een OTA is en geen nieuwe burn.
static const uint8_t EVENT_START = 0xF2;
static const uint8_t EVENT_LEN = 7;

// ── En terug: wij bepalen wat de brug verstuurt ──
//
//   0  0xF3
//   1  modus (0 naar de ketel, 1 antwoord vervangen, 2 dat weer opheffen)
//   2-5 het frame, MSB eerst
//   6  8-bits optelsom
static const uint8_t INJECT_START = 0xF3;
static const uint8_t INJECT_LEN = 7;

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

// Vlaggen in byte 12.
static const uint8_t LINK_STAT_ALIVE = 0x01;
static const uint8_t LINK_BOILER_ALIVE = 0x02;
static const uint8_t LINK_BRAKE_ACTIVE = 0x04;
static const uint8_t LINK_POLICY_VALID = 0x08;

class AureaLink : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;    // statusberichten binnenhalen
  void update() override;  // commandobericht sturen
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Waarnemingen uit het OpenTherm-gesprek ──
  float get_room_temp() const { return tenths_(room_temp_); }
  float get_room_setpoint() const { return tenths_(room_setpoint_); }
  float get_boiler_temp() const { return tenths_(boiler_temp_); }
  // Wat de thermostaat vraagt VOOR de rem. Hiermee zie je hoeveel warmte er
  // werkelijk gevraagd wordt, ook terwijl je de ketel tegenhoudt.
  float get_requested_setpoint() const { return have_data_ ? requested_setpoint_ : NAN; }
  float get_modulation() const { return have_data_ ? modulation_ : NAN; }
  int get_fault_code() const { return fault_code_; }

  bool get_ch_demand() const { return master_status_ & MASTER_CH_ENABLE; }
  bool get_dhw_demand() const { return master_status_ & MASTER_DHW_ENABLE; }
  bool get_ch_active() const { return slave_status_ & SLAVE_CH_ACTIVE; }
  bool get_dhw_active() const { return slave_status_ & SLAVE_DHW_ACTIVE; }
  bool get_flame() const { return slave_status_ & SLAVE_FLAME; }
  bool get_boiler_fault() const { return slave_status_ & SLAVE_FAULT; }

  // ── Diagnose ──
  // In de controlbox is dit je enige venster op de AVR: geen seriële monitor,
  // geen LED. Vandaar dat de link zichzelf bewaakt.
  bool get_link_ok() const {
    return last_rx_ms_ != 0 && (millis() - last_rx_ms_) < LINK_TIMEOUT_MS;
  }
  bool get_thermostat_ok() const { return get_link_ok() && (link_flags_ & LINK_STAT_ALIVE); }
  bool get_boiler_ok() const { return get_link_ok() && (link_flags_ & LINK_BOILER_ALIVE); }
  bool get_brake_active() const { return get_link_ok() && (link_flags_ & LINK_BRAKE_ACTIVE); }
  bool get_avr_has_policy() const { return get_link_ok() && (link_flags_ & LINK_POLICY_VALID); }
  int get_missed() const { return missed_; }
  // Ruwe flanken per statusbericht, los van alle decodering. Nul betekent een
  // dode bus; flanken zonder geldige frames wijst op de decodering.
  int get_stat_edges() const { return have_data_ ? stat_edges_ : -1; }
  int get_boiler_edges() const { return have_data_ ? boiler_edges_ : -1; }
  // Complete frames en pariteitsfouten sinds het vorige statusbericht. Samen
  // met de flanken heb je hiermee drie niveaus: staat er signaal, komt er een
  // frame uit, en klopt dat frame.
  int get_stat_frames() const { return have_data_ ? (frame_counts_ & 0x0F) : -1; }
  int get_parity_fails() const { return have_data_ ? (frame_counts_ >> 4) : -1; }
  int get_bad_checksums() const { return bad_sum_; }
  float get_age() const { return last_rx_ms_ == 0 ? NAN : (millis() - last_rx_ms_) / 1000.0f; }

  // ── Beleid, gezet vanuit de YAML ──
  void set_brake(bool on);
  bool get_brake() const { return brake_on_; }
  void set_brake_setpoint(float c);
  float get_brake_setpoint() const { return brake_setpoint_; }
  void set_subst_temp(bool on);
  bool get_subst_temp() const { return subst_temp_; }
  // Leesrichting van de OT-ontvangst omkeren. De comparator geeft met de
  // bandgap op de plus-ingang het omgekeerde van het pinniveau, maar of de
  // interface-elektronica dat weer terugdraait is niet uit de code te halen.
  // Vandaar hier omzetbaar in plaats van een aanname in de firmware.
  void set_invert_stat(bool on);
  void set_invert_boiler(bool on);
  // Relais op IC2-socket pin 16. Zie avr-bridge.ino: het origineel koppelde
  // dit aan de vraag of de controlbox in het circuit zit.
  void set_relay(bool on);
  bool get_relay() const { return relay_on_; }
  // Statuslampje op IC2-socket pin 27. Uit de firmware volgt actief-hoog;
  // deze vlag is er voor het geval er toch een inverterende driver tussen zit.
  void set_lamp_invert(bool on);

  // ── De thermostaat op afstand zetten ──
  // Data-ID 9 gaat van slave naar master, dus wij kunnen de thermostaat een
  // kamersetpoint opdringen. 0 = geen override, dan regelt hij weer zelf.
  // Dit is standaard-OpenTherm en werkt dus ongeacht welk merk er hangt.
  void set_override_setpoint(float c);
  float get_override_setpoint() const { return override_setpoint_ / 10.0f; }

  // Wat de brug de thermostaat vertelt zolang er geen ketel meepraat.
  void set_standalone_modulation(float pct);
  void set_standalone_flame(bool on);
  void set_force_standalone(bool on);

  // 0 = automatisch, 1 = uit, 2 = aan, 3 = traag knipperen, 4 = snel
  void set_lamp_mode(int mode);

  // ── De aannames die nog niet met signaal bewezen zijn ──
  // Deze staan hier als schakelaar en niet als constante in de AVR, zodat je
  // ze kunt uitproberen zonder de chip uit de socket te halen.
  void set_invert_tx_stat(bool on);
  void set_invert_tx_boiler(bool on);
  void set_swap_channels(bool on);

  // Ruwe lijnstanden, rechtstreeks uit de AVR. Zegt of een bus in rust hoog of
  // laag staat, en dus of er uberhaupt spanning op zit.
  bool get_line_stat() const { return lines_ & 0x01; }
  bool get_line_boiler() const { return lines_ & 0x02; }
  // De hele byte. Bit 4/5/6 zijn de vlaggen zoals de AVR ze heeft ONTVANGEN,
  // dus dit is het bewijs dat de richting ESP32 -> AVR werkt. Zonder dat is
  // elke schakelaar hier een knop die nergens op aangesloten zit.
  int get_lines_raw() const { return have_data_ ? lines_ : -1; }

  // Socket-pin 25 en 16: de schakelaar op de voorkant en het relais.
  bool get_front_switch() const { return io_ & 0x01; }
  bool get_relay_active() const { return io_ & 0x02; }
  // Socket-pin 28: aan/uit-warmtevraag, hoog vanaf 20 graden setpoint.
  bool get_demand_out() const { return io_ & 0x08; }

  // Ongebruikte ingangen met pull-up: pin 23, 24 en 26.
  int get_spare_inputs() const { return have_data_ ? spare_in_ : -1; }
  // Waarom de AVR startte en hoe vaak sinds de koude start.
  int get_reset_cause() const { return have_data_ ? (boot_ & 0x0F) : -1; }
  int get_boot_count() const { return have_data_ ? (boot_ >> 4) : -1; }
  // Tikken waarin de ISR niet op tijd klaar was. Hoort 0 te zijn.
  int get_isr_overruns() const { return have_data_ ? isr_ovr_ : -1; }
  // Losse uitgangen om mee te proberen; zie avr-bridge.ino.
  void set_test_out(uint8_t mask, bool on);
  uint8_t get_test_out() const { return test_out_; }
  void set_relay_follows_switch(bool on);
  // K3, het droge contact voor een aan/uit-ketel. Uit tenzij je er een hebt:
  // hij deelt zijn voorschakelweerstand met de noodbrug.
  void set_demand_enabled(bool on);
  // Rechtstreeks setpoint naar de ketel, zonder tussenkomst van een
  // thermostaat. 0 = uit. Werkt alleen als er geen thermostaat meepraat;
  // die heeft altijd voorrang.
  void set_boiler_setpoint(float c);
  int get_rx_errors_stat() const { return have_data_ ? rx_err_stat_ : -1; }
  int get_rx_errors_boiler() const { return have_data_ ? rx_err_boiler_ : -1; }
  // Aanvoer en retour van de warmtepomp, door te geven aan de thermostaat.
  void set_wp_temps(float supply, float ret);

  // ── Elk Data-ID dat over de bus is gekomen ──
  //
  // De brug stuurt elk frame rauw door, dus alles wat een thermostaat of ketel
  // ooit noemt staat hier. Een nieuwe sensor toevoegen is een regel yaml:
  //
  //   lambda: 'return id(otlink)->get_ot(28);'   // retourtemperatuur
  //
  // Geeft NaN als dit Data-ID nog nooit langsgekomen is of te lang stil is.
  // Standaard kijken we naar wat de KETEL antwoordde; dat is de bron voor
  // vrijwel alles wat je wilt weten. Wil je zien wat de thermostaat vroeg, geef
  // dan OT_FROM_STAT mee.
  float get_ot(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  // Zonder f8.8-omrekening, voor Data-ID's die vlaggen of tellers dragen.
  int get_ot_raw(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  // Hoe lang geleden dit Data-ID langskwam, in seconden. -1 = nooit gezien.
  int get_ot_age(int data_id, OtDirection dir = OT_FROM_BOILER) const;
  // Hoeveel verschillende Data-ID's we tot nu toe voorbij hebben zien komen.
  int get_ot_ids_seen() const { return ids_seen_; }

  // ── Zelf iets laten versturen ──
  //
  // Richting de KETEL zijn wij master, dus daar mogen we uit onszelf praten.
  // De brug schuift dit frame in een gaatje tussen twee thermostaattransacties
  // en houdt het antwoord voor zichzelf.
  void send_to_boiler(uint32_t frame);
  // Gemakshalve: een Write-Data met een temperatuur in f8.8.
  void write_boiler(uint8_t data_id, float value);
  // Read-Data, als je alleen wilt weten wat de ketel ergens van vindt.
  void read_boiler(uint8_t data_id);

  // Richting de THERMOSTAAT zijn wij slave en mag je niets uit jezelf sturen.
  // Wat je daar wel kunt: haar antwoord vervangen. Vraagt ze dit Data-ID, dan
  // krijgt ze deze waarde in plaats van die van de ketel. Acht tegelijk.
  void override_to_thermostat(uint8_t data_id, float value);
  void clear_override(uint8_t data_id);

  // Zet de ruwe OT-log aan: elk frame dat langskomt verschijnt in de log met
  // richting, Data-ID en waarde. Hangt aan dezelfde schakelaar als de
  // frame-log van de warmtepomp.
  void set_raw_debug(bool on) { raw_debug_ = on; }

 protected:
  void handle_status_(const uint8_t *b);
  void handle_event_(const uint8_t *b);
  void send_inject_(uint8_t mode, uint32_t frame);
  static uint16_t f88_from_(float v);
  static uint8_t sum8_(const uint8_t *p, uint8_t n);
  float tenths_(int16_t v) const { return have_data_ ? v / 10.0f : NAN; }

  static const uint32_t LINK_TIMEOUT_MS = 15000;

  // Ontvangst. De buffer is zo groot als het langste bericht; welk bericht het
  // is blijkt uit het startbyte, en dat bepaalt de verwachte lengte.
  uint8_t buf_[STATUS_LEN];
  uint8_t idx_{0};
  uint8_t want_{0};

  // Wat er over de bus kwam, per Data-ID en per richting. Vier richtingen x
  // 256 ID's is 4 kB aan tabellen - op een ESP32 met honderden kB vrij is dat
  // niets, en het bespaart je een burn per sensor die je later bedenkt.
  uint16_t ot_val_[4][256]{};
  uint32_t ot_ms_[4][256]{};
  uint16_t ids_seen_{0};
  bool raw_debug_{false};
  uint32_t last_rx_ms_{0};
  bool have_data_{false};

  uint8_t master_status_{0}, slave_status_{0}, link_flags_{0};
  uint8_t requested_setpoint_{0}, modulation_{0};
  int8_t fault_code_{-1};
  int16_t room_temp_{0}, room_setpoint_{0}, boiler_temp_{0};

  uint8_t last_seq_{0xFF};
  uint16_t missed_{0}, bad_sum_{0};
  uint8_t stat_edges_{0}, boiler_edges_{0}, frame_counts_{0};

  // Beleid
  bool brake_on_{false};
  bool subst_temp_{false};
  bool invert_stat_{false}, invert_boiler_{false};
  bool relay_on_{false};
  bool lamp_invert_{false};
  bool sa_flame_{false}, force_sa_{false};
  int16_t override_setpoint_{0};
  uint8_t lamp_mode_{0}, sa_modulation_{0};
  bool inv_tx_stat_{false}, inv_tx_boiler_{false}, swap_ch_{false};
  uint8_t lines_{0}, rx_err_stat_{0}, rx_err_boiler_{0}, io_{0}, boot_{0}, isr_ovr_{0};
  bool relay_follows_{false}, demand_enabled_{false};
  uint8_t boiler_setpoint_{0};
  uint8_t spare_in_{0}, test_out_{0};
  float brake_setpoint_{10.0f};
  int16_t wp_supply_{0}, wp_return_{0};
};

}  // namespace aurea_link
}  // namespace esphome
