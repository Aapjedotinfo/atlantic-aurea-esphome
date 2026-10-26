/*
 * !!! VEROUDERD - LEES EERST index.html, het verslag (versie 2) !!!
 *
 * ERRATA 26-10-2026 - volledige analyse (analyse-volledig/RAPPORT.md, alle flash-bytes verklaard,
 * emulator vergeleken met de gdb-AVR-simulator). Waar de commentaren hieronder hiervan afwijken, geldt dit:
 *   - Link M1.1 <-> T1.1: 12 bytes + 8-bits som, 9615 baud 8N2, GEEN vaste 0xF0-kop. 0xF0 is alleen de WAARDE
 *     "niet beschikbaar" in byte 0 richting T1.1; andersom is byte 0 het ketelsetpoint.
 *   - M1.1 rekent nergens een CRC (zenden uit tabellen met vooraf berekende CRC) en controleert bij ontvangen
 *     alleen het ID (byte 1): kop, lengte en CRC niet.
 *   - PB3 stuurt de laatst ONTVANGEN buitenunit-bytes op de zendbitklok (echo), niet dezelfde bits als PC3.
 *   - De 5-graden-regel blokkeert een KOUDE START; een draaiende pomp stopt pas als de credit-teller [0x65]
 *     leeg is (ca. 2 uur). [0x65] telt per 60 SECONDEN (niet per 60 ticks).
 *   - DIP2 OFF = aan/uit-ketel (T1.1 is zelf OT-slave, K3/PC5 volgt het setpoint >= 20 in boiler-modus).
 *     Een "pompniveau" bestaat in de firmware niet.
 *   - De weestelegrammen op 0x0baa en 0x0bb4 zijn bewezen onbereikbaar; "B2 02" is een vaste constante.
 *   - T1.1 [0x73] is de foutcode (M1.1 byte 10); de buitentemperatuur wordt in T1.1 weggegooid.
 *   - Een stand-wijziging midden in een ID2-telegram geeft een corrupt telegram.
 * ==========================================================================
 * Geschreven VOORDAT de SBRC/SBRS-bug in avrdis.py was hersteld (die twee
 * verschillen in BIT 9, niet bit 3) -> 24 instructies in T1.1 hadden de
 * OMGEKEERDE betekenis. Bovendien zijn deze drie stellingen hieronder FOUT:
 *   - "ACSR=0x40 zet de analoge comparator uit" -> NEE: 0x40 is bit6 = ACBG
 *     (bandgap-referentie), ACD blijft 0, dus de comparator staat AAN. Het is
 *     de OpenTherm-ONTVANGSTWEG (`SBIC/SBIS ACSR,5` = ACO in sub_0286).
 *   - "sub_0870/0872 is een BCD double-dabble" -> NEE: het is de OpenTherm-
 *     PARITEITSberekening (dit zijn exact de 2 ROL-instructies in T1.1).
 *   - "T1.1 emuleert de ketel richting Anna" -> ONVOLLEDIG: T1.1 is een
 *     TWEEZIJDIGE OT-BRUG - slave richting Anna, MASTER richting de CV-ketel.
 *     Frames worden in beide richtingen doorgekopieerd (0x0490: 0x89->0x8d;
 *     0x053e: 0x8d->0x89). Berichttypes bewijzen het: 0x40/0xC0 = Read-Ack
 *     (slave), 0x10/0x90 = Write-Data en 0x80 = Read-Data (master), 0x70 =
 *     Unknown-DataId als default-tak.
 * Daarmee vervalt ook de conclusie onderaan dit bestand dat bijstook
 * "niet-firmware-gestuurd" zou zijn: de ketel wordt juist via OpenTherm door
 * T1.1 aangestuurd (drempellogica vanaf 0x0676, vergelijking met 20 op
 * 0x0688). Het verslag is leidend.
 * ==========================================================================
 *
 * Haddon T1.1 (IC2) - reverse-engineered reconstructie
 * =======================================================
 * Bron: "Haddon T1.1 - ATMEGA8@DIP28.BIN" (8704 bytes ruwe flash-dump).
 * IC2 uit de Atlantic/Chofu controlbox (zie ../aurea-wp-666/WIRING.md) -
 * de chip NAAST IC1/M1.1, niet vervangen door de ESP32 en momenteel
 * ongebruikt in aurea-wp-666.
 *
 * LEESWIJZER: zelfde uitgangspunt als Haddon_M1.1_reversed.ino - dit is geen
 * originele broncode maar een gedrag-getrouwe reconstructie uit
 * Haddon_T1.1_disassembly.lst. Omdat T1.1's belangrijkste vondst (het is
 * NIET de 666-baud-chip, en de ESP32 hoeft 'm dus niet te vervangen om de
 * warmtepomp te laten werken) al vaststaat, is de vertaling hieronder op
 * ARCHITECTUURNIVEAU nauwkeurig maar minder instructie-voor-instructie
 * uitgeschreven dan de M1.1-versie (tijd/scope-afweging). Voor exacte details
 * per functie: zie de adressen in de comments + de .lst-listing.
 *
 * Chip: ATmega8, zelfde 8MHz aanname als M1.1 (niet apart bevestigd voor
 * deze specifieke chip, maar identieke UBRR/stack/Timer1-configuratie maakt
 * een ander kristal onwaarschijnlijk).
 *
 * KERNCONCLUSIE (bijgewerkt): T1.1 bevat GEEN 666-baud bit-bang-engine naar
 * de buitenunit (geen PC4-polling zoals in M1.1). Het praat met M1.1 over de
 * 9600-baud hardware-UART, MAAR is DAARNAAST de OpenTherm-koppeling naar de
 * Anna-thermostaat: sub_0482 dispatcht op officiele OpenTherm Data-ID's
 * (0,1,2,3,7,14,16,17,24,25,28,49) en sub_0892/sub_0958 zenden die als
 * Manchester-gecodeerd bit-bang-signaal uit op PD3 resp. PD4 (getoggled op
 * basis van een teruggelezen pinstand op de bit-helft - klassieke Manchester-
 * inversie). T1.1 bouwt antwoorden voor ID1/16/24 (Control Setpoint, Room
 * Setpoint, Room Temperature) - waarden die een OT-MASTER zoals Anna juist
 * verstuurt - dus T1.1 emuleert richting Anna de rol van de ketel (OT-slave),
 * leest zo Anna's setpoint/kamertemp uit, en geeft die door aan M1.1 via het
 * 9600-baud-telegram. ONZEKER: welke van PD3/PD4 fysiek naar Anna loopt (de
 * andere kan een gespiegelde/diagnostische uitgang zijn), en waar de losse
 * RX-ingang zit (PC0/PC1/PC3/PD2 komen nergens in bereikbare code voor -
 * lijkt half-duplex op dezelfde pin). Zie het HTML-verslag voor de volledige
 * Data-ID-tabel en de inschatting of de ESP32 dit ook zou kunnen overnemen
 * (softwarematig aannemelijk; hardwarematig afhankelijk van een eventuele
 * al-aanwezige interfaceschakeling bij T1.1's socket - niet uit de flash-dump
 * vast te stellen).
 */

