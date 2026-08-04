# Print doormeten: wat we moeten weten en waarom

Werklijst voor de controlbox-print op tafel. **Chips uit beide sockets**, meter
op doorbel/weerstand. Zo kan geen enkele halfgeleider je meting vervuilen.

De volgorde is niet willekeurig: bovenaan staat wat de huidige blokkade
verklaart, onderaan wat alleen maar netjes is om te weten.

Stand van zaken op het moment van schrijven:

- De AVR in IC2 draait, de link met de ESP32 werkt in beide richtingen.
- Socket-pin 13 heeft aantoonbaar OpenTherm binnengekregen (124 flanken).
- **Met het relais uit staat er geen 18 V op de thermostaatklem.** Daardoor
  krijgt geen enkele thermostaat spanning en zwijgt hij.
- Met het relais aan werkt de thermostaat wel, maar dan voedt de *ketel* de
  bus en staat de controlbox buitenspel &mdash; dat is de noodbrug.

---

## 1. De 18 V-tak richting de thermostaat

Dit is de blokkade. De controlbox hoort zich naar de thermostaat toe als ketel
te gedragen, en een ketel levert de busspanning.

| # | Meten | Waarom het uitmaakt |
|---|---|---|
| 1.1 | Waar komt de 18 V vandaan? Zoek de gelijkrichter/regelaar die op de trafo zit, los van de LM7805. WackoH wijst die tak aan op zijn geannoteerde printfoto. | Zonder bron is er niets te schakelen |
| 1.2 | **Staat er 18 V op die rail zelf** (kast onder spanning)? | Rail dood = voeding stuk, zekering, of gelijkrichter. Rail leeft = het zit verderop |
| 1.3 | Loopt er een **zekering** in die tak? Doorbellen. | Meest banale oorzaak, meest over het hoofd gezien |
| 1.4 | Volg de rail naar de **thermostaatklem**. Wat zit ertussen? Weerstand, transistor, opto? | Zit er een transistor tussen, dan wordt die door iets aangestuurd &mdash; en dat is onze verdachte |
| 1.5 | Als er een transistor/opto tussen zit: **waar gaat zijn basis/ingang heen?** | Dit is de kern. Komt hij op een socket-pin uit, dan hebben wij hem in de hand |

> Uitkomst 1.5 is waar het om draait. Komt die stuurpin uit op IC2 pin 6, dan
> is het onze zendlijn en hebben we simpelweg de polariteit verkeerd. Komt hij
> uit op een van de vijf pinnen die het origineel altijd laag houdt (4, 14, 15,
> 17, 18), dan is het een enable en zetten we hem aan.

---

## 2. IC2-socket: de vier OpenTherm-pinnen

Onze rolverdeling volgt uit de bufferadressen in de originele firmware.
WackoH's pintabel zegt iets anders. Dit beslecht het definitief.

| # | Socket-pin | AVR | Wij denken | Meten |
|---|---|---|---|---|
| 2.1 | **6** | PD4 | zenden naar thermostaat | Waar komt hij uit? Op de thermostaatklem, via welke trap? |
| 2.2 | **13** | PD7 | ontvangen van thermostaat | Volg terug naar de klem. **Noteer de weerstandswaarden van de deler** |
| 2.3 | **5** | PD3 | zenden naar ketel | Idem, richting ketelklem |
| 2.4 | **11** | PD5 | ontvangen van ketel | Idem. Digitaal gelezen, dus hier hoort geen deler naar een comparator |

Punt 2.2 is extra interessant: het origineel leest die pin via de analoge
comparator tegen 1,23 V. De deelverhouding vertelt je welk busniveau daar
onder of boven uitkomt, en dus wat rust is.

---

## 3. Het relais op pin 16

We weten inmiddels wat het *doet* (noodbrug bij een dode hoofdchip), maar niet
wat het fysiek schakelt.

| # | Meten |
|---|---|
| 3.1 | Waar gaat pin 16 heen? Rechtstreeks naar een relaisspoel of via een transistor? |
| 3.2 | **Welke contacten heeft dat relais en wat verbinden ze?** Doorbellen in beide standen |
| 3.3 | Verbindt het in aangetrokken stand de thermostaatklemmen rechtstreeks met de ketelklemmen? |

3.3 is de bevestiging van de noodbrug-theorie. Klopt dat, dan is het beeld
compleet.

---

## 4. De mode-ingang op pin 25

In het origineel knijpt PC2 de hele ketelkant af: geen zenden naar de ketel
(`0x0896`), geen doorgeven (`0x053e`), geen relais (`0x0a94`). **Laag is de
normale stand**, en wij lezen hem hoog.

| # | Meten |
|---|---|
| 4.1 | Waar gaat pin 25 heen? Naar een klem, een jumper, een schakelaar, of nergens? |
| 4.2 | Zit er een externe pull-up of pull-down op? |
| 4.3 | Loopt er een verbinding naar de IC1-socket? |

Er zit geen knop op, dus er moet iets anders zijn dat hem in normaal bedrijf
laag trekt &mdash; anders had die controlbox het nooit gedaan.

---

## 5. De vijf pinnen die het origineel nooit schrijft

Uitgang, permanent laag, nergens beschreven. Kandidaten voor een enable.

