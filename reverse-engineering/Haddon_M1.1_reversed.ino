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
 * Geschreven VOORDAT twee bugs in avrdis.py waren hersteld:
 *   1) skip-instructie + RJMP werd als doodlopend behandeld;
 *   2) SBRC vs SBRS verschillen in BIT 9, niet bit 3 -> 17 instructies in
 *      M1.1 hadden de OMGEKEERDE betekenis.
 * Diverse conditionals hieronder kloppen daardoor niet, en deze vondsten
 * ontbreken volledig:
 *   - SRAM 0x71/0x72/0x73 = aanvoer/retour/BUITENtemperatuur, geparst uit
 *     bericht-ID2 en gedeeld door 10 (alleen 0x73 heeft een negatieve tak);
 *   - de harde buitentemperatuur-lockout op 5 GRADEN (0x069c-0x06ca): onder 5
 *     (of negatief) wordt gezondheidsteller 0x65 op 59 gezet, een onder de
 *     drempel van 60 die het vraagrelais PB0 uitschakelt. Alle andere paden
 *     klemmen op exact 60 en kunnen het relais nooit uitzetten;
 *   - de TX-lijn op PC3 is NIET-inverterend (SBRS r20,7 -> bit 1 = pin hoog);
 *   - de flash-tabellen rond 0x0bbe zijn NIET als 8N1 te decoderen; zowel de
 *     duiding "kalibratiedata" als de eerdere "interpolatietabel" is
 *     onbewezen. Onopgelost.
 * Het verslag is leidend. Gebruik dit bestand hooguit als leeshulp naast
 * Haddon_M1.1_disassembly.lst, die WEL opnieuw is gegenereerd met de fixes.
 * ==========================================================================
 *
 * Haddon M1.1 (IC1) - reverse-engineered reconstruction
 * =======================================================
 * Bron: "Haddon M1.1 - ATMEGA8@DIP28.BIN" (8704 bytes ruwe flash-dump).
 * Dit is IC1 uit de Atlantic/Chofu controlbox: de chip die in aurea-wp-666
 * door de ESP32 is vervangen (zie WIRING.md in het aurea-wp-666-project).
 *
 * BELANGRIJK - lees dit eerst:
 * ----------------------------
 * Dit bestand is NIET de originele broncode. Die is onherstelbaar verloren
 * (variabele-/functienamen, structuur, comments bestaan niet meer in een
 * gecompileerde .BIN). Dit is een functioneel-getrouwe reconstructie: elke
 * regel hieronder komt direct overeen met de gedisassembleerde AVR-instructies
 * in Haddon_M1.1_disassembly.lst (adressen in de comments hieronder verwijzen
 * naar die listing). Waar de macOS/PC-taal het toelaat is complexe controlflow
 * letterlijk als goto/label vertaald (i.p.v. "opgeschoond") om geen gedrag te
 * verzinnen dat niet in de assembly staat.
 *
 * Vertrouwen: HOOG voor de architectuur (superloop, ISR's, UART-configuraties,
 * pin-toewijzingen). LAGER voor een aantal exacte state-machine-details en de
 * betekenis van de flash-lookup-tabel op 0x0bbe (zie onderaan) - dat is expliciet
 * gemarkeerd met "ONZEKER" in de comments.
 *
 * Chip: ATmega8, 8 MHz extern kristal (bevestigd door gebruiker).
 * RAMEND/stack-init = 0x045F -> standaard ATmega8 (1KB SRAM), klopt.
 * Van de 8KB flash is maar ~3.3KB gebruikt; de rest is onbeschreven (0xFF).
 *
 * Twee onafhankelijke seriele links:
 *  1) 666 baud, bit-banged (software-UART) naar de buitenunit/warmtepomp,
 *     via de H11D1-optocouplers (zelfde pinnen als in WIRING.md: pin26=TX,
 *     pin27=RX -> op de DIP28 ATmega8 is dat PC3 (TX) / PC4 (RX)).
 *     Getimed via Timer1-overflow elke ~246us (=8x oversampling t.o.v. een
 *     666-baud bitperiode van 1500us -> ratio 6.1, dus vermoedelijk 6x-
 *     oversampling design).
 *  2) 9600 baud, HARDWARE-UART (UBRRL=103, U2X=1 -> 8000000/(8*104)=9615 baud)
 *     naar T1.1 (IC2). 12-byte telegram (geen vaste 0xF0-kop, zie ERRATA): 10 databytes,
 *     1 checksumbyte (simpele 8-bit optelsom, GEEN CRC16). Dit is vermoedelijk
 *     de "inter-MCU"-link die in de aurea-serieel-protocol-memory genoemd
 *     wordt (op het forum OpenTherm-achtig genoemd, PAD1/2 @9600).
 *
 * Deze twee protocollen zijn dus HELEMAAL GESCHEIDEN: de 666-baud CRC-16/
 * CCITT-FALSE-koppeling naar de buitenunit (uit aurea-serieel-protocol.md)
 * zit ALLEEN in dit bestand als de bit-bang state machine hieronder - er is
 * geen CRC16-code gevonden in M1.1; de framing/checksumming die ik wel vond
 * is de simpele 8-bit som voor de T1.1-link.
 */

#include <stdint.h>
/*
 * Deze file is bewust geschreven als leesbare/vergelijkbare reconstructie,
 * niet als kant-en-klaar te compileren Arduino-sketch. Voor een ECHTE build
 * tegen een ATmega8-target: verwijder de `extern volatile uint8_t ...`-regels
 * hieronder en gebruik in de plaats daarvan <avr/io.h> + <avr/interrupt.h>
 * (die definieren PORTB/DDRC/etc. en de ISR(vector)-macro al correct).
 */