#include <stdint.h>
/* Zie Haddon_M1.1_reversed.ino voor de opmerking over avr/io.h + avr/interrupt.h. */

// ---------------------------------------------------------------------
// Pin-toewijzing (uit sub_012c @0x012c)
// ---------------------------------------------------------------------
// DDRB = 0x1F (PB0-4 output), PORTB init = 0x00
#define PIN_PB2   2   // uitgang, direct gekoppeld aan PC2-knop (sub_0a24: SBIS PINC,2 -> SBI PORTB,2)

// DDRC = 0x30 (PC4,PC5 output), PORTC init = 0xCF (PC0-3 pull-up, incl. PC2 = knop-ingang)
#define PIN_PC2   2   // knop-ingang (meerdere subs pollen SBIC/SBIS PINC,2)
#define PIN_PC4   4   // foutcode-/statusindicator (LED?) - aan/uit + knippupatroon in sub_0aa0
#define PIN_PC5   5   // tweede statusuitgang (sub_0958-groep, drempel op v6c/0x14=20)

// DDRD = 0x1E (PD1,2,3,4 output - veel meer dan M1.1's enkel PD1!), PORTD init = 0x19
// PD0 = hardware-UART RX (van M1.1), PD1 = hardware-UART TX (naar M1.1)
#define PIN_PD3   3   // *** OpenTherm Manchester bit-bang (vermoedelijk de Anna-lijn) *** - sub_0892
#define PIN_PD4   4   // vrijwel identiek tweede Manchester-kanaal (sub_0958) - rol t.o.v. PD3 ONZEKER
#define PIN_PD5   5   // ingang, gepolld in sub_0398-groep (SBIC/SBIS PIND,5) - tweede knop

