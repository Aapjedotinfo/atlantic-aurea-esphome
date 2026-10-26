# Linkprotocol AVR-brug v2 (protocolversie 2)

Stand 26-10-2026. Bij firmware **2.0.0**. Dit document is bindend voor beide kanten: met alleen dit document moet de ESP-kant
(`aurea_link`) te bouwen zijn zonder de AVR-code te lezen. Gebaseerd op `unit2-documentatie\avr-v2-ontwerp-onderscheppen.md`;
waar dit document daarvan afwijkt of iets invult staat dat in hoofdstuk 11.

Alle hexdumps in hoofdstuk 12 zijn gemaakt met `tools/protocol_ref.py`, een encoder die uit dit document is geschreven en
**niet** uit de C-code; de C-tests vergelijken beide byte voor byte.

## 1. Fysieke laag

| | |
|---|---|
| Snelheid | **38400 baud**, 8 databits, geen pariteit, 1 stopbit (8N1). Op de AVR (8 MHz) is dat U2X=1, UBRR=25, dus 38461 baud (0,16 % fout). |
| Byte-tijd | 260 us (10 bits). Een bericht van N bytes duurt N x 0,26 ms. |
| Pinnen AVR | PD0 = RX van de ESP, PD1 = TX naar de ESP (zoals v1). ESP: `rx_pin GPIO19`, `tx_pin GPIO18` (zoals nu). |
| ESPHome | `baud_rate: 38400`, `stop_bits: 1`, `data_bits: 8`, `parity: NONE`. (v1 was 9600 baud met 1 stopbit in de yaml.) De ESP en de AVR moeten tegelijk wisselen. |

## 2. Berichtkader

Elk bericht, in deze volgorde op de lijn:

| Offset | Bytes | Veld | Uitleg |
|---|---|---|---|
| 0 | 1 | `SYNC` | altijd `0xA5` |
| 1 | 1 | `type` | zie hoofdstuk 4 (`0x01` .. `0x09`) |
| 2 | 1 | `lengte` | aantal **payload**-bytes (0..96). Dus niet de lengte van het hele bericht. |
| 3 | 1 | `volgnr` | teller van de **zender**, telt per verzonden bericht op (alle typen samen), loopt rond 255 -> 0. Dient om verlies te zien en (bij VRAAG/ACK) om te koppelen. |
| 4 | `lengte` | payload | per type |
| 4+`lengte` | 1 | `CRC-8` | over `type`, `lengte`, `volgnr` en de hele payload (dus alles tussen `SYNC` en de CRC) |

Totale berichtlengte = `lengte` + 5.

* **Bytevolgorde:** alle getallen van meer dan een byte zijn **big-endian** (hoge byte eerst), net als in v1. Een OpenTherm-frame
  van 32 bits gaat als 4 bytes, MSB eerst (`b3 b2 b1 b0`, dus byte 0 = pariteit/type/..., byte 1 = Data-ID).
* **Temperaturen** zijn getekende 16-bit **tienden graden** (355 = 35,5 C), tenzij anders vermeld. Een OpenTherm-waarde in
  een frame blijft f8.8.
* **CRC-8:** polynoom `0x07` (x^8 + x^2 + x + 1), startwaarde `0x00`, geen reflectie, geen eindxor (CRC-8/SMBUS).
  Controlewaarde: CRC-8 over de ASCII-bytes `123456789` is **`0xF4`**.
* **CRC-16 (alleen voor de tabel):** polynoom `0x1021`, startwaarde `0xFFFF`, geen reflectie, geen eindxor (CRC-16/CCITT-FALSE),
  hoge byte eerst. Controlewaarde over `123456789` is **`0x29B1`**.
* **Onbekend type met geldige CRC:** negeren (voor uitbreidingen); de lengte kan toch gevolgd worden.
* **Verkeerde lengte voor het type:** het bericht is ongeldig (zie ACK-status 1).

## 3. Resync en timing

* **Zendersplicht:** tussen twee berichten (einde stopbit van het ene tot `0xA5` van het volgende) **minstens 5 ms** stilte. De
  AVR doet dat ook. (Vijf ms is ruim 19 byte-tijden; een `0xA5` midden in de payload kan dan nooit als begin worden aangezien.)
* **Ontvanger:**
  1. Wacht op `0xA5`. Andere bytes in deze toestand worden weggegooid.
  2. Lees `type` en `lengte`. `lengte` > 96: afkeuren (stap 5).
  3. Lees `volgnr`, payload en CRC. Tussen twee bytes van een bericht mag **minder dan 5 ms** zitten (de zender schrijft het bericht aaneengesloten); bij een pauze van 5 ms of meer wordt het bericht afgebroken en telt de volgende byte als nieuw begin (zoek `0xA5`).
  4. Klopt de CRC: bericht verwerken, terug naar stap 1. Een bericht direct na een goed bericht hoeft niet te wachten.
  5. **Fout** (CRC, lengte, afgebroken): de ontvanger gooit alles weg tot er **minstens 5 ms geen byte** is binnengekomen, en zoekt dan
     opnieuw `0xA5` (stap 1). Zo valt hij nooit midden in een bericht in.
* Een foutief bericht wordt dus nooit gedeeltelijk toegepast. Een tabel of commando wordt pas gebruikt na een goede CRC **en** als
  alle waarden binnen de grenzen vallen (hoofdstuk 8).
* **Wachtrij AVR -> ESP:** VRAAG gaat voor alles. Dan ACK, HELLO, STATUS en tenslotte EVENT. Een VRAAG moet hooguit wachten op
  het bericht dat al onderweg is (het langst is STATUS, 38 bytes = 9,9 ms) plus de pauze van 5 ms: **hooguit 15 ms** vertraging.
* **Timing van een VRAAG/ANTWOORD:** VRAAG 10 bytes = 2,6 ms, ANTWOORD 11 bytes = 2,9 ms. ESP-verwerking aanbevolen < 50 ms.

## 4. Berichttypen

Alle offsets hieronder zijn **binnen de payload** (offset 0 = de eerste byte na `volgnr`).

