/* test_kern.c - tests van de hardware-onafhankelijke kern (proto.c, tabel.c, brug.c).
 *
 * Twee manieren om te draaien (zie bouw_test_pc.bat en bouw_test_avr.sh):
 *   1. native op de PC (MSVC cl.exe; mingw-gcc is hier niet geinstalleerd);
 *   2. gecompileerd met avr-gcc voor de atmega328p en gedraaid in de AVR-simulator van
 *      avr-gdb, zodat int=16 bit, 8-bits rekenen en de UL-constanten echt getest zijn.
 * Beide draaien dezelfde bron. */
#include "sim.h"

#define F(t, id, v) ot_make((t), (id), (v))
#define STEP 1
volatile uint8_t t_done;

/* ---------- hulpjes ---------- */
volatile uint8_t t_prog;
static void sect(const char *s) { t_prog++; t_sect = s; HOSTPRINT("[%s]\n", s); }

static Sim S;           /* een tegelijk; de AVR-simulator heeft maar 2 kB RAM */
static uint8_t bufA[PROTO_MAX_MSG + 4];
static uint8_t bufB[PROTO_MAX_MSG + 4];

static void cmd(uint8_t f1, uint8_t f2, uint8_t bsp, int16_t ws, int16_t wr, int16_t ov, uint8_t maxmod, uint8_t geldig) {
  uint8_t p[15];
  mk_cmd(p, f1, f2, bsp, ws, wr, ov, 0, 0, 0, 0, maxmod, geldig);
  esp_send(&S, MT_COMMANDO, p, 15);
}
#define C1_REM 0x01
#define C1_SUBST 0x02
#define C1_BRUG 0x10
#define C2_CHOFF 0x08
#define C2_DEMAND 0x10
#define C2_DIRECT 0x20
#define C2_KT_OT (1 << 6)
#define C2_KT_AU (2 << 6)

static const uint8_t R0[0][5] = { {0,0,0,0,0} };   /* leeg */
static void install(const uint8_t *pl, uint8_t n) {
  esp_send(&S, MT_TABEL, pl, n);
  sim_run(&S, 20, 1);
}
static void install_std_tabel(void) {
  uint8_t n = LOADV(bufA, ref_tabel_default);
  esp_raw(&S, bufA, n);
  sim_run(&S, 20, 1);
}

/* een transactie van de thermostaat; geeft de responstijd in ms (0 = geen antwoord) */
static uint32_t transactie(uint32_t f, uint32_t ms) {
  thermo(&S, f);
  sim_run(&S, ms, STEP);
  return S.st_n ? (uint32_t) (S.st_t[0] - S.t_thermo) : 0;
}

/* ---------- 1. CRC en vectoren (onafhankelijk uit Python) ---------- */
static void test_crc_en_vectoren(void) {
  uint8_t n, i;
  static const uint8_t cs[9] = { '1','2','3','4','5','6','7','8','9' };
  sect("CRC en referentievectoren");
  CHECK(crc8(cs, 9) == REF_CRC8_CHECK);
  CHECK(crc16(cs, 9) == REF_CRC16_CHECK);

  { Tabel t; uint8_t body[TAB_HDR_LEN + 5 * TAB_MAX_REGELS];
    tabel_standaard(&t);
    CHECK(t.crc == REF_DEFAULT_TABEL_CRC);
    n = tabel_body(&t, body);
    CHECK(n == sizeof ref_default_body);
    vec(bufA, ref_default_body, n);
    CHECK(memcmp(body, bufA, n) == 0);
  }

  /* de C-encoder moet byte voor byte hetzelfde geven als de Python-encoder */
  sim_init(&S, 0, 0);
  S.b.tx_seq = 0x10;
  sim_run(&S, 100, 1);                                    /* HELLO is op 100 ms aan de beurt */
  { MLog *m = lastlog(&S, MT_HELLO);
    CHECK(m != 0);
    if (m) { CHECK(m->seq == 0x10 && m->len == 14); } }
  /* herbouw HELLO rechtstreeks voor een volledige vergelijking */
  sim_init(&S, 0, 0); S.b.tx_seq = 0x10; S.b.hello_due_ms = 0; S.now = 0;
  n = brug_tx_next(&S.b, 0, bufB);
  CHECK(n == sizeof ref_hello);
  vec(bufA, ref_hello, sizeof ref_hello);
  CHECK(memcmp(bufA, bufB, n) == 0);

  sim_init(&S, 0, 0); S.b.tx_seq = 0x11; S.b.ack[0].seq = 0x21; S.b.ack[0].status = 0; S.b.ack_n = 1;
  n = brug_tx_next(&S.b, 0, bufB);
  vec(bufA, ref_ack, sizeof ref_ack);
  CHECK(n == sizeof ref_ack && memcmp(bufA, bufB, n) == 0);

  sim_init(&S, 0, 0); S.b.tx_seq = 0x13; S.b.vraag_pending = 1; S.b.vraag_dir = DIR_TK; S.b.vraag_frame = REF_FRAME_W1_45;
  n = brug_tx_next(&S.b, 0, bufB);
  vec(bufA, ref_vraag, sizeof ref_vraag);
  CHECK(n == sizeof ref_vraag && memcmp(bufA, bufB, n) == 0);
  CHECK(S.b.vraag_seq == 0x13 && S.b.vraag_sent == 1);

  sim_init(&S, 0, 0); S.b.tx_seq = 0x15; S.b.ev[0].dir = 0; S.b.ev[0].frame = REF_FRAME_W1_45; S.b.ev_head = 1;
  S.b.hello_due_ms = 100000UL; S.b.status_due_ms = 100000UL;
  n = brug_tx_next(&S.b, 0, bufB);
  vec(bufA, ref_event, sizeof ref_event);
  CHECK(n == sizeof ref_event && memcmp(bufA, bufB, n) == 0);

  /* STATUS: dezelfde toestand als de Python-voorbeeldwaarden */
  sim_init(&S, 100000UL, 1);
  S.b.tx_seq = 0x14; S.b.hello_due_ms = 0xFFFFFFF0UL; S.b.handshake = 1;
  S.b.obs.master_status = 0x03; S.b.obs.slave_status = 0x0A; S.b.obs.requested_setpoint = 45;
  S.b.obs.room_temp = 205; S.b.obs.room_setpoint = 210; S.b.obs.boiler_temp = 355;
  S.b.obs.modulation = 30; S.b.obs.fault_code = 0; S.b.obs.stat_alive = 1; S.b.obs.boiler_alive = 1;
  S.b.brake_applied = 1; S.b.pol.valid = 1; S.b.esp_seen = 1; S.b.had_esp = 1; S.b.handshake = 0;
  S.b.act.standaard = 0; S.b.act.crc = 0x1234; S.b.act.versie = 1; S.b.act.n = 10;
  S.b.dip_high = 1; S.b.bridge_on = 1; S.b.aanuit = 0; S.b.nood = 0;
  S.b.esp_ms = 100000UL - 3000UL; S.b.status_due_ms = 0;
  n = brug_tx_next(&S.b, 100000UL, bufB);
  vec(bufA, ref_status, sizeof ref_status);
  CHECK(n == sizeof ref_status);
  CHECK(memcmp(bufA, bufB, n) == 0);
  if (memcmp(bufA, bufB, n)) for (i = 0; i < n; i++) if (bufA[i] != bufB[i]) HOSTPRINT("   byte %d: ref %02X c %02X\n", i, bufA[i], bufB[i]);
}

/* ---------- 2. parser ---------- */
static uint8_t feed_all(Parser *p, const uint8_t *m, uint8_t n, uint32_t *t) {
  uint8_t i, got = 0;
  for (i = 0; i < n; i++) { if (parser_feed(p, m[i], *t)) got++; }
  return got;
}

