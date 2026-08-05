// ╔═══════════════════════════════════════════════════════════════════════╗
// ║  avr-bridge - de OpenTherm-brug voor de IC2-socket                    ║
// ║  ATmega328P @ 8 MHz, drop-in voor de originele "Haddon T1.1"          ║
// ╚═══════════════════════════════════════════════════════════════════════╝
//
// Thermostaat en CV-ketel voeren hun normale OpenTherm-gesprek; wij zitten er
// als doorgeefluik tussen. Alles gaat door - tapwater, storingen, modulatie,
// onbekende Data-ID's. De grip zit in twee substituties onderweg:
//
//   richting de ketel        ID 1  (Control Setpoint)  -> de rem
//   richting de thermostaat  ID 25 en 28              -> temperaturen van de WP
//
// De ESP32 in de IC1-socket zet die substituties via een parameterlink op
// 9600 baud. Valt hij weg, dan gaat alles ongewijzigd door en blijft het huis
// warm. Dat terugvalgedrag hoort juist hier en niet daar.
//
// Zie ../../docs/volledige-controlbox.md voor de onderbouwing.
//
// ── STATUS ────────────────────────────────────────────────────────────────
// NIET OP HARDWARE GETEST, maar het protocolgedeelte is niet gegokt: de
// codering hieronder is regel voor regel uit `sub_0892` van de originele T1.1
// gehaald. Zie de tabel bij "OpenTherm op tickniveau".
//
// Wat de firmware NIET kan zeggen, is wat er aan de andere kant van die
// pinnen op de print gebeurt. De MCU weet welke pin hij aanstuurt; of het
// spoor van socket-pin 6 werkelijk bij een OT-klem uitkomt is een printvraag.
// WackoH mat pin 6 als NC en pin 13 als massa, wij leiden uit de firmware
// twee volwaardige kanalen af. Draai de defines om als `avr-diag` hem gelijk
// geeft - dat is de enige aanname die nog open staat.
//
// De polariteit hoeft trouwens niet gemeten te worden. Wij hangen aan
// dezelfde interface-elektronica als het origineel, dus het volstaat om op
// pinniveau exact hetzelfde te doen. Wat die elektronica er vervolgens van
// maakt, doet niet ter zake.
//
// ── Bouwen ────────────────────────────────────────────────────────────────
//   fqbn arduino:avr:pro:cpu=8MHzatmega328   (zie ../README.md)

#include <string.h>
#include <avr/wdt.h>

// ══════════════════════════════════════════════════════════════════════════
//  Watchdog
// ══════════════════════════════════════════════════════════════════════════
//
// Deze chip zit straks in een dichte controlbox: geen resetknop die iets doet,
// geen seriele monitor, geen manier om erbij te komen. Loopt hij vast, dan
// staat de thermostaat zonder ketel en zonder warmtepomp. Een watchdog van
// twee seconden kost vier regels en haalt dat scenario weg.
//
// Het uitzetten moet in .init3 gebeuren, dus voordat main() draait. Na een
// watchdog-reset blijft WDE namelijk staan met de kortste tijd van 16 ms, en
// dan kom je nooit ver genoeg om hem in setup() alsnog te herconfigureren -
// je krijgt een resetlus die van buiten niet van een dode chip te
// onderscheiden is. Wij hebben geen bootloader die dit voor ons opruimt.
void wdt_early_disable(void) __attribute__((naked, used, section(".init3")));
// Bewaard voordat we MCUSR wissen: hiermee weten we WAAROM de chip startte.
// In .noinit, want die sectie overleeft een reset.
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
//  Pintoewijzing IC2-socket (DIP28)
// ══════════════════════════════════════════════════════════════════════════
//
//   socket-pin 5  = PD3   OpenTherm naar de CV-ketel        (uitgang)
//   socket-pin 11 = PD5   OpenTherm van de CV-ketel         (ingang)
//   socket-pin 6  = PD4   OpenTherm naar de thermostaat     (uitgang)
//   socket-pin 13 = PD7   OpenTherm van de thermostaat      (ingang, AIN1)
//   socket-pin 2  = PD0   9600 baud van de ESP32
//   socket-pin 3  = PD1   9600 baud naar de ESP32
//
// Pin 13 lezen we via de analoge comparator met de interne bandgap, precies
// zoals het origineel. Dat is geen stijlkeuze: het signaal komt van een deler
// en haalt de digitale drempel van 0,6 x VCC waarschijnlijk niet, dus een
// digitale ingang ziet daar niets - ook als er volop verkeer op staat.

#define TX_BOILER_BIT   PD3
#define RX_BOILER_BIT   PD5
#define TX_STAT_BIT     PD4
// RX thermostaat = AIN1 = PD7, via de comparator (geen bit-define nodig)

// Rustniveau van de zendlijnen. 1 = hoog, zoals PORTD = 0x19 in het origineel.
#define OT_IDLE_LEVEL   1

// ══════════════════════════════════════════════════════════════════════════
//  OpenTherm op tickniveau
// ══════════════════════════════════════════════════════════════════════════
//
// 1000 bit/s Manchester: 1 ms per bit, 500 us per halve bit. Timer1 tikt op
// 125 us, dus 8 tikken per bit en 4 per helft - dezelfde tijdbasis die het
// origineel gebruikt (TCNT1-herlaadwaarde 0xFC3A).
//
// De codering is uit `sub_0892` van het origineel gelezen, niet aangenomen:
//
//   PORTD = 0x19          rust: PD3 en PD4 HOOG
//   SBRS r23,7 -> CBI     databit 1 -> eerste helft LAAG
//               -> SBI    databit 0 -> eerste helft HOOG
//   CPI 0x04              toggle halverwege, op tik 4
//   CPI 0x08              volgende bit op tik 8
//   SBRS r23,7 + LSL      MSB eerst
//
// De tweede helft draagt dus de bitwaarde. We bemonsteren op tik 6, ruim weg
// van beide flanken.
//
// Frame: 1 startbit (altijd 1), 32 databits, 1 stopbit (altijd 1) = 34 bits.

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

// ══════════════════════════════════════════════════════════════════════════
//  Frame-indeling
// ══════════════════════════════════════════════════════════════════════════
//   bit 31     even pariteit over de rest
//   bit 30-28  berichttype
//   bit 27-24  ongebruikt
//   bit 23-16  Data-ID
//   bit 15-0   waarde

#define MSG_READ_DATA   0
#define MSG_WRITE_DATA  1
#define MSG_READ_ACK    4
#define MSG_WRITE_ACK   5
#define MSG_DATA_INVALID 6
#define MSG_UNKNOWN_ID  7

#define ID_STATUS            0
#define ID_CONTROL_SETPOINT  1
#define ID_SLAVE_CONFIG      3
#define ID_REMOTE_OVERRIDE   9
#define ID_OVERRIDE_FUNC   100
#define ID_FAULT_FLAGS       5
#define ID_MODULATION       17
#define ID_ROOM_SETPOINT    16
#define ID_ROOM_TEMP        24
#define ID_BOILER_TEMP      25
#define ID_RETURN_TEMP      28

static inline uint8_t  msg_type(uint32_t f) { return (uint8_t) ((f >> 28) & 0x07); }
static inline uint8_t  data_id(uint32_t f)  { return (uint8_t) ((f >> 16) & 0xFF); }
static inline uint16_t data_val(uint32_t f) { return (uint16_t) (f & 0xFFFF); }

