#include "aurea_link.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aurea_link {

static const char *const TAG = "aurea_link";

uint8_t AureaLink::sum8_(const uint8_t *p, uint8_t n) {
  uint8_t s = 0;
  while (n--)
    s = (uint8_t) (s + *p++);
  return s;
}

void AureaLink::setup() { ESP_LOGCONFIG(TAG, "Parameterlink naar de AVR in de IC2-socket"); }

void AureaLink::loop() {
  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b))
      break;

    // Resynchroniseren op een startbyte. Er zijn er twee: het periodieke
    // statusbericht en een los OpenTherm-frame. Welk van de twee bepaalt de
    // lengte die we verwachten. Een 0xF0 of 0xF2 kan ook middenin de data
    // opduiken; de vaste lengte plus de optelsom vangen dat af.
    if (this->idx_ == 0) {
      if (b == STATUS_START)
        this->want_ = STATUS_LEN;
      else if (b == EVENT_START)
        this->want_ = EVENT_LEN;
      else
        continue;
    }

    this->buf_[this->idx_++] = b;
    if (this->idx_ < this->want_)
      continue;

    const uint8_t len = this->want_;
    this->idx_ = 0;

    if (sum8_(this->buf_, len - 1) != this->buf_[len - 1]) {
      this->bad_sum_++;
      ESP_LOGW(TAG, "Bericht met verkeerde optelsom (%u tot nu toe)", this->bad_sum_);
      continue;
    }

    if (len == STATUS_LEN)
      this->handle_status_(this->buf_);
    else
      this->handle_event_(this->buf_);
  }
}

// Een rauw OpenTherm-frame zoals de brug het zag of stuurde.
//
//   bit 31     pariteit
//   bit 30-28  berichttype
//   bit 23-16  Data-ID
//   bit 15-0   waarde
//
// We bewaren alleen de waarde, per Data-ID en per richting. Berichttype hoeft
// niet: uit de richting volgt al of het een vraag of een antwoord was.
void AureaLink::handle_event_(const uint8_t *b) {
  const uint8_t dir = b[1];
  if (dir > OT_TO_STAT)
    return;

  const uint32_t frame = ((uint32_t) b[2] << 24) | ((uint32_t) b[3] << 16) |
                         ((uint32_t) b[4] << 8) | (uint32_t) b[5];
  const uint8_t id = (uint8_t) (frame >> 16);
  const uint8_t type = (uint8_t) ((frame >> 28) & 0x07);
  const uint16_t val = (uint16_t) (frame & 0xFFFF);

  // Een antwoord telt alleen als het er ook een is. Kent de ketel een Data-ID
  // niet, dan komt er DATA-INVALID (6) of UNKNOWN-DATAID (7) terug met een leeg
  // waardeveld. Dat klakkeloos opslaan levert een sensor op die keurig 0 meldt
  // waar het antwoord in werkelijkheid "die vraag ken ik niet" is - en nul is
  // een geloofwaardig getal, dus dat valt niet op.
  //
  // Vragen (van de thermostaat, en wat wij doorsturen) bewaren we wel altijd:
  // daar IS het waardeveld de inhoud, bijvoorbeeld het setpoint in een
  // Write-Data.
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
             id, (unsigned) ((frame >> 28) & 0x07), val);
}

// f8.8: hoge byte is het hele getal met teken, lage byte is 1/256.
static float f88_(uint16_t v) { return (float) ((int16_t) v) / 256.0f; }

// Ouder dan dit en we noemen de waarde niet meer geldig. Een OT-master vraagt
// de meeste Data-ID's ruim binnen een minuut opnieuw op.
static const uint32_t OT_STALE_MS = 120000;

float AureaLink::get_ot(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return NAN;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0 || (millis() - t) > OT_STALE_MS)
    return NAN;
  return f88_(this->ot_val_[dir][data_id]);
}

int AureaLink::get_ot_raw(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return -1;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0 || (millis() - t) > OT_STALE_MS)
    return -1;
  return this->ot_val_[dir][data_id];
}

