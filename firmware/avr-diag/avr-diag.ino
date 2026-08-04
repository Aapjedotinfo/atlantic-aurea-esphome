// ╔═══════════════════════════════════════════════════════════════════════╗
// ║  avr-diag - fase 1 van de volledige controlbox                        ║
// ║  ATmega328P @ 8 MHz in de IC2-socket (was "Haddon T1.1")              ║
// ╚═══════════════════════════════════════════════════════════════════════╝
//
// DEZE FIRMWARE ZENDT NIETS. Alle vier de OpenTherm-kandidaatpinnen staan als
// ingang. Hij telt alleen wat er binnenkomt en rapporteert dat in gewone tekst
// over 9600 baud (PD0/PD1, socket-pin 2 en 3).
//
// Waarvoor: uitzoeken welke socket-pin bij welke kant hoort. Wij leiden uit de
// originele firmware af dat pin 13 (PD7) de thermostaat is en pin 11 (PD5) de
// ketel; WackoH mat pin 13 als massa. Deze schets beslecht dat.
//
// Zie docs/volledige-controlbox.md.
//
// ── Bouwen ─────────────────────────────────────────────────────────────────
//   Board:  MiniCore "ATmega328" -> Clock: 8 MHz internal (bureau)
//                                or 8 MHz external        (in de controlbox)
//   Sketch -> Export Compiled Binary, neem de .hex ZONDER "with_bootloader".
//
// ── Bureau-opstelling (geen kristal nodig, geen controlbox nodig) ──────────
//   ATmega328P op 5 V, pin 7+20 aan +5V, pin 8+22 aan GND, pin 1 via 10k
//   aan +5V. USB-TTL-adapter: zijn RX aan chip-pin 3, GND aan GND.
//   Terminal op 9600 baud. Raak pin 11 of 13 even aan met een draadje aan
//   GND en de tellers moeten oplopen.

#if F_CPU != 8000000UL
#error "Verkeerde klok. Kies een 8 MHz-borddefinitie - op 16 MHz klopt de baudrate niet en zie je alleen rommel."
#endif

// ── Socket-pinnen (DIP28) naar AVR-poorten ────────────────────────────────
//   pin 5  = PD3  OpenTherm naar de CV-ketel      (in het origineel UITgang)
//   pin 6  = PD4  OpenTherm naar de thermostaat   (in het origineel UITgang)
//   pin 11 = PD5  OpenTherm van de CV-ketel       (ingang)
//   pin 13 = PD7  OpenTherm van de thermostaat    (ingang, via comparator)
//
// PD7 lezen we NIET digitaal. Het origineel gebruikt de analoge comparator met
// de interne bandgap van 1,23 V als referentie, en dat is geen toeval: het
// signaal op die pin komt van een deler en haalt de digitale drempel van
// 0,6 x VCC = 3,0 V waarschijnlijk niet. Een digitalRead() ziet daar dus niets,
// ook als er volop verkeer op staat. Wij doen het dus net zo.

static const uint8_t PIN_PD3 = 3;  // socket-pin 5
static const uint8_t PIN_PD4 = 4;  // socket-pin 6
static const uint8_t PIN_PD5 = 5;  // socket-pin 11

// Pulsen korter dan dit zijn geen OpenTherm maar inductie of dender.
static const uint16_t GLITCH_US = 50;

struct Channel {
  volatile uint32_t edges;      // flanken sinds de vorige regel
  volatile uint32_t last_us;    // tijdstip vorige flank
  volatile uint16_t min_us;     // kortste geldige puls - de vingerafdruk
};

static volatile Channel ch_boiler;  // PD5, socket-pin 11
static volatile Channel ch_stat;    // PD7, socket-pin 13, via comparator

static inline void note_edge(volatile Channel &c) {
  uint32_t now = micros();
  uint32_t dt = now - c.last_us;
  c.last_us = now;
  c.edges++;
  if (dt >= GLITCH_US && dt < c.min_us) c.min_us = (uint16_t) dt;
}

// PD5 (socket-pin 11) = PCINT21, zit in de PCINT2-groep.
ISR(PCINT2_vect) { note_edge(ch_boiler); }

// PD7 (socket-pin 13) = AIN1, de negatieve comparator-ingang.
ISR(ANALOG_COMP_vect) { note_edge(ch_stat); }