static bool parity_ok(uint32_t f) {
  f ^= f >> 16; f ^= f >> 8; f ^= f >> 4; f ^= f >> 2; f ^= f >> 1;
  return (f & 1) == 0;
}

static uint32_t with_parity(uint32_t f) {
  f &= 0x7FFFFFFFUL;
  uint32_t t = f;
  t ^= t >> 16; t ^= t >> 8; t ^= t >> 4; t ^= t >> 2; t ^= t >> 1;
  if (t & 1) f |= 0x80000000UL;
  return f;
}

static uint32_t replace_value(uint32_t f, uint16_t v) {
  return with_parity((f & 0x7FFF0000UL) | v);
}

// f8.8: getekend, 1/256 graad. Wij rekenen in tienden.
static inline uint16_t tenths_to_f88(int16_t t)  { return (uint16_t) (((int32_t) t * 256) / 10); }
static inline int16_t  f88_to_tenths(uint16_t v) { return (int16_t) (((int32_t) (int16_t) v * 10) / 256); }

// ══════════════════════════════════════════════════════════════════════════
//  Parameterlink met de ESP32
// ══════════════════════════════════════════════════════════════════════════

static const uint8_t  CMD_START      = 0xF1;
static const uint8_t  CMD_LEN        = 16;
static const uint8_t  STATUS_START   = 0xF0;
static const uint8_t  STATUS_LEN     = 24;
static const uint32_t LINK_TIMEOUT_MS = 60000;
static const uint32_t STATUS_EVERY_MS = 2000;

// ── Doorgifte van elk OpenTherm-frame ─────────────────────────────────────
//
// Het statusbericht hierboven is een samenvatting: acht waarden die deze chip
// zelf uitpakt. Dat was kortzichtig. Elke keer dat je een Data-ID erbij wilt -
// retourtemperatuur, buitentemperatuur, bedrijfsuren - moet de chip uit de voet
// en opnieuw gebrand. En de rest van het gesprek gooiden we gewoon weg.
//
// Daarom gaat nu ELK frame dat hier langskomt ook rauw naar de ESP32, in beide
// richtingen, inclusief wat wij er zelf van maken. Uitpakken gebeurt daar, en
// dat is een OTA in plaats van een burn.
//
//   F2 <richting> <b3> <b2> <b1> <b0> <som>
//
// De richting vertelt of het een vraag of een antwoord was, en van wie:
static const uint8_t  EVENT_START    = 0xF2;
static const uint8_t  EVENT_LEN      = 7;

// ── En andersom: de ESP32 mag zelf frames laten versturen ─────────────────
//
// Zonder dit moet elke nieuwe injectie hier ingebakken worden, en dat is
// precies de fout die we hierboven net rechtgezet hebben. Nu is de brug een
// pijp: wat er verstuurd wordt bepaalt de ESP32.
//
//   F3 <modus> <b3> <b2> <b1> <b0> <som>
//
// Twee richtingen, en die werken wezenlijk anders:
//
//   modus 0  NAAR DE KETEL. Wij zijn daar master, dus we mogen uit onszelf
//            praten. Het frame gaat in een gaatje tussen twee thermostaat-
//            transacties door, en het antwoord houden we voor onszelf.
//
//   modus 1  NAAR DE THERMOSTAAT. Daar zijn wij slave en mag je niets uit
//            jezelf sturen; je mag alleen antwoorden. Dit zet dus geen frame
//            klaar maar een ANTWOORD: vraagt de thermostaat dit Data-ID, dan
//            krijgt ze deze waarde in plaats van die van de ketel.
//
//   modus 2  Die override weer opheffen voor het Data-ID in het frame.
static const uint8_t  INJECT_START   = 0xF3;
static const uint8_t  INJECT_LEN     = 7;

// Wachtrij richting de ketel. Vier is ruim: je injecteert hooguit een frame
// per minuut, en meer dan dat verstoort het gesprek met de thermostaat.
static uint32_t inj_ring[4];
static uint8_t  inj_head, inj_tail;

// Antwoord-overrides richting de thermostaat. Acht plekken; dat is genoeg voor
// alles wat je praktisch wilt vervangen, en een tabel van 256 zou hier een
// kwart van het RAM opeten.
struct OtOverride { uint8_t id; uint16_t val; bool used; };
static OtOverride ovr[8];

static bool ovr_lookup(uint8_t id, uint16_t *out) {
  for (uint8_t i = 0; i < 8; i++)
    if (ovr[i].used && ovr[i].id == id) { *out = ovr[i].val; return true; }
  return false;
}

static void ovr_set(uint8_t id, uint16_t val) {
  for (uint8_t i = 0; i < 8; i++)          // bestaande plek bijwerken
    if (ovr[i].used && ovr[i].id == id) { ovr[i].val = val; return; }
  for (uint8_t i = 0; i < 8; i++)          // anders een vrije plek
    if (!ovr[i].used) { ovr[i].id = id; ovr[i].val = val; ovr[i].used = true; return; }
}

static void ovr_clear(uint8_t id) {
  for (uint8_t i = 0; i < 8; i++)
    if (ovr[i].used && ovr[i].id == id) ovr[i].used = false;
}

#define EV_FROM_STAT    0   // de thermostaat vroeg dit
#define EV_FROM_BOILER  1   // de ketel antwoordde dit
#define EV_TO_BOILER    2   // dit stuurden wij door naar de ketel
#define EV_TO_STAT      3   // dit antwoordden wij de thermostaat

// Ringbuffer, want zenden mag nooit in het doorgeefpad blijven hangen: bij
// 9600 baud kost een bericht 7 ms en dat is zes OpenTherm-bits. We schrijven
// alleen weg als de UART-buffer ruimte heeft.
struct OtEvent { uint8_t dir; uint32_t frame; };
static OtEvent ev_ring[12];
static uint8_t ev_head, ev_tail;

static uint8_t sum8(const uint8_t *p, uint8_t n);   // staat verderop

static void ev_push(uint8_t dir, uint32_t frame) {
  const uint8_t next = (uint8_t) ((ev_head + 1) % 12);
  if (next == ev_tail) return;   // vol: liever een gat dan de brug ophouden
  ev_ring[ev_head].dir   = dir;
  ev_ring[ev_head].frame = frame;
  ev_head = next;
}

static void ev_flush() {
  while (ev_tail != ev_head && Serial.availableForWrite() >= EVENT_LEN) {
    const OtEvent &e = ev_ring[ev_tail];
    uint8_t b[EVENT_LEN];
    b[0] = EVENT_START;
    b[1] = e.dir;
    b[2] = (uint8_t) (e.frame >> 24);
    b[3] = (uint8_t) (e.frame >> 16);
    b[4] = (uint8_t) (e.frame >> 8);
    b[5] = (uint8_t) e.frame;
    b[6] = sum8(b, EVENT_LEN - 1);
    Serial.write(b, EVENT_LEN);
    ev_tail = (uint8_t) ((ev_tail + 1) % 12);
  }
}

