// ╔═══════════════════════════════════════════════════════════════════════╗
// ║  avr-bridge-v2 - de OpenTherm-brug voor de IC2-socket                 ║
// ║  ATmega328P @ 8 MHz, drop-in voor de originele "Haddon T1.1"          ║
// ╚═══════════════════════════════════════════════════════════════════════╝
//
// Versie 2.0.0. Thermostaat en CV-ketel voeren hun normale OpenTherm-gesprek; wij
// zitten er als doorgeefluik tussen, net als v1. Wat nieuw is:
//
//   * een regeltabel per Data-ID en richting (PASS / VAST / VRAAG_ESP / BLOKKEER)
//     die de ESP32 stuurt en die bij een reboot vanzelf weer op de AVR komt;
//   * VRAAG/ANTWOORD: de AVR kan een bericht vasthouden en de ESP32 vragen wat er
//     mee moet gebeuren, met volgnummer en deadline;
//   * link op 38400 baud met CRC-8, HELLO-handshake en instelbare tijden;
//   * veilige opstart: geremd, CH-vlag onderdrukt, noodbrug pas na 240 s;
//   * een verlaat ketelantwoord kan niet meer in de transactie belanden.
//
// De logica zit NIET in dit bestand maar in de gewone C-bestanden ernaast
// (proto.c, tabel.c, brug.c), die ook op de PC getest worden. Dit bestand is alleen
// de hardware: pinnen, de Manchester-tik van v1 (byte voor byte ongewijzigd),
// de UART en de lus. Zie PROTOCOL.md en README.md.
//
// ── Bouwen ────────────────────────────────────────────────────────────────
//   fqbn arduino:avr:pro:cpu=8MHzatmega328   (zie README.md)

#include <string.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include "brug.h"

#define VERSIE_TEKST "2.0.0"

// ══════════════════════════════════════════════════════════════════════════
//  Watchdog (zoals v1: in .init3, voordat main() draait)
// ══════════════════════════════════════════════════════════════════════════
void wdt_early_disable(void) __attribute__((naked, used, section(".init3")));
uint8_t mcusr_saved __attribute__((section(".noinit")));
uint8_t boot_count  __attribute__((section(".noinit")));