static void test_parser(void) {
  Parser p; uint32_t t = 1000; uint8_t n, i, k, got;
  sect("parser, resync, beschadigde berichten");
  parser_init(&p, t);
  /* alle referentieberichten worden geaccepteerd, met 5 ms ertussen */
#define ACCEPT(name, typ) n = LOADV(bufA, name); t += 6; got = feed_all(&p, bufA, n, &t); CHECK(got == 1 && p.type == (typ))
  ACCEPT(ref_hello, MT_HELLO);   ACCEPT(ref_tabel_default, MT_TABEL); ACCEPT(ref_tabel_klein, MT_TABEL);
  ACCEPT(ref_ack, MT_ACK);       ACCEPT(ref_vraag, MT_VRAAG);         ACCEPT(ref_antwoord_door, MT_ANTWOORD);
  ACCEPT(ref_antwoord_verv, MT_ANTWOORD); ACCEPT(ref_antwoord_blok, MT_ANTWOORD); ACCEPT(ref_status, MT_STATUS);
  ACCEPT(ref_commando_rem, MT_COMMANDO);  ACCEPT(ref_commando_vrij, MT_COMMANDO); ACCEPT(ref_event, MT_EVENT);
  ACCEPT(ref_inject_ketel, MT_INJECT);    ACCEPT(ref_inject_ovr, MT_INJECT);      ACCEPT(ref_inject_ovr_weg, MT_INJECT);
  CHECK(p.err_crc == 0 && p.err_len == 0 && p.err_timeout == 0);

  /* direct achter elkaar (zonder pauze) mag ook, na een goed bericht */
  n = LOADV(bufA, ref_ack); t += 6;
  got = feed_all(&p, bufA, n, &t); got += feed_all(&p, bufA, n, &t);
  CHECK(got == 2);

  /* elk enkel beschadigd byte van een TABEL en een COMMANDO: nooit geaccepteerd,
   * en het daaropvolgende goede bericht (na 6 ms) komt wel door */
  { uint8_t bad = 0, lost = 0;
    n = LOADV(bufA, ref_tabel_default);
    for (i = 0; i < n; i++) {
      for (k = 0; k < 2; k++) {
        bufA[i] ^= (uint8_t) (k ? 0x80 : 0x01);
        parser_init(&p, t);
        t += 6; got = feed_all(&p, bufA, n, &t);
        if (got) bad++;
        { uint8_t m2 = LOADV(bufB, ref_ack); t += 6; if (feed_all(&p, bufB, m2, &t) != 1) lost++; }
        bufA[i] ^= (uint8_t) (k ? 0x80 : 0x01);
      }
    }
    CHECK(bad == 0);
    CHECK(lost == 0);
  }

  /* een afgebroken bericht (helft), dan 6 ms stilte, dan een goed bericht */
  parser_init(&p, t);
  n = LOADV(bufA, ref_commando_vrij);
  t += 6; feed_all(&p, bufA, (uint8_t) (n / 2), &t);
  { uint8_t m2 = LOADV(bufB, ref_vraag); t += 6; CHECK(feed_all(&p, bufB, m2, &t) == 1 && p.type == MT_VRAAG); }

  /* een 0xA5 in de payload: geen vals begin; payload met veel A5 */
  { uint8_t pl[8] = { 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5 };
    uint8_t m[20];
    n = msg_build(m, MT_EVENT, 7, pl, 8);
    parser_init(&p, t); t += 6;
    CHECK(feed_all(&p, m, n, &t) == 1 && p.len == 8 && p.buf[7] == 0xA5);
    /* corrupt, daarna meteen een goed bericht zonder pauze: geweigerd (resync-regel) ... */
    m[n - 1] ^= 0xFF; t += 6; CHECK(feed_all(&p, m, n, &t) == 0);
    n = msg_build(m, MT_ACK, 1, pl, 2);
    CHECK(feed_all(&p, m, n, &t) == 0);               /* binnen 5 ms na de fout: nog niet */
    t += 6; CHECK(feed_all(&p, m, n, &t) == 1);       /* na 5 ms stilte: weer synchroon */
  }

  /* te grote lengte */
  { uint8_t m[8] = { PROTO_SYNC, MT_EVENT, 97, 0, 0, 0, 0, 0 };
    parser_init(&p, t); t += 6; feed_all(&p, m, 8, &t); CHECK(p.err_len == 1);
    n = LOADV(bufA, ref_ack); CHECK(feed_all(&p, bufA, n, &t) == 0);
    t += 6; CHECK(feed_all(&p, bufA, n, &t) == 1);
  }

  /* pauze midden in een bericht (>= 5 ms): afgebroken, het volgende begin telt */
  parser_init(&p, t);
  n = LOADV(bufA, ref_commando_rem); t += 6;
  feed_all(&p, bufA, 6, &t);
  t += 5;
  { uint8_t m2 = LOADV(bufB, ref_ack); CHECK(feed_all(&p, bufB, m2, &t) == 1); CHECK(p.err_timeout == 1); }

  /* klok loopt over (millis-overloop) */
  t = 0xFFFFFFF0UL;
  parser_init(&p, t); n = LOADV(bufA, ref_vraag);
  t += 6; CHECK(feed_all(&p, bufA, n, &t) == 1);
  t += 20; CHECK(feed_all(&p, bufA, n, &t) == 1);

  /* nul-lengte */
  { uint8_t m[8]; n = msg_build(m, 0x7F, 3, 0, 0); parser_init(&p, t); t += 6;
    CHECK(feed_all(&p, m, n, &t) == 1 && p.len == 0 && p.type == 0x7F); }
}

/* ---------- 3. tabel ---------- */
static void test_tabel(void) {
  Tabel t; uint8_t pl[100], n;
  uint8_t std[10][5]; uint8_t i;
  sect("tabel: parse, grenzen, ongeldige regels");

  n = LOADV(bufA, ref_tabel_default);
  CHECK(tabel_parse(&t, bufA + 4, (uint8_t) (n - 5)) == ST_OK);
  CHECK(t.n == 10 && t.crc == REF_DEFAULT_TABEL_CRC && t.versie == 1 && t.deadline_ms == 250 && t.nood_s == 240);
  { Tabel d; tabel_standaard(&d);
    CHECK(d.n == 10 && d.standaard == 1);
    CHECK(tabel_zoek(&d, 0, DIR_TK) != 0 && tabel_zoek(&d, 0, DIR_KT) == 0 && tabel_zoek(&d, 99, DIR_TK) == 0);
    for (i = 0; i < 10; i++) { std[i][0] = d.r[i].id; std[i][1] = d.r[i].richting; std[i][2] = d.r[i].modus; std[i][3] = d.r[i].p0; std[i][4] = d.r[i].p1; }
  }
  /* C-encoder == Python-encoder voor het kleine voorbeeld */
  { static const uint8_t kl[2][5] = { { 27, DIR_KT, 0x02, 25, 0 }, { 5, DIR_TK, 0x03, 0, 7 } };
    uint8_t k = mk_tabel(pl, 2, 200, 400, 60, 240, 300, kl, 2);
    n = LOADV(bufA, ref_tabel_klein);
    CHECK(k == (uint8_t) (n - 5) && memcmp(pl, bufA + 4, k) == 0);
  }

  /* grenzen: min en max geldig, een daarbuiten niet */
#define TRY(dl, kw, ew, nd, hd) (tabel_parse(&t, pl, mk_tabel(pl, 1, (dl), (kw), (ew), (nd), (hd), std, 10)))
  CHECK(TRY(250, 400, 60, 240, 300) == ST_OK);
  CHECK(TRY(30, 400, 60, 240, 300) == ST_OK);   CHECK(TRY(29, 400, 60, 240, 300) == ST_GRENS);
  CHECK(TRY(600, 400, 60, 240, 300) == ST_OK);  CHECK(TRY(601, 400, 60, 240, 300) == ST_GRENS);
  CHECK(TRY(250, 100, 60, 240, 300) == ST_OK);  CHECK(TRY(250, 99, 60, 240, 300) == ST_GRENS);
  CHECK(TRY(250, 600, 60, 240, 300) == ST_OK);  CHECK(TRY(250, 601, 60, 240, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 10, 240, 300) == ST_OK);  CHECK(TRY(250, 400, 9, 240, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 600, 1800, 300) == ST_OK); CHECK(TRY(250, 400, 601, 1800, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 60, 60, 300) == ST_OK);   CHECK(TRY(250, 400, 60, 59, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 60, 1800, 300) == ST_OK); CHECK(TRY(250, 400, 60, 1801, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 120, 119, 300) == ST_GRENS);   /* nood_s < esp_weg_s */
  CHECK(TRY(250, 400, 60, 240, 60) == ST_OK);   CHECK(TRY(250, 400, 60, 240, 59) == ST_GRENS);
  CHECK(TRY(250, 400, 60, 240, 900) == ST_OK);  CHECK(TRY(250, 400, 60, 240, 901) == ST_GRENS);
  /* 16-bit-val: 65535+... mag nooit als geldig overkomen */
  CHECK(TRY(0xFFFF, 400, 60, 240, 300) == ST_GRENS);
  CHECK(TRY(250, 400, 60, 0xFFFF, 300) == ST_GRENS);

  /* een mislukte parse laat de bestaande tabel ongemoeid */
  { Tabel keep; tabel_standaard(&keep); t = keep;
    CHECK(TRY(601, 400, 60, 240, 300) == ST_GRENS);
    CHECK(memcmp(&t, &keep, sizeof t) == 0 || t.crc == keep.crc); }

  /* beschadigde CRC-16 en verkeerde lengte */
  { uint8_t k = mk_tabel(pl, 1, 250, 400, 60, 240, 300, std, 10);
    pl[k - 1] ^= 1; CHECK(tabel_parse(&t, pl, k) == ST_GRENS);
    pl[k - 1] ^= 1; CHECK(tabel_parse(&t, pl, (uint8_t) (k - 1)) == ST_LENGTE);
    CHECK(tabel_parse(&t, pl, 5) == ST_LENGTE);
    pl[2] ^= 0x10;  CHECK(tabel_parse(&t, pl, k) == ST_GRENS);          /* inhoud gewijzigd, CRC niet */
  }

  /* ongeldige regels */
#define BADRULE(a, b, c, d, e) do { uint8_t r_[1][5]; r_[0][0]=(a); r_[0][1]=(b); r_[0][2]=(c); r_[0][3]=(d); r_[0][4]=(e); \
   CHECK(tabel_parse(&t, pl, mk_tabel(pl, 1, 250, 400, 60, 240, 300, r_, 1)) == ST_GRENS); } while (0)
#define GOODRULE(a, b, c, d, e) do { uint8_t r_[1][5]; r_[0][0]=(a); r_[0][1]=(b); r_[0][2]=(c); r_[0][3]=(d); r_[0][4]=(e); \
   CHECK(tabel_parse(&t, pl, mk_tabel(pl, 1, 250, 400, 60, 240, 300, r_, 1)) == ST_OK); } while (0)
  BADRULE(5, 2, 0x00, 0, 0);          /* richting 2 */
  BADRULE(5, 0, 0x1D, 0, 0);          /* conditie 7 */
  BADRULE(5, 0, 0x00, 1, 0);          /* PASS met parameter */
  BADRULE(5, 0, 0x04, 0, 0);          /* PASS met conditie */
  BADRULE(5, 1, 0x21, 1, 1);          /* MASKER K>T */
  BADRULE(5, 0, 0x81, 0, 0);          /* ECHO T>K */
  BADRULE(5, 1, 0x81, 1, 0);          /* ECHO met parameter */
  BADRULE(5, 0, 0x22, 0, 0);          /* vlag bij VRAAG */
  BADRULE(5, 0, 0x02, 151, 0);        /* deadline 604 ms */
  BADRULE(5, 0, 0x02, 0, 2);          /* standaardactie 2 */
  BADRULE(5, 0, 0x03, 0, 5);          /* BLOKKEER-type 5 */
  BADRULE(5, 0, 0x03, 1, 7);          /* BLOKKEER p0 */
  BADRULE(5, 0, 0x61, 1, 1);          /* MASKER+FORCEER */
  GOODRULE(5, 0, 0x02, 150, 1);       /* 600 ms, standaard blokkeer */
  GOODRULE(5, 1, 0x03, 0, 6);
  GOODRULE(5, 1, 0x03, 0, 0);
  GOODRULE(5, 0, 0x21, 0xFF, 0x01);
  GOODRULE(5, 1, 0x81, 0, 0);
  { uint8_t r2[2][5] = { { 5, 0, 0x02, 0, 0 }, { 5, 0, 0x03, 0, 7 } };       /* dubbele (id, richting) */
    CHECK(tabel_parse(&t, pl, mk_tabel(pl, 1, 250, 400, 60, 240, 300, r2, 2)) == ST_GRENS);
    r2[1][1] = 1; CHECK(tabel_parse(&t, pl, mk_tabel(pl, 1, 250, 400, 60, 240, 300, r2, 2)) == ST_OK); }
  /* meer dan 16 regels */
  { uint8_t k = mk_tabel(pl, 1, 250, 400, 60, 240, 300, R0, 0); pl[11] = 17; (void) k;
    CHECK(tabel_parse(&t, pl, 14) == ST_GRENS); }
  /* 16 regels is precies goed */
  { uint8_t r16[16][5]; uint8_t j;
    for (j = 0; j < 16; j++) { r16[j][0] = j; r16[j][1] = 0; r16[j][2] = 0; r16[j][3] = 0; r16[j][4] = 0; }
    CHECK(tabel_parse(&t, pl, mk_tabel(pl, 1, 250, 400, 60, 240, 300, r16, 16)) == ST_OK && t.n == 16); }
}

