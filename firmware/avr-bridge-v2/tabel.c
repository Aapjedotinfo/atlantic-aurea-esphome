#include "tabel.h"
#include "proto.h"

/* Bouwoptie: DHW-enable aan in regel 1 van de standaardtabel (zie README en PROTOCOL.md 11.7). */
#ifndef DHW_AAN_STANDAARD
#define DHW_AAN_STANDAARD 1
#endif

/* De ingebouwde standaardtabel: wat het origineel doet bij een geremde ketel. Zie PROTOCOL.md 5.3. */
static const uint8_t def_regels[][5] PROGMEM = {
  {   0, DIR_TK, 0x35, 0x01, DHW_AAN_STANDAARD ? 0x02 : 0x00 }, /* VAST masker, conditie rem of ch_off */
  {   1, DIR_TK, 0x05, 0xFF, 0xFF },                            /* rem-setpoint */
  {  16, DIR_TK, 0x45, 0x08, 0x00 },                            /* 8,0 C, ook bij leesvraag */
  {  14, DIR_TK, 0x19, 0xFF, 0xFB },                            /* max. modulatie (alleen als actief) */
  {   1, DIR_KT, 0x85, 0x00, 0x00 },                            /* ACK-echo */
  {  16, DIR_KT, 0x85, 0x00, 0x00 },
  {  25, DIR_KT, 0x09, 0xFF, 0xFE },                            /* aanvoer WP (bij substitutie) */
  {  28, DIR_KT, 0x09, 0xFF, 0xFD },                            /* retour WP */
  {   9, DIR_KT, 0x4D, 0xFF, 0xFC },                            /* override-setpoint */
  { 100, DIR_KT, 0x4D, 0x00, 0x00 },
};
#define N_DEF ((uint8_t) (sizeof def_regels / sizeof def_regels[0]))

uint8_t regel_ok(const Regel *r) {
  const uint8_t mode = R_MODE(r->modus), cond = R_COND(r->modus), fl = (uint8_t) (r->modus & 0xE0);
  if (r->richting > 1 || cond > COND_MAXMOD) return 0;
  switch (mode) {
    case MODE_PASS:
      return (uint8_t) (r->modus == 0 && r->p0 == 0 && r->p1 == 0);
    case MODE_VAST:
      if (fl & FL_MASKER) {
        if (r->richting != DIR_TK || (fl & (FL_FORCEER | FL_ECHO))) return 0;
      } else if (fl & FL_ECHO) {
        if (r->richting != DIR_KT || r->p0 || r->p1) return 0;
      }
      return 1;
    case MODE_VRAAG:
      return (uint8_t) (fl == 0 && r->p0 <= 150 && r->p1 <= 1);
    default: /* BLOKKEER */
      return (uint8_t) (fl == 0 && r->p0 == 0 && (r->p1 == 0 || r->p1 == 6 || r->p1 == 7));
  }
}

uint8_t tabel_body(const Tabel *t, uint8_t *out) {
  uint8_t i, k = 0;
  out[k++] = t->versie;
  out[k++] = (uint8_t) (t->deadline_ms >> 8); out[k++] = (uint8_t) t->deadline_ms;
  out[k++] = (uint8_t) (t->ketel_ms >> 8);    out[k++] = (uint8_t) t->ketel_ms;
  out[k++] = (uint8_t) (t->esp_weg_s >> 8);   out[k++] = (uint8_t) t->esp_weg_s;
  out[k++] = (uint8_t) (t->nood_s >> 8);      out[k++] = (uint8_t) t->nood_s;
  out[k++] = (uint8_t) (t->rem_hold_s >> 8);  out[k++] = (uint8_t) t->rem_hold_s;
  out[k++] = t->n;
  for (i = 0; i < t->n; i++) {
    out[k++] = t->r[i].id; out[k++] = t->r[i].richting; out[k++] = t->r[i].modus;
    out[k++] = t->r[i].p0; out[k++] = t->r[i].p1;
  }
  return k;
}