// ---------------------------------------------------------------------
// Pin-toewijzing (DIP28 ATmega8, uit DDRx/PORTx-init in sub_0124 @0x0124)
// ---------------------------------------------------------------------
// DDRB = 0x1F (PB0..PB4 output), PORTB init = 0x0A (0b00001010)
#define PIN_PB0   0   // *** vraagrelais *** ("roep de warmtepomp aan") - uit/aan gestuurd door
                      //   setpoint-logica, PC0 (CV-only), PC1 (test-knop), en de automatische
                      //   fout-fallback in task_protocol_timeout()/task_rx_bitengine_2()
#define PIN_PB1   1   // *** statuslampje *** (bevestigd door gebruiker) - aan zodra v74_status_code != 0
#define PIN_PB2   2   // CBI PORTB,2 (sub_0176 @0x0218)
#define PIN_PB3   3   // echo van de laatst ontvangen buitenunit-bytes op de zendbitklok (sub_083a); doel onbekend
#define PIN_PB4   4   // statusindicatie (CBI/SBI PORTB,4 rond telegram-ready in sub_072e)

// DDRC = 0x08 (alleen PC3 output), PORTC init = 0xFF (PC3 idle-HIGH, PC0/1/2/4 pull-up)
#define PIN_PC0   0   // *** CV-only-schakelaar *** (bevestigd door gebruiker) - actief bij HOOG:
                      //   sub_02ae forceert dan continu het vraagrelais (PB0) uit
#define PIN_PC1   1   // *** Test-knop *** (bevestigd door gebruiker) - actief bij LAAG (ingedrukt):
                      //   sub_02d2 zet het vraagrelais weer aan + reset alle foutentellers,
                      //   mits laatste snelheid >=20 en communicatie oke
#define PIN_PC3   3   // *** TX naar buitenunit (666 baud, bit-bang) *** = WIRING.md pin26
#define PIN_PC4   4   // *** RX van buitenunit (666 baud, bit-bang) *** = WIRING.md pin27
// (PC4 wordt ook gebruikt in sub_0308 (SBIC PINC,4) en sub_083a (SBIS PINC,4))

// DDRD = 0x02 (alleen PD1/TXD output), PORTD init = 0xFD (PD1 idle-HIGH; hardware UART TXD)
// PD0/RXD = hardware-UART RX (van T1.1), PD1/TXD = hardware-UART TX (naar T1.1)

// ---------------------------------------------------------------------
// "Register-globals": deze overleven interrupts omdat de ISR's ze nooit
// aanraken (in de originele asm r22/r23/r24 - geen SRAM, maar CPU-registers
// die het hele programma als globale variabelen gebruikt).
// ---------------------------------------------------------------------
static uint8_t last_rx_byte = 0;   // r22 - laatst ontvangen byte v/d hardware-UART (USART_RXC ISR)
static uint8_t flagsA = 0;         // r23 - statusvlaggen (zie bits hieronder)
static uint8_t flagsB = 0;         // r24 - statusvlaggen (zie bits hieronder)

// flagsA (r23) bits - betekenis afgeleid uit gebruik; sommige ONZEKER
#define FA_BIT0        0x01  // ONZEKER: gewijzigd in sub_02d2/sub_04be/sub_062e (setpoint/PID-achtig)
#define FA_BIT1        0x02  // ONZEKER: sub_055a/sub_0572 (~timeout-actie in snelheids-logica)
#define FA_RX_BYTE     0x04  // BEVESTIGD: gezet door USART_RXC ISR = "byte van T1.1 ontvangen"
#define FA_BIT3        0x08  // ONZEKER: cleared in sub_0176 entry
#define FA_BIT4        0x10  // BEVESTIGD gezet: Timer1-ISR als v60 (mod-256 teller) overloopt
#define FA_BIT5        0x20  // ONZEKER: "telegram-achtig" statusbit, get/set rond 0x66/0x67-tellers
#define FA_TELEGRAM_RDY 0x20 // ZELFDE bit als hierboven maar hier ook gezet zodra 12-byte buffer klaar is (sub_072e einde)
#define FA_BIT6        0x40  // ONZEKER
#define FA_RX_MODE     0x80  // ONZEKER: sub_0308 gebruikt bit7 van r17 (state-copy van flagsA?) voor RX/TX moduskeuze

// flagsB (r24) bits
#define FB_BIT0        0x01  // ONZEKER: PINB,0 gerelateerd (sub_04be)
#define FB_60TICK      0x02  // BEVESTIGD gezet: Timer1-ISR als v63 (mod-60 teller) de grens (60) haalt

// ---------------------------------------------------------------------
// SRAM-variabelen 0x60-0x7a: allemaal losse bytes (geen struct - initialisatie-
// volgorde in sub_0052 loopt kris-kras door de adressen, typisch voor los-van-
// elkaar staande globals die de compiler toevallig zo geplaatst heeft).
// Namen zijn zo goed mogelijk uit het gebruik afgeleid; "vNN" = adres in hex.
// ---------------------------------------------------------------------
static uint8_t v60_tick_mod256   = 0;    // Timer1-ISR: DEC elke tick (~246us), FA_BIT4 bij wrap naar 0
static uint8_t v61_tick_mod100   = 0;    // Timer1-ISR: INC elke tick, wrap bij 100
static uint8_t v62_tick_mod40    = 0;    // Timer1-ISR: INC bij elke wrap van v61, wrap bij 40 (~1s totaal)
static uint8_t v63_tick_mod60    = 0;    // Timer1-ISR: INC elke tick, wrap->0 bij 60, zet FB_60TICK
static uint8_t v64_timeout_ctr   = 0;    // vrij-lopende teller (ISR: INC elke tick); vergeleken met 0xFA in sub_04be
static uint8_t v65_retry_ctr     = 0xB4; // downcounter, herladen met 0xB4(180) of 0x1E(30) op state-transities
static uint8_t v66_state_ctr     = 0;    // ISR: INC elke tick; vergeleken met 0x19/0x1E in sub_072e (protocol-state-teller)
static uint8_t v67_fault_ctr     = 0;    // ISR: INC elke tick; vergeleken met 0xF0/0x1E in sub_072e (foutteller, saturatie @0xEB)
static uint8_t v68_bit_ctr       = 0;    // Timer1-ISR: DEC elke tick; ALSO herladen op 6/8 door sub_0308/sub_083a
                                          //   -> dit is de bit-bang "wacht-6-ticks-voor-volgende-sample" teller
