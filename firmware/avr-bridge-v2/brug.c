/* brug.c - kern van de AVR-brug v2: zie brug.h en PROTOCOL.md. */
#include "brug.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t) (((uint16_t) p[0] << 8) | p[1]); }
static void     sat(uint8_t *c) { if (*c < 255) (*c)++; }

/* ══════════════════════════════════════════════════════════════════════════
 *  Contact met de ESP, beleid, overrides
 * ══════════════════════════════════════════════════════════════════════════ */

uint8_t brug_esp_ok(const Brug *b) { return b->esp_seen; }

static void contact(Brug *b, uint32_t now) {
  b->esp_ms = now; b->esp_seen = 1; b->had_esp = 1; b->handshake = 1;
}

static void ovr_clear_all(Brug *b) { memset(b->ovr, 0, sizeof b->ovr); }

static uint8_t ovr_find(const Brug *b, uint8_t id, uint16_t *v) {
  uint8_t i;
  if (!b->pol.valid) return 0;               /* overrides gelden alleen bij een geldig beleid */
  for (i = 0; i < 8; i++)
    if (b->ovr[i].used && b->ovr[i].id == id) { *v = b->ovr[i].val; return 1; }
  return 0;
}

static uint8_t ovr_set(Brug *b, uint8_t id, uint16_t v) {
  uint8_t i;
  for (i = 0; i < 8; i++)
    if (b->ovr[i].used && b->ovr[i].id == id) { b->ovr[i].val = v; return 1; }
  for (i = 0; i < 8; i++)
    if (!b->ovr[i].used) { b->ovr[i].id = id; b->ovr[i].val = v; b->ovr[i].used = 1; return 1; }
  return 0;
}

static void ovr_clear(Brug *b, uint8_t id) {
  uint8_t i;
  for (i = 0; i < 8; i++)
    if (b->ovr[i].used && b->ovr[i].id == id) b->ovr[i].used = 0;
}

/* Het beleid vervalt: alles wat de ESP zette gaat terug naar de veilige standaard.
 * De polariteitsvlaggen blijven (hardware-aannames, geen beleid). */
static void beleid_ongeldig(Brug *b) {
  memset(&b->pol, 0, sizeof b->pol);
  ovr_clear_all(b);
  b->inj_head = b->inj_tail = 0;
}

static void ack_push(Brug *b, uint8_t seq, uint8_t status) {
  if (b->ack_n >= 2) b->ack_n = 1;     /* vol: de oudste vervalt */
  b->ack[b->ack_n].seq = seq; b->ack[b->ack_n].status = status;
  b->ack_n++;
}

static void ev_push(Brug *b, uint8_t dir, uint32_t f) {
  const uint8_t next = (uint8_t) ((b->ev_head + 1) & 7);
  if (next == b->ev_tail) { sat(&b->cnt_ev_gemist); return; }
  b->ev[b->ev_head].dir = dir; b->ev[b->ev_head].frame = f;
  b->ev_head = next;
}

static uint8_t cmd_apply(Brug *b, uint32_t now, const uint8_t *p, uint8_t len) {
  Policy x;
  const uint8_t f1 = p[0], f2 = p[1];
  int16_t ws, wr, ov;
  if (len != 15) return ST_LENGTE;
  ws = (int16_t) be16(p + 3); wr = (int16_t) be16(p + 5); ov = (int16_t) be16(p + 7);
  if ((f2 >> 6) > 2) return ST_GRENS;
  if (p[2] > 90 || p[12] > 90) return ST_GRENS;
  if (ws < -300 || ws > 1500 || wr < -300 || wr > 1500) return ST_GRENS;
  if (ov != 0 && (ov < 50 || ov > 300)) return ST_GRENS;
  if (p[9] > 4 || p[10] > 100 || p[11] > 0x1F || p[13] > 100) return ST_GRENS;
  if (p[14] != 0 && p[14] < 10) return ST_GRENS;

  memset(&x, 0, sizeof x);
  x.valid = 1; x.rx_ms = now; x.geldig_s = p[14];
  x.brake_on = (uint8_t) ((f1 & 0x01) != 0);  x.subst = (uint8_t) ((f1 & 0x02) != 0);
  x.relay_cmd = (uint8_t) ((f1 & 0x10) != 0); x.lamp_inv = (uint8_t) ((f1 & 0x20) != 0);
  x.sa_flame = (uint8_t) ((f1 & 0x40) != 0);  x.force_sa = (uint8_t) ((f1 & 0x80) != 0);
  x.ch_off = (uint8_t) ((f2 & 0x08) != 0);    x.demand = (uint8_t) ((f2 & 0x10) != 0);
  x.rem_direct = (uint8_t) ((f2 & 0x20) != 0); x.keteltype = (uint8_t) (f2 >> 6);
  x.brake_sp = p[2]; x.wp_supply = ws; x.wp_return = wr; x.override = ov;
  x.lamp_mode = p[9]; x.sa_mod = p[10]; x.test_out = p[11]; x.boiler_sp = p[12]; x.max_mod = p[13];
  b->pol = x;
  b->rx_inv_stat   = (uint8_t) ((f1 & 0x04) != 0);
  b->rx_inv_boiler = (uint8_t) ((f1 & 0x08) != 0);
  b->tx_inv_stat   = (uint8_t) ((f2 & 0x01) != 0);
  b->tx_inv_boiler = (uint8_t) ((f2 & 0x02) != 0);
  b->swap_ch       = (uint8_t) ((f2 & 0x04) != 0);
  return ST_OK;
}