void wdt_early_disable(void) {
  mcusr_saved = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

#if F_CPU != 8000000UL
#error "Verkeerde klok. Alle OpenTherm-timing hangt aan 8 MHz."
#endif

// ══════════════════════════════════════════════════════════════════════════
//  Pintoewijzing IC2-socket (DIP28), ongewijzigd t.o.v. v1
// ══════════════════════════════════════════════════════════════════════════
//   socket-pin 5  = PD3   OpenTherm naar de CV-ketel        (uitgang)
//   socket-pin 11 = PD5   OpenTherm van de CV-ketel         (ingang)
//   socket-pin 6  = PD4   OpenTherm naar de thermostaat     (uitgang)
//   socket-pin 13 = PD7   OpenTherm van de thermostaat      (ingang, AIN1)
//   socket-pin 2  = PD0   38400 baud van de ESP32
//   socket-pin 3  = PD1   38400 baud naar de ESP32
//   socket-pin 16 = PB2   K1+K2, de noodbrug
//   socket-pin 28 = PC5   K3, het aan/uit-ketelcontact
//   socket-pin 27 = PC4   statuslampje
//   socket-pin 25 = PC2   dipschakelaar 2 (laag = OpenTherm-ketel, hoog = aan/uit-ketel)

#define TX_BOILER_BIT   PD3
#define RX_BOILER_BIT   PD5
#define TX_STAT_BIT     PD4
// RX thermostaat = AIN1 = PD7, via de comparator (geen bit-define nodig)

// Rustniveau van de zendlijnen. 1 = hoog, zoals PORTD = 0x19 in het origineel.
#define OT_IDLE_LEVEL   1

// ══════════════════════════════════════════════════════════════════════════
//  OpenTherm op tickniveau - BYTE VOOR BYTE UIT v1 (avr-bridge.ino regel 88-357)
// ══════════════════════════════════════════════════════════════════════════
//
// Deze tak is op hardware bevestigd (thermostaat- en ketelkant) en is daarom niet
// aangeraakt. Zie v1 voor de onderbouwing van de codering (sub_0892 van het origineel).

static const uint8_t  TICKS_PER_BIT   = 8;
// Drempel tussen een halve bit (~4 tikken) en een hele (~8). Precies de
// waarde die het origineel gebruikt: CPI r16, 0x06 op 0x02d2 en 0x0300.
static const uint8_t  LONG_INTERVAL   = 6;
// Zo lang stilte betekent dat het frame is afgebroken.
static const uint8_t  RX_GIVE_UP      = 16;
static const uint8_t  FRAME_BITS      = 34;
// 1000 cycli = 125,0 us bij 8 MHz. In CTC-stand, dus de teller reset zichzelf
// in hardware bij een compare match.
//
// Het origineel herlaadde TCNT1 in software met 0xFC3A = 966 cycli, en dat is
// met de hand afgeregeld op zijn eigen ISR: 966 plus ~34 cycli interruptlatentie
// en PUSH's komt op 1000 uit. Onze ISR bedient twee kanalen en kost er zo'n 290,
// dus diezelfde waarde geeft een tik van ~157 us - 25% te traag. Het startbit
// lees je dan nog net goed en het stopbit klopt per ongeluk omdat je tegen die
// tijd de rustlijn leest, maar alles ertussenin is rommel: complete frames met
// een kapotte pariteit.
//
// In CTC hangt de periode niet meer aan de lengte van de ISR.
static const uint16_t TIMER1_TOP      = 999;

enum { CH_STAT = 0, CH_BOILER = 1, N_CH = 2 };

struct OtChannel {
  // Zenden. tx_curbit is de bitwaarde van de lopende bit, aan het begin
  // daarvan al bepaald - zie de ISR voor waarom dat moet.
  volatile uint8_t  tx_active;
  volatile uint32_t tx_shift;
  volatile uint8_t  tx_curbit;
  volatile uint8_t  tx_bit;
  volatile uint8_t  tx_tick;

  // Ontvangen. Flankinterval-decodering, zie de ISR.
  volatile uint8_t  rx_active;
  volatile uint32_t rx_shift;
  volatile uint8_t  rx_bit;
  volatile uint8_t  rx_ticks;     // tikken sinds de vorige flank
  volatile uint8_t  rx_prev_mid;  // was de vorige flank een midden-overgang?
  volatile uint8_t  rx_prev;

  // Resultaat
  volatile uint8_t  rx_ready;
  volatile uint32_t rx_frame;
  volatile uint16_t rx_errors;
  // Frames die compleet binnenkwamen maar zijn weggegooid omdat het vorige
  // nog niet was opgehaald. Er past er maar een tegelijk, en bij een snelle
  // master kan dat gebeuren.
  volatile uint8_t  rx_overruns;

  // Ruwe flankenteller. Los van alle decodering: hiermee zie je of er
  // uberhaupt iets op de lijn staat. Nul betekent een dode bus - dan is het
  // geen decodeerprobleem maar voeding, bedrading of de verkeerde pin.
  volatile uint8_t  rx_edges;
  volatile uint8_t  rx_frames;   // compleet ontvangen frames sinds vorig bericht

  // Leesrichting. De comparator geeft met de bandgap op de plus-ingang het
  // OMGEKEERDE van het pinniveau, maar of de interface-elektronica dat weer
  // terugdraait weten we niet. Vandaar omzetbaar vanuit Home Assistant in
  // plaats van een aanname in de code.
  volatile uint8_t  rx_invert;

  // Zendpolariteit. Eén vlag dekt zowel het rustniveau als de bitcodering:
  // alles omkeren draait beide om. Het origineel idlet hoog en codeert een
  // 1 als "eerste helft laag", en daar gaan we van uit - maar of de driver
  // op de print er nog een trap tussen zet weten we niet, en dat is niet uit
  // de firmware te halen.
  volatile uint8_t  tx_invert;
};

static OtChannel ch[N_CH];

// Kanalen omwisselen. Onze rolverdeling volgt uit de bufferadressen in de
// originele firmware (0x89 = Read-Ack = slave = thermostaat op PD4/AIN1,
// 0x8d = Read/Write-Data = master = ketel op PD3/PD5). WackoH's pintabel
// zegt iets anders. Zolang dat niet met signaal op beide kanten bewezen is,
// moet je het kunnen omdraaien zonder te branden.
static volatile uint8_t swap_ch;

static inline uint8_t phys(uint8_t c) { return swap_ch ? (uint8_t) (1 - c) : c; }

// Beide kanten normaliseren naar "rust = 1". Let op: de twee ontvangstwegen
// hebben in het origineel TEGENGESTELDE polariteit, en dat is fysiek logisch -
// de comparator keert om (bandgap op de plus-ingang, signaal op de min), een
// digitale ingang niet.
//
//   thermostaat  0x029a  SBIC ACSR,5 -> RJMP weg    rust = ACO 1, start bij 0
//   ketel        0x03ac  SBIS PIND,5 -> RJMP weg    rust = PD5 0, start bij 1
//
// Vandaar dat de ketelkant hier omgekeerd wordt gelezen.
static inline uint8_t ot_read(uint8_t c) {
  if (phys(c) == CH_STAT) return (ACSR & _BV(ACO)) ? 1 : 0;
  return (PIND & _BV(RX_BOILER_BIT)) ? 0 : 1;
}

// GEMETEN EIGENSCHAP VAN DE PRINT, geen instelling.
//
// De twee kanten hebben tegengestelde hardware. De ketelkant gaat via een
// optocoupler (PD3 -> R13 -> LED). De thermostaatkant via een discrete keten:
//
//   PD4 hoog -> T1 dicht -> Q4 dicht -> geen 18 V op de bus
//   PD4 laag -> T1 open  -> Q4 open  -> 18 V op de bus
//
// T1 is een NPN met zijn basis vast op AVCC en zijn emitter aan PD4 via R24;
// hij geleidt pas als die emitter onder ~4,3 V komt. Q4 is een PNP met zijn
// emitter op de 18 V, dus die gaat open als zijn basis omlaag wordt getrokken.
//
// Netto: op de thermostaatkant betekent "pin hoog" het tegenovergestelde van
// wat het op de ketelkant betekent. Precies zoals de ontvangstkanten ook al
// tegengesteld waren - comparator tegen digitale ingang. Zelfde oorzaak.
//
// Op hardware bevestigd: zonder deze omkering valt het gesprek met de
// thermostaat stil.
#define TX_INVERT_STAT   1
#define TX_INVERT_BOILER 0

static inline void ot_write(uint8_t c, uint8_t level) {
  uint8_t inv = ch[c].tx_invert;
  inv ^= (c == CH_STAT) ? TX_INVERT_STAT : TX_INVERT_BOILER;
  if (inv) level = (uint8_t) !level;
  const uint8_t bit = (phys(c) == CH_STAT) ? _BV(TX_STAT_BIT) : _BV(TX_BOILER_BIT);
  if (level) PORTD |= bit;
  else       PORTD &= (uint8_t) ~bit;
}

// Beide zendlijnen terugzetten op rust. Nodig zodra de polariteit of de
// kanaalindeling verandert, want in rust schrijft de ISR niets - dan zou een
// omgezette vlag pas bij het volgende frame effect hebben.
static void ot_idle_all() {
  for (uint8_t c = 0; c < N_CH; c++)
    if (!ch[c].tx_active) ot_write(c, OT_IDLE_LEVEL);
}

// ── De tick-ISR: beide kanalen aan dezelfde tijdbasis ─────────────────────
// Hoe vaak de ISR nog bezig was toen de volgende tik al aanbrak. In CTC-stand
// wist de hardware OCF1A bij het binnengaan van de vector; staat hij aan het
// eind weer aan, dan hebben we een tik gemist. Dat is geen schatting maar een
// meting, en een gemiste tik verstoort zowel zenden als ontvangen.
static volatile uint8_t isr_overruns;

ISR(TIMER1_COMPA_vect) {
  for (uint8_t c = 0; c < N_CH; c++) {
    OtChannel &q = ch[c];

    // ── Zenden ──
    if (q.tx_active) {
      // Eerste helft draagt de inverse, tweede helft de bitwaarde zelf.
      uint8_t want = (q.tx_tick < TICKS_PER_BIT / 2) ? (uint8_t) !q.tx_curbit
                                                     : q.tx_curbit;
      ot_write(c, OT_IDLE_LEVEL ? want : (uint8_t) !want);

      if (++q.tx_tick >= TICKS_PER_BIT) {
        q.tx_tick = 0;
        if (++q.tx_bit >= FRAME_BITS) {
          q.tx_active = 0;
          ot_write(c, OT_IDLE_LEVEL);
        } else if (q.tx_bit == FRAME_BITS - 1) {
          q.tx_curbit = 1;                    // stopbit
        } else {
          // Schuifregister in plaats van een schuif over een variabel aantal
          // plaatsen: dat laatste is op een AVR een lus van tientallen cycli
          // per bit, en twee kanalen passen dan niet in een tik van 966.
          q.tx_curbit = (q.tx_shift & 0x80000000UL) ? 1 : 0;
          q.tx_shift <<= 1;
        }
      }
    }

    // ── Ontvangen ──
    uint8_t lvl = ot_read(c);
    if (!OT_IDLE_LEVEL) lvl = !lvl;   // normaliseer naar "idle = 1"
    if (q.rx_invert) lvl = (uint8_t) !lvl;

    if (lvl != q.rx_prev && !q.tx_active && q.rx_edges < 255) q.rx_edges++;

    // ── Flankinterval-decodering, zoals het origineel ──
    //
    // Niet bemonsteren op een vast moment in de bit, maar de tijd TUSSEN
    // flanken meten. In Manchester is elk interval ofwel een halve bit
    // (~4 tikken) ofwel een hele (~8). Elke flank synchroniseert opnieuw, dus
    // een klokafwijking stapelt niet op over de 34 bits van een frame.
    //
    // Mijn eerdere ontvanger synchroniseerde één keer op het startbit en
    // bemonsterde daarna blind. Die werkte, tot hij net niet meer werkte - en
    // was overgevoelig voor polariteit en timing. Het origineel is dat niet,
    // en dit is waarom.
    //
    // Regel: na een midden-overgang is de volgende flank weer een midden als
    // het interval lang was, en een bitgrens als het kort was. Na een
    // bitgrens is de volgende altijd een midden. De bitwaarde is het niveau
    // NA de midden-overgang.
    if (q.rx_ticks < 255) q.rx_ticks++;

    if (lvl != q.rx_prev) {
      const uint8_t iv = q.rx_ticks;
      q.rx_ticks = 0;

      if (!q.rx_active) {
        // Het startbit is een 1, dus zijn eerste helft is LAAG: de neergaande
        // flank vanuit rust is de bitgrens waar het frame begint. Niet
        // meeluisteren terwijl we zelf zenden - dan hoor je je eigen echo.
        if (!q.tx_active && q.rx_prev == 1 && lvl == 0) {
          q.rx_active   = 1;
          q.rx_bit      = 0;
          q.rx_shift    = 0;
          q.rx_prev_mid = 0;        // dit was een grens, geen midden
        }
      } else {
        const uint8_t is_mid = q.rx_prev_mid ? (iv >= LONG_INTERVAL) : 1;
        q.rx_prev_mid = is_mid;

        if (is_mid) {
          const uint8_t b = lvl;
          if (q.rx_bit == 0) {
            if (!b) { q.rx_active = 0; q.rx_errors++; }   // startbit moet 1 zijn
          } else if (q.rx_bit < FRAME_BITS - 1) {
            q.rx_shift = (q.rx_shift << 1) | b;
          } else {
            if (!b) {
              q.rx_errors++;                                 // stopbit moet 1 zijn
            } else if (q.rx_ready) {
              if (q.rx_overruns < 255) q.rx_overruns++;      // vorige nog niet op
            } else {
              q.rx_frame = q.rx_shift;
              q.rx_ready = 1;
              if (q.rx_frames < 15) q.rx_frames++;
            }
            q.rx_active = 0;
          }
          if (q.rx_active) q.rx_bit++;
        }
      }
    } else if (q.rx_active && q.rx_ticks > RX_GIVE_UP) {
      q.rx_active = 0;              // lijn te lang stil: frame afgebroken
      q.rx_errors++;
    }
    q.rx_prev = lvl;
  }

  if (TIFR1 & _BV(OCF1A)) { if (isr_overruns < 255) isr_overruns++; }
}


// ── Zenden en ontvangen van een frame (v1, plus ot_flush) ─────────────────

static void ot_send(uint8_t c, uint32_t frame) {
  uint8_t s = SREG; cli();
  ch[c].tx_shift  = frame;
  ch[c].tx_curbit = 1;      // startbit is altijd 1
  ch[c].tx_bit    = 0;
  ch[c].tx_tick   = 0;
  ch[c].tx_active = 1;
  SREG = s;
}

static bool ot_take(uint8_t c, uint32_t *out) {
  bool got = false;
  uint8_t s = SREG; cli();
  if (ch[c].rx_ready) { *out = ch[c].rx_frame; ch[c].rx_ready = 0; got = true; }
  SREG = s;
  return got;
}

// NIEUW t.o.v. v1: wis wat de ontvanger als klaar heeft staan. Wordt voor elk frame
// naar de ketel aangeroepen, zodat een verlaat ketelantwoord nooit meer bij de
// volgende vraag terechtkomt (review 5a). Ook brug_ot() gooit ketelframes weg die
// buiten een wachtstand binnenkomen; dit is de tweede borging.
static void ot_flush(uint8_t c) {
  uint8_t s = SREG; cli();
  ch[c].rx_ready = 0;
  SREG = s;
}

// ══════════════════════════════════════════════════════════════════════════
//  UART met de ESP32: 38400 baud 8N1, eigen interrupts
// ══════════════════════════════════════════════════════════════════════════
//
// De OpenTherm-tik (Timer1) heeft een eigen vector; AVR-interrupts nesten niet,
// dus een UART-interrupt kan de tik hooguit een paar microseconden vertragen (zie
// README, onderbrekingsbelasting). De ISR's zijn bewust minimaal: een byte in of
// uit een rij, verder niets.
//
// Geen Serial: de ringbuffers van de Arduino-core zijn 64 bytes, de grootste TABEL
// is 99 bytes, en de core-ISR's zijn zwaarder dan nodig.

#define RXN 64                       // macht van twee
static volatile uint8_t rx_buf[RXN];
static volatile uint8_t rx_head, rx_tail;
static volatile uint8_t uart_ovr;    // verloren of foute bytes

static uint8_t          tx_buf[PROTO_MAX_MSG];
static volatile uint8_t tx_len, tx_idx;

ISR(USART_RX_vect) {
  const uint8_t st = UCSR0A;         // eerst de status, dan pas de data
  const uint8_t d  = UDR0;
  const uint8_t h  = rx_head;
  const uint8_t n  = (uint8_t) ((h + 1) & (RXN - 1));
  if (st & (_BV(FE0) | _BV(DOR0))) { if (uart_ovr < 255) uart_ovr++; if (st & _BV(FE0)) return; }
  if (n == rx_tail) { if (uart_ovr < 255) uart_ovr++; return; }
  rx_buf[h] = d;
  rx_head = n;
}

ISR(USART_UDRE_vect) {
  const uint8_t i = tx_idx;
  UDR0 = tx_buf[i];
  tx_idx = (uint8_t) (i + 1);
  if ((uint8_t) (i + 1) >= tx_len) UCSR0B &= (uint8_t) ~_BV(UDRIE0);
}

static void uart_init() {
  UCSR0A = _BV(U2X0);                        // UBRR=25 met U2X: 38461 baud, 0,16 % fout
  UBRR0H = 0;
  UBRR0L = 25;
  UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);        // 8N1
  UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
}