int AureaLink::get_ot_age(int data_id, OtDirection dir) const {
  if (data_id < 0 || data_id > 255)
    return -1;
  const uint32_t t = this->ot_ms_[dir][data_id];
  if (t == 0)
    return -1;
  return (int) ((millis() - t) / 1000);
}

void AureaLink::handle_status_(const uint8_t *b) {
  this->master_status_ = b[1];
  this->slave_status_ = b[2];
  this->requested_setpoint_ = b[3];
  this->room_temp_ = (int16_t) ((b[4] << 8) | b[5]);
  this->room_setpoint_ = (int16_t) ((b[6] << 8) | b[7]);
  this->boiler_temp_ = (int16_t) ((b[8] << 8) | b[9]);
  this->modulation_ = b[10];
  this->fault_code_ = (int8_t) b[11];
  this->link_flags_ = b[12];
  this->stat_edges_ = b[13];
  this->boiler_edges_ = b[14];
  this->frame_counts_ = b[15];
  this->lines_ = b[16];
  this->rx_err_stat_ = b[17];
  this->rx_err_boiler_ = b[18];
  this->io_ = b[19];
  this->spare_in_ = b[20];
  this->boot_ = b[21];
  this->isr_ovr_ = b[22];

  // Volgnummer zit in de bovenste vier bits en loopt rond op 16. Daarmee zie
  // je of er berichten wegvallen zonder dat je een teller hoeft te delen.
  const uint8_t seq = (uint8_t) (b[12] >> 4);
  if (this->last_seq_ != 0xFF) {
    const uint8_t gap = (uint8_t) ((seq - this->last_seq_) & 0x0F);
    if (gap > 1) {
      this->missed_ = (uint16_t) (this->missed_ + gap - 1);
      ESP_LOGW(TAG, "%u statusbericht(en) gemist (%u totaal)", gap - 1, this->missed_);
    }
  }
  this->last_seq_ = seq;

  this->last_rx_ms_ = millis();
  this->have_data_ = true;
}

void AureaLink::update() {
  uint8_t b[CMD_LEN];
  b[0] = CMD_START;
  b[1] = (uint8_t) ((this->brake_on_ ? 0x01 : 0) | (this->subst_temp_ ? 0x02 : 0) |
                    (this->invert_stat_ ? 0x04 : 0) | (this->invert_boiler_ ? 0x08 : 0) |
                    (this->relay_on_ ? 0x10 : 0) | (this->lamp_invert_ ? 0x20 : 0) |
                    (this->sa_flame_ ? 0x40 : 0) | (this->force_sa_ ? 0x80 : 0));
  b[2] = (uint8_t) ((this->inv_tx_stat_ ? 0x01 : 0) | (this->inv_tx_boiler_ ? 0x02 : 0) |
                    (this->swap_ch_ ? 0x04 : 0) | (this->relay_follows_ ? 0x08 : 0) |
                    (this->demand_enabled_ ? 0x10 : 0));
  b[3] = (uint8_t) lroundf(this->brake_setpoint_);
  b[4] = (uint8_t) (this->wp_supply_ >> 8);
  b[5] = (uint8_t) this->wp_supply_;
  b[6] = (uint8_t) (this->wp_return_ >> 8);
  b[7] = (uint8_t) this->wp_return_;
  b[8] = (uint8_t) (this->override_setpoint_ >> 8);
  b[9] = (uint8_t) this->override_setpoint_;
  b[10] = this->lamp_mode_;
  b[11] = this->sa_modulation_;
  b[12] = this->test_out_;
  b[13] = this->boiler_setpoint_;
  b[14] = 0;   // reserve
  b[15] = sum8_(b, CMD_LEN - 1);
  this->write_array(b, CMD_LEN);
}

void AureaLink::set_brake(bool on) {
  if (on == this->brake_on_)
    return;
  this->brake_on_ = on;
  ESP_LOGI(TAG, "Rem %s", on ? "aan - ketel wordt afgeknepen" : "uit - ketel mag leveren");
}

void AureaLink::set_brake_setpoint(float c) {
  this->brake_setpoint_ = clamp(c, 0.0f, 90.0f);
}

