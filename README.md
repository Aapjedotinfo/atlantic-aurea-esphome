# Aurea WP — ESP32/ESPHome-controller voor de Atlantic Aurea

Alternatieve besturing voor de **Atlantic Aurea 5 Hybrid** warmtepomp (een
kostenbespaarde **Chofu AEYC-0643XU**). Een ESP32 vervangt de linker
microcontroller in de controlbox en praat rechtstreeks met de buitenunit op het
eigen 666-baud protocol. Je zegt vanuit **Home Assistant** simpelweg *"geef me X°C
aanvoer"* — de controller kiest zelf de stand (0–7). De CV-ketel stuur je los aan.

**Werkt** — bidirectionele communicatie is live geverifieerd op echte hardware:
alle telegrammen CRC-geldig, temperaturen/vermogen/compressor uitgelezen én de
buitenunit reageert op de gestuurde stand.

## Herkomst van het protocol

Het protocol is niet openbaar; het is gereverse-engineerd door de forumgebruikers
**WackoH** en **_JGC_** in het Tweakers-topic *"Aurea 5 hybrid: interfaces met de
buitenunit en thermostaat"*. De protocol-laag hier is geport uit JGC's werkende
ATmega2560-sketch (v0.1Beta, 19-03-2025). Alle credits voor het uitpluizen gaan
naar hen; dit project giet het in een ESPHome-component met een simpele regeling.

## Het protocol

| Aspect | Waarde |
|--------|--------|
| Fysiek | 666 baud, 8N1, via 2× H11D1 optocoupler |
| Rollen | **Buitenunit = master** (~500 ms cyclus, ~8 ms startpuls) |
| CRC | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), big-endian |
| Timing | ESP zendt ~99 ms ná elk ontvangen frame, min. 300 ms tussen TX |

**Zenden (controlbox → WP):** 4 roterende telegrammen. Alleen `data2` verandert;
de rest zijn vaste constanten:

```
data0 = 19 00 08 00 00 00 D9 B5
data1 = 19 01 0C 00 00 00 00 00 00 00 AA 35
data2 = 19 02 08 <modus> <stand> 00 <crc_hi> <crc_lo>   ← het commando
data3 = 19 03 08 B2 02 00 C1 9A
```
`data2` byte 4 = **modus**, byte 5 = **stand** (0–7):

| Modus | Betekenis |
|-------|-----------|
| `0x00` | uit |
| `0x01` | verwarmen |
| `0x02` | **koelen** — ✅ geverifieerd werkend |

> **Koelen werkt echt.** Op het forum stond `0x02` als onbevestigd en JGC's sketch
> gebruikte het nooit. Getest op echte hardware: na het commando startte de
> compressor binnen ~6 s en zakte de aanvoer van 37,8 °C naar 23,8 °C, terwijl de
> retour op 30,0 °C bleef — delta-T van −6,3 °C, dus de aanvoer werd kouder dan de
> retour. Dat is onmiskenbaar koelbedrijf.

**Ontvangen (WP → controlbox):** `91 <ID> <len> <data…> <crc_hi> <crc_lo>` — géén
aparte end-byte; na de CRC volgt meteen het volgende telegram. Een frame is geldig
als CRC-CCITT over het **hele** frame `0` is. Berekend per bericht-ID (0–3):

- **ID2** — temperaturen, signed 16-bit **little-endian**, /10:
  byte 3-4 = aanvoer, 5-6 = retour, 7-8 = buiten (bv. `ED 00` = 23.7 °C).
  Let op: ID2 wisselt af tussen twee payload-varianten; de temperatuur-bytes
  staan in beide op dezelfde plek, de staart erna verschilt (nog niet ontcijferd).
- **ID3** — byte 9 = compressorsnelheid, byte 10 = vermogen (W = 25.6 × byte).
  Het vermogen is geijkt tegen een Shelly PM: byte `0x1A` → 25.6 × 26 = 666 W
  tegenover 682 W gemeten (~2 % afwijking).
- **ID1** — nog niet ontcijferd. Byte 4 leek defrost maar blijkt gewoon een
  bedrijfsstatus (0x01 zodra de unit draait), dus die sensor is verwijderd.

### Timing is kritisch

De buitenunit is master en verdraagt geen verkeer terwijl hij zendt. Zend daarom
**alleen als de lijn stil is**: minimaal 40 ms geen byte ontvangen (bij 666 baud
duurt één byte ~15 ms). Vertrouw daarbij niet op je parser-state — die kan
ontsporen op een corrupt frame, waarna je dwars door de warmtepomp heen gaat
zenden en álle lange frames sloopt. Je herkent dat probleem aan `0x19`-bytes
(je eigen telegram) middenin een ontvangen frame.

## Regeling

Bewust simpel: **"geef me X°C aanvoer" → de controller kiest zelf stand 0–7**,
op basis van de aanvoerfout én de **delta-T** (aanvoer − retour).

| Aanvoer t.o.v. setpoint | Delta-T | Actie |
|---|---|---|
| Te koud | laag (< dt_high) | stand **omhoog** |
| Te koud | al hoog (≥ dt_high) | **houden** — retour warmt vanzelf op |
| Rond setpoint | hoog (> dt_high) | stand **omlaag** |
| Rond setpoint | in band | **houden** |
| Te warm | — | stand **omlaag** |