| Type | Naam | Richting | Payload-lengte | Totaal |
|---|---|---|---|---|
| `0x01` | HELLO | AVR -> ESP | 14 | 19 |
| `0x02` | TABEL | ESP -> AVR | 14 + 5 x n (n = 0..16) | 19 .. 99 |
| `0x03` | ACK | beide | 2 | 7 |
| `0x04` | VRAAG | AVR -> ESP | 5 | 10 |
| `0x05` | ANTWOORD | ESP -> AVR | 6 | 11 |
| `0x06` | STATUS | AVR -> ESP | 33 | 38 |
| `0x07` | COMMANDO | ESP -> AVR | 15 | 20 |
| `0x08` | EVENT | AVR -> ESP | 5 | 10 |
| `0x09` | INJECT | ESP -> AVR | 5 | 10 |

`EVENT` en `INJECT` zijn **toevoegingen aan het ontwerp** (de twee niet-genummerde berichten van v1: het rauwe OT-frame `0xF2` en de
injectie `0xF3`). Zonder die twee verliest de ESP `get_ot()` voor alle Data-ID's en `write_boiler`, `read_boiler`,
`override_to_thermostat`, `clear_override`.

### 4.1 HELLO (AVR -> ESP, `0x01`, 14 bytes)

Verstuurd na elke reset van de AVR (ongeveer 100 ms na de start), daarna elke 2 s zolang er nog geen geldig ESP-bericht is gehoord
(of 5 s zolang de ESP weg is), en op elk moment dat de AVR terugvalt op de standaardtabel.

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | sw_hoofd | softwareversie, hoofdnummer (2) |
| 1 | 1 | sw_neven | nevenummer (0) |
| 2 | 1 | sw_patch | patchnummer (0) |
| 3 | 1 | bouw_jaar | jaar - 2000 (26) |
| 4 | 1 | bouw_maand | 1..12 |
| 5 | 1 | bouw_dag | 1..31 |
| 6 | 1 | protocol | **2** |
| 7 | 2 | tabel_crc | CRC-16 van de tabel die nu actief is (hoofdstuk 5.4); bij de ingebouwde standaardtabel die van `standaardtabel` (0x9FD3 voor firmware 2.0.0 met DHW-aan) |
| 9 | 1 | resetreden | bit 0 PORF (stroom), bit 1 EXTRF (reset-pin), bit 2 BORF (brownout), bit 3 WDRF (watchdog), zoals `MCUSR` |
| 10 | 1 | bootteller | aantal starts sinds de laatste koude start (0 bij stroom aan, daarna +1 per reset), 0..255 |
| 11 | 2 | kenmerken | bit 0: dipschakelaar 2 hoog (aan/uit-ketel); bit 1: tabel is de ingebouwde standaard; bit 2: firmware is gebouwd met `NOOD_K3_VOLGT`; bit 3: EVENT wordt ondersteund (altijd 1); bit 4: INJECT wordt ondersteund (altijd 1); rest 0 |
| 13 | 1 | max_regels | grootste tabel die de AVR aankan (16) |

De ESP toont `sw_hoofd.sw_neven.sw_patch (bouwdatum)` als tekstsensor "AVR-versie", vergelijkt `protocol` met wat hij kent (onbekend:
melding in HA) en vergelijkt `tabel_crc` met de CRC van zijn eigen tabel. Wijkt die af, dan stuurt hij een TABEL.

### 4.2 TABEL (ESP -> AVR, `0x02`, 14 + 5 x n bytes)

| Off | Bytes | Veld | Eenheid / bereik |
|---|---|---|---|
| 0 | 1 | tabelversie | 0..255, alleen voor weergave (de AVR doet er niets mee, hij gaat wel in de CRC) |
| 1 | 2 | deadline_ms | standaard-deadline ESP-antwoord, ms. **30..600**. Standaard 250. |
| 3 | 2 | ketel_ms | wachttijd op het ketelantwoord, ms. **100..600**. Standaard 400. |
| 5 | 2 | esp_weg_s | tijd zonder geldig ESP-bericht waarna de AVR de ESP als weg beschouwt, s. **10..600**. Standaard 60. |
| 7 | 2 | nood_s | tijd zonder geldig ESP-bericht (gerekend vanaf de AVR-start of het laatste bericht) waarna de noodbrug mag aantrekken, s. **60..1800** en **>= esp_weg_s**. Standaard 240. |
| 9 | 2 | rem_hold_s | minimale tijd tussen twee wissels van de rem (vlag of setpoint), s. **60..900**. Standaard 300. |
| 11 | 1 | n | aantal regels, **0..16** |
| 12 | 5 x n | regels | zie 5.1 |
| 12 + 5n | 2 | tabel_crc | CRC-16 over de bytes **0 t/m 11 + 5n** (alles van `tabelversie` tot en met de laatste regel) |

De AVR rekent de CRC-16 zelf na en vergelijkt. Ook als de berichten-CRC-8 klopt, wordt een tabel met een afwijkende CRC-16, met
een waarde buiten de grenzen, met een ongeldige regel of met dubbele (id, richting) **geheel afgewezen** (ACK-status 2). Er verandert dan
niets.

De AVR neemt een goede tabel pas over **tussen twee OpenTherm-gesprekken** (als er geen vraag openstaat; dat duurt hooguit
ongeveer 0,7 s) en stuurt dan een ACK op het volgnr van de TABEL. Een nieuwe TABEL die binnenkomt vóórdat de vorige is
overgenomen vervangt de wachtende.

### 4.3 ACK (beide richtingen, `0x03`, 2 bytes)

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | bevestigd | het `volgnr` (kopveld) van het bericht dat bevestigd wordt |
| 1 | 1 | status | 0 = OK; 1 = verkeerde lengte of onbekende inhoud voor dit type; 2 = waarde buiten de grenzen of ongeldige regel; 4 = bezet (INJECT-rij vol) |

De AVR stuurt een ACK **altijd** op een TABEL (status 0 of 2) en op een COMMANDO of INJECT **alleen bij een fout** (status 1, 2 of 4).
De ESP mag een ACK op HELLO sturen (status 0) maar hoeft niet: elk geldig ESP-bericht telt voor de AVR als teken van leven.

### 4.4 VRAAG (AVR -> ESP, `0x04`, 5 bytes)

