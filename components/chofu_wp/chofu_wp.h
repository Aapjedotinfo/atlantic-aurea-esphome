#pragma once

// ╔═══════════════════════════════════════════════════════════════╗
// ║  chofu_wp - ESPHome external component                        ║
// ║  Atlantic Aurea 5 Hybrid / Chofu 0x19 protocol @ 666 baud     ║
// ╚═══════════════════════════════════════════════════════════════╝
//
// Protocol-laag 1:1 geport uit de WERKENDE ATmega2560-sketch van _JGC_
// (Tweakers, v0.1Beta 19-03-2025). Dat is gereverse-engineerde, op echte
// hardware bewezen code - vandaar dat de frame-opbouw, timing en CRC exact
// zijn overgenomen. Zie ../../README.md en de memory 'aurea-serieel-protocol'.
//
// De REGELING: "geef me X°C aanvoer" -> deze controller kiest zelf de stand
// (0 t/m max_stand; met stap-modulatie zodat de compressor niet volgas de
// buurman wakker maakt). Waar die X vandaan komt kies je met de modus:
//   auto        - vast setpoint ("Setpoint aanvoer")
//   stooklijn   - uit de buitentemperatuur van de warmtepomp
//   kamertemperatuur - stooklijn, gecorrigeerd met een kamertemperatuur (van
//                 een OpenTherm-thermostaat, Home Assistant of een script)
//   handmatig   - geen regeling, een vaste stand

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

#include <cmath>

namespace esphome {
namespace chofu_wp {

class ChofuWP : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;    // RX + TX-rotatie (continu, master/slave-timing)
  void update() override;  // Regeling, elke 5s
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Status (uitgelezen door template sensors in de YAML) ──
  float get_supply() const { return t_supply_; }
  float get_return() const { return t_return_; }
  float get_outside() const { return t_outside_; }
  float get_delta_t() const { return t_supply_ - t_return_; }
  int get_stand() const { return stand_; }             // Wat WIJ commanderen
  int get_max_stand() const { return max_stand_; }
  float get_setpoint() const { return setpoint_; }      // Het vaste setpoint (modus auto)
  int get_wp_speed() const { return wp_speed_; }        // Wat de WP rapporteert
  float get_power() const { return wp_power_w_; }       // Echt vermogen (W)
  bool get_defrost() const { return defrost_; }
  bool get_running() const { return stand_ > 0; }
  bool get_comm_ok() const {
    return last_valid_rx_ms_ != 0 && (millis() - last_valid_rx_ms_) < COMM_TIMEOUT_MS;
  }

  // ── Instellingen (gezet vanuit template numbers/switch/select) ──
  void set_setpoint(float v);
  void set_system_on(bool on);
  // Modus: zie bovenaan dit bestand.
  enum Mode : uint8_t { MODE_AUTO, MODE_CURVE, MODE_ROOM, MODE_MANUAL };
  void set_mode(Mode m);
  Mode get_mode() const { return mode_; }
  void set_auto_mode(bool a) { set_mode(a ? MODE_AUTO : MODE_MANUAL); }
  void set_manual_stand(int s);
  void set_cooling(bool c);                       // Koelbedrijf (modus 0x02)
  bool get_cooling() const { return cooling_; }
  void set_raw_debug(bool b) { raw_debug_ = b; }  // FRAME-hexlog aan/uit (web-toggle)
  void force_step() { last_step_ms_ = 0; }  // Volgende cyclus mag direct schakelen

  // ── Regel-finesse (alle in te stellen vanuit HA; 0 = uit) ──
  // Middeling: de regeling kijkt naar het gemiddelde van aanvoer en retour over
  // zoveel seconden. De aanvoer zaagt bij draaiende compressor ongeveer elke
  // minuut 3-4 °C; één meting op het moment van een stap is dan toeval. Max 200 s.
  void set_avg_window(float seconds) { avg_s_ = seconds; }
  // Vooruitkijken: beoordeel de aanvoer zoals hij over zoveel minuten verwacht
  // wordt (aanvoer + stijging per minuut x deze tijd). Komt hij er vanzelf, dan
  // niet nog een stand erbij; schiet hij ruim door, dan nu al een stand terug.
  // Alleen vanaf stand 2: stoppen (1 -> 0) blijft aan de echte aanvoer hangen.
  void set_lookahead(float minutes) { lookahead_min_ = minutes; }
  // Niet opschakelen zolang de aanvoer in de laatste 5 minuten meer dan zoveel
  // graden gestegen is: de vorige stap werkt dan nog door.
  void set_rise_limit(float c) { rise_limit_ = c; }
  // Terugschakelen boven stand 1 hoeft niet de volle stap-interval te wachten:
  // omlaag is de veilige kant. 0 = dezelfde interval als omhoog.
  void set_down_interval(uint32_t ms) { down_interval_ms_ = ms; }
  // Het doel schuift hooguit zoveel °C per minuut naar MEER vraag toe; minder
  // vraag gaat direct. Een sprong in het doel duwt de unit zo niet meteen naar
  // een hoge stand waar hij doorheen schiet.
  void set_setpoint_slew(float c_per_min) { slew_c_per_min_ = c_per_min; }

