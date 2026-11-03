#ifndef TABEL_H
#define TABEL_H
/* De regeltabel per Data-ID en richting (PROTOCOL.md hoofdstuk 5). */
#include "platform.h"
KERN_BEGIN

#define TAB_MAX_REGELS   16
#define TAB_HDR_LEN      12   /* versie + 5 tijden + n */

/* grenzen (hoofdstuk 8) */
#define LIM_DEADLINE_MIN_MS   30
#define LIM_DEADLINE_MAX_MS   600
#define LIM_KETEL_MIN_MS      100
#define LIM_KETEL_MAX_MS      600
#define LIM_ESPWEG_MIN_S      10
#define LIM_ESPWEG_MAX_S      600
#define LIM_NOOD_MIN_S        60
#define LIM_NOOD_MAX_S        1800
#define LIM_HOLD_MIN_S        60
#define LIM_HOLD_MAX_S        900

#define DEF_DEADLINE_MS  250
#define DEF_KETEL_MS     400
#define DEF_ESPWEG_S     60
#define DEF_NOOD_S       240
#define DEF_HOLD_S       300

/* richting */
#define DIR_TK 0
#define DIR_KT 1
/* modus, bits 1..0 */
#define MODE_PASS     0
#define MODE_VAST     1
#define MODE_VRAAG    2
#define MODE_BLOKKEER 3
/* conditie, bits 4..2 */
#define COND_ALTIJD       0
#define COND_REM          1
#define COND_SUBST        2
#define COND_OVERRIDE     3
#define COND_CHOFF        4
#define COND_REM_OF_CHOFF 5
#define COND_MAXMOD       6
/* vlaggen */
#define FL_MASKER   0x20
#define FL_FORCEER  0x40
#define FL_ECHO     0x80
/* bijzondere waarden bij VAST (waardevorm) */
#define VAL_REMSP    0xFFFF
#define VAL_AANVOER  0xFFFE
#define VAL_RETOUR   0xFFFD
#define VAL_OVERRIDE 0xFFFC
#define VAL_MAXMOD   0xFFFB

#define R_MODE(m)  ((uint8_t) ((m) & 3))
#define R_COND(m)  ((uint8_t) (((m) >> 2) & 7))

typedef struct { uint8_t id, richting, modus, p0, p1; } Regel;

typedef struct {
  uint8_t  versie;
  uint16_t deadline_ms, ketel_ms, esp_weg_s, nood_s, rem_hold_s;
  uint8_t  n;
  Regel    r[TAB_MAX_REGELS];
  uint16_t crc;
  uint8_t  standaard;     /* 1 = de ingebouwde standaardtabel */
} Tabel;

/* ACK-statussen */
#define ST_OK        0
#define ST_LENGTE    1
#define ST_GRENS     2
#define ST_BEZET     4

uint8_t  regel_ok(const Regel *r);
/* Serialiseert de tabel (zonder crc) naar out: 12 + 5n bytes. Geeft de lengte. */
uint8_t  tabel_body(const Tabel *t, uint8_t *out);
uint16_t tabel_bereken_crc(const Tabel *t);
/* Leest een TABEL-payload; vult *t alleen als alles klopt. Geeft ST_OK, ST_LENGTE of ST_GRENS. */
uint8_t  tabel_parse(Tabel *t, const uint8_t *p, uint8_t len);
void     tabel_standaard(Tabel *t);
/* Zoekt (id, richting). NULL als er geen regel is. */
const Regel *tabel_zoek(const Tabel *t, uint8_t id, uint8_t richting);

KERN_END
#endif