uint16_t tabel_bereken_crc(const Tabel *t) {
  uint8_t b[TAB_HDR_LEN + 5 * TAB_MAX_REGELS];
  return crc16(b, tabel_body(t, b));
}

static uint16_t be16(const uint8_t *p) { return (uint16_t) (((uint16_t) p[0] << 8) | p[1]); }

uint8_t tabel_parse(Tabel *t, const uint8_t *p, uint8_t len) {
  Tabel x;
  uint8_t i, j, n;
  if (len < TAB_HDR_LEN + 2) return ST_LENGTE;
  n = p[11];
  if (n > TAB_MAX_REGELS) return ST_GRENS;
  if (len != (uint8_t) (TAB_HDR_LEN + 5 * n + 2)) return ST_LENGTE;
  if (be16(p + TAB_HDR_LEN + 5 * n) != crc16(p, (uint16_t) (TAB_HDR_LEN + 5 * n))) return ST_GRENS;

  x.versie      = p[0];
  x.deadline_ms = be16(p + 1);
  x.ketel_ms    = be16(p + 3);
  x.esp_weg_s   = be16(p + 5);
  x.nood_s      = be16(p + 7);
  x.rem_hold_s  = be16(p + 9);
  x.n           = n;
  if (x.deadline_ms < LIM_DEADLINE_MIN_MS || x.deadline_ms > LIM_DEADLINE_MAX_MS) return ST_GRENS;
  if (x.ketel_ms < LIM_KETEL_MIN_MS || x.ketel_ms > LIM_KETEL_MAX_MS) return ST_GRENS;
  if (x.esp_weg_s < LIM_ESPWEG_MIN_S || x.esp_weg_s > LIM_ESPWEG_MAX_S) return ST_GRENS;
  if (x.nood_s < LIM_NOOD_MIN_S || x.nood_s > LIM_NOOD_MAX_S || x.nood_s < x.esp_weg_s) return ST_GRENS;
  if (x.rem_hold_s < LIM_HOLD_MIN_S || x.rem_hold_s > LIM_HOLD_MAX_S) return ST_GRENS;

  for (i = 0; i < n; i++) {
    const uint8_t *q = p + TAB_HDR_LEN + 5 * i;
    x.r[i].id = q[0]; x.r[i].richting = q[1]; x.r[i].modus = q[2]; x.r[i].p0 = q[3]; x.r[i].p1 = q[4];
    if (!regel_ok(&x.r[i])) return ST_GRENS;
    for (j = 0; j < i; j++)
      if (x.r[j].id == x.r[i].id && x.r[j].richting == x.r[i].richting) return ST_GRENS;
  }
  x.crc = be16(p + TAB_HDR_LEN + 5 * n);
  x.standaard = 0;
  *t = x;           /* pas nu, in een keer, overnemen */
  return ST_OK;
}

void tabel_standaard(Tabel *t) {
  uint8_t i, j;
  memset(t, 0, sizeof *t);
  t->versie = 0;
  t->deadline_ms = DEF_DEADLINE_MS; t->ketel_ms = DEF_KETEL_MS;
  t->esp_weg_s = DEF_ESPWEG_S; t->nood_s = DEF_NOOD_S; t->rem_hold_s = DEF_HOLD_S;
  t->n = N_DEF;
  for (i = 0; i < N_DEF; i++) {
    uint8_t v[5];
    for (j = 0; j < 5; j++) v[j] = RD8(&def_regels[i][j]);
    t->r[i].id = v[0]; t->r[i].richting = v[1]; t->r[i].modus = v[2]; t->r[i].p0 = v[3]; t->r[i].p1 = v[4];
  }
  t->crc = tabel_bereken_crc(t);
  t->standaard = 1;
}

const Regel *tabel_zoek(const Tabel *t, uint8_t id, uint8_t richting) {
  uint8_t i;
  for (i = 0; i < t->n; i++)
    if (t->r[i].id == id && t->r[i].richting == richting) return &t->r[i];
  return 0;
}
