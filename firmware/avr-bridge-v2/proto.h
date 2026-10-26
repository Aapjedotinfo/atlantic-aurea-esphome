#ifndef PROTO_H
#define PROTO_H
/* Berichtkader en parser van het AVR-ESP-protocol (PROTOCOL.md hoofdstuk 2 en 3). */
#include "platform.h"
KERN_BEGIN

#define PROTO_SYNC         0xA5
#define PROTO_MAX_PAYLOAD  96
#define PROTO_MAX_MSG      (PROTO_MAX_PAYLOAD + 5)
#define PROTO_GAP_MS       5UL     /* minimale stilte tussen berichten, en na een fout */
#define PROTO_BYTE_TIMEOUT_MS 5UL  /* een pauze van zoveel ms of meer midden in een bericht breekt het af */

enum {
  MT_HELLO = 1, MT_TABEL = 2, MT_ACK = 3, MT_VRAAG = 4, MT_ANTWOORD = 5,
  MT_STATUS = 6, MT_COMMANDO = 7, MT_EVENT = 8, MT_INJECT = 9
};

uint8_t  crc8_update(uint8_t crc, uint8_t b);
uint8_t  crc8(const uint8_t *p, uint16_t n);
uint16_t crc16(const uint8_t *p, uint16_t n);

/* Bouwt een volledig bericht in out (minimaal len + 5 bytes). Geeft de totale lengte. */
uint8_t  msg_build(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len);

enum { PS_IDLE = 0, PS_TYPE, PS_LEN, PS_SEQ, PS_PAYLOAD, PS_CRC, PS_HUNT };

typedef struct {
  uint8_t  state;
  uint8_t  type, len, seq, idx, crc;
  uint8_t  buf[PROTO_MAX_PAYLOAD];
  uint32_t last_ms;          /* tijdstip van de vorige byte */
  uint8_t  err_crc, err_len, err_timeout;   /* verzadigende tellers */
} Parser;

void    parser_init(Parser *p, uint32_t now);
/* Voert een byte in. Geeft 1 als er nu een compleet, geldig bericht klaarstaat in
 * p->type / p->seq / p->len / p->buf (geldig tot de volgende aanroep). */
uint8_t parser_feed(Parser *p, uint8_t b, uint32_t now);

KERN_END
#endif
