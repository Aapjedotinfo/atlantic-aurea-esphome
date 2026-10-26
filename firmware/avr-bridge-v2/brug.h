#ifndef BRUG_H
#define BRUG_H
/* De hardware-onafhankelijke kern van de brug: beleid, rem, relais, de regeltabel, de
 * VRAAG/ANTWOORD-toestandsmachine en de berichtenwachtrij naar de ESP.
 * Geen interrupts, geen pinnen, geen klok: alles krijgt `now` (ms) mee. Daardoor
 * test hij op de PC (test/) precies zoals hij op de AVR draait. */
#include "platform.h"
#include "otframe.h"
#include "tabel.h"
#include "proto.h"
KERN_BEGIN

#define SW_HOOFD 2
#define SW_NEVEN 0
#define SW_PATCH 0
#define PROTOCOL_VERSIE 2

/* Vaste tijden (niet instelbaar). */
#define BUDGET_MS          700UL  /* uiterlijk dan moet het antwoord aan de thermostaat beginnen */
#define SLAVE_DELAY_MS      40UL  /* lokaal antwoord: OT-ondergrens is 20 ms */
#define BOILER_RETRY_MS  10000UL
#define BOILER_POLL_MS    1000UL
#define ALIVE_MS          5000UL
#define RELAY_GAP_MS        50UL  /* tussen het ene relais uit en het andere aan */
#define DIP_DEBOUNCE_MS     50UL
#define INJECT_QUIET_MS    150UL  /* na de laatste thermostaatvraag */
#define MASTER_GAP_MS      100UL  /* na het laatste ketelantwoord */
#define STATUS_EVERY_MS   2000UL
#define HELLO_FIRST_MS     100UL
#define HELLO_EVERY_MS    2000UL
#define HELLO_EVERY_WEG_MS 5000UL
#define DEFAULT_BRAKE_SP    10
#define K3_DREMPEL_C        20

/* Bouwoptie: bij een aan/uit-ketel volgt K3 het thermostaat-setpoint na de noodtijd (origineel: "boiler-modus"). */
#ifndef NOOD_K3_VOLGT
#define NOOD_K3_VOLGT 1
#endif

typedef struct {
  uint8_t  master_status, slave_status, requested_setpoint;
  int16_t  room_temp, room_setpoint, boiler_temp;
  uint8_t  modulation, fault_code, stat_alive, boiler_alive;
} Obs;

typedef struct {
  uint8_t  valid;
  uint32_t rx_ms;
  uint8_t  geldig_s;
  uint8_t  brake_on, subst, relay_cmd, lamp_inv, sa_flame, force_sa, ch_off, demand, rem_direct, keteltype;
  uint8_t  brake_sp, lamp_mode, sa_mod, test_out, boiler_sp, max_mod;
  int16_t  wp_supply, wp_return, override;
} Policy;

typedef struct { uint8_t id; uint16_t val; uint8_t used; } Ovr;
typedef struct { uint8_t dir; uint32_t frame; } Ev;
typedef struct { uint8_t seq, status; } AckItem;

/* Momentopname van de hardwaretellers, gevuld door de .ino bij het bouwen van STATUS. */
typedef struct {
  uint8_t edges_stat, edges_boiler, frames_stat, rx_err_stat, rx_err_boiler;
  uint8_t lines, spare, isr_ovr, uart_ovr;
} HwSnap;
typedef void (*HwSnapFn)(HwSnap *);

enum { ST_IDLE = 0, ST_WAIT_ESP_TK, ST_WAIT_BOILER, ST_WAIT_ESP_KT, ST_RESPOND };

typedef struct {
  uint8_t  have_stat;    uint32_t stat;      /* frame van de thermostaat gedecodeerd */
  uint8_t  have_boiler;  uint32_t boiler;    /* frame van de ketel gedecodeerd */
} OtIn;

typedef struct {
  uint8_t  send_boiler;  uint32_t boiler_frame;
  uint8_t  send_stat;    uint32_t stat_frame;
  uint8_t  flush_boiler, flush_stat;         /* wis wat de ontvanger nog als klaar heeft staan */
} OtOut;

typedef struct {
  /* tabellen */
  Tabel act, pend;
  uint8_t pend_valid, pend_seq, to_default;
  /* beleid en waarnemingen */
  Policy pol;
  Obs    obs;
  Ovr    ovr[8];
  uint32_t inj[4]; uint8_t inj_head, inj_tail;
  Ev     ev[8];    uint8_t ev_head, ev_tail;
  uint8_t rx_inv_stat, rx_inv_boiler, tx_inv_stat, tx_inv_boiler, swap_ch;   /* blijven staan, zie PROTOCOL 7.2 */
  /* contact met de ESP */
  uint32_t boot_ms, esp_ms;
  uint8_t  esp_seen, had_esp, handshake;
  /* rem */
  uint8_t  brake_applied, brake_sp_applied;
  uint32_t brake_changed_ms;
  /* dip en relais */
  uint8_t  dip_high, dip_cand;  uint32_t dip_cand_ms;
  uint8_t  bridge_on, k3_on;    uint32_t bridge_off_ms, k3_off_ms;
  uint8_t  aanuit, nood;        /* afgeleid in brug_tick */
  /* transactie */
  uint8_t  st;
  uint32_t t_req, t_state, t_sent, due;
  uint32_t req, sent, pend_frame;
  uint8_t  ours, def_actie, sa_active;
  uint16_t wait_ms;
  uint8_t  vraag_pending, vraag_sent, vraag_dir, vraag_seq; uint32_t vraag_frame;
  uint8_t  ans_have, ans_actie; uint32_t ans_frame;
  uint32_t last_stat_ms, last_boiler_ms, last_boiler_rx_ms, poll_ms, retry_ms;
  uint8_t  poll_turn, parity_fails;
  /* berichtenwachtrij naar de ESP */
  uint8_t  tx_seq;  uint32_t tx_next_ms;
  AckItem  ack[2];  uint8_t ack_n;
  uint32_t hello_due_ms, status_due_ms;
  /* statistiek (verzadigend) */
  uint8_t  cnt_crc, cnt_uart, cnt_vraag_verlopen, cnt_ketel_oud, cnt_ev_gemist, cnt_stat_drop, cnt_antw_negeer;
  /* identiteit */
  uint8_t  reset_cause, boot_count, bouw[3];
  HwSnapFn snap;
} Brug;

void brug_init(Brug *b, uint32_t now, uint8_t reset_cause, uint8_t boot_count, uint8_t dip_high,
               const uint8_t bouw[3], HwSnapFn snap);
/* Een geldig bericht van de ESP (CRC is al gecontroleerd). */
void brug_rx_msg(Brug *b, uint32_t now, uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len);
/* Een OpenTherm-stap. Roep elke lusronde aan, ook zonder frames. */
void brug_ot(Brug *b, uint32_t now, const OtIn *in, OtOut *out);
/* Timers, rem, relais, tabelwissel. Elke lusronde. dip_raw_high = niveau van PC2. */
void brug_tick(Brug *b, uint32_t now, uint8_t dip_raw_high);
/* Bouwt het volgende bericht naar de ESP in buf (minstens PROTO_MAX_MSG bytes). Geeft de lengte, of 0. */
uint8_t brug_tx_next(Brug *b, uint32_t now, uint8_t *buf);

/* Handige uitlezers voor de .ino en de tests */
uint8_t brug_esp_ok(const Brug *b);
void    brug_bouwdatum(const char *date, uint8_t out[3]);   /* "Oct 26 2026" -> 26, 10, 26 */

KERN_END
#endif