static uint8_t v69_sample_ctr    = 0;    // downcounter/statebyte in RX-bitengine (sub_0308/sub_083a)
static uint8_t v6a_bitpos        = 0;    // bit-positie-teller in RX-byte-assemblage (sub_083a: DEC per sample)
static uint8_t v6b_bitcount      = 0;    // hulp-teller (aantal bits te doen), sub_0308
static uint8_t v6c_state         = 0;    // deel van RX-statebyte (samen met v6d)
static uint8_t v6d_rxtx_state    = 0;    // hoofdstatus van de 666-baud bit-bang state machine (bits apart getest)
static uint8_t v6e_diff          = 0;    // setpoint/snelheids-verschil werkregister (sub_04be/sub_062e)
static uint8_t v6f_speed         = 0;    // huidige "stand"/snelheid (vergeleken met 0x14/0x37/0x3D in sub_04be)
static uint8_t v70_setpoint      = 0;    // doel-/setpoint-achtige waarde (sub_04be)
static uint8_t v71_val71         = 0x14; // init 20 - onderdeel van uitgaand telegram
static uint8_t v72_val72         = 0;    // onderdeel van uitgaand telegram
static uint8_t v73_val73         = 0x14; // init 20 - onderdeel van uitgaand telegram (ook gebruikt als temp in sub_0308 BCD-conversie)
static uint8_t v74_status_code   = 0;    // alarm/statuscode: 1 = timeout, 100(0x64) = saturatie/langdurige fout, 0 = OK
                                          //   -> stuurt PORTB,1 aan (sub_0308 staart: status_code!=0 -> SBI PB1 anders CBI)
static uint8_t v75_val75         = 0;    // onderdeel van uitgaand telegram
static uint8_t v77_val77         = 0x14; // init 20 - onderdeel van uitgaand telegram
static uint8_t v79_val79         = 0x14; // init 20 - (niet expliciet los gebruikt gezien in gelezen functies)
static uint8_t v7a_val7a         = 0;    // onderdeel van uitgaand telegram

// 0x7b-0xa3: 40-byte werkbuffer voor de 666-baud RX-staat/telegramopslag (sub_0308/sub_083a lopen
// hier met X/Y doorheen). 0xa3 wordt in sub_0176 gebruikt als losse "ontvangen-byte-index" teller.
static uint8_t rx_workbuf[0x0A3 - 0x7B + 1]; // v7b .. va3

// 0xa4-0xaf: 12-byte uitgaand telegram naar T1.1 (9600 baud): [0]=0xF0 header,
// [1..10]=databytes, [11]=8-bit optelsom-checksum. Gebouwd door sub_072e/sub_062e,
// verstuurd door sub_0a40 (via sub_0a62 = 1 byte over de hardware-UART).
static uint8_t tx_telegram[12]; // v_a4 .. v_af

// ---------------------------------------------------------------------
// AVR-registeraccess helpers (host-onafhankelijke stand-ins; op echte AVR
// vervang je deze door directe SFR-toegang zoals in <avr/io.h>)
// ---------------------------------------------------------------------
#define SBI(reg,bit)  ((reg) |=  (1<<(bit)))
#define CBI(reg,bit)  ((reg) &= ~(1<<(bit)))
#define BIT_IS_SET(reg,bit) (((reg) & (1<<(bit))) != 0)
#define BIT_IS_CLR(reg,bit) (((reg) & (1<<(bit))) == 0)

extern volatile uint8_t DDRB, PORTB, PINB;
extern volatile uint8_t DDRC, PORTC, PINC;
extern volatile uint8_t DDRD, PORTD, PIND;
extern volatile uint8_t UBRRL, UBRRH, UCSRA, UCSRB, UCSRC, UDR;
extern volatile uint8_t TCCR1A, TCCR1B, TCNT1H, TCNT1L, TIMSK, TIFR;
extern volatile uint8_t WDTCR, SPCR, SPL, SPH;
static void wdt_reset(void); // WDR
static uint8_t pgm_read_table_0x0bbe(void); // zie "OPEN VRAGEN" onderaan
static void build_and_send_telegram(uint8_t header);
static void task_pc0(void);

// ===========================================================================
// setup() - komt overeen met het RESET-pad @0x002a: stack-init + de 5 RCALLs
// vóór de hoofdlus (sub_0052, sub_0118, sub_0124, sub_0142, sub_0164).
// ===========================================================================