// Vlaggen in het commandobericht
#define CMD_FLAG_BRAKE        0x01
#define CMD_FLAG_SUBST_TEMP   0x02
#define CMD_FLAG_INV_STAT     0x04
#define CMD_FLAG_INV_BOILER   0x08
#define CMD_FLAG_RELAY        0x10
#define CMD_FLAG_LAMP_INV     0x20
#define CMD_FLAG_SA_FLAME     0x40
#define CMD_FLAG_FORCE_SA     0x80

// Tweede vlaggenbyte: de hardware-aannames die nog niet bewezen zijn.
#define CMD2_INV_TX_STAT      0x01
#define CMD2_INV_TX_BOILER    0x02
#define CMD2_SWAP_CHANNELS    0x04
#define CMD2_RELAY_FOLLOWS    0x08
#define CMD2_DEMAND_ENABLE    0x10

struct Policy {
  bool     brake_on;
  bool     subst_temp;
  uint8_t  brake_setpoint;   // hele graden
  int16_t  wp_supply;        // tienden
  int16_t  wp_return;        // tienden

  // Kamersetpoint dat we de thermostaat opdringen via Data-ID 9, in tienden.
  // 0 = geen override, dan regelt hij weer zelf. Dit is het standaardmechanisme
  // uit OpenTherm en precies hoe de bekende OT-gateway het doet, dus je hebt er
  // geen merkintegratie voor nodig.
  int16_t  override_setpoint;

  uint8_t  lamp_mode;        // 0 auto, 1 uit, 2 aan, 3 traag, 4 snel
  uint8_t  sa_modulation;    // wat we in de zelfstandige stand als modulatie melden
  bool     sa_flame;         // idem voor de vlam-vlag: levert de warmtepomp?
  bool     force_standalone; // zelf antwoorden ook als de ketel wel meepraat
  bool     relay_cmd;        // stand die Home Assistant vraagt
  bool     relay_follows;    // of het relais in plaats daarvan de dipswitch volgt
  bool     demand_enabled;   // mag K3 de aan/uit-ketel aanzetten?
  uint8_t  boiler_setpoint;  // rechtstreeks setpoint naar de ketel, 0 = uit
  uint32_t received_ms;
  bool     valid;
};

static Policy policy;

// Staat van de rem. Hier gedeclareerd omdat het statusbericht hem meestuurt;
// de logica eromheen staat verderop bij "De rem".
static bool     brake_applied;

// Stand van de noodbrug. Hier omdat demand_update() hem nodig heeft voor de
// interlock, en die functie staat eerder in het bestand dan relay_update().
static bool     relay_bridge_on;
static uint32_t brake_changed_ms;

// Waarnemingen die we naar de ESP32 sturen.
struct Observed {
  // Data-ID 0 draagt twee statusbytes: de hoge is van de master (wat de
  // thermostaat wil), de lage van de slave (wat de ketel doet). Die moeten
  // apart blijven - ze in een byte persen kost je precies de bits waar het
  // om gaat: warmtevraag aan de ene kant, vlam en storing aan de andere.
  uint8_t  master_status;
  uint8_t  slave_status;
  uint8_t  requested_setpoint;  // wat de thermostaat vraagt, VOOR de rem
  int16_t  room_temp;
  int16_t  room_setpoint;
  int16_t  boiler_temp;
  uint8_t  modulation;
  uint8_t  fault_code;
  bool     stat_alive;
  bool     boiler_alive;
};

static Observed obs;

// Link-diagnose. In de controlbox is de ESP32 je enige venster, dus dit is
// geen extraatje maar de enige manier om te zien of deze chip nog leeft.
static uint16_t link_seq;
static uint16_t link_bad_sum;
static uint8_t  parity_fails;
static uint32_t last_stat_frame_ms;
static uint32_t last_boiler_frame_ms;

// ── Statuslampje op socket-pin 27 (PC4) ───────────────────────────────────
//
// Dit is het lampje op de voorkant. Het zit op T1.1 en niet op M1.1 - vandaar
// dat het niet meer werkte zodra de originele chip eruit ging.
//
// Polariteit uit de firmware: de init zet PORTC = 0xCF, dus PC4 begint LAAG,
// en op 0x0b6c gaat hij ook laag zodra de foutcode op nul komt. Laag = uit.
// Mocht de LED-driver er toch tussen zitten, dan is het om te zetten vanuit
// Home Assistant zonder opnieuw te branden.
//
// Het origineel knippert hier foutcodes uit. Wij houden vier toestanden aan,
// want juist als je telefoon niets laat zien moet dit lampje iets zeggen:
//
//   uit            alles in orde
//   snel (~4 Hz)   geen thermostaat aan de lijn
//   traag (~1 Hz)  geen ESP32 - de brug draait in terugvalstand
//   aan            storing gemeld door de ketel
static bool lamp_invert;

static void lamp_update() {
  uint8_t on;
  switch (policy.lamp_mode) {
    case 1:  on = 0; break;
    case 2:  on = 1; break;
    case 3:  on = (uint8_t) ((millis() >> 9) & 1); break;
    case 4:  on = (uint8_t) ((millis() >> 7) & 1); break;
    default:
      // Automatisch. Deze tak moet blijven werken als de ESP32 wegvalt, want
      // dan is dit lampje het enige dat nog iets kan zeggen.
      if (obs.slave_status & 0x01)   on = 1;
      else if (!obs.stat_alive)      on = (uint8_t) ((millis() >> 7) & 1);
      else if (!policy.valid)        on = (uint8_t) ((millis() >> 9) & 1);
      else                           on = 0;
      break;
  }

  if (lamp_invert) on = !on;
  if (on) PORTC |=  _BV(PC4);
  else    PORTC &= (uint8_t) ~_BV(PC4);
}

// ── Relais op socket-pin 16, en de schakelaar op socket-pin 25 ────────────
//
// Het origineel koppelt die twee rechtstreeks:
//
//   0a94  SBIS PINC, 2     ; schakelaar laag
//   0a96  SBI  PORTB, 2    ; -> relais aan
//
// Dat gedrag was hier verdwenen omdat het relais alleen vanuit Home Assistant
// werd gezet. Nu allebei mogelijk, want welke van de twee je wilt hangt af van
// wat er fysiek aan die schakelaar hangt - en dat weten we nog niet.
//
// PC2 krijgt een pull-up, net als in het origineel (PORTC = 0xCF zet PC0..PC3
// hoog). De schakelaar trekt hem dus laag.
// ── Ketelvraag-relais K3 op socket-pin 28 (PC5) ───────────────────────────
//
// DOORGEMETEN, en anders dan ik eerst dacht. Dit is geen statusuitgang maar de
// aansturing van K3, een Omron G2R-1 met een wisselcontact van 10 A:
//
//   PC5 -- R -- basis T4 -- collector -- spoel K3
//                        -- emitter   -- frontschakelaar -- massa
//
// Tien ampere is volstrekt overbodig voor een signaallijn. Dit is het droge
// contact waarmee je een AAN/UIT-ketel aanzet - Atlantic's tweede keteltype
// naast OpenTherm. De frontschakelaar in de emitter is de vrijgave.
//
// De drempel komt uit het origineel:
//
//   0944  CPI r16, 0x01    ; Data-ID 1, Control Setpoint
//   094c  CPI r16, 0x14    ; >= 20 graden?
//   0950  SBI PORTC, 5     ; ja  -> hoog
//   0954  CBI PORTC, 5     ; nee -> laag
//
// Een aan/uit-warmtevraag naast het modulerende OT-commando. Wij leiden hem
// af uit dezelfde waarde die we naar de ketel sturen, inclusief de rem: knijp
// je de vraag af, dan valt deze lijn ook weg.
static const uint8_t DEMAND_THRESHOLD_C = 20;