  // ── Stooklijn (modus stooklijn en kamertemperatuur) ──
  // Twee punten: de aanvoer bij -10 °C en bij +15 °C buiten, recht ertussen en
  // daarbuiten vast. Boven de stookgrens is er geen warmtevraag.
  void set_curve_cold(float v) { curve_cold_ = v; }
  void set_curve_warm(float v) { curve_warm_ = v; }
  void set_heating_limit(float c) { heat_limit_ = c; }
  float get_curve_supply() const;  // Stooklijn bij de huidige buitentemperatuur

  // ── Kamertemperatuur (modus kamertemperatuur) ──
  // De kamertemperatuur komt van buiten (de YAML zet hem). Elke keer
  // zetten telt als "vers", ook dezelfde waarde. Komt er langer dan de
  // verlooptijd niets, dan gaat de unit uit: zonder kamer geen regeling.
  void set_room_temp(float v);
  void set_room_target(float v) { room_target_ = v; }
  void set_room_timeout(uint32_t ms) { room_timeout_ms_ = ms; }
  // Kamerinvloed: °C aanvoer per °C dat de kamer te koud (of te warm) is.
  void set_room_gain(float k) { room_gain_ = k; }
  // Kamer-nastelling: °C aanvoer die er per minuut per °C fout bijkomt, zodat
  // het doel ook echt gehaald wordt. 0 = uit (alleen kamerinvloed, zoals Anna).
  void set_room_integral(float ki) { room_ki_ = ki; }
  // Uit zodra de kamer ROOM_HYST boven het doel komt; weer aan als hij
  // ROOM_HYST eronder zakt.
  void set_room_off_when_warm(bool b) { room_off_warm_ = b; }
  bool get_room_fresh() const;
  float get_room_temp() const { return room_temp_; }

  // Het doel waar de regeling nu werkelijk op regelt (na stooklijn, kamer en
  // het schuiven). NaN = geen vraag / niet in automatisch bedrijf.
  float get_effective_setpoint() const { return sp_eff_; }
  // Waarom de regeling doet wat hij doet, voor in HA.
  const char *get_reason() const { return reason_; }

  // ── Compile-time config (uit __init__.py) ──
  void set_setpoint_min(float v) { setpoint_min_ = v; }
  void set_setpoint_max(float v) { setpoint_max_ = v; }
  void set_cooling_min_supply(float v) { cooling_min_supply_ = v; }
  void set_dt_low(float v) { dt_low_ = v; }
  void set_dt_high(float v) { dt_high_ = v; }
  void set_max_stand(int v) { max_stand_ = v; }
  void set_step_interval(uint32_t ms) { step_interval_ms_ = ms; }

 protected:
  // ── Protocol ──
  void handle_rx_();
  void finish_frame_();  // Frame compleet: CRC checken + waarden extraheren
  void abort_frame_(const char *reason);  // Frame afgekeurd in de header
  void maybe_send_();
  void build_data2_(uint8_t *out);
  static uint16_t crc_ccitt_(const uint8_t *data, uint8_t len);
  static bool valid_msg_length_(uint8_t len);

  // ── Regeling ──
  void run_control_();

  // Timing-constanten (identiek aan JGC)
  static const uint32_t SEND_DELAY_MS = 99;           // Zenden 99ms na einde RX
  static const uint32_t SEND_TIMEOUT_MS = 2000;       // Fallback als RX uitblijft
  static const uint32_t MIN_SEND_INTERVAL_MS = 300;   // Min. tijd tussen TX's
  static const uint32_t COMM_TIMEOUT_MS = 60000;      // Geen geldige RX -> fail-safe
  // Harde botsingsbescherming: bij 666 baud duurt één byte ~15 ms, dus 40 ms
  // stilte op de lijn betekent gegarandeerd dat de WP klaar is met zenden.
  static const uint32_t LINE_IDLE_MS = 40;
  // Vooruitkijken: pas eerder terugschakelen als de verwachte aanvoer zoveel
  // graden boven het setpoint uitkomt.
  static constexpr float BRAKE_OVERSHOOT_K = 1.0f;
  // Kamertemperatuur: hysterese rond het kamerdoel voor "uit als de kamer warm is".
  static constexpr float ROOM_HYST = 0.2f;
  // Grenzen van de kamer-nastelling (°C aanvoer).
  static constexpr float ROOM_I_MIN = -8.0f, ROOM_I_MAX = 12.0f;
  // Ondergrens van een doel uit stooklijn of kamertemperatuur: kouder water
  // verwarmt niets meer.
  static constexpr float HEAT_MIN_SUPPLY = 20.0f;