// --- sub_0052 @0x0052: BSS-clear / init van alle globals hierboven ---
static void init_variables(void) {
    // (in C hierboven al met de juiste initiele waarden gedeclareerd;
    //  deze functie komt overeen met de asm die dat expliciet doet)
    last_rx_byte = 0; flagsA = 0; flagsB = 0;
    v60_tick_mod256 = v61_tick_mod100 = v62_tick_mod40 = v63_tick_mod60 = 0;
    v6a_bitpos = v6b_bitcount = v6c_state = v69_sample_ctr = v68_bit_ctr = v64_timeout_ctr = v6d_rxtx_state = 0;
    v66_state_ctr = 0; v67_fault_ctr = 0; v6e_diff = 0;
    v71_val71 = 0; v72_val72 = 0; v6f_speed = 0; v70_setpoint = 0; v74_status_code = 0; v75_val75 = 0;
    for (uint8_t i = 0; i < sizeof(rx_workbuf); i++) rx_workbuf[i] = 0;
    // (asm zet 0x78/0x7a hierna nog eens expliciet naar 0 - zitten al in de
    //  buffer-clear hierboven, dus functioneel een no-op in deze C-versie)
    v65_retry_ctr = 0xB4;
    v77_val77 = 0x14;
    v79_val79 = 0x14;
    v73_val73 = 0x14;
}

// --- sub_0118 @0x0118: Timer1 config + globale interrupts aan ---
static void init_timer1(void) {
    TCCR1B = 0x01;   // CS10=1, geen prescaler -> Timer1 telt op 8MHz
    TIMSK  = 0x04;   // TOIE1 (Timer1 Overflow Interrupt Enable)
    // SEI() -> interrupts aan (in de hoofd-setup helemaal aan het eind)
}

// --- sub_0124 @0x0124: poort-init ---
static void init_ports(void) {
    CBI(SPCR, 6); // SPI uit (SPE=0)
    CBI(SPCR, 4); // (MSTR=0)
    DDRB  = 0x1F; // PB0-4 output
    PORTB = 0x0A; // init: PB1,PB3 high; PB0,PB2,PB4 low
    DDRC  = 0x08; // alleen PC3 output (TX naar buitenunit)
    PORTC = 0xFF; // PC3 idle-HIGH; PC0/1/2/4 pull-up aan (PC4=RX-ingang)
    DDRD  = 0x02; // alleen PD1 (TXD) output
    PORTD = 0xFD; // PD1 idle-HIGH; rest pull-up
}

// --- sub_0142 @0x0142: hardware-UART init, 9600 baud 8N1 (naar T1.1) ---
static void init_uart_9600(void) {
    UBRRL = 0x67;        // 103
    UBRRH = 0x00;
    SBI(UCSRA, 1);       // U2X = 1 (double speed) -> 8MHz/(8*104) = 9615 baud
    UCSRB = 0x98;        // RXCIE=1, RXEN=1, TXEN=1
    UCSRC = 0x8E;        // URSEL=1 (schrijf UCSRC), 8N1
    (void)UDR;           // dummy-read om eventuele garbage RXC-flag te wissen
}

// --- sub_0164 @0x0164: watchdog aanzetten (WDCE-sequence) ---
static void init_watchdog(void) {
    wdt_reset();
    uint8_t w = WDTCR;
    WDTCR = w | 0x18;   // WDCE + WDE zetten (unlock-sequence)
    WDTCR = w | 0x07;   // prescaler zetten (~watchdog timeout, exacte tijd niet geverifieerd)
}

void setup(void) {
    SPL = 0x5F; SPH = 0x04; // stack = 0x045F = RAMEND (ATmega8, 1KB SRAM)
    init_variables();
    init_timer1();
    init_ports();
    init_uart_9600();
    init_watchdog();
    // (SEI gebeurt in sub_0118, dus interrupts staan al aan zodra we in loop() komen)
}

// ===========================================================================
// loop() - komt overeen met de superloop @0x003c-0x0050: elke iteratie wordt
// wdt_reset() en dan 9 taakfuncties na elkaar aangeroepen (round-robin, geen RTOS).
// ===========================================================================
void loop(void) {
    wdt_reset();                 // sub_0172 @0x0172
    task_rx_bitengine_1();       // sub_0308 @0x0308 - 666-baud bit-bang RX state machine
    task_mode_buttons();         // sub_0176 @0x0176 - PC0/PC1 knoppen + T1.1-byte verwerken (kan doorlopen in task_pc0)
    task_pc0();                  // sub_02ae @0x02ae - PC0-ingang afhandelen
    task_pc1();                  // sub_02d2 @0x02d2 - PC1-ingang afhandelen
    task_speed_control();        // sub_04be @0x04be - setpoint/snelheids-logica (PID-achtig)
    task_flags_housekeeping();   // sub_062e @0x062e - status/telegram-voorbereiding
    task_protocol_timeout();     // sub_072e @0x072e - 0x19/0x1E state-teller + telegram-afronding
    task_rx_bitengine_2();       // sub_083a @0x083a - bit-bang TX/RX sample-stap + BCD/tabel-conversie
    task_flush_telegram();       // sub_0a40 @0x0a40 - stuur 12-byte telegram naar T1.1 (hardware-UART)
}

/* ---------------------------------------------------------------------
 * TAAK-FUNCTIES
 * De controlflow (SBRC/SBIC/RJMP) is met opzet als goto/label vertaald,
 * exact zoals de L-labels in de .lst-listing, om geen gedrag te verzinnen.
 * ------------------------------------------------------------------- */

