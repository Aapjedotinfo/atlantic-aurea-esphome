# Bedrading

De ESP32 vervangt de **linker microcontroller (IC1)** in de controlbox van de
Atlantic Aurea. Je hoeft **niets** aan het hoogspannings-/powerline-deel te doen:
de twee **H11D1-optocouplers** (OK1 = ontvangen, OK2 = zenden) scheiden dat al
volledig van de 5V-logica. De ESP32 praat dus alleen met kant-en-klare 5V-signalen.

> **Daarom geen levelshifter.** Eerdere ideeën met levelshifters/aparte
> spanningsdelers zijn overbodig gebleken. De print heeft aan de zendkant al een
> transistor-driver, en aan de ontvangkant al een emitter-follower. Eén weerstand
> naar massa aan de RX-kant en een directe draad aan de TX-kant volstaan.

![De controlbox met de ESP32 gemonteerd](docs/controlbox-esp32.jpg)

*Zo ziet het er in het echt uit: de ESP32 op de plek van de linker ATmega, de
rechter socket leeg, voeding via USB. Linksonder de klemmenstrook naar de
buitenunit.*

## Wat er weg moet

- **Beide originele ATmega8L-chips** (IC1 én IC2) uit hun socket halen.
- De ESP32 komt op de plek van **IC1** (die met de warmtepomp-communicatie).
- In **IC2** komt een eigen **ATmega328P** met de OpenTherm-brug. Wil je die
  (nog) niet, dan laat je de voet leeg en stuur je de CV-ketel los aan; alles
  aan de warmtepompkant werkt dan gewoon.

## Aansluitingen (adapter in de IC1-socket)

| ESP32 | ↔ | IC1-socket | Functie |
|-------|---|-----------|---------|
| **GPIO17** (TX) | → | **pin 26** | signaal naar de warmtepomp |
| **GPIO16** (RX) | ← | **pin 27** | signaal van de warmtepomp |
| **GPIO18** (TX) | → | **pin 3** | naar de brug in IC2 |
| **GPIO19** (RX) | ← | **pin 2** | van de brug in IC2 |
| **GND** | — | **pin 8** (en 22) | massa |
| **5V/VIN** | — | **pin 7** *of* USB-adapter | voeding |

Losse onderdelen op het adapterprintje: **twee weerstandsdelers**.

| Voor | Onderdelen |
|---|---|
| GPIO16 (van de WP) | **10 kΩ** naar GND; de onboard R9 (4k7) is de bovenste helft |
| GPIO19 (van de brug) | **4k7** in serie vanaf pin 2, plus **10 kΩ** naar GND |

De **verbinding tussen IC1 en IC2 ligt al op de print**, gekruist (pin 2 ↔ pin 3).
Je hoeft er dus geen draad voor te trekken — alleen de deler, want de AVR zendt
op 5 V en de ESP32-ingang wil 3,3 V. GPIO18 mag rechtstreeks: 3,3 V is ruim boven
de drempel van een 5 V-AVR-ingang.

## De brug in de IC2-socket

Een ATmega328P-PU met de firmware uit `firmware/avr-bridge/`. **Er is geen enkele
extra draad nodig**: alle vier de OpenTherm-lijnen, de relais, de dipswitch en het
statuslampje liggen al op de bestaande sporen naar die voet. Het kristal van
8 MHz zit er ook al onder.

Zie [README.md](README.md#voor-ic2--de-opentherm-brug) voor de chip, de fuses en
de kant-en-klare hex.

> **Zet dipswitch 2 op ON.** Die zit op socket-pin 25 en bepaalt het keteltype;
> op OFF knijpt de logica de hele ketelkant af.

## Schema

```
                      CONTROLBOX-PRINT (bestaand)                    ADAPTER / ESP32
   ┌───────────────────────────────────────────────────────┐
   │  ZENDEN naar WP (niet-inverterend: pin26 HIGH = LED aan)│
   │                                                         │
   │   +5V ──►|── OK2 ── R10(330Ω) ──┐                       │
   │        LED-anode  LED-kathode   │                       │
   │                              collector                  │
   │                               [T2 BC547]                │
   │   pin26 ── R14(1k) ── base       │emitter               │
   │     ▲                            └── GND                 │
   │     └──────────────────────────────────────────────────┼──── GPIO17 (TX)
   │                                                         │
   │  ONTVANGEN van WP (emitter-follower, niet-inverterend)  │
   │                                                         │
   │   +5V ── OK1 collector                                  │
   │            OK1 emitter ──┬── R17(1k) ── GND             │
   │                          │                              │
   │                          └── R9(4k7) ── pin27 ──────────┼──┬─ GPIO16 (RX)
   │                                                         │  │
   │                                                         │ [10k]
   │                                                         │  │
   │                                                         │ GND
   │   pin7 ── +5V (LM7805)                                  │
   │   pin8 ── GND ──────────────────────────────────────────┼──── GND / 5V
   └───────────────────────────────────────────────────────┘
```

**Waarom dit werkt zonder levelshifter of inversie:**

- **TX:** GPIO17 gaat rechtstreeks op pin 26. De onboard `R14 (1k) → T2-basis`
  zet 3,3V netjes om in genoeg basisstroom om T2 in verzadiging te sturen, dus de
  OK2-LED krijgt zijn volle stroom. De hele keten is niet-inverterend.
- **RX:** de OK1-fototransistor werkt als emitter-follower. De onboard `R9 (4k7)`
  in serie plus jouw `10k` naar GND vormen een deler die het ~5V-signaal terugbrengt
  naar ~3,4V — veilig voor de ESP32-ingang. Ook niet-inverterend.

## Voeding

Twee opties:

1. **USB-adapter (aanbevolen, simpelst).** Prik een gewone 5V USB-wandadapter in
   de bananenpluggen waar eerder de Plugwise-adapter zat, en voed de ESP32 via
   USB/VIN. De controlbox-print hoeft dan zelf geen extra stroom te leveren.
2. **Vanuit de socket (pin 7 = 5V, pin 8 = GND).** De onboard LM7805 kan de ESP32
   plus de twee optocouplers prima voeden. Let op: met de brug in IC2 erbij komt
   de OpenTherm-kant er weer bij, en die hing er in het origineel ook aan — de
   LM7805 wordt gevoed uit de LM7818, dus de 18 V-tak voor OpenTherm leeft zodra
   de 5 V leeft.

## Communicatie-instellingen

666 baud, 8N1. Zie [README.md](README.md) voor het protocol. Verdachte je de
bekabeling, zet dan de **"Debug frames"**-switch aan in de web-GUI: dan verschijnt
elke ontvangen `FRAME … crc=OK/FOUT` in de log, zodat je meteen ziet of er schone
telegrammen binnenkomen.

> De onboard-waarden (R14 1k, R10 330Ω, R9 4k7, R17 1k) zijn afgelezen van één
> controlbox-revisie. Controleer ze op jouw print; de aanpak (TX direct, RX via
> deler) blijft hetzelfde.