// ── Losse uitgangen om mee te proberen ────────────────────────────────────
//
// Het origineel stelt vijf pinnen in als uitgang en houdt ze permanent laag.
// Inmiddels doorgemeten op deze print:
//
//   socket-pin  4  = PD2      socket-pin 17 = PB3
//   socket-pin 14  = PB0      socket-pin 18 = PB4
//   socket-pin 15  = PB1
//
// Alle vijf eindigen blind: het spoor houdt gewoon op. Dat past bij een
// codebasis die over meerdere modellen gedeeld wordt - de firmware stelt ze in,
// deze print gebruikt ze niet. Ze blijven hier staan omdat het niets kost en ze
// op een andere printversie wel bedraad kunnen zijn.
static uint8_t test_out;

static void test_out_update() {
  if (test_out & 0x01) PORTB |=  _BV(PB0); else PORTB &= (uint8_t) ~_BV(PB0);
  if (test_out & 0x02) PORTB |=  _BV(PB1); else PORTB &= (uint8_t) ~_BV(PB1);
  if (test_out & 0x04) PORTB |=  _BV(PB3); else PORTB &= (uint8_t) ~_BV(PB3);
  if (test_out & 0x08) PORTB |=  _BV(PB4); else PORTB &= (uint8_t) ~_BV(PB4);
  if (test_out & 0x10) PORTD |=  _BV(PD2); else PORTD &= (uint8_t) ~_BV(PD2);
}

static void demand_update() {
  // INTERLOCK. Alle drie de relaisspoelen hangen aan een gedeelde
  // voorschakelweerstand R27 van 100 ohm vanaf de 18 V. Met K3 en de noodbrug
  // tegelijk aan zakt de spoelspanning naar zo'n 8,6 V, en dan trekt er niets
  // meer betrouwbaar aan. Atlantic heeft die combinatie nooit bedoeld: het is
  // OF een OpenTherm-ketel OF een aan/uit-ketel.
  //
  // De noodbrug wint, want die houdt het huis warm als wij eruit liggen.
  if (relay_bridge_on) { PORTC &= (uint8_t) ~_BV(PC5); return; }

  uint8_t sp = brake_applied ? policy.brake_setpoint : obs.requested_setpoint;
  if (policy.demand_enabled && sp >= DEMAND_THRESHOLD_C) PORTC |=  _BV(PC5);
  else                                                   PORTC &= (uint8_t) ~_BV(PC5);
}

// Het relais is in het origineel een AUTOMATISCHE NOODOVERBRUGGING, geen
// bediening. Teller 0x6c is een waakhond op de link met IC1: elk geldig
// telegram zet hem op nul en het relais uit (0x01ea, direct na de
// checksum-controle). Bereikt hij 240 - dus lang niets meer van de hoofdchip -
// dan trekt sub_0a24 het relais aan en hangt de thermostaat rechtstreeks aan
// de ketel. Valt de controlbox uit, dan blijft het huis warm.
//
// Dat gedrag nemen we over: zwijgt de ESP32, dan overbruggen we. Onze
// linktimeout van 60 s doet hetzelfde werk als hun teller.
static void relay_update() {
  bool on;
  if (policy.relay_follows) on = (PINC & _BV(PC2)) == 0;
  else if (!policy.valid)   on = true;              // ESP32 stil -> noodstand
  else                      on = policy.relay_cmd;

  relay_bridge_on = on;
  if (on) PORTB |=  _BV(PB2);
  else    PORTB &= (uint8_t) ~_BV(PB2);
}

static uint8_t sum8(const uint8_t *p, uint8_t n) {
  uint8_t s = 0;
  while (n--) s = (uint8_t) (s + *p++);
  return s;
}

static void link_poll() {
  static uint8_t buf[CMD_LEN];
  static uint8_t idx;
  static uint8_t want;

  while (Serial.available()) {
    uint8_t b = (uint8_t) Serial.read();

    // Twee soorten berichten van de ESP32: het parameterbericht en een los
    // frame dat wij moeten versturen. Het startbyte bepaalt de lengte.
    if (idx == 0) {
      if      (b == CMD_START)    want = CMD_LEN;
      else if (b == INJECT_START) want = INJECT_LEN;
      else                        continue;
    }
    buf[idx++] = b;
    if (idx < want) continue;
    const uint8_t len = want;
    idx = 0;

    if (sum8(buf, len - 1) != buf[len - 1]) { link_bad_sum++; continue; }

    if (len == INJECT_LEN) {
      const uint32_t f = ((uint32_t) buf[2] << 24) | ((uint32_t) buf[3] << 16) |
                         ((uint32_t) buf[4] << 8)  | (uint32_t) buf[5];
      const uint8_t  id = (uint8_t) (f >> 16);
      switch (buf[1]) {
        case 0: {                                  // naar de ketel
          const uint8_t next = (uint8_t) ((inj_head + 1) % 4);
          if (next != inj_tail) { inj_ring[inj_head] = f; inj_head = next; }
          break;
        }
        case 1: ovr_set(id, (uint16_t) (f & 0xFFFF)); break;   // antwoord vervangen
        case 2: ovr_clear(id); break;
      }
      continue;
    }

    policy.brake_on       = (buf[1] & CMD_FLAG_BRAKE) != 0;
    policy.subst_temp     = (buf[1] & CMD_FLAG_SUBST_TEMP) != 0;
    ch[CH_STAT].rx_invert   = (buf[1] & CMD_FLAG_INV_STAT) != 0;
    ch[CH_BOILER].rx_invert = (buf[1] & CMD_FLAG_INV_BOILER) != 0;

    // Relais op socket-pin 16 (PB2). In het origineel gaat dit gelijk op met
    // de vraag of de controlbox in het circuit zit: staat het relais af, dan
    // stopt T1.1 ook met rapporteren aan IC1 (sub_0a24 zet bit 6 van r25
    // alleen als PB2 hoog is, en sub_0b7e zendt niet zonder dat bit).
    // Dat gedraagt zich als een omschakelaar, niet als warmtevraag.
    policy.relay_cmd     = (buf[1] & CMD_FLAG_RELAY) != 0;
    policy.relay_follows  = (buf[2] & CMD2_RELAY_FOLLOWS) != 0;
    policy.demand_enabled = (buf[2] & CMD2_DEMAND_ENABLE) != 0;
    policy.boiler_setpoint = buf[13];
    lamp_invert = (buf[1] & CMD_FLAG_LAMP_INV) != 0;
    // Tweede vlaggenbyte. Verandert er iets aan de polariteit of de
    // kanaalindeling, dan meteen de rustniveaus opnieuw zetten.
    const uint8_t f2 = buf[2];
    const uint8_t was = (uint8_t) ((ch[CH_STAT].tx_invert   ? 1 : 0) |
                                   (ch[CH_BOILER].tx_invert ? 2 : 0) |
                                   (swap_ch                 ? 4 : 0));
    ch[CH_STAT].tx_invert   = (f2 & CMD2_INV_TX_STAT) != 0;
    ch[CH_BOILER].tx_invert = (f2 & CMD2_INV_TX_BOILER) != 0;
    swap_ch                 = (f2 & CMD2_SWAP_CHANNELS) != 0;
    if ((f2 & 0x07) != was) ot_idle_all();

    policy.brake_setpoint = buf[3];
    policy.wp_supply      = (int16_t) ((buf[4] << 8) | buf[5]);
    policy.wp_return      = (int16_t) ((buf[6] << 8) | buf[7]);
    policy.override_setpoint = (int16_t) ((buf[8] << 8) | buf[9]);
    policy.lamp_mode      = buf[10];
    policy.sa_modulation  = buf[11];
    test_out              = buf[12];
    policy.sa_flame       = (buf[1] & CMD_FLAG_SA_FLAME) != 0;
    policy.force_standalone = (buf[1] & CMD_FLAG_FORCE_SA) != 0;
    policy.received_ms    = millis();
    policy.valid          = true;
  }

  // Terugvallen als de ESP32 zwijgt. Dit is het gedrag dat het huis warm
  // houdt wanneer daar iets misgaat, dus het hoort hier.
  if (policy.valid && (millis() - policy.received_ms) > LINK_TIMEOUT_MS) {
    policy.valid            = false;
    policy.brake_on         = false;
    policy.subst_temp       = false;
    policy.override_setpoint = 0;      // thermostaat mag weer zelf regelen
    policy.force_standalone = false;
    policy.lamp_mode        = 0;       // lampje terug naar automatisch
  }
}