/* ---------- 4. COMMANDO ---------- */
static void test_commando(void) {
  uint8_t n, p[15];
  sect("COMMANDO: geldig, grenzen, ACK bij fout");
  sim_init(&S, 1000, 0); sim_run(&S, 10, 1);
  n = LOADV(bufA, ref_commando_vrij);
  esp_raw(&S, bufA, n);
  CHECK(S.b.pol.valid == 1 && S.b.pol.brake_on == 0 && S.b.pol.subst == 1 && S.b.pol.wp_supply == 355 &&
        S.b.pol.wp_return == 290 && S.b.pol.geldig_s == 30 && S.b.pol.brake_sp == 10);
  CHECK(S.b.esp_seen == 1);
  sim_run(&S, 10, 1);

  /* buiten de grenzen: niets verandert, ACK status 2 */
  S.now += 10; sim_run(&S, 1, 1);
  n = LOADV(bufA, ref_commando_grens);
  esp_raw(&S, bufA, n);
  CHECK(S.b.pol.brake_sp == 10);
  sim_run(&S, 20, 1);
  { MLog *m = lastlog(&S, MT_ACK);
    CHECK(m && m->p[0] == 0x2D && m->p[1] == ST_GRENS); }

  /* alle bereiken van de losse velden */
  S.b.pol.valid = 0;
#define CMDS(ws, wr, ov, lamp, samod, tout, bsp, mm, gld, f2, bsp0) \
  (mk_cmd(p, 0, (f2), (bsp0), (ws), (wr), (ov), (lamp), (samod), (tout), (bsp), (mm), (gld)), \
   S.now += 6, S.b.pol.valid = 0, S.b.ack_n = 0, (esp_send(&S, MT_COMMANDO, p, 15), S.b.pol.valid))
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 90) == 1);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 91) == 0);
  CHECK(CMDS(1500, -300, 0, 0, 0, 0, 0, 0, 0, 0, 10) == 1);
  CHECK(CMDS(1501, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, -301, 0, 0, 0, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 50, 0, 0, 0, 0, 0, 0, 0, 10) == 1);
  CHECK(CMDS(0, 0, 300, 0, 0, 0, 0, 0, 0, 0, 10) == 1);
  CHECK(CMDS(0, 0, 49, 0, 0, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 301, 0, 0, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 4, 100, 0x1F, 90, 100, 10, 0, 10) == 1);
  CHECK(CMDS(0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 101, 0, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 91, 0, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 101, 0, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 10) == 0);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 10) == 1);
  CHECK(CMDS(0, 0, 0, 0, 0, 0, 0, 0, 0, 3 << 6, 10) == 0);
  /* verkeerde lengte */
  S.now += 6; S.b.pol.valid = 0; S.b.ack_n = 0;
  esp_send(&S, MT_COMMANDO, p, 14);
  CHECK(S.b.pol.valid == 0 && S.b.ack_n == 1 && S.b.ack[0].status == ST_LENGTE);
}

/* ---------- 5. opstart, noodbrug, relais ---------- */
static void test_opstart_noodbrug(void) {
  uint32_t t0;
  sect("opstart: geremd, noodbrug pas na 240 s, K3 alleen bij aan/uit-ketel");
  sim_init(&S, 5000, 0);
  CHECK(S.b.brake_applied == 1 && S.b.brake_sp_applied == 10);
  CHECK(S.b.bridge_on == 0 && S.b.k3_on == 0);
  sim_step(&S, 1);
  CHECK(S.b.bridge_on == 0);                       /* de v1-fout: relais op de eerste ronde */
  t0 = S.now;
  while ((uint32_t) (S.now - t0) < 239000UL) { sim_step(&S, 50); if (S.b.bridge_on) break; }
  CHECK(S.b.bridge_on == 0 && S.b.brake_applied == 1 && S.b.k3_on == 0);
  while ((uint32_t) (S.now - t0) < 241000UL) { sim_step(&S, 50); if (S.b.bridge_on) break; }
  CHECK(S.b.bridge_on == 1);
  CHECK((uint32_t) (S.now - t0) >= 239950UL && (uint32_t) (S.now - t0) <= 240100UL);
  CHECK(S.inv_viol == 0);

  /* de teller loopt vanaf de start, en elk geldig ESP-bericht zet hem terug */
  sim_init(&S, 0xFFFFFF00UL, 0);                   /* millis loopt tijdens de test over */
  t0 = S.now;
  { uint32_t k;
    for (k = 0; k < 6; k++) {                      /* 6 x 200 s met telkens een ACK van de ESP */
      uint32_t u = 0;
      while (u < 200000UL) { sim_step(&S, 100); u += 100; if (S.b.bridge_on) break; }
      CHECK(S.b.bridge_on == 0);
      { uint8_t a[2] = { 0, 0 }; esp_send(&S, MT_ACK, a, 2); }
    }
  }
  CHECK(S.b.bridge_on == 0);
  CHECK(S.b.esp_seen == 1);
  /* nu zwijgt de ESP: 240 s later de brug */
  t0 = S.now;
  while ((uint32_t) (S.now - t0) < 241000UL) { sim_step(&S, 100); if (S.b.bridge_on) break; }
  CHECK(S.b.bridge_on == 1);
  /* weer een ESP-bericht: brug los */
  { uint8_t a[2] = { 0, 0 }; esp_send(&S, MT_ACK, a, 2); sim_run(&S, 10, 1); }
  CHECK(S.b.bridge_on == 0);
  /* terwijl de noodbrug aan is zwijgt de AVR op de OpenTherm-lijnen */
  S.b.esp_ms = S.now - 300000UL;
  sim_run(&S, 200, 1);
  CHECK(S.b.bridge_on == 1);
  transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(S.bo_n == 0 && S.st_n == 0);

  /* OpenTherm-ketel (dip laag): K3 blijft altijd open, ook bij vrijgave en vraag */
  sim_init(&S, 1000, 0);
  cmd(0, C2_DEMAND, 10, 0, 0, 0, 0, 255);          /* rem uit, K3 vrijgegeven */
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 200);
  sim_run(&S, 100, 1);
  CHECK(S.b.brake_applied == 0 && S.b.obs.requested_setpoint == 45);
  CHECK(S.b.k3_on == 0 && S.b.aanuit == 0);

  /* aan/uit-ketel (dip hoog): K3 sluit pas na rem uit + vrijgave + setpoint >= 20 */
  sim_init(&S, 1000, 1);
  sim_run(&S, 100, 1);
  CHECK(S.b.aanuit == 1);
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 200);
  CHECK(S.b.k3_on == 0);                           /* nog geremd (10 C) */
  cmd(0, C2_DEMAND, 10, 0, 0, 0, 0, 255);
  sim_run(&S, 20, 1);
  CHECK(S.b.brake_applied == 0 && S.b.k3_on == 1 && S.b.bridge_on == 0);
  transactie(F(MSG_WRITE_DATA, 1, 0x1000), 200);   /* 16 C: onder de drempel */
  sim_run(&S, 20, 1);
  CHECK(S.b.k3_on == 0);
  transactie(F(MSG_WRITE_DATA, 1, 0x1400), 200);   /* 20 C: aan */
  sim_run(&S, 20, 1);
  CHECK(S.b.k3_on == 1);
  /* zonder vrijgave geen K3 */
  cmd(0, 0, 10, 0, 0, 0, 0, 255); sim_run(&S, 20, 1);
  CHECK(S.b.k3_on == 0);
  /* de ESP zegt "OpenTherm-ketel" tegen de dip in: K3 uit, daarna kan de noodbrug */
  cmd(0, C2_DEMAND | C2_KT_OT, 10, 0, 0, 0, 0, 255); sim_run(&S, 20, 1);
  CHECK(S.b.aanuit == 0 && S.b.k3_on == 0);
  cmd(C1_BRUG, C2_DEMAND | C2_KT_OT, 10, 0, 0, 0, 0, 255); sim_run(&S, 100, 1);
  CHECK(S.b.bridge_on == 1 && S.b.k3_on == 0);
  /* en andersom: dip laag, ESP zegt aan/uit */
  sim_init(&S, 1000, 0);
  cmd(0, C2_DEMAND | C2_KT_AU, 10, 0, 0, 0, 0, 255);
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 200); sim_run(&S, 20, 1);
  CHECK(S.b.aanuit == 1 && S.b.k3_on == 1);
  S.b.pol.valid = 0;                               /* beleid weg: terug naar de dip (laag = OpenTherm) */
  sim_run(&S, 100, 1);
  CHECK(S.b.aanuit == 0 && S.b.k3_on == 0);

  /* aan/uit-ketel, ESP weg na de noodtijd: K3 volgt de thermostaat (NOOD_K3_VOLGT) */
  sim_init(&S, 1000, 1);
  { uint32_t u = 0;
    while (u < 241000UL) { sim_step(&S, 100); u += 100; if (u % 1000 == 0) thermo(&S, F(MSG_WRITE_DATA, 1, 0x2D00)); }
  }
  CHECK(S.b.aanuit == 1 && S.b.bridge_on == 0 && S.b.nood == 1);
