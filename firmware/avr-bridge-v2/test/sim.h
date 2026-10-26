#ifndef SIM_H
#define SIM_H
/* Simulatieomgeving voor de kern: een nagebootste klok, thermostaat, ketel en ESP. */
#include "../brug.h"
#include "vectors.h"

#ifdef __AVR__
  #define HOSTPRINT(...) ((void) 0)
#else
  #include <stdio.h>
  #define HOSTPRINT(...) printf(__VA_ARGS__)
#endif

extern int  t_pass, t_fail;
extern uint16_t fail_lines[80];
extern uint8_t  nfail_lines;
extern const char *t_sect;

#define CHECK(c) do { if (c) t_pass++; else { t_fail++; if (nfail_lines < 80) fail_lines[nfail_lines++] = __LINE__; \
  HOSTPRINT("  FAIL [%s] regel %d: %s\n", t_sect, __LINE__, #c); } } while (0)

#define MAXLOG 12
typedef struct { uint8_t type, seq, len; uint8_t p[12]; uint32_t t; } MLog;

typedef struct {
  Brug     b;
  uint32_t now;
  uint8_t  dip;
  /* kant van de ESP: leest wat de AVR zendt */
  Parser   rp;
  MLog     log[MAXLOG]; uint8_t nlog, wr;
  uint8_t  tx_start_ok; uint32_t last_tx_start; uint8_t last_tx_n; uint8_t gap_viol, tx_bad;
  /* kant van de AVR: leest wat de ESP zendt */
  Parser   ep; uint8_t esp_seq;
  /* OpenTherm */
  uint32_t t_thermo; uint8_t in_stat; uint32_t in_stat_f;
  struct { uint8_t used; uint32_t at, f; } bq[3];
  uint32_t st_f[6], st_t[6]; uint8_t st_n;
  uint32_t bo_f[6], bo_t[6]; uint8_t bo_n;
  uint8_t  bo_silent, bo_wrong_first; uint16_t bo_delay;
  uint8_t  inv_viol;
} Sim;

void     sim_init(Sim *S, uint32_t start, uint8_t dip);
void     sim_step(Sim *S, uint32_t dt);
void     sim_run(Sim *S, uint32_t ms, uint32_t step);
void     thermo(Sim *S, uint32_t frame);
uint32_t reply_for(uint32_t sent);
void     boiler_at(Sim *S, uint32_t at, uint32_t f);
void     esp_raw(Sim *S, const uint8_t *m, uint8_t n);
void     esp_send(Sim *S, uint8_t type, const uint8_t *p, uint8_t len);
MLog    *lastlog(Sim *S, uint8_t type);
uint8_t  count_log(Sim *S, uint8_t type);
uint8_t  mk_tabel(uint8_t *out, uint8_t versie, uint16_t dl, uint16_t kw, uint16_t ew, uint16_t nd, uint16_t hd,
                  const uint8_t r[][5], uint8_t n);
uint8_t  mk_cmd(uint8_t *p, uint8_t f1, uint8_t f2, uint8_t brake_sp, int16_t ws, int16_t wr, int16_t ov,
                uint8_t lamp, uint8_t samod, uint8_t tout, uint8_t bsp, uint8_t maxmod, uint8_t geldig);
uint8_t  vec(uint8_t *dst, const uint8_t *pgm, uint8_t n);
#define LOADV(dst, name) vec(dst, name, (uint8_t) sizeof(name))
#endif