void AureaLink::set_subst_temp(bool on) {
  if (on == this->subst_temp_)
    return;
  this->subst_temp_ = on;
  ESP_LOGI(TAG, "Temperatuursubstitutie naar de thermostaat %s", on ? "aan" : "uit");
}

void AureaLink::set_invert_stat(bool on) {
  if (on == this->invert_stat_)
    return;
  this->invert_stat_ = on;
  ESP_LOGI(TAG, "Leesrichting thermostaatkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink::set_invert_boiler(bool on) {
  if (on == this->invert_boiler_)
    return;
  this->invert_boiler_ = on;
  ESP_LOGI(TAG, "Leesrichting ketelkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink::set_relay(bool on) {
  if (on == this->relay_on_)
    return;
  this->relay_on_ = on;
  ESP_LOGI(TAG, "Relais op socket-pin 16 %s", on ? "aan" : "uit");
}

void AureaLink::set_lamp_invert(bool on) {
  if (on == this->lamp_invert_)
    return;
  this->lamp_invert_ = on;
  ESP_LOGI(TAG, "Statuslampje %s", on ? "omgekeerd" : "normaal");
}

void AureaLink::set_override_setpoint(float c) {
  // Buiten een zinnig kamerbereik betekent "uit". Zo hoef je in de YAML geen
  // aparte schakelaar te hebben: de slider op 0 zetten heft de override op.
  int16_t v = (c < 5.0f || c > 30.0f) ? 0 : (int16_t) lroundf(c * 10.0f);
  if (v == this->override_setpoint_)
    return;
  this->override_setpoint_ = v;
  if (v == 0)
    ESP_LOGI(TAG, "Override opgeheven - de thermostaat regelt weer zelf");
  else
    ESP_LOGI(TAG, "Thermostaat op afstand gezet op %.1f C", v / 10.0f);
}

void AureaLink::set_standalone_modulation(float pct) {
  this->sa_modulation_ = (uint8_t) clamp((int) lroundf(pct), 0, 100);
}

void AureaLink::set_standalone_flame(bool on) { this->sa_flame_ = on; }

void AureaLink::set_force_standalone(bool on) {
  if (on == this->force_sa_)
    return;
  this->force_sa_ = on;
  ESP_LOGI(TAG, "Zelfstandige stand %s", on ? "afgedwongen" : "weer automatisch");
}

void AureaLink::set_invert_tx_stat(bool on) {
  if (on == this->inv_tx_stat_)
    return;
  this->inv_tx_stat_ = on;
  ESP_LOGI(TAG, "Zendpolariteit thermostaatkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink::set_invert_tx_boiler(bool on) {
  if (on == this->inv_tx_boiler_)
    return;
  this->inv_tx_boiler_ = on;
  ESP_LOGI(TAG, "Zendpolariteit ketelkant %s", on ? "omgekeerd" : "normaal");
}

void AureaLink::set_swap_channels(bool on) {
  if (on == this->swap_ch_)
    return;
  this->swap_ch_ = on;
  ESP_LOGW(TAG, "Kanalen %s: thermostaat en ketel zijn %s", on ? "gewisseld" : "normaal",
           on ? "omgedraaid" : "zoals afgeleid uit de firmware");
}

void AureaLink::set_test_out(uint8_t mask, bool on) {
  const uint8_t was = this->test_out_;
  if (on) this->test_out_ |= mask;
  else    this->test_out_ &= (uint8_t) ~mask;
  if (this->test_out_ != was)
    ESP_LOGW(TAG, "Testuitgangen nu 0x%02X - dit stuurt pinnen aan waarvan de "
                  "functie onbekend is", this->test_out_);
}

void AureaLink::set_boiler_setpoint(float c) {
  uint8_t v = (uint8_t) clamp((int) lroundf(c), 0, 90);
  if (v == this->boiler_setpoint_)
    return;
  this->boiler_setpoint_ = v;
  if (v == 0)
    ESP_LOGI(TAG, "Ketel uit");
  else
    ESP_LOGI(TAG, "Ketel rechtstreeks op %u C gezet", v);
}

void AureaLink::set_demand_enabled(bool on) {
  if (on == this->demand_enabled_)
    return;
  this->demand_enabled_ = on;
  ESP_LOGI(TAG, "Ketelvraag-relais K3 %s", on ? "vrijgegeven" : "geblokkeerd");
}

// f8.8: hoge byte hele graden met teken, lage byte 1/256.
uint16_t AureaLink::f88_from_(float v) {
  if (std::isnan(v))
    return 0;
  if (v > 127.0f)
    v = 127.0f;
  if (v < -128.0f)
    v = -128.0f;
  return (uint16_t) ((int16_t) lroundf(v * 256.0f));
}

void AureaLink::send_inject_(uint8_t mode, uint32_t frame) {
  uint8_t b[INJECT_LEN];
  b[0] = INJECT_START;
  b[1] = mode;
  b[2] = (uint8_t) (frame >> 24);
  b[3] = (uint8_t) (frame >> 16);
  b[4] = (uint8_t) (frame >> 8);
  b[5] = (uint8_t) frame;
  b[6] = sum8_(b, INJECT_LEN - 1);
  this->write_array(b, INJECT_LEN);
}

// Pariteitsbit zetten: OpenTherm wil een even aantal enen over het hele frame.
static uint32_t with_parity_(uint32_t f) {
  f &= 0x7FFFFFFFUL;
  uint32_t x = f;
  uint8_t ones = 0;
  while (x) { ones = (uint8_t) (ones + (x & 1)); x >>= 1; }
  return (ones & 1) ? (f | 0x80000000UL) : f;
}

void AureaLink::send_to_boiler(uint32_t frame) { this->send_inject_(0, with_parity_(frame)); }

void AureaLink::write_boiler(uint8_t data_id, float value) {
  const uint32_t f = (1UL << 28) | ((uint32_t) data_id << 16) | f88_from_(value);
  ESP_LOGD(TAG, "Naar de ketel: schrijf ID %u = %.1f", data_id, value);
  this->send_to_boiler(f);
}

void AureaLink::read_boiler(uint8_t data_id) {
  this->send_to_boiler(((uint32_t) data_id << 16));
}

void AureaLink::override_to_thermostat(uint8_t data_id, float value) {
  ESP_LOGD(TAG, "Naar de thermostaat: ID %u wordt %.1f", data_id, value);
  this->send_inject_(1, ((uint32_t) data_id << 16) | f88_from_(value));
}

void AureaLink::clear_override(uint8_t data_id) {
  this->send_inject_(2, ((uint32_t) data_id << 16));
}

void AureaLink::set_relay_follows_switch(bool on) {
  if (on == this->relay_follows_)
    return;
  this->relay_follows_ = on;
  ESP_LOGI(TAG, "Relais volgt %s", on ? "de schakelaar op de voorkant" : "Home Assistant");
}

void AureaLink::set_lamp_mode(int mode) {
  this->lamp_mode_ = (uint8_t) clamp(mode, 0, 4);
}

void AureaLink::set_wp_temps(float supply, float ret) {
  // NaN betekent "de warmtepomp weet het even niet". Dan liever de vorige
  // waarde doorsturen dan een nul, want een nul leest de thermostaat als
  // ijskoud water en dat jaagt haar regellus op.
  if (!std::isnan(supply))
    this->wp_supply_ = (int16_t) lroundf(supply * 10.0f);
  if (!std::isnan(ret))
    this->wp_return_ = (int16_t) lroundf(ret * 10.0f);
}

void AureaLink::dump_config() {
  ESP_LOGCONFIG(TAG, "Aurea parameterlink:");
  ESP_LOGCONFIG(TAG, "  Statusbericht %u bytes, commando %u bytes", STATUS_LEN, CMD_LEN);
  ESP_LOGCONFIG(TAG, "  Terugval van de AVR na 60 s stilte");
  this->check_uart_settings(9600);
}

}  // namespace aurea_link
}  // namespace esphome