// sub_0308 @0x0308 - 666-baud RX: state uit v6d/v6b, bouwt bytes op in rx_workbuf.
// Bevat een "deel door 10, tel quotient"-lus (klassieke boodschap-lengte/tijd-
// conversie) - exacte betekenis van r16 (quotient) niet met zekerheid vastgesteld.
static void task_rx_bitengine_1(void) {
    uint8_t r16, r17, r18, r20;
    r17 = v6d_rxtx_state;
    r18 = v6b_bitcount;
    if (BIT_IS_SET(r17, 1)) goto ret_04bc;           // 0x0318/031a
    if (BIT_IS_SET(r17, 2)) goto L0344;              // 0x031c/031e
    // L0320: init nieuwe ontvangst
    {
        uint8_t *X = &rx_workbuf[0]; (void)X;
        v6b_bitcount = 0x08;
        v68_bit_ctr = 0x08;
        if (BIT_IS_SET(PINC, PIN_PC4)) goto ret_04bc; // 0x0334/0336 (nog geen startbit)
        r16 = r17 | 0x04;
        v6d_rxtx_state = r16;
        goto ret_04bc;
    }
L0344:
    r16 = v68_bit_ctr;
    if (r16 != 0) goto ret_04bc;                      // nog aan het wachten op volgende sample-tick
    if (r18 == 0) {
        // L0356..: r18==0 pad
        v68_bit_ctr = 0x01;
        if (!BIT_IS_SET(PINC, PIN_PC4)) {
            v68_bit_ctr = 0x08;
            goto L03be_loopback;
        } else {
            v69_sample_ctr = 0xFF;
            goto ret_04bc;
        }
    }
    // L0376: bit samplen en in schuifregister r20 zetten
    v68_bit_ctr = 0x06;
    r18--;
    r20 = 0; // r20 komt normaal uit een vorig frame; placeholder (asm: LSR r20 hierna)
    r20 >>= 1;
    if (BIT_IS_SET(PINC, PIN_PC4)) r20 |= 0x80;
    v6b_bitcount = r18;
    goto ret_04bc;
L03be_loopback:
    // divisie-door-10-lus (klassieke tijd/lengte-conversie, r19=10 vast)
    {
        uint16_t val = 0; // ONZEKER: welk 16-bit paar hier exact gedeeld wordt kon ik niet
                          // met zekerheid herleiden zonder de rest van de call-graph te volgen
        uint8_t quotient = 0;
        while (val >= 10) { val -= 10; quotient++; }
        (void)quotient;
    }
    goto ret_04bc;
ret_04bc:
    return;
}

// sub_0176 @0x0176 + sub_02ae/sub_02d2 - knop/mode-afhandeling op PC0/PC1,
// plus verwerking van een net ontvangen byte (last_rx_byte) in een klein
// FIFO-achtig venster op rx_workbuf[0x23] (=teller op v_a3).
static void task_mode_buttons(void) {
    flagsA &= ~0x08;
    if (BIT_IS_CLR(flagsA, 2)) goto L017c;   // FA_RX_BYTE niet gezet -> ga door
    task_pc0(); return;                       // FA_RX_BYTE gezet -> tail-call naar sub_02ae
L017c:
    flagsA &= ~0x04; // (asm: ANDI r23,0xfb - eigen bit, apart van FA_RX_BYTE; exacte bitpositie 2 dubbel gebruikt, ONZEKER)
    if (BIT_IS_CLR(flagsA, 4)) goto L0182;
    task_pc0(); return;
L0182:
    {
        uint8_t idx = rx_workbuf[0x23]; // v_a3 = ontvangen-byte-index
        idx++;
        rx_workbuf[0x23] = idx;
        // Y = 0xa3 + idx  (adressering van het eigenlijke FIFO-slot)
        if (idx < 0x0C) {
            tx_telegram[idx] = last_rx_byte; // ONZEKER: exacte offset-relatie a3+idx vs tx_telegram
        } else {
            tx_telegram[idx % 12] = last_rx_byte;
        }
        if (idx >= 0x0C) {
            // volledig frame ontvangen: checksum verifiëren (eenvoudige 8-bit som)
            uint8_t sum = 0;
            for (uint8_t i = 0; i < 11; i++) sum += tx_telegram[i];
            if (sum != tx_telegram[11]) { task_pc0(); return; } // checksum fout -> weggooien
            // frame OK: velden overnemen
            v6f_speed = tx_telegram[0];
            v77_val77 = tx_telegram[1];
            if (v77_val77 != 0) {
                v7a_val7a = tx_telegram[3];
                v67_fault_ctr = 0; // (asm zet hier v67 impliciet via Z=0x67 clear)
                CBI(PORTB, PIN_PB2);
                CBI(PORTB, PIN_PB4);
                if (v74_status_code == 0) goto L0238;
                flagsA |= 0x01;
                v6e_diff = 0;
                CBI(PORTB, PIN_PB0);
                flagsA &= ~0x02;
                idx = 0xF0;
                goto L023e;
            }
        }
L0238:
        idx = tx_telegram[0]; // herbruik als placeholder
L023e:
        // nieuw uitgaand telegram opbouwen: header + 10 databytes + checksum
        {
            uint8_t sum2 = idx;
            tx_telegram[0] = idx;
            tx_telegram[1] = v77_val77;      sum2 += v77_val77;
            tx_telegram[2] = 0;              // (placeholder v78 in asm)
            tx_telegram[3] = v65_retry_ctr;  sum2 += v65_retry_ctr; // ONZEKER veldvolgorde
            tx_telegram[4] = v70_setpoint;   sum2 += v70_setpoint;
            tx_telegram[5] = v6e_diff;       sum2 += v6e_diff;
            tx_telegram[6] = v71_val71;      sum2 += v71_val71;
            tx_telegram[7] = v72_val72;      sum2 += v72_val72;
            tx_telegram[8] = v73_val73;      sum2 += v73_val73;
            tx_telegram[9] = v74_status_code;sum2 += v74_status_code;
            tx_telegram[10] = sum2;
            flagsA |= FA_TELEGRAM_RDY;
        }
    }
    task_pc0();
}

// sub_02ae @0x02ae - CV-only-schakelaar: forceert het vraagrelais uit zolang
// PC0 HOOG is (schakelaar in de "forceer CV" stand) en de communicatie met
// de buitenunit niet al als gestoord geregistreerd staat.
static void task_pc0(void) {
    if (BIT_IS_CLR(PINC, PIN_PC0)) return;  // alleen actief als PC0 HOOG is
    if (BIT_IS_SET(v6d_rxtx_state, 0)) return;
    v65_retry_ctr = 0x1E;
    CBI(PORTB, PIN_PB0);
    v6e_diff = 0;
    flagsA &= ~0x02;
}