static inline bool uart_tx_idle() { return !(UCSR0B & _BV(UDRIE0)); }

static void uart_tx_start(uint8_t n) {
  tx_idx = 0; tx_len = n;
  UCSR0B |= _BV(UDRIE0);
}

// ══════════════════════════════════════════════════════════════════════════
//  Glue: kern <-> hardware
// ══════════════════════════════════════════════════════════════════════════

static Brug    br;
static Parser  par;
static OtIn    ot_in;
static OtOut   ot_out;
static bool    lamp_invert;          // hardware-aanname, blijft staan als het beleid vervalt
static uint8_t last_pol;             // 0x80 | vlaggen zoals toegepast, voor ot_idle_all

// Momentopname van de hardwaretellers voor STATUS (zoals v1 link_send_status).
static void hw_snap(HwSnap *h) {
  uint8_t s = SREG; cli();
  h->edges_stat   = ch[CH_STAT].rx_edges;   ch[CH_STAT].rx_edges   = 0;
  h->edges_boiler = ch[CH_BOILER].rx_edges; ch[CH_BOILER].rx_edges = 0;
  h->frames_stat  = ch[CH_STAT].rx_frames;  ch[CH_STAT].rx_frames  = 0;
  h->rx_err_stat   = (uint8_t) (ch[CH_STAT].rx_errors   > 255 ? 255 : ch[CH_STAT].rx_errors);
  h->rx_err_boiler = (uint8_t) (ch[CH_BOILER].rx_errors > 255 ? 255 : ch[CH_BOILER].rx_errors);
  h->isr_ovr  = isr_overruns;
  h->uart_ovr = uart_ovr;
  SREG = s;
  h->lines = (uint8_t) (((ACSR & _BV(ACO))            ? 0x01 : 0) |
                        ((PIND & _BV(RX_BOILER_BIT))  ? 0x02 : 0) |
                        ((PORTD & _BV(TX_STAT_BIT))   ? 0x04 : 0) |
                        ((PORTD & _BV(TX_BOILER_BIT)) ? 0x08 : 0) |
                        (swap_ch                      ? 0x10 : 0) |
                        (ch[CH_STAT].tx_invert        ? 0x20 : 0) |
                        (ch[CH_BOILER].tx_invert      ? 0x40 : 0));
  h->spare = (uint8_t) (((PINC & _BV(PC0)) ? 0x01 : 0) |
                        ((PINC & _BV(PC1)) ? 0x02 : 0) |
                        ((PINC & _BV(PC3)) ? 0x04 : 0));
}

