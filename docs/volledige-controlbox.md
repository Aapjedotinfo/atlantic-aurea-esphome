# De hele controlbox in eigen beheer

> **Status: ontwerp, niet gebouwd.** De pinout is afgeleid uit de
> gedisassembleerde originele firmware van IC1 en IC2 en is met hoog vertrouwen
> correct, maar nog nergens doorgemeten. Bouw niets definitief voordat je de
> tabel in [Wat je eerst moet doormeten](#wat-je-eerst-moet-doormeten) hebt
> afgevinkt.
>
> Deze branch staat bewust alleen lokaal.

## Doel

Vandaag vervangt de ESP32 alleen **IC1** en praat hij met de warmtepomp. De
CV-ketel stuur je los aan, en de Anna-thermostaat hangt aan de originele **IC2**
die er nog steeds tussen zit.

Doel: alle drie de kanten in eigen beheer, zodat één regelaar kan besluiten hoe
de warmtevraag over warmtepomp en ketel wordt verdeeld.

| Kant | Rol | Protocol |
|---|---|---|
| Chofu buitenunit | volgt de cadans van de unit | 666 baud, 8N1 |
| Thermostaat | **OpenTherm slave** (doet zich voor als ketel) | OpenTherm, Manchester ~1 kbit/s |
| CV-ketel | **OpenTherm master** (doet zich voor als thermostaat) | OpenTherm, Manchester ~1 kbit/s |

Met "thermostaat" bedoelen we hier bewust niet per se de Anna: elke
OpenTherm-thermostaat past, zie [Moet het Anna zijn?](#moet-het-anna-zijn).

De taakverdeling volgt die van Atlantic zelf: een **ESP32 in de IC1-socket** doet
de warmtepomp en het beleid, een **eigen ATmega328P in de IC2-socket** doet de
twee OpenTherm-kanten. Ze praten via de 9600-baud-link die al op de print ligt.

Waarom niet alles op de ESP32? Twee redenen. De OpenTherm-interface-elektronica
op de print is voor 5 V-logica gemaakt, en een AVR in die socket gebruikt hem
zoals ontworpen. En de 9600-baud-sporen tussen IC1 en IC2 liggen er al, dus deze
opzet kost géén extra bedrading.

## Pinout IC1-socket (was "Haddon M1.1")

DIP28, ATmega8. Alleen de pinnen die er nu toe doen.

| Socket-pin | AVR-pin | Functie | Nodig |
|---|---|---|---|
| 26 | PC3 | **TX naar warmtepomp** (666 baud) | ja |
| 27 | PC4 | **RX van warmtepomp** | ja |
| 2 | PD0 (RXD) | **9600-baud van IC2** | ja |
| 3 | PD1 (TXD) | **9600-baud naar IC2** | ja |
| 7 | VCC | +5 V uit de onboard LM7805 | voeding |
| 8, 22 | GND | massa | ja |
| 23 | PC0 | CV-only-schakelaar (actief bij HOOG) | optioneel |
| 24 | PC1 | test-knop (actief bij LAAG) | optioneel |
| 14 | PB0 | vraagrelais "roep de WP aan" | optioneel |
| 15 | PB1 | statuslampje (aan bij storing) | optioneel |
| 17 | PB3 | tweede bitstroom, functie onopgehelderd | nee |

## Pinout IC2-socket (was "Haddon T1.1")

De rolverdeling is afgeleid uit de bufferadressen in de firmware: buffer `0x89`
bevat Read-Ack-antwoorden (slavegedrag) en gaat naar PD4, buffer `0x8d` bevat
Read-Data/Write-Data (mastergedrag) en gaat naar PD3.

| Socket-pin | AVR-pin | Functie | Richting |
|---|---|---|---|
| **6** | PD4 | **OpenTherm naar de thermostaat** | uit |
| **13** | PD7 (AIN1) | **OpenTherm van de thermostaat** (via comparator) | in |
| **5** | PD3 | **OpenTherm naar de CV-ketel** | uit |
| **11** | PD5 | **OpenTherm van de CV-ketel** | in |
| 2 | PD0 (RXD) | 9600-baud van IC1 | in |
| 3 | PD1 (TXD) | 9600-baud naar IC1 | uit |
| 7 | VCC | +5 V | voeding |
| 8, 22 | GND | massa | massa |
| 25 | PC2 | knop / modusingang | optioneel |
| 16 | PB2 | uitgang die PC2 volgt (relais?) | optioneel |
| 27 | PC4 | foutcode-indicator (knippercode) | optioneel |
| 28 | PC5 | tweede statusuitgang | optioneel |

### Ruststand van de zendlijnen

Uit de poort-init (`DDRD = 0x1E`, `PORTD = 0x19`): PD3 en PD4 zijn uitgangen die
**HOOG** idlen. Neem dat over — zet je ze laag in rust, dan staat de
OpenTherm-bus in de verkeerde toestand.

### Waarom pin 13 voor de thermostaat-ontvangst

De originele firmware leest die kant niet met een gewone digitale ingang maar met
de **analoge comparator**: `SBIC/SBIS ACSR, 5` leest ACO. In de init staat
`ACSR = 0x40`, en bit 6 is ACBG — die vervangt de positieve comparator-ingang
door de interne bandgap van 1,23 V. De negatieve ingang blijft dan AIN1, en AIN1
is PD7, oftewel **socket-pin 13**.

Dat past op de poortconfiguratie: `DDRD = 0x1E` laat PD0, PD5, PD6 en PD7 als
ingang staan, en PD6 (AIN0) blijft ongebruikt — precies wat je verwacht als de
bandgap de plek van AIN0 inneemt.

### Twee OpenTherm-bussen — waarom we daarvan uitgaan

WackoH's pintabel noemt voor IC2 maar één OT-paar (pin 5 en 11), met pin 6 als NC
en pin 13 aan massa. Wij gaan toch van twee uit, omdat de firmware geen ruimte
voor twijfel laat:

- `DDRD = 0x1E` maakt PD3 én PD4 uitgang; `PORTD = 0x19` zet ze allebei hoog.
- `sub_0892` (PD3) en `sub_0958` (PD4) zijn twee complete Manchester-automaten en
  worden **allebei elke lus-iteratie aangeroepen**. Geen dode code.
- Twee ontvangstwegen, elk met een eigen automaat.
- Buffer `0x8d` krijgt Read-Data/Write-Data. **Master-frames bouwen heeft alleen
  zin als er een ketel is om tegen te praten.**
- Lag pin 13 werkelijk aan massa, dan zou de comparator permanent één uitlezen en
  zou de thermostaatrichting nooit één frame afmaken.

Zijn tabel is vermoedelijk niet fout maar **onvolledig**: een OT-ontvangst naar
een comparator-ingang heeft vrijwel altijd een weerstandsdeler met één poot aan
massa, en met het oog volgen levert dan "pin 13 → GND" op. Zijn tabel staat
bovendien vol NC's op IC2; hij was gefocust op de warmtepompkant van IC1 en
schreef zelf dat zijn schema nog niet af was. Zie
[de volledige vergelijking](../../MCU's%20Atlantic/Tweakers/analyse-wackoh.md).

## Zit de OpenTherm-interface op de print?

OpenTherm is geen logisch niveau maar een **stroomlus**. De ketelzijde levert de
busspanning en de thermostaatzijde communiceert door stroom te trekken. Tussen
die bus en een 5 V-MCU-pin hoort dus interface-elektronica.

De originele IC2 stuurde PD3/PD4 rechtstreeks aan en las PD5/PD7 rechtstreeks
uit. Dat kán alleen als die elektronica op de print zit. WackoH's geannoteerde
printfoto bevestigt dat: hij wijst een **18 V-voeding voor OpenTherm** aan.

Richting de thermostaat is de controlbox de **ketel**, en die moet de busspanning leveren —
vandaar die 18 V. Haal die kant dus niet van de spanning af.

## Voeding

`WIRING.md` stelt nu dat de 15 V / 2,8 VA-trafo ruim marge heeft "omdat het
relais en de OpenTherm-MCU niet meer belast worden". **Dat argument vervalt met
dit ontwerp**: de OpenTherm-kant wordt juist weer belast, en de bus richting de thermostaat
moet actief gevoed worden.

2,8 VA is krap. Voed de ESP32 via USB (zoals nu) en laat de trafo de OT-bus
verzorgen. Meet de trafo-temperatuur na een uur draaien.

## Architectuur: thermostaat en ketel gekoppeld, met een rem ertussen

De thermostaat en de CV-ketel praten gewoon met elkaar, via de AVR als **doorgeefluik**.
Alles gaat door: tapwater, storingsmeldingen, modulatie, diagnostiek. Wat je niet
kent geef je door, en dat is meteen het veiligste gedrag.

De grip zit hem niet in het onderbreken van dat gesprek, maar in **twee
substituties**:

```
   Anna ◄──OpenTherm──► [AVR doorgeefluik] ◄──OpenTherm──► CV-ketel
                              │  ▲
              richting ketel: │  │ richting Anna:
              ID 1 = rem      │  │ ID 25/28 = temperaturen van de WP
                              │  │
                         9600 baud (parameters)
                              │
                 ESP32: beleid + warmtepomp (666 baud)
```

**Richting de ketel vervang je ID 1 (Control Setpoint).** Dat is de rem. Zet 'm op
10 °C en de ketel houdt zich koest; laat Anna's waarde door en hij gedraagt zich
normaal. Je hoeft hem dus nooit "uit" te zetten — je zegt alleen dat hij niets
hoeft te leveren.

> Dit is niet verzonnen: de originele T1.1 schrijft richting de ketel precies zo
> een Control Setpoint van **10,0 °C** (`0x0a00` bij adres `0x04be`). Atlantic
> gebruikte dezelfde rem.

**Richting Anna vervang je ID 25 en 28** met de aanvoer- en retourtemperatuur van
de warmtepomp, zodat haar regellus ziet dat er warmte geleverd wordt ook als de
ketel koud staat.

### Waarom dit beter is dan twee losse eindpunten

Een eerdere versie van dit ontwerp maakte de AVR een zelfstandige slave én master,
met alleen parameters over de link. Dat geeft meer controle, maar je koopt er veel
voor:

| | Doorgeefluik met rem | Twee losse eindpunten |
|---|---|---|
| Tapwater (DHW) | werkt vanzelf | moet je zelf goed krijgen |
| Storingen en diagnostiek van de ketel | bereiken Anna gewoon | moet je doorvertalen |
| Onbekende Data-ID's | gaan door | moet je beantwoorden |
| Als jouw elektronica faalt | Anna en ketel draaien door | huis wordt koud |
| Implementatiewerk | Manchester + 3 substituties | twee volledige OT-rollen |

Het enige dat je inlevert is de mogelijkheid om de ketel iets te laten doen wat
Anna helemaal niet vraagt. Dat heb je niet nodig: je wilt hem juist **tegen**
houden, en daar is de setpoint-rem genoeg voor.

### Tapwater blijft buiten schot

Dit is de belangrijkste winst van koppelen. Bij een combiketel wordt tapwater
getriggerd door de stromingssensor in de ketel zelf; de DHW-vlag in Data-ID 0
geeft daar alleen toestemming voor. In een doorgeefluik gaat die vlag ongemoeid
van Anna naar de ketel en werkt warm water precies zoals altijd.

**Raak DHW niet aan.** Neem het niet mee in je bijstook-afweging, en substitueer
niets in Data-ID 0 tenzij je heel zeker weet wat je doet. Koud water uit de kraan
merk je op het slechtst denkbare moment.

Let wel op: tijdens tapwaterbereiding levert de ketel tijdelijk geen CV. Dat is
normaal. Het `DHW-modus actief`-bit uit de slave-status vertelt je wanneer dat
speelt, dus dat is het waard om aan de ESP32 door te geven — puur ter informatie,
niet om op te regelen.

### Eerst meten: ziet de ketel de warmtepomp al?

Of je ID 25/28 überhaupt moet substitueren hangt van de waterzijde af. Beide
bronnen voeden hetzelfde circuit, dus als er circulatie door de ketel is, meet
zijn eigen sensor het water dat de warmtepomp heeft opgewarmd — en klopt zijn
rapportage vanzelf.

Draait de ketelpomp niet mee, dan meet hij stilstaand koud water en heb je de
substitutie wél nodig. Precies daarom bouwde Atlantic 'm in.

**Test:** laat alleen de warmtepomp draaien en vergelijk de aanvoertemperatuur die
de ketel meldt met die van de warmtepomp. Lopen ze mee, dan kun je de substitutie
achterwege laten en is het doorgeefluik nóg simpeler.

### De rem: hoe je de ketel tegenhoudt

Je zet de ketel niet uit, je zegt dat hij niets hoeft te leveren. Dat doe je door
richting de ketel **Data-ID 1 (Control Setpoint)** te vervangen.

| ESP32 wil | Vervang ID 1 door |
|---|---|
| warmtepomp doet het alleen | 10 &deg;C (ketel blijft koud) |
| ketel mag bijstoken | Anna's eigen waarde, of een door jou gekozen setpoint |

Meer heb je niet nodig. De ketel blijft ondertussen gewoon met Anna praten over
status, modulatie, tapwater en storingen — je knijpt alleen de warmtevraag af.

> Kortsluitbeveiliging: laat de rem niet elke paar seconden wisselen, anders gaat
> de ketel pendelen. Een minimale aan- en uittijd van zo'n 5 minuten is genoeg.
> Zet die in de AVR, niet in de ESP32: dan geldt hij ook als het beleid hapert.

### Moet het Anna zijn?

Nee. De brug ziet aan de thermostaatkant alleen "een OpenTherm-master" — welk
merk daar hangt is niet relevant. Een **Remeha iSense**, een **Honeywell**, of wat
je oorspronkelijk in huis had werkt net zo goed.

Sterker nog, je wint er twee dingen mee.

**Home Assistant blijft alles zien, ook zonder Plugwise.** De brug leest het hele
OpenTherm-gesprek mee, dus kamertemperatuur, gewenste temperatuur en warmtevraag
komen via de parameterlink binnen — ongeacht welke thermostaat er hangt. Je hebt
geen Smile-gateway en geen merkintegratie meer nodig om die waarden in HA te
krijgen. Eén afhankelijkheid minder.

**En je kunt de thermostaat toch op afstand zetten.** OpenTherm heeft daar een
standaardmechanisme voor: **Data-ID 9, "Remote override room setpoint"**. Die gaat
van *slave naar master* — dus wij, als slave, kunnen de thermostaat een setpoint
opdringen. Staat er een waarde ≠ 0 in, dan neemt de thermostaat die over; op 0
regelt hij weer zelf. Data-ID 100 bepaalt daarbij hoe de override zich verhoudt
tot handmatig draaien aan de knop.

Dat is precies hoe de bekende OpenTherm Gateway zijn temperatuurcommando's doet,
en Honeywell-thermostaten zijn daar het klassieke doelwit van. Je krijgt dus
dezelfde bediening als met Anna + Smile, maar dan merkonafhankelijk en zonder
tussenliggende gateway.

> Wel verifiëren: niet elke thermostaat ondersteunt remote override, en de
> precieze omgang met de draaiknop verschilt per model. Dat zie je meteen in stap
> 3, zodra je het verkeer kunt meelezen — vraag ID 9 op en kijk of de thermostaat
> erop reageert.

**Wat je opgeeft:** niets wezenlijks. Plugwise kent een paar merkeigen trucs met
bijzondere setpointwaarden, maar daar bouwen we bewust geen logica op — dat zou
precies de merkbinding terugbrengen die deze opzet juist wegneemt. Koelen schakel
je gewoon rechtstreeks in Home Assistant, waar `chofu_wp` al een switch voor
heeft.

Per saldo maakt dit het systeem **eenvoudiger**: één OpenTherm-thermostaat naar
keuze, één brug, één ESP32. Geen cloud, geen gateway, geen merkbinding.

### De vlaggen in Data-ID 0 (ter informatie)

Je hoeft hier niets aan te veranderen, maar je wilt ze wel kunnen lezen. Het
statusbericht is tweerichtings: de master zet in de hoge byte wat er m&aacute;g, de
slave meldt in de lage byte wat er gebeurt.

| Byte | Bit | Betekenis |
|---|---|---|
| master (hoog) | 0 | CH enable &mdash; mag de ketel voor verwarming branden |
| master (hoog) | 1 | DHW enable &mdash; mag de ketel warm tapwater maken |
| slave (laag) | 0 | storing |
| slave (laag) | 1 | CH-modus actief |
| slave (laag) | 2 | DHW-modus actief |
| slave (laag) | 3 | vlam aan |

> Bevestigd door de originele firmware: T1.1 antwoordt Anna met statusdata
> `0x00` (niets), `0x02` (bit 1 = CH actief) of `0x0a` (bit 1 + bit 3 = CH actief
> &eacute;n vlam). Dat past exact op deze bitindeling.

Deze bits doorgeven aan de ESP32 is nuttig om te zien wat er gebeurt &mdash; brandt
de ketel, maakt hij tapwater, is er een storing &mdash; maar je regelt er niet mee.
De rem uit de vorige paragraaf is je enige stuurmiddel, en dat is genoeg.

### Faalgedrag: wat als de ESP32 wegvalt

Dit is de mooiste eigenschap van deze opzet: **er is geen faalgedrag om te
bouwen.** Blijft het parameterbericht uit, dan laat de AVR de substituties
vervallen en is hij een gewoon doorgeefluik. Anna en de ketel praten dan
rechtstreeks met elkaar en het huis wordt verwarmd zoals bij een normale
CV-installatie.

Bouw wel de vervaltijd in &mdash; bijvoorbeeld: een substitutie die 60 s niet
vernieuwd is, vervalt &mdash; zodat dat gedrag automatisch is in plaats van
hoopvol. Het kost &eacute;&eacute;n teller.

Je verliest in die toestand alleen de warmtepomp. De ketel neemt het over, precies
zoals bedoeld.

## Protocol tussen ESP32 en AVR

Atlantic's framing houden we aan: vast startbyte, databytes, 8-bits optelsom.
Twee berichten, allebei met een vaste indeling. Geen OpenTherm-frames &mdash; de
AVR handelt het gesprek zelf af en de link draagt alleen waarden.

**ESP32 &rarr; AVR (substituties, elke paar seconden):**

```
byte 0     0xF1     startbyte
byte 1     vlaggen: rem actief, ID25/28-substitutie actief
byte 2     ketel-setpoint dat de rem oplegt (10 = ketel koud houden)
byte 3-4   aanvoertemperatuur die Anna moet zien (0,1 &deg;C, signed)
byte 5-6   retourtemperatuur die Anna moet zien (0,1 &deg;C, signed)
byte 7     geldigheidsduur in seconden (0 = alle substituties opheffen)
byte 8     8-bits optelsom
```

**AVR &rarr; ESP32 (waarnemingen, elke paar seconden):**

```
byte 0     0xF0     startbyte
byte 1     vlaggen uit Data-ID 0: Anna vraagt CH, Anna vraagt DHW,
           ketel in CH-modus, ketel in DHW-modus, vlam aan, storing
byte 2     door Anna gevraagde control setpoint (&deg;C) - v&oacute;&oacute;r de rem
byte 3-4   kamertemperatuur van Anna (0,1 &deg;C)
byte 5-6   room setpoint van Anna (0,1 &deg;C)
byte 7-8   aanvoertemperatuur zoals de ketel zelf meldt (0,1 &deg;C)
byte 9     modulatieniveau ketel (%)
byte 10    storingscode van de ketel
byte 11    vlaggen: OT-link Anna ok, OT-link ketel ok, rem actief
byte 12    8-bits optelsom
```

Byte 2 in het waarnemingsbericht is belangrijk: dat is wat Anna *zou* vragen als
je niet remde. Daarmee ziet de ESP32 hoeveel warmtevraag er werkelijk is en kan
hij besluiten of de warmtepomp het alleen aankan.

En byte 7-8 is de temperatuur die de ketel z&eacute;lf meet. Vergelijk die met de
aanvoer van de warmtepomp om te bepalen of je de ID25/28-substitutie &uuml;berhaupt
nodig hebt (zie de meting hierboven).

Ruim binnen wat 9600 baud aankan. Alle regellogica zit op de ESP32, in ESPHome,
waar je 'm zonder programmer kunt bijstellen.

### Wat je van het origineel meeneemt

- **De rem via ID 1.** Atlantic schrijft richting de ketel een Control Setpoint
  van 10 &deg;C om hem koest te houden. Precies wat wij doen.
- **Vertel Anna de temperaturen van de w&aacute;rmtepomp** als de ketel koud staat,
  zodat haar regellus niet op hol slaat. Alleen nodig als de ketel het opgewarmde
  water niet zelf al meet.
- **Alles wat je niet kent gaat door.** Onbekende Data-ID's, tapwater,
  storingsmeldingen &mdash; die hoeven jou niet te interesseren.

## Welke chip

Een **ATmega328P in DIP28**: fysiek pin-compatibel met de ATmega8, dus een echte
drop-in, maar met meer flash en RAM en met de Arduino-toolchain erachter.

### De klok: 8 MHz, afgeleid uit de firmware

Dit staat niet in de dumps &mdash; die zijn 8704 bytes, oftewel 8192 flash plus 512
EEPROM, en de fuse-bytes bewaart de programmer apart. Maar de klok is wel uit de
code te berekenen. Beide chips zetten Timer1 op `TCCR1B = 0x01` (geen prescaler)
en herladen `TCNT1` in hun overflow-ISR:

| Chip | Preload | Cycli per tick | Wat de tick moet zijn |
|---|---|---|---|
| M1.1 | `0xF84F` | 1969 | 250,25 &micro;s (&frac16; bit @ 666 baud) |
| T1.1 | `0xFC3A` | 966 | 125 &micro;s (&frac18; bit @ 1000 bps OpenTherm) |

Bij 8 MHz geven die preloads 246,1 en 120,8 &micro;s &mdash; allebei precies 33 &agrave;
34 cycli te kort, en dat is de ISR-overhead (interruptlatentie plus de PUSH's
v&oacute;&oacute;r het herladen). Twee volstrekt verschillende tijdbases, hetzelfde
tekort. Bij 4 of 16 MHz loopt het volledig uit de pas.

**Beide chips draaien dus op 8 MHz.** Je eigen 328P moet dat ook doen, anders
kloppen alle OpenTherm-constanten niet meer.

### Fuses

Of die 8 MHz uit een kristal kwam of uit de interne RC-oscillator staat niet in
de code &mdash; dat zit alleen in de fuses. **Nagekeken op de print: onder de
IC2-voet ligt een 8 MHz-kristal.** Daarmee is het een extern kristal, en dat
bevestigt de kloksnelheid nog eens langs een tweede weg.

| Waar de chip komt | lfuse | hfuse | efuse |
|---|---|---|---|
| **In de controlbox** (kristal) | `0xFF` | `0xD9` | `0xFD` |
| Op het bureau (geen kristal nodig) | `0xE2` | `0xD9` | `0xFD` |

De **`.hex` is voor allebei dezelfde**. De klokbron is een fuse-instelling, geen
compileeroptie: `F_CPU` staat in beide gevallen op 8 MHz. Je hoeft dus niet te
herbouwen als je van bureau naar controlbox gaat, alleen `lfuse` om te zetten.

Zet je 'm op *extern* terwijl er geen kristal is, dan doet de chip helemaal
niets en is niet te zien of het aan de firmware of aan de fuses ligt. Op het
bureau is `0xE2` dus de veilige kant; in de socket mag en moet het `0xFF` zijn.

Dat het een kristal is, is bovendien nodig en niet alleen netjes: de interne
RC loopt op zo'n &plusmn;3%, en voor een 9600-baud-UART heb je ongeveer
&plusmn;2,5% totaal te verdelen. De ESP32-kant is kristalnauwkeurig, dus dat hele
budget zou van de AVR zijn geweest.

> Laat **RSTDISBL** en **DWEN** met rust. Die maken van de resetpin een gewone
> I/O-pin, en dan komt een normale programmer er niet meer bij.

### Twee OT-kanalen op &eacute;&eacute;n AVR

Twee onafhankelijke OT-kanalen op één AVR is aantoonbaar haalbaar: **Atlantic
deed het op een ATmega8 met 1 KB RAM.** `sub_0892` en `sub_0958` zijn twee losse
automaten aan dezelfde Timer1-tick. Je hebt de referentie al uitgeplozen: tick
`0xFC3A` (125 µs), 8 ticks per bit, mid-bit-inversie op tick 4, plus de
pariteitsroutine.

> Gebruik níét de bekende OpenTherm-Arduino-bibliotheek. Die is gebouwd rond één
> instantie met vaste interrupts; twee rollen wringt daarin. Schrijf het zoals
> het origineel: één timer-tick, twee state machines die er los aan hangen.

De programmer heb je al: met de TL866-II heb je de originele chips uitgelezen, en
dezelfde ZIF-voet schrijft de 328P.

Wel iets om rekening mee te houden bij het ontwerp: in de controlbox heb je geen
seri&euml;le monitor. Chip erin, deksel dicht, en dan is de **ESP32 in IC1 je enige
venster**. Bouw de link-diagnose (volgnummer, teller voor gemiste berichten, tijd
sinds het laatste geldige bericht) daarom als eerste in de AVR-firmware, v&oacute;&oacute;r
de OpenTherm-kant. Zie ook het gefaseerde plan.

## De koppeling ESP32 ↔ AVR

Via de IC1-socket, waar de ESP32 al zit. De print verbindt IC1 pin 2/3
kruislings met IC2 pin 3/2, dus er is **geen nieuwe bedrading** nodig.

| IC1-socket pin | Richting | ESP32 |
|---|---|---|
| 2 | ← van de AVR | UART RX op **GPIO19**, via spanningsdeler |
| 3 | → naar de AVR | UART TX op **GPIO18**, rechtstreeks |

De AVR draait op 5 V. Zijn TX naar de ESP32 moet omlaag via dezelfde deler die je
op de warmtepomp-RX al hebt (4k7 + 10 kΩ). Andersom is 3,3 V net genoeg voor de
AVR: die wil minimaal 0,6 × VCC = 3,0 V.

### ESP32-pinnen

| Functie | GPIO |
|---|---|
| WP TX (naar IC1 pin 26) | 17 |
| WP RX (van IC1 pin 27) | 16 |
| Naar de AVR (IC1 pin 3) | 18 |
| Van de AVR (IC1 pin 2) | 19, via 4k7 + 10k |
| Status-LED | 2 |

## Bureau-opstelling: ESP32 &harr; AVR

De eerste stap is de link bewijzen, los van de controlbox en los van OpenTherm.
Een paar keuzes daarbij bepalen of je later voor verrassingen komt te staan.

### Bouw meteen de echte configuratie

Verleidelijk is om de AVR op 3,3 V te zetten &mdash; dan hoef je niets te schuiven.
Doe dat niet. Je deployt straks op **5 V in de socket**, en dan wil je de
niveaus al getest hebben.

Even verleidelijk: een Arduino Uno of Nano pakken. Die draait op **16 MHz**, en al
je OpenTherm-timingconstanten hangen aan de klok. Wat op het bureau werkt, werkt
dan in de controlbox niet.

Twee werkbare routes:

- een **kale ATmega328P op een breadboard** met 8 MHz-kristal en twee
  condensatoren van ~22 pF, via ISP geprogrammeerd;
- of een **Arduino Uno waarvan je het kristal naar 8 MHz vervangt**. Dat werkt
  prima, en heeft een mooi voordeel: zie hieronder.

### Een Uno ombouwen naar 8 MHz

Het voordeel: de 328P op een Uno zit in een **DIP28-voet**. Ontwikkel je op de Uno
met een 8 MHz-kristal, dan kun je de chip er daarna gewoon uit halen en in de
IC2-socket van de controlbox steken. Zelfde pakket, zelfde spanning, zelfde
kloksnelheid &mdash; aangenomen dat de controlbox z&eacute;lf een 8 MHz-kristal voor
IC2 heeft (pin 9/10); zie de fuse-paragraaf hierboven, dat is nog een keer kijken
waard. Je verhuist dan letterlijk de chip die je getest hebt.

**De val zit in `F_CPU`, niet in de hardware.** Compileer je met het bord op
"Arduino Uno", dan gaat de compiler uit van 16 MHz. Draait de chip dan op 8 MHz,
dan zijn al je baudrates gehalveerd en al je `delay()`s verdubbeld &mdash; en dat
merk je pas als niets meer klopt.

Wat je moet regelen:

| Onderwerp | Wat te doen |
|---|---|
| **F_CPU** | Kies een bord-definitie met 8 MHz. MiniCore ("ATmega328, 8 MHz external") of "Arduino Pro Mini, 3.3V 8MHz" &mdash; die laatste alleen voor de klokinstelling; je draait gewoon op 5 V |
| **Bootloader** | Niet nodig. Je programmeert de chip buiten het bord, in de ZIF-voet van de TL866-II, en drukt 'm daarna in de socket |
| **Fuses** | Zie hierboven: `0xFF` bij een kristal, `0xE2` op interne RC. De standaard Uno-fuses staan al op "low power crystal 8&ndash;16 MHz", dus met een kristal hoef je meestal niets te schrijven |

> **Exporteer de juiste .hex.** Sketch &rarr; Export Compiled Binary levert twee
> bestanden op; neem die **zonder** `with_bootloader`.

**Let op D0 en D1.** Dat zijn PD0/PD1, dus j&oacute;uw linkpinnen &mdash; maar ze
hangen op een Uno ook aan de USB-seri&euml;lchip. Meestal zit daar een weerstand
tussen en kun je ze extern gewoon gebruiken, maar open dan niet tegelijk de
seri&euml;le monitor, want die injecteert data op je protocol. Voed de Uno tijdens
de test bij voorkeur niet via USB, en gebruik de `SoftwareSerial`-uitgang naar een
USB-TTL-adapter voor je debugregels.

### Bedrading

```
   ATmega328P (5 V)                              ESP32 (3,3 V)

   pin 3 / PD1 (TX) ──────[ 1k8 ]──┬─────────── GPIO26 (RX)
                                   │
                                 [ 3k3 ]
                                   │
                                  GND

   pin 2 / PD0 (RX) ◄──────────────────────────  GPIO25 (TX)

   GND ─────────────────────────────────────────  GND
```

**De deler is niet optioneel.** De AVR zendt 5 V en de ESP32 verdraagt dat niet.
1k8 + 3k3 geeft 3,24 V; de 4k7 + 10k die je op de warmtepomp-RX al gebruikt komt
op 3,40 V en mag ook, maar zit dichter tegen de grens.

**Andersom hoeft niets.** De AVR wil minimaal 0,6 &times; VCC = 3,0 V zien en de ESP32
levert 3,3 V. Dat werkt, met weinig marge maar betrouwbaar &mdash; precies zoals de
warmtepomp-TX nu al zonder levelshifter werkt.

**Gemeenschappelijke massa is verplicht.** Voed je beide vanaf dezelfde USB-poort
of hub, dan is dat al geregeld; anders trek je er een draad tussen. Voed de AVR
niet vanaf de 3,3 V-regelaar van de ESP32.

### Waar je de seri&euml;le poorten neerzet

Op de ESP32: gebruik een **hardware-UART** (UART1 of UART2) op GPIO25/26, niet
UART0. Die laatste is je USB-console; deel je die, dan lopen je debugregels door
het protocol heen.

Op de AVR zit de moeilijkheid: de ATmega328P heeft er maar **&eacute;&eacute;n**. Die gebruik
je voor de link, want dat is de echte configuratie. Voor debugoutput hang je
tijdelijk een `SoftwareSerial` op twee vrije pinnen naar een USB-TTL-adapter.

> Zodra OpenTherm meedraait moet die SoftwareSerial eruit: hij zet interrupts uit
> tijdens verzenden en dat is precies wat je Manchester-timing sloopt. Gebruik
> vanaf dat moment de link zelf om diagnostiek naar de ESP32 te sturen.

Handig extraatje: hang een USB-TTL-adapter met alleen zijn **RX** aan de
AVR-TX-lijn (massa delen). Dan zie je het verkeer meelopen zonder ergens in te
grijpen.

### Framing: niet op stiltes vertrouwen

Verleidelijk is om berichten te scheiden op een pauze. Doe dat niet &mdash; beide
kanten doen ondertussen ander werk en die gaten zijn niet betrouwbaar.

Gebruik: **startbyte, vaste lengte, checksum.** Loopt de checksum niet, gooi het
bericht weg en zoek opnieuw naar het startbyte. Duikt het startbyte toevallig in
de payload op, dan faalt de checksum en synchroniseer je vanzelf op het volgende
echte bericht. Meer robuustheid heb je bij deze berichtlengtes niet nodig.

### De test zelf

1. **AVR zendt een teller** elke seconde; de ESP32 logt hem. Bewijst richting,
   baudrate en niveaus.
2. **ESP32 stuurt een waarde terug**, de AVR echoot 'm in zijn volgende bericht.
   Bewijst de andere richting en dat beide kanten checksums goed rekenen.
3. **Trek de draad er tussenuit** terwijl het loopt, en steek 'm terug. Beide
   kanten moeten binnen een paar berichten weer synchroon lopen zonder herstart.
   Dit is de test die iedereen overslaat en waar je later spijt van krijgt.
4. **Zet de ESP32 uit** en controleer dat de AVR netjes naar zijn terugvalgedrag
   gaat in plaats van te blijven wachten.

Werkt dat alle vier, dan is de koppeling klaar en kun je OpenTherm erbij bouwen.

## Gefaseerd plan

Niet in één keer bouwen. Elke stap is los te testen, en de originele chip past tot
het eind terug in de socket.

1. **Luister eerst.** Zet een AVR in de IC2-socket die niets zendt en alleen
   rapporteert wat er op de vier kandidaat-pinnen binnenkomt. Daarmee los je in
   e&eacute;n keer op welke pin bij welke kant hoort &mdash; zie
   [Doormeten hoeft nauwelijks](#doormeten-hoeft-nauwelijks).
2. **Bouw de bureau-opstelling** en doorloop de vier tests uit
   [Bureau-opstelling](#bureau-opstelling-esp32--avr). Nog geen OpenTherm: eerst
   bewijzen dat de koppeling staat.
3. **Anna meelezen.** AVR als OT-slave op pin 6/13, met vaste antwoorden. Geef
   Anna's setpoint en kamertemperatuur door aan de ESP32 en publiceer ze in Home
   Assistant. Nog niets met de ketel.
4. **Ketel meelezen.** AVR als OT-master op pin 5/11, ketel op minimale vraag.
   Lees status en aanvoertemperatuur uit.
5. **Ketel echt aansturen** met een vaste, lage setpoint vanuit de ESP32.
   Verifieer dat je 'm aan en uit krijgt, en test de terugvalmodus door de ESP32
   uit te zetten.
6. **De regelaar erbij:** Anna's vraag verdelen over warmtepomp en ketel, zoals
   beschreven in [hybride-tactieken](hybride-tactieken.md), maar dan met echte
   modulatie in plaats van een aan/uit-signaal.

Handig tijdens stap 3 en 4: de **originele T1.1 als referentie**. Zet 'm terug,
luister het 9600-baud-verkeer af met een USB-seriëeladapter, en kijk hoe het
origineel zich gedraagt.

## Doormeten hoeft nauwelijks

Dit is de prettige kant van een drop-in: **de originele chip werkte.** Een
pin-compatibele AVR in dezelfde socket die hetzelfde doet, draait dus op bewezen
hardware. Daarmee vervallen de meeste vragen die je bij een verbouwing zou hebben:

| Vraag | Waarom je 'm niet hoeft te beantwoorden |
|---|---|
| Zit er OT-interface-elektronica op de print? | Ja &mdash; anders had de originele IC2 niet gewerkt |
| Haalt de trafo het? | De belasting is identiek aan het origineel, dat jaren draaide |
| Zijn de niveaus goed? | 5 V in de socket, precies waar de print voor gemaakt is |

Wat je w&eacute;l moet weten is **welke pin bij welke kant hoort** &mdash; dat is het
openstaande conflict met WackoH's tabel. En dat kun je beter waarnemen dan
opmeten.

### Luister eerst, meet daarna

Schrijf als allereerste firmware een **luisteraar**: een AVR die niets zendt en
alleen rapporteert wat er op de vier kandidaat-pinnen binnenkomt. Zet 'm in de
socket met het systeem gewoon in bedrijf.

Binnen een seconde weet je alles:

- **Waar verkeer binnenkomt** lost pin 6 en 13 op. Een thermostaat pollt continu,
  dus op de lijn die naar de thermostaat gaat zie je onmiddellijk Manchester.
- **Welke kant welke is** volgt uit het berichttype: op de thermostaatkant komen
  Read-Data/Write-Data binnen (een master die vraagt), op de ketelkant Read-Ack
  (een slave die antwoordt).

Dat is betrouwbaarder dan sporen volgen op een dubbelzijdige print, en je hebt de
firmware toch al nodig.

### De 9600-baud-link is al bewezen gekruist

Ook dat hoef je niet te meten &mdash; de firmware bewijst het. Beide chips zetten
`UCSRB = 0x98` (RXEN + TXEN) en maken PD1 een uitgang. Op een AVR is **PD0 altijd
RXD en PD1 altijd TXD**, dus een zendpin moet wel op een ontvangstpin uitkomen.
Niet-gekruist zou twee uitgangen tegen elkaar in zetten zonder ontvanger, en dan
had het origineel nooit gewerkt.

WackoH's tabel bevestigt het onafhankelijk: IC1 pin 2 &rarr; IC2 pin 3 en IC1 pin 3
&rarr; IC2 pin 2.

```
IC1 pin 3 (TXD) ────────► IC2 pin 2 (RXD)
IC1 pin 2 (RXD) ◄──────── IC2 pin 3 (TXD)
```

Let bij de ESP32-kant nog wel op de niveaus: de AVR zendt 5 V en dat moet via een
deler omlaag voordat het de ESP32 in gaat. Zie [de koppeling](#de-koppeling-esp32--avr).

## Wat dit ontwerp niet oplost

- **De foutcodes.** IC2 vertaalde storingsnummers naar een knippercode. Welk
  nummer welke storing is, staat niet in de firmware. Zie het verslag.
- **PB3 op IC1.** Nog steeds onopgehelderd, maar aantoonbaar irrelevant voor de
  aansturing.
- **De 5 °C-lockout** komt hiermee niet terug, en dat is de bedoeling.
