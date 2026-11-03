#include "sim.h"

int t_pass, t_fail;
uint16_t fail_lines[80];
uint8_t  nfail_lines;
const char *t_sect = "";

uint8_t vec(uint8_t *dst, const uint8_t *pgm, uint8_t n) {
  uint8_t i;
  for (i = 0; i < n; i++) dst[i] = RD8(pgm + i);
  return n;
}

static void snap_stub(HwSnap *h) {
  memset(h, 0, sizeof *h);
  h->edges_stat = 40; h->edges_boiler = 38; h->frames_stat = 5; h->lines = 0x0F; h->spare = 0x07;
}

void sim_init(Sim *S, uint32_t start, uint8_t dip) {
  static const uint8_t bouw[3] = { 26, 10, 26 };
  memset(S, 0, sizeof *S);
  S->now = start; S->dip = dip;
  brug_init(&S->b, start, 0x01, 0, dip, bouw, snap_stub);
  parser_init(&S->rp, start);
  parser_init(&S->ep, start);
  S->bo_delay = 60;
}

uint32_t reply_for(uint32_t sent) {
  const uint8_t id = ot_id(sent);
  if (ot_type(sent) == MSG_WRITE_DATA) return ot_make(MSG_WRITE_ACK, id, ot_val(sent));
  return ot_make(MSG_READ_ACK, id, (uint16_t) (0x1400 + id));
}

void boiler_at(Sim *S, uint32_t at, uint32_t f) {
  uint8_t i;
  for (i = 0; i < 3; i++)
    if (!S->bq[i].used) { S->bq[i].used = 1; S->bq[i].at = at; S->bq[i].f = f; return; }
}

void thermo(Sim *S, uint32_t frame) {
  S->in_stat = 1; S->in_stat_f = frame; S->t_thermo = S->now + 1; S->st_n = 0; S->bo_n = 0;
}

static void log_msg(Sim *S) {
  MLog *m = &S->log[S->wr];
  uint8_t i;
  m->type = S->rp.type; m->seq = S->rp.seq; m->len = S->rp.len; m->t = S->now;
  for (i = 0; i < 12 && i < S->rp.len; i++) m->p[i] = S->rp.buf[i];
  S->wr = (uint8_t) ((S->wr + 1) % MAXLOG);
  if (S->nlog < MAXLOG) S->nlog++;
}

MLog *lastlog(Sim *S, uint8_t type) {
  uint8_t i, n = S->nlog > MAXLOG ? MAXLOG : S->nlog;
  for (i = 1; i <= n; i++) {
    MLog *m = &S->log[(uint8_t) ((S->wr + MAXLOG - i) % MAXLOG)];
    if (m->type == type) return m;
  }
  return 0;
}

uint8_t count_log(Sim *S, uint8_t type) {
  uint8_t i, c = 0, n = S->nlog > MAXLOG ? MAXLOG : S->nlog;
  for (i = 0; i < n; i++) if (S->log[i].type == type) c++;
  return c;
}