static void link_send_status() {
  uint8_t b[STATUS_LEN];
  b[0]  = STATUS_START;
  b[1]  = obs.master_status;   // thermostaat: CH aan, DHW aan, koelen, OTC
  b[2]  = obs.slave_status;    // ketel: storing, CH-modus, DHW-modus, vlam
  b[3]  = obs.requested_setpoint;
  b[4]  = (uint8_t) (obs.room_temp >> 8);
  b[5]  = (uint8_t) obs.room_temp;
  b[6]  = (uint8_t) (obs.room_setpoint >> 8);
  b[7]  = (uint8_t) obs.room_setpoint;
  b[8]  = (uint8_t) (obs.boiler_temp >> 8);
  b[9]  = (uint8_t) obs.boiler_temp;
  b[10] = obs.modulation;
  b[11] = obs.fault_code;
  b[12] = (uint8_t) ((obs.stat_alive   ? 0x01 : 0) |
                     (obs.boiler_alive ? 0x02 : 0) |
                     (brake_applied    ? 0x04 : 0) |
                     (policy.valid     ? 0x08 : 0) |
                     ((link_seq & 0x0F) << 4));

  // Ruwe flanken sinds het vorige statusbericht, per kant. Dit staat los van
  // alle decodering en scheidt de twee soorten storing: nul flanken is een
  // dode bus (voeding, bedrading, verkeerde pin), flanken zonder geldige
  // frames wijst op de decodering of de polariteit.
  uint8_t s = SREG; cli();
  b[13] = ch[CH_STAT].rx_edges;   ch[CH_STAT].rx_edges   = 0;
  b[14] = ch[CH_BOILER].rx_edges; ch[CH_BOILER].rx_edges = 0;
  // Lage nibble: complete frames. Hoge nibble: daarvan afgekeurd op pariteit.
  // Dat scheidt "er komt niets door" van "het komt door maar klopt niet".
  b[15] = (uint8_t) ((ch[CH_STAT].rx_frames & 0x0F) | ((parity_fails & 0x0F) << 4));
  ch[CH_STAT].rx_frames = 0;
  parity_fails = 0;

  // Ruwe lijnstanden op dit moment. Hiermee zie je zonder multimeter of een
  // bus in rust hoog of laag staat - en dus of er uberhaupt spanning op zit.
  b[16] = (uint8_t) (((ACSR & _BV(ACO))            ? 0x01 : 0) |
                     ((PIND & _BV(RX_BOILER_BIT))  ? 0x02 : 0) |
                     ((PORTD & _BV(TX_STAT_BIT))   ? 0x04 : 0) |
                     ((PORTD & _BV(TX_BOILER_BIT)) ? 0x08 : 0) |
                     (swap_ch                      ? 0x10 : 0) |
                     (ch[CH_STAT].tx_invert        ? 0x20 : 0) |
                     (ch[CH_BOILER].tx_invert      ? 0x40 : 0));

  b[17] = (uint8_t) (ch[CH_STAT].rx_errors   > 255 ? 255 : ch[CH_STAT].rx_errors);
  b[18] = (uint8_t) (ch[CH_BOILER].rx_errors > 255 ? 255 : ch[CH_BOILER].rx_errors);
  SREG = s;

  // Schakelaar en relais, ruw. Hiermee is te zien of het relais werkelijk
  // schakelt en of de knop op de voorkant iets doet.
  b[19] = (uint8_t) (((PINC & _BV(PC2))  ? 0 : 0x01) |   // laag = ingedrukt
                     ((PORTB & _BV(PB2)) ? 0x02 : 0) |
                     (policy.relay_follows ? 0x04 : 0) |
                     ((PORTC & _BV(PC5)) ? 0x08 : 0));
  // De drie ongebruikte ingangen met pull-up. Beweegt daar iets, dan hangt er
  // een schakelaar aan die we nog niet kennen.
  b[20] = (uint8_t) (((PINC & _BV(PC0)) ? 0x01 : 0) |
                     ((PINC & _BV(PC1)) ? 0x02 : 0) |
                     ((PINC & _BV(PC3)) ? 0x04 : 0));
  // Waarom de AVR startte, en hoe vaak sinds de laatste koude start. Een
  // watchdog-reset ziet er van buiten uit als "het werkte en toen niet meer",
  // en zonder dit is dat niet van een protocolprobleem te onderscheiden.
  //   bit 0 PORF  bit 1 EXTRF  bit 2 BORF  bit 3 WDRF
  b[21] = (uint8_t) ((mcusr_saved & 0x0F) | ((boot_count & 0x0F) << 4));

  uint8_t s2 = SREG; cli();
  b[22] = isr_overruns;
  SREG = s2;

  b[23] = sum8(b, STATUS_LEN - 1);
  link_seq++;
  Serial.write(b, STATUS_LEN);
}

// ══════════════════════════════════════════════════════════════════════════
//  De rem
// ══════════════════════════════════════════════════════════════════════════
//
// Kortsluitbeveiliging: laat de rem niet elke paar seconden wisselen, anders
// gaat de ketel pendelen. Vijf minuten minimale aan- en uittijd. Dit staat
// bewust hier en niet op de ESP32, zodat het ook geldt als het beleid hapert.

static const uint32_t BRAKE_MIN_HOLD_MS = 5UL * 60UL * 1000UL;