// Statuslampje op socket-pin 27 (PC4), zoals v1.
static void lamp_update() {
  uint8_t on;
  if (br.pol.valid) lamp_invert = br.pol.lamp_inv;
  switch (br.pol.lamp_mode) {              // 0 als het beleid ongeldig is
    case 1:  on = 0; break;
    case 2:  on = 1; break;
    case 3:  on = (uint8_t) ((millis() >> 9) & 1); break;
    case 4:  on = (uint8_t) ((millis() >> 7) & 1); break;
    default:
      if (br.obs.slave_status & 0x01)  on = 1;                               // storing
      else if (!br.obs.stat_alive)     on = (uint8_t) ((millis() >> 7) & 1); // geen thermostaat
      else if (!br.pol.valid)          on = (uint8_t) ((millis() >> 9) & 1); // geen ESP32
      else                             on = 0;
      break;
  }
  if (lamp_invert) on = !on;
  if (on) PORTC |=  _BV(PC4);
  else    PORTC &= (uint8_t) ~_BV(PC4);
}

static void uitgangen_bijwerken() {
  // Relais: eerst wat uit moet, dan wat aan moet. De kern garandeert al dat nooit
  // beide aan zijn; dit is de tweede borging op pinniveau.
  if (!br.k3_on)     PORTC &= (uint8_t) ~_BV(PC5);
  if (!br.bridge_on) PORTB &= (uint8_t) ~_BV(PB2);
  if (br.bridge_on && !br.k3_on) PORTB |= _BV(PB2);
  if (br.k3_on && !br.bridge_on) PORTC |= _BV(PC5);

  const uint8_t t = br.pol.test_out;       // 0 als het beleid ongeldig is
  if (t & 0x01) PORTB |=  _BV(PB0); else PORTB &= (uint8_t) ~_BV(PB0);
  if (t & 0x02) PORTB |=  _BV(PB1); else PORTB &= (uint8_t) ~_BV(PB1);
  if (t & 0x04) PORTB |=  _BV(PB3); else PORTB &= (uint8_t) ~_BV(PB3);
  if (t & 0x08) PORTB |=  _BV(PB4); else PORTB &= (uint8_t) ~_BV(PB4);
  if (t & 0x10) PORTD |=  _BV(PD2); else PORTD &= (uint8_t) ~_BV(PD2);
}