| # | Socket-pin | AVR | Meten |
|---|---|---|---|
| 5.1 | 4 | PD2 | Gaat hij ergens heen, of eindigt het spoor? |
| 5.2 | 14 | PB0 | idem |
| 5.3 | 15 | PB1 | idem |
| 5.4 | 17 | PB3 | idem |
| 5.5 | 18 | PB4 | idem |

Eindigt een spoor blind, dan is die pin af. Komt er een uit bij de 18 V-tak,
dan hebben we onze enable.

---

## 6. De klemmen

| # | Meten |
|---|---|
| 6.1 | Welk klemmenblok is de **thermostaat** en welk de **ketel**? |
| 6.2 | Is er een apart **emergency**-blok, en waar gaat dat heen? |
| 6.3 | Hangen de iSense en de emulator nu op gescheiden paren of op hetzelfde? |

Op WackoH's schema staan de blokken *SMILE &harr; ANNA* en
*EMERGENCY: SMILE &harr; BOILER*.

---

## 7. IC1-socket, voor later

Niet blokkerend, maar dit is het moment om het mee te nemen.

| # | Socket-pin | AVR | Wij denken |
|---|---|---|---|
| 7.1 | 23 | PC0 | CV-only-schakelaar op de voorkant, actief bij hoog |
| 7.2 | 24 | PC1 | tactile testknop op de print, actief bij laag |
| 7.3 | 15 | PB1 | storingsuitgang, simpel aan/uit |
| 7.4 | 14 | PB0 | vraagrelais |
| 7.5 | 17 | PB3 | tweede bitstroom, functie nooit opgehelderd |

Zit er op 23 en 24 een **externe pull-up naar 5 V**? Dat bepaalt of die
knoppen straks rechtstreeks op een ESP32-pin mogen of via een deler moeten.

---

## Wat we hiermee oplossen

Punt 1.5 verklaart naar verwachting de hele blokkade. Punt 2 sluit de laatste
discussie met WackoH's pintabel. Punt 3.3 bevestigt de noodbrug. De rest is
opruimen.

---

## Gemeten

Bijgewerkt terwijl de print op tafel ligt. Alleen wat werkelijk met de meter is
vastgesteld; afleidingen staan als zodanig gemarkeerd.

### Onderdelen

| Ref | Type | Rol |
|---|---|---|
| OK1, OK2 | H11D1 | warmtepomp-interface, ontvangen en zenden |
| OK5 | TLP521-2 | dubbele optocoupler, **&eacute;&eacute;n OT-bus in beide richtingen** |
| Q1&ndash;Q4 | BC558A | PNP, dus hoogzijdeschakelaars |
| T1&ndash;T4 | BC547A | NPN, laagzijdedrivers |
| K1, K2, K3 | relais | K3 = de omschakeling naar CV-only |
| &mdash; | LM7818 | de 18 V-tak voor OpenTherm |
| &mdash; | LM7805 | 5 V-logica |

### IC2-socket

| Socket-pin | AVR | Gemeten |
|---|---|---|
| 1 | RESET | via R19 (5k) naar +5 V &mdash; pull-up |
| 5 | PD3 | via R13 (330 &ohm;) naar OK5 pin 3 = anode LED kanaal 2. **Zendpad naar de ketel**, ~11 mA |
| 16 | PB2 | via R3 (33k) naar K1; daarnaast via R4 (4k7) naar massa (pull-down) |
| 21 | AREF | aan +5 V |
| 27 | PC4 | stuurt een status-LED aan |
| 28 | PC5 | via weerstand naar de basis van T4 |

### Optocouplers

| Punt | Gemeten | Betekenis |
|---|---|---|
| OK5 pin 8 | +5 V | collector kanaal 1 &rarr; **kanaal 1 is ontvangst**, emitter levert het signaal |
| OK5 pin 3 | van PD3 via R13 | **kanaal 2 is zenden** |
| OK1 pin 5 | +5 V | collector; klopt met `WIRING.md` |
| OK2 pin 1 | +5 V | LED-anode; klopt met `WIRING.md` |

### K3 en de CV-only-schakelaar

```
PC5 ── R ── basis T4
            collector ── spoel K3
            emitter   ── frontschakelaar ── massa
```

K3 trekt dus alleen aan als **PC5 hoog is &eacute;n de schakelaar dicht staat**. PC5 gaat
hoog vanaf een ketelsetpoint van 20 &deg;C (`0944 CPI 0x01` / `094c CPI 0x14`), dus dit
is een handmatige CV-only-stand die pas schakelt bij werkelijke warmtevraag.

> **Gevolg voor onze firmware:** blind het origineel nadoen op PC5 betekent dat
> we onszelf overbruggen zodra er vraag komt. PC5 moet instelbaar zijn.

### K1

Contactset 1: NC zit op de OpenTherm-verbinding naar de ketel. NO is nog niet
gevolgd. **De common is nog onbekend** &mdash; dat is de meting die de rol van K1
vastlegt.

### Nog open

- Waar gaat OK5 pin 4 heen (kathode LED 2)? Bepaalt de zendpolariteit definitief
- Komt OK5 pin 7 uit op socket-pin 11? Zou de ketel-ontvangst bevestigen
- Welke optocoupler doet de thermostaatkant (PD4 en pin 13)?
- Wat zit er tussen de LM7818 en de thermostaatklem?
- Waar komt de common van K1 uit?
- Wat stuurt K2 aan?