Alleen als een regel `VRAAG_ESP` is en de ESP als aanwezig geldt. Maximaal **een** vraag tegelijk open.

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | richting | 0 = T>K (frame van de thermostaat, nog niet naar de ketel), 1 = K>T (antwoord van de ketel, nog niet naar de thermostaat) |
| 1 | 4 | frame | het OpenTherm-frame, MSB eerst |

Het `volgnr` in de kop is het nummer dat de ESP terugstuurt in `ANTWOORD.vraag_seq`.

### 4.5 ANTWOORD (ESP -> AVR, `0x05`, 6 bytes)

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | vraag_seq | het `volgnr` (kop) van de VRAAG waarop dit antwoordt |
| 1 | 1 | actie | 0 = DOORGEVEN (het frame ongewijzigd), 1 = VERVANGEN door `frame`, 2 = BLOKKEREN (AVR antwoordt lokaal met UNKNOWN-DATAID) |
| 2 | 4 | frame | bij VERVANGEN het nieuwe frame, MSB eerst (de pariteit rekent de AVR zelf uit); anders 0 |

De AVR accepteert een ANTWOORD **alleen** als er een vraag openstaat, `vraag_seq` bij die vraag past en de deadline niet verlopen is.
Een ander, oud of te laat volgnummer wordt **genegeerd** (alleen geteld). Een VERVANGEN met een frame waarvan het Data-ID of het
berichttype niet past (T>K: type 0 of 1 en hetzelfde ID; K>T: type 4..7 en hetzelfde ID) is ongeldig en geldt als "geen antwoord":
de AVR doet dan de standaardactie van de regel zodra de deadline verloopt.

### 4.6 STATUS (AVR -> ESP, `0x06`, 33 bytes, elke 2 s)

Vervangt het v1-bericht `0xF0` (24 bytes). "v1" geeft het byteadres in dat bericht.

| Off | Bytes | Veld | Eenheid / inhoud | v1 |
|---|---|---|---|---|
| 0 | 1 | master_status | hoge byte van Data-ID 0 van de thermostaat: bit 0 CH, 1 DHW, 2 koelen | 1 |
| 1 | 1 | slave_status | lage byte van Data-ID 0 van de ketel: bit 0 storing, 1 CH-modus, 2 DHW-modus, 3 vlam | 2 |
| 2 | 1 | requested_setpoint | wat de thermostaat vraagt (Data-ID 1), hele graden, **voor** de rem | 3 |
| 3 | 2 | room_temp | tienden graden (ID 24) | 4-5 |
| 5 | 2 | room_setpoint | tienden graden (ID 16) | 6-7 |
| 7 | 2 | boiler_temp | tienden graden (ID 25 van de ketel) | 8-9 |
| 9 | 1 | modulation | % (ID 17) | 10 |
| 10 | 1 | fault_code | lage byte van ID 5 | 11 |
| 11 | 1 | link | bit 0 thermostaat aanwezig, 1 ketel aanwezig, 2 rem **toegepast**, 3 COMMANDO geldig, 4 ESP aanwezig (<`esp_weg_s`), 5 tabel is de ingebouwde standaard, 6 er staat een vraag open bij de ESP, 7 handshake gedaan (geldig ESP-bericht sinds de laatste HELLO) | 12 (anders) |
| 12 | 1 | io | bit 0 niveau dipschakelaar 2 (1 = hoog = aan/uit), bit 1 noodbrugrelais (PB2) aan, bit 2 K3 (PC5) aan, bit 3 actief keteltype is aan/uit, bit 4 noodbrug is automatisch (ESP-stilte), bit 5 AVR antwoordt zelf (zelfstandig of aan/uit-ketel), rest 0 | 19 (anders) |
| 13 | 1 | flanken_stat | ruwe flanken thermostaatkant sinds het vorige STATUS (verzadigt op 255) | 13 |
| 14 | 1 | flanken_ketel | idem ketelkant | 14 |
| 15 | 1 | frames | lage nibble: complete frames thermostaatkant sinds vorig STATUS (max 15); hoge nibble: daarvan op pariteit afgekeurd | 15 |
| 16 | 1 | lijnen | bit 0 ACO, 1 PD5, 2 PORTD.4 (TX stat), 3 PORTD.3 (TX ketel), 4 swap, 5 tx_invert stat, 6 tx_invert ketel | 16 |
| 17 | 1 | rx_fout_stat | tellerstand OT-ontvangstfouten thermostaatkant (verzadigt op 255) | 17 |
| 18 | 1 | rx_fout_ketel | idem ketelkant | 18 |
| 19 | 1 | spare | bit 0 PC0, 1 PC1, 2 PC3 (ongebruikte ingangen met pull-up) | 20 |
| 20 | 1 | reset | lage nibble `MCUSR` (PORF, EXTRF, BORF, WDRF), hoge nibble bootteller (max 15) | 21 |
| 21 | 1 | isr_overruns | tikken waarin de OT-tik-ISR niet op tijd klaar was (hoort 0 te zijn) | 22 |
| 22 | 2 | tabel_crc | CRC-16 van de actieve tabel | nieuw |
| 24 | 1 | tabel_versie | `tabelversie` van de actieve tabel (0 bij de ingebouwde) | nieuw |
| 25 | 1 | tabel_n | aantal regels van de actieve tabel | nieuw |
| 26 | 1 | crc_fouten | berichten met slechte CRC/lengte van de ESP (verzadigt op 255, niet gewist) | nieuw |
| 27 | 1 | uart_overruns | bytes verloren in de UART of de ontvangstrij (verzadigt) | nieuw |
| 28 | 1 | vraag_verlopen | VRAAG's waarop de deadline verliep zonder geldig antwoord (verzadigt) | nieuw |
| 29 | 1 | ketel_oud | ketelframes weggegooid omdat ID/type niet bij de vraag pasten (verzadigt) | nieuw |
| 30 | 2 | sinds_esp | seconden sinds het laatste geldige ESP-bericht (verzadigt op 65535) | nieuw |
| 32 | 1 | ev_gemist | EVENT's weggegooid omdat de rij vol was (verzadigt) | nieuw |

