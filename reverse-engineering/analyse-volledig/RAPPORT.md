# RAPPORT - Volledige analyse van de originele controlbox-firmware (Haddon M1.1 en T1.1)

Atlantic Aurea 5 hybrid / Chofu AEYC-0643XU. Alle bestanden van dit rapport staan in `analyse-volledig/`. Bij het opstellen is niets geflasht en niets met
Home Assistant of de units gedaan. Niet gepubliceerd (bevatten fabrikantsfirmware): de ruwe dumps, de volledige
disassembly-listings (`listing_*.txt`) en de decoder-controle over alle opcodes (`verify_decoder.txt`); de scripts maken ze
opnieuw als je zelf een dump hebt.

## Inhoudsopgave

0. Werkwijze, bewijsniveaus en gereedschap
1. Dekking: is elke flash-byte verklaard? (dekkingstabel, round-trip, controle van avrdis.py)
2. Functies, aanroepgraaf, interrupts, pinnen, RAM, EEPROM en fuses (beide chips)
3. Telegrammen en protocol (666-baud TX-keten, RX-parser, inter-MCU-link)
4. De twee weestelegrammen op 0x0baa en 0x0bb4
5. De ID3-vorm "B2 02" en de vaste bytes in ID0/ID1/ID3
6. DIP-switches, ketelype, vraagrelais, ketelbijstook en de 5-graden-regel (exacte condities)
7. Emulatie: wat is er echt gedraaid en wat is daarmee bevestigd
8. Correcties op eerdere analyse en onopgeloste punten
9. Bestandsoverzicht

## Samenvatting (de belangrijkste antwoorden)

* **Dekking**: alle 8192 flash-bytes van beide chips zijn verklaard (0 onverklaard). M1.1: 2932 B code, 12 B dode code, 166 B telegramtabellen, 5082 B vector/vrij. T1.1: 3130 B code, 116 B dode code, 4946 B vector/vrij. Round-trip met eigen encoder **en** met GNU `avr-as`: byte voor byte gelijk. `avrdis.py` geeft op de firmware zelf geen enkele afwijking.
* **666-baud TX**: ID1, ID2, ID3, ID0 uit voorgerekende 10-bit-gekaderde tabellen (CRC 0), een telegram per 247 ms (ID1: 307 ms), gap 127,5 ms, luister-voor-zenden. De firmware berekent nergens een CRC (noch bij zenden, noch bij ontvangen). Stand op de lijn = `[0x6E] >> 2` (0..10) = index in tabel B.
* **Weestelegrammen 0x0BAA/0x0BB4**: onbereikbaar, bewezen statisch (alle Z-schrijvers en beide LPM-plaatsen) en door uitputtende emulatie van de enige tabel-lezende routine (1.048.576 combinaties; laagste gelezen adres 0x0BBE).
* **"B2 02"**: een constante in flash (ID3), onafhankelijk van temperatuur, DIP-switch, ketelype of toestand. Alle bytes van ID0/ID1/ID3 zijn onder alle bereikbare toestanden vast; alleen modus, stand en CRC van ID2 variëren.
* **RX**: M1.1 gebruikt van de 0x91-antwoorden alleen het ID (ID1: foutcode idx 6 en "levensteken"; ID2: aanvoer/retour/buiten) en controleert kop, lengte en CRC niet. Defrost, toerental, vermogen en dergelijke worden niet gebruikt.
* **5-graden-regel**: hard (< 5 C of negatief, hele graden), niet instelbaar; blokkeert een *koude start* binnen seconden, laat een draaiende pomp pas stoppen als de credit-teller `[0x65]` leeg is (ca. 2 uur). Gedetailleerde condities in sectie 6.5.
* **DIP-switch 2 (T1.1 PC2)**: ON = OpenTherm-ketel (T1.1 is brug en master naar de ketel), OFF = aan/uit-ketel (geen OT naar de ketel, T1.1 speelt zelf slave, K3 volgt het setpoint in boiler-modus). Er wordt bij reset niets ingelezen; "pompniveau" bestaat in de firmware niet.
* **Emulatie**: beide firmwares draaien in een eigen emulator waarvan de kern per instructie met de gdb-AVR-simulator is vergeleken (480.000 + 24.000 stappen, 0 afwijkingen); de 666-baud-bitstroom en de OpenTherm-uitvoer zijn gemeten en komen overeen met de analyse.
* **Open**: het frame-einde van de 0x91-antwoorden (de code eist een lange laagperiode; of de echte unit die levert is niet te zien in de dump), klok/fuses, het doel van PB3 en de betekenis van B2 02 en van de foutcodes. Volledige lijst in sectie 8.

## 0. Werkwijze, bewijsniveaus en gereedschap

**Uitgangspunt is uitsluitend de flash-dump** (`Haddon M1.1 - ATMEGA8@DIP28.BIN`, `Haddon T1.1 - ATMEGA8@DIP28.BIN`).
Beide bestanden zijn 8704 bytes: 8192 bytes flash (0x0000-0x1FFF) plus 512 bytes EEPROM (0x2000-0x21FF). De EEPROM-helft is in
beide dumps volledig 0xFF. De eerdere reconstructies (`*_reversed.ino`, `index.html`, `avrdis.py`) zijn als hypothese behandeld en
nergens blind overgenomen.

Het woord **verified** wordt alleen gebruikt als een bewering met twee onafhankelijke methoden is bevestigd. Zonder die tweede
methode staat er **hypothese** (of "afgeleid uit code" als het een directe leesbare consequentie van een instructie is die niet
apart is nagespeeld).

Gebruikte gereedschappen en onafhankelijke controles:

| Gereedschap | Rol |
|---|---|
| `avrlib.py` (eigen decoder + encoder) | tweede, onafhankelijke decoder naast `avrdis.py`; encoder voor de round-trip |
| GNU binutils 2.26 (`avr-objdump`, `avr-as`, `avr-ld`, `avr-objcopy`, uit de Arduino-installatie) | referentie-disassembler en -assembler |
| `avr-gdb` met ingebouwde AVR-simulator | onafhankelijke simulator om de kern van de eigen emulator mee te vergelijken |
| `emu.py` (eigen cyclus-nauwkeurige ATmega8-emulator) | de firmware echt laten draaien met timers, UART, poorten, comparator |
| `harness.py`, `ot.py`, `uartdec.py` | stimuli (T1.1-frames, buitenunit-antwoorden, OpenTherm-Manchester) en decoders |

Aannames die niet uit de flash volgen en die ik expliciet noem waar ze meetellen: **klokfrequentie 8 MHz** (volgt niet uit de dump;
wel uit de verhouding van de timerwaarden tot 666,7 baud en uit de eerdere vondst van een 8 MHz-kristal onder de IC2-voet, zie
`index.html`) en **de fysieke polariteit/niveau's op de pinnen** (de dump zegt alleen wat de MCU doet bij een gegeven pinniveau).

## 1. Dekking: is elke flash-byte verklaard?

**Antwoord: ja, voor beide chips, voor alle 8192 flash-bytes. 0 bytes zijn "onverklaard".** Wat er staat is bewezen met een
round-trip (disassembly -> opnieuw samenstellen -> byte voor byte gelijk aan de .BIN) langs twee onafhankelijke wegen.
De 512 EEPROM-bytes zijn in beide dumps 0xFF (niet geprogrammeerd of niet uitgelezen; zie sectie 2.6).

### 1.1 Dekkingstabel (bytes per categorie)

| Categorie | M1.1 | T1.1 | Betekenis |
|---|---:|---:|---|
| vector (RJMP) | 8 | 8 | 4 gebruikte vectoren: RESET, INT0 (lege stub), TIMER1_OVF, USART_RXC |
| vector-leeg (0xFFFF) | 30 | 30 | 15 ongebruikte vectoren, gewist (erased) |
| code, bereikbaar | 2932 | 3130 | 1470 resp. 1569 instructies, bereikbaar vanuit reset of een interruptvector |
| code, onbereikbaar ("dood") | 12 | 116 | geldige instructies waar geen enkel pad heen leidt (zie 1.4) |
| datatabel | 166 | 0 | alleen M1.1: de telegramtabellen 0x0baa-0x0c4f (zie sectie 3 en 4) |
| vrij (0xFF) | 5044 | 4908 | niet-geprogrammeerde flash (inclusief 4 B 0xFF tussen vectortabel en reset-code) |
| **onverklaard** | **0** | **0** | |
| **totaal** | **8192** | **8192** | |

Adresbereiken (volledig in `dekking_M11.csv` en `dekking_T11.csv`):

**M1.1**