void sim_step(Sim *S, uint32_t dt) {
  OtIn in; OtOut out;
  uint8_t buf[PROTO_MAX_MSG], n, i;
  S->now += dt;
  memset(&in, 0, sizeof in);
  if (S->in_stat) { in.have_stat = 1; in.stat = S->in_stat_f; S->in_stat = 0; }
  for (i = 0; i < 3; i++)
    if (S->bq[i].used && (int32_t) (S->now - S->bq[i].at) >= 0) {
      in.have_boiler = 1; in.boiler = S->bq[i].f; S->bq[i].used = 0; break;
    }
  brug_ot(&S->b, S->now, &in, &out);
  if (out.send_boiler) {
    if (S->bo_n < 6) { S->bo_f[S->bo_n] = out.boiler_frame; S->bo_t[S->bo_n] = S->now; S->bo_n++; }
    if (!S->bo_silent) {
      if (S->bo_wrong_first)
        boiler_at(S, S->now + 20, ot_make(MSG_READ_ACK, (uint8_t) (ot_id(out.boiler_frame) ^ 0x40), 0x1111));
      boiler_at(S, S->now + S->bo_delay, reply_for(out.boiler_frame));
    }
  }
  if (out.send_stat) {
    if (S->st_n < 6) { S->st_f[S->st_n] = out.stat_frame; S->st_t[S->st_n] = S->now; S->st_n++; }
  }
  brug_tick(&S->b, S->now, S->dip);
  if (S->b.bridge_on && S->b.k3_on) S->inv_viol++;

  n = brug_tx_next(&S->b, S->now, buf);
  if (n) {
    uint8_t got = 0;
    /* pauze van minstens 5 ms na het vorige bericht */
    if (S->last_tx_n && (uint32_t) (S->now - S->last_tx_start) < ((uint32_t) S->last_tx_n * 26UL + 99UL) / 100UL + 5UL)
      S->gap_viol++;
    S->last_tx_start = S->now; S->last_tx_n = n;
    for (i = 0; i < n; i++)
      if (parser_feed(&S->rp, buf[i], S->now)) { log_msg(S); got = 1; }
    if (!got) S->tx_bad++;
  }
}

void sim_run(Sim *S, uint32_t ms, uint32_t step) {
  uint32_t t;
  for (t = 0; t < ms; t += step) sim_step(S, step);
}

void esp_raw(Sim *S, const uint8_t *m, uint8_t n) {
  uint8_t i;
  for (i = 0; i < n; i++)
    if (parser_feed(&S->ep, m[i], S->now))
      brug_rx_msg(&S->b, S->now, S->ep.type, S->ep.seq, S->ep.buf, S->ep.len);
}

void esp_send(Sim *S, uint8_t type, const uint8_t *p, uint8_t len) {
  uint8_t m[PROTO_MAX_MSG];
  const uint8_t n = msg_build(m, type, S->esp_seq++, p, len);
  esp_raw(S, m, n);
}

uint8_t mk_tabel(uint8_t *out, uint8_t versie, uint16_t dl, uint16_t kw, uint16_t ew, uint16_t nd, uint16_t hd,
                 const uint8_t r[][5], uint8_t n) {
  uint8_t k = 0, i, j;
  uint16_t c;
  out[k++] = versie;
  out[k++] = (uint8_t) (dl >> 8); out[k++] = (uint8_t) dl;
  out[k++] = (uint8_t) (kw >> 8); out[k++] = (uint8_t) kw;
  out[k++] = (uint8_t) (ew >> 8); out[k++] = (uint8_t) ew;
  out[k++] = (uint8_t) (nd >> 8); out[k++] = (uint8_t) nd;
  out[k++] = (uint8_t) (hd >> 8); out[k++] = (uint8_t) hd;
  out[k++] = n;
  for (i = 0; i < n; i++) for (j = 0; j < 5; j++) out[k++] = r[i][j];
  c = crc16(out, k);
  out[k++] = (uint8_t) (c >> 8); out[k++] = (uint8_t) c;
  return k;
}

uint8_t mk_cmd(uint8_t *p, uint8_t f1, uint8_t f2, uint8_t brake_sp, int16_t ws, int16_t wr, int16_t ov,
               uint8_t lamp, uint8_t samod, uint8_t tout, uint8_t bsp, uint8_t maxmod, uint8_t geldig) {
  p[0] = f1; p[1] = f2; p[2] = brake_sp;
  p[3] = (uint8_t) ((uint16_t) ws >> 8); p[4] = (uint8_t) ws;
  p[5] = (uint8_t) ((uint16_t) wr >> 8); p[6] = (uint8_t) wr;
  p[7] = (uint8_t) ((uint16_t) ov >> 8); p[8] = (uint8_t) ov;
  p[9] = lamp; p[10] = samod; p[11] = tout; p[12] = bsp; p[13] = maxmod; p[14] = geldig;
  return 15;
}