Wat uit v1 **vervalt**: het volgnummer in de hoge nibble van `link` (zit nu in de kop; de ESP ziet gemiste berichten aan gaten in `volgnr`),
`relay_follows` (bit 2 van v1-byte 19). De bit "frontschakelaar ingedrukt" (v1-byte 19, bit 0) is nu het ruwe niveau van dip 2 in `io`
bit 0, **met omgekeerde polariteit** (1 = hoog = aan/uit).

### 4.7 COMMANDO (ESP -> AVR, `0x07`, 15 bytes, elke 2 s)

Vervangt `0xF1` (16 bytes). Het is de volledige huidige sturing; wat hier ontbreekt is "uit". Wordt het COMMANDO langer dan
`geldig_s` niet vernieuwd, dan **vervalt het** (zie hoofdstuk 7).

| Off | Bytes | Veld | Bereik / inhoud | v1 |
|---|---|---|---|---|
| 0 | 1 | flags1 | bit 0 rem aan; bit 1 temperatuursubstitutie (aanvoer/retour) aan; bit 2 leesrichting thermostaatkant omkeren; bit 3 leesrichting ketelkant omkeren; bit 4 noodbrug aan op verzoek (alleen bij OpenTherm-ketel); bit 5 lampje omkeren; bit 6 zelfstandige stand: "vlam" melden; bit 7 zelfstandige stand afdwingen | b[1] (zelfde bits) |
| 1 | 1 | flags2 | bit 0 zendpolariteit thermostaatkant omkeren; bit 1 idem ketelkant; bit 2 kanalen wisselen; bit 3 CV-vlag onderdrukken (`ch_off`); bit 4 K3 vrijgeven (`demand_enabled`); bit 5 rem **direct** (houdtijd overslaan); bits 7..6 keteltype: 0 = dipschakelaar, 1 = OpenTherm-ketel, 2 = aan/uit-ketel, 3 = ongeldig | b[2] (bits 0-3 deels anders) |
| 2 | 1 | brake_setpoint | setpoint dat de rem oplegt, hele graden, **0..90** | b[3] |
| 3 | 2 | wp_supply | aanvoer van de warmtepomp voor de thermostaat, tienden, **-300..1500** | b[4-5] |
| 5 | 2 | wp_return | retour, tienden, -300..1500 | b[6-7] |
| 7 | 2 | override | kamersetpoint-override via Data-ID 9, tienden; **0 = uit**, anders 50..300 | b[8-9] |
| 9 | 1 | lamp | 0 auto, 1 uit, 2 aan, 3 traag, 4 snel (**0..4**) | b[10] |
| 10 | 1 | sa_mod | modulatie die de zelfstandige stand meldt, % (**0..100**) | b[11] |
| 11 | 1 | test_out | bit 0 PB0, 1 PB1, 2 PB3, 3 PB4, 4 PD2 (**0..0x1F**) | b[12] |
| 12 | 1 | boiler_sp | rechtstreeks ketelsetpoint zonder thermostaat, hele graden, **0..90**, 0 = uit | b[13] |
| 13 | 1 | max_mod | maximale modulatie %, **0..100**, 0 = niet ingrijpen | b[14] |
| 14 | 1 | geldig_s | geldigheid van dit COMMANDO in seconden: 0 = gebruik `esp_weg_s`, anders **10..255** | nieuw |

**Wat de AVR doet met een COMMANDO:** de velden gaan pas in werking als alle waarden binnen de grenzen vallen (anders: ACK status 2 en niets
verandert). Een geldig COMMANDO zet de beleidstoestand op "geldig" en start de geldigheidsteller opnieuw.
De rem (`flags1.0` met `brake_setpoint`) wordt niet meteen toegepast maar via de houdtijd (hoofdstuk 7.3).

**Weggevallen uit v1:** `relay_follows` (het relais laten volgen op de dipschakelaar; werkte omgekeerd t.o.v. het origineel en de
yaml-schakelaar stond uitgecommentarieerd, zie de review). v1-`relay_cmd` (flags1 bit 4) blijft, maar werkt nu alleen bij een
OpenTherm-ketel en nooit tegelijk met K3.

### 4.8 EVENT (AVR -> ESP, `0x08`, 5 bytes)

Elk OpenTherm-frame dat de AVR ziet of verstuurt, zoals v1 `0xF2` (alleen de som is vervangen door de gewone kop en CRC).

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | richting | 0 = FROM_STAT (thermostaat vroeg dit), 1 = FROM_BOILER (ketel antwoordde), 2 = TO_BOILER (AVR stuurde dit naar de ketel, ook na vervanging), 3 = TO_STAT (AVR antwoordde dit de thermostaat, ook na vervanging) |
| 1 | 4 | frame | het OpenTherm-frame, MSB eerst |

De AVR bewaart er hooguit **7** in een rij; is die vol, dan wordt het nieuwste weggegooid en telt `ev_gemist`. EVENT's hebben de laagste prioriteit.

### 4.9 INJECT (ESP -> AVR, `0x09`, 5 bytes)

Zoals v1 `0xF3`:

| Off | Bytes | Veld | Inhoud |
|---|---|---|---|
| 0 | 1 | modus | 0 = frame **naar de ketel** sturen (de AVR is daar master; het gaat in een gaatje tussen twee thermostaattransacties; de pariteit rekent de AVR uit; type moet 0 of 1 zijn); 1 = **antwoord vervangen**: vraagt de thermostaat dit Data-ID (en antwoordt de ketel met READ-ACK), dan gaat de waarde uit `frame` (onderste 16 bits) mee; 2 = die override **opheffen** voor het Data-ID in `frame` |
| 1 | 4 | frame | OpenTherm-frame; bij modus 1 en 2 tellen alleen Data-ID (bits 23..16) en (modus 1) de waarde (bits 15..0) |

Wachtrij naar de ketel: 4 frames (3 bruikbaar, zoals v1). Acht overrides tegelijk. Vol: ACK status 4. Een override werkt alleen terwijl het COMMANDO geldig is en geldt tot hij wordt opgeheven,
of tot het COMMANDO ongeldig wordt (dan worden **alle** overrides gewist, zie 7.2). Bij een AVR-reset vervallen ze ook.

## 5. De tabel

### 5.1 De tabelregel (5 bytes)