// ---------------------------------------------------------------------
// "Register-globals" (overleven interrupts, zie M1.1-bestand voor uitleg).
// Let op: T1.1 gebruikt een ANDERE registerverdeling dan M1.1 (r20 i.p.v.
// r22 als laatst-ontvangen-byte, r25 i.p.v. r23 als hoofd-vlagbyte).
// ---------------------------------------------------------------------
static uint8_t last_rx_byte = 0;  // r20 - laatste byte van M1.1 (USART_RXC ISR)
static uint8_t flagsA = 0;        // r22 - statusvlaggen (RX-frame-assemblage, net als M1.1's r23)
static uint8_t flagsB = 0;        // r24 - statusvlaggen (timing/retry, net als M1.1's r24)
static uint8_t flagsC = 0;        // r25 - EXTRA vlagbyte t.o.v. M1.1; o.a. FC_RX_BYTE hieronder
#define FC_RX_BYTE 0x04            // gezet door USART_RXC ISR = "byte van M1.1 ontvangen"

// ---------------------------------------------------------------------
// SRAM 0x60-0x7a: zelfde soort losse tellers/statusbytes als M1.1, met
// vrijwel identieke Timer1-ISR-logica (mod-256/100/40/60-tellers). Namen
// hieronder volgen dezelfde conventie als in het M1.1-bestand.
// ---------------------------------------------------------------------
static uint8_t v60_tick_mod256 = 0, v61_tick_mod100 = 0, v62_tick_mod40 = 0, v63_tick_mod60 = 0;
static uint8_t v64_ctr = 0, v65_ctr = 0, v66_ctr = 0, v67_ctr = 0, v68_ctr = 0, v69_bit_ctr = 0;
static uint8_t v6a_state = 0, v6b_unused = 0, v6c_state = 0x14, v6d_state = 0;
static uint8_t v6e_val = 0, v6f_val = 0, v70_val = 0;
static uint8_t v71_val = 0x14, v72_val = 0, v73_status_in = 0; // v73 = ontvangen statusbyte van M1.1!
static uint8_t v74_blink_ctr = 0, v75_blink_reload = 0, v76_val = 0;
static uint8_t v77_val = 0, v78_val = 0, v79_val = 0, v7a_val = 0;

// 0x7b-0x88: RX-werkbuffer (frame van M1.1 wordt hier opgebouwd, teller op 0x82/0x7b-range)
static uint8_t rx_workbuf[0x89 - 0x7B];
// 0x89-0x94(ong.): uitgaand telegram + "display-record"-scratchbuffers (X-pointer bases 0x89/0x8a/0x8c/0x8d/0x8e in de listing)
static uint8_t tx_telegram[12];

#define SBI(reg,bit)  ((reg) |=  (1<<(bit)))
#define CBI(reg,bit)  ((reg) &= ~(1<<(bit)))
#define BIT_IS_SET(reg,bit) (((reg) & (1<<(bit))) != 0)
#define BIT_IS_CLR(reg,bit) (((reg) & (1<<(bit))) == 0)

extern volatile uint8_t DDRB, PORTB, PINB;
extern volatile uint8_t DDRC, PORTC, PINC;
extern volatile uint8_t DDRD, PORTD, PIND;
extern volatile uint8_t UBRRL, UBRRH, UCSRA, UCSRB, UCSRC, UDR, ACSR;
extern volatile uint8_t TCCR1B, TCNT1H, TCNT1L, TIMSK, WDTCR, SPL, SPH;
static void wdt_reset(void);
static void build_reply_telegram(void);
static void uart_send_byte(uint8_t b);