| Bereik | Categorie |
|---|---|
| 0x0000-0x0003 | vectors RESET, INT0 |
| 0x0004-0x000F | 6 ongebruikte vectoren (0xFFFF) |
| 0x0010-0x0011 | vector TIMER1_OVF |
| 0x0012-0x0015 | 2 ongebruikte vectoren |
| 0x0016-0x0017 | vector USART_RXC |
| 0x0018-0x0029 | ongebruikte vectoren en vrij (0xFFFF) |
| 0x002A-0x0157 | code (reset, init, hoofdlus, link-RX, schakelaars) |
| 0x0158-0x0163 | **dode code**: twee onbereikbare routines (`ADCSRA=0x86; ret` en `ACSR=0x40; ret`) |
| 0x0164-0x0BA9 | code (watchdog, link-RX, 666-RX, regelaar, bewaking, TX-machine, link-TX, drie ISR's) |
| 0x0BAA-0x0BB3 | datatabel: weestelegram 1 (10 B) |
| 0x0BB4-0x0BBD | datatabel: weestelegram 2 (10 B) |
| 0x0BBE-0x0BCC | datatabel: segment A = ID1-telegram (15 B) |
| 0x0BCD | opvulbyte 0x00 (nooit gelezen, zie sectie 4) |
| 0x0BCE-0x0BD7 | datatabel: segment C = ID3-telegram (10 B) |
| 0x0BD8-0x0BE1 | datatabel: segment D = ID0-telegram (10 B) |
| 0x0BE2-0x0C4F | datatabel: segment B = 11 varianten ID2-telegram (11 x 10 B) |
| 0x0C50-0x1FFF | vrij (0xFF) |
| 0x2000-0x21FF | EEPROM-beeld, alles 0xFF |

**T1.1**

| Bereik | Categorie |
|---|---|
| 0x0000-0x0029 | vectors (zelfde indeling als M1.1) en vrij |
| 0x002A-0x064B, 0x066A-0x06D3, 0x06F2-0x0733, 0x076C-0x0CD7 | code |
| 0x064C-0x0669, 0x06D4-0x06F1, 0x0734-0x076B | **dode code** (3 blokken, 30+30+56 = 116 B, 58 instructies) |
| 0x0CD8-0x1FFF | vrij (0xFF) |
| 0x2000-0x21FF | EEPROM-beeld, alles 0xFF |

### 1.2 Round-trip (bewijs dat de listing de binary exact beschrijft)

Gevalideerd met `coverage.py` (uitvoer: `roundtrip_M11.txt`, `roundtrip_T11.txt`):

| Methode | M1.1 | T1.1 |
|---|---|---|
| A. eigen encoder: elke gedecodeerde instructie terug naar opcode, samengevoegd met de datatabellen en 0xFF-gebieden, vergeleken met de .BIN | 0 verschillen | 0 verschillen |
| B. de listing als tekst door `avr-as -mmcu=atmega8` + `avr-ld` + `avr-objcopy` (GNU, onafhankelijk), vergeleken met de .BIN | 8192/8192 bytes gelijk | 8192/8192 bytes gelijk |
| C. lineaire sweep door `avr-objdump` vs. eigen decode, per instructie | 1476 instr., 0 afwijkingen | 1627 instr., 0 afwijkingen |

Opmerking over B: de instructies zijn als mnemonic-tekst opnieuw geassembleerd; de datatabellen en 0xFF-gebieden zijn als `.word`
meegegeven (die bevestigen dus niets over zichzelf; hun inhoud is apart gedecodeerd, zie sectie 3 en `tables.py`). De code is
er volledig door gedekt.

De eigen decoder is bovendien over alle 65536 mogelijke opcodes vergeleken met `avr-objdump` (`verify_decoder.py`,
uitvoer `verify_decoder.txt`): 0 verschillen op alle ATmega8-instructies. De 148 gerapporteerde "verschillen" zijn uitsluitend
XMEGA-instructies (XCH, LAS, LAC, LAT) en EIJMP/EICALL/ELPM/SPM Z+, die de ATmega8 niet heeft; encode(decode(w)) == w voor alle opcodes.

### 1.3 Controle van `avrdis.py`

Vergelijking in `compare_avrdis.py` (uitvoer `vergelijk_avrdis.txt`): `avrdis.py` is zelf gedraaid op beide dumps.

* **Op de firmware zelf: geen enkele afwijking.** Dezelfde 1470 (M1.1) en 1569 (T1.1) instructie-adressen als de eigen trace, en
  voor elk adres dezelfde mnemonic, operanden en lengte (RJMP/RCALL/BRxx-doelen, SBRS/SBRC, SBIC/SBIS, LD/ST-varianten, LPM, MUL, MOVW,
  ADIW/SBIW). De skip-instructie-fix (instructie na SBRx/SBIx/CPSE) en de SBRS/SBRC-fix (bit 9 i.p.v. bit 3) zijn aantoonbaar correct.
* **Op de volledige opcode-ruimte zijn er wel fouten**, allemaal zonder gevolg voor deze twee firmwares (geen van die opcodes komt erin voor):
  1. `MOV Rd,Rd` (0x2C00-groep) wordt als `TST` weergegeven (TST is in werkelijkheid `AND Rd,Rd`, 0x2000-groep, en wordt daar als `AND` getoond).
  2. BLD/BST/SBRC/SBRS met gezette bit 3 (ongedefinieerde opcodes, 1024 stuks) worden als gewone instructie gelezen.
  3. JMP/CALL (niet op de ATmega8) worden fout gedecodeerd; 0x95F8 (SPM Z+) niet herkend.
  4. IO-adressen: namen uit een andere tabel (`TWBR(0x00)`) en `MOVW/ADIW/SBIW`-operandvorm wijken cosmetisch af van binutils.
* Conclusie: de bestaande `.lst`-bestanden zijn voor deze twee dumps betrouwbaar. De adressen in dit rapport komen uit de eigen trace
  (`listing_M11.txt`, `listing_T11.txt`), niet uit `avrdis.py`.

### 1.4 Code die nergens heen leidt (dode code)

Beide firmwares delen bibliotheekcode (initialisatie, `uart_putc`, `wdr`) en dragen restanten van elkaar. In geen van beide staan
IJMP/ICALL/JMP/CALL (`lpm_M11.txt`, `lpm_T11.txt`, `z_patterns.txt`), dus de controlestroom is volledig statisch en de dode blokken zijn
aantoonbaar onbereikbaar:

* **M1.1 0x0158-0x0163**: `LDI r16,0x86 / OUT ADCSRA / RET` en `LDI r16,0x40 / OUT ACSR / RET`. ADC-init en comparator-init uit de
  gedeelde bibliotheek; M1.1 gebruikt de ADC en de comparator niet.
* **T1.1 0x064C-0x0669 en 0x06D4-0x06F1** (twee identieke 30-bytes kopieen): `X=0x89; ld; schakel bit 7 van het eerste OT-byte; ...; bit 0 van het
  vierde byte zetten; rjmp`. Compiler-restant van het "toggle bericht-type"-patroon dat de levende takken op 0x06F8, 0x0772 enz. wel gebruiken.
* **T1.1 0x0734-0x076B**: een Read-ACK-opbouw die RAM `[0x75]` test (bij 0: bericht `0xC0 .. 00 00`, anders `0x40 + pariteitsbit via 0x0870` met `[0x75]` als data) en
  die door de voorafgaande `rjmp 0x076C` wordt overgeslagen; geen enkele sprong wijst erheen.

Een geldig stuk code dat nergens vandaan bereikt wordt is dus geen verborgen functie maar een restant; hier is ze ter volledigheid gedecodeerd
(`listing_*.txt`, regels met `[DOOD: onbereikbaar]`).

## 2. Functies, interrupts, pinnen, RAM, EEPROM en fuses

Alle adressen zijn byte-adressen (zoals in de listing). Volledige, automatisch gegenereerde tabellen met aanroepers, aangeroepen
functies en gebruikte IO-registers: `functies_M11.md`, `functies_T11.md`, `aanroepgraaf_*.md`, `io_*.csv`, `ram_*.csv`
(RAM-kruisverwijzing: welke functie leest/schrijft welk adres). Namen en doel zijn mijn eigen interpretatie van de code; waar een
gedrag in de emulator is nagespeeld staat dat in sectie 7.

Beide chips zijn dezelfde "familie" code (gegenereerd door een compiler: elke RAM-variabele wordt via `LDI r30/r31 + LD/ST Z` benaderd,
registers r20-r25 zijn globale variabelen, de ISR's slaan r16-r21 en r28-r31 op maar niet r20-r25).

### 2.1 M1.1 (IC1, warmtepomp-link) - functielijst

Hoofdlus (`0x003C-0x0050`): `wdr_kick -> rx666_parser -> link_rx_frame -> sw_cv_only_PC0 -> btn_test_PC1 -> stand_regelaar -> bewaking_65_lockout -> seconden_watchdog_66 -> tx666_machine -> link_tx_to_T11 -> terug`. Geen SLEEP, geen IJMP/ICALL.

| Adres | Naam | Doel | Aanroeper | Parameters / geraakte registers, RAM en IO |
|---|---|---|---|---|
| 0x002A | reset_main | SP=0x045F, init-routines, hoofdlus | RESET-vector | SPL/SPH |
| 0x0052 | init_ram | RAM en globale registers initialiseren: nul, behalve `[0x65]=0xB4`, `[0x73]=[0x77]=[0x79]=0x14`; wist 0x7B-0xB0 | reset_main | r16-r18, r22-r28, RAM 0x60-0xB0 |
| 0x0118 | init_timer1 | TCCR1B=1 (klok/1), TIMSK=0x04 (TOIE1), `sei` | reset_main | TCCR1B, TIMSK, I-vlag |
| 0x0124 | init_ports | SPI uit (SPCR bit 6,4=0), DDRB=0x1F, PORTB=0x0A, DDRC=0x08, PORTC=0xFF, DDRD=0x02, PORTD=0xFD | reset_main | DDRx/PORTx |
| 0x0142 | init_uart9600 | UBRR=103, U2X=1, UCSRB=0x98 (RXCIE, RXEN, TXEN), UCSRC=0x8E (8N2); 8 MHz geeft 9615 baud | reset_main | UBRRL, UCSRA/B/C |
| 0x0164 | init_wdt | watchdog aan, WDP=7 (ca. 2,1 s) | reset_main | WDTCR |
| 0x0172 | wdr_kick | `wdr` | hoofdlus | - |
| 0x0176 | link_rx_frame | verwerkt de via de UART ontvangen bytes van T1.1 tot een 12-byte frame (zie 3.3) en bouwt het antwoordframe in 0xA4-0xAF | hoofdlus | r21,r22,r23(bits 2-5),RAM 0xA3-0xAF, 0x6E,0x6F,0x67,0x74,0x77-0x7A, PB2/PB4 laag |
| 0x02AE | sw_cv_only_PC0 | als PC0 hoog en `[0x6D].0=0`: `[0x65]=0x1E`, PB0 uit, `[0x6E]=0`, r23.1 wissen | hoofdlus + staartsprong uit 0x0176 | PINC.0, PORTB.0, `[0x65]`,`[0x6E]` |
| 0x02D2 | btn_test_PC1 | als PC1 laag, `[0x6F] >= 20` en `[0x6D].0=0`: deblokkeren (r23.0=0), PB0 aan, `[0x6E]=0x0E`, `[0x64]=1`, `[0x65]=0xB4` | hoofdlus | PINC.1 |
| 0x0308 | rx666_parser | software-UART-ontvanger op PC4 + ontleden van ID1/ID2-antwoorden (zie 3.2) | hoofdlus | PINC.4, r17-r20,r26/r27, RAM 0x68,0x69,0x6B,0x6D,0x71-0x74,0x7B-0x84 |
| 0x04BE | stand_regelaar | vraagrelais starten/stoppen en de "stand" `[0x6E]` regelen op de gevraagde aanvoer `[0x6F]` (zie 3.4/6) | hoofdlus | PINB.0, PORTB.0, `[0x64]`,`[0x65]`,`[0x6E]`,`[0x70]`, r23, r24.0 |
| 0x062E | bewaking_65_lockout | credit-teller `[0x65]` bewaken, 5-graden-regel, F0-melding aan T1.1 (zie 6.5) | hoofdlus | `[0x65]`,`[0x73]`,`[0x6F]`, r23.0/.7, r24.1 |
| 0x072E | seconden_watchdog_66 | `[0x66]`/`[0x67]`: 25-s-resync van de TX-machine, 30-s-communicatiestoring, 240-s-T1.1-weg (zie 3.4) | hoofdlus | `[0x66]`,`[0x67]`,`[0x6D]`,`[0x6C]`,`[0x74]`,PB0,PB4, LPM op 0x0752 |
| 0x083A | tx666_machine | zendt de tabeltelegrammen bit voor bit op PC3 (en PB3), gap-/luister-logica (zie 3.1) | hoofdlus | PORTC.3, PORTB.3, PINC.4, r17-r20,r25, RAM 0x68,0x69,0x6A,0x6C,0x6D,0x6E,0x7B-0x84; LPM op 0x0A22 |
| 0x0A40 | link_tx_to_T11 | als r23.6: r23.5 wissen, `cli`, 12 bytes 0xA4-0xAF via `uart_putc`, r23.6 wissen, `sei` | hoofdlus | UDR, UCSRA.TXC |
| 0x0A62 | uart_putc | byte naar UDR, wacht op TXC (UCSRA bit 6), wis TXC | link_tx_to_T11 | UDR, UCSRA |
| 0x0A6C | ISR TIMER1_OVF | 250-us-tik en alle tijdbases (zie 2.3) | vector 8 | TCNT1, r16-r21,r28-r31 opgeslagen |
| 0x0B66 | ISR INT0 | lege stub (push/pop/reti); INT0 is nooit ingeschakeld (GICR wordt niet geschreven) | vector 1 | - |
| 0x0B98 | ISR USART_RXC | `r22 = UDR; r23.2 = 1` | vector 11 | UDR, r22, r23 |
| 0x0158, 0x015E | (dood) | ADC- en comparator-init uit de bibliotheek | niemand | - |

### 2.2 T1.1 (IC2, OpenTherm-brug) - functielijst

Hoofdlus (`0x0040-0x0054`): `wdr_kick -> link_rx_frame -> ot_rx_thermostaat_ACO -> ot_rx_ketel_PD5 -> ot_verwerk -> ot_tx_ketel_PD3 -> ot_tx_thermostaat_PD4 -> link_timeout_noodbrug -> led_knippercode -> link_tx_naar_M11 -> terug`.
Voor de hoofdlus draait eenmalig `bouw_link_frame` (0x003C) als initialisatie van de TX-buffer.

| Adres | Naam | Doel | Parameters / geraakte registers, RAM en IO |
|---|---|---|---|
| 0x002A | reset_main | SP=0x045F, init, hoofdlus | |
| 0x0056 | init_ram | RAM en registers wissen, `[0x71]=[0x72]=0x14`, wist 0x7B-0x90 | r16-r20,r22,r24,r25 |
| 0x0120 | init_timer1 | TCCR1B=1, TIMSK=4, `sei` | |
| 0x012C | init_ports | DDRB=0x1F, PORTB=0; DDRC=0x30, PORTC=0xCF; DDRD=0x1E, PORTD=0x19 | |
| 0x0146 | init_uart9600 | gelijk aan M1.1 | |
| 0x015C | init_comparator | ACSR=0x40 (ACBG=1: bandgap op AIN0, comparator aan, geen interrupt) | ACSR |
| 0x0162 | init_wdt | watchdog ca. 2,1 s | WDTCR |
| 0x0174 | link_rx_frame | 12-byte frame van M1.1 verzamelen (0x7B = teller, bytes op 0x7C-0x87), som controleren, velden verdelen en `r22`-vlaggen zetten (zie 3.3) | r19,r21,r22,r25, RAM 0x6C,0x6D,0x71-0x73, PB2 laag |
| 0x0232 | bouw_link_frame | TX-frame op 0x7C-0x87 opbouwen uit `[0x6E],[0x77]-[0x7A]` + de 6 laatst ontvangen bytes + som; zet r25.5 | RAM 0x7C-0x87 |
| 0x0286 | ot_rx_thermostaat_ACO | Manchester-ontvanger voor de thermostaat (comparator-uitgang ACO, ACSR bit 5) -> 4 bytes op 0x89-0x8C; zet r24.0 als frame klaar | ACSR, TCNT1, r23, r24 |
| 0x0398 | ot_rx_ketel_PD5 | idem voor de ketel op PD5 -> 4 bytes op 0x8D-0x90; zet r24.4 | PIND.5, TCNT1, r23, r24 |
| 0x0482 | ot_verwerk | de eigenlijke brug: kopieert/herschrijft OT-berichten per Data-ID tussen de buffers (zie 3.5) | PINC.2, RAM 0x6D-0x72,0x77,0x78,0x89-0x90, r22 |
| 0x0870/0x0872 | ot_parity8 / tail | pariteitsbit (even) over de 4 bytes, resultaat als bit 7 in r18 -> bij byte 0 opgeteld | r16,r18,r19 |
| 0x0892 | ot_tx_ketel_PD3 | OT-master-frame naar de ketel op PD3 (of bij PC2 hoog: K3 op PC5 sturen) | PORTD.3, PINC.2, PORTC.5, TCNT1 |
| 0x0958 | ot_tx_thermostaat_PD4 | OT-slave-frame naar de thermostaat op PD4; na afloop eventueel het linkframe verversen | PORTD.4, TCNT1 |
| 0x0A24 | link_timeout_noodbrug | bij `[0x6C] >= 0xF0`: link-fout, bouwt een noodframe, zet PB2 (K1+K2 noodbrug) als PC2 laag, zet PC4 (LED) | PINC.2, PORTB.2, PORTC.4, PINB.2 |
| 0x0AA0 | led_knippercode | storingscode `[0x73]` -> knipperpatroon op PC4 (zie 3.6) | PORTC.4, RAM 0x73-0x75,0x62 |
| 0x0B7E | link_tx_naar_M11 | als r25.6: r25.5 wissen, `cli`, 12 bytes 0x7C-0x87 versturen, `sei` | UDR |
| 0x0BA0 | uart_putc | zoals M1.1 | |
| 0x0BAA | ISR TIMER1_OVF | 125-us-tik en tijdbases (zie 2.3) | TCNT1=0xFC3A |
| 0x0C98 | ISR INT0 | lege stub | |
| 0x0CC6 | ISR USART_RXC | `r20 = UDR; r25.2 = 1` | |

### 2.3 Interrupt-routines en tijdbases (verified waar gemeten in de emulator)

Geen enkele ISR gebruikt iets anders dan Timer1-overflow en USART-RX. **Niet gebruikt in beide chips**: Timer0, Timer2, ADC, SPI, TWI, EEPROM, INT0/INT1 (GICR/MCUCR worden nooit geschreven), analoge-comparator-interrupt, SLEEP, UDRE/TXC-interrupts.

| | M1.1 | T1.1 |
|---|---|---|
| Timer1-herlaadwaarde | 0xF84F = 1969 cycli | 0xFC3A = 966 cycli |
| gemeten ISR-periode (emulator, 8 MHz) | 2002,5 cycli = 250,3 us (min 2002, max 2005) | 997,5 cycli = 124,7 us (min 997, max 1000) |
| tik | 250 us | 125 us |
| `[0x60]` | 256 tikken (64 ms): bij 0 -> r23.4 | 256 tikken (32 ms): bij 0 -> r25.4 |
| `[0x61]`/`[0x62]` | 100 x 40 tikken = 1,0012 s ("seconde") | 200 x 40 tikken = 0,998 s |
| seconde-teller(s) | `[0x64]` (regelaar-klok), `[0x66]` (s sinds ID1-antwoord), `[0x67]` (s sinds T1.1-frame) | `[0x64]`, `[0x6C]` (s sinds M1.1-frame) |
| minuut | `[0x63]` -> 60 s: `[0x65]` -= 1 (bij 0: 0xB4), r24.1 = 1 | `[0x65]` 0..59 |
| bit-timers | `[0x68]`, `[0x69]` worden elke tik verlaagd (666-baud-klok, gap-timer) | `[0x6A]` (tikken sinds flank), `[0x68]` (vrije tikteller), `[0x67]` (stille tikken tijdens RX) |

Bij 8 MHz is de bit-tijd van de 666-baud-lijn gemeten op gemiddeld 12005-12010 cycli (6 tikken van 2002,5 = 12015 nominaal, minus
de granulariteit van de hoofdlus) = 666,3 baud, 0,05 % onder 666,67. De ISR-latentie vanaf overflow tot de herlading is ca. 33,5 cycli (1969 + 33,5 = 2002,5):
**dit bevestigt dat het tabellenwerk "6 ticks van 250 us = 1,5 ms" klopt bij 8 MHz** (zie aanname bovenaan).

### 2.4 Pinnen en hun functie (DIP28, uit de code)

Fysieke interpretatie (relais, LED, optocoupler) staat alleen waar `index.html` het met de meter heeft vastgesteld; dat is gemarkeerd als *(gemeten, index.html)*.

**M1.1 (IC1)** - DDRB=0x1F, PORTB=0x0A, DDRC=0x08, PORTC=0xFF, DDRD=0x02, PORTD=0xFD

| Pin (DIP28) | Poort | Richting | Functie volgens de code |
|---|---|---|---|
| 14 | PB0 | uit | **vraagrelais / "warmtepomp-vraag"** (start/stop in `stand_regelaar`; wordt ook teruggelezen via PINB.0 als toestand). Gemeten: via R21 10 k naar een LED *(gemeten, index.html)* |
| 15 | PB1 | uit | storingsvlag: hoog zolang `[0x74]` (foutcode) <> 0; start hoog (PORTB=0x0A) tot het eerste ID1-antwoord |
| 16 | PB2 | uit | wordt nergens hoog gezet (`cbi PORTB,2` na een geldig T1.1-frame is een no-op): constant laag. Dood restant |
| 17 | PB3 | uit | **echo-bitstroom**: dezelfde bitklok als PC3, maar met de laatst ontvangen buitenunit-bytes (`[0x7B..]`) i.p.v. de tabel (zie 3.1). Functie onbekend |
| 18 | PB4 | uit | "T1.1 stil"-vlag: hoog zodra T1.1 240 s zwijgt (gezet op 0x0836 bij het bouwen van het noodframe); laag bij elk geldig T1.1-frame (0x021A) en zolang `[0x67] < 30` (0x07A4) |
| 23 | PC0 | in (pull-up) | CV-only-schakelaar: **hoog = ketel alleen**, laag = warmtepomp vrijgegeven |
| 24 | PC1 | in (pull-up) | testknop: laag = starten (mits gevraagde aanvoer >= 20) |
| 25, 28 | PC2, PC5 | in (pull-up) | ongebruikt |
| 26 | PC3 | uit | **TX 666 baud** naar de buitenunit (rust hoog, startbit laag) |
| 27 | PC4 | in (pull-up) | **RX 666 baud** van de buitenunit |
| 2 / 3 | PD0 / PD1 | in / uit | RXD / TXD hardware-UART naar T1.1, 9615 baud 8N2 |
| 4,5,6,11,12,13 | PD2-PD7 | in (pull-up) | ongebruikt |
| 19 | PB5 | in (pull-up uit) | ongebruikt |

**T1.1 (IC2)** - DDRB=0x1F, PORTB=0x00, DDRC=0x30, PORTC=0xCF, DDRD=0x1E, PORTD=0x19

| Pin | Poort | Richting | Functie volgens de code |
|---|---|---|---|
| 14,15,17,18 | PB0,PB1,PB3,PB4 | uit | nooit gebruikt, blijven laag (blind einde, *gemeten*: niet aangesloten) |
| 16 | PB2 | uit | **noodbrug K1+K2**: aan als `[0x6C] >= 0xF0` (240 s geen geldig M1.1-frame) en PC2 laag; uit bij elk geldig M1.1-frame. Wordt teruggelezen (PINB.2) |
| 25 | PC2 | in (pull-up) | **DIP-switch 2 = ketelype** (zie sectie 6) |
| 23, 24, 26 | PC0, PC1, PC3 | in (pull-up) | ongebruikt |
| 27 | PC4 | uit | **status-LED** (knippercode van `[0x73]`; aan bij link-fout) |
| 28 | PC5 | uit | **K3 aan/uit-ketelcontact**: alleen gestuurd als PC2 hoog **en** `r22.0` (boiler-modus) |
| 2 / 3 | PD0 / PD1 | in / uit | RXD / TXD naar M1.1 |
| 5 | PD3 | uit | OpenTherm-TX naar de **ketel** (rust hoog; bit 1 = eerst laag dan hoog) |
| 6 | PD4 | uit | OpenTherm-TX naar de **thermostaat** (idem, rust hoog) |
| 11 | PD5 | in | OpenTherm-RX van de **ketel**, rust **laag**, bit 1 = eerst hoog dan laag (geinverteerd t.o.v. TX) |
| 13 | PD7 / AIN1 | in (analoog) | OpenTherm-RX van de **thermostaat** via de comparator tegen de bandgap (ACO, rust 1, bit 1 = eerst laag) |
| 4, 12 | PD2, PD6 | in | ongebruikt |

### 2.5 RAM-variabelen (SRAM 0x60-0xB0) en globale registers

**M1.1**

| Adres / reg | Betekenis | Geschreven door |
|---|---|---|
| r17,r18 | tijdelijk: kopie `[0x6D]` en `[0x6C]` in de TX-machine | |
| r20 | TX-schuifbyte voor PC3 (MSB eerst) / RX-schuifbyte | tx666_machine, rx666_parser |
| r22 | laatst ontvangen UART-byte van T1.1 | ISR_RXC |
| r23 | vlaggen: bit0 = start geblokkeerd; bit1 = regelaar in stapmodus; bit2 = UART-byte ontvangen; bit4 = "stilte voorbij" (64 ms na vorig frame); bit5 = TX-frame naar T1.1 klaar; bit6 = nu versturen; bit7 = buitentemperatuur-koudvlag | zie sectie 6 |
| r24 | bit0 = hoge setpoint ( >= 55 ) vlag; bit1 = minuuttik | ISR, regelaar |
| r25 | schuifbyte voor PB3-echo | tx666_machine |
| r26:r27 (X) | RX-buffer-pointer (0x7B..0x85) | rx666_parser |
| 0x60 | tikteller 256 (-> r23.4) | ISR |
| 0x61 / 0x62 / 0x63 | 25-ms / seconde / minuut | ISR |
| 0x64 | seconden sinds laatste regelstap/start | ISR, regelaar |
| 0x65 | **credit-/gezondheidsteller** (start 0xB4=180; -1 per minuut; 0 -> 0xB4; <60 = stoppen) | ISR, bewaking, regelaar, schakelaars |
| 0x66 | seconden sinds laatste ID1-antwoord | ISR; 0 bij ID1 |
| 0x67 | seconden sinds laatste geldig T1.1-frame | ISR; 0 bij frame |
| 0x68 | bit-timer 666 baud (6 tikken per bit) | ISR dec; TX/RX zetten |
| 0x69 | gap-/stiltetimer (0xFF = net activiteit) | ISR dec; TX/RX zetten |
| 0x6A | resterende bits in huidig TX-byte (8..0) | tx666_machine |
| 0x6B | resterende bits in huidig RX-byte | rx666_parser |
| 0x6C | positie in de 45-byte-tabelstroom (45..1) | tx666_machine |
| 0x6D | TX/RX-toestand: bit0 = TX actief, bit1 = gap/luisterfase, bit2 = antwoord wordt ontvangen, bit7 = tweede gap-fase | tx666/rx666 |
| 0x6E | **stand** (intern 0..40; op de lijn gaat `[0x6E] >> 2` = 0..10) | regelaar, bewaking, link, schakelaars |
| 0x6F | gevraagde aanvoertemperatuur (C) uit T1.1 byte 0 | link_rx_frame, 0x07B6 |
| 0x70 | doelwaarde na begrenzing (34..55) | regelaar |
| 0x71 | aanvoertemperatuur buitenunit (C, ID2 byte 3-4 / 10) | rx666_parser |
| 0x72 | retourtemperatuur (ID2 byte 5-6 / 10) | rx666_parser |
| 0x73 | buitentemperatuur (C, ondertekend 8-bit; start 20; ID2 byte 7-8 / 10) | rx666_parser |
| 0x74 | foutcode (ID1 byte 6; 1 = geen ID1-antwoord 30 s; 100 = T1.1 weg) | rx666_parser, watchdog |
| 0x75 | nooit gelezen (alleen init) | |
| 0x77, 0x78 | kamer-setpoint hele graden / fractie uit T1.1 byte 1-2 (start 0x14, 0) | link_rx_frame |
| 0x79, 0x7A | T1.1 byte 3-4 (start 0x14, 0), worden wel doorgestuurd maar nergens gebruikt | link_rx_frame |
| 0x7B-0x84 | RX-buffer buitenunit (10 bytes: 0x7B = kopbyte 0x91, 0x7C = ID, 0x7D = lengte, 0x7E-0x84 = payload) | rx666_parser |
| 0x85-0xA2 | ongebruikt (gewist) | |
| 0xA3 | UART-RX-bytepositie (0..12) | link_rx_frame |
| 0xA4-0xAF | linkframe-buffer: RX **en** TX (12 bytes, laatste = som) | link_rx_frame, bewaking, watchdog |

**T1.1**

| Adres / reg | Betekenis |
|---|---|
| r20 | laatst ontvangen UART-byte (ISR) |
| r22 | bit0 = **boiler-modus / noodmodus** (OT-berichten ongewijzigd doorgeven); bit1 = warmtepomp actief (link ok en stand>0); bit2 = OT-frame voor de ketel wacht op verzending; bit3 = OT-frame voor de thermostaat wacht; bit4,5,6 = TX-tussenstanden |
| r23 | OT-bitschuifregister / TX-byte |
| r24 | OT-RX-toestand thermostaatzijde (bit1 actief, bit2/3 niveau-latch, bit0 frame klaar) en ketelzijde (bit4 klaar, bit5 actief, bit6/7) |
| r25 | bit2 = UART-byte ontvangen; bit4 = 32-ms-tik; bit5 = linkframe klaar; bit6 = linkframe versturen |
| 0x60-0x62, 0x64, 0x65 | tikken, seconde, seconden, minuut |
| 0x67 | stille tikken tijdens OT-RX; 0x68 vrije tikteller; 0x69 bitteller; 0x6A tikken sinds flank / bitfase |
| 0x6C | seconden sinds laatste geldig M1.1-frame (>= 0xF0: noodfase) |
| 0x6D | stand van M1.1 (byte 6 van het M1.1-frame) |
| 0x6E, 0x6F | ketelsetpoint uit OT ID1: hele graden, fractie |
| 0x71, 0x72 | aanvoer- en retourtemp van de warmtepomp (M1.1 byte 7, 8) -> OT ID25/ID28; start 20 |
| 0x73 | storingscode (M1.1 byte 10; 100 = link-fout) |
| 0x74, 0x75 | LED-toestand en -timer |
| 0x77, 0x78 | kamersetpoint (ID16): hele graden, fractie |
| 0x79, 0x7A | nul, nooit gezet |
| 0x7B | RX-teller UART; 0x7C-0x87: linkframe-buffer (RX en TX gedeeld) |
| 0x89-0x8C | OT-buffer thermostaatzijde (4 bytes: type/pariteit, Data-ID, data hoog, data laag); 0x8D-0x90: idem ketelzijde |

### 2.6 Wat er niet is (belangrijk voor "geen steen onomgedraaid")

* **EEPROM**: geen enkele lees- of schrijftoegang (EECR, EEARL/H en EEDR komen in `io_*.csv` niet voor). Beide dumps hebben 512 B EEPROM, alles 0xFF.
* **Fuses en lockbits** zijn niet in de dump opgenomen. Af te leiden: externe 8 MHz-bron (zonder die is "1,5 ms/bit" niet te halen), reset-vector op 0x0000 (geen
  bootloader, BOOTRST niet geprogrammeerd), software schakelt de watchdog in (WDTON niet nodig), de flash is leesbaar uitgelezen (lock niet gezet, of omzeild).
  Een betrouwbare fuse-waarde is dus **niet** uit de dump te halen (zie sectie 8). De nabouw gebruikt FF/D9/FD voor de ATmega328P (`blogpost-2`), dat is voor die chip.
* Geen tabellen of strings in T1.1 (geen LPM). In M1.1 alleen de telegramtabellen.

## 3. Telegrammen en protocol

### 3.1 De 666-baud TX-keten (M1.1, PC3) - volledig

**De tabellen.** `0x0BAA-0x0C4F` bevat 14 + 2 telegramtabellen in 10-bit-framing: per byte een startbit 0, 8 databits LSB eerst, een stopbit 1;
de frames staan aaneengeschakeld en MSB-eerst in bytes gepakt (`tables.py`, uitvoer in `tabellen_M11.md`). Het allereerste startbit van een telegram staat
**niet** in de tabel maar wordt door de code zelf gezet (`0x0886`: PC3 en PB3 laag). Alle 16 slots decoderen met correcte framing en **CRC-16/CCITT-FALSE = 0**:

| Slot | Adres | Bytes | Telegram (bytes -> CRC hi, lo) |
|---|---|---|---|
| (wees 1) | 0x0BAA | 10 | `19 02 08 00 FF 00 9E C9` |
| (wees 2) | 0x0BB4 | 10 | `19 03 08 00 00 00 37 67` |
| A = ID1 | 0x0BBE | 15 | `19 01 0C 00 00 00 00 00 00 00 AA 35` (12 telegram-bytes; opvulbyte 0x0BCD = 0x00 daarna) |
| C = ID3 | 0x0BCE | 10 | `19 03 08 B2 02 00 C1 9A` |
| D = ID0 | 0x0BD8 | 10 | `19 00 08 00 00 00 D9 B5` |
| B[0] | 0x0BE2 | 10 | `19 02 08 00 00 00 9D 36` (modus 0, stand 0) |
| B[1..10] | 0x0BEC.. | 10 elk | `19 02 08 01 <k> 00 <crc>`: stand 1..10: `99 37, CC 64, FF 55, 66 C2, 55 F3, 00 A0, 33 91, 23 AF, 10 9E, 45 CD` |

**De index in tabel B is rechtstreeks de stand op de lijn (0..10)**: B[k] heeft `stand = k`. De code rekent `k = [0x6E] >> 2`, dus `[0x6E]` is een stand in
kwartstappen (0..40). Modusbyte: 0x00 alleen in B[0], 0x01 in B[1..10]; 0x02 (koelen) komt nergens in de flash voor (verified: alle 16 slots gedecodeerd + alle
LPM-adressen gelezen, `e4_lpm_sweep.txt`).

**De tabelstroom (`tx666_machine`, 0x083A).** De positieteller `[0x6C]` loopt van 45 naar 1 (herlaad 45 bij 0). Per byte selecteert de code (`0x090E-0x0A1C`):

| `[0x6C]` (n) | segment | flash-adres van het byte | inhoud |
|---|---|---|---|
| 45..31 | A | 0x0BBE + (45 - n) | ID1 (15 B-slot, 12 telegram-bytes) |
| 30..21 | B | 0x0BE2 + 10 * ([0x6E]>>2) + (30 - n) | ID2 (stand uit `[0x6E]`) |
| 20..11 | C | 0x0BCE + (20 - n) | ID3 |
| 10..1 | D | 0x0BD8 + (10 - n) | ID0 |

Dus de volgorde op de lijn is **ID1, ID2, ID3, ID0, ID1, ...** (gemeten in de emulator, `e1_out.txt`: start-tijden 0,3155 s ID2, 0,5628 ID3, 0,8101 ID0, 1,0574 ID1, 1,3648 ID2, ...).

**Per byte, de toestandsmachine (`[0x6D]` = r17):**

1. `[0x6A]` > 0: er zijn nog bits van het huidige byte. Is `r17.0 = 0` (nog niet gestart): luister-voor-zenden (0x0864): `[0x68]=6`; is PC4 laag dan `[0x69]=0xFF`
   (lijn bezet), anders mag zenden als `[0x69] < 0xE0` (de ontvangstlijn was >= 31 tikken = 7,8 ms stil). Dan (0x0886) PC3 en PB3 laag (**startbit**), `[0x68]=6`, `r17.0=1`.
   Is `r17.0 = 1`: zodra `[0x68]=0` (6 tikken = 1,5 ms) de volgende bit: PC3 = r20.7, PB3 = r25.7, beide een plaats naar links, `[0x6A]` verlagen.
2. `[0x6A] = 0`: volgend byte nodig. Eerst `[0x6C]` bepalen (0 -> 45), het segment kiezen, het tabelbyte met `LPM` in r20 laden, het gelijknamige RAM-byte `[0x7B + offset]` in r25, `[0x6C]` verlagen, `[0x6A]=8`.
3. **Eerste byte van elk segment terwijl `r17.0 = 1`** (dus: het vorige telegram is klaar): geen nieuw byte, maar het **einde van het telegram**: PC3 en PB3 hoog (rust), `r17.0=0`, `r17.1=1` (gap), `[0x69]=0xFF`, en `[0x6C]` wordt hersteld op zijn waarde van voor de segmentreductie.
4. **Gap** (`r17.1 = 1`, `0x089E`): wacht tot `[0x69] < 2` (254 tikken = 63,6 ms); dan `r17.7=1`, `[0x69]=0xFF` en nog eens 63,6 ms; dan `r17 &= 0x79` (gap voorbij) en volgt het eerste byte van het volgende telegram (stap 1).
   Het hele gap is dus ca. 127,5 ms, plus luister-voor-zenden (>= 0 ms, 7,8 ms als de ontvangstlijn net actief was).
   **In deze gap staat de RX-parser open** (zie 3.2): hij werkt alleen als `r17.1 = 1`.

Gemeten (emulator, 8 MHz): telegramperiode ID2/ID3/ID0 = 247,4 ms (120 ms zendtijd + 127,5 ms gap), ID1 = 307,4 ms; volledige rotatie 1049,6 ms.
Bitduur gemiddeld 12005-12010 cycli (666,3 baud). Alle gemeten zendframes zijn gedecodeerd en CRC-gecontroleerd (e1_out.txt: de eerste 10 s alle CRC OK; de fuzz-tests van sectie 7 controleren duizenden frames).

**Wat bepaalt "stand"?** `[0x6E]`, uitsluitend gezet door:
`stand_regelaar` (start `0x0E`; stapt omhoog/omlaag), `link_rx_frame` (0 bij "geen vraag", 0x0226), `bewaking_65_lockout` (0 bij blokkade), `sw_cv_only_PC0` (0),
`btn_test_PC1` (0x0E), `seconden_watchdog_66` (0 bij 30 s geen ID1-antwoord). Bereik: 0 of 4..40 (de regelaar houdt `[0x6E] >= 4` zodra hij loopt:
aftrekken alleen toegestaan vanaf `>=` 5/6/8 met 1/2/4; optellen stopt bij 0x28 = 40), dus index 0..10 en **nooit buiten tabel B**. De stap gebeurt
alleen als `r17.0 = 0` (TX niet bezig) in `stand_regelaar`, `bewaking`, `sw_cv_only` en `btn_test`; **uitzondering**: `link_rx_frame` en `seconden_watchdog_66` zetten `[0x6E]=0` zonder
die controle, zodat in theorie een ID2-telegram halverwege van variant kan wisselen (gemengde bytes, CRC fout). Dat is uit de code afgeleid, niet nagespeeld (hypothese).

**De 0x19/0x1E-teller in `seconden_watchdog_66` (0x072E)** = `[0x66]` = **seconden sinds het laatste ID1-antwoord** (verhoogd in de seconde-tak van de ISR, op 0 gezet door `rx666_parser` zodra het ontvangen ID 1 is):

| `[0x66]` | actie |
|---|---|
| 0..24 | niets |
| 25 (0x19) | `[0x66]=26`; **TX-machine herstart**: `[0x6D]=0`, r20 = eerste byte van segment A (LPM 0x0BBE = 0x98), `[0x69]=0xFF`, `[0x6A]=8`, `[0x6C]=0x2C` -> de eerstvolgende zendbeurt is weer ID1 |
| 26..29 | niets |
| >= 30 (0x1E) | communicatiestoring: `[0x6E]=0`, PB0 uit, r23.1 uit, `[0x74]=1`; als r23.5 (frame klaar) dan r23.6 (versturen) en `[0x66]=5`; zo niet blijft `[0x66]` oplopen (en wordt dit elke lusronde herhaald) |

Ook `[0x67]` (seconden sinds laatste T1.1-frame): >= 0xF0 (240 s) => `[0x67]=0xEB`, `[0x6F]=0`, PB0 uit, `[0x74]=100`, frame met 0xF0 aan T1.1; < 30 s => PB4 laag.
Alle waarden zijn getabelleerd met functie-tests (`e7_m11_funcs.txt`, sectie E en F).

**PB3 (echo-lijn).** PB3 krijgt dezelfde bitklok als PC3, maar r25 wordt bij elk tabelbyte uit RAM `[0x7B + offset]` gevuld: de laatst ontvangen buitenunit-bytes, vier keer per
rotatie herhaald (bij segment A zelfs met bytes voorbij de 10-byte-buffer: 0x85-0x89 = nul). PB3 is dus geen zendlijn naar de buitenunit maar een
ruwe echo van de RX-buffer, gesynchroniseerd op de TX-klok. Functie niet af te leiden uit de dump (zie sectie 8).

**Boot-eigenaardigheid (gemeten).** `TCNT1` staat na reset op 0; de eerste Timer1-overflow komt dus pas na 65536 cycli (8,2 ms) en de ISR herlaadt dan pas.
Het eerste startbit van het eerste telegram na reset (ID1) duurt daardoor ca. 9,4 ms i.p.v. 1,5 ms: dat eerste telegram is corrupt (de eerste twee bytes), vanaf het volgende telegram klopt alles.

### 3.2 De RX-parser (M1.1, PC4, `rx666_parser` 0x0308)

Alleen actief als `r17.1 = 1` (gap). Software-UART, 8N1, 666 baud:

1. **Start**: in `r17.2 = 0` herhaalt elke pas `X=0x7B`, `[0x6B]=8`, `[0x68]=8`; is PC4 laag, dan `r17.2=1` (ontvangst bezig).
2. **Bits** (elke keer als `[0x68]=0`): als `[0x6B] <> 0`: `[0x68]=6` (1,5 ms), `[0x6B]--`, r20 een plaats naar rechts en bit 7 = PC4 (LSB eerst). Eerste bemonstering 8 tikken (2,0 ms) na de startflank, daarna elke 6 tikken.
3. **Byte opslaan** pas als de volgende startflank wordt gezien (`[0x6B]=0`, PC4 weer laag): `st X+, r20`, `[0x69]=0xFF`, `[0x6B]=8`. Opslag stopt bij X=0x85: **10 bytes (0x7B-0x84)**.
   (Dus het laatste byte van een frame wordt pas opgeslagen als er nog een startflank komt.)
4. **Frame klaar** (0x0390): X=0x85 en `[0x69] < 0x7E` (129 tikken = 32 ms zonder reset van `[0x69]`) => r23.6 gezet als r23.5; `[0x69]=0xFF`; `r17 &= 0xF9` (RX en gap voorbij); ontleden:
   * `[0x7C]` (ID, tweede byte van het frame) = **2**: `[0x71] = (u16 uit 0x7E,0x7F & 0x03FF) / 10`, `[0x72] = idem uit 0x80,0x81`, `[0x73] = idem uit 0x82,0x83` (bij 0x83 >= 4: negatieve tak, `-(...)/10`, afgekapt op hele graden). In het 0x91-frame is dat: aanvoer (idx 3-4), retour (5-6), buiten (7-8), little-endian, 0,1 graad.
   * `[0x7C] = 1`: `[0x66] = 0`; `[0x74] = [0x81]` (idx 6 van het 0x91-frame); PB1 hoog als `[0x74] <> 0`, anders laag.
   * Elk ander ID (0, 3, onbekend): geen enkele byte wordt gebruikt.
5. **Wat M1.1 NIET controleert** (verified met de emulator, `e12_rx_nocheck.txt`): kopbyte 0x91, lengtebyte en **CRC worden niet gecontroleerd**; een frame met kopbyte 0x00 en CRC 0000 wordt gewoon ontleed. Bytes die M1.1 nooit leest: 0x7B (kop), 0x7D (lengte), 0x84 (idx 9) en alles na 10 bytes: dus **defrost, compressor-aan, pomptoerental, compressortoerental en vermogen uit de 0x91-berichten worden door M1.1 niet gebruikt**; storingen alleen via ID1 idx 6 (foutcode) en
   communicatie via de 30-s-regel.

**Belangrijk, onopgelost (zie sectie 8): frame-einde-detectie.** In de emulator (volgens de code) komt stap 4 pas als PC4 minstens ca. 36 ms **laag** blijft nadat de buffer vol is. Bij een UART-conforme, daarna weer hoog rustende lijn
blijft de parser hangen (`[0x6D]=0x06`, `[0x69]` blijft 0xFF doordat stap 2 de rusttoestand steeds opnieuw 0xFF geeft), de gap-timer van de TX-machine deelt `[0x69]` en loopt dan nooit af: er wordt geen telegram meer gezonden tot de 25-s-resync (`[0x66]=0x19`), en de storing 'geen ID1 in 30 s' volgt (`e2c_out.txt`: 1 telegram per 25 s).
Met een laaggaande lijn van >= 40 ms na het frame werkt alles normaal (`e2b_break.py`; dat is ook hoe `harness.py` de buitenunit nabootst, instelbaar via `low_after_ms`). Of de echte buitenunit/optocoupler zo'n lange laagperiode aan het frame-einde laat zien kan de dump niet zeggen.

### 3.3 De inter-MCU-link (9615 baud, 8N2, 12 bytes)

UART-instelling in beide chips: UBRR=103 + U2X => 9615 baud (0,16 % boven 9600), UCSRC=0x8E => **8 databits, 2 stopbits, geen pariteit**. Een frame heeft **12 bytes**; byte 11 = som (mod 256) van byte 0..10; **er is geen 0xF0-kop**:
de eerdere aanname "12-byte telegram met 0xF0" klopt alleen voor byte 0 in M1.1 -> T1.1 (zie onder). Beide kanten ontvangen in een buffer die ook als TX-buffer dient (daardoor wordt de eerstvolgende zending gedeeltelijk met de laatst ontvangen bytes gevuld).
Ontvangst: bytes worden pas aanvaard als de stiltevlag (M1.1 r23.4, T1.1 r25.4) gezet is; na het 12e byte (ook bij een foute som) wordt die gewist en loopt een stilteperiode van 25 ms (M1.1 `[0x60]=100` tikken van 250 us; T1.1 `[0x60]=0xC8` tikken van 125 us) waarin binnenkomende bytes worden genegeerd (na reset: 64 ms resp. 32 ms). Dat is de enige hersynchronisatie.

**T1.1 -> M1.1** (opgebouwd in `bouw_link_frame` 0x0232; verified in de emulator, `e10_p2.log`):

| idx | inhoud | gebruik in M1.1 |
|---|---|---|
| 0 | `[0x6E]` = ketelsetpoint (OT ID1) hele graden | -> `[0x6F]` = gevraagde aanvoertemperatuur |
| 1 | `[0x77]` = kamersetpoint (OT ID16) hele graden | -> `[0x77]`; **0 = "geen vraag"**: dan 0x0226 (r23.0=1, `[0x6E]=0`, PB0 uit, F0 in het antwoord) - zie de opmerking onder de tabel |
| 2 | `[0x78]` kamersetpoint fractie | -> `[0x78]` (alleen als idx1 <> 0) |
| 3, 4 | `[0x79]`, `[0x7A]` (T1.1: altijd 0) | -> `[0x79]`, `[0x7A]`, verder ongebruikt |
| 5..10 | de zes laatst door T1.1 van M1.1 ontvangen bytes (idx 5..10) terugge-echood | genegeerd |
| 11 | som | gecontroleerd |

**Opmerking (verified, `e8_m11_e2e.txt` S3 en een PC-trace):** hetzelfde blokkeerpad 0x0226 wordt uitgevoerd als `[0x74] <> 0` (elke foutcode, ook de 30-s-storing) bij elk geldig T1.1-frame:
`r23.0=1`, `[0x6E]=0`, PB0 uit, F0 in het antwoord. `bewaking_65_lockout` wist r23.0 echter weer in de eerstvolgende hoofdlusronde (als `[0x65] >= 0xA0` en `[0x6F] < 55`) en `stand_regelaar` zet PB0 en `[0x6E]=0x0E`
meteen opnieuw. Netto: bij een actieve foutcode valt het vraagrelais bij elk T1.1-frame ca. een lusronde weg, de stand wordt steeds teruggezet op 14 (de regeling kan niet opklimmen) en T1.1 krijgt F0/foutcode (en schakelt zelf naar boiler-modus).

Verzending door T1.1 (getriggerd aan het eind van `ot_tx_thermostaat_PD4`, 0x09F4-0x0A20; verstuurd door `link_tx_naar_M11`): alleen als sinds het laatste geldige M1.1-frame `[0x6C] >= 5` s en op het einde van een thermostaat-uitwisseling; het frame wordt (her)opgebouwd bij `[0x6C]` = 10, 15, 20, 25 en verstuurd (gemeten: 5,15 s het opstartframe, daarna 11,15 s met actuele waarden, 16, 21, 26). Elk geldig M1.1-frame zet `[0x6C]=0`.

**M1.1 -> T1.1** (opgebouwd in `link_rx_frame`/`bewaking`/`seconden_watchdog`, verstuurd door `link_tx_to_T11` als r23.6; verified, `e8_m11_e2e.txt`):

| idx | inhoud | gebruik in T1.1 |
|---|---|---|
| 0 | **0xF0** = "warmtepomp niet beschikbaar / geen vraag / geblokkeerd" (gezet door `bewaking` 0x0644, `link_rx_frame` 0x0226, `seconden_watchdog` 0x07C4); anders de laatst ontvangen idx 0 van T1.1 (echo: buffer gedeeld) | `>= 0xF0` => `r22.1=0`, `r22.0=1` (boiler-modus) |
| 1, 2 | `[0x77]`, `[0x78]` (echo) | genegeerd |
| 3 | `[0x65]` credit-teller | genegeerd |
| 4 | `[0x6F]` gevraagde aanvoer | genegeerd |
| 5 | `[0x70]` doelwaarde na begrenzing | genegeerd |
| 6 | `[0x6E]` stand (intern, 0..40) | -> `[0x6D]`; 0 => `r22.1=0` |
| 7 | `[0x71]` aanvoer (C) | -> `[0x71]` => OT ID25 naar thermostaat |
| 8 | `[0x72]` retour (C) | -> `[0x72]` => OT ID28 |
| 9 | `[0x73]` buitentemperatuur (C, ondertekend) | **door T1.1 weggegooid** (0x0212-0x0214) |
| 10 | `[0x74]` foutcode | -> `[0x73]`: LED-code; <> 0 => `r22.0=1` (boiler-modus), `r22.1=0` |
| 11 | som | |

Frames worden verstuurd: na een T1.1-frame (antwoord, zodra het volgende buitenunit-antwoord is ontleed), bij 30-s-storing (r23.5 vereist) en bij 240-s-T1.1-verlies.
Een geldig M1.1-frame zet in T1.1 `r22.1=1, r22.0=0`; daarna: `idx6=0` => `r22.1=0`; `idx10<>0` of `idx0>=0xF0` => `r22.1=0, r22.0=1`.
Dus `r22.1` ("warmtepomp loopt") = geldige link en stand>0 en geen fout en geen F0. `r22.0` ("boiler-modus") = fout of F0 of link-timeout. Alle OT-herschrijvingen (3.5) gelden alleen als `r22.0 = 0`.

### 3.4 Beslissingen: wat bepaalt welke toestand

Volledig uitgewerkt (functie-tests in `e7_m11_funcs.txt`, end-to-end in `e8_m11_e2e.txt` en `e9_m11_cold.txt`):

* **Vraagrelais PB0 aan**: `stand_regelaar` (r23.0 = 0, `[0x6D].0 = 0`, `[0x6F] >= 20`, PB0 was uit, geen `[0x6F] >= 61`): PB0 aan, `[0x6E]=0x0E` (stand 3 op de lijn), `[0x64]=1`. Of `btn_test_PC1`.
* **Stappen**: de eerste stap *omhoog* komt pas na `[0x64] >= 250` s (r23.1 nog niet gezet); de eerste stap *omlaag* (aanvoer > doel) direct; vanaf de eerste stap (r23.1 gezet, 0x0566/0x0572) elke `[0x64] >= 30` s. Staptabel (`aanvoer` = `[0x71]`, `doel` = `[0x70]`):
  doel - aanvoer = 1: +1 (stopt bij 40); 2..4: +2 (alleen als `[0x6E] < 39`); >= 5: +4 (alleen als `[0x6E] < 37`); aanvoer > doel: -1 (verschil 1), -2 (2..4), -4 (>= 5), met ondergrenzen die `[0x6E]` op >= 4 houden.
  Het reset-gedrag van `[0x65]` (credit): bij aanvoer > doel en niet r24.0 en niet r23.7: `[0x65]=0xB4`.
* **Doelwaarde `[0x70]`**: `[0x6F] < 20` => uit; 20..33 => 34; 34..54 => zelf; 55..60 => 55; **>= 61 => alleen als PB0 al aan is (dan 55 en r24.0=1), anders weigeren** (`[0x65]=0x1E`) - waarschijnlijk hoog-temperatuur-/warm-water-aanvraag die de warmtepomp niet bedient (hypothese).
* **Credit `[0x65]`**: `-1/min` (ISR; bij 0 terug naar 0xB4), `<0xA0` = niet meer "vers"; `< 60` => blokkeren (r23.0=1), stand 0, PB0 uit, F0 naar T1.1 (`bewaking` 0x06D0). Per minuut met `doel - aanvoer >= 4`: min `(verschil - 4)`, ondergrens 60 (nooit hierdoor <60); met r24.0 (setpoint >= 55): -9/min tot ondergrens 60.

### 3.5 T1.1: de OpenTherm-brug per Data-ID (verified, `e5_t11_ot.py`, `e6_modus*.txt`)

T1.1 is **slave naar de thermostaat** (PD7/ACO in, PD4 uit) en **master naar de ketel** (PD3 uit, PD5 in). Het frame wordt 1:1 doorgegeven, behalve:

| Toestand | Wat de ketel krijgt (PD3) | Wat de thermostaat krijgt (PD4) |
|---|---|---|
| DIP2 laag, **r22.0 = 0** (warmtepomp bedient; ketel "afgeremd") | ID0 (status): als data-hoog bit0 (CH-enable) = 1 => vervangen door `READ ID0 0x0200`; anders ongewijzigd. **ID1 (CH-setpoint): altijd `WRITE ID1 0x0A00` (10 C)**, ook als de thermostaat een READ stuurt. **ID16 (kamersetpoint): altijd `WRITE ID16 0x0800` (8,0 C)**. Alle andere ID's/lees-/schrijfopdrachten ongewijzigd | antwoord van de ketel ongewijzigd, **behalve**: ID1 en ID16 => `WRITEACK` met de **eigen** waarde van de thermostaat teruggeechood; **ID25 => `[0x71]` (aanvoer warmtepomp)**, **ID28 => `[0x72]` (retour)**, als `READACK` (data = hele graden << 8; start 20,0 C) |
| DIP2 laag, **r22.0 = 1** (boiler-modus) | alles ongewijzigd (volledig doorgeven) | alles ongewijzigd |
| **DIP2 hoog** (aan/uit-ketel) | niets (PD3 blijft hoog); met r22.0=1: PC5 (K3) hoog zodra ID1-setpoint >= 20 C | T1.1 beantwoordt alles zelf: ID0 -> `0x0200` als setpoint < 20, `0x0302` als warmtepomp loopt (`r22.1`), anders `0x030A`; ID1/7/14/16/17/24 echo (WRITEACK/READACK); ID2 -> DATAINVALID; ID3 -> `0x0400`; ID25/28 -> 20,0 C (of `[0x71]/[0x72]`); ID49 -> `0x4600`; overige -> UNKNOWNID (`data 0`) |

Meegenomen uit de berichten: ID1 -> `[0x6E]` (hele graden), `[0x6F]` (fractie); ID16 -> `[0x77]`, `[0x78]`. Deze twee waarden gaan via de link (idx0..2) naar M1.1.
De verzonden Read/Write-types zijn meegenomen uit de ketel-/vraag-type (het antwoordtype volgt het ketelantwoord). Paritybits worden door T1.1 zelf berekend (`ot_parity8`).

### 3.6 T1.1 status-LED (PC4) en noodbrug (PB2)

* **LED-knippercode** (`led_knippercode`, 0x0AA0; verified in de emulator, `e10_led_*.log`): foutcode `[0x73]` -> aantal flitsen N (0,4 s aan, 0,6 s uit), daarna 3 s pauze (periode N+3 s):
  1 -> 1; 0x41 -> 2; 0x14, 0x15, 0x46, 0x47, 0x48, 0x52 -> 3; 0x31 -> 4; 0x45 -> 5; 0x53 -> 6; alle overige waarden -> 7; 0 -> uit; 0x64 (100 = link verloren) -> geen knipperen: de LED blijft aan (gezet door `link_timeout_noodbrug`).
  Dit mapt de **foutcodes van de Chofu-buitenunit (ID1 idx 6)** op knippers; de betekenis van de codes zelf staat niet in de flash.
* **Noodbrug PB2** (K1+K2, alleen met DIP2 laag): aan als `[0x6C] >= 0xF0` (240 s zonder geldig M1.1-frame), uit bij elk geldig M1.1-frame (gemeten: `e10_p3.log`). Bij aan: `r22.0=1` (boiler-modus), `[0x73]=100`, LED aan,
  en pas dan een noodframe `00 .. 00 64 64` naar M1.1 (alleen als PB2 echt aan staat: bij DIP2 hoog gaat er niets).

## 4. De twee weestelegrammen op 0x0BAA en 0x0BB4

**Inhoud (verified: decodering + CRC, `tables.py`, `tabellen_M11.md`)**

| Adres | Bytes (10-bit-gekaderd, 10 flash-bytes) | Telegram | CRC-16 over alle 8 bytes |
|---|---|---|---|
| 0x0BAA-0x0BB3 | `98 90 21 08 02 FF 80 27 9A 4F` | `19 02 08 00 FF 00 9E C9` (ID2, modus 0x00, **stand 0xFF**) | 0x0000 (geldig) |
| 0x0BB4-0x0BBD | `98 B0 21 08 02 00 80 2E CB 9B` | `19 03 08 00 00 00 37 67` (ID3, payload `00 00 00` i.p.v. `B2 02 00`) | 0x0000 (geldig) |

Ze staan direct onder segment A (0x0BBE) in dezelfde 10-byte-slotindeling als de levende ID2/ID3/ID0-slots (0x0BCE, 0x0BD8, 0x0BE2...). Geen enkele instructie verwijst ernaar.

**Conclusie: er is geen pad. Bewijs in vier onafhankelijke lagen:**

1. **Er is maar een manier om flash als data te lezen**: `LPM`. De ATmega8 heeft 8 KB flash (< 64 KB), dus geen ELPM; `SPM` komt niet voor en zou geen data lezen. In M1.1 staan precies **twee** `LPM`-instructies
   (`lpm_M11.txt`): `0x0752` met vaste `Z = 0x0BBE` (`LDI r30,0xBE / LDI r31,0x0B`, 0x074E) en `0x0A22` in `tx666_machine`. T1.1 heeft er **geen**.
2. **Alle schrijvers van Z (r30/r31) in M1.1** (`z_patterns.txt`): 156 `LDI`-paren met RAM/IO-waarde (< 0x100), precies **vijf** `LDI`-paren met flash-adres
   (0x074E -> 0x0BBE, 0x092A -> 0x0BD8, 0x095E -> 0x0BCE, 0x09A8 -> 0x0BE2, 0x09E4 -> 0x0BBE), vier paren `add r30,r16 / adc r31,r19` (0x094C, 0x0984, 0x09D0, 0x0A0A) waarbij r19 telkens
   vlak ervoor met `eor r19,r19` op nul is gezet (dus optelling van een **niet-negatieve 8-bit offset**), de reset-`eor r30/r31`, en de `pop r30/r31` in de ISR's. Geen `sbiw/subi/sbci` op Z, geen `mov/movw` naar Z, geen `ld` naar r30/r31, geen `ijmp/icall`.
   Dus het Z-adres bij `LPM` is altijd >= het laagste grondtal **0x0BBE**; de weesadressen 0x0BAA-0x0BBD liggen daaronder. *Onderlooprisico (offset negatief) bestaat dus niet.*
3. **Uitputtende emulatie** van de enige routine die de tabel leest (`tx666_machine`, 0x083A), als subroutine aangeroepen (`e4_lpm_sweep.py`, uitvoer `e4_lpm_sweep.txt`):
   alle 256 waarden van de positieteller `[0x6C]` x alle 256 waarden van de stand `[0x6E]` x 16 waarden van de vlaggenbyte `[0x6D]` (alle combinaties van de bits die getest worden) = **1.048.576 aanroepen**, plus niet-nul `[0x6A]`. Resultaat:
   291 verschillende LPM-adressen, **laagste 0x0BBE, hoogste 0x0CE1; geen enkel adres in 0x0BAA-0x0BBD, geen enkel adres onder 0x0BBE**; binnen het tabelgebied 0x0BBE-0x0C4F wordt alles gelezen behalve de opvulbyte 0x0BCD;
   voor stand-index > 10 (`[0x6E] >= 44`, onbereikbaar volgens 3.1) leest hij de lege 0xFF-flash tot 0x0CE1 (acht keer 0xFF = lijn blijft hoog, geen telegram).
4. **Fuzz-emulatie van de complete firmware** (`e11_fuzz.py`, sectie 7): willekeurige T1.1-frames, buitenunit-antwoorden (ook met beschadigde kop/CRC), schakelaars en versnelde klokken; alle LPM-adressen 0x0BBE-0x0C4F, alle uitgevoerde programmatellerwaarden
   liggen op gedecodeerde (live) instructies, alle 666-baud-telegrammen zijn een van de 14 verwachte (zie sectie 7 voor de gemeten aantallen).

**De door u genoemde routes, afzonderlijk afgehandeld:**

* *Directe verwijzing / Z-berekening*: zie 1-2; er is geen enkel `LDI`-paar dat naar een adres in 0x0BAA-0x0BBD wijst (de enige vijf flash-grondtallen zijn 0x0BBE, 0x0BD8, 0x0BCE, 0x0BE2, 0x0BBE; `z_patterns.txt`).
* *Tabelindex stand/4 met onder- of overloop*: de index is `[0x6E] >> 2` (0..63 mogelijk in 8 bits) maar loopt **naar boven** (grondtal 0x0BE2): `Z = 0x0BE2 + ((10 - n) + 10*idx) mod 256`. Overloop leest hogere flash, nooit lagere. Onderloop kan niet, want er wordt nergens afgetrokken.
  De werkelijke stand is begrensd op 0 of 4..40 (index 0..10), zie 3.1.
* *LPM met opgeteld offset*: de vier optellingen zijn niet-negatief (zie 2); r19 is nul.
* *Sprong in de data*: er zijn geen `ijmp/icall/jmp/call`; elke `rjmp/rcall/brxx` wijst (verified: round-trip en CFG-analyse) naar een instructiestart; de laatste instructie van de code voor de data is de `reti` op 0x0BA8, dus er is ook geen doorvalpad (fall-through) naar 0x0BAA.
* *Ander codepad bij een foutstatus*: foutstatussen (`[0x74]`, `[0x66] >= 30`, `[0x67] >= 0xF0`, `[0x65] < 60`) sturen alleen RAM-variabelen en de T1.1-link, geen enkele ervan raakt het tabelgebied. Uitputtend getest voor de functie die de tabel leest (punt 3).
* *Interrupt springt in data*: de vectoren zijn `rjmp` naar 0x002A, 0x0B66, 0x0A6C, 0x0B98; de 15 overige vectoren zijn 0xFFFF en staan uitgeschakeld (geen enkele daarvan wordt in IMSK/TIMSK/GICR/UCSRB ingeschakeld).
* *Zelfprogrammering*: geen `SPM`, SPMCR wordt niet gebruikt.

**Wat ze zouden betekenen als ze gezonden werden (hypothese, niet uit code afleidbaar):**

* Beide zijn geldige telegrammen (CRC 0, framing correct) in dezelfde structuur als de levende. Weestelegram 1 is een ID2 (compressorcommando) met **modus 0x00 (uit) en stand 0xFF**. De levende set kent bij modus 0 alleen stand 0; "uit met stand 255" is een
  waarde die de tabel van 11 standen nooit gebruikt. Mogelijk een test-/override-waarde ("maximaal") uit een eerdere firmwareversie, of een restant van een 0..255-parametrisering; wat de buitenunit ermee doet is niet bekend (hypothese: negeren omdat modus 0).
* Weestelegram 2 is ID3 met een nul-payload. ID3 bevat bij de levende variant `B2 02 00` (zie sectie 5). Een nul-payload zou die parameter op 0 zetten; effect onbekend.
* Dat ze op 10-byte-slots staan direct voor segment A (en in dezelfde volgorde als de levende ID3/ID0/ID2-slots) wijst erop dat de compiler/linker de tabelconstanten in bronvolgorde heeft neergezet en dat de bron twee extra constanten bevatte die later niet meer
  gebruikt zijn (hypothese). Ze zijn niet bereikbaar, dus ook niet in een foutstatus.
* Aanbeveling: ze **niet** zenden. Er is geen aanwijzing dat de buitenunit er veilig op reageert.

## 5. De ID3-vorm "B2 02" en de vaste bytes in ID0/ID1/ID3

**Wat is "B2 02"?** De ID3-telegram `19 03 08 B2 02 00 C1 9A`: kop `19`, nummer `03`, lengte `08`, **payload `B2 02 00`**, CRC `C1 9A`. Als 16-bit little-endian waarde (zoals de temperaturen in de 0x91-antwoorden)
is `B2 02` = 0x02B2 = **690**; als twee losse bytes 178 en 2. De flash zegt niets over de betekenis. Wat wel vaststaat:

* **Het is een constante in de flash** (segment C, 0x0BCE-0x0BD7), eenmaal gedecodeerd (`tables.py`) en nergens aangepast. Er is **geen** pad waarlangs temperatuur, DIP-switch, ketelype, stand, foutcode of tijd deze bytes beinvloeden:
  de bytes komen uitsluitend uit `LPM` op vaste adressen (sectie 4), er wordt nergens naar flash geschreven en de enige RAM-gevoede bitstroom (PB3) is een andere pin.
* Dat geldt voor **alle bytes van ID0, ID1 en ID3** (segmenten D, A, C): `19 00 08 00 00 00 D9 B5`, `19 01 0C 00 00 00 00 00 00 00 AA 35`, `19 03 08 B2 02 00 C1 9A` zijn onder alle bereikbare toestanden identiek (verified:
  uitputtende emulatie van de TX-machine, `e4_lpm_sweep.txt`, plus de fuzz-run waarin elk gezonden telegram met een van de 14 tabelwaarden is vergeleken, `e11_fuzz_*.txt`).
* **Wat wel kan veranderen**: alleen in ID2 (segment B) de bytes 3 (modus: 0x00 of 0x01), 4 (stand 0..10) en 6-7 (CRC). Alle andere bytes van ID2 (`19 02 08 .. .. 00`) zijn vast. Er zijn geen andere variabele bytes op de 666-baud-lijn.
* Voor wat het getal doet bij de buitenunit staat in de dump geen aanwijzing. De forumtabel van WackoH (Tweakers, tweede post, regel "19-3,3") noteert deze byte als onbekend, "0xB2, no change observed" - dat is een bronverwijzing, geen bewijs.
  Hypotheses (niet verifieerbaar uit de dump): (a) een vaste grens/parameter in 0,1 C (69,0 C) of (b) een model-/capaciteitscode voor de Aurea 5. De twee bytes zouden dan voor elke Aurea 5 hetzelfde zijn; of een andere waarde een ander gedrag oplevert kan alleen op de unit zelf
  worden uitgeprobeerd, en dat is hier uitdrukkelijk niet gedaan.
* De variant met `00 00 00` is weestelegram 2 (sectie 4) en wordt niet gezonden.

**Samenvattend: B2 02 is constant en onafhankelijk van alles, ook van het ketelype en de DIP-switch.**

## 6. DIP-switches, ketelype, vraagrelais, ketelbijstook en de 5-graden-regel

### 6.1 Wat leest de firmware bij het opstarten?

**Niets.** Geen van beide chips leest bij reset een ingang (geen DIP-, jumper- of EEPROM-lezing, geen configuratieblok). De poort-init zet alleen richtingen en pull-ups. Alle "configuratie" wordt **doorlopend** (elke hoofdlusronde) gelezen:

| Chip | Ingang | Waar gelezen | Effect |
|---|---|---|---|
| M1.1 | PC0 (pin 23) | `sw_cv_only_PC0` 0x02AE | hoog = "alleen ketel": PB0 uit, `[0x6E]=0`, `[0x65]=0x1E` (als TX niet bezig). Laag = vrijgegeven. Open schakelaar = hoog door de pull-up = veilige stand |
| M1.1 | PC1 (pin 24) | `btn_test_PC1` 0x02D2 | laag + `[0x6F] >= 20` + TX niet bezig: r23.0 wissen, PB0 aan, `[0x6E]=0x0E`, `[0x64]=1`, `[0x65]=0xB4` |
| M1.1 | PC4, PD0 | RX buitenunit, RX T1.1 | data |
| M1.1 | PINB.0 | `stand_regelaar` | leest het eigen vraagrelais terug (toestand) |
| T1.1 | **PC2 (pin 25) = DIP-switch 2** | `ot_verwerk` 0x053E, `ot_tx_ketel_PD3` 0x0896, `link_timeout_noodbrug` 0x0A94 | zie 6.2 |
| T1.1 | PD5, PD7/ACO | OT-RX ketel / thermostaat | data |
| T1.1 | PINB.2 | `link_timeout_noodbrug` | leest het noodbrug-relais terug |

DIP-switch 1 komt niet op een MCU-pin uit (volgens `index.html` zit die in het schakelpad van de noodbrug); de firmware kent hem dus niet. De bits PC0/PC1/PC3 van T1.1 en PC2/PC5 van M1.1 hebben pull-ups maar worden nergens gelezen.
**Een "pompniveau" (1 of 2) bestaat in geen van beide firmwares**: geen uitgang, geen variabele, geen ingang voor de circulatiepomp; de pomp wordt kennelijk door de buitenunit zelf of buiten deze controlbox geregeld.

### 6.2 DIP-switch 2 / PC2: het ketelype (T1.1) - verified in de emulator (`e6_modus*.txt`, `e10_p1.log`, `e13_t11_modeC_id0.txt`)

| PC2 | Betekenis | Gedrag |
|---|---|---|
| **laag** (switch ON, naar massa) | OpenTherm-ketel | T1.1 is master naar de ketel op PD3/PD5 (de thermostaatverzoeken worden doorgegeven, zie 3.5). Noodbrug PB2 is mogelijk bij link-verlies |
| **hoog** (switch OFF, pull-up) | aan/uit-ketel | **Geen OT naar de ketel** (PD3 blijft hoog). T1.1 speelt zelf de OT-slave naar de thermostaat (3.5, derde regel). K3 (PC5) volgt het ketelsetpoint **alleen in boiler-modus** (`r22.0 = 1`) en alleen op ID1-berichten: PC5 hoog bij setpoint >= 20 C (`CPI 0x14`), laag eronder; bij `r22.0 = 0` (warmtepomp bedient) laag. Geen noodbrug PB2 |

Dus de eerdere formulering "OFF = hele ketelkant afgeknepen" is onvolledig: OFF = **aan/uit-ketelmodus** (K3 + eigen OT-slave), niet "uit". Er wordt uitdrukkelijk niets naar de ketel gezonden.
De 5-graden-regel en de brug-herschrijvingen zitten niet in PC2 maar in `r22.0`.

### 6.3 Ketelbijstook ("boiler-modus", `r22.0`) - exacte condities (T1.1)

`r22.0 = 1` (alles wordt ongewijzigd aan de ketel doorgegeven, ID25/28 komen dan van de ketel, K3 volgt het setpoint) als **een** van deze waar is, geevalueerd bij elk geldig M1.1-frame en bij link-verlies:

1. M1.1 byte 10 (foutcode) <> 0;
2. M1.1 byte 0 >= 0xF0 ("niet beschikbaar"): gezet door M1.1 bij (a) kamersetpoint 0 uit T1.1 (geen vraag), (b) foutcode <> 0 bij een T1.1-frame, (c) `[0x65] < 60` (credit op), (d) de koude-start-blokkade (6.5), (e) `[0x6F] >= 61` zonder lopend relais ( `[0x65]=0x1E`), (f) 30 s geen ID1-antwoord, (g) 240 s geen T1.1-frame;
3. 240 s geen geldig M1.1-frame in T1.1 (`[0x6C] >= 0xF0`; dan ook noodbrug PB2 en LED aan als PC2 laag).

`r22.0 = 0` (warmtepomp "bedient", ketel afgeremd) zodra een geldig M1.1-frame zonder 1 en 2 binnenkomt. Dat omvat ook de toestand dat de pomp nog stilstaat (stand 0, `r22 = 0`).

**Het afremmen** (alleen bij `r22.0 = 0`) is wat T1.1 met de thermostaatberichten doet (3.5): CH-enable uit het statusbericht, ketelsetpoint vast 10 C, kamersetpoint vast 8,0 C; de aanvoer- en retourtemperatuur die de thermostaat leest zijn die van de warmtepomp.
Er is **geen** tweede, onafhankelijke OT-master: T1.1 stuurt de ketel alleen een bericht als de thermostaat er een stuurt (het is een brug, geen poller).

### 6.4 Het vraagrelais (PB0, M1.1) - alle condities

**Aan** (PB0 hoog):
* `stand_regelaar` (0x05EC): als r23.0 = 0, `[0x6D].0 = 0`, `[0x71]` onbelangrijk, `[0x6F] >= 20`, niet (`[0x6F] >= 61` terwijl PB0 uit), PB0 stond uit => PB0 aan, `[0x6E]=0x0E`, `[0x64]=1` (functie-test A in `e7_m11_funcs.txt`).
* `btn_test_PC1` (0x02EC): PC1 laag, `[0x6F] >= 20`, `[0x6D].0 = 0`.

**Uit** (PB0 laag, `[0x6E]=0`):
* PC0 hoog (`sw_cv_only_PC0`, zet `[0x65]=0x1E`);
* `[0x6F] < 20` (0x0608); `[0x6F] >= 61` met PB0 uit (0x0616; PB0 blijft uit, `[0x65]=0x1E`);
* `link_rx_frame` 0x0226 (kamersetpoint 0 of foutcode <> 0; door `bewaking` en `stand_regelaar` binnen een lusronde teruggedraaid als `[0x65] >= 0xA0`, zie 3.3);
* `bewaking` 0x06D0 (`[0x65] < 60`); `bewaking` 0x06C2 (koude-start-blokkade, 6.5);
* `seconden_watchdog` 0x0778 (30 s geen ID1-antwoord) en 0x07B8 (240 s geen T1.1-frame, `[0x6F]=0`).

Het is dus op deze print een logische vlag/LED-sturing; of het ooit een echt relais moet schakelen volgt niet uit de code (index.html: gemeten via R21 naar een LED).

### 6.5 De 5-graden-regel - exacte condities (functie-test C/C2 + end-to-end K1/K1b/K2)

Bron: buitentemperatuur uit het ID2-antwoord (idx 7-8), opgeslagen als `[0x73]` in **hele graden, afgekapt richting nul** (4,9 C -> 4; 5,0 C -> 5; -0,1 C -> 0; -7,5 C -> 0xF9 = -7), **vanaf 20 C tot het eerste ID2-antwoord** (`init_ram`).

"Koud" is `[0x73] < 5` **of** `[0x73] >= 200` (twee-complement negatief): dus alles tot en met 4,x C en alle min-temperaturen; precies 5,0 C en hoger is warm. De drempel is hard (CPI 0x05 op 0x06A8) en niet instelbaar (EEPROM is niet in gebruik).

De test wordt **alleen** uitgevoerd in `bewaking_65_lockout` en **alleen** als `[0x6D].0 = 0` (TX niet bezig) en `[0x65] >= 0xA0` (160, de credit is "vers"). Daarbinnen:

| Toestand | koud | warm |
|---|---|---|
| vraagrelais **uit** (pomp wil starten) | `[0x65] = 59 (0x3B)`, r23.7 (koudvlag) = 1, F0 naar T1.1 (als r23.5), in de volgende ronde: `[0x65] < 60` => r23.0 = 1, `[0x6E]=0`, PB0 uit (**start geblokkeerd**, T1.1 gaat naar boiler-modus) | r23.7 = 0, geen actie |
| vraagrelais **aan**, `[0x65] >= 60` | alleen r23.7 = 1 (koudvlag). **Geen directe stop.** | r23.7 = 0 |

Verder werkt r23.7 door in `stand_regelaar`: bij aanvoer > doel wordt de credit **niet** meer naar 0xB4 teruggezet (`sbrc r23,7` op 0x0518 en 0x05B4), zodat `[0x65]` met 1/min verder daalt tot < 60 en dan het normale stoppad (0x06D0) volgt.
**Concreet:** onder 5 C wordt een *koude start* onmiddellijk (binnen enkele seconden) geweigerd (end-to-end K1, `e9_m11_cold.txt`: vraag vanaf t=8 s bij 3,0 C -> relais blijft uit, `[0x65]=0x3B`, frame `F0 ...` naar T1.1; warm: K1b start na het eerste T1.1-frame), terwijl een
al draaiende pomp pas na het leeglopen van de credit stopt (ca. 121 min bij een volle credit van 180: van 0xB4 naar 59 in stappen van 1 per minuut; K2). De eerdere bewering "de koude tak zet de teller hard op 59 en schakelt zo het vraagrelais uit" geldt dus alleen voor de **start**; voor een lopende pomp werkt de
koudvlag via het uitschakelen van het bijladen. End-to-end K2 (`e9_m11_cold.txt`, minuutklok 30x versneld): pomp draait bij 12 C; op t=16 s wordt het 3,0 C; het relais blijft aan, de koudvlag (r23.7, `r23=0x90`) wordt gezet en de credit daalt van 0xAC naar beneden; na het (handmatig op 0x64 gezette) leeglopen onder 60 gaat PB0 uit,
`[0x6E]=0`, r23.0 (blokkade) gezet en gaan `F0`-frames naar T1.1 (`f0 14 00 0a ...`); de credit loopt daarna verder terug. De `e8`-scenario's S1/S2 (`e8_m11_e2e.txt`) laten zien dat een vraag die *voor* het eerste ID2-antwoord binnenkomt (20 C aangenomen) het relais wel aanzet,
ook als het daarna 4,9 C of -7,5 C blijkt: de koudvlag stopt dan niets onmiddellijk.
Eerlijkheidsnotities:
* de test draait niet meer zodra `[0x65] < 0xA0`; de koudvlag blijft dan staan tot de test weer draait (dat is zo na het terugvallen naar 0xB4: bij het eerstvolgende bijladen na `[0x65] -> 0` of een deblokkering);
* start bij exact 5,0 C: warm (CPI 0x05 BRCS: < 5 is koud). Tot het eerste ID2-antwoord (in de emulator ca. 0,5 s na reset) geldt 20 C.

### 6.6 Waar "stand" op de lijn en "5 graden" elkaar raken

De stand (`[0x6E]`) en de koudvlag staan los van de ID2-tabel: de lockout werkt door `[0x6E]=0` en PB0 uit, waarna tabel B[0] (`19 02 08 00 00 00 9D 36`: modus 0, stand 0) wordt gezonden. De buitenunit krijgt dus geen aparte "lockout"-boodschap; hij krijgt "uit".

## 7. Emulatie: de firmware echt gedraaid

### 7.1 De emulator en haar eigen validatie

`emu.py` is een cyclus-nauwkeurige ATmega8-emulator (alle ATmega8-instructies, juiste cycli, interrupt-latentie van 4 cycli + RETI-regel "een instructie uitvoeren"),
met als randapparatuur precies wat beide firmwares gebruiken: poorten B/C/D (met pull-ups en extern gedreven pinnen), Timer1 (normale modus, alle delers),
USART (8N1/8N2, U2X, TXC/RXC, RX-injectie), analoge-comparator-uitgang (ACO) als callback, SP/SREG, een eenvoudige watchdog-teller. Geen ADC, SPI, TWI, EEPROM, Timer0/2 (niet gebruikt).

Validatie van de **kern** tegen een onafhankelijke implementatie (de AVR-simulator in `avr-gdb`, per instructie PC + r0..r31 + SREG + SP vergeleken):

| Test | Omvang | Resultaat |
|---|---|---|
| `test_emu_vs_gdb.py`: beide firmwares vanaf de resetvector, met willekeurige begin-RAM/-registers/-IO, geen randapparatuur (WDR vervangen door NOP) | 2 chips x 8 runs x 30.000 instructies = 480.000 stappen | 0 afwijkingen (`test_emu_vs_gdb.txt`) |
| `test_opcodes_vs_gdb.py`: willekeurige programma's die elke instructieklasse oefenen (ALU met alle vlaggen, MUL-familie, ADIW/SBIW, alle LD/ST-modi, PUSH/POP, IN/OUT/SBI/CBI/skips, branches, LPM) | 8 programma's x 3000 stappen, 73 instructieklassen | 0 afwijkingen (`test_opcodes_vs_gdb.txt`) |

Alle instructieklassen die in de twee firmwares voorkomen (45 resp. 46) zijn door een van beide tests gedekt, behalve `RETI` en `WDR` (gdb-sim heeft geen interrupts/WDR). Die twee zijn functioneel
bevestigd (de stackdiepte `SP=0x045D` in de hoofdlus blijft over miljoenen cycli en tienduizenden interrupts constant). De randapparatuur-modellen zelf (Timer1, USART, pinnen) zijn niet tegen gdb te vergelijken en zijn
daarom bewezen via hun resultaat: de gemeten ISR-periodes (2002,5 en 997,5 cycli) volgen uit 1969/966 + de code in de ISR, en de uitgezonden bitstromen hebben exact de bitduur die uit de code volgt.

### 7.2 Wat is er gedraaid en wat is bevestigd

| # | Experiment (script -> uitvoer) | Wat het bewijst (verified = code gelezen + emulator) |
|---|---|---|
| E1 | M1.1 vrij lopend 40 s (`e1_freerun.py` -> `e1_out.txt`) | De **uitgaande 666-baud-bitstroom**: ID1, ID2, ID3, ID0 in die volgorde, CRC OK, ID2 = stand 0 (`19 02 08 00 00 00 9D 36`), telegramperiode 247/307 ms, rotatie 1,05 s; 25-s-resync en 30-s-storing (`[0x74]=1`) verified |
| E2 | RX-experimenten (`e2_rx_probe.py`, `e2b_break.py`, `e2c_stall.py`, `e12_rx_nocheck.py`) | frame-einde-detectie (laag >= 36 ms nodig), stalgedrag, **geen kop-/lengte-/CRC-controle**, ID2/ID1-ontleding |
| E3 | M1.1 + gesimuleerde T1.1 en buitenunit (`e3_m11_link.py`, `e8_m11_e2e.py`) | start/stand/ID2-variant op de lijn (stand 14 -> `19 02 08 01 03 00 FF 55`), de link-frames in beide richtingen (3.3), foutcode -> PB1/`[0x74]` |
| E4 | uitputtende sweep `sub_083a` (`e4_lpm_sweep.py` -> `e4_lpm_sweep.txt`) | **weestelegrammen onbereikbaar** (sectie 4) |
| E5/E6 | T1.1 + gesimuleerde thermostaat (Manchester op ACO) en ketel (PD5) (`e5_t11_ot.py`, `e6_t11_ids.py` -> `e6_modus{A,B,C}.txt`) | **OpenTherm-uitvoer** op PD3 en PD4: alle Data-ID's 0..0x40 + 0x73 + 0x7F als READ en WRITE in de drie modi (A: DIP2 laag/pomp, B: boiler-modus, C: DIP2 hoog); pariteit en Manchester-halve-bits van alle uitgezonden frames kloppen |
| E7 | functie-tests M1.1 (`e7_m11_funcs.py` -> `e7_m11_funcs.txt`) | exacte beslistabellen: doelklem, regelstap, 5-graden-regel, schakelaars, 25/30/240-s-regels |
| E9 | 5-graden-regel end-to-end (`e9_m11_cold.py` -> `e9_m11_cold.txt`) | koude start geweigerd; warme start; koud worden tijdens bedrijf (versnelde minuutklok) |
| E10 | T1.1: K3, linkframes, noodbrug, LED (`e10_t11_misc.py`) | 6.2, 3.3, 3.6 |
| E11 | fuzz-invarianten op M1.1 (`e11_fuzz.py` -> `e11_fuzz_{1..4}.txt`) | zie 7.3 |
| E13 | T1.1 DIP2 hoog (`e13_t11_modeC_id0.py`) | ID0-antwoorden van de lokale slave |
| E14 | stand wijzigt midden in ID2 (`e14_mid_change.py`) | gemengde, CRC-foute ID2-telegrammen mogelijk |

### 7.3 Fuzz-run op M1.1

Per seed: 120 s simulatietijd, willekeurige T1.1-frames (0.5-3 s), buitenunit-antwoorden met willekeurige temperaturen/foutcodes en in 25 % van de gevallen met beschadigde kop, lengte, CRC of payload, willekeurige PC0/PC1,
versnelde minuutklok (`[0x63]=58` elke seconde) en versnelde stapklok (`[0x64]`). Invarianten die bij elke 50 ms zijn gecontroleerd:
(1) `[0x6E]` in {0} U [4,40]; (2) alle LPM-adressen binnen 0x0BBE-0x0C4F; (3) elk uitgevoerd programma-adres is een gedecodeerde (live) instructie; (4) elk 666-baud-telegram is geldig en gelijk aan een van de 14 tabelwaarden.

Resultaat (4 seeds x 120 s, `e11_fuzz_1..4.txt`):

| | seed 1 | seed 2 | seed 3 | seed 4 |
|---|---|---|---|---|
| `[0x6E]` waarden gezien | 0,6,10,14,16 | 0,10,14,18 | 0,10,12,14,18 | 0,6,10,14,18 |
| schendingen bereik (0 of 4..40) | geen | geen | geen | geen |
| uitgevoerde PC's buiten live-code | geen | geen | geen | geen |
| verschillende instructies uitgevoerd | 1225 van 1470 | 1236 | 1216 | 1230 |
| LPM-adressen | 0x0BBE-0x0C13 (75), alle binnen de tabel | idem | idem | idem |
| 666-baud-telegrammen | 273, allemaal tabelwaarde | 274, allemaal tabelwaarde | 273, **1 afwijkend** | 274, **1 afwijkend** |

De twee afwijkende telegrammen (seed 3 en 4) zijn beide `19 02 08 01 00 00 9D 36`: modusbyte van een stand >= 1 en de rest van stand 0 - precies het "gemengde ID2" dat ontstaat als `link_rx_frame` midden in een ID2-telegram
`[0x6E]` op 0 zet (E14). De fuzz heeft dit dus onafhankelijk gevonden. Geen enkel ander telegram week af en de gedecodeerde ID0/ID1/ID3 waren in alle runs byte-identiek aan de tabel.

Opklim-test (`e11b_climb.py`, doel 55 C, aanvoer 20 C, versnelde stapklok): zie `e11b_climb.txt` (resultaat in 7.4).

### 7.4 Opklimmen naar de maximale stand (`e11b_climb.txt`, verified)

Doel 55 C, aanvoer 20 C, stapklok versneld: `[0x6E]` loopt 0 -> 14 (start) -> 18 -> 22 -> 26 -> 30 -> 34 -> 38 -> **40 en stopt** (stappen +4, laatste +2). Stand op de lijn dus 3 -> 10. Op de lijn verschijnen
`(modus, stand)` = (1,3), (1,5), (1,6), (1,8), (1,10) (de tussenstanden vallen tussen twee ID2-telegrammen); alle ID2-telegrammen zijn exact tabelvarianten B[k]; het hoogste gelezen flash-adres is **0x0C4F**, het laatste byte van B[10]. De stand 10 is dus het plafond
en de tabelindex komt nooit boven de tabel uit; ook 40 + 4 = 44 (index 11, lege flash) is onbereikbaar omdat bij 0x25 maximaal +4 (tot 0x28), bij 0x27 maximaal +1 en bij 0x28 niet meer wordt opgeteld.

## 8. Correcties op eerdere analyse en onopgeloste punten

### 8.1 Correcties op eerdere reconstructies (`index.html`, `*_reversed.ino`)

| # | Eerdere bewering | Wat de firmware (twee methoden) laat zien |
|---|---|---|
| 1 | "12-byte telegram met 0xF0 en 8-bit som" voor de link | Geen kop. Frames zijn 12 bytes + som; `0xF0` komt alleen voor als **waarde van byte 0 in M1.1 -> T1.1** ("niet beschikbaar", 3.3). In de andere richting is byte 0 het ketelsetpoint |
| 2 | 5-graden-regel: "de koud-tak zet de teller hard op 59 en schakelt zo het vraagrelais uit" | Geldt voor een **koude start** (relais uit) en alleen als `[0x65] >= 0xA0`. Een draaiende pomp stopt niet direct maar niet meer-bijladen van de credit ("koudvlag"). "Enige plek die de teller onder 60 duwt" klopt niet: ook `[0x65]=0x1E` op 0x02BC (PC0), 0x0616 (setpoint >= 61). Zie 6.5 |
| 3 | `[0x65]` wordt "elke 60 ticks (~15 ms)" verlaagd | Elke **60 seconden** (de tak zit in de seconde-afhandeling van de ISR: `[0x63]` telt seconden tot 60). Verified: de ISR-tak op 0x0B1C en de emulator (K2) |
| 4 | DIP2 OFF = "hele ketelkant afgeknepen" | OFF = **aan/uit-ketel**: geen OT naar de ketel, T1.1 speelt zelf OT-slave, K3 (PC5) volgt in boiler-modus het setpoint >= 20 C (6.2) |
| 5 | M1.1 ontleedt "het 0x91-bericht met ID/lengte" | Alleen ID (byte 1) telt; kop, lengte en **CRC worden niet gecontroleerd** (3.2) |
| 6 | PB3 "tweede, parallel meelopende bitstroom naast PC3" | PB3 stuurt niet dezelfde bits maar de **laatst ontvangen buitenunit-bytes** op dezelfde bitklok (echo, 3.1) |
| 7 | T1.1-link: "[0x73] buitentemperatuur" (T1.1) | M1.1 byte 9 (buitentemperatuur) wordt door T1.1 weggegooid; T1.1 `[0x73]` = M1.1 byte 10 = **foutcode** (LED, boiler-modus) |
| 8 | De PB0-"vraagrelais" is een relais | Op de print gemeten als LED-sturing (index.html); in de code is het een vraagvlag met terugleesfunctie (6.4) |
| 9 | stand-index = stand/4 | Bevestigd; de index **is** de stand op de lijn (0..10); `[0x6E]` is intern 0..40 (3.1) |
| 10 | Weestelegrammen "geen adresberekening bereikt ze" | Bevestigd, nu met statisch en uitputtend bewijs (sectie 4) |
| 11 | "M1.1 praat 666 baud, software-UART, CRC-16" | Zenden: bitgeklokte software-UART uit tabellen met vooraf berekende CRC, **de firmware rekent zelf nergens een CRC** (geen CRC-routine in de flash). Ontvangen: geen CRC-controle |

### 8.2 Onopgeloste punten

| # | Punt | Status | Wat is er nodig |
|---|---|---|---|
| 1 | **Frame-einde-detectie van de 0x91-antwoorden** (3.2): volgens de code wordt een antwoord pas ontleed als PC4 na het 10e opgeslagen byte >= ca. 36 ms laag blijft. Met een UART-conforme, hoog rustende lijn loopt de ontvangst vast (eerst 25 s resync, dan 30 s storing). | Code verified, werkelijk lijngedrag **onbekend** | Logic-analyzer op PC4 (IC1 pin 27, na de opto) tijdens een echt antwoord, en op de zendlijn van de buitenunit: is het niveau na het frame laag (of is er een lange break)? Eventueel M1.1 draaien met de echte unit en kijken of `[0x66]` (seconden sinds ID1) onder 25 blijft |
| 2 | **Klokfrequentie en fuses** niet in de dump. 8 MHz is een aanname (consistent met 666,67 baud = 6 x 250 us en met de eerder gevonden kristal; de ISR-overhead 33,5 cycli klopt voor 8 MHz). Lock-/fusebits, BODLEVEL, CKSEL, BOOTSZ onbekend. | niet af te leiden | Uitlezen van de originele chips met een programmer die fuses toont (bijv. avrdude `-U lfuse:r -U hfuse:r`), of foto/meting van het kristal |
| 3 | **PB3 (M1.1 pin 17)**: echo van de laatst ontvangen buitenunit-bytes op de zendbitklok. Functie (diagnose-uitgang? tweede zender?) niet vast te stellen. | Gedrag verified, doel onbekend | Meten of er iets aan pin 17 van IC1 hangt (volgens `index.html` is alleen pin 14 doorgemeten) |
| 4 | **Betekenis van "B2 02"** en van de twee weestelegrammen (stand 0xFF, nul-payload) | onbekend; constanten | Alleen op de unit zelf (niet doen) of via documentatie/servicemenu van Chofu; vergelijken van ID3 bij andere modellen |
| 5 | **Betekenis van de foutcodes** (ID1 idx 6) en dus van de LED-knippercodes; welke code bij welke storing | onbekend | Foutcodelijst van de Chofu AEYC / Atlantic Aurea; de dump bevat alleen de mapping code -> aantal flitsen (3.6) |
| 6 | **Inhoud van de overige 0x91-velden** (defrost idx 4 ID1, compressor-aan, toerental, vermogen): M1.1 gebruikt ze niet, dus de firmware kan ze niet verklaren | buiten de firmware | WackoH-metingen en eigen logging van de ESP32 |
| 7 | **Hoe de echte unit het ID2-telegram interpreteert bij modus 0 met stand 0xFF** (weestelegram 1) en wat "modus 0x02" zou doen | onbekend | idem (niet doen) |
| 8 | **Waarom de limiet 55/61 C** op de gevraagde aanvoer (`[0x6F] >= 61` weigert een start). Hypothese: warm-waterbereiding wordt niet door de warmtepomp bediend. | hypothese | Thermostaatlogs met gelijktijdig setpoint (OT ID1 > 60) |
| 9 | **Niet nagespeeld**: de watchdog-reset (nooit afgelopen in de runs), de EEPROM (niet gebruikt), de analoge-comparator-timing met echte spanningsniveaus, de werkelijke jitter van de OT-bits. De emulator kent geen analoge delen. | bewust buiten scope | Hardwaretest met logic analyzer/oscilloscoop |
| 10 | **Menging van varianten in een ID2-telegram als `[0x6E]` halverwege wijzigt**: nu in de emulator aangetoond (`e14_mid_change.txt`: gemengde bytes, CRC fout, bijv. `19 02 08 01 00 00 9D 36`). Of de echte buitenunit zo'n corrupt telegram stilletjes negeert is niet bekend | verified in emulator; gevolg onbekend | Meten hoe de unit reageert op een CRC-fout (niet doen aan de unit) |
| 11 | **Real-time gedrag van de regelaar** (stappen per 30 s / 250 s) is functioneel getest met geforceerde RAM-waarden/versnelde klokken, niet als echte 10-minutenrun | beperkt bewijs | Lange emulatierun (kost ca. 1 uur rekentijd per 10 min) |
| 12 | **Fysieke polariteit van PC4/PC3 en de opto's** (de dump kent alleen pin-niveaus) | uit `index.html`/WackoH | Meting aan de print |

## 9. Bestandsoverzicht (alles in `analyse-volledig\`)

**Rapport en tabellen**

| Bestand | Inhoud |
|---|---|
| `RAPPORT.md` | dit rapport (samengesteld uit `deel*.md` door `assemble.py`) |
| `dekking_M11.csv`, `dekking_T11.csv` | dekkingskaart: elk adresbereik met categorie (vector / code-live / code-dood / data-tabel / vrij) |
| `roundtrip_M11.txt`, `roundtrip_T11.txt` | resultaat round-trip A (eigen encoder), B (avr-as) en C (avr-objdump) |
| `listing_M11.txt`, `listing_T11.txt` | eigen disassembly (live + dode code, labels, kruisverwijzingen) |
| `tabellen_M11.md` | gedecodeerde telegramtabellen met CRC-controle |
| `functies_M11.md`, `functies_T11.md`, `aanroepgraaf_*.md` | functielijsten, aanroepers, aangeroepen functies, IO-gebruik, aanroepgraaf |
| `io_*.csv`, `ram_*.csv` | alle IO-toegangen (per functie) en RAM-kruisverwijzing (leesters/schrijvers per adres) |
| `lpm_*.txt`, `z_patterns.txt` | alle LPM-plaatsen en alle schrijvers van het Z-register (bewijs sectie 4) |
| `vergelijk_avrdis.txt`, `verify_decoder.txt` | controle van `avrdis.py` en van de eigen decoder tegen `avr-objdump` over alle 65536 opcodes |
| `test_emu_vs_gdb.txt`, `test_opcodes_vs_gdb.txt` | validatie van de emulator-kern tegen de gdb-AVR-simulator |
| `e*_*.txt`, `e*.log` | uitvoer van de afzonderlijke emulatie-experimenten (zie 7.2) |

**Scripts** (alle Python 3; nodig: GNU binutils/avr-gdb uit de Arduino-installatie, paden bovenaan in de scripts)

| Script | Doel |
|---|---|
| `avrlib.py` | eigen AVR-decoder/encoder (tabelgedreven) |
| `analyze.py`, `coverage.py` | recursieve codetracering, listing, dekkingskaart en round-trips |
| `static_analysis.py`, `z_patterns.py`, `tables.py` | functies/IO/RAM, Z-schrijvers/LPM, tabeldecodering |
| `verify_decoder.py`, `compare_avrdis.py`, `verify_helpers.py` | decoderverificatie |
| `emu.py` | de ATmega8-emulator; `harness.py`, `ot.py`, `uartdec.py` stimuli/decoders |
| `test_emu_vs_gdb.py`, `test_opcodes_vs_gdb.py` | validatie tegen gdb-sim |
| `e1`..`e14` | experimenten (E1 vrij lopend, E2 RX, E3/E8 M1.1 end-to-end, E4 LPM-sweep, E5/E6 OpenTherm, E7 functie-tests, E9 koude start, E10 T1.1, E11 fuzz, E12 RX-controle, E13 DIP2-hoog, E14 stand halverwege) |
| `assemble.py` | voegt de `deel*.md` samen tot `RAPPORT.md` |

Reproduceren: `python coverage.py`, `python static_analysis.py`, `python verify_decoder.py`, `python test_emu_vs_gdb.py 30000 8`, `python test_opcodes_vs_gdb.py 8 3000`, daarna de `e*`-scripts (de langste, `e9` en `e11`, draaien enkele minuten tot een halfuur).