| Off | Veld | Inhoud |
|---|---|---|
| 0 | id | OpenTherm Data-ID (0..255) |
| 1 | richting | 0 = **T>K** (bericht van de thermostaat naar de ketel), 1 = **K>T** (antwoord van de ketel naar de thermostaat) |
| 2 | modus | bits 1..0 **modus**; bits 4..2 **conditie**; bit 5 MASKER; bit 6 FORCEER; bit 7 ECHO |
| 3 | p0 | parameter, hoge byte |
| 4 | p1 | parameter, lage byte |

Elke combinatie (id, richting) komt **hoogstens een keer** voor. Een id dat niet in de tabel staat is PASS (doorgeven zoals het is).

**Modus (bits 1..0):**

| Waarde | Naam | Wat de AVR doet | Parameter p0 p1 |
|---|---|---|---|
| 0 | PASS | doorgeven | 0 0 |
| 1 | VAST | invullen/aanpassen, zie 5.2 | waarde, of masker (zie 5.2) |
| 2 | VRAAG_ESP | vasthouden, de ESP vragen, zie hoofdstuk 6 | p0 = deadline in eenheden van 4 ms (**0..150**, 0 = standaard `deadline_ms`); p1 = standaardactie bij verlopen deadline: 0 = PASS, 1 = BLOKKEER |
| 3 | BLOKKEER | niet doorgeven, lokaal beantwoorden | p0 = 0; p1 = berichttype van het lokale antwoord: **0 of 7** = UNKNOWN-DATAID, 6 = DATA-INVALID |

Bij BLOKKEER T>K gaat er niets naar de ketel; de AVR antwoordt de thermostaat zelf (na minstens 40 ms, de OpenTherm-ondergrens is 20 ms)
met het frame van de vraag, type vervangen door p1, waarde ongewijzigd. Bij BLOKKEER K>T wordt het ketelantwoord weggegooid en antwoordt de AVR op dezelfde manier.

**Conditie (bits 4..2):** de regel werkt alleen als de conditie waar is; anders geldt PASS.

| Waarde | Conditie |
|---|---|
| 0 | altijd |
| 1 | de rem is **toegepast** (na de houdtijd; ook na opstart) |
| 2 | temperatuursubstitutie actief (COMMANDO geldig en `flags1.1`) |
| 3 | override-setpoint actief (COMMANDO geldig en `override` <> 0) |
| 4 | `ch_off` actief (COMMANDO geldig en `flags2.3`) |
| 5 | de rem is toegepast **of** `ch_off` actief |
| 6 | `max_mod` actief (COMMANDO geldig en `max_mod` <> 0) |
| 7 | gereserveerd: een tabel met deze waarde wordt afgewezen |

**Vlaggen (alleen bij VAST; anders moeten ze 0 zijn):**

* **MASKER (bit 5):** zie 5.2. Alleen voor T>K.
* **FORCEER (bit 6):** T>K: een READ-DATA wordt als WRITE-DATA doorgegeven (zodat ook een leesvraag het setpoint krijgt zoals in het
  origineel bij ID 16). K>T: ook een UNKNOWN/INVALID-antwoord van de ketel wordt vervangen (de AVR maakt er dan een ACK van).
* **ECHO (bit 7):** alleen K>T. De waarde in het antwoord is de waarde die de thermostaat zelf in zijn vraag stuurde. Alleen als die vraag een WRITE was
  (anders blijft het ketelantwoord ongewijzigd).

### 5.2 VAST: wat er gebeurt

**Waardevorm** (MASKER niet gezet), parameter = 16-bit waarde `p0p1`:

* **T>K:** de waarde van een WRITE-DATA wordt vervangen. Een READ-DATA blijft ongewijzigd, behalve met FORCEER: dan wordt het een WRITE-DATA met deze waarde.
* **K>T:** is het ketelantwoord een ACK (type 4 of 5), dan wordt de waarde vervangen; met FORCEER ook bij type 6 of 7. Het antwoordtype volgt altijd het type van de vraag
  van de thermostaat (READ -> READ-ACK, WRITE -> WRITE-ACK). De AVR berekent de pariteit.

**Bijzondere waarden** (in plaats van een vaste waarde, alleen in de waardevorm):

| p0p1 | Betekenis | Eenheid |
|---|---|---|
| `0xFFFF` | het **rem-setpoint** zoals toegepast (na houdtijd), hele graden -> f8.8, begrensd op 100 | |
| `0xFFFE` | aanvoer van de warmtepomp (COMMANDO `wp_supply`) -> f8.8 | |
| `0xFFFD` | retour van de warmtepomp (`wp_return`) -> f8.8 | |
| `0xFFFC` | override-setpoint (`override`) -> f8.8 | |
| `0xFFFB` | maximale modulatie (`max_mod`) % -> f8.8 | |

**Maskervorm** (MASKER gezet, alleen T>K, alleen type READ/WRITE): `p0` = bits die in de **hoge byte** van de dataveld gewist worden, `p1` = bits die daarna gezet worden:
`hoog = (hoog & ~p0) | p1`. De lage byte blijft. Voor Data-ID 0 is de hoge byte de masterstatus: bit 0 CH-enable, bit 1 DHW-enable.

### 5.3 De ingebouwde standaardtabel (staat in de AVR; "veilige standaard")

Geldt na elke start tot er een geldige TABEL is overgenomen, en nadat de ESP langer dan `esp_weg_s` zwijgt. Tijden: 250 ms, 400 ms, 60 s, 240 s, 300 s. Tabelversie 0.
Dit is wat het origineel doet bij een geremde ketel (T1.1, "r22.0 = 0"), zonder de bijzondere ESP-sturing:

| # | id | richting | modus | betekenis |
|---|---|---|---|---|
| 1 | 0 | T>K | `0x35` (VAST, conditie 5, MASKER) p=`01 02` | masterstatus: CH-enable eraf, DHW-enable aan (alleen als de rem is toegepast of `ch_off`) |
| 2 | 1 | T>K | `0x05` (VAST, conditie 1) p=`FF FF` | Write-Data ID 1 krijgt het rem-setpoint (10 C na opstart) |
| 3 | 16 | T>K | `0x45` (VAST, conditie 1, FORCEER) p=`08 00` | kamersetpoint 8,0 C, ook bij een leesvraag |
| 4 | 14 | T>K | `0x19` (VAST, conditie 6) p=`FF FB` | maximale modulatie volgt `max_mod` (alleen als actief) |
| 5 | 1 | K>T | `0x85` (VAST, conditie 1, ECHO) p=`00 00` | het ACK van ID 1 toont de waarde van de thermostaat |
| 6 | 16 | K>T | `0x85` p=`00 00` | idem ID 16 |
| 7 | 25 | K>T | `0x09` (VAST, conditie 2) p=`FF FE` | aanvoer van de warmtepomp (bij substitutie) |
| 8 | 28 | K>T | `0x09` p=`FF FD` | retour van de warmtepomp |
| 9 | 9 | K>T | `0x4D` (VAST, conditie 3, FORCEER) p=`FF FC` | override-setpoint via ID 9 |
| 10 | 100 | K>T | `0x4D` p=`00 00` | ID 100 (override-functie) = 0 |

Met `DHW_AAN_STANDAARD = 0` bij het bouwen (zie README) wordt p1 van regel 1 `00` en verandert de tabel-CRC (dan wordt de CRC in HELLO anders).
Aanbeveling voor de ESP: stuur deze tien regels (onder voorbehoud van de DHW-keuze) als basis en voeg eigen regels toe; dan is de ESP-tabel een uitbreiding
van wat de AVR ook zonder ESP doet.

### 5.4 Tabel-CRC

CRC-16/CCITT-FALSE over de `tabelversie`, de vijf tijden, `n` en alle regels in tabelvolgorde (precies de payloadbytes 0 t/m 11 + 5n van de TABEL).
De AVR meldt de CRC van zijn actieve tabel in HELLO en in STATUS (22-23). De CRC van de ingebouwde standaardtabel is **0x9FD3**.

## 6. VRAAG en ANTWOORD in de transactie

Een OpenTherm-transactie: de thermostaat stuurt een vraag (T), de AVR stuurt die naar de ketel (K), de ketel antwoordt, de AVR stuurt dat
door naar de thermostaat. De thermostaat eist een antwoord **binnen 800 ms** na het einde van haar frame.

1. **T>K met VRAAG_ESP:** de AVR stuurt `VRAAG(T>K, frame)`, houdt het frame vast en wacht hooguit de deadline van de regel (of `deadline_ms`).
   ANTWOORD DOORGEVEN: het frame gaat ongewijzigd naar de ketel. VERVANGEN: het nieuwe frame gaat naar de ketel. BLOKKEREN: lokaal UNKNOWN-DATAID, de ketel merkt niets.
   Geen geldig ANTWOORD op tijd: de standaardactie (p1 van de regel) en `vraag_verlopen` telt op.
2. **Naar de ketel:** vóór elk frame wist de AVR alles wat de ketelzijde nog als ontvangen had staan. De AVR accepteert daarna alleen een
   ketelframe met geldige pariteit, een antwoordtype dat bij de vraag past (READ -> 4, 6, 7; WRITE -> 5, 6, 7) en **hetzelfde Data-ID** als wat hij stuurde.
   Alles anders (te laat, van een eerdere vraag, ruis) wordt weggegooid (`ketel_oud`) en de AVR blijft wachten.
3. **K>T met VRAAG_ESP:** de AVR stuurt `VRAAG(K>T, ketelframe)` en wacht op het ANTWOORD; de acties betekenen hier: DOORGEVEN = ketelframe ongewijzigd naar de thermostaat,
   VERVANGEN = nieuw frame (type 4..7, hetzelfde ID), BLOKKEREN = UNKNOWN-DATAID.
4. **Altijd een antwoord:** komt er geen (geldig) ketelantwoord binnen `ketel_ms`, dan antwoordt de AVR zelf: Data-ID 0 met de laatst bekende slavestatus als READ-ACK,
   alles anders UNKNOWN-DATAID (zoals v1).
5. **Onwrikbare einddeadline (niet instelbaar):** vanaf het moment dat het frame van de thermostaat is ontvangen heeft de AVR **700 ms** (`BUDGET_MS`) om het antwoord
   te beginnen te sturen. De deadline van de ESP, de wachttijd op de ketel en de ESP-vraag over het ketelantwoord worden daarbinnen afgekapt: ook met alle instellingen
   op hun maximum (600 + 600 + 600 ms) is het antwoord er uiterlijk op 700 ms plus een lusronde (minder dan 3 ms). Dat laat 100 ms marge op de 800 ms.
6. **Aanwezigheid van de ESP:** is de ESP weg (hoofdstuk 7.1), dan wordt een VRAAG_ESP-regel niet gestuurd maar meteen de standaardactie gedaan.
7. Een nieuwe TABEL of COMMANDO verandert niets aan een lopende transactie: een tabel wordt pas daarna overgenomen.

## 7. Toestanden en terugval

### 7.1 Tijdlijn zonder ESP

Alle tijden vanaf het laatste geldige ESP-bericht (bij de start: vanaf de start).

| Tijd | Wat |
|---|---|
| 0 (start) | **geremd** (rem toegepast, 10 C), CH-vlag onderdrukt (standaardtabel, conditie 5), K1/K2 (noodbrug) **los**, K3 **open**, tabel = standaard, keteltype volgens dipschakelaar 2 |
| ~100 ms | HELLO |
| `geldig_s` (of `esp_weg_s`) na het laatste COMMANDO | beleid **ongeldig**: alle COMMANDO-velden gewist (substitutie uit, `ch_off` uit, override uit, lamp auto, `max_mod` 0, `boiler_sp` 0, K3-vrijgave uit, keteltype terug naar dip), alle `override_to_thermostat`-antwoorden gewist, **rem aan met 10 C, direct** (houdtijd telt niet). Staat de rem al aan: blijft. |
| `esp_weg_s` (60 s) | ESP "weg": VRAAG_ESP-regels doen de standaardactie, tabel terug naar de ingebouwde standaard, HELLO elke 5 s |
| `nood_s` (240 s) | **noodbrug** K1+K2 aan (alleen als het keteltype OpenTherm-ketel is); bij een aan/uit-ketel volgt K3 het setpoint van de thermostaat (ongeremd; origineel "boiler-modus"), tenzij gebouwd met `NOOD_K3_VOLGT = 0` |
| elk geldig ESP-bericht | tellers opnieuw; noodbrug los, K3 terug naar normaal |