// ===========================================================================
// setup() - RESET-pad @0x002a: stack-init + 7 RCALLs (1 meer dan M1.1: de
// extra ACSR-init in sub_015c hieronder).
// ===========================================================================
static void init_variables(void);      // sub_0056 @0x0056
static void init_timer1(void) {        // sub_0120 @0x0120 - identiek aan M1.1
    TCCR1B = 0x01; TIMSK = 0x04;
}
static void init_ports(void) {          // sub_012c @0x012c
    DDRB = 0x1F; PORTB = 0x00;
    DDRC = 0x30; PORTC = 0xCF;
    DDRD = 0x1E; PORTD = 0x19;
}
static void init_uart_9600(void) {      // sub_0146 @0x0146 - identiek aan M1.1
    UBRRL = 0x67; UBRRH = 0x00;
    SBI(UCSRA, 1);
    UCSRB = 0x98;
    UCSRC = 0x8E;
    (void) UDR;
}
static void init_analog_comparator_off(void) { // sub_015c @0x015c - ONTBREEKT in M1.1!
    ACSR = 0x40; // ACD=1, comparator uit
}
static void init_watchdog(void) {       // sub_0162 @0x0162 - identiek aan M1.1
    uint8_t w = WDTCR;
    WDTCR = w | 0x18;
    WDTCR = w | 0x07;
}

void setup(void) {
    SPL = 0x5F; SPH = 0x04;
    init_variables();
    init_timer1();
    init_ports();
    init_uart_9600();
    init_analog_comparator_off();
    init_watchdog();
}

static void init_variables(void) {
    last_rx_byte = 0; flagsA = 0; flagsB = 0; flagsC = 0;
    v60_tick_mod256=v61_tick_mod100=v62_tick_mod40=v63_tick_mod60=0;
    v64_ctr=v65_ctr=v66_ctr=v69_bit_ctr=v67_ctr=v68_ctr=v6d_state=0;
    v6a_state=0; v6f_val=0; v70_val=0; v74_blink_ctr=0; v75_blink_reload=0;
    for (uint8_t i = 0; i < sizeof(rx_workbuf); i++) rx_workbuf[i] = 0;
    v71_val = 0x14; v72_val = 0x14; // (asm: r16=0x14 hergebruikt voor twee velden, zie @0x00f8-0x0104)
}

// ===========================================================================
// loop() - superloop @0x0040-0x0054: watchdog + 9 taken.
// ===========================================================================
static void task_rx_frame(void);          // sub_0174 @0x0174 (+ sub_0232 telegram-opbouw)
static void task_pincheck_pc2_bit0(void); // sub_0286 @0x0286
static void task_pincheck_pd5_bit1(void); // sub_0398 @0x0398
static void task_display_dispatch(void);  // sub_0482 @0x0482 (grote mode-tabel 0x0490-0x086a)
static void task_out_pd3(void);           // sub_0892 @0x0892
static void task_out_pd4(void);           // sub_0958 @0x0958
static void task_telegram_build(void);    // sub_0a24 @0x0a24
static void task_faultcode_pc4(void);     // sub_0aa0 @0x0aa0
static void task_flush_telegram(void);    // sub_0b7e @0x0b7e

void loop(void) {
    wdt_reset();                    // sub_0170 @0x0170
    task_rx_frame();
    task_pincheck_pc2_bit0();
    task_pincheck_pd5_bit1();
    task_display_dispatch();
    task_out_pd3();
    task_out_pd4();
    task_telegram_build();
    task_faultcode_pc4();
    task_flush_telegram();
}

/* ---------------------------------------------------------------------
 * TAAK-FUNCTIES (architectuurniveau - zie .lst voor de exacte instructies)
 * ------------------------------------------------------------------- */