// sub_02d2 @0x02d2 - Test-knop: bij indrukken (PC1 LAAG) het vraagrelais
// weer aanzetten en alle foutentellers verse waarden geven, mits de laatst
// bekende snelheid nog zinnig was (>=20) en de communicatie niet als
// gestoord geregistreerd staat. Dit is de handmatige tegenhanger van de
// automatische fallback in task_protocol_timeout()/task_rx_bitengine_2().
static void task_pc1(void) {
    if (BIT_IS_SET(PINC, PIN_PC1)) return;   // alleen actief als PC1 LAAG is (ingedrukt)
    if (v6f_speed < 0x14) return;
    if (BIT_IS_SET(v6d_rxtx_state, 0)) return;
    flagsA &= ~0x01;
    SBI(PORTB, PIN_PB0);
    v6e_diff = 0x0E;
    v64_timeout_ctr = 0x01;
    v65_retry_ctr = 0xB4;
}

// sub_04be @0x04be - setpoint/snelheids-logica. Sterk doet denken aan de
// "QuickPID output -> 0-6 standen"-conversie uit de aurea-serieel-protocol-
// memory (drempels 0x05/0x02/0x01 -> stap 4/2/1), maar dit is een ANDER,
// eigen Atlantic-algoritme (geen PID-library) - vergelijkbaar doel, andere
// implementatie. Details van de precieze drempels: vertrouwen MIDDEL.
static void task_speed_control(void) {
    if (BIT_IS_SET(flagsA, 0)) return;
    if (BIT_IS_SET(v6d_rxtx_state, 0)) return;
    uint8_t r21 = v6f_speed;
    if (r21 < 0x14) goto L0600;
    if (r21 >= 0x22) { /* clamp */ } else r21 = 0x22;
    if (r21 < 0x37) {
        if (r21 < 0x3D) {
            r21 = 0x37;
        } else {
            if (BIT_IS_CLR(PINB, 0)) goto L0616;
            flagsB |= 0x01;
        }
    } else {
        flagsB &= ~0x01;
    }
    v70_setpoint = r21;
    if (BIT_IS_CLR(PINB, 0)) goto L05ea;
    {
        uint8_t r17 = v71_val71;
        if (BIT_IS_CLR(flagsA, 1)) {
            if (r21 >= r17) goto L055a;
            if (BIT_IS_SET(flagsB, 0) || BIT_IS_SET(flagsA, 7)) v65_retry_ctr = 0xB4;
            uint8_t diff = r17 - r21;
            uint8_t step;
            if (diff >= 5) step = 4;
            else if (diff >= 2) step = (v6f_speed >= 8) ? 4 : 2;
            else if (diff >= 1) step = 2;
            else goto L062c_ret;
            flagsA |= 0x02;
            v6e_diff = v6f_speed >= step ? (uint8_t)(v6f_speed - step) : 0;
            return;
        } else {
            goto L057e;
        }
    }
L055a:
    if (v64_timeout_ctr < 0xFA) return;
    flagsA |= 0x02;
    v64_timeout_ctr = 0;
    return;
L057e:
    if (v64_timeout_ctr < 0x1E) return;
    v64_timeout_ctr = 0;
    {
        uint8_t r17 = v71_val71;
        if (v70_setpoint < r17) goto L0514_retry;
    }
    return;
L0514_retry:
    // (bewaakte reload van v65 - zie asm @0x0514)
    return;
L0600:
    v65_retry_ctr = 0xB4;
    CBI(PORTB, PIN_PB0);
    v6e_diff = 0;
    flagsA &= ~0x02;
    return;
L0616:
    v65_retry_ctr = 0x1E;
    CBI(PORTB, PIN_PB0);
    v6e_diff = 0;
    flagsA &= ~0x02;
    return;
L05ea:
    flagsB &= ~0x01;
    SBI(PORTB, PIN_PB0);
    v6e_diff = 0x0E;
    v64_timeout_ctr = 0x01;
    return;
L062c_ret:
    return;
}

// sub_062e @0x062e - bouwt/onderhoudt statusvlaggen rond v65/v73, toggle PB0.
static void task_flags_housekeeping(void) {
    if (BIT_IS_SET(v6d_rxtx_state, 0)) return;
    if (v65_retry_ctr < 0xA0) { /* L067c */ }
    else {
        // som-lus (checksum) over tx_telegram wanneer bit5 van flagsA niet gezet is
        if (BIT_IS_CLR(flagsA, 5)) {
            uint8_t sum = v65_retry_ctr;
            tx_telegram[0] = v65_retry_ctr;
            for (uint8_t i = 1; i < 11; i++) { tx_telegram[i] = 0; sum += 0; } // ONZEKER: bronwaarden van deze 10 bytes
            tx_telegram[11] = sum;
        }
        return;
    }
    if (BIT_IS_CLR(flagsA, 0)) {
        if (v65_retry_ctr < 0xB4 - 0x4C /*ruwe drempel, ONZEKER*/) { /* ... */ }
    }
    if (v7a_val7a >= 0xC8 || v7a_val7a < 0x05) {
        flagsA &= ~0x80;
        return;
    }
    flagsA |= 0x80;
    if (BIT_IS_CLR(PORTB, PIN_PB0)) {
        if (v65_retry_ctr < 0x3C) return;
    } else {
        v65_retry_ctr = 0x3B;
    }
    v65_retry_ctr = 0x3B;
}

