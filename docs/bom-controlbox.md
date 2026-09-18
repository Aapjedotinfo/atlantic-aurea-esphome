# Bill of materials &mdash; controlbox

Onderdelenlijst van de Atlantic Aurea-controlbox, opgebouwd terwijl de print
wordt doorgemeten. Bedoeld om de warmtepompkant te kunnen **nabouwen** voor een
tweede buitenunit die zonder controlbox geleverd is.

De kolom **bron** zegt hoe hard een regel is:

| | |
|---|---|
| `gemeten` | met de meter vastgesteld |
| `afgelezen` | van de opdruk of de kleurcode van het onderdeel |
| `afgeleid` | volgt dwingend uit iets dat wel gemeten is |
| `open` | nog niet vastgesteld |

Zie ook [meetresultaten-ic2.md](meetresultaten-ic2.md),
[print-doormeten.md](print-doormeten.md) en [../WIRING.md](../WIRING.md).

## Voeding en netzijde

| Ref | Type / waarde | Functie | Bron |
|---|---|---|---|
| F1 | zekering **T3,75A L 250V AC**, traag | zit in klem 1 (L); dekt alleen de trafo en het aansluitpunt van de USB-adapter | gemeten + afgelezen |
| TR1 | **Myrra 44700**, E30x18<br>PRI 230V~ 50/60Hz (1-9)<br>SEC 15V~ 2,8VA (6-7) | netvoeding van de hele print | afgelezen |
| R15 | **10 k &plusmn;5%**, flameproof metaaloxide (zalmrode body) | aan klem 2 (N), parallel met R16 | afgelezen + gemeten |
| R16 | **10 k &plusmn;5%**, flameproof metaaloxide | parallel met R15; samen 5,00 k nominaal, in circuit 4,925 k gemeten | afgelezen + gemeten |
| &mdash; | **LM7818** | 18 V-tak voor OpenTherm en de relaisspoelen | afgelezen |
| &mdash; | **LM7805** | 5 V-logica; gevoed uit de uitgang van de LM7818 | afgelezen |

> R15 en R16 zijn **flameproof**, geen koolfilm. Dat type faalt open in plaats
> van door te branden en hoort aan een netingang. Bij nabouw het type
> overnemen, niet alleen de waarde.

> R15 en R16 doen twee dingen tegelijk: ze hangen aan klem 2 naast de trafo, en
> ze zijn de serieweerstand van de stroomlus naar OK1. Zie de buskant hieronder.

### De aardklem gaat nergens heen