// sub_0174/sub_0232 - ontvangt bytes van M1.1 (via last_rx_byte, gezet door
// de USART_RXC-ISR), assembleert een frame in rx_workbuf, valideert met een
// simpele 8-bit-optelsom (zelfde schema als M1.1's kant), en zet bij een
// geldig frame de statusbyte (v73_status_in) en enkele andere velden over.
// Bouwt daarna, net als M1.1, een eigen 12-byte antwoordtelegram (tx_telegram)
// met dezelfde structuur (12 bytes: byte 0 = ketelsetpoint, geen vaste kop; + 8-bit-som).
static void task_rx_frame(void) {
    if (BIT_IS_CLR(flagsC, 2)) return;   // geen nieuwe byte -> niets te doen (vereenvoudigd
                                          // t.o.v. de asm, die ook een tweede vlagbit checkt)
    flagsC &= ~FC_RX_BYTE;
    static uint8_t idx = 0;
    rx_workbuf[idx % sizeof(rx_workbuf)] = last_rx_byte;
    idx++;
    if (idx >= 0x0C) {
        idx = 0;
        uint8_t sum = 0;
        for (uint8_t i = 0; i < 11; i++) sum += rx_workbuf[i];
        if (sum == rx_workbuf[11]) {
            // Frame geldig: velden overnemen (offsets ONZEKER, zie M1.1-bestand
            // punt 3/4 in de "OPEN VRAGEN" - dezelfde onzekerheid geldt hier)
            v73_status_in = rx_workbuf[8];  // ONZEKER welke exacte offset
            v6f_val = rx_workbuf[0];
        }
    }
    build_reply_telegram(); // sub_0232 @0x0232
}

static void build_reply_telegram(void) {
    uint8_t sum = 0;
    tx_telegram[0] = v6e_val;                       sum += tx_telegram[0];
    tx_telegram[1] = v77_val;                       sum += tx_telegram[1];
    tx_telegram[2] = v78_val;                       sum += tx_telegram[2];
    tx_telegram[3] = v79_val;                       sum += tx_telegram[3];
    tx_telegram[4] = v7a_val;                       sum += tx_telegram[4];
    for (uint8_t i = 5; i < 10; i++) { tx_telegram[i] = 0; } // ONZEKER: rest van de velden
    tx_telegram[10] = sum;
    flagsC |= 0x20; // "telegram klaar"-vlag (analoog aan M1.1's FA_TELEGRAM_RDY)
}

// sub_0286 @0x0286 / sub_0398 @0x0398 - twee bijna identieke functies die elk
// een fysieke ingang bewaken (PC2 resp. PD5) met dezelfde ontdender-/debounce-
// achtige tijdslogica (ACSR-comparatorbit als "extra" ingang, Timer1-reload
// 0xFC3A, tellers 0x69/0x6a). Zet bij bevestigde druk een bit in flagsB en
// slaat een korte "3-byte commando" in het uitgaand-telegram-scratchgebied op
// (ST X+ r23 - vermoedelijk een druk-op-de-knop-melding richting M1.1).
static void task_pincheck_pc2_bit0(void) { /* zie sub_0286, .lst @0x0286-0x0396 */ }
static void task_pincheck_pd5_bit1(void) { /* zie sub_0398, .lst @0x0398-0x0480 */ }

// sub_0482 @0x0482 (+ grote tabel 0x0490-0x086a) - dispatch op een "scherm-
// modus"-waarde (geobserveerde gevallen: 0x00,0x01,0x02,0x03,0x07,0x0e,0x10,
// 0x11,0x18,0x19,0x1c,0x31 - en generieke aftelling voor alle andere). Elke
// modus schrijft een 3- of 4-byte "displayrecord" (uit sub_0870/0872: een
// binair->BCD "double dabble"-conversie, klassiek voor het omzetten van een
// getal naar een paar decimale cijfers). ONZEKER of dit een echt LCD/7-
// segmentdisplay aanstuurt of alleen interne statusbytes klaarzet voor het
// telegram naar M1.1 - er is geen apart "display-poort"-adres gevonden dat
// hier exclusief bij hoort, dus vermoedelijk het laatste.
static void task_display_dispatch(void) { /* zie sub_0482 + sub_0870/0872, .lst @0x0482-0x086e */ }

