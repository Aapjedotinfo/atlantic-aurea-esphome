# AVR-brug v2 (experimenteel)

**Status: ongetest op echte hardware voor deze publicatie behalve de proeven van de eigenaar; de AVR-logica is
op de pc niet volledig getest. Gebruik op eigen risico, begin op de bank met de ketel geblokkeerd.**

Een nieuwe versie van de ATmega328P-brug (firmware/avr-bridge-v2) met een nieuw protocol tussen de ESP32 en de AVR,
en het bijbehorende ESPHome-component `aurea_link2`. De huidige brug (firmware/avr-bridge) blijft onveranderd.

## Wat is nieuw
- Link ESP32 naar AVR op **38400 baud** met een eigen berichtformaat (HELLO, TABEL, VRAAG/ANTWOORD, STATUS, COMMANDO), CRC-8, en
  volgnummers zodat een verlaat ketelantwoord nooit bij de verkeerde vraag terechtkomt.
- Een **regeltabel per Data-ID en richting** (doorgeven, vaste waarde, vraag de ESP32, blokkeren) die de ESP32 instelt en na elke
  herstart opnieuw naar de AVR stuurt. In ESPHome: `onderscheppen:` en de trigger `on_vraag`.
- **Veilige opstart:** geremd, CH-vlag onderdrukt, de noodbrug pas na 240 s zonder ESP32 (v1: direct).
- De AVR meldt zijn **softwareversie** (HELLO).
- **TX-polariteit** naar de thermostaat is vanuit de ESP32 omkeerbaar (runtime-vlag). Een Honeywell Chronotherm Touch werkt
  alleen met de polariteit van het origineel (PD4 rust hoog); Anna en Remeha werkten met de oude. Zie de proef in de
  beschrijving van `aurea-link2.yaml.example`.

## Niet compatibel met v1
Een v2-AVR praat niet met de oude ESP-firmware (`aurea_link`) en andersom: beide kanten moeten tegelijk naar v2. Branden met
dezelfde fuses als v1 (lfuse 0xFF, hfuse 0xD9, efuse 0xFD), zie `firmware/avr-bridge-v2/README.md`.

## Waar te beginnen
1. `firmware/avr-bridge-v2/PROTOCOL.md`: het protocol (bytelayouts, tijden, de standaardtabel).
2. `firmware/avr-bridge-v2/README.md`: bouwen, branden en een bankplan.
3. `docs/aurea-link2.yaml.example`: een compileerbaar voorbeeld van de ESP-kant (alleen het component `aurea_link2` nodig).