De vierde klem (&#9143;) van `OUTDOOR UNIT` is nagemeten en komt **nergens anders
op de print uit**. Hij is puur een aansluitpunt voor de aardader van de kabel,
geen beschermingsaarde van de elektronica. Dat past bij de kunststof behuizing:
er is niets metaals om te aarden.

Twee gevolgen:

- Er is **geen aardreferentie op de print**. Geen Y-condensator, geen filtering
  naar aarde. De hele buskant zweeft op de nul van de buitenunit zonder dat er
  ergens een beschermende verbinding tussen zit. Dat is precies waarom je die
  kant als levend behandelt: er is geen aarde die je opvangt.
- Bij de nabouw hoef je met de aardader ook niets bijzonders te doen &mdash;
  netjes afmonteren volstaat. **Tenzij je een metalen kast gebruikt**, en dan
  moet die wél aan die ader, anders is de situatie wezenlijk anders dan hier.

## Warmtepompinterface &mdash; dit is wat nagebouwd moet worden

| Ref | Type / waarde | Functie | Bron |
|---|---|---|---|
| OK1 | **H11D1**, SOIC-6, datumcode 2310 | ontvangen van de warmtepomp. Pin 5 (collector) aan +5 V, pin 4 (emitter) naar R17 en R9 | gemeten + afgelezen |
| OK2 | **H11D1**, SOIC-6 | zenden naar de warmtepomp. Pin 1 (LED-anode) aan +5 V, pin 2 (kathode) naar R10 | gemeten + afgelezen |
| T2 | **BC547A**, NPN | laagzijdedriver voor de LED van OK2, aangestuurd vanaf IC1 pin 26 | gemeten |
| R14 | **1 k** | van IC1 pin 26 naar de basis van T2 | gemeten |
| R10 | **330 &ohm;** | in serie met de LED van OK2 | gemeten |
| R17 | **1 k** | emitter van OK1 naar massa | gemeten |
| R9 | **4k7** | emitter van OK1 naar IC1 pin 27; bovenste helft van de deler naar de ESP32 | gemeten |
| D15 | **6.8CA**, bidirectionele TVS | in dat hoekje; wát hij overbrugt is nog niet vastgesteld | afgelezen |
| R22, R23 | waarde onbekend | hoger op de print, boven T2; rol nog niet vastgesteld | open |

### Buskant &mdash; de koppeling naar klem 2 en 3

| Ref | Type / waarde | Functie | Bron |
|---|---|---|---|
| D7 | **1N4007** | in serie vanaf klem 3; anode naar de klem, kathode naar binnen. Laat alleen stroom van klem 3 naar binnen door | gemeten + afgelezen |
| D8 | **1N4007** | **antiparallel** over de reeks OK2-transistor + OK1-LED; anode op knooppunt X, kathode op Y. Klemt de omgekeerde spanning | gemeten + afgelezen |
| R15 &parallel; R16 | 2&times; **10 k flameproof** = 5 k | serieweerstand naar klem 2; bepaalt de lusstroom | gemeten |
| R11 | **10 k** | parallel over de **LED van OK1**. Leidt capacitief ingekoppelde lekstroom weg zodat de opto niet spookschakelt | gemeten |
| C6 | **2,2 nF** (opdruk `22` met een streep eronder) | idem, parallel over de LED van OK1 | gemeten |
| R12 | **1 M** | tussen **basis en emitter van OK2**. Voorkomt dat een dV/dt-flank via de collector-basiscapaciteit de transistor vals inschakelt | gemeten |
| C1 | **2,2 nF** (zelfde opdruk) | parallel met R12, basis-emitter OK2 | gemeten, onzeker |

> **C1 en C6 zijn niet hard.** De 2,2 nF is in circuit gemeten, met R11 (10 k)
> respectievelijk R12 (1 M) er parallel aan &mdash; zo'n meting trekt makkelijk
> scheef. En de opdruk `22` met een streep eronder laat zich lezen als 22 pF,
> 2,2 nF of 22 nF. Gelukkig is het circuit hier niet kritisch: met 5 k in serie
> geeft zelfs 22 nF een tijdconstante van 110 &micro;s tegen een bittijd van
> 1,5 ms bij 666 baud. **2,2 nF is de veilige middenkeuze** &mdash; te klein
> kost je wat storingsonderdrukking, te groot rondt je flanken af, en daartussen
> is ruimte zat.

```
klem 3
   |
   v  D7
   |
   +---------------- knooppunt Y ---- OK2 pin 5  collector
   |                                        |
  D8  antiparallel                     transistor        OK2 pin 6  base
   |                                        |                  |
   |                                 knooppunt Z -- R12 || C1 --+
   |                                        +-- OK2 pin 4  emitter
   |                                        +-- OK1 pin 1  anode
   |                                  +-----+-----+
   |                               OK1 LED  R11   C6
   |                                  +-----+-----+
   +---------------- knooppunt X ---- OK1 pin 2  kathode
                                            |
                                     R15 || R16  (5k)
                                            |
                                        klem 2
```

Het is een **stroomlus** tussen klem 3 (plus) en klem 2 (retour), met de
transistor van OK2 en de LED van OK1 in serie. Zenden is de lus onderbreken;
ontvangen is zien of er stroom loopt. Beide kanten zien dus ook hun eigen
zending &mdash; echo hoort bij deze topologie.

> R11, R12, C1 en C6 zijn geen luxe. Ze harden beide optocouplers tegen
> stoorpulsen op een lange kabel die langs netspanning loopt. Niet weglaten bij
> de nabouw; ze lijken overbodig tot ze het niet zijn.

> Klem 2 is tegelijk de nul van de netvoeding en de retour van de bus. Het
> signaal is klein, de referentie is dat niet.

**Klem 3 komt uitsluitend op D7 uit** &mdash; nagemeten, er hangt verder niets
aan. Daarmee is deze sectie compleet: elk onderdeel dat aan de buskant gevonden
is heeft een plek in het schema, en er is geen tak meer die nergens heen loopt.

Foto's waar deze reconstructie op rust:
[componentzijde](controlbox-optos-componentzijde.jpg) (OK1, OK2, D7, D8, C6, R11,
R15, R16, met de bandrichting van beide diodes) en
[soldeerzijde](controlbox-optos-soldeerzijde.jpg).

De H11D1 is een **300 V**-fototransistoroptocoupler. Bij nabouw is de 6-pins
DIP-versie pincompatibel met deze SOIC-6 en makkelijker te verwerken.

## OpenTherm- en ketelkant (niet nodig voor de nabouw)

| Ref | Type / waarde | Functie | Bron |
|---|---|---|---|
| OK5 | **TLP521-2** | dubbele optocoupler, &eacute;&eacute;n OT-bus in beide richtingen | gemeten |
| R13 | **330 &ohm;** | IC2 pin 5 naar OK5 pin 3 (LED-anode), zendpad naar de ketel, ~11 mA | gemeten |
| Q1&ndash;Q4 | **BC558A**, PNP | hoogzijdeschakelaars, thermostaatkant op 18 V | afgelezen |
| T1, T3, T4 | **BC547A**, NPN | laagzijdedrivers | afgelezen |
| R24 | **4k7** | IC2 pin 6 (PD4) naar de emitter van T1 | gemeten |
| R3 | **33 k** | met R4 de deler op IC2 pin 13 naar de comparator | gemeten |
| R4 | **4k7** | onderste helft van die deler | gemeten |
| R26 | **10 k** | via een diode van IC2 pin 16 naar de basis van T3 | gemeten |
| R19 | **5 k** | pull-up op RESET | gemeten |
| R21 | **10 k** | IC1 pin 14 naar de status-LED | gemeten |
| K1, K2 | relais 24 VDC | samen aangestuurd door T3; verleggen het aderpaar &rarr; de noodbrug | gemeten |
| K3 | **Omron G2R-1**, 12 VDC, 10 A wisselcontact | warmtevraagcontact voor een aan/uit-ketel, via T4 vanaf IC2 pin 28 | afgelezen |
| R27 | **100 &ohm;** | gedeelde voorschakelweerstand van alle drie de relaisspoelen | gemeten |

## Nodig voor unit 2

Alles hieronder is de **logicakant**, die volledig vastligt. De buskant staat
nog open en is bewust niet in deze lijst opgenomen &mdash; zie het lege veld
hierboven.

| Aantal | Onderdeel | Waarvoor |
|---|---|---|
| 2 | **H11D1** (6-pins DIP mag) | OK1 ontvangen, OK2 zenden |
| 1 | **BC547** | driver voor de LED van OK2 |
| 1 | 1 k | basisweerstand T2 |
| 1 | 330 &ohm; | LED-serieweerstand OK2 |
| 1 | 1 k | emitterweerstand OK1 |
| 1 | 4k7 | bovenste helft van de RX-deler |
| 1 | 10 k | onderste helft van de RX-deler naar massa |
| 1 | 5 V-voeding (USB-adapter) | ESP32 en beide optocouplers |

### Hoe die onderdelen aan de ESP32 hangen

```
ZENDEN   ESP32 -> unit          niet-inverterend

  GPIO17 ── 1k ──┤ basis   BC547
                   emitter ── GND
                   collector ── 330R ── OK2 pin 2  (LED kathode)
  +5V ───────────────────────────────── OK2 pin 1  (LED anode)

  OK2 pin 4 (emitter) ─┐
  OK2 pin 5 (collector)┴── buskant, naar klem 1/2/3   [nog open]


ONTVANGEN   unit -> ESP32       niet-inverterend

  OK1 pin 1 (LED anode) ─┐
  OK1 pin 2 (LED kathode)┴── buskant, naar klem 1/2/3  [nog open]

  +5V ── OK1 pin 5 (collector)
         OK1 pin 4 (emitter) ─┬── 1k ── GND
                              └── 4k7 ──┬── GPIO16
                                        └── 10k ── GND
```

De deler 4k7/10k brengt de ~5 V van de emitter-follower terug naar ~3,4 V, veilig
voor een ESP32-ingang. De zendkant mag rechtstreeks: 3,3 V op die 1 k stuurt T2
in verzadiging. Geen levelshifter nodig, en beide richtingen zijn
niet-inverterend, dus er hoeft in de firmware niets omgekeerd te worden.

Communicatie is **666 baud, 8N1**; zie de `chofu_wp`-component.

> Die 5 V-voeding is het **enige** wat je zelf hoeft te leveren. De 18 V-tak uit
> de trafo was er voor OpenTherm en de relaisspoelen; geen van beide bestaat in
> de opstelling voor unit 2.

> Massa van de USB-voeding en de common van de bus **gescheiden houden**. Dat is
> de scheiding waar die twee optocouplers voor zitten.