static uint8_t inject_apply(Brug *b, const uint8_t *p, uint8_t len) {
  uint32_t f;
  if (len != 5) return ST_LENGTE;
  f = ((uint32_t) p[1] << 24) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 8) | p[4];
  switch (p[0]) {
    case 0: {
      uint8_t next;
      if (ot_type(f) > MSG_WRITE_DATA) return ST_GRENS;
      next = (uint8_t) ((b->inj_head + 1) & 3);
      if (next == b->inj_tail) return ST_BEZET;
      b->inj[b->inj_head] = ot_with_parity(f); b->inj_head = next;
      return ST_OK;
    }
    case 1: return ovr_set(b, ot_id(f), ot_val(f)) ? ST_OK : ST_BEZET;
    case 2: ovr_clear(b, ot_id(f)); return ST_OK;
    default: return ST_GRENS;
  }
}

void brug_rx_msg(Brug *b, uint32_t now, uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len) {
  uint8_t r;
  switch (type) {
    case MT_TABEL:
      contact(b, now);
      r = tabel_parse(&b->pend, p, len);
      if (r == ST_OK) { b->pend_valid = 1; b->pend_seq = seq; b->to_default = 0; }
      else            ack_push(b, seq, r);
      break;
    case MT_COMMANDO:
      contact(b, now);
      r = cmd_apply(b, now, p, len);
      if (r != ST_OK) ack_push(b, seq, r);
      break;
    case MT_INJECT:
      contact(b, now);
      r = inject_apply(b, p, len);
      if (r != ST_OK) ack_push(b, seq, r);
      break;
    case MT_ACK:
      if (len == 2) contact(b, now); else ack_push(b, seq, ST_LENGTE);
      break;
    case MT_ANTWOORD:
      contact(b, now);
      if (len != 6) { ack_push(b, seq, ST_LENGTE); break; }
      if ((b->st == ST_WAIT_ESP_TK || b->st == ST_WAIT_ESP_KT) && b->vraag_sent &&
          p[0] == b->vraag_seq && !b->ans_have && p[1] <= 2) {
        b->ans_have = 1; b->ans_actie = p[1];
        b->ans_frame = ((uint32_t) p[2] << 24) | ((uint32_t) p[3] << 16) | ((uint32_t) p[4] << 8) | p[5];
      } else {
        sat(&b->cnt_antw_negeer);     /* verkeerd, oud of te laat volgnummer: negeren */
      }
      break;
    default:
      break;                          /* onbekend type: negeren, geen levensteken */
  }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Init
 * ══════════════════════════════════════════════════════════════════════════ */

void brug_bouwdatum(const char *d, uint8_t out[3]) {
  static const char mnd[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  uint8_t m = 0, i;
  for (i = 0; i < 12; i++)
    if (d[0] == mnd[3 * i] && d[1] == mnd[3 * i + 1] && d[2] == mnd[3 * i + 2]) { m = (uint8_t) (i + 1); break; }
  out[1] = m;
  out[2] = (uint8_t) (((d[4] == ' ') ? 0 : (d[4] - '0') * 10) + (d[5] - '0'));
  out[0] = (uint8_t) ((d[9] - '0') * 10 + (d[10] - '0'));
}

void brug_init(Brug *b, uint32_t now, uint8_t reset_cause, uint8_t boot_count, uint8_t dip_high,
               const uint8_t bouw[3], HwSnapFn snap) {
  memset(b, 0, sizeof *b);
  tabel_standaard(&b->act);
  b->boot_ms = b->esp_ms = now;               /* de noodtijd telt vanaf de start */
  /* OPSTART GEREMD: rem toegepast, 10 C, de eerste wissel mag meteen. */
  b->brake_applied = 1; b->brake_sp_applied = DEFAULT_BRAKE_SP;
  b->brake_changed_ms = now - (uint32_t) b->act.rem_hold_s * 1000UL;
  b->dip_high = b->dip_cand = (uint8_t) (dip_high != 0); b->dip_cand_ms = now;
  b->bridge_off_ms = b->k3_off_ms = now - 1000UL;
  b->retry_ms = now - BOILER_RETRY_MS;        /* de ketel meteen proberen, geen 10 s verzonnen antwoorden */
  b->last_stat_ms = now - 1000UL;
  b->last_boiler_rx_ms = now - 1000UL;
  b->poll_ms = now;
  b->hello_due_ms = now + HELLO_FIRST_MS;
  b->status_due_ms = now + 1000UL;
  b->tx_next_ms = now;
  b->reset_cause = reset_cause; b->boot_count = boot_count;
  b->bouw[0] = bouw[0]; b->bouw[1] = bouw[1]; b->bouw[2] = bouw[2];
  b->snap = snap;
  b->st = ST_IDLE;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Regels toepassen
 * ══════════════════════════════════════════════════════════════════════════ */

/* De actieve regel voor (id, richting), of NULL als het PASS is of de conditie niet waar is. */
static const Regel *actief(const Brug *b, uint8_t id, uint8_t dir) {
  const Regel *r = tabel_zoek(&b->act, id, dir);
  uint8_t ok;
  if (!r || R_MODE(r->modus) == MODE_PASS) return 0;
  switch (R_COND(r->modus)) {
    case COND_ALTIJD:       ok = 1; break;
    case COND_REM:          ok = b->brake_applied; break;
    case COND_SUBST:        ok = (uint8_t) (b->pol.valid && b->pol.subst); break;
    case COND_OVERRIDE:     ok = (uint8_t) (b->pol.valid && b->pol.override != 0); break;
    case COND_CHOFF:        ok = (uint8_t) (b->pol.valid && b->pol.ch_off); break;
    case COND_REM_OF_CHOFF: ok = (uint8_t) (b->brake_applied || (b->pol.valid && b->pol.ch_off)); break;
    case COND_MAXMOD:       ok = (uint8_t) (b->pol.valid && b->pol.max_mod != 0); break;
    default:                ok = 0; break;
  }
  return ok ? r : 0;
}

static uint16_t waarde_van(const Brug *b, const Regel *r) {
  const uint16_t v = (uint16_t) (((uint16_t) r->p0 << 8) | r->p1);
  switch (v) {
    case VAL_REMSP: {
      const uint8_t sp = b->brake_sp_applied > 100 ? 100 : b->brake_sp_applied;
      return tenths_to_f88((int16_t) ((int16_t) sp * 10));
    }
    case VAL_AANVOER:  return tenths_to_f88(b->pol.wp_supply);
    case VAL_RETOUR:   return tenths_to_f88(b->pol.wp_return);
    case VAL_OVERRIDE: return tenths_to_f88(b->pol.override);
    case VAL_MAXMOD:   return tenths_to_f88((int16_t) ((int16_t) b->pol.max_mod * 10));
    default:           return v;
  }
}

/* VAST, richting thermostaat -> ketel. */
static uint32_t vast_tk(const Brug *b, const Regel *r, uint32_t f) {
  const uint8_t t = ot_type(f);
  if (r->modus & FL_MASKER) {
    if (t == MSG_READ_DATA || t == MSG_WRITE_DATA) {
      uint8_t hi = (uint8_t) (ot_val(f) >> 8);
      hi = (uint8_t) ((hi & (uint8_t) ~r->p0) | r->p1);
      return ot_replace_value(f, (uint16_t) (((uint16_t) hi << 8) | (ot_val(f) & 0xFF)));
    }
    return f;
  }
  if (t == MSG_WRITE_DATA) return ot_replace_value(f, waarde_van(b, r));
  if (t == MSG_READ_DATA && (r->modus & FL_FORCEER))
    return ot_make(MSG_WRITE_DATA, ot_id(f), waarde_van(b, r));
  return f;
}

/* VAST, richting ketel -> thermostaat. f = ketelantwoord. */
static uint32_t vast_kt(const Brug *b, const Regel *r, uint32_t f) {
  const uint8_t ft = ot_type(f);
  const uint8_t rt = ot_type(b->req);
  const uint8_t ack = (uint8_t) (ft == MSG_READ_ACK || ft == MSG_WRITE_ACK);
  uint16_t v;
  if (!ack && !(r->modus & FL_FORCEER)) return f;
  if (r->modus & FL_ECHO) {
    if (rt != MSG_WRITE_DATA) return f;
    v = ot_val(b->req);
  } else {
    v = waarde_van(b, r);
  }
  return ot_make((uint8_t) ((rt == MSG_WRITE_DATA) ? MSG_WRITE_ACK : MSG_READ_ACK), ot_id(f), v);
}

/* Het antwoordtype moet bij de vraag van de thermostaat passen (bv. na FORCEER READ -> WRITE). */
static uint32_t type_fixup(uint32_t req, uint32_t rep) {
  const uint8_t rt = ot_type(req), pt = ot_type(rep);
  if (rt == MSG_READ_DATA && pt == MSG_WRITE_ACK)  return ot_set_type(rep, MSG_READ_ACK);
  if (rt == MSG_WRITE_DATA && pt == MSG_READ_ACK)  return ot_set_type(rep, MSG_WRITE_ACK);
  return rep;
}

static uint32_t lokaal(uint32_t req, uint8_t type) {
  return ot_set_type(req, (uint8_t) (type == 0 ? MSG_UNKNOWN_ID : type));
}

/* Antwoord als de ketel niet (op tijd) antwoordt: zoals v1. */
static uint32_t noodantwoord(const Brug *b, uint32_t req) {
  if (ot_id(req) == ID_STATUS) return ot_make(MSG_READ_ACK, ID_STATUS, b->obs.slave_status);
  return lokaal(req, MSG_UNKNOWN_ID);
}

/* Zelfstandige slave: ketel dood, afgedwongen, of aan/uit-ketel. Zoals v1. */
static uint32_t zelfstandig(const Brug *b, uint32_t req) {
  const uint8_t mt = ot_type(req), id = ot_id(req);
  uint16_t val, v;
  if (mt == MSG_WRITE_DATA) return ot_set_type(req, MSG_WRITE_ACK);
  if (mt != MSG_READ_DATA)  return lokaal(req, MSG_UNKNOWN_ID);
  switch (id) {
    case ID_STATUS: {
      uint8_t slave = 0;
      if (b->obs.master_status & 0x01) slave |= 0x02;      /* CH-modus actief */
      if (b->pol.sa_flame)             slave |= 0x08;      /* "vlam" = de warmtepomp levert */
      val = slave;
      break;
    }
    case ID_SLAVE_CONFIG:  val = 0x0000; break;
    case ID_FAULT_FLAGS:   val = 0x0000; break;
    case ID_MODULATION:    val = tenths_to_f88((int16_t) ((int16_t) b->pol.sa_mod * 10)); break;
    case ID_REMOTE_OVERRIDE:
      val = (b->pol.valid && b->pol.override != 0) ? tenths_to_f88(b->pol.override) : 0x0000;
      break;
    case ID_OVERRIDE_FUNC: val = 0x0000; break;
    case ID_BOILER_TEMP:
    case ID_RETURN_TEMP:
      if (!b->pol.valid) return lokaal(req, MSG_DATA_INVALID);     /* liever eerlijk dan 0 graden melden */
      val = tenths_to_f88(id == ID_BOILER_TEMP ? b->pol.wp_supply : b->pol.wp_return);
      break;
    default:
      if (ovr_find(b, id, &v)) { val = v; break; }
      return lokaal(req, MSG_UNKNOWN_ID);
  }
  return ot_make(MSG_READ_ACK, id, val);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Waarnemingen
 * ══════════════════════════════════════════════════════════════════════════ */

static void note_from_thermostat(Brug *b, uint32_t f) {
  const uint8_t id = ot_id(f), mt = ot_type(f);
  if (mt != MSG_WRITE_DATA && mt != MSG_READ_DATA) return;
  switch (id) {
    case ID_CONTROL_SETPOINT: b->obs.requested_setpoint = (uint8_t) (f88_to_tenths(ot_val(f)) / 10); break;
    case ID_ROOM_TEMP:        b->obs.room_temp = f88_to_tenths(ot_val(f)); break;
    case ID_ROOM_SETPOINT:    b->obs.room_setpoint = f88_to_tenths(ot_val(f)); break;
    case ID_STATUS:           b->obs.master_status = (uint8_t) (ot_val(f) >> 8); break;
    default: break;
  }
}

static void note_from_boiler(Brug *b, uint32_t f) {
  if (ot_type(f) != MSG_READ_ACK && ot_type(f) != MSG_WRITE_ACK) return;
  switch (ot_id(f)) {
    case ID_BOILER_TEMP: b->obs.boiler_temp = f88_to_tenths(ot_val(f)); break;
    case ID_MODULATION:  b->obs.modulation  = (uint8_t) (f88_to_tenths(ot_val(f)) / 10); break;
    case ID_FAULT_FLAGS: b->obs.fault_code  = (uint8_t) (ot_val(f) & 0xFF); break;
    case ID_STATUS:      b->obs.slave_status = (uint8_t) (ot_val(f) & 0xFF); break;
    default: break;
  }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  De OpenTherm-transactie
 * ══════════════════════════════════════════════════════════════════════════ */

/* Verlopen een wachttijd van len ms, gerekend vanaf t_a, binnen het totaalbudget vanaf t_req? */
static uint8_t verlopen(uint32_t now, uint32_t t_req, uint32_t t_a, uint32_t len) {
  uint32_t end = (t_a - t_req) + len;
  if (end > BUDGET_MS) end = BUDGET_MS;
  return (uint8_t) ((now - t_req) >= end);
}

static void to_boiler(Brug *b, uint32_t now, OtOut *out, uint32_t f, uint8_t ours) {
  out->send_boiler = 1; out->boiler_frame = f; out->flush_boiler = 1;
  ev_push(b, 2, f);
  b->sent = f; b->ours = ours; b->t_sent = now; b->st = ST_WAIT_BOILER;
  b->sa_active = 0;
}

static void to_stat(Brug *b, OtOut *out, uint32_t f) {
  out->send_stat = 1; out->stat_frame = f; out->flush_stat = 1;
  ev_push(b, 3, f);
  b->st = ST_IDLE;
}

static void lokaal_antwoord(Brug *b, uint32_t now, uint32_t f, uint8_t met_vertraging) {
  b->pend_frame = f; b->st = ST_RESPOND;
  b->due = met_vertraging ? (b->t_req + SLAVE_DELAY_MS) : now;
  if ((int32_t) (now - b->due) > 0) b->due = now;
}

static void vraag_start(Brug *b, uint32_t now, uint8_t dir, uint32_t f, uint8_t p0, uint8_t p1, uint8_t st) {
  b->vraag_pending = 1; b->vraag_sent = 0; b->vraag_dir = dir; b->vraag_frame = f;
  b->ans_have = 0;
  b->wait_ms = (uint16_t) (p0 ? (uint16_t) p0 * 4 : b->act.deadline_ms);
  b->def_actie = p1;
  b->t_state = now; b->st = st;
}

static void vraag_klaar(Brug *b) { b->vraag_pending = 0; b->vraag_sent = 0; b->ans_have = 0; }

/* Een frame van de thermostaat is binnen; kies wat ermee gebeurt (richting T>K). */
static void tk_toepassen(Brug *b, uint32_t now, OtOut *out, uint32_t f) {
  const Regel *r = actief(b, ot_id(f), DIR_TK);
  uint32_t uit = f;
  if (r) {
    switch (R_MODE(r->modus)) {
      case MODE_VAST: uit = vast_tk(b, r, f); break;
      case MODE_BLOKKEER:
        lokaal_antwoord(b, now, lokaal(f, r->p1), 1);
        return;
      case MODE_VRAAG:
        if (b->esp_seen) { vraag_start(b, now, DIR_TK, f, r->p0, r->p1, ST_WAIT_ESP_TK); return; }
        if (r->p1 == 1) { lokaal_antwoord(b, now, lokaal(f, MSG_UNKNOWN_ID), 1); return; }
        break;
      default: break;
    }
  }
  to_boiler(b, now, out, uit, 0);
}

/* Het ketelantwoord f is binnen; kies wat de thermostaat krijgt (richting K>T). */
static void kt_toepassen(Brug *b, uint32_t now, OtOut *out, uint32_t f) {
  const Regel *r = actief(b, ot_id(f), DIR_KT);
  uint32_t rep = f;
  uint16_t v;
  if (r) {
    switch (R_MODE(r->modus)) {
      case MODE_VAST: rep = vast_kt(b, r, f); break;
      case MODE_BLOKKEER: rep = lokaal(f, r->p1); break;
      case MODE_VRAAG:
        if (b->esp_seen) {
          b->pend_frame = f;
          vraag_start(b, now, DIR_KT, f, r->p0, r->p1, ST_WAIT_ESP_KT);
          return;
        }
        if (r->p1 == 1) rep = lokaal(f, MSG_UNKNOWN_ID);
        break;
      default: break;
    }
  } else if (ot_type(f) == MSG_READ_ACK && ovr_find(b, ot_id(f), &v)) {
    rep = ot_replace_value(f, v);                 /* door de ESP gezet antwoord (INJECT modus 1) */
  }
  to_stat(b, out, type_fixup(b->req, rep));
}

/* ANTWOORD of verlopen deadline bij T>K: ga verder met het frame naar de ketel, of antwoord lokaal. */
static void tk_na_esp(Brug *b, uint32_t now, OtOut *out, uint8_t actie, uint32_t nieuw, uint8_t geldig) {
  const uint32_t f = b->req;
  vraag_klaar(b);
  if (geldig && actie == 2) { lokaal_antwoord(b, now, lokaal(f, MSG_UNKNOWN_ID), 1); return; }
  if (geldig && actie == 1) { to_boiler(b, now, out, ot_with_parity(nieuw), 0); return; }
  if (!geldig && b->def_actie == 1) { lokaal_antwoord(b, now, lokaal(f, MSG_UNKNOWN_ID), 1); return; }
  to_boiler(b, now, out, f, 0);
}

static void kt_na_esp(Brug *b, OtOut *out, uint8_t actie, uint32_t nieuw, uint8_t geldig) {
  const uint32_t f = b->pend_frame;
  vraag_klaar(b);
  if (geldig && actie == 2) { to_stat(b, out, type_fixup(b->req, lokaal(f, MSG_UNKNOWN_ID))); return; }
  if (geldig && actie == 1) { to_stat(b, out, type_fixup(b->req, ot_with_parity(nieuw))); return; }
  if (!geldig && b->def_actie == 1) { to_stat(b, out, type_fixup(b->req, lokaal(f, MSG_UNKNOWN_ID))); return; }
  to_stat(b, out, type_fixup(b->req, f));
}

/* Is een ANTWOORD-frame bruikbaar? T>K: type 0/1 en hetzelfde ID. K>T: type 4..7 en hetzelfde ID. */
static uint8_t esp_frame_ok(uint32_t orig, uint32_t nieuw, uint8_t dir) {
  if (ot_id(nieuw) != ot_id(orig)) return 0;
  if (dir == DIR_TK) return (uint8_t) (ot_type(nieuw) <= MSG_WRITE_DATA);
  return (uint8_t) (ot_type(nieuw) >= MSG_READ_ACK);
}

static uint8_t antwoord_past(uint32_t sent, uint32_t f) {
  const uint8_t st = ot_type(sent), ft = ot_type(f);
  if (ot_id(f) != ot_id(sent)) return 0;
  if (st == MSG_READ_DATA)  return (uint8_t) (ft == MSG_READ_ACK  || ft == MSG_DATA_INVALID || ft == MSG_UNKNOWN_ID);
  if (st == MSG_WRITE_DATA) return (uint8_t) (ft == MSG_WRITE_ACK || ft == MSG_DATA_INVALID || ft == MSG_UNKNOWN_ID);
  return 0;
}

static void idle_zonder_stat(Brug *b, uint32_t now, OtOut *out) {
  if (b->aanuit) return;                          /* aan/uit-ketel: niets naar de ketel */
  if ((uint32_t) (now - b->last_boiler_rx_ms) < MASTER_GAP_MS) return;

  if (b->inj_tail != b->inj_head && (uint32_t) (now - b->last_stat_ms) > INJECT_QUIET_MS) {
    const uint32_t f2 = b->inj[b->inj_tail];
    b->inj_tail = (uint8_t) ((b->inj_tail + 1) & 3);
    to_boiler(b, now, out, f2, 1);
    return;
  }
  if (!b->obs.stat_alive && (uint32_t) (now - b->poll_ms) > BOILER_POLL_MS) {
    const uint8_t want_heat = (uint8_t) (b->pol.valid && b->pol.boiler_sp > 0);
    uint32_t frame;
    b->poll_ms = now;
    if ((b->poll_turn++ & 1) && want_heat)
      frame = ot_make(MSG_WRITE_DATA, ID_CONTROL_SETPOINT,
                      tenths_to_f88((int16_t) ((int16_t) b->pol.boiler_sp * 10)));
    else   /* DHW-enable staat altijd aan (review 5b); CH alleen met de wens van de ESP */
      frame = ot_make(MSG_READ_DATA, ID_STATUS, (uint16_t) ((want_heat ? 0x0100u : 0u) | 0x0200u));
    to_boiler(b, now, out, frame, 1);
  }
}

void brug_ot(Brug *b, uint32_t now, const OtIn *in, OtOut *out) {
  memset(out, 0, sizeof *out);

  /* Noodbrug aan: de thermostaat praat rechtstreeks met de ketel. Wij zwijgen. */
  if (b->bridge_on) {
    b->st = ST_IDLE; vraag_klaar(b);
    return;
  }

  /* Frames van de ketel buiten een wachtstand zijn te laat of ruis: weg ermee (review 5a). */
  if (in->have_boiler && b->st != ST_WAIT_BOILER) { sat(&b->cnt_ketel_oud); }

  switch (b->st) {
    case ST_IDLE:
      if (in->have_stat) {
        const uint32_t f = in->stat;
        b->last_stat_ms = now; b->obs.stat_alive = 1;
        if (!ot_parity_ok(f)) { if (b->parity_fails < 15) b->parity_fails++; break; }
        note_from_thermostat(b, f);
        ev_push(b, 0, f);
        b->t_req = now; b->req = f;
        if (b->aanuit || b->pol.force_sa ||
            (!b->obs.boiler_alive && (uint32_t) (now - b->retry_ms) < BOILER_RETRY_MS)) {
          b->sa_active = 1;
          b->pend_frame = zelfstandig(b, f);
          b->due = now + SLAVE_DELAY_MS; b->st = ST_RESPOND;
          break;
        }
        if (!b->obs.boiler_alive) b->retry_ms = now;
        tk_toepassen(b, now, out, f);
      } else {
        idle_zonder_stat(b, now, out);
      }
      break;

    case ST_WAIT_ESP_TK:
      if (b->ans_have) {
        const uint8_t ok = (uint8_t) (b->ans_actie != 1 || esp_frame_ok(b->req, b->ans_frame, DIR_TK));
        if (ok) { tk_na_esp(b, now, out, b->ans_actie, b->ans_frame, 1); break; }
        b->ans_have = 0;                      /* ongeldig: geldt als geen antwoord */
      }
      if (!b->esp_seen || verlopen(now, b->t_req, b->t_state, b->wait_ms)) {
        sat(&b->cnt_vraag_verlopen);
        tk_na_esp(b, now, out, 0, 0, 0);
      }
      break;

    case ST_WAIT_BOILER:
      if (in->have_boiler) {
        const uint32_t f = in->boiler;
        if (!ot_parity_ok(f) || !antwoord_past(b->sent, f)) { sat(&b->cnt_ketel_oud); }
        else {
          b->last_boiler_ms = b->last_boiler_rx_ms = now; b->obs.boiler_alive = 1;
          note_from_boiler(b, f);
          ev_push(b, 1, f);
          if (b->ours) b->st = ST_IDLE;
          else         kt_toepassen(b, now, out, f);
          break;
        }
      }
      if (b->ours ? ((uint32_t) (now - b->t_sent) >= b->act.ketel_ms)
                  : verlopen(now, b->t_req, b->t_sent, b->act.ketel_ms)) {
        if (b->ours) b->st = ST_IDLE;
        else {
          /* Geen (geldig) ketelantwoord: de thermostaat krijgt toch een antwoord. */
          b->pend_frame = noodantwoord(b, b->req);
          b->due = now; b->st = ST_RESPOND;
        }
      }
      break;

    case ST_WAIT_ESP_KT:
      if (b->ans_have) {
        const uint8_t ok = (uint8_t) (b->ans_actie != 1 || esp_frame_ok(b->pend_frame, b->ans_frame, DIR_KT));
        if (ok) { kt_na_esp(b, out, b->ans_actie, b->ans_frame, 1); break; }
        b->ans_have = 0;
      }
      if (!b->esp_seen || verlopen(now, b->t_req, b->t_state, b->wait_ms)) {
        sat(&b->cnt_vraag_verlopen);
        kt_na_esp(b, out, 0, 0, 0);
      }
      break;

    case ST_RESPOND:
      if ((int32_t) (now - b->due) >= 0) to_stat(b, out, b->pend_frame);
      break;

    default:
      b->st = ST_IDLE;
      break;
  }

  if ((uint32_t) (now - b->last_stat_ms) > ALIVE_MS)   b->obs.stat_alive = 0;
  if ((uint32_t) (now - b->last_boiler_ms) > ALIVE_MS) b->obs.boiler_alive = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Timers, rem, relais
 * ══════════════════════════════════════════════════════════════════════════ */

static void rem_bijwerken(Brug *b, uint32_t now) {
  const uint8_t geldig = b->pol.valid;
  const uint8_t want_on = geldig ? b->pol.brake_on : 1;             /* geen beleid: geremd */
  const uint8_t want_sp = geldig ? b->pol.brake_sp : DEFAULT_BRAKE_SP;
  const uint8_t direct  = (uint8_t) (!geldig || b->pol.rem_direct);
  const uint8_t verschil = (uint8_t) (want_on != b->brake_applied ||
                                      (want_on && want_sp != b->brake_sp_applied));
  if (!verschil) return;
  if (direct || (uint32_t) (now - b->brake_changed_ms) >= (uint32_t) b->act.rem_hold_s * 1000UL) {
    b->brake_applied = want_on;
    if (want_on) b->brake_sp_applied = want_sp;
    b->brake_changed_ms = now;
  }
}

static void relais_bijwerken(Brug *b, uint32_t now) {
  uint8_t kt = b->pol.valid ? b->pol.keteltype : 0;
  uint8_t br_want, k3_want, sp, ongeremd;

  b->aanuit = (uint8_t) (kt == 2 || (kt == 0 && b->dip_high));
  b->nood = (uint8_t) ((uint32_t) (now - b->esp_ms) >= (uint32_t) b->act.nood_s * 1000UL);

  br_want = (uint8_t) (!b->aanuit && (b->nood || (b->pol.valid && b->pol.relay_cmd)));

  ongeremd = (uint8_t) (NOOD_K3_VOLGT && b->nood);
  sp = (b->brake_applied && !ongeremd) ? b->brake_sp_applied : b->obs.requested_setpoint;
  if (!b->obs.stat_alive && !(b->brake_applied && !ongeremd)) sp = 0;   /* geen thermostaat: geen vraag */
  k3_want = (uint8_t) (b->aanuit && !br_want && sp >= K3_DREMPEL_C &&
                       ((b->pol.valid && b->pol.demand) || ongeremd));

  /* Nooit samen aan; de noodbrug wint; tussen uit en aan zit RELAY_GAP_MS. */
  if (!br_want && b->bridge_on) { b->bridge_on = 0; b->bridge_off_ms = now; }
  if (!k3_want && b->k3_on)     { b->k3_on = 0;     b->k3_off_ms = now; }
  if (br_want && !b->bridge_on && !b->k3_on && (uint32_t) (now - b->k3_off_ms) >= RELAY_GAP_MS)
    b->bridge_on = 1;
  if (k3_want && !b->k3_on && !b->bridge_on && (uint32_t) (now - b->bridge_off_ms) >= RELAY_GAP_MS)
    b->k3_on = 1;
}

void brug_tick(Brug *b, uint32_t now, uint8_t dip_raw_high) {
  /* dipschakelaar, 50 ms ontdenderd */
  if ((dip_raw_high != 0) != b->dip_cand) { b->dip_cand = (uint8_t) (dip_raw_high != 0); b->dip_cand_ms = now; }
  else if (b->dip_cand != b->dip_high && (uint32_t) (now - b->dip_cand_ms) >= DIP_DEBOUNCE_MS)
    b->dip_high = b->dip_cand;

  /* geldigheid van het beleid */
  if (b->pol.valid) {
    const uint32_t g = (uint32_t) (b->pol.geldig_s ? b->pol.geldig_s : b->act.esp_weg_s) * 1000UL;
    if ((uint32_t) (now - b->pol.rx_ms) >= g) beleid_ongeldig(b);
  }

  /* de ESP is weg: terug naar de ingebouwde tabel, HELLO weer elke 5 s */
  if (b->esp_seen && (uint32_t) (now - b->esp_ms) >= (uint32_t) b->act.esp_weg_s * 1000UL) {
    b->esp_seen = 0; b->handshake = 0; b->to_default = 1; b->hello_due_ms = now;
  }

  /* een tabel wisselen mag alleen tussen twee gesprekken */
  if (b->st == ST_IDLE && !b->vraag_pending && !b->vraag_sent) {
    if (b->pend_valid) {
      b->act = b->pend; b->pend_valid = 0;
      ack_push(b, b->pend_seq, ST_OK);
    } else if (b->to_default) {
      tabel_standaard(&b->act); b->to_default = 0;
    }
  }

  rem_bijwerken(b, now);
  relais_bijwerken(b, now);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Berichten naar de ESP
 * ══════════════════════════════════════════════════════════════════════════ */

static uint8_t bouw_hello(const Brug *b, uint8_t *p) {
  p[0] = SW_HOOFD; p[1] = SW_NEVEN; p[2] = SW_PATCH;
  p[3] = b->bouw[0]; p[4] = b->bouw[1]; p[5] = b->bouw[2];
  p[6] = PROTOCOL_VERSIE;
  p[7] = (uint8_t) (b->act.crc >> 8); p[8] = (uint8_t) b->act.crc;
  p[9] = b->reset_cause; p[10] = b->boot_count;
  p[11] = 0;
  p[12] = (uint8_t) ((b->dip_high ? 0x01 : 0) | (b->act.standaard ? 0x02 : 0) |
                     (NOOD_K3_VOLGT ? 0x04 : 0) | 0x08 | 0x10);
  p[13] = TAB_MAX_REGELS;
  return 14;
}

static uint8_t bouw_status(Brug *b, uint8_t *p) {
  HwSnap h;
  memset(&h, 0, sizeof h);
  if (b->snap) b->snap(&h);
  p[0] = b->obs.master_status; p[1] = b->obs.slave_status; p[2] = b->obs.requested_setpoint;
  p[3] = (uint8_t) ((uint16_t) b->obs.room_temp >> 8);     p[4] = (uint8_t) b->obs.room_temp;
  p[5] = (uint8_t) ((uint16_t) b->obs.room_setpoint >> 8); p[6] = (uint8_t) b->obs.room_setpoint;
  p[7] = (uint8_t) ((uint16_t) b->obs.boiler_temp >> 8);   p[8] = (uint8_t) b->obs.boiler_temp;
  p[9] = b->obs.modulation; p[10] = b->obs.fault_code;
  p[11] = (uint8_t) ((b->obs.stat_alive ? 0x01 : 0) | (b->obs.boiler_alive ? 0x02 : 0) |
                     (b->brake_applied ? 0x04 : 0) | (b->pol.valid ? 0x08 : 0) |
                     (b->esp_seen ? 0x10 : 0) | (b->act.standaard ? 0x20 : 0) |
                     ((b->vraag_pending || b->vraag_sent) ? 0x40 : 0) | (b->handshake ? 0x80 : 0));
  p[12] = (uint8_t) ((b->dip_high ? 0x01 : 0) | (b->bridge_on ? 0x02 : 0) | (b->k3_on ? 0x04 : 0) |
                     (b->aanuit ? 0x08 : 0) | ((b->bridge_on && b->nood) ? 0x10 : 0) |
                     ((b->sa_active || b->aanuit) ? 0x20 : 0));
  p[13] = h.edges_stat; p[14] = h.edges_boiler;
  p[15] = (uint8_t) ((h.frames_stat > 15 ? 15 : h.frames_stat) | ((b->parity_fails & 0x0F) << 4));
  b->parity_fails = 0;
  p[16] = h.lines; p[17] = h.rx_err_stat; p[18] = h.rx_err_boiler; p[19] = h.spare;
  p[20] = (uint8_t) ((b->reset_cause & 0x0F) | ((b->boot_count > 15 ? 15 : b->boot_count) << 4));
  p[21] = h.isr_ovr;
  p[22] = (uint8_t) (b->act.crc >> 8); p[23] = (uint8_t) b->act.crc;
  p[24] = b->act.versie; p[25] = b->act.n;
  p[26] = b->cnt_crc; p[27] = h.uart_ovr;
  p[28] = b->cnt_vraag_verlopen; p[29] = b->cnt_ketel_oud;
  p[30] = 0; p[31] = 0;           /* sinds_esp: wordt in brug_tx_next ingevuld */
  p[32] = b->cnt_ev_gemist;
  return 33;
}

uint8_t brug_tx_next(Brug *b, uint32_t now, uint8_t *buf) {
  uint8_t pl[PROTO_MAX_PAYLOAD];
  uint8_t type, len, seq, n;

  if ((int32_t) (now - b->tx_next_ms) < 0) return 0;

  if (b->vraag_pending) {                                   /* 1. VRAAG gaat voor alles */
    type = MT_VRAAG; pl[0] = b->vraag_dir;
    pl[1] = (uint8_t) (b->vraag_frame >> 24); pl[2] = (uint8_t) (b->vraag_frame >> 16);
    pl[3] = (uint8_t) (b->vraag_frame >> 8);  pl[4] = (uint8_t) b->vraag_frame;
    len = 5; seq = b->tx_seq++;
    b->vraag_seq = seq; b->vraag_pending = 0; b->vraag_sent = 1;
  } else if (b->ack_n) {                                    /* 2. ACK */
    type = MT_ACK; pl[0] = b->ack[0].seq; pl[1] = b->ack[0].status; len = 2; seq = b->tx_seq++;
    b->ack[0] = b->ack[1]; b->ack_n--;
  } else if (!b->handshake && (int32_t) (now - b->hello_due_ms) >= 0) {   /* 3. HELLO */
    type = MT_HELLO; len = bouw_hello(b, pl); seq = b->tx_seq++;
    b->hello_due_ms = now + (b->had_esp ? HELLO_EVERY_WEG_MS : HELLO_EVERY_MS);
  } else if ((int32_t) (now - b->status_due_ms) >= 0) {     /* 4. STATUS */
    uint32_t s;
    type = MT_STATUS; len = bouw_status(b, pl); seq = b->tx_seq++;
    s = (uint32_t) (now - b->esp_ms) / 1000UL;
    if (!b->had_esp) s = 65535UL;
    if (s > 65535UL) s = 65535UL;
    pl[30] = (uint8_t) (s >> 8); pl[31] = (uint8_t) s;
    b->status_due_ms = now + STATUS_EVERY_MS;
  } else if (b->ev_tail != b->ev_head) {                    /* 5. EVENT */
    const Ev *e = &b->ev[b->ev_tail];
    type = MT_EVENT; pl[0] = e->dir;
    pl[1] = (uint8_t) (e->frame >> 24); pl[2] = (uint8_t) (e->frame >> 16);
    pl[3] = (uint8_t) (e->frame >> 8);  pl[4] = (uint8_t) e->frame;
    len = 5; seq = b->tx_seq++;
    b->ev_tail = (uint8_t) ((b->ev_tail + 1) & 7);
  } else {
    return 0;
  }
  n = msg_build(buf, type, seq, pl, len);
  b->tx_next_ms = now + ((uint32_t) n * 26UL + 99UL) / 100UL + PROTO_GAP_MS;
  return n;
}