// sub_0892 @0x0892 / sub_0958 @0x0958 - *** OpenTherm Manchester-bitbang ***.
// Schuift een buffer (gevuld door task_display_dispatch/sub_0482 met een
// OpenTherm-frame) MSB-eerst naar buiten op PD3 resp. PD4, getimed door
// Timer1-reload 0xFC3A. Op de helft van elke bit-tijd wordt de pin niet naar
// een vaste waarde gezet maar GETOGGELD op basis van de teruggelezen
// pinstand (SBIC PIND,x + invert) - dat is de Manchester-mid-bit-inversie.
// Beide functies zijn vrijwel identiek en lezen deels overlappende buffers
// (0x89/0x8c/0x8d) - vermoedelijk is een van de twee de echte Anna-lijn en
// de andere een gespiegelde/diagnostische uitgang, maar dat is niet hard
// vast te stellen uit de flash-dump alleen.
static void task_out_pd3(void) { /* zie sub_0892, .lst @0x0892-0x0956 */ }
static void task_out_pd4(void) { /* zie sub_0958, .lst @0x0958-0x0a22 */ }

// sub_0a24 @0x0a24 - vergelijkbaar met M1.1's sub_072e: bouwt het definitieve
// 12-byte telegram (header uit v6e, databytes v78/v79/v7a + workbuffer-
// velden, 8-bit-som-checksum) en koppelt PC2 rechtstreeks door naar PB2
// (fysieke doorlus van de knop naar een relais/uitgang), plus PINB.2 als
// tweede ingang die een vlag zet.
static void task_telegram_build(void) {
    build_reply_telegram();
    if (BIT_IS_CLR(PINC, PIN_PC2)) SBI(PORTB, PIN_PB2);
    if (BIT_IS_SET(PINB, 2)) flagsC |= 0x40;
}

// sub_0aa0 @0x0aa0 - *** foutcode-vertaler ***: leest de statusbyte die van
// M1.1 binnenkwam (v73_status_in) en vertaalt een KLEINE, HERKENDE set
// magic-waarden naar een oplopend foutcodenummer 1-7:
//   0x01->1  0x41->2  0x46/0x47/0x48->3  0x52->3  0x14->3  0x15->3
//   0x31->4  0x45->5  0x53->6  (elke andere waarde)->7
// Onbekende/overige waarden (->7) of geen fout: generieke aftel-/knipper-
// logica op PC4 (v74/v75 als knipper-teller). Dit is vermoedelijk de manier
// waarop T1.1 storingen van de hoofdunit (M1.1) laat zien - bv. een LED die
// N keer knippert voor foutcode N, zoals bij veel witgoed/cv-toestellen.
static void task_faultcode_pc4(void) {
    uint8_t code;
    switch (v73_status_in) {
        case 0x01: code = 1; break;
        case 0x41: code = 2; break;
        case 0x46: case 0x47: case 0x48: case 0x52: case 0x14: case 0x15: code = 3; break;
        case 0x31: code = 4; break;
        case 0x45: code = 5; break;
        case 0x53: code = 6; break;
        default:    code = 7; break;
    }
    // (asm: code wordt verder gebruikt om v74/v75 te vullen en PC4 te knipperen;
    //  zie .lst @0x0aa0-0x0b7c voor de exacte knipper-staatmachine)
    (void) code;
}

// sub_0b7e/sub_0ba0 @0x0b7e - flush: stuur tx_telegram[] naar M1.1 over de
// hardware-UART, interrupts uit tijdens transfer (identiek patroon aan
// M1.1's sub_0a40/sub_0a62, maar hier 11 bytes i.p.v. 12 - zie CPI r28,0x88
// vs. M1.1's 0xb0 in de listing).
static void task_flush_telegram(void) {
    if (BIT_IS_SET(flagsC, 6)) return;
    if (BIT_IS_CLR(flagsC, 5)) return;
    flagsC &= ~0x20;
    noInterrupts();
    for (uint8_t i = 0; i < 11; i++) uart_send_byte(tx_telegram[i]);
    flagsC &= ~0x40;
    interrupts();
}
static void uart_send_byte(uint8_t b) { // sub_0ba0 @0x0ba0 - identiek aan M1.1
    UDR = b;
    while (BIT_IS_CLR(UCSRA, 6)) {}
    SBI(UCSRA, 6);
}

