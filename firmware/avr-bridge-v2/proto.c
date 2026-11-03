#include "proto.h"

uint8_t crc8_update(uint8_t crc, uint8_t b) {
  uint8_t i;
  crc ^= b;
  for (i = 0; i < 8; i++)
    crc = (uint8_t) ((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
  return crc;
}

uint8_t crc8(const uint8_t *p, uint16_t n) {
  uint8_t c = 0;
  while (n--) c = crc8_update(c, *p++);
  return c;
}

uint16_t crc16(const uint8_t *p, uint16_t n) {
  uint16_t c = 0xFFFF;
  uint8_t i;
  while (n--) {
    c ^= (uint16_t) ((uint16_t) *p++ << 8);
    for (i = 0; i < 8; i++)
      c = (uint16_t) ((c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1));
  }
  return c;
}

uint8_t msg_build(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len) {
  uint8_t i;
  out[0] = PROTO_SYNC;
  out[1] = type;
  out[2] = len;
  out[3] = seq;
  for (i = 0; i < len; i++) out[4 + i] = payload[i];
  out[4 + len] = crc8(out + 1, (uint16_t) (3 + len));
  return (uint8_t) (len + 5);
}

void parser_init(Parser *p, uint32_t now) {
  memset(p, 0, sizeof *p);
  p->state = PS_IDLE;
  p->last_ms = now;
}

static void sat_inc(uint8_t *c) { if (*c < 255) (*c)++; }

uint8_t parser_feed(Parser *p, uint8_t b, uint32_t now) {
  const uint32_t gap = now - p->last_ms;       /* modulo 2^32: werkt over de millis-overloop */
  p->last_ms = now;

  /* Een lange stilte midden in een bericht: afbreken, en deze byte telt als nieuw begin. */
  if (p->state >= PS_TYPE && p->state <= PS_CRC && gap >= PROTO_BYTE_TIMEOUT_MS) {
    sat_inc(&p->err_timeout);
    p->state = PS_IDLE;
  }
  /* Na een fout: pas weer zoeken na minstens 5 ms stilte. */
  if (p->state == PS_HUNT) {
    if (gap >= PROTO_GAP_MS) p->state = PS_IDLE;
    else return 0;
  }

  switch (p->state) {
    case PS_IDLE:
      if (b == PROTO_SYNC) { p->state = PS_TYPE; p->crc = 0; }
      return 0;
    case PS_TYPE:
      p->type = b; p->crc = crc8_update(0, b); p->state = PS_LEN;
      return 0;
    case PS_LEN:
      if (b > PROTO_MAX_PAYLOAD) { sat_inc(&p->err_len); p->state = PS_HUNT; return 0; }
      p->len = b; p->crc = crc8_update(p->crc, b); p->state = PS_SEQ;
      return 0;
    case PS_SEQ:
      p->seq = b; p->crc = crc8_update(p->crc, b); p->idx = 0;
      p->state = (p->len == 0) ? PS_CRC : PS_PAYLOAD;
      return 0;
    case PS_PAYLOAD:
      p->buf[p->idx++] = b; p->crc = crc8_update(p->crc, b);
      if (p->idx >= p->len) p->state = PS_CRC;
      return 0;
    case PS_CRC:
      if (b == p->crc) { p->state = PS_IDLE; return 1; }
      sat_inc(&p->err_crc);
      p->state = PS_HUNT;
      return 0;
    default:
      p->state = PS_IDLE;
      return 0;
  }
}