Is het beleid ongeldig, dan blijft de ketel dus **geremd**, ook na 60 s. (Het ontwerp zegt op twee plekken iets anders, zie hoofdstuk 11.)

### 7.2 Wat een ongeldig beleid wist
COMMANDO-velden (substitutie, `ch_off`, `test_out`, `max_mod`, `override`, lamp, zelfstandige stand, `boiler_sp`, K3-vrijgave, `noodbrug aan op verzoek`, keteltype-override) en alle
override-antwoorden (`ovr[]`, uit INJECT modus 1). De **polariteits- en kanaalvlaggen** (`inv_rx_*`, `inv_tx_*`, `swap`, `lamp_inv`) blijven staan zoals de ESP ze het laatst zette
(zoals in v1): het zijn hardware-aannames en geen beleid, en terugzetten zou het gesprek met de thermostaat kunnen breken. Na een reset staan ze op normaal.

### 7.3 De rem en de houdtijd
Gewenst (uit het geldige COMMANDO) is: rem aan/uit en een setpoint. Toegepast is wat de AVR werkelijk gebruikt. Een verschil wordt pas toegepast als de **vorige wissel
`rem_hold_s` geleden** is. Een wissel is een verandering van de vlag of, bij rem aan, van het setpoint (**10 <-> 35 C pendelt dus niet**). Uitzonderingen, direct toegepast:
(a) de eerste wissel na de start; (b) `flags2.5` (rem direct) in het COMMANDO, voor storingen; (c) terugval naar rem aan bij ongeldig beleid.

### 7.4 Relais
K1+K2 (noodbrug, PB2), K3 (PC5), allebei in dezelfde code (`relais`):
* noodbrug aan = (keteltype OpenTherm) en (noodtijd verstreken, of geldig COMMANDO met `flags1.4`);
* K3 aan = (keteltype aan/uit) en niet de noodbrug en ((geldig COMMANDO met `flags2.4` en rem-setpoint/setpoint >= 20 C), of (noodtijd verstreken, `NOOD_K3_VOLGT`, setpoint van de thermostaat >= 20 C));
* **nooit samen:** wisselt de ene aan terwijl de ander aan staat, dan gaat de andere eerst uit en komt de nieuwe pas 50 ms later aan. Bij een conflict wint de noodbrug.
* Terwijl de noodbrug aan staat zendt de AVR **niets** op de OpenTherm-lijnen (hij luistert wel), zodat hij nooit botst met het rechtstreekse gesprek.

## 8. Alle grenzen op een rij (een waarde buiten de grenzen = bericht afgewezen)

| Waarde | Grenzen | Standaard |
|---|---|---|
| deadline_ms | 30..600 | 250 |
| ketel_ms | 100..600 | 400 |
| esp_weg_s | 10..600 | 60 |
| nood_s | 60..1800, >= esp_weg_s | 240 |
| rem_hold_s | 60..900 | 300 |
| regels n | 0..16 | 10 |
| regel: richting | 0 of 1 | |
| regel: modus-bits 1..0 | 0..3 | |
| regel: conditie | 0..6 | |
| regel: deadline (p0 bij VRAAG_ESP) | 0..150 (x 4 ms) | 0 |
| regel: standaardactie (p1 bij VRAAG_ESP) | 0 of 1 | |
| regel: BLOKKEER-type (p1) | 0, 6, 7 | |
| regel: p0, p1 bij PASS | 0 | |
| regel: vlaggen buiten VAST | 0 | |
| regel: MASKER alleen T>K; ECHO alleen K>T | | |
| COMMANDO brake_sp, boiler_sp | 0..90 | |
| COMMANDO wp_supply, wp_return | -300..1500 | |
| COMMANDO override | 0 of 50..300 | |
| COMMANDO lamp | 0..4 | |
| COMMANDO sa_mod, max_mod | 0..100 | |
| COMMANDO test_out | 0..31 | |
| COMMANDO geldig_s | 0 of 10..255 | 0 |
| COMMANDO keteltype | 0..2 | 0 |
| INJECT modus | 0..2 | |
| `BUDGET_MS` (antwoord aan thermostaat) | vast 700 | |
| pauze tussen berichten | >= 5 ms | |
| tussen twee bytes van een bericht | < 5 ms | |

## 9. Handshake (herstart-synchronisatie)

1. AVR start: HELLO (versie, protocol, tabel-CRC, resetreden, kenmerken). Elke 2 s herhalen zolang er geen geldig ESP-bericht is.
2. ESP: ziet HELLO (of een STATUS). Verschilt `tabel_crc` van de CRC van zijn eigen tabel, dan stuurt hij TABEL. De AVR controleert, neemt over tussen twee gesprekken
   en stuurt ACK (status 0), of ACK met status 2 (afgewezen: de ESP meldt dat in HA).
3. ESP: stuurt daarna COMMANDO (elke 2 s). Daarmee is het beleid geldig.
4. ESP start later dan de AVR: de AVR stuurt HELLO en STATUS elke 2 s; de ESP stuurt zodra hij het eerste bericht hoort zijn TABEL en COMMANDO.
5. De AVR valt terug op de standaardtabel na `esp_weg_s` en meldt dat met een HELLO (met zijn standaard-CRC); de ESP stuurt dan zijn tabel opnieuw.
6. Een **oude AVR (v1)** stuurt geen HELLO maar `0xF0`-berichten van 9600 baud; de ESP herkent dat aan het uitblijven van HELLO en gebruikt dan de v1-frames. (ESP-kant, buiten deze map.)

## 10. Statistieken en weergave in de ESP
De ESP kan uit STATUS afleiden: `crc_fouten`, `uart_overruns`, `vraag_verlopen`, `ketel_oud`, `ev_gemist`, `isr_overruns` (hoort 0 te zijn) en de tabel-CRC.

## 11. Afwijkingen van en invullingen bij het ontwerp