static void print_us(uint16_t v);

static void reset_channel(volatile Channel &c) {
  c.edges = 0;
  c.min_us = 0xFFFF;
}

void setup() {
  // Alles als ingang zonder pull-up: puur meeluisteren, niets sturen.
  //
  // LET OP: in de controlbox betekent dit dat PD3 en PD4 zweven, terwijl het
  // origineel ze HOOG idlet. De OpenTherm-bussen staan dan dus niet in hun
  // ruststand en je thermostaat zal tijdens deze test niet werken. Dat is de
  // bedoeling - je meet, je bedient nog niet.
  pinMode(PIN_PD3, INPUT);
  pinMode(PIN_PD4, INPUT);
  pinMode(PIN_PD5, INPUT);
  pinMode(7, INPUT);  // PD7, wordt door de comparator gelezen

  reset_channel(ch_boiler);
  reset_channel(ch_stat);

  Serial.begin(9600);

  // Pin-change-interrupt op PD5.
  PCMSK2 = _BV(PCINT21);
  PCIFR  = _BV(PCIF2);   // eventuele oude vlag wissen
  PCICR  = _BV(PCIE2);

  // Analoge comparator precies zoals het origineel 'm opzet:
  //   ACBG (bit 6) = interne bandgap 1,23 V op de plus-ingang
  //   AIN1 (PD7)   = min-ingang
  // Plus ACIE zodat wij op de flank een interrupt krijgen in plaats van te
  // pollen, en ACIS = 00 = interrupt bij elke toggle.
  //
  // In drie stappen, want de datasheet waarschuwt dat het omzetten van ACBG
  // zelf een flank op de comparator-uitgang geeft. Zet je ACIE meteen mee aan,
  // dan vuurt hij daar direct op en begin je met een valse telling.
  ACSR = _BV(ACBG);          // bandgap kiezen, interrupt nog uit
  delayMicroseconds(100);    // bandgap laten inzwaaien (~50 us)
  ACSR = _BV(ACBG) | _BV(ACI);              // valse vlag wissen (1 schrijven)
  ACSR = _BV(ACBG) | _BV(ACIE);             // nu pas scherp

  Serial.println();
  Serial.println(F("avr-diag v1 - 8 MHz - luistert alleen, zendt niets"));
  Serial.println(F("kolommen: t  pin11(PD5)  pin13(PD7/comparator)  pin5  pin6"));
  Serial.println(F("flanken per seconde, en de kortste gemeten puls in us"));
  Serial.println(F("OpenTherm = ~500 us halve bit; alles ver daaronder is ruis"));
  Serial.println();
}

void loop() {
  static uint32_t next_ms = 0;
  static uint32_t seconds = 0;

  if ((int32_t) (millis() - next_ms) < 0) return;
  next_ms += 1000;
  seconds++;

  // Snapshot met de interrupts even uit, anders lees je een halve update.
  uint32_t b_edges, s_edges;
  uint16_t b_min, s_min;
  noInterrupts();
  b_edges = ch_boiler.edges;  b_min = ch_boiler.min_us;  reset_channel(ch_boiler);
  s_edges = ch_stat.edges;    s_min = ch_stat.min_us;    reset_channel(ch_stat);
  interrupts();

  // Niveau van de twee pinnen die het origineel als uitgang gebruikt. Hier
  // alleen om te zien of de print ze ergens naartoe trekt.
  int lvl3 = digitalRead(PIN_PD3);
  int lvl4 = digitalRead(PIN_PD4);
  int aco  = (ACSR & _BV(ACO)) ? 1 : 0;

  Serial.print(F("t="));      Serial.print(seconds);
  Serial.print(F("  pin11 e=")); Serial.print(b_edges);
  Serial.print(F(" min="));   print_us(b_min);
  Serial.print(F("  pin13 e=")); Serial.print(s_edges);
  Serial.print(F(" min="));   print_us(s_min);
  Serial.print(F(" aco="));   Serial.print(aco);
  Serial.print(F("  pin5=")); Serial.print(lvl3);
  Serial.print(F(" pin6="));  Serial.print(lvl4);
  Serial.println();
}

static void print_us(uint16_t v) {
  if (v == 0xFFFF) Serial.print(F("-"));
  else             Serial.print(v);
}
