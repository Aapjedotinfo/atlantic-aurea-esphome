# AVR-brug v2 (firmware 2.0.0)

Nieuwe firmware voor de ATmega328P in de IC2-socket van de Atlantic Aurea-controlbox: de OpenTherm-brug tussen thermostaat en
CV-ketel, aangestuurd door de ESP32. Dit is een **nieuwe, aparte map**; v1 (`..\avr-bridge\`) is niet gewijzigd.
**Niets hiervan is geflasht of op hardware geprobeerd.** Wat op de PC getest is en wat alleen op de bank kan staat onderaan.

Documenten: `PROTOCOL.md` (exacte bytelayout, bindend voor de ESP-kant), dit bestand, en het ontwerp
`unit2-documentatie\avr-v2-ontwerp-onderscheppen.md`.

## Wat v2 is, en het verschil met v1

| | v1 | v2 |
|---|---|---|
| Link met de ESP | 9600 baud, vaste frames `F0..F3`, 8-bits som | **38400 baud 8N1**, kader `A5 type lengte volgnr payload CRC-8`, resync, HELLO met versie |
| Regels | twee substituties + `ovr[]` ingebakken | **tabel per Data-ID en richting** (PASS, VAST, VRAAG_ESP, BLOKKEER), komt van de ESP, ingebouwde standaard |
| Beslissen door de ESP | nee | **VRAAG/ANTWOORD** met volgnummer en deadline (instelbaar, met bovengrens) |
| Opstart | ketel vrij, noodbrug aan tot het eerste commando | **geremd**, CH-vlag onderdrukt, noodbrug los tot **240 s** zonder ESP |
| ESP weg | na 60 s ketel vrij | beleid vervalt, rem **aan** (10 C), tabel terug naar de standaard, noodbrug na 240 s |
| Verlaat ketelantwoord | blijft staan, schuift daarna alle antwoorden een vraag op | wordt gewist; alleen een antwoord met passend Data-ID en type telt |
| ID 0 en ID 16 van het origineel | niet nagebouwd | tabelregels gekoppeld aan de rem (CH-vlag eraf + DHW aan; 8,0 C) |
| Rem-houdtijd | alleen op de vlag, 5 min vast | op vlag **en** setpoint (10 <-> 35 pendelt niet), instelbaar 60..900 s |
| ACK van ID 1/16 | toont 10/35 C | toont de waarde van de thermostaat (echo, zoals het origineel) |
| Dip 2 | alleen een sensor | bepaalt het **keteltype** tot de ESP iets anders zegt; K3 alleen bij aan/uit-ketel |
| K3 en noodbrug | interlock | interlock **plus** 50 ms tussen uit en aan; de AVR zwijgt op de OT-lijnen terwijl de noodbrug aan is |
| Zonder thermostaat (poll) | DHW-bit uit | DHW-bit aan, CH alleen op verzoek van de ESP |
| Eerste 10 s | verzonnen antwoorden | de ketel wordt meteen gevraagd |

Behouden uit v1: de Manchester-tik en de OpenTherm-ontvangst/zenders (**byte voor byte uit v1 gekopieerd**, regel 88-357), de poort-init,
watchdog (2 s, in `.init3`), lampje, testuitgangen, polariteitsvlaggen, zelfstandige slave (ketel dood of afgedwongen), keep-alive-poll,
injectie naar de ketel, overrides van antwoorden, override-setpoint (ID 9/100), max. modulatie. Uit v1 vervallen: `relay_follows` (werkte
omgekeerd, yaml-schakelaar uitgecommentarieerd).

Afwijkingen van en invullingen bij het ontwerp staan in `PROTOCOL.md` hoofdstuk 11 (o.a. EVENT/INJECT toegevoegd, "rem loslaten" uitgelegd als
"rem blijft aan", conditie- en vlagbits in de modus-byte, rem-houdtijd als vijfde tijd, K3-noodstand, DHW-keuze).

## Bestanden

| Bestand | Inhoud |
|---|---|
| `avr-bridge-v2.ino` | alleen hardware: pinnen, Manchester-tik (v1), UART-ISR's, lus |
| `brug.c/.h`, `tabel.c/.h`, `proto.c/.h`, `otframe.h`, `platform.h` | de kern: gewone C, geen hardware, getest op de PC |
| `avr-bridge-v2.hex` | het resultaat, klaar om te branden (zonder bootloader) |
| `PROTOCOL.md` | bytelayout van alle berichten, tabel, tijden, grenzen, hexvoorbeelden |
| `tools/protocol_ref.py` | onafhankelijke referentie-encoder (voor de ESP-kant en voor de tests) |
| `test/` | tests, simulatie, gegenereerde vectoren (`vectors.h`), bouwscripts |

## Bouwen

Zelfde instellingen als v1 (zie `..\README.md`): Arduino Pro/Pro Mini, **ATmega328P, 8 MHz** (`arduino:avr:pro:cpu=8MHzatmega328`).

```bash
"/c/Program Files (x86)/Arduino/arduino-builder.exe" -compile \
  -hardware "C:\Program Files (x86)\Arduino\hardware" \
  -tools "C:\Program Files (x86)\Arduino\tools-builder" \
  -tools "C:\Program Files (x86)\Arduino\hardware\tools\avr" \
  -fqbn=arduino:avr:pro:cpu=8MHzatmega328 -build-path <bouwmap> avr-bridge-v2.ino
```

Neem `avr-bridge-v2.ino.hex` (niet `with_bootloader`). Bouwopties (`-D` in de compilervlaggen of bovenaan het bestand):
`DHW_AAN_STANDAARD` (1, zie PROTOCOL 11.7) en `NOOD_K3_VOLGT` (1, zie 11.6). De builddatum komt uit `__DATE__`.

**Gebruik (van deze hex):** flash 13158 B van 30720 B (42 %); statisch RAM 880 B van 2048 B (43 %), dus **57 % vrij** voor de stack
(de zwaarste aanroepketen is geschat op ~250 B, niet gemeten).

## Branden

Fuses **ongewijzigd t.o.v. v1**: `lfuse 0xFF` (extern kristal, het 8 MHz-kristal onder de IC2-voet), `hfuse 0xD9` (reset naar adres 0, geen
bootloader, SPIEN aan, WDTON uit), `efuse 0xFD` (BOD 2,7 V). Reden om niet af te wijken: dezelfde chip, dezelfde klok, en 0xD9 voorkomt de
"Bootloader branden"-valkuil (BOOTRST). Zie `..\README.md` voor de TL866-II en de ArduinoISP-weg (kip-en-ei met de klok: kristal op het breadboard, of
fuses als laatste).

```bash
avrdude -c avrisp -P COM5 -b 19200 -p m328p -B 125 -U flash:w:avr-bridge-v2.hex:i
avrdude -c avrisp -P COM5 -b 19200 -p m328p -U lfuse:w:0xFF:m -U hfuse:w:0xD9:m -U efuse:w:0xFD:m
```

Let op: de ESP en de AVR moeten **tegelijk** naar 38400 baud (yaml `baud_rate: 38400` bij `avr_uart`, en een ESP-component die dit protocol spreekt).
Een v2-AVR met de huidige v1-ESP praat niet.

## Testplan voor op de bank

Benodigd: de geprogrammeerde ATmega, 5 V, kristal 8 MHz, een USB-seriele adapter (38400 8N1) of de ESP32, een OpenTherm-bron (`hardware\testavr.py`
of een tweede ESP), liefst een scoop of logic analyzer.

1. **Leven:** voeding aan. Binnen ~100 ms komt een HELLO (`A5 01 0E ...`, versie 2.0.0, protocol 2, tabel-CRC `9FD3` bij DHW-aan-standaard). Daarna elke 2 s
   een HELLO en een STATUS (38 bytes). Controleer met de hexdumps uit PROTOCOL.md hoofdstuk 12 en `tools/protocol_ref.py`.
2. **CRC:** stuur het voorbeeld-COMMANDO uit hoofdstuk 12 met een seriele adapter (min. 5 ms ertussen). STATUS `link` bit 3 gaat aan. Verander een byte: STATUS
   `crc_fouten` telt op en er verandert niets.
3. **Tabel:** stuur de voorbeeld-TABEL; een ACK met status 0 komt terug en STATUS `tabel_crc` wordt de CRC uit het voorbeeld. Stuur de tabel met
   `deadline 700 ms`: ACK met status 2, niets verandert.
4. **Opstart:** na de voeding: `link` bit 2 (rem) aan, `io` bit 1 (noodbrug) **uit** en pas na 240 s zonder bericht aan; K3 (PC5) blijft laag. Meet PB2 en PC5 met een
   multimeter of led. Met dip 2 hoog (aan/uit) blijft PB2 altijd laag.
5. **ESP-stilte:** COMMANDO met rem uit en `geldig_s = 15` sturen, dan stoppen: na 15 s moet de rem weer aan zijn (direct), de ID 1-waarde richting ketel weer 10 C.
6. **OpenTherm met een bron:** zie `hardware\testavr.py`. Verwacht met de standaardtabel: READ ID 0 met data `0300` -> naar de ketel `0200`; WRITE ID 1 `2D00` -> ketel `0A00`,
   antwoord aan de thermostaat `2D00`; READ ID 16 -> WRITE `0800`. Dat is het gedrag van het origineel (analyse T1.1).
7. **Ketel traag of zwijgend:** laat de ketelsimulator 450 ms of niets doen: de thermostaat krijgt binnen ~400 ms toch een antwoord; een laat antwoord daarna wordt niet doorgegeven
   (STATUS `ketel_oud` telt op).
8. **VRAAG/ANTWOORD:** tabel met een `VRAAG_ESP`-regel (zie voorbeeld), VRAAG komt uit, stuur het ANTWOORD met hetzelfde volgnummer; met een ander volgnummer of te laat gebeurt er niets.
9. **Timing/UART (zie onder):** scoop op PD4/PD3 tijdens verkeer op de link; `isr_overruns` (STATUS byte 21) moet **0** blijven, `uart_overruns` (27) ook.
10. **Dan pas** de controlbox: ketel geblokkeerd, de Remeha als proefthermostaat.

## Wat op de PC getest is (en wat niet): LET OP, de PC-tests zijn NIET (volledig) gedraaid

* **Gecompileerd en gelinkt** met de Arduino-toolchain (avr-gcc 7.3, `-Wall`): geen fouten, een eigen `USART_RX_vect` botst niet met de core. De `.hex` is zo gebouwd.
* **Onafhankelijk gecontroleerd:** alleen de protocolspecificatie zelf: `tools/protocol_ref.py` (geschreven uit PROTOCOL.md, niet uit de C-code) levert de hexvoorbeelden
  en `test/vectors.h`; de CRC-controlewaarden (0xF4, 0x29B1) komen uit Python. Een vergelijking C-encoder tegen die vectoren **is geschreven maar niet met succes afgerond**.
* **Testcode aanwezig maar niet bruikbaar als bewijs:** `test/` bevat een simulatie (klok, thermostaat, ketel, ESP) en een testrunner (`test_kern.c`) voor CRC/parser/resync, tabelgrenzen,
  COMMANDO, HELLO/fallback, VRAAG/ANTWOORD (verkeerd/laat volgnummer, deadline), tabelwissel, ketelantwoord, tijdbudget 700 ms, opstart/noodbrug/K3, interlock.
  Er is geen mingw-gcc en de aanwezige Visual Studio mist de C-headers, dus native draaien lukte niet. Een run met avr-gcc in de AVR-simulator van avr-gdb gaf
  273 geslaagd / 57 mislukt, maar die run is **niet betrouwbaar**: de testbouw heeft meer dan 2 kB RAM nodig (de simulator kapt dat af) en met meer geheugen
  liep de run te lang om af te wachten. Ik heb de mislukkingen **niet** onderzocht; reken er niet op dat de kernlogica correct is tot de tests schoon draaien. Opruimen en afmaken: volgende stap.
* De simulator kent geen randapparatuur, dus de hele firmware (ISR's, UART, pinnen) is op de PC nooit gedraaid.

## Alleen op hardware te bevestigen

* De OpenTherm-ISR is ongewijzigd, maar de **extra belasting door de UART bij 38400 baud** is alleen geschat (zie onder) en moet op de bank gemeten worden (`isr_overruns`).
* Dat de UART-bytes echt foutloos binnenkomen (0,16 % baudfout met U2X, UBRR=25) en dat de ESP-kant 5 ms pauze tussen berichten en aaneengesloten bytes levert.
* Wat K1/K2 precies afkoppelen; of de AVR tijdens de noodbrug beter kan zwijgen (dat doet hij nu) en of K3 en de noodbrug elektrisch nooit samen aantrekken.
* Het gedrag van de ketel op de rem (10 C, CH-vlag eraf, DHW aan, 8,0 C kamersetpoint), en of DHW-aan gewenst is (docs adviseren "raak DHW niet aan").
* Of Anna, Remeha en Honeywell het echo-ACK en de READ-ACK-vervanging van ID 16 accepteren.
* De polariteit van PD4 (`TX_INVERT_STAT = 1`), onveranderd uit v1: ontwerp 8 besluit "houden"; niet gemeten.
* Dat de dipschakelaar 2 de verwachte kant op staat (laag = OpenTherm-ketel).
* Brown-out/watchdog-gedrag en het opstarten gelijktijdig met de ESP (HELLO/TABEL-volgorde in de praktijk).

## Onderbrekingsbelasting bij 38400 baud (schatting, uit de disassemblage van de gebouwde hex)

* Tik-ISR (Timer1, 125 us): ongewijzigd uit v1, ~290 cycli = ~36 us = ~29 % van de CPU (v1-schatting, niet opnieuw gemeten).
* UART-RX-ISR: ~75 cycli = ~9 us per byte; UDRE-ISR: ~65 cycli = ~8 us per byte. Bij een bytetijd van 260 us is dat hoogstens ~3,5 % + ~3,1 % bij volle link in beide richtingen.
  Timer0 (`millis`) < 0,5 %. Totaal ongeveer 36 %; er is dus ruim 60 % over voor de lus.
* **Effect op de tik:** een UART-ISR die net bezig is vertraagt de tik hooguit ~9 us (twee achter elkaar ~17 us) op een tik van 125 us: de periode zelf ligt vast
  (CTC-hardware), alleen het afhandelmoment schuift. De bitklok van 1 ms (8 tikken) wordt niet verstoord; de ontvanger meet intervallen in tikken (drempel 6 van 8).
* **Effect op de UART:** een tik-ISR (36 us) kan een UART-byte hooguit ~36 us uitstellen, ver onder twee bytetijden (520 us); een verloren byte is daardoor niet te verwachten.
* Dit is een schatting; `isr_overruns` en `uart_overruns` in STATUS zijn de bankmeting.