#if NOOD_K3_VOLGT
  CHECK(S.b.k3_on == 1);
#endif
  /* thermostaat verdwijnt: K3 gaat uit */
  sim_run(&S, 7000, 100);
  CHECK(S.b.k3_on == 0);
  CHECK(S.inv_viol == 0);
}

/* 1800 s en 900 s: de UL-rekenkunde (16-bit int op de AVR) */
static void test_lange_tijden(void) {
  uint8_t pl[100], n; uint8_t std[10][5], i; Tabel d;
  sect("lange tijden: 1800 s en 900 s (UL-rekenkunde)");
  tabel_standaard(&d);
  for (i = 0; i < 10; i++) { std[i][0] = d.r[i].id; std[i][1] = d.r[i].richting; std[i][2] = d.r[i].modus; std[i][3] = d.r[i].p0; std[i][4] = d.r[i].p1; }
  sim_init(&S, 100, 0);
  n = mk_tabel(pl, 3, 250, 400, 600, 1800, 900, std, 10);
  esp_send(&S, MT_TABEL, pl, n);
  sim_run(&S, 10, 1);
  CHECK(S.b.act.nood_s == 1800 && S.b.act.rem_hold_s == 900 && S.b.act.esp_weg_s == 600);
  /* noodbrug op exact 1800 s na het laatste ESP-bericht */
  S.b.esp_ms = S.now - 1799999UL; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.bridge_on == 0);
  S.b.esp_ms = S.now - 1800000UL; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.bridge_on == 1);
  S.b.esp_ms = S.now;  brug_tick(&S.b, S.now, 0);
  /* houdtijd 900 s */
  cmd(0, 0, 10, 0, 0, 0, 0, 255);                  /* rem uit: eerste wissel mag meteen */
  brug_tick(&S.b, S.now, 0);
  CHECK(S.b.brake_applied == 0);
  S.b.pol.rx_ms = S.now; S.b.pol.brake_on = 1;
  S.now += 899000UL; S.b.esp_ms = S.now; S.b.pol.rx_ms = S.now; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.brake_applied == 0);
  S.now += 1000UL; S.b.esp_ms = S.now; S.b.pol.rx_ms = S.now; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.brake_applied == 1);
  /* esp_weg 600 s */
  S.now += 599000UL; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.esp_seen == 1);
  S.now += 1000UL; brug_tick(&S.b, S.now, 0);
  CHECK(S.b.esp_seen == 0);
}

/* ---------- 6. de standaardregels op het OpenTherm-verkeer ---------- */
static void test_standaard_regels(void) {
  uint32_t el;
  sect("standaardtabel op OpenTherm-verkeer");
  sim_init(&S, 2000, 0);
  sim_run(&S, 200, 1);

  /* ID 0: CH-vlag eraf, DHW aan (origineel: 0x0200) */
  el = transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(S.bo_n == 1 && ot_type(S.bo_f[0]) == MSG_READ_DATA && ot_id(S.bo_f[0]) == 0 && ot_val(S.bo_f[0]) == 0x0200);
  CHECK(ot_parity_ok(S.bo_f[0]));
  CHECK(S.st_n == 1 && S.st_f[0] == reply_for(S.bo_f[0]) && el >= 20 && el < 100);
  /* de thermostaat stuurt CH aan, DHW uit: wordt CH uit, DHW aan */
  transactie(F(MSG_READ_DATA, 0, 0x0100), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0200);
  /* alleen DHW blijft DHW */
  transactie(F(MSG_READ_DATA, 0, 0x0200), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0200);
  /* de lage byte (slave-deel) blijft */
  transactie(F(MSG_READ_DATA, 0, 0x0355), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0255);

  /* ID 1: de rem op 10 C, en het ACK toont de waarde van de thermostaat (echo) */
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 300);
  CHECK(S.bo_n == 1 && ot_type(S.bo_f[0]) == MSG_WRITE_DATA && ot_val(S.bo_f[0]) == 0x0A00);
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_WRITE_ACK && ot_id(S.st_f[0]) == 1 && ot_val(S.st_f[0]) == 0x2D00);
  CHECK(ot_parity_ok(S.st_f[0]));
  CHECK(S.b.obs.requested_setpoint == 45);          /* ESP ziet wat de thermostaat vroeg */

  /* ID 16: altijd Write 8,0 C, ook bij leesvraag; de thermostaat krijgt weer een READ-ACK */
  transactie(F(MSG_READ_DATA, 16, 0x1500), 300);
  CHECK(S.bo_n == 1 && ot_type(S.bo_f[0]) == MSG_WRITE_DATA && ot_val(S.bo_f[0]) == 0x0800);
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_READ_ACK && ot_id(S.st_f[0]) == 16);
  transactie(F(MSG_WRITE_DATA, 16, 0x1500), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0800 && ot_type(S.st_f[0]) == MSG_WRITE_ACK && ot_val(S.st_f[0]) == 0x1500);

  /* alles wat niet in de tabel staat gaat 1:1 door */
  transactie(F(MSG_READ_DATA, 17, 0), 300);
  CHECK(S.bo_f[0] == F(MSG_READ_DATA, 17, 0) && S.st_f[0] == reply_for(S.bo_f[0]));
  transactie(F(MSG_WRITE_DATA, 14, 0x3200), 300);          /* ID 14: max_mod niet actief -> doorgeven */
  CHECK(ot_val(S.bo_f[0]) == 0x3200);
  transactie(F(MSG_READ_DATA, 25, 0), 300);                /* substitutie uit -> ketelwaarde */
  CHECK(S.st_f[0] == reply_for(S.bo_f[0]));

  /* rem af: ID 0/1/16 gaan onveranderd door; nog geen substitutie */
  cmd(0, 0, 10, 0, 0, 0, 0, 255); sim_run(&S, 10, 1);
  CHECK(S.b.brake_applied == 0);
  transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0300);
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x2D00 && ot_val(S.st_f[0]) == 0x2D00);
  transactie(F(MSG_READ_DATA, 16, 0x1500), 300);
  CHECK(ot_type(S.bo_f[0]) == MSG_READ_DATA && ot_val(S.bo_f[0]) == 0x1500);

  /* ch_off alleen (rem af): CH-vlag eraf, rest blijft */
  cmd(0, C2_CHOFF, 10, 0, 0, 0, 0, 255); sim_run(&S, 10, 1);
  transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0200);
  cmd(0, 0, 10, 0, 0, 0, 0, 255); sim_run(&S, 10, 1);
  transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0300);

  /* temperatuursubstitutie ID 25 en 28 */
  cmd(C1_SUBST, 0, 10, 355, 290, 0, 0, 255); sim_run(&S, 10, 1);
  transactie(F(MSG_READ_DATA, 25, 0), 300);
  CHECK(ot_type(S.st_f[0]) == MSG_READ_ACK && ot_val(S.st_f[0]) == tenths_to_f88(355));
  transactie(F(MSG_READ_DATA, 28, 0), 300);
  CHECK(ot_val(S.st_f[0]) == tenths_to_f88(290) && ot_parity_ok(S.st_f[0]));
  transactie(F(MSG_READ_DATA, 17, 0), 300);                /* andere ID's niet */
  CHECK(S.st_f[0] == reply_for(S.bo_f[0]));

  /* max. modulatie ID 14 */
  cmd(0, 0, 10, 0, 0, 0, 40, 255); sim_run(&S, 10, 1);
  transactie(F(MSG_WRITE_DATA, 14, 0x6400), 300);
  CHECK(ot_val(S.bo_f[0]) == tenths_to_f88(400));
  transactie(F(MSG_READ_DATA, 14, 0), 300);                /* een leesvraag blijft staan */
  CHECK(ot_type(S.bo_f[0]) == MSG_READ_DATA);

  /* override-setpoint via ID 9 en 100 (ook als de ketel UNKNOWN antwoordt: FORCEER) */
  cmd(0, 0, 10, 0, 0, 215, 0, 255); sim_run(&S, 10, 1);
  thermo(&S, F(MSG_READ_DATA, 9, 0));
  boiler_at(&S, S.now + 50, F(MSG_UNKNOWN_ID, 9, 0));
  S.bo_silent = 1; sim_run(&S, 300, 1); S.bo_silent = 0;
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_READ_ACK && ot_val(S.st_f[0]) == tenths_to_f88(215));
  transactie(F(MSG_READ_DATA, 100, 0), 300);
  CHECK(ot_type(S.st_f[0]) == MSG_READ_ACK && ot_val(S.st_f[0]) == 0);
  cmd(0, 0, 10, 0, 0, 0, 0, 255); sim_run(&S, 10, 1);
  transactie(F(MSG_READ_DATA, 9, 0), 300);                 /* override weer weg */
  CHECK(S.st_f[0] == reply_for(S.bo_f[0]));
}

/* ---------- 7. rem: houdtijd, setpoint-houdtijd, directe terugval ---------- */
static void run_cmd(uint32_t ms, uint8_t f1, uint8_t f2, uint8_t bsp) {
  uint32_t u = 0;
  while (u < ms) { cmd(f1, f2, bsp, 0, 0, 0, 0, 100); sim_run(&S, 10000, 100); u += 10000; }
}