1. **EVENT en INJECT** zijn toegevoegd (de ESP gebruikt ze; zonder zouden `get_ot`, `write_boiler` e.d. verdwijnen).
2. **Terugval en "rem loslaten".** Het ontwerp (hoofdstuk 3.5 en de tekst van de opdracht) zegt: bij ESP-stilte de veilige standaard (alles PASS, rem aan, CH-vlag onderdrukt).
   Hoofdstuk 6 van het ontwerp zegt "`ovr[]` laten vervallen als het beleid ongeldig wordt, en de rem loslaten". Gekozen: **de rem blijft aan** (geremd) bij ongeldig beleid;
   "loslaten" is uitgelegd als: de houdtijd en het onthouden van een eerder toegepaste rem laten los, niet de ketel vrijgeven. Een rem die al aan staat blijft aan,
   een rem die uit stond gaat direct aan. Dat sluit aan bij "bij onzekerheid blijft de ketel geremd". v1 deed het omgekeerd (ketel vrij bij ESP-stilte).
3. **Tabelregel met conditie en vlaggen.** De regel is 5 bytes zoals in het ontwerp; de modus-byte draagt daarnaast de conditie (rem, substitutie, override, ch_off, max_mod) en de vlaggen
   MASKER/FORCEER/ECHO, zodat ID 0, 1, 16 gekoppeld aan de rem kunnen zijn en de v1-substituties (ID 14, 25, 28, 9, 100) tabelregels worden.
4. **Rem-houdtijd** is een vijfde instelbare tijd in de kop (60..900 s), naast de vier uit het ontwerp.
5. **Berichtgroottes** wijken af van de schattingen in het ontwerp (HELLO 19 bytes i.p.v. 14, COMMANDO 20, STATUS 38, TABEL 19..99) vanwege de volledige v1-inhoud.
6. **K3 in de noodstand** bij een aan/uit-ketel (volgt het thermostaat-setpoint na `nood_s`, zoals de "boiler-modus" van het origineel): dit staat niet in het ontwerp en is
   achter `NOOD_K3_VOLGT` gezet (standaard 1).
7. **DHW** in regel 1 (DHW-enable aan): zo staat het in het ontwerp ("CH-vlag eraf, DHW") en zo doet het origineel, maar `volledige-controlbox.md` adviseert "raak DHW niet aan".
   Instelbaar met `DHW_AAN_STANDAARD` bij het bouwen en per regel via de tabel.
8. `relay_follows` vervalt (zie 4.7).

## 12. Voorbeelden (hexdumps)

Gemaakt met `python -B tools/protocol_ref.py --dump` (onafhankelijke encoder). Volgnummers zijn willekeurig gekozen.

**HELLO (AVR 2.0.0, build 2026-10-26, protocol 2, standaardtabel, spanning aan)** (19 bytes)

```
A5 01 0E 10 02 00 00 1A 0A 1A 02 9F D3 01 00 00 1E 10 CA
```

**TABEL (standaardtabel, 10 regels, versie 1, tijden standaard)** (69 bytes)

```
A5 02 40 21 01 00 FA 01 90 00 3C 00 F0 01 2C 0A 00 00 35 01 02 01 00 05 FF FF 10 00 45 08 00 0E 00 19 FF FB 01 01 85 00 00 10 01 85 00 00 19 01 09 FF FE 1C 01 09 FF FD 09 01 4D FF FC 64 01 4D 00 00 E4 0F 6C
```

**TABEL (klein: een VRAAG_ESP-regel op ID 27 K>T + blokkeer ID 5 T>K, deadline 100 ms)** (29 bytes)

```
A5 02 18 22 02 00 C8 01 90 00 3C 00 F0 01 2C 02 1B 01 02 19 00 05 00 03 00 07 B6 A8 DC
```

**ACK (op TABEL volgnr 0x21, status 0 = OK)** (7 bytes)

```
A5 03 02 11 21 00 F8
```

**ACK (op TABEL volgnr 0x22, status 2 = buiten de grenzen)** (7 bytes)

```
A5 03 02 12 22 02 74
```

**VRAAG (richting T>K, Write-Data ID 1 = 45,0 C -> frame 10012D00)** (10 bytes)

```
A5 04 05 13 00 10 01 2D 00 48
```

**ANTWOORD DOORGEVEN (op VRAAG 0x13)** (11 bytes)

```
A5 05 06 24 13 00 00 00 00 00 C1
```

**ANTWOORD VERVANGEN (op VRAAG 0x13, nieuw frame 10010A00 = Write-Data ID 1 = 10,0 C)** (11 bytes)

```
A5 05 06 25 13 01 10 01 0A 00 F2
```

**ANTWOORD BLOKKEREN (op VRAAG 0x13)** (11 bytes)

```
A5 05 06 26 13 02 00 00 00 00 BC
```

**STATUS (voorbeeldwaarden)** (38 bytes)

```
A5 06 21 14 03 0A 2D 00 CD 00 D2 01 63 1E 00 1F 03 28 26 05 0F 00 00 07 01 00 12 34 01 0A 00 00 00 00 00 03 00 BD
```

**COMMANDO (rem aan, 10 C, verder niets)** (20 bytes)

```
A5 07 0F 27 01 00 0A 00 00 00 00 00 00 00 00 00 00 00 00 E4
```

**COMMANDO (rem uit, substitutie aan, aanvoer 35,5 C, retour 29,0 C, ketel-setpoint 0, geldigheid 30 s)** (20 bytes)

```
A5 07 0F 28 02 00 0A 01 63 01 22 00 00 00 00 00 00 00 1E AF
```

**EVENT (richting FROM_STAT, frame 10012D00)** (10 bytes)

```
A5 08 05 15 00 10 01 2D 00 6A
```

**INJECT naar de ketel (Write-Data ID 27 = 8,5 C)** (10 bytes)

```
A5 09 05 29 00 90 1B 08 80 45
```

**INJECT override antwoord (ID 3 = 0x0100)** (10 bytes)

```
A5 09 05 2A 01 40 03 01 00 56
```

**INJECT override opheffen (ID 3)** (10 bytes)

```
A5 09 05 2B 02 C0 03 00 00 FD
```

Tabel-CRC-16 van de ingebouwde standaardtabel: **0x9FD3**. Controlewaarden: CRC-8("123456789") = 0xF4, CRC-16("123456789") = 0x29B1.