static void polariteit_bijwerken() {
  ch[CH_STAT].rx_invert   = br.rx_inv_stat;
  ch[CH_BOILER].rx_invert = br.rx_inv_boiler;
  const uint8_t nu = (uint8_t) (0x80 | (br.tx_inv_stat ? 1 : 0) | (br.tx_inv_boiler ? 2 : 0) | (br.swap_ch ? 4 : 0));
  if (nu != last_pol) {
    ch[CH_STAT].tx_invert   = br.tx_inv_stat;
    ch[CH_BOILER].tx_invert = br.tx_inv_boiler;
    swap_ch                 = br.swap_ch;
    ot_idle_all();                         // rustniveaus opnieuw zetten, zoals v1
    last_pol = nu;
  }
}

// ══════════════════════════════════════════════════════════════════════════

void setup() {
  // ── Poort-init letterlijk gelijk aan het origineel en v1 ──
  //   DDRB = 0x1F  PORTB = 0x00   PB0..PB4 uitgang, alle laag  -> K1/K2 (PB2) LOS
  //   DDRC = 0x30  PORTC = 0xCF   PC4/PC5 uitgang laag         -> K3 (PC5) OPEN
  //   DDRD = 0x1E  PORTD = 0x19   PD1..PD4 uitgang; PD0 pull-up, PD3/PD4 hoog
  DDRB  = 0x1F;  PORTB = 0x00;
  DDRC  = 0x30;  PORTC = 0xCF;
  DDRD  = 0x1E;  PORTD = 0x19;

  ot_write(CH_BOILER, OT_IDLE_LEVEL);
  ot_write(CH_STAT,   OT_IDLE_LEVEL);

  for (uint8_t c = 0; c < N_CH; c++) {
    memset((void *) &ch[c], 0, sizeof(OtChannel));
    ch[c].rx_prev = 1;
  }

  uart_init();

  // Comparator met de interne bandgap op de plus-ingang, AIN1 (PD7) op de min (v1).
  ACSR = _BV(ACBG);
  delayMicroseconds(100);
  ACSR = _BV(ACBG) | _BV(ACI);

  // Timer1 in CTC zonder prescaler: exact 125 us per tik (v1).
  TCCR1A = 0;
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A  = TIMER1_TOP;
  TCNT1  = 0;
  TIMSK1 = _BV(OCIE1A);

  if (mcusr_saved & _BV(PORF)) boot_count = 0;
  else                         boot_count++;

  char d[12];
  strcpy_P(d, PSTR(__DATE__));
  uint8_t bouw[3];
  brug_bouwdatum(d, bouw);

  const uint32_t nu = millis();
  brug_init(&br, nu, (uint8_t) (mcusr_saved & 0x0F), boot_count,
            (uint8_t) ((PINC & _BV(PC2)) ? 1 : 0), bouw, hw_snap);
  parser_init(&par, nu);

  sei();
  wdt_enable(WDTO_2S);
}