static void test_rem(void) {
  sect("rem: houdtijd op vlag en setpoint, directe terugval, opstart");
  sim_init(&S, 0xFFFFFF00UL, 0);
  CHECK(S.b.brake_applied == 1);
  cmd(0, 0, 10, 0, 0, 0, 0, 100); sim_run(&S, 10, 1);
  CHECK(S.b.brake_applied == 0);                    /* eerste wissel na de start mag meteen */
  run_cmd(30000, C1_REM, 0, 10);
  CHECK(S.b.brake_applied == 0);                    /* binnen de houdtijd: nog niet */
  run_cmd(260000, C1_REM, 0, 10);                   /* 290 s */
  CHECK(S.b.brake_applied == 0);
  run_cmd(20000, C1_REM, 0, 10);                    /* 310 s */
  CHECK(S.b.brake_applied == 1 && S.b.brake_sp_applied == 10);
  /* setpoint 10 <-> 35 pendelt niet */
  run_cmd(30000, C1_REM, 0, 35);
  CHECK(S.b.brake_sp_applied == 10);                /* houdtijd loopt vanaf de laatste wissel */
  run_cmd(280000, C1_REM, 0, 35);
  CHECK(S.b.brake_applied == 1 && S.b.brake_sp_applied == 35);
  run_cmd(30000, C1_REM, 0, 10);
  CHECK(S.b.brake_sp_applied == 35);                /* terug naar 10: ook gehouden */
  run_cmd(280000, C1_REM, 0, 10);
  CHECK(S.b.brake_sp_applied == 10);
  /* de toegepaste setpoint gaat ook echt naar de ketel */
  run_cmd(280000, C1_REM, 0, 35);
  CHECK(S.b.brake_sp_applied == 35);
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 300);
  CHECK(ot_val(S.bo_f[0]) == tenths_to_f88(350));

  /* "rem direct" slaat de houdtijd over (voor storingen) */
  cmd(0, C2_DIRECT, 35, 0, 0, 0, 0, 100); sim_run(&S, 10, 1);
  CHECK(S.b.brake_applied == 0);

  /* beleid vervalt: geremd en direct, ook binnen de houdtijd; overrides weg */
  { uint8_t p[5]; p[0] = 1; p[1] = 0x10; p[2] = 0x00; p[3] = 0x00; p[4] = 0x00;       /* ID 0x00? zie hieronder */
    p[1] = (uint8_t) (MSG_READ_ACK << 4); p[2] = 3; p[3] = 0x01; p[4] = 0x00;           /* override ID 3 = 0x0100 */
    esp_send(&S, MT_INJECT, p, 5); }
  sim_run(&S, 10, 1);
  transactie(F(MSG_READ_DATA, 3, 0), 300);
  CHECK(ot_val(S.st_f[0]) == 0x0100);                                                  /* override werkt zolang het beleid geldig is */
  S.b.esp_ms = S.now;
  cmd(0, 0, 10, 0, 0, 0, 0, 10);                    /* geldig 10 s */
  sim_run(&S, 9000, 100);
  CHECK(S.b.pol.valid == 1 && S.b.brake_applied == 0);
  sim_run(&S, 1500, 100);
  CHECK(S.b.pol.valid == 0 && S.b.brake_applied == 1 && S.b.brake_sp_applied == 10);
  { uint8_t any = 0, i; for (i = 0; i < 8; i++) any |= S.b.ovr[i].used; CHECK(any == 0); }
  transactie(F(MSG_READ_DATA, 3, 0), 300);
  CHECK(S.st_f[0] == reply_for(S.bo_f[0]));         /* geen override meer */
  /* de brug en CH-vlag-regel gelden weer, zonder ESP */
  transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(ot_val(S.bo_f[0]) == 0x0200);
}

/* ---------- 8. ketelantwoord: laat, verkeerd ID, verkeerd type, pariteit ---------- */
static void test_ketelantwoord(void) {
  uint32_t el;
  sect("ketelantwoord: te laat, ander Data-ID, ander type, slechte pariteit");
  sim_init(&S, 2000, 0); sim_run(&S, 200, 1);

  /* de ketel is te langzaam (450 ms > 400 ms): de thermostaat krijgt toch een antwoord */
  S.bo_delay = 450;
  el = transactie(F(MSG_READ_DATA, 0, 0x0300), 600);
  CHECK(S.st_n == 1 && el >= 395 && el <= 410);
  CHECK(ot_type(S.st_f[0]) == MSG_READ_ACK && ot_id(S.st_f[0]) == 0);    /* ID 0: laatste slavestatus */
  /* het late antwoord is binnengekomen en is weggegooid: de volgende vraag krijgt zijn EIGEN antwoord */
  S.bo_delay = 60;
  el = transactie(F(MSG_READ_DATA, 17, 0), 300);
  CHECK(S.st_n == 1 && ot_id(S.st_f[0]) == 17 && S.st_f[0] == reply_for(S.bo_f[0]));
  CHECK(S.b.cnt_ketel_oud >= 1);
  /* een vraag die niet ID 0 is krijgt na de time-out UNKNOWN-DATAID */
  S.bo_delay = 450;
  transactie(F(MSG_READ_DATA, 17, 0), 600);
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID && ot_id(S.st_f[0]) == 17);

  /* stale antwoord valt midden in de volgende wachttijd: bezet zijn en negeren */
  sim_init(&S, 2000, 0); sim_run(&S, 200, 1);
  S.bo_silent = 1;
  transactie(F(MSG_READ_DATA, 17, 0), 450);               /* ketel zwijgt: fallback */
  CHECK(S.st_n == 1);
  S.bo_silent = 0; S.bo_delay = 100;
  thermo(&S, F(MSG_READ_DATA, 19, 0));
  boiler_at(&S, S.now + 30, reply_for(F(MSG_READ_DATA, 17, 0)));     /* het late antwoord op de vorige vraag */
  sim_run(&S, 300, 1);
  CHECK(S.st_n == 1 && ot_id(S.st_f[0]) == 19 && S.st_f[0] == reply_for(F(MSG_READ_DATA, 19, 0)));

  /* verkeerd Data-ID eerst, dan het goede */
  sim_init(&S, 2000, 0); sim_run(&S, 200, 1);
  S.bo_wrong_first = 1;
  transactie(F(MSG_READ_DATA, 19, 0), 300);
  CHECK(S.st_n == 1 && ot_id(S.st_f[0]) == 19 && S.b.cnt_ketel_oud >= 1);
  S.bo_wrong_first = 0;
  /* goed ID maar verkeerd type (WRITE gevraagd, READ-ACK terug) */
  thermo(&S, F(MSG_WRITE_DATA, 40, 0x1234));
  S.bo_silent = 1;
  boiler_at(&S, S.now + 20, F(MSG_READ_ACK, 40, 0x9999));
  boiler_at(&S, S.now + 50, F(MSG_WRITE_ACK, 40, 0x1234));
  sim_run(&S, 300, 1); S.bo_silent = 0;
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_WRITE_ACK && ot_val(S.st_f[0]) == 0x1234);
  /* slechte pariteit wordt weggegooid */
  thermo(&S, F(MSG_READ_DATA, 41, 0));
  S.bo_silent = 1;
  boiler_at(&S, S.now + 20, F(MSG_READ_ACK, 41, 0x4242) ^ 0x80000000UL);
  boiler_at(&S, S.now + 50, F(MSG_READ_ACK, 41, 0x4343));
  sim_run(&S, 300, 1); S.bo_silent = 0;
  CHECK(S.st_n == 1 && ot_val(S.st_f[0]) == 0x4343);
  /* DATA-INVALID en UNKNOWN-ID zijn geldige antwoorden en gaan door */
  thermo(&S, F(MSG_READ_DATA, 42, 0));
  S.bo_silent = 1; boiler_at(&S, S.now + 20, F(MSG_UNKNOWN_ID, 42, 0)); sim_run(&S, 100, 1); S.bo_silent = 0;
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID);
  /* een ketelframe zonder dat er iets openstaat wordt geteld en weggegooid */
  { uint8_t before = S.b.cnt_ketel_oud;
    boiler_at(&S, S.now + 5, F(MSG_READ_ACK, 5, 1)); sim_run(&S, 50, 1);
    CHECK(S.b.cnt_ketel_oud == before + 1 && S.st_n == 1); }
  /* de thermostaat met slechte pariteit krijgt geen antwoord en de brug blijft werken */
  S.st_n = 0;
  thermo(&S, F(MSG_READ_DATA, 17, 0) ^ 0x80000000UL); sim_run(&S, 100, 1);
  CHECK(S.st_n == 0 && S.b.parity_fails == 1);
  transactie(F(MSG_READ_DATA, 17, 0), 300);
  CHECK(S.st_n == 1);
}

/* ---------- 9. VRAAG / ANTWOORD ---------- */
static const uint8_t RV[3][5] = {
  { 5,  DIR_TK, 0x02, 0,  0 },    /* ID 5 T>K: vraag, standaard deadline, bij verlopen PASS */
  { 27, DIR_KT, 0x02, 25, 0 },    /* ID 27 K>T: vraag, 100 ms, PASS */
  { 7,  DIR_TK, 0x02, 0,  1 },    /* ID 7 T>K: vraag, standaard deadline, bij verlopen BLOKKEER */
};

static void antwoord(uint8_t vseq, uint8_t actie, uint32_t frame) {
  uint8_t p[6];
  p[0] = vseq; p[1] = actie;
  p[2] = (uint8_t) (frame >> 24); p[3] = (uint8_t) (frame >> 16); p[4] = (uint8_t) (frame >> 8); p[5] = (uint8_t) frame;
  esp_send(&S, MT_ANTWOORD, p, 6);
}

static void vraag_setup(void) {
  uint8_t pl[100], n;
  sim_init(&S, 3000, 0); sim_run(&S, 200, 1);
  n = mk_tabel(pl, 2, 200, 400, 60, 240, 300, RV, 3);
  install(pl, n);
  CHECK(S.b.act.n == 3);
}