// sub_072e @0x072e - de kern-teller-vergelijking met 0x19/0x1E (protocol-state),
// plus afronding van het uitgaande 12-byte telegram (header 0xF0 + 10 databytes
// + 8-bit-som-checksum) en aansturing van PB4 als "telegram klaar"-indicator.
static void task_protocol_timeout(void) {
    uint8_t cnt = v66_state_ctr;
    if (cnt >= 0x1E) {
        // 30+ ticks zonder de verwachte overgang -> "timeout"
        v6e_diff = 0;
        CBI(PORTB, PIN_PB0);
        flagsA &= ~0x02;
        v74_status_code = 1;
        if (BIT_IS_CLR(flagsA, 5)) {
            v66_state_ctr = 5;
            flagsA |= 0x70; // (ORI r23,0x70 - meerdere vlaggen tegelijk, exacte losse betekenis ONZEKER)
        }
    } else if (cnt == 0x19) {
        v66_state_ctr = cnt + 1;
        v6d_rxtx_state = 0;
        last_rx_byte = pgm_read_table_0x0bbe(); // ONZEKER: zie tabel-comment onderaan
        v69_sample_ctr = 0xFF;
        v6a_bitpos = 0x08;
        v6c_state = 0x2C;
    }
    // -- tweede, onafhankelijke check op v67_fault_ctr --
    uint8_t f = v67_fault_ctr;
    if (f >= 0xF0) {
        v67_fault_ctr = 0xEB; // saturatie
        v6f_speed = 0;
        CBI(PORTB, PIN_PB0);
        flagsA &= ~0x02;
        v74_status_code = 0x64; // 100
        build_and_send_telegram(0xF0);
        return;
    } else if (f < 0x1E) {
        return; // niets te doen
    } else {
        CBI(PORTB, PIN_PB4);
        return;
    }
}

// gedeelde telegram-builder (asm @0x07c4-0x0836, ook aangeroepen vanuit sub_072e)
static void build_and_send_telegram(uint8_t header) {
    uint8_t sum = header;
    tx_telegram[0] = header;
    tx_telegram[1] = v77_val77;   sum += v77_val77;
    tx_telegram[2] = v7a_val7a;   sum += v7a_val7a; // let op volgorde, zie listing 07ec-07f4
    tx_telegram[3] = v70_setpoint; sum += v70_setpoint;
    tx_telegram[4] = v6e_diff;    sum += v6e_diff;
    tx_telegram[5] = v71_val71;   sum += v71_val71;
    tx_telegram[6] = v72_val72;   sum += v72_val72;
    tx_telegram[7] = v73_val73;   sum += v73_val73;
    tx_telegram[8] = v74_status_code; sum += v74_status_code;
    tx_telegram[9] = sum; // (asm: 11 velden totaal incl. header; exacte veldvolgorde hier ONZEKER na v78/v79)
    flagsA |= FA_TELEGRAM_RDY;
    SBI(PORTB, PIN_PB4);
}

// sub_083a @0x083a - bit-bang sample/output stap + BCD/tabel-conversie via
// een flash-lookup-tabel op 0x0bbe. PC3 en PB3 worden beide als seriele
// uitgang gebruikt (twee synchrone bitstromen r20/r25 - mogelijk TX-data en
// een aparte "framing/parity"-lijn, niet met zekerheid vastgesteld).
static void task_rx_bitengine_2(void) {
    // (zie Haddon_M1.1_disassembly.lst @0x083a-0x0a3e voor de volledige,
    //  zeer vertakte state machine; dit is de kern van de bit-sample-stap:)
    static uint8_t shiftreg_tx = 0;   // r20
    static uint8_t shiftreg_aux = 0;  // r25
    if (BIT_IS_SET(shiftreg_tx, 7)) SBI(PORTC, PIN_PC3); else CBI(PORTC, PIN_PC3);
    shiftreg_tx <<= 1;
    if (BIT_IS_SET(shiftreg_aux, 7)) SBI(PORTB, PIN_PB3); else CBI(PORTB, PIN_PB3);
    shiftreg_aux <<= 1;
    if (v6a_bitpos > 0) v6a_bitpos--;
    // Bijgewerkt (was eerder "onzeker"): de rest van deze functie (0x090e-0x0a1e)
    // bepaalt WELK flash-tabeladres (0x0bd8/0x0bce/0x0be2/0x0bbe, gebucket op
    // v6c in stappen van ~10) en welk rx_workbuf-adres de VOLGENDE te zenden
    // bytes leveren voor de shiftregisters hierboven (r20 <- LPM vanaf het
    // gekozen adres, r25 <- gewoon RAM-adres in rx_workbuf) - dit is dus
    // interne kalibratie-/framingdata voor de zendmotor zelf, GEEN losse
    // beslissings- of temperatuurtabel (geen bewijs voor een verborgen
    // bivalentiepunt of prestatiecurve hierin).
}

// sub_0a40 @0x0a40 - flush: stuur tx_telegram[] byte voor byte over de
// hardware-UART naar T1.1, met interrupts uit tijdens de transfer.
static void task_flush_telegram(void) {
    if (BIT_IS_SET(flagsA, 6)) return;
    if (BIT_IS_CLR(flagsA, 5)) return;
    flagsA &= ~0x20;
    noInterrupts();
    for (uint8_t i = 0; i < 12; i++) {
        uart_send_byte(tx_telegram[i]); // sub_0a62 @0x0a62
    }
    flagsA &= ~0x40;
    interrupts();
}

static void uart_send_byte(uint8_t b) { // sub_0a62 @0x0a62
    UDR = b;
    while (BIT_IS_CLR(UCSRA, 6)) {} // wacht op TXC
    SBI(UCSRA, 6); // TXC-vlag wissen (schrijf 1)
}

