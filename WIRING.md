# Bedrading

De ESP32 vervangt de **linker microcontroller (IC1)** in de controlbox van de
Atlantic Aurea. Je hoeft **niets** aan het hoogspannings-/powerline-deel te doen:
de twee **H11D1-optocouplers** (OK1 = ontvangen, OK2 = zenden) scheiden dat al
volledig van de 5V-logica. De ESP32 praat dus alleen met kant-en-klare 5V-signalen.

> **Daarom geen levelshifter.** Eerdere ideeën met levelshifters/aparte
> spanningsdelers zijn overbodig gebleken. De print heeft aan de zendkant al een
> transistor-driver, en aan de ontvangkant al een emitter-follower. Eén weerstand
> naar massa aan de RX-kant en een directe draad aan de TX-kant volstaan.

## Wat er weg moet

- **Beide originele ATmega8L-chips** (IC1 én IC2) uit hun socket halen.
  De ESP32 komt op de plek van IC1 (die met de warmtepomp-communicatie).
- De CV-ketel stuur je los aan; het relais en de OpenTherm-MCU (IC2) blijven
  ongebruikt.

## Aansluitingen (adapter in de IC1-socket)

| ESP32 | ↔ | IC1-socket | Functie |
|-------|---|-----------|---------|
| **GPIO17** (TX) | → | **pin 26** | signaal naar de warmtepomp |
| **GPIO16** (RX) | ← | **pin 27** | signaal van de warmtepomp |
| **GND** | — | **pin 8** (en 22) | massa |
| **5V/VIN** | — | **pin 7** *of* USB-adapter | voeding |

Plus **één** los onderdeel op het adapterprintje: een **10 kΩ** weerstand van
**GPIO16 naar GND**.

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
   plus de twee optocouplers prima voeden. De trafo (15V, 2,8VA) heeft daar ruim
   voldoende marge voor nu het relais en de OpenTherm-MCU niet meer belast worden.

## Communicatie-instellingen

666 baud, 8N1. Zie [README.md](README.md) voor het protocol. Verdachte je de
bekabeling, zet dan de **"Debug frames"**-switch aan in de web-GUI: dan verschijnt
elke ontvangen `FRAME … crc=OK/FOUT` in de log, zodat je meteen ziet of er schone
telegrammen binnenkomen.

> De onboard-waarden (R14 1k, R10 330Ω, R9 4k7, R17 1k) zijn afgelezen van één
> controlbox-revisie. Controleer ze op jouw print; de aanpak (TX direct, RX via
> deler) blijft hetzelfde.