static uint32_t vraag_frame_van(MLog *m) {
  return ((uint32_t) m->p[1] << 24) | ((uint32_t) m->p[2] << 16) | ((uint32_t) m->p[3] << 8) | m->p[4];
}

static void test_vraag_antwoord(void) {
  MLog *m; uint8_t vs;
  sect("VRAAG/ANTWOORD");
  /* T>K: DOORGEVEN */
  vraag_setup();
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 30, 1);
  m = lastlog(&S, MT_VRAAG);
  CHECK(m && m->p[0] == DIR_TK && vraag_frame_van(m) == F(MSG_READ_DATA, 5, 0));
  CHECK(S.bo_n == 0);                                   /* vastgehouden */
  vs = m ? m->seq : 0;
  antwoord(vs, 0, 0); sim_run(&S, 300, 1);
  CHECK(S.bo_n == 1 && S.bo_f[0] == F(MSG_READ_DATA, 5, 0) && S.st_n == 1 && S.st_f[0] == reply_for(S.bo_f[0]));

  /* T>K: VERVANGEN */
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 30, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  antwoord(vs, 1, F(MSG_WRITE_DATA, 5, 0x0042)); sim_run(&S, 300, 1);
  CHECK(S.bo_n == 1 && ot_type(S.bo_f[0]) == MSG_WRITE_DATA && ot_val(S.bo_f[0]) == 0x0042 && ot_parity_ok(S.bo_f[0]));
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_READ_ACK && ot_val(S.st_f[0]) == 0x0042);

  /* T>K: VERVANGEN met een ander Data-ID is ongeldig: geldt als geen antwoord, standaardactie PASS bij de deadline */
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 30, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  antwoord(vs, 1, F(MSG_WRITE_DATA, 6, 0x0042)); sim_run(&S, 400, 1);
  CHECK(S.bo_n == 1 && S.bo_f[0] == F(MSG_READ_DATA, 5, 0) && S.bo_t[0] - S.t_thermo >= 195 && S.bo_t[0] - S.t_thermo <= 215);

  /* T>K: BLOKKEREN: de ketel merkt niets, lokaal UNKNOWN, na minstens 40 ms (OT: >= 20 ms) */
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 10, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  antwoord(vs, 2, 0); sim_run(&S, 300, 1);
  CHECK(S.bo_n == 0 && S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID && ot_id(S.st_f[0]) == 5);
  CHECK(S.st_t[0] - S.t_thermo >= 40);

  /* verkeerd volgnummer wordt genegeerd; de deadline regelt het daarna (PASS) */
  { uint8_t before;
    thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 30, 1);
    m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
    before = S.b.cnt_antw_negeer;
    antwoord((uint8_t) (vs + 1), 2, 0); antwoord((uint8_t) (vs - 1), 2, 0); antwoord((uint8_t) (vs ^ 0x80), 1, F(MSG_WRITE_DATA, 5, 1));
    sim_run(&S, 20, 1);
    CHECK(S.b.cnt_antw_negeer == (uint8_t) (before + 3) && S.bo_n == 0 && S.b.st == ST_WAIT_ESP_TK);
    sim_run(&S, 400, 1);
    CHECK(S.bo_n == 1 && S.bo_f[0] == F(MSG_READ_DATA, 5, 0) && S.b.cnt_vraag_verlopen >= 1);
    /* het goede volgnummer na de deadline: negeren */
    before = S.b.cnt_antw_negeer;
    antwoord(vs, 2, 0); sim_run(&S, 100, 1);
    CHECK(S.b.cnt_antw_negeer == (uint8_t) (before + 1) && S.st_n == 1 && ot_type(S.st_f[0]) != MSG_UNKNOWN_ID);
  }
  /* een ANTWOORD zonder openstaande vraag */
  { uint8_t before = S.b.cnt_antw_negeer; sim_run(&S, 50, 1); antwoord(5, 0, 0); CHECK(S.b.cnt_antw_negeer == (uint8_t) (before + 1)); }

  /* deadline met standaardactie BLOKKEER (ID 7) */
  thermo(&S, F(MSG_READ_DATA, 7, 0)); sim_run(&S, 400, 1);
  CHECK(S.bo_n == 0 && S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID && S.st_t[0] - S.t_thermo >= 195 && S.st_t[0] - S.t_thermo <= 250);

  /* K>T: het ketelantwoord wordt gevraagd en vervangen */
  S.bo_delay = 60;
  thermo(&S, F(MSG_READ_DATA, 27, 0)); sim_run(&S, 100, 1);
  m = lastlog(&S, MT_VRAAG);
  CHECK(m && m->p[0] == DIR_KT && vraag_frame_van(m) == reply_for(F(MSG_READ_DATA, 27, 0)));
  CHECK(S.st_n == 0 && S.bo_n == 1);
  vs = m ? m->seq : 0;
  antwoord(vs, 1, F(MSG_READ_ACK, 27, 0x5555)); sim_run(&S, 100, 1);
  CHECK(S.st_n == 1 && S.st_f[0] == F(MSG_READ_ACK, 27, 0x5555));
  /* K>T: BLOKKEREN, DOORGEVEN, en verlopen (100 ms, PASS) */
  thermo(&S, F(MSG_READ_DATA, 27, 0)); sim_run(&S, 100, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  antwoord(vs, 2, 0); sim_run(&S, 50, 1);
  CHECK(S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID && ot_id(S.st_f[0]) == 27);
  thermo(&S, F(MSG_READ_DATA, 27, 0)); sim_run(&S, 100, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  antwoord(vs, 0, 0); sim_run(&S, 50, 1);
  CHECK(S.st_n == 1 && S.st_f[0] == reply_for(F(MSG_READ_DATA, 27, 0)));
  thermo(&S, F(MSG_READ_DATA, 27, 0)); sim_run(&S, 400, 1);
  CHECK(S.st_n == 1 && S.st_f[0] == reply_for(F(MSG_READ_DATA, 27, 0)) && (S.st_t[0] - S.t_thermo) >= 155 && (S.st_t[0] - S.t_thermo) <= 185);

  /* zonder ESP (esp_seen weg): meteen de standaardactie, geen VRAAG */
  { uint8_t before = count_log(&S, MT_VRAAG);
    S.b.esp_seen = 0;
    thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 300, 1);
    CHECK(S.bo_n == 1 && S.st_n == 1 && S.bo_t[0] - S.t_thermo <= 3);
    thermo(&S, F(MSG_READ_DATA, 7, 0)); sim_run(&S, 300, 1);
    CHECK(S.bo_n == 0 && S.st_n == 1 && ot_type(S.st_f[0]) == MSG_UNKNOWN_ID);
    CHECK(count_log(&S, MT_VRAAG) == before);
    S.b.esp_seen = 1;
  }
}

/* ---------- 10. tabelwissel tijdens een openstaande vraag ---------- */
static void test_tabelwissel(void) {
  MLog *m; uint8_t vs, pl[100], n, seq;
  sect("tabelwissel: uitgesteld tijdens een vraag, ACK, afwijzing");
  vraag_setup();
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 30, 1);
  m = lastlog(&S, MT_VRAAG); vs = m ? m->seq : 0;
  CHECK(S.b.st == ST_WAIT_ESP_TK);
  { static const uint8_t rn[1][5] = { { 99, DIR_TK, 0x03, 0, 7 } };
    n = mk_tabel(pl, 9, 300, 400, 60, 240, 300, rn, 1);
    seq = S.esp_seq;
    esp_send(&S, MT_TABEL, pl, n); sim_run(&S, 20, 1);
    CHECK(S.b.act.n == 3 && S.b.pend_valid == 1);          /* nog niet toegepast */
    antwoord(vs, 0, 0); sim_run(&S, 300, 1);
    CHECK(S.b.act.n == 1 && S.b.act.versie == 9 && S.b.pend_valid == 0);
    m = lastlog(&S, MT_ACK);
    CHECK(m && m->p[0] == seq && m->p[1] == ST_OK);
  }
  /* de tabel komt niet middenin een gesprek: tijdens WAIT_BOILER ook niet */
  { static const uint8_t rn2[1][5] = { { 98, DIR_TK, 0x03, 0, 7 } };
    S.bo_delay = 100;
    thermo(&S, F(MSG_READ_DATA, 17, 0)); sim_run(&S, 20, 1);
    CHECK(S.b.st == ST_WAIT_BOILER);
    n = mk_tabel(pl, 10, 300, 400, 60, 240, 300, rn2, 1);
    esp_send(&S, MT_TABEL, pl, n); sim_run(&S, 10, 1);
    CHECK(S.b.act.versie == 9 && S.b.pend_valid == 1);
    sim_run(&S, 300, 1);
    CHECK(S.b.act.versie == 10 && S.st_n == 1 && S.st_f[0] == reply_for(F(MSG_READ_DATA, 17, 0)));
  }
  /* beschadigde tabel (CRC-16) en tabel buiten de grenzen: niets verandert, ACK status 2 */
  n = LOADV(bufA, ref_tabel_grens);
  seq = bufA[3];
  esp_raw(&S, bufA, n); sim_run(&S, 20, 1);
  m = lastlog(&S, MT_ACK);
  CHECK(S.b.act.versie == 10 && S.b.pend_valid == 0 && m && m->p[0] == seq && m->p[1] == ST_GRENS);
  n = LOADV(bufA, ref_tabel_default);
  bufA[20] ^= 0x04;                                         /* bericht-CRC klopt dan niet meer */
  { uint8_t cb = S.b.cnt_crc; (void) cb; }
  S.now += 10;
  esp_raw(&S, bufA, n); sim_run(&S, 20, 1);
  CHECK(S.b.act.versie == 10);
  /* een tabel met goede bericht-CRC-8 maar kapotte tabel-CRC-16 */
  { uint8_t tp[100], k2, mm[110];
    k2 = LOADV(tp, ref_tabel_default); k2 = (uint8_t) (k2 - 5);
    memcpy(tp, bufA, 0);
    vec(bufB, ref_tabel_default, (uint8_t) (k2 + 5));
    memcpy(tp, bufB + 4, k2);
    tp[k2 - 1] ^= 1;
    n = msg_build(mm, MT_TABEL, 77, tp, k2);
    S.now += 10; esp_raw(&S, mm, n); sim_run(&S, 20, 1);
    m = lastlog(&S, MT_ACK);
    CHECK(S.b.act.versie == 10 && m && m->p[0] == 77 && m->p[1] == ST_GRENS);
  }
}