Delta-T is hier bewust een **rem**, geen gaspedaal: bij een koude start is delta-T
van nature hoog, en dan nóg meer vermogen geven zou overschieten. Daarbovenop
**stap-modulatie** (max ±1 stand per interval) zodat de compressor niet volgas
gaat. Geen stooklijn.

Bij **koelen** keert de regeling om: dan is de fout `aanvoer − setpoint` en de
delta-T `retour − aanvoer`, waardoor dezelfde tabel blijft gelden. Het
stap-interval is instelbaar in Home Assistant — 120 s is rustig en
compressorvriendelijk voor verwarmen, maar bij koelen werkt die traagheid je
tegen (je schiet voorbij het setpoint); 30–45 s regelt daar strakker.

**Setpoint-bereik: 6,5 – 60 °C**, het volledige technische bereik uit de
Chofu-datasheet (*Operating Range / Leaving Water Temperature*: koelen `6.5℃~`,
verwarmen `~60℃`). Atlantic noemt 55 °C, maar dat is hun overdrachtspunt naar de
gasketel — geen hardwarelimiet.

Overige parameters (`dt_low`, `dt_high`, `max_stand`, `setpoint_min/max`,
`cooling_min_supply`) staan als opties in `aurea-wp.yaml` onder `chofu_wp:`.

### Entiteiten in Home Assistant / web-GUI

| Entiteit | Type | Functie |
|----------|------|---------|
| Setpoint aanvoer | number 6,5–60 °C | primaire knop — gewenste aanvoertemperatuur |
| Modus | select | auto (regeling) / handmatig |
| Handmatige stand | number 0–7 | directe stand in modus handmatig |
| Stap-interval | number 10–600 s | hoe snel de regeling van stand mag wisselen |
| Systeem | switch | master aan/uit |
| Koelen | switch | koelbedrijf i.p.v. verwarmen (uit na reboot) |
| Debug frames | switch | ruwe FRAME-hexlog aan/uit (standaard uit) |
| Aanvoer / Retour / Buiten / Delta T | sensor | temperaturen |
| Vermogen | sensor (W) | echt vermogen uit de WP |
| Compressor / Stand | sensor | draaisnelheid WP / door ons gestuurde stand |
| Actief / Communicatie | binary_sensor | status |

Fail-safe: >60 s zonder geldig telegram → terug naar stand 0.

## Structuur

```
aurea-wp-666/
├── aurea-wp.yaml           device-config (entities, wifi, ota)
├── secrets.yaml.example    kopieer naar secrets.yaml en vul in
├── WIRING.md               bedrading + ASCII-schema
├── README.md
└── components/
    └── chofu_wp/
        ├── __init__.py     protocol- + regelparameters
        ├── chofu_wp.h
        └── chofu_wp.cpp    protocol + simpele regeling
```

## Aan de slag

1. **Bedrading:** zie [WIRING.md](WIRING.md). Kort: ESP32 in de IC1-socket,
   GPIO17→pin 26 (TX), pin 27→GPIO16 (RX, met 10k naar GND), 5V/GND. Geen
   levelshifter nodig — de optocouplers doen de scheiding al.
2. **Secrets:** `cp secrets.yaml.example secrets.yaml` en vul je wifi + een
   API-sleutel in.
3. **Flashen:** eerste keer via USB, daarna OTA:
   ```
   esphome run aurea-wp.yaml
   ```
   Of importeer in de Home Assistant **ESPHome Device Builder**: kopieer
   `aurea-wp.yaml` + de map `components/` naar de addon-configmap en vul de keys
   (`wifi_ssid`, `wifi_password`, `api_encryption`, `fallback_password`) in je
   `secrets.yaml` aan.

## Diagnose

Zet de **"Debug frames"**-switch aan (web-GUI of Home Assistant) om elk ontvangen
frame als hex te loggen met CRC-resultaat:

```
FRAME ID2 [19] 91 02 12 ED 00 E1 00 C6 00 ... crc=0000 OK
```

Zo zie je meteen of er schone telegrammen binnenkomen. Laat 'm normaal uit voor een
rustige log.

## ⚠️ Disclaimer

Je past hardware in je warmtepomp-controlbox aan die op netspanning zit en
(mogelijk) onder garantie valt. Doe dit alleen als je weet wat je doet, en op
eigen risico. Dit project is niet gelieerd aan of goedgekeurd door Atlantic/Chofu.
Het protocol is reverse-engineered en kan per model/firmware verschillen.

**Koelen:** een CV-systeem is meestal niet geïsoleerd tegen koud water. Onder het
dauwpunt gaan leidingen en radiatoren **condenseren** — met kans op waterschade.
Houd het setpoint hoog genoeg (vuistregel: boven ~17 °C) of isoleer je leidingen.

## Credits

- **WackoH** en **_JGC_** (Tweakers) — reverse-engineering van het protocol.
- Regeling, ESPHome-component en documentatie: dit project.