// ===========================================================================
// INTERRUPTS - functioneel identiek aan M1.1 (zelfde tick-architectuur,
// zelfde lege INT0-stub, zelfde "1 byte + vlag"-USART-RXC).
// ===========================================================================
ISR(TIMER1_OVF_vect) {
    TCNT1H = 0xFC; TCNT1L = 0x3A; // LET OP: andere reload dan M1.1 (0xF84F)!
                                   // diff = 65536-0xFC3A = 966 cycli @ 8MHz = ~121us.
                                   // Geen duidelijke 666-baud-relatie (ratio 12.4x,
                                   // minder schoon dan M1.1's 6.1x) - past bij de
                                   // conclusie dat T1.1 geen bit-bang-UART draait;
                                   // dit is vermoedelijk gewoon een iets snellere
                                   // "algemene" tick voor de knop-ontdendering.
    v69_bit_ctr--;
    if (--v60_tick_mod256 == 0) flagsC |= 0x10;
    if (++v61_tick_mod100 == 0xC8) { // LET OP: 200 i.p.v. M1.1's 100!
        v61_tick_mod100 = 0;
        if (++v62_tick_mod40 == 0x28) v62_tick_mod40 = 0;
    }
    v64_ctr++;
    if (++v63_tick_mod60 >= 0x3C) {
        v63_tick_mod60 = 0;
        if (--v65_ctr == 0) { /* asm: reload-detail niet 1-op-1 hetzelfde als M1.1 */ }
        flagsB |= 0x02;
    }
}
ISR(INT0_vect) { /* leeg, zie M1.1-bestand voor de vermoedelijke reden (wake-from-sleep) */ }
ISR(USART_RXC_vect) {
    last_rx_byte = UDR;
    flagsC |= FC_RX_BYTE;
}
static void wdt_reset(void) {}

/* ===========================================================================
 * SAMENVATTING VAN WAT T1.1 FYSIEK DOET (bijgewerkt):
 * - *** OpenTherm-koppeling naar de Anna-thermostaat *** op PD3 (en een
 *   vrijwel identiek tweede kanaal op PD4, rol t.o.v. PD3 onzeker): een
 *   Data-ID-tabel (sub_0482) die exact de officiele OpenTherm-IDs dekt
 *   (Status, Control Setpoint, Master/Slave Config, Cooling Control, Max
 *   modulatie, Room Setpoint, Modulatieniveau, Room/Boiler/Return-
 *   temperatuur), Manchester-gecodeerd verzonden (sub_0892/sub_0958). T1.1
 *   emuleert hiermee richting Anna de ketel (OT-slave) en geeft Anna's
 *   setpoint/kamertemperatuur door aan M1.1 via het 9600-baud-telegram.
 * - EEN drukknop op PC2 (en een tweede op PD5), met doorlus naar PB2 als
 *   relais/uitgang.
 * - Nog een ingang op PINB.2 (rol niet vastgesteld).
 * - Statusindicatie op PC4 (foutcode-knipperlogica, zie task_faultcode_pc4)
 *   en PC5 (tweede indicator).
 * - Praat met M1.1 over 9600 baud, NIET rechtstreeks met de buitenunit.
 *
 * BELANGRIJKSTE CONCLUSIE VOOR JOUW VRAAG: de ESP32 hoeft T1.1 niet te
 * vervangen om de Chofu/Aurea-warmtepompregeling te laten werken - dat deed
 * en doet alleen M1.1/IC1. Wat je WEL mist door T1.1's socket leeg te laten:
 * de OpenTherm-koppeling met Anna (dus het automatisch uitlezen van Anna's
 * setpoint/kamertemperatuur), de foutcode-indicatie, en de twee knoppen.
 * Of de ESP32 dat ook kan overnemen: softwarematig aannemelijk (Manchester-
 * OpenTherm-bitbanging is bekend terrein), maar dat vereist waarschijnlijk
 * wel een paar extra draadjes vanaf vrije ESP32-GPIO's naar de PD3/PD4-
 * punten (of een equivalente eigen OpenTherm-interfaceschakeling) - of de
 * controlbox-print daar al passende interface-hardware voor heeft (zoals de
 * H11D1-optocouplers voor de 666-baud-kant) is niet uit deze flash-dump vast
 * te stellen en vraagt fysieke inspectie van de print.
 * =========================================================================== */