/* ---------- 11. HELLO, handshake, terugval ---------- */
static void test_hello_en_terugval(void) {
  uint8_t pl[100], n, i;
  sect("HELLO-handshake, terugval na ESP-stilte");
  sim_init(&S, 1000, 0);
  sim_run(&S, 5000, 1);
  CHECK(count_log(&S, MT_HELLO) == 3);                    /* 100 ms, 2,1 s, 4,1 s */
  { MLog *m = lastlog(&S, MT_HELLO);
    CHECK(m && m->len == 14 && m->p[0] == 2 && m->p[1] == 0 && m->p[2] == 0 && m->p[3] == 26 && m->p[6] == 2);
    CHECK(m && (((uint16_t) m->p[7] << 8) | m->p[8]) == REF_DEFAULT_TABEL_CRC);
    CHECK(m && m->p[9] == 0x01); }
  CHECK(S.gap_viol == 0 && S.tx_bad == 0);

  /* ESP stuurt de tabel van het ontwerp, AVR neemt hem over en bevestigt */
  { uint8_t seq = S.esp_seq;
    n = LOADV(bufA, ref_tabel_default);
    bufA[3] = seq;                                           /* volgnr aanpassen vergt nieuwe CRC */
    bufA[n - 1] = crc8(bufA + 1, (uint16_t) (n - 2));
    esp_raw(&S, bufA, n); sim_run(&S, 30, 1);
    { MLog *a = lastlog(&S, MT_ACK); CHECK(a && a->p[0] == seq && a->p[1] == 0); }
    CHECK(S.b.act.versie == 1 && S.b.act.standaard == 0 && S.b.act.crc == REF_DEFAULT_TABEL_CRC);
    CHECK(S.b.handshake == 1);
  }
  { uint8_t before = count_log(&S, MT_HELLO);
    sim_run(&S, 10000, 1);                                   /* ESP is er (dit is ook een kort contact) */
    CHECK(count_log(&S, MT_HELLO) == before || S.b.esp_seen == 1);
  }

  /* terugval: de ESP zwijgt langer dan esp_weg_s met een eigen tabel actief */
  sim_init(&S, 1000, 0); sim_run(&S, 300, 1);
  { static const uint8_t kl[2][5] = { { 27, DIR_KT, 0x02, 25, 0 }, { 5, DIR_TK, 0x03, 0, 7 } };
    n = mk_tabel(pl, 5, 200, 400, 20, 100, 300, kl, 2);
    install(pl, n);
    CHECK(S.b.act.n == 2 && S.b.act.esp_weg_s == 20); }
  cmd(C1_REM, 0, 10, 0, 0, 0, 0, 255);                       /* beleid blijft 255 s geldig */
  sim_run(&S, 18000, 100);
  CHECK(S.b.act.n == 2 && S.b.esp_seen == 1 && S.b.pol.valid == 1);
  sim_run(&S, 3000, 100);
  CHECK(S.b.esp_seen == 0 && S.b.act.standaard == 1 && S.b.act.n == 10);
  CHECK(S.b.pol.valid == 1);                                 /* beleid heeft een eigen geldigheid */
  /* HELLO elke 5 s met de standaard-CRC */
  { uint8_t before = count_log(&S, MT_HELLO);
    sim_run(&S, 11000, 100);
    CHECK((uint8_t) (count_log(&S, MT_HELLO) - before) >= 2);
    { MLog *m = lastlog(&S, MT_HELLO); CHECK(m && (((uint16_t) m->p[7] << 8) | m->p[8]) == REF_DEFAULT_TABEL_CRC); } }
  /* de volgorde van de 4 kenmerken i.v.m. bit 1 (standaardtabel) */
  { MLog *m = lastlog(&S, MT_HELLO); CHECK(m && (m->p[11] == 0) && ((m->p[12] & 0x02) == 0x02)); }
  (void) i;
}

/* ---------- 12. tijdbudget: nooit later dan 700 ms ---------- */
static const uint8_t RMAX[2][5] = {
  { 5, DIR_TK, 0x02, 150, 0 },    /* 600 ms wachten op de ESP, dan PASS */
  { 5, DIR_KT, 0x02, 150, 0 },    /* en nog eens 600 ms op het ketelantwoord */
};

static void test_budget(void) {
  uint8_t pl[100], n, d; uint32_t el, worst = 0;
  static const uint16_t delays[] = { 20, 50, 100, 300, 450, 650, 5000 };
  sect("tijdbudget: antwoord aan de thermostaat uiterlijk 700 ms, alle instellingen op hun maximum");
  for (d = 0; d < 7; d++) {
    uint8_t silent = (uint8_t) (delays[d] == 5000);
    sim_init(&S, 0xFFFFFC00UL, 0); sim_run(&S, 200, 1);
    n = mk_tabel(pl, 1, 600, 600, 600, 1800, 900, RMAX, 2);
    install(pl, n);
    S.bo_delay = delays[d]; S.bo_silent = silent;
    el = transactie(F(MSG_READ_DATA, 5, 0), 1000);          /* de ESP antwoordt nooit */
    CHECK(S.st_n == 1);
    CHECK(el >= 20 && el <= 703);
    if (el > worst) worst = el;
  }
  HOSTPRINT("  langste responstijd: %lu ms\n", (unsigned long) worst);
  /* standaardwaarden: ESP zwijgt (250 ms), ketel traag (tot 400 ms): ook binnen het budget */
  sim_init(&S, 1000, 0); sim_run(&S, 200, 1);
  { static const uint8_t r1[1][5] = { { 5, DIR_TK, 0x02, 0, 0 } };
    n = mk_tabel(pl, 1, 250, 400, 60, 240, 300, r1, 1); install(pl, n);
    S.bo_silent = 1;
    el = transactie(F(MSG_READ_DATA, 5, 0), 1000);
    CHECK(S.st_n == 1 && el <= 703 && el >= 600);            /* 250 ms + 400 ms wachten, geklemd op 700 */
  }
  /* ESP antwoordt op het laatste moment (599 ms) */
  sim_init(&S, 1000, 0); sim_run(&S, 200, 1);
  n = mk_tabel(pl, 1, 600, 600, 600, 1800, 900, RMAX, 2); install(pl, n);
  S.bo_delay = 30;
  thermo(&S, F(MSG_READ_DATA, 5, 0)); sim_run(&S, 598, 1);
  { MLog *m = lastlog(&S, MT_VRAAG); uint8_t vs = m ? m->seq : 0; antwoord(vs, 0, 0); }
  sim_run(&S, 500, 1);
  CHECK(S.st_n == 1 && (S.st_t[0] - S.t_thermo) <= 703);
  /* de OT-ondergrens van 20 ms voor lokaal gegenereerde antwoorden */
  sim_init(&S, 1000, 0); sim_run(&S, 200, 1);
  { static const uint8_t rb[1][5] = { { 5, DIR_TK, 0x03, 0, 7 } };
    n = mk_tabel(pl, 1, 250, 400, 60, 240, 300, rb, 1); install(pl, n);
    el = transactie(F(MSG_READ_DATA, 5, 0), 300);
    CHECK(S.st_n == 1 && el >= 40 && el <= 45);
  }
  sim_init(&S, 1000, 1); sim_run(&S, 200, 1);               /* aan/uit: zelfstandig antwoord */
  el = transactie(F(MSG_READ_DATA, 0, 0x0300), 300);
  CHECK(S.st_n == 1 && el >= 40 && el <= 45 && S.bo_n == 0);
  CHECK(ot_type(S.st_f[0]) == MSG_READ_ACK && ot_id(S.st_f[0]) == 0 && (ot_val(S.st_f[0]) & 0x02));
  /* pass-through is altijd >= 20 ms (34 ms zenden + ketel) */
  sim_init(&S, 1000, 0); sim_run(&S, 200, 1);
  S.bo_delay = 34;
  el = transactie(F(MSG_READ_DATA, 17, 0), 300);
  CHECK(el >= 20);
}

/* ---------- 13. relais-interlock onder stress ---------- */
static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525UL + 1013904223UL; return rng_state >> 8; }

static void test_interlock(void) {
  uint32_t i; uint16_t bridge_n = 0, k3_n = 0, both = 0;
  sect("relais: K3 en noodbrug nooit samen (stress)");
  sim_init(&S, 1000, 0);
  for (i = 0; i < 20000UL; i++) {
    uint32_t r = rnd();
    if ((r & 0x3FF) < 6) S.dip = (uint8_t) !S.dip;
    if ((r & 0x3FF) == 0x3FF) {
      uint8_t f1 = (uint8_t) ((r >> 10) & 0x13), f2 = (uint8_t) (((r >> 14) & 0x10) | ((r >> 16) & 0x03) << 6);
      if ((f2 >> 6) == 3) f2 &= 0x3F;
      cmd(f1 & 0x11, f2 | C2_DIRECT, 10, 0, 0, 0, 0, (uint8_t) (10 + ((r >> 20) & 0x3F)));
    }
    if ((r & 0xFF) == 0x7F) S.b.esp_ms -= ((r >> 10) & 1) ? 300000UL : 1UL;   /* soms verdwijnt de ESP voor lang */
    if ((r & 0xFF) == 0x55) thermo(&S, F(MSG_WRITE_DATA, 1, (uint16_t) (((r >> 12) & 1) ? 0x2D00 : 0x0A00)));
    sim_step(&S, 7);
    if (S.b.bridge_on) bridge_n++;
    if (S.b.k3_on) k3_n++;
    if (S.b.bridge_on && S.b.k3_on) both++;
  }
  CHECK(both == 0 && S.inv_viol == 0);
  CHECK(bridge_n > 100 && k3_n > 100);                        /* de test heeft beide toestanden echt bezocht */
  HOSTPRINT("  brug %u stappen, K3 %u stappen, samen %u\n", bridge_n, k3_n, both);

  /* 50 ms tussen het ene uit en het andere aan */
  sim_init(&S, 1000, 1);
  cmd(0, C2_DEMAND, 10, 0, 0, 0, 0, 255); transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 50); sim_run(&S, 20, 1);
  CHECK(S.b.k3_on == 1);
  cmd(C1_BRUG, C2_DEMAND | C2_KT_OT, 10, 0, 0, 0, 0, 255);
  sim_step(&S, 1);
  CHECK(S.b.k3_on == 0 && S.b.bridge_on == 0);               /* K3 uit, de brug wacht */
  sim_run(&S, 40, 1);
  CHECK(S.b.bridge_on == 0);
  sim_run(&S, 20, 1);
  CHECK(S.b.bridge_on == 1 && S.b.k3_on == 0);
}

