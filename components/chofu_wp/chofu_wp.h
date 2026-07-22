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
// De REGELING is bewust simpel gehouden: "geef me X°C aanvoer" -> deze
// controller kiest zelf stand 0-7 (met stap-modulatie zodat de compressor
// niet volgas de buurman wakker maakt). Plus een handmatige override.
// Geen stooklijn, geen delta-T-correctie - die zaten in de oude .ino maar
// zijn hier weggelaten.

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
  void set_auto_mode(bool a);
  void set_manual_stand(int s);
  void set_cooling(bool c);                       // Koelbedrijf (experimenteel)
  bool get_cooling() const { return cooling_; }
  void set_raw_debug(bool b) { raw_debug_ = b; }  // FRAME-hexlog aan/uit (web-toggle)
  void force_step() { last_step_ms_ = 0; }  // Volgende cyclus mag direct schakelen

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
  uint8_t max_stand_{7};
  uint32_t step_interval_ms_{120000};
  bool system_on_{true};
  bool auto_mode_{true};
  bool cooling_{false};  // false = verwarmen, true = koelen (experimenteel)
  uint8_t manual_stand_{1};

  // ── Regel-state ──
  uint8_t stand_{0};        // Gecommandeerde stand 0-7
  uint32_t last_step_ms_{0};

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