void loop() {
  wdt_reset();
  const uint32_t now = millis();

  // 1. Bytes van de ESP32 -> parser -> kern
  while (rx_tail != rx_head) {
    const uint8_t b = rx_buf[rx_tail];
    rx_tail = (uint8_t) ((rx_tail + 1) & (RXN - 1));
    if (parser_feed(&par, b, now))
      brug_rx_msg(&br, now, par.type, par.seq, par.buf, par.len);
  }
  {
    const uint16_t e = (uint16_t) par.err_crc + par.err_len + par.err_timeout;
    br.cnt_crc = (uint8_t) (e > 255 ? 255 : e);
  }

  // 2. OpenTherm: wat de ISR binnenhaalde door de kern laten beslissen
  ot_in.have_stat   = ot_take(CH_STAT,   &ot_in.stat);
  ot_in.have_boiler = ot_take(CH_BOILER, &ot_in.boiler);
  brug_ot(&br, now, &ot_in, &ot_out);
  if (ot_out.flush_boiler) ot_flush(CH_BOILER);
  if (ot_out.flush_stat)   ot_flush(CH_STAT);
  if (ot_out.send_boiler)  ot_send(CH_BOILER, ot_out.boiler_frame);
  if (ot_out.send_stat)    ot_send(CH_STAT,   ot_out.stat_frame);

  // 3. Timers, rem, relais
  brug_tick(&br, now, (uint8_t) ((PINC & _BV(PC2)) ? 1 : 0));
  polariteit_bijwerken();
  uitgangen_bijwerken();
  lamp_update();

  // 4. Naar de ESP32 (alleen als de vorige klaar is; de kern bewaakt de pauze van 5 ms)
  if (uart_tx_idle()) {
    const uint8_t n = brug_tx_next(&br, now, tx_buf);
    if (n) uart_tx_start(n);
  }
}