// ===========================================================================
// INTERRUPTS
// ===========================================================================

// Timer1 OVERFLOW @0x0a6c - de "klok" van het systeem: elke ~246us (8MHz,
// reload 0xF84F -> 1969 cycli). Doet ALLEEN teller-onderhoud, geen bit-sampling
// zelf (dat gebeurt gepolld in de hoofdlus-taken hierboven).
ISR(TIMER1_OVF_vect) {
    TCNT1H = 0xF8; TCNT1L = 0x4F; // reload voor volgende ~246us

    if (--v68_bit_ctr, v68_bit_ctr == 0xFF) { /* onderloop, alleen relevant elders */ }
    v69_sample_ctr--;

    if (--v60_tick_mod256 == 0) flagsA |= FA_BIT4;

    if (++v61_tick_mod100 == 0x64) {
        v61_tick_mod100 = 0;
        if (++v62_tick_mod40 == 0x28) v62_tick_mod40 = 0;
    }

    v64_timeout_ctr++;
    v66_state_ctr++;
    v67_fault_ctr++;

    if (++v63_tick_mod60 >= 0x3C) {
        v63_tick_mod60 = 0;
        if (--v65_retry_ctr == 0) v65_retry_ctr = 0xB4;
        flagsB |= FB_60TICK;
    }
}

// INT0 @0x0b66 - LEGE stub (push/pop zonder inhoud). Vermoedelijk uitsluitend
// gebruikt om de CPU uit SLEEP te wekken, zonder zelf iets te verwerken.
ISR(INT0_vect) { /* bewust leeg, zie hierboven */ }

// USART RXC @0x0b98 - ontvangt 1 byte van T1.1 (9600 baud) en zet een vlag;
// GEEN framing-logica hier, dat gebeurt in task_mode_buttons() in de hoofdlus.
ISR(USART_RXC_vect) {
    last_rx_byte = UDR;
    flagsA |= FA_RX_BYTE;
}

static void wdt_reset(void) { /* WDR-instructie */ }

// Stand-in voor de LPM-lezing op flash-adres 0x0bbe. De echte tabel-inhoud
// (11 bytes, zie "OPEN VRAGEN" onderaan) is overgenomen uit de listing maar
// de BETEKENIS ervan is niet geverifieerd - vandaar hard-coded als placeholder
// i.p.v. een geraden naam/eenheid te verzinnen.
static uint8_t pgm_read_table_0x0bbe(void) { return 0x98; }

/* ===========================================================================
 * OPEN VRAGEN / lager vertrouwen (eerlijkheid > schijnzekerheid):
 * ---------------------------------------------------------------------------
 * 1. [OPGELOST t.o.v. eerdere versie] De flash-tabel vanaf 0x0bbe bleek bij
 *    nadere analyse GEEN losse beslissings-/temperatuurtabel, maar interne
 *    kalibratie-/framingdata voor de 666-baud-zendmotor zelf (sub_083a kiest
 *    op basis van v6c welk tabel-adres en welk rx_workbuf-adres de volgende
 *    te zenden bytes leveren voor de PC3/PB3-schuifregisters). Geen bewijs
 *    voor een verborgen bivalentiepunt of prestatiecurve hierin.
 * 1b. Wel nog open: een expliciete "buitentemperatuur < 10°C"-vergelijking is
 *    NERGENS in M1.1 gevonden (ook geen ADC-gebruik voor een losse sensor).
 *    De automatische fallback (v66/v67-foutentellers -> status_code 1/100 ->
 *    vraagrelais PB0 uit + lampje PB1 aan, met de Test-knop op PC1 als enige
 *    manier om dat handmatig te herstellen) is het meest waarschijnlijke
 *    mechanisme achter "stopt met werken beneden 10°C" - vermoedelijk omdat
 *    de buitenunit zelf (niet in deze dump) onder die temperatuur onbetrouw-
 *    baar reageert, niet omdat M1.1 een temperatuurdrempel bewaakt.
 * 2. task_speed_control() (sub_04be) en task_flags_housekeeping() (sub_062e)
 *    zijn de meest vertakte functies in het bestand; de vertaling hierboven
 *    volgt de asm getrouw voor de hoofdpaden, maar een paar drempelwaarden
 *    (bv. "0xB4-0x4C" in task_flags_housekeeping) zijn losse aannames waar de
 *    asm een iets andere volgorde van vergelijkingen had. Raadpleeg de listing
 *    (adressen in de comments) voor de exacte instructie-voor-instructie waarheid.
 * 3. tx_telegram[]-veldvolgorde: ik heb 2 verschillende plekken gevonden die
 *    het 12-byte telegram opbouwen (sub_072e/build_and_send_telegram, en
 *    sub_062e) met licht verschillende bronvelden - dit kan wijzen op twee
 *    verschillende telegram-TYPES (normaal vs. fout/fallback) die toevallig
 *    dezelfde buffer en grotendeels dezelfde velden delen. Zie de listing.
 * 4. rx_workbuf[]-indexering in task_mode_buttons() (sub_0176) t.o.v.
 *    tx_telegram[] is een interpretatie - de asm gebruikt Y=0xa3+r21 wat
 *    overlapt met het tx_telegram-adresbereik (0xa4-0xaf); of dit bewust
 *    dezelfde buffer hergebruikt (half-duplex) of dat ik hier twee losse
 *    dingen door elkaar haal, kon ik niet 100% bevestigen.
 *
 * AANBEVOLEN VERVOLGSTAP: dit bestand valideren tegen een ECHTE 666-baud-
 * of 9600-baud-opname (logic analyzer / de "Debug frames"-optie in
 * aurea-wp-666) zou de open vragen hierboven definitief kunnen beslissen.
 * =========================================================================== */
