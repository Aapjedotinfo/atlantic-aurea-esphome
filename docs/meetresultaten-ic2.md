# Meetresultaten IC2-socket

Doorgemeten met de print op tafel en beide chips uit de voet. Alleen wat met de
meter is vastgesteld. Afleidingen uit de firmware staan in
[print-doormeten.md](print-doormeten.md) en in het
[reverse-engineering-verslag](../../MCU's%20Atlantic/index.html).

ATmega8 in DIP28, "Haddon T1.1".

## Pinnen

| Pin | AVR | Gemeten | Status |
|---|---|---|---|
| 1 | RESET | R19 (5k) naar +5 V | pull-up, verder niets |
| 2 | PD0 | naar de IC1-voet, kaal koper | interconnect RX |
| 3 | PD1 | naar de IC1-voet, kaal koper | interconnect TX |
| 4 | PD2 | niet aangesloten | &mdash; |
| 5 | PD3 | R13 (330 &ohm;) naar OK5 pin 3 | zendpad, LED-anode kanaal 2 |
| 6 | PD4 | R24 (4k7) naar de emitter van T1 | T1 in gemeenschappelijke basis |
| 7 | VCC | | |
| 8 | GND | | |
| 9 | XTAL1 | 8 MHz-kristal | bevestigd |
| 10 | XTAL2 | 8 MHz-kristal | bevestigd |
| 11 | PD5 | OK5 pin 7 = emitter kanaal 1 | ontvangst, zoals voorspeld |
| 12 | PD6 | niet aangesloten | |
| 13 | PD7 | R3 (33k); ook R4 (4k7) naar massa | AIN1, deler naar de comparator |
| 14 | PB0 | niet aangesloten | |
| 15 | PB1 | niet aangesloten | |
| 16 | PB2 | diode, dan R26 (10k) naar de basis van T3 | T3 schakelt K1 **en** K2 tegelijk |
| 17 | PB3 | niet aangesloten | &mdash; |
| 18 | PB4 | niet aangesloten | &mdash; |
| 19 | PB5 | | |
| 20 | AVCC | ook naar de basis van T1 | |
| 21 | AREF | +5 V | |
| 22 | GND | | |
| 23 | PC0 | | |
| 24 | PC1 | | |
| 25 | PC2 | dipswitch pin 2; dip pin 4 (het andere contact) naar massa | **ON = PC2 laag = normaal bedrijf. OFF = PC2 hoog = ketelkant afgeknepen.** Stond OFF tijdens de eerste tests |
| 26 | PC3 | | |
| 27 | PC4 | status-LED | |
| 28 | PC5 | weerstand naar T4 | T4 stuurt K3 |

## IC1-socket (voor zover meegenomen)

| Pin | AVR | Gemeten |
|---|---|---|
| 14 | PB0 | R21 (10k) naar de status-LED |

## Onderdelen

| Ref | Type | Wat we ervan weten |
|---|---|---|
| OK1 | H11D1 | warmtepomp, ontvangen. Pin 5 (collector) aan +5 V |
| OK2 | H11D1 | warmtepomp, zenden. Pin 1 (LED-anode) aan +5 V |
| OK5 | TLP521-2 | pin 8 (collector 1) aan +5 V &rarr; kanaal 1 is ontvangst; pin 3 is de zend-LED |
| Q1&ndash;Q4 | BC558A | PNP, hoogzijde |
| T1&ndash;T4 | BC547A | NPN, laagzijde. T2 is de warmtepomp-zendlijn vanaf IC1 pin 26 |
| K1 | relais, 24 VDC | spoel aan T3 (pin 16). Contactset 1: NC op de OT-verbinding naar de ketel. COM nog onbekend |
| K2 | relais, 24 VDC | spoel aan T3, schakelt samen met K1 vanaf pin 16. Samen verleggen ze het aderpaar &rarr; **dit is de noodbrug** |
| K3 | Omron G2R-1, 12 VDC | 10 A wisselcontact, dus vermogen en geen signaal. Spoel via T4 vanaf PC5, frontschakelaar in serie in de emitter &rarr; **warmtevraag-contact voor een aan/uit-ketel**, niet de noodbrug |
| LM7818 | | 18 V-tak voor OpenTherm; voedt ook de LM7805 |
| LM7805 | | 5 V-logica, ingang komt van de uitgang van de LM7818 |

## Bedrading buiten de socket

| Van | Naar | Via |
|---|---|---|
| PC5 (pin 28) | basis T4 | weerstand |
| PD7 (pin 13) | R3 (33k) en R4 (4k7) naar massa | deler 33k/4k7 &rarr; 18 V wordt 2,2 V |
| PB2 (pin 16) | diode &rarr; R26 (10k) &rarr; basis T3 | |
| T3 collector | spoelen van **K1 en K2** | beide relais aan &eacute;&eacute;n driver |
| +18 V | R27 (100 &ohm;) naar de relaisspoelen | gedeelde voorschakelweerstand |
| R27 | spoel K3, andere kant | K3 hangt dus ook aan diezelfde 18 V via R27 |
| PD4 (pin 6) | T1 emitter | R24 (4k7) |
| T1 basis | pin 20 (AVCC) | |
| T1 collector | basis Q4 | |
| Q4 emitter | Q1 emitter | |
| Q1 emitter | uitgang LM7818 (+18 V) | |
| uitgang LM7818 | ingang LM7805 | dus 18 V leeft altijd als de 5 V leeft |
| Q1 emitter / +18 V | C5 (100 nF, opdruk 104) | ontkoppeling |
| C5 | R1 (33k) | andere kant van R1 gaat naar de collector van Q1 |
| C5 | pin 8 (GND) | |
| R1 (33k) | collector Q1 | |
| OK5 pin 7 (emitter kanaal 1) | R18 (10k) naar massa | emitterweerstand, tevens pull-down |
| T4 collector | spoel K3 | bevestigd |
| T4 emitter | frontschakelaar | daarna massa |

---

## Voorlopige conclusies

### Bevestigd

- **PD3/PD5 = ketelkant (master), PD4/PD7 = thermostaatkant (slave).** Dubbel
  bewezen: uit de firmware (bufferadressen) en nu uit het koper. WackoH's
  "pin 13 = massa" is definitief onjuist voor deze print.
- **K1 + K2 samen zijn de noodbrug**, aangestuurd vanaf PB2 via T3. Verlegt het
  hele aderpaar van de thermostaat rechtstreeks naar de ketel zodra de
  hoofdchip (of bij ons: de ESP32) niet meer rapporteert.
- **K3 is een aparte functie**: een droog 10 A-contact voor een aan/uit-ketel,
  aangestuurd vanaf PC5 via T4 met de frontschakelaar in serie. Dit is
  Atlantic's tweede ondersteunde keteltype naast OpenTherm.
- **K3 en K1/K2 zijn niet bedoeld om samen aan te staan.** Ze delen &eacute;&eacute;n
  voorschakelweerstand (R27, 100 &ohm;) vanaf +18 V; met alle drie spoelen
  tegelijk zakt de spanning onder wat nodig is om aan te trekken.
- **Er is een echte niveauvertaler naar de thermostaatbus**, geen los
  optocouplertje: PD4 &rarr; R24 &rarr; T1 (basis vast op AVCC, dus
  stroomgestuurd via de emitter) &rarr; basis Q4 &rarr; Q4 (PNP, emitter op
  +18 V). Dit is een bewust ontworpen schakeling, geen bijzaak.

### Doorbraak: de ketelkant werkt

Nadat bleek dat **thermostaat en ketel verwisseld aangesloten** waren, en dat
recht is gezet:

- `Ketel verbonden` = **ON**, aanhoudend
- 124&ndash;132 flanken per meetinterval op socket-pin 11
- De AVR decodeert de antwoorden van de ketel-emulator op zijn eigen polls

Daarmee is de complete ketelketen bevestigd: pin 5 via R13 naar de opto-LED,
het antwoord via OK5 pin 7 op pin 11, de leesrichting zoals afgeleid uit
`SBIS PIND,5`, de Manchester-codering uit `sub_0892` en de CTC-timing.

De omkeer-test was sluitend: leesrichting omgedraaid gaf nul flanken en
`verbonden` = OFF, terugzetten bracht beide direct terug.

Dat de eerdere metingen niets opleverden is hiermee grotendeels verklaard: er
werd tegen de verkeerde klemmen gemeten. De comparator las ~18 V op de
thermostaat-ontvangst omdat daar de ketel-emulator hing, die als slave zijn
eigen busspanning levert.

### Nog open &mdash; dit lost de dode klem op

- **Waar gaat de collector van Q4 heen?** Dit is de output van de driver die we
  net in kaart brachten. Komt hij rechtstreeks op de thermostaatklem uit, dan
  is de driver compleet en zit het probleem elders (bedrading, een volgend
  onderdeel, of onze PD4-aansturing). Breekt het spoor eerder af, dan is dat
  de plek.
- **Waar komt de basis van Q1 vandaan?** Q1 en Q4 delen hun emitter op +18 V;
  wat Q1 aanstuurt is nog niet gemeten.

Zodra deze twee bekend zijn is het OT-mysterie waarschijnlijk opgelost.

### Tweede dipswitch-schakelaar gevonden

Dip 1 (pin 1/3, naast dip 2 die op PC2 zit): **pin 1 zit op het NC-contact
van K2.** Zit dus in het schakelpad van de noodbrug zelf, niet op een
socket-pin. Vermoedelijk de tweede ader van het aderpaar dat de brug verlegt
(K1 = ketel-OT-ader, K2 = de andere ader).

Nog te bepalen: waar de andere kant van K2's NC-contact heen gaat, waar
dip-pin 3 op uitkomt, en hoe dit samenhangt met de eerder genoemde
gelijkrichterbrug van 4 diodes.

### Belangrijk voor de tests tot nu toe

**De dipswitches stonden op OFF (niet verbonden) tijdens alle eerdere tests.**
Dat verklaart PC2 = hoog: er zat geen expliciete pull-down via de dip, dus de
interne pull-up won. Als deze dip in de originele opzet dicht moet voor
normaal (OpenTherm-)bedrijf, dan hebben we tot nu toe steeds in de verkeerde
stand getest &mdash; onafhankelijk van alle andere firmware- en pincontroles.

**Bevestigd:** dip pin 4 (tegenover pin 2) zit aan massa. Dus ON = PC2 dicht
naar massa = laag = normaal bedrijf; OFF = open = PC2 hoog via de interne
pull-up = ketelkant afgeknepen. Ondubbelzinnig, geen aanname meer.

**Actie: zet de dip op ON, controleer dat PC2 laag wordt, en herhaal de
basistest** (komt er dan spanning op de thermostaatklem?) voordat verder
gezocht wordt naar Q4/Q1.