  // ── Config / regelparameters ──
  float setpoint_{35.0f};
  // Volledig technisch bereik uit de Chofu-datasheet (AEYC-0643XU),
  // "Operating Range / Leaving Water Temperature":
  //   verwarmen ~60°C  |  koelen 6.5°C~
  // (Atlantic noemt 55°C, maar dat is hun overdrachtspunt naar de gasketel,
  //  geen hardwarelimiet.)
  float setpoint_min_{6.5f}, setpoint_max_{60.0f};
  // Absolute vorstbeveiliging bij koelen - ligt bewust ONDER de fabrieksgrens
  // van 6.5°C, dus hij zit legitiem bedrijf nooit in de weg.
  float cooling_min_supply_{5.0f};
  float dt_low_{4.0f}, dt_high_{8.0f};  // Gezonde delta-T-band (°C)
  // Instelbaar t/m 10; de default blijft 7.
  // Uit de gedisassembleerde Atlantic-firmware (IC1, "Haddon M1.1") blijkt een
  // vooraf uitgerekende telegramtabel voor stand 1 t/m 10 - alle met geldige
  // CRC-16 - en een interne stand-variabele die tot 40 loopt met index =
  // stand/4. Stand 10 is in het origineel dus echt bereikbaar.
  // NIET bewezen voor elke buitenunit: die tabel zit in de BINNENunit, en
  // Atlantic gebruikt een gedeelde codebasis over meerdere modellen (beide
  // MCU's dragen aantoonbaar dode code van elkaar). JGC gebruikte 6 stappen,
  // wij 7, de tabel heeft er 10. Verhoog daarom stapsgewijs en controleer of
  // de sensoren "Vermogen" en "Toerental" - die de WP zelf rapporteert -
  // daadwerkelijk meestijgen. Blijven die vlak, dan pakt jouw unit de hogere
  // trappen niet op en is de rest tabelvulling voor een groter model.
  uint8_t max_stand_{7};
  uint32_t step_interval_ms_{120000};
  // Uit tot de schakelaar in de YAML iets anders zegt. Er zit een gat tussen
  // het opstarten van dit component en het herstellen van die schakelaar; in
  // dat gat hoort de warmtepomp stil te staan.
  bool system_on_{false};
  Mode mode_{MODE_AUTO};
  bool cooling_{false};  // false = verwarmen, true = koelen (experimenteel)
  uint8_t manual_stand_{1};

  // ── Regel-state ──
  uint8_t stand_{0};        // Gecommandeerde stand, 0 t/m max_stand_
  uint32_t last_step_ms_{0};
  // Historie van aanvoer en retour, elke 10 s een monster: 40 plekken = 400 s.
  static const uint8_t HIST_N = 40;
  float sup_hist_[HIST_N];
  float ret_hist_[HIST_N];
  uint8_t hist_idx_{0}, hist_cnt_{0};
  uint32_t hist_last_ms_{0};
  uint8_t avg_samples_() const;
  float avg_hist_(const float *h, uint8_t from, uint8_t n) const;
  float get_rise_() const;  // Stijging over 5 min; NaN = te weinig historie
  float get_rate_() const;  // Stijging per minuut van de gemiddelde aanvoer
  float avg_s_{0.0f};
  float lookahead_min_{0.0f};
  float rise_limit_{0.0f};
  bool rise_holding_{false};
  uint32_t down_interval_ms_{0};
  float slew_c_per_min_{0.0f};
  float sp_eff_{NAN};
  uint32_t sp_eff_ms_{0};
  const char *reason_{"opstarten"};
  void set_reason_(const char *r);
  // Doel uit de modus, zonder schuiven. NaN = geen vraag.
  float target_supply_(uint32_t now);

  // ── Stooklijn / kamertemperatuur ──
  float curve_cold_{50.0f}, curve_warm_{30.0f};
  float heat_limit_{18.0f};
  float room_temp_{NAN};
  uint32_t room_ms_{0};
  uint32_t room_timeout_ms_{30 * 60000};
  float room_target_{20.0f};
  float room_gain_{4.0f};
  float room_ki_{0.0f};
  float room_i_{0.0f};
  uint32_t room_i_ms_{0};
  bool room_off_warm_{false};
  bool room_demand_{true};
  bool above_heat_limit_{false};

  // ── Data uit de warmtepomp ──
  float t_supply_{NAN}, t_return_{NAN}, t_outside_{NAN};
  uint8_t wp_speed_{0};
  float wp_power_w_{0};
  bool defrost_{false};
  uint32_t last_valid_rx_ms_{0};

  // ── TX-rotatie ──
  uint8_t telegram_count_{0};
  uint32_t prev_msg_sent_ms_{0};
  uint32_t prev_msg_in_ended_ms_{0};
  bool is_receiving_{false};

  // ── RX-state machine ──
  enum RxState { WAIT_START, READ_HEADER, READ_PAYLOAD };
  RxState rx_state_{WAIT_START};
  uint8_t rx_hdr_idx_{0};
  uint8_t rx_id_{0};
  uint8_t rx_datalen_{0};
  uint8_t rx_idx_{0};

  // Diagnose: elk compleet frame + CRC hex-loggen. Standaard UIT; aan te
  // zetten via de "Debug frames"-switch in de web-GUI / Home Assistant.
  bool raw_debug_{false};
  uint8_t frame_[40];        // Compleet frame vanaf 0x91
  uint8_t frame_len_{0};
  uint32_t last_byte_ms_{0}; // Tijdstip laatst ontvangen byte (lijn-stiltebewaking)
};

}  // namespace chofu_wp
}  // namespace esphome