/* ---------- 14. events, injectie, overrides ---------- */
static void test_events_inject(void) {
  uint8_t p[5], i, ev0 = 0, ev1 = 0, ev2 = 0, ev3 = 0;
  sect("EVENT's, INJECT naar de ketel, overrides");
  sim_init(&S, 2000, 0); sim_run(&S, 200, 1);
  S.nlog = 0; S.wr = 0;
  transactie(F(MSG_WRITE_DATA, 17, 0x1200), 100);
  sim_run(&S, 100, 1);
  for (i = 0; i < S.nlog; i++) if (S.log[i].type == MT_EVENT) {
    uint8_t d = S.log[i].p[0];
    if (d == 0) ev0++; if (d == 1) ev1++; if (d == 2) ev2++; if (d == 3) ev3++;
  }
  CHECK(ev0 == 1 && ev1 == 1 && ev2 == 1 && ev3 == 1);

  /* INJECT naar de ketel */
  { uint32_t f = F(MSG_WRITE_DATA, 27, 0x0880);
    p[0] = 0; p[1] = (uint8_t) (f >> 24); p[2] = (uint8_t) (f >> 16); p[3] = (uint8_t) (f >> 8); p[4] = (uint8_t) f;
    S.st_n = 0; S.bo_n = 0;
    esp_send(&S, MT_INJECT, p, 5);
    sim_run(&S, 400, 1);
    CHECK(S.bo_n == 1 && S.bo_f[0] == f && S.st_n == 0);          /* het antwoord blijft bij ons */
    p[0] = 0; p[1] = 0x70; esp_send(&S, MT_INJECT, p, 5);          /* type 7: geweigerd */
    CHECK(S.b.ack_n >= 1);
  }
  /* de ketel is bezig met een vraag van de thermostaat: injectie wacht */
  S.bo_delay = 100; S.st_n = 0; S.bo_n = 0;
  thermo(&S, F(MSG_READ_DATA, 17, 0));
  sim_run(&S, 5, 1);
  { uint32_t f = F(MSG_WRITE_DATA, 27, 0x0880);
    p[0] = 0; p[1] = (uint8_t) (f >> 24); p[2] = (uint8_t) (f >> 16); p[3] = (uint8_t) (f >> 8); p[4] = (uint8_t) f;
    esp_send(&S, MT_INJECT, p, 5); }
  sim_run(&S, 600, 1);
  CHECK(S.bo_n == 2 && S.st_n == 1 && ot_id(S.bo_f[0]) == 17 && ot_id(S.bo_f[1]) == 27);
  CHECK(S.bo_t[1] - S.bo_t[0] >= 100 + 100);                        /* eerst antwoord + 100 ms master-pauze */

  /* override van het antwoord (modus 1) en opheffen (modus 2) */
  cmd(0, 0, 10, 0, 0, 0, 0, 255);
  { uint32_t f = F(MSG_READ_ACK, 27, 0x0A00);
    p[0] = 1; p[1] = (uint8_t) (f >> 24); p[2] = (uint8_t) (f >> 16); p[3] = (uint8_t) (f >> 8); p[4] = (uint8_t) f;
    esp_send(&S, MT_INJECT, p, 5);
    transactie(F(MSG_READ_DATA, 27, 0), 300);
    CHECK(S.st_n == 1 && ot_val(S.st_f[0]) == 0x0A00 && ot_type(S.st_f[0]) == MSG_READ_ACK);
    p[0] = 2; esp_send(&S, MT_INJECT, p, 5);
    transactie(F(MSG_READ_DATA, 27, 0), 300);
    CHECK(S.st_f[0] == reply_for(S.bo_f[0]));
    /* acht overrides passen, de negende niet */
    { uint8_t j;
      for (j = 0; j < 9; j++) { p[0] = 1; p[1] = 0x40; p[2] = (uint8_t) (30 + j); p[3] = 0; p[4] = (uint8_t) j; S.b.ack_n = 0; esp_send(&S, MT_INJECT, p, 5); }
      CHECK(S.b.ack_n == 1 && S.b.ack[0].status == ST_BEZET); }
  }
}

/* ---------- 15. zonder thermostaat: keep-alive-poll ---------- */
static void test_poll(void) {
  sect("zonder thermostaat: poll met DHW-bit, CH alleen op verzoek van de ESP");
  sim_init(&S, 1000, 0);
  sim_run(&S, 2500, 1);
  CHECK(S.bo_n >= 2);
  CHECK(S.bo_n >= 1 && ot_type(S.bo_f[0]) == MSG_READ_DATA && ot_id(S.bo_f[0]) == 0 && ot_val(S.bo_f[0]) == 0x0200);
  S.bo_n = 0;
  {
    uint8_t p[15], k;
    mk_cmd(p, C1_REM, 0, 10, 0, 0, 0, 0, 0, 0, 55, 0, 255);
    esp_send(&S, MT_COMMANDO, p, 15);
    sim_run(&S, 3500, 1);
    { uint8_t seen_w = 0, seen_ch = 0;
      for (k = 0; k < S.bo_n && k < 6; k++) {
        if (ot_type(S.bo_f[k]) == MSG_WRITE_DATA && ot_id(S.bo_f[k]) == 1 && ot_val(S.bo_f[k]) == tenths_to_f88(550)) seen_w = 1;
        if (ot_type(S.bo_f[k]) == MSG_READ_DATA && ot_id(S.bo_f[k]) == 0 && ot_val(S.bo_f[k]) == 0x0300) seen_ch = 1;
      }
      CHECK(seen_w && seen_ch); }
  }
}

/* ---------- 16. TX-wachtrij: prioriteit en pauze ---------- */
static void test_tx(void) {
  uint32_t el; MLog *m;
  sect("TX-wachtrij: pauze van 5 ms en VRAAG achter een STATUS");
  vraag_setup();
  sim_run(&S, 6000, 1);
  CHECK(S.gap_viol == 0 && S.tx_bad == 0);
  /* een STATUS (38 bytes) begint net voordat de VRAAG aan de beurt komt */
  S.b.status_due_ms = S.now + 2;
  sim_run(&S, 2, 1);                                      /* STATUS begint */
  thermo(&S, F(MSG_READ_DATA, 5, 0));
  sim_run(&S, 40, 1);
  m = lastlog(&S, MT_VRAAG);
  CHECK(m != 0);
  el = m ? (uint32_t) (m->t - S.t_thermo) : 999;
  CHECK(el <= 16);
  HOSTPRINT("  VRAAG-vertraging achter een STATUS: %lu ms\n", (unsigned long) el);
  CHECK(S.gap_viol == 0 && S.tx_bad == 0);
}

/* ---------- 17. STATUS-inhoud vs wat de ESP verwacht ---------- */
static void test_status(void) {
  MLog *m;
  sect("STATUS: waarnemingen en vlaggen");
  sim_init(&S, 1000, 0); sim_run(&S, 100, 1);
  transactie(F(MSG_WRITE_DATA, 1, 0x2D00), 100);
  transactie(F(MSG_WRITE_DATA, 24, 0x1480), 100);        /* kamertemperatuur 20,5 */
  transactie(F(MSG_WRITE_DATA, 16, 0x1500), 100);        /* kamersetpoint 21,0 */
  S.b.status_due_ms = S.now; sim_run(&S, 40, 1);
  m = lastlog(&S, MT_STATUS);
  CHECK(m && m->len == 33);
  if (m) {
    CHECK(m->p[2] == 45);                                 /* gevraagd setpoint, voor de rem */
    CHECK((int16_t) (((uint16_t) m->p[3] << 8) | m->p[4]) == 205);
    CHECK((int16_t) (((uint16_t) m->p[5] << 8) | m->p[6]) == 210);
    CHECK((m->p[11] & 0x01) && (m->p[11] & 0x02) && (m->p[11] & 0x04) && !(m->p[11] & 0x08));   /* thermostaat, ketel, rem; geen beleid */
    CHECK((m->p[12] & 0x02) == 0 && (m->p[12] & 0x04) == 0);
  }
}

/* ---------- main ---------- */
#ifdef __AVR__
void __attribute__((noinline)) test_end(void) { __asm__ __volatile__("nop"); }
#endif
int main(void) {
  test_crc_en_vectoren();
  test_parser();
  test_tabel();
  test_commando();
  test_opstart_noodbrug();
  test_lange_tijden();
  test_standaard_regels();
  test_rem();
  test_ketelantwoord();
  test_vraag_antwoord();
  test_tabelwissel();
  test_hello_en_terugval();
  test_budget();
  test_interlock();
  test_events_inject();
  test_poll();
  test_tx();
  test_status();
  HOSTPRINT("\nGESLAAGD: %d, MISLUKT: %d\n", t_pass, t_fail);
  t_done = 1;
#ifdef __AVR__
  test_end();
  for (;;) { }
#else
  return t_fail ? 1 : 0;
#endif
}