// Wordt elke lus aangeroepen, niet vanuit het doorgeefpad. Dat was fout: dan
// liep de rem alleen bij te werken zolang er een ketel meepraatte, en meldde
// het statusbericht in de zelfstandige stand een bevroren waarde.
static void brake_update() {
  bool want = policy.valid && policy.brake_on;
  if (want != brake_applied && (millis() - brake_changed_ms) >= BRAKE_MIN_HOLD_MS) {
    brake_applied    = want;
    brake_changed_ms = millis();
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Doorgeefluik
// ══════════════════════════════════════════════════════════════════════════

static void note_from_thermostat(uint32_t f) {
  const uint8_t id = data_id(f);
  const uint8_t mt = msg_type(f);
  if (mt != MSG_WRITE_DATA && mt != MSG_READ_DATA) return;

  switch (id) {
    case ID_CONTROL_SETPOINT: obs.requested_setpoint = (uint8_t) (f88_to_tenths(data_val(f)) / 10); break;
    case ID_ROOM_TEMP:        obs.room_temp     = f88_to_tenths(data_val(f)); break;
    case ID_ROOM_SETPOINT:    obs.room_setpoint = f88_to_tenths(data_val(f)); break;
    case ID_STATUS:           obs.master_status = (uint8_t) (data_val(f) >> 8); break;
    default: break;
  }
}

static void note_from_boiler(uint32_t f) {
  if (msg_type(f) != MSG_READ_ACK && msg_type(f) != MSG_WRITE_ACK) return;

  switch (data_id(f)) {
    case ID_BOILER_TEMP: obs.boiler_temp = f88_to_tenths(data_val(f)); break;
    case ID_MODULATION:  obs.modulation  = (uint8_t) (f88_to_tenths(data_val(f)) / 10); break;
    case ID_FAULT_FLAGS: obs.fault_code   = (uint8_t) (data_val(f) & 0xFF); break;
    case ID_STATUS:      obs.slave_status = (uint8_t) (data_val(f) & 0xFF); break;
    default: break;
  }
}

// Onderweg naar de ketel: alleen ID 1 kan worden vervangen.
static uint32_t substitute_to_boiler(uint32_t f) {
  if (!brake_applied) return f;
  if (msg_type(f) != MSG_WRITE_DATA || data_id(f) != ID_CONTROL_SETPOINT) return f;
  // Begrenzen voordat we naar f8.8 rekenen: boven 127 graden loopt die
  // omrekening over en zou de rem als negatieve temperatuur aankomen. De ESP32
  // knijpt al af, maar de AVR moet ook op zichzelf veilig zijn.
  uint8_t sp = policy.brake_setpoint > 100 ? 100 : policy.brake_setpoint;
  return replace_value(f, tenths_to_f88((int16_t) (sp * 10)));
}

// Onderweg naar de thermostaat: ID 25 en 28 krijgen de temperaturen van de
// warmtepomp, zodat haar regellus ziet dat er warmte geleverd wordt ook als
// de ketel koud staat.
static uint32_t substitute_to_thermostat(uint32_t f) {
  // Door de ESP32 gezette antwoorden gaan voor. Zo kun je de thermostaat elke
  // waarde voorschotelen zonder dat daar firmware voor nodig is - bijvoorbeeld
  // de buitentemperatuur van de warmtepomp op ID 27.
  uint16_t v;
  if (msg_type(f) == MSG_READ_ACK && ovr_lookup(data_id(f), &v))
    return replace_value(f, v);

  if (!policy.valid) return f;

  // De override werkt ook in doorgeefstand. De ketel weet niets van ons
  // setpoint, dus we vervangen zijn antwoord - en desnoods maken we er een
  // Read-Ack van, want een ketel die ID 9 niet kent antwoordt met Unknown.
  const uint8_t id = data_id(f);
  if (override_active() && (id == ID_REMOTE_OVERRIDE || id == ID_OVERRIDE_FUNC)) {
    const uint16_t v = (id == ID_REMOTE_OVERRIDE)
                       ? tenths_to_f88(policy.override_setpoint) : 0x0000;
    return with_parity(((uint32_t) MSG_READ_ACK << 28) | ((uint32_t) id << 16) | v);
  }

  if (!policy.subst_temp || msg_type(f) != MSG_READ_ACK) return f;

  switch (id) {
    case ID_BOILER_TEMP: return replace_value(f, tenths_to_f88(policy.wp_supply));
    case ID_RETURN_TEMP: return replace_value(f, tenths_to_f88(policy.wp_return));
    default: return f;
  }
}

// De thermostaat verwacht antwoord binnen 800 ms; de ketel geeft het ons
// meestal binnen 100 ms. Ruim binnen budget, en zo werkt elke OT-gateway.
static const uint32_t BOILER_REPLY_TIMEOUT_MS = 400;

// OpenTherm schrijft voor dat een slave niet eerder dan 20 ms antwoordt. Bij
// het doorgeven zit die vertraging er vanzelf in (het frame naar de ketel
// duurt al 34 ms), maar in de zelfstandige stand moeten we hem maken.
static const uint32_t SLAVE_REPLY_DELAY_MS = 40;

enum BridgeState { BR_IDLE, BR_WAIT_BOILER, BR_RESPOND };
static BridgeState br_state;
static uint32_t    br_started_ms;
static uint32_t    br_pending;      // klaarstaand antwoord voor de thermostaat
static uint32_t    br_request;      // de vraag die we naar de ketel doorgaven
static uint32_t    br_due_ms;
static uint32_t    br_poll_ms;      // eigen poll richting de ketel
static uint32_t    br_retry_ms;     // laatste hertest van een dood gewaande ketel
static bool        br_ours;         // wacht de thermostaat op dit antwoord, of wij?

// Hoe vaak we een ketel die niet antwoordt opnieuw proberen, terwijl er wel
// een thermostaat praat. Elke poging kost die ene vraag BOILER_REPLY_TIMEOUT_MS
// extra, dus niet te vaak; maar een ketel die terugkomt moet ook niet minuten
// onopgemerkt blijven.
static const uint32_t BOILER_RETRY_MS = 10000;

// Een OpenTherm-master hoort minstens elke seconde iets te sturen, anders valt
// de slave in storing. Zolang we alleen doorgaven, stopte de ketel dus zodra de
// thermostaat wegviel - precies het moment waarop je hem het hardst nodig hebt.
static const uint32_t BOILER_POLL_MS = 1000;

// ── Zelfstandige slave ────────────────────────────────────────────────────
//
// Zonder ketel is er niets om door te geven, en dan blijft een OT-master in
// zijn opstartfase steken: hij herhaalt Data-ID 0 en komt nooit toe aan
// kamertemperatuur of setpoint. Antwoorden we zelf, dan loopt het gesprek
// door en blijft de thermostaat gewoon werken.
//
// Dit is ook het gedrag dat je wilt als de ketel ooit uitvalt of losgekoppeld
// wordt: het huis draait dan verder op de warmtepomp alleen.
//
// LET OP: dit zijn verzonnen antwoorden. Alleen gebruiken zolang de ketel
// aantoonbaar afwezig is - zodra hij meepraat gaat alles weer ongewijzigd
// door, want wat je niet kent geef je door.

static uint16_t standalone_status() {
  uint8_t slave = 0;
  if (obs.master_status & 0x01) slave |= 0x02;   // CH-modus actief
  // "Vlam" staat hier voor: de warmtepomp levert daadwerkelijk. Dat bepaalt de
  // ESP32, want alleen die weet of de compressor draait.
  if (policy.sa_flame)          slave |= 0x08;
  return (uint16_t) slave;
}

// Data-ID 9 gaat van slave naar master: wij kunnen de thermostaat dus een
// kamersetpoint opdringen. Een waarde ongelijk nul neemt hij over, op 0 regelt
// hij weer zelf. ID 100 vertelt hoe de override zich verhoudt tot handmatig
// draaien aan de knop; 0 betekent dat onze waarde blijft staan.
static bool override_active() { return policy.valid && policy.override_setpoint != 0; }

static uint32_t standalone_reply(uint32_t req) {
  const uint8_t mt = msg_type(req);
  const uint8_t id = data_id(req);

  // Een schrijfactie bevestigen we ongewijzigd; we hebben geen mening.
  if (mt == MSG_WRITE_DATA)
    return with_parity((req & 0x0FFFFFFFUL) | ((uint32_t) MSG_WRITE_ACK << 28));

  if (mt != MSG_READ_DATA)
    return with_parity((req & 0x0FFFFFFFUL) | ((uint32_t) MSG_UNKNOWN_ID << 28));

  uint16_t val;
  switch (id) {
    case ID_STATUS:       val = standalone_status(); break;
    case ID_SLAVE_CONFIG: val = 0x0000; break;   // modulerend, geen tapwater
    case ID_FAULT_FLAGS:  val = 0x0000; break;
    case ID_MODULATION:   val = tenths_to_f88((int16_t) (policy.sa_modulation * 10)); break;
    case ID_REMOTE_OVERRIDE:
      val = override_active() ? tenths_to_f88(policy.override_setpoint) : 0x0000;
      break;
    case ID_OVERRIDE_FUNC: val = 0x0000; break;
    // De temperaturen van de warmtepomp, zodat de regellus van de thermostaat
    // ziet dat er warmte geleverd wordt.
    //
    // Maar alleen als de ESP32 ze ons heeft verteld. Zonder die gegevens staat
    // wp_supply gewoon op 0, en 0 graden aanvoer melden is het slechtste wat je
    // kunt doen: een modulerende thermostaat leest dat als ijskoud water en
    // gaat volle bak vragen. Dan liever eerlijk zeggen dat we het niet weten -
    // DATA-INVALID, zodat hij het later opnieuw probeert.
    case ID_BOILER_TEMP:
    case ID_RETURN_TEMP:
      if (!policy.valid)
        return with_parity((req & 0x0FFFFFFFUL) | ((uint32_t) MSG_DATA_INVALID << 28));
      val = (id == ID_BOILER_TEMP) ? tenths_to_f88(policy.wp_supply)
                                   : tenths_to_f88(policy.wp_return);
      break;
    default: {
      // Heeft de ESP32 hier een antwoord voor klaargezet, geef dat dan.
      uint16_t v;
      if (ovr_lookup(id, &v)) { val = v; break; }
      // Onbekend netjes afwijzen is beter dan zwijgen: dan gaat de master
      // door naar het volgende Data-ID in plaats van te blijven herhalen.
      return with_parity((req & 0x0FFFFFFFUL) | ((uint32_t) MSG_UNKNOWN_ID << 28));
    }
  }
  return with_parity(((uint32_t) MSG_READ_ACK << 28) | ((uint32_t) id << 16) | val);
}

static void bridge_poll() {
  uint32_t f;

  switch (br_state) {
    case BR_IDLE:
      if (!ot_take(CH_STAT, &f)) {
        // Geen thermostaat: de ketel zelf warm houden met een statusvraag.
        // Zonder warmtevraag in de hoge byte, dus dit zet niets aan - het houdt
        // alleen het gesprek in leven. En het maakt de ketelkant los te testen
        // van de thermostaatkant, wat anders onmogelijk is: de ketel is slave
        // en zwijgt tot wij iets vragen.
        // Staat er iets van de ESP32 klaar voor de ketel? Dan nu, want de
        // ketelkant is vrij. Wel pas als de thermostaat even stil is: komt er
        // midden in onze injectie een vraag binnen, dan wacht zij tot de ketel
        // geantwoord heeft. Dat past ruim binnen haar 800 ms, maar zuinig aan.
        if (inj_tail != inj_head && (millis() - last_stat_frame_ms) > 150) {
          const uint32_t f2 = inj_ring[inj_tail];
          inj_tail = (uint8_t) ((inj_tail + 1) % 4);
          br_ours = true;
          ot_send(CH_BOILER, f2);
          ev_push(EV_TO_BOILER, f2);
          br_started_ms = millis();
          br_state = BR_WAIT_BOILER;
          break;
        }

        if (!obs.stat_alive && (millis() - br_poll_ms) > BOILER_POLL_MS) {
          br_poll_ms = millis();

          // Zonder thermostaat zijn wij zelf de master. We wisselen af tussen
          // de statusvraag en het schrijven van het setpoint, want dat is het
          // minimum dat een OT-master moet doen om een ketel te laten stoken.
          //
          // Setpoint 0 betekent uit: dan zetten we ook de CV-vlag niet, en dan
          // blijft dit puur een keep-alive.
          static uint8_t poll_turn;
          const bool     want_heat = policy.valid && policy.boiler_setpoint > 0;
          uint32_t       frame;

          if ((poll_turn++ & 1) && want_heat) {
            // Write-Data ID 1: het setpoint zelf, in f8.8.
            frame = ((uint32_t) MSG_WRITE_DATA << 28) |
                    ((uint32_t) ID_CONTROL_SETPOINT << 16) |
                    tenths_to_f88((int16_t) (policy.boiler_setpoint * 10));
          } else {
            // Read-Data ID 0, met onze warmtevraag in de hoge byte.
            frame = ((uint32_t) MSG_READ_DATA << 28) |
                    ((uint32_t) ID_STATUS << 16) |
                    (want_heat ? 0x0100UL : 0UL);
          }
          frame = with_parity(frame);
          br_ours = true;
          ot_send(CH_BOILER, frame);
          ev_push(EV_TO_BOILER, frame);
          br_started_ms = millis();
          br_state = BR_WAIT_BOILER;
        }
        break;
      }
      last_stat_frame_ms = millis();
      obs.stat_alive = true;
      if (!parity_ok(f)) { if (parity_fails < 15) parity_fails++; break; }
      note_from_thermostat(f);
      ev_push(EV_FROM_STAT, f);

      // Zelf antwoorden als de ketel weg is - anders blijft de thermostaat
      // hangen. Maar NIET voorgoed.
      //
      // Hier zat een grendel. De ketel wordt alleen "levend" door een antwoord,
      // en een antwoord komt alleen op een vraag. Ging obs.boiler_alive een
      // keer uit terwijl er een thermostaat hing, dan nam deze tak voortaan
      // elke beurt over en werd er nooit meer iets naar de ketel gestuurd. De
      // eigen poll verderop zit achter !obs.stat_alive, dus die sprong ook niet
      // bij. Resultaat: "Ketel verbonden" bleef uit tot de AVR opnieuw startte,
      // ook als de ketel er allang weer was.
      //
      // Nu proberen we het elke BOILER_RETRY_MS gewoon opnieuw, door een echte
      // vraag van de thermostaat door te geven. Antwoordt de ketel, dan staat
      // hij meteen weer aan; antwoordt hij niet, dan vangt de time-out in
      // BR_WAIT_BOILER dat af en krijgt de thermostaat alsnog antwoord.
      if (policy.force_standalone ||
          (!obs.boiler_alive &&
           (millis() - br_retry_ms) < BOILER_RETRY_MS)) {
        br_pending = standalone_reply(f);
        br_due_ms  = millis() + SLAVE_REPLY_DELAY_MS;
        br_state   = BR_RESPOND;
        break;
      }
      if (!obs.boiler_alive) br_retry_ms = millis();

      br_request = f;
      br_ours = false;
      {
        const uint32_t out = substitute_to_boiler(f);
        ot_send(CH_BOILER, out);
        ev_push(EV_TO_BOILER, out);
      }
      br_started_ms = millis();
      br_state = BR_WAIT_BOILER;
      break;

    case BR_RESPOND:
      if ((int32_t) (millis() - br_due_ms) >= 0) {
        ot_send(CH_STAT, br_pending);
        ev_push(EV_TO_STAT, br_pending);
        br_state = BR_IDLE;
      }
      break;

    case BR_WAIT_BOILER:
      if (ot_take(CH_BOILER, &f)) {
        last_boiler_frame_ms = millis();
        obs.boiler_alive = true;
        if (parity_ok(f)) {
          note_from_boiler(f);
          ev_push(EV_FROM_BOILER, f);
          // Was dit een antwoord op onze eigen vraag, dan heeft de thermostaat
          // er niets op staan wachten. Wel doorsturen zou een antwoord zijn op
          // een vraag die zij nooit stelde.
          if (!br_ours) {
            const uint32_t out = substitute_to_thermostat(f);
            ot_send(CH_STAT, out);
            ev_push(EV_TO_STAT, out);
          }
        }
        br_state = BR_IDLE;
      } else if (br_ours && (millis() - br_started_ms) > BOILER_REPLY_TIMEOUT_MS) {
        // Onze eigen vraag bleef onbeantwoord. Niemand wacht erop, dus gewoon
        // door - de thermostaat hoeft hier niets van te merken.
        br_state = BR_IDLE;
      } else if ((millis() - br_started_ms) > BOILER_REPLY_TIMEOUT_MS) {
        // Geen antwoord van de ketel. NIET zwijgen: een OT-master die
        // herhaaldelijk geen antwoord krijgt concludeert dat de ketel weg is
        // en stopt met praten. Het origineel deed dit ook niet - buffer 0x89
        // heeft een Unknown-DataId-terugval, dus T1.1 bleef altijd antwoorden.
        //
        // Status kennen we zelf nog uit het laatste geldige antwoord; de rest
        // wijzen we netjes af, want daar gaat een master gewoon op door.
        if (data_id(br_request) == ID_STATUS) {
          br_pending = with_parity(((uint32_t) MSG_READ_ACK << 28) |
                                   ((uint32_t) ID_STATUS << 16) | obs.slave_status);
        } else {
          br_pending = with_parity((br_request & 0x0FFFFFFFUL) |
                                   ((uint32_t) MSG_UNKNOWN_ID << 28));
        }
        br_due_ms = millis();     // de 20 ms zijn er allang overheen
        br_state  = BR_RESPOND;
      }
      break;
  }

  if ((millis() - last_stat_frame_ms)   > 5000) obs.stat_alive   = false;
  if ((millis() - last_boiler_frame_ms) > 5000) obs.boiler_alive = false;
}

// ══════════════════════════════════════════════════════════════════════════

void setup() {
  // ── Poort-init letterlijk gelijk aan het origineel ──
  //
  //   DDRB = 0x1F  PORTB = 0x00   PB0..PB4 uitgang, alle laag
  //   DDRC = 0x30  PORTC = 0xCF   PC4/PC5 uitgang laag, PC0..PC3 pull-up
  //   DDRD = 0x1E  PORTD = 0x19   PD1..PD4 uitgang; PD0 pull-up, PD3/PD4 hoog
  //
  // Eerdere versies zetten alleen de pinnen die ik nodig dacht te hebben, en
  // lieten PB0, PB1, PB3, PB4 en PD2 als ingang staan. Het origineel stuurt
  // die actief laag. Hangt daar ergens een enable aan, dan zweeft hij bij ons
  // in plaats van dat hij aan staat - en dat zie je nergens aan terug.
  //
  // Dit kost niets en haalt een hele klasse verschillen weg.
  DDRB  = 0x1F;  PORTB = 0x00;
  DDRC  = 0x30;  PORTC = 0xCF;
  DDRD  = 0x1E;  PORTD = 0x19;

  // Daarna onze eigen afwijkingen erbovenop.
  ot_write(CH_BOILER, OT_IDLE_LEVEL);
  ot_write(CH_STAT,   OT_IDLE_LEVEL);

  // Relais op socket-pin 16. Begint UIT: we weten uit de firmware wel wanneer
  // Atlantic 'm schakelde, maar niet wat er fysiek achter zit. Zou het toch
  // een warmtevraag-relais zijn, dan wil je niet dat de ketel aanslaat zodra
  // deze chip opstart.
  DDRB |= _BV(PB2);
  PORTB &= (uint8_t) ~_BV(PB2);

  // Schakelaar op socket-pin 25 als ingang met pull-up, zoals het origineel.
  DDRC  &= (uint8_t) ~_BV(PC2);
  PORTC |= _BV(PC2);

  // Statuslampje op socket-pin 27, uit bij het opstarten - net als het
  // origineel, dat PORTC op 0xCF zet.
  DDRC  |= _BV(PC4);
  PORTC &= (uint8_t) ~_BV(PC4);

  for (uint8_t c = 0; c < N_CH; c++) {
    memset((void *) &ch[c], 0, sizeof(OtChannel));
    ch[c].rx_prev = 1;
  }

  Serial.begin(9600);

  // Comparator met de interne bandgap op de plus-ingang, AIN1 (PD7) op de min.
  // In stappen, want het omzetten van ACBG geeft zelf een flank.
  ACSR = _BV(ACBG);
  delayMicroseconds(100);
  ACSR = _BV(ACBG) | _BV(ACI);

  // Timer1 in CTC zonder prescaler: exact 125 us per tik, ongeacht hoe lang
  // de ISR duurt.
  TCCR1A = 0;
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A  = TIMER1_TOP;
  TCNT1  = 0;
  TIMSK1 = _BV(OCIE1A);

  // Startteller: bij een koude start op nul, anders ophogen. Zo is een
  // resetlus zichtbaar zonder dat je erbij hoeft te staan.
  if (mcusr_saved & _BV(PORF)) boot_count = 0;
  else                         boot_count++;

  // Zo ver terugzetten dat de eerste wissel meteen mag. Anders zou de rem na
  // een koude start vijf minuten lang niet kunnen aangrijpen.
  brake_changed_ms = millis() - BRAKE_MIN_HOLD_MS;
  sei();

  // Ruim boven de lusduur: de zwaarste iteratie is een frame opbouwen en
  // 17 bytes in de UART-buffer schrijven, en dat is een kwestie van
  // microseconden.
  wdt_enable(WDTO_2S);
}

void loop() {
  static uint32_t next_status_ms;

  wdt_reset();

  bridge_poll();
  ev_flush();
  link_poll();
  brake_update();
  lamp_update();
  relay_update();
  demand_update();
  test_out_update();

  if ((int32_t) (millis() - next_status_ms) >= 0) {
    next_status_ms = millis() + STATUS_EVERY_MS;
    link_send_status();
  }
}
